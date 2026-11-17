/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_time.c
 * @brief A clock the case advances by hand, and delays that cost nothing.
 */
#include <string.h>
#include <sys/time.h>

#include "esp_timer.h"

#include "fakes.h"
#include "kode_log.h"
#include "khal_time.h"
#include "kqp_config.h"   /* KQP_TICKS_PER_SEC */

/** @brief Microseconds in a second, for the wall clock's whole seconds. */
#define FAKE_US_PER_S  1000000ULL

static uint64_t s_now_us;
static uint64_t s_wall_us;
static void   (*s_delay_hook)(void);

/** @brief The QP tick's period: kqp_tick_start() starts its timer with it. */
#define FAKE_TICK_US   (FAKE_US_PER_S / (uint64_t)KQP_TICKS_PER_SEC)

/** @brief One periodic timer, as the case sees it: named by its callback, as on the board. */
typedef struct {
    void   (*cb)(void *);   /**< The callback; NULL for a free slot. */
    void    *arg;           /**< Passed to @c cb when a case fires it. */
    uint64_t period_us;     /**< What it was started with. */
    bool     running;       /**< Started and not stopped. */
    unsigned made;          /**< How many times khal_timer_periodic() made it. */
} fake_periodic_t;

/* The periodic timers. Not cleared by fake_time_reset(): each is started once
 * per binary, the way it is once per boot, and a case clears the counters it
 * reads with fake_time_periodic_forget(). The counters and
 * fake_time_periodic_running() are the QP tick's: the timer with its period. */
static fake_periodic_t s_periodic[KHAL_TIMER_PERIODIC_SLOTS];
static unsigned s_run_stops;
static unsigned s_run_starts;

void fake_time_reset(void)
{
    s_now_us  = 0U;
    /* A case that never touches the wall clock starts with it unset,
     * not with the previous case's date. */
    s_wall_us = 0U;
    s_delay_hook = NULL;
    fake_time_once_forget();
}

void fake_time_delay_hook(void (*hook)(void))
{
    s_delay_hook = hook;
}

void fake_time_advance_ms(uint32_t ms)
{
    s_now_us += (uint64_t)ms * 1000U;
}

void fake_time_advance_us(uint32_t us)
{
    s_now_us += (uint64_t)us;
}

uint64_t khal_time_us(void)
{
    return s_now_us;
}

uint32_t khal_time_ms(void)
{
    return (uint32_t)(s_now_us / 1000U);
}

void khal_delay_ms(uint32_t ms)
{
    fake_time_advance_ms(ms);

    /* Whatever another task does while this caller sleeps. Cleared before it
     * runs, so a hook that sleeps again does not recurse. */
    if ( NULL != s_delay_hook )
    {
        void (* const hook)(void) = s_delay_hook;
        s_delay_hook = NULL;
        hook();
    }
}

/* A fake, not IDF's CMock mock: klog stamps every line with it, and a mock wants each call. */
/**
 * @brief The clock klog stamps its lines from.
 * @return Microseconds since the case's reset.
 */
int64_t esp_timer_get_time(void)
{
    return (int64_t)s_now_us;
}

void fake_time_wall_set(uint64_t unix_us)
{
    s_wall_us = unix_us;
}

uint64_t khal_time_wall_us(void)
{
    return s_wall_us;
}

void khal_time_wall_set(uint64_t unix_us)
{
    s_wall_us = unix_us;
}

bool khal_time_wall_known(void)
{
    return ((uint64_t)KODE_CLOCK_KNOWN_AFTER_S * 1000000ULL) < s_wall_us;
}

uint64_t khal_time_at_wall_s(int64_t wall_s)
{
    uint64_t at = 0U;

    if ( 0 <= wall_s )
    {
        uint64_t const due_wall = (uint64_t)wall_s * FAKE_US_PER_S;

        at = s_now_us;
        if ( due_wall > s_wall_us )
        {
            at += due_wall - s_wall_us;
        }
        if ( 0U == at )
        {
            at = 1U;
        }
    }
    return at;
}

/**
 * @brief The wall clock klog stamps its lines from.
 *
 * Here for the same reason esp_timer_get_time() is: klog reads it on every
 * line, and a suite whose log lines depended on the build machine's calendar
 * could not assert on one.
 *
 * There is no NULL check on @p tv, and there cannot be one: glibc declares
 * this function `__nonnull((1))`, and GCC then rejects the comparison itself
 * (`-Werror=nonnull-compare`). A guard that the toolchain refuses to compile
 * is not a guard. The only caller in this build is klog, which passes a
 * stack object.
 *
 * @param[out] tv Filled with the fake's wall clock.
 * @param[in]  tz Ignored, as everywhere.
 * @return 0, always.
 */
int gettimeofday(struct timeval *tv, void *tz)
{
    (void)tz;
    tv->tv_sec  = (time_t)(s_wall_us / 1000000ULL);
    tv->tv_usec = (suseconds_t)(s_wall_us % 1000000ULL);
    return 0;
}

/**
 * @brief The periodic timer named by @p cb, or a free one when none is.
 * @param cb The callback.
 * @return The slot; NULL when @p cb has none and every slot is taken.
 */
static fake_periodic_t *periodic_slot_for(void (*cb)(void *))
{
    fake_periodic_t *slot = NULL;

    for ( unsigned i = 0U; (KHAL_TIMER_PERIODIC_SLOTS > i) && (NULL == slot); i++ )
    {
        if ( s_periodic[i].cb == cb )
        {
            slot = &s_periodic[i];
        }
    }
    for ( unsigned i = 0U; (KHAL_TIMER_PERIODIC_SLOTS > i) && (NULL == slot); i++ )
    {
        if ( NULL == s_periodic[i].cb )
        {
            slot = &s_periodic[i];
        }
    }
    return slot;
}

/**
 * @brief The periodic timer started with @p period_us.
 * @param period_us The period.
 * @return The slot; NULL when no timer has that period.
 */
static fake_periodic_t *periodic_slot_at(uint64_t period_us)
{
    fake_periodic_t *slot = NULL;

    for ( unsigned i = 0U; (KHAL_TIMER_PERIODIC_SLOTS > i) && (NULL == slot); i++ )
    {
        if ( (NULL != s_periodic[i].cb) && (s_periodic[i].period_us == period_us) )
        {
            slot = &s_periodic[i];
        }
    }
    return slot;
}

kode_status_t khal_timer_periodic(void (*cb)(void *), void *arg, uint64_t period_us)
{
    fake_periodic_t *const slot = periodic_slot_for(cb);

    if ( NULL == slot )
    {
        return KODE_NO_RESOURCES;
    }
    slot->cb        = cb;
    slot->arg       = arg;
    slot->period_us = period_us;
    slot->running   = true;
    ++slot->made;
    return KODE_OK;
}

kode_status_t khal_timer_periodic_run(void (*cb)(void *), bool run)
{
    fake_periodic_t *slot = NULL;

    if ( NULL != cb )
    {
        slot = periodic_slot_for(cb);
    }
    if ( (NULL == slot) || (slot->cb != cb) )
    {
        return KODE_DENIED;
    }
    if ( FAKE_TICK_US == slot->period_us )
    {
        if ( run )
        {
            ++s_run_starts;
        }
        else
        {
            ++s_run_stops;
        }
    }
    slot->running = run;
    return KODE_OK;
}

/* The one-shots, as the case sees them: armed and waiting, never run by a
 * timer. A case fires them by hand, oldest first, and the clock moves to the
 * moment each was due. */
typedef struct {
    void    (*cb)(void *);
    void     *arg;
    uint64_t  due_us;
    uint32_t  order;
    bool      used;
} fake_once_t;

static fake_once_t s_once[KHAL_TIMER_ONCE_SLOTS];
static uint32_t    s_once_order;
static uint64_t    s_once_last_delay_us;

kode_status_t khal_timer_once(void (*cb)(void *), void *arg, uint64_t delay_us)
{
    kode_status_t rc = KODE_NO_RESOURCES;

    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    for ( unsigned i = 0U; KHAL_TIMER_ONCE_SLOTS > i; i++ )
    {
        if ( !s_once[i].used )
        {
            s_once[i].used   = true;
            s_once[i].cb     = cb;
            s_once[i].arg    = arg;
            s_once[i].due_us = s_now_us + delay_us;
            s_once[i].order  = ++s_once_order;
            s_once_last_delay_us = delay_us;
            rc = KODE_OK;
            break;
        }
    }
    return rc;
}

unsigned fake_time_once_pending(void)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; KHAL_TIMER_ONCE_SLOTS > i; i++ )
    {
        if ( s_once[i].used )
        {
            ++n;
        }
    }
    return n;
}

uint64_t fake_time_once_last_delay_us(void)
{
    return s_once_last_delay_us;
}

bool fake_time_once_fire(void)
{
    int pick = -1;

    for ( unsigned i = 0U; KHAL_TIMER_ONCE_SLOTS > i; i++ )
    {
        if ( s_once[i].used
            && ((0 > pick) || (s_once[i].due_us < s_once[pick].due_us)) )
        {
            pick = (int)i;
        }
    }
    if ( 0 > pick )
    {
        return false;
    }
    {
        fake_once_t const t = s_once[pick];

        s_once[pick].used = false;   /* free before the callback, as the real one is */
        if ( t.due_us > s_now_us )
        {
            s_now_us = t.due_us;
        }
        t.cb(t.arg);
    }
    return true;
}

void fake_time_once_forget(void)
{
    (void)memset(s_once, 0, sizeof(s_once));
    s_once_last_delay_us = 0U;
}

void fake_time_periodic_forget(void)
{
    s_run_stops  = 0U;
    s_run_starts = 0U;
}

unsigned fake_time_periodic_stops(void)
{
    return s_run_stops;
}

unsigned fake_time_periodic_starts(void)
{
    return s_run_starts;
}

bool fake_time_periodic_running(void)
{
    return fake_time_periodic_running_at(FAKE_TICK_US);
}

bool fake_time_periodic_running_at(uint64_t period_us)
{
    fake_periodic_t const *const slot = periodic_slot_at(period_us);

    return (NULL != slot) && slot->running;
}

unsigned fake_time_periodic_made_at(uint64_t period_us)
{
    fake_periodic_t const *const slot = periodic_slot_at(period_us);
    unsigned made = 0U;

    if ( NULL != slot )
    {
        made = slot->made;
    }
    return made;
}

bool fake_time_periodic_fire_at(uint64_t period_us)
{
    fake_periodic_t const *const slot = periodic_slot_at(period_us);
    bool const fired = (NULL != slot) && slot->running;

    if ( fired )
    {
        slot->cb(slot->arg);
    }
    return fired;
}
