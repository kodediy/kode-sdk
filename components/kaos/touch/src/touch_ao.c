/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file touch_ao.c
 * @brief AO_Touch: the Active Object that owns the touch panel.
 */
#include "sdkconfig.h"
#include "kaos.h"
#include "kaos_start.h"

#include <stdio.h>

#include "kbsp.h"
#include "kqp.h"
#include "kaos_claim.h"
#include "kdrv_cst820.h"
#include "kode_log.h"
#include "khal_time.h"
#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.touch"

/** Only report movement beyond this many pixels while a finger is down. */
#define TOUCH_MOVE_EPSILON 2

/** Empty polls after an edge before deciding there is no finger behind it. */
#define TOUCH_EMPTY_POLLS  3U

/** Polls a contact stays in its circle before it stops counting as input. */
#define TOUCH_STILL_POLLS  (KODE_TOUCH_STILL_MS / KODE_TOUCH_POLL_MS)

/** Milliseconds in a second, for the log. */
#define TOUCH_MS_PER_S     1000U

/** The controller's IRQ, as a bit of the expander's byte. */
#define TP_INT_BIT ((uint8_t)(1U << KBSP_IOEXP_BIT_TP_INT))

/* The timed looks for a controller that did not answer the bring-up
 * (kqp_config.h): the waits before the first ones, then the spaced ones. */
static const uint16_t LOOK_SOON_MS[] = KODE_TOUCH_LOOK_SOON_MS;
#define LOOK_SOON    ((uint8_t)Q_DIM(LOOK_SOON_MS))   /**< How many looks come soon. */
#define LOOK_SPACED  ((uint8_t)(KODE_TOUCH_LOOK_FOR_MS / KODE_TOUCH_LOOK_EVERY_MS)) /**< How many spaced looks follow. */
#define LOOK_TIMED   ((uint8_t)(LOOK_SOON + LOOK_SPACED))   /**< All the timed looks. */
_Static_assert((Q_DIM(LOOK_SOON_MS) + (KODE_TOUCH_LOOK_FOR_MS / KODE_TOUCH_LOOK_EVERY_MS)) <= UINT8_MAX,
               "the timed looks are counted in a uint8_t");

/* The guided test's script: the board's two targets (kbsp_board.h). The test
 * checks the panel; it calibrates nothing. A tap off the target is a slip,
 * forgiven until the KODE_TOUCH_TEST_TRIES-th on the same target, which fails
 * the step. */
#define TEST_STEPS      KODE_TOUCH_TEST_STEPS   /**< Targets in the script.      */
#define TEST_MISS_MAX   KODE_TOUCH_TEST_TRIES   /**< Taps off one target that fail it. */
typedef struct {
    int16_t x;
    int16_t y;
} target_t;
static const target_t TARGETS[TEST_STEPS] = {
    { KBSP_TOUCH_TEST_A_X_PX, KBSP_TOUCH_TEST_A_Y_PX },
    { KBSP_TOUCH_TEST_B_X_PX, KBSP_TOUCH_TEST_B_Y_PX },
};

#if CONFIG_KODE_LOWPOWER
/** @brief The settings the rest changes, as the controller held them awake. */
typedef struct {
    int32_t irq_ctl;     /**< KDRV_CST_IRQ_CTL: what pulses INT.            */
    int32_t dis_sleep;   /**< KDRV_CST_DIS_AUTO_SLEEP.                      */
    int32_t sleep_time;  /**< KDRV_CST_AUTO_SLEEP_TIME.                     */
} touch_awake_t;
#endif

typedef struct {
    QActive      super;
    QTimeEvt     poll;
    QTimeEvt     test_tmo;   /**< One-shot: the guided test's step deadline. */
    bool         pressed;    /**< Last reported state.  */
    int16_t      x;          /**< Last published point. */
    int16_t      y;
    uint8_t      empty;      /**< Polls since the edge that found no finger. */
    int16_t      still_x;    /**< Where the contact settled: the centre of
                              *   the circle it has stayed in.          */
    int16_t      still_y;
    uint32_t     still_polls; /**< Polls it has stayed in that circle.   */

    /* The guided test, while one is running. */
    bool         test_on;      /**< A run is in progress.                    */
    uint8_t      test_origin;  /**< Who asked for it: TEST_ORIGIN_AUTOTEST or a client. */
    bool         test_failed;  /**< Some step so far was not a clean pass.   */
    uint8_t      test_miss;    /**< Taps off the target on this step so far. */
    uint8_t      test_step;    /**< Index into TARGETS.                      */
    int32_t      sum_x;        /**< The raw samples of this step's tap,      */
    int32_t      sum_y;        /**<   summed, and                            */
    uint16_t     n_samples;    /**<   how many.                              */

    /* The looks for a controller that did not answer the bring-up. */
    QTimeEvt     look;         /**< One-shot: the next timed look.           */
    bool         missing;      /**< The panel is up and no controller answered. */
    uint8_t      looks;        /**< Timed looks made since the bring-up's try. */
    uint16_t     asked;        /**< Every look since then, the interrupt's too. */
    uint32_t     missing_ms;   /**< khal_time_ms() of the bring-up's try.    */
    uint32_t     look_ms;      /**< khal_time_ms() of the last look.         */
#if CONFIG_KODE_LOWPOWER
    QTimeEvt      restore_tmo;  /**< One-shot: the next try at the awake settings. */
    uint8_t       lp_who;       /**< The id lowpower_join() gave.            */
    bool          awake_known;  /**< @c awake was read at bring-up.          */
    bool          rested;       /**< The controller holds the rest settings. */
    touch_awake_t awake;        /**< What the rest puts back.                */
#endif
} Touch;

static Touch l_touch;
QActive * const AO_Touch = &l_touch.super;

static QState Touch_initial(Touch * const me, void const * const par);
static QState Touch_idle(Touch * const me, QEvt const * const e);
static QState Touch_up(Touch * const me, QEvt const * const e);
static QState Touch_armed(Touch * const me, QEvt const * const e);
static QState Touch_tracking(Touch * const me, QEvt const * const e);
static QState Touch_held(Touch * const me, QEvt const * const e);

/* ---- Requests ------------------------------------------------------------ */

void touch_field_request(QActive *who, uint8_t op, uint16_t field, int32_t value)
{
    TouchFieldReqEvt *e = Q_NEW_X(TouchFieldReqEvt, 0U, TOUCH_FIELD_REQ_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; field request dropped");
        return;
    }
    e->who   = who;
    e->op    = op;
    e->field = field;
    e->value = value;
    QACTIVE_POST(AO_Touch, &e->super, NULL);
}

void touch_subscribe(QActive const *who)
{
    QActive_subscribe(who, TOUCH_EVT_SIG);
}

void touch_test_inject(int16_t x, int16_t y, bool pressed)
{
    TouchEvt *j = NULL;

    /* Only a running test has a judge to tell. A read of one byte the AO
     * writes: a test that starts or ends under it costs one event at most. */
    if ( l_touch.test_on )
    {
        j = Q_NEW_X(TouchEvt, 0U, TOUCH_INJECT_SIG);
    }
    if ( NULL != j )
    {
        j->x       = x;
        j->y       = y;
        j->pressed = pressed;
        j->t_ms    = khal_time_ms();
        j->stale   = false;
        /* A refused post gives the event back to its pool. */
        (void)QACTIVE_POST_X(AO_Touch, &j->super, 1U, NULL);
    }
}

void touch_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(TOUCH_TEST_REQ_SIG);
    QACTIVE_POST(AO_Touch, &evt, NULL);
}

void touch_test_request_from(uint8_t origin)
{
    TestReqEvt *ev = Q_NEW_X(TestReqEvt, 0U, TOUCH_TEST_REQ_FROM_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the touch test was not asked for");
        return;
    }
    ev->origin = origin;
    QACTIVE_POST(AO_Touch, &ev->super, NULL);
}

void touch_test_cancel_request(uint8_t origin)
{
    TestReqEvt *ev = Q_NEW_X(TestReqEvt, 0U, TOUCH_TEST_CANCEL_SIG);
    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; the touch test runs out by its own limits");
        return;
    }
    ev->origin = origin;
    QACTIVE_POST(AO_Touch, &ev->super, NULL);
}

/* ---- Construction -------------------------------------------------------- */

void Touch_ctor(void)
{
    Touch * const me = &l_touch;
    QActive_ctor(&me->super, Q_STATE_CAST(&Touch_initial));
    QTimeEvt_ctorX(&me->poll, &me->super, TOUCH_POLL_SIG, 0U);
    QTimeEvt_ctorX(&me->test_tmo, &me->super, TOUCH_TEST_TMO_SIG, 0U);
    me->pressed     = false;
    me->x           = 0;
    me->y           = 0;
    me->empty       = 0U;
    me->still_x     = 0;
    me->still_y     = 0;
    me->still_polls = 0U;
    me->test_on     = false;
    me->test_origin = TEST_ORIGIN_AUTOTEST;
    me->test_failed = false;
    me->test_miss   = 0U;
    me->test_step   = 0U;
    me->sum_x       = 0;
    me->sum_y       = 0;
    me->n_samples   = 0U;
    QTimeEvt_ctorX(&me->look, &me->super, TOUCH_LOOK_SIG, 0U);
    me->missing     = false;
    me->looks       = 0U;
    me->asked       = 0U;
    me->missing_ms  = 0U;
    me->look_ms     = 0U;
#if CONFIG_KODE_LOWPOWER
    QTimeEvt_ctorX(&me->restore_tmo, &me->super, TOUCH_RESTORE_SIG, 0U);
    me->lp_who      = KODE_LOWPOWER_WHO_NONE;
    me->awake_known = false;
    me->rested      = false;
    me->awake       = (touch_awake_t){ 0, 0, 0 };
#endif
}

AO_STORAGE(touch, KQP_AO_QUEUE_LEN, KQP_AO_STACK_MEDIUM);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_touch_register() ---------------- */

/**
 * @brief The touch panel's row: AO_Touch judges it and answers TEST_RESULT_SIG.
 *
 * AO_Touch shows one target at a time and judges each tap; a tap off the
 * target is asked again. The steps arrive as TOUCH_TEST_SIG.
 *
 * @return false: the answer comes back as TEST_RESULT_SIG.
 */
static bool touch_row_tp(void)
{
    touch_test_request();
    return false;
}
#endif

void kaos_touch_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t TOUCH_ROWS[] = {
        { "tp", AUTOTEST_ID_TP, touch_row_tp, KROW_ASYNC, AUTOTEST_G_NONE, KODE_TOUCH_TEST_BUDGET_MS,
          NULL, 0U, "Touch",
          "Touch not working",
          AUTOTEST_ORDER_TP },
    };

    kaos_require(krows_add(TOUCH_ROWS, sizeof(TOUCH_ROWS) / sizeof(TOUCH_ROWS[0])),
                 "touch rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&touch_verbs), "touch verbs");
#endif
}

void kaos_touch_start(void)
{
    Touch_ctor();
    kaos_touch_register();
    AO_START(AO_Touch, touch, KODE_PRIO_TOUCH);
}

/**
 * @brief Claim the touch panel, subscribe to the display and the expander, and go idle.
 *
 * Must not ask AO_Display for its state: AO_Display has no queue yet when this runs.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Touch_idle.
 */
static QState Touch_initial(Touch * const me, void const * const par)
{
    (void)par;
    kaos_claim(KBSP_RES_TOUCH, me, "AO_Touch");
    QActive_subscribe(&me->super, DISPLAY_STATE_SIG);
    QActive_subscribe(&me->super, IOEXP_INPUT_SIG);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "touch");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
    }
#endif
    /* Do not ask AO_Display for its state here: it starts after this AO and
     * has no queue yet, so the post would assert and boot-loop. Its
     * announcement is still ahead, and subscribing is enough. */
    return Q_TRAN(&Touch_idle);
}

/* ---- Publishing and answering ------------------------------------------- */

/**
 * @brief Pin a controller coordinate to the glass.
 *
 * The controller counts in twelve bits and the panel is 410x502, so a finger
 * on the bezel can read past the last pixel. Nobody downstream is told about
 * a point that is not on the glass.
 *
 * @param v     The controller's coordinate on one axis.
 * @param limit The panel's size on that axis, in pixels.
 * @return @p v within 0..@p limit - 1.
 */
static int16_t clamp_axis(int16_t v, uint32_t limit)
{
    int16_t out = v;

    if ( 0 > v )
    {
        out = 0;
    }
    else if ( (uint32_t)v >= limit )
    {
        out = (int16_t)(limit - 1U);
    }
    else
    {
        /* On the glass: as read. */
    }
    return out;
}

/**
 * @brief Publish a sample. Bounded: on a dry pool nothing goes out, and the
 *        caller leaves its state as it was so the next poll says it again.
 * @param x       The point's x, on the glass.
 * @param y       The point's y, on the glass.
 * @param pressed Whether a finger is down.
 * @param stale   A release for a contact that stopped counting (TouchEvt).
 * @return true when the sample went out.
 */
static bool touch_publish(int16_t x, int16_t y, bool pressed, bool stale)
{
    TouchEvt *ev = Q_NEW_X(TouchEvt, 0U, TOUCH_EVT_SIG);
    if ( NULL == ev )
    {
        return false;
    }
    ev->x       = x;
    ev->y       = y;
    ev->pressed = pressed;
    ev->t_ms    = khal_time_ms();
    ev->stale   = stale;
    QACTIVE_PUBLISH(&ev->super, AO_Touch);
    return true;
}

/**
 * @brief Whether @p x, @p y has left the circle the contact settled in; when
 *        it has, the circle moves there and the count starts again.
 * @param me The AO.
 * @param x  The sample's x, on the glass.
 * @param y  The sample's y, on the glass.
 * @return true when the contact moved off.
 */
static bool touch_still_moved(Touch * const me, int16_t x, int16_t y)
{
    int const dx = (int)x - (int)me->still_x;
    int const dy = (int)y - (int)me->still_y;
    bool const moved = (KODE_TOUCH_STILL_PX < dx) || (-KODE_TOUCH_STILL_PX > dx)
                       || (KODE_TOUCH_STILL_PX < dy) || (-KODE_TOUCH_STILL_PX > dy);

    if ( moved )
    {
        me->still_x     = x;
        me->still_y     = y;
        me->still_polls = 0U;
    }
    return moved;
}

/** @brief Answer a field request to whoever asked. Bounded, never asserts. */
static void field_answer(TouchFieldReqEvt const *q, kode_status_t rc, int32_t value)
{
    if ( NULL == q->who )
    {
        return;
    }
    TouchFieldEvt *ev = Q_NEW_X(TouchFieldEvt, 0U, TOUCH_FIELD_RSP_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; a field answer is lost");
        return;
    }
    ev->op    = q->op;
    ev->field = q->field;
    ev->rc    = (uint8_t)rc;
    ev->value = value;
    QACTIVE_POST(q->who, &ev->super, NULL);
}

/** @brief One field of the chip, as asked. SET is followed by a GET so the
 *  answer carries what the chip kept. */
static kode_status_t field_do(TouchFieldReqEvt const *q, int32_t *value)
{
    kdrv_cst820_field_t const f = (kdrv_cst820_field_t)q->field;
    kode_status_t rc = KODE_DENIED;

    switch ( q->op )
    {
        case FIELD_OP_GET: {
            rc = kdrv_cst820_get(f, value);
            break;
        }
        case FIELD_OP_SET: {
            rc = kdrv_cst820_set(f, *value);
            if ( KODE_OK == rc )
            {
                rc = kdrv_cst820_get(f, value);
            }
            break;
        }
        case FIELD_OP_DUMP: {
            kdrv_cst820_dump();
            rc = KODE_OK;
            break;
        }
        default: {
            break;
        }
    }
    return rc;
}

/** @brief Did the expander's owner just see the controller's IRQ fall? */
static bool irq_fell(IoexpInputEvt const *in)
{
    return in->ok
        && ((in->changed & TP_INT_BIT) != 0U)
        && ((in->levels & TP_INT_BIT) == 0U);
}

/* ---- The guided test ------------------------------------------------------ */

/** @brief Publish one step of the test. Bounded, never asserts. */
static void test_publish(Touch * const me, uint8_t phase, int16_t gx, int16_t gy)
{
    TouchTestEvt *ev = Q_NEW_X(TouchTestEvt, 0U, TOUCH_TEST_SIG);
    if ( NULL == ev )
    {
        return;
    }
    ev->phase  = phase;
    ev->step   = (uint8_t)(me->test_step + 1U);
    ev->total  = TEST_STEPS;
    ev->tx     = TARGETS[me->test_step].x;
    ev->ty     = TARGETS[me->test_step].y;
    ev->gx     = gx;
    ev->gy     = gy;
    ev->origin = me->test_origin;
    QACTIVE_PUBLISH(&ev->super, &me->super);
}

/**
 * @brief Publish a verdict. Bounded, never asserts.
 *
 * @param me     The AO.
 * @param ok     The verdict.
 * @param origin Whose it is: the run's own, or the origin of a request that
 *               was refused while another run was up.
 */
static void test_verdict(Touch * const me, bool ok, uint8_t origin)
{
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL != ev )
    {
        (void)snprintf(ev->name, sizeof(ev->name), "%s", "tp");
        ev->ok = ok;
        ev->origin = origin;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

/** @brief Show the current step's target and start its clock. */
static void test_prompt(Touch * const me)
{
    me->sum_x     = 0;
    me->sum_y     = 0;
    me->n_samples = 0U;
    KLOG_N(TAG, "test: step %u of %u - tap the target at (%d,%d)",
           (unsigned)(me->test_step + 1U), (unsigned)TEST_STEPS,
           (int)TARGETS[me->test_step].x, (int)TARGETS[me->test_step].y);
    test_publish(me, TP_TEST_PROMPT, 0, 0);
    (void)QTimeEvt_disarm(&me->test_tmo);
    QTimeEvt_armX(&me->test_tmo, KQP_TICKS_PER_MS(KODE_TOUCH_TEST_STEP_MS), 0U);
}

/**
 * @brief Every step is over: the verdict. Nothing is fitted or stored; the
 *        test proves the panel answers, it does not calibrate it.
 */
static void test_finish(Touch * const me)
{
    bool const ok = !me->test_failed;

    if ( ok )
    {
        KLOG_N(TAG, "test: both targets were tapped");
    }
    else
    {
        KLOG_E(TAG, "test: FAILED - see the steps above");
    }
    test_publish(me, TP_TEST_DONE, 0, 0);
    me->test_on = false;
    test_verdict(me, ok, me->test_origin);
}

/** @brief This step is over: the next one, or the end. */
static void test_advance(Touch * const me)
{
    (void)QTimeEvt_disarm(&me->test_tmo);
    if ( (uint8_t)(me->test_step + 1U) >= TEST_STEPS )
    {
        test_finish(me);
        return;
    }
    ++me->test_step;
    me->test_miss = 0U;
    test_prompt(me);
}

/**
 * @brief The tap has lifted: judge its mean against the target.
 *
 * The mean of every sample while the finger was down, rather than the first
 * or the last, because a finger lands and rolls. A tap off the target is a
 * miss and the same target is asked again; the TEST_MISS_MAX-th on a step
 * fails it.
 */
static void test_judge(Touch * const me)
{
    int16_t const gx = (int16_t)(me->sum_x / (int32_t)me->n_samples);
    int16_t const gy = (int16_t)(me->sum_y / (int32_t)me->n_samples);
    int32_t dx = (int32_t)gx - (int32_t)TARGETS[me->test_step].x;
    int32_t dy = (int32_t)gy - (int32_t)TARGETS[me->test_step].y;

    if ( 0 > dx )
    {
        dx = -dx;
    }
    if ( 0 > dy )
    {
        dy = -dy;
    }
    if ( ((int32_t)KODE_TOUCH_TEST_TOL_PX >= dx) && ((int32_t)KODE_TOUCH_TEST_TOL_PX >= dy) )
    {
        KLOG_N(TAG, "test: tap read (%d,%d), target (%d,%d): ok",
               (int)gx, (int)gy, (int)TARGETS[me->test_step].x,
               (int)TARGETS[me->test_step].y);
        test_publish(me, TP_TEST_PASS, gx, gy);
        test_advance(me);
    }
    else if ( (uint8_t)(me->test_miss + 1U) < TEST_MISS_MAX )
    {
        ++me->test_miss;
        KLOG_W(TAG, "test: tap read (%d,%d), target (%d,%d): off by (%ld,%ld), "
                    "more than %u px; the same target again",
               (int)gx, (int)gy, (int)TARGETS[me->test_step].x,
               (int)TARGETS[me->test_step].y, (long)dx, (long)dy,
               (unsigned)KODE_TOUCH_TEST_TOL_PX);
        test_publish(me, TP_TEST_MISS, gx, gy);
        test_prompt(me);
    }
    else
    {
        KLOG_E(TAG, "test: tap read (%d,%d), target (%d,%d): off by (%ld,%ld), "
                    "more than %u px, %u times",
               (int)gx, (int)gy, (int)TARGETS[me->test_step].x,
               (int)TARGETS[me->test_step].y, (long)dx, (long)dy,
               (unsigned)KODE_TOUCH_TEST_TOL_PX, (unsigned)TEST_MISS_MAX);
        me->test_failed = true;
        test_publish(me, TP_TEST_FAIL, gx, gy);
        test_advance(me);
    }
}

/**
 * @brief `test component=tp` was asked for, with a controller up.
 *
 * @param me     The AO.
 * @param origin Who asked: stamped on every step of the run it starts.
 */
static void test_start(Touch * const me, uint8_t origin)
{
    uint8_t chip_id = 0U;
    uint8_t proj_id = 0U;
    uint8_t fw_ver  = 0U;

    if ( me->test_on )
    {
        /* Answered, in the asker's own number: its wait for a verdict ends,
         * and a refused test never counts as a pass. The run in flight
         * keeps its origin. */
        KLOG_W(TAG, "test: busy with a run from origin %u; the request from "
                    "origin %u is refused", (unsigned)me->test_origin,
               (unsigned)origin);
        test_verdict(me, false, origin);
        return;
    }
    me->test_on     = true;
    me->test_origin = origin;
    me->test_failed = false;
    me->test_miss   = 0U;
    me->test_step   = 0U;

    /* From the bring-up cache: the chip may be asleep right now, and a NAK
     * must not be able to fail an identity that was read fine at boot. */
    if ( kdrv_cst820_identity(&chip_id, &proj_id, &fw_ver) != KODE_OK )
    {
        KLOG_E(TAG, "test: no identity was read at bring-up");
        me->test_failed = true;
    }
    else if ( KDRV_CST820_CHIP_ID != chip_id )
    {
        KLOG_E(TAG, "test: chip_id=0x%02X, a CST820 reads 0x%02X (proj_id=0x%02X fw_ver=0x%02X)",
               (unsigned)chip_id, (unsigned)KDRV_CST820_CHIP_ID, (unsigned)proj_id,
               (unsigned)fw_ver);
        me->test_failed = true;
    }
    else
    {
        KLOG_N(TAG, "test: chip_id=0x%02X proj_id=0x%02X fw_ver=0x%02X: a CST820",
               (unsigned)chip_id, (unsigned)proj_id, (unsigned)fw_ver);
    }
    test_prompt(me);
}

#if CONFIG_KODE_LOWPOWER
/* ---- The rest ------------------------------------------------------------- */

/**
 * @brief Read the settings the rest changes, while the chip is awake.
 *
 * Called right after the bring-up, the one moment the chip is known to answer.
 * Without all three the rest leaves the chip alone: it sleeps by itself
 * eventually, and a rest that cannot be undone is worse than none.
 *
 * @param me The AO.
 */
static void awake_read(Touch * const me)
{
    me->awake_known =
        (kdrv_cst820_get(KDRV_CST_IRQ_CTL, &me->awake.irq_ctl) == KODE_OK)
        && (kdrv_cst820_get(KDRV_CST_DIS_AUTO_SLEEP, &me->awake.dis_sleep) == KODE_OK)
        && (kdrv_cst820_get(KDRV_CST_AUTO_SLEEP_TIME, &me->awake.sleep_time) == KODE_OK);
    if ( !me->awake_known )
    {
        KLOG_W(TAG, "the awake settings did not read; the chip is not put to rest");
    }
}

/**
 * @brief Put the chip to rest: no interrupt, auto-sleep on and quick.
 *
 * Each write may NAK if the chip has already gone to sleep, which is the rest
 * itself, so the results are not judged.
 *
 * @param me The AO.
 */
static void touch_rest(Touch * const me)
{
    if ( me->awake_known )
    {
        (void)kdrv_cst820_set(KDRV_CST_IRQ_CTL, 0);
        (void)kdrv_cst820_set(KDRV_CST_DIS_AUTO_SLEEP, 0);
        (void)kdrv_cst820_set(KDRV_CST_AUTO_SLEEP_TIME, (int32_t)KODE_TOUCH_REST_SLEEP_S);
        me->rested = true;
    }
}

/**
 * @brief Write the awake settings back; try again soon if the chip is asleep.
 *
 * Idempotent: the writes are the same every time, and the retry timer is
 * disarmed before it is armed, so a stale expiry cannot arm it twice.
 *
 * @param me The AO.
 */
static void touch_wake(Touch * const me)
{
    bool const ok =
        (kdrv_cst820_set(KDRV_CST_DIS_AUTO_SLEEP, me->awake.dis_sleep) == KODE_OK)
        && (kdrv_cst820_set(KDRV_CST_AUTO_SLEEP_TIME, me->awake.sleep_time) == KODE_OK)
        && (kdrv_cst820_set(KDRV_CST_IRQ_CTL, me->awake.irq_ctl) == KODE_OK);

    (void)QTimeEvt_disarm(&me->restore_tmo);
    if ( ok )
    {
        me->rested = false;
        KLOG_D(TAG, "touch is back");
    }
    else
    {
        QTimeEvt_armX(&me->restore_tmo,
                      KQP_TICKS_PER_MS(KODE_TOUCH_RESTORE_RETRY_MS), 0U);
    }
}

/**
 * @brief LOWPOWER_ROUND_SIG: rest, and answer once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void touch_round(Touch * const me, const LowPowerRoundEvt * const r)
{
    if ( me->test_on )
    {
        lowpower_ready(me->lp_who, r->round, true, "touch test");
    }
    else
    {
        /* A wake still being retried is cancelled: the chip is still in its
         * rest settings, which is where this round wants it. */
        (void)QTimeEvt_disarm(&me->restore_tmo);
        if ( !me->rested )
        {
            touch_rest(me);
        }
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}

/**
 * @brief LOWPOWER_STATE_SIG: on AWAKE, put the chip back if it was rested.
 *
 * @param me The AO.
 * @param s  The change.
 */
static void touch_state(Touch * const me, const LowPowerStateEvt * const s)
{
    if ( ((uint8_t)KODE_LP_AWAKE == s->state) && me->rested )
    {
        touch_wake(me);
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/* ---- A controller that did not answer the bring-up ------------------------- */

/**
 * @brief Arm the next timed look, if any is left.
 *
 * The first ones come soon, the rest spaced (kqp_config.h); past the last
 * nothing is armed and only the interrupt line asks for a look.
 *
 * @param me The AO.
 */
static void look_arm(Touch * const me)
{
    uint32_t wait_ms = KODE_TOUCH_LOOK_EVERY_MS;

    if ( LOOK_TIMED <= me->looks )
    {
        return;
    }
    if ( LOOK_SOON > me->looks )
    {
        wait_ms = LOOK_SOON_MS[me->looks];
    }
    (void)QTimeEvt_disarm(&me->look);
    QTimeEvt_armX(&me->look, KQP_TICKS_PER_MS(wait_ms), 0U);
}

/**
 * @brief The bring-up's try found no controller: start looking for it.
 *
 * @param me The AO.
 */
static void look_begin(Touch * const me)
{
    me->missing    = true;
    me->looks      = 0U;
    me->asked      = 0U;
    me->missing_ms = khal_time_ms();
    me->look_ms    = me->missing_ms;
    look_arm(me);
}

/**
 * @brief Stop looking: the controller is up, or there is no panel to carry one.
 *
 * @param me The AO.
 */
static void look_end(Touch * const me)
{
    (void)QTimeEvt_disarm(&me->look);
    me->missing = false;
}

/**
 * @brief Look once for the controller, and bring it up if it answers.
 *
 * One short probe; the bring-up, with its identity check, only when something
 * answered. Says at which look it answered and how long after the bring-up's
 * try: the measurement of how late a controller can be.
 *
 * @param me The AO.
 * @return true when the controller is up.
 */
static bool look_once(Touch * const me)
{
    bool up = false;

    me->look_ms = khal_time_ms();
    if ( UINT16_MAX > me->asked )
    {
        me->asked++;
    }
    if ( kdrv_cst820_answers() && (kdrv_cst820_init() == KODE_OK) )
    {
        look_end(me);
#if CONFIG_KODE_LOWPOWER
        awake_read(me);
#endif
        KLOG_N(TAG, "the CST820 answered at look %u, %lu ms after the bring-up's try; "
                    "armed on the expander's P6",
               (unsigned)me->asked, (unsigned long)(me->look_ms - me->missing_ms));
        up = true;
    }
    return up;
}

/* ---- States -------------------------------------------------------------- */

/** Waiting for the panel: the shared reset makes any earlier init pointless. */
static QState Touch_idle(Touch * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case DISPLAY_STATE_SIG: {
            if ( !Q_EVT_CAST(DisplayStateEvt)->up )
            {
                KLOG_W(TAG, "no panel; touch shares its reset line, so no touch");
                look_end(me);
                status = Q_HANDLED();
            }
            else if ( me->missing )
            {
                /* The panel said again that it is up: one more look, not a
                 * second bring-up's worth of probes. */
                status = Q_HANDLED();
                if ( look_once(me) )
                {
                    status = Q_TRAN(&Touch_armed);
                }
            }
            else if ( kdrv_cst820_init() != KODE_OK )
            {
                /* The panel's bring-up has just pulsed the reset line the two
                 * share: the controller may still be coming back. */
                KLOG_W(TAG, "no CST820 at 0x%02X yet; looking again",
                       (unsigned)KBSP_I2C_ADDR_TOUCH);
                look_begin(me);
                status = Q_HANDLED();
            }
            else
            {
#if CONFIG_KODE_LOWPOWER
                awake_read(me);
#endif
                KLOG_I(TAG, "up; armed on the expander's P6, polling every %ums "
                            "while a finger is down", (unsigned)KODE_TOUCH_POLL_MS);
                status = Q_TRAN(&Touch_armed);
            }
            break;
        }
        case TOUCH_LOOK_SIG: {
            status = Q_HANDLED();
            if ( me->missing )
            {
                me->looks++;
                if ( look_once(me) )
                {
                    status = Q_TRAN(&Touch_armed);
                }
                else if ( LOOK_TIMED <= me->looks )
                {
                    KLOG_W(TAG, "no CST820 in %u looks over %u s; from here it is looked "
                                "for only when the glass is touched",
                           (unsigned)me->asked,
                           (unsigned)((khal_time_ms() - me->missing_ms) / TOUCH_MS_PER_S));
                }
                else
                {
                    look_arm(me);
                }
            }
            break;
        }
        case IOEXP_INPUT_SIG: {
            /* A controller in its low-power scan answers nothing until a
             * finger touches the glass, and this line is how it says so. */
            status = Q_HANDLED();
            if ( me->missing && irq_fell(Q_EVT_CAST(IoexpInputEvt))
                && (KODE_TOUCH_LOOK_EDGE_MS <= (khal_time_ms() - me->look_ms)) )
            {
                if ( look_once(me) )
                {
                    status = Q_TRAN(&Touch_armed);
                }
            }
            break;
        }
        case TOUCH_FIELD_REQ_SIG: {
            /* Nothing to serve it from. A refusal rather than silence: the
             * console's watchdog would otherwise be the only thing to say so. */
            TouchFieldReqEvt const * const q = Q_EVT_CAST(TouchFieldReqEvt);
            field_answer(q, KODE_DENIED, q->value);
            status = Q_HANDLED();
            break;
        }
        case TOUCH_TEST_REQ_SIG:
        case TOUCH_TEST_REQ_FROM_SIG: {
            me->test_origin = TEST_ORIGIN_AUTOTEST;
            if ( (enum_t)TOUCH_TEST_REQ_FROM_SIG == e->sig )
            {
                me->test_origin = Q_EVT_CAST(TestReqEvt)->origin;
            }
            KLOG_E(TAG, "test: no CST820 came up; nothing to tap");
            test_verdict(me, false, me->test_origin);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* No controller: nothing to rest. The timed looks stop with the
             * board, which is not woken to look for a chip. */
            (void)QTimeEvt_disarm(&me->look);
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           false, NULL);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            if ( ((uint8_t)KODE_LP_AWAKE == Q_EVT_CAST(LowPowerStateEvt)->state) && me->missing )
            {
                look_arm(me);
            }
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

/** The controller is there. What armed and tracking have in common: the
 *  requests that do not care whether a finger is down. */
static QState Touch_up(Touch * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case TOUCH_FIELD_REQ_SIG: {
            TouchFieldReqEvt const * const q = Q_EVT_CAST(TouchFieldReqEvt);
            int32_t value = q->value;
            kode_status_t const rc = field_do(q, &value);
            field_answer(q, rc, value);
            status = Q_HANDLED();
            break;
        }
        case TOUCH_TEST_REQ_SIG:
        case TOUCH_TEST_REQ_FROM_SIG: {
            uint8_t origin = TEST_ORIGIN_AUTOTEST;

            if ( (enum_t)TOUCH_TEST_REQ_FROM_SIG == e->sig )
            {
                origin = Q_EVT_CAST(TestReqEvt)->origin;
            }
            test_start(me, origin);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            touch_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            touch_state(me, Q_EVT_CAST(LowPowerStateEvt));
            status = Q_HANDLED();
            break;
        }
        case TOUCH_RESTORE_SIG: {
            if ( me->rested )
            {
                touch_wake(me);
            }
            status = Q_HANDLED();
            break;
        }
#endif
        case TOUCH_TEST_CANCEL_SIG: {
            if ( me->test_on && (me->test_origin == Q_EVT_CAST(TestReqEvt)->origin) )
            {
                /* Its asker is gone: no more prompts, and no verdict. */
                (void)QTimeEvt_disarm(&me->test_tmo);
                me->test_on = false;
                KLOG_N(TAG, "test: cancelled by origin %u", (unsigned)me->test_origin);
            }
            status = Q_HANDLED();
            break;
        }
        case TOUCH_INJECT_SIG: {
            if ( me->test_on )
            {
                TouchEvt const * const j = Q_EVT_CAST(TouchEvt);

                if ( j->pressed )
                {
                    me->sum_x += (int32_t)j->x;
                    me->sum_y += (int32_t)j->y;
                    ++me->n_samples;
                }
                else if ( 0U < me->n_samples )
                {
                    test_judge(me);     /* the injected tap lifted */
                }
                else
                {
                    /* A lift with no press before it. */
                }
            }
            status = Q_HANDLED();
            break;
        }
        case TOUCH_TEST_TMO_SIG: {
            if ( me->test_on )
            {
                KLOG_E(TAG, "test: nobody tapped (%d,%d) in %u ms",
                       (int)TARGETS[me->test_step].x, (int)TARGETS[me->test_step].y,
                       (unsigned)KODE_TOUCH_TEST_STEP_MS);
                me->test_failed = true;
                test_publish(me, TP_TEST_TIMEOUT, 0, 0);
                test_advance(me);
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

/** Nothing is read here. The controller's IRQ falling is what starts a poll. */
static QState Touch_armed(Touch * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case IOEXP_INPUT_SIG: {
            if ( irq_fell(Q_EVT_CAST(IoexpInputEvt)) )
            {
                KLOG_I(TAG, "edge: P6 fell, polling starts at t=%lums",
                       (unsigned long)khal_time_ms());
                me->empty = 0U;
                status = Q_TRAN(&Touch_tracking);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&Touch_up);
            break;
        }
    }
    return status;
}

/**
 * @brief A finger is, or is about to be, on the glass: poll until it has gone.
 *
 * The poll is a one-shot that each poll taken arms again, NOT a periodic
 * timer. QP posts a time event with no margin, and a periodic one goes on
 * posting whether or not this AO has run: sixteen periods without the CPU
 * fill its queue, a sixth of a second at this period, and a full queue is an
 * assertion and a reset. Armed again by the poll itself, one waits at most.
 */
static QState Touch_tracking(Touch * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            QTimeEvt_armX(&me->poll, KQP_TICKS_PER_MS(KODE_TOUCH_POLL_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->poll);
            status = Q_HANDLED();
            break;
        }
        case TOUCH_POLL_SIG: {
            bool    pressed = false;
            int16_t rx = 0;
            int16_t ry = 0;

            /* The next poll, a period from this one. A transition below
             * leaves the state, and the exit takes it back. */
            (void)QTimeEvt_rearm(&me->poll, KQP_TICKS_PER_MS(KODE_TOUCH_POLL_MS));

            /* A chip that did not answer ends the touch: a release goes out
             * with the last position rather than leave the GUI pressed. */
            if ( kdrv_cst820_read(&pressed, &rx, &ry) != KODE_OK )
            {
                pressed = false;
            }

            status = Q_HANDLED();
            if ( pressed )
            {
                int16_t const x = clamp_axis(rx, KBSP_LCD_H_RES);
                int16_t const y = clamp_axis(ry, KBSP_LCD_V_RES);
                if ( me->test_on )
                {
                    me->sum_x += rx;
                    me->sum_y += ry;
                    ++me->n_samples;
                }
                /* Hysteresis against the last PUBLISHED position, not the last
                 * sample: a slow drag would otherwise stay under epsilon at
                 * every step and never be published. Epsilon is for jitter. */
                int const dx = (int)x - (int)me->x;
                int const dy = (int)y - (int)me->y;
                bool const moved = ((TOUCH_MOVE_EPSILON < dx) || (-TOUCH_MOVE_EPSILON > dx)
                                 || (TOUCH_MOVE_EPSILON < dy) || (-TOUCH_MOVE_EPSILON > dy));
                /* The state moves only with what went out: a press or a
                 * move the pool could not carry is still news at the next
                 * poll. */
                if ( !me->pressed || moved )
                {
                    if ( touch_publish(x, y, true, false) )
                    {
                        if ( !me->pressed )
                        {
                            me->still_x     = x;   /* where it lands */
                            me->still_y     = y;
                            me->still_polls = 0U;
                        }
                        me->x = x;
                        me->y = y;
                        me->pressed = true;
                    }
                }
                if ( me->pressed )
                {
                    /* The circle follows the contact even during the guided
                     * test; only the count waits for the test to end. */
                    bool const left = touch_still_moved(me, x, y);
                    if ( (!left) && (!me->test_on) )
                    {
                        ++me->still_polls;
                        /* Still for a minute: not a finger any more. The
                         * release is what lets everybody downstream stop
                         * waiting for one. */
                        if ( TOUCH_STILL_POLLS <= me->still_polls )
                        {
                            if ( touch_publish(me->x, me->y, false, true) )
                            {
                                uint32_t const still_s = KODE_TOUCH_STILL_MS / TOUCH_MS_PER_S;
                                me->pressed = false;
                                KLOG_N(TAG, "a contact still for %u s at (%d,%d) is "
                                            "no longer input", (unsigned)still_s,
                                       (int)me->x, (int)me->y);
                                status = Q_TRAN(&Touch_held);
                            }
                        }
                    }
                }
            }
            else if ( me->pressed )
            {
                /* The last coordinates. Until the release goes out the poll
                 * goes on, or downstream the finger would never lift. */
                if ( touch_publish(me->x, me->y, false, false) )
                {
                    me->pressed = false;
                    if ( me->test_on && (0U < me->n_samples) )
                    {
                        test_judge(me);     /* the tap is what lifted */
                    }
                    status = Q_TRAN(&Touch_armed);
                }
            }
            else
            {
                /* An edge with nothing behind it. A few looks, then back to
                 * waiting - a glitch must not start a poll that never ends. */
                ++me->empty;
                if ( (uint8_t)TOUCH_EMPTY_POLLS <= me->empty )
                {
                    KLOG_D(TAG, "edge with nothing behind it; back to armed at "
                                "t=%lums (no publish)", (unsigned long)khal_time_ms());
                    status = Q_TRAN(&Touch_armed);
                }
            }
            break;
        }
        case IOEXP_INPUT_SIG: {
            /* Already polling; the line's edges add nothing while we look. */
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* A finger is on the glass: a touch is input, not a rest. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "finger down");
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&Touch_up);
            break;
        }
    }
    return status;
}

/**
 * @brief A contact that stopped counting: looked at once a second until it
 *        lifts or moves off, with nothing published and no round refused.
 */
static QState Touch_held(Touch * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            /* One-shot, as Touch_tracking's: each look arms the next. */
            QTimeEvt_armX(&me->poll, KQP_TICKS_PER_MS(KODE_TOUCH_HELD_POLL_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->poll);
            status = Q_HANDLED();
            break;
        }
        case TOUCH_POLL_SIG: {
            bool    pressed = false;
            int16_t rx = 0;
            int16_t ry = 0;

            (void)QTimeEvt_rearm(&me->poll, KQP_TICKS_PER_MS(KODE_TOUCH_HELD_POLL_MS));
            if ( kdrv_cst820_read(&pressed, &rx, &ry) != KODE_OK )
            {
                pressed = false;
            }
            status = Q_HANDLED();
            if ( (!pressed) || me->test_on )
            {
                /* Gone, or the guided test wants the next tap: an edge
                 * starts the next touch as ever. */
                status = Q_TRAN(&Touch_armed);
            }
            else
            {
                int16_t const x = clamp_axis(rx, KBSP_LCD_H_RES);
                int16_t const y = clamp_axis(ry, KBSP_LCD_V_RES);

                if ( touch_still_moved(me, x, y) && touch_publish(x, y, true, false) )
                {
                    /* Moved off: somebody is there after all. */
                    me->x       = x;
                    me->y       = y;
                    me->pressed = true;
                    status = Q_TRAN(&Touch_tracking);
                }
            }
            break;
        }
        case IOEXP_INPUT_SIG: {
            status = Q_HANDLED();   /* the poll is what looks */
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* Not a finger: the controller rests as with none, and its next
             * edge after the wake starts a touch again. */
            touch_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_TRAN(&Touch_armed);
            break;
        }
#endif
        default: {
            status = Q_SUPER(&Touch_up);
            break;
        }
    }
    return status;
}
