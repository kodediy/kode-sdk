/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_buttons.c
 * @brief The six buttons: five decoded from the expander's byte, one read here.
 */
#include "kdrv_buttons.h"

#include "kbsp.h"
#include "khal_gpio.h"
#include "kode_log.h"

#define TAG "kdrv.buttons"

/** Which expander bit each button is, or NOT_ON_EXP for the one that is not there. */
#define NOT_ON_EXP 0xFFU
static const uint8_t EXP_BIT[KDRV_BTN_COUNT] = {
    [KDRV_BTN_UP]    = KBSP_IOEXP_BIT_PAD_UP,
    [KDRV_BTN_DOWN]  = KBSP_IOEXP_BIT_PAD_DOWN,
    [KDRV_BTN_LEFT]  = KBSP_IOEXP_BIT_PAD_LEFT,
    [KDRV_BTN_RIGHT] = KBSP_IOEXP_BIT_PAD_RIGHT,
    [KDRV_BTN_TOP]   = NOT_ON_EXP,   /* GPIO35 */
    [KDRV_BTN_BOT]   = KBSP_IOEXP_BIT_BTN_BOT,
};

/* "pad-" is not decoration. The state words are "pressed" and "released", and
 * a button called "down" next to one of those reads as neither a name nor a
 * state - "down down" was a real log line and it explained nothing. */
static const char *const NAMES[KDRV_BTN_COUNT] = {
    [KDRV_BTN_UP]    = "pad-up",
    [KDRV_BTN_DOWN]  = "pad-down",
    [KDRV_BTN_LEFT]  = "pad-left",
    [KDRV_BTN_RIGHT] = "pad-right",
    [KDRV_BTN_TOP]   = "top",
    [KDRV_BTN_BOT]   = "bottom",
};

kode_status_t kdrv_buttons_init(void)
{
    /* R38 is on the board; the internal pull-up costs nothing and covers a
     * board without it. */
    if ( khal_gpio_input(KBSP_PIN_BUTTON_TOP, KHAL_GPIO_PULL_UP) != KODE_OK )
    {
        KLOG_E(TAG, "cannot configure GPIO%u", (unsigned)KBSP_PIN_BUTTON_TOP);
        return KODE_ERROR;
    }
    KLOG_D(TAG, "top button on GPIO%u; the other five arrive from the expander",
           (unsigned)KBSP_PIN_BUTTON_TOP);
    return KODE_OK;
}

static void (*s_top_cb)(void *arg);   /* The owner's edge. */
static void *s_top_arg;               /* Its argument. */
static volatile bool s_top_wake;      /* The pin is armed as a light-sleep wake. */

/** @brief GPIO35 moved. Interrupt context: withdraw the wake, then call the owner. */
static void top_isr(void *arg)
{
    (void)arg;
    if ( s_top_wake )
    {
        s_top_wake = false;
        (void)khal_gpio_wake(KBSP_PIN_BUTTON_TOP, false);
    }
    s_top_cb(s_top_arg);
}

kode_status_t kdrv_buttons_top_attach(void (*cb)(void *arg), void *arg)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    s_top_cb  = cb;
    s_top_arg = arg;
    return khal_gpio_irq_attach(KBSP_PIN_BUTTON_TOP, KHAL_GPIO_IRQ_ANY_EDGE, &top_isr, NULL);
}

kode_status_t kdrv_buttons_top_sleep_wake(bool armed)
{
    kode_status_t rc = KODE_OK;

    if ( armed )
    {
        rc = khal_gpio_wake_low(KBSP_PIN_BUTTON_TOP);
        s_top_wake = (KODE_OK == rc);
    }
    else if ( s_top_wake )
    {
        s_top_wake = false;
        rc = khal_gpio_wake(KBSP_PIN_BUTTON_TOP, false);
    }
    else
    {
        /* The press withdrew it, or it was never armed. */
    }
    return rc;
}

void kdrv_buttons_top_rearm(void)
{
    khal_gpio_irq_enable(KBSP_PIN_BUTTON_TOP);
}

bool kdrv_buttons_top_pressed(void)
{
    /* Active low: high is at rest. */
    return !khal_gpio_level(KBSP_PIN_BUTTON_TOP);
}

uint8_t kdrv_buttons_decode(uint8_t exp_levels, bool top_pressed)
{
    uint8_t mask = 0U;

    for ( uint8_t b = 0U; (uint8_t)KDRV_BTN_COUNT > b; ++b )
    {
        bool pressed;
        if ( NOT_ON_EXP == EXP_BIT[b] )
        {
            pressed = top_pressed;
        }
        else
        {
            /* Active low, all five: high is at rest. */
            pressed = ((exp_levels & (uint8_t)(1U << EXP_BIT[b])) == 0U);
        }
        if ( pressed )
        {
            mask |= (uint8_t)(1U << b);
        }
    }
    return mask;
}

const char *kdrv_button_name(kdrv_button_t b)
{
    if ( KDRV_BTN_COUNT <= b )
    {
        return "?";
    }
    return NAMES[b];
}
