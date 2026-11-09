/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ir_ao.c
 * @brief AO_Ir: the Active Object that owns the infrared block.
 */
#include "sdkconfig.h"
#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"

#include <stdio.h>
#include <string.h>

#include "kqp.h"
#include "kqp_worker.h"
#include "kaos_claim.h"
#include "kdrv_ir.h"
#include "khal_system.h"
#include "khal_time.h"
#include "knec.h"
#include "kode_log.h"
#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.ir"

/* cppcheck-suppress misra-c2012-5.9 ; QP's module name for Q_ASSERT: one static of that name per file is QP's design */
Q_DEFINE_THIS_MODULE("ir_ao")

/** @brief How often a wait for the C5 link looks for a held request past its budget. */
#define IR_LINK_CHECK_MS  100U

/** @brief The worker's task name; `ps` prints it. */
#define IR_WORKER_NAME   "ir_worker"

/** @brief The worker's stack, in bytes, in PSRAM. A peer send is one esp-hosted
 *         RPC and a few locals; an overflow corrupts a neighbour silently, so
 *         8 KB at least, whatever `ps` reads as free. */
#define IR_WORKER_STACK  8192U

/** @brief Jobs that may wait besides the one running: none is ever queued,
 *         because AO_Ir submits only while no call is out. */
#define IR_WORKER_DEPTH  1U

/** @brief Alignment of the capture buffer in PSRAM, in bytes. */
#define IR_SYM_ALIGN     4U

/** @brief How long a call to the C5 may be out before this AO stops waiting
 *         for it and tells whoever does that it failed: the longest the
 *         driver's call can legitimately take, and a margin. */
#define IR_JOB_DEADLINE_MS  (KDRV_IR_CALL_MAX_MS + KODE_IR_JOB_MARGIN_MS)

/** @brief Size of the capture buffer, in bytes. */
#define IR_SYM_BYTES     (KDRV_IR_RX_MAX_SYMBOLS * sizeof(kdrv_ir_symbol_t))

/** @brief Size of the raw frame's copy, in bytes. */
#define IR_TX_BYTES      (KDRV_IR_TX_MAX_SYMBOLS * sizeof(kdrv_ir_symbol_t))

/** @brief The one capture request AO_Ir will answer, copied out of its event.
 *
 * Copied because QF collects the event at the end of its step, and the request
 * waits for a frame. The pointers are the requester's, kept alive past its wait
 * (kaos.h); the deadline bounds how long AO_Ir holds them.
 */
typedef struct {
    bool        busy;   /**< Whether somebody is waiting for a frame.      */
    bool        held;   /**< Whether its deadline is armed and unexpired.  */
    IrSymbol   *out;    /**< IrSymbol[], the requester's.                  */
    size_t      cap;    /**< How many symbols it holds.                    */
    size_t     *n;      /**< Where to write how many were copied.          */
    khal_sem_t  done;   /**< Signalled once, when the answer is in place.  */
    uint32_t    gen;    /**< The requester's tag for this request.         */
    uint32_t   *ack;    /**< Where @c gen goes, just before @c done.       */
    QActive    *ask;    /**< An ask: where @c rsp is posted.               */
    IrCaptureRspEvt *rsp; /**< An ask's answer, taken with the request.    */
} IrCapture;

/** @brief The production row, while it is running.
 *
 * The nonces are kept as a history rather than one value because the row has
 * to tell its own late echo from somebody else's frame. Both arrive as a NEC
 * frame under IR_TEST_ADDR; only the history says which attempt drew the
 * command, and only the current attempt's nonce is a pass.
 */
typedef struct {
    bool     running;   /**< A row is in flight and owns what arrives.     */
    bool     waiting;   /**< Its attempt deadline is armed and unexpired.  */
    uint8_t  attempt;   /**< Which attempt is waiting, 1-based.            */
    uint16_t nonce[KODE_IR_TEST_TRIES]; /**< What each attempt transmitted. */
} IrTest;

/**
 * @brief The last frame the receiver produced, for the console's `ir`.
 *
 * Set from every non-empty capture before it is routed, so it includes our own
 * echoes and frames a production row consumed. Read only to answer
 * ir_info_request().
 */
typedef struct {
    bool     have;      /**< false until the first capture arrives.       */
    uint16_t addr;       /**< NEC address, when @c nec.                    */
    uint16_t cmd;        /**< NEC command, when @c nec.                    */
    uint16_t symbols;    /**< How many symbols arrived, decoded or not.    */
    bool     nec;        /**< Whether the frame decoded as NEC.            */
    uint32_t at_ms;      /**< khal_time_ms() when it arrived.              */
} IrLast;

/** @brief The call to the C5 the worker makes. */
typedef enum {
    IR_OP_ARM = 0,  /**< kdrv_ir_rx_arm().                                   */
    IR_OP_SEND,     /**< kdrv_ir_send() of @c IrJob::src.                    */
    IR_OP_PRESENT,  /**< kdrv_ir_rx_present(): the production row's probe.   */
    IR_OP_IDLE      /**< kdrv_ir_rx_idle(): the console's `ir` reading.      */
} IrOp;

/**
 * @brief The one call to the C5 that may be out, and what the AO needs to
 *        finish it.
 *
 * AO-owned, the exception for a worker's argument: @c op to @c sym are the
 * worker's from submit to the done event, written only while no call is out.
 * Everything after @c sym is this AO's alone.
 */
typedef struct {
    IrOp             op;        /**< What the worker calls.                    */
    uint16_t         n;         /**< How many symbols of @c src a send carries. */
    const kdrv_ir_symbol_t *src; /**< The frame a send transmits: @c sym for a
                                  *   NEC frame, Ir's @c tx for a raw one.     */
    uint32_t         carrier_hz; /**< The carrier a send uses.                */
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS]; /**< A NEC frame, encoded here.        */
    bool             busy;      /**< A call is out: set at submit, cleared by
                                 *   that call's done event and nothing else. */
    bool             expired;   /**< Its deadline passed and the requester was
                                 *   told; the guard waits for the call.       */
    uint16_t         seq;       /**< Sequence number of the call that is out.   */
    bool             loud;      /**< An arm: its failure is an error to log.    */
    bool             row;       /**< A send: one attempt of the production row. */
    uint16_t         addr;      /**< A send: the NEC address, for the log.      */
    uint16_t         cmd;       /**< A send: the NEC command, for the log.      */
    uint32_t         window_ms; /**< A send: the echo window it opens.          */
    bool             raw;       /**< A send: a raw frame, answered below.       */
    uint32_t        *raw_rc;    /**< A raw send: where its status goes.         */
    khal_sem_t       raw_done;  /**< A raw send: posted once it is answered.    */
    uint32_t         raw_gen;   /**< A raw send: the requester's tag.           */
    uint32_t        *raw_ack;   /**< A raw send: where the tag is written back. */
    QActive         *raw_ask;   /**< A raw send's ask: where its answer is posted. */
    StatusRspEvt    *raw_rsp;   /**< That answer, taken with the request.       */
    QActive         *who;       /**< A reading: where the status answer goes.   */
} IrJob;

typedef struct {
    QActive          super;
    QTimeEvt         echo;      /**< One-shot: the suppression window's end.   */
    QTimeEvt         hold;      /**< One-shot: the capture request's deadline. */
    QTimeEvt         wait;      /**< One-shot: one row attempt's deadline.     */
    QTimeEvt         jobdl;     /**< One-shot: the call to the C5's deadline.  */
    bool             up;        /**< Whether the block came up at all.         */
    bool             deaf;      /**< True while our own transmission echoes.   */
    /* Expired events of each one-shot still queued, to be dropped. Each takes
     * a queue slot, so the count is bounded; in practice it is at most one.
     * See deafen(). */
    uint8_t          echo_stale;/**< Expired IR_ECHO_SIG events still queued.  */
    uint8_t          hold_stale;/**< Expired IR_CAPTURE_TO_SIG still queued.   */
    uint8_t          wait_stale;/**< Expired IR_TEST_TO_SIG still queued.      */
    uint8_t          jobdl_stale;/**< Expired IR_JOB_TO_SIG still queued.      */
    KqpWorker        worker;    /**< Makes the calls to the C5, on a PSRAM stack. */
    IrJob            job;       /**< The call that is out, if any.             */
    bool             rdy_pending;/**< A capture was announced while a call was out. */
    bool             retry_due; /**< A row attempt's deadline passed while a call
                                 *   was out.                                  */
    bool             rearm_due; /**< An arm was decided while a call was out:
                                 *   pump() makes it when the call returns.    */
    bool             rearm_loud;/**< That arm follows a capture read out, so a
                                 *   refusal is an error.                      */
    bool             link_wait; /**< Requests wait for the C5 link, asked of
                                 *   AO_Radio; RADIO_STATE_SIG is heard.      */
    uint32_t         link_since;/**< khal_time_ms() when that wait began.     */
    bool             arm_owed;  /**< A listener's arm waits for the link: the
                                 *   next UP arms the receiver, and
                                 *   RADIO_STATE_SIG is heard until then.     */
    bool             link_refused;/**< AO_Radio said OFF or FAILED: until the
                                 *   link is seen up, a request needing it is
                                 *   refused at once.                         */
    QTimeEvt         linkchk;   /**< Periodic while it lasts: a held request
                                 *   past its budget is answered.             */
    QEQueue          deferq;    /**< Requests held while a call is out, or
                                 *   while the link is asked for.             */
    QEvtPtr          defer_sto[KODE_IR_DEFER_DEPTH - 1U]; /**< Its ring; the queue
                                 *   keeps one more in its front slot.         */
    IrCapture        capture;   /**< The one request waiting for a frame.      */
    IrTest           test;      /**< The production row's own state.           */
    IrLast           last;      /**< The last capture heard, for `ir`.         */
    size_t           n;         /**< How many symbols the last capture held.   */
    kdrv_ir_symbol_t *sym;      /**< That capture: KDRV_IR_RX_MAX_SYMBOLS of
                                 *   them, in PSRAM (Ir_initial); only this
                                 *   AO's step reads or writes it.            */
    kdrv_ir_symbol_t *tx;       /**< A raw frame copied from its requester:
                                 *   KDRV_IR_TX_MAX_SYMBOLS, in PSRAM
                                 *   (Ir_initial); written only with no call
                                 *   out, read by the worker during one.      */
} Ir;

static Ir l_ir;
QActive * const AO_Ir = &l_ir.super;

static QState Ir_initial(Ir * const me, void const * const par);
static QState Ir_ready(Ir * const me, QEvt const * const e);

/**
 * @brief Offer a decoded capture to the production row.
 *
 * Declared early: the row reads a capture before the echo window does.
 *
 * @param me   The AO.
 * @param nec  Whether the capture decoded as NEC.
 * @param addr Its address, meaningless unless @p nec.
 * @param cmd  Its command, meaningless unless @p nec.
 * @return true when the row took it, so nobody else hears of it.
 */
static bool test_capture(Ir * const me, bool nec, uint16_t addr, uint16_t cmd);

/* What each call to the C5 comes to once it has returned. Declared here
 * because job_complete() sits above the rows and the status answer that
 * finish them. */
static void send_done(Ir * const me, kode_status_t rc);
static void present_done(Ir * const me, kode_status_t rc, bool present);
static void info_done(Ir * const me, kode_status_t rc, bool high);
static bool job_submit(Ir * const me, IrOp op);
static void rearm(Ir * const me, bool loud);
static void capture_refuse(IrCaptureEvt const *q, kode_status_t st);

/* The capture, as an event. Static and immutable: the ISR allocates nothing. */
static const QEvt RDY_EVT = QEVT_INITIALIZER(IR_RX_RDY_SIG);

/* ---- Requests ------------------------------------------------------------ */

/**
 * @brief Stamp an answer with the generation of the request it belongs to.
 *
 * Always immediately before the semaphore that wakes the requester, and nowhere
 * else: the requester reads it to tell its own answer from an abandoned one.
 * The release store pairs with the requester's acquire load to publish what was
 * written through its pointers to the other core; the semaphore alone does not.
 *
 * @param ack Where the tag goes; NULL is a requester that does not want one.
 * @param gen The tag the request carried.
 */
static void ack_write(uint32_t *ack, uint32_t gen)
{
    if ( NULL != ack )
    {
        __atomic_store_n(ack, gen, __ATOMIC_RELEASE);
    }
}

/* ---- The asks: kode_sys's requests, answered to the asking AO --------------
 * One door per asker and kind: an ask takes it, the answer opens it again
 * just before it is posted, so the asker may ask again from the answer's
 * handler. The device's own contention is the handler's refusal, answered.
 */

/** @brief The kinds of ask, one door each per asker. */
enum {
    IR_ASK_SEND = 0,  /**< ir_send_raw_ask() */
    IR_ASK_CAPTURE,   /**< ir_capture_ask()  */
    IR_ASK_LAST,      /**< ir_last_ask()     */
    IR_ASK_KINDS      /**< How many there are. */
};

/** @brief The doors, one per kind of ask. */
static kaos_ask_door_t s_ir_doors[IR_ASK_KINDS];

QEvt const *ir_send_evt(uint16_t addr, uint16_t cmd)
{
    IrSendEvt  *e   = Q_NEW_X(IrSendEvt, 0U, IR_SEND_REQ_SIG);
    QEvt const *evt = NULL;

    if ( NULL != e )
    {
        e->addr = addr;
        e->cmd  = cmd;
        evt     = &e->super;
    }
    return evt;
}

void ir_send_request(uint16_t addr, uint16_t cmd)
{
    QEvt const *e = ir_send_evt(addr, cmd);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a frame was not sent");
        return;
    }
    QACTIVE_POST(AO_Ir, e, NULL);
}

/**
 * @brief Answer a capture request that never reached AO_Ir: zero symbols,
 *        the tag, then the semaphore.
 *
 * @param n    Where the count goes, or NULL.
 * @param done The requester's semaphore, or NULL.
 * @param gen  The requester's tag.
 * @param ack  Where the tag goes, or NULL.
 */
static void capture_unposted(size_t *n, khal_sem_t done, uint32_t gen, uint32_t *ack)
{
    if ( NULL != n )
    {
        *n = 0U;
    }
    ack_write(ack, gen);
    if ( NULL != done )
    {
        khal_sem_signal(done);
    }
}

void ir_capture_request(IrSymbol *out, size_t cap, size_t *n, khal_sem_t done,
                        uint32_t ms, uint32_t gen, uint32_t *ack)
{
    IrCaptureEvt *e = Q_NEW_X(IrCaptureEvt, 0U, IR_CAPTURE_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a capture request is dropped");
        capture_unposted(n, done, gen, ack);
        return;
    }
    e->out  = out;
    e->cap  = cap;
    e->n    = n;
    e->done = done;
    e->ms   = ms;
    e->posted_ms = khal_time_ms();
    e->gen  = gen;
    e->ack  = ack;
    e->ask  = NULL;
    e->rsp  = NULL;
    /* App traffic, from any task: the margin is AO_Ir's own, and QP
     * recycles the event it refuses. */
    if ( !QACTIVE_POST_X(AO_Ir, &e->super, KQP_IR_CMD_MARGIN, NULL) )
    {
        KLOG_W(TAG, "AO_Ir's queue is full; a capture request is answered empty");
        capture_unposted(n, done, gen, ack);
    }
}

/**
 * @brief Answer a raw send's requester: status, tag, then the semaphore; or
 *        the asking AO, once.
 *
 * @param rc   Where the status goes, or NULL.
 * @param done The requester's semaphore, or NULL.
 * @param gen  The requester's tag.
 * @param ack  Where the tag goes, or NULL.
 * @param ask  The asking AO, or NULL for a waiter.
 * @param rsp  The ask's answer.
 * @param st   The status.
 */
static void raw_answer(uint32_t *rc, khal_sem_t done, uint32_t gen,
                       uint32_t *ack, QActive *ask, StatusRspEvt *rsp,
                       kode_status_t st)
{
    /* One route per request: its makers set the waiter, the ask or the console, never two. */
    Q_ASSERT((NULL == ask) || (NULL == done));
    if ( NULL != rc )
    {
        *rc = (uint32_t)st;
    }
    ack_write(ack, gen);
    if ( NULL != done )
    {
        khal_sem_signal(done);
    }
    if ( NULL != ask )
    {
        rsp->rc = (uint8_t)st;
        kaos_ask_answer(&s_ir_doors[IR_ASK_SEND], ask, &rsp->super, AO_Ir);
    }
}

void ir_send_raw_request(const IrSymbol *sym, uint16_t n, uint32_t carrier_hz,
                         uint32_t start_ms, uint32_t *rc, khal_sem_t done,
                         uint32_t gen, uint32_t *ack)
{
    IrSendRawEvt *e = Q_NEW_X(IrSendRawEvt, 0U, IR_SEND_RAW_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a raw frame was not sent");
        raw_answer(rc, done, gen, ack, NULL, NULL, KODE_NO_RESOURCES);
        return;
    }
    e->sym        = sym;
    e->n          = n;
    e->carrier_hz = carrier_hz;
    e->posted_ms  = khal_time_ms();
    e->start_ms   = start_ms;
    e->rc         = rc;
    e->done       = done;
    e->gen        = gen;
    e->ack        = ack;
    e->ask        = NULL;
    e->rsp        = NULL;
    QACTIVE_POST(AO_Ir, &e->super, NULL);
}

void ir_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(IR_TEST_REQ_SIG);
    QACTIVE_POST(AO_Ir, &evt, NULL);
}

void ir_info_request(QActive *who)
{
    IrInfoReqEvt *e = Q_NEW_X(IrInfoReqEvt, 0U, IR_INFO_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a status request is dropped");
        return;
    }
    e->who = who;
    QACTIVE_POST(AO_Ir, &e->super, NULL);
}

void ir_last_request(void *out, bool *have, khal_sem_t done,
                     uint32_t gen, uint32_t *ack)
{
    IrLastEvt *e = Q_NEW_X(IrLastEvt, 0U, IR_LAST_REQ_SIG);
    if ( NULL == e )
    {
        /* Answered, not dropped: a caller must never sleep on a semaphore
         * nothing will signal. The stamp goes too, or the caller would take
         * this answer for someone else's. */
        KLOG_E(TAG, "out of events; a last-frame request is dropped");
        if ( NULL != have )
        {
            *have = false;
        }
        ack_write(ack, gen);
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->out  = out;
    e->have = have;
    e->done = done;
    e->gen  = gen;
    e->ack  = ack;
    e->ask  = NULL;
    e->rsp  = NULL;
    QACTIVE_POST(AO_Ir, &e->super, NULL);
}

kode_status_t ir_send_raw_ask(QActive *who, const IrSymbol *sym, uint16_t n,
                              uint32_t carrier_hz, uint32_t start_ms)
{
    kode_status_t rc = kaos_ask_take(&s_ir_doors[IR_ASK_SEND], who);
    StatusRspEvt *a;
    IrSendRawEvt *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(StatusRspEvt, 0U, IR_SEND_RAW_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_ir_doors[IR_ASK_SEND], who, NULL);
    }
    e = Q_NEW_X(IrSendRawEvt, 0U, IR_SEND_RAW_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_ir_doors[IR_ASK_SEND], who, &a->super);
    }
    e->sym        = sym;
    e->n          = n;
    e->carrier_hz = carrier_hz;
    e->posted_ms  = khal_time_ms();
    e->start_ms   = start_ms;
    e->rc         = NULL;
    e->done       = NULL;
    e->gen        = 0U;     /* an ask is answered by its event, not matched by a tag */
    e->ack        = NULL;
    e->ask        = who;
    e->rsp        = a;
    return kaos_ask_post(&s_ir_doors[IR_ASK_SEND], who, AO_Ir, &e->super, &a->super,
                         KQP_IR_CMD_MARGIN);
}

kode_status_t ir_capture_ask(QActive *who, IrSymbol *out, size_t cap, uint32_t ms)
{
    kode_status_t    rc = kaos_ask_take(&s_ir_doors[IR_ASK_CAPTURE], who);
    IrCaptureRspEvt *a;
    IrCaptureEvt    *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(IrCaptureRspEvt, 0U, IR_CAPTURE_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_ir_doors[IR_ASK_CAPTURE], who, NULL);
    }
    e = Q_NEW_X(IrCaptureEvt, 0U, IR_CAPTURE_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_ir_doors[IR_ASK_CAPTURE], who, &a->super);
    }
    e->out  = out;
    e->cap  = cap;
    e->n    = NULL;
    e->done = NULL;
    e->ms   = ms;
    e->posted_ms = khal_time_ms();
    e->gen  = 0U;
    e->ack  = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_ir_doors[IR_ASK_CAPTURE], who, AO_Ir, &e->super, &a->super,
                         KQP_IR_CMD_MARGIN);
}

kode_status_t ir_last_ask(QActive *who)
{
    kode_status_t rc = kaos_ask_take(&s_ir_doors[IR_ASK_LAST], who);
    IrLastRspEvt *a;
    IrLastEvt    *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(IrLastRspEvt, 0U, IR_LAST_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_ir_doors[IR_ASK_LAST], who, NULL);
    }
    e = Q_NEW_X(IrLastEvt, 0U, IR_LAST_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_ir_doors[IR_ASK_LAST], who, &a->super);
    }
    e->out  = NULL;
    e->have = NULL;
    e->done = NULL;
    e->gen  = 0U;
    e->ack  = NULL;
    e->ask  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_ir_doors[IR_ASK_LAST], who, AO_Ir, &e->super, &a->super,
                         KQP_IR_CMD_MARGIN);
}

/* ---- The capture callback ------------------------------------------------ */

/**
 * @brief A capture is ready, on the C5 link's thread: make an event of it and leave.
 *
 * @param ctx The AO, handed to kdrv_ir_init() and not needed: AO_Ir is a
 *            single instance and the event is static.
 */
static void on_capture(void *ctx)
{
    (void)ctx;
    QACTIVE_POST(AO_Ir, &RDY_EVT, (void *)0);
}

/* ---- Construction -------------------------------------------------------- */

void Ir_ctor(void)
{
    Ir * const me = &l_ir;
    QActive_ctor(&me->super, Q_STATE_CAST(&Ir_initial));
    QTimeEvt_ctorX(&me->echo, &me->super, IR_ECHO_SIG, 0U);
    QTimeEvt_ctorX(&me->hold, &me->super, IR_CAPTURE_TO_SIG, 0U);
    QTimeEvt_ctorX(&me->wait, &me->super, IR_TEST_TO_SIG, 0U);
    QTimeEvt_ctorX(&me->jobdl, &me->super, IR_JOB_TO_SIG, 0U);
    QTimeEvt_ctorX(&me->linkchk, &me->super, IR_LINK_TO_SIG, 0U);
    QEQueue_init(&me->deferq, me->defer_sto, Q_DIM(me->defer_sto));
    me->up           = false;
    me->deaf         = false;
    me->echo_stale   = 0U;
    me->hold_stale   = 0U;
    me->wait_stale   = 0U;
    me->jobdl_stale  = 0U;
    me->rdy_pending  = false;
    me->retry_due    = false;
    me->rearm_due    = false;
    me->rearm_loud   = false;
    me->link_wait    = false;
    me->link_since   = 0U;
    me->arm_owed     = false;
    me->link_refused = false;
    me->sym          = NULL;
    me->tx           = NULL;
    (void)memset(&me->worker, 0, sizeof(me->worker));
    (void)memset(&me->job, 0, sizeof(me->job));
    me->n            = 0U;
    me->test.running = false;
    me->test.waiting = false;
    me->test.attempt = 0U;
    (void)memset(me->test.nonce, 0, sizeof(me->test.nonce));
    me->capture.busy = false;
    me->capture.held = false;
    me->capture.out  = NULL;
    me->capture.cap  = 0U;
    me->capture.n    = NULL;
    me->capture.done = NULL;
    me->capture.gen  = 0U;
    me->capture.ack  = NULL;
    me->last.have    = false;
    me->last.addr    = 0U;
    me->last.cmd     = 0U;
    me->last.symbols = 0U;
    me->last.nec     = false;
    me->last.at_ms   = 0U;
}

AO_STORAGE(ir, KQP_AO_QUEUE_LEN, KQP_AO_STACK_SMALL);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_ir_register() ---------------- */

/** @brief The infrared row: transmit a frame with a random command and require
 *         that exact frame back. The receiver is judged first, so a failure
 *         says which half. */
static bool ir_row_ir(void)
{
    ir_test_request();
    return false;   /* answered later, with TEST_RESULT_SIG */
}
#endif

void kaos_ir_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t IR_ROWS[] = {
        { "ir", AUTOTEST_ID_IR, ir_row_ir, KROW_ASYNC, AUTOTEST_G_NONE, KODE_IR_TEST_BUDGET_MS,
          NULL, 0U, "Infrared",
          "Infrared not working",
          AUTOTEST_ORDER_IR },
    };

    kaos_require(krows_add(IR_ROWS, sizeof(IR_ROWS) / sizeof(IR_ROWS[0])),
                 "ir rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&ir_verbs), "ir verbs");
#endif
}

void kaos_ir_start(void)
{
    Ir_ctor();
    kaos_ir_register();
    AO_START(AO_Ir, ir, KODE_PRIO_IR);
}

/**
 * @brief Claim the block, take the PSRAM buffers, bring up the driver and the worker.
 *
 * Any of the three failing leaves the AO in Ir_ready without @c up, hearing and
 * sending nothing.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Ir_ready.
 */
static QState Ir_initial(Ir * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_IR, me, "AO_Ir");

    /* cppcheck-suppress misra-c2012-11.5 ; PSRAM storage aligned for any object, and used as symbols */
    me->sym = (kdrv_ir_symbol_t *)khal_alloc_psram(IR_SYM_ALIGN, IR_SYM_BYTES);
    /* cppcheck-suppress misra-c2012-11.5 ; PSRAM storage aligned for any object, and used as symbols */
    me->tx  = (kdrv_ir_symbol_t *)khal_alloc_psram(IR_SYM_ALIGN, IR_TX_BYTES);
    if ( (NULL == me->sym) || (NULL == me->tx) )
    {
        KLOG_E(TAG, "no memory for a capture or a frame; nothing is heard "
                    "and nothing can be sent");
    }
    else if ( kdrv_ir_init(&on_capture, me) != KODE_OK )
    {
        KLOG_E(TAG, "the infrared block did not come up; nothing is heard and "
                    "nothing can be sent");
    }
    else if ( kqp_worker_start(&me->worker, &me->super, IR_WORKER_NAME,
                              IR_WORKER_STACK, KODE_PRIO_IR, IR_WORKER_DEPTH,
                              true) != KODE_OK )
    {
        KLOG_E(TAG, "the infrared worker would not start; nothing is heard "
                    "and nothing can be sent");
    }
    else
    {
        me->up = true;
        KLOG_I(TAG, "up; listening, carrier %u Hz",
               (unsigned)KDRV_IR_CARRIER_HZ);
        /* kdrv_ir_init() arms nothing: with the link up that is a request over
         * it, from the thread starting the AO. The first job arms the
         * receiver; it is refused quietly while the link is down or the
         * receiver already listens. */
        rearm(me, false);
    }
    return Q_TRAN(&Ir_ready);
}

/* ---- Timers -------------------------------------------------------------- *
 * The echo window, the capture request and the row's attempt are one-shots a
 * later event may replace before their expiry is dispatched; the helpers below
 * handle that once for all three.
 */

/**
 * @brief @p ms as QP ticks.
 *
 * @param ms The interval, clamped to KODE_SYS_IR_TIMER_MS_MAX.
 * @return The tick count. Zero only for @p ms of zero, which nothing may arm -
 *         QP asserts on a one-shot of zero ticks.
 */
static uint32_t ms_ticks(uint32_t ms)
{
    uint32_t capped = ms;

    if ( KODE_SYS_IR_TIMER_MS_MAX < capped )
    {
        capped = KODE_SYS_IR_TIMER_MS_MAX;
    }
    return KQP_TICKS_PER_MS(capped);
}

/* ---- The calls to the C5 --------------------------------------------------- */

/**
 * @brief The call as the worker makes it.
 *
 * Runs on the worker's task and touches only its argument, the AO's IrJob,
 * valid until the done event. A line reading's level travels as the done
 * event's word. Once the link drops the driver answers KODE_DENIED; a call
 * already inside the vendor's RPC is the vendor's to unwind.
 *
 * @param arg   The AO's IrJob.
 * @param value Receives 1 when a line reading found the line high.
 * @return What the driver answered.
 */
/* cppcheck-suppress constParameterCallback ; KqpJobFn fixes the signature, and its argument is void * */
static kode_status_t ir_job_run(void *arg, uint32_t *value)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the worker hands back the pointer it was given, which is the AO's job storage */
    IrJob const * const j = (IrJob const *)arg;
    kode_status_t       rc    = KODE_DENIED;
    bool                level = false;

    *value = 0U;
    switch ( j->op )
    {
        case IR_OP_ARM: {
            rc = kdrv_ir_rx_arm();
            break;
        }
        case IR_OP_SEND: {
            rc = kdrv_ir_send(j->src, (size_t)j->n, j->carrier_hz);
            break;
        }
        case IR_OP_PRESENT: {
            rc = kdrv_ir_rx_present(&level);
            break;
        }
        case IR_OP_IDLE: {
            rc = kdrv_ir_rx_idle(&level);
            break;
        }
        default: {
            break;
        }
    }
    if ( level )
    {
        *value = 1U;
    }
    return rc;
}

/**
 * @brief Hand the worker a call, with its deadline, and raise the guard.
 *
 * Only while no call is out. The caller has written what the call carries
 * into @c job already.
 *
 * @param me The AO.
 * @param op The call.
 * @return true when the worker has it; false when a call is out or the
 *         worker would not take it, and nothing changed.
 */
static bool job_submit(Ir * const me, IrOp op)
{
    uint16_t seq = 0U;

    if ( me->job.busy )
    {
        return false;
    }
    me->job.op = op;
    if ( kqp_worker_submit(&me->worker, &ir_job_run, &me->job, IR_JOB_DONE_SIG,
                          &seq) != KODE_OK )
    {
        return false;
    }
    me->job.seq     = seq;
    me->job.busy    = true;
    me->job.expired = false;
    QTimeEvt_armX(&me->jobdl, (QTimeEvtCtr)ms_ticks(IR_JOB_DEADLINE_MS),
                  0U);
    return true;
}

/**
 * @brief Finish what the call that returned was for.
 *
 * @param me    The AO.
 * @param rc    The status the call returned; KODE_TIMEOUT for a deadline.
 * @param level A line reading's level; false for the other calls.
 */
static void job_complete(Ir * const me, kode_status_t rc, bool level)
{
    switch ( me->job.op )
    {
        case IR_OP_ARM: {
            if ( me->job.loud && (KODE_OK != rc) )
            {
                KLOG_E(TAG, "the receiver would not listen again; nothing "
                            "more will be heard");
            }
            break;
        }
        case IR_OP_SEND: {
            send_done(me, rc);
            break;
        }
        case IR_OP_PRESENT: {
            present_done(me, rc, level);
            break;
        }
        case IR_OP_IDLE: {
            info_done(me, rc, level);
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * @brief The worker's call returned.
 *
 * An event whose sequence number is not the call's - or that arrives with no
 * call out - answers nothing this AO is waiting for, and is dropped. A call
 * that returns after its deadline lowers the guard and nothing else: whoever
 * waited for it was told long ago.
 *
 * @param me The AO.
 * @param e  The done event.
 */
static void on_job_done(Ir * const me, KqpWorkerDoneEvt const * const e)
{
    kode_status_t const rc = (kode_status_t)e->status;
    bool                late;

    if ( (!me->job.busy) || (e->seq != me->job.seq) )
    {
        KLOG_D(TAG, "worker answer seq %u is not the call's (%u); dropped",
               (unsigned)e->seq, (unsigned)me->job.seq);
        return;
    }
    late = me->job.expired;
    if ( (!late) && kqp_timer_stale(&me->jobdl, true) )
    {
        me->jobdl_stale += 1U;
    }
    me->job.busy    = false;
    me->job.expired = false;
    if ( late )
    {
        KLOG_W(TAG, "the call to the C5 returned after its deadline (rc=%d)",
               (int)rc);
        if ( me->job.raw )
        {
            /* The one requester the deadline did not answer: its frame was
             * still being read. */
            send_done(me, rc);
        }
        return;
    }
    job_complete(me, rc, 0U != e->value);
}

/**
 * @brief The call has been out for IR_JOB_DEADLINE_MS.
 *
 * Whoever waits for it is told it failed, and the guard stays up: the worker
 * still holds @c job, and the next call must not be written over it.
 *
 * @param me The AO.
 */
static void on_job_deadline(Ir * const me)
{
    uint32_t const deadline_ms = IR_JOB_DEADLINE_MS;

    if ( 0U < me->jobdl_stale )
    {
        me->jobdl_stale -= 1U;
        return;
    }
    if ( me->job.busy && (!me->job.expired) )
    {
        me->job.expired = true;
        KLOG_E(TAG, "a call to the C5 did not return in %u ms; its requester "
                    "is told it failed",
               (unsigned)deadline_ms);
        job_complete(me, KODE_TIMEOUT, false);
    }
}

/* ---- What arrives -------------------------------------------------------- */

/**
 * @brief Answer the pending capture request, if there is one.
 *
 * The symbols come from the AO's own buffer. A capture longer than the
 * requester's is truncated and said so, never wrapped - the driver's own
 * rule, one layer up.
 *
 * @param me   The AO.
 * @param have How many symbols of that buffer are the answer; 0 answers empty
 *             (a displaced request, an expired deadline, a block never up).
 */
static void capture_answer(Ir * const me, size_t have, kode_status_t st)
{
    size_t took = have;

    if ( !me->capture.busy )
    {
        return;
    }
    if ( kqp_timer_stale(&me->hold, me->capture.held) )
    {
        me->hold_stale += 1U;
    }
    me->capture.held = false;
    if ( took > me->capture.cap )
    {
        KLOG_W(TAG, "capture of %u symbols truncated to %u",
               (unsigned)took, (unsigned)me->capture.cap);
        took = me->capture.cap;
    }
    if ( NULL != me->capture.out )
    {
        /* Field by field: the requester's IrSymbol is not the driver's type. */
        for ( size_t i = 0U; i < took; ++i )
        {
            me->capture.out[i].mark_us  = me->sym[i].mark_us;
            me->capture.out[i].space_us = me->sym[i].space_us;
        }
    }
    if ( NULL != me->capture.n )
    {
        *me->capture.n = took;
    }
    me->capture.busy = false;
    ack_write(me->capture.ack, me->capture.gen);
    if ( NULL != me->capture.done )
    {
        khal_sem_signal(me->capture.done);
    }
    if ( NULL != me->capture.ask )
    {
        IrCaptureRspEvt * const a   = me->capture.rsp;
        QActive * const         who = me->capture.ask;

        /* The lent buffer was filled above and is never touched again. */
        me->capture.ask = NULL;
        me->capture.rsp = NULL;
        a->rc = (uint8_t)st;
        a->n  = (uint32_t)took;
        kaos_ask_answer(&s_ir_doors[IR_ASK_CAPTURE], who, &a->super, AO_Ir);
    }
}

/**
 * @brief Publish one frame on IR_RX_SIG. Bounded: a dry pool drops it.
 *
 * @param me   The AO.
 * @param addr The NEC address, meaningless unless @p nec.
 * @param cmd  The NEC command, meaningless unless @p nec.
 * @param nec  Whether the capture decoded as NEC.
 */
static void publish_frame(Ir * const me, uint16_t addr, uint16_t cmd, bool nec)
{
    IrRxEvt *ev = Q_NEW_X(IrRxEvt, 0U, IR_RX_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a frame is lost");
        return;
    }
    ev->addr    = addr;
    ev->cmd     = cmd;
    ev->symbols = (uint16_t)me->n;
    ev->nec     = nec;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Arm the receiver, or owe the arm to pump() while a call is out.
 *
 * @param me   The AO.
 * @param loud A capture was just read out, so the receiver is disarmed and a
 *             refusal is an error; otherwise it may listen already, and the
 *             driver's refusal says nothing.
 */
static void rearm(Ir * const me, bool loud)
{
    if ( me->job.busy )
    {
        me->rearm_due = true;
        if ( loud )
        {
            me->rearm_loud = true;
        }
        return;
    }
    me->job.loud = loud;
    if ( (!job_submit(me, IR_OP_ARM)) && loud )
    {
        KLOG_E(TAG, "the receiver would not listen again; nothing more will "
                    "be heard");
    }
}

/**
 * @brief Read the capture out, listen again, and decide who hears of it.
 *
 * The receiver takes one capture per arming, so the re-arm goes to the worker
 * before the symbols are used, but after the empty check: kdrv_ir_rx_present()
 * re-arms and loses what was in flight, so an empty take is reachable with the
 * receiver already listening. A readiness event while a call is out is served
 * when it returns (pump()), or before a deadline or the echo window's end.
 *
 * @param me The AO.
 */
static void heard(Ir * const me)
{
    uint16_t addr = 0U;
    uint16_t cmd  = 0U;
    bool     nec;

    me->n = kdrv_ir_rx_take(me->sym, KDRV_IR_RX_MAX_SYMBOLS);

    if ( 0U == me->n )
    {
        KLOG_D(TAG, "a capture was announced and there was none; the receiver "
                    "is still armed");
        return;
    }

    rearm(me, true);

    nec = knec_decode(me->sym, me->n, &addr, &cmd);

    /* Recorded for `ir` before routing: an echo or a row's attempt is still
     * what the receiver produced. */
    me->last.have    = true;
    me->last.addr    = addr;
    me->last.cmd     = cmd;
    me->last.symbols = (uint16_t)me->n;
    me->last.nec     = nec;
    me->last.at_ms   = khal_time_ms();

    /* Before the echo window, which is a tenth of the row's
     * KODE_IR_TEST_ECHO_MS: a late echo would be published as a remote.
     * While a row runs, everything heard is the row's. */
    if ( test_capture(me, nec, addr, cmd) )
    {
        return;
    }

    if ( me->deaf )
    {
        /* Ours. Logged rather than silent, because "the remote does nothing"
         * and "the remote is us" look identical from outside this line. */
        if ( nec )
        {
            KLOG_D(TAG, "echo suppressed: addr=0x%04X cmd=0x%04X",
                   (unsigned)addr, (unsigned)cmd);
        }
        else
        {
            KLOG_D(TAG, "echo suppressed: %u symbols, not NEC",
                   (unsigned)me->n);
        }
        return;
    }

    if ( nec )
    {
        KLOG_D(TAG, "heard addr=0x%04X cmd=0x%04X in %u symbols",
               (unsigned)addr, (unsigned)cmd, (unsigned)me->n);
    }
    else
    {
        KLOG_D(TAG, "heard %u symbols, not NEC", (unsigned)me->n);
    }
    capture_answer(me, me->n, KODE_OK);
    publish_frame(me, addr, cmd, nec);
}

/* ---- What leaves --------------------------------------------------------- */

/**
 * @brief How long @p n symbols take on the wire, in milliseconds, rounded up.
 *
 * Understates the transmitter's time by about 33 ms: kdrv_ir_send() expands the
 * frame's final zero space to 32767 us, when the emitter is dark (the echo
 * window is unaffected) but the driver refuses another send. With that tail a
 * frame of all ones (about 87 ms) misses NEC's 110 ms repeat period: count it
 * before tightening the schedule.
 *
 * @param sym The symbols.
 * @param n   How many.
 * @return The duration, at least 1 ms for anything at all.
 */
static uint32_t frame_ms(const kdrv_ir_symbol_t *sym, size_t n)
{
    uint32_t us = 0U;
    size_t   i;

    for ( i = 0U; i < n; ++i )
    {
        us += (uint32_t)sym[i].mark_us;
        us += (uint32_t)sym[i].space_us;
    }
    return (us + 999U) / 1000U;
}

/**
 * @brief Stop publishing what the receiver hears for @p ms.
 *
 * Disarm before arm: QP refuses to arm a running time event. The previous
 * window's IR_ECHO_SIG may already be queued while disarm reports "not
 * running"; dispatched later it would end this window at once, so the arming
 * is counted and the handler drops one queued event per count.
 *
 * @param me The AO.
 * @param ms How long the window is, in milliseconds.
 */
static void deafen(Ir * const me, uint32_t ms)
{
    uint32_t const ticks = ms_ticks(ms);

    if ( kqp_timer_stale(&me->echo, me->deaf) )
    {
        me->echo_stale += 1U;
    }
    QTimeEvt_armX(&me->echo, (QTimeEvtCtr)ticks, 0U);
    me->deaf = true;
}

/**
 * @brief Encode one NEC frame and hand it to the worker to send.
 *
 * Only with no call out: the frame is built in the job's buffer, which the
 * worker may be reading. The echo window opens in send_done(), and only if the
 * frame went out, so a refused send does not mute a real remote.
 *
 * @param me   The AO.
 * @param addr The address.
 * @param cmd  The command.
 * @param row  Whether this is one attempt of the production row, for which a
 *             refused frame is a verdict.
 * @return true when the worker has the frame; false when it was not encoded
 *         or the worker would not take it, and nothing is out.
 */
static bool send_begin(Ir * const me, uint16_t addr, uint16_t cmd, bool row)
{
    size_t n = 0U;

    if ( me->job.busy )
    {
        return false;
    }
    if ( knec_encode(addr, cmd, me->job.sym, KNEC_SYMBOLS, &n) != KODE_OK )
    {
        KLOG_E(TAG, "addr=0x%04X cmd=0x%04X would not encode",
               (unsigned)addr, (unsigned)cmd);
        return false;
    }
    me->job.n          = (uint16_t)n;
    me->job.src        = me->job.sym;
    me->job.carrier_hz = KDRV_IR_CARRIER_HZ;
    me->job.raw        = false;
    me->job.addr       = addr;
    me->job.cmd        = cmd;
    me->job.row        = row;
    me->job.window_ms  = frame_ms(me->job.sym, n) + KODE_IR_ECHO_TAIL_MS;
    if ( !job_submit(me, IR_OP_SEND) )
    {
        KLOG_W(TAG, "addr=0x%04X cmd=0x%04X was not sent: the worker has no "
                    "room", (unsigned)addr, (unsigned)cmd);
        return false;
    }
    return true;
}

/**
 * @brief Start sending the frame @p q lends, on its carrier.
 *
 * The symbols are copied into @c tx in the driver's type, and the worker sends
 * the copy. A frame over KDRV_IR_TX_MAX_SYMBOLS is refused, never cut; a worker
 * with no room answers at once.
 *
 * @param me The AO, with no call out.
 * @param q  The request.
 */
static void raw_begin(Ir * const me, IrSendRawEvt const *q)
{
    uint16_t i;

    if ( (NULL == q->sym) || (0U == q->n) || (KDRV_IR_TX_MAX_SYMBOLS < q->n) )
    {
        KLOG_W(TAG, "a raw frame of %u symbols is not one the transmitter takes",
               (unsigned)q->n);
        raw_answer(q->rc, q->done, q->gen, q->ack, q->ask, q->rsp, KODE_DENIED);
        return;
    }
    if ( NULL == me->tx )
    {
        raw_answer(q->rc, q->done, q->gen, q->ack, q->ask, q->rsp, KODE_NO_RESOURCES);
        return;
    }
    for ( i = 0U; i < q->n; ++i )
    {
        me->tx[i].mark_us  = q->sym[i].mark_us;
        me->tx[i].space_us = q->sym[i].space_us;
    }
    /* Checked after the copy: a frame held behind other calls past its start
     * budget must not go out after the app was told it did not. A copy read
     * after its waiter left is discarded unsent. Unsigned, across the wrap. */
    if ( (khal_time_ms() - q->posted_ms) > q->start_ms )
    {
        KLOG_W(TAG, "a raw frame of %u symbols waited too long to start; "
                    "not sent", (unsigned)q->n);
        raw_answer(q->rc, q->done, q->gen, q->ack, q->ask, q->rsp, KODE_TIMEOUT);
        return;
    }
    me->job.src        = me->tx;
    me->job.n          = q->n;
    me->job.carrier_hz = q->carrier_hz;
    me->job.raw        = true;
    me->job.row        = false;
    me->job.raw_rc     = q->rc;
    me->job.raw_done   = q->done;
    me->job.raw_gen    = q->gen;
    me->job.raw_ack    = q->ack;
    me->job.raw_ask    = q->ask;
    me->job.raw_rsp    = q->rsp;
    me->job.window_ms  = frame_ms(me->job.src, q->n) + KODE_IR_ECHO_TAIL_MS;
    if ( !job_submit(me, IR_OP_SEND) )
    {
        KLOG_W(TAG, "a raw frame of %u symbols was not sent: the worker has "
                    "no room", (unsigned)q->n);
        me->job.raw = false;
        raw_answer(q->rc, q->done, q->gen, q->ack, q->ask, q->rsp, KODE_NO_RESOURCES);
    }
}

/**
 * @brief Take the capture request in @p q, answering whatever it displaces.
 *
 * One request at a time; the older one is answered empty, never forgotten, so
 * no requester sleeps forever. The new one's deadline bounds how long AO_Ir
 * keeps the requester's pointers, and runs from the post: a request that spent
 * it while held is answered now.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void capture_take(Ir * const me, IrCaptureEvt const *q)
{
    uint32_t const spent = khal_time_ms() - q->posted_ms;  /* across the wrap */
    uint32_t       left  = 0U;
    uint32_t       ticks;

    if ( spent < q->ms )
    {
        left = q->ms - spent;
    }
    ticks = ms_ticks(left);
    if ( (0U != q->ms) && (0U == ticks) )
    {
        /* Spent while it was held: answered, and the one pending kept. */
        KLOG_D(TAG, "a capture request spent its wait held; answered empty");
        capture_refuse(q, KODE_TIMEOUT);
        return;
    }

    if ( me->capture.busy )
    {
        KLOG_W(TAG, "a second capture request displaces the first");
        capture_answer(me, 0U, KODE_DENIED);
    }
    me->capture.busy = true;
    me->capture.held = false;
    me->capture.out  = q->out;
    me->capture.cap  = q->cap;
    me->capture.n    = q->n;
    me->capture.done = q->done;
    me->capture.gen  = q->gen;
    me->capture.ack  = q->ack;
    me->capture.ask  = q->ask;
    me->capture.rsp  = q->rsp;

    if ( !me->up )
    {
        KLOG_W(TAG, "no infrared block; a capture is answered empty");
        capture_answer(me, 0U, KODE_IO_ERROR);
        return;
    }
    /* The C5 link is usually down when the driver comes up, so the receiver is
     * armed here, when somebody wants to hear; refused quietly when it already
     * listens. The deadline below runs on from the post. */
    rearm(me, false);
    if ( 0U == q->ms )
    {
        /* Nobody is waiting, so there is nothing to hold the buffer for. */
        KLOG_W(TAG, "a capture request with no deadline is answered empty");
        capture_answer(me, 0U, KODE_DENIED);
        return;
    }

    QTimeEvt_armX(&me->hold, (QTimeEvtCtr)ticks, 0U);
    me->capture.held = true;
}

/* ---- The production row -------------------------------------------------- *
 * `test component=ir`: the board emits and listens for itself. Step 1 judges
 * the receiver alone, emitting nothing; step 2 judges the pair, so a failure
 * names the half to replace. The command is a nonce from the hardware RNG,
 * fresh per attempt, so no lamp, remote or neighbouring board can fake a pass.
 * Only its deadline ends an attempt: a frame that is not the nonce must not
 * spend one, or one burst of ambient infrared would fail a good board.
 */

/**
 * @brief The address every frame the row transmits carries.
 *
 * Fixed: the nonce is the command. Not zero, so a decoder reporting all zeroes
 * is not mistaken for our own frame.
 */
#define IR_TEST_ADDR  0xA55AU

/**
 * @brief The nonce the attempt now waiting transmitted.
 *
 * @param me The AO.
 * @return That command, or zero before the first attempt has drawn one.
 */
static uint16_t test_nonce(const Ir * const me)
{
    uint16_t nonce = 0U;

    if ( (0U < me->test.attempt) && (KODE_IR_TEST_TRIES >= me->test.attempt) )
    {
        nonce = me->test.nonce[me->test.attempt - 1U];
    }
    return nonce;
}

/**
 * @brief Whether @p cmd is a nonce an EARLIER attempt of this run transmitted.
 *
 * Only earlier ones: the current attempt's nonce is a pass and is answered
 * before this is asked.
 *
 * @param me  The AO.
 * @param cmd The command that came back.
 * @return true when this board sent that value itself, a moment ago.
 */
static bool test_was_ours(const Ir * const me, uint16_t cmd)
{
    bool    ours = false;
    uint8_t i;
    uint8_t upto = me->test.attempt;

    if ( KODE_IR_TEST_TRIES < upto )
    {
        upto = KODE_IR_TEST_TRIES;
    }
    for ( i = 0U; (i + 1U) < upto; ++i )
    {
        if ( me->test.nonce[i] == cmd )
        {
            ours = true;
        }
    }
    return ours;
}

/**
 * @brief Publish the row's verdict and let the receiver go back to work.
 *
 * @param me The AO.
 * @param ok Whether the block passed.
 */
static void test_finish(Ir * const me, bool ok)
{
    TestResultEvt *ev;

    if ( kqp_timer_stale(&me->wait, me->test.waiting) )
    {
        me->wait_stale += 1U;
    }
    me->test.waiting = false;
    me->test.running = false;

    ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the infrared verdict is lost");
        return;
    }
    (void)snprintf(ev->name, sizeof(ev->name), "%s", "ir");
    ev->ok = ok;
    ev->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Start the clock on the echo of the frame that just went out.
 *
 * @param me The AO.
 */
static void test_wait(Ir * const me)
{
    /* Rounded UP to whole ticks, as the suppression window is: a deadline one
     * tick short of the wait is an attempt given less than its budget. */
    uint32_t const ticks = ms_ticks(KODE_IR_TEST_ECHO_MS);

    /* Counted, not only disarmed, as in deafen(): the deadline this replaces
     * may already be queued and would end the new attempt at once. */
    if ( kqp_timer_stale(&me->wait, me->test.waiting) )
    {
        me->wait_stale += 1U;
    }
    QTimeEvt_armX(&me->wait, (QTimeEvtCtr)ticks, 0U);
    me->test.waiting = true;
}

/**
 * @brief Draw a nonce and hand its frame to the worker.
 *
 * The clock on the echo starts in send_done(), when the frame is on the link.
 *
 * @param me The AO.
 */
static void test_attempt(Ir * const me)
{
    if ( KODE_IR_TEST_TRIES <= me->test.attempt )
    {
        /* Unreachable: test_retry() is the only caller that comes back here
         * and it gives up at the cap. Guarded anyway, because the nonce below
         * indexes the history with it. */
        return;
    }
    me->test.attempt += 1U;
    me->test.nonce[me->test.attempt - 1U] = (uint16_t)(khal_random() & 0xFFFFU);

    KLOG_N(TAG, "attempt %u/%u: sending addr=0x%04X nonce=0x%04X",
           (unsigned)me->test.attempt, (unsigned)KODE_IR_TEST_TRIES,
           (unsigned)IR_TEST_ADDR, (unsigned)test_nonce(me));

    if ( !send_begin(me, IR_TEST_ADDR, test_nonce(me), true) )
    {
        KLOG_E(TAG, "the emitter would not take the frame; the transmitter "
                    "is the half to replace");
        test_finish(me, false);
    }
}

/**
 * @brief The frame of a send has been put on the link, or has not.
 *
 * @param me The AO.
 * @param rc What the driver answered.
 */
static void send_done(Ir * const me, kode_status_t rc)
{
    if ( me->job.raw )
    {
        /* A raw frame: its requester is answered whatever happened, but not
         * on the deadline: on_job_done() gives the call's own status when it
         * returns. Only a frame that went out has an echo to be deaf to. */
        if ( me->job.expired )
        {
            return;
        }
        me->job.raw = false;
        if ( KODE_OK == rc )
        {
            deafen(me, me->job.window_ms);
            KLOG_D(TAG, "sent a raw frame of %u symbols at %lu Hz",
                   (unsigned)me->job.n, (unsigned long)me->job.carrier_hz);
        }
        else
        {
            KLOG_W(TAG, "a raw frame of %u symbols was not sent",
                   (unsigned)me->job.n);
        }
        raw_answer(me->job.raw_rc, me->job.raw_done, me->job.raw_gen,
                   me->job.raw_ack, me->job.raw_ask, me->job.raw_rsp, rc);
        return;
    }
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "addr=0x%04X cmd=0x%04X was not sent",
               (unsigned)me->job.addr, (unsigned)me->job.cmd);
        if ( me->job.row && me->test.running )
        {
            KLOG_E(TAG, "the emitter would not take the frame; the "
                        "transmitter is the half to replace");
            test_finish(me, false);
        }
        return;
    }
    deafen(me, me->job.window_ms);
    KLOG_D(TAG, "sent addr=0x%04X cmd=0x%04X", (unsigned)me->job.addr,
           (unsigned)me->job.cmd);
    if ( me->job.row && me->test.running )
    {
        test_wait(me);
    }
}

/**
 * @brief This attempt's deadline expired. Try again, or give up.
 *
 * Only the deadline gets here. A frame that is not the nonce does not, and
 * that is deliberate - see test_capture().
 *
 * @param me The AO.
 */
static void test_retry(Ir * const me)
{
    if ( KODE_IR_TEST_TRIES <= me->test.attempt )
    {
        KLOG_E(TAG, "the receiver is fitted but nothing came back in %u "
                    "attempts; the emitter is the half to replace",
               (unsigned)me->test.attempt);
        test_finish(me, false);
        return;
    }
    test_attempt(me);
}

static bool test_capture(Ir * const me, bool nec, uint16_t addr, uint16_t cmd)
{
    if ( !me->test.running )
    {
        return false;
    }

    if ( !nec )
    {
        KLOG_D(TAG, "attempt %u: %u symbols that are not NEC; still waiting",
               (unsigned)me->test.attempt, (unsigned)me->n);
        return true;
    }

    if ( (IR_TEST_ADDR == addr) && (test_nonce(me) == cmd) )
    {
        KLOG_N(TAG, "attempt %u: nonce=0x%04X came back exactly; the emitter "
                    "and the receiver hear each other",
               (unsigned)me->test.attempt, (unsigned)test_nonce(me));
        test_finish(me, true);
        return true;
    }

    /* Not this attempt's nonce, and NOT a fault: a remote, a lamp, or our own
     * late echo. It must not spend an attempt: a retry transmits at once while
     * the replaced frame is in flight, so one collision would cascade through
     * every attempt. Only the deadline ends an attempt; do not make this a retry.
     */
    if ( (IR_TEST_ADDR == addr) && test_was_ours(me, cmd) )
    {
        KLOG_D(TAG, "attempt %u: our own echo of 0x%04X arriving late; still "
                    "waiting for nonce=0x%04X",
               (unsigned)me->test.attempt, (unsigned)cmd,
               (unsigned)test_nonce(me));
    }
    else
    {
        KLOG_W(TAG, "attempt %u: heard addr=0x%04X cmd=0x%04X, not "
                    "nonce=0x%04X; still waiting",
               (unsigned)me->test.attempt, (unsigned)addr, (unsigned)cmd,
               (unsigned)test_nonce(me));
    }
    return true;
}

/**
 * @brief Run the row: the receiver first, and only then the pair.
 *
 * Step 1 is the worker's probe; present_done() judges it and goes on to the
 * pair.
 *
 * @param me The AO.
 */
static void test_start(Ir * const me)
{
    if ( me->test.running )
    {
        KLOG_W(TAG, "a row is already running; the second request is ignored");
        return;
    }
    me->test.running = true;
    me->test.attempt = 0U;
    (void)memset(me->test.nonce, 0, sizeof(me->test.nonce));

    if ( !me->up )
    {
        KLOG_E(TAG, "the infrared block never came up; neither half can be "
                    "judged");
        test_finish(me, false);
        return;
    }

    /* Step 1 first: the probe disables the receive channel while it drives
     * the pin, losing any capture in flight, so between a send and its echo it
     * would lose the row's own frame. It waits on the C5, so it is the worker's. */
    if ( !job_submit(me, IR_OP_PRESENT) )
    {
        KLOG_E(TAG, "receiver: could not be probed (the worker has no room)");
        test_finish(me, false);
    }
}

/**
 * @brief The probe returned: judge the receiver, then start on the pair.
 *
 * @param me      The AO.
 * @param rc      What the driver answered.
 * @param present Whether the line recovered, meaningless unless @p rc is OK.
 */
static void present_done(Ir * const me, kode_status_t rc, bool present)
{
    if ( !me->test.running )
    {
        return;
    }
    if ( KODE_OK != rc )
    {
        KLOG_E(TAG, "receiver: could not be probed (rc=%d)", (int)rc);
        test_finish(me, false);
        return;
    }
    if ( !present )
    {
        KLOG_E(TAG, "receiver: absent - the line does not come back up on its "
                    "own; the receiver is the half to replace");
        test_finish(me, false);
        return;
    }
    KLOG_N(TAG, "receiver: present - the line recovers through its own "
                "pull-up");

    test_attempt(me);
}

/* ---- Status ---------------------------------------------------------------
 * The console's `ir` and the facade's last-frame read, answered from what this
 * AO holds. `ir` also reads the resting level through the worker
 * (kdrv_ir_rx_idle(): it emits nothing and leaves the RX channel alone). The
 * last frame needs no C5 and is answered at once in every state. The console
 * gets an event; the facade, a blocked task, lends its frame and a semaphore.
 */

/**
 * @brief Build the status answer and post it to @p who.
 *
 * @param me   The AO.
 * @param who  Where the answer goes.
 * @param high What the line read; false when it was not read at all.
 */
static void info_reply(Ir * const me, QActive *who, bool high)
{
    IrInfoEvt *ev = Q_NEW_X(IrInfoEvt, 0U, IR_INFO_RSP_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a status answer is lost");
        return;
    }
    ev->up        = me->up;
    ev->deaf      = me->deaf;
    ev->rx_high   = high;
    ev->have_last = me->last.have;
    ev->addr      = me->last.addr;
    ev->cmd       = me->last.cmd;
    ev->symbols   = me->last.symbols;
    ev->nec       = me->last.nec;
    ev->age_ms    = 0U;
    if ( me->last.have )
    {
        ev->age_ms = khal_time_ms() - me->last.at_ms;
    }
    QACTIVE_POST(who, &ev->super, NULL);
}

/**
 * @brief Serve a status request: ask the worker for the reading, or answer
 *        at once without it - no block, or a call already out. Never held,
 *        so it waits for no link.
 *
 * @param me  The AO.
 * @param who Where the answer goes; NULL is served and not answered.
 */
static void info_serve(Ir * const me, QActive *who)
{
    if ( NULL == who )
    {
        return;
    }
    if ( (!me->up) || me->job.busy )
    {
        info_reply(me, who, false);
        return;
    }
    me->job.who = who;
    if ( !job_submit(me, IR_OP_IDLE) )
    {
        info_reply(me, who, false);
    }
}

/**
 * @brief The reading returned: answer the status request it was for.
 *
 * @param me   The AO.
 * @param rc   What the driver answered.
 * @param high Whether the line rests high, meaningless unless @p rc is OK.
 */
static void info_done(Ir * const me, kode_status_t rc, bool high)
{
    info_reply(me, me->job.who, (KODE_OK == rc) && high);
}

/**
 * @brief Copy the last frame into the requester's own memory and wake it.
 *
 * All within the step that received the request: AO_Ir holds the requester's
 * pointers only while filling them, and nothing is armed. The stamp goes right
 * before the semaphore, so the requester can tell this answer from one it gave
 * up on.
 *
 * @param me The AO.
 * @param q  The request. A NULL @c out or @c have is skipped, not refused; a
 *           NULL @c done leaves nobody to wake, which is the caller's affair.
 */
static void last_serve(Ir * const me, IrLastEvt const *q)
{
    if ( NULL != q->have )
    {
        *q->have = me->last.have;
    }
    if ( (NULL != q->out) && me->last.have )
    {
        kdrv_ir_frame_t * const out = (kdrv_ir_frame_t *)q->out;
        out->addr    = me->last.addr;
        out->cmd     = me->last.cmd;
        out->symbols = me->last.symbols;
        out->nec     = me->last.nec;
    }
    ack_write(q->ack, q->gen);
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }
    if ( NULL != q->ask )
    {
        IrLastRspEvt * const a = q->rsp;

        a->rc   = (uint8_t)KODE_OK;
        a->have = me->last.have;
        (void)memset(&a->frame, 0, sizeof(a->frame));
        if ( me->last.have )
        {
            a->frame.addr    = me->last.addr;
            a->frame.cmd     = me->last.cmd;
            a->frame.symbols = me->last.symbols;
            a->frame.nec     = me->last.nec;
        }
        kaos_ask_answer(&s_ir_doors[IR_ASK_LAST], q->ask, &a->super, AO_Ir);
    }
}

/* ---- Requests that need the C5, while a call is out -------------------------- */

/**
 * @brief Answer a capture request that was refused, empty.
 *
 * The requester is answered here and not left asleep, and the request the AO
 * already holds is not disturbed.
 *
 * @param q  The request.
 * @param st What the asker is told: KODE_DENIED, or KODE_TIMEOUT.
 */
static void capture_refuse(IrCaptureEvt const *q, kode_status_t st)
{
    /* One route per request: its makers set the waiter, the ask or the console, never two. */
    Q_ASSERT((NULL == q->ask) || (NULL == q->done));
    if ( NULL != q->n )
    {
        *q->n = 0U;
    }
    ack_write(q->ack, q->gen);
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }
    if ( NULL != q->ask )
    {
        IrCaptureRspEvt * const a = q->rsp;

        a->rc = (uint8_t)st;
        a->n  = 0U;
        kaos_ask_answer(&s_ir_doors[IR_ASK_CAPTURE], q->ask, &a->super, AO_Ir);
    }
}

/** @brief Why a held or arriving request is refused. */
typedef enum {
    IR_REFUSE_BUSY = 0, /**< The hold queue has no room: KODE_DENIED.        */
    IR_REFUSE_NO_LINK,  /**< The C5 link would not come up: KODE_DENIED.     */
    IR_REFUSE_LATE      /**< Its budget ran out waiting for the link: KODE_TIMEOUT. */
} IrRefuse;

/**
 * @brief Refuse a held or arriving request: log it, and answer whoever waits.
 *
 * @param me  The AO.
 * @param e   The request: IR_SEND_REQ_SIG, IR_SEND_RAW_REQ_SIG,
 *            IR_CAPTURE_REQ_SIG or IR_TEST_REQ_SIG.
 * @param why Why.
 */
static void request_refuse(Ir * const me, QEvt const * const e, IrRefuse why)
{
    const char   *said = "busy";
    kode_status_t st   = KODE_DENIED;

    if ( IR_REFUSE_NO_LINK == why )
    {
        said = "no C5 link";
    }
    else if ( IR_REFUSE_LATE == why )
    {
        said = "the C5 link did not come in time";
        st   = KODE_TIMEOUT;
    }
    else
    {
        /* Busy. */
    }
    switch ( e->sig )
    {
        case IR_SEND_RAW_REQ_SIG: {
            IrSendRawEvt const * const q = Q_EVT_CAST(IrSendRawEvt);
            KLOG_W(TAG, "a raw frame of %u symbols was not sent: %s",
                   (unsigned)q->n, said);
            raw_answer(q->rc, q->done, q->gen, q->ack, q->ask, q->rsp, st);
            break;
        }
        case IR_SEND_REQ_SIG: {
            IrSendEvt const * const q = Q_EVT_CAST(IrSendEvt);
            KLOG_W(TAG, "addr=0x%04X cmd=0x%04X was not sent: %s",
                   (unsigned)q->addr, (unsigned)q->cmd, said);
            break;
        }
        case IR_CAPTURE_REQ_SIG: {
            KLOG_W(TAG, "a capture request is answered empty: %s", said);
            capture_refuse(Q_EVT_CAST(IrCaptureEvt), st);
            break;
        }
        case IR_TEST_REQ_SIG: {
            if ( (IR_REFUSE_BUSY != why) && (!me->test.running) )
            {
                KLOG_E(TAG, "neither half can be judged: %s", said);
                test_finish(me, false);
            }
            else
            {
                KLOG_W(TAG, "a row is already running; the second request is "
                            "ignored");
            }
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * @brief Hold a request until the call that is out returns.
 *
 * Kept rather than refused: the asker cannot tell "not yet" from "no". Only
 * a full queue refuses.
 *
 * @param me The AO.
 * @param e  The request.
 * @return true when it is held.
 */
static bool request_hold(Ir * const me, QEvt const * const e)
{
    bool const held = QActive_defer(&me->super, &me->deferq, e);

    if ( !held )
    {
        request_refuse(me, e, IR_REFUSE_BUSY);
    }
    return held;
}

#if defined(CONFIG_KODEOS_RADIO)
/**
 * @brief Hold a request until the C5 link is up, and ask AO_Radio for the
 *        link the first time one waits.
 *
 * @param me The AO.
 * @param e  The request.
 */
static void link_hold(Ir * const me, QEvt const * const e)
{
    uint32_t const ticks = ms_ticks(IR_LINK_CHECK_MS);

    if ( request_hold(me, e) && (!me->link_wait) )
    {
        me->link_wait  = true;
        me->link_since = khal_time_ms();
        KLOG_I(TAG, "the C5 link is down; asked for, and the request waits");
        QActive_subscribe(&me->super, RADIO_STATE_SIG);
        QTimeEvt_armX(&me->linkchk, (QTimeEvtCtr)ticks, (QTimeEvtCtr)ticks);
        radio_up_request();
    }
}

/**
 * @brief Stop waiting for the link, and stop hearing about it.
 *
 * A check already queued finds no wait and does nothing.
 *
 * @param me The AO.
 */
static void link_end(Ir * const me)
{
    me->link_wait = false;
    (void)QTimeEvt_disarm(&me->linkchk);
    if ( !me->arm_owed )
    {
        QActive_unsubscribe(&me->super, RADIO_STATE_SIG);
    }
}

/**
 * @brief Owe a listener's arm to the next UP, and ask for the link if nobody
 *        has: the first owed arm stands for every one after it.
 *
 * @param me The AO.
 */
static void arm_owe(Ir * const me)
{
    if ( me->arm_owed )
    {
        return;
    }
    me->arm_owed = true;
    KLOG_I(TAG, "a listener waits for the C5 link; the receiver is armed when it is up");
    QActive_subscribe(&me->super, RADIO_STATE_SIG);
    if ( (!me->link_wait) && (!me->link_refused) )
    {
        radio_up_request();
    }
}

/**
 * @brief Whether a held request's own budget is spent.
 *
 * A raw frame has its start budget and a capture its wait, both from the
 * moment it was posted; anything else has KODE_IR_LINK_WAIT_MS from the start
 * of the wait for the link.
 *
 * @param me  The AO.
 * @param e   The request.
 * @param now khal_time_ms().
 * @return true when it is to be answered KODE_TIMEOUT.
 */
static bool held_late(Ir const * const me, QEvt const * const e, uint32_t now)
{
    bool const link_late = (now - me->link_since) >= KODE_IR_LINK_WAIT_MS;
    bool       late      = link_late;

    if ( (QSignal)IR_SEND_RAW_REQ_SIG == e->sig )
    {
        IrSendRawEvt const * const q = Q_EVT_CAST(IrSendRawEvt);
        late = (now - q->posted_ms) > q->start_ms;
    }
    else if ( (QSignal)IR_CAPTURE_REQ_SIG == e->sig )
    {
        IrCaptureEvt const * const q = Q_EVT_CAST(IrCaptureEvt);
        late = (now - q->posted_ms) >= q->ms;
    }
    else
    {
        /* The link's own wait. */
    }
    return late;
}

/**
 * @brief IR_LINK_TO_SIG: answer every held request whose budget is spent,
 *        keeping the others in their order.
 *
 * Whatever the reason the link has not come - a bring-up still running, a
 * state publish lost to a dry pool - no request waits past its own budget,
 * and the asker's door is free again. When nothing is left the wait ends, and
 * the next request asks for the link again.
 *
 * @param me The AO.
 */
static void held_expire(Ir * const me)
{
    uint32_t const now  = khal_time_ms();
    uint16_t const held = QEQueue_getUse(&me->deferq);

    if ( !me->link_wait )
    {
        return;
    }
    for ( uint16_t i = 0U; i < held; ++i )
    {
        QEvt const * const e = QEQueue_get(&me->deferq, me->super.prio);

        if ( NULL == e )
        {
            break;
        }
        if ( held_late(me, e, now) )
        {
            request_refuse(me, e, IR_REFUSE_LATE);
        }
        else
        {
            /* Back at the end: one pass over the queue keeps its order. */
            (void)QActive_defer(&me->super, &me->deferq, e);
        }
        QF_gc(e);
    }
    if ( QEQueue_isEmpty(&me->deferq) )
    {
        link_end(me);
    }
}

/**
 * @brief Refuse every request held, the link that they waited for being gone.
 * @param me The AO.
 */
static void held_refuse(Ir * const me)
{
    QEvt const *e = QEQueue_get(&me->deferq, me->super.prio);

    while ( NULL != e )
    {
        request_refuse(me, e, IR_REFUSE_NO_LINK);
        QF_gc(e);
        e = QEQueue_get(&me->deferq, me->super.prio);
    }
}

/**
 * @brief AO_Radio's state of the link, while requests wait for it or a
 *        listener's arm is owed.
 *
 * While requests wait: UP arms the receiver (after any call out) and leaves
 * pump() to serve what is held; OFF and FAILED refuse them, and every later
 * request until the link is seen up. With only an arm owed, UP arms the
 * receiver.
 *
 * @param me The AO.
 * @param s  The link's state.
 */
static void link_heard(Ir * const me, RadioStateEvt const *s)
{
    bool const up = ((uint8_t)KRADIO_UP == s->link);

    if ( up )
    {
        me->link_refused = false;
    }
    if ( me->link_wait )
    {
        if ( up )
        {
            me->arm_owed = false;
            link_end(me);
            KLOG_I(TAG, "the C5 link is up; serving what waited for it");
            rearm(me, false);
        }
        else if ( ((uint8_t)KRADIO_OFF == s->link)
                 || ((uint8_t)KRADIO_FAILED == s->link) )
        {
            KLOG_N(TAG, "the C5 link will not come; infrared is refused until it is up");
            me->link_refused = true;
            held_refuse(me);
            link_end(me);
        }
        else
        {
            /* On its way. */
        }
    }
    else if ( me->arm_owed && up )
    {
        me->arm_owed = false;
        QActive_unsubscribe(&me->super, RADIO_STATE_SIG);
        KLOG_I(TAG, "the C5 link is up; the receiver is armed for the listener");
        rearm(me, false);
    }
    else
    {
        /* Published before the unsubscribe and dispatched after it, or on
         * the way to UP while a listener's arm is owed. */
    }
}
#endif /* CONFIG_KODEOS_RADIO */

/**
 * @brief Whether a capture is a listener's arm (kode_event.c's
 *        ir_arm_listener()): no buffer, no waiter, no deadline, so it holds
 *        nobody's memory.
 *
 * @param q The request.
 * @return true for a listener's arm.
 */
static bool capture_arms_only(IrCaptureEvt const *q)
{
    return (NULL == q->out) && (NULL == q->done) && (NULL == q->ask) && (0U == q->ms);
}

/**
 * @brief A listener's arm: arm the receiver, or owe the arm - to the next UP
 *        while the link is down, to pump() while a call is out. Answered to
 *        nobody, and never held.
 *
 * @param me The AO.
 */
static void listener_arm(Ir * const me)
{
    if ( !me->up )
    {
        return;   /* Nothing to arm: Ir_initial said so. */
    }
#if defined(CONFIG_KODEOS_RADIO)
    if ( !kdrv_radio_link_up() )
    {
        arm_owe(me);
        return;
    }
#endif
    rearm(me, false);
}

/**
 * @brief Hold a request that cannot be served now, or say that it can.
 *
 * A call out holds it until the call returns; a link that is down holds it
 * until AO_Radio has brought it up, unless AO_Radio has said it will not and
 * the link has not been seen up since: then it is refused. A block that never
 * came up waits for nothing: the request is served, and refused there.
 *
 * @param me The AO.
 * @param e  A request that needs the C5.
 * @return true when it is held or refused; false when the caller serves it.
 */
static bool request_waits(Ir * const me, QEvt const * const e)
{
    bool waits = true;
#if defined(CONFIG_KODEOS_RADIO)
    bool const link_down = !kdrv_radio_link_up();

    if ( !link_down )
    {
        me->link_refused = false;
    }
#endif

    if ( me->job.busy )
    {
        (void)request_hold(me, e);
    }
#if defined(CONFIG_KODEOS_RADIO)
    else if ( me->up && link_down && me->link_refused )
    {
        request_refuse(me, e, IR_REFUSE_NO_LINK);
    }
    else if ( me->up && (me->link_wait || link_down) )
    {
        link_hold(me, e);
    }
#endif
    else
    {
        waits = false;
    }
    return waits;
}

/**
 * @brief Serve the readiness held back during a call, ahead of a timer.
 *
 * A frame announced before a deadline or the echo window's end is heard first,
 * so a capture request is not answered empty, nor a frame published as
 * foreign, because the timer's event was handled first.
 *
 * @param me The AO.
 */
static void ready_first(Ir * const me)
{
    if ( me->rdy_pending )
    {
        me->rdy_pending = false;
        heard(me);
    }
}

/**
 * @brief Take up what was held back, once no call is out.
 *
 * Run at the end of every handled event: an owed arm is made, a capture
 * announced during the call is read, a timed-out row attempt is retried, then
 * ONE held request is recalled (at the front, keeping their order); the next
 * waits for the end of that event. Nothing held is recalled while the link is
 * asked for.
 *
 * @param me The AO.
 */
static void pump(Ir * const me)
{
    if ( me->job.busy )
    {
        return;
    }
    if ( me->rearm_due )
    {
        bool const loud = me->rearm_loud;

        me->rearm_due  = false;
        me->rearm_loud = false;
        rearm(me, loud);
    }
    if ( (!me->job.busy) && me->rdy_pending )
    {
        me->rdy_pending = false;
        heard(me);
    }
    if ( (!me->job.busy) && me->retry_due )
    {
        me->retry_due = false;
        if ( me->test.running )
        {
            test_retry(me);
        }
    }
    if ( (!me->job.busy) && (!me->link_wait) )
    {
        (void)QActive_recall(&me->super, &me->deferq);
    }
}

/* ---- The state ----------------------------------------------------------- */

/** @brief The only state: listen, transmit, and stay deaf to ourselves. */
static QState Ir_ready(Ir * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case IR_RX_RDY_SIG: {
            if ( me->job.busy )
            {
                me->rdy_pending = true;
            }
            else
            {
                heard(me);
            }
            status = Q_HANDLED();
            break;
        }
        case IR_SEND_REQ_SIG: {
            if ( !request_waits(me, e) )
            {
                IrSendEvt const * const q = Q_EVT_CAST(IrSendEvt);
                (void)send_begin(me, q->addr, q->cmd, false);
            }
            status = Q_HANDLED();
            break;
        }
        case IR_SEND_RAW_REQ_SIG: {
            if ( !request_waits(me, e) )
            {
                raw_begin(me, Q_EVT_CAST(IrSendRawEvt));
            }
            status = Q_HANDLED();
            break;
        }
        case IR_CAPTURE_REQ_SIG: {
            IrCaptureEvt const * const q = Q_EVT_CAST(IrCaptureEvt);

            if ( capture_arms_only(q) )
            {
                listener_arm(me);
            }
            else if ( !request_waits(me, e) )
            {
                capture_take(me, q);
            }
            else
            {
                /* Held or refused. */
            }
            status = Q_HANDLED();
            break;
        }
        case IR_ECHO_SIG: {
            /* One drop per counted generation (deafen()). Do not remove the
             * counter: the dropped event would end a window opened after it. */
            ready_first(me);
            if ( 0U < me->echo_stale )
            {
                me->echo_stale -= 1U;
            }
            else
            {
                me->deaf = false;
                KLOG_D(TAG, "the echo window is closed");
            }
            status = Q_HANDLED();
            break;
        }
        case IR_CAPTURE_TO_SIG: {
            /* The same generation gate: this may be the deadline of a request
             * already answered and replaced. */
            ready_first(me);
            if ( 0U < me->hold_stale )
            {
                me->hold_stale -= 1U;
            }
            else if ( me->capture.busy )
            {
                me->capture.held = false;   /* this event is its expiry */
                KLOG_D(TAG, "a capture request heard nothing in time");
                capture_answer(me, 0U, KODE_TIMEOUT);
            }
            else
            {
                /* Answered already, and its deadline counted with it. */
            }
            status = Q_HANDLED();
            break;
        }
        case IR_TEST_REQ_SIG: {
            if ( !request_waits(me, e) )
            {
                test_start(me);
            }
            status = Q_HANDLED();
            break;
        }
        case IR_INFO_REQ_SIG: {
            info_serve(me, Q_EVT_CAST(IrInfoReqEvt)->who);
            status = Q_HANDLED();
            break;
        }
        case IR_JOB_DONE_SIG: {
            on_job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            status = Q_HANDLED();
            break;
        }
        case IR_JOB_TO_SIG: {
            on_job_deadline(me);
            status = Q_HANDLED();
            break;
        }
        case IR_LAST_REQ_SIG: {
            last_serve(me, Q_EVT_CAST(IrLastEvt));
            status = Q_HANDLED();
            break;
        }
#if defined(CONFIG_KODEOS_RADIO)
        case RADIO_STATE_SIG: {
            link_heard(me, Q_EVT_CAST(RadioStateEvt));
            status = Q_HANDLED();
            break;
        }
        case IR_LINK_TO_SIG: {
            held_expire(me);
            status = Q_HANDLED();
            break;
        }
#endif
        case IR_TEST_TO_SIG: {
            /* The same gate again: an attempt's stale deadline, dispatched
             * behind the next attempt's start, would end it before it had a
             * window and could fail the row. */
            if ( 0U < me->wait_stale )
            {
                me->wait_stale -= 1U;
            }
            else if ( me->test.running )
            {
                me->test.waiting = false;   /* this event is its expiry */
                KLOG_W(TAG, "attempt %u: nothing came back within %u ms",
                       (unsigned)me->test.attempt,
                       (unsigned)KODE_IR_TEST_ECHO_MS);
                if ( me->job.busy )
                {
                    /* The retry sends a frame, which waits for the call that
                     * is out; pump() makes it when that returns. */
                    me->retry_due = true;
                }
                else
                {
                    test_retry(me);
                }
            }
            else
            {
                /* The row ended before its deadline, which counted with it. */
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    if ( Q_HANDLED() == status )
    {
        pump(me);
    }
    return status;
}
