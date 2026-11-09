/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_ir.c
 * @brief The two facade calls that block, and what each decides on its own.
 */
#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"
#include "Mockkdrv_ir.h"

#include "fakes.h"
#include "fake_khal_worker.h"
#include "kaos.h"
#include "khal_sync.h"
#include "knec.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief The frame the cases below have AO_Ir hear. */
#define HEARD_ADDR  0x0012U
#define HEARD_CMD   0x0034U

/**
 * @brief What a stand-in running inside a wait could not do, if anything.
 *
 * Set instead of asserted, and read back by the case afterwards. The reason
 * is the claim flag: both facade calls take one before they post and give it
 * back on the way out, and a Unity assertion longjmps straight to the end of
 * the case. One firing inside a wait hook - which runs INSIDE
 * khal_sem_wait_timeout(), inside the facade call - therefore skips the
 * release, and the flag stays claimed for the rest of the binary because
 * nothing on the host ever clears it: kode_init() is what does that on the
 * target and no case runs it. Every later facade case would then fail with
 * KODE_DENIED, so one real failure would arrive as a dozen false ones. A
 * hook records; the case asserts, once the call it interrupted has returned
 * and given the claim back.
 */
static const char *s_hook_fault;

/** @brief Give AO_Ir somewhere to receive, and forget whatever it held. */
static void ir_fresh(void)
{
    qf_support_init();
    qf_support_give_queue(AO_Ir);
    fake_sem_reset();
    fake_time_reset();
    s_hook_fault = NULL;

    for ( ;; )
    {
        const QEvt *e = NULL;
        if ( xQueueReceive(AO_Ir->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
}

/** @brief How many requests are waiting for AO_Ir. */
static unsigned ir_pending(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_Ir->eQueue);
}

/**
 * @brief Make sure AO_Ir is constructed and dispatching, once per binary.
 *
 * test_ir_ao.c owns AO_Ir's bring-up and its cases run ahead of this file's,
 * so the claim below normally finds the work already done. The branch exists
 * so this file still stands on its own; if it ever runs first, test_ir_ao.c
 * says so loudly rather than quietly ("the driver was never given a
 * callback") because its own stub would then never be installed.
 */
static void ir_ao_up(void)
{
    Mockkdrv_ir_Init();
    qf_support_init();
    /* The block is behind the C5 link: up, so nothing waits for it. */
    fake_radio_set_link(true);

    /* The worker posts its done event with a margin, which QP answers from
     * the queue it lays out in the AO's own osObject. Another file's fake of
     * a neighbour may have given AO_Ir a queue of the heap's, which QP reads
     * as always full - and a worker waits for room as long as it takes. Put
     * the proper queue in its place. */
    if ( (NULL != AO_Ir->eQueue)
        && ((void *)AO_Ir->eQueue != (void *)&AO_Ir->osObject.queue) )
    {
        const QEvt *held = NULL;

        while ( xQueueReceive(AO_Ir->eQueue, &held, 0U) == pdTRUE )
        {
            QF_gc(held);
        }
        AO_Ir->eQueue = NULL;
    }

    if ( qf_support_claim_ao(AO_Ir) )
    {
        kdrv_ir_init_IgnoreAndReturn(KODE_OK);
        Ir_ctor();
        qf_support_give_queue(AO_Ir);
        AO_Ir->prio  = KODE_PRIO_IR;
        AO_Ir->pthre = KODE_PRIO_IR;
        QActive_register_(AO_Ir);
        QASM_INIT(AO_Ir, (void *)0, 0U);
        /* The receiver is armed by the first job AO_Ir hands its worker. */
        kdrv_ir_rx_arm_IgnoreAndReturn(KODE_OK);
        (void)fake_khal_worker_run_next_for("ir_worker");
        (void)qf_support_settle(AO_Ir);
    }
    ir_fresh();
}

/**
 * @brief Hand AO_Ir one NEC frame through its own receive path.
 *
 * The readiness event is what the receive interrupt posts and nothing else,
 * so posting it here is the whole of what the interrupt does. AO_Ir then
 * reads the driver - mocked - in its own step, which is where its record of
 * the last frame is written.
 */
static void ir_hears_a_frame(void)
{
    static const QEvt rdy = QEVT_INITIALIZER(IR_RX_RDY_SIG);
    kdrv_ir_symbol_t  sym[KNEC_SYMBOLS];
    size_t            n = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          knec_encode(HEARD_ADDR, HEARD_CMD, sym,
                                      KNEC_SYMBOLS, &n));
    kdrv_ir_rx_take_ExpectAnyArgsAndReturn(n);
    kdrv_ir_rx_take_ReturnMemThruPtr_out((void *)sym,
                                         (int)(n * sizeof(kdrv_ir_symbol_t)));
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);

    QACTIVE_POST(AO_Ir, &rdy, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    /* The re-arm is a call to the C5, and the worker's. */
    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for("ir_worker"));
    (void)qf_support_settle(AO_Ir);
}

/**
 * @brief Let AO_Ir run, as the fake semaphore's stand-in for another task.
 *
 * Armed with fake_sem_wait_hook(), so it runs from INSIDE the facade's own
 * wait. That is where AO_Ir runs on the board too, being above every other
 * Active Object, and it is the only place a single-threaded host can put it.
 *
 * The signal afterwards stands in for kode_init() and for nothing else: that
 * is what creates the semaphore the facade lends AO_Ir, no host case runs it,
 * and AO_Ir does not signal a semaphore it was handed as NULL. Everything
 * else here is the real thing - AO_Ir writes the frame through the pointer
 * the request actually carried, in its own step.
 */
static void ir_runs_while_asleep(void)
{
    (void)qf_support_pump(AO_Ir, 4U);
    khal_sem_signal(NULL);
}

/** @brief What a second, concurrent facade call was answered with. */
static kode_status_t s_second_rc;

/** @brief A second kode_block_ir_capture_symbols(), from inside the first one's wait. */
static void capture_again(void)
{
    static kdrv_ir_symbol_t buf[4];
    s_second_rc = kode_block_ir_capture_symbols(buf, 4U, NULL, 100U);
}

/** @brief A second kode_block_ir_last(), from inside the first one's wait. */
static void last_again(void)
{
    static kdrv_ir_frame_t frame;
    s_second_rc = kode_block_ir_last(&frame);
}

/** @brief Take the next request; fails the case when there is none. */
static const IrCaptureEvt *ir_capture_take(void)
{
    static const QEvt *s_held;
    const QEvt        *e = NULL;

    if ( NULL != s_held )
    {
        QF_gc(s_held);
        s_held = NULL;
    }
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Ir->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(IR_CAPTURE_REQ_SIG, (int)e->sig);
    s_held = e;
    return (const IrCaptureEvt *)e;
}

/** @brief Take the next last-frame request; fails the case when there is none. */
static const IrLastEvt *ir_last_take(void)
{
    static const QEvt *s_held;
    const QEvt        *e = NULL;

    if ( NULL != s_held )
    {
        QF_gc(s_held);
        s_held = NULL;
    }
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Ir->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(IR_LAST_REQ_SIG, (int)e->sig);
    s_held = e;
    return (const IrLastEvt *)e;
}

/**
 * @brief AO_Ir answering a request whose caller has already given up.
 *
 * One token on a binary semaphore and nothing else - which is exactly what
 * an abandoned request leaves behind, since AO_Ir signals once per request on
 * every path it has, deadline included. The NULL is the fake's own doing:
 * kode_init() creates the semaphores and no host case runs it.
 */
static void ir_answers_too_late(void)
{
    khal_sem_signal(NULL);
}

/** @brief How many symbols the stand-in below writes into the request. */
static size_t s_answer_n;

/** @brief What it writes: distinguishable, and different in each field. */
static kdrv_ir_symbol_t answer_symbol(size_t i)
{
    kdrv_ir_symbol_t s;

    s.mark_us  = (uint16_t)(0x1000U + (unsigned)i);
    s.space_us = (uint16_t)(0x2000U + (unsigned)i);
    return s;
}

/**
 * @brief Take the head of AO_Ir's queue from inside a wait hook.
 *
 * The same job as ir_capture_take() and ir_last_take() with every assertion
 * taken out of it, for the reason s_hook_fault carries: this runs inside a
 * facade call that is holding a claim, and an assertion here would leave the
 * claim taken for the rest of the binary. It reports and returns instead.
 *
 * @param sig The signal the head is required to carry.
 * @return The event, still owned by the caller, which must QF_gc() it; NULL
 *         when there was nothing to take or it was not @p sig.
 */
static const QEvt *ir_hook_head(enum_t sig)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Ir->eQueue, &e, 0U) != pdTRUE )
    {
        s_hook_fault = "the stand-in found nothing in AO_Ir's queue";
        return NULL;
    }
    if ( (enum_t)e->sig != sig )
    {
        s_hook_fault = "the stand-in found the wrong request at the head";
        QF_gc(e);
        return NULL;
    }
    return e;
}

/**
 * @brief AO_Ir answering the capture request at the head of its queue.
 *
 * Everything AO_Ir does on that path, in the order it does it: the symbols,
 * the count, then the generation of the request being answered, then the
 * semaphore. The head of the queue is the OLDEST request outstanding, which
 * is what makes this stand in for both things a case needs - a caller's own
 * answer when its request is the only one, and an answer owed to a caller
 * that gave up when it is not.
 *
 * Armed with fake_sem_wait_hook(), so it runs from inside a facade wait, on
 * the same storage that wait will read. That is the whole hazard in one
 * place.
 */
static void ir_answers_head_capture(void)
{
    const QEvt * const   e = ir_hook_head((enum_t)IR_CAPTURE_REQ_SIG);
    const IrCaptureEvt  *req;
    kdrv_ir_symbol_t    *out;

    if ( NULL == e )
    {
        return;
    }
    req = (const IrCaptureEvt *)e;
    out = (kdrv_ir_symbol_t *)req->out;
    if ( NULL == out )
    {
        s_hook_fault = "the capture request carried nowhere to write";
        QF_gc(e);
        return;
    }

    for ( size_t i = 0U; i < s_answer_n; ++i )
    {
        out[i] = answer_symbol(i);
    }
    if ( NULL != req->n )
    {
        *req->n = s_answer_n;
    }
    if ( NULL != req->ack )
    {
        *req->ack = req->gen;
    }
    khal_sem_signal(req->done);
    QF_gc(e);
}

/**
 * @brief AO_Ir answering the last-frame request at the head of its queue.
 *
 * The last_serve() shape: the frame, the flag, the generation, the semaphore.
 */
static void ir_answers_head_last(void)
{
    const QEvt * const  e = ir_hook_head((enum_t)IR_LAST_REQ_SIG);
    const IrLastEvt    *req;
    kdrv_ir_frame_t    *out;

    if ( NULL == e )
    {
        return;
    }
    req = (const IrLastEvt *)e;
    out = (kdrv_ir_frame_t *)req->out;
    if ( NULL == out )
    {
        s_hook_fault = "the last-frame request carried nowhere to write";
        QF_gc(e);
        return;
    }

    out->addr    = HEARD_ADDR;
    out->cmd     = HEARD_CMD;
    out->symbols = KNEC_SYMBOLS;
    out->nec     = true;
    if ( NULL != req->have )
    {
        *req->have = true;
    }
    if ( NULL != req->ack )
    {
        *req->ack = req->gen;
    }
    khal_sem_signal(req->done);
    QF_gc(e);
}

/** @brief How long the wait that a stand-in interrupted had asked for. */
static uint32_t s_wait_when_answered;

/** @brief How long the foreign answer below takes to arrive, in ms. */
#define FOREIGN_ANSWER_MS  400U

/**
 * @brief Let time pass, then answer the request at the head of the queue.
 *
 * ir_answers_head_capture() with a clock that moves, and the movement is the
 * whole point. A caller that discards a foreign answer has been asleep for
 * however long that answer took to arrive, and must resume its wait for what
 * is LEFT of its deadline. The host clock never advances on its own, so
 * nothing else in this file can tell that arithmetic from one that starts the
 * whole deadline again.
 */
static void ir_answers_head_capture_late(void)
{
    s_wait_when_answered = fake_sem_last_wait_ms();
    fake_time_advance_ms(FOREIGN_ANSWER_MS);
    ir_answers_head_capture();
}

/** @brief Fail the case with whatever a stand-in could not do, if anything. */
static void ir_hook_ok(void)
{
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
}

/* ---- What is refused before anything is posted ---------------------------
 * ir_capture_request() has no opinion on a NULL or empty buffer - it would
 * happily arm a deadline over nothing and answer it later with zero
 * symbols, a wasted round trip through AO_Ir for an answer the caller could
 * have been given on the spot. The facade catches both before AO_Ir's queue
 * ever sees them.
 */

TEST_CASE("a null buffer is refused before anything reaches AO_Ir",
          "[kode_ir]")
{
    ir_fresh();

    size_t n = 99U;
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_ir_capture_symbols(NULL, 4U, &n, 100U));
    TEST_ASSERT_EQUAL_UINT(0U, ir_pending());
    /* Untouched, the same rule kode_sys_nvs_get() and kode_block_http_get_buffer() keep: a
     * refusal must not overwrite an output the caller already trusted. */
    TEST_ASSERT_EQUAL_UINT(99U, n);
}

TEST_CASE("a zero-capacity buffer is refused before anything reaches AO_Ir",
          "[kode_ir]")
{
    ir_fresh();

    kdrv_ir_symbol_t buf[4];
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_ir_capture_symbols(buf, 0U, NULL, 100U));
    TEST_ASSERT_EQUAL_UINT(0U, ir_pending());
}

/* ---- What the facade waits, against what it hands AO_Ir -------------------
 * AO_Ir rounds the millisecond count it is given UP to whole QP ticks before
 * arming its own deadline (ir_ao.c's ms_ticks()), so AO_Ir's real deadline
 * can land up to one tick after the value it was asked for. A caller who
 * waited exactly that value could give up a moment before AO_Ir was ever
 * going to answer. kode_block_ir_capture_symbols() therefore waits longer than the
 * deadline it hands AO_Ir - never the same, never less.
 */

TEST_CASE("kode_block_ir_capture_symbols forwards the caller's own deadline to AO_Ir",
          "[kode_ir]")
{
    ir_fresh();

    kdrv_ir_symbol_t buf[4];
    size_t           n = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, &n, 250U));

    const IrCaptureEvt * const req = ir_capture_take();
    TEST_ASSERT_EQUAL_UINT32(250U, req->ms);
    TEST_ASSERT_EQUAL_UINT(4U, req->cap);
}

TEST_CASE("kode_block_ir_capture_symbols waits longer than the deadline it hands AO_Ir",
          "[kode_ir]")
{
    ir_fresh();

    kdrv_ir_symbol_t buf[4];
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 250U));

    const IrCaptureEvt * const req = ir_capture_take();
    TEST_ASSERT_EQUAL_UINT32(250U, req->ms);

    /* Strictly greater, not merely different: an implementation that waited
     * LESS than AO_Ir's own deadline would fail this the same as one that
     * waited the same - both let this call give up before AO_Ir necessarily
     * has. */
    TEST_ASSERT_GREATER_THAN_UINT32(req->ms, fake_sem_last_wait_ms());
}

TEST_CASE("a zero deadline is still handed to AO_Ir and still outlasted",
          "[kode_ir]")
{
    ir_fresh();

    kdrv_ir_symbol_t buf[4];
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 0U));

    const IrCaptureEvt * const req = ir_capture_take();
    TEST_ASSERT_EQUAL_UINT32(0U, req->ms);
    TEST_ASSERT_GREATER_THAN_UINT32(0U, fake_sem_last_wait_ms());
}

/* The other end of the same scale, and the one that breaks quietly. A wait is
 * turned into ticks by a 32-bit multiplication further down, so a huge
 * millisecond count wraps there into a SHORT wait - a caller asking to wait
 * forever would be told it timed out almost at once. The ceiling is AO_Ir's
 * own: it holds a request for at most KODE_SYS_IR_TIMER_MS_MAX, so there is
 * nothing past that to wait for anyway. */
TEST_CASE("a deadline nothing could wait out is clamped to one AO_Ir arms",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4];

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
                          kode_block_ir_capture_symbols(buf, 4U, NULL, UINT32_MAX));

    /* Forwarded as asked - the clamp AO_Ir needs is AO_Ir's own to apply. */
    const IrCaptureEvt * const req = ir_capture_take();
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, req->ms);

    /* But waited for as long as AO_Ir can hold it, and not a millisecond
     * that the conversion cannot carry. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_SYS_IR_TIMER_MS_MAX,
                                        fake_sem_last_wait_ms());
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(KODE_SYS_IR_TIMER_MS_MAX + 1000U,
                                     fake_sem_last_wait_ms());
}

/* ---- Whose memory AO_Ir is lent, and who may read it ----------------------
 * Both waits are bounded, so both can end with the request still sitting in
 * AO_Ir's queue - and a call that returns takes its stack frame with it while
 * AO_Ir still holds whatever it was lent. So neither call lends AO_Ir
 * anything of the caller's: what it gets is the facade's own storage, which
 * outlives every caller, and the caller's buffer is filled from that
 * afterwards - only when the wait genuinely ended on an answer.
 */

TEST_CASE("a capture lends AO_Ir the facade's buffer, not the caller's",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4];

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 250U));

    const IrCaptureEvt * const req = ir_capture_take();
    TEST_ASSERT_NOT_NULL(req->out);
    TEST_ASSERT_FALSE_MESSAGE((void *)buf == req->out,
                              "AO_Ir was lent the caller's own buffer");
    /* And somewhere to write the count, whether or not the caller wanted it. */
    TEST_ASSERT_NOT_NULL(req->n);
}

TEST_CASE("a last-frame read lends AO_Ir the facade's frame, not the caller's",
          "[kode_ir]")
{
    kdrv_ir_frame_t frame = { 0U, 0U, 0U, false };

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_last(&frame));

    const IrLastEvt * const req = ir_last_take();
    TEST_ASSERT_NOT_NULL(req->out);
    TEST_ASSERT_FALSE_MESSAGE(req->out == (void *)&frame,
                              "AO_Ir was lent the caller's own frame");
    TEST_ASSERT_NOT_NULL(req->have);
}

/* A wait that ran out is not an answer, and reporting it as one is worse than
 * useless: the caller is told a capture completed with nothing in it, when in
 * fact nobody has answered yet and the count it would have read is a value
 * this call never received. */
TEST_CASE("a capture whose wait ran out says so, and writes nothing",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[2] = { { 0xAAAAU, 0xBBBBU }, { 0xCCCCU, 0xDDDDU } };
    size_t           n      = 99U;

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 2U, &n, 50U));
    /* Both outputs untouched: nothing was written, so nothing is reported. */
    TEST_ASSERT_EQUAL_UINT(99U, n);
    TEST_ASSERT_EQUAL_HEX16(0xAAAAU, buf[0].mark_us);
    TEST_ASSERT_EQUAL_HEX16(0xDDDDU, buf[1].space_us);
}

/* ---- What a capture that IS answered copies out --------------------------
 * The other half of the shared buffer: what a caller gets when its own answer
 * does arrive. Everything here goes through storage this facade owns and two
 * clamps around it, so nothing is copied out on trust - not the count AO_Ir
 * reported, and not the capacity the caller asked for.
 */

TEST_CASE("a capture that is answered copies the symbols out", "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4] = { { 0U, 0U }, { 0U, 0U }, { 0U, 0U },
                                { 0xEEEEU, 0xFFFFU } };
    size_t           n      = 99U;
    kode_status_t    rc;

    ir_fresh();
    s_answer_n = 3U;
    fake_sem_wait_hook(&ir_answers_head_capture);

    rc = kode_block_ir_capture_symbols(buf, 4U, &n, 100U);
    ir_hook_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, rc);

    TEST_ASSERT_EQUAL_UINT(3U, n);
    for ( size_t i = 0U; 3U > i; ++i )
    {
        TEST_ASSERT_EQUAL_HEX16(answer_symbol(i).mark_us, buf[i].mark_us);
        TEST_ASSERT_EQUAL_HEX16(answer_symbol(i).space_us, buf[i].space_us);
    }
    /* Three were reported, so three were written and the fourth is the
     * caller's own still. */
    TEST_ASSERT_EQUAL_HEX16(0xEEEEU, buf[3].mark_us);
}

/* The copy is clamped on the way out because the storage is shared: the count
 * comes from another Active Object, and a call with a smaller capacity than
 * the one whose symbols are sitting in that buffer is exactly the copy that
 * would run past the caller's array. */
TEST_CASE("a capture copies out no more than the caller asked for",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4] = { { 0U, 0U }, { 0U, 0U },
                                { 0xEEEEU, 0xFFFFU }, { 0xEEEEU, 0xFFFFU } };
    size_t           n      = 99U;
    kode_status_t    rc;

    ir_fresh();
    /* More than this call's capacity: what the shared buffer holds is not
     * this call's business, and six is what a longer capture left there. */
    s_answer_n = 6U;
    fake_sem_wait_hook(&ir_answers_head_capture);

    rc = kode_block_ir_capture_symbols(buf, 2U, &n, 100U);
    ir_hook_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, rc);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, n, "and the count says so too");
    TEST_ASSERT_EQUAL_HEX16(answer_symbol(1U).space_us, buf[1].space_us);
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xEEEEU, buf[2].mark_us,
                                    "the copy ran past the caller's array");
    TEST_ASSERT_EQUAL_HEX16(0xEEEEU, buf[3].mark_us);
}

/* And clamped on the way in, against the same buffer: the receiver keeps
 * KDRV_IR_RX_MAX_SYMBOLS and the driver truncates a longer capture at that
 * ceiling anyway, so asking AO_Ir for more would be asking it to write past
 * the storage this facade lends it. */
TEST_CASE("a capture asking for more than the receiver keeps asks for less",
          "[kode_ir]")
{
    static kdrv_ir_symbol_t big[KDRV_IR_RX_MAX_SYMBOLS + 4U];

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
                          kode_block_ir_capture_symbols(big, KDRV_IR_RX_MAX_SYMBOLS + 4U,
                                          NULL, 50U));

    const IrCaptureEvt * const req = ir_capture_take();
    TEST_ASSERT_EQUAL_UINT(KDRV_IR_RX_MAX_SYMBOLS, req->cap);
}

/* ---- The answer a caller that gave up is still owed -----------------------
 * The semaphore is binary and AO_Ir signals once per request on every path it
 * has, so a caller that gave up before its answer landed leaves that answer
 * owed. It arrives later - on this same semaphore, into this same storage -
 * and it can arrive while a caller that came afterwards is asleep. Waking on
 * it and reading it would not be reading something stale: it would be
 * reporting somebody else's answer, out of a buffer AO_Ir may be writing at
 * that very moment. Every request therefore carries a generation, and a
 * caller reads nothing until the generation it finds is its own.
 */

TEST_CASE("an abandoned capture answer is not read as the next caller's",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4];
    size_t           n = 99U;

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 50U));
    ir_answers_too_late();

    /* Its own wait ends on its own answer or not at all - and nothing has
     * answered this one. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, &n, 50U));
    TEST_ASSERT_EQUAL_UINT(99U, n);
}

TEST_CASE("an abandoned last-frame answer is not read as the next caller's",
          "[kode_ir]")
{
    kdrv_ir_frame_t frame = { 0xBEEFU, 0U, 0U, false };

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_last(&frame));
    ir_answers_too_late();

    /* KODE_TIMEOUT, not KODE_DENIED: taking the stale token would have made
     * this call say the receiver has heard nothing, which is a different
     * claim entirely and a false one. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_last(&frame));
    TEST_ASSERT_EQUAL_HEX16(0xBEEFU, frame.addr);
}

/* The one that a poll before posting cannot reach, and the reason the
 * generation exists. The abandoned answer does not arrive before the next
 * caller starts - it arrives DURING that caller's wait, which is when AO_Ir
 * actually gets round to the request the first caller left behind. The
 * stand-in writes it exactly where AO_Ir would: into the shared buffer this
 * caller's own request also points at. */
TEST_CASE("an answer owed to a caller that gave up is not the next one's",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4] = { { 0xAAAAU, 0xBBBBU }, { 0U, 0U },
                                { 0U, 0U }, { 0U, 0U } };
    size_t           n      = 99U;

    ir_fresh();

    /* One caller gives up. Its request stays in AO_Ir's queue, unanswered. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 50U));
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());

    s_answer_n = 3U;
    fake_sem_wait_hook(&ir_answers_head_capture);

    /* The next caller must not report what that one was owed, however
     * plausible it looks: three symbols and a token, arriving while this
     * call is asleep on the very semaphore they were meant for. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, &n, 50U));
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(99U, n,
                                   "somebody else's count was reported");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xAAAAU, buf[0].mark_us,
                                    "somebody else's symbols were copied");
    /* The answer really was produced - this call's own request is what is
     * left in the queue, the first one's having been taken to answer it. */
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());
}

TEST_CASE("a last-frame answer owed to a caller that gave up is not the next one's",
          "[kode_ir]")
{
    kdrv_ir_frame_t frame = { 0xBEEFU, 0U, 0U, false };

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_last(&frame));
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());

    fake_sem_wait_hook(&ir_answers_head_last);

    /* A frame, a have of true and a token - all of it real, and none of it
     * this call's. KODE_OK here would hand the application a frame nobody
     * ever told this call about. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_last(&frame));
    ir_hook_ok();
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(0xBEEFU, frame.addr,
                                    "somebody else's frame was copied out");
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());
}

/* The arithmetic that resuming needs and nothing else here exercises. A
 * caller that discards a foreign answer does not start its deadline again -
 * it waits for what is left of the one it asked for, having already spent
 * part of it asleep. Every other case in this file runs on a clock that never
 * moves, so the two are indistinguishable in all of them; here the clock is
 * advanced while the foreign answer is produced, which is the only thing that
 * tells them apart. */
TEST_CASE("a wait resumed after a foreign answer is short by what it spent",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4] = { { 0xAAAAU, 0xBBBBU }, { 0U, 0U },
                                { 0U, 0U }, { 0U, 0U } };
    size_t           n      = 99U;

    ir_fresh();

    /* One caller gives up, leaving its request in AO_Ir's queue - and with it
     * the answer that will be produced inside the next caller's wait. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 1000U));
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());

    s_answer_n           = 3U;
    s_wait_when_answered = 0U;
    fake_sem_wait_hook(&ir_answers_head_capture_late);

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, &n, 1000U));
    ir_hook_ok();

    /* The stand-in ran inside a real wait, so there is a first wait to
     * compare the second against. */
    TEST_ASSERT_GREATER_THAN_UINT32(0U, s_wait_when_answered);
    TEST_ASSERT_GREATER_THAN_UINT32(FOREIGN_ANSWER_MS, s_wait_when_answered);

    /* The second wait is the remainder, to the millisecond. An implementation
     * that asked for the whole deadline again would show
     * s_wait_when_answered here and could be kept asleep for very nearly
     * twice what its caller asked for. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(s_wait_when_answered - FOREIGN_ANSWER_MS,
                                     fake_sem_last_wait_ms(),
                                     "the deadline was restarted, not resumed");
    /* And nothing of the foreign answer was reported as this caller's. */
    TEST_ASSERT_EQUAL_UINT(99U, n);
    TEST_ASSERT_EQUAL_HEX16(0xAAAAU, buf[0].mark_us);
}

/* ---- kode_block_ir_last(): whose record it reports ------------------------------
 * The name promises the last frame HEARD, and that is AO_Ir's record, not
 * this facade's. An application that never calls kode_block_ir_capture_symbols() still
 * gets every frame the receiver produced - and that is the whole point,
 * because kode_block_ir_capture_symbols() is a blocking call an application is not
 * obliged to make.
 */

TEST_CASE("kode_block_ir_last refuses a null out", "[kode_ir]")
{
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_ir_last(NULL));
}

/* The case that pins the whole of it. Nothing in this file ever completes a
 * kode_block_ir_capture_symbols() - AO_Ir answers none of the requests the cases above
 * leave in its queue - so a facade reporting what IT last decoded has
 * nothing at all to report here, while AO_Ir has heard a frame and knows
 * exactly which one. */
TEST_CASE("kode_block_ir_last reports what AO_Ir heard, with no capture of its own",
          "[kode_ir]")
{
    kdrv_ir_frame_t frame = { 0U, 0U, 0U, false };

    ir_ao_up();
    ir_hears_a_frame();

    fake_sem_reset();
    fake_sem_wait_hook(&ir_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_ir_last(&frame));

    TEST_ASSERT_TRUE(frame.nec);
    TEST_ASSERT_EQUAL_HEX16(HEARD_ADDR, frame.addr);
    TEST_ASSERT_EQUAL_HEX16(HEARD_CMD, frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(KNEC_SYMBOLS, frame.symbols);

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A round trip that never comes back is not "nothing was heard" - it means
 * the system stopped, and saying KODE_DENIED for it would tell an
 * application the receiver is quiet when in fact nobody answered. */
TEST_CASE("kode_block_ir_last says so when AO_Ir does not answer at all",
          "[kode_ir]")
{
    kdrv_ir_frame_t frame = { 0xBEEFU, 0U, 0U, false };

    ir_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_last(&frame));
    /* It did ask, though - the question reached AO_Ir's queue. */
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());
    TEST_ASSERT_EQUAL_HEX16(0xBEEFU, frame.addr);
}

/* ---- One caller at a time, and the other one told so ----------------------
 * Both blocking calls sleep on a semaphore created once at startup, because
 * khal_sync.h has no destructor and one per call would leak one per call.
 * That makes each of them non-reentrant: two callers sharing one token would
 * wake each other and read answers meant for somebody else, silently. The
 * hook below is a second caller arriving at the only moment it can do harm -
 * while the first is asleep - and the answer it must get is a refusal.
 */

TEST_CASE("a second kode_block_ir_capture_symbols while the first is asleep is refused",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4];

    ir_fresh();
    s_second_rc = KODE_OK;
    fake_sem_wait_hook(&capture_again);

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 250U));

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_second_rc,
                                  "the second caller is turned away");
    /* And turned away before AO_Ir heard of it: one request in the queue,
     * not two, so the first caller's deadline is not displaced either. */
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());
}

TEST_CASE("a second kode_block_ir_last while the first is asleep is refused",
          "[kode_ir]")
{
    kdrv_ir_frame_t frame = { 0U, 0U, 0U, false };

    ir_fresh();
    s_second_rc = KODE_OK;
    fake_sem_wait_hook(&last_again);

    /* Asserted, not discarded: a first call that quietly refused itself
     * would leave the second one's refusal proving nothing. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_last(&frame));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, s_second_rc);
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());
}

/* The refusal is for a caller that overlaps, not for the second caller ever:
 * a claim not given back would turn one call into a permanent lockout, which
 * is a worse failure than the one it was guarding against. */
TEST_CASE("the claim is given back, so the next caller is let through",
          "[kode_ir]")
{
    kdrv_ir_symbol_t buf[4];

    ir_fresh();
    /* Neither is KODE_DENIED: each was let through and posted its own
     * request. Both time out because nothing answers either one here. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 10U));
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_capture_symbols(buf, 4U, NULL, 10U));
    TEST_ASSERT_EQUAL_UINT(2U, ir_pending());
}

/* ---- kode_block_ir_send_raw() -----------------------------------------------------
 * The frame is copied into the facade's own buffer and lent to AO_Ir: the
 * caller may reuse its array the moment the call returns.
 */

/** @brief Symbols the cases send. */
#define RAW_N           3U
/** @brief A carrier other than our receiver's. */
#define RAW_CARRIER_HZ  40000U

static const void      *s_raw_lent;                 /**< What AO_Ir was lent. */
static kode_sys_ir_symbol_t s_raw_got[RAW_N];       /**< What it held.        */
static uint16_t         s_raw_got_n;
static uint32_t         s_raw_got_hz;

/** @brief Stand in for AO_Ir: take the raw request at the head and answer OK. */
static void ir_answers_head_raw(void)
{
    const QEvt * const e = ir_hook_head((enum_t)IR_SEND_RAW_REQ_SIG);

    if ( NULL == e )
    {
        return;
    }
    const IrSendRawEvt * const req = (const IrSendRawEvt *)e;
    s_raw_lent   = req->sym;
    s_raw_got_n  = req->n;
    s_raw_got_hz = req->carrier_hz;
    if ( (NULL != req->sym) && (RAW_N >= req->n) )
    {
        (void)memcpy(s_raw_got, req->sym, req->n * sizeof(kode_sys_ir_symbol_t));
    }
    if ( NULL != req->rc )
    {
        *req->rc = (uint32_t)KODE_OK;
    }
    if ( NULL != req->ack )
    {
        *req->ack = req->gen;
    }
    khal_sem_signal(req->done);
    QF_gc(e);
}

TEST_CASE("kode_block_ir_send_raw refuses a bad argument and posts nothing", "[kode_ir]")
{
    kode_sys_ir_symbol_t sym[RAW_N] = { { 9000U, 4500U }, { 560U, 560U }, { 560U, 0U } };

    ir_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_ir_send_raw(NULL, RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_ir_send_raw(sym, 0U, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kode_block_ir_send_raw(sym, KODE_SYS_IR_RAW_MAX_SYMBOLS + 1U,
                                                 RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_ir_send_raw(sym, RAW_N, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, ir_pending());
}

TEST_CASE("kode_block_ir_send_raw lends the frame while it waits and returns the answer", "[kode_ir]")
{
    kode_sys_ir_symbol_t sym[RAW_N] = { { 9000U, 4500U }, { 560U, 1690U }, { 560U, 0U } };
    kode_status_t        rc;

    ir_fresh();
    s_raw_lent = NULL;
    fake_sem_wait_hook(&ir_answers_head_raw);

    rc = kode_block_ir_send_raw(sym, RAW_N, RAW_CARRIER_HZ);
    ir_hook_ok();
    TEST_ASSERT_EQUAL_INT(KODE_OK, rc);
    TEST_ASSERT_NOT_NULL(s_raw_lent);
    TEST_ASSERT_EQUAL_UINT16(RAW_N, s_raw_got_n);
    TEST_ASSERT_EQUAL_UINT32(RAW_CARRIER_HZ, s_raw_got_hz);
    TEST_ASSERT_EQUAL_MEMORY(sym, s_raw_got, sizeof(sym));
}

/* A call that timed out leaves its answer owed: until AO_Ir has answered the
 * old request, which still writes the call's status, the next is refused. */
TEST_CASE("kode_block_ir_send_raw refuses the next send while an answer is owed",
          "[kode_ir]")
{
    kode_sys_ir_symbol_t sym[RAW_N] = { { 9000U, 4500U }, { 560U, 560U }, { 560U, 0U } };

    ir_fresh();
    /* No stand-in: nothing answers, and the wait runs out. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ir_send_raw(sym, RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED,
                                  kode_block_ir_send_raw(sym, RAW_N, RAW_CARRIER_HZ),
                                  "a second send went out under an unanswered one");
    TEST_ASSERT_EQUAL_UINT(1U, ir_pending());

    /* AO_Ir answers the old request at last; the path is free again. */
    ir_answers_head_raw();
    ir_hook_ok();
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_ir_send_raw(sym, RAW_N, RAW_CARRIER_HZ));
    ir_hook_ok();
}
