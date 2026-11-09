/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file buttons_ao.c
 * @brief AO_Buttons: the Active Object that owns the six push-buttons.
 */
#include "kaos.h"
#include "kaos_start.h"

#include <stdio.h>

#include "kbsp.h"
#include "kqp.h"
#include "kaos_claim.h"
#include "kdrv_buttons.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kaos.buttons"

typedef struct {
    QActive  super;
    QTimeEvt settle;       /**< One-shot: the confirming sample is due.      */
    QTimeEvt test_tmo;     /**< One-shot: the guided test's step deadline.   */
    QTimeEvt hold;         /**< One-shot: the top or the bottom button has
                            *   been down for KODE_BUTTON_HOLD_MS.          */
    uint8_t  hold_id;      /**< kdrv_button_t the hold timer is running for;
                            *   KDRV_BTN_COUNT when it is not running.      */
    uint8_t  stable;       /**< What the world currently believes.            */
    uint8_t  candidate;    /**< The sample before last, waiting to be confirmed. */
    uint8_t  exp_levels;   /**< The expander's byte as last published.        */
    bool     top_off;      /**< GPIO35's interrupt is off since its edge,
                            *   waiting for the settle to re-arm it.         */
    bool     settling;     /**< The settle is armed: a change is waiting for
                            *   the sample it will ask for.                  */
    bool     settled;      /**< The settle fired and asked; the next reading
                            *   that arrives is its sample - see feed().     */
    bool     dump_pending; /**< `btn_read` is waiting for the next reading.   */
    bool     rot_waiting;  /**< A pad rotation arrived with the pad held, and
                            *   applies once the last pad button is let go. */
    uint8_t  rot_pending;  /**< The quarter turns it asked for.               */

    /* The top button's double press: see double_track(). */
    uint32_t top_release_ms; /**< khal_time_ms() at the top button's last
                              *   confirmed release.                          */
    bool     top_armed;    /**< That release may start a double: the press it
                            *   ended did not reach hold and was not itself
                            *   the second of one.                           */
    bool     top_held;     /**< The press now down reached hold.              */
    bool     top_doubled;  /**< The press now down completed a double.        */

    /* The guided test, while one is running. */
    bool     test_on;      /**< A run is in progress.                         */
    uint8_t  test_origin;  /**< Who asked for it: TEST_ORIGIN_AUTOTEST or a client. */
    bool     test_judged;  /**< This step's press has been judged; waiting
                            *   for everything to be released.               */
    bool     test_failed;  /**< Some step so far was not a clean pass.        */
    uint8_t  test_step;    /**< Index into TEST_SEQ.                          */
    uint8_t  test_wrong;   /**< Wrong presses on this step so far.            */
    uint8_t  test_doubles; /**< Presses of this step that took two buttons down. */
    bool     test_retry;   /**< The press was forgiven: ask for the same button again. */
    bool     test_gaveup;  /**< The last wrong press was the one that fails the test. */
    bool     test_pend;    /**< The asked-for button is down alone so far: its
                            *   pass waits for the release.                  */
    uint8_t  test_seen;    /**< Every button that went down since this step's
                            *   first press, bounce included.                */
    uint32_t test_t0;      /**< khal_time_ms() when the judged press went down. */
    bool     test_timed;   /**< A press was judged on this step: test_t0 is its. */
} Buttons;

/* The production line's script: one press per button, pad and side buttons
 * alternating, so a bridged track or a misrouted pin shows as the wrong name.
 * PAD_LEFT and BOTTOM held together reset the board in hardware, which is why
 * every prompt waits for the previous release. */
static const kdrv_button_t TEST_SEQ[] = {
    KDRV_BTN_LEFT, KDRV_BTN_BOT, KDRV_BTN_DOWN, KDRV_BTN_TOP,
    KDRV_BTN_RIGHT, KDRV_BTN_UP,
};
#define TEST_STEPS ((uint8_t)(sizeof(TEST_SEQ) / sizeof(TEST_SEQ[0])))
_Static_assert(TEST_STEPS == KODE_BUTTONS_TEST_STEPS,
               "the budget counts the steps of the script");

static Buttons l_buttons;
QActive * const AO_Buttons = &l_buttons.super;

/* What buttons_down() answers: every button as the last BUTTON_EVT_SIG about
 * it said, from this AO or buttons_inject(), so a poll agrees with what a
 * subscriber was told. One word changed by atomic one-bit or/and: readers see
 * a whole state, and two writing tasks cannot lose each other's bit. */
static uint32_t s_down;

/* What buttons_presses() answers: up-to-down transitions per button since
 * boot, counted where s_down is set (confirmed and injected alike). Atomic
 * adds on 32-bit words, so no reader sees half of one. */
static uint32_t s_presses[KDRV_BTN_COUNT];

/** @brief Record the word that is about to go out about button @p id. */
static void down_note(uint8_t id, bool pressed)
{
    uint32_t const bit = 1UL << id;
    if ( pressed )
    {
        uint32_t const was = __atomic_fetch_or(&s_down, bit, __ATOMIC_RELAXED);
        if ( (was & bit) == 0U )
        {
            (void)__atomic_fetch_add(&s_presses[id], 1U, __ATOMIC_RELAXED);
        }
    }
    else
    {
        (void)__atomic_fetch_and(&s_down, ~bit, __ATOMIC_RELAXED);
    }
}

/** @brief The turns the pad can take: 0, 90, 180 and 270 degrees. */
#define BUTTONS_PAD_QUARTERS 4U

/** @brief The pad's four bits of buttons_down()'s word: every id below TOP. */
#define BUTTONS_PAD_MASK ((uint8_t)((1U << (uint32_t)KDRV_BTN_TOP) - 1U))


/* The quarter turns in force. Written only by this AO; read by
 * buttons_inject() from the console's task too. One byte, so a load or a
 * store of it is whole on the P4, and volatile so every read is one. */
static volatile uint8_t s_rot;

/** @brief The id button @p id publishes with the glass as it is turned now. */
static uint8_t pad_map(uint8_t id)
{
    /* What each pad button publishes, by the quarter turns of the glass and the
     * button as wired (kdrv_button_t's UP, DOWN, LEFT, RIGHT, which come before
     * TOP). 180 swaps both axes; 90 is the inverse of 270. TOP and BOT are
     * side buttons and never turn. */
    static const uint8_t PAD_MAP[BUTTONS_PAD_QUARTERS][KDRV_BTN_TOP] = {
        /*  UP              DOWN            LEFT            RIGHT                     */
        { KDRV_BTN_UP,    KDRV_BTN_DOWN,  KDRV_BTN_LEFT,  KDRV_BTN_RIGHT },  /*   0 */
        { KDRV_BTN_RIGHT, KDRV_BTN_LEFT,  KDRV_BTN_UP,    KDRV_BTN_DOWN  },  /*  90 */
        { KDRV_BTN_DOWN,  KDRV_BTN_UP,    KDRV_BTN_RIGHT, KDRV_BTN_LEFT  },  /* 180 */
        { KDRV_BTN_LEFT,  KDRV_BTN_RIGHT, KDRV_BTN_DOWN,  KDRV_BTN_UP    },  /* 270 */
    };
    uint8_t published = id;
    if ( (uint8_t)KDRV_BTN_TOP > id )
    {
        published = PAD_MAP[s_rot][id];
    }
    return published;
}

/** @brief Turn the pad by @p quarter_turns, from now on. */
static void rot_apply(Buttons * const me, uint8_t quarter_turns)
{
    s_rot = quarter_turns;
    me->rot_waiting = false;
    KLOG_I(TAG, "the pad follows the glass at %u degrees",
           (unsigned)quarter_turns * 90U);
}

/** @brief Apply a turn that waited for the pad, once the pad is idle. */
static void rot_settle(Buttons * const me)
{
    if ( me->rot_waiting && ((buttons_down() & BUTTONS_PAD_MASK) == 0U) )
    {
        rot_apply(me, me->rot_pending);
    }
}

static QState Buttons_initial(Buttons * const me, void const * const par);
static QState Buttons_watching(Buttons * const me, QEvt const * const e);

/* GPIO35's edge, as an event. Static and immutable: the ISR allocates nothing. */
static const QEvt TOP_INT_EVT = QEVT_INITIALIZER(BUTTONS_TOP_INT_SIG);

void buttons_dump_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BUTTONS_DUMP_REQ_SIG);
    QACTIVE_POST(AO_Buttons, &evt, NULL);
}

void buttons_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(BUTTONS_TEST_REQ_SIG);
    QACTIVE_POST(AO_Buttons, &evt, NULL);
}

void buttons_test_request_from(uint8_t origin)
{
    TestReqEvt *ev = Q_NEW_X(TestReqEvt, 0U, BUTTONS_TEST_REQ_FROM_SIG);
    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; the button test was not asked for");
        return;
    }
    ev->origin = origin;
    QACTIVE_POST(AO_Buttons, &ev->super, NULL);
}

void buttons_test_cancel_request(uint8_t origin)
{
    TestReqEvt *ev = Q_NEW_X(TestReqEvt, 0U, BUTTONS_TEST_CANCEL_SIG);
    if ( NULL == ev )
    {
        KLOG_W(TAG, "out of events; the button test runs out by its own limits");
        return;
    }
    ev->origin = origin;
    QACTIVE_POST(AO_Buttons, &ev->super, NULL);
}

/** @brief Bytes of the `pressed=` list: six names and their '+'s. */
#define BUTTONS_REC_WHO_MAX  64U

/** @brief The word a step's outcome is written with; NULL for a phase with no answer. */
static const char *buttons_test_word(uint8_t phase)
{
    const char *w = NULL;

    switch ( phase )
    {
        case BTN_TEST_PASS:       w = "pass";       break;
        case BTN_TEST_TIMEOUT:    w = "timeout";    break;
        case BTN_TEST_STUCK:      w = "stuck";      break;
        case BTN_TEST_WRONG:      w = "wrong";      break;
        case BTN_TEST_DOUBLE:     w = "double";     break;
        case BTN_TEST_DOUBLEFAIL: w = "doublefail"; break;
        case BTN_TEST_HELD:       w = "held";       break;
        case BTN_TEST_GIVEUP:     w = "giveup";     break;
        default:                  w = NULL;         break;
    }
    return w;
}

size_t buttons_test_record(ButtonsTestEvt const *t, char *out, size_t size)
{
    const char *name;
    const char *word;
    int         w = 0;

    if ( (NULL == t) || (NULL == out) || (0U == size) || ((uint8_t)KDRV_BTN_COUNT <= t->button) )
    {
        return 0U;
    }
    name = kdrv_button_name((kdrv_button_t)t->button);
    word = buttons_test_word(t->phase);
    if ( BTN_TEST_PROMPT == t->phase )
    {
        w = snprintf(out, size, "step=%u of=%u press=%s", (unsigned)t->step, (unsigned)t->total, name);
    }
    else if ( BTN_TEST_PASS == t->phase )
    {
        w = snprintf(out, size, "step=%u button=%s result=pass ms=%u", (unsigned)t->step, name, (unsigned)t->ms);
    }
    else if ( BTN_TEST_TIMEOUT == t->phase )
    {
        w = snprintf(out, size, "step=%u button=%s result=timeout", (unsigned)t->step, name);
    }
    else if ( NULL != word )
    {
        /* Who was pressed, by name, joined with '+': for a wrong press or a
         * give-up, what was pressed instead; for a double, the asked-for
         * button and what came with it; for a stuck or held step, what is
         * down. */
        char   who[BUTTONS_REC_WHO_MAX];
        size_t n = 0U;
        who[0] = '\0';
        for ( uint8_t b = 0U; (uint8_t)KDRV_BTN_COUNT > b; ++b )
        {
            if ( (t->pressed & (uint8_t)(1U << b)) != 0U )
            {
                const char *sep = "+";
                int         k;
                if ( 0U == n )
                {
                    sep = "";
                }
                k = snprintf(&who[n], sizeof(who) - n, "%s%s", sep, kdrv_button_name((kdrv_button_t)b));
                if ( (0 < k) && ((size_t)k < (sizeof(who) - n)) )
                {
                    n += (size_t)k;
                }
            }
        }
        w = snprintf(out, size, "step=%u button=%s result=%s pressed=%s ms=%u", (unsigned)t->step, name, word,
                     who, (unsigned)t->ms);
    }
    else
    {
        /* BTN_TEST_DONE: the verdict follows as TEST_RESULT_SIG. */
        out[0] = '\0';
    }
    if ( 0 >= w )
    {
        return 0U;
    }
    if ( (size_t)w >= size )
    {
        return size - 1U;
    }
    return (size_t)w;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kscreen.c, which this check does not join to this one */
void buttons_rotation_request(uint8_t quarter_turns)
{
    static const ButtonsRotateEvt ROT_EVT[BUTTONS_PAD_QUARTERS] = {
        { QEVT_INITIALIZER(BUTTONS_ROTATE_SIG), 0U },
        { QEVT_INITIALIZER(BUTTONS_ROTATE_SIG), 1U },
        { QEVT_INITIALIZER(BUTTONS_ROTATE_SIG), 2U },
        { QEVT_INITIALIZER(BUTTONS_ROTATE_SIG), 3U },
    };
    if ( BUTTONS_PAD_QUARTERS <= quarter_turns )
    {
        KLOG_W(TAG, "a pad rotation of %u quarter turns is not one; ignored",
               (unsigned)quarter_turns);
        return;
    }
    QACTIVE_POST(AO_Buttons, &ROT_EVT[quarter_turns].super, NULL);
}

uint8_t buttons_down(void)
{
    return (uint8_t)__atomic_load_n(&s_down, __ATOMIC_RELAXED);
}

uint32_t buttons_presses(uint8_t id)
{
    if ( (uint8_t)KDRV_BTN_COUNT <= id )
    {
        return 0U;
    }
    return __atomic_load_n(&s_presses[id], __ATOMIC_RELAXED);
}

bool buttons_inject(uint8_t id, bool pressed)
{
    if ( (uint8_t)KDRV_BTN_COUNT <= id )
    {
        return false;
    }
    ButtonEvt *e = Q_NEW_X(ButtonEvt, 0U, BUTTON_EVT_SIG);
    if ( NULL == e )
    {
        return false;
    }
    uint8_t const published = pad_map(id);   /* a thumb, as far as an app knows */
    down_note(published, pressed);
    e->id      = published;
    e->pressed = pressed;
    e->t_ms    = khal_time_ms();
    QACTIVE_PUBLISH(&e->super, AO_Buttons);
    if ( !pressed && ((uint8_t)KDRV_BTN_TOP > published)
        && ((buttons_down() & BUTTONS_PAD_MASK) == 0U) )
    {
        /* This task is not the AO: a turn that waited for the pad is
         * applied there, where a real release applies it. A full queue only
         * leaves the turn to the next edge. */
        static const QEvt PAD_IDLE_EVT = QEVT_INITIALIZER(BUTTONS_PAD_IDLE_SIG);
        (void)QACTIVE_POST_X(AO_Buttons, &PAD_IDLE_EVT, 1U, NULL);
    }
    return true;
}

/** @brief GPIO35 moved. Interrupt context: make an event of it and leave. */
static void on_top_edge(void *arg)
{
    (void)arg;
    kqp_post_from_isr(AO_Buttons, &TOP_INT_EVT);
}

void Buttons_ctor(void)
{
    Buttons * const me = &l_buttons;
    QActive_ctor(&me->super, Q_STATE_CAST(&Buttons_initial));
    QTimeEvt_ctorX(&me->settle, &me->super, BUTTONS_SETTLE_SIG, 0U);
    QTimeEvt_ctorX(&me->test_tmo, &me->super, BUTTONS_TEST_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->hold, &me->super, BUTTONS_HOLD_TMO_SIG, 0U);
    me->hold_id      = (uint8_t)KDRV_BTN_COUNT;
    me->stable       = 0U;
    me->candidate    = 0U;
    me->exp_levels   = 0xFFU;   /* every pin at rest until the owner says otherwise */
    me->settling     = false;
    me->top_off      = false;
    me->settled      = false;
    me->dump_pending = false;
    me->top_release_ms = 0U;
    me->top_armed      = false;
    me->top_held       = false;
    me->top_doubled    = false;
    me->rot_waiting  = false;
    me->rot_pending  = 0U;
    s_rot = 0U;   /* the pad as wired */
    me->test_on      = false;
    me->test_origin  = TEST_ORIGIN_AUTOTEST;
    me->test_judged  = false;
    me->test_failed  = false;
    me->test_step    = 0U;
    me->test_wrong   = 0U;
    me->test_doubles = 0U;
    me->test_retry   = false;
    me->test_gaveup  = false;
    me->test_pend    = false;
    me->test_seen    = 0U;
    me->test_t0      = 0U;
    me->test_timed   = false;
}

AO_STORAGE(buttons, KQP_AO_QUEUE_LEN, KQP_AO_STACK_SMALL);

void kaos_buttons_start(void)
{
    Buttons_ctor();
    AO_START(AO_Buttons, buttons, KODE_PRIO_BUTTONS);
}

/* ---- The guided test ------------------------------------------------------ */

/**
 * @brief How long the judged press has been down, in milliseconds, saturated to what an event carries.
 *
 * A step that was judged without a press (a button already down as it began,
 * or one that was down when the step timed out) has no press to time: 0.
 */
static uint16_t test_press_ms(Buttons const * const me)
{
    uint16_t ms = 0U;

    if ( me->test_timed )
    {
        uint32_t const held = khal_time_ms() - me->test_t0;

        ms = UINT16_MAX;
        if ( (uint32_t)UINT16_MAX > held )
        {
            ms = (uint16_t)held;
        }
    }
    return ms;
}

/**
 * @brief Publish one step of the test. Bounded, never asserts.
 *
 * @param me      The AO.
 * @param phase   BTN_TEST_*.
 * @param pressed The sample that ended the step; 0 for a prompt.
 * @param ms      How long the press lasted, 0 for a phase that has none.
 */
static void test_publish(Buttons * const me, uint8_t phase, uint8_t pressed, uint16_t ms)
{
    ButtonsTestEvt *ev = Q_NEW_X(ButtonsTestEvt, 0U, BTN_TEST_SIG);
    if ( NULL == ev )
    {
        return;
    }
    ev->phase   = phase;
    ev->step    = (uint8_t)(me->test_step + 1U);
    ev->total   = TEST_STEPS;
    ev->button  = (uint8_t)TEST_SEQ[me->test_step];
    ev->pressed = pressed;
    ev->origin  = me->test_origin;
    ev->ms      = ms;
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
static void test_verdict(Buttons * const me, bool ok, uint8_t origin)
{
    TestResultEvt *ev = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL != ev )
    {
        (void)snprintf(ev->name, sizeof(ev->name), "%s", "buttons");
        ev->ok = ok;
        ev->origin = origin;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

static void test_await_release(Buttons * const me);

/**
 * @brief Ask for the current step's button and start its clock.
 *
 * A step starts with every button up: one that is down is stuck, and the
 * step fails as held without a prompt.
 */
static void test_prompt(Buttons * const me)
{
    me->test_judged = false;
    me->test_timed  = false;
    me->test_pend   = false;
    me->test_seen   = 0U;
    if ( 0U != me->stable )
    {
        KLOG_E(TAG, "test: 0x%02X down as step %u begins; stuck",
               (unsigned)me->stable, (unsigned)(me->test_step + 1U));
        me->test_failed = true;
        me->test_retry  = false;
        test_publish(me, BTN_TEST_HELD, me->stable, 0U);
        me->test_judged = true;
        test_await_release(me);
        return;
    }
    KLOG_N(TAG, "test: step %u of %u - press and release %s",
           (unsigned)(me->test_step + 1U), (unsigned)TEST_STEPS,
           kdrv_button_name(TEST_SEQ[me->test_step]));
    test_publish(me, BTN_TEST_PROMPT, 0U, 0U);
    (void)QTimeEvt_disarm(&me->test_tmo);
    QTimeEvt_armX(&me->test_tmo, KQP_TICKS_PER_MS(KODE_BUTTONS_TEST_STEP_MS), 0U);
}

/** @brief This step is over: the same button again, the next step, or the verdict. */
static void test_advance(Buttons * const me)
{
    (void)QTimeEvt_disarm(&me->test_tmo);
    if ( me->test_retry )
    {
        me->test_retry = false;   /* a forgiven slip: the same button again */
        test_prompt(me);
        return;
    }
    me->test_wrong   = 0U;
    me->test_doubles = 0U;
    if ( me->test_gaveup || ((uint8_t)(me->test_step + 1U) >= TEST_STEPS) )
    {
        me->test_step = (uint8_t)(TEST_STEPS - 1U);
        test_publish(me, BTN_TEST_DONE, 0U, 0U);
        me->test_on = false;
        if ( me->test_failed )
        {
            KLOG_E(TAG, "test: FAILED - see the steps above");
        }
        else
        {
            KLOG_N(TAG, "test: every button answered to its own name");
        }
        test_verdict(me, !me->test_failed, me->test_origin);
        return;
    }
    ++me->test_step;
    test_prompt(me);
}

/**
 * @brief Wait for whatever is held to be released, for KODE_BUTTONS_TEST_RELEASE_MS.
 *
 * The release brings the next prompt, so the operator is never asked for the
 * second half of the hardware reset combination while holding the first. A
 * button still down when the timer runs out is stuck: a failed unit.
 */
static void test_await_release(Buttons * const me)
{
    (void)QTimeEvt_disarm(&me->test_tmo);
    QTimeEvt_armX(&me->test_tmo, KQP_TICKS_PER_MS(KODE_BUTTONS_TEST_RELEASE_MS), 0U);
}

/**
 * @brief The asked-for button went down together with another.
 *
 * Asked again once; the second time on a step it is a fail.
 *
 * @param me   The AO.
 * @param seen Every button that went down in this press.
 */
static void test_double(Buttons * const me, uint8_t seen)
{
    ++me->test_doubles;
    if ( KODE_BUTTONS_TEST_DOUBLE_TRIES > me->test_doubles )
    {
        me->test_retry = true;
        KLOG_N(TAG, "test: %s went down with others (0x%02X); asked again",
               kdrv_button_name(TEST_SEQ[me->test_step]), (unsigned)seen);
        test_publish(me, BTN_TEST_DOUBLE, seen, test_press_ms(me));
    }
    else
    {
        me->test_failed = true;
        me->test_retry  = false;
        KLOG_E(TAG, "test: %s went down with others (0x%02X) again",
               kdrv_button_name(TEST_SEQ[me->test_step]), (unsigned)seen);
        test_publish(me, BTN_TEST_DOUBLEFAIL, seen, test_press_ms(me));
    }
}

/**
 * @brief Judge a confirmed sample against the step in progress.
 *
 * The first non-empty sample of a step is the press. The asked-for button
 * alone passes on its release, unless another goes down first (a double); with
 * others it is a double, and others without it are wrong. A wrong press or a
 * first double is asked again; the third wrong press fails the run. The step
 * then waits for every release (test_await_release()).
 *
 * @param me     The AO.
 * @param sample Every button now down, bit (1 << kdrv_button_t).
 */
static void test_judge(Buttons * const me, uint8_t sample)
{
    if ( !me->test_on )
    {
        return;
    }
    uint8_t const asked = (uint8_t)(1U << (uint8_t)TEST_SEQ[me->test_step]);
    if ( !me->test_judged )
    {
        if ( 0U == sample )
        {
            return;
        }
        me->test_judged = true;
        me->test_seen   = sample;
        me->test_t0     = khal_time_ms();
        me->test_timed  = true;
        if ( sample == asked )
        {
            me->test_pend = true;
        }
        else if ( (sample & asked) != 0U )
        {
            test_double(me, sample);
        }
        else
        {
            ++me->test_wrong;
            if ( KODE_BUTTONS_TEST_TRIES > me->test_wrong )
            {
                me->test_retry = true;
                KLOG_N(TAG, "test: asked for %s, got 0x%02X (wrong %u of %u, asked again)",
                       kdrv_button_name(TEST_SEQ[me->test_step]), (unsigned)sample,
                       (unsigned)me->test_wrong, (unsigned)KODE_BUTTONS_TEST_TRIES);
                test_publish(me, BTN_TEST_WRONG, sample, 0U);
            }
            else
            {
                KLOG_E(TAG, "test: asked for %s, got 0x%02X a third time; the test fails",
                       kdrv_button_name(TEST_SEQ[me->test_step]), (unsigned)sample);
                me->test_failed = true;
                me->test_gaveup = true;
                test_publish(me, BTN_TEST_GIVEUP, sample, 0U);
            }
        }
        test_await_release(me);
    }
    else if ( me->test_pend )
    {
        me->test_seen = (uint8_t)(me->test_seen | sample);
        if ( (me->test_seen & (uint8_t)~asked) != 0U )
        {
            me->test_pend = false;
            test_double(me, me->test_seen);
        }
        else if ( 0U == sample )
        {
            me->test_pend = false;
            KLOG_N(TAG, "test: %s ok", kdrv_button_name(TEST_SEQ[me->test_step]));
            test_publish(me, BTN_TEST_PASS, asked, test_press_ms(me));
            test_advance(me);
        }
        else
        {
            /* Still the asked-for button alone: the release decides. */
        }
    }
    else if ( 0U == sample )
    {
        test_advance(me);
    }
    else
    {
        /* Still held: nothing to say until it is not. */
    }
}

/**
 * @brief `test component=buttons` was asked for.
 *
 * @param me     The AO.
 * @param origin Who asked: stamped on every step of the run it starts.
 */
static void test_start(Buttons * const me, uint8_t origin)
{
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
    /* Before anything is published: the refusal below is this request's. */
    me->test_origin = origin;
    if ( 0U != me->stable )
    {
        KLOG_E(TAG, "test: a button is held (0x%02X); release everything and "
                    "run it again", (unsigned)me->stable);
        me->test_step = 0U;
        test_publish(me, BTN_TEST_HELD, me->stable, 0U);
        test_verdict(me, false, origin);
        return;
    }
    me->test_on      = true;
    me->test_failed  = false;
    me->test_gaveup  = false;
    me->test_wrong   = 0U;
    me->test_doubles = 0U;
    me->test_retry   = false;
    me->test_step    = 0U;
    test_prompt(me);
}

/**
 * @brief Claim the top button, hear the expander, and attach the top button's edge.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Buttons_watching.
 */
static QState Buttons_initial(Buttons * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_BTN_TOP, me, "AO_Buttons");
    QActive_subscribe(&me->super, IOEXP_INPUT_SIG);

    if ( kdrv_buttons_init() != KODE_OK )
    {
        KLOG_W(TAG, "no top button; the other five still arrive from the expander");
    }
    else if ( kdrv_buttons_top_attach(&on_top_edge, me) != KODE_OK )
    {
        KLOG_W(TAG, "GPIO%u will not interrupt; the top button is only read "
                    "alongside the others", (unsigned)KBSP_PIN_BUTTON_TOP);
    }
    else
    {
        KLOG_I(TAG, "up; waiting for edges, settle %ums",
               (unsigned)KODE_SYS_BUTTON_SETTLE_MS);
    }
    return Q_TRAN(&Buttons_watching);
}

/* What confirmed an edge: the sample that agreed with the one before it. It
 * names the sample's source, not the button's pin - the top button lives on
 * GPIO35, but the sample that confirms its press is as often the expander's
 * byte, taken when the settle asked for a second look. */
static const char *const SAMPLE_EXPANDER = "the expander's byte";
static const char *const SAMPLE_GPIO35   = "GPIO35's edge";

/** @brief Is this one of the two buttons that can be held? */
static bool holdable(kdrv_button_t b)
{
    return (KDRV_BTN_TOP == b) || (KDRV_BTN_BOT == b);
}

/**
 * @brief Start or stop the hold clock for a confirmed edge.
 *
 * One clock: a press of the top or bottom button (re)arms it, and the release
 * of the timed button disarms it; the pad never holds. Nothing is armed during
 * the guided test: a hold answered by AO_Boot would reset the line's board.
 *
 * @param me      The AO.
 * @param b       The button whose edge was just confirmed.
 * @param pressed The edge.
 */
static void hold_track(Buttons * const me, kdrv_button_t b, bool pressed)
{
    if ( !holdable(b) )
    {
        return;
    }
    if ( pressed && !me->test_on )
    {
        (void)QTimeEvt_disarm(&me->hold);
        me->hold_id = (uint8_t)b;
        QTimeEvt_armX(&me->hold, KQP_TICKS_PER_MS(KODE_BUTTON_HOLD_MS), 0U);
    }
    else if ( !pressed && ((uint8_t)b == me->hold_id) )
    {
        (void)QTimeEvt_disarm(&me->hold);
        me->hold_id = (uint8_t)KDRV_BTN_COUNT;
    }
    else
    {
        /* A release of a button that was not being timed. */
    }
}

/**
 * @brief Watch the top button's confirmed edges for a double press.
 *
 * A release arms the detector unless its press reached hold or was itself the
 * second of a double (three quick presses make one double). A press within
 * KODE_BUTTON_DOUBLE_MS of an armed release is a double; any press uses up the
 * arming. The gap is an unsigned subtraction, right across the 49-day wrap.
 * Fed only by publish_change(): console injections never make a double.
 *
 * @param me      The AO.
 * @param pressed The top button's confirmed edge.
 * @param t_ms    khal_time_ms() as that edge's ButtonEvt carries it.
 */
static void double_track(Buttons * const me, bool pressed, uint32_t t_ms)
{
    if ( !pressed )
    {
        if ( me->top_held || me->top_doubled )
        {
            me->top_armed = false;
        }
        else
        {
            me->top_armed = true;
        }
        me->top_release_ms = t_ms;
        me->top_held       = false;
        me->top_doubled    = false;
        return;
    }
    uint32_t const gap = t_ms - me->top_release_ms;
    if ( me->top_armed && (KODE_BUTTON_DOUBLE_MS >= gap) )
    {
        me->top_doubled = true;
        ButtonDoubleEvt *ev = Q_NEW_X(ButtonDoubleEvt, 0U, BUTTON_DOUBLE_SIG);
        if ( NULL != ev )
        {
            ev->id   = (uint8_t)KDRV_BTN_TOP;
            ev->t_ms = t_ms;
            QACTIVE_PUBLISH(&ev->super, &me->super);
            KLOG_D(TAG, "top pressed twice, %u ms after the first release",
                   (unsigned)gap);
        }
    }
    me->top_armed = false;
}

/**
 * @brief Announce one button's new state.
 *
 * One log line per edge: a press on the top button launches an app, and a
 * launch nobody made must be traceable to the sample that confirmed it.
 *
 * @param sample SAMPLE_EXPANDER or SAMPLE_GPIO35: where the confirming
 *               sample came from.
 */
static void publish_change(Buttons * const me, kdrv_button_t b, bool pressed,
                           const char *sample)
{
    /* Before the allocation: this is the owner's word about the button even
     * when the pool is too dry to carry it, and a poll should not go on
     * reporting the state before a confirmed edge. */
    uint8_t const published = pad_map((uint8_t)b);
    down_note(published, pressed);
    uint32_t const t_ms = khal_time_ms();   /* kept: the event is not ours once published */
    ButtonEvt *ev = Q_NEW_X(ButtonEvt, 0U, BUTTON_EVT_SIG);
    if ( NULL != ev )
    {
        ev->id      = published;
        ev->pressed = pressed;
        ev->t_ms    = t_ms;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
    /* After this release has gone out under the turn its press had. */
    rot_settle(me);
    if ( NULL == ev )
    {
        return;   /* bounded: dropping a press beats asserting */
    }
    const char *word = "released";
    if ( pressed )
    {
        word = "pressed";
    }
    KLOG_D(TAG, "%s %s, confirmed by %s", kdrv_button_name(b), word, sample);
    hold_track(me, b, pressed);
    if ( KDRV_BTN_TOP == b )
    {
        double_track(me, pressed, t_ms);
    }
}

/**
 * @brief One sample of the six into the debounce.
 *
 * A change moves only when the settle's own sample, taken
 * KODE_SYS_BUTTON_SETTLE_MS after it, agrees with the sample before: other
 * samples (each expander publish, GPIO35's edges) can be microseconds apart and
 * would confirm a transient, and BOT rings on release. The settle is armed once
 * per change, not restarted per sample, or a touched glass publishing every few
 * milliseconds would keep it from ever firing.
 *
 * @param source  SAMPLE_EXPANDER or SAMPLE_GPIO35: where @p sample came from,
 *                for the log line of an edge it confirms.
 * @param settled true when @p sample is the one the settle asked for.
 */
static void feed(Buttons * const me, uint8_t sample, const char *source,
                 bool settled)
{
    uint8_t confirmed = 0U;
    uint8_t released  = 0U;
    if ( settled )
    {
        confirmed = (uint8_t)(sample & me->candidate);
        released  = (uint8_t)(~sample & ~me->candidate);
    }
    uint8_t const next = (uint8_t)((me->stable | confirmed) & ~released);
    me->candidate = sample;

    uint8_t const changed = (uint8_t)(next ^ me->stable);
    me->stable = next;
    for ( uint8_t b = 0U; (uint8_t)KDRV_BTN_COUNT > b; ++b )
    {
        uint8_t const bit = (uint8_t)(1U << b);
        if ( (changed & bit) != 0U )
        {
            publish_change(me, (kdrv_button_t)b, (next & bit) != 0U, source);
        }
    }
    if ( 0U != changed )
    {
        test_judge(me, next);
    }

    if ( (me->candidate == me->stable) && !me->top_off )
    {
        /* Everything agrees: whatever change was waiting was a glitch. */
        (void)QTimeEvt_disarm(&me->settle);
        me->settling = false;
    }
    else if ( (me->candidate == me->stable) && !me->settling )
    {
        /* Everything agrees, but GPIO35's interrupt is off since its edge and
         * only the settle re-arms it. Disarming here after a bounce at the old
         * level would leave the top button dead until a reset, so the settle
         * runs anyway: it changes nothing and re-arms the pin. */
        QTimeEvt_armX(&me->settle, KQP_TICKS_PER_MS(KODE_SYS_BUTTON_SETTLE_MS), 0U);
        me->settling = true;
    }
    else if ( !me->settling )
    {
        QTimeEvt_armX(&me->settle, KQP_TICKS_PER_MS(KODE_SYS_BUTTON_SETTLE_MS), 0U);
        me->settling = true;
    }
    else
    {
        /* Already waiting for the settle's sample; this one does not restart
         * the clock. */
    }
}

/** @brief Answer `btn_read` with the reading that just arrived. */
static void answer_dump(Buttons * const me, uint8_t sample, bool ok)
{
    ButtonsStateEvt *ev = Q_NEW_X(ButtonsStateEvt, 0U, BTN_STATE_SIG);
    if ( NULL != ev )
    {
        ev->pressed = sample;
        ev->ok      = ok;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
    me->dump_pending = false;
}

/**
 * @brief The only state: settles edges into presses, holds, `btn_read` and the guided test.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Buttons_watching(Buttons * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case IOEXP_INPUT_SIG: {
            IoexpInputEvt const * const in = Q_EVT_CAST(IoexpInputEvt);
            if ( !in->ok )
            {
                /* A failed read is NOT a sample. */
                if ( me->dump_pending )
                {
                    KLOG_E(TAG, "cannot read the buttons - is the expander there?");
                    answer_dump(me, 0U, false);
                }
                status = Q_HANDLED();
                break;
            }
            me->exp_levels = in->levels;
            uint8_t const sample = kdrv_buttons_decode(in->levels,
                                                       kdrv_buttons_top_pressed());
            if ( me->dump_pending )
            {
                /* The raw reading, not the debounced belief: the console
                 * answers `btn_read` with it and closes the command. */
                answer_dump(me, sample, true);
            }
            /* The first reading after the settle asked is its own sample, the
             * only one that may confirm a change. */
            if ( me->settled )
            {
                me->settled = false;
                feed(me, sample, SAMPLE_EXPANDER, true);
            }
            else
            {
                feed(me, sample, SAMPLE_EXPANDER, false);
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_TOP_INT_SIG: {
            /* GPIO35 moved; the expander byte is the last one published. The
             * pin's interrupt stays off until the settle re-arms it. */
            me->top_off = true;   /* off until the settle re-arms it */
#if CONFIG_KODE_LOWPOWER
            {
                static const QEvt top = QEVT_INITIALIZER(LOWPOWER_TOP_SIG);

                QACTIVE_POST(AO_LowPower, &top, me);
            }
#endif
            bool const pressed = kdrv_buttons_top_pressed();
            unsigned level = 1U;
            if ( pressed )
            {
                level = 0U;
            }
            KLOG_D(TAG, "GPIO35 edge: level=%u at %lu ms", level,
                   (unsigned long)khal_time_ms());
            feed(me, kdrv_buttons_decode(me->exp_levels, pressed), SAMPLE_GPIO35,
                 false);
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_SETTLE_SIG: {
            me->settling = false;
            me->settled  = true;
            me->top_off  = false;
            kdrv_buttons_top_rearm();
            ioexp_read_request();   /* the confirming sample arrives as IOEXP_INPUT_SIG */
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_ROTATE_SIG: {
            uint8_t const q = Q_EVT_CAST(ButtonsRotateEvt)->quarter_turns;
            if ( (buttons_down() & BUTTONS_PAD_MASK) != 0U )
            {
                /* A press and its release must name the same button. */
                me->rot_pending = q;
                me->rot_waiting = true;
            }
            else
            {
                rot_apply(me, q);
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_PAD_IDLE_SIG: {
            rot_settle(me);
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_DUMP_REQ_SIG: {
            me->dump_pending = true;
            ioexp_read_request();
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_TEST_REQ_SIG:
        case BUTTONS_TEST_REQ_FROM_SIG: {
            uint8_t origin = TEST_ORIGIN_AUTOTEST;

            if ( (enum_t)BUTTONS_TEST_REQ_FROM_SIG == e->sig )
            {
                origin = Q_EVT_CAST(TestReqEvt)->origin;
            }
            /* A hold armed a tick before the run started must not fire into
             * it: the prompts ask for presses, not holds. */
            (void)QTimeEvt_disarm(&me->hold);
            me->hold_id = (uint8_t)KDRV_BTN_COUNT;
            test_start(me, origin);
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_TEST_CANCEL_SIG: {
            if ( me->test_on && (me->test_origin == Q_EVT_CAST(TestReqEvt)->origin) )
            {
                /* Its asker is gone: no more prompts, and no verdict. */
                (void)QTimeEvt_disarm(&me->test_tmo);
                me->test_on    = false;
                me->test_pend  = false;
                me->test_retry = false;
                KLOG_N(TAG, "test: cancelled by origin %u", (unsigned)me->test_origin);
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_TEST_TMO_SIG: {
            if ( !me->test_on )
            {
                /* A run that ended between the tick and its arrival. */
            }
            else if ( !me->test_judged )
            {
                KLOG_E(TAG, "test: nobody pressed %s in %u ms",
                       kdrv_button_name(TEST_SEQ[me->test_step]),
                       (unsigned)KODE_BUTTONS_TEST_STEP_MS);
                me->test_failed = true;
                test_publish(me, BTN_TEST_TIMEOUT, me->stable, 0U);
                if ( 0U == me->stable )
                {
                    test_advance(me);
                }
                else
                {
                    me->test_judged = true;   /* the release will advance it */
                    test_await_release(me);
                }
            }
            else
            {
                /* Judged and still held past the release limit: stuck. */
                KLOG_E(TAG, "test: 0x%02X still held %u ms after %s was judged; "
                            "stuck", (unsigned)me->stable,
                       (unsigned)KODE_BUTTONS_TEST_RELEASE_MS,
                       kdrv_button_name(TEST_SEQ[me->test_step]));
                me->test_failed = true;
                me->test_pend   = false;
                me->test_retry  = false;   /* a stuck button ends the step, forgiven or not */
                test_publish(me, BTN_TEST_STUCK, me->stable, test_press_ms(me));
                test_advance(me);
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTONS_HOLD_TMO_SIG: {
            /* Publish only if the timed button is still down after debounce:
             * a stray expiry after a release must not become a hold. */
            uint8_t const id  = me->hold_id;
            uint8_t const bit = (uint8_t)(1U << id);
            me->hold_id = (uint8_t)KDRV_BTN_COUNT;
            if ( ((uint8_t)KDRV_BTN_COUNT > id) && ((me->stable & bit) != 0U) )
            {
                if ( (uint8_t)KDRV_BTN_TOP == id )
                {
                    me->top_held = true;   /* this press will not start a double */
                }
                ButtonHoldEvt *ev = Q_NEW_X(ButtonHoldEvt, 0U, BUTTON_HOLD_SIG);
                if ( NULL != ev )
                {
                    ev->id = id;
                    QACTIVE_PUBLISH(&ev->super, &me->super);
                    KLOG_I(TAG, "%s held %u ms",
                           kdrv_button_name((kdrv_button_t)id),
                           (unsigned)KODE_BUTTON_HOLD_MS);
                }
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
