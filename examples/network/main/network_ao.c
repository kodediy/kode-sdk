/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file network_ao.c
 * @brief The network from an Active Object: ask AO_Net, and be told.
 */
#include "network_ao.h"

#include "kode.h"
#include "kode_worker.h"

#define TAG "network"

/** What the top button pings. A literal address, so that a name that does not
 *  resolve cannot be mistaken for a network that does not answer. */
#define NW_PING_HOST      "1.1.1.1"

/** How many echoes, one a second; at most KODE_SYS_NET_PING_MAX. */
#define NW_PING_COUNT     4U

/** What the bottom button fetches. Plain http:// keeps the example about the
 *  request and not about certificates; https:// works the same way. A URL is
 *  shorter than KODE_SYS_NET_URL_MAX, 80 characters, or refused. */
#define NW_GET_URL        "http://example.com/"

/** The body's room: the GET keeps this many of the first bytes. */
#define NW_BODY_MAX       256U

/** How much of the body is logged: the log line is short. */
#define NW_PREVIEW_CHARS  60U

/* The printable ASCII range; anything else in the preview becomes '.'. */
#define NW_PRINT_FIRST    0x20U  /**< The first printable character, ' '. */
#define NW_PRINT_LAST     0x7EU  /**< The last printable character, '~'.  */

/** The worker's task name. */
#define NW_WORKER_NAME        "nw_worker"

/** Stack of the worker's task, in bytes. It is in PSRAM, so a figure with
 *  room costs no internal RAM, and an overflow there corrupts a neighbour
 *  without a sound: 8 KB is the least the SDK's own workers get. */
#define NW_WORKER_STACK_BYTES 8192U

/** Jobs that may wait besides the one running: the AO sends one at a time. */
#define NW_WORKER_DEPTH       1U

/** @brief This AO's private signals. Posted to itself, never published. */
enum NetworkSignals {
    NW_JOB_DONE_SIG = MAX_SIG    /**< The worker's job returned. */
};

/** @brief What the ping job is asked to do: read by the worker, never changed. */
typedef struct {
    char const *host;    /**< Who to ping.        */
    uint32_t    count;   /**< How many echoes.    */
} NwPingJob;

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive        super;
    kode_worker_t  worker;     /**< Runs the blocking ping, off the step.   */
    uint16_t       job_seq;    /**< Sequence number of the job we wait for. */
} Network;

/* The queue holds a burst of button edges and the one answer out at a time;
 * KQP_AO_QUEUE_LEN, the SDK's own default, has room to spare. The stack is
 * the SDK's default too. The types are not interchangeable: the queue holds
 * QEvtPtr, and the stack is measured in StackType_t words even though
 * KQP_AO_STACK_BYTES counts bytes. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static Network l_ao;
QActive * const AO_Network = &l_ao.super;

/* LENT to AO_Net's worker by every GET, until its answer. */
static char l_body[NW_BODY_MAX];

/* The job's argument lives for the whole run, not on the stack of the step
 * that submits it: the worker reads it later, from its own task, and an
 * argument must stay valid until the done event. */
static NwPingJob l_ping_job = { NW_PING_HOST, NW_PING_COUNT };

static QState Network_initial(Network * const me, void const * const par);
static QState Network_idle(Network * const me, QEvt const * const e);
static QState Network_waiting(Network * const me, QEvt const * const e);
static QState Network_pinging(Network * const me, QEvt const * const e);
static QState Network_fetching(Network * const me, QEvt const * const e);
static QState Network_job_pinging(Network * const me, QEvt const * const e);

void Network_ctor(void)
{
    Network * const me = &l_ao;
    QActive_ctor(&me->super, Q_STATE_CAST(&Network_initial));
    me->job_seq = 0U;
}

void Network_start(void)
{
    /* Name the FreeRTOS task after the AO, so that an assert names it. */
    QActive_setAttr(AO_Network, TASK_NAME_ATTR, "network");

    /* KODE_PRIO_USER_LOW, 1, is below AO_Net's 2: the AO that asks sits under
     * the AO that answers, so a request may be picked up and answered before
     * the step that made it is over, and the answer waits in this queue until
     * that step is. That order is what makes the transition in Network_idle
     * safe. */
    QACTIVE_START(AO_Network, KODE_PRIO_USER_LOW,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/* A job may wait on kode.h, but never reach the flash (ESP-IDF flash or NVS,
 * kode_setting_*()): its stack is in PSRAM. */
/**
 * @brief The job the worker runs: a blocking ping.
 *
 * This runs on the worker's task, not on the AO's, so it may wait as long as
 * kode_net_ping() needs. It only reads its argument, calls the helper and
 * returns: whether the session ran goes back to the AO as the done event's
 * status, and the number of echoes that came back as its value. Nothing here
 * decides what the AO does next.
 *
 * @param arg   The NwPingJob that was submitted.
 * @param value Receives the echoes that came back.
 * @return KODE_OK when the session ran, even if nothing answered; KODE_ERROR
 *         when it did not (no network, a name that did not resolve).
 */
static kode_status_t Network_ping_job(void * const arg, uint32_t * const value)
{
    NwPingJob const * const job = (NwPingJob const *)arg;
    kode_net_ping_t const p = kode_net_ping(job->host, job->count);
    kode_status_t rc = KODE_ERROR;

    *value = p.received;
    if ( p.ok )
    {
        rc = KODE_OK;
    }
    return rc;
}

/**
 * @brief Log the first bytes of a body, each one that does not print as '.'.
 * @param kept How many bytes the GET put in l_body.
 */
static void log_preview(uint32_t const kept)
{
    char     line[NW_PREVIEW_CHARS + 1U];
    uint32_t n = kept;
    uint32_t i;

    if ( NW_PREVIEW_CHARS < n )
    {
        n = NW_PREVIEW_CHARS;
    }
    for ( i = 0U; i < n; i++ )
    {
        uint8_t const c = (uint8_t)l_body[i];

        if ( (NW_PRINT_FIRST <= c) && (NW_PRINT_LAST >= c) )
        {
            line[i] = (char)c;
        }
        else
        {
            line[i] = '.';
        }
    }
    line[n] = '\0';
    KLOG_I(TAG, "first bytes: %s", line);
}

/**
 * @brief The initial transition: subscribe to the buttons, start the worker,
 *        then run.
 *
 * No request is made here: this AO asks for nothing until a button says so.
 * The answers and the worker's done events are posted to this AO, so they
 * need no subscription. The worker is started here, once, because the initial
 * transition runs once.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Network_idle.
 */
static QState Network_initial(Network * const me, void const * const par)
{
    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);

    kode_status_t const wrc = kode_worker_start(&me->worker, &me->super,
                                                NW_WORKER_NAME,
                                                NW_WORKER_STACK_BYTES,
                                                KODE_PRIO_USER_LOW,
                                                NW_WORKER_DEPTH);
    if ( KODE_OK != wrc )
    {
        KLOG_E(TAG, "the worker did not start (status %d); the left button "
                    "will be refused", (int)wrc);
    }

    KLOG_I(TAG, "up; the top button pings %s, the bottom one fetches %s, "
                "the left one pings on the worker",
           NW_PING_HOST, NW_GET_URL);
    return Q_TRAN(&Network_idle);
}

/**
 * @brief Nothing is out: a button press sends a request and starts waiting.
 *
 * The request is made in the transition, before the state changes, and that
 * is safe. A request only posts an event to AO_Net, and AO_Net outranks this
 * AO, so it may run and answer - "no network" is answered at once - before
 * this function has returned. The answer waits in this AO's queue until the
 * step is over, by which time the state is the one waiting for it.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Network_idle(Network * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = Q_EVT_CAST(ButtonEvt);
            kode_status_t rc = KODE_OK;

            status = Q_HANDLED();
            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                rc = kode_sys_net_ping(&me->super, NW_PING_HOST,
                                       (uint8_t)NW_PING_COUNT);
                if ( KODE_OK == rc )
                {
                    KLOG_I(TAG, "ping %s, %u echoes", NW_PING_HOST,
                           (unsigned)NW_PING_COUNT);
                    status = Q_TRAN(&Network_pinging);
                }
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                /* LENT: AO_Net's worker writes l_body until NET_HTTP_RSP_SIG. */
                rc = kode_sys_http_get(&me->super, NW_GET_URL, l_body,
                                       NW_BODY_MAX);
                if ( KODE_OK == rc )
                {
                    KLOG_I(TAG, "GET %s", NW_GET_URL);
                    status = Q_TRAN(&Network_fetching);
                }
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_LEFT == (uint32_t)be->id) )
            {
                /* The job is handed over and the step goes on: submit never
                 * blocks. The sequence number comes back through the pointer
                 * and is what the done event is matched against. */
                rc = kode_worker_submit(&me->worker, &Network_ping_job,
                                        &l_ping_job, (enum_t)NW_JOB_DONE_SIG,
                                        &me->job_seq);
                if ( KODE_OK == rc )
                {
                    KLOG_I(TAG, "worker ping %s, %u echoes", NW_PING_HOST,
                           (unsigned)NW_PING_COUNT);
                    status = Q_TRAN(&Network_job_pinging);
                }
            }
            else
            {
                /* A release, or another button: nothing to do. */
            }
            if ( KODE_OK != rc )
            {
                KLOG_W(TAG, "not taken (status %d)", (int)rc);
            }
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief Something is out: what ping, GET and the worker's job have in common.
 *
 * This AO has one door per kind of request, AO_Net does one job at a time,
 * and this AO's worker holds one job at a time too. Refusing the button press
 * here, where the reason is known, says so without sending anything.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Network_waiting(Network * const me, QEvt const * const e)
{
    QState status;

    (void)me;

    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = Q_EVT_CAST(ButtonEvt);

            if ( be->pressed )
            {
                KLOG_I(TAG, "still waiting for the last answer; "
                            "one request at a time");
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }

    return status;
}

/**
 * @brief Waiting for a ping's answer.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Network_pinging(Network * const me, QEvt const * const e)
{
    QState status;

    (void)me;

    switch ( e->sig )
    {
        case NET_PING_RSP_SIG: {
            kode_sys_net_answer_t const * const a =
                Q_EVT_CAST(kode_sys_net_answer_t);
            kode_sys_ping_stats_t const * const p = &a->stats;

            /* rc not KODE_OK is "there was no session": no network, or the
             * name did not resolve, or AO_Net was busy. A session where
             * nothing answered is KODE_OK with rx 0 - the host is not there,
             * which is not the same as being unable to ask. */
            if ( (uint8_t)KODE_OK == a->rc )
            {
                KLOG_I(TAG, "%s: %u of %u back, min/avg/max %u/%u/%u ms",
                       NW_PING_HOST, (unsigned)p->rx, (unsigned)p->tx,
                       (unsigned)p->min_ms, (unsigned)p->avg_ms,
                       (unsigned)p->max_ms);
            }
            else
            {
                /* The board joins a network from the console first: `wifi_up`,
                 * then `wifi_join ssid="<network>" pass="<password>"`. */
                KLOG_W(TAG, "no ping session (status %u): is the board on a "
                            "network? `wifi_join` on the console, see "
                            "network_ao.c", (unsigned)a->rc);
            }
            status = Q_TRAN(&Network_idle);
            break;
        }
        default: {
            status = Q_SUPER(&Network_waiting);
            break;
        }
    }

    return status;
}

/**
 * @brief Waiting for a GET's answer; the body is in l_body once it comes.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Network_fetching(Network * const me, QEvt const * const e)
{
    QState status;

    (void)me;

    switch ( e->sig )
    {
        case NET_HTTP_RSP_SIG: {
            kode_sys_http_answer_t const * const h =
                Q_EVT_CAST(kode_sys_http_answer_t);

            /* rc says whether the request happened, status what the server
             * made of it: only the first is an error of ours to report. */
            if ( (uint8_t)KODE_OK == h->rc )
            {
                KLOG_I(TAG, "status %u, body %lu bytes, %lu of them kept",
                       (unsigned)h->result.status,
                       (unsigned long)h->result.body_len,
                       (unsigned long)h->result.kept);
                log_preview(h->result.kept);
            }
            else
            {
                KLOG_W(TAG, "no GET (status %u): is the board on a network? "
                            "`wifi_join` on the console, see network_ao.c",
                       (unsigned)h->rc);
            }
            status = Q_TRAN(&Network_idle);
            break;
        }
        default: {
            status = Q_SUPER(&Network_waiting);
            break;
        }
    }

    return status;
}

/**
 * @brief Waiting for the worker: one done event, whenever the ping returns.
 *
 * The event carries the job's sequence number, matched against the one
 * kode_worker_submit() gave, so a done event that is not this job's is
 * never taken for it.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState Network_job_pinging(Network * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case NW_JOB_DONE_SIG: {
            kode_worker_done_t const * const d = Q_EVT_CAST(kode_worker_done_t);

            if ( d->seq != me->job_seq )
            {
                /* Not the job this AO waits for. */
                status = Q_HANDLED();
            }
            else
            {
                /* The status is a byte on the wire; read it back as the
                 * kode_status_t it was. KODE_OK is a session that ran, and
                 * the value says how many echoes came back. */
                if ( KODE_OK == (kode_status_t)d->status )
                {
                    KLOG_I(TAG, "%s: %u of %u back (worker)", NW_PING_HOST,
                           (unsigned)d->value, (unsigned)NW_PING_COUNT);
                }
                else
                {
                    KLOG_W(TAG, "the ping job found no network");
                }
                status = Q_TRAN(&Network_idle);
            }
            break;
        }
        default: {
            status = Q_SUPER(&Network_waiting);
            break;
        }
    }

    return status;
}
