/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_led.c
 * @brief The KTD2026 driver, checked by what it says on the bus.
 */
#include "unity.h"

#include "Mockkhal_i2c.h"

#include "fakes.h"

#include "kbsp.h"
#include "kdrv_led.h"

/* The register map, repeated here on purpose. A test that imported the
 * driver's own #defines would agree with it by construction and could never
 * catch a changed constant - which is the thing most worth catching. */
#define LED_ADDR        0x30U
#define REG_EN_RST      0x00U
#define REG_LED_EN      0x04U
#define REG_ISET1       0x06U
#define REG_ISET2       0x07U
#define REG_ISET3       0x08U

#define RESET_FULL      0x07U
#define EN_NORMAL       0x18U
#define EN_SLEEP        0x08U   /* on only while SDA toggles: asleep at rest */

/* I = 0.125*(code+1) mA over a 5.0 mA full scale, so 255 is 39 and 0 is 0. */
#define CODE_FULL       39U
#define CODE_ZERO       0U

/* Any non-NULL address will do: the driver only ever passes this back to
 * khal_i2c, which is mocked. What matters is that it is not NULL - the
 * driver's own "already up" guard is `s_dev != NULL`, so a mock that left
 * the handle unset would send every later case through bring-up again. */
static khal_i2c_dev_t s_handle = (khal_i2c_dev_t)(void *)&s_handle;

/** @brief The bus side of a bring-up up to the handle: bus, ACK, attach. */
static void expect_attach(void)
{
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(LED_ADDR, 100U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
}

/* The load-bearing write. A chip that missed 0x18 ACKs everything after it
 * and goes dark the first time another device drives the bus - so a miss
 * must not be remembered as "up". The proof that it was not is the case
 * after this one: it expects the whole sequence again, from the probe. */
TEST_CASE("an enable control that did not land is not remembered as up",
          "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    expect_attach();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, RESET_FULL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_NORMAL, KODE_ERROR);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_remove_ExpectAndReturn(s_handle, KODE_OK);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_led_init());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* A bus another device holds NAKs a probe once in a while. The bring-up asks
 * again before it calls the part absent, and says so only when the last try
 * fails. */
TEST_CASE("three probes that NAK are an absent part, said once", "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(LED_ADDR, 100U, false);
    khal_i2c_probe_ExpectAndReturn(LED_ADDR, 100U, false);
    khal_i2c_probe_ExpectAndReturn(LED_ADDR, 100U, false);
    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_led_init());
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("bringing the LED up resets the chip, NACKed as the part does, and "
          "takes it out of shutdown",
          "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    /* The first probe NAKs, as it does when another device holds the bus; the
     * second ACKs, and the bring-up goes on without an error line. */
    khal_i2c_init_ExpectAndReturn(KODE_OK);
    khal_i2c_probe_ExpectAndReturn(LED_ADDR, 100U, false);
    khal_i2c_probe_ExpectAndReturn(LED_ADDR, 100U, true);
    khal_i2c_add_ExpectAnyArgsAndReturn(KODE_OK);
    khal_i2c_add_ReturnThruPtr_out(&s_handle);
    fake_klog_reset();

    /* A full reset, then "device always on". The chip resets on the first
     * write and never ACKs it, so the mock NACKs it the way the part does
     * and the bring-up goes on regardless. The second value is the one worth
     * pinning: with En Ctrl = 00 the chip drops into shutdown whenever SCL or
     * SDA go low, so traffic from any of the five other devices on this
     * shared bus would silently kill the LED. 0x18, never 0x00. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, RESET_FULL, KODE_ERROR);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_NORMAL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    /* and the channels left dark until somebody asks for a colour, with the
     * chip asleep: 190 uA awake with nothing lit, 0.1 uA asleep. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x00U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_SLEEP, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_led_init());
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());   /* the "ready" line, no error */

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("a colour is written as three currents and one enable mask",
          "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    /* Red only: the red die hangs on OUT3 (the board's wiring), so OUT3 at
     * full scale, OUT1 and OUT2 at zero, and an
     * enable mask that lights channel 3 alone - after the wake, since the
     * chip sleeps while nothing is lit. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_NORMAL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_FULL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x10U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_led_rgb(255U, 0U, 0U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

TEST_CASE("blue lights channel two, which is what pins OUT2 to B",
          "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_FULL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x04U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_led_rgb(0U, 0U, 255U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* (0,0,0) means "off", not "black is the new colour". The driver has to keep
 * the last real colour or a later "led on" lights nothing at all - which is
 * a bug that looks like dead hardware. */
TEST_CASE("turning off keeps the colour, and turning on restores it",
          "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    /* Off: every channel at zero current and an empty enable mask. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x00U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_SLEEP, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_led_set(false));

    /* On again: woken, and the blue from the previous case comes back, not
     * black. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_NORMAL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_FULL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x04U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_led_set(true));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The chip wakes on SDA toggling, and the write that wakes it is the toggling.
 * Should the address byte of that write land before the chip is up, it is
 * asked once more rather than reported as a dead LED. */
TEST_CASE("a wake the chip did not acknowledge is asked once more",
          "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    /* Put it to sleep. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x00U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_SLEEP, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_led_set(false));

    /* The wake: refused once, taken the second time, then the colour. */
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_NORMAL, KODE_ERROR);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_EN_RST, EN_NORMAL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET1, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET2, CODE_FULL, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_ISET3, CODE_ZERO, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    khal_i2c_write_reg_ExpectAndReturn(NULL, REG_LED_EN, 0x04U, KODE_OK);
    khal_i2c_write_reg_IgnoreArg_dev();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_led_set(true));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}

/* The bus failing partway is the case nobody exercises on a bench, because
 * making a real I2C write fail on demand is hard. Here it is one line. */
TEST_CASE("a failed write is reported and stops the rest", "[kdrv_led]")
{
    Mockkhal_i2c_Init();

    khal_i2c_write_reg_ExpectAnyArgsAndReturn(KODE_ERROR);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_led_rgb(255U, 255U, 255U));

    Mockkhal_i2c_Verify();
    Mockkhal_i2c_Destroy();
}
