/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_tca6408a_ao.c
 * @brief AO_IoExp: one chip, one owner, and an interrupt that becomes events.
 */
/* Named after the chip to sort after test_kdrv_tca6408a.c, whose first case needs it down. */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp.h"
#include "kbsp_owners.h"
#include "kdrv_tca6408a.h"
#include "kqp.h"
#include "lowpower_cases.h"
#include "owners_support.h"
#include "qf_support.h"
#include "sdkconfig.h"

#define REG_INPUT     0x00U
#define REG_OUTPUT    0x01U
#define REG_POLARITY  0x02U
#define REG_CONFIG    0x03U

static const QEvt INT_EVT = QEVT_INITIALIZER(IOEXP_INT_SIG);

/* ---- The chip, as a register file ---------------------------------------- */

static uint8_t  s_regs[4];
static bool     s_bus_dead;
static unsigned s_reads;
static unsigned s_writes;

static kode_status_t on_read(khal_i2c_dev_t dev, uint8_t reg, uint8_t *buf,
                             size_t len, int ncalls)
{
    (void)dev; (void)ncalls;
    if ( s_bus_dead || (REG_CONFIG < reg) || (1U != len) )
    {
        return KODE_ERROR;
    }
    buf[0] = s_regs[reg];
    ++s_reads;
    return KODE_OK;
}

static kode_status_t on_write(khal_i2c_dev_t dev, uint8_t reg, uint8_t val, int ncalls)
{
    (void)dev; (void)ncalls;
    if ( s_bus_dead || (REG_CONFIG < reg) )
    {
        return KODE_ERROR;
    }
    s_regs[reg] = val;
    ++s_writes;
    return KODE_OK;
}

/** @brief What the driver was asked to call from GPIO6's interrupt. */
static khal_gpio_cb_t s_int_cb;
static void          *s_int_arg;

static kode_status_t capture_attach(uint8_t pin, khal_gpio_irq_t kind,
                                    khal_gpio_cb_t cb, void *arg, int ncalls)
{
    (void)ncalls;
    TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_IOEXP_INT, pin);
    TEST_ASSERT_EQUAL_INT(KHAL_GPIO_IRQ_LOW_LEVEL, kind);
    s_int_cb  = cb;
    s_int_arg = arg;
    return KODE_OK;
}

/**
 * @brief Bring AO_IoExp up, once for the whole binary.
 *
 * The chip answers with P6 and P7 configured as outputs - P7 is the panel's
 * and must be left alone, P6 is the one thing a previous firmware could have
 * left that the owner must undo - and every pin reading high. The boot
 * publish is left in the spy for the first case.
 */
static void ioexp_up_once(void)
{
    static bool s_started;

    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_IgnoreAndReturn(true);
    khal_i2c_add_IgnoreAndReturn(KODE_OK);
    khal_i2c_read_reg_StubWithCallback(&on_read);
    khal_i2c_write_reg_StubWithCallback(&on_write);
    Mockkhal_gpio_Init();

    qf_support_init();
    (void)ao_spy();
    ao_spy_subscribe(IOEXP_INPUT_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);

    s_bus_dead = false;
    s_reads    = 0U;
    s_writes   = 0U;

    if ( !s_started )
    {
        ao_spy_drain();
        s_regs[REG_INPUT]    = 0xFFU;
        s_regs[REG_OUTPUT]   = 0xFFU;
        s_regs[REG_POLARITY] = 0x00U;
        s_regs[REG_CONFIG]   = 0x3FU;
        khal_gpio_input_ExpectAndReturn(KBSP_PIN_IOEXP_INT, KHAL_GPIO_PULL_NONE, KODE_OK);
        khal_gpio_irq_attach_StubWithCallback(&capture_attach);

        IoExp_ctor();
        qf_support_give_queue(AO_IoExp);
        AO_IoExp->prio  = KODE_PRIO_IOEXP;
        AO_IoExp->pthre = KODE_PRIO_IOEXP;
        QActive_register_(AO_IoExp);
        QASM_INIT(AO_IoExp, (void *)0, 0U);
        s_started = true;
    }
    fake_klog_reset();
}

/** @brief Verify both mocks and put them away. */
static void mocks_down(void)
{
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Bring-up ------------------------------------------------------------ */

/* First in the file: the boot publish is still in the spy. */
TEST_CASE("AO_IoExp owns the chip, puts a stray output back to an input, leaves "
          "P7 alone and publishes the first reading", "[ioexp_ao]")
{
    ioexp_up_once();

    TEST_ASSERT_EQUAL_STRING("AO_IoExp", kbsp_owner_name(KBSP_RES_IOEXP));
    TEST_ASSERT_NOT_NULL(s_int_cb);

    /* C6 back to 1, C7 untouched: 0x3F -> 0x7F, in exactly one write. */
    TEST_ASSERT_EQUAL_HEX8(0x7FU, s_regs[REG_CONFIG]);
    TEST_ASSERT_EQUAL_UINT(1U, s_writes);

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const IoexpInputEvt *in = (const IoexpInputEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_INT(IOEXP_INPUT_SIG, in->super.sig);
    TEST_ASSERT_TRUE(in->ok);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, in->levels);
    TEST_ASSERT_EQUAL_HEX8(0x00U, in->changed);
    ao_spy_drain();

    mocks_down();
}

/* ---- The interrupt ------------------------------------------------------- */

/* The callback the driver was handed runs in the ISR and does one thing:
 * makes an event of the edge. The AO then reads the register (which is what
 * releases the line), says what changed, and re-arms. */
TEST_CASE("the interrupt callback becomes a read, a publish of what changed, and a re-arm",
          "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    s_regs[REG_INPUT] = 0xFBU;                      /* P2 went low */
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    s_int_cb(s_int_arg);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_IoExp, 4U));

    TEST_ASSERT_EQUAL_UINT(1U, s_reads);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const IoexpInputEvt *in = (const IoexpInputEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_INT(IOEXP_INPUT_SIG, in->super.sig);
    TEST_ASSERT_TRUE(in->ok);
    TEST_ASSERT_EQUAL_HEX8(0xFBU, in->levels);
    TEST_ASSERT_EQUAL_HEX8(0x04U, in->changed);
    ao_spy_drain();

    /* And back: the change is measured against the last reading. */
    s_regs[REG_INPUT] = 0xFFU;
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    QASM_DISPATCH(AO_IoExp, &INT_EVT, 0U);
    in = (const IoexpInputEvt *)ao_spy_take();
    TEST_ASSERT_EQUAL_HEX8(0x04U, in->changed);
    ao_spy_drain();

    mocks_down();
}

/* A failed read is not a reading: nothing may decode "the bus died" as
 * "every button released". And the line is NOT re-armed on it: the read is
 * what releases INT, so after a failed one the line is still low, and a
 * low-level interrupt re-armed on a low line fires again at once - a storm
 * at this AO's priority, each round holding the shared bus for a whole I2C
 * timeout. A one-shot looks again instead. */
TEST_CASE("a read the chip refused is published as not ok, the line is not re-armed, "
          "and a one-shot looks again", "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    s_bus_dead = true;
    QASM_DISPATCH(AO_IoExp, &INT_EVT, 0U);   /* no khal_gpio_irq_enable expected */

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const IoexpInputEvt *in = (const IoexpInputEvt *)ao_spy_take();
    TEST_ASSERT_FALSE(in->ok);
    TEST_ASSERT_EQUAL_HEX8(0x00U, in->levels);
    TEST_ASSERT_EQUAL_HEX8(0x00U, in->changed);
    ao_spy_drain();

    /* Nothing until the retry is due. */
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_IoExp,
                                   QF_SUPPORT_STEPS(KODE_IOEXP_RETRY_MS) - 1U));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* Back to life by then: the retry reads, reports what moved against the
     * last reading that succeeded, and re-arms the line. */
    s_bus_dead = false;
    s_regs[REG_INPUT] = 0xFEU;
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_IoExp, 1U));
    in = (const IoexpInputEvt *)ao_spy_take();
    TEST_ASSERT_TRUE(in->ok);
    TEST_ASSERT_EQUAL_HEX8(0x01U, in->changed);
    ao_spy_drain();
    s_regs[REG_INPUT] = 0xFFU;
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    QASM_DISPATCH(AO_IoExp, &INT_EVT, 0U);
    ao_spy_drain();

    mocks_down();
}

/* A chip that stays dead is looked at once per retry, not once per bounce
 * of a line it cannot release, and each look is published as not ok. */
TEST_CASE("a chip still dead at the retry is looked at again, and the line stays un-armed",
          "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    s_bus_dead = true;
    QASM_DISPATCH(AO_IoExp, &INT_EVT, 0U);
    ao_spy_drain();
    for ( int round = 0; 2 > round; ++round )
    {
        TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_IoExp,
                                       QF_SUPPORT_STEPS(KODE_IOEXP_RETRY_MS)));
        TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
        const IoexpInputEvt *in = (const IoexpInputEvt *)ao_spy_take();
        TEST_ASSERT_FALSE(in->ok);
        ao_spy_drain();
    }

    /* Alive again: the next retry re-arms the line and arms no retry. */
    s_bus_dead = false;
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_IoExp,
                                   QF_SUPPORT_STEPS(KODE_IOEXP_RETRY_MS)));
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT(0U, qf_support_tick(AO_IoExp,
                                   QF_SUPPORT_STEPS(KODE_IOEXP_RETRY_MS)));

    mocks_down();
}

/* The debounce downstream needs a second look 30 ms after a change. It asks
 * for one, and gets an event whether or not anything moved - and no re-arm,
 * because no interrupt was served. */
TEST_CASE("a read request publishes the inputs even when nothing changed",
          "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    ioexp_read_request();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_IoExp, 4U));

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    const IoexpInputEvt *in = (const IoexpInputEvt *)ao_spy_take();
    TEST_ASSERT_TRUE(in->ok);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, in->levels);
    TEST_ASSERT_EQUAL_HEX8(0x00U, in->changed);
    ao_spy_drain();

    mocks_down();
}

/* ---- Field requests ------------------------------------------------------ */

/** @brief Ask, pump, and hand back the one answer the spy received. */
static const IoexpFieldEvt *ask(uint8_t op, uint16_t field, int32_t value)
{
    ioexp_field_request(ao_spy(), op, field, value);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_IoExp, 4U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "a request is always answered");
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_EQUAL_INT(IOEXP_FIELD_RSP_SIG, e->sig);
    return (const IoexpFieldEvt *)e;
}

/* This is how the panel gets its supply: AO_Display asks for o7 and c7. The
 * answer carries what the chip kept, read back, not what was asked. */
TEST_CASE("a field write is answered to whoever asked with what the chip kept",
          "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    const IoexpFieldEvt *a = ask(FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 0);
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_SET, a->op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_TCA_O7, a->field);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(0, a->value);
    TEST_ASSERT_EQUAL_HEX8(0x7FU, s_regs[REG_OUTPUT]);
    ao_spy_drain();

    /* Put it back so the rest of the file sees the chip at rest. */
    (void)ask(FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 1);
    ao_spy_drain();

    mocks_down();
}

TEST_CASE("a field the driver refuses is answered with the refusal, and the bus is not touched",
          "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    const IoexpFieldEvt *a = ask(FIELD_OP_SET, (uint16_t)KDRV_TCA_INPUT, 5);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, s_reads);
    TEST_ASSERT_EQUAL_UINT(0U, s_writes);
    ao_spy_drain();

    mocks_down();
}

TEST_CASE("a read request for one field answers with its value", "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    const IoexpFieldEvt *a = ask(FIELD_OP_GET, (uint16_t)KDRV_TCA_CONFIG, 0);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(0x7F, a->value);
    ao_spy_drain();

    mocks_down();
}

TEST_CASE("a dump request reads the whole chip and says so", "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    const IoexpFieldEvt *a = ask(FIELD_OP_DUMP, 0U, 0);
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_DUMP, a->op);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(4U, s_reads);
    ao_spy_drain();

    mocks_down();
}

/* ---- The self-test ------------------------------------------------------- */

/** @brief Run `test component=ioexp` and hand back the verdict. */
static const TestResultEvt *self_test(void)
{
    ioexp_test_request();
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_IoExp, 4U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "a self-test is always answered");
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, e->sig);
    return (const TestResultEvt *)e;
}

/* The verdict is about the chip doing its job: it answers, P0..P6 face the
 * way the board needs, and reading the inputs releases the interrupt line. */
TEST_CASE("the self-test passes when the chip answers, the inputs face in and INT is idle",
          "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    khal_gpio_level_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true);
    const TestResultEvt *v = self_test();
    TEST_ASSERT_EQUAL_STRING("ioexp", v->name);
    TEST_ASSERT_TRUE(v->ok);
    ao_spy_drain();

    mocks_down();
}

TEST_CASE("the self-test fails when the interrupt line stays asserted after a read",
          "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    khal_gpio_level_ExpectAndReturn(KBSP_PIN_IOEXP_INT, false);
    const TestResultEvt *v = self_test();
    TEST_ASSERT_FALSE(v->ok);
    ao_spy_drain();

    mocks_down();
}

TEST_CASE("the self-test fails when an input pin faces out", "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    s_regs[REG_CONFIG] = 0x7DU;                     /* P1 an output */
    khal_gpio_level_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true);
    const TestResultEvt *v = self_test();
    TEST_ASSERT_FALSE(v->ok);
    s_regs[REG_CONFIG] = 0x7FU;
    ao_spy_drain();

    mocks_down();
}

/* ---- The panel's park ------------------------------------------------------ */

/* With AO_Display not linked, kode_init() parks the panel by asking the
 * expander's owner to cut VCI_EN (P7), the way AO_Display asks for it, the
 * other way round: the level first, then the direction, so a pin that a cold
 * power-on left an input never drives high on its way to low. */
TEST_CASE("an absent AO_Display's park asks AO_IoExp to cut the panel's supply, "
          "level then direction", "[ioexp_ao]")
{
    ioexp_up_once();
    ao_spy_drain();

    bool const panel = owners_lend(KBSP_RES_PANEL, AO_Display);
#if CONFIG_KODEOS_MIRROR
    bool const jpeg  = owners_lend(KBSP_RES_JPEG, AO_Display);
    bool const ppa   = owners_lend(KBSP_RES_PPA, AO_Display);
#endif
    s_regs[REG_OUTPUT] = 0xFFU;
    s_regs[REG_CONFIG] = 0xFFU;                     /* a cold board: P7 an input */
    s_writes = 0U;

    kaos_park_display();

    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_IoExp, 1U));
    TEST_ASSERT_EQUAL_HEX8(0x7FU, s_regs[REG_OUTPUT]);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, s_regs[REG_CONFIG]);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_IoExp, 1U));
    TEST_ASSERT_EQUAL_HEX8(0x7FU, s_regs[REG_CONFIG]);
    TEST_ASSERT_EQUAL_UINT(2U, s_writes);
    TEST_ASSERT_EQUAL_STRING("parked(AO_Display)", kbsp_owner_name(KBSP_RES_PANEL));

    owners_return(KBSP_RES_PANEL, AO_Display, "AO_Display", panel);
#if CONFIG_KODEOS_MIRROR
    owners_return(KBSP_RES_JPEG, AO_Display, "AO_Display", jpeg);
    owners_return(KBSP_RES_PPA, AO_Display, "AO_Display", ppa);
#endif
    s_regs[REG_OUTPUT] = 0xFFU;
    s_regs[REG_CONFIG] = 0x7FU;
    ao_spy_drain();

    mocks_down();
}

/* ---- The board's rest ------------------------------------------------------- */

/** @brief A round arms the two wake lines: INT, then the top button's own pin. */
static void expect_wakes_armed(void)
{
    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_OK);
    khal_gpio_wake_low_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KODE_OK);
}

/** @brief AWAKE takes the two wakes away: the top button's, then INT's. */
static void expect_wakes_withdrawn(void)
{
    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, false, KODE_OK);
    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, false, KODE_OK);
}

/* This AO does not rest: it is what wakes the board. The round arms INT and the
 * top button as light-sleep wakes, once (OFF after ASLEEP finds them armed), and
 * AWAKE takes them away, once. */
TEST_CASE("a round arms INT and the top button as light-sleep wakes, and AWAKE takes them away",
          "[ioexp_ao][lowpower]")
{
    ioexp_up_once();
    ao_spy_drain();

    expect_wakes_armed();
    lp_round(AO_IoExp, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    lp_round(AO_IoExp, KODE_LP_OFF);            /* armed already: no second call */
    lp_expect_answer(false, NULL);

    expect_wakes_withdrawn();
    lp_awake(AO_IoExp);
    lp_awake(AO_IoExp);                         /* an abandoned round's: nothing */

    mocks_down();
}

/* The wake the interrupt withdrew comes back with the re-arm that follows the
 * read, and the press that woke the board is published like any other. */
TEST_CASE("a wake is re-armed after the read that served it", "[ioexp_ao][lowpower]")
{
    ioexp_up_once();
    ao_spy_drain();
    expect_wakes_armed();
    lp_round(AO_IoExp, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    s_regs[REG_INPUT] = 0xFDU;                  /* P1 went low: a press */
    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, false, KODE_OK);   /* in the interrupt */
    s_int_cb(s_int_arg);
    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_OK);    /* after the read */
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_IoExp, 4U));

    const IoexpInputEvt *in = (const IoexpInputEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_EQUAL_INT(IOEXP_INPUT_SIG, in->super.sig);
    TEST_ASSERT_EQUAL_HEX8(0x02U, in->changed);
    ao_spy_drain();

    s_regs[REG_INPUT] = 0xFFU;
    expect_wakes_withdrawn();
    lp_awake(AO_IoExp);
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    QASM_DISPATCH(AO_IoExp, &INT_EVT, 0U);      /* disarmed: the plain re-arm */
    ao_spy_drain();

    mocks_down();
}

/* A read that failed leaves the line low and the wake withdrawn; sleeping on
 * that would leave the board deaf, or looping on a line nobody can release. */
TEST_CASE("a read waiting for its retry refuses the round until it succeeds",
          "[ioexp_ao][lowpower]")
{
    ioexp_up_once();
    ao_spy_drain();

    s_bus_dead = true;
    QASM_DISPATCH(AO_IoExp, &INT_EVT, 0U);      /* the read fails; a retry is armed */
    ao_spy_drain();

    lp_round(AO_IoExp, KODE_LP_ASLEEP);
    lp_expect_answer(true, "expander retry");
    lp_expect_no_more();

    s_bus_dead = false;
    s_regs[REG_INPUT] = 0xFFU;
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_tick(AO_IoExp, QF_SUPPORT_STEPS(KODE_IOEXP_RETRY_MS) + 1U));
    ao_spy_drain();

    expect_wakes_armed();
    lp_round(AO_IoExp, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    expect_wakes_withdrawn();
    lp_awake(AO_IoExp);

    mocks_down();
}

TEST_CASE("a wake that cannot be armed refuses the round, because the board would be deaf",
          "[ioexp_ao][lowpower]")
{
    ioexp_up_once();
    ao_spy_drain();

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_ERROR);
    lp_round(AO_IoExp, KODE_LP_ASLEEP);
    lp_expect_answer(true, "no wake line");

    lp_awake(AO_IoExp);                         /* never armed: nothing to take away */

    mocks_down();
}

/* The top button is one more way out, not the only one: INT and the board's rest
 * go on without it, and AWAKE has only INT to take away. */
TEST_CASE("a top button that will not arm costs a warning, not the round",
          "[ioexp_ao][lowpower]")
{
    ioexp_up_once();
    ao_spy_drain();

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_OK);
    khal_gpio_wake_low_ExpectAndReturn(KBSP_PIN_BUTTON_TOP, KODE_ERROR);
    lp_round(AO_IoExp, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, false, KODE_OK);
    lp_awake(AO_IoExp);

    mocks_down();
}

/* A radio wake keeps the expander's INT armed as the light-sleep wake: the
 * glass is asleep and BOT must still be able to wake the board. No call is
 * expected on STATE(RADIO); CMock fails an unexpected one. */
TEST_CASE("STATE(RADIO) leaves INT armed as the light-sleep wake; AWAKE takes it away",
          "[ioexp_ao][lowpower]")
{
    ioexp_up_once();
    ao_spy_drain();

    expect_wakes_armed();
    lp_round(AO_IoExp, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    lp_radio(AO_IoExp);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the radio wake published something");

    expect_wakes_withdrawn();
    lp_awake(AO_IoExp);

    mocks_down();
}
