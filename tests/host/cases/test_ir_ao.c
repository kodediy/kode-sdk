/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ir_ao.c
 * @brief AO_Ir: what it publishes, and what it refuses to publish.
 */
#include <string.h>

#include "unity.h"

#include "qpc.h"
#include "Mockkdrv_ir.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "khal_sync.h"
#include "knec.h"
#include "kode_sys.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "qf_support.h"

/* The frame every case here sends and hears: two bits set out of thirty-two,
 * so 52.2 ms on the wire. */
#define FRAME_ADDR  0x0004U
#define FRAME_CMD   0x0008U

/* How many 10 ms ticks that frame's suppression window lasts: 53 ms rounded
 * up from the symbols, plus KODE_IR_ECHO_TAIL_MS, rounded up again to whole
 * ticks. Spelled out rather than computed, so that changing the tail changes
 * a number somebody reads - which is what happened when the block moved to
 * the C5 and the tail went from 30 ms to 120. */
#define ECHO_TICKS  18U

/* What a capture request here asks to wait, and the same in whole ticks. A
 * requester's deadline is the thing that keeps AO_Ir from writing into a
 * stack frame that has already returned, so every case that makes a request
 * states one. */
#define HOLD_MS     50U
#define HOLD_TICKS  5U

/** @brief What kdrv_ir_init() was asked to call from the receive interrupt. */
static kdrv_ir_rx_cb_t s_rx_cb;
static void           *s_rx_ctx;

static kode_status_t capture_init(kdrv_ir_rx_cb_t cb, void *ctx, int ncalls)
{
    (void)ncalls;
    s_rx_cb  = cb;
    s_rx_ctx = ctx;
    return KODE_OK;
}

/** @brief The name AO_Ir gives its worker. */
#define IR_WORKER  "ir_worker"

/** @brief What the worker held the moment AO_Ir came up, and whether its
 *         stack is in PSRAM; read once, by the first case. */
static unsigned s_start_jobs;
static bool     s_start_psram;
static unsigned s_start_buffers;

/**
 * @brief Run the worker's oldest job, and let the AO read its answer.
 *
 * The job runs on the case's own thread, as the fake worker has it, and the
 * done event it posts is dispatched at once with no tick, so simulated time
 * does not move.
 */
static void run_job(void)
{
    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_run_next_for(IR_WORKER),
                             "the worker had a job to run");
    (void)qf_support_settle(AO_Ir);
}

/**
 * @brief Run every job the worker holds, and the ones their answers submit.
 *
 * A row is a chain of them - the probe, then a frame - and a recalled request
 * can start another once a job returns.
 */
static void run_jobs(void)
{
    unsigned guard = 0U;

    while ( (fake_khal_worker_pending_for(IR_WORKER) > 0U) && (16U > guard) )
    {
        run_job();
        ++guard;
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(IR_WORKER),
                                   "the worker was left with a job");
}

/**
 * @brief Bring AO_Ir up, once for the whole binary.
 *
 * Once, because QActive_register_() asserts on a taken priority. Nothing
 * carries across between cases except the suppression window, and every case
 * that opens one closes it again before it ends.
 */
static void ir_up_once(void)
{
    Mockkdrv_ir_Init();
    qf_support_init();
    /* The block is behind the C5 link; up here, unless a case takes it down. */
    fake_radio_set_link(true);
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(IR_RX_SIG);

    /* The worker posts its done event with a margin: it needs the queue QP lays out in osObject. */
    qf_support_margin_queue(AO_Ir);

    if ( qf_support_claim_ao(AO_Ir) )
    {
        /* kdrv_ir_init() arms nothing: the receiver is armed by the first
         * job AO_Ir hands its worker, so a link that is already up does not
         * make the initial transition wait on the C5. */
        kdrv_ir_init_StubWithCallback(&capture_init);
        Ir_ctor();
        qf_support_give_queue(AO_Ir);
        AO_Ir->prio  = KODE_PRIO_IR;
        AO_Ir->pthre = KODE_PRIO_IR;
        QActive_register_(AO_Ir);
        fake_psram_reset();
        QASM_INIT(AO_Ir, (void *)0, 0U);
        s_start_buffers = fake_psram_alloc_count();
        s_start_jobs  = fake_khal_worker_pending_for(IR_WORKER);
        s_start_psram = fake_khal_worker_last_psram_for(IR_WORKER);
        kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
        run_job();
    }
    qf_support_give_queue(AO_Ir);
    /* AO_Ir is constructed once for the whole binary, so its mailbox is not
     * emptied by anything else: a case that ended with its echo window still
     * armed leaves that expiry for the next case's first tick to deliver, and
     * the cases below count dispatches. Emptied here, so every one of them
     * counts only its own. */
    {
        const QEvt *stray;

        while ( (NULL != AO_Ir->eQueue)
               && (xQueueReceive(AO_Ir->eQueue, &stray, 0U) == pdTRUE) )
        {
            QF_gc(stray);
        }
    }
    fake_klog_reset();
    ao_spy_drain();
}

/**
 * @brief Post a capture as the interrupt does, and leave it in the queue.
 *
 * Separate from deliver() because event ORDER is the subject of one of the
 * cases below: an event that has been posted and not yet dispatched is the
 * only way to put something ahead of a timer's expiry.
 *
 * @param sym The symbols the driver should hand over.
 * @param n   How many.
 */
static void post_capture(const kdrv_ir_symbol_t *sym, size_t n)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s_rx_cb, "the driver was never given a callback");
    kdrv_ir_rx_take_ExpectAnyArgsAndReturn(n);
    kdrv_ir_rx_take_ReturnMemThruPtr_out((void *)sym,
                                         (int)(n * sizeof(kdrv_ir_symbol_t)));
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    s_rx_cb(s_rx_ctx);
}

/**
 * @brief Expect the arm every capture request now makes on its way in.
 *
 * The receiver is the C5's, and that link is normally still down when the
 * driver comes up - so nothing armed it then. AO_Ir arms it when somebody
 * first wants to hear something instead, and the driver's refusal when it is
 * already listening says nothing, which is the ordinary case.
 */
static void expect_capture_arm(void)
{
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
}

/** @brief Hand the AO a capture as the interrupt would, then let it run. */
static void deliver(const kdrv_ir_symbol_t *sym, size_t n)
{
    post_capture(sym, n);
    (void)qf_support_pump(AO_Ir, 4U);
    run_jobs();
}

/** @brief Ask for FRAME_ADDR/FRAME_CMD and let the AO queue it. */
static void transmit(void)
{
    kdrv_ir_send_ExpectAnyArgsAndReturn(KODE_OK);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_pump(AO_Ir, 4U),
                                   "the request reaches the AO");
    run_job();
}

/**
 * @brief Let the suppression window expire, and prove it lasted its length.
 *
 * Only sound with no pump in between: qf_support_pump() turns the clock a
 * tick of its own before it dispatches anything, so a case that has heard
 * something since transmitting has already spent part of the window. Those
 * use echo_window_drains() instead.
 */
static void echo_window_closes(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, ECHO_TICKS - 1U),
                                   "the window is still open a tick short of it");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_tick(AO_Ir, 1U),
                                   "and closes on the tick it was armed for");
}

/** @brief Run the window out, so the next case starts with nothing armed. */
static void echo_window_drains(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_tick(AO_Ir, ECHO_TICKS),
                                   "the window closes, exactly once");
}

/** @brief One NEC frame's symbols, as the encoder makes them. */
static void a_frame(kdrv_ir_symbol_t *sym)
{
    size_t n = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          knec_encode(FRAME_ADDR, FRAME_CMD, sym, KNEC_SYMBOLS, &n));
    TEST_ASSERT_EQUAL_UINT(KNEC_SYMBOLS, n);
}

/* ---- Ownership ----------------------------------------------------------- */

TEST_CASE("AO_Ir owns the infrared block", "[ir_ao]")
{
    ir_up_once();
    TEST_ASSERT_EQUAL_STRING("AO_Ir", kbsp_owner_name(KBSP_RES_IR));
    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The initial transition runs on the thread that starts the AO, so it may not
 * wait on the C5: the arm that used to sit inside kdrv_ir_init() is the first
 * job the worker is handed instead, and a peer send touches no flash, so the
 * worker's stack is in PSRAM. THE CASE ABOVE BRINGS THE AO UP, which is the
 * only moment these two are observable. */
TEST_CASE("AO_Ir hands its worker the arm as its first job, on a PSRAM stack",
          "[ir_ao]")
{
    ir_up_once();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_start_jobs,
                                   "the receiver is armed by a job, not by the initial transition");
    TEST_ASSERT_TRUE_MESSAGE(s_start_psram, "the worker's stack is in PSRAM");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_start_buffers,
                                   "and so are the capture buffer and the raw frame's copy");
    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- The last frame heard -------------------------------------------------
 * ir_last_request() is how a caller with no queue of its own - the facade,
 * on behalf of an application - reads what ir_info_request() tells the
 * console. The answer goes into the caller's memory and the caller is woken
 * on a semaphore, the ir_capture_request() shape; unlike a capture it
 * carries no deadline, because it waits for nothing and is answered inside
 * the step that receives it.
 *
 * THE CASE BELOW MUST STAY AHEAD OF EVERY DELIVERY IN THIS FILE. AO_Ir's
 * record of the last frame is set for the life of the binary the first time
 * anything is heard, and this is the only place from which "nothing yet" is
 * still reachable.
 */

TEST_CASE("a last-frame request before anything is heard writes nothing",
          "[ir_ao]")
{
    kdrv_ir_frame_t frame = { 0xAAAAU, 0x5555U, 7U, true };
    bool            have  = true;
    uint32_t        ack   = 0U;
    khal_sem_t      sem;

    ir_up_once();
    sem = khal_sem_create();
    fake_sem_reset();

    ir_last_request(&frame, &have, sem, 7U, &ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));

    /* Even an answer of "nothing" is stamped: a caller that could not tell
     * this from an answer it never collected would have to guess. */
    TEST_ASSERT_EQUAL_UINT32(7U, ack);

    TEST_ASSERT_FALSE(have);
    /* Left exactly as the caller had it: an answer of "nothing" must not
     * look like a frame of zeroes. */
    TEST_ASSERT_EQUAL_HEX16(0xAAAAU, frame.addr);
    TEST_ASSERT_EQUAL_HEX16(0x5555U, frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(7U, frame.symbols);
    /* And the caller is woken all the same - once. */
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- What it publishes --------------------------------------------------- */

TEST_CASE("a frame the receiver heard is published", "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    ir_up_once();
    a_frame(sym);

    deliver(sym, KNEC_SYMBOLS);

    const QEvt *e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(IR_RX_SIG, e->sig);
    const IrRxEvt *rx = (const IrRxEvt *)e;
    TEST_ASSERT_TRUE(rx->nec);
    TEST_ASSERT_EQUAL_HEX16(FRAME_ADDR, rx->addr);
    TEST_ASSERT_EQUAL_HEX16(FRAME_CMD, rx->cmd);
    TEST_ASSERT_EQUAL_UINT16(KNEC_SYMBOLS, rx->symbols);
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A remote this board has no codec for is still news: the symbols are what a
 * recording is made of, and refusing to mention a frame because it did not
 * decode would make `ir_rx` blind to everything but NEC. */
TEST_CASE("a frame that is not NEC is still published, undecoded", "[ir_ao]")
{
    kdrv_ir_symbol_t sym[8];

    ir_up_once();
    for ( size_t i = 0U; 8U > i; ++i )
    {
        sym[i].mark_us  = 900U;
        sym[i].space_us = 900U;
    }

    deliver(sym, 8U);

    const IrRxEvt *rx = (const IrRxEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_EQUAL_INT(IR_RX_SIG, rx->super.sig);
    TEST_ASSERT_FALSE(rx->nec);
    TEST_ASSERT_EQUAL_UINT16(8U, rx->symbols);
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A readiness event can outlive its own capture: kdrv_ir_rx_present() - which
 * the production row calls - disables the receive channel while it drives the
 * pin and arms it again afterwards, losing whatever was in flight. The
 * interrupt has already posted by then. So the AO reads nothing, the receiver
 * is ALREADY listening, and arming it a second time is refused - which the
 * AO used to report as "nothing more will be heard" on a board with nothing
 * wrong with it. */
TEST_CASE("a readiness event with no capture behind it does not arm again",
          "[ir_ao]")
{
    ir_up_once();

    /* No kdrv_ir_rx_arm_ExpectAndReturn() on purpose: an unexpected call to
     * it is exactly what this case is looking for. */
    kdrv_ir_rx_take_ExpectAnyArgsAndReturn(0U);
    TEST_ASSERT_NOT_NULL(s_rx_cb);
    s_rx_cb(s_rx_ctx);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_pump(AO_Ir, 4U),
                                   "the readiness event still reaches the AO");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "and nothing is published for a frame that never was");

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- What it refuses to publish ------------------------------------------ */

/* The case this file exists for. An application driving a remote must not
 * see its own keypresses arrive as input a few milliseconds after it sent
 * them - and the board can hear itself, so without the window it would. */
TEST_CASE("our own echo is not published", "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    ir_up_once();
    a_frame(sym);

    transmit();
    deliver(sym, KNEC_SYMBOLS);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the board heard itself and said nothing");
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A transmission the driver refused produces no photons, so there is no echo
 * to suppress - and a window opened for one would make the AO deaf to a real
 * remote for its length, which is a bug that only shows up on a board that is
 * already having a bad day. The AO opens the window only when kdrv_ir_send()
 * said yes, and that is a claim, so it is asserted. */
TEST_CASE("a transmission that was refused opens no window", "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    ir_up_once();
    a_frame(sym);

    kdrv_ir_send_ExpectAnyArgsAndReturn(KODE_ERROR);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_pump(AO_Ir, 4U),
                                   "the request reaches the AO");
    run_job();

    deliver(sym, KNEC_SYMBOLS);

    const IrRxEvt *rx = (const IrRxEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rx, "nothing went out, so this one is somebody else's");
    TEST_ASSERT_EQUAL_HEX16(FRAME_ADDR, rx->addr);
    TEST_ASSERT_EQUAL_HEX16(FRAME_CMD, rx->cmd);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, ECHO_TICKS),
                                   "and there was no window armed to close");

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* QTimeEvt_disarm() answers "was not running" both for a timer that never ran
 * and for one that fired a moment ago and left its event in the queue. Only
 * the AO knows which, and the difference is a whole window: a send request
 * dispatched ahead of that queued event re-arms and goes deaf, and then the
 * expired event of the window it replaced arrives and clears the flag. The
 * board would publish its own echo as somebody's remote. */
TEST_CASE("a window that just opened is not closed by the one it replaced",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    ir_up_once();
    a_frame(sym);

    transmit();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, ECHO_TICKS - 1U),
                                   "a tick short of the first window closing");

    /* The second frame's answer is posted and deliberately NOT dispatched,
     * so it sits in the queue ahead of the IR_ECHO_SIG the next tick is about
     * to post. That ordering is the whole defect, and it is an ordinary one:
     * two frames back to back is what holding a button on a remote looks
     * like. The window opens when the call returns, so it is the answer, not
     * the request, that has to be ahead of the expiry. */
    kdrv_ir_send_ExpectAnyArgsAndReturn(KODE_OK);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_run_next_for(IR_WORKER),
                             "the second frame is with the worker");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, qf_support_tick(AO_Ir, 1U),
                                   "the second frame's answer, then the first window's expiry");

    deliver(sym, KNEC_SYMBOLS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "the second transmission's echo is still ours");

    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The other half of the same claim: the window is a window and not a mute
 * button, so the remote in somebody's hand is heard the moment it closes. */
TEST_CASE("a frame after the echo window is published again", "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    ir_up_once();
    a_frame(sym);

    transmit();
    echo_window_closes();
    ao_spy_drain();

    deliver(sym, KNEC_SYMBOLS);

    const IrRxEvt *rx = (const IrRxEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(rx, "the window closed, so this one is somebody else's");
    TEST_ASSERT_EQUAL_HEX16(FRAME_ADDR, rx->addr);
    TEST_ASSERT_EQUAL_HEX16(FRAME_CMD, rx->cmd);
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- The capture request ------------------------------------------------- */

TEST_CASE("a capture request is answered with the symbols of the next frame",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got = 0U;

    ir_up_once();
    a_frame(sym);
    (void)memset(out, 0, sizeof(out));
    fake_sem_reset();

    expect_capture_arm();
    ir_capture_request(out, KNEC_SYMBOLS, &got, khal_sem_create(), HOLD_MS,
                       0U, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
                                   "nothing has been heard yet");

    deliver(sym, KNEC_SYMBOLS);

    TEST_ASSERT_EQUAL_UINT(KNEC_SYMBOLS, got);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "the requester is woken exactly once");
    TEST_ASSERT_EQUAL_MEMORY(sym, out, sizeof(sym));
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A requester asleep on a semaphore nobody will ever signal is the one
 * failure this AO must not be able to cause, so the request it displaces is
 * answered empty rather than forgotten. */
TEST_CASE("a second capture request answers the first rather than leaving it asleep",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    IrSymbol         first[KNEC_SYMBOLS];
    IrSymbol         second[KNEC_SYMBOLS];
    size_t           got_first  = 99U;
    size_t           got_second = 0U;
    uint32_t         ack        = 0U;

    ir_up_once();
    a_frame(sym);

    expect_capture_arm();
    ir_capture_request(first, KNEC_SYMBOLS, &got_first, khal_sem_create(),
                       HOLD_MS, 11U, &ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();

    fake_sem_reset();
    expect_capture_arm();
    ir_capture_request(second, KNEC_SYMBOLS, &got_second, khal_sem_create(),
                       HOLD_MS, 12U, &ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "the displaced requester is woken");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, got_first, "and told it heard nothing");
    /* Stamped with the DISPLACED request's generation, not the one that
     * displaced it. Both requesters share this word when they share a
     * semaphore, and this is what lets the second one see that the token it
     * may wake on belongs to the first. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(11U, ack,
                                     "the answer names the request it ends");

    deliver(sym, KNEC_SYMBOLS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KNEC_SYMBOLS, got_second,
                                   "the frame belongs to the request that displaced it");
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The case the deadline exists for. @c out, @c n and @c done belong to a
 * blocked caller, and a caller blocks for as long as it said it would - so an
 * answer written after that is a memcpy into a stack frame that has returned.
 * A frame may never arrive at all: nobody presses a button. So the deadline,
 * not the frame, is what guarantees an answer, and it has to land while the
 * caller is still asleep. */
TEST_CASE("a capture request nobody answers is closed by its own deadline",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got = 99U;

    ir_up_once();
    a_frame(sym);
    (void)memset(out, 0, sizeof(out));
    fake_sem_reset();

    expect_capture_arm();
    ir_capture_request(out, KNEC_SYMBOLS, &got, khal_sem_create(), HOLD_MS,
                       0U, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();

    /* HOLD_TICKS - 1, because the pump above turned the clock a tick of its
     * own before the AO armed anything. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, HOLD_TICKS - 1U),
                                   "the requester gets the whole wait it asked for");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
                                   "and is not woken early");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_tick(AO_Ir, 1U),
                                   "the deadline is the AO's own event");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "woken while it is still waiting, which is the point");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, got, "with nothing, because nothing came");

    /* And the frame that finally arrives belongs to whoever asks next. On the
     * board `out` is gone by now, so writing into it would be a write through
     * a dead pointer - the hazard this whole deadline removes. */
    deliver(sym, KNEC_SYMBOLS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "the answer was written exactly once");
    TEST_ASSERT_EQUAL_UINT(0U, got);
    for ( size_t i = 0U; KNEC_SYMBOLS > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, out[i].mark_us,
                                         "the buffer is untouched after the deadline");
    }
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* An application driving a remote must not be handed its own keypress, and
 * that holds for the request as much as for the publication: both are
 * "somebody else's frame arriving as input", and the board's own transmission
 * is neither. */
TEST_CASE("the board's own transmission is not handed to a pending capture request",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got = 99U;

    ir_up_once();
    a_frame(sym);
    (void)memset(out, 0, sizeof(out));
    fake_sem_reset();

    expect_capture_arm();
    ir_capture_request(out, KNEC_SYMBOLS, &got, khal_sem_create(), HOLD_MS,
                       0U, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();

    transmit();
    deliver(sym, KNEC_SYMBOLS);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
                                   "still waiting for a frame from outside");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(99U, got, "and nothing was written");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "nor published, for the same reason");

    /* Which leaves the deadline as the only thing that can end this wait -
     * the request outlives the window, and is answered empty. */
    (void)qf_support_tick(AO_Ir, ECHO_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "the deadline ends it, since the echo could not");
    TEST_ASSERT_EQUAL_UINT(0U, got);
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- The production row -------------------------------------------------- *
 * `test component=ir`. Two steps and two separable verdicts: step 1 judges
 * the receiver on its own and emits nothing, step 2 judges the pair by
 * transmitting a nonce and requiring it back exactly. What the cases below
 * are really about is that those two stay separable - a row that transmits
 * before it has judged the receiver reports one bit, and one bit does not
 * tell a production operator which half to replace.
 */

/** @brief The address the row transmits under; ir_ao.c's IR_TEST_ADDR. */
#define ROW_ADDR  0xA55AU

/** @brief KODE_IR_TEST_ECHO_MS as whole QP ticks, one attempt's deadline. */
#define WAIT_TICKS  QF_SUPPORT_STEPS(KODE_IR_TEST_ECHO_MS)

/**
 * @brief The most ticks a row frame's suppression window can last.
 *
 * ECHO_TICKS above is one frame's exact length because the cases above send
 * one known frame. The row's command is a nonce, so its frames vary: a NEC
 * frame of all ones is 86 ms against the 52 ms of the one this file otherwise
 * uses. This is that worst case plus KODE_IR_ECHO_TAIL_MS - 86 + 120 ms, 21
 * ticks - and it is what a row case has to run out before the next one
 * starts: a window still armed here is an expiry the next case's first pump
 * delivers and counts as its own. Spelled out, for ECHO_TICKS' reason.
 */
#define ROW_ECHO_TICKS  21U

/** @brief What kdrv_ir_rx_present() reports; static, because CMock copies
 *         from this pointer when the call happens rather than when it is
 *         armed, so a local would have gone out of scope by then. */
static bool s_present;

/** @brief The frame the AO last handed the driver, and how many it has. */
static kdrv_ir_symbol_t s_sent[KNEC_SYMBOLS];
static size_t           s_sent_n;
static unsigned         s_sends;

/** @brief Record what was transmitted instead of merely allowing it. */
static kode_status_t capture_send(const kdrv_ir_symbol_t *sym, size_t n,
                                  uint32_t carrier_hz, int ncalls)
{
    (void)ncalls;
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KDRV_IR_CARRIER_HZ, carrier_hz,
                                     "the only carrier our own receiver hears");
    TEST_ASSERT_EQUAL_UINT(KNEC_SYMBOLS, n);
    (void)memcpy(s_sent, sym, n * sizeof(kdrv_ir_symbol_t));
    s_sent_n = n;
    ++s_sends;
    return KODE_OK;
}

/**
 * @brief Bring the AO up with the row's seams armed and its counters clear.
 *
 * @param present What the probe should report the receiver as.
 */
static void row_up(bool present)
{
    ir_up_once();
    ao_spy_subscribe(TEST_RESULT_SIG);

    s_sends   = 0U;
    s_sent_n  = 0U;
    s_present = present;

    /* A stub rather than an expectation: every case here asserts on how many
     * times the AO transmitted and on what, and a case that must NOT transmit
     * needs the call to be observable rather than merely unexpected. */
    kdrv_ir_send_StubWithCallback(&capture_send);
    kdrv_ir_rx_present_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_ir_rx_present_ReturnThruPtr_present(&s_present);
}

/** @brief Ask for the row and let it run: the probe, then the first frame. */
static void row_request(void)
{
    ir_test_request();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_pump(AO_Ir, 4U),
                                   "the request reaches the AO");
    run_jobs();
}

/** @brief The verdict the AO published, or NULL if it has not judged yet. */
static const TestResultEvt *row_verdict(void)
{
    const QEvt *e = ao_spy_take();

    while ( NULL != e )
    {
        if ( TEST_RESULT_SIG == e->sig )
        {
            return (const TestResultEvt *)e;
        }
        e = ao_spy_take();
    }
    return NULL;
}

/** @brief The command in the frame the AO last transmitted - its nonce. */
static uint16_t sent_nonce(void)
{
    uint16_t addr = 0U;
    uint16_t cmd  = 0U;

    TEST_ASSERT_TRUE_MESSAGE(knec_decode(s_sent, s_sent_n, &addr, &cmd),
                             "the row transmits a NEC frame");
    TEST_ASSERT_EQUAL_HEX16_MESSAGE(ROW_ADDR, addr,
                                    "under the row's own address");
    return cmd;
}

/**
 * @brief Hand the AO a frame that is not the nonce it is waiting for.
 *
 * Somebody else's remote, as far as the row can tell - which is the whole
 * point of the nonce: what comes back has to be the exact value this board
 * drew, not merely a NEC frame.
 *
 * @param nonce The value the AO is waiting for, so this can differ from it.
 */
static void deliver_wrong(uint16_t nonce)
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    size_t           n = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          knec_encode(ROW_ADDR, (uint16_t)(nonce ^ 0xFFFFU),
                                      sym, KNEC_SYMBOLS, &n));
    deliver(sym, n);
}

/**
 * @brief Arm the probe for one more row, the way row_up() does for the first.
 *
 * A case that runs two rows needs two answers from kdrv_ir_rx_present(), and
 * CMock consumes an expectation per call.
 */
static void row_probe_again(void)
{
    kdrv_ir_rx_present_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_ir_rx_present_ReturnThruPtr_present(&s_present);
}

/** @brief Run out whatever the row left armed, so the next case starts clean. */
static void row_settles(void)
{
    (void)qf_support_tick(AO_Ir, ROW_ECHO_TICKS);
    ao_spy_drain();
}

/* Step 1 exists to be reported on its own. A row that transmitted anyway
 * would still fail, and would still be right - and would have thrown away
 * the one thing the operator needed, which is WHICH half to replace. */
TEST_CASE("the row fails at the receiver, and transmits nothing", "[ir_ao]")
{
    const TestResultEvt *r;

    row_up(false);
    row_request();

    r = row_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "an absent receiver is judged at once");
    TEST_ASSERT_EQUAL_STRING("ir", r->name);
    TEST_ASSERT_FALSE(r->ok);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_sends,
                                   "and judged without emitting a photon");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "one verdict per run, and nothing else on the bus");
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The board hearing itself, which is the whole reason this row can exist. */
TEST_CASE("the row passes on the first exact echo, and transmits once", "[ir_ao]")
{
    const TestResultEvt *r;

    row_up(true);
    row_request();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends,
                                   "a present receiver is followed by one frame");
    TEST_ASSERT_NULL_MESSAGE(row_verdict(), "nothing has come back yet");

    deliver(s_sent, s_sent_n);

    r = row_verdict();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("ir", r->name);
    TEST_ASSERT_TRUE(r->ok);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends,
                                   "a match ends the row; nothing is sent twice");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "one verdict per run, and nothing else on the bus");
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The reason the row retries at all is to survive one collision with ambient
 * infrared - so a frame that is not the nonce must not be the thing that
 * spends an attempt, or the protection becomes the failure it was meant to
 * prevent. Worse than one lost attempt: a retry transmits at once while the
 * previous frame is still on its way home, so counting the collision would
 * cascade through all three attempts and fail a working board.
 *
 * Ambient first, our own echo a few milliseconds later. That is the sequence
 * a production cell with a fluorescent lamp in it actually produces, and the
 * row has to pass on it. */
TEST_CASE("an ambient frame does not spend the attempt, and the echo still passes",
          "[ir_ao]")
{
    const TestResultEvt *r;
    uint16_t             nonce;

    row_up(true);
    row_request();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    nonce = sent_nonce();

    deliver_wrong(nonce);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends,
                                   "somebody else's frame costs no attempt");
    TEST_ASSERT_NULL_MESSAGE(row_verdict(), "and judges nothing");

    deliver(s_sent, s_sent_n);

    r = row_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "the echo arrives inside the same attempt");
    TEST_ASSERT_EQUAL_STRING("ir", r->name);
    TEST_ASSERT_TRUE_MESSAGE(r->ok, "one collision does not condemn a good board");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends,
                                   "and the row never had to transmit twice");
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The same claim for the frame the board is likeliest of all to hear: its
 * own. An attempt's echo is roughly 85 ms behind its transmission, so once an
 * attempt has ended the frame it sent can still arrive during the next one -
 * carrying the row's own address and a nonce the row itself drew. That is not
 * a fault of the emitter, and the log has to say which of the two it was. */
TEST_CASE("our own echo from a spent attempt does not count against the next",
          "[ir_ao]")
{
    kdrv_ir_symbol_t     stale[KNEC_SYMBOLS];
    size_t               stale_n = 0U;
    const TestResultEvt *r;
    uint16_t             first;

    row_up(true);
    row_request();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    first = sent_nonce();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          knec_encode(ROW_ADDR, first, stale, KNEC_SYMBOLS,
                                      &stale_n));

    /* Attempt 1 hears nothing in time, so attempt 2 goes out. */
    (void)qf_support_tick(AO_Ir, WAIT_TICKS);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_sends, "the deadline spends the attempt");
    TEST_ASSERT_TRUE_MESSAGE(sent_nonce() != first, "with a fresh nonce");

    deliver(stale, stale_n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_sends,
                                   "attempt 1's frame is ours, so attempt 2 keeps waiting");
    TEST_ASSERT_NULL(row_verdict());

    deliver(s_sent, s_sent_n);

    r = row_verdict();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("ir", r->name);
    TEST_ASSERT_TRUE_MESSAGE(r->ok, "attempt 2's own echo is the pass");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_sends, "two attempts, not three");
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The generation gate, for the third of this AO's one-shots. A row that ends
 * on an echo leaves its attempt deadline already expired and posted, and the
 * request that starts the NEXT row can sit ahead of that event in the queue -
 * a second `test component=ir` typed while the first was still waiting. The
 * stale deadline then arrives with a row running and an attempt that has not
 * had a millisecond of its window, and ends it. Two rows, because that is the
 * ordering that reaches it: a post always lands behind what is already
 * queued. */
TEST_CASE("a deadline the previous row left behind does not cut the next row short",
          "[ir_ao]")
{
    kdrv_ir_symbol_t     echo[KNEC_SYMBOLS];
    size_t               echo_n;
    const TestResultEvt *r;

    row_up(true);
    row_request();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    echo_n = s_sent_n;
    (void)memcpy(echo, s_sent, sizeof(echo));

    /* A tick short of the first attempt's deadline. */
    (void)qf_support_tick(AO_Ir, WAIT_TICKS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends, "the row is still waiting");

    /* Both posted and deliberately NOT dispatched, so they sit ahead of the
     * IR_TEST_TO_SIG the next tick is about to post. */
    post_capture(echo, echo_n);
    row_probe_again();
    ir_test_request();

    /* One tick: the expiry lands behind them and is dispatched last, which is
     * the whole defect - the echo ends the first row, the request starts the
     * second, and then the first row's deadline arrives on top of it. */
    (void)qf_support_tick(AO_Ir, 1U);
    /* The echo that ended the first row left the receiver to be armed, and the
     * second row's request waited for that call to return; both run now. */
    run_jobs();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_sends,
                                   "the second row transmitted once, and the stale deadline did not retry it");
    r = row_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "the first row passed on its echo");
    TEST_ASSERT_TRUE(r->ok);

    /* And the second row's attempt still owns its whole window - which also
     * proves the stale event was consumed rather than left queued. */
    (void)qf_support_tick(AO_Ir, WAIT_TICKS - 2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_sends, "so it never had to go again");

    deliver(s_sent, s_sent_n);
    r = row_verdict();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE_MESSAGE(r->ok, "and it passes on its own echo like any other");
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The row owns the receiver while it runs, and ir_capture_request() says so:
 * a request that overlaps a row hears nothing, because a row frame is this
 * board's own transmission and that is the one thing a capture request never
 * delivers. The requester is not lost, though - its own deadline still lands
 * while it is asleep, which is the whole contract. */
TEST_CASE("a capture request across the row is answered empty, never with a row frame",
          "[ir_ao]")
{
    IrSymbol             out[KNEC_SYMBOLS];
    size_t               got = 99U;
    const TestResultEvt *r;

    row_up(true);
    (void)memset(out, 0, sizeof(out));
    fake_sem_reset();

    expect_capture_arm();
    ir_capture_request(out, KNEC_SYMBOLS, &got, khal_sem_create(), HOLD_MS,
                       0U, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();

    row_request();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);

    deliver(s_sent, s_sent_n);
    r = row_verdict();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE_MESSAGE(r->ok, "the row heard itself");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
                                   "the row consumed that frame, so the requester is still asleep");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(99U, got, "and nothing was written for it");

    (void)qf_support_tick(AO_Ir, HOLD_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "its own deadline ends the wait, exactly once");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, got, "with nothing, as the contract says");
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* Silence is the ordinary failure - a dead emitter produces no frame at all -
 * so the row cannot depend on anything arriving to move it along. Each
 * attempt has its own deadline, and the last one that expires is the
 * verdict. The deadline is also the ONLY thing that spends an attempt, which
 * is why the cap is asserted here rather than off a mismatch.
 *
 * And each attempt draws a fresh nonce: whoever produced a frame that did not
 * match would have to guess a new value every time, which is what makes three
 * attempts no easier to fake than one. */
TEST_CASE("an echo that never arrives fails on the deadline, one attempt at a time",
          "[ir_ao]")
{
    const TestResultEvt *r;
    uint16_t             first;
    uint16_t             second;
    uint16_t             third;

    row_up(true);
    row_request();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    first = sent_nonce();

    (void)qf_support_tick(AO_Ir, WAIT_TICKS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends,
                                   "the attempt gets its full window");
    (void)qf_support_tick(AO_Ir, 1U);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_sends,
                                   "and the deadline starts the next one");
    second = sent_nonce();
    TEST_ASSERT_TRUE_MESSAGE(first != second, "under a fresh nonce");
    TEST_ASSERT_NULL(row_verdict());

    (void)qf_support_tick(AO_Ir, WAIT_TICKS);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT(3U, s_sends);
    third = sent_nonce();
    TEST_ASSERT_TRUE_MESSAGE(second != third, "and a fresh one again");
    TEST_ASSERT_NULL(row_verdict());

    (void)qf_support_tick(AO_Ir, WAIT_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KODE_IR_TEST_TRIES, s_sends,
                                   "the cap holds against silence too");
    r = row_verdict();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("ir", r->name);
    TEST_ASSERT_FALSE(r->ok);
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- The last frame heard, once there is one ------------------------------
 * The counterpart of the case near the top of this file: what the answer is
 * once the receiver has actually produced something. The second case is the
 * one that matters, because it pins WHICH frame this reports - the receiver's
 * own record rather than the subset that reached the bus.
 */

TEST_CASE("a last-frame request answers with what the receiver last produced",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    kdrv_ir_frame_t  frame = { 0U, 0U, 0U, false };
    bool             have  = false;
    uint32_t         ack   = 0U;
    khal_sem_t       sem;

    ir_up_once();
    sem = khal_sem_create();
    a_frame(sym);
    deliver(sym, KNEC_SYMBOLS);
    ao_spy_drain();
    fake_sem_reset();

    ir_last_request(&frame, &have, sem, 3U, &ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));

    TEST_ASSERT_EQUAL_UINT32(3U, ack);
    TEST_ASSERT_TRUE(have);
    TEST_ASSERT_TRUE(frame.nec);
    TEST_ASSERT_EQUAL_HEX16(FRAME_ADDR, frame.addr);
    TEST_ASSERT_EQUAL_HEX16(FRAME_CMD, frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(KNEC_SYMBOLS, frame.symbols);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A frame suppressed as this board's own echo never reaches the bus, so a
 * subscriber to IR_RX_SIG cannot know it happened. AO_Ir does, and this is
 * the request that says so: what it reports is the receiver's own record,
 * not the filtered stream. Getting this wrong would make the answer mean
 * "the last frame somebody else was told about", which is a different
 * question and one nothing asked. */
TEST_CASE("a suppressed echo is still the last frame the receiver produced",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    kdrv_ir_frame_t  frame = { 0U, 0U, 0U, false };
    bool             have  = false;
    khal_sem_t       sem;

    ir_up_once();
    sem = khal_sem_create();
    a_frame(sym);

    transmit();
    deliver(sym, KNEC_SYMBOLS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "nothing left AO_Ir at all");

    fake_sem_reset();
    ir_last_request(&frame, &have, sem, 0U, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));

    TEST_ASSERT_TRUE_MESSAGE(have, "and it was still heard");
    TEST_ASSERT_EQUAL_HEX16(FRAME_ADDR, frame.addr);
    TEST_ASSERT_EQUAL_HEX16(FRAME_CMD, frame.cmd);

    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- The worker ------------------------------------------------------------
 * Every call to the C5 - a frame, a line reading, an arm - runs on AO_Ir's
 * worker, so the AO's step never waits on the link. What these cases pin is
 * what that changes for whoever talks to the AO meanwhile: the answers are the
 * same, and a request that needs the C5 while a call is out is held and served
 * when it returns, in the order it came.
 */

/** @brief What the line reading stub reports, and how often it was called. */
static bool          s_idle_high;
static kode_status_t s_idle_rc;
static unsigned      s_idle_calls;

static kode_status_t idle_stub(bool *high, int ncalls)
{
    (void)ncalls;
    ++s_idle_calls;
    if ( KODE_OK == s_idle_rc )
    {
        *high = s_idle_high;
    }
    return s_idle_rc;
}

/** @brief Arm the stubs the worker cases share, with the counters cleared. */
static void worker_up(void)
{
    ir_up_once();
    s_idle_high  = true;
    s_idle_rc    = KODE_OK;
    s_idle_calls = 0U;
    s_sends      = 0U;
    s_sent_n     = 0U;
    kdrv_ir_rx_idle_StubWithCallback(&idle_stub);
    kdrv_ir_send_StubWithCallback(&capture_send);
}

/** @brief Take the status answer the AO posted to the spy. */
static const IrInfoEvt *info_answer(void)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the status request was answered");
    TEST_ASSERT_EQUAL_INT(IR_INFO_RSP_SIG, (int)e->sig);
    return (const IrInfoEvt *)e;
}

TEST_CASE("a frame is sent by the worker, and what is heard meanwhile is read after it",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    worker_up();
    a_frame(sym);

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_pump(AO_Ir, 4U),
                                   "the request reaches the AO");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_sends,
                                   "and the step is over before the call is made");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(IR_WORKER),
                                   "the call is the worker's");

    /* The echo can beat the call's answer to the AO. It is held, not read: no
     * take is expected until the window it belongs to is open. */
    TEST_ASSERT_NOT_NULL(s_rx_cb);
    s_rx_cb(s_rx_ctx);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_pump(AO_Ir, 4U),
                                   "the readiness event reaches the AO");
    TEST_ASSERT_EQUAL_UINT(0U, s_sends);

    kdrv_ir_rx_take_ExpectAnyArgsAndReturn(KNEC_SYMBOLS);
    kdrv_ir_rx_take_ReturnMemThruPtr_out((void *)sym, (int)sizeof(sym));
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    run_job();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends, "the worker made the call");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "and the frame heard meanwhile was our own echo");
    run_jobs();
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("the status answer waits for the line reading and says what it read",
          "[ir_ao]")
{
    const IrInfoEvt *rsp;

    worker_up();

    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_idle_calls,
                                   "the step does not wait on the C5");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "so nothing is answered yet");

    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, s_idle_calls);
    rsp = info_answer();
    TEST_ASSERT_TRUE(rsp->up);
    TEST_ASSERT_TRUE_MESSAGE(rsp->rx_high, "the level the C5 read");
    ao_spy_drain();

    /* A reading the C5 would not give is the same answer it always was: the
     * line is reported low. */
    s_idle_rc = KODE_TIMEOUT;
    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    rsp = info_answer();
    TEST_ASSERT_TRUE(rsp->up);
    TEST_ASSERT_FALSE(rsp->rx_high);
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a request that arrives while a call is out is held, then served in order",
          "[ir_ao]")
{
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got   = 99U;
    kdrv_ir_frame_t  frame = { 0U, 0U, 0U, false };
    bool             have  = false;
    uint32_t         ack   = 0U;
    khal_sem_t       sem;

    worker_up();
    sem = khal_sem_create();
    fake_sem_reset();

    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    ir_capture_request(out, KNEC_SYMBOLS, &got, sem, HOLD_MS, 0U, NULL);
    ir_last_request(&frame, &have, sem, 5U, &ack);
    (void)qf_support_settle(AO_Ir);

    /* Nothing that needs the C5 ran. The last-frame request does not, and is
     * answered at once from what the AO holds. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(IR_WORKER),
                                   "still only the line reading");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5U, ack, "the last-frame request is not held");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "and the capture request is not answered");
    TEST_ASSERT_EQUAL_UINT(0U, s_sends);

    /* The reading returns: its requester first, then the frame that asked
     * next. */
    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, s_idle_calls);
    (void)info_answer();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_sends, "the frame is with the worker");
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());

    /* Then the capture request, which arms the receiver. */
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));
    run_job();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
                                   "it waits for a frame, under its own deadline");

    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Ir, HOLD_TICKS));
    TEST_ASSERT_EQUAL_UINT(2U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT(0U, got);
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a request over the hold queue's room is refused as busy", "[ir_ao]")
{
    const IrInfoEvt *rsp;

    worker_up();

    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    for ( unsigned i = 0U; (unsigned)KODE_IR_DEFER_DEPTH > i; ++i )
    {
        ir_send_request(FRAME_ADDR, FRAME_CMD);
    }
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "all of them are held");

    /* One more of each: a status request is answered at once, without the
     * reading, and a frame is not sent. */
    ir_info_request(ao_spy());
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    rsp = info_answer();
    TEST_ASSERT_FALSE_MESSAGE(rsp->rx_high, "answered without asking the C5");
    TEST_ASSERT_EQUAL_UINT(0U, s_idle_calls);
    ao_spy_drain();

    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)KODE_IR_DEFER_DEPTH, s_sends,
                                   "the held ones went out, the refused one did not");
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The sequence number is how the AO tells the answer it waits for from one it
 * does not. A done event nobody is waiting for must neither answer a
 * requester nor free the guard. */
TEST_CASE("a done event that is not the call's is dropped by its sequence",
          "[ir_ao]")
{
    KqpWorkerDoneEvt *foreign;

    worker_up();

    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));

    foreign = Q_NEW(KqpWorkerDoneEvt, IR_JOB_DONE_SIG);
    foreign->seq    = 0xBEEFU;
    foreign->status = (uint8_t)KODE_OK;
    foreign->value  = 0U;
    QACTIVE_POST(AO_Ir, &foreign->super, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Ir));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "someone else's answer is nobody's answer");

    run_job();
    TEST_ASSERT_TRUE(info_answer()->rx_high);
    ao_spy_drain();

    /* With nothing out, a late one is dropped just the same. */
    foreign = Q_NEW(KqpWorkerDoneEvt, IR_JOB_DONE_SIG);
    foreign->seq    = 0xBEEFU;
    foreign->status = (uint8_t)KODE_OK;
    foreign->value  = 0U;
    QACTIVE_POST(AO_Ir, &foreign->super, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Ir));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The C5 bounds its own answers, but a link that hangs must not leave a
 * requester waiting on a reply nobody is making. */
TEST_CASE("a call that does not return in time fails its requester, and the guard holds",
          "[ir_ao]")
{
    const IrInfoEvt *rsp;
    unsigned const   steps = QF_SUPPORT_STEPS(KDRV_IR_CALL_MAX_MS + KODE_IR_JOB_MARGIN_MS);

    worker_up();

    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, steps - 1U),
                                   "the call has its whole time");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and nobody is told early");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, qf_support_tick(AO_Ir, 1U),
                                   "the deadline is the AO's own event");
    rsp = info_answer();
    TEST_ASSERT_FALSE_MESSAGE(rsp->rx_high, "the first requester is told the reading failed");
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_idle_calls, "the call has still not returned");

    /* It returns at last. Its answer was given, so nobody is told twice, and
     * the request held behind it is served now. */
    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, s_idle_calls);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));
    TEST_ASSERT_EQUAL_UINT(0U, s_sends);
    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a worker with no room leaves the AO free: no reading, no frame, no wait",
          "[ir_ao]")
{
    worker_up();

    fake_khal_worker_refuse_for(IR_WORKER, true);
    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    TEST_ASSERT_FALSE_MESSAGE(info_answer()->rx_high,
                              "answered at once, without the reading");
    ao_spy_drain();

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(IR_WORKER));
    TEST_ASSERT_EQUAL_UINT(0U, s_sends);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, ECHO_TICKS),
                                   "and no window was opened for a frame that never left");

    /* Nothing is left waiting on a call that was never made. */
    fake_khal_worker_refuse_for(IR_WORKER, false);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    echo_window_drains();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A row attempt's deadline is the AO's own event and cannot wait, but the
 * frame it retries with can: the attempt is spent at once, and the next one
 * goes out when the call that is out returns. */
TEST_CASE("a row attempt that runs out of time while a call is out retries when it returns",
          "[ir_ao]")
{
    const TestResultEvt *r;

    row_up(true);
    s_idle_high  = true;
    s_idle_rc    = KODE_OK;
    s_idle_calls = 0U;
    kdrv_ir_rx_idle_StubWithCallback(&idle_stub);

    row_request();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);

    ir_info_request(ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, qf_support_tick(AO_Ir, WAIT_TICKS),
                                   "the echo window's end, and the deadline");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends,
                                   "but the next frame waits for the reading");

    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));
    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_sends, "and goes out when it returns");

    deliver(s_sent, s_sent_n);
    (void)ao_spy_take();   /* the status answer */
    r = row_verdict();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE_MESSAGE(r->ok, "the second attempt's echo is the pass");
    row_settles();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* Strict queue order used to guarantee that a frame announced before the end
 * of the echo window was heard while the window was still open. With a call
 * out the readiness is only flagged, so the window's end serves it first. */
TEST_CASE("a frame announced before the echo window's end is heard under that window",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    worker_up();
    a_frame(sym);

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Ir, ECHO_TICKS - 2U));

    /* A second frame is with the worker when the echo of the first arrives. */
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));
    kdrv_ir_rx_take_ExpectAnyArgsAndReturn(KNEC_SYMBOLS);
    kdrv_ir_rx_take_ReturnMemThruPtr_out((void *)sym, (int)sizeof(sym));
    TEST_ASSERT_NOT_NULL(s_rx_cb);
    s_rx_cb(s_rx_ctx);

    /* The readiness, then the first window's end. */
    TEST_ASSERT_EQUAL_UINT(2U, qf_support_tick(AO_Ir, 2U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "it is our own echo, not a remote");

    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT(2U, s_sends);
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The same guarantee for a capture request's deadline: a frame announced
 * before it answers the request, and does not leave it to be answered empty. */
TEST_CASE("a frame announced before a capture's deadline answers it",
          "[ir_ao]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got = 99U;

    worker_up();
    a_frame(sym);
    fake_sem_reset();

    expect_capture_arm();
    ir_capture_request(out, KNEC_SYMBOLS, &got, khal_sem_create(), HOLD_MS,
                       0U, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();

    /* A call is out when the frame arrives (and the deadline follows). */
    ir_info_request(ao_spy());
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));
    kdrv_ir_rx_take_ExpectAnyArgsAndReturn(KNEC_SYMBOLS);
    kdrv_ir_rx_take_ReturnMemThruPtr_out((void *)sym, (int)sizeof(sym));
    s_rx_cb(s_rx_ctx);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, qf_support_tick(AO_Ir, HOLD_TICKS),
                                   "the readiness, then the deadline");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(), "answered once");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KNEC_SYMBOLS, got,
                                   "with the frame, not empty");

    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    run_jobs();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- A frame the requester lends --------------------------------------------
 * kode_block_ir_send_raw()'s path: the symbols are lent by pointer and copied
 * as the call starts, sent on the carrier the requester names, and the answer
 * comes back the way a capture's does.
 */

/** @brief A carrier other than our own receiver's, so a case sees it pass. */
#define RAW_CARRIER_HZ  40000U
/** @brief Symbols in the lent frame. */
#define RAW_N           5U
/** @brief How long a requester lets AO_Ir take to start the call: long
 *         enough for every case below but the one about running out. */
#define RAW_START_MS    60000U
/** @brief A mark and a space far from NEC's, so the frame is unmistakable. */
#define RAW_MARK_US     500U
#define RAW_SPACE_US    600U
/** @brief What a lent buffer holds before the owner touches it. */
#define LENT_FILL       0x5AU

static kdrv_ir_symbol_t s_raw_seen[KDRV_IR_TX_MAX_SYMBOLS];
static size_t           s_raw_seen_n;
static uint32_t         s_raw_seen_hz;
static unsigned         s_raw_calls;
static kode_status_t    s_raw_rc;

/** @brief kdrv_ir_send() as the worker calls it: keep what it was handed. */
static kode_status_t raw_send(const kdrv_ir_symbol_t *sym, size_t n,
                              uint32_t carrier_hz, int ncalls)
{
    (void)ncalls;
    TEST_ASSERT_TRUE(KDRV_IR_TX_MAX_SYMBOLS >= n);
    (void)memcpy(s_raw_seen, sym, n * sizeof(kdrv_ir_symbol_t));
    s_raw_seen_n  = n;
    s_raw_seen_hz = carrier_hz;
    ++s_raw_calls;
    return s_raw_rc;
}

/** @brief Bring the AO up with the send stubbed by raw_send(). */
static void raw_up(kode_status_t rc)
{
    ir_up_once();
    s_raw_calls = 0U;
    s_raw_rc    = rc;
    kdrv_ir_send_StubWithCallback(&raw_send);
}

/** @brief The lent frame. */
static void raw_frame(kode_sys_ir_symbol_t *sym)
{
    for ( uint16_t i = 0U; RAW_N > i; ++i )
    {
        sym[i].mark_us  = (uint16_t)(RAW_MARK_US + i);
        sym[i].space_us = (uint16_t)(RAW_SPACE_US + i);
    }
}

TEST_CASE("a raw request sends the lent frame on its carrier and answers OK",
          "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 7U, &ack);
    (void)qf_support_pump(AO_Ir, 4U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_raw_calls, "the call is the worker's");
    run_job();

    TEST_ASSERT_EQUAL_UINT(1U, s_raw_calls);
    TEST_ASSERT_EQUAL_UINT(RAW_N, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT32(RAW_CARRIER_HZ, s_raw_seen_hz);
    TEST_ASSERT_EQUAL_MEMORY(sym, s_raw_seen, sizeof(sym));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(7U, ack);
    TEST_ASSERT_TRUE_MESSAGE(khal_sem_wait_timeout(done, 0U), "the requester was woken");
    echo_window_drains();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a raw request's frame is copied as its call starts: changed after, nothing sent changes",
          "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    kode_sys_ir_symbol_t copy[RAW_N];
    khal_sem_t const     done = khal_sem_create();
    uint32_t             rc   = 0xFFU;
    uint32_t             ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    (void)memcpy(copy, sym, sizeof(sym));
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 17U, &ack);
    (void)qf_support_pump(AO_Ir, 4U);
    (void)memset(sym, LENT_FILL, sizeof(sym));   /* the call is the worker's now */
    run_job();

    TEST_ASSERT_EQUAL_UINT(1U, s_raw_calls);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(copy, s_raw_seen, sizeof(copy), "the driver read the lent frame in place");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(17U, ack);
    echo_window_drains();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a raw frame longer than the transmitter takes is refused KODE_DENIED, never cut",
          "[ir_ao][raw]")
{
    static kode_sys_ir_symbol_t big[KDRV_IR_TX_MAX_SYMBOLS + 1U];
    khal_sem_t const            done = khal_sem_create();
    uint32_t                    rc   = 0xFFU;
    uint32_t                    ack  = 0U;
    const StatusRspEvt         *a;

    raw_up(KODE_OK);
    ao_spy_drain();
    ir_send_raw_request(big, (uint16_t)(KDRV_IR_TX_MAX_SYMBOLS + 1U), RAW_CARRIER_HZ,
                        RAW_START_MS, &rc, done, 19U, &ack);
    (void)qf_support_pump(AO_Ir, 4U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_EQUAL_UINT32(19U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));

    TEST_ASSERT_EQUAL_INT(KODE_OK, ir_send_raw_ask(ao_spy(), big, (uint16_t)(KDRV_IR_TX_MAX_SYMBOLS + 1U),
                                                   RAW_CARRIER_HZ, RAW_START_MS));
    (void)qf_support_pump(AO_Ir, 4U);
    a = (const StatusRspEvt *)(const void *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(a, "the ask is answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)IR_SEND_RAW_RSP_SIG, (uint32_t)a->super.sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    ao_spy_release();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_raw_calls, "a frame past the bound reached the driver");
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(IR_WORKER));

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a raw request behind a NEC job waits for it", "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_pump(AO_Ir, 4U);
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 9U, &ack);
    (void)qf_support_pump(AO_Ir, 4U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xFFU, rc, "answered before its turn");

    run_job();   /* the NEC frame */
    TEST_ASSERT_EQUAL_UINT(1U, s_raw_calls);
    TEST_ASSERT_EQUAL_UINT(KNEC_SYMBOLS, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT32(KDRV_IR_CARRIER_HZ, s_raw_seen_hz);
    run_jobs();  /* the held raw frame */
    TEST_ASSERT_EQUAL_UINT(2U, s_raw_calls);
    TEST_ASSERT_EQUAL_UINT(RAW_N, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT32(RAW_CARRIER_HZ, s_raw_seen_hz);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(9U, ack);
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a raw frame the driver refuses is answered with that status",
          "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_ERROR);
    raw_frame(sym);
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 11U, &ack);
    (void)qf_support_pump(AO_Ir, 4U);
    run_job();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, rc);
    TEST_ASSERT_EQUAL_UINT32(11U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, ECHO_TICKS),
                                   "a frame that never went out opened a window");

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A raw send's answer is its call's own status, so one that outlives its
 * deadline is answered only when the call returns. */
TEST_CASE("a raw frame whose call outlives its deadline is answered when it returns",
          "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done  = khal_sem_create();
    uint32_t         rc    = 0xFFU;
    uint32_t         ack   = 0U;
    unsigned const   steps = QF_SUPPORT_STEPS(KDRV_IR_CALL_MAX_MS + KODE_IR_JOB_MARGIN_MS);

    raw_up(KODE_OK);
    raw_frame(sym);
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 13U, &ack);
    (void)qf_support_pump(AO_Ir, 4U);

    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Ir, steps));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0xFFU, rc,
                                     "answered while the worker still holds the frame");
    TEST_ASSERT_FALSE(khal_sem_wait_timeout(done, 0U));

    run_job();   /* the call returns at last */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(13U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));
    (void)qf_support_tick(AO_Ir, ECHO_TICKS);
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The facade waits a bounded time, and a request held behind other calls can
 * outlast it. AO_Ir must not start a call its requester has stopped waiting
 * for: the frame would go out after the app was told it did not. */
TEST_CASE("a raw request held past its start budget is answered and never sent",
          "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_pump(AO_Ir, 4U);
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, 100U, &rc, done, 15U, &ack);
    (void)qf_support_pump(AO_Ir, 4U);
    fake_time_advance_ms(200U);

    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_raw_calls, "only the NEC frame went out");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_TIMEOUT, rc);
    TEST_ASSERT_EQUAL_UINT32(15U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A worker with no room never takes the frame: the requester is answered at
 * once instead of being left asleep on a call that was never made. */
TEST_CASE("a raw request the worker has no room for is answered and never sent",
          "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    fake_khal_worker_refuse_for(IR_WORKER, true);
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done,
                        21U, &ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_NO_RESOURCES, rc);
    TEST_ASSERT_EQUAL_UINT32(21U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(IR_WORKER));
    TEST_ASSERT_EQUAL_UINT(0U, s_raw_calls);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, ECHO_TICKS),
                                   "no window for a frame that never left");
    fake_khal_worker_refuse_for(IR_WORKER, false);
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* The hold queue full: the raw request is refused as busy, answered once, and
 * the held frames still go out in their order. */
TEST_CASE("a raw request over the hold queue's room is refused as busy",
          "[ir_ao][raw]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    for ( unsigned i = 0U; (unsigned)KODE_IR_DEFER_DEPTH > i; ++i )
    {
        ir_send_request(FRAME_ADDR, FRAME_CMD);
    }
    (void)qf_support_settle(AO_Ir);

    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done,
                        22U, &ack);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_EQUAL_UINT32(22U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));

    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U + (unsigned)KODE_IR_DEFER_DEPTH, s_raw_calls,
                                   "the NEC frames went out, the raw one did not");
    TEST_ASSERT_FALSE_MESSAGE(khal_sem_wait_timeout(done, 0U),
                              "answered once, not again");
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- The link -------------------------------------------------------------------
 * The block is behind the C5 link, which AO_Radio brings up. A request that
 * finds it down asks AO_Radio for it once, is held, and is served when the link
 * is up - or refused when AO_Radio says it will not come. AO_Radio is a mailbox
 * here, and its answer is posted to AO_Ir by hand.
 */

/** @brief Give AO_Radio a mailbox, and empty it. */
static void radio_mailbox(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Radio);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief How many link requests AO_Ir posted to AO_Radio's mailbox; empties it. */
static unsigned radio_ups(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)RADIO_UP_REQ_SIG == e->sig )
        {
            ++n;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief AO_Radio's state of the link, as AO_Ir hears it. */
static void link_says(kradio_link_t link)
{
    RadioStateEvt * const e = Q_NEW(RadioStateEvt, RADIO_STATE_SIG);

    e->link      = (uint8_t)link;
    e->fw_known  = false;
    e->fw_major  = 0U;
    e->fw_minor  = 0U;
    e->fw_patch  = 0U;
    e->sd_hosted = false;
    QACTIVE_POST(AO_Ir, &e->super, NULL);
    (void)qf_support_settle(AO_Ir);
}

TEST_CASE("a request with the C5 link down asks AO_Radio for it once, waits, and "
          "is served in order once it is up", "[ir_ao][link]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    radio_mailbox();
    fake_radio_set_link(false);

    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 31U, &ack);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, radio_ups(), "the link is asked for once");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(IR_WORKER),
                                   "nothing went to the C5 with no link to carry it");
    TEST_ASSERT_FALSE_MESSAGE(khal_sem_wait_timeout(done, 0U), "the raw send waits");

    link_says(KRADIO_STARTING);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(IR_WORKER),
                                   "on its way is not up");

    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_raw_calls, "both frames went out");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(31U, ack);
    TEST_ASSERT_TRUE_MESSAGE(khal_sem_wait_timeout(done, 0U), "the raw send is answered");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, radio_ups(), "nothing asked again");
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/** @brief How often AO_Ir looks for a held request past its budget, in ms, as a literal so a changed period is caught. */
#define LINK_CHECK_MS  100U

/* QP posts a time event with no margin, and a queue that is full is an
 * assertion and a reset. The check of a held request is no exception: one that
 * is periodic goes on posting whether or not the AO took the last, so one
 * waits at most, and the check that is taken arms the next. The count of
 * what waits fails on the periodic timer; the rest are guards. */
TEST_CASE("a check of the held requests nobody took is not followed by another; "
          "the one taken arms the next", "[ir_ao][link]")
{
    raw_up(KODE_OK);
    radio_mailbox();
    fake_radio_set_link(false);

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());

    /* Five periods go by and AO_Ir never runs. */
    qf_support_time_passes(5U * KQP_TICKS_PER_MS(LINK_CHECK_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, (unsigned)uxQueueMessagesWaiting(AO_Ir->eQueue),
                                   "every period queued a check the AO had not taken");

    TEST_ASSERT_EQUAL_UINT(1U, qf_support_settle(AO_Ir));

    /* One period after the check that was taken, the next. */
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Ir, QF_SUPPORT_STEPS(LINK_CHECK_MS)));

    /* The link comes, the request is served, and no check follows. */
    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    echo_window_drains();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, 2U * QF_SUPPORT_STEPS(LINK_CHECK_MS)),
                                   "a check was queued after the wait had ended");
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* A check queued before the wait ended is taken after it and arms nothing.
 * Every assertion here is a guard that holds on the periodic timer too, which
 * the end of the wait disarmed already. */
TEST_CASE("a check of the held requests taken after the wait ended arms nothing",
          "[ir_ao][link]")
{
    static const QEvt check = QEVT_INITIALIZER(IR_LINK_TO_SIG);

    raw_up(KODE_OK);
    radio_mailbox();
    fake_radio_set_link(false);

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());

    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    echo_window_drains();

    QASM_DISPATCH(AO_Ir, &check, 0U);
    qf_support_time_passes(2U * KQP_TICKS_PER_MS(LINK_CHECK_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (unsigned)uxQueueMessagesWaiting(AO_Ir->eQueue),
                                   "a check followed a wait that had ended");
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("once AO_Radio says the link will not come, requests are refused at once and "
          "ask nothing, until the link is seen up again", "[ir_ao][link]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    khal_sem_t const done = khal_sem_create();
    uint32_t         rc   = 0xFFU;
    uint32_t         ack  = 0U;

    raw_up(KODE_OK);
    raw_frame(sym);
    radio_mailbox();
    fake_radio_set_link(false);

    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 33U, &ack);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());

    link_says(KRADIO_FAILED);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_EQUAL_UINT32(33U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_raw_calls, "nothing was sent");
    TEST_ASSERT_EQUAL_UINT(0U, fake_khal_worker_pending_for(IR_WORKER));

    /* The next request is answered at once: no wait, and no unmount of the
     * card for a bring-up that will not come. A listener's arm is owed. */
    rc = 0xFFU;
    ir_send_raw_request(sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS, &rc, done, 34U, &ack);
    ir_capture_request(NULL, 0U, NULL, NULL, 0U, 0U, NULL);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, rc);
    TEST_ASSERT_EQUAL_UINT32(34U, ack);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(done, 0U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, radio_ups(), "nothing asks for the link again");

    /* Brought up by somebody else: the owed arm is made, and a request that
     * finds the link down after that asks again. */
    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    fake_radio_set_link(false);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, radio_ups(), "a link seen up is worth asking for again");
    link_says(KRADIO_OFF);
    TEST_ASSERT_EQUAL_UINT(0U, s_raw_calls);

    /* Left as the next case expects: the link up, and seen so. */
    fake_radio_set_link(true);
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT(1U, s_raw_calls);
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/** @brief A static event AO_Radio's mailbox holds, to take its room with. */
static const QEvt s_radio_room = QEVT_INITIALIZER(RADIO_STATE_REQ_SIG);

TEST_CASE("the ask for the link leaves AO_Radio's margin to its own traffic",
          "[ir_ao][link]")
{
    const QEvt  *e = NULL;
    UBaseType_t  room;

    radio_mailbox();
    while ( uxQueueSpacesAvailable(AO_Radio->eQueue) > (UBaseType_t)KQP_RADIO_POST_MARGIN )
    {
        TEST_ASSERT_TRUE(QACTIVE_POST_X(AO_Radio, &s_radio_room, 0U, NULL));
    }
    radio_up_request();
    room = uxQueueSpacesAvailable(AO_Radio->eQueue);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE((UBaseType_t)KQP_RADIO_POST_MARGIN, room,
                                   "dropped: the asker's own budget covers it");
}

/** @brief Steps of 10 ms past AO_Ir's wait for the link. */
#define LINK_WAIT_STEPS (QF_SUPPORT_STEPS(KODE_IR_LINK_WAIT_MS) + 1U)

TEST_CASE("a status request while a call is out and the link is asked for is answered at once",
          "[ir_ao][link]")
{
    worker_up();
    radio_mailbox();
    fake_radio_set_link(false);

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    ir_info_request(ao_spy());
    ir_info_request(ao_spy());
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());
    TEST_ASSERT_FALSE_MESSAGE(info_answer()->rx_high,
                              "the second, without the reading the first is making");

    run_job();
    TEST_ASSERT_TRUE_MESSAGE(info_answer()->rx_high, "and the first with it");
    ao_spy_drain();

    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT(1U, s_sends);
    echo_window_drains();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("the link coming up while a call is out arms the receiver once the call returns",
          "[ir_ao][link]")
{
    worker_up();
    radio_mailbox();
    fake_radio_set_link(false);

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());

    /* A status reading needs no link: it is the call out when the link comes. */
    ir_info_request(ao_spy());
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));

    fake_radio_set_link(true);
    link_says(KRADIO_UP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(IR_WORKER),
                                   "the arm waits for the reading");

    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    run_jobs();
    (void)info_answer();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends, "and the held frame goes out after it");
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a listener's owed arm stays owed while a call is out, and runs when it returns",
          "[ir_ao][link]")
{
    worker_up();
    radio_mailbox();
    fake_radio_set_link(false);

    ir_capture_request(NULL, 0U, NULL, NULL, 0U, 0U, NULL);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, LINK_WAIT_STEPS),
                                   "owed, not held: no check runs for it");

    ir_info_request(ao_spy());
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(IR_WORKER));

    fake_radio_set_link(true);
    link_says(KRADIO_UP);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    run_jobs();
    (void)info_answer();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/** @brief How long a held capture below waits for the link, in ms: three fifths of its HOLD_MS. */
#define HELD_MS   30U

TEST_CASE("a capture held for the link keeps the deadline it was posted with",
          "[ir_ao][link]")
{
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got = 99U;
    khal_sem_t const sem = khal_sem_create();

    worker_up();
    radio_mailbox();
    fake_radio_set_link(false);
    fake_sem_reset();

    ir_capture_request(out, KNEC_SYMBOLS, &got, sem, HOLD_MS, 0U, NULL);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());
    fake_time_advance_ms(HELD_MS);

    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);   /* the link's */
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);   /* the capture's */
    link_says(KRADIO_UP);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT(0U, fake_sem_signals());

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, QF_SUPPORT_STEPS(HOLD_MS - HELD_MS) - 1U),
                                   "what is left of its deadline, and no more");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Ir, 1U));
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT(0U, got);

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("a capture whose deadline ran out while it was held is answered when it is taken",
          "[ir_ao][link]")
{
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got = 99U;
    khal_sem_t const sem = khal_sem_create();

    worker_up();
    radio_mailbox();
    fake_radio_set_link(false);
    fake_sem_reset();

    ir_capture_request(out, KNEC_SYMBOLS, &got, sem, HOLD_MS, 0U, NULL);
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());
    fake_time_advance_ms(HOLD_MS);   /* and no check ran to see it */

    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(), "answered as it is taken");
    TEST_ASSERT_EQUAL_UINT(0U, got);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Ir, HOLD_TICKS),
                                   "with no deadline left behind it");

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/** @brief How many captured log lines are warnings. */
static unsigned warnings(void)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);

        if ( (NULL != l) && (strncmp(l, "!W", 2U) == 0) )
        {
            ++n;
        }
    }
    return n;
}

TEST_CASE("listeners' arms with the link down are one owed arm: the hold queue keeps its "
          "room, and nothing warns", "[ir_ao][link]")
{
    worker_up();
    radio_mailbox();
    fake_radio_set_link(false);
    fake_klog_reset();

    for ( uint32_t i = 0U; (uint32_t)KODE_IR_DEFER_DEPTH >= i; ++i )
    {
        ir_capture_request(NULL, 0U, NULL, NULL, 0U, 0U, NULL);
    }
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, radio_ups(), "the link is asked for once");

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_settle(AO_Ir);

    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sends, "the frame was held, not refused as busy");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, warnings(), "an arm is nobody's bad news");
    echo_window_drains();
    ao_spy_drain();

    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/** @brief A static event AO_Ir ignores, to take room in its queue with. */
static const QEvt s_ir_room = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

TEST_CASE("a capture request that would take AO_Ir's last slots is answered empty, not posted",
          "[ir_ao]")
{
    IrSymbol         out[KNEC_SYMBOLS];
    size_t           got = 99U;
    uint32_t         ack = 0U;
    khal_sem_t const sem = khal_sem_create();
    const QEvt      *e   = NULL;
    UBaseType_t      room;

    worker_up();
    while ( uxQueueSpacesAvailable(AO_Ir->eQueue) > (UBaseType_t)KQP_IR_CMD_MARGIN )
    {
        TEST_ASSERT_TRUE(QACTIVE_POST_X(AO_Ir, &s_ir_room, 0U, NULL));
    }

    ir_capture_request(out, KNEC_SYMBOLS, &got, sem, HOLD_MS, 7U, &ack);
    room = uxQueueSpacesAvailable(AO_Ir->eQueue);
    /* Emptied unrun before anything is judged: what is left would starve the next cases. */
    while ( xQueueReceive(AO_Ir->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE((UBaseType_t)KQP_IR_CMD_MARGIN, room,
                                   "the margin is left to AO_Ir's own traffic");
    TEST_ASSERT_EQUAL_UINT(0U, got);
    TEST_ASSERT_EQUAL_UINT32(7U, ack);
    TEST_ASSERT_TRUE_MESSAGE(khal_sem_wait_timeout(sem, 0U), "and its waiter is woken");
    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

/* ---- kode_sys's requests: answered to the asking AO --------------------------
 * kode_sys_ir_send_raw(), _capture() and _last() post and return; the answer is
 * an event posted to the AO that asked, the spy here, carrying the status. The
 * lent frame is copied as its call starts; the lent capture buffer is written
 * before the answer and never after.
 */

/** @brief A static event AO_Ir ignores, to fill its queue with. */
static const QEvt s_ir_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief Most blocks a case takes out of a pool to leave it dry. */
#define DRY_MAX  128U

/** @brief The next thing the spy holds, which must be @p sig. */
static const void *ask_take(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "every accepted request is answered");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)sig, (uint32_t)e->sig, "the wrong answer");
    return (const void *)e;
}

/** @brief Empty AO_Ir's queue unrun. */
static void ir_queue_empty(void)
{
    const QEvt *e;

    while ( xQueueReceive(AO_Ir->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief AO_Ir was last built with no infrared block. */
static bool s_ir_absent;

/** @brief A kdrv_ir_init() that finds no infrared block. */
static kode_status_t init_absent(kdrv_ir_rx_cb_t cb, void *ctx, int ncalls)
{
    (void)cb;
    (void)ctx;
    (void)ncalls;
    return KODE_IO_ERROR;
}

/**
 * @brief Build AO_Ir again, with its infrared block when @p fitted.
 * @param fitted Whether kdrv_ir_init() finds the block.
 */
static void ir_rebuild(bool fitted)
{
    ir_queue_empty();
    qf_support_reap_timers();
    if ( fitted )
    {
        kdrv_ir_init_StubWithCallback(&capture_init);
    }
    else
    {
        kdrv_ir_init_StubWithCallback(&init_absent);
    }
    Ir_ctor();
    QASM_INIT(AO_Ir, (void *)0, 0U);
    if ( fitted )
    {
        kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
        run_job();
    }
    s_ir_absent = !fitted;
}

/** @brief AO_Ir up with its send stubbed, both askers' mailboxes empty. */
static void ask_up(kode_status_t send_rc)
{
    raw_up(send_rc);
    if ( s_ir_absent )
    {
        /* A case that left the block absent is undone on the way into the next. */
        ir_rebuild(true);
    }
    ir_queue_empty();
    ao_spy_drain();
    ao_spy_other_drain();
}

/** @brief Leave nothing armed or owed behind a case. */
static void ask_down(void)
{
    ao_spy_drain();
    ao_spy_other_drain();
    Mockkdrv_ir_Verify();
    Mockkdrv_ir_Destroy();
}

TEST_CASE("kode_sys_ir_send_raw sends the lent frame and answers the asking AO once",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t           sym[RAW_N];
    kode_sys_ir_symbol_t           copy[RAW_N];
    const kode_sys_ir_send_answer_t *a;

    ask_up(KODE_OK);
    raw_frame(sym);
    (void)memcpy(copy, sym, sizeof(sym));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_send_raw(ao_spy(), sym,
                                                        RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED,
        kode_sys_ir_send_raw(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ),
        "a second request of the kind waits for the first's answer");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing is answered before the call returns");
    run_job();

    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(1U, s_raw_calls);
    TEST_ASSERT_EQUAL_UINT32(RAW_CARRIER_HZ, s_raw_seen_hz);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(copy, s_raw_seen, sizeof(copy), "the driver sent the lent frame");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(copy, sym, sizeof(copy), "the lent frame is left as it was");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once, not published as well");
    echo_window_drains();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
        kode_sys_ir_send_raw(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ),
        "the answer opens the door again");
    (void)qf_support_pump(AO_Ir, 4U);
    run_job();
    (void)ask_take(IR_SEND_RAW_RSP_SIG);
    echo_window_drains();
    ask_down();
}

TEST_CASE("a raw ask's frame is copied as its call starts: changed after, nothing sent changes",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t             sym[RAW_N];
    kode_sys_ir_symbol_t             copy[RAW_N];
    const kode_sys_ir_send_answer_t *a;

    ask_up(KODE_OK);
    raw_frame(sym);
    (void)memcpy(copy, sym, sizeof(sym));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_send_raw(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    (void)memset(sym, LENT_FILL, sizeof(sym));   /* the call is the worker's now */
    run_job();

    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(copy, s_raw_seen, sizeof(copy), "the driver read the lent frame in place");
    echo_window_drains();
    ask_down();
}

TEST_CASE("a raw ask the driver refuses is answered with the driver's status",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t             sym[RAW_N];
    const kode_sys_ir_send_answer_t *a;

    ask_up(KODE_DENIED);
    raw_frame(sym);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_send_raw(ao_spy(), sym,
                                                        RAW_N, RAW_CARRIER_HZ));
    (void)qf_support_pump(AO_Ir, 4U);
    run_job();
    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ask_down();
}

TEST_CASE("two AOs sending raw frames are both accepted and both answered",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t             sym[RAW_N];
    const kode_sys_ir_send_answer_t *a;
    const QEvt                      *o;

    ask_up(KODE_OK);
    raw_frame(sym);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_send_raw(ao_spy(), sym,
                                                        RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
        kode_sys_ir_send_raw(ao_spy_other(), sym, RAW_N, RAW_CARRIER_HZ),
        "the door is the asker's, not the kind's");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kode_sys_ir_send_raw(ao_spy_other(), sym, RAW_N, RAW_CARRIER_HZ));
    (void)qf_support_pump(AO_Ir, 4U);
    run_jobs();

    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered too");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)IR_SEND_RAW_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, ((const kode_sys_ir_send_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_NULL(ao_spy_other_take());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT(2U, s_raw_calls);
    echo_window_drains();
    ask_down();
}

TEST_CASE("a lent raw frame whose call outlives its deadline is answered only when the call returns",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t             sym[RAW_N];
    kode_sys_ir_symbol_t             copy[RAW_N];
    const kode_sys_ir_send_answer_t *a;
    uint32_t const                   steps = QF_SUPPORT_STEPS(KDRV_IR_CALL_MAX_MS + KODE_IR_JOB_MARGIN_MS);

    ask_up(KODE_OK);
    raw_frame(sym);
    (void)memcpy(copy, sym, sizeof(sym));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_send_raw(ao_spy(), sym,
                                                        RAW_N, RAW_CARRIER_HZ));
    (void)qf_support_pump(AO_Ir, 4U);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Ir, steps));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "answered while the worker still holds the lent frame");

    run_job();   /* the call returns at last */
    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(copy, sym, sizeof(copy), "the lent frame is left as it was");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    (void)qf_support_tick(AO_Ir, ECHO_TICKS);
    ask_down();
}

TEST_CASE("a raw ask held past its start budget is answered KODE_TIMEOUT and never sent",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t             sym[RAW_N];
    const kode_sys_ir_send_answer_t *a;

    ask_up(KODE_OK);
    raw_frame(sym);

    ir_send_request(FRAME_ADDR, FRAME_CMD);
    (void)qf_support_pump(AO_Ir, 4U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ir_send_raw_ask(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ, 100U));
    (void)qf_support_pump(AO_Ir, 4U);
    fake_time_advance_ms(200U);

    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_raw_calls, "only the NEC frame went out");
    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    echo_window_drains();
    ask_down();
}

TEST_CASE("a raw ask held for a link no state ever answers is answered KODE_TIMEOUT "
          "on its own budget, and its door is free again", "[ir_ao][ask][link]")
{
    kode_sys_ir_symbol_t             sym[RAW_N];
    const kode_sys_ir_send_answer_t *a;

    ask_up(KODE_OK);
    raw_frame(sym);
    radio_mailbox();
    fake_radio_set_link(false);

    TEST_ASSERT_EQUAL_INT(KODE_OK, ir_send_raw_ask(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ, 100U));
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "nothing is answered inside the budget");

    /* No RADIO_STATE_SIG ever comes: the publish was lost. */
    for ( unsigned step = 0U; 30U > step; ++step )
    {
        fake_time_advance_ms(10U);
        (void)qf_support_tick(AO_Ir, 1U);
    }
    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_raw_calls, "nothing was sent");

    /* The door is free, and the wait is over: the next ask asks for the link again. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, ir_send_raw_ask(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ, RAW_START_MS));
    (void)qf_support_settle(AO_Ir);
    TEST_ASSERT_EQUAL_UINT(1U, radio_ups());
    fake_radio_set_link(true);
    kdrv_ir_rx_arm_ExpectAndReturn(KODE_OK);
    link_says(KRADIO_UP);
    run_jobs();
    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    echo_window_drains();
    ask_down();
}

TEST_CASE("a raw ask over the hold queue's room is answered KODE_DENIED and its frame is untouched",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t             sym[RAW_N];
    kode_sys_ir_symbol_t             copy[RAW_N];
    const kode_sys_ir_send_answer_t *a;

    ask_up(KODE_OK);
    raw_frame(sym);
    (void)memcpy(copy, sym, sizeof(sym));
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    for ( uint32_t i = 0U; (uint32_t)KODE_IR_DEFER_DEPTH > i; ++i )
    {
        ir_send_request(FRAME_ADDR, FRAME_CMD);
    }
    (void)qf_support_settle(AO_Ir);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_send_raw(ao_spy(), sym,
                                                        RAW_N, RAW_CARRIER_HZ));
    (void)qf_support_settle(AO_Ir);
    a = (const kode_sys_ir_send_answer_t *)ask_take(IR_SEND_RAW_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(copy, sym, sizeof(copy), "a refused frame is never touched");

    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U + (uint32_t)KODE_IR_DEFER_DEPTH, s_raw_calls,
                                   "the NEC frames went out, the raw one did not");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered once");
    echo_window_drains();
    ask_down();
}

TEST_CASE("a capture ask with no infrared block is answered KODE_IO_ERROR and its buffer is left alone",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t                    out[KNEC_SYMBOLS];
    kdrv_ir_symbol_t                    sentinel[KNEC_SYMBOLS];
    const kode_sys_ir_capture_answer_t *a;

    ask_up(KODE_OK);
    ir_rebuild(false);
    (void)memset(out, LENT_FILL, sizeof(out));
    (void)memcpy(sentinel, out, sizeof(out));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_capture(ao_spy(), out,
                                                       KNEC_SYMBOLS, HOLD_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    a = (const kode_sys_ir_capture_answer_t *)ask_take(IR_CAPTURE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, (uint32_t)a->n);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, out, sizeof(out), "no block, yet the lent buffer was written");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered once");
    ir_rebuild(true);
    ask_down();
}

TEST_CASE("a capture ask over the hold queue's room is answered KODE_DENIED and its buffer is untouched",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t                    out[KNEC_SYMBOLS];
    kdrv_ir_symbol_t                    sentinel[KNEC_SYMBOLS];
    const kode_sys_ir_capture_answer_t *a;

    ask_up(KODE_OK);
    (void)memset(out, LENT_FILL, sizeof(out));
    (void)memcpy(sentinel, out, sizeof(out));
    ir_send_request(FRAME_ADDR, FRAME_CMD);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    for ( uint32_t i = 0U; (uint32_t)KODE_IR_DEFER_DEPTH > i; ++i )
    {
        ir_send_request(FRAME_ADDR, FRAME_CMD);
    }
    (void)qf_support_settle(AO_Ir);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_capture(ao_spy(), out,
                                                       KNEC_SYMBOLS, HOLD_MS));
    (void)qf_support_settle(AO_Ir);
    a = (const kode_sys_ir_capture_answer_t *)ask_take(IR_CAPTURE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, (uint32_t)a->n);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, out, sizeof(out), "a refused capture is never written");

    run_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered once");
    echo_window_drains();
    ask_down();
}

TEST_CASE("kode_sys_ir_capture fills the lent buffer, then answers once, and never writes it again",
          "[ir_ao][ask]")
{
    kdrv_ir_symbol_t                    sym[KNEC_SYMBOLS];
    kode_sys_ir_symbol_t                    out[KNEC_SYMBOLS];
    kdrv_ir_symbol_t                    after[KNEC_SYMBOLS];
    const kode_sys_ir_capture_answer_t *a;

    ask_up(KODE_OK);
    a_frame(sym);
    (void)memset(out, LENT_FILL, sizeof(out));

    expect_capture_arm();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_capture(ao_spy(), out,
                                                       KNEC_SYMBOLS, HOLD_MS));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED,
        kode_sys_ir_capture(ao_spy(), out, KNEC_SYMBOLS, HOLD_MS),
        "a second capture of the asker's waits for the first's answer");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing has been heard yet");

    deliver(sym, KNEC_SYMBOLS);
    a = (const kode_sys_ir_capture_answer_t *)ask_take(IR_CAPTURE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(KNEC_SYMBOLS, a->n);
    TEST_ASSERT_EQUAL_MEMORY(sym, out, sizeof(sym));
    (void)ask_take(IR_RX_SIG);   /* the frame is published as ever, apart from the answer */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the answer was posted once, not published");
    ao_spy_release();

    /* A later frame belongs to nobody: the answered buffer is not written. */
    (void)memcpy(after, out, sizeof(after));
    deliver(sym, 2U);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(after, out, sizeof(after), "the lent buffer was written after the answer");
    ao_spy_drain();
    ask_down();
}

/** @brief Symbols the short capture below has room for. */
#define SHORT_ROOM  5U

/** @brief What a lent symbol's field reads after LENT_FILL. */
#define LENT_FILL16  0x5A5AU

TEST_CASE("a capture copies each symbol's mark and space into the asker's symbols, and nothing past its room",
          "[ir_ao][ask]")
{
    kdrv_ir_symbol_t                    sym[KNEC_SYMBOLS];
    kode_sys_ir_symbol_t                out[KNEC_SYMBOLS];
    const kode_sys_ir_capture_answer_t *a;

    ask_up(KODE_OK);
    a_frame(sym);
    (void)memset(out, LENT_FILL, sizeof(out));

    expect_capture_arm();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_capture(ao_spy(), out, SHORT_ROOM, HOLD_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    deliver(sym, KNEC_SYMBOLS);

    a = (const kode_sys_ir_capture_answer_t *)ask_take(IR_CAPTURE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(SHORT_ROOM, a->n);
    for ( uint32_t i = 0U; SHORT_ROOM > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT16(sym[i].mark_us, out[i].mark_us);
        TEST_ASSERT_EQUAL_UINT16(sym[i].space_us, out[i].space_us);
    }
    for ( uint32_t i = SHORT_ROOM; KNEC_SYMBOLS > i; ++i )
    {
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(LENT_FILL16, out[i].mark_us, "written past the room it lent");
        TEST_ASSERT_EQUAL_HEX16_MESSAGE(LENT_FILL16, out[i].space_us, "written past the room it lent");
    }
    (void)ask_take(IR_RX_SIG);
    ao_spy_drain();
    ask_down();
}

TEST_CASE("a capture ask nobody answers is closed by its deadline with KODE_TIMEOUT, buffer untouched",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t                    out[KNEC_SYMBOLS];
    kdrv_ir_symbol_t                    sentinel[KNEC_SYMBOLS];
    const kode_sys_ir_capture_answer_t *a;

    ask_up(KODE_OK);
    (void)memset(out, LENT_FILL, sizeof(out));
    (void)memcpy(sentinel, out, sizeof(out));

    expect_capture_arm();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_capture(ao_spy(), out,
                                                       KNEC_SYMBOLS, HOLD_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    (void)qf_support_tick(AO_Ir, HOLD_TICKS);

    a = (const kode_sys_ir_capture_answer_t *)ask_take(IR_CAPTURE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, a->n);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, out, sizeof(out), "nothing was heard, nothing was written");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ask_down();
}

TEST_CASE("a capture ask displaced by another asker's is answered KODE_DENIED, and the other hears the frame",
          "[ir_ao][ask]")
{
    kdrv_ir_symbol_t                    sym[KNEC_SYMBOLS];
    kode_sys_ir_symbol_t                    first[KNEC_SYMBOLS];
    kode_sys_ir_symbol_t                    second[KNEC_SYMBOLS];
    kdrv_ir_symbol_t                    sentinel[KNEC_SYMBOLS];
    const kode_sys_ir_capture_answer_t *a;
    const QEvt                         *o;

    ask_up(KODE_OK);
    a_frame(sym);
    (void)memset(first, LENT_FILL, sizeof(first));
    (void)memcpy(sentinel, first, sizeof(first));
    (void)memset(second, 0, sizeof(second));

    expect_capture_arm();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_capture(ao_spy(), first,
                                                       KNEC_SYMBOLS, HOLD_MS));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();
    expect_capture_arm();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
        kode_sys_ir_capture(ao_spy_other(), second, KNEC_SYMBOLS, HOLD_MS),
        "the door is the asker's, not the kind's");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    run_job();

    a = (const kode_sys_ir_capture_answer_t *)ask_take(IR_CAPTURE_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, first, sizeof(first), "the displaced buffer is untouched");
    ao_spy_release();

    deliver(sym, KNEC_SYMBOLS);
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)IR_CAPTURE_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, ((const kode_sys_ir_capture_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_EQUAL_MEMORY(sym, second, sizeof(sym));
    (void)ask_take(IR_RX_SIG);   /* the frame is published as ever, apart from the answer */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
    ask_down();
}

TEST_CASE("kode_sys_ir_last answers what the receiver last produced, once",
          "[ir_ao][ask]")
{
    kdrv_ir_symbol_t                 sym[KNEC_SYMBOLS];
    const kode_sys_ir_last_answer_t *a;

    ask_up(KODE_OK);
    a_frame(sym);
    deliver(sym, KNEC_SYMBOLS);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ir_last(ao_spy()));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_last(ao_spy()));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Ir, 4U));
    a = (const kode_sys_ir_last_answer_t *)ask_take(IR_LAST_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->have);
    TEST_ASSERT_TRUE(a->frame.nec);
    TEST_ASSERT_EQUAL_HEX16(FRAME_ADDR, a->frame.addr);
    TEST_ASSERT_EQUAL_HEX16(FRAME_CMD, a->frame.cmd);
    TEST_ASSERT_EQUAL_UINT16(KNEC_SYMBOLS, a->frame.symbols);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_ir_last(ao_spy()), "the answer opens the door again");
    (void)qf_support_pump(AO_Ir, 4U);
    (void)ask_take(IR_LAST_RSP_SIG);
    ask_down();
}

TEST_CASE("a plain task's or a badly asked infrared request is refused at once and posts nothing",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    kode_sys_ir_symbol_t out[KNEC_SYMBOLS];

    ask_up(KODE_OK);
    raw_frame(sym);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_send_raw(NULL, sym,
                                                            RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_send_raw(ao_spy(), NULL, RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_send_raw(ao_spy(), sym,
                                                            0U, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_send_raw(ao_spy(), sym,
                                                            KODE_SYS_IR_RAW_MAX_SYMBOLS + 1U,
                                                            RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_send_raw(ao_spy(), sym,
                                                            RAW_N, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_capture(NULL, out,
                                                           KNEC_SYMBOLS, HOLD_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_capture(ao_spy(), NULL, KNEC_SYMBOLS, HOLD_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_capture(ao_spy(), out, 0U, HOLD_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_capture(ao_spy(), out,
                                                           KNEC_SYMBOLS, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ir_last(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Ir->eQueue));

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_ir_last(ao_spy()), "a refusal takes no door");
    (void)qf_support_pump(AO_Ir, 4U);
    (void)ask_take(IR_LAST_RSP_SIG);
    ask_down();
}

TEST_CASE("a full AO_Ir queue refuses kode_sys's requests with no resources",
          "[ir_ao][ask]")
{
    kode_sys_ir_symbol_t sym[RAW_N];
    kode_sys_ir_symbol_t out[KNEC_SYMBOLS];

    ask_up(KODE_OK);
    raw_frame(sym);

    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Ir, &s_ir_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ir_send_raw(ao_spy(), sym,
                                                                  RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ir_capture(ao_spy(), out,
                                                                 KNEC_SYMBOLS, HOLD_MS));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ir_last(ao_spy()));
    ir_queue_empty();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_ir_last(ao_spy()),
                                  "nothing was posted, so the door is open");
    (void)qf_support_pump(AO_Ir, 4U);
    (void)ask_take(IR_LAST_RSP_SIG);
    ask_down();
}

TEST_CASE("a dry pool refuses kode_sys's infrared request with no resources and posts nothing",
          "[ir_ao][ask]")
{
    static StatusRspEvt *held[DRY_MAX];
    kode_sys_ir_symbol_t sym[RAW_N];
    uint32_t             n = 0U;

    ask_up(KODE_OK);
    raw_frame(sym);

    while ( DRY_MAX > n )
    {
        held[n] = Q_NEW_X(StatusRspEvt, 0U, IR_SEND_RAW_RSP_SIG);
        if ( NULL == held[n] )
        {
            break;
        }
        n++;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(DRY_MAX, n, "the pool never ran dry");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ir_send_raw(ao_spy(), sym,
                                                                  RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Ir->eQueue));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        QF_gc(&held[i]->super);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
        kode_sys_ir_send_raw(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ),
        "nothing was posted, so the door is open");
    (void)qf_support_pump(AO_Ir, 4U);
    run_job();
    (void)ask_take(IR_SEND_RAW_RSP_SIG);
    echo_window_drains();
    ask_down();
}

TEST_CASE("an infrared ask whose request finds its pool dry gives the answer back and posts nothing",
          "[ir_ao][ask]")
{
    static qf_support_dry_t dry;
    kode_sys_ir_symbol_t    sym[RAW_N];
    uint16_t                before;

    ask_up(KODE_OK);
    raw_frame(sym);
    before = qf_support_pool_dry_second(&dry, (uint16_t)sizeof(IrSendRawEvt), (uint16_t)sizeof(StatusRspEvt));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, before, "the pools could not be set up");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ir_send_raw(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(before, qf_support_pool_free(&dry), "the answer went back to its pool");
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Ir->eQueue));
    qf_support_pool_give_back(&dry);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_ir_send_raw(ao_spy(), sym, RAW_N, RAW_CARRIER_HZ),
                                  "nothing was posted, so the door is open");
    (void)qf_support_pump(AO_Ir, 4U);
    run_job();
    (void)ask_take(IR_SEND_RAW_RSP_SIG);
    echo_window_drains();
    ask_down();
}

/** @brief The body of the full-queue case, run in a child: the asker's queue is full, then AO_Ir answers into it. */
static void full_asker_body(void *arg)
{
    (void)arg;
    (void)qf_support_queue_stuff(ao_spy_other(), &s_ir_filler);
    (void)kode_sys_ir_last(ao_spy_other());
    (void)qf_support_settle(AO_Ir);
}

TEST_CASE("an answer into an asker's full queue is QP's assert, not a drop",
          "[ir_ao][ask]")
{
    ask_up(KODE_OK);

    /* QP asserts with Q_onError(), which aborts this binary, so the post runs
     * in a child and the parent reads how it died. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)ASK_CHILD_ABORTED,
                                    (uint32_t)ask_support_fork_run(&full_asker_body, NULL),
                                  "the child was not stopped by QP's assert");
    ask_down();
}
