/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_sleep.c
 * @brief The radio's sleep sentinel: keeps the link to the C5 alive while the P4 light-sleeps.
 */
#include "kdrv_radio.h"

#if CONFIG_KODE_LOWPOWER

#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "kbsp_pins.h"
#include "khal_power.h"
#include "kode_log.h"
#include "kdrv_radio_sleep.h"
#if CONFIG_KODEOS_BLE
#include "kdrv_ble.h"
#endif

#define TAG  "kdrv.radio.sleep"   /**< This file's log tag. */

/* ---- Sizes and names ---- */

/** The sentinel's task: the handshake logs, so more than a bare loop needs. */
#define SLEEP_TASK_STACK    4096U
/** Where it runs: with the Active Object that serves the radio's traffic (the spike's value). */
#define SLEEP_TASK_PRIO     3U
/** How long a caller of kdrv_radio_sleep() waits: a deaf wake, its window and a few GAP windows. */
#define SLEEP_CMD_WAIT_MS   10000U
/** The PM lock held while a wake is served, and across the way down. */
#define SLEEP_LOCK_NAME     "radio.sleep"
/** Wakes logged one by one before the log goes quiet (the stats keep counting). */
#define SLEEP_LOG_FIRST     8U

/** esp-hosted's SDIO tasks, by the names it gives them (sdio_drv.c). */
#define SDIO_TASK_WRITE        "sdio_write"
#define SDIO_TASK_READ         "sdio_read"
#define SDIO_TASK_PROCESS_RX   "sdio_process_rx"
#define SDIO_TASKS             3U   /**< How many of them there are. */

/* ---- esp-hosted's private entry points ---- */

/* Declared only in esp-hosted's private header,
 * host/drivers/transport/transport_drv.h (line 169 in 2.12.9); the component
 * exports both from sdio_drv.c and nothing public calls them. They send the
 * C5 the message that the host enters or leaves power save: an SDIO register
 * write, 0 when the C5 took it, an error while the C5 sleeps. */
extern int bus_inform_slave_host_power_save_start(void);
extern int bus_inform_slave_host_power_save_stop(void);

/* The hook below: esp-hosted's every outgoing frame goes through
 * esp_hosted_tx() (transport_drv.h line 159, defined in sdio_drv.c), called
 * from its other translation units, so the linker's --wrap catches it. */
int __real_esp_hosted_tx(uint8_t iface_type, uint8_t iface_num,
                         uint8_t *payload_buf, uint16_t payload_len, uint8_t buff_zerocopy,
                         uint8_t *buffer_to_free, void (*free_buf_func)(void *ptr), uint8_t flags);
int __wrap_esp_hosted_tx(uint8_t iface_type, uint8_t iface_num,
                         uint8_t *payload_buf, uint16_t payload_len, uint8_t buff_zerocopy,
                         uint8_t *buffer_to_free, void (*free_buf_func)(void *ptr), uint8_t flags);

/* ---- State ---- */

/** @brief What a caller asks of the task. */
typedef enum {
    CMD_NONE = 0,   /**< Nothing.                         */
    CMD_START,      /**< Go to sleep: bus down, line armed. */
    CMD_STOP        /**< Leave the bus awake.             */
} sleep_cmd_t;

static TaskHandle_t        s_sentinel;              /**< The sentinel; NULL until the first start. */
static SemaphoreHandle_t   s_sentinel_done;         /**< Given by the task when a command is done. */
static khal_power_lock_t   s_wake_lock = KHAL_POWER_LOCK_NONE;   /**< Held while a wake is served. */

static atomic_uint         s_cmd;                   /**< A sleep_cmd_t; written by the caller, taken by the task. */
static atomic_bool         s_bus_down;              /**< The bus is silent: the line and the hook may wake the task. */
static atomic_bool         s_ev_c5;                 /**< The C5's line asserted while the bus was down. */
static atomic_bool         s_ev_tx;                 /**< A frame was queued while the bus was down. */
static atomic_uint         s_cmd_status;            /**< The kode_status_t of the last command. */

static kdrv_radio_sleep_note_cb_t s_stage_note;     /**< Staged by the caller before the doorbell. */
static void                      *s_stage_arg;      /**< Its argument. */
static kdrv_radio_sleep_note_cb_t s_note;           /**< Used by the task only. */
static void                      *s_note_arg;       /**< Its argument. */

static bool                s_wake_armed;                 /**< Task only: the bus is down and the line armed. */
static bool                s_stood_down;            /**< Task only: the way down failed; the lock stays held. */

static atomic_uint         s_tx[KRADIO_SLEEP_TX_OTHER];   /**< Frames queued while the bus was down, by kind. */
static atomic_uint         s_wakes_c5;              /**< Stats: wakes by the C5. */
static atomic_uint         s_wakes_tx;              /**< Stats: wakes by the P4's traffic. */
static atomic_uint         s_deaf;                  /**< Stats: wakes never answered. */
static atomic_uint         s_stood_down_count;      /**< Stats: times the way down failed. */
static atomic_uint         s_knocks;                /**< Stats: attempts spent. */
static atomic_uint         s_knocks_max;            /**< Stats: the most one wake needed. */

/* ---- The real calls behind the rules ---- */

/**
 * @brief Tell the C5 the host enters or leaves power save.
 * @param on true when it enters, false when it leaves.
 * @return 0 when the C5 took it; an error while the C5 sleeps.
 */
static int real_power_save(bool on)
{
    if ( on )
    {
        return bus_inform_slave_host_power_save_start();
    }
    return bus_inform_slave_host_power_save_stop();
}

/**
 * @brief Resume or suspend esp-hosted's SDIO tasks; one not found is skipped.
 * @param run true to resume them, false to suspend them.
 */
static void real_sdio_tasks(bool run)
{
    static const char *const names[SDIO_TASKS] = {
        SDIO_TASK_WRITE, SDIO_TASK_READ, SDIO_TASK_PROCESS_RX,
    };

    for ( size_t i = 0U; SDIO_TASKS > i; i++ )
    {
        TaskHandle_t const t = xTaskGetHandle(names[i]);

        if ( NULL != t )
        {
            if ( run )
            {
                vTaskResume(t);
            }
            else
            {
                vTaskSuspend(t);
            }
        }
    }
}

/**
 * @brief Whether every SDIO task is parked, none in the middle of a transaction.
 *
 * A task blocked on its queue or semaphore, or already suspended, is parked.
 * Suspending one that holds the SDIO host's mutex would leave the way back up
 * waiting for it for ever.
 */
static bool real_sdio_settled(void)
{
    static const char *const names[SDIO_TASKS] = {
        SDIO_TASK_WRITE, SDIO_TASK_READ, SDIO_TASK_PROCESS_RX,
    };

    for ( size_t i = 0U; SDIO_TASKS > i; i++ )
    {
        TaskHandle_t const t = xTaskGetHandle(names[i]);

        if ( NULL != t )
        {
            eTaskState const st = eTaskGetState(t);

            if ( (st != eBlocked) && (st != eSuspended) )
            {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Block the calling task.
 * @param ms How long, in ms.
 */
static void real_delay_ms(uint32_t ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms));
}

/**
 * @brief The BLE GAP event count, which changes when the link moves.
 * @return kdrv_ble_activity(); 0 in a build without BLE.
 */
static uint32_t real_gap_activity(void)
{
#if CONFIG_KODEOS_BLE
    return kdrv_ble_activity();
#else
    return 0U;
#endif
}

/** @brief Whether a command waits for the task (the caller of kdrv_radio_sleep() blocks on it). */
static bool real_cmd_waiting(void)
{
    return atomic_load(&s_cmd) != (unsigned int)CMD_NONE;
}

/* ---- Events: the line and the hook ---- */

/** @brief Wake the task. Task context. */
static void ring_task(void)
{
    if ( NULL != s_sentinel )
    {
        (void)xTaskNotifyGive(s_sentinel);
    }
}

/**
 * @brief Add to a counter that several tasks may bump.
 * @param c The counter.
 * @param n What to add.
 * @return The value after the addition.
 */
static unsigned int counter_add(atomic_uint *c, unsigned int n)
{
    unsigned int seen = atomic_load(c);

    while ( !atomic_compare_exchange_weak(c, &seen, seen + n) )
    {
        /* seen was reloaded; try again */
    }
    return seen + n;
}

/**
 * @brief The C5's line asserted. Interrupt context, once per arming.
 * @param arg Unused.
 */
static void c5_line_isr(void *arg)
{
    BaseType_t higher = pdFALSE;

    (void)arg;
    /* A TX wake leaves the line armed while the bus is up; if the C5 raises
     * it then, the traffic is served already. The line stays disarmed until
     * the way down arms it again, and a level still high will wake it then. */
    if ( atomic_load(&s_bus_down) && (NULL != s_sentinel) )
    {
        atomic_store(&s_ev_c5, true);
        vTaskNotifyGiveFromISR(s_sentinel, &higher);
        portYIELD_FROM_ISR(higher);
    }
}

/**
 * @brief Every frame esp-hosted sends: count it while the bus is down, call
 *        the sentinel, send it.
 *
 * The frame goes on to esp-hosted's own queue, whose writer is suspended;
 * it leaves when the sentinel resumes the bus.
 */
/* cppcheck-suppress misra-c2012-8.7 ; the linker's --wrap routes esp-hosted's calls here; no source file names it */
int __wrap_esp_hosted_tx(uint8_t iface_type, uint8_t iface_num,
                         uint8_t *payload_buf, uint16_t payload_len, uint8_t buff_zerocopy,
                         uint8_t *buffer_to_free, void (*free_buf_func)(void *ptr), uint8_t flags)
{
    if ( atomic_load(&s_bus_down) )
    {
        kradio_sleep_tx_t const slot = kradio_sleep_tx_slot(iface_type);

        if ( KRADIO_SLEEP_TX_OTHER != slot )
        {
            (void)counter_add(&s_tx[slot], 1U);
        }
        atomic_store(&s_ev_tx, true);
        ring_task();
    }
    return __real_esp_hosted_tx(iface_type, iface_num, payload_buf, payload_len,
                                buff_zerocopy, buffer_to_free, free_buf_func, flags);
}

/* ---- The task ---- */

/** @brief Raise the most-knocks mark. */
static void knocks_max_raise(uint16_t knocks)
{
    unsigned int seen = atomic_load(&s_knocks_max);

    while ( ((unsigned int)knocks > seen) &&
           !atomic_compare_exchange_weak(&s_knocks_max, &seen, (unsigned int)knocks) )
    {
        /* seen was reloaded; try again */
    }
}

/** @brief Book one wake's cost. */
static void account(bool heard, uint16_t knocks)
{
    (void)counter_add(&s_knocks, (unsigned int)knocks);
    knocks_max_raise(knocks);
    if ( !heard )
    {
        (void)counter_add(&s_deaf, 1U);
    }
}

/** @brief Drop the events that arrived while the bus was up. */
static void drain_events(void)
{
    atomic_store(&s_ev_c5, false);
    atomic_store(&s_ev_tx, false);
}

/**
 * @brief Arm the C5's line, the first time and every time after.
 * @return true when the line is armed.
 */
static bool line_arm(void)
{
    static bool known;   /* khal_power_wake_arm() has been called: from now on the line is rearmed */

    if ( known )
    {
        khal_power_wake_rearm(KBSP_PIN_C5_WAKE_P4);
        return true;
    }
    if ( khal_power_wake_arm(KBSP_PIN_C5_WAKE_P4, true, &c5_line_isr, NULL) != KODE_OK )
    {
        return false;
    }
    known = true;
    return true;
}

/**
 * @brief Mark the bus silent or live. See kradio_sleep_seam_t.
 *
 * Marked silent, the hook rings on every frame and the line's interrupt
 * counts; the old events are dropped first, and the line is armed after the
 * mark so a level that is already high is not lost.
 *
 * @param down true to mark the bus silent and arm the line.
 * @return false only when the line would not arm.
 */
static bool real_bus_down(bool down)
{
    if ( !down )
    {
        atomic_store(&s_bus_down, false);
        return true;
    }
    drain_events();
    atomic_store(&s_bus_down, true);
    return line_arm();
}

static const kradio_sleep_seam_t REAL = {
    .power_save   = &real_power_save,
    .sdio_tasks   = &real_sdio_tasks,
    .sdio_settled = &real_sdio_settled,
    .bus_down     = &real_bus_down,
    .delay_ms     = &real_delay_ms,
    .gap_activity = &real_gap_activity,
    .cmd_waiting  = &real_cmd_waiting,
};

/**
 * @brief The way down, whole: bus marked, C5 told, tasks suspended, line armed.
 *
 * On a failure the bus is left up and the sentinel not armed.
 *
 * @return KODE_OK; KODE_ERROR when the line would not arm; KODE_IO_ERROR when
 *         the C5 did not take power save; KODE_DENIED when the SDIO tasks
 *         never settled. The reason is logged.
 */
static kode_status_t go_down(void)
{
    kode_status_t st = KODE_OK;

    switch ( kradio_sleep_down(&REAL) )
    {
    case KRADIO_DOWN_OK:
        s_wake_armed = true;
        break;
    case KRADIO_DOWN_NO_LINE:
        KLOG_E(TAG, "the C5's wake line would not arm; the bus stays up");
        st = KODE_ERROR;
        break;
    case KRADIO_DOWN_NO_PS:
        KLOG_W(TAG, "the C5 did not take power save; the bus stays up");
        st = KODE_IO_ERROR;
        break;
    default:
        KLOG_W(TAG, "the SDIO tasks never settled; the bus stays up");
        st = KODE_DENIED;
        break;
    }
    return st;
}

/** @brief Build the note for the owner from the counters and hand it over. */
static void note_send(uint8_t kind, uint16_t knocks)
{
    kdrv_radio_sleep_note_t n;

    if ( NULL == s_note )
    {
        return;
    }
    n.kind = kind;
    n.knocks = knocks;
    n.tx_sta = (uint32_t)atomic_load(&s_tx[KRADIO_SLEEP_TX_STA]);
    n.tx_serial = (uint32_t)atomic_load(&s_tx[KRADIO_SLEEP_TX_SERIAL]);
    n.tx_hci = (uint32_t)atomic_load(&s_tx[KRADIO_SLEEP_TX_HCI]);
    s_note(&n, s_note_arg);
}

/**
 * @brief The way back down failed: keep the bus usable, the P4 awake, and say so.
 *
 * The bus is up (the policy leaves it so on every refusal), so the one thing
 * left to guard against is the P4 light-sleeping over it: the sentinel's lock
 * stays held until the owner starts or stops the sentinel again.
 */
static void stand_down(void)
{
    s_wake_armed = false;
    s_stood_down = true;
    (void)counter_add(&s_stood_down_count, 1U);
    KLOG_E(TAG, "the sentinel stands down; the bus is up and the P4 stays awake");
    note_send(KDRV_RADIO_WOKE_STOOD_DOWN, 0U);
}

/** @brief CMD_START. */
static kode_status_t do_start(void)
{
    kode_status_t st = KODE_OK;

    s_note = s_stage_note;
    s_note_arg = s_stage_arg;
    if ( !s_wake_armed )
    {
        khal_power_lock(s_wake_lock, true);
        st = go_down();
        if ( KODE_OK == st )
        {
            s_stood_down = false;
        }
        if ( !s_stood_down )
        {
            khal_power_lock(s_wake_lock, false);
        }
    }
    return st;
}

/** @brief CMD_STOP: serve the C5 once more and leave the bus up. */
static kode_status_t do_stop(void)
{
    uint16_t knocks = 0U;
    bool heard;

    s_note = NULL;
    s_note_arg = NULL;
    if ( !s_wake_armed )
    {
        /* The bus is up already; a stood-down sentinel lets the P4 go. */
        s_stood_down = false;
        khal_power_lock(s_wake_lock, false);
        return KODE_OK;
    }
    khal_power_lock(s_wake_lock, true);
    (void)real_bus_down(false);
    heard = kradio_sleep_up(&REAL, &knocks);
    account(heard, knocks);
    if ( heard )
    {
        s_wake_armed = false;
        khal_power_lock(s_wake_lock, false);
        return KODE_OK;
    }
    /* Deaf: the bus is still silent, so the sentinel is still the one keeping it. */
    KLOG_W(TAG, "the C5 did not answer the stop; the bus stays down");
    if ( !real_bus_down(true) )
    {
        /* The line armed at the start; it cannot fail now. Never leave the
         * tasks suspended with no way to wake them. */
        (void)real_bus_down(false);
        real_sdio_tasks(true);
        stand_down();
        return KODE_IO_ERROR;
    }
    khal_power_lock(s_wake_lock, false);
    return KODE_IO_ERROR;
}

/**
 * @brief One wake: the bus up, the note, the bus down again.
 * @param c5_line true when the C5's line woke the P4; false for the P4's own traffic.
 */
static void do_wake(bool c5_line)
{
    uint16_t knocks = 0U;
    uint8_t kind = KDRV_RADIO_WOKE_TX;
    unsigned int count = 0U;
    bool heard;

    khal_power_lock(s_wake_lock, true);
    (void)real_bus_down(false);
    heard = kradio_sleep_wake_once(&REAL, &knocks);
    account(heard, knocks);

    if ( !heard )
    {
        kind = KDRV_RADIO_WOKE_DEAF;
        count = (unsigned int)atomic_load(&s_deaf);
        KLOG_W(TAG, "the C5 did not wake (%u)", count);
    }
    else if ( c5_line )
    {
        kind = KDRV_RADIO_WOKE_C5;
        count = counter_add(&s_wakes_c5, 1U);
    }
    else
    {
        count = counter_add(&s_wakes_tx, 1U);
    }
    if ( heard && (SLEEP_LOG_FIRST >= count) )
    {
        KLOG_D(TAG, "woke #%u (kind %u), %u knock(s)", count, (unsigned int)kind, (unsigned int)knocks);
    }
    note_send(kind, knocks);

    if ( !heard )
    {
        /* The C5 still thinks the host is in power save and the SDIO tasks
         * are still suspended: nothing to announce, only the bus to mark and
         * the line to arm again. A C5 that raised the line will be knocked at
         * again.
         *
         * Until it answers, a task that sends through esp-hosted queues its
         * frame for the suspended writer and, once the writer's queue is full,
         * blocks in esp-hosted's _h_queue_item; frames queued during this
         * wake are not rung for, and wait for the line or the next frame. */
        if ( !real_bus_down(true) )
        {
            /* The line armed at the start; it cannot fail now. Never leave the
             * tasks suspended with no way to wake them. */
            (void)real_bus_down(false);
            real_sdio_tasks(true);
            stand_down();
        }
    }
    else
    {
        s_wake_armed = false;
        /* A command that waits decides: a stop finds the bus up and answers
         * at once, a start goes down itself. Going down first would make the
         * caller wait for the way down and the way up. */
        if ( !real_cmd_waiting() )
        {
            if ( go_down() != KODE_OK )
            {
                stand_down();
            }
        }
    }
    if ( !s_stood_down )
    {
        khal_power_lock(s_wake_lock, false);
    }
}

/**
 * @brief The task: wait for the doorbell, then do what was asked or what woke it.
 * @param arg Unused.
 */
static void sentinel_task(void *arg)
{
    (void)arg;
    for ( ;; )
    {
        (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sleep_cmd_t const cmd = (sleep_cmd_t)atomic_exchange(&s_cmd, (unsigned int)CMD_NONE);

        if ( CMD_START == cmd )
        {
            atomic_store(&s_cmd_status, (unsigned int)do_start());
            (void)xSemaphoreGive(s_sentinel_done);
        }
        else if ( CMD_STOP == cmd )
        {
            atomic_store(&s_cmd_status, (unsigned int)do_stop());
            (void)xSemaphoreGive(s_sentinel_done);
        }
        else if ( s_wake_armed )
        {
            bool const c5 = atomic_exchange(&s_ev_c5, false);
            bool const tx = atomic_exchange(&s_ev_tx, false);

            if ( c5 || tx )
            {
                do_wake(c5);
            }
        }
        else
        {
            /* a stale doorbell */
        }
    }
}

/** @brief Make the task, its semaphore and its lock, once. */
static kode_status_t sentinel_make(void)
{
    if ( NULL != s_sentinel )
    {
        return KODE_OK;
    }
    if ( KHAL_POWER_LOCK_NONE == s_wake_lock )
    {
        if ( khal_power_lock_new(SLEEP_LOCK_NAME, &s_wake_lock) != KODE_OK )
        {
            return KODE_NO_RESOURCES;
        }
    }
    if ( NULL == s_sentinel_done )
    {
        s_sentinel_done = xSemaphoreCreateBinary();
        if ( NULL == s_sentinel_done )
        {
            return KODE_NO_RESOURCES;
        }
    }
    if ( xTaskCreate(&sentinel_task, "radio.sleep", SLEEP_TASK_STACK, NULL,
                    SLEEP_TASK_PRIO, &s_sentinel) != pdPASS )
    {
        s_sentinel = NULL;
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

kode_status_t kdrv_radio_sleep(bool sleep, kdrv_radio_sleep_note_cb_t note, void *arg)
{
    if ( sleep )
    {
        kode_status_t const made = sentinel_make();

        if ( KODE_OK != made )
        {
            return made;
        }
    }
    else if ( NULL == s_sentinel )
    {
        return KODE_OK;     /* never started: the bus has always been up */
    }
    else
    {
        /* a stop has something to stop */
    }
    s_stage_note = note;
    s_stage_arg = arg;
    (void)xSemaphoreTake(s_sentinel_done, 0);     /* a late answer to a command that timed out */
    if ( sleep )
    {
        atomic_store(&s_cmd, (unsigned int)CMD_START);
    }
    else
    {
        atomic_store(&s_cmd, (unsigned int)CMD_STOP);
    }
    ring_task();
    if ( xSemaphoreTake(s_sentinel_done, pdMS_TO_TICKS(SLEEP_CMD_WAIT_MS)) != pdTRUE )
    {
        return KODE_TIMEOUT;
    }
    return (kode_status_t)atomic_load(&s_cmd_status);
}

void kdrv_radio_sleep_stats(kdrv_radio_sleep_stats_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    out->wakes_c5 = (uint32_t)atomic_load(&s_wakes_c5);
    out->wakes_tx = (uint32_t)atomic_load(&s_wakes_tx);
    out->deaf = (uint32_t)atomic_load(&s_deaf);
    out->stood_down = (uint32_t)atomic_load(&s_stood_down_count);
    out->knocks = (uint32_t)atomic_load(&s_knocks);
    out->knocks_max = (uint16_t)atomic_load(&s_knocks_max);
}

#endif /* CONFIG_KODE_LOWPOWER */
