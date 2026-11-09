/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_led_test.c
 * @brief The `led` row: what the LED is driven to, what is asked, and what an answer does.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_buttons.h"
#include "kode_log.h"
#include "kqp.h"
#include "qf_support.h"

/* The KTD2026's registers, repeated from test_kdrv_led.c for that file's
 * reason: importing the driver's own constants would agree by construction. */
#define REG_LED_EN      0x04U
#define REG_ISET1       0x06U
#define REG_ISET2       0x07U
#define REG_ISET3       0x08U
#define CODE_ZERO       0U
/* The row drives a sink at KODE_LED_TEST_LEVEL (96 of 255): 5.0 mA x 96/255
 * = 1.882 mA, 0.125 mA a code, rounded and less one - 14. */
#define CODE_ROW        14U

/** @brief The channel-enable byte for one lit sink, and for none. */
#define EN_NONE         0x00U
/* The dies hang on the sinks the board gave them: red on OUT3, green on
 * OUT1, blue on OUT2. */
#define EN_R            0x10U
#define EN_G            0x01U
#define EN_B            0x04U

/* QP's subscriber sets, one per published signal. Package-private to QP and
 * declared here rather than reached through qp_pkg.h, which hides it behind
 * QP_IMPL: one case below has to ask whether an AO is subscribed, and nothing
 * public answers that. */
extern QSubscrList * QActive_subscrList_;

/** @brief The operator's deadline for one colour in whole QP ticks, and one
 *         past it. */
#define STEP_TICKS      ((KODE_LED_TEST_STEP_MS / 10U) + 1U)
/** @brief One heartbeat period in whole QP ticks, and one past it. */
#define BLINK_TICKS     ((KODE_LED_BLINK_MS / 10U) + 1U)

/* ---- What reached the chip -------------------------------------------------- */

#define DRIVE_MAX 16

/** @brief One colour as the chip was told it: three currents and the sinks. */
typedef struct {
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t en;
} drive_t;

static drive_t  s_drive[DRIVE_MAX];
static unsigned s_drives;
static drive_t  s_partial;

static kode_status_t on_write_reg(khal_i2c_dev_t dev, uint8_t reg, uint8_t val,
                                  int calls)
{
    (void)dev;
    (void)calls;

    switch ( reg )
    {
        case REG_ISET1: {
            s_partial.r = val;
            break;
        }
        case REG_ISET2: {
            s_partial.g = val;
            break;
        }
        case REG_ISET3: {
            s_partial.b = val;
            break;
        }
        case REG_LED_EN: {
            /* The enable is the last of the four, so it is what commits the
             * colour: nothing on the glass changes until the sinks are told
             * to conduct. */
            s_partial.en = val;
            if ( (unsigned)DRIVE_MAX > s_drives )
            {
                s_drive[s_drives] = s_partial;
            }
            ++s_drives;
            break;
        }
        default: {
            break;
        }
    }
    return KODE_OK;
}

/** @brief Forget every colour the chip was told. */
static void drives_forget(void)
{
    (void)memset(s_drive, 0, sizeof(s_drive));
    (void)memset(&s_partial, 0, sizeof(s_partial));
    s_drives = 0U;
}

/**
 * @brief Insist the @p idx-th colour the chip was driven to is this one.
 *
 * @param idx  Which drive, in the order they happened.
 * @param r    ISET1's code.
 * @param g    ISET2's code.
 * @param b    ISET3's code.
 * @param en   The channel-enable byte.
 * @param what What to say when it is not.
 */
static void drive_expect(unsigned idx, uint8_t r, uint8_t g, uint8_t b,
                         uint8_t en, const char *what)
{
    TEST_ASSERT_TRUE_MESSAGE(s_drives > idx, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(r, s_drive[idx].r, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(g, s_drive[idx].g, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(b, s_drive[idx].b, what);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(en, s_drive[idx].en, what);
}

/** @brief Insist the LED was put out by the @p idx-th drive. */
static void dark_expect(unsigned idx, const char *what)
{
    drive_expect(idx, CODE_ZERO, CODE_ZERO, CODE_ZERO, EN_NONE, what);
}

/* ---- Bringing the AO up ----------------------------------------------------- */

/** @brief Arm or disarm the heartbeat, the way the console's verb does. */
static void heartbeat(bool on)
{
    LedEvt e = { .super = QEVT_INITIALIZER(LED_HEARTBEAT_ENABLE_SIG),
                 .on = false };

    e.on = on;
    QASM_DISPATCH(AO_LedMgr, &e.super, 0U);
}

/**
 * @brief Bring AO_LedMgr up.
 *
 * test_led_mgr.c's led_mgr_bring_up(), plus the queue - the row arms a
 * one-shot, and a QTimeEvt with nowhere to expire is not a timer.
 * qf_support_claim_ao() is what keeps the two led files from registering one
 * AO twice, whichever of them the linker puts first.
 *
 * No bus traffic is expected here and that is a fact about kdrv_led: it keeps
 * its device handle in a file static and kdrv_led_init() returns early once
 * it is set, so the chip was brought up by the driver's own cases earlier in
 * the binary.
 */
static void led_test_up_once(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_subscribe(LED_TEST_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);
    ao_spy_drain();

    if ( qf_support_claim_ao(AO_LedMgr) )
    {
        LedMgr_ctor();
        AO_LedMgr->prio  = KODE_PRIO_LED;
        AO_LedMgr->pthre = KODE_PRIO_LED;
        QActive_register_(AO_LedMgr);
        QASM_INIT(AO_LedMgr, (void *)0, 0U);
    }
    qf_support_give_queue(AO_LedMgr);

    khal_i2c_write_reg_StubWithCallback(&on_write_reg);

    /* A case that failed mid-row was abandoned by Unity where it stood, so
     * this AO can arrive here inside the row with a deadline armed and a
     * heartbeat waiting to be given back. Run it out and put the timers down
     * before the next case asserts anything: a leftover periodic timer posts
     * into a queue nobody drains, and QP answers a full queue with a reset
     * somewhere else entirely. */
    (void)qf_support_tick(AO_LedMgr, STEP_TICKS);
    heartbeat(false);

    drives_forget();
    fake_klog_reset();
    ao_spy_drain();
}

/**
 * @brief Run out whatever the case left in flight and give the events back.
 *
 * Two things must be true of this AO when a case ends: no row is running, and
 * no timer is armed. An armed one left behind is what corrupts QF's list for
 * whichever file constructs an AO next, and a heartbeat left ticking fills
 * this AO's queue on the next file's pump.
 */
static void led_test_down(void)
{
    (void)qf_support_tick(AO_LedMgr, STEP_TICKS);
    heartbeat(false);
    ao_spy_drain();
}

/* ---- Driving the row --------------------------------------------------------- */

/** @brief Ask for the row the way `test component=led` does. */
static void row_request(void)
{
    static const QEvt req = QEVT_INITIALIZER(LED_TEST_REQ_SIG);

    QASM_DISPATCH(AO_LedMgr, &req, 0U);
}

/** @brief One edge of one button, as AO_Buttons publishes it. */
static void button(kdrv_button_t id, bool pressed)
{
    ButtonEvt e = { .super = QEVT_INITIALIZER(BUTTON_EVT_SIG),
                    .id = 0U, .pressed = false };

    e.id      = (uint8_t)id;
    e.pressed = pressed;
    QASM_DISPATCH(AO_LedMgr, &e.super, 0U);
}

/** @brief The operator says yes: a press of BUTTON_TOP, and its release. */
static void answer_yes(void)
{
    button(KDRV_BTN_TOP, true);
    button(KDRV_BTN_TOP, false);
}

/** @brief The operator says no: a press of BUTTON_BOT, and its release. */
static void answer_no(void)
{
    button(KDRV_BTN_BOT, true);
    button(KDRV_BTN_BOT, false);
}

/** @brief Take the next event the spy holds and insist it is @p sig. */
static const QEvt *spy_expect(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the row said nothing at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sig, (int)e->sig, "the wrong record");
    return e;
}

/** @brief Take the next record and insist on its step, phase and colour. */
static void record_expect(uint8_t colour, uint8_t phase)
{
    const LedTestEvt *r = (const LedTestEvt *)spy_expect(LED_TEST_SIG);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(phase, r->phase, "the wrong kind of record");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(colour, r->colour, "a record about the wrong colour");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(colour + 1U), r->step,
                                    "a question and its outcome share the colour's step");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)LED_COLOUR_COUNT, r->total);
}

/** @brief Insist the row ended - the closing record, then the verdict. */
static void verdict_expect(uint8_t colour, bool ok)
{
    record_expect(colour, (uint8_t)LED_TEST_DONE);

    const TestResultEvt *v = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);

    TEST_ASSERT_EQUAL_STRING("led", v->name);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)ok, (int)v->ok, "the wrong verdict");
}

/** @brief Insist the @p idx-th drive lit @p colour and nothing else. */
static void lit_expect(unsigned idx, uint8_t colour, const char *what)
{
    static const uint8_t ISET[LED_COLOUR_COUNT][3] = {
        { CODE_ZERO, CODE_ZERO, CODE_ROW },   /* red:   OUT3 */
        { CODE_ROW,  CODE_ZERO, CODE_ZERO },  /* green: OUT1 */
        { CODE_ZERO, CODE_ROW,  CODE_ZERO }   /* blue:  OUT2 */
    };
    static const uint8_t EN[LED_COLOUR_COUNT] = { EN_R, EN_G, EN_B };

    drive_expect(idx, ISET[colour][0], ISET[colour][1], ISET[colour][2],
                 EN[colour], what);
}

/**
 * @brief Start the row and confirm every colour before @p colour.
 *
 * Leaves the question about @p colour outstanding, so the case decides what
 * the operator says.
 *
 * @param colour LED_COLOUR_*
 */
static void row_to_colour(uint8_t colour)
{
    drives_forget();
    row_request();
    for ( uint8_t c = (uint8_t)LED_COLOUR_RED; ; ++c )
    {
        lit_expect((unsigned)c, c, "the row lit the wrong sink");
        record_expect(c, (uint8_t)LED_TEST_ASK);
        if ( c == colour )
        {
            break;
        }
        answer_yes();
        record_expect(c, (uint8_t)LED_TEST_PASS);
    }
}

/** @brief Confirm @p colour and every one after it, through to the verdict. */
static void row_pass_from(uint8_t colour)
{
    for ( uint8_t c = colour; ; ++c )
    {
        answer_yes();
        record_expect(c, (uint8_t)LED_TEST_PASS);
        if ( (uint8_t)LED_COLOUR_BLUE == c )
        {
            break;
        }
        record_expect((uint8_t)(c + 1U), (uint8_t)LED_TEST_ASK);
    }
    verdict_expect((uint8_t)LED_COLOUR_BLUE, true);
}

/* ---- 1. The heartbeat ------------------------------------------------------- */

/* The heartbeat is this AO's own, and it is the one thing that can put a
 * colour on the chip while somebody is being asked about another. It has to
 * stop for the row and it has to come back exactly as it was found - a
 * production row that left the status light dead would be a board an
 * operator cannot read. */
TEST_CASE("the row stops the heartbeat and puts it back", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    heartbeat(true);
    row_to_colour((uint8_t)LED_COLOUR_RED);

    /* A whole blink period inside the row. Nothing may toggle. */
    (void)qf_support_tick(AO_LedMgr, BLINK_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_drives,
        "the heartbeat blinked over the colour being asked about");

    row_pass_from((uint8_t)LED_COLOUR_RED);

    drives_forget();
    (void)qf_support_tick(AO_LedMgr, BLINK_TICKS);
    TEST_ASSERT_TRUE_MESSAGE(0U < s_drives,
        "the heartbeat never came back and the board went dark");

    /* And the other half: a row run with no heartbeat must not start one. */
    heartbeat(false);
    row_to_colour((uint8_t)LED_COLOUR_RED);
    row_pass_from((uint8_t)LED_COLOUR_RED);
    drives_forget();
    (void)qf_support_tick(AO_LedMgr, BLINK_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_drives,
        "the row started a heartbeat that was never running");

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- 2. The whole row ------------------------------------------------------- */

/* The order is the contract: one colour lit, then its question, then the
 * answer, then the next colour; and the LED out again at the end. */
TEST_CASE("three confirmed colours pass and the row ends dark", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    row_to_colour((uint8_t)LED_COLOUR_BLUE);
    answer_yes();
    record_expect((uint8_t)LED_COLOUR_BLUE, (uint8_t)LED_TEST_PASS);
    verdict_expect((uint8_t)LED_COLOUR_BLUE, true);

    /* Red, green, blue, off: four drives and not one more. */
    lit_expect(0U, (uint8_t)LED_COLOUR_RED, "red never lit");
    lit_expect(1U, (uint8_t)LED_COLOUR_GREEN, "green never lit");
    lit_expect(2U, (uint8_t)LED_COLOUR_BLUE, "blue never lit");
    dark_expect(3U, "the row left the LED lit behind it");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4U, s_drives,
                                   "the row drove the LED more times than it said");

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- 3. The two ways a colour fails ------------------------------------------ */

/* Green is the colour both are shown on, so that "the row stops there" has a
 * colour before it that passed and one after it that must never light. */
TEST_CASE("a colour the operator refuses fails the row and ends it", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    row_to_colour((uint8_t)LED_COLOUR_GREEN);
    answer_no();
    record_expect((uint8_t)LED_COLOUR_GREEN, (uint8_t)LED_TEST_FAIL);
    verdict_expect((uint8_t)LED_COLOUR_GREEN, false);

    dark_expect(2U, "the row left the LED green behind it");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_drives,
        "the row carried on to blue past a green it had already failed");

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* An operator cannot ship a unit nobody looked at, so an empty bench is a
 * failed verdict - and the row ends there rather than waiting out two more
 * colours for somebody who is not coming. */
TEST_CASE("a colour nobody answers is a timeout and ends the row", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    row_to_colour((uint8_t)LED_COLOUR_GREEN);
    (void)qf_support_tick(AO_LedMgr, STEP_TICKS - 2U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "the row gave up before its deadline");
    (void)qf_support_tick(AO_LedMgr, 2U);
    record_expect((uint8_t)LED_COLOUR_GREEN, (uint8_t)LED_TEST_TIMEOUT);
    verdict_expect((uint8_t)LED_COLOUR_GREEN, false);

    dark_expect(2U, "a row that gave up left the LED however it found it");
    TEST_ASSERT_EQUAL_UINT(3U, s_drives);

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Every colour has the whole of its deadline. The one that timed the colour
 * before it was stopped when that colour was answered, and must not be what
 * ends this one early. */
TEST_CASE("an answer gives the next colour a deadline of its own", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    row_to_colour((uint8_t)LED_COLOUR_RED);
    (void)qf_support_tick(AO_LedMgr, STEP_TICKS - 2U);
    answer_yes();
    record_expect((uint8_t)LED_COLOUR_RED, (uint8_t)LED_TEST_PASS);
    record_expect((uint8_t)LED_COLOUR_GREEN, (uint8_t)LED_TEST_ASK);

    (void)qf_support_tick(AO_LedMgr, STEP_TICKS - 2U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(),
        "green was timed by what was left of red's deadline");
    row_pass_from((uint8_t)LED_COLOUR_GREEN);

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- 4. What is not an answer ------------------------------------------------- */

/* The pad has six buttons and two of them mean something here. A release is
 * the tail of whatever was pressed - the answer before it, or a finger that
 * was down when the row started - and the four directions are an operator
 * finding the right button. */
TEST_CASE("a release and the pad's other buttons are not answers", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    row_to_colour((uint8_t)LED_COLOUR_RED);
    button(KDRV_BTN_TOP, false);
    button(KDRV_BTN_BOT, false);
    button(KDRV_BTN_UP, true);
    button(KDRV_BTN_DOWN, true);
    button(KDRV_BTN_LEFT, true);
    button(KDRV_BTN_RIGHT, true);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "something that is not an answer was taken for one");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_drives, "the row moved on without an answer");

    row_pass_from((uint8_t)LED_COLOUR_RED);

    /* And outside the row the pad is nobody's business here: a press must
     * not start, answer or light anything. */
    drives_forget();
    answer_yes();
    answer_no();
    TEST_ASSERT_NULL(ao_spy_take());
    TEST_ASSERT_EQUAL_UINT(0U, s_drives);

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The LED's owner has no business with the pad outside the row, so it hears
 * it for the row and not a press longer: subscribed on the way in, given back
 * on every way out. */
TEST_CASE("the pad is subscribed for the row and given back after it", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    TEST_ASSERT_FALSE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[BUTTON_EVT_SIG].set,
                         (uint_fast8_t)AO_LedMgr->prio),
        "the LED's owner listens to the pad with no row running");

    row_to_colour((uint8_t)LED_COLOUR_RED);
    TEST_ASSERT_TRUE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[BUTTON_EVT_SIG].set,
                         (uint_fast8_t)AO_LedMgr->prio),
        "the row is waiting for an answer it never asked to hear");

    answer_no();
    record_expect((uint8_t)LED_COLOUR_RED, (uint8_t)LED_TEST_FAIL);
    verdict_expect((uint8_t)LED_COLOUR_RED, false);
    TEST_ASSERT_FALSE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[BUTTON_EVT_SIG].set,
                         (uint_fast8_t)AO_LedMgr->prio),
        "the row kept the pad after it ended");

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- 5. The row owns the LED -------------------------------------------------- */

/* An app publishing a colour mid-row is not doing anything wrong: it cannot
 * know a production row is running. Applying it would put a colour on the
 * chip while the operator is being asked about another - so it is refused,
 * and the refusal is on the wire as a debug line rather than silent. */
TEST_CASE("a colour asked for during the row is refused", "[led_test]")
{
    LedRgbEvt e = { .super = QEVT_INITIALIZER(LED_RGB_SIG),
                    .r = 0U, .g = 255U, .b = 0U };

    Mockkhal_i2c_Init();
    led_test_up_once();
    kode_log_set_level("kaos.ledmgr", KODE_LOG_DEBUG);

    row_to_colour((uint8_t)LED_COLOUR_RED);
    drives_forget();
    fake_klog_reset();
    QASM_DISPATCH(AO_LedMgr, &e.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_drives,
        "a colour landed on the chip over the one being asked about");
    TEST_ASSERT_NOT_NULL_MESSAGE(fake_klog_last(),
                                 "the colour was dropped without a word");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(fake_klog_last(), "owns the LED"),
        "the refusal does not say who is holding the LED");
    TEST_ASSERT_NULL_MESSAGE(strstr(fake_klog_last(), "recorded"),
        "a colour that really is dropped was called recorded");

    /* The two that are policy say the other thing, because they are not
     * ignored at all: what they ask for is what the exit obeys, and a line
     * that said "ignored" would send somebody looking for a bug in the
     * heartbeat that came back on. */
    fake_klog_reset();
    heartbeat(true);
    TEST_ASSERT_NOT_NULL_MESSAGE(fake_klog_last(),
                                 "the heartbeat request was dropped silently");
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(fake_klog_last(), "recorded"),
        "a request kept for the exit was reported as ignored");
    heartbeat(false);

    /* And the row is unharmed: red is still what the answer is about. */
    row_pass_from((uint8_t)LED_COLOUR_RED);

    /* The heartbeat is the exception, and the reason it is worth a case of
     * its own: "blink from now on" is an answer about the future. Refusing it
     * for the length of the row is right; forgetting it is not, and an
     * operator who turned the light off would watch it come back on a couple
     * of seconds later. Off during the row means off at the end of it. */
    heartbeat(true);
    row_to_colour((uint8_t)LED_COLOUR_RED);
    heartbeat(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_drives,
        "a heartbeat disable reached the chip over the colour being asked about");
    row_pass_from((uint8_t)LED_COLOUR_RED);
    drives_forget();
    (void)qf_support_tick(AO_LedMgr, BLINK_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_drives,
        "the row put back a heartbeat the operator had asked it to stop");

    /* And the other direction: asked for during the row, running after it. */
    row_to_colour((uint8_t)LED_COLOUR_RED);
    heartbeat(true);
    row_pass_from((uint8_t)LED_COLOUR_RED);
    drives_forget();
    (void)qf_support_tick(AO_LedMgr, BLINK_TICKS);
    TEST_ASSERT_TRUE_MESSAGE(0U < s_drives,
        "a heartbeat asked for during the row never started");

    kode_log_set_level("kaos.ledmgr", KODE_LOG_INFO);
    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A second `test component=led` while the first is running is the same row
 * asked for twice. Restarting would leave the first request's verdict
 * unpublished, which is a console waiting out its budget for nothing. */
TEST_CASE("a request during the row does not restart it", "[led_test]")
{
    Mockkhal_i2c_Init();
    led_test_up_once();

    row_to_colour((uint8_t)LED_COLOUR_GREEN);
    row_request();
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "the row asked about red again");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_drives, "the row went back to red");
    row_pass_from((uint8_t)LED_COLOUR_GREEN);

    led_test_down();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}
