/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_touch_ao.c
 * @brief AO_Touch: what it reports, the lost drag, the guided test and the fields it serves.
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
#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_cst820.h"
#include "kqp.h"
#include "kset.h"
#include "qf_support.h"

#define FOREIGN_ORIGIN 7U   /**< The number of a client other than the autotest. */

/* What an older kodeOS left in the store: a calibration under `kset/tcal`,
 * an identity scale and a few pixels of offset. Nothing reads it any more. */
#define OLD_TCAL_KEY  "tcal"
#define OLD_TCAL_DX   3
#define OLD_TCAL_DY   (-2)

#define REG_FINGERS     0x02U
#define REG_XH          0x03U
#define REG_XL          0x04U
#define REG_YH          0x05U
#define REG_YL          0x06U
#define REG_CHIP_ID     0xA7U
#define REG_AUTO_SLEEP  0xF9U
#define REG_IRQ_CTL     0xFAU
#define REG_DIS_SLEEP   0xFEU

/* What the chip holds awake, seeded before the bring-up reads it: the rest
 * changes all three and the wake must put exactly these back. */
#define AWAKE_IRQ_CTL    0x60U
#define AWAKE_DIS_SLEEP  0x01U
#define AWAKE_SLEEP_S    0x0AU

/** @brief The poll event, which is what a case feeds the AO instead of time. */
static const QEvt POLL = QEVT_INITIALIZER(TOUCH_POLL_SIG);

/* ---- The chip, as a register file ---------------------------------------- */

static uint8_t  s_regs[256];
static bool     s_bus_dead;
static unsigned s_reads;
static unsigned s_bus_tries;     /**< Reads asked of the chip, answered or not. */

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int ncalls)
{
    (void)dev; (void)ncalls;
    ++s_bus_tries;
    if ( s_bus_dead )
    {
        return KODE_ERROR;
    }
    (void)memcpy(buf, &s_regs[reg], len);
    ++s_reads;
    return KODE_OK;
}

static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val, int ncalls)
{
    (void)dev; (void)ncalls;
    if ( s_bus_dead )
    {
        return KODE_ERROR;
    }
    s_regs[reg] = val;
    return KODE_OK;
}

/** @brief Put a finger on the register file, or take it off. */
static void finger(bool down, int16_t x, int16_t y)
{
    s_regs[REG_FINGERS] = down ? 1U : 0U;
    s_regs[REG_XH] = (uint8_t)(((uint16_t)x >> 8) & 0x0FU);
    s_regs[REG_XL] = (uint8_t)((uint16_t)x & 0xFFU);
    s_regs[REG_YH] = (uint8_t)(((uint16_t)y >> 8) & 0x0FU);
    s_regs[REG_YL] = (uint8_t)((uint16_t)y & 0xFFU);
}

/* The driver is already up: test_kdrv_cst820.c sorts first and hands the mock a real handle. */
/**
 * @brief Bring AO_Touch up, once for the whole binary, and idle.
 *
 * Once, for the same reason AO_Power is: Touch_tracking arms a QTimeEvt
 * on entry, and re-running the constructor over a timer that is
 * still linked into QF's list corrupts that list. The AO is left in
 * Touch_idle here and driven into sampling by the case that wants it.
 */
static void touch_up_once(void)
{
    static bool s_started;

    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(TOUCH_EVT_SIG);
    ao_spy_subscribe(TOUCH_TEST_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);

    s_bus_dead = false;
    s_reads    = 0U;

    if ( !s_started )
    {
        (void)memset(s_regs, 0, sizeof(s_regs));
        s_regs[REG_CHIP_ID]   = KDRV_CST820_CHIP_ID;
        s_regs[REG_IRQ_CTL]   = AWAKE_IRQ_CTL;
        s_regs[REG_DIS_SLEEP] = AWAKE_DIS_SLEEP;
        s_regs[REG_AUTO_SLEEP] = AWAKE_SLEEP_S;
        Touch_ctor();
        qf_support_give_queue(AO_Touch);
        AO_Touch->prio  = KODE_PRIO_TOUCH;
        AO_Touch->pthre = KODE_PRIO_TOUCH;
        QActive_register_(AO_Touch);
        QASM_INIT(AO_Touch, (void *)0, 0U);
        qf_support_background(AO_Touch);
        s_started = true;
    }

    fake_klog_reset();
    ao_spy_drain();
}

/** @brief Verify the mock and put it away. */
static void mock_down(void)
{
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
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

/**
 * @brief Announce the panel, which is what lets this AO touch the chip.
 * @param up Whether the panel came up.
 */
static void announce_panel(bool up)
{
    DisplayStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(DISPLAY_STATE_SIG);
    e.up    = up;
    QASM_DISPATCH(AO_Touch, &e.super, 0U);
}

/** @brief The expander's owner says P6 moved to @p low. */
static void p6_moves(bool low)
{
    IoexpInputEvt e;

    e.super   = (QEvt)QEVT_INITIALIZER(IOEXP_INPUT_SIG);
    e.levels  = 0xFFU;
    if ( low )
    {
        e.levels = (uint8_t)~(1U << KBSP_IOEXP_BIT_TP_INT);
    }
    e.changed = (uint8_t)(1U << KBSP_IOEXP_BIT_TP_INT);
    e.ok      = true;
    QASM_DISPATCH(AO_Touch, &e.super, 0U);
}

/**
 * @brief Drive the AO into Touch_armed with a working controller.
 *
 * The first time through, an old calibration record is seeded, so the whole
 * file runs with one in the store that the AO has to leave alone.
 */
static void touch_armed(void)
{
    static bool s_seeded;

    touch_up_once();
    if ( !s_seeded )
    {
        struct {
            uint32_t magic; uint16_t version; uint16_t pad;
            int32_t ax; int32_t bx; int32_t ay; int32_t by;
        } rec = { 0x4C43544BUL, 1U, 0U, 1024, OLD_TCAL_DX, 1024, OLD_TCAL_DY };
        fake_nvs_seed(KSET_NAMESPACE, OLD_TCAL_KEY, &rec, sizeof(rec));
        s_seeded = true;
    }
    announce_panel(true);
    ao_spy_drain();
}

/** @brief Drive the AO into Touch_tracking: the controller's IRQ fell. */
static void touch_sampling(void)
{
    touch_armed();
    p6_moves(true);
}

/** @brief Feed one sample in and let the AO decide what it means. */
static void sample(bool pressed, int16_t x, int16_t y)
{
    finger(pressed, x, y);
    QASM_DISPATCH(AO_Touch, &POLL, 0U);
}

/** @brief The next published sample, or NULL if the AO said nothing. */
static const TouchEvt *published(void)
{
    const QEvt * const e = ao_spy_take();

    if ( (NULL == e) || (TOUCH_EVT_SIG != e->sig) )
    {
        return NULL;
    }
    return (TouchEvt const *)e;
}

/**
 * @brief Dispatch whatever is waiting in AO_Touch's queue.
 *
 * By hand rather than through qf_support_pump(): this AO is a background
 * one, for its poll timer, and the pump drains every background
 * queue before it dispatches - a request posted a moment ago included.
 */
static void serve(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Touch->eQueue, &e, 0U) == pdTRUE )
    {
        QASM_DISPATCH(AO_Touch, e, 0U);
        QF_gc(e);
    }
}

/** @brief Ask for a field, let the AO serve it, and take the answer. */
static const TouchFieldEvt *field(uint8_t op, uint16_t f, int32_t value)
{
    touch_field_request(ao_spy(), op, f, value);
    serve();
    const QEvt * const e = ao_spy_take();
    if ( (NULL == e) || (TOUCH_FIELD_RSP_SIG != e->sig) )
    {
        return NULL;
    }
    return (TouchFieldEvt const *)e;
}

/* ---- Waiting for the panel ------------------------------------------------- */

TEST_CASE("AO_Touch owns the touch controller", "[touch_ao]")
{
    touch_up_once();
    TEST_ASSERT_EQUAL_STRING("AO_Touch", kbsp_owner_name(KBSP_RES_TOUCH));
    mock_down();
}

/* The two share the GPIO7 reset line, so bringing the panel up resets the
 * touch controller. Initialising before the panel is not merely early - it
 * is undone, and the board comes up with a chip nobody configured. And with
 * no controller up there is nothing to serve a field from: the answer is a
 * refusal, not silence, because the console's watchdog would otherwise be
 * the only thing that ever said so. */
TEST_CASE("the controller is not touched until the panel says it is up, and a "
          "field asked meanwhile is refused", "[touch_ao]")
{
    touch_up_once();

    announce_panel(false);
    QASM_DISPATCH(AO_Touch, &POLL, 0U);
    p6_moves(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_reads, "idle: nothing reads the chip");

    const TouchFieldEvt * const a = field(FIELD_OP_GET, (uint16_t)KDRV_CST_AUTO_SLEEP_TIME, 0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_CST_AUTO_SLEEP_TIME, a->field);
    TEST_ASSERT_EQUAL_UINT(0U, s_reads);

    mock_down();
}

/* ---- A controller that does not answer at the bring-up -------------------- */

/* These cases run before any other brings the controller up, in this order:
 * the AO has no way back to Touch_idle, and the last of them finds the chip. */

static void round_for(kode_lp_state_t target);
static void state_awake(void);
static void expect_answer(bool busy, const char *why);

static bool     s_answers;   /**< Whether the chip ACKs a probe.  */
static uint32_t s_probes;    /**< Probes asked of the bus.        */

static bool on_probe(uint16_t addr, uint32_t timeout_ms, int ncalls)
{
    (void)addr; (void)timeout_ms; (void)ncalls;
    ++s_probes;
    return s_answers;
}

/** @brief The handle a bring-up gets: any address that is not NULL. */
static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

static kode_status_t on_add(uint16_t addr, khal_i2c_dev_t *out, int ncalls)
{
    (void)addr; (void)ncalls;
    *out = s_handle;
    return KODE_OK;
}

/**
 * @brief Open a case with a chip that answers probes, or does not.
 * @param answers Whether it ACKs.
 */
static void chip(bool answers)
{
    touch_up_once();
    khal_i2c_remove_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_StubWithCallback(&on_probe);
    s_answers = answers;
    s_probes  = 0U;
}

/** @brief Let @p ms go by: the clock and the AO's own timers. */
static void run_ms(uint32_t ms)
{
    fake_time_advance_ms(ms);
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(ms));
}

/* The panel's bring-up has just pulsed the reset line the two share, so the
 * one try the AO used to make could land on a controller still coming back,
 * and the touch stayed dead until a restart. The looks come at 0.2, 0.5, 1 and
 * 2 s after that try, then every 10 s; each is one probe. */
TEST_CASE("a controller silent at the bring-up is looked for again: four times soon, then spaced",
          "[touch_ao]")
{
    chip(true);
    (void)kdrv_cst820_park();      /* The driver lets go of the chip, as a boot finds it. */
    TEST_ASSERT_FALSE(kdrv_cst820_present());
    s_answers = false;

    announce_panel(true);
    TEST_ASSERT_TRUE_MESSAGE(logged("looking again"), "the first try's failure was final");
    TEST_ASSERT_FALSE(kdrv_cst820_present());

    s_probes = 0U;
    run_ms(350U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_probes, "no look 0.2 s after the try");
    run_ms(400U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_probes, "no look at 0.5 s");
    run_ms(750U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_probes, "no look at 1 s");
    run_ms(1100U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4U, s_probes, "no look at 2 s");
    run_ms(8400U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4U, s_probes, "a spaced look came early");
    run_ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(5U, s_probes, "no look 10 s after the last soon one");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_reads, "a chip that never answered was read");
    mock_down();
}

/* A board at rest is not woken every ten seconds to look for a chip. */
TEST_CASE("a round stops the looks, and AWAKE starts them again", "[touch_ao][lowpower]")
{
    chip(false);

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    run_ms(30000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_probes, "the board was woken to look for the chip");

    state_awake();
    run_ms(KODE_TOUCH_LOOK_EVERY_MS + 500U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_probes, "the looks did not start again at AWAKE");
    mock_down();
}

/* A controller in its low-power scan answers nothing until a finger touches
 * the glass, and its line is how it says so: the best moment to look. Not on
 * every edge, though: a contact pulses the line many times a second. */
TEST_CASE("the interrupt line falling looks at once, at most once a second", "[touch_ao]")
{
    chip(false);
    fake_time_advance_ms(KODE_TOUCH_LOOK_EDGE_MS);

    p6_moves(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_probes, "the edge did not look");
    p6_moves(false);
    p6_moves(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_probes, "a second edge within the second looked again");

    fake_time_advance_ms(KODE_TOUCH_LOOK_EDGE_MS);
    p6_moves(false);
    p6_moves(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_probes, "a later edge did not look");
    mock_down();
}

/* Five minutes, as AO_Display gives a panel. After that a board with no
 * controller is left alone, but a touch still looks. */
TEST_CASE("the timed looks end after five minutes, and say so; the interrupt still looks",
          "[touch_ao]")
{
    chip(false);

    run_ms(KODE_TOUCH_LOOK_FOR_MS + 20000U);
    TEST_ASSERT_TRUE_MESSAGE(logged("only when the glass is touched"), "the end was not said");

    s_probes = 0U;
    run_ms(60000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_probes, "the timed looks went on past their end");
    p6_moves(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_probes, "a touch no longer looks");
    p6_moves(false);
    mock_down();
}

/* The log line is the measurement: at which look, and how long after the panel. */
TEST_CASE("the look that finds the controller arms the touch and says when", "[touch_ao]")
{
    chip(false);
    fake_time_advance_ms(KODE_TOUCH_LOOK_EDGE_MS);

    s_answers = true;
    khal_i2c_add_StubWithCallback(&on_add);
    p6_moves(true);                 /* A finger wakes it, and its line says so. */
    TEST_ASSERT_TRUE_MESSAGE(kdrv_cst820_present(), "the chip answered and was not taken");
    TEST_ASSERT_TRUE_MESSAGE(logged("answered at look"), "when it answered was not said");
    p6_moves(false);

    /* Armed, like after a bring-up that worked at once. */
    p6_moves(true);
    sample(true, 50, 60);
    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "found, and its press was not published");
    TEST_ASSERT_EQUAL_INT16(50, e->x);
    TEST_ASSERT_EQUAL_INT16(60, e->y);
    sample(false, 50, 60);
    ao_spy_drain();

    s_probes = 0U;
    run_ms(KODE_TOUCH_LOOK_EVERY_MS * 2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_probes, "the looks went on after the chip was found");
    mock_down();
}

/* A calibration an older kodeOS stored is of no use to this one: the
 * controller's coordinates are the panel's. The record is in the store from
 * the first case of the file, and a sample at (100, 100) reaches everybody
 * as (100, 100), not shifted by what the record says. */
TEST_CASE("test_old_tcal_blob_is_ignored", "[touch_ao]")
{
    touch_sampling();

    sample(true, 100, 100);

    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT16(100, e->x);
    TEST_ASSERT_EQUAL_INT16(100, e->y);
    sample(false, 100, 100);
    ao_spy_drain();
    mock_down();
}

/* ---- Waiting for the interrupt ------------------------------------------- */

/* The whole point of the interrupt: an armed AO reads nothing. Most of the
 * day the glass is untouched, and until this every one of those days was
 * sixty-six I2C transactions a second for nothing. */
TEST_CASE("nothing is read until the controller's IRQ falls", "[touch_ao]")
{
    touch_armed();

    QASM_DISPATCH(AO_Touch, &POLL, 0U);
    p6_moves(false);                        /* the line went high: not a touch */

    IoexpInputEvt other;
    other.super   = (QEvt)QEVT_INITIALIZER(IOEXP_INPUT_SIG);
    other.levels  = 0xFEU;                  /* a button, not the touch */
    other.changed = 0x01U;
    other.ok      = true;
    QASM_DISPATCH(AO_Touch, &other.super, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, s_reads);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Touch, 10U),
                                   "armed, nothing polls");
    mock_down();
}

/* The IRQ falls and the AO starts polling. Whether the controller pulses INT
 * once per report or once per touch differs between CST816S firmwares, so
 * from the first edge to the release it polls rather than trusting the line. */
TEST_CASE("the IRQ falling starts the poll, and a press is read from it",
          "[touch_ao]")
{
    touch_armed();

    p6_moves(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the edge alone says nothing");

    finger(true, 40, 50);
    /* KODE_TOUCH_POLL_MS is 10: the first poll is one step later. */
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Touch, 1U));
    TEST_ASSERT_EQUAL_UINT(1U, s_reads);

    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->pressed);
    TEST_ASSERT_EQUAL_INT16(40, e->x);
    TEST_ASSERT_EQUAL_INT16(50, e->y);

    /* Back to rest for the cases that follow. */
    sample(false, 40, 50);
    ao_spy_drain();
    mock_down();
}

/* QP posts a time event with no margin, and a queue that is full is an
 * assertion and a reset. A poll that is periodic goes on posting whether or
 * not the AO took the last one: sixteen periods without the CPU fill its
 * queue, and at ten milliseconds a period that is a stall of a sixth of a
 * second. So one poll waits at most, and the one that is taken arms the next. */
TEST_CASE("a poll nobody took is not followed by another; the one taken arms the next",
          "[touch_ao]")
{
    touch_armed();
    p6_moves(true);
    finger(true, 40, 50);

    /* Fifteen periods go by and AO_Touch never runs. */
    for ( uint32_t i = 0U; (15U * KQP_TICKS_PER_MS(KODE_TOUCH_POLL_MS)) > i; ++i )
    {
        QTIMEEVT_TICK_X(0U, (void *)0);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, (unsigned)uxQueueMessagesWaiting(AO_Touch->eQueue),
                                   "every period queued a poll the AO had not taken");

    serve();
    TEST_ASSERT_EQUAL_UINT(1U, s_reads);
    TEST_ASSERT_NOT_NULL(published());

    /* One period after the poll that was taken, the next. */
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_Touch, 1U));
    TEST_ASSERT_EQUAL_UINT(2U, s_reads);

    sample(false, 40, 50);
    ao_spy_drain();
    mock_down();
}

/* An IRQ with no finger behind it - a lift the AO had already seen, a glitch
 * on the line - polls a few times and gives up, rather than polling forever. */
TEST_CASE("an IRQ that brings no finger returns to waiting after a few empty polls",
          "[touch_ao]")
{
    touch_sampling();

    sample(false, 0, 0);
    sample(false, 0, 0);
    sample(false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(3U, s_reads);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* Armed again: this poll is not a read. */
    QASM_DISPATCH(AO_Touch, &POLL, 0U);
    TEST_ASSERT_EQUAL_UINT(3U, s_reads);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Touch, 10U),
                                   "nothing armed after giving up");
    mock_down();
}

/* ---- Press and release ----------------------------------------------------- */

/* The glass is 410x502 and the controller counts to 4095: a sample past the
 * right or bottom edge reaches everybody pinned to the last pixel, as it did
 * when a calibration stood in between. (The driver masks the controller's
 * twelve bits, so a negative sample cannot be made here; the clamp pins it to
 * 0 all the same.) */
TEST_CASE("test_sample_past_edge_is_clamped", "[touch_ao]")
{
    touch_sampling();

    sample(true, 4000, 4000);
    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->pressed);
    TEST_ASSERT_EQUAL_INT16((int16_t)(KBSP_LCD_H_RES - 1U), e->x);
    TEST_ASSERT_EQUAL_INT16((int16_t)(KBSP_LCD_V_RES - 1U), e->y);

    sample(true, 4010, 4010);
    const TouchEvt * const f = published();
    TEST_ASSERT_NULL_MESSAGE(f, "still the last pixel: nothing moved");

    sample(true, 100, 4000);
    const TouchEvt * const g = published();
    TEST_ASSERT_NOT_NULL(g);
    TEST_ASSERT_EQUAL_INT16(100, g->x);
    TEST_ASSERT_EQUAL_INT16((int16_t)(KBSP_LCD_V_RES - 1U), g->y);

    sample(false, 100, 4000);
    ao_spy_drain();
    mock_down();
}

TEST_CASE("a press is published with its coordinates", "[touch_ao]")
{
    touch_sampling();

    sample(true, 100, 200);

    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->pressed);
    TEST_ASSERT_EQUAL_INT16(100, e->x);
    TEST_ASSERT_EQUAL_INT16(200, e->y);

    sample(false, 100, 200);
    ao_spy_drain();
    mock_down();
}

/* A finger resting still costs nothing downstream. That is the whole reason
 * this AO filters at all: the GUI never has to know the panel is polled. */
TEST_CASE("a finger resting still is published once, not every poll",
          "[touch_ao]")
{
    touch_sampling();

    sample(true, 50, 60);
    TEST_ASSERT_NOT_NULL(published());

    sample(true, 50, 60);
    sample(true, 51, 61);      /* inside epsilon: jitter, not motion */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a still finger must not fill the GUI's queue");

    sample(false, 51, 61);
    ao_spy_drain();
    mock_down();
}

/* The release carries the last coordinates rather than zeros: a subscriber
 * deciding what was tapped reads them, and a release at (0,0) would put every
 * tap in the top-left corner. */
TEST_CASE("a release is published, and keeps the last coordinates",
          "[touch_ao]")
{
    touch_sampling();

    sample(true, 300, 400);
    TEST_ASSERT_NOT_NULL(published());

    sample(false, 0, 0);

    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->pressed);
    TEST_ASSERT_EQUAL_INT16(300, e->x);
    TEST_ASSERT_EQUAL_INT16(400, e->y);
    mock_down();
}

/* How fast a finger moved is a matter of when each sample was read, and AO_Gui
 * is dispatched late under a render pass, so each sample carries the instant
 * it was read. */
TEST_CASE("a sample carries the time it was read", "[touch_ao]")
{
    uint32_t const PRESS_MS   = 2500U;
    uint32_t const RELEASE_MS = 120U;

    touch_sampling();
    fake_time_reset();

    fake_time_advance_ms(PRESS_MS);
    sample(true, 100, 200);
    const TouchEvt * e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_TRUE(e->pressed);
    TEST_ASSERT_EQUAL_UINT32(PRESS_MS, e->t_ms);

    fake_time_advance_ms(RELEASE_MS);
    sample(false, 0, 0);
    e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->pressed);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(PRESS_MS + RELEASE_MS, e->t_ms,
        "the release carries its own time, not the press's");
    mock_down();
}

/* No finger, nothing to say. */
TEST_CASE("an empty panel says nothing at all", "[touch_ao]")
{
    touch_sampling();

    sample(false, 0, 0);
    sample(false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    sample(false, 0, 0);       /* the third empty poll: back to armed */
    mock_down();
}

/* And a release is announced once, because after it the AO is waiting for
 * the interrupt again and a poll is not even a read. */
TEST_CASE("a release is announced once, and ends the polling", "[touch_ao]")
{
    touch_sampling();

    sample(true, 10, 10);
    (void)published();
    sample(false, 10, 10);
    TEST_ASSERT_NOT_NULL(published());

    unsigned const reads = s_reads;
    QASM_DISPATCH(AO_Touch, &POLL, 0U);     /* armed: no read is expected */
    TEST_ASSERT_EQUAL_UINT(reads, s_reads);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Touch, 10U),
                                   "released: the poll is disarmed");
    mock_down();
}

/* A controller that stops answering with a finger down is a touch that has
 * ended as far as anybody downstream can know: the release goes out with the
 * last position, rather than leaving the GUI pressed for good. */
TEST_CASE("a controller that stops answering mid-touch ends the touch",
          "[touch_ao]")
{
    touch_sampling();

    sample(true, 120, 130);
    TEST_ASSERT_NOT_NULL(published());

    s_bus_dead = true;
    sample(true, 120, 130);

    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_FALSE(e->pressed);
    TEST_ASSERT_EQUAL_INT16(120, e->x);

    s_bus_dead = false;
    mock_down();
}

/* ---- A dry event pool ------------------------------------------------------ */

/** @brief More blocks than the host harness's small pool holds. */
#define POOL_HOLD_MAX 64U

/**
 * @brief Take every block a TouchEvt would come from, so the next one fails.
 * @param[out] held Where the blocks are kept until pool_give_back().
 * @return How many were taken.
 */
static size_t pool_take_all(const QEvt *held[POOL_HOLD_MAX])
{
    size_t n = 0U;

    while ( POOL_HOLD_MAX > n )
    {
        TouchEvt * const e = Q_NEW_X(TouchEvt, 0U, TOUCH_EVT_SIG);
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

/* A release the pool could not carry used to be dropped while the AO went
 * back to waiting for the interrupt: nothing would ever say it again, and
 * downstream the finger stayed down - no auto-lock, Kody kept awake, the next
 * tap read as a drag. Now the AO keeps polling until the release goes out. */
TEST_CASE("a release the pool could not carry is said at the next poll",
          "[touch_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];

    touch_sampling();
    sample(true, 300, 400);
    TEST_ASSERT_NOT_NULL(published());

    size_t const n = pool_take_all(held);
    sample(false, 0, 0);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    pool_give_back(held, n);

    sample(false, 0, 0);
    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the lost release was never said again");
    TEST_ASSERT_FALSE(e->pressed);
    TEST_ASSERT_EQUAL_INT16(300, e->x);
    TEST_ASSERT_EQUAL_INT16(400, e->y);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_tick(AO_Touch, 10U),
                                   "released: the poll is disarmed");
    mock_down();
}

/* The same for a press: a finger resting still is published once, so a press
 * the pool dropped was never said at all. */
TEST_CASE("a press the pool could not carry is said at the next poll",
          "[touch_ao]")
{
    const QEvt *held[POOL_HOLD_MAX];

    touch_sampling();
    size_t const n = pool_take_all(held);
    sample(true, 50, 60);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    pool_give_back(held, n);

    sample(true, 50, 60);
    const TouchEvt * const e = published();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the lost press was never said again");
    TEST_ASSERT_TRUE(e->pressed);
    TEST_ASSERT_EQUAL_INT16(50, e->x);
    TEST_ASSERT_EQUAL_INT16(60, e->y);

    sample(false, 0, 0);
    TEST_ASSERT_NOT_NULL(published());
    mock_down();
}

/* ---- The drag that was lost ------------------------------------------------ */

/* THE case. Twelve samples, each two pixels along - every single one at or
 * below the epsilon that separates jitter from motion. Measured against the
 * last SAMPLE, not one of them moves and the AO says nothing for the whole
 * gesture; measured against the last PUBLISHED point they accumulate, and the
 * finger is followed. */
TEST_CASE("a slow drag is followed, not swallowed", "[touch_ao]")
{
    unsigned moves = 0U;

    touch_sampling();

    sample(true, 100, 100);
    TEST_ASSERT_NOT_NULL_MESSAGE(published(), "the press itself");

    for ( int16_t i = 1; 12 >= i; ++i )
    {
        sample(true, (int16_t)(100 + (2 * i)), 100);
    }

    for ( ;; )
    {
        const TouchEvt * const e = published();
        if ( NULL == e )
        {
            break;
        }
        ++moves;
        TEST_ASSERT_TRUE(e->pressed);
    }

    TEST_ASSERT_GREATER_THAN_UINT_MESSAGE(0U, moves,
        "every step was inside epsilon of the one before it, so measuring "
        "against the last SAMPLE loses the entire gesture");

    sample(false, 124, 100);
    ao_spy_drain();
    mock_down();
}

/* The other half of the same property: a genuine jump is published, and a
 * jump backwards counts as much as a jump forwards. */
TEST_CASE("movement is measured in both directions on both axes",
          "[touch_ao]")
{
    touch_sampling();

    sample(true, 200, 200);
    TEST_ASSERT_NOT_NULL(published());

    sample(true, 150, 200);          /* left */
    TEST_ASSERT_NOT_NULL_MESSAGE(published(), "a finger moving left is moving");

    sample(true, 150, 150);          /* up */
    TEST_ASSERT_NOT_NULL_MESSAGE(published(), "so is one moving up");

    sample(false, 150, 150);
    ao_spy_drain();
    mock_down();
}

/* ---- Fields ---------------------------------------------------------------- */

/* The console's `tp`: the name is resolved before it gets here, the driver's
 * verdict travels back to whoever asked, and a write answers with what the
 * chip kept rather than what was sent. */
TEST_CASE("a field is read, written and dumped for whoever asks, and the answer is posted back",
          "[touch_ao]")
{
    touch_armed();

    s_regs[REG_AUTO_SLEEP] = 2U;
    const TouchFieldEvt *a = field(FIELD_OP_GET, (uint16_t)KDRV_CST_AUTO_SLEEP_TIME, 0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_GET, a->op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_CST_AUTO_SLEEP_TIME, a->field);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(2, a->value);

    a = field(FIELD_OP_SET, (uint16_t)KDRV_CST_AUTO_SLEEP_TIME, 5);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(5, a->value);
    TEST_ASSERT_EQUAL_UINT8(5U, s_regs[REG_AUTO_SLEEP]);

    a = field(FIELD_OP_SET, (uint16_t)KDRV_CST_CHIP_ID, 1);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, a->rc);

    s_bus_dead = true;
    a = field(FIELD_OP_GET, (uint16_t)KDRV_CST_AUTO_SLEEP_TIME, 0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_ERROR, a->rc);
    s_bus_dead = false;

    a = field(FIELD_OP_DUMP, 0U, 0);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);

    /* A NULL asker is served and not answered. */
    touch_field_request(NULL, FIELD_OP_SET, (uint16_t)KDRV_CST_AUTO_SLEEP_TIME, 7);
    serve();
    TEST_ASSERT_EQUAL_UINT8(7U, s_regs[REG_AUTO_SLEEP]);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    mock_down();
}

/* ---- The guided test ------------------------------------------------------- */

/* The script: two points inside the clear band of the screen protector.
 * Repeated here rather than imported. */
#define TP_TOTAL 2U
static const int16_t TARGET_X[TP_TOTAL] = { 80, 335 };
static const int16_t TARGET_Y[TP_TOTAL] = { 215, 285 };
#define MISS_DX 80   /**< Far enough off a target to be a miss. */

/** @brief The next event of @p sig in the spy, skipping the touch samples the
 *  taps also publish; NULL when the spy runs dry. */
static const QEvt *take_sig(enum_t sig)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            return NULL;
        }
        if ( e->sig == sig )
        {
            return e;
        }
    }
}

/** @brief One tap, as the finger does it: down, two samples, up. */
static void tap(int16_t raw_x, int16_t raw_y)
{
    p6_moves(true);
    sample(true, raw_x, raw_y);
    sample(true, raw_x, raw_y);
    sample(false, raw_x, raw_y);
}

/** @brief Expect the prompt for step @p n. */
static void expect_prompt(uint8_t n)
{
    const TouchTestEvt * const t = (const TouchTestEvt *)take_sig(TOUCH_TEST_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "a prompt");
    TEST_ASSERT_EQUAL_UINT8(TP_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(n, t->step);
    TEST_ASSERT_EQUAL_UINT8(TP_TOTAL, t->total);
    TEST_ASSERT_EQUAL_INT16(TARGET_X[n - 1U], t->tx);
    TEST_ASSERT_EQUAL_INT16(TARGET_Y[n - 1U], t->ty);
}

/** @brief Expect the outcome @p phase for step @p n, read at (gx, gy). */
static void expect_outcome(uint8_t phase, uint8_t n, int16_t gx, int16_t gy)
{
    const TouchTestEvt * const t = (const TouchTestEvt *)take_sig(TOUCH_TEST_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "an outcome");
    TEST_ASSERT_EQUAL_UINT8(phase, t->phase);
    TEST_ASSERT_EQUAL_UINT8(n, t->step);
    TEST_ASSERT_EQUAL_INT16(gx, t->gx);
    TEST_ASSERT_EQUAL_INT16(gy, t->gy);
}

/** @brief Expect the run to close with the verdict @p ok. */
static void expect_done(bool ok)
{
    const TouchTestEvt * const t = (const TouchTestEvt *)take_sig(TOUCH_TEST_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "the last step is over");
    TEST_ASSERT_EQUAL_UINT8(TP_TEST_DONE, t->phase);

    const TestResultEvt * const v = (const TestResultEvt *)take_sig(TEST_RESULT_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "the verdict");
    TEST_ASSERT_EQUAL_STRING("tp", v->name);
    TEST_ASSERT_EQUAL(ok, v->ok);
}

/* Two targets, two taps that read ten pixels left and four below where they
 * should - a sensor that sits a little off the glass. Both are within
 * tolerance, so the run passes, and nothing is written to the store. */
TEST_CASE("test_two_targets", "[touch_ao]")
{
    touch_armed();

    touch_test_request();
    serve();
    TEST_ASSERT_TRUE_MESSAGE(logged("chip_id=0xB7"), "the identity is checked first");
    expect_prompt(1U);

    for ( uint8_t n = 1U; TP_TOTAL >= n; ++n )
    {
        int16_t const rx = (int16_t)(TARGET_X[n - 1U] - 10);
        int16_t const ry = (int16_t)(TARGET_Y[n - 1U] + 4);
        tap(rx, ry);
        expect_outcome(TP_TEST_PASS, n, rx, ry);
        if ( TP_TOTAL > n )
        {
            expect_prompt((uint8_t)(n + 1U));
        }
    }
    expect_done(true);
    ao_spy_drain();
    mock_down();
}

/* A test that passed leaves nothing behind in the store: it proves the panel
 * answers, it does not calibrate it. */
TEST_CASE("test_no_calibration_written", "[touch_ao]")
{
    touch_armed();

    TEST_ASSERT_EQUAL_UINT(0U, fake_nvs_count());
    touch_test_request();
    serve();
    expect_prompt(1U);
    for ( uint8_t n = 1U; TP_TOTAL >= n; ++n )
    {
        tap(TARGET_X[n - 1U], TARGET_Y[n - 1U]);
        expect_outcome(TP_TEST_PASS, n, TARGET_X[n - 1U], TARGET_Y[n - 1U]);
        if ( TP_TOTAL > n )
        {
            expect_prompt((uint8_t)(n + 1U));
        }
    }
    expect_done(true);
    TEST_ASSERT_EQUAL_UINT(0U, fake_nvs_count());

    /* The next sample is published as the controller read it. */
    p6_moves(true);
    sample(true, 100, 100);
    const TouchEvt * const e = (const TouchEvt *)take_sig(TOUCH_EVT_SIG);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT16(100, e->x);
    TEST_ASSERT_EQUAL_INT16(100, e->y);
    sample(false, 100, 100);
    ao_spy_drain();
    mock_down();
}

/* A tap off the target is a slip: it is told as a miss, the same target is
 * asked again, and the run goes on from there. */
TEST_CASE("test_miss_twice_then_hit", "[touch_ao]")
{
    touch_armed();

    touch_test_request();
    serve();
    expect_prompt(1U);

    for ( unsigned k = 0U; 2U > k; ++k )
    {
        tap((int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
        expect_outcome(TP_TEST_MISS, 1U, (int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
        expect_prompt(1U);
        ao_spy_drain();   /* the spy holds every event it handed out; the pool is small */
    }
    tap(TARGET_X[0], TARGET_Y[0]);
    expect_outcome(TP_TEST_PASS, 1U, TARGET_X[0], TARGET_Y[0]);
    expect_prompt(2U);

    tap(TARGET_X[1], TARGET_Y[1]);
    expect_outcome(TP_TEST_PASS, 2U, TARGET_X[1], TARGET_Y[1]);
    expect_done(true);
    ao_spy_drain();
    mock_down();
}

/* The slips are counted per step: a step that was missed once and then hit
 * leaves the next one with all of its tries. */
TEST_CASE("test_miss_count_restarts_on_each_step", "[touch_ao]")
{
    touch_armed();

    touch_test_request();
    serve();
    expect_prompt(1U);

    tap((int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
    expect_outcome(TP_TEST_MISS, 1U, (int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
    expect_prompt(1U);
    tap(TARGET_X[0], TARGET_Y[0]);
    expect_outcome(TP_TEST_PASS, 1U, TARGET_X[0], TARGET_Y[0]);
    expect_prompt(2U);
    ao_spy_drain();

    for ( unsigned k = 0U; 2U > k; ++k )
    {
        tap((int16_t)(TARGET_X[1] - MISS_DX), TARGET_Y[1]);
        expect_outcome(TP_TEST_MISS, 2U, (int16_t)(TARGET_X[1] - MISS_DX), TARGET_Y[1]);
        expect_prompt(2U);
        ao_spy_drain();
    }
    tap(TARGET_X[1], TARGET_Y[1]);
    expect_outcome(TP_TEST_PASS, 2U, TARGET_X[1], TARGET_Y[1]);
    expect_done(true);
    ao_spy_drain();
    mock_down();
}

/* The third tap off the same target fails the step. The run still asks for
 * the other target so the log carries the whole picture, and the verdict is
 * a fail. */
TEST_CASE("test_third_miss_fails", "[touch_ao]")
{
    touch_armed();

    touch_test_request();
    serve();
    expect_prompt(1U);

    for ( unsigned k = 0U; 2U > k; ++k )
    {
        tap((int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
        expect_outcome(TP_TEST_MISS, 1U, (int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
        expect_prompt(1U);
        ao_spy_drain();
    }
    tap((int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
    expect_outcome(TP_TEST_FAIL, 1U, (int16_t)(TARGET_X[0] + MISS_DX), TARGET_Y[0]);
    expect_prompt(2U);

    tap(TARGET_X[1], TARGET_Y[1]);
    expect_outcome(TP_TEST_PASS, 2U, TARGET_X[1], TARGET_Y[1]);
    expect_done(false);
    TEST_ASSERT_EQUAL_UINT(0U, fake_nvs_count());
    ao_spy_drain();
    mock_down();
}

/* A client other than the autotest asks for the same script and hears every
 * step tagged with its own number. */
TEST_CASE("test_request_from_carries_origin", "[touch_ao]")
{
    touch_armed();

    touch_test_request_from(FOREIGN_ORIGIN);
    serve();
    const TouchTestEvt * const t = (const TouchTestEvt *)take_sig(TOUCH_TEST_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "a request from a client is answered with the first prompt");
    TEST_ASSERT_EQUAL_UINT8(TP_TEST_PROMPT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(FOREIGN_ORIGIN, t->origin);

    /* Finish the run by timeouts so the next case starts clean. */
    for ( uint8_t n = 1U; TP_TOTAL >= n; ++n )
    {
        (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_TEST_STEP_MS) + 1U);
    }
    ao_spy_drain();
    mock_down();
}

/* A client whose own run ended (AO_TestDot's abort) cancels the test it
 * asked for: no prompt and no verdict follow. Another client's cancel leaves
 * a run alone. */
TEST_CASE("test_touch_cancel_ends_the_clients_run_with_no_verdict", "[touch_ao]")
{
    touch_armed();

    touch_test_request_from(FOREIGN_ORIGIN);
    serve();
    ao_spy_drain();

    touch_test_cancel_request((uint8_t)(FOREIGN_ORIGIN + 1U));
    serve();
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_TEST_STEP_MS) + 1U);
    TEST_ASSERT_NOT_NULL_MESSAGE(take_sig(TOUCH_TEST_SIG), "another client's cancel leaves the run going");
    ao_spy_drain();

    touch_test_cancel_request(FOREIGN_ORIGIN);
    serve();
    ao_spy_drain();
    for ( uint8_t n = 1U; TP_TOTAL >= n; ++n )
    {
        (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_TEST_STEP_MS) + 1U);
    }
    unsigned heard = 0U;
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (TOUCH_TEST_SIG == e->sig) || (TEST_RESULT_SIG == e->sig) )
        {
            heard++;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard, "no prompt and no verdict after the cancel");
    mock_down();
}

/* A client that asks while another run is up is told no, in its own number;
 * the running run keeps the origin it started with. */
TEST_CASE("test_touch_request_during_a_run_gets_a_refusal", "[touch_ao]")
{
    touch_armed();

    touch_test_request();                    /* the autotest's run is up */
    serve();
    expect_prompt(1U);

    fake_klog_reset();
    touch_test_request_from(FOREIGN_ORIGIN);
    serve();
    const TestResultEvt * const v = (const TestResultEvt *)take_sig(TEST_RESULT_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(v, "a request during a run is answered with a verdict");
    TEST_ASSERT_EQUAL_STRING("tp", v->name);
    TEST_ASSERT_FALSE_MESSAGE(v->ok, "the refusal is a failure, never a pass");
    TEST_ASSERT_EQUAL_UINT8(FOREIGN_ORIGIN, v->origin);
    TEST_ASSERT_TRUE_MESSAGE(logged("busy with a run from origin 0"), "the log says why");
    ao_spy_drain();

    /* The run that was up goes on, still the autotest's. */
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_TEST_STEP_MS) + 1U);
    const TouchTestEvt * const t = (const TouchTestEvt *)take_sig(TOUCH_TEST_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "the running run's first step times out");
    TEST_ASSERT_EQUAL_UINT8(TP_TEST_TIMEOUT, t->phase);
    TEST_ASSERT_EQUAL_UINT8(TEST_ORIGIN_AUTOTEST, t->origin);
    ao_spy_drain();

    for ( uint8_t n = 2U; TP_TOTAL >= n; ++n )
    {
        (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_TEST_STEP_MS) + 1U);
    }
    ao_spy_drain();
    mock_down();
}

/* The console's budget for the row has to outlast the worst run the rules
 * allow, worked out here from the rules' own numbers and not from the
 * macro's expression: two targets; on each, three attempts - two forgiven
 * misses and the final tap - and every attempt may take a whole 20 s step;
 * and five seconds of slack. */
TEST_CASE("test_touch_budget_covers_worst_case", "[touch_ao]")
{
    uint32_t const step_ms    = 20000U;
    uint32_t const attempts   = 2U + 1U;
    uint32_t const worst_step = attempts * step_ms;

    TEST_ASSERT_EQUAL_UINT32(2U, KODE_TOUCH_TEST_STEPS);
    TEST_ASSERT_EQUAL_UINT32(attempts, KODE_TOUCH_TEST_TRIES);
    TEST_ASSERT_EQUAL_UINT32(step_ms, KODE_TOUCH_TEST_STEP_MS);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32((2U * worst_step) + 5000U, KODE_TOUCH_TEST_BUDGET_MS);
}

/* Nobody taps: the step times out after KODE_TOUCH_TEST_STEP_MS and the run
 * moves on to the next target rather than waiting for ever. A second request
 * while a run is up is ignored, because two scripts on one glass would each
 * be judging the other's taps. */
TEST_CASE("a step nobody taps times out and the run moves on; a second request is ignored",
          "[touch_ao]")
{
    touch_armed();

    touch_test_request();
    serve();
    expect_prompt(1U);

    touch_test_request();
    serve();
    TEST_ASSERT_NULL_MESSAGE(take_sig(TOUCH_TEST_SIG), "no second prompt");

    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_TEST_STEP_MS) + 1U);
    expect_outcome(TP_TEST_TIMEOUT, 1U, 0, 0);
    expect_prompt(2U);

    for ( uint8_t n = 2U; TP_TOTAL >= n; ++n )
    {
        tap(TARGET_X[n - 1U], TARGET_Y[n - 1U]);
        expect_outcome(TP_TEST_PASS, n, TARGET_X[n - 1U], TARGET_Y[n - 1U]);
        if ( TP_TOTAL > n )
        {
            expect_prompt((uint8_t)(n + 1U));
        }
    }
    expect_done(false);
    TEST_ASSERT_EQUAL_UINT(0U, fake_nvs_count());
    ao_spy_drain();
    mock_down();
}

/* ---- The rest -------------------------------------------------------------- */

#if !CONFIG_KODE_LOWPOWER
#error "test_touch_ao.c needs CONFIG_KODE_LOWPOWER: main/CMakeLists.txt spells it out"
#endif

static uint32_t s_round_no;

/** @brief AO_LowPower opens a round for @p target. */
static void round_for(kode_lp_state_t target)
{
    LowPowerRoundEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(LOWPOWER_ROUND_SIG);
    e.target = (uint8_t)target;
    e.round  = ++s_round_no;
    QASM_DISPATCH(AO_Touch, &e.super, 0U);
}

/** @brief AO_LowPower says the board is AWAKE (again). */
static void state_awake(void)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_AWAKE;
    e.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    e.prev  = (uint8_t)KODE_LP_ASLEEP;
    QASM_DISPATCH(AO_Touch, &e.super, 0U);
}

/** @brief The next answer must be for the open round, with this verdict. */
static void expect_answer(bool busy, const char *why)
{
    LowPowerReadyEvt a;

    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_ready_take(&a), "the round was not answered");
    TEST_ASSERT_EQUAL_UINT32(s_round_no, a.round);
    TEST_ASSERT_EQUAL_INT(busy, a.busy);
    if ( NULL != why )
    {
        TEST_ASSERT_EQUAL_STRING(why, a.why);
    }
}

static void expect_no_more_answers(void)
{
    LowPowerReadyEvt a;

    TEST_ASSERT_FALSE_MESSAGE(fake_lowpower_ready_take(&a), "a second answer for one round");
}

/** @brief The three registers the rest changes hold the awake settings. */
static void expect_awake_settings(void)
{
    TEST_ASSERT_EQUAL_HEX8(AWAKE_IRQ_CTL, s_regs[REG_IRQ_CTL]);
    TEST_ASSERT_EQUAL_HEX8(AWAKE_DIS_SLEEP, s_regs[REG_DIS_SLEEP]);
    TEST_ASSERT_EQUAL_HEX8(AWAKE_SLEEP_S, s_regs[REG_AUTO_SLEEP]);
}

/* The rest: the interrupt off, auto-sleep on and quick. A second round (OFF
 * after ASLEEP) finds the chip already there and writes nothing. */
TEST_CASE("a round puts the controller to sleep: no interrupt, auto-sleep on",
          "[touch_ao][lowpower]")
{
    touch_armed();

    round_for(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, s_regs[REG_IRQ_CTL], "INT still pulses");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, s_regs[REG_DIS_SLEEP], "auto-sleep still disabled");
    TEST_ASSERT_EQUAL_HEX8(KODE_TOUCH_REST_SLEEP_S, s_regs[REG_AUTO_SLEEP]);
    expect_answer(false, NULL);
    expect_no_more_answers();

    s_regs[REG_IRQ_CTL] = 0x55U;    /* anything a second rest would overwrite */
    round_for(KODE_LP_OFF);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x55U, s_regs[REG_IRQ_CTL], "OFF rewrote a rest already made");
    expect_answer(false, NULL);

    s_regs[REG_IRQ_CTL] = 0U;
    state_awake();
    expect_awake_settings();
    mock_down();
}

/* A chip that answers is put back at once, and an AWAKE that repeats writes
 * nothing more. */
TEST_CASE("AWAKE puts the awake settings back, once", "[touch_ao][lowpower]")
{
    touch_armed();
    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);

    state_awake();
    expect_awake_settings();

    s_regs[REG_IRQ_CTL] = 0x55U;
    state_awake();
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x55U, s_regs[REG_IRQ_CTL], "a second AWAKE wrote again");
    s_regs[REG_IRQ_CTL] = AWAKE_IRQ_CTL;
    mock_down();
}

/* The chip is asleep and NAKs: the round still answers (that IS the rest),
 * and the wake is retried on a short timer until a finger lets it through. */
TEST_CASE("a controller that NAKs the wake is tried again until it answers",
          "[touch_ao][lowpower]")
{
    touch_armed();

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    s_bus_dead = true;      /* the chip has gone to sleep, as the rest meant it to */

    s_bus_tries = 0U;
    state_awake();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_bus_tries, "the wake was not tried at once");
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_RESTORE_RETRY_MS) + 1U);
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_RESTORE_RETRY_MS) + 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_bus_tries, "one more try per retry period");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, s_regs[REG_IRQ_CTL], "restored through a dead bus");

    s_bus_dead = false;
    s_regs[REG_IRQ_CTL] = 0U;
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_RESTORE_RETRY_MS) + 1U);
    expect_awake_settings();

    /* Done: the timer is not armed any more. */
    s_regs[REG_IRQ_CTL] = 0x55U;
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_RESTORE_RETRY_MS) * 3U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x55U, s_regs[REG_IRQ_CTL], "the retry went on after it answered");
    s_regs[REG_IRQ_CTL] = AWAKE_IRQ_CTL;
    mock_down();
}

/* A round that finds a wake still being retried cancels it: the chip is in
 * its rest settings, which is where the round wants it. */
TEST_CASE("a round during a retried wake cancels the retry", "[touch_ao][lowpower]")
{
    touch_armed();

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    s_bus_dead = true;
    state_awake();

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    s_bus_dead = false;
    s_regs[REG_IRQ_CTL] = 0U;
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_RESTORE_RETRY_MS) * 3U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0U, s_regs[REG_IRQ_CTL], "a wake went on while the board slept");

    state_awake();
    expect_awake_settings();
    mock_down();
}

/* A finger on the glass is input, not a moment to rest. */
TEST_CASE("a finger down refuses the round", "[touch_ao][lowpower]")
{
    touch_sampling();

    round_for(KODE_LP_ASLEEP);
    expect_answer(true, "finger down");
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(AWAKE_IRQ_CTL, s_regs[REG_IRQ_CTL], "rested under a finger");

    for ( unsigned i = 0U; 4U > i; ++i )
    {
        sample(false, 0, 0);
    }
    mock_down();
}

/* ---- A contact that does not move ---------------------------------------- */

/** @brief Polls in KODE_TOUCH_STILL_MS. */
#define STILL_POLLS  (KODE_TOUCH_STILL_MS / KODE_TOUCH_POLL_MS)

/**
 * @brief Hold a contact around (@p x, @p y) for @p n polls, wobbling by
 *        @p wobble pixels; count what went out.
 * @return How many samples were published, the first press included.
 */
static unsigned hold_for(uint32_t n, int16_t x, int16_t y, int16_t wobble)
{
    unsigned out = 0U;

    for ( uint32_t i = 0U; n > i; ++i )
    {
        int16_t const d = ((i % 2U) == 0U) ? wobble : (int16_t)-wobble;

        sample(true, (int16_t)(x + d), (int16_t)(y - d));
        while ( NULL != published() )
        {
            ++out;
        }
        ao_spy_drain();   /* a taken event holds its block until the drain */
    }
    return out;
}

TEST_CASE("a contact still for a minute is released, stale, and refuses no round",
          "[touch_ao][still]")
{
    touch_sampling();
    TEST_ASSERT_EQUAL_UINT(1U, hold_for(STILL_POLLS - 2U, 200, 250, 0));
    round_for(KODE_LP_ASLEEP);
    expect_answer(true, "finger down");

    sample(true, 200, 250);
    sample(true, 200, 250);
    const TouchEvt * const t = published();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "still after a minute, and still a finger");
    TEST_ASSERT_FALSE(t->pressed);
    TEST_ASSERT_TRUE(t->stale);
    TEST_ASSERT_TRUE(logged("is no longer input"));

    /* Still there, and nobody hears of it again. */
    TEST_ASSERT_EQUAL_UINT(0U, hold_for(30U, 200, 250, 0));
    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    state_awake();
    mock_down();
}

/* The same holds once a second: a contact that stopped counting is looked at
 * by one poll at a time, however long AO_Touch goes without running. */
TEST_CASE("a held contact's look nobody took is not followed by another",
          "[touch_ao][still]")
{
    touch_sampling();
    TEST_ASSERT_EQUAL_UINT(2U, hold_for(STILL_POLLS, 200, 250, 0));

    /* Two of its periods go by and AO_Touch never runs. */
    for ( uint32_t i = 0U; ((2U * KQP_TICKS_PER_MS(KODE_TOUCH_HELD_POLL_MS)) + 1U) > i; ++i )
    {
        QTIMEEVT_TICK_X(0U, (void *)0);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, (unsigned)uxQueueMessagesWaiting(AO_Touch->eQueue),
                                   "every period queued a look the AO had not taken");

    serve();
    sample(false, 200, 250);
    ao_spy_drain();
    mock_down();
}

TEST_CASE("a wobble the size of a cloth's is still the same still contact",
          "[touch_ao][still]")
{
    touch_sampling();
    (void)hold_for(STILL_POLLS + 2U, 200, 250, 5);
    TEST_ASSERT_TRUE(logged("is no longer input"));
    for ( unsigned i = 0U; 4U > i; ++i )
    {
        sample(false, 0, 0);
    }
    mock_down();
}

TEST_CASE("a finger that keeps moving is never cut", "[touch_ao][still]")
{
    touch_sampling();
    for ( uint32_t i = 0U; (2U * STILL_POLLS) > i; ++i )
    {
        /* A slow drag, back and forth across the glass. */
        int16_t const x = (int16_t)(50 + (int16_t)((i / 4U) % 300U));

        sample(true, x, 250);
        ao_spy_drain();
    }
    TEST_ASSERT_FALSE(logged("is no longer input"));
    ao_spy_drain();
    sample(false, 0, 0);
    const TouchEvt * const t = published();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FALSE(t->stale);
    mock_down();
}

TEST_CASE("a contact that stopped counting is a finger again once it moves off, "
          "and waits for an edge once it lifts", "[touch_ao][still]")
{
    touch_sampling();
    (void)hold_for(STILL_POLLS + 2U, 200, 250, 0);
    TEST_ASSERT_TRUE(logged("is no longer input"));
    ao_spy_drain();

    sample(true, 260, 250);
    const TouchEvt * t = published();
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "moved off and not said");
    TEST_ASSERT_TRUE(t->pressed);
    TEST_ASSERT_FALSE(t->stale);
    sample(false, 0, 0);
    t = published();
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FALSE(t->pressed);

    /* Held again, then lifted: back to waiting, nothing said. */
    p6_moves(true);
    (void)hold_for(STILL_POLLS + 2U, 100, 100, 0);
    ao_spy_drain();
    sample(false, 0, 0);
    TEST_ASSERT_NULL(published());
    s_reads = 0U;
    sample(false, 0, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_reads, "polling after the lift");
    mock_down();
}

TEST_CASE("the guided test is never cut by a still contact", "[touch_ao][still]")
{
    touch_armed();
    touch_test_request();
    serve();
    p6_moves(true);
    ao_spy_drain();
    (void)hold_for(STILL_POLLS + 2U, 200, 250, 0);
    TEST_ASSERT_FALSE(logged("is no longer input"));
    touch_test_cancel_request(TEST_ORIGIN_AUTOTEST);
    serve();
    for ( unsigned i = 0U; 4U > i; ++i )
    {
        sample(false, 0, 0);
    }
    mock_down();
}

/* The guided test owns the glass for its length. */
TEST_CASE("the guided test refuses the round", "[touch_ao][lowpower]")
{
    touch_armed();
    touch_test_request();
    serve();
    ao_spy_drain();

    round_for(KODE_LP_ASLEEP);
    expect_answer(true, "touch test");
    TEST_ASSERT_EQUAL_HEX8(AWAKE_IRQ_CTL, s_regs[REG_IRQ_CTL]);

    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_TEST_STEP_MS) * 6U);
    ao_spy_drain();
    mock_down();
}

/** @brief AO_LowPower says the radio alone is awake: the glass is not. */
static void state_radio(void)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_RADIO;
    e.cause = (uint8_t)KODE_LP_CAUSE_TIMER;
    e.prev  = (uint8_t)KODE_LP_ASLEEP;
    QASM_DISPATCH(AO_Touch, &e.super, 0U);
}

/* A radio wake leaves the controller in its rest: nothing is written, no
 * read is tried, and the AWAKE that follows still puts it back. */
TEST_CASE("STATE(RADIO) leaves a rested controller alone; AWAKE still puts it back",
          "[touch_ao][lowpower]")
{
    touch_armed();
    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);

    s_regs[REG_IRQ_CTL] = 0x55U;    /* anything a restore would overwrite */
    s_bus_tries = 0U;
    ao_spy_drain();
    state_radio();
    (void)qf_support_tick(AO_Touch, QF_SUPPORT_STEPS(KODE_TOUCH_RESTORE_RETRY_MS) * 3U);
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x55U, s_regs[REG_IRQ_CTL], "the radio wake restored the chip");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_bus_tries, "the radio wake touched the bus");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the radio wake published something");

    state_awake();
    expect_awake_settings();
    mock_down();
}
