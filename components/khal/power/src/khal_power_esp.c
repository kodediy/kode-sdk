/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_power_esp.c
 * @brief khal_power on ESP-IDF: esp_pm's lock and light sleep, GPIO level wakes, and deep sleep with ext1.
 */
#include "khal_power.h"

#if defined(CONFIG_KODE_LOWPOWER)

#include <stdatomic.h>
#include <stddef.h>

#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"

#include "kbsp_pins.h"
#include "khal_gpio.h"
#include "kode_log.h"

#define TAG "khal.power"   /**< This file's log tag. */

/* cppcheck-suppress misra-c2012-2.5 ; used below, in a region cppcheck reads under CONFIG_KODE_LOWPOWER, whose macro uses it does not record */
#define PM_CPU_MHZ  400   /**< The clock, fixed: light sleep is the saving, not scaling. */
/* cppcheck-suppress misra-c2012-2.5 ; used below, in a region cppcheck reads under CONFIG_KODE_LOWPOWER, whose macro uses it does not record */
#define LOCK_COUNT  (KHAL_POWER_LOCKS)   /**< The pool of locks khal_power_lock_new() hands out. */

/* ---- Locks ---- */

/** @brief A lock and whether this module holds it, so hold and release are idempotent. */
typedef struct {
    esp_pm_lock_handle_t handle;   /**< NULL until created. */
    atomic_bool          held;     /**< true between acquire and release. */
} power_lock_t;

static power_lock_t s_awake;                  /**< The one lock that makes the board AWAKE. */
static power_lock_t s_locks[LOCK_COUNT];      /**< The locks of khal_power_lock_new(). */
static atomic_uint  s_locks_used;             /**< How many of s_locks are taken. */
static bool         s_ready;                  /**< khal_power_init() has run. */

/* ---- Light-sleep accounting ---- */

static atomic_uint   s_seq;        /**< Odd while the exit callback is adding. */
static uint64_t      s_slept_us;   /**< Light sleep so far; written by the exit callback only. */
static uint32_t      s_sleeps;     /**< Sleeps so far; written by the exit callback only. */
static int64_t       s_init_us;    /**< esp_timer reading at khal_power_init(). */

/* ---- Wake lines ---- */

/** @brief One armed wake line. */
typedef struct {
    uint32_t             gpio;         /**< The pin; meaningful when @c used. */
    bool                 level_high;   /**< The active level. */
    bool                 used;         /**< Taken. */
    khal_power_wake_cb_t cb;           /**< Called from the interrupt. */
    void                *arg;          /**< Passed to @c cb. */
} wake_line_t;

static wake_line_t s_lines[KHAL_POWER_WAKE_LINES];   /**< The armed lines. */

/* ---- Deep sleep ---- */

/* cppcheck-suppress misra-c2012-8.9 ; a file-scope object on purpose: its section is what makes it outlive deep sleep */
static RTC_DATA_ATTR uint32_t s_off_note;   /**< khal_power_off_note_set(). */

/** @brief A pin deep sleep holds, and the level it holds it at. */
typedef struct {
    uint8_t pin;     /**< The P4 GPIO number. */
    bool    level;   /**< The rest level. */
} held_pin_t;

/**
 * @brief The C5's enable, held low (the C5 in reset) while the P4 is off.
 *
 * Apart from the table below because it is written last, right before deep
 * sleep starts: an OFF round leaves the esp-hosted link up, and the C5 dropping
 * out from under its SDIO tasks while they still run can fail a transaction,
 * which a build with CONFIG_ESP_HOSTED_TRANSPORT_RESTART_ON_FAILURE answers by
 * rebooting the P4.
 */
static const held_pin_t s_c5_en = { KBSP_PIN_C5_EN, false };

/** @brief Every other pin the board needs held while the P4 is off, at its rest level. */
static const held_pin_t s_held[] = {
    { KBSP_PIN_SD_PWR,     true  },   /* the card's supply off              */
    { KBSP_PIN_NFC_CS,     true  },   /* the ST25R deselected               */
    { KBSP_PIN_RFID_PULL,  true  },   /* the LF front end at rest           */
    { KBSP_PIN_DSP_TP_RST, false },   /* the panel and touch held in reset  */
};

/* cppcheck-suppress misra-c2012-2.5 ; used below, in a region cppcheck reads under CONFIG_KODE_LOWPOWER, whose macro uses it does not record */
#define HELD_COUNT  (sizeof(s_held) / sizeof(s_held[0]))   /**< Entries in s_held. */

/* ---- Locks ---- */

/**
 * @brief Create an esp_pm lock that keeps light sleep out, released.
 *
 * @param l    The lock to fill.
 * @param name Its name in esp_pm.
 * @return KODE_OK; KODE_NO_RESOURCES when esp_pm cannot create it.
 */
static kode_status_t lock_make(power_lock_t *l, const char *name)
{
    if ( esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, name, &l->handle) != ESP_OK )
    {
        return KODE_NO_RESOURCES;
    }
    atomic_init(&l->held, false);
    return KODE_OK;
}

/**
 * @brief Acquire or release a lock, only when that changes it; a lock never created is left alone.
 *
 * @param l    The lock.
 * @param hold true to acquire it, false to release it.
 */
static void lock_set(power_lock_t *l, bool hold)
{
    if ( NULL == l->handle )
    {
        return;
    }
    if ( atomic_exchange(&l->held, hold) == hold )
    {
        return;
    }
    if ( hold )
    {
        (void)esp_pm_lock_acquire(l->handle);
    }
    else
    {
        (void)esp_pm_lock_release(l->handle);
    }
}

/* ---- Light-sleep callback (idle task, may run with the cache off) ---- */

/**
 * @brief Add one light sleep and its length to the stats; a sleep of 0 us is not one.
 *
 * Runs on the idle task, maybe with the cache off, so nothing it touches may
 * be in flash.
 *
 * @param sleep_time_us How long the chip slept; 0 when the sleep was skipped or refused.
 * @param arg           Unused.
 * @return ESP_OK.
 */
static esp_err_t IRAM_ATTR sleep_exit_cb(int64_t sleep_time_us, void *arg)
{
    (void)arg;
    /* ESP-IDF calls the exit callbacks also when the sleep was skipped or
     * refused, with 0 as the time slept: that is not a sleep. */
    if ( 0 >= sleep_time_us )
    {
        return ESP_OK;
    }
    atomic_fetch_add(&s_seq, 1U);
    s_slept_us += (uint64_t)sleep_time_us;
    s_sleeps++;
    atomic_fetch_add(&s_seq, 1U);
    return ESP_OK;
}

kode_status_t khal_power_init(void)
{
    if ( s_ready )
    {
        return KODE_OK;
    }
    if ( lock_make(&s_awake, "kode.awake") != KODE_OK )
    {
        return KODE_NO_RESOURCES;
    }
    lock_set(&s_awake, true);

    esp_pm_sleep_cbs_register_config_t cbs = {
        .exit_cb = sleep_exit_cb,
    };
    if ( esp_pm_light_sleep_register_cbs(&cbs) != ESP_OK )
    {
        KLOG_W(TAG, "light-sleep accounting not registered");
    }

    const esp_pm_config_t pm = {
        .max_freq_mhz       = PM_CPU_MHZ,
        .min_freq_mhz       = PM_CPU_MHZ,
        .light_sleep_enable = true,
    };
    esp_err_t const err = esp_pm_configure(&pm);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "power management would not configure: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    s_init_us = esp_timer_get_time();
    s_ready   = true;
    return KODE_OK;
}

void khal_power_hold(bool hold)
{
    lock_set(&s_awake, hold);
}

kode_status_t khal_power_lock_new(const char *name, khal_power_lock_t *out)
{
    if ( (NULL == name) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    unsigned int const slot = atomic_fetch_add(&s_locks_used, 1U);
    if ( LOCK_COUNT <= slot )
    {
        atomic_store(&s_locks_used, (unsigned int)LOCK_COUNT);
        return KODE_NO_RESOURCES;
    }
    if ( lock_make(&s_locks[slot], name) != KODE_OK )
    {
        return KODE_NO_RESOURCES;
    }
    *out = (khal_power_lock_t)slot;
    return KODE_OK;
}

void khal_power_lock(khal_power_lock_t l, bool hold)
{
    if ( LOCK_COUNT <= l )
    {
        return;
    }
    lock_set(&s_locks[l], hold);
}

/* ---- Wake lines ---- */

/**
 * @brief A wake line's interrupt: disarm the line, then call its callback.
 *
 * Runs in interrupt context. The line disarms itself first: a level that
 * stays asserted would fire again at once.
 *
 * @param arg The line's wake_line_t.
 */
static void wake_isr(void *arg)
{
    wake_line_t *const line = (wake_line_t *)arg;

    if ( (NULL == line) || (NULL == line->cb) )
    {
        return;
    }
    /* A level that stays asserted would wake the chip again at once, forever:
     * the line disarms itself before anyone is told. */
    (void)gpio_intr_disable((gpio_num_t)line->gpio);
    (void)gpio_wakeup_disable((gpio_num_t)line->gpio);
    line->cb(line->arg);
}

/**
 * @brief The GPIO interrupt type for a line's active level.
 *
 * @param level_high true when the line is active high.
 * @return GPIO_INTR_HIGH_LEVEL or GPIO_INTR_LOW_LEVEL.
 */
static gpio_int_type_t wake_type(bool level_high)
{
    if ( level_high )
    {
        return GPIO_INTR_HIGH_LEVEL;
    }
    return GPIO_INTR_LOW_LEVEL;
}

/**
 * @brief Find the armed line on a pin.
 *
 * @param gpio The pin.
 * @return The line, or NULL when none is on @p gpio.
 */
static wake_line_t *wake_find(uint32_t gpio)
{
    for ( size_t i = 0U; KHAL_POWER_WAKE_LINES > i; i++ )
    {
        if ( s_lines[i].used && (s_lines[i].gpio == gpio) )
        {
            return &s_lines[i];
        }
    }
    return NULL;
}

/**
 * @brief The line for a pin: the one already on it, else a free one.
 *
 * @param gpio The pin.
 * @return The line, or NULL when every line is taken.
 */
static wake_line_t *wake_take(uint32_t gpio)
{
    wake_line_t *line = wake_find(gpio);

    if ( NULL != line )
    {
        return line;
    }
    for ( size_t i = 0U; KHAL_POWER_WAKE_LINES > i; i++ )
    {
        if ( !s_lines[i].used )
        {
            return &s_lines[i];
        }
    }
    return NULL;
}

kode_status_t khal_power_wake_arm(uint32_t gpio, bool level_high,
                                  khal_power_wake_cb_t cb, void *arg)
{
    if ( (NULL == cb) || ((uint32_t)GPIO_NUM_MAX <= gpio) )
    {
        return KODE_DENIED;
    }
    wake_line_t *const line = wake_take(gpio);
    if ( NULL == line )
    {
        return KODE_NO_RESOURCES;
    }
    if ( khal_gpio_service() != KODE_OK )
    {
        return KODE_ERROR;
    }

    gpio_num_t const pin = (gpio_num_t)gpio;
    gpio_pull_mode_t bias = GPIO_PULLUP_ONLY;
    if ( level_high )
    {
        bias = GPIO_PULLDOWN_ONLY;
    }
    /* The slot is complete before the handler goes in: adding a handler
     * enables the pin's interrupt, and a line already at its active level
     * runs wake_isr at once. */
    line->gpio       = gpio;
    line->level_high = level_high;
    line->cb         = cb;
    line->arg        = arg;
    line->used       = true;
    if ( (gpio_set_direction(pin, GPIO_MODE_INPUT) != ESP_OK)
        || (gpio_set_pull_mode(pin, bias) != ESP_OK)
        || (gpio_set_intr_type(pin, wake_type(level_high)) != ESP_OK)
        || (gpio_isr_handler_add(pin, wake_isr, line) != ESP_OK) )
    {
        line->used = false;
        return KODE_ERROR;
    }
    /* Only arm and rearm enable the interrupt, on purpose. */
    (void)gpio_intr_disable(pin);
    khal_power_wake_rearm(gpio);
    return KODE_OK;
}

void khal_power_wake_rearm(uint32_t gpio)
{
    wake_line_t const *const line = wake_find(gpio);

    if ( NULL == line )
    {
        return;
    }
    gpio_num_t const pin = (gpio_num_t)gpio;
    (void)gpio_wakeup_enable(pin, wake_type(line->level_high));
    (void)esp_sleep_enable_gpio_wakeup();
    (void)gpio_intr_enable(pin);
}

void khal_power_stats(khal_power_stats_t *out)
{
    unsigned int before;
    unsigned int after;

    if ( NULL == out )
    {
        return;
    }
    /* The exit callback is the only writer: read until it was not mid-update. */
    do
    {
        before        = atomic_load(&s_seq);
        out->slept_us = s_slept_us;
        out->sleeps   = s_sleeps;
        after         = atomic_load(&s_seq);
    } while ( (before != after) || ((before % 2U) != 0U) );

    out->since_us = 0U;
    if ( s_ready )
    {
        out->since_us = (uint64_t)(esp_timer_get_time() - s_init_us);
    }
}

/* ---- Deep sleep ---- */

bool khal_power_woke_from_off(void)
{
    uint32_t const causes = esp_sleep_get_wakeup_causes();

    return (esp_reset_reason() == ESP_RST_DEEPSLEEP)
           && ((causes & (1UL << (uint32_t)ESP_SLEEP_WAKEUP_EXT1)) != 0U);
}

/**
 * @brief Set a held pin to its rest level and let the hold go, after a deep sleep.
 *
 * @param p The pin and its rest level.
 */
static void pin_release(const held_pin_t *p)
{
    gpio_num_t const pin = (gpio_num_t)p->pin;

    /* While the pad is still held: the register takes effect the moment the hold goes. */
    (void)gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    (void)gpio_set_level(pin, (uint32_t)p->level);
    (void)gpio_hold_dis(pin);
}

/**
 * @brief Drive a pin to its rest level and hold it there through deep sleep.
 *
 * @param p The pin and its rest level.
 */
static void pin_hold(const held_pin_t *p)
{
    gpio_num_t const pin = (gpio_num_t)p->pin;

    (void)gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    (void)gpio_set_level(pin, (uint32_t)p->level);
    (void)gpio_hold_en(pin);
}

void khal_power_holds_release(void)
{
    if ( esp_reset_reason() != ESP_RST_DEEPSLEEP )
    {
        return;   /* nothing is held; do not reset the C5 on a cold boot */
    }
    for ( size_t i = 0U; HELD_COUNT > i; i++ )
    {
        pin_release(&s_held[i]);
    }
    pin_release(&s_c5_en);
}

void khal_power_off_note_set(uint32_t note)
{
    s_off_note = note;
}

uint32_t khal_power_off_note(void)
{
    return s_off_note;
}

void khal_power_off_enter(void)
{
    for ( size_t i = 0U; HELD_COUNT > i; i++ )
    {
        pin_hold(&s_held[i]);
    }

    /* The light-sleep GPIO wakes are not deep-sleep sources: only ext1 stays. */
    (void)esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    esp_err_t const err = esp_sleep_enable_ext1_wakeup_io(1ULL << KBSP_PIN_IOEXP_INT,
                                                          ESP_EXT1_WAKEUP_ANY_LOW);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "no wake line for deep sleep: %s", esp_err_to_name(err));
    }
    KLOG_N(TAG, "deep sleep");
    /* The last write before the chip stops: see s_c5_en. */
    pin_hold(&s_c5_en);
    esp_deep_sleep_start();
}

#endif /* CONFIG_KODE_LOWPOWER */
