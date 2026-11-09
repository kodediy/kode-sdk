/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_system_esp.c
 * @brief khal_system on ESP-IDF.
 */
#include "khal_system.h"

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "esp_chip_info.h"
#include "esp_cpu.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_memory_utils.h"
#include "esp_psram.h"
#include "esp_random.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "esp_attr.h"
#include "esp_private/esp_clk.h"
#include "soc/lp_system_reg.h"
#include "soc/rtc.h"
#include "soc/soc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

/* The board's malloc policy: plain malloc() goes to PSRAM (kbsp's sdkconfig.board). */
#if !CONFIG_SPIRAM_USE_MALLOC || (CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL != 0)
#error "this project does not carry the Kode Dot's memory policy: read sdk/components/kbsp/sdkconfig.board before your own sdkconfig defaults (CONFIG_SPIRAM_USE_MALLOC=y, CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=0)"
#endif

void khal_restart(void)
{
    esp_restart();
}

void khal_panic(const char *why)
{
    esp_system_abort(why);
}

uint32_t khal_random(void)
{
    /* Not cryptography: the RNG is fully seeded only once RF or the SAR ADC runs. */
    return esp_random();
}

void khal_panic_print(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    esp_rom_vprintf(fmt, ap);
    va_end(ap);
}

size_t khal_heap_free(void)
{
    return (size_t)esp_get_free_heap_size();
}

size_t khal_psram_free(void)
{
    /* cppcheck-suppress misra-c2012-10.1 ; MALLOC_CAP_SPIRAM is ESP-IDF's own shifted constant */
    /* cppcheck-suppress misra-c2012-12.2 ; the same constant: a shift of a signed 1 by 10 */
    return heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}

size_t khal_psram_largest(void)
{
    /* cppcheck-suppress misra-c2012-10.1 ; MALLOC_CAP_SPIRAM is ESP-IDF's own shifted constant */
    /* cppcheck-suppress misra-c2012-12.2 ; the same constant: a shift of a signed 1 by 10 */
    return heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
}

/** @brief The pool every DMA buffer and every ISR allocation comes from. */
#define SYS_HEAP_INTERNAL_CAPS \
    (MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT)

size_t khal_heap_internal_free(void)
{
    return heap_caps_get_free_size(SYS_HEAP_INTERNAL_CAPS);
}

size_t khal_heap_internal_largest(void)
{
    return heap_caps_get_largest_free_block(SYS_HEAP_INTERNAL_CAPS);
}

size_t khal_heap_internal_low(void)
{
    return heap_caps_get_minimum_free_size(SYS_HEAP_INTERNAL_CAPS);
}

/** @brief How many refusals print a line; every one is counted. */
#define SYS_HEAP_WATCH_LINES 4U

/** @brief Tasks one khal_task_list() snapshot holds; more and it holds none. */
#define SYS_TASKS_MAX 32U

/** @brief Refusals since khal_heap_watch(). Written from any context. */
static uint32_t s_alloc_failures;

/**
 * @brief Count one refused allocation, and print the first few.
 *
 * Runs in the context that asked, ISR included: no lock, no allocation.
 *
 * @param size  Bytes that were asked for.
 * @param caps  The capabilities they were asked with.
 * @param func  The heap function that failed; ESP-IDF's, not the caller's.
 */
static void heap_refused(size_t size, uint32_t caps, const char *func)
{
    const char *who = "isr";
    uint32_t const n = __atomic_add_fetch(&s_alloc_failures, 1U, __ATOMIC_RELAXED);

    (void)func;
    if ( SYS_HEAP_WATCH_LINES < n )
    {
        return;
    }
    if ( xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED )
    {
        who = "boot";
    }
    else if ( xPortInIsrContext() == pdFALSE )
    {
        who = pcTaskGetName(NULL);
    }
    else
    {
        /* An ISR has no task to name. */
    }
    khal_panic_print("\r\nkodeOS: heap refused %u bytes, caps 0x%x, task '%s'\r\n",
                     (unsigned)size, (unsigned)caps, who);
}

void khal_heap_watch(void)
{
    (void)heap_caps_register_failed_alloc_callback(&heap_refused);
}

uint32_t khal_heap_alloc_failures(void)
{
    return __atomic_load_n(&s_alloc_failures, __ATOMIC_RELAXED);
}

size_t khal_task_list(khal_task_info_t *out, size_t max, size_t *total)
{
    if ( NULL != total )
    {
        *total = 0U;
    }
#if CONFIG_FREERTOS_USE_TRACE_FACILITY
    if ( (NULL == out) || (0U == max) )
    {
        return 0U;
    }

    /* On the stack, not the heap. A too-small array gets no snapshot, so
     * total comes from the scheduler apart. */
    TaskStatus_t status[SYS_TASKS_MAX];

    if ( NULL != total )
    {
        *total = (size_t)uxTaskGetNumberOfTasks();
    }
    UBaseType_t const n = uxTaskGetSystemState(status, (UBaseType_t)SYS_TASKS_MAX,
                                               NULL);
    size_t kept = 0U;
    for ( UBaseType_t i = 0U; (i < n) && (kept < max); ++i )
    {
        const char *name = status[i].pcTaskName;
        if ( NULL == name )
        {
            name = "?";
        }
        (void)snprintf(out[kept].name, sizeof(out[kept].name), "%s", name);
        out[kept].prio = (uint8_t)status[i].uxCurrentPriority;
        out[kept].stack_free = (uint32_t)status[i].usStackHighWaterMark;
        ++kept;
    }
    return kept;
#else
    (void)out;
    (void)max;
    return 0U;
#endif
}

uint32_t khal_task_stack_free(void)
{
    /* StackType_t is a byte here, so the mark is already in bytes. */
    if ( xTaskGetCurrentTaskHandle() == NULL )
    {
        return 0U;
    }
    return (uint32_t)uxTaskGetStackHighWaterMark(NULL);
}

bool khal_task_stack_in_psram(void)
{
    /* A local lives on the caller's stack: where it lives is the answer. */
    int32_t probe = 0;

    return esp_ptr_external_ram(&probe);
}

const char *khal_task_name(void)
{
    TaskHandle_t const self = xTaskGetCurrentTaskHandle();
    if ( NULL == self )
    {
        return NULL;
    }
    return pcTaskGetName(self);
}

/* The per-task pointer lives in a FreeRTOS thread-local slot. Index 0 is the
 * pthread layer's, so the slot is the next one and kbsp's sdkconfig.board
 * asks for two. The count is Kconfig's, not configNUM_THREAD_LOCAL_STORAGE_POINTERS:
 * with deletion callbacks on, FreeRTOS doubles that one to make room for the
 * callbacks, and the doubled half is not a slot. */
_Static_assert(KHAL_TASK_SLOT_INDEX < CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS,
               "raise CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS in kbsp's sdkconfig.board");

/** @brief Where a started task finds its function and argument. */
typedef struct {
    khal_task_fn_t fn;   /**< What the task runs. */
    void          *arg;  /**< Handed to @c fn. */
} sys_task_ctx_t;

/** @brief Alignment of a task's context block, in bytes. */
#define SYS_TASK_CTX_ALIGN 8U

/** @brief Body of every started task: take the context, run the function, end. */
static void sys_task_main(void *ctx_ptr)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the task parameter is the void * khal_task_start() passed */
    sys_task_ctx_t *const ctx = (sys_task_ctx_t *)ctx_ptr;
    khal_task_fn_t const fn = ctx->fn;
    void *const arg = ctx->arg;

    khal_free(ctx);
    fn(arg);
    vTaskDeleteWithCaps(NULL);
}

kode_status_t khal_task_start(khal_task_fn_t fn, const char *name,
                              uint32_t stack_bytes, uint32_t prio, void *arg)
{
    if ( (NULL == fn) || (NULL == name) || (0U == stack_bytes) )
    {
        return KODE_DENIED;
    }
    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is the context */
    sys_task_ctx_t *const ctx = khal_alloc_psram(SYS_TASK_CTX_ALIGN, sizeof(*ctx));
    if ( NULL == ctx )
    {
        return KODE_NO_RESOURCES;
    }
    ctx->fn = fn;
    ctx->arg = arg;
    /* cppcheck-suppress misra-c2012-10.1 ; MALLOC_CAP_SPIRAM is ESP-IDF's own shifted constant */
    if ( xTaskCreatePinnedToCoreWithCaps(sys_task_main, name, stack_bytes, ctx,
                                         (UBaseType_t)prio, NULL, tskNO_AFFINITY,
                                         MALLOC_CAP_SPIRAM) != pdPASS )
    {
        khal_free(ctx);
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

void *khal_task_self(void)
{
    return (void *)xTaskGetCurrentTaskHandle();
}

void *khal_task_slot_get(void)
{
    return pvTaskGetThreadLocalStoragePointer(NULL, (BaseType_t)KHAL_TASK_SLOT_INDEX);
}

void khal_task_slot_set(void *p)
{
    vTaskSetThreadLocalStoragePointer(NULL, (BaseType_t)KHAL_TASK_SLOT_INDEX, p);
}

/* The slot's owner is freed by FreeRTOS's own deletion callback, which exists
 * only when the board asks for it (kbsp's sdkconfig.board does). */
#if !CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS
#error "khal_task_slot_set_owned() needs CONFIG_FREERTOS_TLSP_DELETION_CALLBACKS=y: read sdk/components/kbsp/sdkconfig.board before your own sdkconfig defaults"
#endif

/** @brief What the latest khal_task_slot_set_owned() asked to run; one owner uses the slot. */
static khal_task_slot_free_t s_slot_free;

/**
 * @brief FreeRTOS's deletion callback for the slot: hand the pointer to its owner.
 *
 * FreeRTOS calls it from prvDeleteTCB(): in the task that deletes this one,
 * which for a task ending through vTaskDeleteWithCaps(NULL) is the temporary
 * clean-up task, or in the idle task when a task ends through vTaskDelete(NULL),
 * as app_main's does. Outside any critical section, but it must not block.
 *
 * @param index The slot's index, unused.
 * @param p     What the task left in the slot.
 */
static void sys_slot_deleted(int index, void *p)
{
    (void)index;
    if ( (NULL != p) && (NULL != s_slot_free) )
    {
        s_slot_free(p);
    }
}

void khal_task_slot_set_owned(void *p, khal_task_slot_free_t free_fn)
{
    TlsDeleteCallbackFunction_t del_cb = NULL;

    if ( NULL != free_fn )
    {
        s_slot_free = free_fn;
        del_cb = &sys_slot_deleted;
    }
    vTaskSetThreadLocalStoragePointerAndDelCallback(NULL, (BaseType_t)KHAL_TASK_SLOT_INDEX, p, del_cb);
}

/** @brief Stack of the supervisor task, in bytes (StackType_t is a byte here). */
#define SYS_WATCH_STACK_BYTES 3072U

/** @brief Alignment of the supervisor's stack, in bytes (the RISC-V ABI's). */
#define SYS_WATCH_STACK_ALIGN 16U

/** @brief Milliseconds in a second, to turn the task watchdog's Kconfig seconds. */
#define SYS_MS_PER_S 1000U

/** @brief Name of the supervisor task, as `tasks` and the watchdog print it. */
#define SYS_WATCH_TASK_NAME "kqp.watch"

#if CONFIG_ESP_TASK_WDT_EN
/* Idle tasks the project's Kconfig asks the task watchdog to check. */
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0
#define SYS_WATCH_IDLE0 (1U << 0)
#else
#define SYS_WATCH_IDLE0 0U
#endif
#if CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1
#define SYS_WATCH_IDLE1 (1U << 1)
#else
#define SYS_WATCH_IDLE1 0U
#endif

/** @brief The supervisor's stack: internal RAM, as every static is. */
static StackType_t s_watch_stack[SYS_WATCH_STACK_BYTES]
    __attribute__((aligned(SYS_WATCH_STACK_ALIGN)));
/** @brief The supervisor's control block. */
static StaticTask_t s_watch_tcb;
/** @brief What the task runs once a period. */
static void (*s_watch_pass)(void);
/** @brief The sleep between passes, in scheduler ticks. */
static TickType_t s_watch_ticks;
/** @brief True once the task exists; a second start is refused. */
static bool s_watch_started;
/** @brief The task, for khal_watch_rest() to wake it. */
static TaskHandle_t s_watch_task;
/** @brief The board sleeps: khal_watch_rest(). */
static volatile bool s_watch_resting;

/**
 * @brief The supervisor's loop: sleep, run the pass, feed the watchdog.
 *
 * While the board sleeps it is out of the watchdog and waits for the word
 * that the board is up, with no timeout, so it wakes nothing. The period's
 * sleep is a wait on the same notification: one that comes early only brings
 * the next pass forward.
 *
 * @param arg Unused.
 */
static void watch_task(void *arg)
{
    (void)arg;
    for ( ;; )
    {
        if ( s_watch_resting )
        {
            (void)esp_task_wdt_delete(NULL);
            while ( s_watch_resting )
            {
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            }
            (void)esp_task_wdt_add(NULL);
        }
        (void)ulTaskNotifyTake(pdTRUE, s_watch_ticks);
        s_watch_pass();
        (void)esp_task_wdt_reset();
    }
}

kode_status_t khal_watch_start(void (*pass)(void), uint32_t period_ms)
{
    kode_status_t st = KODE_OK;
    TickType_t const ticks = pdMS_TO_TICKS(period_ms);

    if ( s_watch_started || (NULL == pass) || (0U == ticks) )
    {
        st = KODE_DENIED;
    }
    else
    {
        /* Keep the project's timeout and idle checks; only panic is ours. */
        esp_task_wdt_config_t const cfg = {
            .timeout_ms = (uint32_t)CONFIG_ESP_TASK_WDT_TIMEOUT_S * SYS_MS_PER_S,
            .idle_core_mask = SYS_WATCH_IDLE0 | SYS_WATCH_IDLE1,
            .trigger_panic = true,
        };
        /* A project that left the watchdog uninitialised has nothing to
         * reconfigure; the init is then the same call with the same values. */
        esp_err_t err = esp_task_wdt_reconfigure(&cfg);
        if ( ESP_OK != err )
        {
            err = esp_task_wdt_init(&cfg);
        }
        if ( ESP_OK != err )
        {
            st = KODE_ERROR;
        }
    }

    if ( KODE_OK == st )
    {
        s_watch_pass = pass;
        s_watch_ticks = ticks;
        TaskHandle_t const task = xTaskCreateStatic(
            &watch_task, SYS_WATCH_TASK_NAME, SYS_WATCH_STACK_BYTES, NULL,
            (UBaseType_t)(configMAX_PRIORITIES - 1), s_watch_stack, &s_watch_tcb);
        /* Subscribed from here, not from the task: a refusal is then an
         * answer, and the task's first feed comes a whole period later. */
        if ( (NULL == task) || (ESP_OK != esp_task_wdt_add(task)) )
        {
            if ( NULL != task )
            {
                vTaskDelete(task);
            }
            st = KODE_ERROR;
        }
        else
        {
            s_watch_task    = task;
            s_watch_started = true;
        }
    }
    return st;
}

void khal_watch_rest(bool rest)
{
    s_watch_resting = rest;
    /* Going to sleep needs no wake: the task finds out at its next one. */
    if ( (!rest) && s_watch_started )
    {
        (void)xTaskNotifyGive(s_watch_task);
    }
}
#else
kode_status_t khal_watch_start(void (*pass)(void), uint32_t period_ms)
{
    /* No task watchdog in this build: nothing would watch the supervisor. */
    (void)pass;
    (void)period_ms;
    return KODE_DENIED;
}

void khal_watch_rest(bool rest)
{
    /* No supervisor to rest. */
    (void)rest;
}
#endif /* CONFIG_ESP_TASK_WDT_EN */

/** @brief The silicon revision is reported as MXX: major hundreds, minor units. */
#define SYS_CHIP_REV_DIVISOR  100U

kode_status_t khal_chip_info(khal_chip_info_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    (void)memset(out, 0, sizeof(*out));

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    if ( CHIP_ESP32P4 == chip.model )
    {
        out->model = "ESP32-P4";
    }
    else
    {
        out->model = "unknown chip";
    }
    out->rev_major = (uint8_t)((uint32_t)chip.revision / SYS_CHIP_REV_DIVISOR);
    out->rev_minor = (uint8_t)((uint32_t)chip.revision % SYS_CHIP_REV_DIVISOR);
    out->psram_bytes = (uint32_t)esp_psram_get_size();

    uint32_t flash = 0U;
    if ( esp_flash_get_size(NULL, &flash) != ESP_OK )
    {
        return KODE_ERROR;
    }
    out->flash_bytes = flash;
    return KODE_OK;
}

void *khal_alloc_psram(size_t align, size_t size)
{
    return heap_caps_aligned_calloc(align, 1U, size, MALLOC_CAP_SPIRAM);
}

void *khal_realloc_psram(void *p, size_t size)
{
    return heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM);
}

void khal_free(void *p)
{
    heap_caps_free(p);
}

/* ---- The 32 kHz crystal across a warm boot ---------------------------------
 * select_rtc_slow_clk() calibrates the crystal twice on every boot, 3000 of its
 * cycles each: 183 ms of the boot spent asking a clock that has been running
 * all along. The LP domain outlives a software reset, the crystal and the
 * selector with it, and so does the last calibration (RTC_SLOW_CLK_CAL_REG). So
 * when the selector still stands on the crystal the two first calls answer the
 * last value. A power-on, or any reset that took the selector back to the RC,
 * calibrates in full: there the first call is what proves the crystal is
 * running. Light sleep calibrates again by itself, on every sleep. */
#define SYS_XTAL32K_CAL_NOMINAL 16000000U   /**< 32768 Hz in rtc_clk_cal()'s 13.19 microseconds. */
#define SYS_XTAL32K_CAL_MARGIN    160000U   /**< 1 %: a stored value further off is not this crystal's. */
#define SYS_BOOT_CALS                  2U   /**< How many calls select_rtc_slow_clk() makes. */

uint32_t __real_rtc_clk_cal(soc_clk_freq_calculation_src_t cal_clk_sel, uint32_t slowclk_cycles);
uint32_t __wrap_rtc_clk_cal(soc_clk_freq_calculation_src_t cal_clk_sel, uint32_t slowclk_cycles);

/**
 * @brief rtc_clk_cal() that skips the boot's two crystal calibrations when the last one holds.
 *
 * The kept value answers only when the slow clock still stands on the 32 kHz
 * crystal and the value is within 1 % of the nominal; otherwise, and after
 * those two calls, it calibrates in full.
 *
 * @param cal_clk_sel    The clock to calibrate.
 * @param slowclk_cycles How many of its cycles to count.
 * @return The clock's period, in rtc_clk_cal()'s 13.19 microseconds.
 */
uint32_t IRAM_ATTR __wrap_rtc_clk_cal(soc_clk_freq_calculation_src_t cal_clk_sel, uint32_t slowclk_cycles)
{
    static DRAM_ATTR uint8_t  s_boot_calls;
    static DRAM_ATTR uint32_t s_kept_cal;

    if ( SYS_BOOT_CALS > s_boot_calls )
    {
        if ( 0U == s_boot_calls )
        {
            uint32_t const cal = esp_clk_slowclk_cal_get();

            if ( (SOC_RTC_SLOW_CLK_SRC_XTAL32K == rtc_clk_slow_src_get())
                && (SYS_XTAL32K_CAL_NOMINAL - SYS_XTAL32K_CAL_MARGIN <= cal)
                && (SYS_XTAL32K_CAL_NOMINAL + SYS_XTAL32K_CAL_MARGIN >= cal) )
            {
                s_kept_cal = cal;
            }
        }
        ++s_boot_calls;
        if ( 0U != s_kept_cal )
        {
            return s_kept_cal;
        }
    }
    return __real_rtc_clk_cal(cal_clk_sel, slowclk_cycles);
}

/* ---- The PSRAM's timing across a warm boot ----------------------------------
 * mspi_timing_psram_tuning() sweeps 4 phases and 31 delay lines, 100 reads each:
 * 153 ms of every boot, and the answer is the same as the last time, a few degrees
 * away. The sweep's two answers are kept in an LP store register and tried first:
 * sixteen reads of the test pattern, and a single wrong byte means the full sweep.
 * So does the register being blank, and so does every SYS_TUNE_USES-th boot, for a
 * board that has drifted slowly into a margin the reads do not see. All of it runs
 * before the PSRAM and the cache are up: IRAM and DRAM only, and nothing logs. */
#define SYS_TUNE_REG        LP_SYSTEM_REG_LP_STORE13_REG
#define SYS_TUNE_MAGIC      0x4B54U  /**< In the register's top half. */
#define SYS_TUNE_MAGIC_AT   16U
#define SYS_TUNE_USES_AT    8U
#define SYS_TUNE_USES_MASK  0xFFU
#define SYS_TUNE_PHASE_AT   5U
#define SYS_TUNE_PHASE_MASK 0x07U
#define SYS_TUNE_DELAY_MASK 0x1FU
#define SYS_TUNE_PHASES     4U       /**< mspi_timing_get_psram_tuning_phases(). */
#define SYS_TUNE_DELAYS     31U      /**< mspi_timing_get_psram_tuning_delaylines(). */
#define SYS_TUNE_USES       200U
#define SYS_TUNE_READS      16U
#define SYS_TUNE_DATA_LEN   128U     /**< MSPI_TIMING_TEST_DATA_LEN. */
#define SYS_TUNE_DATA_ADDR  0x80U    /**< MSPI_TIMING_PSRAM_TEST_DATA_ADDR. */
#define SYS_TUNE_CFG_WORDS  64U      /**< Room for a mspi_timing_config_t, private to IDF. */

/* IDF's own, declared here because the header that has them is private to it. */
void mspi_timing_enter_low_speed_early(void);
void mspi_timing_enter_high_speed_early(void);
void mspi_timing_psram_init(uint32_t psram_freq_mhz);
void mspi_timing_config_psram_prepare_reference_data(uint8_t *buf, uint32_t len);
void mspi_timing_config_psram_write_data(uint8_t *buf, uint32_t addr, uint32_t len);
void mspi_timing_config_psram_read_data(uint8_t *buf, uint32_t addr, uint32_t len);
void mspi_timing_get_psram_tuning_phases(void *configs);
void mspi_timing_get_psram_tuning_delaylines(void *configs);
void mspi_timing_config_psram_set_tuning_delayline(const void *configs, uint8_t id);
void __real_mspi_timing_psram_tuning(void);
void __real_mspi_timing_psram_set_best_tuning_phase(const void *configs, uint8_t best_id);
void __real_mspi_timing_psram_set_best_tuning_delayline(const void *configs, uint8_t best_id);
void __wrap_mspi_timing_psram_tuning(void);
void __wrap_mspi_timing_psram_set_best_tuning_phase(const void *configs, uint8_t best_id);
void __wrap_mspi_timing_psram_set_best_tuning_delayline(const void *configs, uint8_t best_id);

static DRAM_ATTR uint8_t s_tune_phase;   /**< What the sweep chose. */
static DRAM_ATTR uint8_t s_tune_delay;

/**
 * @brief mspi_timing_psram_set_best_tuning_phase() that also notes the phase, to keep it.
 *
 * @param configs IDF's tuning configs.
 * @param best_id The phase the sweep chose.
 */
void IRAM_ATTR __wrap_mspi_timing_psram_set_best_tuning_phase(const void *configs, uint8_t best_id)
{
    s_tune_phase = best_id;
    __real_mspi_timing_psram_set_best_tuning_phase(configs, best_id);
}

/**
 * @brief mspi_timing_psram_set_best_tuning_delayline() that also notes the line, to keep it.
 *
 * @param configs IDF's tuning configs.
 * @param best_id The delay line the sweep chose.
 */
void IRAM_ATTR __wrap_mspi_timing_psram_set_best_tuning_delayline(const void *configs, uint8_t best_id)
{
    s_tune_delay = best_id;
    __real_mspi_timing_psram_set_best_tuning_delayline(configs, best_id);
}

/**
 * @brief Try the kept timing: every read of the pattern must come back whole.
 *
 * @param phase The phase's id.
 * @param delay The delay line's id.
 * @return true with the kept timing set; false with the registers to be swept.
 */
static bool IRAM_ATTR tune_try(uint8_t phase, uint8_t delay)
{
    uint32_t cfg[SYS_TUNE_CFG_WORDS];
    uint8_t  ref[SYS_TUNE_DATA_LEN];
    uint8_t  got[SYS_TUNE_DATA_LEN];
    bool     good = true;

    mspi_timing_enter_low_speed_early();
    mspi_timing_config_psram_prepare_reference_data(ref, SYS_TUNE_DATA_LEN);
    mspi_timing_config_psram_write_data(ref, SYS_TUNE_DATA_ADDR, SYS_TUNE_DATA_LEN);
    mspi_timing_psram_init(CONFIG_SPIRAM_SPEED);
    mspi_timing_get_psram_tuning_phases(cfg);
    __real_mspi_timing_psram_set_best_tuning_phase(cfg, phase);
    mspi_timing_get_psram_tuning_delaylines(cfg);
    __real_mspi_timing_psram_set_best_tuning_delayline(cfg, delay);
    mspi_timing_config_psram_set_tuning_delayline(cfg, delay);
    for ( uint32_t r = 0U; good && (SYS_TUNE_READS > r); ++r )
    {
        mspi_timing_config_psram_read_data(got, SYS_TUNE_DATA_ADDR, SYS_TUNE_DATA_LEN);
        for ( uint32_t i = 0U; SYS_TUNE_DATA_LEN > i; ++i )
        {
            good = good && (ref[i] == got[i]);
        }
    }
    return good;
}

/**
 * @brief mspi_timing_psram_tuning() that tries the timing kept in an LP register before sweeping.
 *
 * It sweeps in full, and keeps the answer, when the register is blank, the
 * kept timing fails a read, or it has served SYS_TUNE_USES boots. Runs before
 * the PSRAM and the cache are up: IRAM and DRAM only, and nothing logs.
 */
void IRAM_ATTR __wrap_mspi_timing_psram_tuning(void)
{
    uint32_t const word  = REG_READ(SYS_TUNE_REG);
    uint32_t const uses  = (word >> SYS_TUNE_USES_AT) & SYS_TUNE_USES_MASK;
    uint8_t const  phase = (uint8_t)((word >> SYS_TUNE_PHASE_AT) & SYS_TUNE_PHASE_MASK);
    uint8_t const  delay = (uint8_t)(word & SYS_TUNE_DELAY_MASK);

    if ( ((word >> SYS_TUNE_MAGIC_AT) == SYS_TUNE_MAGIC) && (SYS_TUNE_USES > uses)
        && (SYS_TUNE_PHASES > phase) && (SYS_TUNE_DELAYS > delay)
        && tune_try(phase, delay) )
    {
        REG_WRITE(SYS_TUNE_REG, (word & ~((uint32_t)SYS_TUNE_USES_MASK << SYS_TUNE_USES_AT))
                                | ((uses + 1U) << SYS_TUNE_USES_AT));
        mspi_timing_enter_high_speed_early();
        return;
    }
    __real_mspi_timing_psram_tuning();
    REG_WRITE(SYS_TUNE_REG, ((uint32_t)SYS_TUNE_MAGIC << SYS_TUNE_MAGIC_AT)
                            | ((uint32_t)s_tune_phase << SYS_TUNE_PHASE_AT)
                            | (uint32_t)s_tune_delay);
}

#if !CONFIG_FREERTOS_UNICORE
/** @brief Stack of the one-shot task that releases core 1, in bytes. */
#define SYS_CORE1_RELEASE_STACK 1024U
#define SYS_CORE_PRO 0   /**< The core that starts the scheduler. */
#define SYS_CORE_APP 1   /**< The core held still meanwhile. */

/**
 * @brief Release core 1, then delete itself: the first task core 0 runs.
 *
 * @param arg Unused.
 */
static void core1_release_task(void *arg)
{
    (void)arg;
    esp_cpu_unstall(SYS_CORE_APP);
    vTaskDelete(NULL);
}

/**
 * @brief Hold core 1 still while core 0 starts the scheduler.
 *
 * ESP-IDF's weak hook, on core 0 before vTaskStartScheduler(). Core 1's wait
 * loop otherwise races the new stacks' fills into a store fault on core 0.
 */
void port_start_app_hook(void);
void port_start_app_hook(void)
{
    esp_cpu_stall(SYS_CORE_APP);
    if ( xTaskCreatePinnedToCore(core1_release_task, "core1_go", SYS_CORE1_RELEASE_STACK, NULL,
                                (UBaseType_t)(configMAX_PRIORITIES - 1), NULL, SYS_CORE_PRO) != pdPASS )
    {
        esp_cpu_unstall(SYS_CORE_APP);
    }
}
#endif /* !CONFIG_FREERTOS_UNICORE */
