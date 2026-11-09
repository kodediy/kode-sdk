/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file led_mgr.c
 * @brief LedMgr: the Active Object that owns the LED.
 */
#include "sdkconfig.h"
#include "kaos.h"
#include "kaos_start.h"

#include <stdio.h>

#include "kqp.h"
#include "kaos_claim.h"
#include "kdrv_led.h"
#include "kdrv_buttons.h"   /* KDRV_BTN_TOP and KDRV_BTN_BOT: the row's answers */
#include "kode_log.h"
#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.ledmgr"

/** @brief The three colours as the driver takes them, by LED_COLOUR_*. */
static const uint8_t TEST_RGB[LED_COLOUR_COUNT][3] = {
    { KODE_LED_TEST_LEVEL, 0U, 0U },   /* LED_COLOUR_RED   */
    { 0U, KODE_LED_TEST_LEVEL, 0U },   /* LED_COLOUR_GREEN */
    { 0U, 0U, KODE_LED_TEST_LEVEL }    /* LED_COLOUR_BLUE  */
};

/** @brief The LED owner AO, its heartbeat state and the row's. */
typedef struct {
    QActive     super;
    QTimeEvt    heartbeat;         /**< Private periodic blink timer.        */
    QTimeEvtCtr blink_ticks;       /**< Current heartbeat period, in ticks.  */
    bool        heartbeat_enabled; /**< Set once a caller asked for blinking. */
    /* The `led` row. A substate of active that takes the LED away from
     * everybody, this AO's own heartbeat included, for as long as somebody
     * is being asked about it. */
    uint8_t     colour;            /**< LED_COLOUR_* being lit and asked about. */
    QTimeEvt    test_wait;         /**< LED_TEST_WAIT_SIG: the operator's
                                    *   deadline for this colour.            */
    bool        test_wait_live;    /**< An arming is outstanding.            */
    uint32_t    test_wait_stale;   /**< Expiries queued for a colour that was
                                    *   already answered.                    */
#if CONFIG_KODE_LOWPOWER
    uint8_t     lp_who;            /**< The id lowpower_join() gave.          */
    bool        rested;            /**< The board's rest has the LED dark and
                                    *   the heartbeat down.                   */
#endif
} LedMgr;

static LedMgr l_ledMgr;
QActive * const AO_LedMgr = &l_ledMgr.super;

static QState LedMgr_initial(LedMgr * const me, void const * const par);
static QState LedMgr_active(LedMgr * const me, QEvt const * const e);
static QState LedMgr_testing(LedMgr * const me, QEvt const * const e);

/** @brief Construct LedMgr. Start it afterwards with QACTIVE_START. */
void LedMgr_ctor(void)
{
    LedMgr * const me = &l_ledMgr;
    QActive_ctor(&me->super, Q_STATE_CAST(&LedMgr_initial));
    QTimeEvt_ctorX(&me->heartbeat, &me->super, LED_HEARTBEAT_SIG, 0U);
    me->blink_ticks       = KQP_TICKS_PER_MS(KODE_LED_BLINK_MS);
    me->heartbeat_enabled = false;
    me->colour            = (uint8_t)LED_COLOUR_RED;
    QTimeEvt_ctorX(&me->test_wait, &me->super, LED_TEST_WAIT_SIG, 0U);
    me->test_wait_live  = false;
    me->test_wait_stale = 0U;
#if CONFIG_KODE_LOWPOWER
    me->lp_who          = KODE_LOWPOWER_WHO_NONE;
    me->rested          = false;
#endif
}

AO_STORAGE(ledmgr, KQP_AO_QUEUE_LEN, KQP_AO_STACK_SMALL);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_ledmgr_register() ---------------- */

/** @brief The LED's row: AO_LedMgr runs it, the operator answers it on the
 *         pad, and the verdict comes back as TEST_RESULT_SIG. */
static bool ledmgr_row_led(void)
{
    led_test_request();
    return false;   /* answered later, with TEST_RESULT_SIG */
}
#endif

void kaos_ledmgr_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t LED_ROWS[] = {
        { "led", AUTOTEST_ID_LED, ledmgr_row_led, KROW_ASYNC, AUTOTEST_G_NONE, KODE_LED_TEST_BUDGET_MS,
          NULL, 0U, "Light",
          "Light not working",
          AUTOTEST_ORDER_LED },
    };

    kaos_require(krows_add(LED_ROWS, sizeof(LED_ROWS) / sizeof(LED_ROWS[0])),
                 "ledmgr rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&led_verbs), "ledmgr verbs");
#endif
}

void kaos_ledmgr_start(void)
{
    LedMgr_ctor();
    kaos_ledmgr_register();
    AO_START(AO_LedMgr, ledmgr, KODE_PRIO_LED);
}

void led_test_request(void)
{
    static const QEvt req = QEVT_INITIALIZER(LED_TEST_REQ_SIG);
    QACTIVE_POST(AO_LedMgr, &req, (void *)0);
}

/* ---- The `led` row --------------------------------------------------------
 *
 * Red, green and blue in turn, each held until the operator answers on the pad
 * (BUTTON_TOP yes, BUTTON_BOT no, taken on the press) or KODE_LED_TEST_STEP_MS
 * passes. A substate of active: everything else keeps working, but every
 * signal that would light the LED, the heartbeat included, is held off until
 * the row ends. The pad is subscribed only while the row runs.
 */

/** @brief What a colour is called in the log. */
static const char *colour_name(uint8_t colour)
{
    static const char * const NAMES[LED_COLOUR_COUNT] = { "red", "green", "blue" };

    if ( (uint8_t)LED_COLOUR_COUNT <= colour )
    {
        return "?";
    }
    return NAMES[colour];
}

/**
 * @brief Publish one record of the row, about the colour being asked.
 *
 * A question and its outcome carry the same step number, the colour's place.
 *
 * @param me    The AO.
 * @param phase LED_TEST_*
 */
static void test_record(LedMgr * const me, uint8_t phase)
{
    LedTestEvt *e = Q_NEW_X(LedTestEvt, 0U, LED_TEST_SIG);

    if ( NULL == e )
    {
        /* Bounded, never fatal: a record nobody sees costs the operator a
         * line, and asserting here would cost them the board. */
        return;
    }
    e->step   = (uint8_t)(me->colour + 1U);
    e->total  = (uint8_t)LED_COLOUR_COUNT;
    e->phase  = phase;
    e->colour = me->colour;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief End the row: the closing record, then the verdict.
 *
 * The caller takes the transition out and logs the reason before this call:
 * the console prints only `result=fail`.
 *
 * @param me The AO.
 * @param ok Whether the LED passed.
 */
static void test_finish(LedMgr * const me, bool ok)
{
    TestResultEvt *e;

    test_record(me, (uint8_t)LED_TEST_DONE);

    e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the led verdict is lost");
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", "led");
    e->ok = ok;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief Disarm a one-shot and say whether its expiry is still queued.
 *
 * A queued expiry belongs to nothing any more; acting on it would end the
 * colour that replaced the one it was timing.
 *
 * @param te   The one-shot.
 * @param live Whether the caller considers a previous arming outstanding.
 * @return true when an expired event for @p te is still queued.
 */
static bool timer_stale(QTimeEvt * const te, bool live)
{
    bool stale = false;

    if ( !QTimeEvt_disarm(te) )
    {
        stale = live;
    }
    return stale;
}

/** @brief Stop the row's one-shot, counting an expiry that is already queued. */
static void wait_stop(LedMgr * const me)
{
    if ( timer_stale(&me->test_wait, me->test_wait_live) )
    {
        me->test_wait_stale += 1U;
    }
    me->test_wait_live = false;
}

/**
 * @brief Light @p me->colour and ask about it.
 *
 * The question goes out after the write, so the reader already sees the colour.
 *
 * @param me The AO.
 */
static void colour_ask(LedMgr * const me)
{
    (void)kdrv_led_rgb(TEST_RGB[me->colour][0],
                       TEST_RGB[me->colour][1],
                       TEST_RGB[me->colour][2]);
    KLOG_N(TAG, "test led: step %u of %u - is the LED %s? TOP = yes, BOT = no",
           (unsigned)(me->colour + 1U), (unsigned)LED_COLOUR_COUNT,
           colour_name(me->colour));
    test_record(me, (uint8_t)LED_TEST_ASK);

    wait_stop(me);
    QTimeEvt_armX(&me->test_wait, KQP_TICKS_PER_MS(KODE_LED_TEST_STEP_MS), 0U);
    me->test_wait_live = true;
}

/**
 * @brief Say that the row is holding the LED, and which request paid for it.
 *
 * @param e The event being dropped.
 */
static void refuse_mid_row(QEvt const * const e)
{
    KLOG_D(TAG, "the led row owns the LED; signal %d ignored", (int)e->sig);
}

/**
 * @brief Say that a request was kept for the exit rather than applied now.
 *
 * For the two policy signals: what they ask for is what the exit obeys, so the
 * log must not say "ignored".
 *
 * @param e The event being recorded.
 */
static void record_mid_row(QEvt const * const e)
{
    KLOG_D(TAG, "the led row owns the LED; signal %d is recorded for the exit "
                "and the timer stays down for the row", (int)e->sig);
}

/**
 * @brief `test component=led`: the LED, judged by whoever is looking at it.
 *
 * Every way out goes through Q_EXIT_SIG, which leaves the LED dark, gives the
 * pad back and arms the heartbeat as it was last asked for.
 */
static QState LedMgr_testing(LedMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            me->colour = (uint8_t)LED_COLOUR_RED;
            /* Zeroed per row: an expiry queued by the previous row was dropped in
             * active, and counting it here would swallow a genuine deadline. */
            me->test_wait_stale = 0U;

            /* The heartbeat is the one thing that could light the LED behind
             * the row's back, and it belongs to this AO - so it is taken away
             * here and armed again in Q_EXIT_SIG, whichever way the row
             * ends. */
            (void)QTimeEvt_disarm(&me->heartbeat);

            QActive_subscribe(&me->super, BUTTON_EVT_SIG);
            colour_ask(me);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            wait_stop(me);
            QActive_unsubscribe(&me->super, BUTTON_EVT_SIG);
            (void)kdrv_led_set(false);
            if ( me->heartbeat_enabled )
            {
                /* What was last asked for, at the period last asked for. The blink
                 * keeps the last colour the row lit until somebody asks for another. */
                QTimeEvt_armX(&me->heartbeat, me->blink_ticks, me->blink_ticks);
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const b = Q_EVT_CAST(ButtonEvt);

            if ( !b->pressed )
            {
                /* The release of an answer, or of whatever was down when the
                 * row started. */
                status = Q_HANDLED();
                break;
            }
            if ( (uint8_t)KDRV_BTN_TOP == b->id )
            {
                KLOG_N(TAG, "test led: %s confirmed", colour_name(me->colour));
                test_record(me, (uint8_t)LED_TEST_PASS);
                if ( (uint8_t)(me->colour + 1U) >= (uint8_t)LED_COLOUR_COUNT )
                {
                    KLOG_N(TAG, "test led: PASS - red, green and blue each "
                                "confirmed at the bench");
                    test_finish(me, true);
                    status = Q_TRAN(&LedMgr_active);
                }
                else
                {
                    me->colour += 1U;
                    colour_ask(me);
                    status = Q_HANDLED();
                }
            }
            else if ( (uint8_t)KDRV_BTN_BOT == b->id )
            {
                /* The row stops on the first colour that was refused: an
                 * operator needs one sentence, and the colours after it are
                 * a different repair on the same board. */
                test_record(me, (uint8_t)LED_TEST_FAIL);
                KLOG_E(TAG, "test led: FAIL on %s - the operator says the LED "
                            "is not showing it", colour_name(me->colour));
                test_finish(me, false);
                status = Q_TRAN(&LedMgr_active);
            }
            else
            {
                KLOG_D(TAG, "test led: button %u is not an answer; TOP = yes, "
                            "BOT = no", (unsigned)b->id);
                status = Q_HANDLED();
            }
            break;
        }
        case LED_TEST_WAIT_SIG: {
            if ( 0U < me->test_wait_stale )
            {
                /* A deadline for a colour already answered. The live flag belongs to
                 * the arming that replaced it, so do not clear it here. */
                me->test_wait_stale -= 1U;
                status = Q_HANDLED();
                break;
            }
            me->test_wait_live = false;   /* this event IS its expiry */
            /* Nobody is there, and nobody will be there for the next colour
             * either: the row ends here rather than waiting out the rest. */
            test_record(me, (uint8_t)LED_TEST_TIMEOUT);
            KLOG_E(TAG, "test led: FAIL on %s - nobody answered in %u ms",
                   colour_name(me->colour), (unsigned)KODE_LED_TEST_STEP_MS);
            test_finish(me, false);
            status = Q_TRAN(&LedMgr_active);
            break;
        }
        case LED_HEARTBEAT_ENABLE_SIG: {
            /* Policy that outlives the row: the flag is written and the timer left
             * down, so the exit arms what was LAST asked for. */
            me->heartbeat_enabled = Q_EVT_CAST(LedEvt)->on;
            record_mid_row(e);
            status = Q_HANDLED();
            break;
        }
        case LED_BLINK_PERIOD_SIG: {
            /* The other half of the policy: recorded for the exit, timer left
             * down. A period below KODE_LED_BLINK_MIN_MS is refused as in active. */
            uint16_t const    ms    = Q_EVT_CAST(LedBlinkEvt)->period_ms;
            QTimeEvtCtr const ticks = KQP_TICKS_PER_MS((QTimeEvtCtr)ms);

            if ( KODE_LED_BLINK_MIN_MS <= ms )
            {
                me->blink_ticks = ticks;
                record_mid_row(e);
            }
            else
            {
                KLOG_W(TAG, "blink period %ums too short; ignored",
                       (unsigned)Q_EVT_CAST(LedBlinkEvt)->period_ms);
            }
            status = Q_HANDLED();
            break;
        }
        case LED_SET_SIG:
        case LED_TOGGLE_SIG:
        case LED_RGB_SIG:
        case LED_HEARTBEAT_SIG: {
            /* The row owns the LED: a colour now would be answered for the wrong
             * one. Dropped, not deferred: it would be stale by the row's end. */
            refuse_mid_row(e);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* The operator is looking at the LED and the pad is their answer. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "led test");
            status = Q_HANDLED();
            break;
        }
#endif
        case LED_TEST_REQ_SIG: {
            /* Not deferred: the row this would queue behind is the same row,
             * and restarting it would leave the first request's verdict
             * unpublished - which is a console waiting out its budget for an
             * answer that is never coming. */
            KLOG_W(TAG, "test led: already running; the request is ignored");
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&LedMgr_active);
            break;
        }
    }
    return status;
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief LOWPOWER_ROUND_SIG: the LED dark and the heartbeat down; answer once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void led_round(LedMgr * const me, const LowPowerRoundEvt * const r)
{
    (void)QTimeEvt_disarm(&me->heartbeat);
    (void)kdrv_led_set(false);
    me->rested = true;
    lowpower_ready(me->lp_who, r->round, false, NULL);
}

/**
 * @brief LOWPOWER_STATE_SIG: on AWAKE, the heartbeat back if it is wanted.
 *
 * Idempotent: the timer is disarmed before it is armed, so a repeated AWAKE
 * (an abandoned round publishes one) cannot arm it twice, and one that finds
 * the LED never rested does nothing.
 *
 * @param me The AO.
 * @param s  The change.
 */
static void led_state(LedMgr * const me, const LowPowerStateEvt * const s)
{
    if ( ((uint8_t)KODE_LP_AWAKE == s->state) && me->rested )
    {
        me->rested = false;
        if ( me->heartbeat_enabled )
        {
            (void)QTimeEvt_disarm(&me->heartbeat);
            QTimeEvt_armX(&me->heartbeat, me->blink_ticks, me->blink_ticks);
        }
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief Claim the LED, start it off with the heartbeat down, and subscribe.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to LedMgr_active.
 */
static QState LedMgr_initial(LedMgr * const me, void const * const par)
{
    (void)par;
    KLOG_D(TAG, "FSM start: initial -> active");
    kaos_claim(KBSP_RES_LED, me, "AO_LedMgr");
    kdrv_led_init(); /* LED starts OFF; heartbeat stays disarmed until asked for */

    QActive_subscribe(&me->super, LED_SET_SIG);
    QActive_subscribe(&me->super, LED_TOGGLE_SIG);
    QActive_subscribe(&me->super, LED_BLINK_PERIOD_SIG);
    QActive_subscribe(&me->super, LED_HEARTBEAT_ENABLE_SIG);
    QActive_subscribe(&me->super, LED_RGB_SIG);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "led");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
    }
#endif

    KLOG_I(TAG, "up; LED off, heartbeat disabled (default %ums)",
           (unsigned)KODE_LED_BLINK_MS);
    return Q_TRAN(&LedMgr_active);
}

/**
 * @brief The LED served: set, toggle, colour, blink period, heartbeat and the test row.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LedMgr_active(LedMgr * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case LED_HEARTBEAT_SIG: {
#if CONFIG_KODE_LOWPOWER
            /* A beat queued before the rest must not light the LED after it. */
            if ( !me->rested )
            {
                (void)kdrv_led_toggle();
            }
#else
            (void)kdrv_led_toggle();
#endif
            status = Q_HANDLED();
            break;
        }
        case LED_SET_SIG: {
            KLOG_D(TAG, "evt LED_SET on=%d", (int)Q_EVT_CAST(LedEvt)->on);
            (void)kdrv_led_set(Q_EVT_CAST(LedEvt)->on);
            status = Q_HANDLED();
            break;
        }
        case LED_TOGGLE_SIG: {
            KLOG_D(TAG, "evt LED_TOGGLE");
            (void)kdrv_led_toggle();
            status = Q_HANDLED();
            break;
        }
        case LED_RGB_SIG: {
            LedRgbEvt const * const rgb = Q_EVT_CAST(LedRgbEvt);
            KLOG_D(TAG, "evt LED_RGB %u,%u,%u",
                   (unsigned)rgb->r, (unsigned)rgb->g, (unsigned)rgb->b);
            (void)kdrv_led_rgb(rgb->r, rgb->g, rgb->b);
            status = Q_HANDLED();
            break;
        }
        case LED_BLINK_PERIOD_SIG: {
            uint16_t const ms = Q_EVT_CAST(LedBlinkEvt)->period_ms;
            QTimeEvtCtr const ticks = KQP_TICKS_PER_MS((QTimeEvtCtr)ms);
            if ( KODE_LED_BLINK_MIN_MS > ms )
            {
                KLOG_W(TAG, "blink period %ums too short; ignored",
                       (unsigned)ms);
            }
            else
            {
                me->blink_ticks = ticks;
                if ( me->heartbeat_enabled )
                {
                    (void)QTimeEvt_disarm(&me->heartbeat);
                    QTimeEvt_armX(&me->heartbeat,
                                  me->blink_ticks, me->blink_ticks);
                }
                KLOG_I(TAG, "blink period now %ums", (unsigned)ms);
            }
            status = Q_HANDLED();
            break;
        }
        case LED_HEARTBEAT_ENABLE_SIG: {
            bool const enable = Q_EVT_CAST(LedEvt)->on;
            me->heartbeat_enabled = enable;
            if ( enable )
            {
                /* disarm first so a repeated enable is idempotent: arming an
                 * already-armed QTimeEvt corrupts the time-event list. */
                (void)QTimeEvt_disarm(&me->heartbeat);
                QTimeEvt_armX(&me->heartbeat, me->blink_ticks, me->blink_ticks);
                KLOG_I(TAG, "heartbeat enabled");
            }
            else
            {
                (void)QTimeEvt_disarm(&me->heartbeat);
                (void)kdrv_led_set(false);
                KLOG_I(TAG, "heartbeat disabled");
            }
            status = Q_HANDLED();
            break;
        }
        case LED_TEST_REQ_SIG: {
            status = Q_TRAN(&LedMgr_testing);
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            led_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            led_state(me, Q_EVT_CAST(LowPowerStateEvt));
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
