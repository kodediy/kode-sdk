/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_inject.c
 * @brief The injected finger: what `touch`, `tap`, `swipe` and `btn` actually publish.
 */
#include <string.h>

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_buttons.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/** @brief Bring AO_Console up once, and leave it idle whatever ran before. */
static void console_idle(void)
{
    qf_support_init();

    owners_up();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }

    qf_support_give_queue(AO_Storage);
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Send one command line in, as AO_Log publishes it. */
static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Feed one command line to AO_Console. */
static void cli(const char *line)
{
    console_idle();
    ao_spy_drain();
    ao_spy_subscribe(TOUCH_EVT_SIG);
    ao_spy_subscribe(BUTTON_EVT_SIG);
    ao_spy_subscribe(BUTTON_HOLD_SIG);
    ao_spy_subscribe(BUTTON_DOUBLE_SIG);
    type_line(line);
}

/**
 * @brief Advance the clock and let AO_Console see its own timers.
 *
 * Not qf_support_pump(): AO_Console is registered as a background AO in
 * console_idle(), and qf_support_tick() drains every background AO except
 * the one named here - which is what makes a specific timer (the tap
 * release, same as the command watchdog in test_console_flow.c) observable
 * one tick at a time instead of being swallowed by the pump.
 */
static void tick(unsigned ticks)
{
    (void)qf_support_tick(AO_Console, ticks);
}

static bool replied(const char *needle)
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

/* KCLI_TAP_HOLD_MS is private to console_ao.c, same reasoning as
 * CMD_TIMEOUT_TICKS in test_console_flow.c: repeated here rather than
 * imported, so a test sharing the number agrees with it by construction.
 * At the 100 Hz QP tick that file also relies on, 60 ms is 6 ticks. */
#define TAP_HOLD_TICKS   ((60U * 100U) / 1000U)

/* KCLI_INJ_RETRY_MS, 60 ms, in the same steps: how long a release that found
 * the pool dry waits before it asks again. */
#define INJ_RETRY_TICKS  ((60U * 100U) / 1000U)

TEST_CASE("touch down publishes a pressed sample", "[console_inject]")
{
    cli("touch x=100 y=200 state=down");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, ev->sig);

    TouchEvt const * const e = (TouchEvt const *)ev;
    TEST_ASSERT_EQUAL_INT16(100, e->x);
    TEST_ASSERT_EQUAL_INT16(200, e->y);
    TEST_ASSERT_TRUE(e->pressed);
}

TEST_CASE("touch up publishes a released sample at the same place",
          "[console_inject]")
{
    cli("touch x=7 y=9 state=up");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, ev->sig);

    TouchEvt const * const e = (TouchEvt const *)ev;
    TEST_ASSERT_EQUAL_INT16(7, e->x);
    TEST_ASSERT_EQUAL_INT16(9, e->y);
    TEST_ASSERT_FALSE(e->pressed);
}

/* Nothing downstream can tell an injected event from a real one, and a real
 * one carries the time it happened - so an injected one carries the time of
 * the command. A subscriber that decides on elapsed time (a double press, a
 * drag) then sees a host's script the way it sees a thumb. */
TEST_CASE("an injected touch carries the time of the command",
          "[console_inject]")
{
    uint32_t const COMMAND_MS = 4242U;

    fake_time_reset();
    fake_time_advance_ms(COMMAND_MS);
    cli("touch x=100 y=200 state=down");

    const QEvt * const touch = ao_spy_take();
    TEST_ASSERT_NOT_NULL(touch);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, touch->sig);
    TEST_ASSERT_EQUAL_UINT32(COMMAND_MS, ((TouchEvt const *)touch)->t_ms);

    cli("btn id=left state=press");

    const QEvt * const button = ao_spy_take();
    TEST_ASSERT_NOT_NULL(button);
    TEST_ASSERT_EQUAL(BUTTON_EVT_SIG, button->sig);
    TEST_ASSERT_EQUAL_UINT32(COMMAND_MS, ((ButtonEvt const *)button)->t_ms);
}

/* "move" is a sample with the finger still down - h_touch derives that as
 * v[2].u != 1U, where 1 is "up"'s index in A_TOUCH_STATES. A regression to
 * == 0U ("down"'s index) would make "move" publish pressed = false and
 * every other case here would still pass, because none of them ever say
 * "move". */
TEST_CASE("touch move publishes a pressed sample, not a release",
          "[console_inject]")
{
    cli("touch x=50 y=60 state=move");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, ev->sig);

    TouchEvt const * const e = (TouchEvt const *)ev;
    TEST_ASSERT_EQUAL_INT16(50, e->x);
    TEST_ASSERT_EQUAL_INT16(60, e->y);
    TEST_ASSERT_TRUE(e->pressed);
}

TEST_CASE("btn publishes the named button", "[console_inject]")
{
    cli("btn id=top state=press");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL(BUTTON_EVT_SIG, ev->sig);

    ButtonEvt const * const e = (ButtonEvt const *)ev;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, e->id);
    TEST_ASSERT_TRUE(e->pressed);
}

/* A coordinate off the panel is refused rather than clamped. Clamping would
 * invent a tap somewhere the caller did not ask for, and a caller that got
 * its arithmetic wrong should hear so. Checking only that nothing was
 * published would also pass a build that renamed or dropped the `x` key
 * entirely - reading the actual `>err` line is what proves the value was
 * looked at and rejected for being out of range, not merely unrecognised. */
TEST_CASE("a coordinate off the panel is refused", "[console_inject]")
{
    cli("touch x=410 y=0 state=down");

    TEST_ASSERT_NULL(ao_spy_take());
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=x"),
        "x=410 is one past KBSP_LCD_H_RES and should be refused as a bad "
        "value for the x key specifically");
}

/* All six ids, not just "top": A_BTN_IDS is hand-written prose next to
 * kdrv_button_t's own declaration order, and nothing stops the two from
 * drifting apart at any position - "top" alone would not notice "left"
 * and "right" transposed, and even that pair would not notice "up" and
 * "down" transposed. This checks every position against kdrv_button_t
 * directly, in one pass. */
TEST_CASE("btn maps every id to its own kdrv_button_t value",
          "[console_inject]")
{
    static const struct {
        const char    *word;
        kdrv_button_t  id;
    } CASES[] = {
        { "up",    KDRV_BTN_UP    },
        { "down",  KDRV_BTN_DOWN  },
        { "left",  KDRV_BTN_LEFT  },
        { "right", KDRV_BTN_RIGHT },
        { "top",   KDRV_BTN_TOP   },
        { "bot",   KDRV_BTN_BOT   },
    };

    for ( unsigned i = 0U; (sizeof(CASES) / sizeof(CASES[0])) > i; ++i )
    {
        char line[32];
        (void)snprintf(line, sizeof(line), "btn id=%s state=press",
                       CASES[i].word);
        cli(line);

        const QEvt * const ev = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(ev, CASES[i].word);
        TEST_ASSERT_EQUAL(BUTTON_EVT_SIG, ev->sig);

        ButtonEvt const * const e = (ButtonEvt const *)ev;
        TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)CASES[i].id, e->id,
                                        CASES[i].word);
        TEST_ASSERT_TRUE_MESSAGE(e->pressed, CASES[i].word);
    }
}

/* `press` has its cases above and this is `release`'s: a BUTTON_EVT_SIG for
 * the named button, let go. It sits between `press` and `hold` in
 * A_BTN_STATES, so a handler that mixed up the indices shows here or in the
 * hold cases below. */
TEST_CASE("btn release publishes the named button released",
          "[console_inject]")
{
    cli("btn id=bot state=release");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL(BUTTON_EVT_SIG, ev->sig);

    ButtonEvt const * const e = (ButtonEvt const *)ev;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, e->id);
    TEST_ASSERT_FALSE(e->pressed);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- btn state=hold: the event AO_Buttons' clock would publish -----------
 * An injected press never reaches AO_Buttons' hold clock, so without this
 * word nothing a host sends can make the board hear a hold - and BOT held is
 * home. The cases read the published event itself, as the press cases above
 * do: a hold that answered `>ok` and published a release, or nothing, is
 * the failure worth catching.
 */

TEST_CASE("btn hold publishes the bottom button's hold and answers ok",
          "[console_inject]")
{
    cli("btn id=bot state=hold");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "the hold never landed");
    TEST_ASSERT_EQUAL(BUTTON_HOLD_SIG, ev->sig);

    ButtonHoldEvt const * const h = (ButtonHoldEvt const *)ev;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, h->id);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "one hold is one event");
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE_MESSAGE(replied("console: injected btn bot hold"),
        "an injected hold must be findable in the log, as a press is");
}

TEST_CASE("btn hold names the top button when the top button is held",
          "[console_inject]")
{
    cli("btn id=top state=hold");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL(BUTTON_HOLD_SIG, ev->sig);

    ButtonHoldEvt const * const h = (ButtonHoldEvt const *)ev;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, h->id);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A real hold comes between a press and its release and is neither: AO_Buttons
 * publishes no BUTTON_EVT_SIG when its clock runs out, and what a poll reads
 * does not move. Every event the spy received is read, so a hold that also
 * published a press or a release shows here whichever order they went out
 * in. */
TEST_CASE("btn hold publishes no button change and moves no polled state",
          "[console_inject]")
{
    uint8_t const before = buttons_down();

    cli("btn id=top state=hold");

    unsigned holds   = 0U;
    unsigned changes = 0U;
    const QEvt *ev   = ao_spy_take();
    while ( NULL != ev )
    {
        if ( (QSignal)BUTTON_HOLD_SIG == ev->sig )
        {
            ++holds;
        }
        else if ( (QSignal)BUTTON_EVT_SIG == ev->sig )
        {
            ++changes;
        }
        else
        {
            /* Touch samples cannot come from a `btn`; not counted. */
        }
        ev = ao_spy_take();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, holds, "exactly one hold");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, changes,
        "a hold is not a press or a release");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(before, buttons_down(),
        "a hold changes no button's state, so a poll must read the same");
}

/* AO_Buttons times the top and the bottom button only, and ButtonHoldEvt
 * promises a subscriber exactly those two ids. A pad hold would be an event
 * the board itself never publishes - kodeOS's interface disarms on any hold,
 * so it would end a pad repeat in a way no thumb can - so all four pad ids
 * are refused, naming the key, and nothing goes out. */
TEST_CASE("btn hold on a pad button is refused, because the pad never holds",
          "[console_inject]")
{
    static const char *const PAD[] = { "up", "down", "left", "right" };

    for ( unsigned i = 0U; (sizeof(PAD) / sizeof(PAD[0])) > i; ++i )
    {
        char line[32];
        (void)snprintf(line, sizeof(line), "btn id=%s state=hold", PAD[i]);
        cli(line);

        TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), PAD[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=id"), PAD[i]);
        TEST_ASSERT_FALSE_MESSAGE(replied("console: injected btn"), PAD[i]);
    }
}

/* ---- btn state=double: the event AO_Buttons' detector would publish -----
 * Injected presses bypass the detector on purpose, so a bench script stays
 * deterministic; this word is the one way to put a double on the bus, and it
 * does so for the top button alone, the only one the board ever doubles.
 */

TEST_CASE("btn double publishes the top button's double and answers ok",
          "[console_inject]")
{
    cli("btn id=top state=double");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(ev, "the double never landed");
    TEST_ASSERT_EQUAL(BUTTON_DOUBLE_SIG, ev->sig);

    ButtonDoubleEvt const * const d = (ButtonDoubleEvt const *)ev;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, d->id);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "one double is one event: no press and no release with it");
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE_MESSAGE(replied("console: injected btn top double"),
        "an injected double must be findable in the log, as a press is");
}

TEST_CASE("an injected double carries the time of the command", "[console_inject]")
{
    uint32_t const CALL_MS = 4321U;

    fake_time_reset();
    fake_time_advance_ms(CALL_MS);
    cli("btn id=top state=double");

    const QEvt * const ev = ao_spy_take();
    TEST_ASSERT_NOT_NULL(ev);
    TEST_ASSERT_EQUAL(BUTTON_DOUBLE_SIG, ev->sig);
    TEST_ASSERT_EQUAL_UINT32(CALL_MS, ((ButtonDoubleEvt const *)ev)->t_ms);
}

TEST_CASE("btn double moves no polled state", "[console_inject]")
{
    uint8_t const before = buttons_down();

    cli("btn id=top state=double");

    TEST_ASSERT_EQUAL_HEX8_MESSAGE(before, buttons_down(),
        "a double changes no button's state, so a poll must read the same");
}

/* Only the top button doubles, and ButtonDoubleEvt promises that id. The
 * other five are refused, naming the key, and nothing goes out. */
TEST_CASE("btn double refuses any other id", "[console_inject]")
{
    static const char *const OTHERS[] = { "up", "down", "left", "right", "bot" };

    for ( unsigned i = 0U; (sizeof(OTHERS) / sizeof(OTHERS[0])) > i; ++i )
    {
        char line[32];
        (void)snprintf(line, sizeof(line), "btn id=%s state=double", OTHERS[i]);
        cli(line);

        TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), OTHERS[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=id"), OTHERS[i]);
        TEST_ASSERT_FALSE_MESSAGE(replied("console: injected btn"), OTHERS[i]);
    }
}

/* Four words, and still a closed enum: a word that is not one of them is
 * the grammar's own refusal, naming the key. */
TEST_CASE("btn with a state word it does not know is refused as a bad state",
          "[console_inject]")
{
    cli("btn id=top state=held");

    TEST_ASSERT_NULL(ao_spy_take());
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=state"),
        "an unknown state word is a bad value for the state key");
}

/* ---- tap: the two-step property ---------------------------------------
 * h_tap exists at all because publishing a press and a release in the same
 * run-to-completion step would give LVGL two samples in one pass, and its
 * input reader would see no press. So the property worth testing is not
 * "a tap eventually releases" - it is "the release is not there yet the
 * instant the press is". The ao_spy_count() check below is what actually
 * catches a regression that collapses h_tap back into one step; without
 * it this case would pass whether the release fires early or on time.
 */
TEST_CASE("a tap presses now and releases only after the hold, not both at "
          "once", "[console_inject]")
{
    cli("tap x=205 y=250");

    const QEvt * const press = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(press, "the press itself never landed");
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, press->sig);

    TouchEvt const * const p = (TouchEvt const *)press;
    TEST_ASSERT_EQUAL_INT16(205, p->x);
    TEST_ASSERT_EQUAL_INT16(250, p->y);
    TEST_ASSERT_TRUE(p->pressed);

    /* THE assertion: a press and a release published in the same step
     * would already have both waiting here, with the clock never having
     * moved at all. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the release must not be queued before the hold has run");

    tick(TAP_HOLD_TICKS + 2U);

    const QEvt * const release = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(release, "the hold ran out with no release");
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, release->sig);

    TouchEvt const * const r = (TouchEvt const *)release;
    TEST_ASSERT_EQUAL_INT16(205, r->x);
    TEST_ASSERT_EQUAL_INT16(250, r->y);
    TEST_ASSERT_FALSE(r->pressed);
}

/* A tap is two samples, and each carries its own time: the press the time of
 * the command, the release the time the hold ran out. A release stamped with
 * the press's time would read as a finger that never lingered. */
TEST_CASE("a tap's release carries the time the hold ran out, not the "
          "press's", "[console_inject]")
{
    uint32_t const COMMAND_MS = 5000U;
    uint32_t const HOLD_MS    = 100U;   /* past KCLI_TAP_HOLD_MS */

    fake_time_reset();
    fake_time_advance_ms(COMMAND_MS);
    cli("tap x=205 y=250");

    const QEvt * const press = ao_spy_take();
    TEST_ASSERT_NOT_NULL(press);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, press->sig);
    TEST_ASSERT_EQUAL_UINT32(COMMAND_MS, ((TouchEvt const *)press)->t_ms);

    fake_time_advance_ms(HOLD_MS);
    tick(TAP_HOLD_TICKS + 2U);

    const QEvt * const release = ao_spy_take();
    TEST_ASSERT_NOT_NULL(release);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, release->sig);
    TEST_ASSERT_EQUAL_UINT32(COMMAND_MS + HOLD_MS,
                             ((TouchEvt const *)release)->t_ms);
}

/* One tap in flight at a time. A second `tap` arriving before the first's
 * release is not merely refused - it must not touch inj_x/inj_y, or the
 * eventual release would land wherever the refused call pointed instead of
 * where the armed one did. There is no direct getter for those fields, so
 * this reads them the only way a host could: through the release itself. */
TEST_CASE("a second tap while one is armed is refused, and does not move it",
          "[console_inject]")
{
    cli("tap x=10 y=20");

    const QEvt * const press = ao_spy_take();
    TEST_ASSERT_NOT_NULL(press);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, press->sig);

    fake_klog_reset();
    type_line("tap x=99 y=99");

    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4 msg=busy"),
        "a second tap while one is armed must be refused as busy");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a refused tap must publish nothing");

    tick(TAP_HOLD_TICKS + 2U);

    const QEvt * const release = ao_spy_take();
    TEST_ASSERT_NOT_NULL(release);
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, release->sig);

    /* The FIRST tap's coordinates, not the refused second one's - proof
     * that the busy call never touched inj_x/inj_y. */
    TouchEvt const * const r = (TouchEvt const *)release;
    TEST_ASSERT_EQUAL_INT16(10, r->x);
    TEST_ASSERT_EQUAL_INT16(20, r->y);
    TEST_ASSERT_FALSE(r->pressed);
}

/* h_touch's own busy guard (see its doc comment in console_ao.c): a `touch`
 * arriving while a `tap` is armed would race the pending release for
 * LVGL's last sample, the exact hazard a second `tap` is already refused
 * for. */
TEST_CASE("touch is refused while a tap is armed, for the same reason a "
          "second tap is", "[console_inject]")
{
    cli("tap x=1 y=2");

    const QEvt * const press = ao_spy_take();
    TEST_ASSERT_NOT_NULL(press);

    fake_klog_reset();
    type_line("touch x=3 y=4 state=down");

    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4 msg=busy"),
        "a touch while a tap's release is still owed must be refused");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a refused touch must publish nothing");

    /* Let the armed tap's own release fire, leaving the console idle. */
    tick(TAP_HOLD_TICKS + 2U);
    (void)ao_spy_take();
}

/* ---- swipe: a finger replayed at the touch sensor's rate -------------------
 * The console cannot be told the time of every sample over a cable, so it
 * makes them itself: one a KODE_TOUCH_POLL_MS, each stamped with the time it
 * was PLANNED for, not the time it went out. The cases below read those stamps
 * and the positions, because the speed of a replay is nothing else.
 *
 * One tick() is one 10 ms step, and the sensor's poll period is the same 10:
 * the first assert says so, so a change to either fails here and not in a
 * count that is off by one. */
_Static_assert(KODE_TOUCH_POLL_MS == 10U,
               "a tick() step is 10 ms; a swipe sample is one per poll");

/** @brief The next event, which must be a TouchEvt. */
static TouchEvt const *take_touch(void)
{
    const QEvt * const e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "a sample is missing");
    TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, e->sig);
    return (TouchEvt const *)e;
}

/**
 * @brief Check one sample, naming its place in the replay when it is wrong.
 * @param n       Its number in the replay, for the message: 0 is the press.
 * @param e       The sample.
 * @param x       Where it must be.
 * @param y       Where it must be.
 * @param pressed Whether the finger is down.
 * @param t_ms    The stamp it must carry.
 */
static void expect_touch(unsigned n, TouchEvt const *e, int16_t x, int16_t y,
                         bool pressed, uint32_t t_ms)
{
    char msg[40];

    (void)snprintf(msg, sizeof(msg), "sample %u: x", n);
    TEST_ASSERT_EQUAL_INT16_MESSAGE(x, e->x, msg);
    (void)snprintf(msg, sizeof(msg), "sample %u: y", n);
    TEST_ASSERT_EQUAL_INT16_MESSAGE(y, e->y, msg);
    (void)snprintf(msg, sizeof(msg), "sample %u: pressed", n);
    TEST_ASSERT_EQUAL_MESSAGE(pressed, e->pressed, msg);
    (void)snprintf(msg, sizeof(msg), "sample %u: t_ms", n);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(t_ms, e->t_ms, msg);
}

/** @brief Nothing is waiting: the step that just ran published nothing. */
static void expect_quiet(const char *why)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), why);
}

TEST_CASE("swipe publishes a press, a move every ten milliseconds and a "
          "release", "[console_inject]")
{
    uint32_t const T = 5000U;

    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=205 y0=20 x1=205 y1=420 ms=200");

    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the swipe answers at the start");
    TEST_ASSERT_TRUE_MESSAGE(
        replied("console: injected swipe 205,20 -> 205,420 in 200 ms, "
                "hold 0 ms"),
        "the swipe says what it is replaying");

    /* The press is out at once, stamped with the time of the command, and
     * nothing else with it. */
    expect_touch(0U, take_touch(), 205, 20, true, T);
    expect_quiet("only the press goes out at the command");

    for ( unsigned k = 1U; 20U >= k; ++k )
    {
        tick(1U);
        expect_touch(k, take_touch(), 205, (int16_t)(20U + (20U * k)), true,
                     T + (10U * k));
        if ( 20U > k )
        {
            expect_quiet("one move a step");
        }
    }

    /* With no hold the finger lifts where the last move put it, in the step
     * that made that move: both are planned for T + 200. */
    expect_touch(21U, take_touch(), 205, 420, false, T + 200U);
    expect_quiet("a swipe is a press, twenty moves and a release");

    tick(10U);
    expect_quiet("a finished swipe publishes nothing more");
}

TEST_CASE("a swipe that is not a whole number of samples ends on time",
          "[console_inject]")
{
    uint32_t const T = 7000U;

    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=0 y0=0 x1=100 y1=200 ms=25");

    expect_touch(0U, take_touch(), 0, 0, true, T);

    tick(1U);
    expect_touch(1U, take_touch(), 40, 80, true, T + 10U);
    expect_quiet("the second move is not due yet");

    tick(1U);
    expect_touch(2U, take_touch(), 80, 160, true, T + 20U);
    expect_quiet("the last move is not due yet");

    /* Not T + 30: the plan ends at T + 25, and so does the last move, which
     * is exactly the end point rather than a step short of it. */
    tick(1U);
    expect_touch(3U, take_touch(), 100, 200, true, T + 25U);
    expect_touch(4U, take_touch(), 100, 200, false, T + 25U);
    expect_quiet("no sample past the plan");
}

TEST_CASE("hold keeps the finger still before the release", "[console_inject]")
{
    uint32_t const T = 9000U;

    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=205 y0=20 x1=205 y1=420 ms=200 hold=100");

    TEST_ASSERT_TRUE_MESSAGE(
        replied("in 200 ms, hold 100 ms"),
        "the swipe says how long it holds");
    expect_touch(0U, take_touch(), 205, 20, true, T);

    for ( unsigned k = 1U; 20U >= k; ++k )
    {
        tick(1U);
        expect_touch(k, take_touch(), 205, (int16_t)(20U + (20U * k)), true,
                     T + (10U * k));
    }

    /* A resting finger publishes nothing: AO_Touch does not either. */
    for ( unsigned h = 1U; 10U > h; ++h )
    {
        tick(1U);
        expect_quiet("a held finger publishes nothing");
    }

    tick(1U);
    expect_touch(21U, take_touch(), 205, 420, false, T + 300U);
    expect_quiet("one release");
}

TEST_CASE("swipe rounds each position to the nearest, ties away from where "
          "it started", "[console_inject]")
{
    uint32_t const T = 100U;

    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=0 y0=10 x1=10 y1=0 ms=30");
    (void)take_touch();

    /* A third, two thirds, all of it: 3.33 -> 3, 6.67 -> 7, 10. The way back
     * is the same numbers mirrored, so a swipe has no favourite direction. */
    tick(1U);
    expect_touch(1U, take_touch(), 3, 7, true, T + 10U);
    tick(1U);
    expect_touch(2U, take_touch(), 7, 3, true, T + 20U);
    tick(1U);
    expect_touch(3U, take_touch(), 10, 0, true, T + 30U);
    expect_touch(4U, take_touch(), 10, 0, false, T + 30U);

    /* Half a pixel on each axis, in opposite directions. */
    cli("swipe x0=0 y0=1 x1=1 y1=0 ms=20");
    (void)take_touch();
    tick(1U);
    expect_touch(5U, take_touch(), 1, 0, true, T + 10U);
    tick(1U);
    (void)take_touch();
    (void)take_touch();
}

TEST_CASE("swipe runs from the shortest to the longest replay the grammar "
          "allows", "[console_inject]")
{
    uint32_t const T = 1U;

    /* The shortest: one sample, the whole way, at the corner of the glass. */
    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=409 y0=501 x1=0 y1=0 ms=10");
    TEST_ASSERT_TRUE(replied(">ok"));
    expect_touch(0U, take_touch(), 409, 501, true, T);
    tick(1U);
    expect_touch(1U, take_touch(), 0, 0, true, T + 10U);
    expect_touch(2U, take_touch(), 0, 0, false, T + 10U);
    expect_quiet("one move and the release");

    /* The longest: 500 moves, a hold of as many steps, and the release at
     * the end of both. Read a step at a time, the way a host would, because
     * the spy and the pool hold far fewer than 500 samples. */
    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=0 y0=0 x1=409 y1=501 ms=5000 hold=5000");
    TEST_ASSERT_TRUE(replied(">ok"));
    (void)take_touch();
    ao_spy_release();

    unsigned moves = 0U;
    unsigned releases = 0U;
    uint32_t last_t = 0U;
    for ( unsigned step = 1U; 1000U >= step; ++step )
    {
        tick(1U);
        const QEvt *e;
        while ( (e = ao_spy_take()) != NULL )
        {
            TouchEvt const * const t = (TouchEvt const *)e;
            TEST_ASSERT_EQUAL(TOUCH_EVT_SIG, e->sig);
            if ( t->pressed )
            {
                ++moves;
            }
            else
            {
                ++releases;
            }
            last_t = t->t_ms;
            if ( 500U == moves )
            {
                TEST_ASSERT_EQUAL_INT16(409, t->x);
                TEST_ASSERT_EQUAL_INT16(501, t->y);
            }
        }
        ao_spy_release();
    }
    TEST_ASSERT_EQUAL_UINT(500U, moves);
    TEST_ASSERT_EQUAL_UINT(1U, releases);
    TEST_ASSERT_EQUAL_UINT32(T + 10000U, last_t);
}

TEST_CASE("a second injected gesture is busy", "[console_inject]")
{
    uint32_t const T = 3000U;

    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=100 y0=100 x1=100 y1=300 ms=100");
    expect_touch(0U, take_touch(), 100, 100, true, T);

    static const char *const others[] = {
        "swipe x0=1 y0=2 x1=3 y1=4 ms=50",
        "tap x=5 y=6",
        "touch x=7 y=8 state=down",
    };
    for ( unsigned i = 0U; (sizeof(others) / sizeof(others[0])) > i; ++i )
    {
        fake_klog_reset();
        type_line(others[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4 msg=busy"), others[i]);
        expect_quiet("a refused gesture publishes nothing");
    }

    /* The refusals changed nothing about the swipe in flight. */
    for ( unsigned k = 1U; 10U >= k; ++k )
    {
        tick(1U);
        expect_touch(k, take_touch(), 100, (int16_t)(100U + (20U * k)), true,
                     T + (10U * k));
    }
    expect_touch(11U, take_touch(), 100, 300, false, T + 100U);

    /* And once it has released, the finger is free again. */
    cli("tap x=1 y=2");
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "a tap after the swipe is taken");
    (void)take_touch();
    tick(TAP_HOLD_TICKS + 2U);
    (void)take_touch();
}

TEST_CASE("swipe is refused while a tap is armed", "[console_inject]")
{
    uint32_t const T = 2000U;

    fake_time_reset();
    fake_time_advance_ms(T);
    cli("tap x=1 y=2");
    (void)take_touch();

    fake_klog_reset();
    type_line("swipe x0=1 y0=2 x1=3 y1=4 ms=50");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4 msg=busy"),
        "a swipe while a tap's release is still owed must be refused");
    expect_quiet("a refused swipe publishes nothing");

    tick(TAP_HOLD_TICKS + 2U);
    expect_touch(1U, take_touch(), 1, 2, false, T);
    tick(5U);
    expect_quiet("the refused swipe left nothing armed");
}

TEST_CASE("swipe refuses what it cannot replay", "[console_inject]")
{
    static const struct {
        const char *line;
        const char *answer;
    } refused[] = {
        { "swipe x0=410 y0=0 x1=0 y1=0 ms=100",   ">err code=3 msg=x0" },
        { "swipe x0=0 y0=502 x1=0 y1=0 ms=100",   ">err code=3 msg=y0" },
        { "swipe x0=0 y0=0 x1=410 y1=0 ms=100",   ">err code=3 msg=x1" },
        { "swipe x0=0 y0=0 x1=0 y1=502 ms=100",   ">err code=3 msg=y1" },
        { "swipe x0=0 y0=0 x1=0 y1=0 ms=5",       ">err code=3 msg=ms" },
        { "swipe x0=0 y0=0 x1=0 y1=0 ms=9",       ">err code=3 msg=ms" },
        { "swipe x0=0 y0=0 x1=0 y1=0 ms=5001",    ">err code=3 msg=ms" },
        { "swipe x0=0 y0=0 x1=0 y1=0 ms=100 hold=5001",
                                                  ">err code=3 msg=hold" },
        { "swipe x0=0 y0=0 x1=0 y1=0 ms=100 hold=-1",
                                                  ">err code=3 msg=hold" },
        { "swipe x0=0 y0=0 x1=0 y1=0",             ">err code=2 msg=ms" },
        { "swipe y0=0 x1=0 y1=0 ms=100",          ">err code=2 msg=x0" },
    };

    for ( unsigned i = 0U; (sizeof(refused) / sizeof(refused[0])) > i; ++i )
    {
        cli(refused[i].line);
        TEST_ASSERT_TRUE_MESSAGE(replied(refused[i].answer), refused[i].line);
        expect_quiet("a refused swipe publishes nothing");
        tick(3U);
        expect_quiet("a refused swipe arms nothing");
    }
}

/* A move or a release the pool has no block for must not be dropped: the
 * press is already on the bus, and a finger that never lifts pins whatever it
 * landed on. The stamps are the plan's, so the late sample is still where and
 * when the replay said it was.
 *
 * The pool is shared by every case in the binary and a failed assert leaves
 * the case where it stands, so nothing is asserted while a block is held:
 * what the dry steps did is read into variables, the blocks go back, and only
 * then is anything judged. */
TEST_CASE("a swipe whose release finds the pool dry retries it, with the "
          "planned stamp", "[console_inject]")
{
    uint32_t const T = 40000U;
    TouchEvt *held[KQP_SMALL_POOL_COUNT];
    unsigned n_held = 0U;

    fake_time_reset();
    fake_time_advance_ms(T);
    cli("swipe x0=10 y0=10 x1=30 y1=50 ms=20 hold=100");
    expect_touch(0U, take_touch(), 10, 10, true, T);
    tick(1U);
    expect_touch(1U, take_touch(), 20, 30, true, T + 10U);
    tick(1U);
    expect_touch(2U, take_touch(), 30, 50, true, T + 20U);
    ao_spy_release();

    /* Every block of the pool TouchEvt comes from, taken. */
    TouchEvt *e;
    while ( (KQP_SMALL_POOL_COUNT > n_held)
           && ((e = Q_NEW_X(TouchEvt, 0U, TOUCH_EVT_SIG)) != NULL) )
    {
        held[n_held] = e;
        ++n_held;
    }
    e = Q_NEW_X(TouchEvt, 0U, TOUCH_EVT_SIG);
    bool const was_dry = (NULL == e);
    if ( NULL != e )
    {
        QF_gc(&e->super);
    }

    fake_klog_reset();
    tick(10U);   /* the hold runs out at the tenth: the release is due */
    unsigned const out_when_due = ao_spy_count();
    tick(2U);
    unsigned const out_while_dry = ao_spy_count();

    for ( unsigned i = 0U; i < n_held; ++i )
    {
        QF_gc(&held[i]->super);
    }

    TEST_ASSERT_TRUE_MESSAGE(was_dry, "the pool could not be emptied");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, out_when_due,
        "a release with no block to carry it publishes nothing");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, out_while_dry, "and keeps waiting");

    tick(INJ_RETRY_TICKS + 2U);
    expect_touch(3U, take_touch(), 30, 50, false, T + 120U);
    expect_quiet("the release goes out once");

    unsigned warnings = 0U;
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, "stalled") != NULL) )
        {
            ++warnings;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, warnings,
        "a stall is said once, not once a retry");

    tick(20U);
    expect_quiet("nothing is armed after the release");
}
