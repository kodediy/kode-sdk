/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_pwm.c
 * @brief khal_pwm's table: which channel runs on which timer, under one lock.
 */
#include "khal_pwm.h"

#include <stddef.h>

#include "khal_pwm_port.h"
#include "khal_sync.h"
#include "kode_log.h"

#define TAG "khal.pwm"   /**< This file's log tag. */

/* duty_count() multiplies 2^bits by the duty in 32 bits. */
_Static_assert((UINT32_MAX / KHAL_PWM_DUTY_MAX) >= (1UL << KHAL_PWM_PORT_BITS_MAX),
               "2^KHAL_PWM_PORT_BITS_MAX times KHAL_PWM_DUTY_MAX must fit in 32 bits");

/** @brief One timer: its users, and what it runs at while it has any. */
typedef struct {
    uint8_t  users;   /**< Channels on it; 0 is free. */
    bool     own;     /**< Held by one own_timer channel: nobody joins it. */
    uint32_t hz;      /**< Its frequency while it has users. */
    uint8_t  bits;    /**< Its duty resolution while it has users. */
} pwm_timer_t;

/** @brief One channel. */
typedef struct {
    bool    open;       /**< Handed out. */
    uint8_t gpio;       /**< The pin it drives, while open. */
    uint8_t timer;      /**< The timer it runs on, while open. */
    uint8_t bits_req;   /**< The resolution it asked for: a number, or KHAL_PWM_BITS_WIDEST. */
} pwm_chan_t;

static khal_mutex_t s_pwm_lock;                    /**< Guards both tables; NULL before init. */
static pwm_timer_t  s_timer[KHAL_PWM_TIMERS];      /**< Every timer, free at start. */
static pwm_chan_t   s_chan[KHAL_PWM_CHANNELS];     /**< Every channel, free at start. */

/**
 * @brief The widest resolution the clock gives at @p hz.
 * @param hz The frequency, not zero.
 * @return 1..KHAL_PWM_PORT_BITS_MAX; 1 also when even one bit does not fit.
 */
static uint8_t bits_widest(uint32_t hz)
{
    uint8_t bits = (uint8_t)KHAL_PWM_PORT_BITS_MAX;

    while ( (1U < bits) && (((uint64_t)hz << bits) > KHAL_PWM_PORT_CLK_HZ) )
    {
        --bits;
    }
    return bits;
}

/**
 * @brief The resolution a request gets at @p hz, if the clock reaches it.
 * @param hz   The frequency, not zero.
 * @param req  A number of bits, or KHAL_PWM_BITS_WIDEST.
 * @param bits Written only when this answers true.
 * @return false when the resolution is out of range or past the clock at @p hz.
 */
static bool bits_resolve(uint32_t hz, uint8_t req, uint8_t *bits)
{
    uint8_t b = req;

    if ( KHAL_PWM_BITS_WIDEST == b )
    {
        b = bits_widest(hz);
    }
    if ( (uint8_t)KHAL_PWM_PORT_BITS_MAX < b )
    {
        return false;
    }
    if ( ((uint64_t)hz << b) > KHAL_PWM_PORT_CLK_HZ )
    {
        return false;
    }
    *bits = b;
    return true;
}

/**
 * @brief @p duty_x10 tenths of a percent as a count at @p bits.
 * @param bits     The timer's resolution.
 * @param duty_x10 0..KHAL_PWM_DUTY_MAX.
 * @return 0..2^bits inclusive, so 100.0 % is full on.
 */
static uint32_t duty_count(uint8_t bits, uint16_t duty_x10)
{
    /* The count that is full on. */
    uint32_t const on = 1UL << bits;

    return (on * (uint32_t)duty_x10) / KHAL_PWM_DUTY_MAX;
}

/**
 * @brief A shared timer already at @p hz and @p bits.
 * @param hz   The frequency.
 * @param bits The resolution.
 * @param skip A timer not to answer, or KHAL_PWM_NONE.
 * @return The timer, or KHAL_PWM_NONE.
 */
static uint8_t timer_shared(uint32_t hz, uint8_t bits, uint8_t skip)
{
    for ( uint8_t t = 0U; (uint8_t)KHAL_PWM_TIMERS > t; ++t )
    {
        if ( t != skip )
        {
            pwm_timer_t const * const tm = &s_timer[t];

            if ( (0U < tm->users) && !tm->own && (tm->hz == hz) && (tm->bits == bits) )
            {
                return t;
            }
        }
    }
    return KHAL_PWM_NONE;
}

/**
 * @brief The lowest timer with no users.
 * @return The timer, or KHAL_PWM_NONE.
 */
static uint8_t timer_free(void)
{
    for ( uint8_t t = 0U; (uint8_t)KHAL_PWM_TIMERS > t; ++t )
    {
        if ( 0U == s_timer[t].users )
        {
            return t;
        }
    }
    return KHAL_PWM_NONE;
}

/**
 * @brief The lowest channel not handed out.
 * @return The channel, or KHAL_PWM_NONE.
 */
static uint8_t chan_free(void)
{
    for ( uint8_t c = 0U; (uint8_t)KHAL_PWM_CHANNELS > c; ++c )
    {
        if ( !s_chan[c].open )
        {
            return c;
        }
    }
    return KHAL_PWM_NONE;
}

/**
 * @brief Record a timer just set up for its first channel.
 * @param t    The timer.
 * @param hz   Its frequency.
 * @param bits Its resolution.
 * @param own  Whether it is that channel's own.
 */
static void timer_claim(uint8_t t, uint32_t hz, uint8_t bits, bool own)
{
    s_timer[t].users = 0U;
    s_timer[t].own   = own;
    s_timer[t].hz    = hz;
    s_timer[t].bits  = bits;
}

/**
 * @brief Take a channel off its timer, stopping the timer when it was the last user.
 * @param ch The channel, open.
 */
static void timer_leave(uint8_t ch)
{
    uint8_t const t = s_chan[ch].timer;

    --s_timer[t].users;
    if ( 0U == s_timer[t].users )
    {
        khal_pwm_port_timer_stop(t);
        s_timer[t].own = false;
    }
}

/**
 * @brief Whether @p ch names an open channel.
 * @param ch The handle.
 * @return false for KHAL_PWM_NONE, a number past the table, or a closed channel.
 */
static bool chan_is_open(khal_pwm_t ch)
{
    return ( ((uint8_t)KHAL_PWM_CHANNELS > ch) && s_chan[ch].open );
}

kode_status_t khal_pwm_init(void)
{
    if ( NULL == s_pwm_lock )
    {
        s_pwm_lock = khal_mutex_create();
        if ( NULL == s_pwm_lock )
        {
            KLOG_E(TAG, "no lock for the LEDC table");
            return KODE_NO_RESOURCES;
        }
    }
    return KODE_OK;
}

/**
 * @brief khal_pwm_open() with the arguments checked and the lock held.
 * @param cfg  The request.
 * @param bits The resolution it resolved to.
 * @param out  The handle, written on success.
 * @return As khal_pwm_open().
 */
static kode_status_t open_locked(const khal_pwm_cfg_t *cfg, uint8_t bits,
                                 khal_pwm_t *out)
{
    uint8_t const ch = chan_free();
    uint8_t t = KHAL_PWM_NONE;
    bool fresh = false;

    if ( KHAL_PWM_NONE == ch )
    {
        KLOG_D(TAG, "all %u LEDC channels are in use", (unsigned)KHAL_PWM_CHANNELS);
        return KODE_NO_RESOURCES;
    }
    if ( !cfg->own_timer )
    {
        t = timer_shared(cfg->hz, bits, KHAL_PWM_NONE);
    }
    if ( KHAL_PWM_NONE == t )
    {
        t = timer_free();
        if ( KHAL_PWM_NONE == t )
        {
            KLOG_D(TAG, "no LEDC timer free for %lu Hz", (unsigned long)cfg->hz);
            return KODE_NO_RESOURCES;
        }
        if ( khal_pwm_port_timer_set(t, cfg->hz, bits) != KODE_OK )
        {
            return KODE_ERROR;
        }
        fresh = true;
    }
    if ( khal_pwm_port_chan_start(ch, cfg->gpio, t, duty_count(bits, cfg->duty_x10))
        != KODE_OK )
    {
        if ( fresh )
        {
            khal_pwm_port_timer_stop(t);
        }
        return KODE_ERROR;
    }
    if ( fresh )
    {
        timer_claim(t, cfg->hz, bits, cfg->own_timer);
    }
    ++s_timer[t].users;
    s_chan[ch].open     = true;
    s_chan[ch].gpio     = cfg->gpio;
    s_chan[ch].timer    = t;
    s_chan[ch].bits_req = cfg->bits;
    *out = ch;
    return KODE_OK;
}

kode_status_t khal_pwm_open(const khal_pwm_cfg_t *cfg, khal_pwm_t *out)
{
    uint8_t bits = 0U;
    kode_status_t rc;

    if ( NULL == out )
    {
        KLOG_D(TAG, "open: a handle to write is wanted");
        return KODE_DENIED;
    }
    *out = KHAL_PWM_NONE;
    if ( (NULL == cfg) || (NULL == s_pwm_lock) )
    {
        KLOG_D(TAG, "open: a request is wanted, after khal_pwm_init()");
        return KODE_DENIED;
    }
    if ( (0U == cfg->hz) || ((uint16_t)KHAL_PWM_DUTY_MAX < cfg->duty_x10)
        || !bits_resolve(cfg->hz, cfg->bits, &bits) )
    {
        KLOG_D(TAG, "open: %lu Hz at %u bits, duty %u is out of range",
               (unsigned long)cfg->hz, (unsigned)cfg->bits, (unsigned)cfg->duty_x10);
        return KODE_DENIED;
    }
    khal_mutex_lock(s_pwm_lock);
    rc = open_locked(cfg, bits, out);
    khal_mutex_unlock(s_pwm_lock);
    return rc;
}

/**
 * @brief Move an open channel to timer @p nt.
 * @param ch The channel, already bound to @p nt by the port.
 * @param nt The timer it runs on now.
 */
static void chan_move(uint8_t ch, uint8_t nt)
{
    timer_leave(ch);
    ++s_timer[nt].users;
    s_chan[ch].timer = nt;
}

/**
 * @brief khal_pwm_set() on an open channel, with the lock held.
 * @param ch       The channel.
 * @param hz       The frequency, not zero.
 * @param duty_x10 The duty, already checked.
 * @return As khal_pwm_set().
 */
static kode_status_t set_locked(uint8_t ch, uint32_t hz, uint16_t duty_x10)
{
    uint8_t const t = s_chan[ch].timer;
    uint8_t bits = 0U;
    uint8_t nt;

    if ( !bits_resolve(hz, s_chan[ch].bits_req, &bits) )
    {
        KLOG_D(TAG, "set: channel %u cannot run at %lu Hz", (unsigned)ch,
               (unsigned long)hz);
        return KODE_DENIED;
    }
    if ( (s_timer[t].hz == hz) && (s_timer[t].bits == bits) )
    {
        return khal_pwm_port_chan_duty(ch, duty_count(bits, duty_x10));
    }
    nt = KHAL_PWM_NONE;
    if ( !s_timer[t].own )
    {
        nt = timer_shared(hz, bits, t);
    }
    if ( KHAL_PWM_NONE != nt )
    {
        if ( khal_pwm_port_chan_bind(ch, nt, duty_count(bits, duty_x10)) != KODE_OK )
        {
            return KODE_ERROR;
        }
        chan_move(ch, nt);
        return KODE_OK;
    }
    if ( 1U == s_timer[t].users )
    {
        /* Its only user: retune in place. */
        if ( khal_pwm_port_timer_set(t, hz, bits) != KODE_OK )
        {
            return KODE_ERROR;
        }
        s_timer[t].hz   = hz;
        s_timer[t].bits = bits;
        return khal_pwm_port_chan_duty(ch, duty_count(bits, duty_x10));
    }
    nt = timer_free();
    if ( KHAL_PWM_NONE == nt )
    {
        KLOG_D(TAG, "set: no LEDC timer free for %lu Hz; channel %u keeps %lu Hz",
               (unsigned long)hz, (unsigned)ch, (unsigned long)s_timer[t].hz);
        return KODE_NO_RESOURCES;
    }
    if ( khal_pwm_port_timer_set(nt, hz, bits) != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( khal_pwm_port_chan_bind(ch, nt, duty_count(bits, duty_x10)) != KODE_OK )
    {
        khal_pwm_port_timer_stop(nt);
        return KODE_ERROR;
    }
    timer_claim(nt, hz, bits, false);
    chan_move(ch, nt);
    return KODE_OK;
}

kode_status_t khal_pwm_set(khal_pwm_t ch, uint32_t hz, uint16_t duty_x10)
{
    kode_status_t rc;

    if ( (NULL == s_pwm_lock) || (0U == hz) || ((uint16_t)KHAL_PWM_DUTY_MAX < duty_x10) )
    {
        KLOG_D(TAG, "set: %lu Hz, duty %u is out of range", (unsigned long)hz,
               (unsigned)duty_x10);
        return KODE_DENIED;
    }
    khal_mutex_lock(s_pwm_lock);
    if ( chan_is_open(ch) )
    {
        rc = set_locked(ch, hz, duty_x10);
    }
    else
    {
        KLOG_D(TAG, "set: channel %u is not open", (unsigned)ch);
        rc = KODE_DENIED;
    }
    khal_mutex_unlock(s_pwm_lock);
    return rc;
}

/**
 * @brief The own timer under @p ch, for pause and resume.
 * @param ch The handle.
 * @return The timer, or KHAL_PWM_NONE when @p ch is not open or not own_timer.
 */
static uint8_t own_timer_of(khal_pwm_t ch)
{
    if ( !chan_is_open(ch) )
    {
        return KHAL_PWM_NONE;
    }
    if ( !s_timer[s_chan[ch].timer].own )
    {
        return KHAL_PWM_NONE;
    }
    return s_chan[ch].timer;
}

kode_status_t khal_pwm_pause(khal_pwm_t ch)
{
    uint8_t const t = own_timer_of(ch);

    if ( KHAL_PWM_NONE == t )
    {
        KLOG_D(TAG, "pause: channel %u has no timer of its own", (unsigned)ch);
        return KODE_DENIED;
    }
    return khal_pwm_port_timer_pause(t);
}

kode_status_t khal_pwm_resume(khal_pwm_t ch)
{
    uint8_t const t = own_timer_of(ch);

    if ( KHAL_PWM_NONE == t )
    {
        KLOG_D(TAG, "resume: channel %u has no timer of its own", (unsigned)ch);
        return KODE_DENIED;
    }
    return khal_pwm_port_timer_resume(t);
}

void khal_pwm_close(khal_pwm_t ch)
{
    if ( NULL == s_pwm_lock )
    {
        return;
    }
    khal_mutex_lock(s_pwm_lock);
    if ( chan_is_open(ch) )
    {
        khal_pwm_port_chan_stop(ch, s_chan[ch].gpio);
        timer_leave(ch);
        s_chan[ch].open = false;
    }
    khal_mutex_unlock(s_pwm_lock);
}
