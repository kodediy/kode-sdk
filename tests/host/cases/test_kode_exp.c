/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_exp.c
 * @brief The header facade: claims for the app on first use, refuses when owned.
 */
#include "unity.h"

#include "Mockkhal_exp_port.h"
#include "Mockkhal_gpio.h"
#include "Mockkhal_pwm.h"

#include <stdatomic.h>
#include <string.h>

#include "fakes.h"
#include "khal_exp.h"
#include "khal_once.h"
#include "kbsp_owners.h"
#include "kode.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"

/** @brief A token for an owner that is not the facade. */
static int s_tenant;

/**
 * @brief Put the position table back to FREE, whatever the last file left.
 *
 * The same arrangement, and the same three ignores, that test_khal_exp.c's
 * exp_fresh() uses: s_pos is static across the binary, so khal_exp_init()
 * has to close whatever ADC channel or PWM slot is still open before it can
 * say every position is free. Ignored around the init and only there - a
 * case that goes on to expect one of these wants its argument checked.
 */
static void exp_fresh(void)
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
}

/* Refusals come first: once the facade claims the header it keeps it for the rest of the binary. */
TEST_CASE("a facade call is KODE_EXISTS while an AO owns the header", "[kode_exp]")
{
    kode_sys_exp_info_t info;

    exp_fresh();
    /* test_console_exp.c may already have claimed for the bench; either way
     * the holder is not the facade's token, which is what matters here. */
    (void)kbsp_claim(KBSP_RES_EXP, &s_tenant, "AO_Tenant");
    TEST_ASSERT_EQUAL(KODE_EXISTS, kode_sys_exp_output(1U, true));
    TEST_ASSERT_EQUAL(KODE_EXISTS, kode_sys_exp_info(1U, &info));
    kbsp_release(KBSP_RES_EXP, &s_tenant);
    Mockkhal_gpio_Verify();   /* nothing touched the pin */
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

/* The standard layer's pins ask the same door, so the refusal is theirs too; it
 * is here for the order this file's first case explains. */
TEST_CASE("a pin call says the header is held while an AO owns it", "[kode_exp]")
{
    exp_fresh();
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    (void)kbsp_claim(KBSP_RES_EXP, &s_tenant, "AO_Tenant");
    TEST_ASSERT_FALSE(kode_pin_write(1U, true));
    TEST_ASSERT_EQUAL_STRING("the expansion header is held by something else in this image", kode_error());
    TEST_ASSERT_FALSE(kode_pin_read(1U));
    TEST_ASSERT_EQUAL_STRING("the expansion header is held by something else in this image", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_pin_read_analog(8U));
    TEST_ASSERT_EQUAL_STRING("the expansion header is held by something else in this image", kode_error());
    TEST_ASSERT_FALSE(kode_pin_write_pwm(1U, 1000U, 50U));
    TEST_ASSERT_EQUAL_STRING("the expansion header is held by something else in this image", kode_error());
    kbsp_release(KBSP_RES_EXP, &s_tenant);
    Mockkhal_gpio_Verify();   /* nothing touched the pin */
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

TEST_CASE("the first facade call claims the header for the app", "[kode_exp]")
{
    exp_fresh();
    /* Nobody holds it: the facade's own token gets it. */
    khal_gpio_output_ExpectAndReturn(8U, true, KODE_OK);
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_output(1U, true));
    TEST_ASSERT_EQUAL_STRING("app (kode facade)", kbsp_owner_name(KBSP_RES_EXP));
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

TEST_CASE("kode_sys_exp_adc configures on first read and reads after", "[kode_exp]")
{
    uint16_t raw = 0U;
    int32_t  mv  = 0;
    bool     cal = false;
    static uint16_t port_raw = 1U;
    static int32_t  port_mv  = 1;
    static bool     port_cal = true;

    exp_fresh();
    khal_exp_port_adc_open_ExpectAndReturn(0U, KODE_OK);
    khal_exp_port_adc_read_ExpectAndReturn(0U, NULL, NULL, NULL, KODE_OK);
    khal_exp_port_adc_read_IgnoreArg_raw();
    khal_exp_port_adc_read_IgnoreArg_mv();
    khal_exp_port_adc_read_IgnoreArg_calibrated();
    khal_exp_port_adc_read_ReturnThruPtr_raw(&port_raw);
    khal_exp_port_adc_read_ReturnThruPtr_mv(&port_mv);
    khal_exp_port_adc_read_ReturnThruPtr_calibrated(&port_cal);
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_adc(14U, &raw, &mv, &cal));
    Mockkhal_exp_port_Verify();
    Mockkhal_pwm_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

TEST_CASE("kode_sys_exp_info reports a position and refuses what is not one", "[kode_exp]")
{
    kode_sys_exp_info_t info;
    kode_sys_exp_info_t kept;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_info(8U, &info));
    TEST_ASSERT_EQUAL_UINT8(21U, info.gpio);
    TEST_ASSERT_TRUE(info.adc);
    TEST_ASSERT_EQUAL(KODE_SYS_EXP_FREE, info.mode);
    TEST_ASSERT_NULL(info.tag);

    /* Out of range and NULL are KODE_ERROR and leave the output as it was. */
    (void)memset(&kept, 0x5A, sizeof(kept));
    info = kept;
    TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_exp_info(0U, &info));
    TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_exp_info(15U, &info));
    TEST_ASSERT_EQUAL_MEMORY(&kept, &info, sizeof(info));
    TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_exp_info(1U, NULL));
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
    Mockkhal_pwm_Destroy();
}

/* The take shares exp_claim() with every other kode_sys_exp_* call, so the refusal
 * while an AO owns the header is the first case's; this one is the take. */
TEST_CASE("kode_sys_exp_ctrl claims the header and takes a controller", "[kode_exp]")
{
    uint8_t id = 0xFFU;

    exp_fresh();
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_ctrl(KODE_SYS_EXP_I2C, "fck", &id));
    TEST_ASSERT_EQUAL_UINT8(KBSP_EXP_I2C_ID, id);
    TEST_ASSERT_EQUAL_STRING("app (kode facade)", kbsp_owner_name(KBSP_RES_EXP));
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_exp_return("fck"));
    Mockkhal_gpio_Verify();
    Mockkhal_gpio_Destroy();
    Mockkhal_exp_port_Destroy();
}
