/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_exp.c
 * @brief khal_exp's table: portable, no lock, one owner.
 */
#include "khal_exp.h"

#include <string.h>

#include "kbsp_exp.h"
#include "khal_exp_port.h"
#include "khal_pwm.h"
#include "kode_log.h"

#define TAG "khal.exp"   /**< This file's log tag. */

/** @brief What one position holds. */
typedef struct {
    khal_exp_mode_t mode;     /**< What the position is doing. */
    const char     *tag;      /**< LENT only: who holds it. */
    khal_pwm_t      pwm;      /**< PWM only: the channel khal_pwm handed out. */
} slot_t;

static slot_t s_pos[KBSP_EXP_COUNT];   /**< One per position, in kbsp's order. */
/** @brief Who holds each of KBSP_EXP_CTRL's controllers; NULL while free. */
static const char *s_ctrl_tag[KBSP_EXP_CTRL_COUNT];

/* The header speaks khal_pwm's duty unit, so a duty passes through unscaled. */
_Static_assert(KHAL_EXP_PWM_DUTY_MAX == KHAL_PWM_DUTY_MAX,
               "khal_exp and khal_pwm must count duty in the same tenths of a percent");

/* idx_of() subtracts 1, which maps positions onto kbsp's table only while the counts agree. */
_Static_assert(KBSP_EXP_COUNT == KHAL_EXP_POS_MAX,
               "kbsp's table and the position range must be the same header");

/**
 * @brief Say, once and in the same words, that a position is not a header position.
 * @param ask The call that was refused, for the operator reading the log.
 * @param pos The position as it was asked for.
 */
static void deny_pos(const char *ask, uint8_t pos)
{
    KLOG_D(TAG, "%s EXP%u: the header has positions %u..%u", ask, (unsigned)pos,
           (unsigned)KHAL_EXP_POS_MIN, (unsigned)KHAL_EXP_POS_MAX);
}

/**
 * @brief Position to table index.
 * @param pos The position as it was asked for.
 * @return The index, or KBSP_EXP_COUNT when out of range.
 */
static size_t idx_of(uint8_t pos)
{
    if ( ((uint8_t)KHAL_EXP_POS_MIN > pos) || ((uint8_t)KHAL_EXP_POS_MAX < pos) )
    {
        return KBSP_EXP_COUNT;
    }
    return (size_t)pos - 1U;
}

/**
 * @brief Take a position out of its mode, giving back its ADC channel or PWM channel.
 * @param i The table index.
 * @return KODE_OK; KODE_DENIED for a LENT position, which only khal_exp_return() ends.
 */
static kode_status_t leave_mode(size_t i)
{
    slot_t * const s = &s_pos[i];

    if ( KHAL_EXP_LENT == s->mode )
    {
        KLOG_D(TAG, "EXP%u is lent to %s; return it first", (unsigned)(i + 1U), s->tag);
        return KODE_DENIED;
    }
    if ( KHAL_EXP_ADC == s->mode )
    {
        khal_exp_port_adc_close(KBSP_EXP[i].adc_chan);
    }
    if ( KHAL_EXP_PWM == s->mode )
    {
        khal_pwm_close(s->pwm);
    }
    s->mode = KHAL_EXP_FREE;
    s->tag  = NULL;
    return KODE_OK;
}

kode_status_t khal_exp_init(void)
{
    for ( size_t i = 0U; KBSP_EXP_COUNT > i; ++i )
    {
        /* A re-init voids loans, which leave_mode() would refuse; a loan holds no channel or slot. */
        if ( KHAL_EXP_LENT == s_pos[i].mode )
        {
            s_pos[i].mode = KHAL_EXP_FREE;
            s_pos[i].tag  = NULL;
        }
        /* Cannot refuse now; it closes the ADC channel or the PWM channel. */
        (void)leave_mode(i);
        khal_gpio_release(KBSP_EXP[i].gpio);
    }
    for ( size_t c = 0U; KBSP_EXP_CTRL_COUNT > c; ++c )
    {
        s_ctrl_tag[c] = NULL;
    }
    return KODE_OK;
}

kode_status_t khal_exp_input(uint8_t pos, khal_gpio_pull_t pull)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("input on", pos);
        return KODE_DENIED;
    }
    if ( leave_mode(i) != KODE_OK )
    {
        return KODE_DENIED;
    }
    if ( khal_gpio_input(KBSP_EXP[i].gpio, pull) != KODE_OK )
    {
        return KODE_ERROR;
    }
    s_pos[i].mode = KHAL_EXP_IN;
    return KODE_OK;
}

kode_status_t khal_exp_output(uint8_t pos, bool level)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("output on", pos);
        return KODE_DENIED;
    }
    if ( leave_mode(i) != KODE_OK )
    {
        return KODE_DENIED;
    }
    if ( khal_gpio_output(KBSP_EXP[i].gpio, level) != KODE_OK )
    {
        return KODE_ERROR;
    }
    s_pos[i].mode = KHAL_EXP_OUT;
    return KODE_OK;
}

kode_status_t khal_exp_write(uint8_t pos, bool level)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("write on", pos);
        return KODE_DENIED;
    }
    if ( KHAL_EXP_OUT != s_pos[i].mode )
    {
        KLOG_D(TAG, "write on EXP%u: it is not an output", (unsigned)pos);
        return KODE_DENIED;
    }
    return khal_gpio_write(KBSP_EXP[i].gpio, level);
}

/* khal_exp_level() and khal_exp_info() log nothing: `exp` polls all fourteen. */
bool khal_exp_level(uint8_t pos)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        return false;
    }
    if ( (KHAL_EXP_IN != s_pos[i].mode) && (KHAL_EXP_OUT != s_pos[i].mode) )
    {
        return false;
    }
    return khal_gpio_level(KBSP_EXP[i].gpio);
}

kode_status_t khal_exp_adc(uint8_t pos)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("adc on", pos);
        return KODE_DENIED;
    }
    if ( KBSP_EXP_NO_ADC == KBSP_EXP[i].adc_chan )
    {
        KLOG_D(TAG, "EXP%u has no ADC channel", (unsigned)pos);
        return KODE_DENIED;
    }
    if ( KHAL_EXP_ADC == s_pos[i].mode )
    {
        return KODE_OK;
    }
    if ( leave_mode(i) != KODE_OK )
    {
        return KODE_DENIED;
    }
    if ( khal_exp_port_adc_open(KBSP_EXP[i].adc_chan) != KODE_OK )
    {
        return KODE_ERROR;
    }
    s_pos[i].mode = KHAL_EXP_ADC;
    return KODE_OK;
}

kode_status_t khal_exp_adc_read(uint8_t pos, uint16_t *raw, int32_t *mv,
                                bool *calibrated)
{
    size_t const i = idx_of(pos);

    if ( (NULL == raw) || (NULL == mv) || (NULL == calibrated) )
    {
        KLOG_D(TAG, "adc_read on EXP%u: raw, mv and calibrated are all wanted",
               (unsigned)pos);
        return KODE_DENIED;
    }
    /* Filled before the table is judged: refusals print zeroes, not stack. */
    *raw        = 0U;
    *mv         = 0;
    *calibrated = false;
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("adc_read on", pos);
        return KODE_DENIED;
    }
    if ( KHAL_EXP_ADC != s_pos[i].mode )
    {
        KLOG_D(TAG, "adc_read on EXP%u: it is not an analog input",
               (unsigned)pos);
        return KODE_DENIED;
    }
    return khal_exp_port_adc_read(KBSP_EXP[i].adc_chan, raw, mv, calibrated);
}

kode_status_t khal_exp_pwm(uint8_t pos, uint32_t hz, uint16_t duty_x10)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("pwm on", pos);
        return KODE_DENIED;
    }
    if ( (KHAL_EXP_PWM_HZ_MIN > hz) || (KHAL_EXP_PWM_HZ_MAX < hz)
        || ((uint16_t)KHAL_EXP_PWM_DUTY_MAX < duty_x10) )
    {
        KLOG_D(TAG, "EXP%u: %lu Hz / %u is outside the PWM range",
               (unsigned)pos, (unsigned long)hz, (unsigned)duty_x10);
        return KODE_DENIED;
    }
    if ( KHAL_EXP_PWM == s_pos[i].mode )
    {
        return khal_pwm_set(s_pos[i].pwm, hz, duty_x10);
    }
    if ( KHAL_EXP_LENT == s_pos[i].mode )
    {
        KLOG_D(TAG, "pwm on EXP%u: it is lent to %s; return it first",
               (unsigned)pos, s_pos[i].tag);
        return KODE_DENIED;
    }
    /* The channel before the mode change, so a refusal leaves the position
     * exactly as it was. */
    const khal_pwm_cfg_t cfg = {
        .gpio      = KBSP_EXP[i].gpio,
        .hz        = hz,
        .bits      = KHAL_PWM_BITS_WIDEST,
        .duty_x10  = duty_x10,
        .own_timer = false,
    };
    khal_pwm_t ch = KHAL_PWM_NONE;
    kode_status_t const rc = khal_pwm_open(&cfg, &ch);
    if ( KODE_OK != rc )
    {
        KLOG_D(TAG, "EXP%u: no PWM channel at %lu Hz", (unsigned)pos,
               (unsigned long)hz);
        return rc;
    }
    /* Cannot refuse: LENT was refused above. */
    (void)leave_mode(i);
    s_pos[i].mode = KHAL_EXP_PWM;
    s_pos[i].pwm  = ch;
    return KODE_OK;
}

kode_status_t khal_exp_irq_attach(uint8_t pos, khal_gpio_irq_t kind,
                                  khal_gpio_cb_t cb, void *arg)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("irq_attach on", pos);
        return KODE_DENIED;
    }
    if ( KHAL_EXP_IN != s_pos[i].mode )
    {
        KLOG_D(TAG, "irq_attach on EXP%u: it is not an input", (unsigned)pos);
        return KODE_DENIED;
    }
    if ( NULL == cb )
    {
        KLOG_D(TAG, "irq_attach on EXP%u: a callback is wanted", (unsigned)pos);
        return KODE_DENIED;
    }
    return khal_gpio_irq_attach(KBSP_EXP[i].gpio, kind, cb, arg);
}

void khal_exp_irq_enable(uint8_t pos)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("irq_enable on", pos);
        return;
    }
    if ( KHAL_EXP_IN != s_pos[i].mode )
    {
        KLOG_D(TAG, "irq_enable on EXP%u: it is not an input", (unsigned)pos);
        return;
    }
    khal_gpio_irq_enable(KBSP_EXP[i].gpio);
}

kode_status_t khal_exp_release(uint8_t pos)
{
    size_t const i = idx_of(pos);
    if ( KBSP_EXP_COUNT <= i )
    {
        deny_pos("release on", pos);
        return KODE_DENIED;
    }
    if ( leave_mode(i) != KODE_OK )
    {
        return KODE_DENIED;
    }
    khal_gpio_release(KBSP_EXP[i].gpio);
    return KODE_OK;
}

kode_status_t khal_exp_lend(const uint8_t *pos, size_t n, const char *tag,
                            uint8_t *gpio)
{
    if ( (NULL == pos) || (NULL == tag) || (NULL == gpio) || (0U == n)
        || ((size_t)KHAL_EXP_POS_MAX < n) )
    {
        /* No tag to name here: a NULL one is exactly what this refuses. */
        KLOG_D(TAG, "lend: pos, tag and gpio are wanted, and n is 1..%u",
               (unsigned)KHAL_EXP_POS_MAX);
        return KODE_DENIED;
    }
    /* Check every position before marking any: all or nothing. */
    for ( size_t k = 0U; k < n; ++k )
    {
        size_t const i = idx_of(pos[k]);
        if ( KBSP_EXP_COUNT <= i )
        {
            deny_pos("lend of", pos[k]);
            return KODE_DENIED;
        }
        if ( KHAL_EXP_FREE != s_pos[i].mode )
        {
            KLOG_D(TAG, "EXP%u is not free for %s", (unsigned)pos[k], tag);
            return KODE_DENIED;
        }
    }
    for ( size_t k = 0U; k < n; ++k )
    {
        size_t const i = idx_of(pos[k]);
        s_pos[i].mode = KHAL_EXP_LENT;
        s_pos[i].tag  = tag;
        gpio[k]       = KBSP_EXP[i].gpio;
    }
    return KODE_OK;
}

kode_status_t khal_exp_return(const char *tag)
{
    if ( NULL == tag )
    {
        KLOG_D(TAG, "return: a tag is wanted; it is what names the loan");
        return KODE_DENIED;
    }
    for ( size_t i = 0U; KBSP_EXP_COUNT > i; ++i )
    {
        if ( (KHAL_EXP_LENT == s_pos[i].mode) && (NULL != s_pos[i].tag)
            && (strcmp(s_pos[i].tag, tag) == 0) )
        {
            s_pos[i].mode = KHAL_EXP_FREE;
            s_pos[i].tag  = NULL;
            khal_gpio_release(KBSP_EXP[i].gpio);
        }
    }
    for ( size_t c = 0U; KBSP_EXP_CTRL_COUNT > c; ++c )
    {
        if ( (NULL != s_ctrl_tag[c]) && (strcmp(s_ctrl_tag[c], tag) == 0) )
        {
            s_ctrl_tag[c] = NULL;
        }
    }
    return KODE_OK;
}

kode_status_t khal_exp_ctrl(kbsp_exp_bus_t bus, const char *tag, uint8_t *id)
{
    if ( (NULL == tag) || (NULL == id) || (KBSP_EXP_UART < bus) )
    {
        KLOG_D(TAG, "ctrl: a kind, a tag and somewhere to put the number are wanted");
        return KODE_DENIED;
    }
    for ( size_t c = 0U; KBSP_EXP_CTRL_COUNT > c; ++c )
    {
        if ( (KBSP_EXP_CTRL[c].bus == bus) && (NULL == s_ctrl_tag[c]) )
        {
            s_ctrl_tag[c] = tag;
            *id = KBSP_EXP_CTRL[c].id;
            return KODE_OK;
        }
    }
    KLOG_D(TAG, "ctrl for %s: every one of that kind is held", tag);
    return KODE_NO_RESOURCES;
}

void khal_exp_ctrl_info(size_t idx, khal_exp_ctrl_info_t *out)
{
    if ( (NULL == out) || (KBSP_EXP_CTRL_COUNT <= idx) )
    {
        return;
    }
    out->bus = KBSP_EXP_CTRL[idx].bus;
    out->id  = KBSP_EXP_CTRL[idx].id;
    out->tag = s_ctrl_tag[idx];
}

void khal_exp_info(uint8_t pos, khal_exp_info_t *out)
{
    size_t const i = idx_of(pos);
    if ( (KBSP_EXP_COUNT <= i) || (NULL == out) )
    {
        return;
    }
    out->gpio = KBSP_EXP[i].gpio;
    out->adc  = (KBSP_EXP_NO_ADC != KBSP_EXP[i].adc_chan);
    out->lp   = KBSP_EXP[i].lp;
    out->mode = s_pos[i].mode;
    out->tag  = s_pos[i].tag;
}
