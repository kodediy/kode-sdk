/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_tca6408a.c
 * @brief The TCA6408A driver, checked by what it says on the bus.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_gpio.h"
#include "Mockkhal_i2c.h"

#include "fakes.h"
#include "kbsp.h"
#include "kdrv_tca6408a.h"

/* The register map, repeated rather than imported, so a wrong driver constant fails here. */
#define REG_INPUT     0x00U
#define REG_OUTPUT    0x01U
#define REG_POLARITY  0x02U
#define REG_CONFIG    0x03U

/** @brief Any non-NULL handle; every use of it is mocked. */
static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

/** @brief Queue one mocked register read returning @p val. */
static void expect_r8(uint8_t reg, uint8_t val, uint8_t *hold)
{
    *hold = val;
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, 1U, KODE_OK);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
    khal_i2c_read_reg_ReturnMemThruPtr_buf(hold, 1U);
}

/** @brief Refuse the next read of @p reg, as a dead bus would. */
static void expect_r8_fail(uint8_t reg)
{
    khal_i2c_read_reg_ExpectAndReturn(NULL, reg, NULL, 1U, KODE_ERROR);
    khal_i2c_read_reg_IgnoreArg_dev();
    khal_i2c_read_reg_IgnoreArg_buf();
}

/** @brief Expect one register write of @p val to @p reg. */
static void expect_w8(uint8_t reg, uint8_t val)
{
    khal_i2c_write_reg_ExpectAndReturn(NULL, reg, val, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
}

/** @brief The bus side of a bring-up: probe, attach, read all four registers. */
static void expect_bring_up(uint8_t in, uint8_t out, uint8_t pol, uint8_t cfg,
                            uint8_t hold[4])
{
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_IOEXP, 100U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8(REG_INPUT,    in,  &hold[0]);
    expect_r8(REG_OUTPUT,   out, &hold[1]);
    expect_r8(REG_POLARITY, pol, &hold[2]);
    expect_r8(REG_CONFIG,   cfg, &hold[3]);
}

/** @brief Bring the driver up once per binary, as a cold power-up would. */
static void bring_up(void)
{
    static bool    s_up;
    static uint8_t hold[4];

    if ( s_up )
    {
        return;
    }
    expect_bring_up(0xFFU, 0xFFU, 0x00U, 0xFFU, hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_init());
    s_up = true;
}

/** @brief Read @p f after @p reg answers @p raw. */
static int32_t get_after(kdrv_tca6408a_field_t f, uint8_t reg, uint8_t raw)
{
    uint8_t hold;
    int32_t v = -1;

    expect_r8(reg, raw, &hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_get(f, &v));
    return v;
}

/* ---- Bring-up ------------------------------------------------------------ */

/* First in the file, and it has to be: the only case that can see the driver
 * before it is up. The chip has no identity register, so "nothing ACKs at
 * 0x20" is the whole of what absence looks like. */
TEST_CASE("a bus with nothing at 0x20 is no expander", "[kdrv_tca6408a]")
{
    Mockkhal_i2c_Init();
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_IOEXP, 100U, false);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_tca6408a_init());
    TEST_ASSERT_FALSE(kdrv_tca6408a_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* An ACK and then a register that would not read is not an expander that is
 * up - and the device it was attached as goes back, so the retry that
 * follows adds one device rather than a second. */
TEST_CASE("an expander whose registers would not read stays down and hands its device back",
          "[kdrv_tca6408a]")
{
    uint8_t hold;

    Mockkhal_i2c_Init();
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(KBSP_I2C_ADDR_IOEXP, 100U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    expect_r8(REG_INPUT, 0xFFU, &hold);
    expect_r8_fail(REG_OUTPUT);
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_tca6408a_init());
    TEST_ASSERT_FALSE(kdrv_tca6408a_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Bring-up reads the chip and writes nothing. Its RESET pin is strapped high
 * on this board, so what the registers hold is whatever the last firmware
 * left - a fact to report, never to correct on a guess. */
TEST_CASE("bringing the expander up reads its four registers and writes nothing",
          "[kdrv_tca6408a]")
{
    Mockkhal_i2c_Init();
    bring_up();
    TEST_ASSERT_TRUE(kdrv_tca6408a_present());
    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Decoding ------------------------------------------------------------ */

TEST_CASE("the input register decodes whole and by pin", "[kdrv_tca6408a]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT32(0x5A, get_after(KDRV_TCA_INPUT, REG_INPUT, 0x5AU));
    TEST_ASSERT_EQUAL_INT32(1,    get_after(KDRV_TCA_I6,    REG_INPUT, 0x40U));
    TEST_ASSERT_EQUAL_INT32(0,    get_after(KDRV_TCA_I6,    REG_INPUT, 0xBFU));
    TEST_ASSERT_EQUAL_INT32(0,    get_after(KDRV_TCA_I0,    REG_INPUT, 0xFEU));
    TEST_ASSERT_EQUAL_INT32(127,  get_after(KDRV_TCA_CONFIG, REG_CONFIG, 0x7FU));
    TEST_ASSERT_EQUAL_INT32(0,    get_after(KDRV_TCA_C7,    REG_CONFIG, 0x7FU));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- Writing ------------------------------------------------------------- */

/* Making P7 an output is the write that powers the panel. The other seven
 * bits of the register belong to other pins, so it is a masked read-modify-
 * write - and the readback compares this field's bit alone, because a
 * neighbouring input may legitimately have moved between the two reads. */
TEST_CASE("writing one bit is a masked read-modify-write checked on that bit alone",
          "[kdrv_tca6408a]")
{
    uint8_t before;
    uint8_t after;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(REG_CONFIG, 0xFFU, &before);
    expect_w8(REG_CONFIG, 0x7FU);
    expect_r8(REG_CONFIG, 0x7EU, &after);   /* bit 0 moved: not our business */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_set(KDRV_TCA_C7, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a write the chip did not keep is an error, not a setting",
          "[kdrv_tca6408a]")
{
    uint8_t before;
    uint8_t after;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(REG_OUTPUT, 0xFFU, &before);
    expect_w8(REG_OUTPUT, 0x7FU);
    expect_r8(REG_OUTPUT, 0xFFU, &after);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_tca6408a_set(KDRV_TCA_O7, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a whole-register field covers all eight bits", "[kdrv_tca6408a]")
{
    uint8_t before;
    uint8_t after;

    Mockkhal_i2c_Init();
    bring_up();

    expect_r8(REG_CONFIG, 0xFFU, &before);
    expect_w8(REG_CONFIG, 0x7FU);
    expect_r8(REG_CONFIG, 0x7FU, &after);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_set(KDRV_TCA_CONFIG, 0x7F));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* Refused before the bus is touched: the mock would fail on any transaction. */
TEST_CASE("inputs are read-only and a value the field cannot hold is refused",
          "[kdrv_tca6408a]")
{
    Mockkhal_i2c_Init();
    bring_up();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_set(KDRV_TCA_INPUT, 0));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_set(KDRV_TCA_I3, 1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_set(KDRV_TCA_O7, 2));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_set(KDRV_TCA_OUTPUT, 256));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_set(KDRV_TCA_OUTPUT, -1));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_set(KDRV_TCA_FIELD_MAX, 0));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The bus dying ------------------------------------------------------- */

/* Said once on the way down and once on the way back. Every read of the
 * expander's inputs comes through here, so a line per failure would bury
 * the console the line is trying to warn. */
TEST_CASE("a bus that stops answering is said once, and once when it returns",
          "[kdrv_tca6408a]")
{
    uint8_t hold;
    int32_t v = -1;

    Mockkhal_i2c_Init();
    bring_up();

    unsigned const quiet = fake_klog_count();
    expect_r8_fail(REG_INPUT);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_tca6408a_get(KDRV_TCA_INPUT, &v));
    expect_r8_fail(REG_INPUT);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_tca6408a_get(KDRV_TCA_INPUT, &v));
    TEST_ASSERT_EQUAL_INT32(-1, v);
    TEST_ASSERT_EQUAL_UINT(quiet + 1U, fake_klog_count());

    expect_r8(REG_INPUT, 0xFFU, &hold);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_get(KDRV_TCA_INPUT, &v));
    TEST_ASSERT_EQUAL_UINT(quiet + 2U, fake_klog_count());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The table ----------------------------------------------------------- */

TEST_CASE("the table names four registers and thirty-two pins", "[kdrv_tca6408a]")
{
    TEST_ASSERT_EQUAL_INT(36, (int)KDRV_TCA_FIELD_MAX);

    const kdrv_tca6408a_field_info_t *in = kdrv_tca6408a_field_info(KDRV_TCA_INPUT);
    TEST_ASSERT_NOT_NULL(in);
    TEST_ASSERT_EQUAL_STRING("input", in->name);
    TEST_ASSERT_FALSE(in->writable);
    TEST_ASSERT_EQUAL_INT32(0,   in->min);
    TEST_ASSERT_EQUAL_INT32(255, in->max);

    const kdrv_tca6408a_field_info_t *c7 = kdrv_tca6408a_field_info(KDRV_TCA_C7);
    TEST_ASSERT_EQUAL_STRING("c7", c7->name);
    TEST_ASSERT_EQUAL_STRING("bool", c7->unit);
    TEST_ASSERT_TRUE(c7->writable);
    TEST_ASSERT_EQUAL_INT32(1, c7->max);

    TEST_ASSERT_EQUAL_STRING("i0", kdrv_tca6408a_field_info(KDRV_TCA_I0)->name);
    TEST_ASSERT_EQUAL_STRING("o7", kdrv_tca6408a_field_info(KDRV_TCA_O7)->name);
    TEST_ASSERT_EQUAL_STRING("n3", kdrv_tca6408a_field_info(KDRV_TCA_N3)->name);
    TEST_ASSERT_EQUAL_STRING("polarity", kdrv_tca6408a_field_info(KDRV_TCA_POLARITY)->name);
    TEST_ASSERT_NULL(kdrv_tca6408a_field_info(KDRV_TCA_FIELD_MAX));
}

TEST_CASE("every field is found by its name, and an unknown name is refused",
          "[kdrv_tca6408a]")
{
    for ( int f = 0; (int)KDRV_TCA_FIELD_MAX > f; ++f )
    {
        kdrv_tca6408a_field_t got = KDRV_TCA_FIELD_MAX;
        const char *name = kdrv_tca6408a_field_info((kdrv_tca6408a_field_t)f)->name;
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_field_by_name(name, &got));
        TEST_ASSERT_EQUAL_INT(f, (int)got);
    }
    kdrv_tca6408a_field_t got;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_field_by_name("p9", &got));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_field_by_name(NULL, &got));
}

/* ---- Dump ---------------------------------------------------------------- */

TEST_CASE("a dump reads the four registers and logs them", "[kdrv_tca6408a]")
{
    uint8_t hold[4];

    Mockkhal_i2c_Init();
    bring_up();

    unsigned const before = fake_klog_count();
    expect_r8(REG_INPUT,    0x7FU, &hold[0]);
    expect_r8(REG_OUTPUT,   0xFFU, &hold[1]);
    expect_r8(REG_POLARITY, 0x00U, &hold[2]);
    expect_r8(REG_CONFIG,   0x7FU, &hold[3]);
    kdrv_tca6408a_dump();
    TEST_ASSERT_TRUE(fake_klog_count() > before);

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* ---- The interrupt line -------------------------------------------------- */

static unsigned s_edges;

static void on_edge(void *arg)
{
    (void)arg;
    ++s_edges;
}

/** @brief What the driver handed khal_gpio as GPIO6's callback. */
static khal_gpio_cb_t s_isr;
static void          *s_isr_arg;

static kode_status_t capture_isr(uint8_t pin, khal_gpio_irq_t kind,
                                 khal_gpio_cb_t cb, void *arg, int ncalls)
{
    (void)ncalls;
    TEST_ASSERT_EQUAL_UINT8(KBSP_PIN_IOEXP_INT, pin);
    TEST_ASSERT_EQUAL_INT(KHAL_GPIO_IRQ_LOW_LEVEL, kind);
    s_isr     = cb;
    s_isr_arg = arg;
    return KODE_OK;
}

/** @brief Attach with the owner's callback and keep the driver's own. */
static void attach_for_cases(void)
{
    s_edges = 0;
    khal_gpio_input_ExpectAndReturn(KBSP_PIN_IOEXP_INT, KHAL_GPIO_PULL_NONE, KODE_OK);
    khal_gpio_irq_attach_StubWithCallback(&capture_isr);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_int_attach(on_edge, &s_edges));
    TEST_ASSERT_NOT_NULL(s_isr);
}

/* The INT pin is open drain and stays low until the input register is read,
 * so it is armed as a level, not an edge: an edge missed while the P4 was
 * busy would be a button lost, a level waits. */
TEST_CASE("the interrupt line is GPIO6, armed low-level, re-armed on request",
          "[kdrv_tca6408a]")
{
    Mockkhal_gpio_Init();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_tca6408a_int_attach(NULL, NULL));

    attach_for_cases();

    /* And the owner's callback is what the interrupt reaches. */
    s_isr(s_isr_arg);
    TEST_ASSERT_EQUAL_INT(1, s_edges);

    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    kdrv_tca6408a_int_rearm();

    khal_gpio_level_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true);
    TEST_ASSERT_TRUE(kdrv_tca6408a_int_idle());

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* ---- The light-sleep wake ----------------------------------------------------- */

/* Armed, the line wakes the chip; the interrupt takes the wake away before the
 * owner hears of it (a line that stays low would wake the chip again at once),
 * and the re-arm that follows the read puts it back with the interrupt. */
TEST_CASE("armed, the interrupt withdraws the wake before the owner's callback and re-arm restores it",
          "[kdrv_tca6408a][lowpower]")
{
    Mockkhal_gpio_Init();
    attach_for_cases();

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_sleep_wake(true));

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, false, KODE_OK);
    s_isr(s_isr_arg);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_edges, "the owner was not told");

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_OK);
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    kdrv_tca6408a_int_rearm();

    /* Withdrawn, the line is the plain interrupt again, at every step. */
    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, false, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_sleep_wake(false));
    s_isr(s_isr_arg);                          /* no wake call expected */
    TEST_ASSERT_EQUAL_INT(2, s_edges);
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    kdrv_tca6408a_int_rearm();                 /* and none here */

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A wake the HAL would not arm is not believed armed: the interrupt does not
 * try to withdraw it, and the re-arm does not put it back. */
TEST_CASE("a wake that would not arm is not believed armed", "[kdrv_tca6408a][lowpower]")
{
    Mockkhal_gpio_Init();
    attach_for_cases();

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_tca6408a_sleep_wake(true));

    s_isr(s_isr_arg);
    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    kdrv_tca6408a_int_rearm();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}

/* A withdrawal that failed still means the owner wants the plain interrupt. */
TEST_CASE("a wake that would not withdraw is not put back by the re-arm",
          "[kdrv_tca6408a][lowpower]")
{
    Mockkhal_gpio_Init();
    attach_for_cases();

    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, true, KODE_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_tca6408a_sleep_wake(true));
    khal_gpio_wake_ExpectAndReturn(KBSP_PIN_IOEXP_INT, false, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_tca6408a_sleep_wake(false));

    khal_gpio_irq_enable_Expect(KBSP_PIN_IOEXP_INT);
    kdrv_tca6408a_int_rearm();

    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
}
