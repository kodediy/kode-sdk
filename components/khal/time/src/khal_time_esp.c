/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_time_esp.c
 * @brief khal_time on ESP-IDF: esp_timer, and the C library's wall clock.
 */
#include "khal_time.h"

#include <stdatomic.h>
#include <stddef.h>
#include <sys/time.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "kode_log.h"   /* KODE_CLOCK_KNOWN_AFTER_S */

#define US_PER_MS  1000U      /**< Microseconds in a millisecond. */
#define US_PER_S   1000000ULL /**< Microseconds in a second. */

uint64_t khal_time_us(void)
{
    return (uint64_t)esp_timer_get_time();
}

uint32_t khal_time_ms(void)
{
    return (uint32_t)((uint64_t)esp_timer_get_time() / US_PER_MS);
}

uint64_t khal_time_wall_us(void)
{
    struct timeval tv;

    /* The C library adds the RTC's offset to esp_timer, so a date survives a reset. */
    if ( gettimeofday(&tv, NULL) != 0 )
    {
        return 0U;
    }

    return ((uint64_t)tv.tv_sec * US_PER_S) + (uint64_t)tv.tv_usec;
}

void khal_time_wall_set(uint64_t unix_us)
{
    const struct timeval tv = {
        .tv_sec  = (time_t)(unix_us / US_PER_S),
        .tv_usec = (suseconds_t)(unix_us % US_PER_S),
    };

    (void)settimeofday(&tv, NULL);
}

bool khal_time_wall_known(void)
{
    return khal_time_wall_us()
           > ((uint64_t)KODE_CLOCK_KNOWN_AFTER_S * US_PER_S);
}

uint64_t khal_time_at_wall_s(int64_t wall_s)
{
    uint64_t at = 0U;

    if (wall_s >= 0) {
        uint64_t const due_wall = (uint64_t)wall_s * US_PER_S;
        uint64_t const wall     = khal_time_wall_us();

        at = khal_time_us();
        if (due_wall > wall) {
            at += due_wall - wall;
        }
        if (at == 0U) {
            at = 1U;
        }
    }
    return at;
}

void khal_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/** @brief A periodic timer that can be stopped: named by its callback. */
typedef struct {
    void             (*cb)(void *);   /**< The callback; NULL for a free slot. */
    esp_timer_handle_t timer;         /**< The timer behind it. */
    uint64_t           period_us;     /**< Kept for a restart. */
    bool               running;       /**< Started and not stopped. */
} periodic_slot_t;

static periodic_slot_t s_periodic[KHAL_TIMER_PERIODIC_SLOTS];   /**< The named periodic timers. */

/**
 * @brief The slot named by @p cb, or a free one when none is.
 * @param cb The callback.
 * @return The slot; NULL when @p cb has none and every slot is taken.
 */
static periodic_slot_t *periodic_slot_for(void (*cb)(void *))
{
    periodic_slot_t *free_slot = NULL;

    for ( size_t i = 0U; i < KHAL_TIMER_PERIODIC_SLOTS; i++ )
    {
        if ( s_periodic[i].cb == cb )
        {
            return &s_periodic[i];
        }
        if ( (NULL == free_slot) && (NULL == s_periodic[i].cb) )
        {
            free_slot = &s_periodic[i];
        }
    }
    return free_slot;
}

kode_status_t khal_timer_periodic(void (*cb)(void *), void *arg, uint64_t period_us)
{
    esp_timer_handle_t timer = NULL;
    periodic_slot_t *const slot = periodic_slot_for(cb);
    const esp_timer_create_args_t args = {
        .callback = cb,
        .arg      = arg,
        .name     = "khal_tick",
    };

    if ( NULL == slot )
    {
        return KODE_NO_RESOURCES;
    }
    if ( esp_timer_create(&args, &timer) != ESP_OK )
    {
        return KODE_NO_RESOURCES;
    }
    if ( esp_timer_start_periodic(timer, period_us) != ESP_OK )
    {
        (void)esp_timer_delete(timer);
        return KODE_ERROR;
    }
    slot->cb        = cb;
    slot->timer     = timer;
    slot->period_us = period_us;
    slot->running   = true;
    return KODE_OK;
}

kode_status_t khal_timer_periodic_run(void (*cb)(void *), bool run)
{
    periodic_slot_t *slot = NULL;

    for ( size_t i = 0U; (i < KHAL_TIMER_PERIODIC_SLOTS) && (NULL != cb); i++ )
    {
        if ( s_periodic[i].cb == cb )
        {
            slot = &s_periodic[i];
        }
    }
    if ( NULL == slot )
    {
        return KODE_DENIED;
    }
    if ( slot->running == run )
    {
        return KODE_OK;
    }
    if ( run )
    {
        if ( esp_timer_start_periodic(slot->timer, slot->period_us) != ESP_OK )
        {
            return KODE_ERROR;
        }
    }
    else
    {
        if ( esp_timer_stop(slot->timer) != ESP_OK )
        {
            return KODE_ERROR;
        }
    }
    slot->running = run;
    return KODE_OK;
}

/** @brief A one-shot slot: its timer is made on first use and reused after. */
typedef struct {
    esp_timer_handle_t timer;     /**< NULL until first used. */
    atomic_bool        busy;      /**< Armed and not yet fired. */
    void             (*cb)(void *);   /**< What to call. */
    void              *arg;       /**< Passed to @c cb. */
} once_slot_t;

/**
 * @brief A one-shot's timer fired: free its slot, then call its callback.
 * @param slot_arg The slot's once_slot_t.
 */
static void once_fire(void *slot_arg)
{
    /* cppcheck-suppress misra-c2012-11.5 ; esp_timer hands back the pointer it was given, which is the slot */
    once_slot_t *const slot = (once_slot_t *)slot_arg;
    void (*const cb)(void *) = slot->cb;
    void *const arg          = slot->arg;

    /* Free before the call: the callback may arm the next one. */
    atomic_store(&slot->busy, false);
    cb(arg);
}

kode_status_t khal_timer_once(void (*cb)(void *), void *arg, uint64_t delay_us)
{
    static once_slot_t s_once[KHAL_TIMER_ONCE_SLOTS];   /* The pending one-shots. */

    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    for ( size_t i = 0U; i < KHAL_TIMER_ONCE_SLOTS; i++ )
    {
        once_slot_t *const slot = &s_once[i];

        if ( atomic_exchange(&slot->busy, true) )
        {
            continue;
        }
        if ( NULL == slot->timer )
        {
            const esp_timer_create_args_t args = {
                .callback = once_fire,
                .arg      = slot,
                .name     = "khal_once",
            };
            if ( esp_timer_create(&args, &slot->timer) != ESP_OK )
            {
                slot->timer = NULL;
                atomic_store(&slot->busy, false);
                return KODE_NO_RESOURCES;
            }
        }
        slot->cb  = cb;
        slot->arg = arg;
        if ( esp_timer_start_once(slot->timer, delay_us) != ESP_OK )
        {
            atomic_store(&slot->busy, false);
            return KODE_ERROR;
        }
        return KODE_OK;
    }
    return KODE_NO_RESOURCES;
}
