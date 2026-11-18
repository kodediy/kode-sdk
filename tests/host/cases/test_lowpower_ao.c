/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_lowpower_ao.c
 * @brief AO_LowPower: the state machine and the round that enters a state.
 */
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkhal_power.h"
#include "Mockkhal_gpio.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp.h"
#include "kdrv_buttons.h"
#include "khal_console.h"
#include "khal_exp.h"
#include "khal_time.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kode_log.h"
#include "kqp.h"
#include "kqp_bsp.h"
#include "kset.h"
#include "config_support.h"
#include "probe_ao.h"
#include "qf_support.h"

#if !CONFIG_KODE_LOWPOWER
#error "test_lowpower_ao.c needs CONFIG_KODE_LOWPOWER: main/CMakeLists.txt spells it out"
#endif

/** @brief The source a case's requests say they came from. */
#define SRC_TEST        9U

/** @brief Another port's source, for the cases that tell two askers apart. */
#define SRC_OTHER       7U

/** @brief The expander byte with BOT low and the pad up. */
#define LEVELS_BOT      0xFEU
/** @brief The expander byte with PAD_UP low and BOT up. */
#define LEVELS_PAD_UP   0xFDU
/** @brief The edge of BOT. */
#define CHANGED_BOT     0x01U
/** @brief The edge of PAD_UP. */
#define CHANGED_PAD_UP  0x02U

/** @brief `sleep_after` / `off_after` word indexes the cases use. */
#define WORD_30S        0U   /**< sleep_after: "30s".   */
#define WORD_1H         1U   /**< off_after: "1h".      */
#define WORD_NEVER      5U   /**< Either: "never".      */

/** @brief The default idle time, milliseconds. */
#define IDLE_DEF_MS     (KODE_LOWPOWER_SLEEP_AFTER_DEF_S * 1000U)
/** @brief The idle time of "30s", milliseconds. */
#define IDLE_30S_MS     30000U
/** @brief A minute with no wall clock, microseconds. */
#define MINUTE_US       60000000ULL
/** @brief Seconds, as the cases write waits. */
#define SECOND_MS       1000U

/* ---- What the lock and the deep sleep were asked ------------------------------- */

static unsigned s_hold_true;        /**< khal_power_hold(true) calls.                      */
static unsigned s_hold_false;       /**< khal_power_hold(false) calls.                     */
static unsigned s_off_enter;        /**< khal_power_off_enter() calls.                     */
static bool     s_tick_at_release;  /**< The QP tick was running when the lock was let go. */
static unsigned s_spy_at_release;   /**< Events the spy held when the lock was let go.     */
static bool     s_said_at_release;  /**< The "entering ASLEEP" line was logged by then.    */
static bool     s_tick_at_hold;     /**< The QP tick was running when the lock was taken.  */
static unsigned s_spy_at_hold;      /**< Events the spy held when the lock was taken.      */

/** @brief What khal_power_stats() answers; a case sets it before the reading it wants it at. */
static khal_power_stats_t s_stats;

static void on_stats(khal_power_stats_t *out, int calls)
{
    (void)calls;
    *out = s_stats;
}

static bool log_has(const char *needle)
{
    bool found = false;

    for ( unsigned i = 0U; (fake_klog_count() > i) && !found; i++ )
    {
        const char *line = fake_klog_line(i);

        found = (NULL != line) && (strstr(line, needle) != NULL);
    }
    return found;
}

/** @brief Set an enum setting by the index of its word. */
static void set_word(const char *key, uint8_t word)
{
    const KodeConfigAnsEvt *a = config_ask(AO_LowPower,
        &(config_ask_t){ .key = key, .set = true, .e = word });

    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
}

static void on_hold(bool hold, int calls)
{
    (void)calls;
    if ( hold )
    {
        s_hold_true++;
        s_tick_at_hold = fake_time_periodic_running();
        s_spy_at_hold  = ao_spy_count();
    }
    else
    {
        s_hold_false++;
        s_tick_at_release = fake_time_periodic_running();
        s_spy_at_release  = ao_spy_count();
        s_said_at_release = log_has("entering ASLEEP");
    }
}

static void on_off_enter(int calls)
{
    (void)calls;
    s_off_enter++;
}

/* ---- Bring-up ---------------------------------------------------------------------- */

/**
 * @brief Register AO_LowPower once, construct it afresh and run its initial transition.
 *
 * @param power_ok What khal_power_init() answers.
 */
static void lp_up(kode_status_t power_ok)
{
    const QEvt *stray = NULL;

    qf_support_init();
    qf_support_reap_timers();
    (void)ao_spy();
    ao_spy_subscribe(LOWPOWER_ROUND_SIG);
    ao_spy_subscribe(LOWPOWER_STATE_SIG);

    Mockkhal_power_Init();
    khal_power_init_IgnoreAndReturn(power_ok);
    khal_power_hold_Stub(&on_hold);
    khal_power_off_enter_Stub(&on_off_enter);
    khal_power_stats_Stub(&on_stats);
    (void)memset(&s_stats, 0, sizeof(s_stats));
    fake_radio_set_sleep_stats(NULL);

    s_hold_true       = 0U;
    s_hold_false      = 0U;
    s_off_enter       = 0U;
    s_tick_at_release = true;
    s_spy_at_release  = 0U;
    s_said_at_release = false;
    s_tick_at_hold    = false;
    s_spy_at_hold     = 0U;
    fake_time_once_forget();

    kqp_tick_start();
    fake_time_periodic_forget();

    LowPower_ctor();
    qf_support_give_queue(AO_LowPower);
    AO_LowPower->prio  = KODE_PRIO_LOWPOWER;
    AO_LowPower->pthre = KODE_PRIO_LOWPOWER;
    if ( qf_support_claim_ao(AO_LowPower) )
    {
        QActive_register_(AO_LowPower);
    }
    while ( xQueueReceive(AO_LowPower->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    fake_klog_reset();
    QASM_INIT(AO_LowPower, (void *)0, 0U);
    ao_spy_drain();
}

/** @brief Leave the bus, the tick and the spy as the next case expects to find them. */
static void lp_down(void)
{
    const QEvt *stray = NULL;

    /* AWAKE counts the idle time on a time event that would outlive the case
     * and fire into whichever AO is built next at this address. "never" stops
     * the count the way a user would. */
    set_word(KSET_KEY_SLEEP_AFTER, WORD_NEVER);
    fake_time_once_forget();
    /* The subscription outlives the case, and every expander reading a later
     * file publishes would land in a queue nobody drains - each holding one of
     * the harness's thirty-two event blocks. */
    QActive_unsubscribeAll(AO_LowPower);
    while ( xQueueReceive(AO_LowPower->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    kqp_tick_rest(false);
    khal_console_rest(false);
    ao_spy_drain();
    Mockkhal_power_Destroy();
}

/** @brief Join @p n participants named A, B, C... and check their ids. */
static void join(uint8_t n)
{
    static const char * const names[] = { "A", "B", "C" };

    for ( uint8_t i = 0U; i < n; i++ )
    {
        TEST_ASSERT_EQUAL_UINT8(i, lowpower_join(ao_spy(), names[i]));
    }
}

static void say_ready(uint8_t who, uint32_t round, bool busy, const char *why)
{
    lowpower_ready(who, round, busy, why);
    (void)qf_support_settle(AO_LowPower);
}

static void ask(kode_lp_state_t target)
{
    lowpower_request(target, SRC_TEST);
    (void)qf_support_settle(AO_LowPower);
}

/** @brief Ask for @p target as the port @p src. */
static void ask_from(kode_lp_state_t target, uint8_t src)
{
    lowpower_request(target, src);
    (void)qf_support_settle(AO_LowPower);
}

/** @brief Take the next event and insist it is the round for @p target. */
static uint32_t expect_round(kode_lp_state_t target)
{
    const LowPowerRoundEvt *r = (const LowPowerRoundEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no round was published");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_ROUND_SIG, (int)r->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)target, r->target);
    return r->round;
}

/** @brief Take the next event and insist it is a change to @p state. */
static void expect_state(kode_lp_state_t state, kode_lp_cause_t cause,
                         kode_lp_state_t prev)
{
    const LowPowerStateEvt *s = (const LowPowerStateEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(s, "no state was published");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_STATE_SIG, (int)s->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)state, s->state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)cause, s->cause);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)prev, s->prev);
}

/** @brief Put the board ASLEEP through a request and two ready answers. */
static void go_asleep(void)
{
    uint32_t round;

    join(2U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();
}

/** @brief Hand AO_LowPower one expander reading. */
static void expander(uint8_t levels, uint8_t changed)
{
    IoexpInputEvt in = { QEVT_INITIALIZER(IOEXP_INPUT_SIG), levels, changed, true, true };

    QASM_DISPATCH(AO_LowPower, &in.super, 0U);
}

/** @brief Hand AO_LowPower any event as it would be dispatched. */
static void feed(const QEvt *e)
{
    QASM_DISPATCH(AO_LowPower, e, 0U);
}

/** @brief Hand AO_LowPower one reading of the charger. */
static void charger_reading(bool vbus)
{
    PowerEvt p;

    (void)memset(&p, 0, sizeof(p));
    p.super = (QEvt)QEVT_INITIALIZER(POWER_EVT_SIG);
    p.vbus  = vbus;
    feed(&p.super);
}

/** @brief Hand AO_LowPower a console line's note, from @p src. */
static void console_line(uint8_t src)
{
    LowPowerInputEvt n = { QEVT_INITIALIZER(LOWPOWER_INPUT_SIG), src };

    feed(&n.super);
}

/** @brief Hand AO_LowPower a button's change. */
static void button_event(void)
{
    ButtonEvt b = { QEVT_INITIALIZER(BUTTON_EVT_SIG), KDRV_BTN_TOP, true, 0U };

    feed(&b.super);
}

/** @brief Hand AO_LowPower a touch sample. */
static void touch_event(void)
{
    TouchEvt t = { QEVT_INITIALIZER(TOUCH_EVT_SIG), 1, 1, true, 0U, false };

    feed(&t.super);
}

/** @brief Wait @p ms of the QP tick for AO_LowPower. */
static void wait_ms(uint32_t ms)
{
    (void)qf_support_tick(AO_LowPower, QF_SUPPORT_STEPS(ms));
}

/** @brief Take every event queued for @p ao and count those with signal @p sig. */
static unsigned queued(QActive *ao, enum_t sig)
{
    const QEvt *e = NULL;
    unsigned n = 0U;

    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)sig == e->sig )
        {
            n++;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief Ask for ASLEEP and answer the two participants: the board is ASLEEP. */
static void sleep_now(void)
{
    uint32_t round;

    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();
}

/** @brief Fire the one-shot that is due first and let AO_LowPower hear it. */
static void fire_one_shot(void)
{
    TEST_ASSERT_TRUE_MESSAGE(fake_time_once_fire(), "no one-shot was armed");
    (void)qf_support_settle(AO_LowPower);
}

/* ---- The round ---------------------------------------------------------------------- */

TEST_CASE("a request for ASLEEP publishes one round for it", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(2U);

    ask(KODE_LP_ASLEEP);

    TEST_ASSERT_EQUAL_UINT32(1U, expect_round(KODE_LP_ASLEEP));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing else was published yet");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_false, "the lock is held until all are ready");

    say_ready(0U, 1U, false, NULL);
    say_ready(1U, 1U, false, NULL);
    lp_down();
}

TEST_CASE("all ready enters ASLEEP: the news, the tick, the lock, in that order",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);

    say_ready(0U, 1U, false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_false, "one answer is not all of them");
    say_ready(1U, 1U, false, NULL);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_true);
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_periodic_stops());
    TEST_ASSERT_FALSE_MESSAGE(s_tick_at_release, "the tick stops before the lock is let go");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_spy_at_release,
                                   "the state is published before the lock is let go");
    TEST_ASSERT_TRUE_MESSAGE(s_said_at_release,
                             "the board says what it enters before the P4 can stop logging");
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("one busy answer abandons the round and says who and why", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);

    say_ready(0U, round, false, NULL);
    say_ready(1U, round, true, "flash write");

    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);
    TEST_ASSERT_EQUAL_UINT(0U, fake_time_periodic_stops());
    TEST_ASSERT_EQUAL_STRING("B: flash write", lowpower_last_refusal());
    TEST_ASSERT_TRUE(log_has("B: flash write"));
    /* An owner that already rested for the round is told to come back. */
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    /* And the board is AWAKE and asks again. */
    ask(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT32(2U, expect_round(KODE_LP_ASLEEP));
    say_ready(0U, 2U, false, NULL);
    say_ready(1U, 2U, false, NULL);
    lp_down();
}

TEST_CASE("a participant that stays silent abandons the round on the budget, named",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);

    (void)qf_support_tick(AO_LowPower, QF_SUPPORT_STEPS(KODE_LOWPOWER_ROUND_MS - 100U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the budget has not run out yet");

    (void)qf_support_tick(AO_LowPower, QF_SUPPORT_STEPS(200U));
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);
    TEST_ASSERT_EQUAL_STRING("B: no answer", lowpower_last_refusal());
    TEST_ASSERT_TRUE(log_has("B did not answer"));
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("an answer for an old round or a second one from the same owner changes nothing",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(2U);

    /* Round 1 is cancelled by a request for AWAKE. */
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);
    ask(KODE_LP_AWAKE);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    ask(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT32(2U, expect_round(KODE_LP_ASLEEP));

    /* Round 1's answers come in late, both of them. */
    say_ready(0U, 1U, false, NULL);
    say_ready(1U, 1U, false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "round 1 is over");
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);

    /* Round 2: A answers twice; B has not answered, so it is not all of them. */
    say_ready(0U, 2U, false, NULL);
    say_ready(0U, 2U, false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "A's second answer is not B's");
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);

    /* A busy answer from a participant that already spoke is not an abort. */
    say_ready(0U, 2U, true, "late");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    say_ready(1U, 2U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);

    /* An answer once ASLEEP is no business of anyone's. */
    say_ready(1U, 2U, false, NULL);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

TEST_CASE("an answer from an id that never joined is dropped", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);

    say_ready(7U, 1U, true, "ghost");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    say_ready(0U, 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("waking restores the tick, then the lock, then tells the owners", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    fake_time_periodic_forget();

    ask(KODE_LP_AWAKE);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_periodic_starts());
    TEST_ASSERT_TRUE_MESSAGE(s_tick_at_hold, "the tick runs before the lock is taken");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_spy_at_hold,
                                   "AWAKE is published after the lock is taken");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_ASLEEP);
    lp_down();
}

/* About 200 wakes a second of the 300 a sleeping board was measured at, when its
 * console reader went on looking at a dock nobody can be on. */
TEST_CASE("the console's reader rests with the board, and only then", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    TEST_ASSERT_FALSE(fake_console_resting());

    go_asleep();
    TEST_ASSERT_TRUE_MESSAGE(fake_console_resting(), "asleep, the reader still polls");

    ask(KODE_LP_AWAKE);
    TEST_ASSERT_FALSE_MESSAGE(fake_console_resting(), "awake, the dock would answer a second late");
    lp_down();
}

TEST_CASE("BOT pressed during a round wins: the round is abandoned and late answers are ignored",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);

    expander(LEVELS_BOT, CHANGED_BOT);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_STRING("BOT: pressed", lowpower_last_refusal());

    say_ready(1U, round, false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the late answer enters nothing");
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);
    TEST_ASSERT_EQUAL_UINT(0U, fake_time_periodic_stops());
    lp_down();
}

TEST_CASE("another pad, or BOT already held when the round began, does not abandon it",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(1U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);   /* PAD_UP pressed, BOT up   */
    expander(LEVELS_BOT, CHANGED_PAD_UP);      /* BOT low, but no new edge */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    say_ready(0U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("a request while a round runs is refused, and a request for AWAKE cancels it",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);

    ask(KODE_LP_OFF);
    TEST_ASSERT_EQUAL_STRING("request: a round is running", lowpower_last_refusal());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    say_ready(0U, 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("OFF from AWAKE runs a round with target OFF, then deep sleep after the grace",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_OFF);
    round = expect_round(KODE_LP_OFF);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);

    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_TRUE(log_has("entering OFF"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_off_enter,
                                   "the log and the owners get their grace first");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_false, "deep sleep needs no light-sleep lock");

    (void)qf_support_tick(AO_LowPower, QF_SUPPORT_STEPS(KODE_LOWPOWER_OFF_GRACE_MS + 10U));
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);

    (void)qf_support_tick(AO_LowPower, QF_SUPPORT_STEPS(KODE_LOWPOWER_OFF_GRACE_MS + 10U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_off_enter, "once");
    lp_down();
}

TEST_CASE("OFF from ASLEEP goes direct: tick and lock back, the round, no AWAKE in between",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    go_asleep();
    fake_time_periodic_forget();

    ask(KODE_LP_OFF);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_TRUE_MESSAGE(s_tick_at_hold, "the tick runs before the lock is taken");
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_periodic_starts());
    round = expect_round(KODE_LP_OFF);   /* the first thing published */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no STATE(AWAKE) before the round");
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and none after it");
    (void)qf_support_tick(AO_LowPower, QF_SUPPORT_STEPS(KODE_LOWPOWER_OFF_GRACE_MS + 10U));
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    lp_down();
}

TEST_CASE("with nobody joined a request is decided at once", "[lowpower_ao]")
{
    lp_up(KODE_OK);

    ask(KODE_LP_ASLEEP);

    (void)expect_round(KODE_LP_ASLEEP);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);
    lp_down();
}

TEST_CASE("a board whose power management did not start refuses to sleep and says why",
          "[lowpower_ao]")
{
    lp_up(KODE_ERROR);
    join(1U);

    ask(KODE_LP_ASLEEP);

    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_STRING("request: power management did not start",
                             lowpower_last_refusal());
    TEST_ASSERT_TRUE(log_has("refused"));
    lp_down();
}

TEST_CASE("a request for the state the board is in, or for none, changes nothing",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);

    ask(KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    ask((kode_lp_state_t)7);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_STRING("request: no such state", lowpower_last_refusal());

    go_asleep();
    ask(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_true);
    lp_down();
}

TEST_CASE("joins: a full table or a round already begun refuses", "[lowpower_ao]")
{
    lp_up(KODE_OK);

    TEST_ASSERT_EQUAL_UINT8(KODE_LOWPOWER_WHO_NONE, lowpower_join(NULL, "none"));
    TEST_ASSERT_EQUAL_UINT8(KODE_LOWPOWER_WHO_NONE, lowpower_join(ao_spy(), NULL));
    for ( uint8_t i = 0U; KODE_LOWPOWER_PARTICIPANTS > i; i++ )
    {
        TEST_ASSERT_EQUAL_UINT8(i, lowpower_join(ao_spy(), "p"));
    }
    TEST_ASSERT_EQUAL_UINT8(KODE_LOWPOWER_WHO_NONE, lowpower_join(ao_spy(), "late"));
    lp_down();

    lp_up(KODE_OK);
    ask(KODE_LP_ASLEEP);                       /* nobody joined: decided, but begun */
    TEST_ASSERT_EQUAL_UINT8(KODE_LOWPOWER_WHO_NONE, lowpower_join(ao_spy(), "late"));
    TEST_ASSERT_TRUE(log_has("late joined after the first round"));
    lp_down();
}

TEST_CASE("a busy reason longer than the event holds is cut, not overrun", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);

    say_ready(0U, 1U, true, "a reason far longer than twenty-four bytes");

    TEST_ASSERT_EQUAL_STRING("A: a reason far longer tha", lowpower_last_refusal());
    lp_down();
}

/* ---- Idle time ---------------------------------------------------------------------- */

TEST_CASE("idle time with no input enters ASLEEP, on the default 2 min", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(1U);

    wait_ms(IDLE_DEF_MS - 100U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "two minutes have not gone by");
    wait_ms(200U);

    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("a button, a touch and a console line each start the idle time over",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);

    wait_ms(20U * SECOND_MS);
    button_event();                       /* due at 50 s */
    wait_ms(20U * SECOND_MS);
    touch_event();                        /* t=40 s: due at 70 s */
    wait_ms(20U * SECOND_MS);
    console_line(KODE_CONSOLE_SRC_BLE);   /* t=60 s: due at 90 s */
    wait_ms(20U * SECOND_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the count kept starting over");

    wait_ms(11U * SECOND_MS);             /* t=91 s */
    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("any input abandons a round the idle time began", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);

    wait_ms(IDLE_30S_MS + 100U);
    (void)expect_round(KODE_LP_ASLEEP);

    touch_event();

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_STRING("input: board used", lowpower_last_refusal());
    say_ready(0U, 1U, false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the late answer enters nothing");

    /* And the count began again. */
    wait_ms(IDLE_30S_MS - 100U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    wait_ms(200U);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();
}

/** @brief Hand AO_LowPower the release of a contact that stopped counting. */
static void touch_stale_release(void)
{
    TouchEvt t = { QEVT_INITIALIZER(TOUCH_EVT_SIG), 1, 1, false, 0U, true };

    feed(&t.super);
}

TEST_CASE("the release of a contact that stopped counting is nobody at the board",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);

    wait_ms(20U * SECOND_MS);
    touch_stale_release();                /* the count does not start over */
    wait_ms(10U * SECOND_MS + 100U);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();

    /* Nor does it abandon a round the idle time began. */
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);
    wait_ms(IDLE_30S_MS + 100U);
    uint32_t const round = expect_round(KODE_LP_ASLEEP);

    touch_stale_release();
    say_ready(0U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("a round somebody asked for ignores touches and buttons", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);

    touch_event();
    button_event();
    console_line(KODE_CONSOLE_SRC_SERIAL);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    say_ready(0U, 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("sleep_after never counts no idle time", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_NEVER);

    wait_ms(IDLE_DEF_MS + 10U * SECOND_MS);

    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

/* ---- The cable ----------------------------------------------------------------------- */

TEST_CASE("a cable refuses a request to sleep or to power off, and says so", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    charger_reading(true);

    ask(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no round for a board on a cable");
    TEST_ASSERT_EQUAL_STRING("cable: external power", lowpower_last_refusal());
    TEST_ASSERT_TRUE(log_has("external power"));

    ask(KODE_LP_OFF);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    charger_reading(false);
    ask(KODE_LP_ASLEEP);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("idle time on a cable does not sleep, and looks again later", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);
    charger_reading(true);

    wait_ms(IDLE_30S_MS + 100U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    charger_reading(false);
    wait_ms(IDLE_30S_MS + 100U);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("a cable that appears during a round abandons it", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);

    charger_reading(true);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_STRING("cable: external power", lowpower_last_refusal());
    say_ready(0U, 1U, false, NULL);
    say_ready(1U, 1U, false, NULL);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);
    lp_down();
}

TEST_CASE("a cable seen at the minute wakes the board", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();

    charger_reading(false);                 /* an ordinary reading: still ASLEEP */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    charger_reading(true);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    lp_down();
}

/* ---- What wakes ASLEEP --------------------------------------------------------------- */

TEST_CASE("only a press of BOT wakes ASLEEP among the pad, the touch and the buttons",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);
    expander(LEVELS_BOT, CHANGED_PAD_UP);      /* BOT low, but no new edge */
    expander(0xFFU, CHANGED_BOT);              /* BOT released             */
    button_event();
    touch_event();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing else wakes the board");
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_true);

    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    lp_down();
}

TEST_CASE("the top button's line wakes ASLEEP and the log names the top button",
          "[lowpower_ao]")
{
    static const QEvt top = QEVT_INITIALIZER(LOWPOWER_TOP_SIG);

    lp_up(KODE_OK);
    go_asleep();

    feed(&top);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TOP, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_TRUE_MESSAGE(log_has("AWAKE again (TOP)"), "the wake is said by its own name");
    TEST_ASSERT_FALSE_MESSAGE(log_has("AWAKE again (boot)"), "a wake is never called a boot");
    lp_down();
}

TEST_CASE("a message from the broker wakes ASLEEP", "[lowpower_ao]")
{
    MqttMsgEvt m;

    lp_up(KODE_OK);
    go_asleep();
    (void)memset(&m, 0, sizeof(m));
    m.super = (QEvt)QEVT_INITIALIZER(MQTT_MSG_SIG);

    feed(&m.super);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_MQTT, KODE_LP_ASLEEP);
    lp_down();
}

TEST_CASE("a console line wakes ASLEEP by the port it came on", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();

    console_line(KODE_CONSOLE_SRC_BLE);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BLE, KODE_LP_ASLEEP);

    sleep_now();
    console_line(KODE_CONSOLE_SRC_NET);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_MQTT, KODE_LP_ASLEEP);

    sleep_now();
    console_line(KODE_CONSOLE_SRC_SERIAL);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_ASLEEP);
    lp_down();
}

/* ---- A state no event could carry ---------------------------------------------------- */

/** @brief More blocks than the host harness's small pool holds. */
#define POOL_HOLD_MAX 64U

/**
 * @brief Take every block a LowPowerStateEvt would come from, so the next one fails.
 *
 * @param[out] held Where the blocks are kept until pool_give_back().
 * @return How many were taken.
 */
static size_t pool_take_all(const QEvt *held[POOL_HOLD_MAX])
{
    size_t n = 0U;

    while ( POOL_HOLD_MAX > n )
    {
        LowPowerStateEvt * const e = Q_NEW_X(LowPowerStateEvt, 0U, LOWPOWER_STATE_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    return n;
}

/** @brief Give back every block pool_take_all() took. */
static void pool_give_back(const QEvt *held[], size_t n)
{
    for ( size_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
}

/* The owners rested for the round wait for STATE(AWAKE) to restore their
 * devices, and nothing else will tell them: a wake whose event the pool could
 * not carry is published again on a short timer, until it goes out once. */
TEST_CASE("a wake the event pool could not carry is published again, once",
          "[lowpower_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];

    lp_up(KODE_OK);
    go_asleep();

    size_t const n = pool_take_all(held);
    expander(LEVELS_BOT, CHANGED_BOT);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the pool was not dry");
    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a retry went out of a dry pool");

    pool_give_back(held, n);
    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 2U);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);

    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the retry went on after it was said");
    lp_down();
}

/* The late AWAKE is the news only while the board is still awake: a new round
 * that began meanwhile has its own event, and the old one is not said. */
TEST_CASE("a wake owed is dropped when the board has gone on to another state",
          "[lowpower_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];
    uint32_t    round;

    lp_up(KODE_OK);
    go_asleep();

    size_t const n = pool_take_all(held);
    expander(LEVELS_BOT, CHANGED_BOT);
    pool_give_back(held, n);

    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a stale AWAKE went out over ASLEEP");
    lp_down();
}

/* The AWAKE owed is retried by a timer that only reads the state, and the
 * state stays AWAKE while the board rests: a round that began must kill it. */
TEST_CASE("a wake owed is not published while a round is in flight", "[lowpower_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];
    uint32_t    round;

    lp_up(KODE_OK);
    go_asleep();

    size_t const n = pool_take_all(held);
    expander(LEVELS_BOT, CHANGED_BOT);
    pool_give_back(held, n);

    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "an owed AWAKE went out in the middle of a round");

    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

/* ---- The minute and off-after -------------------------------------------------------- */

TEST_CASE("each minute asleep asks for a quiet charger reading and the glass, then rearms",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    TEST_ASSERT_EQUAL_UINT(1U, fake_time_once_pending());
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(MINUTE_US, fake_time_once_last_delay_us(),
                                     "no wall clock: a whole minute");

    fire_one_shot();

    TEST_ASSERT_EQUAL_UINT(1U, queued(AO_Power, POWER_PEEK_REQ_SIG));
    TEST_ASSERT_EQUAL_UINT(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_time_once_pending(), "the next minute is armed");
    TEST_ASSERT_EQUAL_UINT64(MINUTE_US, fake_time_once_last_delay_us());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

TEST_CASE("with the wall clock set the minute waits for the next one", "[lowpower_ao]")
{
    /* 20.5 s past a full minute. */
    fake_time_wall_set(1800000000ULL * 1000000ULL + 20500000ULL);
    lp_up(KODE_OK);
    go_asleep();

    TEST_ASSERT_EQUAL_UINT64(MINUTE_US - 20500000ULL, fake_time_once_last_delay_us());
    lp_down();
}

TEST_CASE("asleep for off_after goes to OFF, with the minutes before it", "[lowpower_ao]")
{
    unsigned minutes = 0U;
    unsigned glass = 0U;
    uint32_t round;

    lp_up(KODE_OK);
    set_word(KSET_KEY_OFF_AFTER, WORD_1H);
    go_asleep();
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    while ( (ao_spy_count() == 0U) && (100U > minutes) )
    {
        fire_one_shot();
        minutes++;
        glass += queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(60U, minutes, "an hour of minutes");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(59U, glass, "the last one was the hour, not a minute");
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    round = expect_round(KODE_LP_OFF);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_OFF_AFTER, KODE_LP_ASLEEP);
    wait_ms(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    lp_down();
}

TEST_CASE("off_after never keeps the board ASLEEP for good", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    set_word(KSET_KEY_OFF_AFTER, WORD_NEVER);
    go_asleep();

    for ( unsigned i = 0U; 130U > i; i++ )
    {
        fire_one_shot();
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "two hours asleep and no round");
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_once_pending());
    lp_down();
}

TEST_CASE("a one-shot left over from an earlier sleep is not a minute", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();                                 /* one-shot due at 60 s       */
    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    fake_time_advance_ms(30U * SECOND_MS);
    sleep_now();                                 /* its own, due at 90 s       */
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT(2U, fake_time_once_pending());

    fire_one_shot();                             /* the old one, at 60 s       */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG),
                                   "it is not yet this sleep's minute");

    fire_one_shot();                             /* the new one, at 90 s       */
    TEST_ASSERT_EQUAL_UINT(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG));
    TEST_ASSERT_EQUAL_UINT(1U, queued(AO_Power, POWER_PEEK_REQ_SIG));
    lp_down();
}

TEST_CASE("sleeps in a burst never run out of one-shots: the last one outstanding rearms",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(2U);
    for ( unsigned i = 0U; KHAL_TIMER_ONCE_SLOTS > i; i++ )
    {
        sleep_now();
        expander(LEVELS_BOT, CHANGED_BOT);
        ao_spy_drain();
        fake_time_advance_ms(SECOND_MS);
    }
    TEST_ASSERT_EQUAL_UINT(KHAL_TIMER_ONCE_SLOTS, fake_time_once_pending());
    sleep_now();                                 /* every slot is taken        */
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT(KHAL_TIMER_ONCE_SLOTS, fake_time_once_pending());
    TEST_ASSERT_TRUE(log_has("one-shots are out"));

    /* The four of the earlier sleeps come due first; the last of them finds
     * nothing outstanding and arms the minute of the sleep that lives on. */
    for ( unsigned i = 0U; KHAL_TIMER_ONCE_SLOTS > i; i++ )
    {
        fire_one_shot();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG),
                                   "none of them was this sleep's minute");
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_once_pending());
    fire_one_shot();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG),
                                   "the sleep that has lived on got its minute");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_time_once_pending(), "and the next one is armed");
    lp_down();
}

/* ---- Work that falls due asleep ----------------------------------------------------- */

/** @brief Tell AO_LowPower, as participant @p who, that its work is due @p after_ms from now. */
static void wake_in(uint8_t who, uint32_t after_ms)
{
    TEST_ASSERT_TRUE(lowpower_wake_at(who, khal_time_us() + ((uint64_t)after_ms * 1000ULL)));
    (void)qf_support_settle(AO_LowPower);
}

/** @brief Expect the board to have woken for an owner's time. */
static void expect_timer_wake(void)
{
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "the lock is taken on the way up");
}

TEST_CASE("an owner's time given in the round wakes the board for it, before the minute",
          "[lowpower_ao]")
{
    lowpower_info_t info;
    uint32_t        round;

    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    wake_in(1U, 20U * SECOND_MS);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(20U * 1000000ULL, fake_time_once_last_delay_us(),
                                     "the one-shot is the owner's time, not the minute");
    fire_one_shot();

    expect_timer_wake();
    TEST_ASSERT_TRUE(log_has("B has work due"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG),
                                   "the board woke for the owner, not for the minute");
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, info.state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_CAUSE_TIMER, info.cause);
    lp_down();
}

TEST_CASE("a time that arrives asleep pulls the one-shot forward", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();                                 /* the minute is due at 60 s */
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_once_pending());

    wake_in(0U, 10U * SECOND_MS);

    TEST_ASSERT_EQUAL_UINT(2U, fake_time_once_pending());
    TEST_ASSERT_EQUAL_UINT64(10U * 1000000ULL, fake_time_once_last_delay_us());
    fire_one_shot();
    expect_timer_wake();

    fire_one_shot();                             /* the minute's, now not its business */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT(0U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG));
    lp_down();
}

TEST_CASE("a time beyond the minute waits for the minute that reaches it", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    wake_in(0U, 150U * SECOND_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_time_once_pending(), "the minute is nearer: one one-shot");

    fire_one_shot();                             /* 60 s */
    fire_one_shot();                             /* 120 s */
    TEST_ASSERT_EQUAL_UINT(2U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(30U * 1000000ULL, fake_time_once_last_delay_us(),
                                     "the last stretch is cut to the owner's time");
    fire_one_shot();                             /* 150 s */
    expect_timer_wake();
    lp_down();
}

TEST_CASE("a time that has already passed wakes the board at once", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    fake_time_advance_ms(5U * SECOND_MS);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(lowpower_wake_at(0U, 1000000ULL));      /* at 1 s */
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT64(1U, fake_time_once_last_delay_us());
    fire_one_shot();
    expect_timer_wake();
    lp_down();
}

TEST_CASE("the earliest of the times given is the one that counts", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    wake_in(0U, 40U * SECOND_MS);
    wake_in(1U, 25U * SECOND_MS);
    wake_in(0U, 50U * SECOND_MS);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    TEST_ASSERT_EQUAL_UINT64(25U * 1000000ULL, fake_time_once_last_delay_us());
    fire_one_shot();                             /* a one-shot per time given, oldest due first */
    expect_timer_wake();
    TEST_ASSERT_TRUE(log_has("B has work due"));
    lp_down();
}

TEST_CASE("the times are forgotten at every wake: the next sleep does not wake for them",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    wake_in(0U, 20U * SECOND_MS);
    expander(LEVELS_BOT, CHANGED_BOT);           /* BOT wakes it first, at 0 s */
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    fake_time_advance_ms(5U * SECOND_MS);
    sleep_now();                                 /* its minute is due at 65 s */
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    fire_one_shot();                             /* the owner's, at 20 s */
    fire_one_shot();                             /* the first sleep's minute, at 60 s */
    fire_one_shot();                             /* this sleep's minute, at 65 s */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nobody woke the board");
    TEST_ASSERT_EQUAL_UINT(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG));
    lp_down();
}

TEST_CASE("the times are forgotten when a round begins: the owners say them again",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    wake_in(0U, 20U * SECOND_MS);                /* a stale note, given awake */
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(MINUTE_US, fake_time_once_last_delay_us(),
                                     "the stale time did not shorten the sleep");
    lp_down();
}

TEST_CASE("off_after that falls due with an owner's time goes to OFF, not to a wake",
          "[lowpower_ao]")
{
    unsigned minutes = 0U;
    uint32_t round;

    lp_up(KODE_OK);
    set_word(KSET_KEY_OFF_AFTER, WORD_1H);
    go_asleep();
    wake_in(0U, 3600U * SECOND_MS);
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    while ( (ao_spy_count() == 0U) && (100U > minutes) )
    {
        fire_one_shot();
        minutes++;
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    }

    round = expect_round(KODE_LP_OFF);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_OFF_AFTER, KODE_LP_ASLEEP);
    lp_down();
}

/* ---- The settings -------------------------------------------------------------------- */

TEST_CASE("sleep_after: default, set, stored and read back after a restart", "[lowpower_ao]")
{
    const KodeConfigAnsEvt *a;

    lp_up(KODE_OK);

    a = config_ask(AO_LowPower, &(config_ask_t){ .key = KSET_KEY_SLEEP_AFTER });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("2m", a->value);
    TEST_ASSERT_FALSE(a->stored);

    a = config_ask(AO_LowPower, &(config_ask_t){ .key = KSET_KEY_SLEEP_AFTER,
                                                  .set = true, .e = 3U });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("5m", a->value);
    TEST_ASSERT_TRUE(a->stored);
    ao_spy_drain();

    /* A boot: the store is read. lp_down() has put "never" there, the last
     * word it set, so the stored word is that one. */
    lp_down();
    lp_up(KODE_OK);
    a = config_ask(AO_LowPower, &(config_ask_t){ .key = KSET_KEY_SLEEP_AFTER });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("never", a->value);
    TEST_ASSERT_TRUE(a->stored);
    lp_down();
}

TEST_CASE("off_after: default, every word, never", "[lowpower_ao]")
{
    static const char * const words[] = { "20m", "1h", "6h", "24h", "7d", "never" };
    const KodeConfigAnsEvt *a;

    lp_up(KODE_OK);

    a = config_ask(AO_LowPower, &(config_ask_t){ .key = KSET_KEY_OFF_AFTER });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("20m", a->value);
    TEST_ASSERT_FALSE(a->stored);
    TEST_ASSERT_EQUAL_UINT32(20U * 60U, KODE_LOWPOWER_OFF_AFTER_DEF_S);

    for ( uint8_t i = 0U; Q_DIM(words) > i; i++ )
    {
        a = config_ask(AO_LowPower, &(config_ask_t){ .key = KSET_KEY_OFF_AFTER,
                                                      .set = true, .e = i });
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_STRING(words[i], a->value);
        TEST_ASSERT_TRUE(a->stored);
        ao_spy_drain();
    }
    lp_down();
}

TEST_CASE("a stored value this build does not list reads as the default, not stored",
          "[lowpower_ao]")
{
    const uint32_t odd = 1234U;
    const KodeConfigAnsEvt *a;

    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_OFF_AFTER, &odd, sizeof(odd));
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_SLEEP_AFTER, &odd, 1U);
    lp_up(KODE_OK);

    a = config_ask(AO_LowPower, &(config_ask_t){ .key = KSET_KEY_OFF_AFTER });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("20m", a->value);
    TEST_ASSERT_FALSE(a->stored);
    a = config_ask(AO_LowPower, &(config_ask_t){ .key = KSET_KEY_SLEEP_AFTER });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("2m", a->value);
    TEST_ASSERT_FALSE(a->stored);
    lp_down();
}

TEST_CASE("another owner's key is left unanswered", "[lowpower_ao]")
{
    lp_up(KODE_OK);

    TEST_ASSERT_NULL(config_ask(AO_LowPower, &(config_ask_t){ .key = "bright" }));
    lp_down();
}

TEST_CASE("changing sleep_after counts the new time from now", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);

    wait_ms(60U * SECOND_MS);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);
    wait_ms(IDLE_30S_MS - 100U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    wait_ms(200U);

    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();
}

/* ---- The refusal, published ---------------------------------------------------------- */

/** @brief Take the next event and insist it is a refusal; its reason is copied out. */
static void expect_refused(char *why, size_t size)
{
    const LowPowerRefusedEvt *r = (const LowPowerRefusedEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no refusal was published");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_REFUSED_SIG, (int)r->super.sig);
    (void)snprintf(why, size, "%s", r->why);
}

/* The console closes `sleep` on this: a refusal that only sat in
 * lowpower_last_refusal() would leave the verb waiting for its watchdog. */
TEST_CASE("a cable's refusal is published with its reason", "[lowpower_ao]")
{
    char why[KODE_LOWPOWER_REFUSAL_MAX];

    lp_up(KODE_OK);
    join(1U);
    charger_reading(true);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    ask(KODE_LP_OFF);

    expect_refused(why, sizeof(why));
    TEST_ASSERT_EQUAL_STRING("cable: external power", why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one refusal, one event");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a request refused for a round already running is published", "[lowpower_ao]")
{
    char why[KODE_LOWPOWER_REFUSAL_MAX];

    lp_up(KODE_OK);
    join(1U);
    ask(KODE_LP_ASLEEP);
    (void)expect_round(KODE_LP_ASLEEP);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    ask(KODE_LP_OFF);

    expect_refused(why, sizeof(why));
    TEST_ASSERT_EQUAL_STRING("request: a round is running", why);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a busy owner's refusal is published after the board stays AWAKE",
          "[lowpower_ao]")
{
    char why[KODE_LOWPOWER_REFUSAL_MAX];

    lp_up(KODE_OK);
    join(2U);
    ask(KODE_LP_ASLEEP);
    uint32_t const round = expect_round(KODE_LP_ASLEEP);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    /* The request's window is over, so the refusal is final. */
    fake_time_advance_ms(KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS);
    say_ready(1U, round, true, "writing a file");

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    expect_refused(why, sizeof(why));
    TEST_ASSERT_EQUAL_STRING("B: writing a file", why);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

/* The console closes its pending verb only on a refusal that carries its own
 * port: one meant for another port must not answer it. */
TEST_CASE("a refused request carries the source that asked", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    charger_reading(true);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    ask_from(KODE_LP_OFF, SRC_OTHER);

    const LowPowerRefusedEvt *r = (const LowPowerRefusedEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(SRC_OTHER, r->src);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("an abandoned round is refused to the source that began it", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(2U);
    ask_from(KODE_LP_ASLEEP, SRC_OTHER);
    uint32_t const round = expect_round(KODE_LP_ASLEEP);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    /* The request's window is over, so the refusal is final. */
    fake_time_advance_ms(KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS);
    say_ready(1U, round, true, "writing a file");

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    const LowPowerRefusedEvt *r = (const LowPowerRefusedEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(SRC_OTHER, r->src);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a request refused behind a running round names its own source, not the round's",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    ask_from(KODE_LP_ASLEEP, SRC_OTHER);
    (void)expect_round(KODE_LP_ASLEEP);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    ask_from(KODE_LP_OFF, SRC_TEST);

    const LowPowerRefusedEvt *r = (const LowPowerRefusedEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8(SRC_TEST, r->src);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one refusal, one event");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a round the idle time began is refused to no port", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);
    wait_ms(IDLE_30S_MS + 100U);
    uint32_t const round = expect_round(KODE_LP_ASLEEP);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    say_ready(0U, round, true, "writing a file");

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    const LowPowerRefusedEvt *r = (const LowPowerRefusedEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_LOG_SRC_NONE, r->src,
        "no console is waiting on a round nobody asked for");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a request that goes through publishes no refusal", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();

    ask(KODE_LP_ASLEEP);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);

    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

/* ---- A request that meets a busy owner ----------------------------------------------- */

/** @brief Step of the wait below, milliseconds: what the harness ticks in. */
#define WAIT_STEP_MS    10U

/**
 * @brief Let @p ms go by on the clock AO_LowPower reads and on its tick.
 *
 * In steps, so the clock reads what the tick has counted when a time event
 * runs out and the AO looks at it.
 *
 * @param ms A multiple of WAIT_STEP_MS.
 */
static void wait_real(uint32_t ms)
{
    for ( uint32_t t = 0U; t < ms; t += WAIT_STEP_MS )
    {
        fake_time_advance_ms(WAIT_STEP_MS);
        wait_ms(WAIT_STEP_MS);
    }
}

/** @brief Take the next event and insist it is a refusal for @p src, with @p why. */
static void expect_refused_for(uint8_t src, const char *why)
{
    const LowPowerRefusedEvt *r = (const LowPowerRefusedEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no refusal was published");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_REFUSED_SIG, (int)r->super.sig);
    TEST_ASSERT_EQUAL_UINT8(src, r->src);
    TEST_ASSERT_EQUAL_STRING(why, r->why);
}

/** @brief Watch the refusals too, with nothing yet in the spy. */
static void watch_refusals(void)
{
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    ao_spy_drain();
}

TEST_CASE("an order for OFF from ASLEEP waits for a busy owner and goes OFF, never AWAKE",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    go_asleep();

    ask(KODE_LP_OFF);
    round = expect_round(KODE_LP_OFF);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, true, "transfer");

    TEST_ASSERT_EQUAL_STRING("B: transfer", lowpower_last_refusal());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no STATE(AWAKE): the owners stay at rest");
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);

    /* B alone is asked again, in the same round. */
    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS + 10U);
    TEST_ASSERT_EQUAL_UINT32(round, expect_round(KODE_LP_OFF));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and only B");
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_ASLEEP);
    wait_real(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    lp_down();
}

TEST_CASE("BOT and a request for AWAKE do not end an order for OFF", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(1U);
    ask(KODE_LP_OFF);
    round = expect_round(KODE_LP_OFF);

    expander(LEVELS_BOT, CHANGED_BOT);
    button_event();
    touch_event();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "BOT and the inputs leave the order standing");

    ask(KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_STRING("request: a round is running", lowpower_last_refusal());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    say_ready(0U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    wait_real(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    lp_down();
}

TEST_CASE("an order for OFF that an owner never finishes goes OFF when its budget runs out",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    watch_refusals();
    ask(KODE_LP_OFF);
    round = expect_round(KODE_LP_OFF);
    say_ready(0U, round, false, NULL);

    /* B says busy at every ask for five seconds, then stops answering. */
    say_ready(1U, round, true, "transfer");
    for ( unsigned i = 0U; 5U > i; i++ )
    {
        wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS);
        TEST_ASSERT_EQUAL_UINT32(round, expect_round(KODE_LP_OFF));
        if ( 4U > i )
        {
            say_ready(1U, round, true, "transfer");
        }
    }
    wait_real(KODE_LOWPOWER_ROUND_MS + 100U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "a silence is not the end of an order's round");

    wait_real(KODE_LOWPOWER_ORDER_MS - (5U * KODE_LOWPOWER_REQUEST_RETRY_MS)
              - KODE_LOWPOWER_ROUND_MS);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_TRUE(log_has("B did not answer"));
    TEST_ASSERT_TRUE(log_has("going OFF without them"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "an order that went through is refused to no one");
    wait_real(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("an order for OFF asks a busy owner alone again, in the same round, and goes through",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    watch_refusals();
    ask(KODE_LP_OFF);
    round = expect_round(KODE_LP_OFF);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, true, "radio busy");

    /* The board does not come back AWAKE, and nobody is answered. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no STATE(AWAKE), no refusal");
    TEST_ASSERT_TRUE(log_has("OFF waits for B (radio busy)"));
    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS - 100U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the wait has not run out yet");
    wait_real(200U);

    TEST_ASSERT_EQUAL_UINT32(round, expect_round(KODE_LP_OFF));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "A, at rest, is not asked again");
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one success, no refusal");

    wait_real(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
    TEST_ASSERT_EQUAL_UINT(1U, s_off_enter);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a request refused for longer than its window is answered once, after it",
          "[lowpower_ao]")
{
    uint32_t round;
    uint32_t rounds = 0U;

    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask_from(KODE_LP_ASLEEP, SRC_OTHER);

    /* Busy at every round, answered at once: a round a second. */
    for ( uint32_t t = 0U; KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS > t;
         t += KODE_LOWPOWER_REQUEST_RETRY_MS )
    {
        round = expect_round(KODE_LP_ASLEEP);
        rounds++;
        say_ready(0U, round, true, "last reason");
        expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nobody is answered inside the window");
        wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS);
    }

    TEST_ASSERT_EQUAL_UINT32(KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS / KODE_LOWPOWER_REQUEST_RETRY_MS,
                             rounds);
    expect_refused_for(SRC_OTHER, "A: last reason");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "exactly one refusal");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, queued(AO_LowPower, LOWPOWER_RETRY_TMO_SIG),
                                   "and nothing is asked again");
    wait_real(3U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the request stays given up");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("an owner that does not answer is retried like a busy one", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(2U);
    watch_refusals();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);

    wait_real(KODE_LOWPOWER_ROUND_MS + 100U);
    TEST_ASSERT_EQUAL_STRING("B: no answer", lowpower_last_refusal());
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a silence is not answered at once");

    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT32(round + 1U, expect_round(KODE_LP_ASLEEP));
    say_ready(0U, round + 1U, false, NULL);
    say_ready(1U, round + 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a silence that outlasts the window is answered once, with its reason",
          "[lowpower_ao]")
{
    uint32_t round = 0U;

    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask(KODE_LP_ASLEEP);

    /* A round of 2 s and a wait of 1 s: the fourth round begins at 9 s, the
     * window is over when it times out at 11 s, and that is the answer. */
    for ( unsigned i = 0U; 4U > i; i++ )
    {
        round = expect_round(KODE_LP_ASLEEP);
        wait_real(KODE_LOWPOWER_ROUND_MS + 100U);
        expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
        if ( 3U > i )
        {
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "still inside the window");
            wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS - 100U);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(4U, round);
    expect_refused_for(SRC_TEST, "A: no answer");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "exactly one refusal");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("external power is not retried: one refusal at once", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    watch_refusals();

    /* At the request. */
    charger_reading(true);
    ask_from(KODE_LP_OFF, SRC_OTHER);
    expect_refused_for(SRC_OTHER, "cable: external power");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing is asked again");

    /* In the middle of the round. */
    charger_reading(false);
    ask_from(KODE_LP_OFF, SRC_OTHER);
    (void)expect_round(KODE_LP_OFF);
    charger_reading(true);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_AWAKE);
    expect_refused_for(SRC_OTHER, "cable: external power");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing is asked again");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a cable plugged in while the request waits ends the wait", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask(KODE_LP_ASLEEP);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    charger_reading(true);

    expect_refused_for(SRC_TEST, "cable: external power");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and the round does not begin again");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a cable plugged in while an order for OFF waits ends it: on a cable the board stays on",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask(KODE_LP_OFF);
    say_ready(0U, expect_round(KODE_LP_OFF), true, "radio busy");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    charger_reading(true);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_AWAKE);
    expect_refused_for(SRC_TEST, "cable: external power");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and nobody is asked again");
    TEST_ASSERT_EQUAL_UINT(0U, s_off_enter);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("BOT while the request waits ends the wait with the last refusal", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask_from(KODE_LP_ASLEEP, SRC_OTHER);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS / 2U);

    expander(LEVELS_BOT, CHANGED_BOT);

    expect_refused_for(SRC_OTHER, "A: radio busy");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the round does not begin again");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("BOT in a round the retry began ends the request at once", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask(KODE_LP_ASLEEP);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS + 10U);
    (void)expect_round(KODE_LP_ASLEEP);

    expander(LEVELS_BOT, CHANGED_BOT);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_AWAKE);
    expect_refused_for(SRC_TEST, "BOT: pressed");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and it is not asked again");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("the other inputs do not end the wait, as they do not end the round", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    button_event();
    touch_event();
    console_line((uint8_t)KODE_CONSOLE_SRC_SERIAL);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS + 10U);
    TEST_ASSERT_EQUAL_UINT32(round + 1U, expect_round(KODE_LP_ASLEEP));
    say_ready(0U, round + 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a request for AWAKE ends the wait with the last refusal", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask_from(KODE_LP_ASLEEP, SRC_OTHER);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    ask_from(KODE_LP_AWAKE, SRC_TEST);

    expect_refused_for(SRC_OTHER, "A: radio busy");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the round does not begin again");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a new request replaces the waiting one and answers its asker", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask_from(KODE_LP_ASLEEP, SRC_OTHER);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    ask_from(KODE_LP_ASLEEP, SRC_TEST);

    expect_refused_for(SRC_OTHER, "A: radio busy");
    TEST_ASSERT_EQUAL_UINT32(round + 1U, expect_round(KODE_LP_ASLEEP));
    say_ready(0U, round + 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("the same port asking again is answered once, by the new request", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(1U);
    watch_refusals();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    ask(KODE_LP_ASLEEP);

    TEST_ASSERT_EQUAL_UINT32(round + 1U, expect_round(KODE_LP_ASLEEP));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the old asker is the new one");
    say_ready(0U, round + 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a round the idle time began is refused busy and not retried", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);
    wait_real(IDLE_30S_MS + 100U);
    round = expect_round(KODE_LP_ASLEEP);
    watch_refusals();

    say_ready(0U, round, true, "writing a file");

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    expect_refused_for((uint8_t)KODE_LOG_SRC_NONE, "A: writing a file");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no round is begun again");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

/* ---- The summary of the last rest ---------------------------------------------------- */

/** @brief The cell's current the cases read, mA (out of the cell is negative). */
#define SAMPLE_MA       (-7)
/** @brief The cell's voltage the cases read, mV. */
#define SAMPLE_MV       3850U
/** @brief Minutes that overfill the ring by ten. */
#define MINUTES_OVER    (KODE_LOWPOWER_LAST_SAMPLES + 10U)

/** @brief Hand AO_LowPower the reading its peek was answered with. */
static void power_sample(int16_t ma, uint16_t mv)
{
    PowerEvt p;

    (void)memset(&p, 0, sizeof(p));
    p.super      = (QEvt)QEVT_INITIALIZER(POWER_EVT_SIG);
    p.current_ma = ma;
    p.vbat_mv    = mv;
    feed(&p.super);
}

/** @brief A minute passes asleep and AO_Power answers the peek. */
static void minute_with(int16_t ma, uint16_t mv)
{
    fire_one_shot();
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    power_sample(ma, mv);
}

/* The record is on the store the moment the sample is: a reset that follows
 * (the port opening, a watchdog) finds the rest as it stood. */
TEST_CASE("summary_kept_across_reset: the rest is read back by a board that booted after it",
          "[lowpower_ao]")
{
    lowpower_last_t last;
    const lowpower_sample_t *sample;
    const kdrv_radio_sleep_stats_t before = { .wakes_c5 = 3U, .wakes_tx = 1U, .deaf = 0U };
    const kdrv_radio_sleep_stats_t during = { .wakes_c5 = 7U, .wakes_tx = 3U, .deaf = 1U };

    fake_nvs_reset();
    fake_time_wall_set(1800000000ULL * 1000000ULL);
    lp_up(KODE_OK);
    fake_radio_set_sleep_stats(&before);
    s_stats.sleeps = 4U;
    go_asleep();

    fake_radio_set_sleep_stats(&during);
    s_stats.slept_us = 54000000ULL;
    s_stats.since_us = 60000000ULL;
    s_stats.sleeps   = 19U;
    minute_with(SAMPLE_MA, SAMPLE_MV);
    fire_one_shot();                                 /* the next minute stores it */

    lp_down();
    lp_up(KODE_OK);                                  /* the board booted again   */
    TEST_ASSERT_TRUE_MESSAGE(lowpower_last_read(&last), "nothing was kept");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOWPOWER_LAST_VERSION, last.version);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_LOWPOWER_LAST_OPEN, last.end_cause,
        "no wake had been written: the rest was still on");
    TEST_ASSERT_EQUAL_UINT32(1800000000U, last.start_s);
    TEST_ASSERT_EQUAL_UINT32(60U, last.duration_s);
    TEST_ASSERT_EQUAL_UINT8(90U, last.slept_pct);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(15U, last.sleeps, "the rest's, not the boot's");
    TEST_ASSERT_EQUAL_UINT32(4U, last.wakes_c5);
    TEST_ASSERT_EQUAL_UINT32(2U, last.wakes_tx);
    TEST_ASSERT_EQUAL_UINT32(1U, last.deaf);
    TEST_ASSERT_EQUAL_UINT8(1U, last.count);
    sample = lowpower_last_sample(&last, 0U);
    TEST_ASSERT_NOT_NULL(sample);
    TEST_ASSERT_EQUAL_UINT32(60U, sample->t_s);
    TEST_ASSERT_EQUAL_INT16(SAMPLE_MA, sample->ma);
    TEST_ASSERT_EQUAL_UINT16(SAMPLE_MV, sample->mv);
    TEST_ASSERT_NULL(lowpower_last_sample(&last, 1U));
    lp_down();
}

TEST_CASE("a wake closes the summary with its cause and lets the working copy go",
          "[lowpower_ao]")
{
    lowpower_last_t last;
    unsigned const freed = fake_psram_free_count();

    fake_nvs_reset();
    lp_up(KODE_OK);
    go_asleep();
    minute_with(SAMPLE_MA, SAMPLE_MV);
    minute_with(SAMPLE_MA, SAMPLE_MV);
    fake_time_advance_ms(20U * SECOND_MS);

    expander(LEVELS_BOT, CHANGED_BOT);

    TEST_ASSERT_TRUE(lowpower_last_read(&last));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_CAUSE_BOT, last.end_cause);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(140U, last.duration_s, "the rest, to the wake");
    TEST_ASSERT_EQUAL_UINT8(2U, last.count);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(freed + 1U, fake_psram_free_count(),
                                   "the working copy stays only as long as the rest");
    lp_down();
}

TEST_CASE("the ring keeps the last samples, oldest first, when it wraps", "[lowpower_ao]")
{
    lowpower_last_t last;
    const lowpower_sample_t *sample;

    fake_nvs_reset();
    lp_up(KODE_OK);
    set_word(KSET_KEY_OFF_AFTER, WORD_NEVER);
    go_asleep();
    for ( unsigned i = 0U; MINUTES_OVER > i; i++ )
    {
        minute_with((int16_t)i, (uint16_t)(3000U + i));
    }
    fire_one_shot();                                  /* the next minute stores the last */

    TEST_ASSERT_TRUE(lowpower_last_read(&last));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOWPOWER_LAST_SAMPLES, last.count);
    sample = lowpower_last_sample(&last, 0U);
    TEST_ASSERT_NOT_NULL(sample);
    TEST_ASSERT_EQUAL_INT16_MESSAGE(10, sample->ma, "the first ten were overwritten");
    TEST_ASSERT_EQUAL_UINT32(11U * 60U, sample->t_s);
    sample = lowpower_last_sample(&last, KODE_LOWPOWER_LAST_SAMPLES - 1U);
    TEST_ASSERT_NOT_NULL(sample);
    TEST_ASSERT_EQUAL_INT16((int16_t)(MINUTES_OVER - 1U), sample->ma);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)(3000U + MINUTES_OVER - 1U), sample->mv);
    TEST_ASSERT_NULL(lowpower_last_sample(&last, KODE_LOWPOWER_LAST_SAMPLES));
    lp_down();
}

TEST_CASE("the summary is written at the next minute, before the glass is asked",
          "[lowpower_ao]")
{
    fake_nvs_reset();
    lp_up(KODE_OK);
    go_asleep();
    unsigned const base = fake_nvs_set_count();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(base, fake_nvs_set_count(), "nothing is written on entering");
    fire_one_shot();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(base, fake_nvs_set_count(), "the peek is not the reading");
    power_sample(SAMPLE_MA, SAMPLE_MV);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(base, fake_nvs_set_count(),
                                   "the reading is kept, not written, while the glass writes");

    power_sample(SAMPLE_MA, SAMPLE_MV);               /* a poll nobody asked for  */
    TEST_ASSERT_EQUAL_UINT(base, fake_nvs_set_count());

    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    fire_one_shot();                                  /* the next minute          */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(base + 1U, fake_nvs_set_count(),
                                   "the last minute's sample is stored as the next one starts");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG),
                                   "and the glass is asked after it");
    lp_down();
}

/* The store write and the glass's write must not overlap: the write runs in
 * this AO's step and the request is posted after it returns. The hook runs
 * inside the store write and sees whether the glass was asked already. */
static unsigned s_asked_during_save;

static void asked_during_save(void)
{
    s_asked_during_save = (unsigned)uxQueueMessagesWaiting(AO_Display->eQueue);
}

TEST_CASE("the glass is asked for the minute only after the summary is stored",
          "[lowpower_ao]")
{
    fake_nvs_reset();
    lp_up(KODE_OK);
    go_asleep();
    minute_with(SAMPLE_MA, SAMPLE_MV);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    s_asked_during_save = 0xFFU;
    fake_nvs_set_hook(&asked_during_save);

    fire_one_shot();

    fake_nvs_set_hook(NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_asked_during_save,
                                   "the glass was asked while the store was written");
    TEST_ASSERT_EQUAL_UINT(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG));
    lp_down();
}

TEST_CASE("a rest too short for its first minute leaves the earlier summary alone",
          "[lowpower_ao]")
{
    lowpower_last_t last;

    fake_nvs_reset();
    lp_up(KODE_OK);
    go_asleep();
    minute_with(SAMPLE_MA, SAMPLE_MV);
    expander(LEVELS_BOT, CHANGED_BOT);
    ao_spy_drain();
    unsigned const base = fake_nvs_set_count();

    sleep_now();
    fake_time_advance_ms(10U * SECOND_MS);
    expander(LEVELS_BOT, CHANGED_BOT);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(base, fake_nvs_set_count(), "a ten second rest wrote");
    TEST_ASSERT_TRUE(lowpower_last_read(&last));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_CAUSE_BOT, last.end_cause);
    TEST_ASSERT_EQUAL_UINT8(1U, last.count);
    lp_down();
}

TEST_CASE("a cable at the minute wakes the board and is no sample", "[lowpower_ao]")
{
    lowpower_last_t last;

    fake_nvs_reset();
    lp_up(KODE_OK);
    go_asleep();
    fire_one_shot();

    charger_reading(true);

    TEST_ASSERT_FALSE_MESSAGE(lowpower_last_read(&last), "a charging current is not the rest's");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_ASLEEP);
    lp_down();
}

TEST_CASE("an OFF request while ASLEEP ends the summary with that request", "[lowpower_ao]")
{
    lowpower_last_t last;
    uint32_t round;

    fake_nvs_reset();
    lp_up(KODE_OK);
    go_asleep();
    minute_with(SAMPLE_MA, SAMPLE_MV);

    ask(KODE_LP_OFF);

    TEST_ASSERT_TRUE(lowpower_last_read(&last));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_CAUSE_REQUEST, last.end_cause);
    round = expect_round(KODE_LP_OFF);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_ASLEEP);
    wait_ms(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
    lp_down();
}

TEST_CASE("no memory for the summary does not stop the sleep", "[lowpower_ao]")
{
    lowpower_last_t last;

    fake_nvs_reset();
    lp_up(KODE_OK);
    fake_psram_refuse(1U);

    go_asleep();
    minute_with(SAMPLE_MA, SAMPLE_MV);

    TEST_ASSERT_TRUE(log_has("no memory for the summary"));
    TEST_ASSERT_FALSE(lowpower_last_read(&last));
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);
    lp_down();
}

TEST_CASE("a record of another layout reads as none", "[lowpower_ao]")
{
    lowpower_last_t last;
    uint8_t         stale[KODE_LOWPOWER_LAST_HEAD_BYTES] = { KODE_LOWPOWER_LAST_VERSION };

    fake_nvs_reset();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_LP_LAST, stale, sizeof(stale));
    TEST_ASSERT_FALSE_MESSAGE(lowpower_last_read(&last), "a short record was taken");

    (void)memset(&last, 0, sizeof(last));
    last.version = (uint8_t)(KODE_LOWPOWER_LAST_VERSION + 1U);
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_LP_LAST, &last, sizeof(last));
    TEST_ASSERT_FALSE_MESSAGE(lowpower_last_read(&last), "another version was taken");
}

/* ---- lowpower_info ---------------------------------------------------------------------- */

TEST_CASE("the info reads AWAKE since boot, with the stored times and no wakes",
          "[lowpower_ao]")
{
    lowpower_info_t info;

    lp_up(KODE_OK);
    fake_time_advance_ms(7U * SECOND_MS);

    lowpower_info_get(&info);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, info.state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_CAUSE_BOOT, info.cause);
    TEST_ASSERT_EQUAL_UINT32(7U, info.for_s);
    TEST_ASSERT_EQUAL_UINT32(KODE_LOWPOWER_SLEEP_AFTER_DEF_S, info.sleep_after_s);
    TEST_ASSERT_EQUAL_UINT32(KODE_LOWPOWER_OFF_AFTER_DEF_S, info.off_after_s);
    TEST_ASSERT_EQUAL_UINT32(0U, info.wakes_bot + info.wakes_ble + info.wakes_mqtt);
    lp_down();
}

TEST_CASE("the info counts the wakes by the cause that ended each sleep",
          "[lowpower_ao]")
{
    lowpower_info_t info;
    MqttMsgEvt      m;

    lp_up(KODE_OK);
    join(2U);
    (void)memset(&m, 0, sizeof(m));
    m.super = (QEvt)QEVT_INITIALIZER(MQTT_MSG_SIG);

    /* BOT, then the broker, then BOT again. */
    sleep_now();
    expander(LEVELS_BOT, CHANGED_BOT);
    ao_spy_drain();
    sleep_now();
    feed(&m.super);
    ao_spy_drain();
    sleep_now();
    expander(LEVELS_BOT, CHANGED_BOT);

    lowpower_info_get(&info);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, info.state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_CAUSE_BOT, info.cause);
    TEST_ASSERT_EQUAL_UINT32(2U, info.wakes_bot);
    TEST_ASSERT_EQUAL_UINT32(1U, info.wakes_mqtt);
    TEST_ASSERT_EQUAL_UINT32(0U, info.wakes_ble);
    lp_down();
}

TEST_CASE("the info times the state from its change, and an abandoned round is no change",
          "[lowpower_ao]")
{
    lowpower_info_t info;

    lp_up(KODE_OK);
    join(1U);
    fake_time_advance_ms(10U * SECOND_MS);
    ask(KODE_LP_ASLEEP);
    say_ready(0U, expect_round(KODE_LP_ASLEEP), true, "busy");
    ao_spy_drain();
    fake_time_advance_ms(5U * SECOND_MS);

    lowpower_info_get(&info);

    TEST_ASSERT_EQUAL_UINT32_MESSAGE(15U, info.for_s,
        "a round given up kept AWAKE and must not restart its clock");
    lp_down();
}

/* ---- The dark wake --------------------------------------------------------------------- */

/** @brief The dark-wake owners' ids in the cases below: A is not one. */
#define WHO_PLAIN       0U
#define WHO_NET         1U
#define WHO_RADIO       2U

/** @brief Join a plain owner, then two dark-wake owners, in that order. */
static void join_dark(void)
{
    TEST_ASSERT_EQUAL_UINT8(WHO_PLAIN, lowpower_join(ao_spy(), "A"));
    TEST_ASSERT_EQUAL_UINT8(WHO_NET, lowpower_join_dark(ao_spy(), "B"));
    TEST_ASSERT_EQUAL_UINT8(WHO_RADIO, lowpower_join_dark(ao_spy(), "C"));
}

/** @brief Answer a round for ASLEEP as all three owners, B giving its time first. */
static void rest_all(uint32_t round, uint32_t b_wake_ms)
{
    if ( 0U != b_wake_ms )
    {
        TEST_ASSERT_TRUE(lowpower_wake_at(WHO_NET,
                         khal_time_us() + ((uint64_t)b_wake_ms * 1000ULL)));
    }
    say_ready(WHO_PLAIN, round, false, NULL);
    say_ready(WHO_NET, round, false, NULL);
    say_ready(WHO_RADIO, round, false, NULL);
}

/** @brief Put the board ASLEEP with B's time @p b_wake_ms from now. */
static void radio_asleep(uint32_t b_wake_ms)
{
    join_dark();
    ask(KODE_LP_ASLEEP);
    rest_all(expect_round(KODE_LP_ASLEEP), b_wake_ms);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
}

/** @brief Fire the one-shot of B's time and insist the radio woke, alone. */
static void dark_wake(void)
{
    fire_one_shot();
    expect_state(KODE_LP_DARK, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "nothing else was published: the glass owners hear no AWAKE");
}

/** @brief Take two rounds off the spy: the dark-wake owners', and no one else's. */
static uint32_t expect_dark_rounds(void)
{
    uint32_t round = expect_round(KODE_LP_ASLEEP);

    TEST_ASSERT_EQUAL_UINT32(round, expect_round(KODE_LP_ASLEEP));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "two dark-wake owners, two rounds");
    return round;
}

/** @brief Both dark-wake owners rest: the board is ASLEEP again. */
static void dark_rest_both(uint32_t round, uint32_t b_wake_ms)
{
    if ( 0U != b_wake_ms )
    {
        TEST_ASSERT_TRUE(lowpower_wake_at(WHO_NET,
                         khal_time_us() + ((uint64_t)b_wake_ms * 1000ULL)));
    }
    say_ready(WHO_NET, round, false, NULL);
    say_ready(WHO_RADIO, round, false, NULL);
}

static void dark_done_all(void)
{
    lowpower_dark_done(WHO_NET);
    (void)qf_support_settle(AO_LowPower);
    lowpower_dark_done(WHO_RADIO);
    (void)qf_support_settle(AO_LowPower);
}

TEST_CASE("a dark-wake owner's time wakes with the glass dark: the tick and the lock, STATE(DARK), no AWAKE",
          "[lowpower_ao]")
{
    lowpower_info_t info;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running(), "asleep has no tick");
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_true);

    dark_wake();

    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running(), "the radio work needs the tick");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "and the lock");
    TEST_ASSERT_TRUE(log_has("B has work due: a dark wake"));
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_LP_ASLEEP, info.state,
                                    "the board is still ASLEEP to anyone who asks");
    TEST_ASSERT_EQUAL_UINT32(1U, info.wakes_timer);

    dark_done_all();
    dark_rest_both(expect_dark_rounds(), 0U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    lp_down();
}

TEST_CASE("the dark wake ends when every dark-wake owner is done: a round to those two alone, then ASLEEP",
          "[lowpower_ao]")
{
    uint32_t round;
    lowpower_info_t info;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();

    lowpower_dark_done(WHO_NET);
    (void)qf_support_settle(AO_LowPower);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one owner is still at work");
    lowpower_dark_done(WHO_RADIO);
    (void)qf_support_settle(AO_LowPower);

    round = expect_dark_rounds();
    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running(), "still awake until they have rested");
    dark_rest_both(round, 23U * 3600U * SECOND_MS);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);

    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running(), "the tick stops again");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_hold_false, "and the lock goes, once more");
    TEST_ASSERT_FALSE_MESSAGE(s_tick_at_release, "the tick stops before the lock is let go");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, s_spy_at_release,
                                          "the state was published before the lock was let go");
    TEST_ASSERT_TRUE_MESSAGE(log_has("dark wake done"), "and said");
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_ASLEEP, info.state);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_off_enter, "ASLEEP, not OFF");
    lp_down();
}

TEST_CASE("the minute goes on through a dark wake and after it",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    dark_done_all();
    round = expect_dark_rounds();
    dark_rest_both(round, 0U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    ao_spy_drain();
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);

    fire_one_shot();                             /* the minute, armed when the radio woke */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG),
                                   "the glass still writes its minute");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

TEST_CASE("the second time given in the dark wake's round is the next dark wake",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();                                /* at 20 s */
    dark_done_all();
    round = expect_dark_rounds();
    dark_rest_both(round, 15U * SECOND_MS);     /* 35 s */
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(15U * 1000000ULL, fake_time_once_last_delay_us(),
                                     "a one-shot for the owner's new time, nearer than the minute");
    fire_one_shot();
    expect_state(KODE_LP_DARK, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    dark_done_all();
    dark_rest_both(expect_dark_rounds(), 0U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    lp_down();
}

TEST_CASE("a dark wake that no one finishes is ended by its bound",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();

    wait_ms(KODE_LOWPOWER_DARK_MAX_MS - SECOND_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "not yet");
    wait_ms(2U * SECOND_MS);

    round = expect_dark_rounds();
    TEST_ASSERT_TRUE(log_has("are up"));
    dark_rest_both(round, 0U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    lp_down();
}

TEST_CASE("BOT during a dark wake is a whole AWAKE, and the lock is taken once",
          "[lowpower_ao]")
{
    lowpower_info_t info;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();

    expander(LEVELS_BOT, CHANGED_BOT);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "the dark wake's lock stands; no second");
    TEST_ASSERT_TRUE(fake_time_periodic_running());
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, info.state);
    TEST_ASSERT_EQUAL_UINT32(1U, info.wakes_bot);
    TEST_ASSERT_EQUAL_UINT32(1U, info.wakes_timer);

    /* A late word of a dark-wake owner is nothing to anyone now. */
    lowpower_dark_done(WHO_NET);
    (void)qf_support_settle(AO_LowPower);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

TEST_CASE("a message, a console line or a cable during a dark wake is a whole AWAKE",
          "[lowpower_ao]")
{
    MqttMsgEvt m;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    (void)memset(&m, 0, sizeof(m));
    m.super = (QEvt)QEVT_INITIALIZER(MQTT_MSG_SIG);

    dark_wake();
    feed(&m.super);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_MQTT, KODE_LP_ASLEEP);

    ao_spy_drain();
    lp_down();
    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    console_line((uint8_t)KODE_CONSOLE_SRC_BLE);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BLE, KODE_LP_ASLEEP);

    ao_spy_drain();
    lp_down();
    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    charger_reading(true);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_ASLEEP);
    lp_down();
}

TEST_CASE("BOT while the dark-wake owners are resting is a whole AWAKE, and their late answers are dropped",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    dark_done_all();
    round = expect_dark_rounds();

    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    ao_spy_drain();

    dark_rest_both(round, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the board does not go back to sleep");
    TEST_ASSERT_TRUE(fake_time_periodic_running());
    lp_down();
}

TEST_CASE("a dark-wake owner busy on the dark-wake round turns the wake into a whole AWAKE",
          "[lowpower_ao]")
{
    char why[KODE_LOWPOWER_REFUSAL_MAX];

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    ao_spy_subscribe(LOWPOWER_REFUSED_SIG);
    dark_wake();
    dark_done_all();

    say_ready(WHO_NET, expect_dark_rounds(), true, "net job");

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    expect_refused(why, sizeof(why));
    TEST_ASSERT_EQUAL_STRING("B: net job", why);
    TEST_ASSERT_TRUE(fake_time_periodic_running());
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    lp_down();
}

TEST_CASE("a dark-wake owner silent on the dark-wake round turns the wake into a whole AWAKE",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    dark_done_all();
    round = expect_dark_rounds();

    say_ready(WHO_NET, round, false, NULL);
    wait_ms(KODE_LOWPOWER_ROUND_MS + SECOND_MS);

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(log_has("C did not answer"));
    lp_down();
}

TEST_CASE("an owner that is not a dark-wake owner still gets a whole AWAKE for its time",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join_dark();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(lowpower_wake_at(WHO_PLAIN, khal_time_us() + (20ULL * 1000000ULL)));
    say_ready(WHO_PLAIN, round, false, NULL);
    say_ready(WHO_NET, round, false, NULL);
    say_ready(WHO_RADIO, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();

    fire_one_shot();

    expect_timer_wake();
    lp_down();
}

TEST_CASE("another owner's later time outlives a dark wake and still wakes the whole board",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join_dark();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(lowpower_wake_at(WHO_PLAIN, khal_time_us() + (90ULL * 1000000ULL)));
    rest_all(round, 20U * SECOND_MS);                 /* B's time is the earlier */
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();

    dark_wake();                                     /* at 20 s */
    dark_done_all();
    dark_rest_both(expect_dark_rounds(), 0U);       /* the dark-wake owners give no time */
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    ao_spy_drain();

    for ( unsigned i = 0U; (10U > i) && (ao_spy_count() == 0U); i++ )
    {   /* the minute, then A's time */
        fire_one_shot();
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    }
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_hold_true, "the dark wake's lock, then the whole wake's");
    TEST_ASSERT_TRUE(log_has("A has work due: waking for it"));
    lp_down();
}

TEST_CASE("another owner's time that comes inside a dark wake wakes the whole board",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    join_dark();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(lowpower_wake_at(WHO_PLAIN, khal_time_us() + (30ULL * 1000000ULL)));
    rest_all(round, 20U * SECOND_MS);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();

    dark_wake();                                      /* at 20 s; nobody finishes */
    fire_one_shot();                                  /* A's time, at 30 s */

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "the dark wake's lock stands; no second");
    lp_down();
}

TEST_CASE("a dark-wake owner's time and the minute due together do the minute's work too",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    radio_asleep(60U * SECOND_MS);                    /* the minute is 60 s away as well */

    fire_one_shot();

    expect_state(KODE_LP_DARK, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, queued(AO_Display, DISPLAY_MINUTE_REQ_SIG),
                                   "the glass missed its minute");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, queued(AO_Power, POWER_PEEK_REQ_SIG),
                                   "and so did the reading of the cable");

    dark_done_all();                                 /* a case never ends inside the wake */
    dark_rest_both(expect_dark_rounds(), 0U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    lp_down();
}

TEST_CASE("off_after reached during a dark wake goes to OFF without lighting the lock twice",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    set_word(KSET_KEY_OFF_AFTER, WORD_1H);
    radio_asleep(3590U * SECOND_MS);
    for ( unsigned i = 0U; (100U > i) && (ao_spy_count() == 0U); i++ )
    {   /* the minutes, up to B's time at 3590 s */
        fire_one_shot();
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    }
    expect_state(KODE_LP_DARK, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);

    for ( unsigned i = 0U; (100U > i) && (ao_spy_count() == 0U); i++ )
    {
        fire_one_shot();
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    }

    round = expect_round(KODE_LP_OFF);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "the lock was already held");
    say_ready(WHO_PLAIN, round, false, NULL);
    say_ready(WHO_NET, round, false, NULL);
    say_ready(WHO_RADIO, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_OFF_AFTER, KODE_LP_ASLEEP);
    lp_down();
}

TEST_CASE("a dark-wake owner's word outside a dark wake is dropped",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);

    lowpower_dark_done(WHO_NET);
    (void)qf_support_settle(AO_LowPower);

    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running(), "it woke nothing");
    lp_down();
}

/* ---- A dark wake's states no event could carry ----------------------------------------- */

/**
 * @brief Hand AO_LowPower an answer without taking a block from the pool.
 *
 * lowpower_ready() allocates, and a case that has run the pool dry still has
 * to answer.
 *
 * @param who   The participant.
 * @param round The round answered.
 */
static void feed_ready(uint8_t who, uint32_t round)
{
    LowPowerReadyEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_READY_SIG);
    a.who   = who;
    a.round = round;
    feed(&a.super);
}

/* Net and Radio start their work on STATE(DARK) and on nothing else: a wake
 * whose event the pool could not carry is published again on the timer, which
 * runs because the tick is back. */
TEST_CASE("STATE(DARK) the pool could not carry is published again, once",
          "[lowpower_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);

    size_t const n = pool_take_all(held);
    fire_one_shot();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the pool was not dry");
    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running(), "the retry needs the tick");
    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a retry went out of a dry pool");

    pool_give_back(held, n);
    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 2U);
    expect_state(KODE_LP_DARK, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(log_has("DARK published late"));

    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the retry went on after it was said");

    dark_done_all();                                 /* a case never ends inside the wake */
    dark_rest_both(expect_dark_rounds(), 0U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    lp_down();
}

/* A rest must not begin with Net and Radio still in DARK: when no event can
 * carry the ASLEEP that ends the wake, the wake keeps the tick and the lock,
 * and lets go only after the state went out. */
TEST_CASE("the dark wake keeps the lock until its ASLEEP is published",
          "[lowpower_ao]")
{
    const QEvt     *held[POOL_HOLD_MAX];
    lowpower_info_t info;
    uint32_t        round;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    dark_done_all();
    round = expect_dark_rounds();
    say_ready(WHO_NET, round, false, NULL);

    size_t const n = pool_take_all(held);
    feed_ready(WHO_RADIO, round);                     /* the last answer: the wake ends */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the pool was not dry");
    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running(), "the tick stopped before the state");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_false, "the lock went before the state");

    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a retry went out of a dry pool");
    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running(), "the tick stopped on a dry pool");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_false, "the lock went on a dry pool");
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_ASLEEP, info.state);

    pool_give_back(held, n);
    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 2U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running(), "the tick stops once it is said");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_hold_false, "and the lock goes, once");
    TEST_ASSERT_FALSE_MESSAGE(s_tick_at_release, "the tick stops before the lock is let go");
    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, s_spy_at_release,
                                          "the state was published before the lock was let go");
    TEST_ASSERT_TRUE(log_has("ASLEEP published late"));

    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the retry went on after it was said");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_hold_false, "the lock went twice");
    lp_down();
}

/* The owed ASLEEP is the news of a rest that did not happen if the board wakes
 * whole first: AWAKE alone is said, the lock stands once and is not taken twice. */
TEST_CASE("a whole AWAKE ends the dark wake that was waiting to publish its ASLEEP",
          "[lowpower_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];
    uint32_t    round;

    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    dark_done_all();
    round = expect_dark_rounds();
    say_ready(WHO_NET, round, false, NULL);

    size_t const n = pool_take_all(held);
    feed_ready(WHO_RADIO, round);
    pool_give_back(held, n);
    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(fake_time_periodic_running());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "the lock was taken twice");

    wait_ms(KODE_LOWPOWER_STATE_RETRY_MS * 5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a stale ASLEEP went out over AWAKE");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_false, "the lock was let go");
    lp_down();
}

/* ---- Rest once an owner's work is done ------------------------------------------------- */

/** @brief Put the board ASLEEP and wake it for an owner's time: AWAKE, cause TIMER. */
static void timer_wake(void)
{
    go_asleep();
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
    wake_in(0U, 10U * SECOND_MS);
    fire_one_shot();
    expect_timer_wake();
    ao_spy_drain();
}

/** @brief The scheduler asks to rest, and AO_LowPower hears it. */
static void ask_rest(void)
{
    TEST_ASSERT_TRUE(lowpower_request_rest(KODE_LOWPOWER_SRC_SCHED));
    (void)qf_support_settle(AO_LowPower);
}

/** @brief Take the round for ASLEEP and answer both participants ready. */
static void rest_round_agreed(void)
{
    uint32_t const round = expect_round(KODE_LP_ASLEEP);

    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
}

TEST_CASE("test_lp_rest_after_timer_wake", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    watch_refusals();
    timer_wake();

    ask_rest();

    rest_round_agreed();
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one answer, and no refusal beside it");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("test_lp_rest_after_input", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    watch_refusals();
    timer_wake();
    touch_event();

    ask_rest();

    expect_refused_for(KODE_LOWPOWER_SRC_SCHED, KODE_LOWPOWER_WHY_USED);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "exactly one answer, and no round");
    TEST_ASSERT_TRUE(log_has("the board is in use"));
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("each kind of input after a timer wake keeps the board from resting", "[lowpower_ao]")
{
    static const uint8_t ports[] = {
        (uint8_t)KODE_CONSOLE_SRC_SERIAL, (uint8_t)KODE_CONSOLE_SRC_BLE,
        (uint8_t)KODE_CONSOLE_SRC_NET,
    };

    for ( size_t i = 0U; Q_DIM(ports) > i; i++ )
    {
        lp_up(KODE_OK);
        timer_wake();
        console_line(ports[i]);
        ask_rest();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a console line is use of the board");
        lp_down();
    }

    lp_up(KODE_OK);
    timer_wake();
    button_event();
    ask_rest();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a button is use of the board");
    lp_down();

    lp_up(KODE_OK);
    timer_wake();
    charger_reading(true);
    charger_reading(false);
    ask_rest();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a cable that came and went is use of the board");
    lp_down();
}

TEST_CASE("a wake for BOT, and the boot, are use of the board", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(2U);
    ask_rest();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the boot is not an owner's time");
    lp_down();

    lp_up(KODE_OK);
    go_asleep();
    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    ask_rest();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a press of BOT woke it");
    lp_down();
}

TEST_CASE("a rest asked asleep or in a round is ignored", "[lowpower_ao]")
{
    lowpower_info_t info;
    uint32_t        round;

    lp_up(KODE_OK);
    go_asleep();
    watch_refusals();
    ask_rest();
    expect_refused_for(KODE_LOWPOWER_SRC_SCHED, "request: board not AWAKE");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "asleep: one refusal, nothing else");
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_ASLEEP, info.state);
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();

    lp_up(KODE_OK);
    timer_wake();
    watch_refusals();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    ask_rest();
    expect_refused_for(KODE_LOWPOWER_SRC_SCHED, KODE_LOWPOWER_WHY_RUNNING);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a round is running: one refusal, it stands");
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and the round's own answer is the state");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a person who comes while the rest's round runs ends it", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    watch_refusals();
    timer_wake();
    ask_rest();
    (void)expect_round(KODE_LP_ASLEEP);

    touch_event();

    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    expect_refused_for(KODE_LOWPOWER_SRC_SCHED, "input: board used");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a person who comes while the rest waits to begin again ends it", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    watch_refusals();
    timer_wake();
    ask_rest();
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the rest waits, nobody is answered");

    touch_event();

    expect_refused_for(KODE_LOWPOWER_SRC_SCHED, "input: board used");
    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS + 100U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no round begins under the thumb");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("test_lp_sched_line_not_input", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    join(1U);
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);

    wait_ms(10U * SECOND_MS);
    console_line(KODE_CONSOLE_SRC_SCHED);        /* would move the deadline to 40 s */
    wait_ms(10U * SECOND_MS);
    lowpower_input_note(KODE_CONSOLE_SRC_SCHED); /* the facade must not call it a cable */
    (void)qf_support_settle(AO_LowPower);
    wait_ms(9U * SECOND_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the idle time ran to 30 s");

    wait_ms(2U * SECOND_MS);                     /* t = 31 s */
    say_ready(0U, expect_round(KODE_LP_ASLEEP), false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("a scheduled line is neither use of the board nor a wake", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    timer_wake();

    console_line(KODE_CONSOLE_SRC_SCHED);
    ask_rest();
    rest_round_agreed();
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    console_line(KODE_CONSOLE_SRC_SCHED);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "asleep, a scheduled line wakes nothing");
    lp_down();
}

TEST_CASE("a scheduled line does not end the rest's round", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    timer_wake();
    ask_rest();
    (void)expect_round(KODE_LP_ASLEEP);

    console_line(KODE_CONSOLE_SRC_SCHED);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the round stands");
    lp_down();
}

TEST_CASE("a rest that cannot be posted says so by returning false", "[lowpower_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];
    bool        posted;

    lp_up(KODE_OK);
    timer_wake();

    size_t const n = pool_take_all(held);
    posted = lowpower_request_rest(KODE_LOWPOWER_SRC_SCHED);
    pool_give_back(held, n);

    TEST_ASSERT_FALSE(posted);
    lp_down();
}

TEST_CASE("a rest yields to a person's request that waits to begin again", "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    timer_wake();
    watch_refusals();
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, true, "radio busy");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the request waits, nobody is answered");

    ask_rest();

    expect_refused_for(KODE_LOWPOWER_SRC_SCHED, KODE_LOWPOWER_WHY_PENDING);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one refusal, and the requester is not told");
    TEST_ASSERT_EQUAL_STRING_MESSAGE("B: radio busy", lowpower_last_refusal(),
                                     "the waiting request keeps the refusal it will be answered with");

    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS + 100U);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(round + 1U, expect_round(KODE_LP_ASLEEP),
                                     "the request still begins again");
    say_ready(0U, round + 1U, false, NULL);
    say_ready(1U, round + 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

/* ---- The wakes nobody used, and the rest's other paths -------------------------------- */

/** @brief Minutes between the entry's wakes in the off-after cases. */
#define ENTRY_EVERY_MIN 20U
/** @brief Milliseconds in a minute. */
#define MINUTE_MS       (60U * SECOND_MS)
/** @brief How long the entry's work keeps each wake awake, milliseconds. */
#define ENTRY_WORK_MS   (5U * SECOND_MS)

/** @brief Fire the one-shots, the minute's work drained, until something is published. */
static void sleep_until_news(void)
{
    unsigned n = 0U;

    while ( (ao_spy_count() == 0U) && (200U > n) )
    {
        fire_one_shot();
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        n++;
    }
}

/** @brief The rest the scheduler asks after its work, with its next time in the round. */
static void rest_with_next(uint32_t next_ms)
{
    uint32_t round;

    ask_rest();
    round = expect_round(KODE_LP_ASLEEP);
    wake_in(1U, next_ms);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();
}

/* test_lp_off_after_with_entries */
TEST_CASE("an entry that wakes the board unattended does not keep off_after away",
          "[lowpower_ao]")
{
    uint64_t t0;
    uint32_t round;

    lp_up(KODE_OK);
    set_word(KSET_KEY_OFF_AFTER, WORD_1H);
    join(2U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    wake_in(1U, ENTRY_EVERY_MIN * MINUTE_MS);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();
    t0 = khal_time_us();

    /* Two wakes for the entry, nobody at the board: each works, then rests. */
    for ( unsigned i = 0U; 2U > i; i++ )
    {
        sleep_until_news();
        expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
        ao_spy_drain();
        wait_real(ENTRY_WORK_MS);
        rest_with_next(ENTRY_EVERY_MIN * MINUTE_MS);
    }

    /* The hour is counted from the first sleep, across both wakes. */
    sleep_until_news();
    round = expect_round(KODE_LP_OFF);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)MINUTE_MS * 60U * 1000U, khal_time_us() - t0,
                                     "off_after came an hour after the first sleep");
    TEST_ASSERT_TRUE(log_has("off-after reached"));
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_OFF_AFTER, KODE_LP_ASLEEP);
    wait_ms(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
    lp_down();
}

TEST_CASE("a wake somebody used starts the off_after count again", "[lowpower_ao]")
{
    uint64_t t1;
    uint32_t round;

    lp_up(KODE_OK);
    set_word(KSET_KEY_OFF_AFTER, WORD_1H);
    join(2U);
    ask(KODE_LP_ASLEEP);
    round = expect_round(KODE_LP_ASLEEP);
    wake_in(1U, ENTRY_EVERY_MIN * MINUTE_MS);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    ao_spy_drain();

    sleep_until_news();
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    ao_spy_drain();
    touch_event();                               /* somebody picks it up */
    sleep_now();
    t1 = khal_time_us();

    sleep_until_news();
    (void)expect_round(KODE_LP_OFF);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)MINUTE_MS * 60U * 1000U, khal_time_us() - t1,
                                     "an hour from the sleep after the use");
    lp_down();
}

TEST_CASE("a press of BOT after a timer wake is use of the board", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    watch_refusals();
    timer_wake();
    expander(LEVELS_BOT, CHANGED_BOT);

    ask_rest();

    expect_refused_for(KODE_LOWPOWER_SRC_SCHED, KODE_LOWPOWER_WHY_USED);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one refusal, and no round");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a rest that meets a busy owner begins again, goes through, and is answered once",
          "[lowpower_ao]")
{
    uint32_t round;

    lp_up(KODE_OK);
    watch_refusals();
    timer_wake();
    ask_rest();
    round = expect_round(KODE_LP_ASLEEP);
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, true, "check dwell");
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no answer while the rest waits");

    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS + 100U);
    TEST_ASSERT_EQUAL_UINT32(round + 1U, expect_round(KODE_LP_ASLEEP));
    say_ready(0U, round + 1U, false, NULL);
    say_ready(1U, round + 1U, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one answer, the state, and no refusal");
    QActive_unsubscribe(ao_spy(), LOWPOWER_REFUSED_SIG);
    lp_down();
}

TEST_CASE("a scheduled line's note through the facade wakes nothing asleep", "[lowpower_ao]")
{
    lowpower_info_t info;
    unsigned held;

    lp_up(KODE_OK);
    go_asleep();
    held = s_hold_true;

    lowpower_input_note(KODE_CONSOLE_SRC_SCHED);
    (void)qf_support_settle(AO_LowPower);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no STATE, no round");
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_ASLEEP, info.state);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(held, s_hold_true, "the lock was not taken back");
    lp_down();
}

/* ---- An app's rest, part by part --------------------------------------------------- */

/** @brief The ids the owners of the parts join with, in join_owners()'s order. */
#define PART_WHO_AUDIO    0U
#define PART_WHO_STORAGE  1U
#define PART_WHO_TOUCH    2U
#define PART_WHO_DISPLAY  3U
#define PART_WHO_RADIO    4U
#define PART_WHO_IOEXP    5U

/** @brief The expander byte with the touch line low and every button up. */
#define LEVELS_TOUCH   0xBFU
/** @brief The edge of the touch line. */
#define CHANGED_TOUCH  0x40U

/** @brief How far from a step's budget the cases look, either side of it. */
#define STEP_SLACK_MS  100U

/** @brief The bytes of "screen: ", which a glass's refusal begins with. */
#define SCREEN_PREFIX_LEN  8U

/** @brief One-shots a case fires at most on the way to a wake time. */
#define HOP_FIRES_MAX  12U

/** @brief A position of the expansion header the cases wake on. */
#define WAKE_POS       3U

static khal_power_wake_cb_t s_wake_cb;   /**< The callback the last arming gave.     */
static uint32_t s_wake_gpio;             /**< The pin of the last arming.            */
static bool     s_wake_level;            /**< Its level.                             */
static unsigned s_wake_arms;             /**< khal_power_wake_arm() calls.           */
static unsigned s_wake_disarms;          /**< khal_power_wake_disarm() calls.        */
static kode_status_t s_wake_rc;          /**< What khal_power_wake_arm() answers.    */

static kode_status_t on_wake_arm(uint32_t gpio, bool level_high, khal_power_wake_cb_t cb,
                                 void *arg, int calls)
{
    (void)arg;
    (void)calls;
    s_wake_arms++;
    s_wake_gpio  = gpio;
    s_wake_level = level_high;
    s_wake_cb    = cb;
    return s_wake_rc;
}

static void on_wake_disarm(uint32_t gpio, int calls)
{
    (void)gpio;
    (void)calls;
    s_wake_disarms++;
}

/** @brief Bring AO_LowPower up with the owners of every part joined, and listen to the rest's news. */
static void rest_up(void)
{
    static const char * const names[] = {
        "audio", "storage", "touch", "display", "radio", "ioexp"
    };

    lp_up(KODE_OK);
    ao_spy_subscribe(LOWPOWER_PART_SIG);
    khal_power_wake_arm_Stub(&on_wake_arm);
    khal_power_wake_disarm_Stub(&on_wake_disarm);
    /* The header's table drives its pins through khal_gpio as it changes hands. */
    khal_gpio_input_IgnoreAndReturn(KODE_OK);
    khal_gpio_release_Ignore();
    s_wake_cb      = NULL;
    s_wake_gpio    = 0U;
    s_wake_level   = false;
    s_wake_arms    = 0U;
    s_wake_disarms = 0U;
    s_wake_rc      = KODE_OK;
    for ( uint8_t i = 0U; Q_DIM(names) > i; i++ )
    {
        TEST_ASSERT_EQUAL_UINT8(i, lowpower_join(ao_spy(), names[i]));
    }
}

/** @brief Post a request for parts from @p src and let AO_LowPower take it. */
static void rest_post_from(const lowpower_part_req_t *req, uint8_t src)
{
    QEvt const * const e = lowpower_part_req_evt(req, src);

    TEST_ASSERT_NOT_NULL(e);
    QACTIVE_POST(AO_LowPower, e, NULL);
    (void)qf_support_settle(AO_LowPower);
}

/** @brief Post a request for parts as a console port does. */
static void rest_post(const lowpower_part_req_t *req)
{
    rest_post_from(req, SRC_TEST);
}

/** @brief Ask for @p parts with no wake source. */
static void rest_ask(uint8_t parts)
{
    lowpower_part_req_t const req = { .parts = parts };

    rest_post(&req);
}

/** @brief Ask for every part back. */
static void rest_end(void)
{
    lowpower_part_req_t const req = { .end = true };

    rest_post(&req);
}

/** @brief Take the next event and insist it is the round posted for @p part alone. */
static uint32_t expect_part_round(uint8_t part)
{
    const LowPowerRoundEvt *r = (const LowPowerRoundEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no owner was asked");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_ROUND_SIG, (int)r->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_ASLEEP, r->target);
    TEST_ASSERT_EQUAL_HEX8(part, r->parts);
    return r->round;
}

/** @brief Take the next event and insist it is this news of the rest. */
static const LowPowerPartEvt *expect_news(kode_lp_part_what_t what, kode_status_t rc,
                                          uint8_t parts)
{
    const LowPowerPartEvt *n = (const LowPowerPartEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(n, "no news of the rest was published");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_PART_SIG, (int)n->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)what, n->what);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)rc, n->rc);
    TEST_ASSERT_EQUAL_HEX8(parts, n->parts);
    return n;
}

/** @brief Take the next event and insist it is the AWAKE posted to one owner. */
static void expect_awake_posted(void)
{
    const LowPowerStateEvt *s = (const LowPowerStateEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(s, "no owner was told to come back");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_STATE_SIG, (int)s->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, s->state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, s->prev);
}

/**
 * @brief The owner of @p part answers busy at every ask, at once, until the request gives up.
 *
 * An ask a second while the next one falls inside the window; each must be
 * the round for @p part alone, and nothing else may come between two asks.
 *
 * @param who  The owner's participant id.
 * @param part The part it is asked for.
 * @param why  Its reason.
 * @return How many times it was asked.
 */
static uint32_t say_busy_to_the_end(uint8_t who, uint8_t part, const char *why)
{
    uint32_t asks = 0U;

    for ( ;; )
    {
        say_ready(who, expect_part_round(part), true, why);
        asks++;
        TEST_ASSERT_LESS_OR_EQUAL_UINT32_MESSAGE(
            KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS / KODE_LOWPOWER_REQUEST_RETRY_MS, asks,
            "asked again past the window");
        if ( 0U != ao_spy_count() )
        {
            break;
        }
        wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS);
    }
    return asks;
}

/** @brief Hand AO_LowPower the glass's answer to a request to sleep or wake. */
static void glass_says(bool asleep, kode_status_t rc, display_sleep_why_t why)
{
    DisplaySleepEvt s;

    (void)memset(&s, 0, sizeof(s));
    s.super  = (QEvt)QEVT_INITIALIZER(DISPLAY_SLEEP_SIG);
    s.asleep = asleep;
    s.rc     = (uint8_t)rc;
    s.why    = (uint8_t)why;
    s.who    = DISPLAY_SLEEP_WHO_REST;
    feed(&s.super);
}

/** @brief Take the next event and insist it asks the glass to sleep or to wake. */
static void expect_glass_asked(bool sleep)
{
    const DisplaySleepReqEvt *r = (const DisplaySleepReqEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "the glass was not asked");
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_SLEEP_REQ_SIG, (int)r->super.sig);
    TEST_ASSERT_EQUAL_INT(sleep, r->sleep);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(DISPLAY_SLEEP_WHO_REST, r->who, "asked through the app's door");
}

/** @brief Put the screen and the CPU at rest with the wake sources of @p req. */
static void cpu_rest(lowpower_part_req_t req)
{
    req.parts = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    rest_post(&req);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN | KODE_LP_PART_CPU);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);
}

/** @brief Give up the board's round a case left open, so no round's timer outlives the case. */
static void round_cancel(void)
{
    ask(KODE_LP_AWAKE);
    ao_spy_drain();
}

/** @brief The parts a task reads as at rest. */
static uint8_t parts_read(void)
{
    lowpower_part_last_t last;

    lowpower_part_last(&last);
    return last.parts;
}

TEST_CASE("a rest of the sound asks the audio owner alone and publishes no state", "[lowpower_ao]")
{
    lowpower_part_last_t last;
    uint32_t round;

    lowpower_part_req_t const req = { .parts = KODE_LP_PART_SOUND | KODE_LP_PART_TOUCH };

    rest_up();
    rest_post_from(&req, (uint8_t)KODE_LOWPOWER_SRC_APP);

    round = expect_part_round(KODE_LP_PART_SOUND);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one owner is asked, and nothing is published");
    lowpower_part_last(&last);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, last.parts, "not at rest until the owner says so");
    TEST_ASSERT_TRUE_MESSAGE(last.open, "a task reads that a request is being arranged");

    say_ready(PART_WHO_AUDIO, round, false, NULL);
    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);

    /* The sound is put away and held nowhere: it comes back at its next use. */
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_TOUCH);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the board's state did not change");
    lowpower_part_last(&last);
    TEST_ASSERT_EQUAL_UINT16(1U, last.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, last.rc);
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_TOUCH, last.parts);
    TEST_ASSERT_FALSE(last.open);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_false, "the lock is held: the board is AWAKE");
    lp_down();
}

TEST_CASE("the parts are asked one at a time, each after the one before answered", "[lowpower_ao]")
{
    rest_up();
    rest_ask(KODE_LP_PART_TOUCH | KODE_LP_PART_SOUND | KODE_LP_PART_CARD);

    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "only the next owner is asked");
    say_ready(PART_WHO_STORAGE, expect_part_round(KODE_LP_PART_CARD), false, NULL);
    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);

    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_TOUCH);
    lp_down();
}

/* As a requested round is begun again: a second after the busy answer, the
 * same owner is asked again in a round of its own, and nobody is answered
 * meanwhile. */
TEST_CASE("a busy owner is asked again a second later, and its part rests then", "[lowpower_ao]")
{
    lowpower_part_last_t last;
    uint32_t refused;
    uint32_t again;

    rest_up();
    rest_ask(KODE_LP_PART_SOUND | KODE_LP_PART_RADIO);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    refused = expect_part_round(KODE_LP_PART_RADIO);
    say_ready(PART_WHO_RADIO, refused, true, "radio busy");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a busy owner ended the rest at once");
    lowpower_part_last(&last);
    TEST_ASSERT_TRUE_MESSAGE(last.open, "a task reads that the rest is still being arranged");

    /* A second answer to the round it refused is not the step's. */
    say_ready(PART_WHO_RADIO, refused, false, NULL);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());

    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS - STEP_SLACK_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "asked again before its wait was over");
    wait_real(STEP_SLACK_MS);
    again = expect_part_round(KODE_LP_PART_RADIO);
    TEST_ASSERT_NOT_EQUAL_MESSAGE(refused, again, "the owner is asked in a round of a new number");

    say_ready(PART_WHO_RADIO, again, false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_RADIO);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

TEST_CASE("an owner still busy at the end of the window stops the rest there: its part is named "
          "and what rested stays", "[lowpower_ao]")
{
    const LowPowerPartEvt *n;
    lowpower_info_t info;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH);
    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);
    ao_spy_drain();
    rest_ask(KODE_LP_PART_SOUND | KODE_LP_PART_CARD | KODE_LP_PART_SCREEN);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);

    TEST_ASSERT_EQUAL_UINT32(KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS / KODE_LOWPOWER_REQUEST_RETRY_MS,
                             say_busy_to_the_end(PART_WHO_STORAGE, KODE_LP_PART_CARD, "file write"));
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, KODE_LP_PART_TOUCH);
    TEST_ASSERT_EQUAL_STRING("card: file write", n->why);
    TEST_ASSERT_EQUAL_UINT8(SRC_TEST, n->src);
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_STRING("card: file write", info.part_why);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", lowpower_part_why(), "a console's refusal is not the app's");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the screen is never asked");
    wait_real(2U * KODE_LOWPOWER_REQUEST_RETRY_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the rest stays given up");
    lp_down();
}

TEST_CASE("an owner that does not answer is named when its time is up", "[lowpower_ao]")
{
    const LowPowerPartEvt *n;

    rest_up();
    rest_ask(KODE_LP_PART_SOUND);
    (void)expect_part_round(KODE_LP_PART_SOUND);

    wait_ms(KODE_LOWPOWER_PART_STEP_MS - STEP_SLACK_MS);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    wait_ms(2U * STEP_SLACK_MS);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("sound: no answer", n->why);
    lp_down();
}

TEST_CASE("the radio without the sound is refused whole and nobody is asked", "[lowpower_ao]")
{
    const LowPowerPartEvt *n;

    rest_up();
    rest_ask(KODE_LP_PART_RADIO | KODE_LP_PART_CARD);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("radio: needs sound at rest", n->why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no owner was asked");
    lp_down();
}

TEST_CASE("the sound rests before the radio is asked", "[lowpower_ao]")
{
    rest_up();
    rest_ask(KODE_LP_PART_RADIO | KODE_LP_PART_SOUND);

    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    say_ready(PART_WHO_RADIO, expect_part_round(KODE_LP_PART_RADIO), false, NULL);

    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_RADIO);
    lp_down();
}

TEST_CASE("the screen is asked through the glass's own door, and rests on its answer", "[lowpower_ao]")
{
    rest_up();
    rest_ask(KODE_LP_PART_SCREEN);

    expect_glass_asked(true);
    /* The answer of somebody's earlier wake is not this step's. */
    glass_says(false, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);

    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN);
    lp_down();
}

TEST_CASE("a glass that refuses says why in the answer", "[lowpower_ao]")
{
    const LowPowerPartEvt *n;

    rest_up();
    rest_ask(KODE_LP_PART_SCREEN);
    expect_glass_asked(true);

    glass_says(false, KODE_DENIED, DISPLAY_SLEEP_WHY_LENT);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING_LEN("screen: ", n->why, SCREEN_PREFIX_LEN);
    /* The glass's own words, as many as the answer holds. */
    TEST_ASSERT_EQUAL_STRING_LEN(display_sleep_why_text((uint8_t)DISPLAY_SLEEP_WHY_LENT),
                                 &n->why[SCREEN_PREFIX_LEN], strlen(&n->why[SCREEN_PREFIX_LEN]));
    TEST_ASSERT_TRUE(strlen(n->why) > SCREEN_PREFIX_LEN);
    lp_down();
}

TEST_CASE("the panel takes the touch and the screen first, then its supply", "[lowpower_ao]")
{
    const DisplaySupplyReqEvt *q;
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_PANEL);

    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    q = (const DisplaySupplyReqEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(q, "the supply was not asked for");
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_SUPPLY_REQ_SIG, (int)q->super.sig);
    round = q->round;
    say_ready(PART_WHO_DISPLAY, round, false, NULL);

    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK,
                      KODE_LP_PART_PANEL | KODE_LP_PART_SCREEN | KODE_LP_PART_TOUCH);
    lp_down();
}

TEST_CASE("a part whose owner is not in the build is at rest without asking", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    ao_spy_subscribe(LOWPOWER_PART_SIG);
    TEST_ASSERT_EQUAL_UINT8(0U, lowpower_join(ao_spy(), "audio"));

    rest_ask(KODE_LP_PART_SOUND | KODE_LP_PART_CARD | KODE_LP_PART_SCREEN);
    say_ready(0U, expect_part_round(KODE_LP_PART_SOUND), false, NULL);

    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN);
    lp_down();
}

/* A task reads the reason after it has seen its answer: the console's answer
 * in between must not have written over it. */
TEST_CASE("the reason an app's request was refused outlives a console's answer", "[lowpower_ao]")
{
    lowpower_part_req_t const card = { .parts = KODE_LP_PART_CARD };
    lowpower_info_t info;

    rest_up();
    rest_post_from(&card, (uint8_t)KODE_LOWPOWER_SRC_APP);
    (void)say_busy_to_the_end(PART_WHO_STORAGE, KODE_LP_PART_CARD, "file write");
    ao_spy_drain();

    rest_ask(KODE_LP_PART_TOUCH);
    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_STRING("card: file write", lowpower_part_why());
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", info.part_why, "the console's request stood");
    lp_down();
}

/* The console's sequence is open, so no task is waiting for an answer of its
 * own: the app's refusal can be counted, and its task hears it at once. */
TEST_CASE("an app's request that finds a console's rest being arranged is answered to its task",
          "[lowpower_ao]")
{
    lowpower_part_req_t const card = { .parts = KODE_LP_PART_CARD };
    lowpower_part_last_t last;
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH);
    round = expect_part_round(KODE_LP_PART_TOUCH);

    rest_post_from(&card, (uint8_t)KODE_LOWPOWER_SRC_APP);

    lowpower_part_last(&last);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(1U, last.answers, "the task waits its whole bound for nothing");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, last.rc);
    TEST_ASSERT_EQUAL_STRING("rest: underway", lowpower_part_why());
    say_ready(PART_WHO_TOUCH, round, false, NULL);
    ao_spy_drain();
    lp_down();
}

/* kode_sys_screen_sleep(false) wakes a glass the rest put to sleep: the DSI
 * link is up and holds the P4 awake, so the screen is no longer a part at rest. */
TEST_CASE("a glass the app woke through its own door is no longer held, and the CPU part is refused",
          "[lowpower_ao]")
{
    lowpower_part_req_t const cpu = { .parts = KODE_LP_PART_CPU };
    const LowPowerPartEvt *n;
    DisplaySleepEvt s;

    rest_up();
    rest_ask(KODE_LP_PART_SCREEN);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN);

    (void)memset(&s, 0, sizeof(s));
    s.super  = (QEvt)QEVT_INITIALIZER(DISPLAY_SLEEP_SIG);
    s.asleep = false;
    s.rc     = (uint8_t)KODE_OK;
    s.who    = DISPLAY_SLEEP_WHO_APP;
    feed(&s.super);

    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, parts_read(), "a glass that is awake still reads as at rest");
    rest_post(&cpu);
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("cpu: needs the screen at rest", n->why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_false, "the P4 was told to sleep with the link up");
    lp_down();
}

TEST_CASE("a second request while one is being arranged is refused, and the first goes on",
          "[lowpower_ao]")
{
    const LowPowerPartEvt *n;
    uint32_t round;

    lowpower_part_req_t const req = { .parts = KODE_LP_PART_TOUCH };
    lowpower_part_last_t last;

    rest_up();
    rest_post_from(&req, (uint8_t)KODE_LOWPOWER_SRC_APP);
    round = expect_part_round(KODE_LP_PART_TOUCH);

    /* The refusal is told to its asker on the bus, and what the task that
     * waits for the first reads does not move: no answer of its own yet. */
    rest_ask(KODE_LP_PART_CARD);
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("rest: underway", n->why);
    TEST_ASSERT_EQUAL_UINT8(SRC_TEST, n->src);
    lowpower_part_last(&last);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, last.answers, "another's refusal counted as the task's answer");
    TEST_ASSERT_TRUE(last.open);

    say_ready(PART_WHO_TOUCH, round, false, NULL);
    n = expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_TOUCH);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOWPOWER_SRC_APP, n->src);
    lowpower_part_last(&last);
    TEST_ASSERT_EQUAL_UINT16(1U, last.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, last.rc);
    lp_down();
}

TEST_CASE("the end tells the owners that restore, the CPU side first, and forgets the rest",
          "[lowpower_ao]")
{
    rest_up();
    rest_ask(KODE_LP_PART_SOUND | KODE_LP_PART_CARD | KODE_LP_PART_TOUCH | KODE_LP_PART_RADIO);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    say_ready(PART_WHO_STORAGE, expect_part_round(KODE_LP_PART_CARD), false, NULL);
    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);
    say_ready(PART_WHO_RADIO, expect_part_round(KODE_LP_PART_RADIO), false, NULL);
    ao_spy_drain();

    rest_end();

    /* The radio and the touch are told; the sound and the card come back at their next use. */
    expect_awake_posted();
    expect_awake_posted();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

TEST_CASE("the end wakes the glass and waits for it", "[lowpower_ao]")
{
    rest_up();
    rest_ask(KODE_LP_PART_SCREEN);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    ao_spy_drain();

    rest_end();
    expect_glass_asked(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "not answered until the glass is back");
    glass_says(false, KODE_OK, DISPLAY_SLEEP_WHY_NONE);

    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    lp_down();
}

TEST_CASE("the CPU part answers first, then stops the tick and lets the lock go", "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };

    rest_up();
    cpu_rest(req);

    TEST_ASSERT_EQUAL_UINT(1U, fake_time_periodic_stops());
    TEST_ASSERT_FALSE_MESSAGE(s_tick_at_release, "the tick stops before the lock is let go");
    TEST_ASSERT_TRUE_MESSAGE(1U <= s_spy_at_release, "the answer is published before the lock goes");
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_SCREEN | KODE_LP_PART_CPU, parts_read());
    lp_down();
}

TEST_CASE("a button ends the CPU part alone: the tick and the lock back, the screen still away",
          "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_TRUE_MESSAGE(s_tick_at_hold, "the tick runs before the lock is taken");
    expect_awake_posted();   /* the expander's owner: its line is the plain interrupt again */
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_BUTTON, n->cause);
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_SCREEN, parts_read());
    lp_down();
}

TEST_CASE("a touch ends the CPU part only when the app asked for it", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();
    expander(LEVELS_TOUCH, CHANGED_TOUCH);
    touch_event();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_true, "a touch nobody asked for wakes nothing");
    lp_down();

    rest_up();
    req.wake_touch = true;
    cpu_rest(req);
    ao_spy_drain();
    expander(LEVELS_TOUCH, CHANGED_TOUCH);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_TOUCH, n->cause);
    lp_down();
}

TEST_CASE("the time the app gave ends the CPU part", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    req.wake_after_ms = 5000U;
    cpu_rest(req);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_time_once_pending(), "the time, and the minute");

    fire_one_shot();                             /* the one due first: the app's time */

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_TIMER, n->cause);
    lp_down();
}

/* No one-shot can be taken back, so a long time is reached in hops of a
 * minute: one that outlives its rest is gone within that. */
TEST_CASE("a wake time longer than a hop is reached hop by hop, and wakes only when it comes",
          "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;

    uint64_t const after_us = (uint64_t)((2U * KODE_LOWPOWER_PART_HOP_MS) + 5000U) * 1000ULL;
    uint64_t t0;

    rest_up();
    req.wake_after_ms = (2U * KODE_LOWPOWER_PART_HOP_MS) + 5000U;
    t0 = khal_time_us();
    cpu_rest(req);
    ao_spy_drain();

    /* Hops and minutes come due on the way; the fake's clock moves to each. */
    for ( unsigned i = 0U; (HOP_FIRES_MAX > i) && (0U == s_hold_true); i++ )
    {
        TEST_ASSERT_LESS_OR_EQUAL_UINT64_MESSAGE(t0 + after_us, khal_time_us(),
                                                 "the wake time passed and nothing woke");
        fire_one_shot();
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    }
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(t0 + after_us, khal_time_us(), "woken before its time");
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_TIMER, n->cause);
    lp_down();
}

/* A rest woken early leaves its one-shot out. The next rest's time is served
 * by a one-shot of its own, and the old one's event wakes nothing. */
TEST_CASE("the one-shot of a rest that ended early does not wake the next rest", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };

    rest_up();
    req.wake_after_ms = 3000U;
    cpu_rest(req);
    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);     /* woken by a button: its one-shot stays out */
    ao_spy_drain();

    req.parts         = KODE_LP_PART_CPU;
    req.wake_after_ms = 20000U;
    rest_post(&req);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT(2U, s_hold_false);

    fake_time_advance_ms(3000U);
    fire_one_shot();                             /* the first rest's */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "an old one-shot woke the new rest");

    fake_time_advance_ms(17000U);
    fire_one_shot();
    TEST_ASSERT_EQUAL_UINT(2U, s_hold_true);
    lp_down();
}

TEST_CASE("the header's line is armed at its level, wakes the CPU part and is let go", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;
    khal_exp_info_t info;

    rest_up();
    khal_exp_info(WAKE_POS, &info);
    req.wake_pin   = WAKE_POS;
    req.wake_level = true;
    cpu_rest(req);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT(1U, s_wake_arms);
    TEST_ASSERT_EQUAL_UINT32(info.gpio, s_wake_gpio);
    TEST_ASSERT_TRUE(s_wake_level);
    TEST_ASSERT_NOT_NULL(s_wake_cb);

    s_wake_cb(NULL);
    (void)qf_support_settle(AO_LowPower);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_wake_disarms, "the line is given back after the wake");
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_PIN, n->cause);
    lp_down();
}

TEST_CASE("a line that will not arm refuses the CPU part and the tick keeps running", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    s_wake_rc      = KODE_NO_RESOURCES;
    req.parts      = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    req.wake_pin   = WAKE_POS;
    rest_post(&req);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);

    expect_awake_posted();   /* the expander's line is given back */
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_STRING("pin: no wake line", n->why);
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);
    TEST_ASSERT_EQUAL_UINT(0U, fake_time_periodic_stops());
    lp_down();
}

TEST_CASE("a console line ends the CPU part", "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();

    console_line((uint8_t)KODE_CONSOLE_SRC_BLE);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_CONSOLE, n->cause);
    lp_down();
}

TEST_CASE("the CPU part is refused on a cable, and the other parts are not", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    charger_reading(true);

    req.parts = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    rest_post(&req);
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("cable: external power", n->why);

    rest_ask(KODE_LP_PART_TOUCH);
    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_TOUCH);
    lp_down();
}

/* The request is judged on its arrival, and a busy owner of the CPU part is
 * asked again for up to the retry window: a cable that comes in meanwhile
 * must still keep the P4 out of light sleep. */
TEST_CASE("a cable that comes while the CPU part's owner is asked again refuses the CPU part",
          "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    req.parts = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    rest_post(&req);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), true, "expander busy");
    expect_awake_posted();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a busy owner ended the rest at once");

    charger_reading(true);
    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);

    expect_awake_posted();
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_STRING("cable: external power", n->why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_false, "the lock was let go with a cable in");
    lp_down();
}

/** @brief Bring AO_LowPower up with the touch's owner and one other, and the touch at rest for an app. */
static void touch_rests_beside_another(void)
{
    lp_up(KODE_OK);
    ao_spy_subscribe(LOWPOWER_PART_SIG);
    TEST_ASSERT_EQUAL_UINT8(0U, lowpower_join(ao_spy(), "touch"));
    TEST_ASSERT_EQUAL_UINT8(1U, lowpower_join(ao_spy(), "storage"));
    rest_ask(KODE_LP_PART_TOUCH);
    say_ready(0U, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);
    ao_spy_drain();
}

/** @brief Bring AO_LowPower up with every owner joined, and the sound and the radio put away for an app. */
static void radio_rests(void)
{
    rest_up();
    rest_ask(KODE_LP_PART_SOUND | KODE_LP_PART_RADIO);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    say_ready(PART_WHO_RADIO, expect_part_round(KODE_LP_PART_RADIO), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_RADIO);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/** @brief Take the next event and insist it is the board's round for @p target, nobody's part. */
static uint32_t expect_board_round(kode_lp_state_t target)
{
    const LowPowerRoundEvt *r = (const LowPowerRoundEvt *)ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(r, "no owner was asked for the board's round");
    TEST_ASSERT_EQUAL_INT((int)LOWPOWER_ROUND_SIG, (int)r->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)target, r->target);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, r->parts, "the board's round is nobody's part");
    return r->round;
}

/* The owners of what an app put away are asked last: a round another owner
 * refuses never reaches them, and the app's rest is as it was. */
TEST_CASE("a round of the board asks the other owners first, and takes the rest only when they are ready",
          "[lowpower_ao]")
{
    uint32_t round;

    touch_rests_beside_another();

    ask(KODE_LP_ASLEEP);
    round = expect_board_round(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the touch's owner was asked with another still to answer");
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_TOUCH, parts_read());

    say_ready(1U, round, false, NULL);
    (void)expect_news(KODE_LP_PART_TAKEN, KODE_OK, 0U);
    TEST_ASSERT_EQUAL_UINT32(round, expect_board_round(KODE_LP_ASLEEP));
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());

    say_ready(0U, round, false, NULL);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    lp_down();
}

TEST_CASE("a round of the board an owner refuses leaves the app's rest as it was", "[lowpower_ao]")
{
    uint32_t round;

    touch_rests_beside_another();
    set_word(KSET_KEY_SLEEP_AFTER, WORD_30S);
    ao_spy_drain();

    wait_ms(IDLE_30S_MS);
    round = expect_board_round(KODE_LP_ASLEEP);
    say_ready(1U, round, true, "file write");

    /* The owner that was asked is told the round is off; nobody else heard of it. */
    expect_awake_posted();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the rest was told over, or its owner woken");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KODE_LP_PART_TOUCH, parts_read(), "an abandoned round undid the app's rest");

    rest_end();
    expect_awake_posted();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    lp_down();
}

/* A rest half arranged has an owner that was asked and has not answered: the
 * board takes all of it, as it did before the round learned to ask last. */
TEST_CASE("the board asking while a rest is being arranged answers it and takes over", "[lowpower_ao]")
{
    const LowPowerPartEvt *n;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH | KODE_LP_PART_CARD);
    say_ready(PART_WHO_STORAGE, expect_part_round(KODE_LP_PART_CARD), false, NULL);
    (void)expect_part_round(KODE_LP_PART_TOUCH);

    ask(KODE_LP_ASLEEP);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("board: takes the rest", n->why);
    (void)expect_news(KODE_LP_PART_TAKEN, KODE_OK, 0U);
    (void)expect_round(KODE_LP_ASLEEP);
    round_cancel();
    lp_down();
}

/* OFF asked for is an order no owner can refuse: nothing is left to keep apart. */
TEST_CASE("an order for OFF takes the app's rest at once and asks everybody together", "[lowpower_ao]")
{
    uint32_t round;

    touch_rests_beside_another();

    ask(KODE_LP_OFF);

    (void)expect_news(KODE_LP_PART_TAKEN, KODE_OK, 0U);
    round = expect_round(KODE_LP_OFF);
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    (void)qf_support_tick(AO_LowPower, QF_SUPPORT_STEPS(KODE_LOWPOWER_OFF_GRACE_MS + 10U));
    lp_down();
}

/* AO_Radio asks for AWAKE when its sentinel stands down with the bus up: the
 * C5 is awake, and only an AWAKE gives the radio back. AWAKE asked for is a
 * state of the board's own, so the app's rest is over, and every owner hears it. */
TEST_CASE("a request for AWAKE while an app rests parts takes the rest and says AWAKE again",
          "[lowpower_ao]")
{
    radio_rests();
    ask(KODE_LP_AWAKE);

    (void)expect_news(KODE_LP_PART_TAKEN, KODE_OK, 0U);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the owners hear one AWAKE, published");
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

TEST_CASE("a request for AWAKE while a rest is being arranged answers it and takes the rest",
          "[lowpower_ao]")
{
    const LowPowerPartEvt *n;
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH | KODE_LP_PART_CARD);
    say_ready(PART_WHO_STORAGE, expect_part_round(KODE_LP_PART_CARD), false, NULL);
    round = expect_part_round(KODE_LP_PART_TOUCH);

    ask(KODE_LP_AWAKE);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("board: takes the rest", n->why);
    (void)expect_news(KODE_LP_PART_TAKEN, KODE_OK, 0U);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    say_ready(PART_WHO_TOUCH, round, false, NULL);   /* nothing waits for it any more */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

/* The same AWAKE given to a round that kept the app's rest for last: the
 * round is given up, and the owners it had not asked yet restore with the rest. */
TEST_CASE("a request for AWAKE in a round that kept the app's rest for last takes the rest as well",
          "[lowpower_ao]")
{
    touch_rests_beside_another();

    ask(KODE_LP_ASLEEP);
    (void)expect_board_round(KODE_LP_ASLEEP);
    ask(KODE_LP_AWAKE);

    (void)expect_news(KODE_LP_PART_TAKEN, KODE_OK, 0U);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the owners hear one AWAKE, published");
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

TEST_CASE("a request for a state wakes a sleeping P4 first, then asks the others", "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();

    ask(KODE_LP_ASLEEP);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "the tick and the lock are back for the round");
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_REQUEST, n->cause);
    /* Five owners hold no part of the rest and are asked now; the glass's is asked last. */
    for ( unsigned i = 0U; 5U > i; i++ )
    {
        (void)expect_board_round(KODE_LP_ASLEEP);
    }
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_SCREEN, parts_read());
    round_cancel();
    lp_down();
}

TEST_CASE("parts are refused while the board is not AWAKE", "[lowpower_ao]")
{
    const LowPowerPartEvt *n;

    lp_up(KODE_OK);
    ao_spy_subscribe(LOWPOWER_PART_SIG);
    go_asleep();

    rest_ask(KODE_LP_PART_SOUND);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("board: not awake", n->why);
    lp_down();
}

/* ---- What a sleeping P4 still owes the board ---------------------------------------- */

/** @brief The id the scheduler joins with, after join_owners()'s six. */
#define PART_WHO_SCHED  6U

/** @brief Fire the one-shot due first and count the readings of the charger it asked for. */
static unsigned minute_fired(void)
{
    fire_one_shot();
    return queued(AO_Power, POWER_PEEK_REQ_SIG);
}

/* Nothing else reads the charger with the tick stopped: AO_Power polls on a
 * QTimeEvt and the charger has no interrupt line. */
TEST_CASE("with the P4 asleep the minute asks for a reading of the charger, and a cable in it ends the CPU part",
          "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    cpu_rest(req);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_time_once_pending(), "no minute was armed for the sleeping P4");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, minute_fired(), "the minute did not ask for a reading");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_true, "a minute is not a wake");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_time_once_pending(), "the next minute was not armed");

    charger_reading(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_true, "a reading with no cable is not a wake");
    charger_reading(true);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_CABLE, n->cause);
    lp_down();
}

/* An app that names no wake source cannot leave the board asleep until the
 * cell cuts out: off_after counts from the moment the P4 went to rest. */
TEST_CASE("a P4 left asleep for off_after takes the board OFF", "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;
    unsigned fires = 0U;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();

    while ( (0U == ao_spy_count()) && ((KODE_LOWPOWER_OFF_AFTER_DEF_S / KODE_LOWPOWER_MINUTE_S) + 2U > fires) )
    {
        (void)minute_fired();
        fires++;
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(KODE_LOWPOWER_OFF_AFTER_DEF_S / KODE_LOWPOWER_MINUTE_S, fires,
                                   "off_after did not count from the P4's rest");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "the round needs the tick and the lock back");
    expect_awake_posted();                       /* the expander's owner: its line is plain again */
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_OWNER, n->cause);
    (void)expect_board_round(KODE_LP_OFF);
    round_cancel();
    lp_down();
}

TEST_CASE("an owner's time that comes with the P4 asleep ends the CPU part", "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;
    uint64_t due;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();
    due = khal_time_us() + (90ULL * 1000000ULL);
    TEST_ASSERT_TRUE(lowpower_wake_at(PART_WHO_STORAGE, due));
    (void)qf_support_settle(AO_LowPower);

    (void)minute_fired();                        /* the minute, before the time */
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_true);
    (void)minute_fired();                        /* the one-shot armed for the time itself */

    TEST_ASSERT_EQUAL_UINT64_MESSAGE(due, khal_time_us(), "the P4 was not woken at the owner's time");
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_OWNER, n->cause);
    lp_down();
}

/* The scheduler's minute is a QTimeEvt: with the tick stopped it gives its
 * next entry as a wake time, as it does for ASLEEP, and is told to arm again. */
TEST_CASE("the CPU part asks the scheduler for its next minute, after the wake lines", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };

    rest_up();
    TEST_ASSERT_EQUAL_UINT8(PART_WHO_SCHED, lowpower_join(ao_spy(), "scheduler"));
    req.parts = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    rest_post(&req);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_false, "the scheduler has not given its time yet");
    say_ready(PART_WHO_SCHED, expect_part_round(KODE_LP_PART_CPU), false, NULL);

    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN | KODE_LP_PART_CPU);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);
    expect_awake_posted();                       /* the expander's owner */
    expect_awake_posted();                       /* the scheduler: its minute runs again */
    (void)expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    lp_down();
}

TEST_CASE("a scheduler with work in hand refuses the CPU part, the wake lines are given back, "
          "and the part's owners are asked again from the first", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };

    rest_up();
    TEST_ASSERT_EQUAL_UINT8(PART_WHO_SCHED, lowpower_join(ao_spy(), "scheduler"));
    req.parts = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    rest_post(&req);
    expect_glass_asked(true);
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    say_ready(PART_WHO_SCHED, expect_part_round(KODE_LP_PART_CPU), true, "entry running");

    expect_awake_posted();
    expect_awake_posted();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the busy scheduler ended the rest at once");
    TEST_ASSERT_EQUAL_UINT(0U, s_hold_false);

    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    say_ready(PART_WHO_SCHED, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN | KODE_LP_PART_CPU);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);
    expect_awake_posted();                       /* the expander's owner */
    expect_awake_posted();                       /* the scheduler */
    (void)expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    lp_down();
}

/* AO_Power says it before it opens the battery path, and what it does next
 * takes time the stopped tick would not give it. */
TEST_CASE("the cell at its cut ends the CPU part", "[lowpower_ao]")
{
    static const QEvt going = QEVT_INITIALIZER(POWER_SHIP_SIG);
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();

    feed(&going);

    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_BATTERY, n->cause);
    lp_down();
}

/* ---- Parts that do not stay, lines in use, answers that come late --------------------- */

TEST_CASE("the sound and the card asked again are put away again", "[lowpower_ao]")
{
    rest_up();
    rest_ask(KODE_LP_PART_SOUND | KODE_LP_PART_CARD);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    say_ready(PART_WHO_STORAGE, expect_part_round(KODE_LP_PART_CARD), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);

    /* A sound played and a file read since then brought both back. */
    rest_ask(KODE_LP_PART_SOUND | KODE_LP_PART_CARD);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    say_ready(PART_WHO_STORAGE, expect_part_round(KODE_LP_PART_CARD), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    lp_down();
}

TEST_CASE("the radio asks the sound's owner in its own request, whatever rested before", "[lowpower_ao]")
{
    const LowPowerPartEvt *n;

    rest_up();
    rest_ask(KODE_LP_PART_SOUND);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    ao_spy_drain();

    rest_ask(KODE_LP_PART_RADIO);
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("radio: needs sound at rest", n->why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the radio was asked with the sound unknown");

    rest_ask(KODE_LP_PART_RADIO | KODE_LP_PART_SOUND);
    (void)say_busy_to_the_end(PART_WHO_AUDIO, KODE_LP_PART_SOUND, "audio playing");
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("sound: audio playing", n->why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the C5 was put to sleep under a sound");
    lp_down();
}

TEST_CASE("a line of the header on loan is refused as a wake source, and stays as it was", "[lowpower_ao]")
{
    static const uint8_t pos[] = { WAKE_POS };
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;
    khal_exp_info_t info;
    uint8_t gpio = 0U;

    rest_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_lend(pos, Q_DIM(pos), "case", &gpio));
    req.parts    = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    req.wake_pin = WAKE_POS;
    rest_post(&req);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("pin: in use", n->why);
    TEST_ASSERT_EQUAL_UINT(0U, s_wake_arms);
    khal_exp_info(WAKE_POS, &info);
    TEST_ASSERT_EQUAL_INT(KHAL_EXP_LENT, info.mode);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_return("case"));
    lp_down();
}

TEST_CASE("an input of the header armed as a wake line is an input again after the wake", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    khal_exp_info_t info;

    rest_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_input(WAKE_POS, KHAL_GPIO_PULL_NONE));
    req.wake_pin = WAKE_POS;
    cpu_rest(req);
    TEST_ASSERT_EQUAL_UINT(1U, s_wake_arms);

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);

    TEST_ASSERT_EQUAL_UINT(1U, s_wake_disarms);
    khal_exp_info(WAKE_POS, &info);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KHAL_EXP_IN, info.mode, "the position did not go back to its owner's table");
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_release(WAKE_POS));
    lp_down();
}

/* The owner did put its part away, only late: the end of the rest has to
 * bring it back, so it is held from the moment it says so. */
TEST_CASE("an owner that answers after its time has its part held all the same", "[lowpower_ao]")
{
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH);
    round = expect_part_round(KODE_LP_PART_TOUCH);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);

    say_ready(PART_WHO_TOUCH, round, false, NULL);
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_TOUCH, parts_read());

    rest_end();
    expect_awake_posted();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    lp_down();
}

/* The radio's round waited behind a stop to a silent C5: kode_rest() gave up
 * on it, the app ended its rest, and the ready came after. Nothing holds the
 * radio for the app any more, so its owner is told to come back. */
TEST_CASE("a part that rests after its time, the rest ended since, is told to come back",
          "[lowpower_ao]")
{
    const LowPowerPartEvt *n;
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_RADIO | KODE_LP_PART_SOUND);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    round = expect_part_round(KODE_LP_PART_RADIO);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("radio: no answer", n->why);

    rest_end();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);   /* nothing was held to bring back */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    say_ready(PART_WHO_RADIO, round, false, NULL);
    expect_awake_posted();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "more than the one AWAKE");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, parts_read(), "a part held for a rest that is over");
    lp_down();
}

/* The same for the glass: asleep after its time and after the end, it is woken. */
TEST_CASE("a screen that sleeps after its time, the rest ended since, is woken", "[lowpower_ao]")
{
    rest_up();
    rest_ask(KODE_LP_PART_SCREEN);
    expect_glass_asked(true);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);

    rest_end();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);

    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    expect_glass_asked(false);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());

    glass_says(false, KODE_OK, DISPLAY_SLEEP_WHY_NONE);   /* its answer: nothing waits for it */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

/* A new rest without the late part does not hold it: nothing would ever bring
 * it back, so its owner is told to come back at once. */
TEST_CASE("a part that rests after its time while the next rest asks for others comes back at once",
          "[lowpower_ao]")
{
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH);
    round = expect_part_round(KODE_LP_PART_TOUCH);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);

    rest_ask(KODE_LP_PART_SOUND);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);

    say_ready(PART_WHO_TOUCH, round, false, NULL);
    expect_awake_posted();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "more than the one AWAKE");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, parts_read(), "a part held that no rest wants");

    rest_end();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);   /* nothing was held to bring back */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    lp_down();
}

/* A rest asked since holds the part itself, though the last request asked for
 * another: the late answer leaves it at rest. */
TEST_CASE("a part that rests after its time and again in a later rest stays at rest", "[lowpower_ao]")
{
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH);
    round = expect_part_round(KODE_LP_PART_TOUCH);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);

    rest_ask(KODE_LP_PART_TOUCH);
    say_ready(PART_WHO_TOUCH, expect_part_round(KODE_LP_PART_TOUCH), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_TOUCH);

    rest_ask(KODE_LP_PART_SOUND);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_TOUCH);

    say_ready(PART_WHO_TOUCH, round, false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a part the rest holds was told to come back");
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_TOUCH, parts_read());

    rest_end();
    expect_awake_posted();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    lp_down();
}

/* Each late step keeps its own answer owed: a second one does not drop the first. */
TEST_CASE("two parts that rest after their time, one rest after the other, both come back",
          "[lowpower_ao]")
{
    uint32_t touch;
    uint32_t radio;

    rest_up();
    rest_ask(KODE_LP_PART_TOUCH);
    touch = expect_part_round(KODE_LP_PART_TOUCH);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);

    rest_ask(KODE_LP_PART_RADIO | KODE_LP_PART_SOUND);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    radio = expect_part_round(KODE_LP_PART_RADIO);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);

    rest_end();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);

    say_ready(PART_WHO_TOUCH, touch, false, NULL);
    expect_awake_posted();
    say_ready(PART_WHO_RADIO, radio, false, NULL);
    expect_awake_posted();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "more than one AWAKE each");
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

/* The glass's owner had no room for the wake: the screen is not left asleep
 * with nobody to wake it, it is held, and the next end of the rest wakes it. */
TEST_CASE("a screen that sleeps after its time, with no room to wake it, is held for the next end",
          "[lowpower_ao]")
{
    static const QEvt filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

    rest_up();
    rest_ask(KODE_LP_PART_SCREEN);
    expect_glass_asked(true);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);

    rest_end();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);

    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(ao_spy(), &filler), "the queue never filled");
    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KODE_LP_PART_SCREEN, parts_read(), "the asleep screen was dropped");

    rest_end();
    expect_glass_asked(false);
    glass_says(false, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

/**
 * @brief Fill the owners' queue (the spy's) and leave @p room slots of it free.
 *
 * With one slot free a post with a margin of one finds no room, and a publish
 * still fits.
 *
 * @param room The slots left free.
 */
static void owners_room(unsigned room)
{
    static const QEvt filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(ao_spy(), &filler), "the queue never filled");
    for ( unsigned i = 0U; room > i; i++ )
    {
        (void)ao_spy_take();
    }
    ao_spy_release();
}

/* The radio's owner had no room for the end's AWAKE: the radio is not dropped
 * from the rest with nobody to bring it back. It is held, and its owner is
 * asked again a second later, as a busy owner is. */
TEST_CASE("an owner with no room for the end's AWAKE is asked again a second later, "
          "its part held until then", "[lowpower_ao]")
{
    lowpower_part_last_t last;

    radio_rests();
    owners_room(1U);
    rest_end();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, (unsigned)uxQueueSpacesAvailable(ao_spy()->eQueue),
                                   "the end was answered with the radio not told");
    lowpower_part_last(&last);
    TEST_ASSERT_TRUE_MESSAGE(last.open, "the end is still being arranged");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KODE_LP_PART_RADIO, last.parts, "the radio was dropped from the rest");
    ao_spy_drain();

    wait_real(KODE_LOWPOWER_REQUEST_RETRY_MS - STEP_SLACK_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "asked again before its wait was over");
    wait_real(STEP_SLACK_MS);
    expect_awake_posted();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

TEST_CASE("an owner with no room for the end's AWAKE to the end of the window stops the end "
          "there, its part still at rest", "[lowpower_ao]")
{
    lowpower_part_last_t last;
    lowpower_info_t info;

    radio_rests();
    owners_room(1U);
    rest_end();
    wait_real(KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (unsigned)uxQueueSpacesAvailable(ao_spy()->eQueue),
                                   "the end was never answered");
    ao_spy_drain();
    lowpower_info_get(&info);
    TEST_ASSERT_EQUAL_STRING("radio: out of events", info.part_why);
    lowpower_part_last(&last);
    TEST_ASSERT_FALSE(last.open);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KODE_LP_PART_RADIO, last.parts, "the radio was dropped from the rest");

    rest_end();
    expect_awake_posted();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

/* The same for a part that rested after its time, the rest ended since: held,
 * as the glass is, so the next end of the rest brings it back. */
TEST_CASE("a part that rests after its time, with no room to tell it to come back, is held for the next end",
          "[lowpower_ao]")
{
    uint32_t round;

    rest_up();
    rest_ask(KODE_LP_PART_RADIO | KODE_LP_PART_SOUND);
    say_ready(PART_WHO_AUDIO, expect_part_round(KODE_LP_PART_SOUND), false, NULL);
    round = expect_part_round(KODE_LP_PART_RADIO);
    wait_ms(KODE_LOWPOWER_PART_STEP_MS + STEP_SLACK_MS);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    rest_end();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);

    owners_room(0U);
    say_ready(PART_WHO_RADIO, round, false, NULL);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(KODE_LP_PART_RADIO, parts_read(),
                                   "the radio was dropped with nobody to bring it back");

    rest_end();
    expect_awake_posted();
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, 0U);
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    lp_down();
}

/* The app's own door (kode_sys_screen_sleep()) answers on the same signal. */
TEST_CASE("an answer to the app's own display request is not this rest's", "[lowpower_ao]")
{
    DisplaySleepEvt mine;

    rest_up();
    rest_ask(KODE_LP_PART_SCREEN);
    expect_glass_asked(true);

    (void)memset(&mine, 0, sizeof(mine));
    mine.super  = (QEvt)QEVT_INITIALIZER(DISPLAY_SLEEP_SIG);
    mine.asleep = true;
    mine.rc     = (uint8_t)KODE_OK;
    mine.who    = DISPLAY_SLEEP_WHO_APP;
    feed(&mine.super);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the app's answer settled the rest's step");

    glass_says(true, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN);
    lp_down();
}

/** @brief One-shots a case fires to be past the ones an earlier rest left out. */
#define ONE_SHOTS_SEEN 4U

/* The owners say their times again in every rest of the CPU: one kept from
 * the rest before has long passed, and would end this one at its first minute. */
TEST_CASE("an owner's time from the rest before does not end the next rest of the CPU", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };

    rest_up();
    cpu_rest(req);
    TEST_ASSERT_TRUE(lowpower_wake_at(PART_WHO_STORAGE, khal_time_us() + (90ULL * 1000000ULL)));
    (void)qf_support_settle(AO_LowPower);
    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);     /* a button, long before that time */
    ao_spy_drain();
    fake_time_advance_ms(5U * 60U * 1000U);      /* and the time passes with the P4 awake */

    req.parts = KODE_LP_PART_CPU;
    rest_post(&req);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN | KODE_LP_PART_CPU);
    TEST_ASSERT_EQUAL_UINT(2U, s_hold_false);
    /* The one-shots of the rest before come first, then this rest's own minutes. */
    for ( unsigned fires = 0U; ONE_SHOTS_SEEN > fires; fires++ )
    {
        (void)minute_fired();
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_hold_true, "a time that had passed woke the new rest");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the new rest was told over");
    lp_down();
}

/** @brief A listener's interrupt, for the cases that give a position one; never called. */
static void pin_listener(void *arg)
{
    (void)arg;
}

/* A wake line would replace the interrupt the position's owner attached. */
TEST_CASE("an input of the header with an interrupt attached is refused as a wake source", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    khal_gpio_irq_attach_IgnoreAndReturn(KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_input(WAKE_POS, KHAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_irq_attach(WAKE_POS, KHAL_GPIO_IRQ_ANY_EDGE,
                                                       &pin_listener, NULL));
    req.parts    = KODE_LP_PART_SCREEN | KODE_LP_PART_CPU;
    req.wake_pin = WAKE_POS;
    rest_post(&req);

    n = expect_news(KODE_LP_PART_ANSWER, KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_STRING("pin: in use", n->why);
    TEST_ASSERT_EQUAL_UINT(0U, s_wake_arms);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_release(WAKE_POS));
    lp_down();
}

TEST_CASE("an input of the header gets its own pull back after the wake", "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };
    khal_exp_info_t info;

    rest_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_input(WAKE_POS, KHAL_GPIO_PULL_UP));
    req.wake_pin   = WAKE_POS;
    req.wake_level = true;                       /* the wake line pulls it down */
    cpu_rest(req);

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);

    khal_exp_info(WAKE_POS, &info);
    TEST_ASSERT_EQUAL_INT(KHAL_EXP_IN, info.mode);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KHAL_GPIO_PULL_UP, info.pulled, "the position kept the wake line's pull");
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_exp_release(WAKE_POS));
    lp_down();
}

/* ---- kode_rest() and kode_rest_end(): a task's wait for the answer ----------------- */

/** @brief AO_LowPower, for a task's wait: take what is in its queue, at each of the task's waits. */
static void rest_owner_step(void)
{
    (void)qf_support_settle(AO_LowPower);
    /* The hook runs once: the owner goes on answering for the whole wait. */
    fake_time_delay_hook(&rest_owner_step);
}

/** @brief AO_LowPower up with no owner joined, the layer up, and the owner answering. */
static void task_rest_up(void)
{
    lp_up(KODE_OK);
    qf_support_margin_queue(AO_LowPower);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_time_reset();
    fake_time_delay_hook(&rest_owner_step);
}

/** @brief Leave the clock and AO_LowPower as the next case expects them. */
static void task_rest_down(void)
{
    fake_time_delay_hook(NULL);
    (void)qf_support_settle(AO_LowPower);
    lp_down();
}

/* No owner of these parts is in the build, so they are at rest without
 * asking: what is under test is the call's wait for its own answer. */
TEST_CASE("kode_rest waits for AO_LowPower and is true once the parts rest; kode_rest_end brings them back",
          "[lowpower_ao]")
{
    task_rest_up();

    TEST_ASSERT_TRUE(kode_rest(KODE_REST_SOUND | KODE_REST_TOUCH | KODE_REST_SCREEN, NULL));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_TOUCH | KODE_LP_PART_SCREEN, parts_read());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_REST_BOUND_MS, kode_sys_time_ms());

    TEST_ASSERT_TRUE(kode_rest_end());
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    task_rest_down();
}

TEST_CASE("kode_rest is false at once with the part and the reason when the request cannot be",
          "[lowpower_ao]")
{
    task_rest_up();

    TEST_ASSERT_FALSE(kode_rest(KODE_REST_RADIO, NULL));
    TEST_ASSERT_EQUAL_STRING("the rest did not begin: radio: needs sound at rest", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(KODE_REST_BOUND_MS, kode_sys_time_ms(),
                                         "a refusal waited out the whole bound");

    /* A bit above the parts there are is refused by name too. */
    TEST_ASSERT_FALSE(kode_rest(0x100U | (uint32_t)KODE_REST_SOUND, NULL));
    TEST_ASSERT_EQUAL_STRING("the rest did not begin: request: no such part", kode_error());
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    task_rest_down();
}

TEST_CASE("kode_rest is false at once while another request is being arranged, and leaves it alone",
          "[lowpower_ao]")
{
    uint32_t round;

    task_rest_up();
    TEST_ASSERT_EQUAL_UINT8(0U, lowpower_join(ao_spy(), "touch"));
    rest_ask(KODE_LP_PART_TOUCH);                /* the console's, its owner still to answer */
    round = expect_part_round(KODE_LP_PART_TOUCH);

    TEST_ASSERT_FALSE(kode_rest(KODE_REST_SCREEN, NULL));
    TEST_ASSERT_EQUAL_STRING("the rest did not begin: another request is being arranged", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_REST_BOUND_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, queued(AO_LowPower, LOWPOWER_PART_REQ_SIG),
                                   "the request was posted all the same");

    say_ready(0U, round, false, NULL);           /* the first goes on to its end */
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_TOUCH, parts_read());
    ao_spy_drain();
    task_rest_down();
}

TEST_CASE("kode_rest that nobody answers is false after its bound, and says how long", "[lowpower_ao]")
{
    task_rest_up();
    fake_time_delay_hook(NULL);

    TEST_ASSERT_FALSE(kode_rest(KODE_REST_SOUND, NULL));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_REST_BOUND_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_STRING("the rest did not begin in 37000 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, queued(AO_LowPower, LOWPOWER_PART_REQ_SIG),
                                   "the request was posted more than once");
    task_rest_down();
}

TEST_CASE("kode_rest hands the wake sources to the CPU part, and kode_rest_end wakes it",
          "[lowpower_ao]")
{
    kode_rest_wake_t const wake = { .after_ms = 2000U };

    task_rest_up();

    /* No display owner in this build: the CPU part needs no screen at rest. */
    TEST_ASSERT_TRUE(kode_rest(KODE_REST_CPU, &wake));
    TEST_ASSERT_EQUAL_HEX8(KODE_LP_PART_CPU, parts_read());
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_time_once_pending(), "the time, and the minute");

    TEST_ASSERT_TRUE(kode_rest_end());
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_EQUAL_HEX8(0U, parts_read());
    task_rest_down();
}

/** @brief What kode_rest() answered inside the probe Active Object. */
typedef struct {
    bool ok;                      /**< Its answer.               */
    char error[KODE_ERROR_MAX];   /**< kode_error() right after. */
} RestFromAo;

/** @brief The longest a case waits for the probe Active Object to run its job. */
#define REST_PROBE_WAIT_MS 2000U

/** @brief The probe's job: ask for a rest from inside an Active Object. */
static void rest_from_ao_job(void *ctx)
{
    RestFromAo *const r = (RestFromAo *)ctx;

    r->ok = kode_rest(KODE_REST_SOUND, NULL);
    (void)snprintf(r->error, sizeof(r->error), "%s", kode_error());
}

/* kode_rest() waits for its answer, and an Active Object never blocks: its
 * door is kode_sys_rest(), answered with LOWPOWER_PART_SIG. */
TEST_CASE("kode_rest is refused inside an Active Object, which is named, and posts nothing",
          "[lowpower_ao]")
{
    RestFromAo r = { true, "" };

    task_rest_up();
    fake_time_delay_hook(NULL);

    TEST_ASSERT_TRUE(probe_ao_run(&rest_from_ao_job, &r, REST_PROBE_WAIT_MS));

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("kode_rest was called from an Active Object (" PROBE_AO_NAME
                             "); kode.h calls block - call kode_sys_* instead",
                             r.error);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, queued(AO_LowPower, LOWPOWER_PART_REQ_SIG),
                                   "a request was posted all the same");
    task_rest_down();
}

/* ---- The look for a cable ------------------------------------------------------------ */

/** @brief The period of the look for a cable, microseconds. */
#define LOOK_US  ((uint64_t)KODE_LOWPOWER_CABLE_LOOK_MS * 1000ULL)

/** @brief How many requests to show the cable port again AO_Log was posted; its queue is emptied. */
static unsigned bounces(void)
{
    return queued(AO_Log, LOG_BOUNCE_REQ_SIG);
}

/**
 * @brief Empty AO_Power's queue, counting what was asked of it.
 * @param peeks Receives how many whole readings were asked (POWER_PEEK_REQ_SIG).
 * @return How many looks for a cable alone were asked (POWER_CABLE_REQ_SIG).
 */
static unsigned cable_looks(unsigned *peeks)
{
    const QEvt *e = NULL;
    unsigned    looks = 0U;

    *peeks = 0U;
    while ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)POWER_CABLE_REQ_SIG == e->sig )
        {
            looks++;
        }
        else if ( (QSignal)POWER_PEEK_REQ_SIG == e->sig )
        {
            (*peeks)++;
        }
        else
        {
            /* Not what this counts. */
        }
        QF_gc(e);
    }
    return looks;
}

/** @brief Fire the look's timer once and check it asked AO_Power for the cable alone. */
static void look_fires_once(void)
{
    unsigned peeks = 0U;

    TEST_ASSERT_TRUE(fake_time_periodic_fire_at(LOOK_US));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, cable_looks(&peeks), "the look asks whether a cable is in");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, peeks, "the look asks for no whole reading");
}

TEST_CASE("asleep, the charger is read every few seconds on a timer of its own; awake, it is not",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);

    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running_at(LOOK_US), "asleep, the look runs");
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running(), "and the tick does not");
    look_fires_once();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_time_once_pending(), "the minute's one-shot is the only one");

    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running_at(LOOK_US),
                              "awake, AO_Power polls the charger by itself");
    lp_down();
}

TEST_CASE("a cable the look finds wakes the board and has the cable port shown again, once",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    (void)bounces();

    charger_reading(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no cable, no wake");
    TEST_ASSERT_EQUAL_UINT(0U, bounces());

    charger_reading(true);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, bounces(), "the port was off the bus when the cable came");
    TEST_ASSERT_FALSE(fake_time_periodic_running_at(LOOK_US));

    charger_reading(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, bounces(), "once for one cable");
    lp_down();
}

TEST_CASE("a cable in the first reading after another wake has the cable port shown again",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    (void)bounces();

    charger_reading(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, bounces(), "the cable came before the look saw it");
    lp_down();
}

TEST_CASE("a cable that comes to a board that is awake leaves the cable port alone",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    (void)bounces();
    charger_reading(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, bounces(), "the board never slept");
    charger_reading(false);

    go_asleep();
    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    charger_reading(false);
    charger_reading(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, bounces(),
                                   "the reading after the wake had no cable: the port was on the bus");
    lp_down();
}

/** @brief Leave ASLEEP by the top button. */
static void leave_by_top(void)
{
    static const QEvt top = QEVT_INITIALIZER(LOWPOWER_TOP_SIG);

    feed(&top);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_TOP, KODE_LP_ASLEEP);
}

/** @brief Leave ASLEEP by a message from the broker. */
static void leave_by_message(void)
{
    MqttMsgEvt m;

    (void)memset(&m, 0, sizeof(m));
    m.super = (QEvt)QEVT_INITIALIZER(MQTT_MSG_SIG);
    feed(&m.super);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_MQTT, KODE_LP_ASLEEP);
}

/** @brief Leave ASLEEP by a console line. */
static void leave_by_line(void)
{
    console_line(KODE_CONSOLE_SRC_BLE);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BLE, KODE_LP_ASLEEP);
}

/** @brief Leave ASLEEP for an owner's time. */
static void leave_by_owner_time(void)
{
    wake_in(0U, 10U * SECOND_MS);
    fire_one_shot();
    expect_timer_wake();
}

/** @brief The round for OFF that leaving ASLEEP began: both owners ready, then OFF. */
static void leave_to_off(kode_lp_cause_t cause)
{
    uint32_t const round = expect_round(KODE_LP_OFF);

    say_ready(0U, round, false, NULL);
    say_ready(1U, round, false, NULL);
    expect_state(KODE_LP_OFF, cause, KODE_LP_ASLEEP);
    wait_ms(KODE_LOWPOWER_OFF_GRACE_MS + 10U);
}

/** @brief Leave ASLEEP for OFF at the minute that reaches off_after (an hour). */
static void leave_by_off_after(void)
{
    for ( unsigned minutes = 0U; (ao_spy_count() == 0U) && (100U > minutes); minutes++ )
    {
        fire_one_shot();
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
    }
    leave_to_off(KODE_LP_CAUSE_OFF_AFTER);
}

/** @brief Leave ASLEEP for OFF on request. */
static void leave_by_off_request(void)
{
    ask(KODE_LP_OFF);
    leave_to_off(KODE_LP_CAUSE_REQUEST);
}

/** @brief Leave ASLEEP for AWAKE on request. */
static void leave_by_awake_request(void)
{
    ask(KODE_LP_AWAKE);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_REQUEST, KODE_LP_ASLEEP);
}

/* BOT and a cable have their cases above; these are the rest of ASLEEP's ways
 * out, each of which takes the lock back. */
TEST_CASE("every other way out of ASLEEP ends the look for a cable", "[lowpower_ao]")
{
    static const struct {
        const char *name;
        void      (*leave)(void);
    } ways[] = {
        { "the top button",       &leave_by_top },
        { "a broker's message",   &leave_by_message },
        { "a console line",       &leave_by_line },
        { "an owner's time",      &leave_by_owner_time },
        { "off_after",            &leave_by_off_after },
        { "a request for OFF",    &leave_by_off_request },
        { "a request for AWAKE",  &leave_by_awake_request },
    };

    for ( size_t i = 0U; Q_DIM(ways) > i; i++ )
    {
        lp_up(KODE_OK);
        set_word(KSET_KEY_OFF_AFTER, WORD_1H);
        go_asleep();
        (void)queued(AO_Display, DISPLAY_MINUTE_REQ_SIG);
        (void)queued(AO_Power, POWER_PEEK_REQ_SIG);
        TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running_at(LOOK_US), ways[i].name);

        ways[i].leave();
        TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running_at(LOOK_US), ways[i].name);
        lp_down();
    }
}

/* khal_timer_periodic() makes a new timer at every call: one made again for
 * the same callback would leave the first running where nothing stops it. */
TEST_CASE("two sleeps make the look's timer once", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    go_asleep();
    expander(LEVELS_BOT, CHANGED_BOT);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);

    sleep_now();
    TEST_ASSERT_TRUE(fake_time_periodic_running_at(LOOK_US));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_time_periodic_made_at(LOOK_US),
                                   "the second sleep started the timer again; it made none");
    lp_down();
}

/* AO_Power is at rest through a dark wake and polls nothing: the look is the
 * only reading of the charger there is. */
TEST_CASE("a dark wake keeps the look, and it runs on after", "[lowpower_ao]")
{
    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    TEST_ASSERT_TRUE(fake_time_periodic_running_at(LOOK_US));

    dark_wake();
    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running_at(LOOK_US), "a cable must still be seen");
    (void)queued(AO_Power, POWER_CABLE_REQ_SIG);
    look_fires_once();

    dark_done_all();
    dark_rest_both(expect_dark_rounds(), 0U);
    expect_state(KODE_LP_ASLEEP, KODE_LP_CAUSE_TIMER, KODE_LP_DARK);
    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running_at(LOOK_US), "the lock is let go again");
    lp_down();
}

TEST_CASE("a cable the look finds in a dark wake wakes the board whole, and the look ends",
          "[lowpower_ao]")
{
    lp_up(KODE_OK);
    radio_asleep(20U * SECOND_MS);
    dark_wake();
    (void)bounces();

    charger_reading(true);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, bounces());
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running_at(LOOK_US), "awake for good: no look");
    ao_spy_drain();
    lp_down();
}

/* The CPU part lets go of the lock as ASLEEP does, so its light sleeps take
 * the cable port off the bus as well. */
TEST_CASE("the CPU part's rest reads the charger every few seconds, and its end stops the look",
          "[lowpower_ao]")
{
    lowpower_part_req_t req = { 0 };

    /* The look's timer outlives a case: a first rest and its end leave it
     * stopped whatever the case before left, so the second rest proves the start. */
    rest_up();
    cpu_rest(req);
    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running_at(LOOK_US), "the lock is back: no look");
    ao_spy_drain();

    req.parts = KODE_LP_PART_CPU;
    rest_post(&req);
    say_ready(PART_WHO_IOEXP, expect_part_round(KODE_LP_PART_CPU), false, NULL);
    (void)expect_news(KODE_LP_PART_ANSWER, KODE_OK, KODE_LP_PART_SCREEN | KODE_LP_PART_CPU);
    TEST_ASSERT_EQUAL_UINT(2U, s_hold_false);
    (void)queued(AO_Power, POWER_PEEK_REQ_SIG);

    TEST_ASSERT_TRUE_MESSAGE(fake_time_periodic_running_at(LOOK_US), "with the lock let go, the look runs");
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running(), "and the tick does not");
    look_fires_once();

    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);
    TEST_ASSERT_EQUAL_UINT(2U, s_hold_true);
    TEST_ASSERT_FALSE_MESSAGE(fake_time_periodic_running_at(LOOK_US), "the lock is back: no look");
    lp_down();
}

TEST_CASE("a cable the look finds ends the CPU part and has the cable port shown again, once",
          "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };
    const LowPowerPartEvt *n;

    rest_up();
    cpu_rest(req);
    ao_spy_drain();
    (void)bounces();

    charger_reading(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_hold_true, "no cable, no wake");
    TEST_ASSERT_EQUAL_UINT(0U, bounces());

    charger_reading(true);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    expect_awake_posted();
    n = expect_news(KODE_LP_PART_WOKE, KODE_OK, KODE_LP_PART_SCREEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_WAKE_CABLE, n->cause);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, bounces(), "the port was off the bus when the cable came");
    TEST_ASSERT_FALSE(fake_time_periodic_running_at(LOOK_US));

    charger_reading(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, bounces(), "once for one cable");
    lp_down();
}

TEST_CASE("a cable in the first reading after the CPU part ended another way has the cable port shown again",
          "[lowpower_ao]")
{
    lowpower_part_req_t const req = { 0 };

    rest_up();
    cpu_rest(req);
    ao_spy_drain();
    expander(LEVELS_PAD_UP, CHANGED_PAD_UP);
    TEST_ASSERT_EQUAL_UINT(1U, s_hold_true);
    (void)bounces();

    charger_reading(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, bounces(), "the cable came before the look saw it");
    lp_down();
}

/* The request is posted with a margin; AO_Log's queue without room for it
 * refuses it, and the refusal is said by AO_LowPower, never asserted. */
TEST_CASE("a cable whose request AO_Log's queue has no room for is said, and the board wakes",
          "[lowpower_ao]")
{
    static const QEvt filler = QEVT_INITIALIZER(LOG_FLUSH_REQ_SIG);

    lp_up(KODE_OK);
    go_asleep();
    (void)bounces();
    TEST_ASSERT_TRUE(qf_support_queue_stuff(AO_Log, &filler));

    charger_reading(true);
    expect_state(KODE_LP_AWAKE, KODE_LP_CAUSE_VBUS, KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE_MESSAGE(log_has("the cable port is not shown to the host again"),
                             "a refused request was not said");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, bounces(), "a refused request was queued");
    lp_down();
}
