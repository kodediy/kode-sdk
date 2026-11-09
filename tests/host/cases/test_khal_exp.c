/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_exp.c
 * @brief khal_exp's table: what a position may become, and what it refuses.
 */
#include <string.h>

#include "unity.h"

#include "Mockkhal_exp_port.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_pwm.h"

#include "kbsp_exp.h"
#include "khal_exp.h"

static void exp_fresh(void)
{
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    Mockkhal_pwm_Init();
    /* init() puts every pin to high-Z and undoes whatever the case before
     * this one left in the table - s_pos is static across the binary - so
     * fourteen releases in any order, plus whatever ADC channel or PWM slot
     * has to be closed on the way. Ignored here and only here: a case that
     * goes on to expect one of these wants its argument checked, and ignore
     * mode would let any argument through. */
    khal_gpio_release_Ignore();
    khal_exp_port_adc_close_Ignore();
    khal_pwm_close_Ignore();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_init());
    khal_gpio_release_StopIgnore();
    khal_exp_port_adc_close_StopIgnore();
    khal_pwm_close_StopIgnore();
    /* Every case starts from FREE: khal_exp_init() is idempotent and resets
     * the table, which is what lets one binary hold all of these. */
}

static void exp_done(void)
{
    Mockkhal_gpio_Verify();
    Mockkhal_exp_port_Verify();
    Mockkhal_pwm_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

static khal_pwm_cfg_t s_open_cfg;     /**< What the last khal_pwm_open() was asked. */
static uint32_t       s_open_calls;   /**< khal_pwm_open() calls since pwm_opens(). */
static kode_status_t  s_open_rc;      /**< What khal_pwm_open() answers. */
static khal_pwm_t     s_open_h;       /**< The handle it gives on KODE_OK. */

/**
 * @brief Stand-in for khal_pwm_open(): record the request field by field.
 *
 * A stub rather than an expectation because CMock compares a struct pointer
 * byte by byte, padding included, and the padding of a local is not zeroed.
 *
 * @param cfg   The request.
 * @param out   The handle written.
 * @param calls How many calls CMock has seen.
 * @return s_open_rc.
 */
static kode_status_t open_stub(const khal_pwm_cfg_t *cfg, khal_pwm_t *out,
                               int calls)
{
    (void)calls;
    ++s_open_calls;
    s_open_cfg = *cfg;
    if ( KODE_OK == s_open_rc )
    {
        *out = s_open_h;
    }
    else
    {
        *out = KHAL_PWM_NONE;
    }
    return s_open_rc;
}

/**
 * @brief Make the next khal_pwm_open() calls answer @p rc with handle @p h.
 * @param rc What khal_pwm_open() answers.
 * @param h  The handle on KODE_OK.
 */
static void pwm_opens(kode_status_t rc, khal_pwm_t h)
{
    s_open_calls = 0U;
    s_open_rc = rc;
    s_open_h = h;
    khal_pwm_open_Stub(&open_stub);
}

/**
 * @brief The last request was a shared channel at the widest resolution.
 * @param gpio The pin asked for.
 * @param hz   The frequency asked for.
 * @param duty The duty asked for.
 */
static void assert_opened(uint8_t gpio, uint32_t hz, uint16_t duty)
{
    TEST_ASSERT_EQUAL_UINT8(gpio, s_open_cfg.gpio);
    TEST_ASSERT_EQUAL_UINT32(hz, s_open_cfg.hz);
    TEST_ASSERT_EQUAL_UINT8(KHAL_PWM_BITS_WIDEST, s_open_cfg.bits);
    TEST_ASSERT_EQUAL_UINT16(duty, s_open_cfg.duty_x10);
    TEST_ASSERT_FALSE(s_open_cfg.own_timer);
}

TEST_CASE("a position out of range is refused before anything is touched",
          "[khal_exp]")
{
    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_input(0U, KHAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_output(15U, true));
    TEST_ASSERT_FALSE(khal_exp_level(15U));
    exp_done();
}

TEST_CASE("an input is configured on the position's GPIO with its pull",
          "[khal_exp]")
{
    khal_exp_info_t info;

    exp_fresh();
    khal_gpio_input_ExpectAndReturn(8U, KHAL_GPIO_PULL_UP, KODE_OK);   /* EXP1 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_input(1U, KHAL_GPIO_PULL_UP));
    khal_exp_info(1U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_IN, info.mode);
    TEST_ASSERT_EQUAL_UINT8(8U, info.gpio);

    khal_gpio_level_ExpectAndReturn(8U, true);
    TEST_ASSERT_TRUE(khal_exp_level(1U));
    exp_done();
}

TEST_CASE("an ADC is refused on a position without a channel", "[khal_exp]")
{
    exp_fresh();
    /* EXP1 is GPIO8: digital, LP, no ADC1 channel. The port is not called. */
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_adc(1U));
    exp_done();
}

TEST_CASE("an ADC position opens its channel and reads through the port",
          "[khal_exp]")
{
    uint16_t raw = 0U;
    int32_t  mv  = 0;
    bool     cal = false;
    static uint16_t port_raw = 2048U;
    static int32_t  port_mv  = 1650;
    static bool     port_cal = true;

    exp_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(0U, KODE_OK);   /* EXP14 = GPIO16 = ch0 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_adc(14U));

    khal_exp_port_adc_read_ExpectAndReturn(0U, NULL, NULL, NULL, KODE_OK);
    khal_exp_port_adc_read_IgnoreArg_raw();
    khal_exp_port_adc_read_IgnoreArg_mv();
    khal_exp_port_adc_read_IgnoreArg_calibrated();
    khal_exp_port_adc_read_ReturnThruPtr_raw(&port_raw);
    khal_exp_port_adc_read_ReturnThruPtr_mv(&port_mv);
    khal_exp_port_adc_read_ReturnThruPtr_calibrated(&port_cal);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_adc_read(14U, &raw, &mv, &cal));
    TEST_ASSERT_EQUAL_UINT16(2048U, raw);
    TEST_ASSERT_EQUAL_INT32(1650, mv);
    TEST_ASSERT_TRUE(cal);

    /* Reading a position that is not ADC touches nothing. */
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_adc_read(1U, &raw, &mv, &cal));
    exp_done();
}

TEST_CASE("a mode change closes what the old mode opened", "[khal_exp]")
{
    exp_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(5U, KODE_OK);   /* EXP8 = GPIO21 = ch5 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_adc(8U));
    /* ADC to OUT: the channel is closed before the pin is driven. */
    khal_exp_port_adc_close_Expect(5U);
    khal_gpio_output_ExpectAndReturn(21U, false, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_output(8U, false));
    /* OUT to PWM: nothing to close; a channel opens. */
    pwm_opens(KODE_OK, 3U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(8U, 1000U, 250U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_open_calls);
    /* PWM to IN: the channel closes before the pin is an input. */
    khal_pwm_close_Expect(3U);
    khal_gpio_input_ExpectAndReturn(21U, KHAL_GPIO_PULL_DOWN, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_input(8U, KHAL_GPIO_PULL_DOWN));
    exp_done();
}

TEST_CASE("pwm opens a shared channel at the widest bits", "[khal_exp]")
{
    exp_fresh();
    pwm_opens(KODE_OK, 3U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(8U, 1000U, 250U));   /* EXP8 = GPIO21 */
    assert_opened(21U, 1000U, 250U);
    /* A PWM position retunes its own channel: nothing opens. */
    khal_pwm_set_ExpectAndReturn(3U, 2000U, 500U, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(8U, 2000U, 500U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_open_calls);
    exp_done();
}

TEST_CASE("no channel from khal_pwm leaves the position as it was", "[khal_exp]")
{
    khal_exp_info_t info;

    exp_fresh();
    khal_gpio_output_ExpectAndReturn(21U, true, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_output(8U, true));
    pwm_opens(KODE_NO_RESOURCES, KHAL_PWM_NONE);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, khal_exp_pwm(8U, 1000U, 250U));
    khal_exp_info(8U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_OUT, info.mode);
    exp_done();
}

TEST_CASE("a refused retune keeps the position pwm", "[khal_exp]")
{
    khal_exp_info_t info;

    exp_fresh();
    pwm_opens(KODE_OK, 3U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(8U, 1000U, 250U));
    khal_pwm_set_ExpectAndReturn(3U, 5000U, 250U, KODE_NO_RESOURCES);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, khal_exp_pwm(8U, 5000U, 250U));
    khal_exp_info(8U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_PWM, info.mode);
    /* The same channel is the one handed back. */
    khal_pwm_close_Expect(3U);
    khal_gpio_release_Expect(21U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_release(8U));
    exp_done();
}

TEST_CASE("init closes every pwm position", "[khal_exp]")
{
    exp_fresh();
    pwm_opens(KODE_OK, 3U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(1U, 1000U, 250U));
    pwm_opens(KODE_OK, 5U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(2U, 2000U, 250U));
    khal_pwm_close_Expect(3U);
    khal_pwm_close_Expect(5U);
    khal_gpio_release_Ignore();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_init());
    khal_gpio_release_StopIgnore();
    exp_done();
}

TEST_CASE("a PWM outside its ranges is refused", "[khal_exp]")
{
    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_pwm(1U, 0U, 100U));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_pwm(1U, 40001U, 100U));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_pwm(1U, 1000U, 1001U));
    exp_done();
}

TEST_CASE("a loan is all or nothing and returns by tag", "[khal_exp]")
{
    static const uint8_t four[4] = { 1U, 2U, 3U, 4U };
    static const uint8_t two[2]  = { 5U, 6U };
    uint8_t gpio[4] = { 0U };
    khal_exp_info_t info;

    exp_fresh();
    /* The third of four is OUT: the whole set is refused, nothing marked. */
    khal_gpio_output_ExpectAndReturn(10U, true, KODE_OK);   /* EXP3 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_output(3U, true));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_lend(four, 4U, "x", gpio));
    khal_exp_info(1U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_FREE, info.mode);

    /* Two free ones under "y", GPIOs back in the caller's order. */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_lend(two, 2U, "y", gpio));
    TEST_ASSERT_EQUAL_UINT8(12U, gpio[0]);   /* EXP5 */
    TEST_ASSERT_EQUAL_UINT8(13U, gpio[1]);   /* EXP6 */
    khal_exp_info(5U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_LENT, info.mode);
    TEST_ASSERT_EQUAL_STRING("y", info.tag);

    /* A pin function on a lent position is refused; release too. */
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_input(5U, KHAL_GPIO_PULL_NONE));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_release(5U));

    /* Returning "x" frees nothing; returning "y" frees exactly EXP5 and EXP6. */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_return("x"));
    khal_exp_info(5U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_LENT, info.mode);
    khal_gpio_release_Expect(12U);
    khal_gpio_release_Expect(13U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_return("y"));
    khal_exp_info(6U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_FREE, info.mode);
    TEST_ASSERT_NULL(info.tag);
    exp_done();
}

TEST_CASE("an interrupt needs an input first, and re-arms by position",
          "[khal_exp]")
{
    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      khal_exp_irq_attach(2U, KHAL_GPIO_IRQ_ANY_EDGE, NULL, NULL));
    khal_gpio_input_ExpectAndReturn(9U, KHAL_GPIO_PULL_UP, KODE_OK);   /* EXP2 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_input(2U, KHAL_GPIO_PULL_UP));
    khal_gpio_irq_attach_ExpectAndReturn(9U, KHAL_GPIO_IRQ_ANY_EDGE, NULL, NULL, KODE_OK);
    khal_gpio_irq_attach_IgnoreArg_cb();
    TEST_ASSERT_EQUAL(KODE_OK,
                      khal_exp_irq_attach(2U, KHAL_GPIO_IRQ_ANY_EDGE,
                                          (khal_gpio_cb_t)0x1, NULL));
    khal_gpio_irq_enable_Expect(9U);
    khal_exp_irq_enable(2U);
    exp_done();
}

TEST_CASE("an uncalibrated port still fills the raw count", "[khal_exp]")
{
    uint16_t raw = 0U;
    int32_t  mv  = 0;
    bool     cal = true;
    static uint16_t port_raw = 100U;
    static int32_t  port_mv  = 80;
    static bool     port_cal = false;

    exp_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(1U, KODE_OK);   /* EXP11 = GPIO17 = ch1 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_adc(11U));
    khal_exp_port_adc_read_ExpectAndReturn(1U, NULL, NULL, NULL, KODE_OK);
    khal_exp_port_adc_read_IgnoreArg_raw();
    khal_exp_port_adc_read_IgnoreArg_mv();
    khal_exp_port_adc_read_IgnoreArg_calibrated();
    khal_exp_port_adc_read_ReturnThruPtr_raw(&port_raw);
    khal_exp_port_adc_read_ReturnThruPtr_mv(&port_mv);
    khal_exp_port_adc_read_ReturnThruPtr_calibrated(&port_cal);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_adc_read(11U, &raw, &mv, &cal));
    TEST_ASSERT_EQUAL_UINT16(100U, raw);
    TEST_ASSERT_FALSE(cal);
    exp_done();
}

/*
 * "Always filled" has to be true of the table's own refusals too, not just of
 * the port's: the `exp` row prints raw and mv without reading the status, so a
 * read the table turned away has to leave zeroes rather than the caller's
 * stack.
 */
TEST_CASE("a refused read still leaves its three outputs defined", "[khal_exp]")
{
    uint16_t raw = 1U;
    int32_t  mv  = 1;
    bool     cal = true;

    exp_fresh();
    /* EXP1 is GPIO8 and has no ADC1 channel, so it is never in ADC mode and
     * the port is not reached. */
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_adc_read(1U, &raw, &mv, &cal));
    TEST_ASSERT_EQUAL_UINT16(0U, raw);
    TEST_ASSERT_EQUAL_INT32(0, mv);
    TEST_ASSERT_FALSE(cal);
    exp_done();
}

/*
 * The one path that changes every position's mode without going through
 * leave_mode() used to zero the table instead, which left an ADC channel
 * open and a LEDC slot running with nothing tracking either, and left a
 * tenant believing it still held its GPIOs.
 */
TEST_CASE("a second init undoes every mode and voids every loan", "[khal_exp]")
{
    static const uint8_t two[2] = { 5U, 6U };
    uint8_t gpio[2] = { 0U };
    khal_exp_info_t info;

    exp_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(0U, KODE_OK);   /* EXP14 = GPIO16 = ch0 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_adc(14U));
    pwm_opens(KODE_OK, 0U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(1U, 500U, 500U));   /* EXP1 */
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_lend(two, 2U, "cam", gpio));

    /* The ADC channel and the PWM channel are closed, each named. */
    khal_exp_port_adc_close_Expect(0U);
    khal_pwm_close_Expect(0U);
    khal_gpio_release_Ignore();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_init());
    khal_gpio_release_StopIgnore();

    khal_exp_info(14U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_FREE, info.mode);
    khal_exp_info(1U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_FREE, info.mode);
    /* The loan is gone, and its tenant was not asked. */
    khal_exp_info(5U, &info);
    TEST_ASSERT_EQUAL(KHAL_EXP_FREE, info.mode);
    TEST_ASSERT_NULL(info.tag);

    /* EXP1 is free again: EXP2 opens a channel of its own. */
    pwm_opens(KODE_OK, 0U);
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_pwm(2U, 500U, 500U));   /* EXP2 = GPIO9 */
    assert_opened(9U, 500U, 500U);
    exp_done();
}

/* ---- The controllers an app may take ---------------------------------------- */

/** @brief Index of the first KBSP_EXP_CTRL entry of @p bus. */
static size_t ctrl_first(kbsp_exp_bus_t bus)
{
    size_t i = 0U;
    while ( (KBSP_EXP_CTRL_COUNT > i) && (KBSP_EXP_CTRL[i].bus != bus) )
    {
        ++i;
    }
    return i;
}

TEST_CASE("an app takes I2C1, then there is no I2C left", "[khal_exp][ctrl]")
{
    uint8_t id = 0xFFU;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_I2C, "fck", &id));
    TEST_ASSERT_EQUAL_UINT8(KBSP_EXP_I2C_ID, id);
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, khal_exp_ctrl(KBSP_EXP_I2C, "cam", &id));
    exp_done();
}

TEST_CASE("SPI is SPI3_HOST", "[khal_exp][ctrl]")
{
    uint8_t id = 0xFFU;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_SPI, "fck", &id));
    TEST_ASSERT_EQUAL_UINT8(KBSP_EXP_SPI_ID, id);
    exp_done();
}

TEST_CASE("the UARTs come lowest first, then none", "[khal_exp][ctrl]")
{
    uint8_t id = 0xFFU;

    exp_fresh();
    for ( uint8_t want = (uint8_t)KBSP_EXP_UART_FIRST;
         (uint8_t)KBSP_EXP_UART_LAST >= want; ++want )
    {
        TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_UART, "fck", &id));
        TEST_ASSERT_EQUAL_UINT8(want, id);
    }
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, khal_exp_ctrl(KBSP_EXP_UART, "fck", &id));
    exp_done();
}

TEST_CASE("a return frees one tag's controllers and not another's",
          "[khal_exp][ctrl]")
{
    uint8_t id = 0xFFU;
    khal_exp_ctrl_info_t info;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_I2C, "a", &id));
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_SPI, "b", &id));
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_return("a"));

    khal_exp_ctrl_info(ctrl_first(KBSP_EXP_I2C), &info);
    TEST_ASSERT_NULL(info.tag);
    khal_exp_ctrl_info(ctrl_first(KBSP_EXP_SPI), &info);
    TEST_ASSERT_EQUAL_STRING("b", info.tag);

    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_I2C, "c", &id));
    TEST_ASSERT_EQUAL_UINT8(KBSP_EXP_I2C_ID, id);
    exp_done();
}

TEST_CASE("a second init frees every controller", "[khal_exp][ctrl]")
{
    uint8_t id = 0xFFU;
    khal_exp_ctrl_info_t info;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_SPI, "fck", &id));
    exp_done();
    exp_fresh();   /* khal_exp_init() again */
    for ( size_t i = 0U; KBSP_EXP_CTRL_COUNT > i; ++i )
    {
        khal_exp_ctrl_info(i, &info);
        TEST_ASSERT_NULL(info.tag);
    }
    exp_done();
}

TEST_CASE("a NULL tag, a NULL id or a bus out of range is refused",
          "[khal_exp][ctrl]")
{
    uint8_t id = 0xFFU;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_ctrl(KBSP_EXP_I2C, NULL, &id));
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_exp_ctrl(KBSP_EXP_I2C, "fck", NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED,
                      khal_exp_ctrl((kbsp_exp_bus_t)(KBSP_EXP_UART + 1), "fck", &id));
    exp_done();
}

TEST_CASE("the info walk names each controller, its state and its tag",
          "[khal_exp][ctrl]")
{
    uint8_t id = 0xFFU;
    khal_exp_ctrl_info_t info;
    khal_exp_ctrl_info_t untouched;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_ctrl(KBSP_EXP_UART, "gnss", &id));
    for ( size_t i = 0U; KBSP_EXP_CTRL_COUNT > i; ++i )
    {
        khal_exp_ctrl_info(i, &info);
        TEST_ASSERT_EQUAL_INT(KBSP_EXP_CTRL[i].bus, info.bus);
        TEST_ASSERT_EQUAL_UINT8(KBSP_EXP_CTRL[i].id, info.id);
        if ( (KBSP_EXP_UART == info.bus) && (info.id == id) )
        {
            TEST_ASSERT_EQUAL_STRING("gnss", info.tag);
        }
        else
        {
            TEST_ASSERT_NULL(info.tag);
        }
    }
    (void)memset(&untouched, 0xA5, sizeof(untouched));
    info = untouched;
    khal_exp_ctrl_info(KBSP_EXP_CTRL_COUNT, &info);
    TEST_ASSERT_EQUAL_MEMORY(&untouched, &info, sizeof(info));
    exp_done();
}
