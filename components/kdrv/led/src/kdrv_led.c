/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_led.c
 * @brief The status LED: an RGB behind a KTD2026 current-sink driver (U8).
 */
#include "kdrv_led.h"

#include "kbsp.h"
#include "khal_i2c.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kdrv.led"

/* ---- KTD2026 register map ------------------------------------------------ */
#define KTD_REG_EN_RST      0x00  /* [2:0] reset/timer slot, [4:3] enable ctrl */
#define KTD_REG_LED_EN      0x04  /* 2 bits per channel                        */
#define KTD_REG_ISET1       0x06  /* OUT1 current, 0.125 mA/LSB                */
#define KTD_REG_ISET2       0x07
#define KTD_REG_ISET3       0x08

#define KTD_RESET_FULL      0x07  /* full register reset; wait ~200us after   */
/* Never 0x00 while lit: with En Ctrl 00 the chip shuts down whenever SCL or SDA goes low. */
#define KTD_EN_NORMAL       0x18  /* Tslot1 + "always on" + 1x ramp scale     */
#define KTD_EN_SLEEP        0x08  /* Tslot1 + "on while SDA toggles": asleep  */

#define KTD_CH_OFF          0x00
#define KTD_CH_ON           0x01
#define KTD_LED_EN(l1, l2, l3)  ((uint8_t)(((l3) << 4) | ((l2) << 2) | (l1)))

/* Full-scale current per channel. Conservative on purpose: the chip goes to
 * 24 mA, but this LED is a status indicator inside a case, not a torch. */
#define KTD_FULL_SCALE_MA   5.0f
#define KTD_PROBE_MS        100U
#define KTD_PROBE_TRIES     3U    /* probes of the bring-up before the part is called absent */
#define KTD_PROBE_GAP_MS    20U   /* wait between two of them */

static khal_i2c_dev_t s_dev;
static uint8_t        s_r, s_g, s_b;   /* last colour asked for */
static bool           s_on;
static bool           s_led_asleep;  /* Reg0 holds KTD_EN_SLEEP: nothing is lit */

/**
 * @brief Put the chip to sleep: nothing is lit, and the channels are off.
 *
 * A write that does not land leaves it awake, which costs 190 uA and nothing
 * else; the next dark write tries again.
 */
static void led_sleep(void)
{
    if ( !s_led_asleep
        && (khal_i2c_write_reg(s_dev, KTD_REG_EN_RST, KTD_EN_SLEEP) == KODE_OK) )
    {
        s_led_asleep = true;
    }
}

/**
 * @brief Wake the chip before anything is lit.
 *
 * Asleep, the chip wakes on SDA toggling, and this write is the toggling: if
 * its address byte arrives before the chip is up, it is asked once more.
 *
 * @return KODE_OK, or the bus's error when neither attempt landed.
 */
static kode_status_t led_wake(void)
{
    kode_status_t st = KODE_OK;

    if ( s_led_asleep )
    {
        st = khal_i2c_write_reg(s_dev, KTD_REG_EN_RST, KTD_EN_NORMAL);
        if ( KODE_OK != st )
        {
            st = khal_i2c_write_reg(s_dev, KTD_REG_EN_RST, KTD_EN_NORMAL);
        }
        if ( KODE_OK == st )
        {
            s_led_asleep = false;
        }
    }
    return st;
}

/**
 * @brief 0..255 -> KTD current code.
 *
 * I = 0.125*(code+1) mA, so code 0 is already a lit channel; "off" is expressed
 * in the channel-enable register instead.
 *
 * @param level The brightness, 0..255.
 * @return The current code, 0x00..0xBF (24 mA, the chip's ceiling).
 */
static uint8_t level_to_code(uint8_t level)
{
    float const ma = (KTD_FULL_SCALE_MA * (float)level) / 255.0f;
    int code = (int)((ma / 0.125f) + 0.5f) - 1;
    if ( 0 > code )
    {
        code = 0;
    }
    else if ( 0xBF < code )
    {
        code = 0xBF;   /* 24 mA, the chip's ceiling */
    }
    return (uint8_t)code;
}

/** @brief Does the KTD2026 ACK, given a few tries on a bus another device may hold? */
static bool led_probe(void)
{
    for ( uint8_t i = 0U; KTD_PROBE_TRIES > i; ++i )
    {
        if ( khal_i2c_probe(KBSP_I2C_ADDR_LED, KTD_PROBE_MS) )
        {
            return true;
        }
        if ( (uint8_t)(i + 1U) < KTD_PROBE_TRIES )
        {
            khal_delay_ms(KTD_PROBE_GAP_MS);
        }
    }
    return false;
}

kode_status_t kdrv_led_park(void)
{
    /* One quiet probe first: a part that is not fitted is a fact here, said
     * once by the caller, and no warning of the bring-up's. */
    if ( (khal_i2c_init() != KODE_OK) || !khal_i2c_probe(KBSP_I2C_ADDR_LED, KTD_PROBE_MS) )
    {
        return KODE_ERROR;
    }
    return kdrv_led_init();
}

kode_status_t kdrv_led_init(void)
{
    if ( NULL != s_dev )
    {
        return KODE_OK;
    }

    kode_status_t st = khal_i2c_init();
    if ( KODE_OK != st )
    {
        return st;
    }
    if ( !led_probe() )
    {
        KLOG_E(TAG, "no ACK at 0x%02X in %u tries - LED driver absent?",
               (unsigned)KBSP_I2C_ADDR_LED, (unsigned)KTD_PROBE_TRIES);
        return KODE_ERROR;
    }
    st = khal_i2c_add(KBSP_I2C_ADDR_LED, &s_dev);
    if ( KODE_OK != st )
    {
        s_dev = NULL;
        return st;
    }

    /* The full reset is not acknowledged: the chip resets on the write and
     * NACKs it, every time, so its status says nothing and is cast away. The
     * enable control after it is the load-bearing write (KTD_EN_NORMAL):
     * a chip that missed it sits in En Ctrl = 00 and goes dark the
     * first time anything else drives the bus, while every write after it
     * still ACKs. So a miss there is not remembered as "up": the device goes
     * back and the handle is forgotten, and the next kdrv_led_rgb() - which
     * brings the chip up on entry - runs the whole sequence again. */
    (void)khal_i2c_write_reg(s_dev, KTD_REG_EN_RST, KTD_RESET_FULL);
    khal_delay_ms(1U);                            /* datasheet: ~200us */
    st = khal_i2c_write_reg(s_dev, KTD_REG_EN_RST, KTD_EN_NORMAL);
    if ( KODE_OK != st )
    {
        KLOG_E(TAG, "KTD2026 did not take its enable control; "
                    "not up, the next call retries");
        (void)khal_i2c_remove(s_dev);
        s_dev = NULL;
        return st;
    }

    /* Start dark, with a colour ready for a bare kdrv_led_set(true), and
     * asleep until something is lit. */
    s_r = s_g = s_b = 0xFFU;
    s_on = false;
    s_led_asleep = false;
    (void)khal_i2c_write_reg(s_dev, KTD_REG_LED_EN,
                             KTD_LED_EN(KTD_CH_OFF, KTD_CH_OFF, KTD_CH_OFF));
    led_sleep();

    KLOG_D(TAG, "KTD2026 ready at 0x%02X (RGB, %.1fmA full scale)",
           (unsigned)KBSP_I2C_ADDR_LED, KTD_FULL_SCALE_MA);
    return KODE_OK;
}

/** Push a colour to the hardware without touching the remembered one. */
static kode_status_t led_drive(uint8_t r, uint8_t g, uint8_t b)
{
    bool const lit = ((r | g | b) != 0U);
    kode_status_t st = KODE_OK;

    if ( lit )
    {
        st = led_wake();
    }
    /* OUT1 is the green die, OUT2 the blue, OUT3 the red (see the top of
     * this file): the sinks are written in OUT order, the colours land where
     * the board put them. */
    if ( KODE_OK == st )
    {
        st = khal_i2c_write_reg(s_dev, KTD_REG_ISET1, level_to_code(g));
    }
    if ( KODE_OK == st )
    {
        st = khal_i2c_write_reg(s_dev, KTD_REG_ISET2, level_to_code(b));
    }
    if ( KODE_OK == st )
    {
        st = khal_i2c_write_reg(s_dev, KTD_REG_ISET3, level_to_code(r));
    }
    if ( KODE_OK == st )
    {
        uint8_t on1 = KTD_CH_OFF;
        uint8_t on2 = KTD_CH_OFF;
        uint8_t on3 = KTD_CH_OFF;

        if ( 0U < g )
        {
            on1 = KTD_CH_ON;
        }
        if ( 0U < b )
        {
            on2 = KTD_CH_ON;
        }
        if ( 0U < r )
        {
            on3 = KTD_CH_ON;
        }
        st = khal_i2c_write_reg(s_dev, KTD_REG_LED_EN, KTD_LED_EN(on1, on2, on3));
    }
    if ( (KODE_OK == st) && !lit )
    {
        led_sleep();   /* the channels are off: the datasheet's order */
    }
    return st;
}

kode_status_t kdrv_led_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    kode_status_t const st = kdrv_led_init();
    if ( KODE_OK != st )
    {
        return st;
    }
    /* (0,0,0) means "off", not "black is the new colour": keep the last real
     * colour so a later kdrv_led_set(true) restores it. */
    if ( (r | g | b) != 0U )
    {
        s_r = r;
        s_g = g;
        s_b = b;
    }
    s_on = ((r | g | b) != 0U);
    return led_drive(r, g, b);
}

kode_status_t kdrv_led_set(bool on)
{
    /* Bring the chip up first: the default colour is only known after
     * kdrv_led_init(), and reading s_* before it would light nothing at all
     * on the very first "led on". */
    kode_status_t const st = kdrv_led_init();
    if ( KODE_OK != st )
    {
        return st;
    }
    return on ? kdrv_led_rgb(s_r, s_g, s_b) : kdrv_led_rgb(0U, 0U, 0U);
}

kode_status_t kdrv_led_toggle(void)
{
    return kdrv_led_set(!s_on);
}
