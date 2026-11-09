/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_buttons_ao.c
 * @brief AO_Buttons: the debounce, fed by edges instead of a clock.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkhal_gpio.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_buttons.h"
#include "kode_sys.h"
#include "kqp.h"
#include "qf_support.h"

#define FOREIGN_ORIGIN 7U   /**< The number of a client other than the autotest. */

#define BIT(b) ((uint8_t)(1U << (unsigned)(b)))

/* Expander readings, as the owner would publish them: low = pressed. */
#define ALL_UP        0xFFU
#define PAD_UP_DOWN   0xFDU   /* P1 low                 */
#define LEFT_DOWN     0xFBU   /* P2 low                 */
#define PAD_DOWN_DOWN 0xF7U   /* P3 low                 */
#define RIGHT_DOWN    0xEFU   /* P4 low                 */
#define BOT_DOWN      0xFEU   /* P0 low                 */
#define BOT_AND_UP    0xFCU   /* P0 and P1 low          */

static const QEvt SETTLE   = QEVT_INITIALIZER(BUTTONS_SETTLE_SIG);
static const QEvt TOP_INT  = QEVT_INITIALIZER(BUTTONS_TOP_INT_SIG);
static const QEvt HOLD_TMO = QEVT_INITIALIZER(BUTTONS_HOLD_TMO_SIG);

/** @brief What the driver was asked to call from GPIO35's interrupt. */
static khal_gpio_cb_t s_top_cb;
static void          *s_top_arg;

static kode_status_t capture_attach(uint8_t pin, khal_gpio_irq_t kind,
                                    khal_gpio_cb_t cb, void *arg, int ncalls)
{
    (void)ncalls;
    TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_BUTTON_TOP, pin);
    TEST_ASSERT_EQUAL_INT(KHAL_GPIO_IRQ_ANY_EDGE, kind);
    s_top_cb  = cb;
    s_top_arg = arg;
    return KODE_OK;
}

/**
 * @brief Bring AO_Buttons up, once for the whole binary.
 *
 * Once, because QActive_register_() asserts on a taken priority. The AO has
 * no periodic timer any more, so nothing keeps posting to it between cases;
 * what does carry across is `stable` and `candidate`, which is the thing
 * under test - debounce IS state between samples - so every case settles the
 * AO into "nothing pressed" first, with settle().
 */
static void buttons_up_once(void)
{
    static bool s_started;

    Mockkhal_gpio_Init();
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(BUTTON_EVT_SIG);
    ao_spy_subscribe(BUTTON_HOLD_SIG);
    ao_spy_subscribe(BUTTON_DOUBLE_SIG);
    ao_spy_subscribe(BTN_STATE_SIG);
    ao_spy_subscribe(BTN_TEST_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);

    if ( !s_started )
    {
        khal_gpio_input_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KHAL_GPIO_PULL_UP, KODE_OK);
        khal_gpio_irq_attach_StubWithCallback(&capture_attach);
        Buttons_ctor();
        qf_support_give_queue(AO_Buttons);
        AO_Buttons->prio  = KODE_PRIO_BUTTONS;
        AO_Buttons->pthre = KODE_PRIO_BUTTONS;
        QActive_register_(AO_Buttons);
        QASM_INIT(AO_Buttons, (void *)0, 0U);
        s_started = true;
    }
    fake_klog_reset();
    ao_spy_drain();
}

/** @brief Feed one expander reading with GPIO35 at @p top_high. */
static void feed(uint8_t levels, bool top_high)
{
    IoexpInputEvt e;
    e.super   = (QEvt)QEVT_INITIALIZER(IOEXP_INPUT_SIG);
    e.levels  = levels;
    e.changed = 0U;      /* the debounce does not care what the owner thinks moved */
    e.ok      = true;
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, top_high);
    QASM_DISPATCH(AO_Buttons, &e.super, 0U);
}

/** @brief The settle: re-arms GPIO35 and asks the expander for a second look. */
static void settle_fires(void)
{
    unsigned const asked = fake_neighbour_ioexp_posts();
    khal_gpio_irq_enable_Expect(KBSP_PIN_BUTTON_TOP);
    QASM_DISPATCH(AO_Buttons, &SETTLE, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(asked + 1U, fake_neighbour_ioexp_posts(),
                                   "the settle asks the expander for a second look");
    /* AO_IoExp's mailbox is sixteen deep and nobody dispatches it here; a
     * guided run settles more than sixteen times, so the ask is taken out
     * once it has been counted. */
    fake_neighbours_reset();
}

/**
 * @brief Drive the AO into "nothing pressed" whatever it believed before.
 *
 * The double-press detector keeps its last release between cases like the
 * debounce keeps its samples, but every case starts with the clock back at
 * zero, so a release an earlier case left behind can look recent. Any press
 * of the top button spends what a release armed, so one is made here, and
 * its own release is left behind a window's worth of time: whatever the case
 * presses next is never paired with a release from before it.
 */
static void settle(void)
{
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);

    feed(ALL_UP, false);
    settle_fires();
    feed(ALL_UP, false);
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);

    ao_spy_drain();
    fake_time_advance_ms(KODE_BUTTON_DOUBLE_MS + 1U);
}

/* ---- Ownership ----------------------------------------------------------- */

TEST_CASE("AO_Buttons owns GPIO35 and arms its edge", "[buttons_ao]")
{
    buttons_up_once();
    TEST_ASSERT_EQUAL_STRING("AO_Buttons", kbsp_owner_name(KBSP_RES_BTN_TOP));
    TEST_ASSERT_NOT_NULL(s_top_cb);
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The debounce -------------------------------------------------------- */

/* The case that earns this file. One reading says pressed, the confirming
 * one says released: a bounce, and nobody hears of it. */
TEST_CASE("a one-sample glitch reaches nobody", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    feed(PAD_UP_DOWN, true);
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a press is published once, after the confirming sample, and so is the release",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    feed(PAD_UP_DOWN, true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one sample is not a press");
    settle_fires();
    feed(PAD_UP_DOWN, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_INT(BUTTON_EVT_SIG, b->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_UP, b->id);
    TEST_ASSERT_TRUE(b->pressed);
    ao_spy_drain();

    /* Held: no more news. */
    feed(PAD_UP_DOWN, true);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_UP, b->id);
    TEST_ASSERT_FALSE(b->pressed);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A double press and a drag are judged from when they happened, and AO_Gui
 * dispatches late under a render pass, so the publisher stamps the event.
 * The time is the confirming sample's, not the first look at the pin: a
 * press is a press from the moment the settle agrees, and that is the
 * instant a subscriber should be told about. */
TEST_CASE("a confirmed press carries the time it was confirmed", "[buttons_ao]")
{
    uint32_t const FIRST_LOOK_MS  = 1000U;
    uint32_t const RELEASE_GAP_MS = 500U;

    buttons_up_once();
    settle();
    fake_time_reset();

    fake_time_advance_ms(FIRST_LOOK_MS);
    feed(LEFT_DOWN, true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "one sample is not a press");
    fake_time_advance_ms(KODE_SYS_BUTTON_SETTLE_MS);
    settle_fires();
    feed(LEFT_DOWN, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_TRUE(b->pressed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(FIRST_LOOK_MS + KODE_SYS_BUTTON_SETTLE_MS, b->t_ms,
        "the press is stamped by the sample that confirmed it");
    ao_spy_drain();

    fake_time_advance_ms(RELEASE_GAP_MS);
    feed(ALL_UP, true);
    fake_time_advance_ms(KODE_SYS_BUTTON_SETTLE_MS);
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_FALSE(b->pressed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(
        FIRST_LOOK_MS + KODE_SYS_BUTTON_SETTLE_MS + RELEASE_GAP_MS + KODE_SYS_BUTTON_SETTLE_MS,
        b->t_ms, "the release carries its own time, not the press's");
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("an injected press carries the time of the call", "[buttons_ao]")
{
    uint32_t const CALL_MS = 777U;

    buttons_up_once();
    settle();
    fake_time_reset();

    fake_time_advance_ms(CALL_MS);
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT32(CALL_MS, b->t_ms);
    ao_spy_drain();

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("two buttons changing in one sample are two events", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    feed(BOT_AND_UP, true);
    settle_fires();
    feed(BOT_AND_UP, true);
    TEST_ASSERT_EQUAL_UINT(2U, ao_spy_count());
    uint8_t seen = 0U;
    for ( unsigned i = 0U; 2U > i; ++i )
    {
        const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
        TEST_ASSERT_TRUE(b->pressed);
        seen |= BIT(b->id);
    }
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_BOT) | BIT(KDRV_BTN_UP), seen);
    ao_spy_drain();
    settle();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- What a polling app reads -------------------------------------------- */

/* kode_sys_button_read_mask() is the state the event stream describes, not the raw
 * pins: one sample is not a press there either, and the release clears it
 * when the release is published and not before. */
TEST_CASE("kode_sys_button_read_mask is the debounced state, press and release",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();
    TEST_ASSERT_EQUAL_HEX32(0U, kode_sys_button_read_mask());

    feed(BOT_AND_UP, true);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0U, kode_sys_button_read_mask(),
                                    "one sample is not a press here either");
    settle_fires();
    feed(BOT_AND_UP, true);
    TEST_ASSERT_EQUAL_HEX32(BIT(KDRV_BTN_BOT) | BIT(KDRV_BTN_UP), kode_sys_button_read_mask());
    ao_spy_drain();

    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(BIT(KDRV_BTN_BOT) | BIT(KDRV_BTN_UP),
                                    kode_sys_button_read_mask(),
                                    "released on the confirming sample, not the first");
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_HEX32(0U, kode_sys_button_read_mask());
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* An injected press is published like a real one and reads as down until the
 * next word about that button - a real edge on another button in between
 * leaves it alone. */
TEST_CASE("an injected press reads as down until its own release",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_INT(BUTTON_EVT_SIG, b->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, b->id);
    TEST_ASSERT_TRUE(b->pressed);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_HEX32(BIT(KDRV_BTN_TOP), kode_sys_button_read_mask());

    /* A real press and release of another button, meanwhile. */
    feed(LEFT_DOWN, true);
    settle_fires();
    feed(LEFT_DOWN, true);
    TEST_ASSERT_EQUAL_HEX32(BIT(KDRV_BTN_TOP) | BIT(KDRV_BTN_LEFT), kode_sys_button_read_mask());
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_HEX32(BIT(KDRV_BTN_TOP), kode_sys_button_read_mask());
    ao_spy_drain();

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    TEST_ASSERT_EQUAL_HEX32(0U, kode_sys_button_read_mask());
    TEST_ASSERT_FALSE_MESSAGE(buttons_inject((uint8_t)KDRV_BTN_COUNT, true),
                              "an id past the six is refused");
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The same button from both sources: whatever the stream last said about it
 * is what a poll answers, so a subscriber and a poller never disagree. A
 * real press, then an injected release of it, reads as released - exactly
 * what every subscriber was just told. */
TEST_CASE("a poll answers what the last event about a button said, whoever sent it",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    feed(BOT_DOWN, true);
    settle_fires();
    feed(BOT_DOWN, true);
    TEST_ASSERT_EQUAL_HEX32(BIT(KDRV_BTN_BOT), kode_sys_button_read_mask());
    ao_spy_drain();

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_BOT, false));
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_FALSE(b->pressed);
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0U, kode_sys_button_read_mask(),
        "the stream said released, so the poll says released");
    ao_spy_drain();

    /* The finger lifts: the AO confirms a release of its own, and both views
     * still agree. */
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_HEX32(0U, kode_sys_button_read_mask());
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The pin of its own -------------------------------------------------- */

/* GPIO35's interrupt is a sample too: the expander byte is the last one
 * published, the pin is read now. The interrupt stays off until the settle
 * re-arms it, so a bouncing contact posts once per arming. */
TEST_CASE("the top button's edge is a sample, confirmed by the settle",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false);
    s_top_cb(s_top_arg);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    settle_fires();
    feed(ALL_UP, false);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, b->id);
    TEST_ASSERT_TRUE(b->pressed);
    ao_spy_drain();

    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, true);
    QASM_DISPATCH(AO_Buttons, &TOP_INT, 0U);
    settle_fires();
    feed(ALL_UP, true);
    b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, b->id);
    TEST_ASSERT_FALSE(b->pressed);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* One INFO line per confirmed edge, naming the button, its new state and the
 * sample that confirmed it: the trace a launch nobody made is found by. */
TEST_CASE("every confirmed edge is one line naming the button, the state and the sample",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    fake_klog_reset();
    feed(LEFT_DOWN, true);
    settle_fires();
    feed(LEFT_DOWN, true);                 /* confirmed by the expander's byte */
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    ao_spy_drain();
    const char *line = fake_klog_last();
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(line, "pad-left pressed, confirmed by the expander's byte"), line);

    /* The top button: its own edge is one INFO line with the level and the
     * time, and the press is confirmed by the settle's sample and named so. */
    settle();
    fake_klog_reset();
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false);
    s_top_cb(s_top_arg);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    line = fake_klog_last();
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, "GPIO35 edge: level=0 at "), line);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    settle_fires();
    feed(ALL_UP, false);                   /* the settle's sample */
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    ao_spy_drain();
    line = fake_klog_last();
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NOT_NULL_MESSAGE(
        strstr(line, "top pressed, confirmed by the expander's byte"), line);

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The launch nobody made. Every sample reads GPIO35, and the expander
 * publishes for reasons of its own - a tap on the glass, the gauge, a pad
 * button - so two samples microseconds apart used to confirm a transient on
 * the pin as a press. Now only the settle's own sample may move the top
 * button; a transient that an unrelated reading happens to catch is a
 * candidate and nothing more, and a press held until the settle asks is
 * still a press. */
TEST_CASE("a GPIO35 transient caught by an unrelated expander reading is not a press",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    /* The edge, with the pin still low, then an expander publish nobody here
     * asked for - the pin still low when it is sampled. Two agreeing samples,
     * and no press. */
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false);
    s_top_cb(s_top_arg);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    feed(ALL_UP, false);                   /* unrelated: the touch IRQ, say */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "an unrelated sample confirmed the top button");
    feed(PAD_UP_DOWN, false);              /* another, a pad edge */
    feed(ALL_UP, false);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* The transient is over by the time the settle asks: nothing, ever. */
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a top press held until the settle asks is a press, whatever else was read meanwhile",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false);
    s_top_cb(s_top_arg);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    feed(ALL_UP, false);                   /* unrelated readings in the window */
    feed(ALL_UP, false);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    settle_fires();
    feed(ALL_UP, false);                   /* the settle's sample: still held */
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, b->id);
    TEST_ASSERT_TRUE(b->pressed);
    ao_spy_drain();

    /* And the release the same way: the pin's edge is a candidate, an
     * unrelated reading does not release it, the settle's sample does. */
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, true);
    QASM_DISPATCH(AO_Buttons, &TOP_INT, 0U);
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, b->id);
    TEST_ASSERT_FALSE(b->pressed);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* GPIO35's interrupt is off from its edge until the settle re-arms it. An edge
 * read mid-bounce at the level the button already had changes nothing for the
 * debounce, and it used to arm no settle: the interrupt stayed off and the top
 * button was dead until a reset. */
TEST_CASE("an edge that reads as the level the top button already had still re-arms the pin",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    /* A release bounce: the edge fires, the pin already reads released again. */
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, true);
    s_top_cb(s_top_arg);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* Nothing differs, yet the settle is armed: it fires on its own and
     * turns the interrupt back on. */
    unsigned const asked = fake_neighbour_ioexp_posts();
    khal_gpio_irq_enable_Expect(KBSP_PIN_BUTTON_TOP);
    unsigned fired = 0U;
    for ( unsigned i = 0U; (QF_SUPPORT_STEPS(100U) > i) && (0U == fired); ++i )
    {
        fired = qf_support_pump(AO_Buttons, 4U);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fired, "the settle never fired");
    TEST_ASSERT_EQUAL_UINT(asked + 1U, fake_neighbour_ioexp_posts());
    fake_neighbours_reset();

    /* Its sample agrees with the world: no event, and the next press is heard. */
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false);
    QASM_DISPATCH(AO_Buttons, &TOP_INT, 0U);
    settle_fires();
    feed(ALL_UP, false);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, b->id);
    TEST_ASSERT_TRUE(b->pressed);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The clock ----------------------------------------------------------- */

/* The one place time enters: a change arms a 30 ms one-shot, and when it
 * fires the AO re-arms GPIO35 and asks the expander. Nothing is armed while
 * nothing is changing. */
TEST_CASE("the settle fires 30 ms after a change, and not otherwise", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Buttons, 10U),
                                   "at rest, no timer runs");

    unsigned const asked = fake_neighbour_ioexp_posts();
    feed(PAD_UP_DOWN, true);
    khal_gpio_irq_enable_Expect(KBSP_PIN_BUTTON_TOP);
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Buttons, 2U));
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons, 1U));
    TEST_ASSERT_EQUAL_UINT(asked + 1U, fake_neighbour_ioexp_posts());

    feed(ALL_UP, true);              /* a glitch after all */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Buttons, 10U),
                                   "settled: the timer is not re-armed");

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The expander going away --------------------------------------------- */

/* A failed read is NOT a sample. Feeding it as "all released" would publish
 * a release for every held button the moment the expander hiccups - and the
 * confirming sample that never comes would make it permanent. */
TEST_CASE("a failed expander read is not a sample", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    feed(PAD_UP_DOWN, true);
    settle_fires();
    feed(PAD_UP_DOWN, true);
    ao_spy_drain();                   /* the press is out */

    IoexpInputEvt dead;
    dead.super   = (QEvt)QEVT_INITIALIZER(IOEXP_INPUT_SIG);
    dead.levels  = 0U;
    dead.changed = 0U;
    dead.ok      = false;
    QASM_DISPATCH(AO_Buttons, &dead.super, 0U);     /* no GPIO read either */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Buttons, 10U),
                                   "nothing to confirm, nothing armed");

    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- btn_read ------------------------------------------------------------ */

/* The console's snapshot is a fresh reading, not the debounced belief: it
 * asks the expander, and answers when the reading comes back. */
TEST_CASE("btn_read asks for a reading and answers with it, raw", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    unsigned const asked = fake_neighbour_ioexp_posts();
    buttons_dump_request();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    TEST_ASSERT_EQUAL_UINT(asked + 1U, fake_neighbour_ioexp_posts());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing to say yet");

    feed(RIGHT_DOWN, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonsStateEvt *s = (const ButtonsStateEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_INT(BTN_STATE_SIG, s->super.sig);
    TEST_ASSERT_TRUE(s->ok);
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_RIGHT), s->pressed);
    ao_spy_drain();

    /* Answered once: the next reading is not a second answer. */
    khal_gpio_irq_enable_Expect(KBSP_PIN_BUTTON_TOP);
    QASM_DISPATCH(AO_Buttons, &SETTLE, 0U);
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("btn_read on an expander that did not answer says so", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    buttons_dump_request();
    (void)qf_support_pump(AO_Buttons, 4U);

    IoexpInputEvt dead;
    dead.super   = (QEvt)QEVT_INITIALIZER(IOEXP_INPUT_SIG);
    dead.levels  = 0U;
    dead.changed = 0U;
    dead.ok      = false;
    QASM_DISPATCH(AO_Buttons, &dead.super, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonsStateEvt *s = (const ButtonsStateEvt *)ao_spy_take();
    TEST_ASSERT_FALSE(s->ok);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The guided test ------------------------------------------------------- */

/* The production line's button test: the AO asks for one button at a time
 * and judges that exactly that one was pressed. Six steps, one per button,
 * the pad and the two individual buttons alternating while they last, so a
 * bridge between two pad tracks or a pin mapped to the wrong button is
 * caught; the next prompt waits for every button to be released, because
 * two of them held together are a hardware reset of the P4. The sequence is
 * the AO's; a case repeats it here so a change to it is a change somebody
 * reads. */
#define SEQ_LEN 6U
static const kdrv_button_t SEQ[SEQ_LEN] = {
    KDRV_BTN_LEFT, KDRV_BTN_BOT, KDRV_BTN_DOWN, KDRV_BTN_TOP,
    KDRV_BTN_RIGHT, KDRV_BTN_UP,
};

/* The rules' numbers, repeated here for the same reason: a step has 20 s,
 * a press 3 s to be let go, a wrong press is forgiven twice, a press that
 * takes two buttons down once. In 10 ms ticks. */
#define STEP_TICKS     2000U
#define RELEASE_TICKS  300U

/** @brief The expander byte and GPIO35 level that press @p b alone. */
static void levels_for(kdrv_button_t b, uint8_t *exp, bool *top_high)
{
    *exp = ALL_UP;
    *top_high = true;
    switch ( b )
    {
        case KDRV_BTN_UP:    *exp = PAD_UP_DOWN;   break;
        case KDRV_BTN_DOWN:  *exp = PAD_DOWN_DOWN; break;
        case KDRV_BTN_LEFT:  *exp = LEFT_DOWN;     break;
        case KDRV_BTN_RIGHT: *exp = RIGHT_DOWN;    break;
        case KDRV_BTN_BOT:   *exp = BOT_DOWN;      break;
        default:             *top_high = false;    break;   /* TOP */
    }
}

/** @brief Press @p exp/@p top_high through the debounce (two samples). */
static void press_confirmed(uint8_t exp, bool top_high)
{
    feed(exp, top_high);
    settle_fires();
    feed(exp, top_high);
}

/** @brief Release everything through the debounce. */
static void release_confirmed(void)
{
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
}

/** @brief The next BTN_TEST_SIG in the spy, skipping the BUTTON_EVT_SIGs. */
static const ButtonsTestEvt *next_test_evt(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            return NULL;
        }
        if ( BTN_TEST_SIG == e->sig )
        {
            return (const ButtonsTestEvt *)e;
        }
    }
}

/** @brief The next TEST_RESULT_SIG in the spy, skipping everything else. */
static const TestResultEvt *next_verdict(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            return NULL;
        }
        if ( TEST_RESULT_SIG == e->sig )
        {
            return (const TestResultEvt *)e;
        }
    }
}

/** @brief Ask for the test and check the first prompt. */
static void test_starts(void)
{
    buttons_test_request();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "a request is answered with the first prompt");
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SEQ_LEN, t->total);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_LEFT, t->button);
    ao_spy_drain();
}

/** @brief Press @p b alone and let it go, and leave the spy empty. */
static void press_and_release(kdrv_button_t b)
{
    uint8_t exp;
    bool    top_high;

    levels_for(b, &exp, &top_high);
    press_confirmed(exp, top_high);
    release_confirmed();
}

/** @brief Run out the steps from @p first on (1-based) by timeouts and check the verdict. */
static void rest_times_out(unsigned first, bool expect_ok)
{
    for ( unsigned i = first; SEQ_LEN >= i; ++i )
    {
        (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    }
    const TestResultEvt *v = next_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "the run ends in a verdict");
    TEST_ASSERT_EQUAL_STRING("buttons", v->name);
    TEST_ASSERT_EQUAL(expect_ok, v->ok);
    ao_spy_drain();
}

TEST_CASE("test_six_steps", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    for ( unsigned i = 0U; SEQ_LEN > i; ++i )
    {
        uint8_t exp;
        bool    top_high;
        levels_for(SEQ[i], &exp, &top_high);

        press_confirmed(exp, top_high);
        TEST_ASSERT_NULL_MESSAGE(next_test_evt(), "a clean press is passed on its release, not before");
        ao_spy_drain();

        release_confirmed();
        const ButtonsTestEvt *t = next_test_evt();
        TEST_ASSERT_NOT_NULL_MESSAGE(t, "the release passes the step");
        TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PASS, t->phase);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1U), t->step);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)SEQ[i], t->button);
        t = next_test_evt();
        if ( (SEQ_LEN - 1U) > i )
        {
            TEST_ASSERT_NOT_NULL_MESSAGE(t, "the release brings the next prompt");
            TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 2U), t->step);
            TEST_ASSERT_EQUAL_UINT8((uint8_t)SEQ[i + 1U], t->button);
            ao_spy_drain();
        }
        else
        {
            TEST_ASSERT_NOT_NULL(t);
            TEST_ASSERT_EQUAL_UINT8(BTN_TEST_DONE, t->phase);
        }
    }

    const TestResultEvt *v = next_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "the run ends in a verdict");
    TEST_ASSERT_EQUAL_STRING("buttons", v->name);
    TEST_ASSERT_TRUE(v->ok);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Buttons, 2U * STEP_TICKS),
                                   "nothing is armed once the run is over");

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* One other button is forgiven, twice: it is reported as WRONG, the same
 * button is asked for again, and the run still ends in a pass. */
TEST_CASE("test_wrong_twice_then_right", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    for ( unsigned k = 0U; 2U > k; ++k )
    {
        press_confirmed(PAD_DOWN_DOWN, true);   /* step 1 wants pad-left */
        const ButtonsTestEvt *t = next_test_evt();
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_EQUAL_UINT8(BTN_TEST_WRONG, t->phase);
        TEST_ASSERT_EQUAL_UINT8(1U, t->step);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_LEFT, t->button);
        TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_DOWN), t->pressed);
        ao_spy_drain();

        release_confirmed();
        t = next_test_evt();
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
        TEST_ASSERT_EQUAL_UINT8(1U, t->step);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_LEFT, t->button);
        ao_spy_drain();
    }

    press_and_release(SEQ[0]);
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PASS, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(2U, t->step);
    ao_spy_drain();

    for ( unsigned i = 1U; SEQ_LEN > i; ++i )
    {
        press_and_release(SEQ[i]);
        ao_spy_drain();
    }
    /* The verdict went with the drain; ask the AO's own state instead. */
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Buttons, 2U * STEP_TICKS));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The third wrong press on a step ends the run, with no further prompt, and
 * the unit fails. */
TEST_CASE("test_wrong_three_times_gives_up_and_fails", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    /* Step 1 wants pad-left; pad-up and pad-down come instead, three times. */
    for ( unsigned k = 0U; 3U > k; ++k )
    {
        press_confirmed(PAD_UP_DOWN & PAD_DOWN_DOWN, true);
        const ButtonsTestEvt *t = next_test_evt();
        TEST_ASSERT_NOT_NULL(t);
        uint8_t want = BTN_TEST_WRONG;
        if ( 2U == k )
        {
            want = BTN_TEST_GIVEUP;
        }
        TEST_ASSERT_EQUAL_UINT8(want, t->phase);
        TEST_ASSERT_EQUAL_UINT8(1U, t->step);
        TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_UP) | BIT(KDRV_BTN_DOWN), t->pressed);
        ao_spy_drain();
        release_confirmed();
        t = next_test_evt();
        TEST_ASSERT_NOT_NULL(t);
        if ( 2U > k )
        {
            TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
            TEST_ASSERT_EQUAL_UINT8(1U, t->step);
            ao_spy_drain();
        }
        else
        {
            TEST_ASSERT_EQUAL_UINT8_MESSAGE(BTN_TEST_DONE, t->phase, "a give-up ends the run, no prompt");
            TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "only the verdict is left");
        }
    }
    const TestResultEvt *v = next_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "the run ends at the third");
    TEST_ASSERT_FALSE_MESSAGE(v->ok, "giving up is a fail of the test");
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Buttons, 2U * STEP_TICKS));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The asked-for button and another down in one press: asked again once. */
TEST_CASE("test_double_once_then_right", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    press_confirmed(LEFT_DOWN & PAD_DOWN_DOWN, true);        /* pad-left and pad-down together */
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_DOUBLE, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_LEFT) | BIT(KDRV_BTN_DOWN), t->pressed);
    ao_spy_drain();
    release_confirmed();
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    ao_spy_drain();

    press_and_release(SEQ[0]);
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PASS, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    ao_spy_drain();

    for ( unsigned i = 1U; SEQ_LEN > i; ++i )
    {
        press_and_release(SEQ[i]);
        ao_spy_drain();
    }
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Buttons, 2U * STEP_TICKS));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* Twice on one step: a fail naming both, and the step ends. */
TEST_CASE("test_double_twice_fails", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    for ( unsigned k = 0U; 2U > k; ++k )
    {
        press_confirmed(LEFT_DOWN & PAD_DOWN_DOWN, true);
        const ButtonsTestEvt *t = next_test_evt();
        TEST_ASSERT_NOT_NULL(t);
        uint8_t want = BTN_TEST_DOUBLE;
        uint8_t next = 1U;   /* the same step again */
        if ( 1U == k )
        {
            want = BTN_TEST_DOUBLEFAIL;
            next = 2U;       /* the step is over */
        }
        TEST_ASSERT_EQUAL_UINT8(want, t->phase);
        TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_LEFT) | BIT(KDRV_BTN_DOWN), t->pressed);
        ao_spy_drain();
        release_confirmed();
        t = next_test_evt();
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
        TEST_ASSERT_EQUAL_UINT8(next, t->step);
        ao_spy_drain();
    }
    rest_times_out(2U, false);

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The right button first, another going down before the release: the same
 * rule, caught mid-press. */
TEST_CASE("another button going down during a clean press is a double", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    press_confirmed(LEFT_DOWN, true);
    TEST_ASSERT_NULL(next_test_evt());
    ao_spy_drain();
    press_confirmed(LEFT_DOWN & PAD_DOWN_DOWN, true);
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_DOUBLE, t->phase);
    ao_spy_drain();
    release_confirmed();
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    ao_spy_drain();

    for ( unsigned i = 0U; SEQ_LEN > i; ++i )
    {
        press_and_release(SEQ[i]);
        ao_spy_drain();
    }
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Buttons, 2U * STEP_TICKS));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The press is timed from the debounce's confirmation to the release's. */
TEST_CASE("test_pass_reports_ms", "[buttons_ao]")
{
    uint8_t exp;
    bool    top_high;

    buttons_up_once();
    settle();
    test_starts();

    levels_for(SEQ[0], &exp, &top_high);
    press_confirmed(exp, top_high);
    fake_time_advance_ms(250U);
    release_confirmed();
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PASS, t->phase);
    TEST_ASSERT_EQUAL_UINT16(250U, t->ms);
    ao_spy_drain();

    for ( unsigned i = 1U; SEQ_LEN > i; ++i )
    {
        press_and_release(SEQ[i]);
        ao_spy_drain();
    }
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_Buttons, 2U * STEP_TICKS));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The console's budget for the row has to outlast the worst run the rules
 * allow, worked out here from the rules' own numbers and not from the
 * macro's expression: six steps; on each, four attempts - two forgiven wrong
 * presses, one forgiven double and the final press - and every attempt, right
 * or wrong, may take a whole 20 s step to be made and 3 s to be let go; and
 * five seconds of slack. */
TEST_CASE("test_budget_covers_worst_case", "[buttons_ao]")
{
    uint32_t const step_ms    = 20000U;
    uint32_t const release_ms = 3000U;
    uint32_t const attempts   = 2U + 1U + 1U;
    uint32_t const worst_step = attempts * (step_ms + release_ms);

    TEST_ASSERT_EQUAL_UINT32(6U, KODE_BUTTONS_TEST_STEPS);
    TEST_ASSERT_EQUAL_UINT32(step_ms, KODE_BUTTONS_TEST_STEP_MS);
    TEST_ASSERT_EQUAL_UINT32(release_ms, KODE_BUTTONS_TEST_RELEASE_MS);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((6U * worst_step) + 5000U, KODE_BUTTONS_TEST_BUDGET_MS);
}

TEST_CASE("a step nobody answers times out after 20 s and the run moves on",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Buttons, STEP_TICKS - 1U),
                                   "not yet: 20 s is 2000 ticks");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons, 1U));
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_TIMEOUT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "nothing is held, so the next prompt follows at once");
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(2U, t->step);
    ao_spy_drain();

    /* Let the other five time out too: the verdict is fail, and honest. */
    for ( unsigned i = 1U; SEQ_LEN > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons, STEP_TICKS));
    }
    const TestResultEvt *v = next_verdict();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_EQUAL_STRING("buttons", v->name);
    TEST_ASSERT_FALSE(v->ok);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A held button at the start is not a step the operator got wrong, it is a
 * test that cannot begin: BUTTON_BOT and PAD_LEFT held together reset the
 * board in hardware, and step one asks for PAD_LEFT. */
TEST_CASE("test_held_at_step_start_fails", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    press_confirmed(BOT_DOWN, true);
    ao_spy_drain();

    buttons_test_request();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "the held button is named");
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_HELD, t->phase);
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_BOT), t->pressed);
    TEST_ASSERT_NULL_MESSAGE(next_test_evt(), "and no prompt follows");

    /* The answer is the verdict itself, so `test` closes rather than waits. */
    buttons_test_request();
    (void)qf_support_pump(AO_Buttons, 4U);
    const TestResultEvt *v = next_verdict();
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_FALSE(v->ok);
    ao_spy_drain();

    release_confirmed();
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A client other than the autotest asks for the same script and hears every
 * step tagged with its own number; the plain request is the autotest's. */
TEST_CASE("test_request_from_carries_origin", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    buttons_test_request_from(FOREIGN_ORIGIN);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "a request from a client is answered with the first prompt");
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(FOREIGN_ORIGIN, t->origin);
    ao_spy_drain();

    /* Finish the run by timeouts so the next case starts clean. */
    for ( unsigned i = 0U; SEQ_LEN > i; ++i )
    {
        (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    }
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/** @brief Let every step of a run time out, so the run ends and the spy is emptied. */
static void run_times_out(void)
{
    for ( unsigned i = 0U; SEQ_LEN > i; ++i )
    {
        (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    }
    ao_spy_drain();
}

/* A client whose own run ended (AO_TestDot's abort) cancels the test it
 * asked for: the prompts stop and no verdict follows. Another client's
 * cancel leaves a run alone. */
TEST_CASE("test_cancel_ends_the_clients_run_with_no_verdict", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    buttons_test_request_from(FOREIGN_ORIGIN);
    (void)qf_support_pump(AO_Buttons, 4U);
    ao_spy_drain();

    buttons_test_cancel_request((uint8_t)(FOREIGN_ORIGIN + 1U));
    (void)qf_support_pump(AO_Buttons, 4U);
    (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    TEST_ASSERT_NOT_NULL_MESSAGE(next_test_evt(), "another client's cancel leaves the run going");
    ao_spy_drain();

    buttons_test_cancel_request(FOREIGN_ORIGIN);
    (void)qf_support_pump(AO_Buttons, 4U);
    ao_spy_drain();
    for ( unsigned i = 0U; SEQ_LEN > i; ++i )
    {
        (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    }
    unsigned heard = 0U;
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (BTN_TEST_SIG == e->sig) || (TEST_RESULT_SIG == e->sig) )
        {
            heard++;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard, "no prompt and no verdict after the cancel");

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/** @brief Hold BOT, ask for the test with @p from (a client's origin), and check the refusal's origin. */
static void refusal_carries(bool foreign, uint8_t expected)
{
    press_confirmed(BOT_DOWN, true);
    ao_spy_drain();

    if ( foreign )
    {
        buttons_test_request_from(FOREIGN_ORIGIN);
    }
    else
    {
        buttons_test_request();
    }
    (void)qf_support_pump(AO_Buttons, 4U);
    const TestResultEvt *v = next_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "a held button is answered with a verdict");
    TEST_ASSERT_FALSE(v->ok);
    TEST_ASSERT_EQUAL_UINT8(expected, v->origin);
    ao_spy_drain();

    release_confirmed();
    ao_spy_drain();
}

/* The refusal for a held button is a verdict of the request that was refused,
 * not of whichever run came before it. */
TEST_CASE("test_refusal_carries_request_origin", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    /* A foreign run first, then the autotest's request refused. */
    buttons_test_request_from(FOREIGN_ORIGIN);
    (void)qf_support_pump(AO_Buttons, 4U);
    ao_spy_drain();
    run_times_out();
    refusal_carries(false, TEST_ORIGIN_AUTOTEST);

    /* And the reverse: the autotest's run first, then a foreign request refused. */
    buttons_test_request();
    (void)qf_support_pump(AO_Buttons, 4U);
    ao_spy_drain();
    run_times_out();
    refusal_carries(true, FOREIGN_ORIGIN);

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/** @brief Did klog see a line containing @p needle since the reset? */
static bool logged(const char *needle)
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

/* A client that asks while another run is up is told no, in its own number:
 * silence would leave it waiting for a verdict that never comes, and the
 * running run keeps the origin it started with. */
TEST_CASE("test_buttons_request_during_a_run_gets_a_refusal", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();                           /* the autotest's run is up */

    fake_klog_reset();
    buttons_test_request_from(FOREIGN_ORIGIN);
    (void)qf_support_pump(AO_Buttons, 4U);
    const TestResultEvt *v = next_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "a request during a run is answered with a verdict");
    TEST_ASSERT_EQUAL_STRING("buttons", v->name);
    TEST_ASSERT_FALSE_MESSAGE(v->ok, "the refusal is a failure, never a pass");
    TEST_ASSERT_EQUAL_UINT8(FOREIGN_ORIGIN, v->origin);
    TEST_ASSERT_NULL_MESSAGE(next_test_evt(), "no prompt for the refused request");
    TEST_ASSERT_TRUE_MESSAGE(logged("busy with a run from origin 0"), "the log says why");
    ao_spy_drain();

    /* The run that was up goes on, still the autotest's. */
    (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "the running run's first step times out");
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_TIMEOUT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(TEST_ORIGIN_AUTOTEST, t->origin);
    ao_spy_drain();

    for ( unsigned i = 1U; SEQ_LEN > i; ++i )
    {
        (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    }
    v = next_verdict();
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "the run still ends in its own verdict");
    TEST_ASSERT_EQUAL_UINT8(TEST_ORIGIN_AUTOTEST, v->origin);
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a second request during a run is ignored", "[buttons_ao]")
{
    buttons_up_once();
    settle();
    test_starts();

    buttons_test_request();
    (void)qf_support_pump(AO_Buttons, 4U);
    TEST_ASSERT_NULL_MESSAGE(next_test_evt(), "no second prompt");
    ao_spy_drain();

    /* Finish the run by timeouts so the next case starts clean. */
    for ( unsigned i = 0U; SEQ_LEN > i; ++i )
    {
        (void)qf_support_tick(AO_Buttons, STEP_TICKS);
    }
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A button still held three seconds after it was judged is not waited for
 * any longer: the step is failed as stuck, naming it, and the run moves on
 * without forgiving it. A key that never comes back is exactly the defect
 * the line is looking for, and a run that waits for it for ever tells
 * nobody. The button still being down when the next prompt goes out is
 * named again, as held. */
TEST_CASE("test_not_released_in_3s_fails", "[buttons_ao]")
{
    uint8_t exp;
    bool    top_high;

    buttons_up_once();
    settle();
    test_starts();

    levels_for(KDRV_BTN_LEFT, &exp, &top_high);
    press_confirmed(exp, top_high);
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NULL_MESSAGE(t, "a clean press waits for its release");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Buttons, RELEASE_TICKS - 1U),
                                   "the release gets three seconds");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons, 1U));
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_STUCK, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(1U << KDRV_BTN_LEFT), t->pressed);
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "the step ends, forgiven or not: the next one begins");
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_HELD, t->phase);
    TEST_ASSERT_EQUAL_UINT8(2U, t->step);
    ao_spy_drain();

    release_confirmed();
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(3U, t->step);
    ao_spy_drain();
    rest_times_out(3U, false);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Buttons, 2U * STEP_TICKS),
                                   "nothing is armed once the run is over");

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A step judged without a press has no press to time: a button already down
 * as the step began, still down when the release limit runs out, is stuck
 * with no duration, not the one the step before it left behind. */
TEST_CASE("test_stuck_after_held_reports_no_ms", "[buttons_ao]")
{
    uint8_t exp;
    bool    top_high;

    buttons_up_once();
    settle();
    test_starts();

    levels_for(KDRV_BTN_LEFT, &exp, &top_high);
    press_confirmed(exp, top_high);
    fake_time_advance_ms(1234U);
    (void)qf_support_tick(AO_Buttons, RELEASE_TICKS);
    const ButtonsTestEvt *t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_STUCK, t->phase);
    TEST_ASSERT_EQUAL_UINT8(1U, t->step);
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_HELD, t->phase);
    TEST_ASSERT_EQUAL_UINT8(2U, t->step);
    ao_spy_drain();

    fake_time_advance_ms(1234U);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons, RELEASE_TICKS));
    t = next_test_evt();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_EQUAL_UINT8(BTN_TEST_STUCK, t->phase);
    TEST_ASSERT_EQUAL_UINT8(2U, t->step);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)(1U << KDRV_BTN_LEFT), t->pressed);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(0U, t->ms, "no press was timed on this step");
    ao_spy_drain();

    release_confirmed();
    ao_spy_drain();
    rest_times_out(3U, false);

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The hold --------------------------------------------------------- */

/** @brief Confirm a bottom-button press and drop its ButtonEvt from the spy. */
static void bot_confirmed_down(void)
{
    feed(BOT_DOWN, true);
    settle_fires();
    feed(BOT_DOWN, true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "the press itself");
    TEST_ASSERT_EQUAL(BUTTON_EVT_SIG, ao_spy_next_sig());
    ao_spy_drain();
}

TEST_CASE("the bottom button held past the threshold publishes one hold",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    bot_confirmed_down();

    /* The clock is real, so let it run out rather than dispatching its
     * signal by hand: that is what proves the threshold is the 600 ms it
     * claims to be, and it leaves no timer armed behind the case. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(
        0U,
        qf_support_tick(AO_Buttons, QF_SUPPORT_STEPS(KODE_BUTTON_HOLD_MS) - 1U),
        "not a hold until the threshold");
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons, 1U));

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonHoldEvt *h = (const ButtonHoldEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL(BUTTON_HOLD_SIG, h->super.sig);
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_BTN_BOT, h->id);
    ao_spy_drain();

    /* The release is an ordinary edge, and there is no second hold. */
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL(BUTTON_EVT_SIG, ao_spy_next_sig());
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a bottom button released before the threshold never holds",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    bot_confirmed_down();
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
    ao_spy_drain();

    /* The timer was disarmed on the release; even a stray expiry must not
     * turn into a hold, because the button is no longer down. */
    QASM_DISPATCH(AO_Buttons, &HOLD_TMO, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("the pad never holds", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    feed(PAD_UP_DOWN, true);
    settle_fires();
    feed(PAD_UP_DOWN, true);
    ao_spy_drain();

    QASM_DISPATCH(AO_Buttons, &HOLD_TMO, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "only the top and the bottom button hold");

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("the top button held publishes one hold with the top id",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    /* The top button confirms only on the settle's own sample. */
    feed(ALL_UP, false);
    settle_fires();
    feed(ALL_UP, false);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons,
                                QF_SUPPORT_STEPS(KODE_BUTTON_HOLD_MS)));
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonHoldEvt *h = (const ButtonHoldEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL(BUTTON_HOLD_SIG, h->super.sig);
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_BTN_TOP, h->id);
    ao_spy_drain();

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The double press ------------------------------------------------------
 * A double press is the top button pressed twice: the second press confirmed
 * within KODE_BUTTON_DOUBLE_MS of the first one's release, the first never
 * having reached hold. Only this AO has the confirmed edges and the times
 * they were confirmed: AO_Gui at priority 3 would time its dispatches, not
 * the presses. So the detector is here and the cases drive real edges
 * through the real debounce, with the fake clock moved the way a thumb moves
 * it.
 */

/** @brief How long a finger stays down on a quick press. */
#define TAP_DOWN_MS     100U
/** @brief A gap well inside the window, between a release and the next press. */
#define QUICK_GAP_MS    300U
/** @brief The rhythm of the case that presses three times. */
#define EVEN_GAP_MS     200U

/** @brief Press @p exp / @p top_high through the debounce, the settle's time passing. */
static void press_timed(uint8_t exp, bool top_high)
{
    feed(exp, top_high);
    fake_time_advance_ms(KODE_SYS_BUTTON_SETTLE_MS);
    settle_fires();
    feed(exp, top_high);
}

/** @brief Release everything through the debounce, the settle's time passing. */
static void release_timed(void)
{
    feed(ALL_UP, true);
    fake_time_advance_ms(KODE_SYS_BUTTON_SETTLE_MS);
    settle_fires();
    feed(ALL_UP, true);
}

/** @brief The top button's confirmed press. */
static void top_press(void)
{
    press_timed(ALL_UP, false);
}

/**
 * @brief Take what the spy holds and say how many doubles were in it.
 *
 * Reads every event, so a double that went out anywhere in the gesture is
 * counted whichever order the others were published in, and leaves the spy
 * empty for the next step.
 */
static unsigned doubles_taken(void)
{
    unsigned n = 0U;
    const QEvt *e = ao_spy_take();
    while ( NULL != e )
    {
        if ( (QSignal)BUTTON_DOUBLE_SIG == e->sig )
        {
            ++n;
        }
        e = ao_spy_take();
    }
    ao_spy_drain();
    return n;
}

TEST_CASE("two quick top presses publish one double after the second press",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(),
                                   "a first press is only itself");
    ao_spy_drain();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "the release is only itself");
    ao_spy_drain();

    fake_time_advance_ms(QUICK_GAP_MS);
    top_press();

    TEST_ASSERT_EQUAL_UINT(2U, ao_spy_count());
    const ButtonEvt *p = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL(BUTTON_EVT_SIG, p->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, p->id);
    TEST_ASSERT_TRUE_MESSAGE(p->pressed, "the second press is published as it always was");
    const ButtonDoubleEvt *d = (const ButtonDoubleEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_MESSAGE(BUTTON_DOUBLE_SIG, d->super.sig,
                              "the double follows the press it completes");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, d->id);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(p->t_ms, d->t_ms,
                                     "the double is stamped by the second press");
    ao_spy_drain();

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/** @brief Room for the log text the double's case builds to look for. */
#define GAP_TEXT_MAX    64U

/* The line names the gap the detector measured, from the release to the
 * confirming sample of the second press, not the window it was allowed. */
TEST_CASE("the double's line says the gap it measured", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(QUICK_GAP_MS);
    fake_klog_reset();
    top_press();
    TEST_ASSERT_EQUAL_UINT(1U, doubles_taken());

    char want[GAP_TEXT_MAX];
    (void)snprintf(want, sizeof want, "top pressed twice, %u ms after",
                   (unsigned)(QUICK_GAP_MS + KODE_SYS_BUTTON_SETTLE_MS));
    const char *line = fake_klog_last();
    TEST_ASSERT_NOT_NULL(line);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(line, want), line);

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a second press after the window is not a double", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(KODE_BUTTON_DOUBLE_MS + 1U);
    top_press();
    TEST_ASSERT_EQUAL_UINT(0U, doubles_taken());

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The window is counted from the first press's release to the second press's
 * confirmation, and its last millisecond is inside it. The two phases land
 * the second press exactly on the edge and one millisecond past it. */
TEST_CASE("a second press exactly at the window's edge is a double, one millisecond later is not",
          "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();
    fake_time_advance_ms(KODE_BUTTON_DOUBLE_MS - KODE_SYS_BUTTON_SETTLE_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, doubles_taken(),
                                   "the window includes its last millisecond");
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(TAP_DOWN_MS);
    top_press();
    ao_spy_drain();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();
    fake_time_advance_ms((KODE_BUTTON_DOUBLE_MS + 1U) - KODE_SYS_BUTTON_SETTLE_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, doubles_taken(),
                                   "one millisecond past the window is a new first press");

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The time a subscriber is told is the counter's, and the counter wraps at
 * about 49 days. The interval is an unsigned subtraction, so a pair of
 * presses that straddles the wrap is a double like any other. */
TEST_CASE("a double across the wrap of the millisecond counter is still a double",
          "[buttons_ao]")
{
    uint32_t const NEAR_WRAP_MS = UINT32_MAX - 200U;

    buttons_up_once();
    settle();
    fake_time_reset();
    fake_time_advance_ms(NEAR_WRAP_MS);

    top_press();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(QUICK_GAP_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT(2U, ao_spy_count());
    const ButtonEvt *p = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(NEAR_WRAP_MS, p->t_ms,
                                         "the counter did wrap between the presses");
    const ButtonDoubleEvt *d = (const ButtonDoubleEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL(BUTTON_DOUBLE_SIG, d->super.sig);
    ao_spy_drain();

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The person was holding, not tapping: a press that reached hold never
 * starts a double, however soon the next one follows its release. */
TEST_CASE("a first press held to hold does not start a double", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    ao_spy_drain();
    fake_time_advance_ms(KODE_BUTTON_HOLD_MS);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Buttons,
                                QF_SUPPORT_STEPS(KODE_BUTTON_HOLD_MS)));
    TEST_ASSERT_EQUAL(BUTTON_HOLD_SIG, ao_spy_next_sig());
    ao_spy_drain();

    release_timed();
    ao_spy_drain();
    fake_time_advance_ms(TAP_DOWN_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, doubles_taken(),
                                   "a press soon after a hold is just a press");

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A hold ends the pairing for that press only: the quick taps that follow it
 * pair up as usual. */
TEST_CASE("quick taps after a hold are a double again", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    fake_time_advance_ms(KODE_BUTTON_HOLD_MS);
    (void)qf_support_tick(AO_Buttons, QF_SUPPORT_STEPS(KODE_BUTTON_HOLD_MS));
    ao_spy_drain();
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(TAP_DOWN_MS);
    top_press();
    ao_spy_drain();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();
    fake_time_advance_ms(QUICK_GAP_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT(1U, doubles_taken());

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* Three quick presses make one double, not two: the press that completes a
 * double is spent, and its release starts nothing. */
TEST_CASE("three quick presses make one double", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    TEST_ASSERT_EQUAL_UINT(0U, doubles_taken());
    fake_time_advance_ms(EVEN_GAP_MS);
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(EVEN_GAP_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, doubles_taken(),
                                   "the second press makes the double");
    fake_time_advance_ms(EVEN_GAP_MS);
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(EVEN_GAP_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, doubles_taken(),
                                   "the third press is not a second double");

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* Pairs, not a running count: the third press starts a new pair, and the
 * fourth completes it. */
TEST_CASE("four quick presses make two doubles", "[buttons_ao]")
{
    unsigned doubles = 0U;

    buttons_up_once();
    settle();

    for ( unsigned i = 0U; 4U > i; ++i )
    {
        top_press();
        doubles += doubles_taken();
        fake_time_advance_ms(EVEN_GAP_MS);
        release_timed();
        ao_spy_drain();
        fake_time_advance_ms(EVEN_GAP_MS);
    }
    TEST_ASSERT_EQUAL_UINT(2U, doubles);

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("the bottom button never makes a double", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    press_timed(BOT_DOWN, true);
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();
    fake_time_advance_ms(QUICK_GAP_MS);
    press_timed(BOT_DOWN, true);
    TEST_ASSERT_EQUAL_UINT(0U, doubles_taken());

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("the pad never makes a double", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    press_timed(PAD_UP_DOWN, true);
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();
    fake_time_advance_ms(QUICK_GAP_MS);
    press_timed(PAD_UP_DOWN, true);
    TEST_ASSERT_EQUAL_UINT(0U, doubles_taken());

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* Injection bypasses the detector, so a bench script stays deterministic:
 * `btn id=top state=double` is the one way to put a double on the bus. */
TEST_CASE("injected presses never make a double", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    fake_time_advance_ms(TAP_DOWN_MS);
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    fake_time_advance_ms(TAP_DOWN_MS);
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, doubles_taken(), "no double from injection");

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("an injected press does not complete a real first press", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    top_press();
    fake_time_advance_ms(TAP_DOWN_MS);
    release_timed();
    ao_spy_drain();

    fake_time_advance_ms(TAP_DOWN_MS);
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    TEST_ASSERT_EQUAL_UINT(0U, doubles_taken());
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("an injected release does not start a double for a real press", "[buttons_ao]")
{
    buttons_up_once();
    settle();

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    fake_time_advance_ms(TAP_DOWN_MS);
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    ao_spy_drain();

    fake_time_advance_ms(TAP_DOWN_MS);
    top_press();
    TEST_ASSERT_EQUAL_UINT(0U, doubles_taken());

    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The pad follows the glass ------------------------------------------- */

/** @brief Ask AO_Buttons for @p quarter_turns and let it take the request. */
static void rotate(uint8_t quarter_turns)
{
    buttons_rotation_request(quarter_turns);
    (void)qf_support_pump(AO_Buttons, 4U);
}

/** @brief Press what @p levels holds down, confirmed; return the published id. */
static uint8_t press_id(uint8_t levels)
{
    feed(levels, true);
    settle_fires();
    feed(levels, true);
    const ButtonEvt * const b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(b, "the press was not published");
    TEST_ASSERT_EQUAL_INT(BUTTON_EVT_SIG, b->super.sig);
    TEST_ASSERT_TRUE(b->pressed);
    uint8_t const id = b->id;
    ao_spy_drain();
    return id;
}

/** @brief Let go of everything, confirmed; return the released id. */
static uint8_t release_id(void)
{
    feed(ALL_UP, true);
    settle_fires();
    feed(ALL_UP, true);
    const ButtonEvt * const b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(b, "the release was not published");
    TEST_ASSERT_FALSE(b->pressed);
    uint8_t const id = b->id;
    ao_spy_drain();
    return id;
}

/** @brief Each pad button, pressed and released alone, publishes these ids. */
static void pad_reads(uint8_t up, uint8_t down, uint8_t left, uint8_t right)
{
    TEST_ASSERT_EQUAL_UINT8(up, press_id(PAD_UP_DOWN));
    TEST_ASSERT_EQUAL_UINT8(up, release_id());
    TEST_ASSERT_EQUAL_UINT8(down, press_id(PAD_DOWN_DOWN));
    TEST_ASSERT_EQUAL_UINT8(down, release_id());
    TEST_ASSERT_EQUAL_UINT8(left, press_id(LEFT_DOWN));
    TEST_ASSERT_EQUAL_UINT8(left, release_id());
    TEST_ASSERT_EQUAL_UINT8(right, press_id(RIGHT_DOWN));
    TEST_ASSERT_EQUAL_UINT8(right, release_id());
}

/** @brief Back to the glass as wired, for the next case. */
static void rotation_end(void)
{
    rotate(0U);
    settle();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The way Mando IR turns its glass: what is up in the user's hand is the
 * pad's right, and so on round. */
TEST_CASE("the pad at 270 publishes UP for RIGHT, DOWN for LEFT, LEFT for UP and RIGHT for DOWN",
          "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    rotate(3U);
    pad_reads(KDRV_BTN_LEFT, KDRV_BTN_RIGHT, KDRV_BTN_DOWN, KDRV_BTN_UP);
    rotation_end();
}

TEST_CASE("the pad at 180 swaps up with down and left with right",
          "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    rotate(2U);
    pad_reads(KDRV_BTN_DOWN, KDRV_BTN_UP, KDRV_BTN_RIGHT, KDRV_BTN_LEFT);
    rotation_end();
}

TEST_CASE("the pad at 90 is the inverse of 270", "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    rotate(1U);
    pad_reads(KDRV_BTN_RIGHT, KDRV_BTN_LEFT, KDRV_BTN_UP, KDRV_BTN_DOWN);
    rotation_end();
}

/* TOP and BOT are the side buttons, not the pad. */
TEST_CASE("top and bot never rotate", "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    rotate(3U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, press_id(BOT_DOWN));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, release_id());

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_TOP, b->id);
    ao_spy_drain();
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    ao_spy_drain();
    rotation_end();
}

/* A press and its release always map alike: a rotation that lands with the
 * pad held waits until it is let go, so the release says what the press
 * said and nothing is left down for good. */
TEST_CASE("a rotation asked while the pad is held waits for its release",
          "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_UP, press_id(PAD_UP_DOWN));
    rotate(3U);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KDRV_BTN_UP, release_id(),
        "the release must say what its press said");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_LEFT, press_id(PAD_UP_DOWN));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_LEFT, release_id());
    TEST_ASSERT_EQUAL_HEX32(0U, kode_sys_button_read_mask());
    rotation_end();
}

/* The console's `btn` is a thumb as far as an app can tell, so a bench test
 * sees the pad the way the glass reads it. */
TEST_CASE("an injected pad press is rotated too", "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    rotate(3U);
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_RIGHT, true));
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_UP, b->id);
    TEST_ASSERT_TRUE(b->pressed);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_HEX32(BIT(KDRV_BTN_UP), kode_sys_button_read_mask());

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_RIGHT, false));
    b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_UP, b->id);
    TEST_ASSERT_FALSE(b->pressed);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_HEX32(0U, kode_sys_button_read_mask());
    rotation_end();
}

/* btn_read judges the board, not the app: it reports the buttons as wired. */
TEST_CASE("btn_read stays physical under a rotation", "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    rotate(3U);

    buttons_dump_request();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Buttons, 4U));
    feed(RIGHT_DOWN, true);
    const ButtonsStateEvt *s = (const ButtonsStateEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(BTN_STATE_SIG, s->super.sig);
    TEST_ASSERT_EQUAL_HEX8(BIT(KDRV_BTN_RIGHT), s->pressed);
    ao_spy_drain();

    khal_gpio_irq_enable_Expect(KBSP_PIN_BUTTON_TOP);
    QASM_DISPATCH(AO_Buttons, &SETTLE, 0U);
    feed(ALL_UP, true);
    ao_spy_drain();
    rotation_end();
}

TEST_CASE("a rotation past 270 is ignored", "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    rotate(4U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_RIGHT, press_id(RIGHT_DOWN));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_RIGHT, release_id());
    rotation_end();
}

/* An injected pad press holds a turn back like a thumb does, and its injected
 * release lets it through: nothing else may be needed to apply it. */
TEST_CASE("a rotation held back by an injected press applies on its injected release",
          "[buttons_ao][rotation]")
{
    buttons_up_once();
    settle();
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_UP, true));
    ao_spy_drain();
    rotate(3U);
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_UP, false));
    const ButtonEvt *b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KDRV_BTN_UP, b->id,
        "the release must say what its press said");
    ao_spy_drain();
    (void)qf_support_pump(AO_Buttons, 4U);

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_UP, true));
    b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KDRV_BTN_LEFT, b->id,
        "the turn never applied after the pad went idle");
    ao_spy_drain();
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_UP, false));
    ao_spy_drain();
    rotation_end();
}

/* ---- Only the settle's sample confirms -------------------------------------- */

/* Measured on 6c:0a: as BOT is let go its line rings, and the expander answers
 * each edge with a reading about 0.3 ms after the last. Two equal readings in a
 * row used to be enough, so the ringing published a release, a press nobody
 * made, and a second release within one millisecond. A change now waits for the
 * settle's own sample, as the top button's always did. */
TEST_CASE("a release that rings is one release, with no press inside it",
          "[buttons_ao][debounce]")
{
    buttons_up_once();
    settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, press_id(BOT_DOWN));

    feed(ALL_UP, true);     /* the readings of 14:16:15.351, back to back */
    feed(ALL_UP, true);
    feed(BOT_DOWN, true);
    feed(BOT_DOWN, true);
    feed(ALL_UP, true);
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "readings between two settles published a change");

    settle_fires();
    feed(ALL_UP, true);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const ButtonEvt * const b = (const ButtonEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, b->id);
    TEST_ASSERT_FALSE(b->pressed);
    ao_spy_drain();
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* The other side of it: a quick finger holds each level longer than a settle,
 * so every tap of a fast run is still a press and a release. */
TEST_CASE("fast taps each confirm on their settle", "[buttons_ao][debounce]")
{
    buttons_up_once();
    settle();
    for ( unsigned i = 0U; 5U > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, press_id(BOT_DOWN));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BTN_BOT, release_id());
    }
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- How many presses a button has had ----------------------------------- */

/* The count is monotonic since boot and shared by the whole binary, so each
 * case reads it before and after. BOT is not a pad button, so the pad's
 * rotation cannot move its presses to another id. */
TEST_CASE("presses count debounced press edges", "[buttons_ao][presses]")
{
    buttons_up_once();
    settle();
    uint32_t const before = buttons_presses((uint8_t)KDRV_BTN_BOT);
    uint32_t const other  = buttons_presses((uint8_t)KDRV_BTN_UP);

    press_confirmed(BOT_DOWN, true);
    TEST_ASSERT_EQUAL_UINT32(before + 1U, kode_sys_button_read_presses((uint8_t)KDRV_BTN_BOT));
    release_confirmed();
    TEST_ASSERT_EQUAL_UINT32(before + 1U, kode_sys_button_read_presses((uint8_t)KDRV_BTN_BOT));
    press_confirmed(BOT_DOWN, true);
    TEST_ASSERT_EQUAL_UINT32(before + 2U, kode_sys_button_read_presses((uint8_t)KDRV_BTN_BOT));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(other, buttons_presses((uint8_t)KDRV_BTN_UP),
                                     "another button's count does not move");
    ao_spy_drain();
    release_confirmed();
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a release alone counts nothing", "[buttons_ao][presses]")
{
    buttons_up_once();
    settle();
    press_confirmed(BOT_DOWN, true);
    ao_spy_drain();
    uint32_t const held = buttons_presses((uint8_t)KDRV_BTN_BOT);

    release_confirmed();
    TEST_ASSERT_EQUAL_UINT32(held, buttons_presses((uint8_t)KDRV_BTN_BOT));
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("an injected press counts", "[buttons_ao][presses]")
{
    buttons_up_once();
    settle();
    uint32_t const before = buttons_presses((uint8_t)KDRV_BTN_TOP);

    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, true));
    TEST_ASSERT_EQUAL_UINT32(before + 1U, kode_sys_button_read_presses((uint8_t)KDRV_BTN_TOP));
    TEST_ASSERT_TRUE(buttons_inject((uint8_t)KDRV_BTN_TOP, false));
    TEST_ASSERT_EQUAL_UINT32(before + 1U, kode_sys_button_read_presses((uint8_t)KDRV_BTN_TOP));
    ao_spy_drain();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

TEST_CASE("a bad button id reads zero presses", "[buttons_ao][presses]")
{
    buttons_up_once();
    TEST_ASSERT_EQUAL_UINT32(0U, buttons_presses((uint8_t)KDRV_BTN_COUNT));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_button_read_presses((uint8_t)KDRV_BTN_COUNT));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_button_read_presses(UINT8_MAX));

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}
