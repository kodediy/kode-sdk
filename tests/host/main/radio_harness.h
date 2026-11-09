/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_harness.h
 * @brief What a case needs to drive AO_Radio: fake workers, the held done event, the way to off.
 */
#ifndef RADIO_HARNESS_H_
#define RADIO_HARNESS_H_

#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "esp_system.h"

#include "qpc.h"
#include "Mockesp_wifi.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi_default.h"
#include "Mockesp_event.h"

#include "esp_app_desc.h"   /* to lay IDF's descriptor in a test image */

#include "ao_spy.h"
#include "config_support.h"
#include "fake_wifi_events.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kdrv_ble.h"
#include "kdrv_ble_app.h"
#include "kdrv_c5.h"
#include "kdrv_c5svc.h"
#include "khal_identity.h"
#include "kode_wire.h"
#include "kdrv_kfw.h"
#include "kdrv_radio.h"
#include "kdrv_storage.h"
#include "kdrv_wifi.h"
#include "kode_sys.h"
#include "khal_crc32.h"
#include "khal_nvs.h"
#include "khal_sync.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "kset.h"
#include "lowpower_cases.h"
#include "qf_support.h"

/* ---- Fixture ----------------------------------------------------------------- */

/* ---- The workers ------------------------------------------------------------
 * A job the AO hands a worker waits in the fake worker's queue until a case
 * pulls it. The radio worker's job runs (the driver fake records it) and its
 * done event is taken out of the AO's queue and HELD: the job is "inside the
 * worker" until the case ends it with finish(), which delivers the event with
 * the status the case chose - as the board delivers it when the C5 answers.
 * The service worker's sends run and answer at once, and their done events
 * wait in the queue for the next pump, as an event posted from another task
 * does.
 */
#define RADIO_WORKER      "radio_worker"
#define RADIO_SVC_WORKER  "radio_svc"

static const QEvt      *s_held;       /**< The radio worker's done event, withheld. */
static kdrv_radio_req_t s_held_req;   /**< The job it is for.                       */
static kdrv_radio_req_t s_last_req;   /**< The job that was last held.              */
static unsigned         s_submits;    /**< Jobs the radio worker has held.          */

/** @brief Take the first event of @p sig out of AO_Radio's queue, or NULL. */
static const QEvt *take_queued(enum_t sig)
{
    const QEvt *found = NULL;
    UBaseType_t n     = uxQueueMessagesWaiting(AO_Radio->eQueue);

    for ( UBaseType_t i = 0U; i < n; ++i )
    {
        const QEvt *e = NULL;

        TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
        if ( (NULL == found) && ((QSignal)sig == e->sig) )
        {
            found = e;
        }
        else
        {
            TEST_ASSERT_TRUE(xQueueSend(AO_Radio->eQueue, &e, 0U) == pdTRUE);
        }
    }
    return found;
}

/* AO_Storage is not dispatched here, so the harness plays it for the one ask
 * that is answered: the unmount that gates the C5's reset. It unmounts the
 * card the way AO_Storage's worker does and posts the answer - once per
 * request, which the pointer remembers until the request leaves the queue.
 * A case that wants to answer by hand turns this off. */
static bool        s_gate_auto = true;
#define GATE_SEEN_MAX  16U
static const QEvt *s_gate_seen[GATE_SEEN_MAX];   /**< Asks already answered. */

/** @brief Whether @p e was answered already. */
static bool gate_seen(const QEvt *e)
{
    for ( unsigned i = 0U; GATE_SEEN_MAX > i; ++i )
    {
        if ( s_gate_seen[i] == e )
        {
            return true;
        }
    }
    return false;
}

/** @brief Answer the radio's unmounts waiting in AO_Storage's queue, once each. */
static void answer_gate(void)
{
    const QEvt *ev[32];
    const QEvt *now[GATE_SEEN_MAX] = { NULL };
    unsigned    asks = 0U;
    UBaseType_t n    = uxQueueMessagesWaiting(AO_Storage->eQueue);

    if ( 32U < n )
    {
        n = 32U;
    }
    for ( UBaseType_t i = 0U; i < n; ++i )
    {
        TEST_ASSERT_TRUE(xQueueReceive(AO_Storage->eQueue, &ev[i], 0U) == pdTRUE);
    }
    for ( UBaseType_t i = 0U; i < n; ++i )
    {
        if ( (QSignal)STORAGE_CMD_SIG == ev[i]->sig )
        {
            StorageCmdEvt const *c = (StorageCmdEvt const *)ev[i];

            if ( ((storage_op_t)c->op == STORAGE_OP_UNMOUNT)
                && (c->who == AO_Radio) && (GATE_SEEN_MAX > asks) )
            {
                now[asks++] = ev[i];
            }
        }
        TEST_ASSERT_TRUE(xQueueSend(AO_Storage->eQueue, &ev[i], 0U) == pdTRUE);
    }
    /* Answer the ones not answered yet, then remember exactly the ones that
     * are in the queue: a request that left it may reuse its pool block. */
    {
        bool fresh[GATE_SEEN_MAX] = { false };

        for ( unsigned i = 0U; i < asks; ++i )
        {
            fresh[i] = !gate_seen(now[i]);
        }
        (void)memset(s_gate_seen, 0, sizeof(s_gate_seen));
        for ( unsigned i = 0U; i < asks; ++i )
        {
            s_gate_seen[i] = now[i];
        }
        for ( unsigned i = 0U; i < asks; ++i )
        {
            if ( fresh[i] )
            {
                static const QEvt rsp = QEVT_INITIALIZER(STORAGE_CMD_RSP_SIG);

                kdrv_storage_unmount(KDRV_MEDIUM_SD);
                QASM_DISPATCH(AO_Radio, &rsp, 0U);
            }
        }
    }
}

/**
 * @brief Run what the AO handed the workers.
 * @return true when anything ran.
 */
static bool pull_jobs(void)
{
    bool any = false;

    if ( s_gate_auto )
    {
        answer_gate();
    }

    while ( fake_khal_worker_pending_for(RADIO_WORKER) > 0U )
    {
        TEST_ASSERT_NULL_MESSAGE(s_held,
            "a second job reached the radio worker while it is inside one");
        TEST_ASSERT_TRUE_MESSAGE(uxQueueSpacesAvailable(AO_Radio->eQueue) > 0U,
            "AO_Radio's queue is full before the worker answers");
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_WORKER));
        s_held = take_queued(RADIO_JOB_SIG);
        TEST_ASSERT_NOT_NULL_MESSAGE(s_held,
            "the radio worker's done event was not queued");
        s_held_req = *fake_radio_ran();
        s_last_req = s_held_req;
        ++s_submits;
        any = true;
    }
    while ( fake_khal_worker_pending_for(RADIO_SVC_WORKER) > 0U )
    {
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
        any = true;
    }
    return any;
}

static unsigned radio_settle(void);

/** @brief Deliver what waits for the AO, with no tick, and run what it hands out. */
static void drain(void)
{
    unsigned n;

    do
    {
        n = radio_settle();
    } while ( pull_jobs() || (0U < n) );
}

/** @brief One 10 ms step, then whatever it set going. */
static void pump(void)
{
    (void)qf_support_pump(AO_Radio, 32U);
    drain();
}

/** @brief Let @p steps 10 ms steps go by, answering what the AO hands the workers. */
static void tick(unsigned steps)
{
    drain();
    (void)qf_support_tick(AO_Radio, steps);
    drain();
}

/**
 * @brief Deliver what waits for the AO, and play AO_Storage's answer to the
 *        unmounts it asked for.
 * @return How many events the AO dispatched.
 */
static unsigned radio_settle(void)
{
    unsigned n = qf_support_settle(AO_Radio);

    if ( s_gate_auto )
    {
        answer_gate();
        n += qf_support_settle(AO_Radio);
    }
    return n;
}

/** @brief Dispatch @p e by hand, then run what the AO handed the workers. */
static void dispatch_evt(const QEvt *e)
{
    QASM_DISPATCH(AO_Radio, e, 0U);
    (void)pull_jobs();
}

static void dispatch(enum_t sig)
{
    QEvt e = QEVT_INITIALIZER(sig);
    dispatch_evt(&e);
}

/** @brief Whether a job is inside the radio worker, waiting to be ended. */
static bool job_held(void)
{
    return NULL != s_held;
}

/** @brief The job inside the radio worker, arguments included. */
static const kdrv_radio_req_t *held_req(void)
{
    return &s_held_req;
}

/** @brief The job the radio worker held last, running or ended. */
static const kdrv_radio_req_t *last_req(void)
{
    return &s_last_req;
}

/** @brief Whether a job waits in the radio worker's queue behind the held one. */
static bool job_queued(void)
{
    return fake_khal_worker_pending_for(RADIO_WORKER) > 0U;
}

/** @brief How many jobs the radio worker has held since the reset. */
static unsigned job_submits(void)
{
    return s_submits;
}

/** @brief Empty AO_Storage's mailbox. */
static void storage_drain(void)
{
    const QEvt *e;

    (void)memset(s_gate_seen, 0, sizeof(s_gate_seen));

    qf_support_give_queue(AO_Storage);
    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/**
 * @brief Empty AO_Net's mailbox.
 *
 * AO_Net subscribes to WIFI_STATE_SIG and nobody dispatches it here, so
 * every state the radio publishes stays referenced in its queue - a pool
 * block each, for the rest of the binary, unless it is collected.
 */
static void net_drain(void)
{
#if CONFIG_KODEOS_NET
    const QEvt *e;

    if ( NULL == AO_Net->eQueue )
    {
        return;
    }
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
#endif
}

/**
 * @brief Empty AO_Boot's mailbox.
 *
 * AO_Boot is not dispatched here: its queue is the record of the answers the
 * radio owes a jump.
 *
 * @return How many BOOT_RADIO_PARKED_SIG it held.
 */
static unsigned boot_parked_taken(void)
{
    const QEvt *e;
    unsigned n = 0U;

    if ( NULL == AO_Boot->eQueue )
    {
        return 0U;
    }
    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)BOOT_RADIO_PARKED_SIG == e->sig )
        {
            n += 1U;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief The next storage command the radio asked for, or -1. */
static int next_storage_op(void)
{
    const QEvt *e = NULL;
    int op = -1;

    if ( xQueueReceive(AO_Storage->eQueue, &e, 0U) != pdTRUE )
    {
        return -1;
    }
    if ( (QSignal)STORAGE_CMD_SIG == e->sig )
    {
        op = (int)((const StorageCmdEvt *)e)->op;
    }
    else
    {
        op = -2;   /* something else: an open, a read */
    }
    QF_gc(e);
    return op;
}

/** @brief Whether any captured log line contains @p needle. */
static bool logged(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/** @brief The index of the first captured line containing @p needle, or -1. */
static int log_index(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return (int)i;
        }
    }
    return -1;
}

/** @brief Whether @p a and @p b end with the same outcome: a join and the
 *         wait for the one the C5 makes are the same event to the AO. */
static bool same_outcome(kdrv_radio_job_t a, kdrv_radio_job_t b)
{
    bool same = (a == b);

    if ( ((KDRV_RADIO_JOB_WIFI_JOIN == a) && (KDRV_RADIO_JOB_WIFI_AWAIT == b))
        || ((KDRV_RADIO_JOB_WIFI_AWAIT == a) && (KDRV_RADIO_JOB_WIFI_JOIN == b)) )
    {
        same = true;
    }
    return same;
}

/** @brief Hand the held done event to the AO with @p rc, and recycle it. */
static void deliver_held(kode_status_t rc)
{
    KqpWorkerDoneEvt *d;

    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "no job is with the worker");
    d      = (KqpWorkerDoneEvt *)(void *)s_held;
    s_held = NULL;
    d->status = (uint8_t)rc;
    QASM_DISPATCH(AO_Radio, &d->super, 0U);
    QF_gc(&d->super);
}

/** @brief End the running job and deliver its outcome to the AO, alone. */
static void finish_only(kdrv_radio_job_t job, kode_status_t rc)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "no job is with the worker");
    TEST_ASSERT_TRUE_MESSAGE(same_outcome(job, s_held_req.type),
        "the job the worker holds is not the one being finished");
    fake_radio_effects(s_held_req.type, rc);
    deliver_held(rc);
    pump();
}

/**
 * @brief End the running job and deliver its outcome to the AO.
 *
 * A connect that worked is followed by the answer AO_Storage owes the
 * remount: the link is `starting` until the card is back, and nobody
 * dispatches AO_Storage here.
 */
static void finish(kdrv_radio_job_t job, kode_status_t rc)
{
    finish_only(job, rc);
    if ( (KDRV_RADIO_JOB_CONNECT == job) && (KODE_OK == rc) )
    {
        dispatch(STORAGE_CMD_RSP_SIG);
        pump();
    }
}

/**
 * @brief The link is lost, with a job inside the worker or none.
 *
 * What the driver does when a C5 stops answering (KODE_TIMEOUT: the job that
 * found it mute fails, the AO is told, and the job comes back) or when the
 * transport drops (KODE_ERROR: the job, if any, fails on its own after the
 * AO has heard). The AO hears of the link first; the job comes back after.
 */
static void finish_lost(kode_status_t why)
{
    bool const inside = job_held();

    fake_radio_lose_link(why);
    pump();
    if ( inside )
    {
        deliver_held(KODE_ERROR);
        pump();
    }
}

/** @brief The running job's type, or -1 when nothing runs. */
static int running_job(void)
{
    if ( !job_held() )
    {
        return -1;
    }
    return (int)held_req()->type;
}

/**
 * @brief End the running job the way the real worker most often would,
 *        so the AO can be walked to wherever a case needs it.
 */
static void finish_whatever_runs(void)
{
    switch ( (kdrv_radio_job_t)running_job() )
    {
        case KDRV_RADIO_JOB_CONNECT:    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK); break;
        case KDRV_RADIO_JOB_DISCONNECT: finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK); break;
        case KDRV_RADIO_JOB_WIFI_SCAN:  finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR); break;
        case KDRV_RADIO_JOB_WIFI_JOIN:  finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR); break;
        case KDRV_RADIO_JOB_WIFI_LEAVE: finish(KDRV_RADIO_JOB_WIFI_LEAVE, KODE_OK); break;
        case KDRV_RADIO_JOB_WIFI_STOP:  finish(KDRV_RADIO_JOB_WIFI_STOP, KODE_OK); break;
        case KDRV_RADIO_JOB_WIFI_RSSI:  finish(KDRV_RADIO_JOB_WIFI_RSSI, KODE_OK); break;
        case KDRV_RADIO_JOB_WIFI_PS:    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK); break;
        case KDRV_RADIO_JOB_WIFI_ADOPT: finish(KDRV_RADIO_JOB_WIFI_ADOPT, KODE_ERROR); break;
        case KDRV_RADIO_JOB_C5_INFO:    finish(KDRV_RADIO_JOB_C5_INFO, KODE_ERROR); break;
        case KDRV_RADIO_JOB_C5_ABORT:   finish(KDRV_RADIO_JOB_C5_ABORT, KODE_OK); break;
        case KDRV_RADIO_JOB_C5_BEGIN:   finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_ERROR); break;
        case KDRV_RADIO_JOB_C5_WRITE:   finish(KDRV_RADIO_JOB_C5_WRITE, KODE_ERROR); break;
        case KDRV_RADIO_JOB_C5_FINISH:  finish(KDRV_RADIO_JOB_C5_FINISH, KODE_ERROR); break;
        case KDRV_RADIO_JOB_BLE_START:  finish(KDRV_RADIO_JOB_BLE_START, KODE_ERROR); break;
        case KDRV_RADIO_JOB_BLE_ADV:    finish(KDRV_RADIO_JOB_BLE_ADV, KODE_OK); break;
        case KDRV_RADIO_JOB_BLE_SCAN:   finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_ERROR); break;
        case KDRV_RADIO_JOB_BLE_QUIET:  finish(KDRV_RADIO_JOB_BLE_QUIET, KODE_OK); break;
        case KDRV_RADIO_JOB_BLE_STOP:   finish(KDRV_RADIO_JOB_BLE_STOP, KODE_OK); break;
        case KDRV_RADIO_JOB_BLE_UNPAIR: finish(KDRV_RADIO_JOB_BLE_UNPAIR, KODE_OK); break;
        case KDRV_RADIO_JOB_BLE_RSP:    finish(KDRV_RADIO_JOB_BLE_RSP, KODE_OK); break;
        case KDRV_RADIO_JOB_EXP_POWER:  finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK); break;
        case KDRV_RADIO_JOB_AMP:        finish(KDRV_RADIO_JOB_AMP, KODE_OK); break;
        case KDRV_RADIO_JOB_SLEEP:      finish(KDRV_RADIO_JOB_SLEEP, KODE_OK); break;
        default:                        finish(KDRV_RADIO_JOB_C5_ABORT, KODE_ERROR); break;
    }
}

/** @brief The next published link state in the spy, or -1. */
static int take_link(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            return -1;
        }
        if ( (QSignal)RADIO_STATE_SIG == e->sig )
        {
            return (int)((const RadioStateEvt *)e)->link;
        }
    }
}

/** @brief Ask the AO where the link is and read the answer. */
static int link_now(void)
{
    ao_spy_drain();
    dispatch(RADIO_STATE_REQ_SIG);
    return take_link();
}

/** @brief Bring AO_Radio up once, and give the AOs it posts to a mailbox. */
static void radio_up_once(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_subscribe(RADIO_STATE_SIG);
    ao_spy_subscribe(WIFI_STATE_SIG);
    ao_spy_subscribe(WIFI_SCAN_ENTRY_SIG);
    ao_spy_subscribe(WIFI_SCAN_END_SIG);
    ao_spy_subscribe(BLE_SCAN_END_SIG);
    ao_spy_subscribe(BLE_PAIR_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);
    ao_spy_drain();

    qf_support_give_queue(AO_Storage);
    qf_support_give_queue(AO_Boot);

    /* The worker posts its done event with a margin: it needs the queue QP lays out in osObject. */
    qf_support_margin_queue(AO_Radio);

    if ( qf_support_claim_ao(AO_Radio) )
    {
        Radio_ctor();
        qf_support_give_queue(AO_Radio);
        AO_Radio->prio  = KODE_PRIO_RADIO;
        AO_Radio->pthre = KODE_PRIO_RADIO;
        QActive_register_(AO_Radio);
        QASM_INIT(AO_Radio, (void *)0, 0U);
    }
    qf_support_give_queue(AO_Radio);
}

/**
 * @brief Walk the AO back to `off`, whatever the previous case left.
 *
 * The runner resets the fake before every case, so a job the previous case
 * left running is gone from the fake while the AO still waits on it. The
 * walk therefore does not only finish what the fake holds: a link that is
 * starting or stopping with nothing running gets the outcome it waits for,
 * and a link that is up and will not come down is busy with a forgotten job,
 * which any outcome ends - the AO's busy flag is one flag, whatever the job.
 */
static void settle_to_off(void)
{
    /* A case of the board's rest may leave the sentinel armed: the board
     * waking is what stops it, and the link will not come down before. */
    lp_awake(AO_Radio);
    for ( unsigned guard = 0U; 24U > guard; ++guard )
    {
        while ( job_held() )
        {
            finish_whatever_runs();
        }
        int const link = link_now();
        if ( (int)KRADIO_OFF == link )
        {
            return;
        }
        if ( (int)KRADIO_STARTING == link )
        {
            finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
        }
        else if ( (int)KRADIO_STOPPING == link )
        {
            finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
        }
        else
        {
            dispatch(RADIO_DOWN_REQ_SIG);
            pump();
            if ( ((int)KRADIO_UP == link) && (link_now() == (int)KRADIO_UP)
                && !job_held() )
            {
                /* Refused as busy, for a job the reset forgot. Which one is
                 * unknown; end the four kinds a case can leave in flight -
                 * the last one is a C5 update's first job, whose failure
                 * is what brings Radio_updating back to Radio_up. */
                if ( (guard % 4U) == 0U )
                {
                    finish(KDRV_RADIO_JOB_C5_INFO, KODE_ERROR);
                }
                else if ( (guard % 4U) == 1U )
                {
                    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
                }
                else if ( (guard % 4U) == 2U )
                {
                    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
                }
                else
                {
                    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_ERROR);
                }
            }
        }
    }
    TEST_FAIL_MESSAGE("AO_Radio could not be walked back to off");
}

/** @brief A fresh case: the AO off, the fakes healthy, the records empty. */
static void radio_fresh(void)
{
    radio_up_once();
    radio_lp_expect_net(true);   /* the rounds wait for AO_Net, as on the board */
    s_gate_auto = true;
    settle_to_off();
    fake_radio_reset();
    fake_hosted_reset();
    /* The real worker forgets the snapshot on the way out of DISCONNECT;
     * the fake has no worker, so the walk to off leaves it standing. */
    kdrv_c5_forget();
    storage_drain();
    net_drain();
    (void)boot_parked_taken();
    fake_klog_reset();
    ao_spy_drain();
}

/** @brief From off to up by hand, the way `radio_up` does it. */
static void go_up(void)
{
    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    storage_drain();
    fake_klog_reset();
    ao_spy_drain();
}


#endif /* RADIO_HARNESS_H_ */
