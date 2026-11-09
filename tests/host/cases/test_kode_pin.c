/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_pin.c
 * @brief kode_pin_*() over the real header facade and khal_exp, with the GPIO, ADC and LEDC mocked.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "Mockkhal_exp_port.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_pwm.h"

#include "fakes.h"
#include "khal_exp.h"
#include "khal_once.h"
#include "kode.h"
#include "kode_pin_lock.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"

#define PIN_OUT_POS   1U      /**< A digital-only pin: EXP1.                      */
#define PIN_OUT_GPIO  8U      /**< The GPIO behind it.                            */
#define PIN_ADC_POS   8U      /**< An analog pin: EXP8.                           */
#define PIN_ADC_GPIO  21U     /**< The GPIO behind it.                            */
#define PIN_ADC_CHAN  5U      /**< Its ADC1 channel.                              */
#define PIN_ADC_RAW   2048U   /**< The count the port answers.                    */
#define PIN_ADC_MV    1650    /**< The millivolts the port answers.               */
#define PIN_PWM_HANDLE 3U     /**< The channel khal_pwm hands out.                */
#define PIN_HZ        1000U   /**< A frequency inside the range.                  */
#define PIN_DUTY_PCT  50U     /**< A duty inside the range.                       */
#define PIN_DUTY_X10  500U    /**< Its tenths of a percent.                       */
#define PIN_BELOW     0U      /**< The position under the first.                  */
#define PIN_ABOVE     15U     /**< The position over the last.                    */
#define PIN_DUTY_OVER 101U    /**< A duty over 100 %.                             */
#define PIN_HZ_OVER   40001U  /**< A frequency over the highest.                  */
#define PIN_LENT_TAG  "mesh"  /**< Who holds the lent pins.                       */

static khal_pwm_cfg_t s_open_cfg;    /**< What the last khal_pwm_open() was asked. */
static uint32_t       s_open_calls;  /**< khal_pwm_open() calls since the case began. */

/**
 * @brief Stand-in for khal_pwm_open(): record the request, hand out PIN_PWM_HANDLE.
 * @param cfg   The request.
 * @param out   The handle written.
 * @param calls How many calls CMock has seen.
 * @return KODE_OK.
 */
static kode_status_t open_stub(const khal_pwm_cfg_t *cfg, khal_pwm_t *out, int32_t calls)
{
    (void)calls;
    s_open_calls++;
    s_open_cfg = *cfg;
    *out       = PIN_PWM_HANDLE;
    return KODE_OK;
}

/** @brief Mocks up, the header's table FREE, locks real and the system marked up. */
static void pin_fresh(void)
{
    Mockkhal_gpio_Init();
    Mockkhal_exp_port_Init();
    Mockkhal_pwm_Init();
    khal_gpio_release_Ignore();
    khal_exp_port_adc_close_Ignore();
    khal_pwm_close_Ignore();
    TEST_ASSERT_EQUAL(KODE_OK, khal_exp_init());
    khal_gpio_release_StopIgnore();
    khal_exp_port_adc_close_StopIgnore();
    khal_pwm_close_StopIgnore();
    s_open_calls = 0U;
    khal_pwm_open_Stub(&open_stub);
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

/** @brief Every call the case expected happened; the mocks go down. */
static void pin_done(void)
{
    Mockkhal_gpio_Verify();
    Mockkhal_exp_port_Verify();
    Mockkhal_pwm_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

/**
 * @brief What @p pos is doing.
 * @param pos The position.
 * @return Its mode.
 */
static kode_sys_exp_mode_t pin_mode(uint8_t pos)
{
    kode_sys_exp_info_t info;

    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(pos, &info));
    return info.mode;
}

/** @brief Script the port's next analog read to answer @p mv. */
static void port_reads(int32_t mv, kode_status_t rc)
{
    static uint16_t raw;
    static int32_t  volts;
    static bool     cal;

    raw   = PIN_ADC_RAW;
    volts = mv;
    cal   = true;
    khal_exp_port_adc_read_ExpectAndReturn(PIN_ADC_CHAN, NULL, NULL, NULL, rc);
    khal_exp_port_adc_read_IgnoreArg_raw();
    khal_exp_port_adc_read_IgnoreArg_mv();
    khal_exp_port_adc_read_IgnoreArg_calibrated();
    khal_exp_port_adc_read_ReturnThruPtr_raw(&raw);
    khal_exp_port_adc_read_ReturnThruPtr_mv(&volts);
    khal_exp_port_adc_read_ReturnThruPtr_calibrated(&cal);
}

TEST_CASE("the pin range is the header's", "[kode_pin]")
{
    TEST_ASSERT_EQUAL_UINT32(KHAL_EXP_POS_MIN, KODE_PIN_FIRST);
    TEST_ASSERT_EQUAL_UINT32(KHAL_EXP_POS_MAX, KODE_PIN_LAST);
    TEST_ASSERT_EQUAL_UINT32(KHAL_EXP_PWM_HZ_MIN, KODE_PIN_PWM_HZ_MIN);
    TEST_ASSERT_EQUAL_UINT32(KHAL_EXP_PWM_HZ_MAX, KODE_PIN_PWM_HZ_MAX);
}

TEST_CASE("a write makes the pin an output and drives it", "[kode_pin]")
{
    pin_fresh();
    khal_gpio_output_ExpectAndReturn(PIN_OUT_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_OUT_POS, true));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_OUT, pin_mode(PIN_OUT_POS));

    /* Already an output: the level is driven, the pin is not configured again. */
    khal_gpio_write_ExpectAndReturn(PIN_OUT_GPIO, false, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_OUT_POS, false));
    pin_done();
}

TEST_CASE("a refused write says so", "[kode_pin]")
{
    pin_fresh();
    khal_gpio_output_ExpectAndReturn(PIN_OUT_GPIO, true, KODE_ERROR);
    TEST_ASSERT_FALSE(kode_pin_write(PIN_OUT_POS, true));
    TEST_ASSERT_EQUAL_STRING("pin 1 could not be made an output", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_OUT_POS));
    pin_done();
}

TEST_CASE("a read of an output reads its level and leaves it an output", "[kode_pin]")
{
    pin_fresh();
    khal_gpio_output_ExpectAndReturn(PIN_OUT_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_OUT_POS, true));

    khal_gpio_level_ExpectAndReturn(PIN_OUT_GPIO, true);
    TEST_ASSERT_TRUE(kode_pin_read(PIN_OUT_POS));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_OUT, pin_mode(PIN_OUT_POS));
    pin_done();
}

TEST_CASE("a read of a free pin makes it an input with no pull", "[kode_pin]")
{
    pin_fresh();
    khal_gpio_input_ExpectAndReturn(PIN_OUT_GPIO, KHAL_GPIO_PULL_NONE, KODE_OK);
    khal_gpio_level_ExpectAndReturn(PIN_OUT_GPIO, true);
    TEST_ASSERT_TRUE(kode_pin_read(PIN_OUT_POS));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_OUT_POS));

    /* Already an input: only the level is read. */
    khal_gpio_level_ExpectAndReturn(PIN_OUT_GPIO, false);
    TEST_ASSERT_FALSE(kode_pin_read(PIN_OUT_POS));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    pin_done();
}

TEST_CASE("a refused read says so and answers false", "[kode_pin]")
{
    pin_fresh();
    khal_gpio_input_ExpectAndReturn(PIN_OUT_GPIO, KHAL_GPIO_PULL_NONE, KODE_ERROR);
    TEST_ASSERT_FALSE(kode_pin_read(PIN_OUT_POS));
    TEST_ASSERT_EQUAL_STRING("pin 1 could not be made an input", kode_error());
    pin_done();
}

TEST_CASE("an analog read configures the pin and answers the port's millivolts", "[kode_pin]")
{
    pin_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(PIN_ADC_CHAN, KODE_OK);
    port_reads(PIN_ADC_MV, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(PIN_ADC_MV, kode_pin_read_analog(PIN_ADC_POS));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_ADC, pin_mode(PIN_ADC_POS));

    /* Already analog: the channel stays open and is read again. */
    port_reads(PIN_ADC_MV, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(PIN_ADC_MV, kode_pin_read_analog(PIN_ADC_POS));
    pin_done();
}

TEST_CASE("an analog read of a pin with no analog input is refused", "[kode_pin]")
{
    pin_fresh();
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(PIN_OUT_POS));
    TEST_ASSERT_EQUAL_STRING("pin 1 has no analog input", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_OUT_POS));
    pin_done();
}

TEST_CASE("an analog read that fails or reads below zero answers 0 and says so", "[kode_pin]")
{
    pin_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(PIN_ADC_CHAN, KODE_OK);
    port_reads(0, KODE_IO_ERROR);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(PIN_ADC_POS));
    TEST_ASSERT_EQUAL_STRING("pin 8 could not be read", kode_error());

    port_reads(-1, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(PIN_ADC_POS));
    TEST_ASSERT_EQUAL_STRING("pin 8 could not be read", kode_error());
    pin_done();
}

TEST_CASE("an analog channel that will not open is said", "[kode_pin]")
{
    pin_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(PIN_ADC_CHAN, KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(PIN_ADC_POS));
    TEST_ASSERT_EQUAL_STRING("pin 8 could not be read", kode_error());
    pin_done();
}

TEST_CASE("a pwm of 1000 Hz at 50 percent hands the header a duty of 500", "[kode_pin]")
{
    pin_fresh();
    TEST_ASSERT_TRUE(kode_pin_write_pwm(PIN_OUT_POS, PIN_HZ, PIN_DUTY_PCT));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_open_calls);
    TEST_ASSERT_EQUAL_UINT8(PIN_OUT_GPIO, s_open_cfg.gpio);
    TEST_ASSERT_EQUAL_UINT32(PIN_HZ, s_open_cfg.hz);
    TEST_ASSERT_EQUAL_UINT16(PIN_DUTY_X10, s_open_cfg.duty_x10);
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_PWM, pin_mode(PIN_OUT_POS));

    /* A pin that is already pwm is retuned, not opened again. */
    khal_pwm_set_ExpectAndReturn(PIN_PWM_HANDLE, KODE_PIN_PWM_HZ_MAX, KHAL_EXP_PWM_DUTY_MAX, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write_pwm(PIN_OUT_POS, KODE_PIN_PWM_HZ_MAX, 100U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_open_calls);
    pin_done();
}

TEST_CASE("a pwm with no channel left says so", "[kode_pin]")
{
    pin_fresh();
    khal_pwm_open_Stub(NULL);
    khal_pwm_open_ExpectAndReturn(NULL, NULL, KODE_NO_RESOURCES);
    khal_pwm_open_IgnoreArg_cfg();
    khal_pwm_open_IgnoreArg_out();
    TEST_ASSERT_FALSE(kode_pin_write_pwm(PIN_OUT_POS, PIN_HZ, PIN_DUTY_PCT));
    TEST_ASSERT_EQUAL_STRING("pin 1 has no PWM channel or timer free", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_OUT_POS));
    pin_done();
}

TEST_CASE("a duty over 100 and a frequency outside the range are refused naming the range", "[kode_pin]")
{
    pin_fresh();
    TEST_ASSERT_FALSE(kode_pin_write_pwm(PIN_OUT_POS, PIN_HZ, PIN_DUTY_OVER));
    TEST_ASSERT_EQUAL_STRING("duty 101 % is out of range: use 0 to 100", kode_error());

    TEST_ASSERT_FALSE(kode_pin_write_pwm(PIN_OUT_POS, PIN_HZ_OVER, PIN_DUTY_PCT));
    TEST_ASSERT_EQUAL_STRING("frequency 40001 Hz is out of range: use 1 to 40000", kode_error());

    TEST_ASSERT_FALSE(kode_pin_write_pwm(PIN_OUT_POS, 0U, PIN_DUTY_PCT));
    TEST_ASSERT_EQUAL_STRING("frequency 0 Hz is out of range: use 1 to 40000", kode_error());

    TEST_ASSERT_EQUAL_UINT32(0U, s_open_calls);
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_OUT_POS));
    pin_done();
}

TEST_CASE("the edges of the pwm range are accepted", "[kode_pin]")
{
    pin_fresh();
    TEST_ASSERT_TRUE(kode_pin_write_pwm(PIN_OUT_POS, KODE_PIN_PWM_HZ_MIN, 0U));
    TEST_ASSERT_EQUAL_UINT16(0U, s_open_cfg.duty_x10);
    TEST_ASSERT_EQUAL_UINT32(KODE_PIN_PWM_HZ_MIN, s_open_cfg.hz);
    pin_done();
}

TEST_CASE("a pin outside 1 to 14 is refused by every call", "[kode_pin]")
{
    pin_fresh();
    TEST_ASSERT_FALSE(kode_pin_write(PIN_BELOW, true));
    TEST_ASSERT_EQUAL_STRING("pin 0 does not exist: use 1 to 14", kode_error());
    TEST_ASSERT_FALSE(kode_pin_read(PIN_ABOVE));
    TEST_ASSERT_EQUAL_STRING("pin 15 does not exist: use 1 to 14", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(PIN_ABOVE));
    TEST_ASSERT_EQUAL_STRING("pin 15 does not exist: use 1 to 14", kode_error());
    TEST_ASSERT_FALSE(kode_pin_write_pwm(PIN_BELOW, PIN_HZ, PIN_DUTY_PCT));
    TEST_ASSERT_EQUAL_STRING("pin 0 does not exist: use 1 to 14", kode_error());
    pin_done();
}

TEST_CASE("a pin lent to a module is refused naming it", "[kode_pin]")
{
    uint8_t pos  = PIN_ADC_POS;
    uint8_t gpio = 0U;

    pin_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_lend(&pos, 1U, PIN_LENT_TAG, &gpio));

    TEST_ASSERT_FALSE(kode_pin_write(PIN_ADC_POS, true));
    TEST_ASSERT_EQUAL_STRING("pin 8 is lent to mesh", kode_error());
    TEST_ASSERT_FALSE(kode_pin_read(PIN_ADC_POS));
    TEST_ASSERT_EQUAL_STRING("pin 8 is lent to mesh", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(PIN_ADC_POS));
    TEST_ASSERT_EQUAL_STRING("pin 8 is lent to mesh", kode_error());
    TEST_ASSERT_FALSE(kode_pin_write_pwm(PIN_ADC_POS, PIN_HZ, PIN_DUTY_PCT));
    TEST_ASSERT_EQUAL_STRING("pin 8 is lent to mesh", kode_error());

    /* The module gives it back and the pin is the app's again. */
    khal_gpio_release_Ignore();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_return(PIN_LENT_TAG));
    khal_gpio_release_StopIgnore();
    khal_gpio_output_ExpectAndReturn(PIN_ADC_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_ADC_POS, true));
    pin_done();
}

TEST_CASE("a pwm pin written becomes an output", "[kode_pin]")
{
    pin_fresh();
    TEST_ASSERT_TRUE(kode_pin_write_pwm(PIN_OUT_POS, PIN_HZ, PIN_DUTY_PCT));

    khal_pwm_close_Expect(PIN_PWM_HANDLE);
    khal_gpio_output_ExpectAndReturn(PIN_OUT_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_OUT_POS, true));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_OUT, pin_mode(PIN_OUT_POS));
    pin_done();
}

TEST_CASE("an analog pin read digitally becomes an input and a pwm pin read does too", "[kode_pin]")
{
    pin_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(PIN_ADC_CHAN, KODE_OK);
    port_reads(PIN_ADC_MV, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(PIN_ADC_MV, kode_pin_read_analog(PIN_ADC_POS));

    khal_exp_port_adc_close_Expect(PIN_ADC_CHAN);
    khal_gpio_input_ExpectAndReturn(PIN_ADC_GPIO, KHAL_GPIO_PULL_NONE, KODE_OK);
    khal_gpio_level_ExpectAndReturn(PIN_ADC_GPIO, false);
    TEST_ASSERT_FALSE(kode_pin_read(PIN_ADC_POS));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_ADC_POS));

    TEST_ASSERT_TRUE(kode_pin_write_pwm(PIN_ADC_POS, PIN_HZ, PIN_DUTY_PCT));
    khal_pwm_close_Expect(PIN_PWM_HANDLE);
    khal_gpio_input_ExpectAndReturn(PIN_ADC_GPIO, KHAL_GPIO_PULL_NONE, KODE_OK);
    khal_gpio_level_ExpectAndReturn(PIN_ADC_GPIO, true);
    TEST_ASSERT_TRUE(kode_pin_read(PIN_ADC_POS));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_IN, pin_mode(PIN_ADC_POS));
    pin_done();
}

TEST_CASE("an output read analog becomes analog", "[kode_pin]")
{
    pin_fresh();
    khal_gpio_output_ExpectAndReturn(PIN_ADC_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_ADC_POS, true));

    khal_exp_port_adc_open_ExpectAndReturn(PIN_ADC_CHAN, KODE_OK);
    port_reads(PIN_ADC_MV, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(PIN_ADC_MV, kode_pin_read_analog(PIN_ADC_POS));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_ADC, pin_mode(PIN_ADC_POS));
    pin_done();
}

TEST_CASE("a success clears the error a failure left", "[kode_pin]")
{
    pin_fresh();
    TEST_ASSERT_FALSE(kode_pin_write(PIN_BELOW, true));
    TEST_ASSERT_NOT_EQUAL(0, kode_error()[0]);
    khal_gpio_output_ExpectAndReturn(PIN_OUT_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_OUT_POS, true));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    pin_done();
}

TEST_CASE("readying pins for a bus releases what kode_pin made them", "[kode_pin]")
{
    uint8_t const pins[] = { PIN_OUT_POS, PIN_ADC_POS };

    pin_fresh();
    TEST_ASSERT_TRUE(kode_pin_write_pwm(PIN_OUT_POS, PIN_HZ, PIN_DUTY_PCT));
    khal_exp_port_adc_open_ExpectAndReturn(PIN_ADC_CHAN, KODE_OK);
    port_reads(PIN_ADC_MV, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(PIN_ADC_MV, kode_pin_read_analog(PIN_ADC_POS));

    khal_pwm_close_Expect(PIN_PWM_HANDLE);
    khal_gpio_release_Expect(PIN_OUT_GPIO);
    khal_exp_port_adc_close_Expect(PIN_ADC_CHAN);
    khal_gpio_release_Expect(PIN_ADC_GPIO);
    TEST_ASSERT_TRUE(kode_pin_lock_take("test_kode_pin"));
    TEST_ASSERT_TRUE(kode_pin_ready_for_bus(pins, 2U));
    kode_pin_lock_give();
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_OUT_POS));
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, pin_mode(PIN_ADC_POS));
    pin_done();
}

TEST_CASE("readying pins for a bus judges them all before it releases any", "[kode_pin]")
{
    uint8_t const pins[] = { PIN_OUT_POS, PIN_ADC_POS };
    uint8_t       pos    = PIN_ADC_POS;
    uint8_t       gpio   = 0U;

    pin_fresh();
    khal_gpio_output_ExpectAndReturn(PIN_OUT_GPIO, true, KODE_OK);
    TEST_ASSERT_TRUE(kode_pin_write(PIN_OUT_POS, true));
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_lend(&pos, 1U, PIN_LENT_TAG, &gpio));

    TEST_ASSERT_TRUE(kode_pin_lock_take("test_kode_pin"));
    TEST_ASSERT_FALSE(kode_pin_ready_for_bus(pins, 2U));
    kode_pin_lock_give();
    TEST_ASSERT_EQUAL_STRING("pin 8 is lent to mesh", kode_error());
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_OUT, pin_mode(PIN_OUT_POS));

    khal_gpio_release_Expect(PIN_ADC_GPIO);
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_return(PIN_LENT_TAG));
    pin_done();
}
