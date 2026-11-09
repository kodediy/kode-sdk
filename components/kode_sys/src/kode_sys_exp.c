/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_exp.c
 * @brief The expansion header's doors: each claims J3 for the app, then forwards to khal_exp.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"

#include "kbsp_owners.h"
#include "khal_exp.h"

#include <string.h>

_Static_assert(KODE_SYS_EXP_POS_MIN == KHAL_EXP_POS_MIN, "the first position is the header's");
_Static_assert(KODE_SYS_EXP_POS_MAX == KHAL_EXP_POS_MAX, "the last position is the header's");
_Static_assert(KODE_SYS_EXP_PWM_HZ_MIN == KHAL_EXP_PWM_HZ_MIN, "the lowest frequency is the header's");
_Static_assert(KODE_SYS_EXP_PWM_HZ_MAX == KHAL_EXP_PWM_HZ_MAX, "the highest frequency is the header's");
_Static_assert(KODE_SYS_EXP_PWM_DUTY_MAX == KHAL_EXP_PWM_DUTY_MAX, "full duty is the header's");
_Static_assert((uint32_t)KHAL_GPIO_PULL_NONE == (uint32_t)KODE_SYS_GPIO_PULL_NONE, "pulls follow khal's");
_Static_assert((uint32_t)KHAL_GPIO_PULL_UP == (uint32_t)KODE_SYS_GPIO_PULL_UP, "pulls follow khal's");
_Static_assert((uint32_t)KHAL_GPIO_PULL_DOWN == (uint32_t)KODE_SYS_GPIO_PULL_DOWN, "pulls follow khal's");
_Static_assert((uint32_t)KHAL_GPIO_IRQ_LOW_LEVEL == (uint32_t)KODE_SYS_GPIO_IRQ_LOW_LEVEL, "interrupt kinds follow khal's");
_Static_assert((uint32_t)KHAL_GPIO_IRQ_ANY_EDGE == (uint32_t)KODE_SYS_GPIO_IRQ_ANY_EDGE, "interrupt kinds follow khal's");
_Static_assert((uint32_t)KHAL_GPIO_IRQ_HIGH_LEVEL == (uint32_t)KODE_SYS_GPIO_IRQ_HIGH_LEVEL, "interrupt kinds follow khal's");
_Static_assert((uint32_t)KHAL_EXP_FREE == (uint32_t)KODE_SYS_EXP_FREE, "position modes follow khal's");
_Static_assert((uint32_t)KHAL_EXP_IN == (uint32_t)KODE_SYS_EXP_IN, "position modes follow khal's");
_Static_assert((uint32_t)KHAL_EXP_OUT == (uint32_t)KODE_SYS_EXP_OUT, "position modes follow khal's");
_Static_assert((uint32_t)KHAL_EXP_ADC == (uint32_t)KODE_SYS_EXP_ADC, "position modes follow khal's");
_Static_assert((uint32_t)KHAL_EXP_PWM == (uint32_t)KODE_SYS_EXP_PWM, "position modes follow khal's");
_Static_assert((uint32_t)KHAL_EXP_LENT == (uint32_t)KODE_SYS_EXP_LENT, "position modes follow khal's");
_Static_assert((uint32_t)KBSP_EXP_I2C == (uint32_t)KODE_SYS_EXP_I2C, "controller kinds follow kbsp's");
_Static_assert((uint32_t)KBSP_EXP_SPI == (uint32_t)KODE_SYS_EXP_SPI, "controller kinds follow kbsp's");
_Static_assert((uint32_t)KBSP_EXP_UART == (uint32_t)KODE_SYS_EXP_UART, "controller kinds follow kbsp's");

static uint8_t s_exp_token;   /**< Its address is the owner token the table holds. */

/**
 * @brief Hold the header for the app, or say who else does.
 * @return true when the app holds it, false when something else already does.
 */
static bool exp_claim(void)
{
    return kbsp_claim(KBSP_RES_EXP, &s_exp_token, "app (kode facade)");
}

kode_status_t kode_sys_exp_input(uint8_t pos, kode_sys_gpio_pull_t bias)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_input(pos, (khal_gpio_pull_t)bias);
}

kode_status_t kode_sys_exp_output(uint8_t pos, bool level)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_output(pos, level);
}

kode_status_t kode_sys_exp_write(uint8_t pos, bool level)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_write(pos, level);
}

bool kode_sys_exp_level(uint8_t pos)
{
    if ( !exp_claim() )
    {
        return false;
    }
    return khal_exp_level(pos);
}

kode_status_t kode_sys_exp_pwm(uint8_t pos, uint32_t hz, uint16_t duty_x10)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_pwm(pos, hz, duty_x10);
}

kode_status_t kode_sys_exp_irq(uint8_t pos, kode_sys_gpio_irq_t kind, kode_sys_gpio_cb_t cb,
                               void *arg)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_irq_attach(pos, (khal_gpio_irq_t)kind, cb, arg);
}

void kode_sys_exp_irq_enable(uint8_t pos)
{
    if ( exp_claim() )
    {
        khal_exp_irq_enable(pos);
    }
}

kode_status_t kode_sys_exp_release(uint8_t pos)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_release(pos);
}

kode_status_t kode_sys_exp_info(uint8_t pos, kode_sys_exp_info_t *out)
{
    khal_exp_info_t info;

    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    if ( (NULL == out) || (KHAL_EXP_POS_MIN > pos) || (KHAL_EXP_POS_MAX < pos) )
    {
        return KODE_DENIED;
    }
    khal_exp_info(pos, &info);
    out->gpio = info.gpio;
    out->adc  = info.adc;
    out->lp   = info.lp;
    out->mode = (kode_sys_exp_mode_t)info.mode;
    out->tag  = info.tag;
    return KODE_OK;
}

kode_status_t kode_sys_exp_lend(const uint8_t *pos, size_t n, const char *tag,
                            uint8_t *gpio)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_lend(pos, n, tag, gpio);
}

kode_status_t kode_sys_exp_return(const char *tag)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_return(tag);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_exp_ctrl(kode_sys_exp_bus_t bus, const char *tag, uint8_t *id)
{
    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    return khal_exp_ctrl((kbsp_exp_bus_t)bus, tag, id);
}

/* khal_exp_adc_read() refuses a position that is not analog, so configure it here on the first call. */
kode_status_t kode_sys_exp_adc(uint8_t pos, uint16_t *raw, int32_t *mv,
                           bool *calibrated)
{
    khal_exp_info_t info;

    if ( !exp_claim() )
    {
        return KODE_EXISTS;
    }
    /* Zeroed (KHAL_EXP_FREE): khal_exp_info() leaves it untouched for a position off the connector. */
    (void)memset(&info, 0, sizeof(info));
    khal_exp_info(pos, &info);
    if ( KHAL_EXP_ADC != info.mode )
    {
        kode_status_t const rc = khal_exp_adc(pos);
        if ( KODE_OK != rc )
        {
            return rc;
        }
    }
    return khal_exp_adc_read(pos, raw, mv, calibrated);
}
