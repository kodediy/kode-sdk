/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file rfid_ao.c
 * @brief AO_Rfid: owner of the 125 kHz LF front end, its on-demand field and its worker.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"
#include "kaos_claim.h"
#include "kbsp_owners.h"
#include "kdrv_rfid_lf.h"
#include "khal_sync.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.rfid"

/* cppcheck-suppress misra-c2012-5.9 ; QP's module name for Q_ASSERT: one static of that name per file is QP's design */
Q_DEFINE_THIS_MODULE("rfid_ao")

_Static_assert(KDRV_RFID_LF_ID_BYTES == KQP_RFID_ID_BYTES,
               "the driver's EM4100 ID is the events' ID");

/** @brief How far past a request's @c ms the deadline sits, as a pure backstop
 *         for a worker that never answers - the ordinary end is the outcome. */
#define RFID_DEADLINE_SLACK_MS  1000U

/** @brief The worker's stack, in bytes. The read buffers live in PSRAM and
 *         the driver's primitives call no deep library. The stack is in PSRAM,
 *         where an overflow corrupts a neighbour silently: 8 KB at least,
 *         whatever `ps` reads as free. */
#define RFID_WORKER_STACK  8192U

/** @brief Jobs that may wait besides the one running: none is ever queued,
 *         because AO_Rfid submits only while no job is in flight. */
#define RFID_WORKER_DEPTH  1U

/** @brief The most an EMULATE job takes, in ms: a 10 ms edge count, PSRAM and DMA allocation, PARLIO bring-up, margin. */
#define RFID_EMU_START_MS  250U

/** @brief The most an EMU_STOP job takes, in ms: a PARLIO disable and a release. */
#define RFID_EMU_STOP_MS   100U

/** @brief What kind of request is in flight. */
typedef enum {
    REQ_NONE = 0,
    REQ_READ,    /**< Facade read: RFID_READ_REQ_SIG.      */
    REQ_WRITE,   /**< Facade clone: RFID_WRITE_REQ_SIG.    */
    REQ_ROW,     /**< The production row's own read loop.  */
    REQ_CAPTURE, /**< A carrier-off window: RFID_FIELD_REQ_SIG. */
    REQ_CERT,    /**< A held FCC certification carrier (cert build only). */
    REQ_EMULATE, /**< Facade tag emulation start: RFID_EMU_REQ_SIG.   */
    REQ_EMU_STOP,/**< Facade tag emulation stop: RFID_EMU_STOP_REQ_SIG. */
} req_kind_t;

/** @brief The request in flight, and where its answer goes. */
typedef struct {
    req_kind_t     kind;
    uint8_t       *id;     /**< READ: the requester's 5-byte ID buffer.     */
    uint8_t       *rf_div; /**< READ: out, the bit rate decoded at.         */
    bool          *have;   /**< READ: out, whether a tag turned up.         */
    uint32_t      *rc;     /**< CAPTURE, EMULATE, EMU_STOP: out, the outcome
                            *   as kode_status_t.                          */
    bool          *ok;     /**< WRITE: out, whether the read-back matched.  */
    uint8_t        wid[KQP_RFID_ID_BYTES]; /**< WRITE: a copy of the ID, to compare against. */
    krfid_field_t *stats;  /**< CAPTURE: the statistics the worker writes -
                            *   the requester's, or Rfid's own @c stats when
                            *   it lent none.                               */
    size_t        *n;      /**< CAPTURE: the points the worker wrote - the
                            *   requester's count, or @c points when it lent
                            *   none.                                       */
    khal_sem_t     done;   /**< Posted when the answer is filled.           */
    uint32_t       gen;    /**< The requester's tag for this request.       */
    uint32_t      *ack;    /**< Where @c gen is written back, before
                            *   @c done.                                    */
    QActive       *who;    /**< An AO to post the answer to, or NULL for a
                            *   facade call that waits on @c done.          */
    QActive       *ask;    /**< An ask's AO: the answer below is posted to it. */
    RfidOpRspEvt  *op;     /**< A READ's or WRITE's ask answer, taken with the request. */
    RfidFieldRspEvt *fld;   /**< A CAPTURE's.                                  */
    RfidEmuRspEvt *emu;    /**< An EMULATE's or EMU_STOP's.                   */
    bool           late;   /**< An ask's deadline passed with the job still
                            *   running: it is answered when the job returns. */
} Req;

/** @brief The production row's state. The row is running while the request
 *         in flight is REQ_ROW. */
typedef struct {
    uint8_t attempts;                        /**< Windows tried so far.                */
    uint8_t streak;                          /**< Identical decodes in a row.          */
    bool    have_last;                       /**< Whether @c last_id holds a decode.   */
    uint8_t last_id[KDRV_RFID_LF_ID_BYTES];  /**< The last ID decoded, for the streak. */
    uint8_t mode;                            /**< READER_TEST_*: what this run of the
                                              *   row judges.                          */
} Row;

/** @brief The last tag decoded, whoever asked for it. */
typedef struct {
    bool     have;
    uint8_t  id[KDRV_RFID_LF_ID_BYTES];
    uint8_t  rf_div;
    uint32_t at_ms;
} Last;

/**
 * @brief The job the worker runs and what it leaves for the AO: the driver's
 *        job, the ID a read decoded and the bit rate it came at.
 *
 * AO-owned, because the worker uses it from the submit until the done event
 * and the AO reads the results only then: it is not written again before the
 * next submit.
 */
typedef struct {
    kdrv_rfid_lf_job_t job;                        /**< What to run.                          */
    uint8_t            id[KDRV_RFID_LF_ID_BYTES];  /**< Out: a read's ID, a clone's read-back. */
    uint8_t            rf_div;                     /**< Out: the bit rate it was read at.     */
} RfidJob;

/** @brief What the done event of the current job came to. */
typedef struct {
    bool           ok;      /**< The job succeeded.                          */
    bool           broken;  /**< It failed, and not for want of a tag: the
                             *   front end itself refused - no buffers, no
                             *   converter, a job it would not take.         */
    const uint8_t *id;      /**< RfidJob::id; meaningful on a read or clone. */
    uint8_t        rf_div;  /**< RfidJob::rf_div.                            */
    kode_status_t  st;      /**< What the job answered.                      */
} RfidOutcome;

typedef struct {
    QActive  super;
    QTimeEvt deadline;   /**< RFID_DEADLINE_SIG, one per request.           */
    uint32_t dl_stale;   /**< Expiries that belong to an older arming.      */
    bool     dl_live;
    bool     job;        /**< The worker holds a job, until its done event.
                          *   Every request is refused while it is set.     */
    uint16_t job_seq;    /**< Sequence number of that job.                  */
    KqpWorker worker;    /**< Runs the jobs, on a PSRAM stack.              */
    RfidJob  wjob;       /**< The job and its results.                      */
    bool     field;      /**< The carrier is up: a read, clone or row job, or
                          *   a held certification carrier. Never a capture. */
    uint32_t reads;      /**< Windows that decoded a tag since boot.        */
    uint32_t writes;     /**< Clones whose read-back matched since boot.    */
    uint32_t windows;    /**< Windows that closed with nothing.             */
    krfid_field_t stats; /**< A capture's statistics when its requester lent
                          *   no memory for them.                           */
    size_t   points;     /**< A capture's point count when its requester
                          *   lent no memory for it.                        */
    Req      req;
    Row      row;
    Last     last;
    QActive *emu_ask;    /**< The emulation's opener: its ask's AO, NULL for
                          *   a waiting task. Only it may stop it.          */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;     /**< The id lowpower_join() gave.                  */
#endif
} Rfid;

static Rfid l_rfid;
QActive * const AO_Rfid = &l_rfid.super;

/** @brief A tag is being emulated: set when the EMULATE job answers OK, cleared
 *         when the EMU_STOP job ends. Atomic so rfid_is_emulating() is safe
 *         from any task. */
static atomic_bool s_emulating;

static QState Rfid_initial(Rfid * const me, void const * const par);
static QState Rfid_ready(Rfid * const me, QEvt const * const e);

/* ---- The asks: kode_sys's requests, answered to the asking AO --------------
 * One door per asker and kind: an ask takes it, the answer opens it again
 * just before it is posted, so the asker may ask again from the answer's
 * handler. The front end's own contention is the handler's refusal, answered.
 */

/** @brief The kinds of ask, one door each per asker. */
enum {
    RFID_ASK_OP = 0,   /**< rfid_read_ask() and rfid_write_ask()           */
    RFID_ASK_EMU,      /**< rfid_emulate_ask() and rfid_emulate_stop_ask() */
    RFID_ASK_CAPTURE,  /**< rfid_field_ask()                               */
    RFID_ASK_CERT,     /**< rfid_cert_ask()                                */
    RFID_ASK_KINDS     /**< How many there are. */
};

/** @brief The doors, one per kind of ask. */
static kaos_ask_door_t s_rfid_doors[RFID_ASK_KINDS];

/* The row's per-window judge is reached from the outcome path, so it is
 * declared here rather than moved. */
static void row_on_window(Rfid * const me, RfidOutcome const * const e);
static void row_finish(Rfid * const me, bool ok);

/**
 * @brief Whether the front end is taken for a request of @p kind.
 * @param me   The AO.
 * @param kind The request's kind.
 * @return true when that request must be refused.
 */
static bool busy_for(const Rfid * const me, req_kind_t kind)
{
    bool held = (REQ_NONE != me->req.kind) || me->job;

    if ( atomic_load(&s_emulating) && (REQ_EMU_STOP != kind) )
    {
        held = true;
    }
    return held;
}

/**
 * @brief Whether the front end is taken by anything.
 * @param me The AO.
 * @return true when a new request must be refused.
 */
static bool busy(const Rfid * const me)
{
    return busy_for(me, REQ_NONE);
}

/* ---- The deadline --------------------------------------------------------- *
 * A one-shot that has fired and posted cannot be told from one never armed, so
 * each arming is counted and a late expiry is dropped rather than allowed to
 * end the request that replaced it.
 */

/**
 * @brief @p ms as QP ticks, never zero.
 * @param ms The interval.
 * @return The tick count (QP asserts on a one-shot of zero).
 */
static uint32_t ms_ticks(uint32_t ms)
{
    uint32_t ticks = KQP_TICKS_PER_MS(ms);

    if ( 0U == ticks )
    {
        ticks = 1U;
    }
    return ticks;
}

/** @brief Arm the request's deadline for @p ms. */
static void deadline_arm(Rfid * const me, uint32_t ms)
{
    if ( kqp_timer_stale(&me->deadline, me->dl_live) )
    {
        me->dl_stale += 1U;
    }
    me->dl_live = true;
    QTimeEvt_armX(&me->deadline, ms_ticks(ms), 0U);
}

/** @brief Stop the request's deadline. */
static void deadline_stop(Rfid * const me)
{
    if ( kqp_timer_stale(&me->deadline, me->dl_live) )
    {
        me->dl_stale += 1U;
    }
    me->dl_live = false;
}

/* ---- Answering ------------------------------------------------------------ */

/**
 * @brief Hand the request its answer and let its caller go.
 *
 * The generation is written into @c ack before @c done is signalled, and that
 * write is what orders the requester's other pointers against a reader on the
 * other core.
 *
 * @param me The AO.
 */
static void req_release(Rfid * const me)
{
    Req * const r = &me->req;

    if ( REQ_NONE == r->kind )
    {
        return;
    }
    if ( NULL != r->ack )
    {
        *r->ack = r->gen;
    }
    if ( NULL != r->done )
    {
        khal_sem_signal(r->done);
    }
    r->kind = REQ_NONE;
    r->done = NULL;
    r->ack  = NULL;
    r->who  = NULL;
    r->ask  = NULL;
    r->late = false;
}

/**
 * @brief Fill a read's or a clone's answer.
 *
 * @param ev     The answer.
 * @param write  Whether this is a clone's result rather than a read's.
 * @param rc     KODE_OK on success; KODE_TIMEOUT for an empty window; KODE_DENIED
 *               when the request was refused.
 * @param id     The 5 bytes read or written, or NULL.
 * @param rf_div The bit rate a read decoded at.
 */
static void op_pack(RfidOpRspEvt * const ev, bool write, uint32_t rc,
                    const uint8_t *id, uint8_t rf_div)
{
    ev->write  = write;
    ev->rc     = rc;
    (void)memset(ev->id, 0, sizeof(ev->id));
    if ( NULL != id )
    {
        (void)memcpy(ev->id, id, sizeof(ev->id));
    }
    ev->rf_div = rf_div;
}

/**
 * @brief Post a console's async answer, when a console (not the facade) asked.
 *
 * The facade waits on @c done; a console AO gets a RfidOpRspEvt on
 * RFID_OP_RSP_SIG instead, so it never blocks. A no-op when @c who is NULL.
 *
 * @param me     The AO (the event's sender).
 * @param who    The console AO to answer, or NULL for a facade call.
 * @param write  Whether this is a clone's result rather than a read's.
 * @param rc     KODE_OK on success; KODE_TIMEOUT for an empty window; KODE_DENIED
 *               when the request was refused.
 * @param id     The 5 bytes read or written, or NULL.
 * @param rf_div The bit rate a read decoded at.
 */
static void console_answer(Rfid * const me, QActive *who, bool write,
                           uint32_t rc, const uint8_t *id, uint8_t rf_div)
{
    RfidOpRspEvt *ev;

    if ( NULL == who )
    {
        return;
    }
    ev = Q_NEW_X(RfidOpRspEvt, 0U, RFID_OP_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a console rfid answer is lost");
        return;
    }
    op_pack(ev, write, rc, id, rf_div);
    QACTIVE_POST(who, &ev->super, &me->super);
}

/**
 * @brief Answer a read's or a clone's ask, once.
 *
 * @param ask    The asking AO; NULL for a console or a waiter, which is a no-op.
 * @param ev     The answer, taken with the request.
 * @param write  Whether this is a clone's result rather than a read's.
 * @param rc     What it came to.
 * @param id     The 5 bytes read or written, or NULL.
 * @param rf_div The bit rate a read decoded at.
 */
static void op_ask_answer(QActive *ask, RfidOpRspEvt * const ev, bool write,
                          uint32_t rc, const uint8_t *id, uint8_t rf_div)
{
    if ( NULL == ask )
    {
        return;
    }
    op_pack(ev, write, rc, id, rf_div);
    kaos_ask_answer(&s_rfid_doors[RFID_ASK_OP], ask, &ev->super, AO_Rfid);
}

/**
 * @brief Fill a RfidFieldRspEvt.
 *
 * Every field is set by hand and the event is never cleared with memset:
 * QEvt's header belongs to the pool.
 *
 * @param ev     The answer.
 * @param st     What the request came to.
 * @param f      The window's statistics, or NULL when there are none; the
 *               answer is then all zeros.
 * @param points The points the worker wrote into the requester's waveform.
 *               Ignored without @p f.
 */
static void field_pack(RfidFieldRspEvt * const ev, kode_status_t st,
                       const krfid_field_t *f, size_t points)
{
    ev->st     = (uint32_t)st;
    ev->min    = 0U;
    ev->mean   = 0U;
    ev->peak   = 0U;
    ev->ac     = 0U;
    ev->n      = 0U;
    ev->points = 0U;
    if ( NULL != f )
    {
        ev->min    = f->min;
        ev->mean   = f->mean;
        ev->peak   = f->peak;
        ev->ac     = f->ac;
        ev->n      = f->n;
        ev->points = points;
    }
}

/**
 * @brief Post a RfidFieldRspEvt to an Active Object that asked for a capture.
 *
 * @param me     The AO (the event's sender).
 * @param to     Where it goes. NULL posts nothing: a blocked task was asking.
 * @param st     What the request came to.
 * @param f      The window's statistics, or NULL when there are none.
 * @param points The points the worker wrote into the requester's waveform.
 */
static void field_rsp_post(Rfid * const me, QActive *to, kode_status_t st,
                           const krfid_field_t *f, size_t points)
{
    RfidFieldRspEvt *ev;

    if ( NULL == to )
    {
        return;
    }
    ev = Q_NEW_X(RfidFieldRspEvt, 0U, RFID_FIELD_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; an RFID capture answer is lost");
        return;
    }
    field_pack(ev, st, f, points);
    QACTIVE_POST(to, &ev->super, &me->super);
}

/**
 * @brief Refuse a request that cannot be served, at once.
 *
 * @param done Its semaphore.
 * @param gen  Its generation.
 * @param ack  Where to write @p gen. May be NULL.
 */
static void req_refuse(khal_sem_t done, uint32_t gen, uint32_t *ack)
{
    if ( NULL != ack )
    {
        *ack = gen;
    }
    if ( NULL != done )
    {
        khal_sem_signal(done);
    }
}

/**
 * @brief The job as the worker runs it: the driver's blocking call.
 *
 * Runs on the worker's task and touches only its argument, which is the AO's
 * RfidJob and stays valid until the done event.
 *
 * @param arg   The AO's RfidJob.
 * @param value Unused; what the job found is in the RfidJob.
 * @return What the driver answered.
 */
static kode_status_t rfid_job_run(void *arg, uint32_t *value)
{
    /* cppcheck-suppress misra-c2012-11.5 ; arg is the RfidJob that was submitted: KqpJobFn carries a void * */
    RfidJob * const j = (RfidJob *)arg;

    *value = 0U;
    return kdrv_rfid_lf_run(&j->job, j->id, &j->rf_div);
}

/**
 * @brief Whether the caller is this AO's worker, for the driver's capture check.
 * @return true on the worker's own task.
 */
static bool rfid_in_worker(void)
{
    return kqp_worker_is_self(&l_rfid.worker);
}

/**
 * @brief Hand a job to the worker and remember which one it is.
 *
 * Only while no job is in flight: the job is copied into the AO's storage,
 * which the worker owns from here to the done event.
 *
 * @param me  The AO.
 * @param job The job to run.
 * @return true when the worker has it and @c job is set; false when the
 *         worker would not take it, and nothing changed.
 */
static bool rfid_job_start(Rfid * const me, const kdrv_rfid_lf_job_t *job)
{
    uint16_t seq = 0U;

    me->wjob.job = *job;
    if ( kqp_worker_submit(&me->worker, &rfid_job_run, &me->wjob,
                          RFID_WORKER_DONE_SIG, &seq) != KODE_OK )
    {
        return false;
    }
    me->job_seq = seq;
    me->job     = true;
    return true;
}

/**
 * @brief Take a request, or refuse to submit it without disturbing anything.
 *
 * @param me      The AO.
 * @param kind    Which kind.
 * @param ms      The window it asked for.
 * @param done    Its semaphore.
 * @param gen     Its generation.
 * @param ack     Where to write @p gen. May be NULL.
 * @param job     The job to submit to the worker.
 * @param carrier Whether the job raises the carrier: true for a read or a
 *                clone, false for a capture, which must hear only somebody
 *                else's field.
 * @return true when the job is with the worker, and the field is up when
 *         @p carrier says the job raises it; false when the front end is
 *         busy_for() @p kind or the worker would not take the job.
 */
static bool req_take(Rfid * const me, req_kind_t kind, uint32_t ms,
                     khal_sem_t done, uint32_t gen, uint32_t *ack,
                     const kdrv_rfid_lf_job_t *job, bool carrier)
{
    if ( busy_for(me, kind) )
    {
        return false;
    }
    if ( !rfid_job_start(me, job) )
    {
        return false;
    }
    me->req.kind = kind;
    me->req.done = done;
    me->req.gen  = gen;
    me->req.ack  = ack;
    me->req.ask  = NULL;
    me->req.late = false;
    me->field    = carrier;
    deadline_arm(me, ms + RFID_DEADLINE_SLACK_MS);
    return true;
}

/**
 * @brief Publish one decoded EM4100 identity on the bus.
 *
 * @param me     The AO.
 * @param id     The 5-byte ID.
 * @param rf_div The bit rate it was read at.
 */
static void publish_tag(Rfid * const me, const uint8_t id[KDRV_RFID_LF_ID_BYTES], uint8_t rf_div)
{
    RfidTagEvt *ev = Q_NEW_X(RfidTagEvt, 0U, RFID_TAG_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a decoded tag is not published");
        return;
    }
    (void)memcpy(ev->id, id, sizeof(ev->id));
    ev->rf_div = rf_div;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Record the last decode, for the `rfid` snapshot.
 *
 * @param me     The AO.
 * @param id     The 5-byte ID.
 * @param rf_div The bit rate it was read at.
 */
static void note_last(Rfid * const me, const uint8_t id[KDRV_RFID_LF_ID_BYTES], uint8_t rf_div)
{
    me->last.have   = true;
    (void)memcpy(me->last.id, id, sizeof(me->last.id));
    me->last.rf_div = rf_div;
    me->last.at_ms  = khal_time_ms();
}

/* ---- The requests --------------------------------------------------------- */

/**
 * @brief Serve RFID_READ_REQ_SIG.
 *
 * Refused with KODE_DENIED for a zero @c ms, while the front end is busy(),
 * or when the worker will not take the job.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_read_req(Rfid * const me, RfidReqEvt const * const q)
{
    kdrv_rfid_lf_job_t job;

    if ( NULL != q->have )
    {
        *q->have = false;
    }
    if ( 0U == q->ms )
    {
        req_refuse(q->done, q->gen, q->ack);
        console_answer(me, q->who, false, (uint32_t)KODE_DENIED, NULL, 0U);
        op_ask_answer(q->ask, q->rsp, false, (uint32_t)KODE_DENIED, NULL, 0U);
        return;
    }

    (void)memset(&job, 0, sizeof(job));
    job.type = KDRV_RFID_JOB_READ;
    job.ms   = q->ms;
    if ( !req_take(me, REQ_READ, q->ms, q->done, q->gen, q->ack, &job, true) )
    {
        req_refuse(q->done, q->gen, q->ack);
        console_answer(me, q->who, false, (uint32_t)KODE_DENIED, NULL, 0U);
        op_ask_answer(q->ask, q->rsp, false, (uint32_t)KODE_DENIED, NULL, 0U);
        return;
    }
    me->req.id     = q->id;
    me->req.rf_div = q->rf_div;
    me->req.have   = q->have;
    me->req.who    = q->who;
    me->req.ask    = q->ask;
    me->req.op     = q->rsp;
}

/**
 * @brief Serve RFID_WRITE_REQ_SIG.
 *
 * Refused with KODE_DENIED for a NULL @c id or a zero @c ms, while the front
 * end is busy(), or when the worker will not take the job.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_write_req(Rfid * const me, RfidWriteReqEvt const * const q)
{
    kdrv_rfid_lf_job_t job;

    if ( NULL != q->ok )
    {
        *q->ok = false;
    }
    if ( (NULL == q->id) || (0U == q->ms) )
    {
        req_refuse(q->done, q->gen, q->ack);
        console_answer(me, q->who, true, (uint32_t)KODE_DENIED, NULL, 0U);
        op_ask_answer(q->ask, q->rsp, true, (uint32_t)KODE_DENIED, NULL, 0U);
        return;
    }

    (void)memset(&job, 0, sizeof(job));
    job.type = KDRV_RFID_JOB_CLONE;
    job.ms   = q->ms;
    (void)memcpy(job.id, q->id, sizeof(job.id));
    if ( !req_take(me, REQ_WRITE, q->ms, q->done, q->gen, q->ack, &job,
                  true) )
    {
        req_refuse(q->done, q->gen, q->ack);
        console_answer(me, q->who, true, (uint32_t)KODE_DENIED, NULL, 0U);
        op_ask_answer(q->ask, q->rsp, true, (uint32_t)KODE_DENIED, NULL, 0U);
        return;
    }
    (void)memcpy(me->req.wid, q->id, sizeof(me->req.wid));
    me->req.ok  = q->ok;
    me->req.who = q->who;
    me->req.ask = q->ask;
    me->req.op  = q->rsp;
}

/**
 * @brief Serve RFID_FIELD_REQ_SIG: hand a carrier-off window to the worker,
 *        or refuse it at once.
 *
 * Refused with KODE_ERROR for a zero @c ms, and with KODE_DENIED while busy()
 * or when the worker will not take the job. A refusal reaches an AO as a
 * RfidFieldRspEvt and a blocked task through its semaphore, @c rc holding the
 * same status. A window over KDRV_RFID_LF_CAPTURE_MS_MAX is capped. The
 * carrier is never raised.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_capture_req(Rfid * const me, RfidFieldEvt const * const q)
{
    kdrv_rfid_lf_job_t job;
    kode_status_t      st    = KODE_DENIED;
    uint32_t           ms    = q->ms;
    /* The one place the event's untyped statistics are given their type. */
    /* cppcheck-suppress misra-c2012-11.5 ; the event carries the statistics untyped (kqp_signals.h cannot name the type) */
    krfid_field_t     *stats = (krfid_field_t *)q->out;
    size_t            *n     = q->n;

    /* One route per request: its makers set the waiter, the ask or the console, never two. */
    Q_ASSERT((NULL == q->ask) || ((NULL == q->done) && (NULL == q->to)));
    if ( KDRV_RFID_LF_CAPTURE_MS_MAX < ms )
    {
        ms = KDRV_RFID_LF_CAPTURE_MS_MAX;
    }

    if ( 0U == ms )
    {
        st = KODE_ERROR;
    }
    else
    {
        /* An Active Object that lends no memory is answered by value, so the
         * worker is lent this AO's own instead. */
        if ( NULL == stats )
        {
            stats = &me->stats;
        }
        if ( NULL == n )
        {
            n = &me->points;
        }
        (void)memset(&job, 0, sizeof(job));
        job.type  = KDRV_RFID_JOB_CAPTURE;
        job.ms    = ms;
        job.field = stats;
        job.wave  = q->wave;
        job.cap   = q->cap;
        job.n     = n;
        if ( req_take(me, REQ_CAPTURE, ms, q->done, q->gen, q->ack, &job,
                     false) )
        {
            st = KODE_OK;
        }
    }
    if ( KODE_OK != st )
    {
        if ( NULL != q->rc )
        {
            *q->rc = (uint32_t)st;
        }
        field_rsp_post(me, q->to, st, NULL, 0U);
        req_refuse(q->done, q->gen, q->ack);
        if ( NULL != q->ask )
        {
            field_pack(q->rsp, st, NULL, 0U);
            kaos_ask_answer(&s_rfid_doors[RFID_ASK_CAPTURE], q->ask, &q->rsp->super, AO_Rfid);
        }
        return;
    }
    me->req.stats = stats;
    me->req.n     = n;
    me->req.rc    = q->rc;
    me->req.who   = q->to;
    me->req.ask   = q->ask;
    me->req.fld    = q->rsp;
}

/**
 * @brief Answer an emulation ask that was refused, KODE_DENIED.
 *
 * @param q    The request; a waiter's (no ask) is a no-op.
 * @param stop Whether it was a stop.
 */
static void emu_ask_refuse(RfidEmuEvt const * const q, bool stop)
{
    /* One route per request: its makers set the waiter or the ask, never two. */
    Q_ASSERT((NULL == q->ask) || (NULL == q->done));
    if ( NULL != q->ask )
    {
        q->rsp->rc   = (uint8_t)KODE_DENIED;
        q->rsp->stop = stop;
        kaos_ask_answer(&s_rfid_doors[RFID_ASK_EMU], q->ask, &q->rsp->super, AO_Rfid);
    }
}

/**
 * @brief Serve RFID_EMU_REQ_SIG; KODE_DENIED when busy().
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_emulate_req(Rfid * const me, RfidEmuEvt const * const q)
{
    kdrv_rfid_lf_job_t job;

    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)KODE_DENIED;
    }
    (void)memset(&job, 0, sizeof(job));
    job.type = KDRV_RFID_JOB_EMULATE;
    (void)memcpy(job.id, q->id, sizeof(job.id));
    if ( !req_take(me, REQ_EMULATE, RFID_EMU_START_MS, q->done, q->gen, q->ack,
                  &job, false) )
    {
        req_refuse(q->done, q->gen, q->ack);
        emu_ask_refuse(q, false);
        return;
    }
    me->req.rc  = q->rc;
    me->req.ask = q->ask;
    me->req.emu = q->rsp;
    me->emu_ask = q->ask;
}

/**
 * @brief Serve RFID_EMU_STOP_REQ_SIG; KODE_DENIED when none plays, it is held,
 *        or the stop is not from the emulation's opener.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void on_emu_stop_req(Rfid * const me, RfidEmuEvt const * const q)
{
    kdrv_rfid_lf_job_t job;

    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)KODE_DENIED;
    }
    (void)memset(&job, 0, sizeof(job));
    job.type = KDRV_RFID_JOB_EMU_STOP;
    if ( (!atomic_load(&s_emulating)) || (q->ask != me->emu_ask)
        || (!req_take(me, REQ_EMU_STOP, RFID_EMU_STOP_MS, q->done, q->gen,
                      q->ack, &job, false)) )
    {
        req_refuse(q->done, q->gen, q->ack);
        emu_ask_refuse(q, true);
        return;
    }
    me->req.rc  = q->rc;
    me->req.ask = q->ask;
    me->req.emu = q->rsp;
}

/* ---- The outcome ---------------------------------------------------------- */

/**
 * @brief A read finished: fill the requester and, on a decode, publish.
 *
 * @param me The AO.
 * @param e  The outcome.
 */
static void read_done(Rfid * const me, RfidOutcome const * const e)
{
    Req * const r = &me->req;

    if ( e->ok )
    {
        me->reads += 1U;
        note_last(me, e->id, e->rf_div);
        publish_tag(me, e->id, e->rf_div);
        if ( NULL != r->id )
        {
            (void)memcpy(r->id, e->id, KDRV_RFID_LF_ID_BYTES);
        }
        if ( NULL != r->rf_div )
        {
            *r->rf_div = e->rf_div;
        }
        if ( NULL != r->have )
        {
            *r->have = true;
        }
    }
    else
    {
        me->windows += 1U;
        if ( NULL != r->have )
        {
            *r->have = false;
        }
    }
    if ( e->ok )
    {
        console_answer(me, r->who, false, (uint32_t)KODE_OK, e->id, e->rf_div);
        op_ask_answer(r->ask, r->op, false, (uint32_t)KODE_OK, e->id, e->rf_div);
    }
    else
    {
        console_answer(me, r->who, false, (uint32_t)KODE_TIMEOUT, NULL, 0U);
        op_ask_answer(r->ask, r->op, false, (uint32_t)KODE_TIMEOUT, NULL, 0U);
    }
    req_release(me);
}

/**
 * @brief A clone finished: the requester is told whether the read-back matched.
 *
 * @param me The AO.
 * @param e  The outcome.
 */
static void write_done(Rfid * const me, RfidOutcome const * const e)
{
    Req * const r = &me->req;
    bool        matched = e->ok
                          && (memcmp(e->id, r->wid, sizeof(r->wid)) == 0);

    if ( matched )
    {
        me->writes += 1U;
        note_last(me, e->id, e->rf_div);
        KLOG_N(TAG, "clone read back %02X %02X %02X %02X %02X - match",
               (unsigned)e->id[0], (unsigned)e->id[1], (unsigned)e->id[2],
               (unsigned)e->id[3], (unsigned)e->id[4]);
    }
    else
    {
        KLOG_W(TAG, "clone read-back did not match the ID written");
    }
    if ( NULL != r->ok )
    {
        *r->ok = matched;
    }
    if ( matched )
    {
        console_answer(me, r->who, true, (uint32_t)KODE_OK, r->wid, 0U);
        op_ask_answer(r->ask, r->op, true, (uint32_t)KODE_OK, r->wid, 0U);
    }
    else
    {
        console_answer(me, r->who, true, (uint32_t)KODE_TIMEOUT, r->wid, 0U);
        op_ask_answer(r->ask, r->op, true, (uint32_t)KODE_TIMEOUT, r->wid, 0U);
    }
    req_release(me);
}

/**
 * @brief A capture ended: tell the requester what it came to, post the
 *        statistics to an Active Object that asked, and let the caller go.
 *
 * On the done event the job has already written the statistics, the waveform
 * and the point count. A failure carries no statistics and is KODE_ERROR. The
 * deadline ends a capture with @p ok false; the worker may still write what it
 * was lent, and @c job keeps every later request out until it reports.
 *
 * @param me The AO.
 * @param ok Whether the worker read the window.
 */
static void capture_done(Rfid * const me, bool ok)
{
    Req * const   r  = &me->req;
    kode_status_t st = KODE_ERROR;

    if ( ok )
    {
        st = KODE_OK;
    }
    /* Before req_release(), whose write of @c ack orders it against the
     * requester on the other core. */
    if ( NULL != r->rc )
    {
        *r->rc = (uint32_t)st;
    }
    if ( ok )
    {
        field_rsp_post(me, r->who, st, r->stats, *r->n);
    }
    else
    {
        field_rsp_post(me, r->who, st, NULL, 0U);
    }
    if ( NULL != r->ask )
    {
        QActive * const who = r->ask;

        /* A deadline passed while the worker held the lent wave: the answer
         * waited for it, and says so. */
        if ( r->late )
        {
            st = KODE_TIMEOUT;
        }
        if ( ok && (!r->late) )
        {
            field_pack(r->fld, st, r->stats, *r->n);
        }
        else
        {
            field_pack(r->fld, st, NULL, 0U);
        }
        r->ask = NULL;
        kaos_ask_answer(&s_rfid_doors[RFID_ASK_CAPTURE], who, &r->fld->super, AO_Rfid);
    }
    req_release(me);
}

/**
 * @brief Answer an EMULATE or EMU_STOP request.
 *
 * @param me The AO.
 * @param st The status to give.
 */
static void emu_done(Rfid * const me, kode_status_t st)
{
    if ( NULL != me->req.rc )
    {
        *me->req.rc = (uint32_t)st;
    }
    if ( NULL != me->req.ask )
    {
        QActive * const who = me->req.ask;

        me->req.ask       = NULL;
        me->req.emu->rc   = (uint8_t)st;
        me->req.emu->stop = REQ_EMU_STOP == me->req.kind;
        kaos_ask_answer(&s_rfid_doors[RFID_ASK_EMU], who, &me->req.emu->super, AO_Rfid);
    }
    req_release(me);
}

/**
 * @brief Handle RFID_WORKER_DONE_SIG: the job has ended and dropped its
 *        carrier, so the front end is free; then end the request the outcome
 *        is for, if a deadline has not already.
 *
 * An event that is not the current job's, or arrives with no job in flight, is
 * dropped. A job and a certification carrier refuse each other, so the
 * @c field cleared here is always the job's.
 *
 * @param me The AO.
 * @param e  The done event.
 */
static void on_worker_done(Rfid * const me, KqpWorkerDoneEvt const * const e)
{
    RfidOutcome       o;
    kode_status_t const st = (kode_status_t)e->status;

    if ( (!me->job) || (e->seq != me->job_seq) )
    {
        KLOG_D(TAG, "worker answer seq %u is not the job's (%u); dropped",
               (unsigned)e->seq, (unsigned)me->job_seq);
        return;
    }
    me->job   = false;
    me->field = false;

    o.ok     = (KODE_OK == st);
    o.broken = (KODE_OK != st) && (KODE_TIMEOUT != st);
    o.id     = me->wjob.id;
    o.rf_div = me->wjob.rf_div;
    o.st     = st;

    /* The flag follows the job, whether or not a request still waits for it. */
    if ( KDRV_RFID_JOB_EMULATE == me->wjob.job.type )
    {
        if ( o.ok )
        {
            atomic_store(&s_emulating, true);
        }
        else
        {
            me->emu_ask = NULL;
        }
    }
    else if ( KDRV_RFID_JOB_EMU_STOP == me->wjob.job.type )
    {
        atomic_store(&s_emulating, false);
        me->emu_ask = NULL;
    }
    else
    {
        /* A window, a clone or a capture leaves the loop as it was. */
    }

    switch ( me->req.kind )
    {
        case REQ_ROW: {
            /* One deadline covers the whole row, so it stays armed. */
            row_on_window(me, &o);
            break;
        }
        case REQ_READ: {
            deadline_stop(me);
            read_done(me, &o);
            break;
        }
        case REQ_WRITE: {
            deadline_stop(me);
            write_done(me, &o);
            break;
        }
        case REQ_CAPTURE: {
            deadline_stop(me);
            capture_done(me, o.ok);
            break;
        }
        case REQ_EMULATE:
        case REQ_EMU_STOP: {
            deadline_stop(me);
            emu_done(me, o.st);
            break;
        }
        default: {
            /* No live request: the deadline already answered it. The outcome
             * is discarded, never given to the next requester. */
            break;
        }
    }

    /* The worker has taken the carrier down. With nothing to follow - no
     * row's next window, no new request, no held carrier (busy() covers
     * all three) - RFID_PULL goes back to rest, where R11 stops sinking
     * its 0.33 mA. The next use pulls it low again first. */
    if ( !busy(me) )
    {
        (void)kdrv_rfid_lf_rest();
    }
}

/* ---- The production row --------------------------------------------------- */

/**
 * @brief Publish a verdict of `test component=rfid`, and touch nothing else.
 *
 * @param me The AO.
 * @param ok Whether it passed.
 */
static void row_verdict(Rfid * const me, bool ok)
{
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the RFID verdict is lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", "rfid");
    ev->ok = ok;
    ev->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief End the running row: retire its request and its deadline, and
 *        publish its verdict.
 *
 * @c job and @c field are the worker's outcome's to clear, not this, so a
 * row ended by its deadline leaves the front end refused until the worker
 * reports.
 *
 * @param me The AO.
 * @param ok Whether it passed.
 */
static void row_finish(Rfid * const me, bool ok)
{
    me->req.kind = REQ_NONE;
    deadline_stop(me);
    row_verdict(me, ok);
}

/**
 * @brief End the running row with nothing judged: no card was held to the
 *        reader. Retires what row_finish() retires.
 *
 * @param me The AO.
 */
static void rfid_row_skip(Rfid * const me)
{
    TestResultEvt *ev;

    me->req.kind = REQ_NONE;
    deadline_stop(me);
    ev = Q_NEW_X(TestResultEvt, 0U, TEST_SKIP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the RFID row's answer is lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", "rfid");
    ev->ok = false;
    ev->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Submit the row's next read window.
 *
 * Called last wherever it is called, row_on_window() included: the outcome
 * being taken is read from @c wjob, which the next job is about to use, so
 * nothing may read it once that job is with the worker.
 *
 * @param me The AO.
 */
static void row_submit(Rfid * const me)
{
    kdrv_rfid_lf_job_t job;

    (void)memset(&job, 0, sizeof(job));
    job.type = KDRV_RFID_JOB_READ;
    job.ms   = KODE_RFID_TEST_WINDOW_MS;
    if ( !rfid_job_start(me, &job) )
    {
        KLOG_E(TAG, "row: the worker is busy");
        row_finish(me, false);
        return;
    }
    me->field = true;
}

/**
 * @brief One row window closed: judge it, then go again or give a verdict.
 *
 * A stable EM4100 ID decoded KODE_RFID_TEST_TRIES windows in a row is the pass;
 * KODE_RFID_TEST_WINDOWS windows without one is the fail. Readings are logged
 * before the verdict, as in every row of the house. The row owns the front end
 * while it runs, so nothing is published on RFID_TAG_SIG - only the verdict.
 *
 * @param me The AO.
 * @param e  The window's outcome.
 */
static void row_on_window(Rfid * const me, RfidOutcome const * const e)
{
    me->row.attempts += 1U;

    if ( READER_TEST_FIELD == me->row.mode )
    {
        /* The owner's row with nothing in front of the reader: the front
         * end energised and the worker brought a whole window back. What
         * the window held is not judged - there is no tag to decode. */
        if ( e->broken )
        {
            KLOG_E(TAG, "row: the front end would not read a window at all");
            row_finish(me, false);
            return;
        }
        KLOG_N(TAG, "row: the front end energised and read one window; "
                    "nothing is in front of it to decode");
        row_finish(me, true);
        return;
    }

    if ( e->ok )
    {
        me->reads += 1U;
        note_last(me, e->id, e->rf_div);
        KLOG_N(TAG, "row: window %u decoded %02X %02X %02X %02X %02X rf=%u",
               (unsigned)me->row.attempts,
               (unsigned)e->id[0], (unsigned)e->id[1], (unsigned)e->id[2],
               (unsigned)e->id[3], (unsigned)e->id[4], (unsigned)e->rf_div);
        if ( me->row.have_last
            && (memcmp(e->id, me->row.last_id, sizeof(me->row.last_id)) == 0) )
        {
            me->row.streak += 1U;
        }
        else
        {
            me->row.streak    = 1U;
            me->row.have_last = true;
            (void)memcpy(me->row.last_id, e->id, sizeof(me->row.last_id));
        }
        if ( KODE_RFID_TEST_TRIES <= me->row.streak )
        {
            KLOG_N(TAG, "row: %u identical decodes - pass",
                   (unsigned)me->row.streak);
            row_finish(me, true);
            return;
        }
    }
    else
    {
        me->windows      += 1U;
        me->row.streak    = 0U;
        me->row.have_last = false;
        KLOG_N(TAG, "row: window %u flat", (unsigned)me->row.attempts);
    }

    if ( KODE_RFID_TEST_WINDOWS <= me->row.attempts )
    {
        if ( READER_TEST_CARD == me->row.mode )
        {
            KLOG_N(TAG, "row: no card was held to the reader; nothing to judge");
            rfid_row_skip(me);
            return;
        }
        KLOG_W(TAG, "row: %u windows without a stable ID - fail",
               (unsigned)KODE_RFID_TEST_WINDOWS);
        row_finish(me, false);
        return;
    }
    row_submit(me);
}

/**
 * @brief Start `test component=rfid`.
 *
 * A busy() front end fails the row at once and leaves whatever holds it - a
 * request in flight, its deadline, the worker's job - exactly as it was: the
 * refusal is a verdict and nothing else.
 *
 * @param me The AO.
 */
static void row_start(Rfid * const me, uint8_t mode)
{
    if ( busy(me) )
    {
        KLOG_W(TAG, "row: the front end is busy");
        row_verdict(me, false);
        return;
    }

    me->row.attempts  = 0U;
    me->row.streak    = 0U;
    me->row.have_last = false;
    me->row.mode      = mode;
    me->req.kind      = REQ_ROW;

    /* One whole-row backstop, not one per window: the verdict comes inside
     * the budget even from a worker that wedges. */
    deadline_arm(me, KODE_RFID_TEST_BUDGET_MS);
    if ( READER_TEST_FIELD == mode )
    {
        KLOG_N(TAG, "row: energising the front end for one read window");
    }
    else
    {
        KLOG_N(TAG, "row: reading up to %u windows for a stable EM4100 ID",
               (unsigned)KODE_RFID_TEST_WINDOWS);
    }
    row_submit(me);
}

/* ---- The FCC certification carrier (cert build only) ---------------------- */

#if CONFIG_KODEOS_NFC_CERT

/**
 * @brief Hand a certification request its outcome and let its caller go.
 *
 * @c ack is written before @c done is signalled, so a caller woken by a token
 * it did not earn keeps waiting.
 *
 * @param q  The request.
 * @param rc The outcome.
 */
static void cert_answer(RfidCertEvt const * const q, kode_status_t rc)
{
    /* One route per request: its makers set the waiter, the ask or the console, never two. */
    Q_ASSERT((NULL == q->ask) || (NULL == q->done));
    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)rc;
    }
    if ( NULL != q->ack )
    {
        *q->ack = q->gen;
    }
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }
    if ( NULL != q->ask )
    {
        q->rsp->rc = (uint8_t)rc;
        kaos_ask_answer(&s_rfid_doors[RFID_ASK_CERT], q->ask, &q->rsp->super, AO_Rfid);
    }
}

/**
 * @brief Raise, switch or drop the held maximum-drive 125 kHz field
 *        (RFID_CERT_REQ_SIG).
 *
 * No window and no worker job: held at boosted maximum drive, straight from
 * the driver, until a stop, for a chamber measurement. Held as REQ_CERT, so
 * other requests are refused while it is up. Answered in this step; success
 * means the carrier was commanded, not that RF radiated. Every mode, a stop
 * included, is refused busy while a request or a worker job holds the front
 * end: a stop would free the ADC and buffers under the worker's window.
 *
 * @param me The AO.
 * @param q  The request: its @c mode and its answer channel.
 */
static void cert_serve(Rfid * const me, RfidCertEvt const * const q)
{
    uint8_t mode = q->mode;

    if ( me->job || atomic_load(&s_emulating)
        || ((REQ_NONE != me->req.kind) && (REQ_CERT != me->req.kind)) )
    {
        /* The worker may be inside its ADC window right now. No certification
         * carrier is held meanwhile: raising one is refused the same way. */
        if ( 0U == mode )
        {
            KLOG_W(TAG, "certification carrier off refused: the front end is busy");
        }
        else
        {
            KLOG_W(TAG, "certification carrier refused: the front end is busy");
        }
        cert_answer(q, KODE_DENIED);
        return;
    }

    if ( 0U == mode )
    {
        /* Only a held certification carrier, or nothing, is left to drop, and
         * neither arms a deadline. */
        (void)kdrv_rfid_lf_field_off();
        (void)kdrv_rfid_lf_rest();   /* nothing uses the receiver now */
        me->field    = false;
        me->req.kind = REQ_NONE;
        KLOG_N(TAG, "certification carrier off");
        cert_answer(q, KODE_OK);
        return;
    }

    /* The driver takes any prior carrier down itself before it raises the next
     * one - joining a running OOK loop and re-boosting the rail - so switching
     * carrier<->modulated needs no field_off() here. */
    bool modulated = (2U == mode);
    if ( kdrv_rfid_lf_cert_carrier(modulated) != KODE_OK )
    {
        (void)kdrv_rfid_lf_field_off();
        /* The try drove RFID_PULL low on its way to the carrier. */
        (void)kdrv_rfid_lf_rest();
        me->field    = false;
        me->req.kind = REQ_NONE;
        KLOG_E(TAG, "the RFID certification carrier would not come up");
        cert_answer(q, KODE_ERROR);
        return;
    }
    me->req.kind = REQ_CERT;
    me->field    = true;
    cert_answer(q, KODE_OK);
}

#endif /* CONFIG_KODEOS_NFC_CERT */

/* ---- The console's question ----------------------------------------------- */

/**
 * @brief Answer RFID_INFO_REQ_SIG with everything `rfid` prints.
 *
 * @param me  The AO.
 * @param who Where the answer goes.
 */
static void info_serve(Rfid * const me, QActive *who)
{
    RfidInfoEvt *ev;

    if ( NULL == who )
    {
        return;
    }
    ev = Q_NEW_X(RfidInfoEvt, 0U, RFID_INFO_RSP_SIG);
    if ( NULL == ev )
    {
        return;
    }

    ev->field   = me->field;
    ev->have    = me->last.have;
    ev->rf_div  = 0U;
    ev->age_ms  = 0U;
    ev->reads   = me->reads;
    ev->writes  = me->writes;
    ev->windows = me->windows;
    (void)memset(ev->id, 0, sizeof(ev->id));
    if ( me->last.have )
    {
        (void)memcpy(ev->id, me->last.id, sizeof(ev->id));
        ev->rf_div = me->last.rf_div;
        ev->age_ms = khal_time_ms() - me->last.at_ms;
    }
    QACTIVE_POST(who, &ev->super, &me->super);
}

/* ---- The AO --------------------------------------------------------------- */

void Rfid_ctor(void)
{
    Rfid * const me = &l_rfid;

    QActive_ctor(&me->super, Q_STATE_CAST(&Rfid_initial));
    QTimeEvt_ctorX(&me->deadline, &me->super, RFID_DEADLINE_SIG, 0U);
    me->dl_stale = 0U;
    me->dl_live  = false;
    atomic_store(&s_emulating, false);
    me->emu_ask  = NULL;
    me->job      = false;
    me->job_seq  = 0U;
    (void)memset(&me->worker, 0, sizeof(me->worker));
    (void)memset(&me->wjob, 0, sizeof(me->wjob));
    me->field    = false;
    me->reads    = 0U;
    me->writes   = 0U;
    me->windows  = 0U;
    (void)memset(&me->stats, 0, sizeof(me->stats));
    me->points   = 0U;
    (void)memset(&me->req, 0, sizeof(me->req));
    me->row.attempts  = 0U;
    me->row.streak    = 0U;
    me->row.have_last = false;
    me->last.have     = false;
    me->last.at_ms    = 0U;
#if CONFIG_KODE_LOWPOWER
    me->lp_who        = KODE_LOWPOWER_WHO_NONE;
#endif
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief LOWPOWER_ROUND_SIG: the receiver's pull released, and answer once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void rfid_round(Rfid const * const me, const LowPowerRoundEvt * const r)
{
    if ( busy(me) )
    {
        lowpower_ready(me->lp_who, r->round, true, "rfid busy");
    }
    else
    {
        (void)kdrv_rfid_lf_rest();
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

AO_STORAGE(rfid, KQP_AO_QUEUE_LEN, KQP_AO_STACK_SMALL);   /* the blocking work runs on its worker */

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_rfid_register() ---------------- */

/** @brief The RFID row: AO_Rfid runs it and answers TEST_RESULT_SIG. */
static bool rfid_row_rfid(void)
{
    rfid_test_request();
    return false;
}

/** @brief The 125 kHz front end with nothing in front of it: the carrier
 *         comes on and the receive path sees it. */
static bool rfid_row_field(void)
{
    rfid_test_request_mode(READER_TEST_FIELD);
    return false;
}

/** @brief A 125 kHz card held to the back: an ID that reads is a pass, and
 *         none is nothing to judge. */
static bool rfid_row_card(void)
{
    rfid_test_request_mode(READER_TEST_CARD);
    return false;
}
#endif

void kaos_rfid_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t RFID_ROWS[] = {
        { "rfid", AUTOTEST_ID_RFID, rfid_row_rfid, KROW_ASYNC, AUTOTEST_G_NONE, KODE_RFID_TEST_BUDGET_MS,
          NULL, 0U, "125 kHz reader",
          "Reader not working",
          AUTOTEST_ORDER_RFID },
        { "rfid_field", AUTOTEST_ID_RFID_FIELD, rfid_row_field, KROW_ASYNC,
          AUTOTEST_G_NONE, KODE_RFID_TEST_BUDGET_MS,
          "rfid", 0U, "125 kHz reader",
          "Reader not working",
          AUTOTEST_ORDER_RFID_FIELD },
        { "rfid_card", AUTOTEST_ID_RFID_CARD, rfid_row_card, KROW_ASYNC,
          AUTOTEST_G_NONE, KODE_RFID_TEST_BUDGET_MS,
          "rfid", 0U, "125 kHz card read",
          "Card not read",
          AUTOTEST_ORDER_RFID_CARD },
    };

    kaos_require(krows_add(RFID_ROWS, sizeof(RFID_ROWS) / sizeof(RFID_ROWS[0])),
                 "rfid rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&rfid_verbs), "rfid verbs");
#endif
}

void kaos_rfid_start(void)
{
    Rfid_ctor();
    kaos_rfid_register();
    AO_START(AO_Rfid, rfid, KODE_PRIO_RFID);
}

/** @brief Initial transition: claim the front end, start the worker, go ready. */
static QState Rfid_initial(Rfid * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_RFID, me, "AO_Rfid");
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "rfid");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif

    (void)kdrv_rfid_lf_init();
    kdrv_rfid_lf_set_worker_check(&rfid_in_worker);
    if ( kqp_worker_start(&me->worker, &me->super, "rfid_worker",
                         RFID_WORKER_STACK, KODE_PRIO_RFID, RFID_WORKER_DEPTH,
                         true) != KODE_OK )
    {
        KLOG_E(TAG, "the rfid worker would not start; the front end stays down");
    }
    KLOG_I(TAG, "up; field down until somebody asks");
    return Q_TRAN(&Rfid_ready);
}

/** @brief The AO's only state: every request for the front end is served here. */
static QState Rfid_ready(Rfid * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case RFID_READ_REQ_SIG: {
            on_read_req(me, Q_EVT_CAST(RfidReqEvt));
            status = Q_HANDLED();
            break;
        }
        case RFID_WRITE_REQ_SIG: {
            on_write_req(me, Q_EVT_CAST(RfidWriteReqEvt));
            status = Q_HANDLED();
            break;
        }
        case RFID_FIELD_REQ_SIG: {
            on_capture_req(me, Q_EVT_CAST(RfidFieldEvt));
            status = Q_HANDLED();
            break;
        }
        case RFID_EMU_REQ_SIG: {
            on_emulate_req(me, Q_EVT_CAST(RfidEmuEvt));
            status = Q_HANDLED();
            break;
        }
        case RFID_EMU_STOP_REQ_SIG: {
            on_emu_stop_req(me, Q_EVT_CAST(RfidEmuEvt));
            status = Q_HANDLED();
            break;
        }
        case RFID_WORKER_DONE_SIG: {
            on_worker_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            status = Q_HANDLED();
            break;
        }
        case RFID_DEADLINE_SIG: {
            if ( 0U < me->dl_stale )
            {
                me->dl_stale -= 1U;
            }
            else
            {
                /* The request is answered here; the worker's job is not
                 * over. @c job and @c field stay as they are until its
                 * outcome arrives, and that outcome answers nobody. */
                me->dl_live = false;
                if ( REQ_ROW == me->req.kind )
                {
                    KLOG_W(TAG, "row: no worker answer inside the budget; "
                                "busy until the worker reports");
                    row_finish(me, false);
                }
                else if ( REQ_READ == me->req.kind )
                {
                    me->windows += 1U;
                    if ( NULL != me->req.have )
                    {
                        *me->req.have = false;
                    }
                    console_answer(me, me->req.who, false,
                                   (uint32_t)KODE_TIMEOUT, NULL, 0U);
                    op_ask_answer(me->req.ask, me->req.op, false,
                                  (uint32_t)KODE_TIMEOUT, NULL, 0U);
                    req_release(me);
                }
                else if ( REQ_WRITE == me->req.kind )
                {
                    if ( NULL != me->req.ok )
                    {
                        *me->req.ok = false;
                    }
                    console_answer(me, me->req.who, true,
                                   (uint32_t)KODE_TIMEOUT, NULL, 0U);
                    op_ask_answer(me->req.ask, me->req.op, true,
                                  (uint32_t)KODE_TIMEOUT, NULL, 0U);
                    req_release(me);
                }
                else if ( REQ_CAPTURE == me->req.kind )
                {
                    KLOG_W(TAG, "capture: no worker answer inside the "
                                "deadline; busy until the worker reports");
                    if ( NULL != me->req.ask )
                    {
                        /* The worker may still write the asker's wave: its
                         * answer waits for the worker to return. */
                        me->req.late = true;
                    }
                    else
                    {
                        capture_done(me, false);
                    }
                }
                else if ( (REQ_EMULATE == me->req.kind)
                         || (REQ_EMU_STOP == me->req.kind) )
                {
                    KLOG_W(TAG, "emulation: no worker answer inside the "
                                "deadline; busy until the worker reports");
                    if ( NULL != me->req.ask )
                    {
                        /* The loop may still come up, or still stop: an ask
                         * is told what the job came to, once it returns. */
                        me->req.late = true;
                    }
                    else
                    {
                        emu_done(me, KODE_TIMEOUT);
                    }
                }
                else
                {
                    /* Nothing in flight: a stale expiry. */
                }
            }
            status = Q_HANDLED();
            break;
        }
        case RFID_TEST_REQ_SIG: {
            row_start(me, Q_EVT_CAST(ReaderTestReqEvt)->mode);
            status = Q_HANDLED();
            break;
        }
        case RFID_INFO_REQ_SIG: {
            RfidInfoReqEvt const * const q = Q_EVT_CAST(RfidInfoReqEvt);
            info_serve(me, q->who);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            rfid_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODEOS_NFC_CERT
        case RFID_CERT_REQ_SIG: {
            RfidCertEvt const * const q = Q_EVT_CAST(RfidCertEvt);
            cert_serve(me, q);
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/* ---- What the rest of kodeOS asks with ------------------------------------ */

void rfid_info_request(QActive *who)
{
    RfidInfoReqEvt *e = Q_NEW_X(RfidInfoReqEvt, 0U, RFID_INFO_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a status request is dropped");
        return;
    }
    e->who = who;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

void rfid_test_request_mode(uint8_t mode)
{
    static const ReaderTestReqEvt ROW   = {
        QEVT_INITIALIZER(RFID_TEST_REQ_SIG), READER_TEST_ROW
    };
    static const ReaderTestReqEvt FIELD = {
        QEVT_INITIALIZER(RFID_TEST_REQ_SIG), READER_TEST_FIELD
    };
    static const ReaderTestReqEvt CARD  = {
        QEVT_INITIALIZER(RFID_TEST_REQ_SIG), READER_TEST_CARD
    };

    if ( READER_TEST_FIELD == mode )
    {
        QACTIVE_POST(AO_Rfid, &FIELD.super, NULL);
    }
    else if ( READER_TEST_CARD == mode )
    {
        QACTIVE_POST(AO_Rfid, &CARD.super, NULL);
    }
    else
    {
        QACTIVE_POST(AO_Rfid, &ROW.super, NULL);
    }
}

void rfid_test_request(void)
{
    rfid_test_request_mode(READER_TEST_ROW);
}

#if CONFIG_KODEOS_NFC_CERT
void rfid_cert_request(uint8_t mode, uint32_t *rc, khal_sem_t done,
                       uint32_t gen, uint32_t *ack)
{
    RfidCertEvt *e = Q_NEW_X(RfidCertEvt, 0U, RFID_CERT_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an RFID certification carrier request is dropped");
        /* The caller is blocked on @p done; release it rather than leave it to
         * wait out its whole timeout for an answer that will never come. */
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->mode = mode;
    e->rc   = rc;
    e->done = done;
    e->gen  = gen;
    e->ack  = ack;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

kode_status_t rfid_cert_ask(QActive *who, uint8_t mode)
{
    kode_status_t rc = kaos_ask_take(&s_rfid_doors[RFID_ASK_CERT], who);
    StatusRspEvt *a;
    RfidCertEvt  *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(StatusRspEvt, 0U, RFID_CERT_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_CERT], who, NULL);
    }
    e = Q_NEW_X(RfidCertEvt, 0U, RFID_CERT_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_CERT], who, &a->super);
    }
    e->mode = mode;
    e->rc   = NULL;
    e->done = NULL;
    e->gen  = 0U;
    e->ack  = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_rfid_doors[RFID_ASK_CERT], who, AO_Rfid, &e->super, &a->super,
                         KQP_RFID_CMD_MARGIN);
}
#endif /* CONFIG_KODEOS_NFC_CERT */

void rfid_read_request(uint8_t *id, uint8_t *rf_div, bool *have, khal_sem_t done,
                       uint32_t ms, uint32_t gen, uint32_t *ack)
{
    RfidReqEvt *e = Q_NEW_X(RfidReqEvt, 0U, RFID_READ_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an RFID read is dropped");
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->id     = id;
    e->rf_div = rf_div;
    e->have   = have;
    e->done   = done;
    e->gen    = gen;
    e->ack    = ack;
    e->ms     = ms;
    e->who    = NULL;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

void rfid_write_request(const uint8_t *id, bool *ok, khal_sem_t done,
                        uint32_t ms, uint32_t gen, uint32_t *ack)
{
    RfidWriteReqEvt *e = Q_NEW_X(RfidWriteReqEvt, 0U, RFID_WRITE_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an RFID clone is dropped");
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->id   = id;
    e->ok   = ok;
    e->done = done;
    e->gen  = gen;
    e->ack  = ack;
    e->ms   = ms;
    e->who  = NULL;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

void rfid_capture_request(void *out, uint16_t *wave, size_t cap, size_t *n,
                          uint32_t *rc, khal_sem_t done, uint32_t ms,
                          uint32_t gen, uint32_t *ack, QActive *to)
{
    RfidFieldEvt *e = Q_NEW_X(RfidFieldEvt, 0U, RFID_FIELD_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; an RFID capture is dropped");
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        req_refuse(done, gen, ack);
        return;
    }
    e->out  = out;
    e->wave = wave;
    e->cap  = cap;
    e->n    = n;
    e->rc   = rc;
    e->done = done;
    e->ms   = ms;
    e->gen  = gen;
    e->ack  = ack;
    e->to   = to;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

void rfid_read_console_request(QActive *who, uint32_t ms)
{
    RfidReqEvt *e = Q_NEW_X(RfidReqEvt, 0U, RFID_READ_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a console RFID read is dropped");
        return;
    }
    /* The result travels in the RfidOpRspEvt, not through caller pointers, so
     * the read outputs are NULL and there is no semaphore to wait on. */
    e->id     = NULL;
    e->rf_div = NULL;
    e->have   = NULL;
    e->done   = NULL;
    e->gen    = 0U;
    e->ack    = NULL;
    e->ms     = ms;
    e->who    = who;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

void rfid_clone_console_request(QActive *who, const uint8_t *id, uint32_t ms)
{
    RfidWriteReqEvt *e = Q_NEW_X(RfidWriteReqEvt, 0U, RFID_WRITE_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a console RFID clone is dropped");
        return;
    }
    /* @p id must stay valid until this request is dispatched; the console runs
     * one command at a time, so its own buffer does. The answer is a
     * RfidOpRspEvt, so there is no @c ok pointer and no semaphore. */
    e->id   = id;
    e->ok   = NULL;
    e->done = NULL;
    e->gen  = 0U;
    e->ack  = NULL;
    e->ms   = ms;
    e->who  = who;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

void rfid_emulate_request(const uint8_t *id, uint32_t *rc, khal_sem_t done,
                          uint32_t gen, uint32_t *ack)
{
    RfidEmuEvt *e;

    if ( NULL == id )
    {
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_ERROR;
        }
        req_refuse(done, gen, ack);
        return;
    }
    e = Q_NEW_X(RfidEmuEvt, 0U, RFID_EMU_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a tag emulation request is dropped");
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        req_refuse(done, gen, ack);
        return;
    }
    (void)memcpy(e->id, id, sizeof(e->id));
    e->rc   = rc;
    e->done = done;
    e->gen  = gen;
    e->ack  = ack;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

void rfid_emulate_stop_request(uint32_t *rc, khal_sem_t done, uint32_t gen,
                               uint32_t *ack)
{
    RfidEmuEvt *e = Q_NEW_X(RfidEmuEvt, 0U, RFID_EMU_STOP_REQ_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a tag emulation stop is dropped");
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        req_refuse(done, gen, ack);
        return;
    }
    (void)memset(e->id, 0, sizeof(e->id));
    e->rc   = rc;
    e->done = done;
    e->gen  = gen;
    e->ack  = ack;
    e->ask = NULL;
    e->rsp = NULL;
    QACTIVE_POST(AO_Rfid, &e->super, NULL);
}

bool rfid_is_emulating(void)
{
    return atomic_load(&s_emulating);
}

/* ---- kode_sys's asks ------------------------------------------------------- */

kode_status_t rfid_read_ask(QActive *who, uint32_t ms)
{
    kode_status_t rc = kaos_ask_take(&s_rfid_doors[RFID_ASK_OP], who);
    RfidOpRspEvt *a;
    RfidReqEvt   *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(RfidOpRspEvt, 0U, RFID_OP_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_OP], who, NULL);
    }
    e = Q_NEW_X(RfidReqEvt, 0U, RFID_READ_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_OP], who, &a->super);
    }
    e->id     = NULL;
    e->rf_div = NULL;
    e->have   = NULL;
    e->done   = NULL;
    e->gen    = 0U;     /* an ask is answered by its event, not matched by a tag */
    e->ack    = NULL;
    e->ms     = ms;
    e->who    = NULL;
    e->ask    = who;
    e->rsp    = a;
    return kaos_ask_post(&s_rfid_doors[RFID_ASK_OP], who, AO_Rfid, &e->super, &a->super,
                         KQP_RFID_CMD_MARGIN);
}

kode_status_t rfid_write_ask(QActive *who, const uint8_t *id, uint32_t ms)
{
    kode_status_t    rc = kaos_ask_take(&s_rfid_doors[RFID_ASK_OP], who);
    RfidOpRspEvt    *a;
    RfidWriteReqEvt *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(RfidOpRspEvt, 0U, RFID_OP_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_OP], who, NULL);
    }
    e = Q_NEW_X(RfidWriteReqEvt, 0U, RFID_WRITE_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_OP], who, &a->super);
    }
    (void)memcpy(e->wid, id, sizeof(e->wid));
    e->id   = e->wid;
    e->ok   = NULL;
    e->done = NULL;
    e->gen  = 0U;
    e->ack  = NULL;
    e->ms   = ms;
    e->who  = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_rfid_doors[RFID_ASK_OP], who, AO_Rfid, &e->super, &a->super,
                         KQP_RFID_CMD_MARGIN);
}

/**
 * @brief The start and the stop of an emulation ask, which differ in the signal.
 * @param who The asking AO.
 * @param id  The ID to play, or NULL for a stop.
 * @return As rfid_emulate_ask().
 */
static kode_status_t rfid_emu_ask(QActive *who, const uint8_t *id)
{
    kode_status_t  rc = kaos_ask_take(&s_rfid_doors[RFID_ASK_EMU], who);
    RfidEmuRspEvt *a;
    RfidEmuEvt    *e;
    QSignal        sig = (QSignal)RFID_EMU_STOP_REQ_SIG;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( NULL != id )
    {
        sig = (QSignal)RFID_EMU_REQ_SIG;
    }
    a = Q_NEW_X(RfidEmuRspEvt, 0U, RFID_EMU_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_EMU], who, NULL);
    }
    e = Q_NEW_X(RfidEmuEvt, 0U, sig);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_EMU], who, &a->super);
    }
    (void)memset(e->id, 0, sizeof(e->id));
    if ( NULL != id )
    {
        (void)memcpy(e->id, id, sizeof(e->id));
    }
    e->rc   = NULL;
    e->done = NULL;
    e->gen  = 0U;
    e->ack  = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_rfid_doors[RFID_ASK_EMU], who, AO_Rfid, &e->super, &a->super,
                         KQP_RFID_CMD_MARGIN);
}

kode_status_t rfid_emulate_ask(QActive *who, const uint8_t *id)
{
    if ( NULL == id )
    {
        return KODE_DENIED;
    }
    return rfid_emu_ask(who, id);
}

kode_status_t rfid_emulate_stop_ask(QActive *who)
{
    return rfid_emu_ask(who, NULL);
}

kode_status_t rfid_field_ask(QActive *who, uint16_t *wave, size_t cap, uint32_t ms)
{
    kode_status_t    rc = kaos_ask_take(&s_rfid_doors[RFID_ASK_CAPTURE], who);
    RfidFieldRspEvt *a;
    RfidFieldEvt    *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(RfidFieldRspEvt, 0U, RFID_FIELD_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_CAPTURE], who, NULL);
    }
    e = Q_NEW_X(RfidFieldEvt, 0U, RFID_FIELD_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_rfid_doors[RFID_ASK_CAPTURE], who, &a->super);
    }
    e->out  = NULL;
    e->wave = wave;
    e->cap  = cap;
    e->n    = NULL;
    e->rc   = NULL;
    e->done = NULL;
    e->ms   = ms;
    e->gen  = 0U;
    e->ack  = NULL;
    e->to   = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_rfid_doors[RFID_ASK_CAPTURE], who, AO_Rfid, &e->super, &a->super,
                         KQP_RFID_CMD_MARGIN);
}

