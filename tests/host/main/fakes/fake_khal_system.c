/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_system.c
 * @brief The system seam, with a restart a case can survive.
 */
#include <stdatomic.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdkconfig.h"
#include "fakes.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "esp_image_format.h"

#include "khal_system.h"
#include "kqp_config.h"

/* The task slots' owners and the one pointer a case follows; see
 * khal_task_slot_set_owned() below. Up here with what fake_system_reset()
 * clears. */
static atomic_uint           s_slot_owned;           /**< Slots given an owner since the reset. */
static _Atomic(void *)       s_slot_watched;         /**< The pointer fake_task_slot_watch() follows. */
static atomic_uint           s_slot_watch_frees;     /**< khal_free() calls with it. */
static SemaphoreHandle_t     s_slot_ended;           /**< Given once its owner has run. */

static jmp_buf  s_landing;
static bool     s_armed;
static unsigned s_restarts;
static unsigned s_panics;

/* The entropy source, made repeatable. A case about the infrared row asserts
 * that two attempts drew DIFFERENT nonces, which a real random source would
 * satisfy only most of the time - and a suite that fails once in a few
 * hundred runs is worse than one that never tests the property at all. So the
 * host gets a linear congruential sequence reseeded before every case: the
 * values differ from each other, which is what the row is judged on, and they
 * are the same on every run, which is what a suite is judged on. */
static uint32_t s_rng;
static esp_err_t s_image_verify_result = ESP_OK;

/* How many of the next khal_alloc_psram() calls answer NULL. Up here with
 * what fake_system_reset() clears before every case, and apart from the
 * allocation counters further down, which only the cases that read them
 * clear. */
static unsigned s_psram_refusals;

/* How many khal_alloc_psram() calls still succeed before those refusals
 * start: 0 unless a case asked for a refusal partway through. */
static unsigned s_psram_grants;

/* What khal_task_stack_in_psram() answers: a host stack is never in PSRAM
 * unless a case says so. */
static bool s_stack_in_psram;

/* What khal_heap_alloc_failures() answers: the refusals a board would have
 * counted, which a host with gigabytes to give never produces. */
static uint32_t s_alloc_failures;

/* What khal_heap_internal_low() answers. A healthy board by default, so the
 * `mem` row passes in every case that is not about it. */
#define HEAP_LOW_DEFAULT  (96U * 1024U)
static size_t s_internal_low = HEAP_LOW_DEFAULT;

/* What khal_psram_free() answers. Two blocks over the PSRAM row's reserve by
 * default, so the row passes quickly in every run that is not about it. */
#define PSRAM_FREE_DEFAULT \
    (KODE_PSRAM_TEST_RESERVE_BYTES + (2U * KODE_PSRAM_TEST_BLOCK_BYTES))
static size_t s_psram_free = PSRAM_FREE_DEFAULT;

/* PSRAM held through khal_alloc_psram() since the case began. A board's
 * free count falls as blocks are taken, and so does khal_psram_free() here:
 * every block carries its size in a header in front of it. */
static size_t s_psram_held;

/* What khal_psram_largest() answers at most: a heap with no fragmentation
 * by default, so the largest block is whatever is free. */
#define PSRAM_LARGEST_DEFAULT  SIZE_MAX
static size_t s_psram_largest = PSRAM_LARGEST_DEFAULT;

/** @brief What sits in front of every fake PSRAM block. */
typedef union {
    size_t      size;   /**< The block's length, for khal_free().  */
    max_align_t align;  /**< Keeps the block itself aligned.       */
} psram_hdr_t;

/** @brief The seed every case starts from; any odd value would do. */
#define RNG_SEED  0x1234ABCDU

void fake_system_reset(void)
{
    s_armed                = false;
    s_restarts             = 0U;
    s_panics               = 0U;
    s_rng                  = RNG_SEED;
    s_image_verify_result  = ESP_OK;
    s_psram_refusals       = 0U;
    s_psram_grants         = 0U;
    s_stack_in_psram       = false;
    s_alloc_failures       = 0U;
    s_internal_low         = HEAP_LOW_DEFAULT;
    s_psram_free           = PSRAM_FREE_DEFAULT;
    s_psram_held           = 0U;
    s_psram_largest        = PSRAM_LARGEST_DEFAULT;
    atomic_store(&s_slot_owned, 0U);
    fake_task_slot_watch(NULL);
}

void fake_psram_set_largest(size_t bytes)
{
    s_psram_largest = bytes;
}

size_t khal_psram_largest(void)
{
    size_t const free_b = khal_psram_free();

    if ( s_psram_largest < free_b )
    {
        return s_psram_largest;
    }
    return free_b;
}

/** @brief Count @p size bytes out of the held total, never below zero. */
static void psram_unhold(size_t size)
{
    if ( s_psram_held >= size )
    {
        s_psram_held -= size;
    }
    else
    {
        s_psram_held = 0U;
    }
}

void fake_psram_set_free(size_t bytes)
{
    s_psram_free = bytes;
}

size_t khal_psram_free(void)
{
    if ( s_psram_free > s_psram_held )
    {
        return s_psram_free - s_psram_held;
    }
    return 0U;
}

uint32_t khal_random(void)
{
    /* Numerical Recipes' constants: full period over 32 bits, so no two
     * consecutive draws are equal. */
    s_rng = (s_rng * 1664525U) + 1013904223U;
    return s_rng;
}

unsigned fake_system_restarts(void)
{
    return s_restarts;
}

jmp_buf *fake_system_arm_restart(void)
{
    s_armed = true;
    return &s_landing;
}

void khal_restart(void)
{
    ++s_restarts;
    if ( s_armed )
    {
        s_armed = false;
        longjmp(s_landing, 1);
    }
    /* Nobody armed a landing point, and this function may not return. Ending
     * the process is the honest answer: a case that reaches a restart it did
     * not expect has already stopped testing what it thought it was. */
    (void)fprintf(stderr, "khal_restart() with no landing point armed\n");
    exit(99);
}

unsigned fake_system_panics(void)
{
    return s_panics;
}

void khal_panic(const char *why)
{
    (void)why;
    ++s_panics;
    if ( s_armed )
    {
        s_armed = false;
        longjmp(s_landing, 1);
    }
    /* Same reasoning as khal_restart() above: noreturn, no landing point
     * armed, so the process ends rather than falling through into whatever
     * a case did not mean to test. */
    (void)fprintf(stderr, "khal_panic() with no landing point armed\n");
    exit(99);
}

/* A board like the bench one: the numbers the banner prints are not asserted
 * on, only that they are here. */
#define FAKE_CHIP_REV_MAJOR  3U                    /**< Silicon v3.2.        */
#define FAKE_CHIP_REV_MINOR  2U                    /**< Silicon v3.2.        */
#define FAKE_CHIP_FLASH      (32U * 1024U * 1024U) /**< The 32 MB NOR.       */
#define FAKE_CHIP_PSRAM      (32U * 1024U * 1024U) /**< The 32 MB HEX PSRAM. */

/** @brief The chip's own MAC, e8:f6:0a:00:00:01: a P4's prefix, as its USB port gives it. */
static const uint8_t FAKE_CHIP_MAC[KHAL_CHIP_MAC_LEN] = { 0xE8U, 0xF6U, 0x0AU, 0x00U, 0x00U, 0x01U };

kode_status_t khal_chip_info(khal_chip_info_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    out->model       = "ESP32-P4";
    out->rev_major   = (uint8_t)FAKE_CHIP_REV_MAJOR;
    out->rev_minor   = (uint8_t)FAKE_CHIP_REV_MINOR;
    out->flash_bytes = FAKE_CHIP_FLASH;
    out->psram_bytes = FAKE_CHIP_PSRAM;
    (void)memcpy(out->mac, FAKE_CHIP_MAC, sizeof(out->mac));
    return KODE_OK;
}

size_t khal_heap_free(void)
{
    return 64U * 1024U;
}

size_t khal_heap_internal_free(void)
{
    return 32U * 1024U;
}

size_t khal_heap_internal_largest(void)
{
    return 16U * 1024U;
}

size_t khal_heap_internal_low(void)
{
    return s_internal_low;
}

/* A host task's stack is the host's own, and nobody judges it here: the
 * figure is only ever logged. */
uint32_t khal_task_stack_free(void)
{
    return 2048U;
}

void fake_task_stack_in_psram_set(bool on)
{
    s_stack_in_psram = on;
}

bool khal_task_stack_in_psram(void)
{
    return s_stack_in_psram;
}

void fake_heap_set_internal_low(size_t bytes)
{
    s_internal_low = bytes;
}

void fake_heap_set_alloc_failures(uint32_t n)
{
    s_alloc_failures = n;
}

uint32_t khal_heap_alloc_failures(void)
{
    return s_alloc_failures;
}

void khal_heap_watch(void)
{
    /* The host has no pool to watch. */
}

/* What khal_watch_rest() was told last, and how many times it was told. */
static bool     s_watch_resting;
static unsigned s_watch_rests;

void khal_watch_rest(bool rest)
{
    s_watch_resting = rest;
    s_watch_rests += 1U;
}

bool fake_watch_resting(void)
{
    return s_watch_resting;
}

unsigned fake_watch_rest_calls(void)
{
    return s_watch_rests;
}

void khal_keep_init(void)
{
    /* The host has no PSRAM to keep. */
}

size_t khal_task_list(khal_task_info_t *out, size_t max, size_t *total)
{
    (void)out;
    (void)max;
    if ( NULL != total )
    {
        *total = 0U;
    }
    return 0U;
}

const char *khal_task_name(void)
{
    return "host";
}

/** @brief The priority the calling task is at, as the cases set and read it. */
static uint32_t s_task_priority = 1U;

uint32_t khal_task_priority_set(uint32_t priority)
{
    uint32_t const was = s_task_priority;

    s_task_priority = priority;
    return was;
}

void fake_task_priority_set(uint32_t priority)
{
    s_task_priority = priority;
}

uint32_t fake_task_priority(void)
{
    return s_task_priority;
}

/* The task helpers call the same FreeRTOS functions the board does, on the
 * linux port: a started task is a real thread, and the slot is a real
 * thread-local pointer. Only the stack's home differs - the port allocates it. */
_Static_assert(KHAL_TASK_SLOT_INDEX < CONFIG_FREERTOS_THREAD_LOCAL_STORAGE_POINTERS,
               "tests/host/sdkconfig.defaults must carry the board's thread-local slot count");

/** @brief Where a started task finds its function and argument. */
typedef struct {
    khal_task_fn_t fn;   /**< What the task runs. */
    void          *arg;  /**< Handed to @c fn. */
} fake_task_ctx_t;

static void fake_slot_ends(void);

/** @brief Body of every started task: take the context, run the function, end. */
static void fake_task_main(void *ctx_ptr)
{
    fake_task_ctx_t *const ctx = (fake_task_ctx_t *)ctx_ptr;
    khal_task_fn_t const fn = ctx->fn;
    void *const arg = ctx->arg;

    free(ctx);
    fn(arg);
    fake_slot_ends();
    vTaskDelete(NULL);
}

kode_status_t khal_task_start(khal_task_fn_t fn, const char *name,
                              uint32_t stack_bytes, uint32_t prio, void *arg)
{
    if ( (NULL == fn) || (NULL == name) || (0U == stack_bytes) )
    {
        return KODE_DENIED;
    }
    fake_task_ctx_t *const ctx = malloc(sizeof(*ctx));
    if ( NULL == ctx )
    {
        return KODE_NO_RESOURCES;
    }
    ctx->fn  = fn;
    ctx->arg = arg;
    if ( xTaskCreate(fake_task_main, name, stack_bytes, ctx,
                    (UBaseType_t)prio, NULL) != pdPASS )
    {
        free(ctx);
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

/* A slot with an owner is freed when its task ends. On the board FreeRTOS
 * runs the owner from the task's deletion callback, in the task that deletes
 * this one or in the idle task. The linux port of ESP-IDF v6.1 defines
 * portCLEAN_UP_TCB() as vPortCancelThread() alone, so it never runs a
 * deletion callback: this fake runs the owner itself, at the end of every
 * task khal_task_start() made, in that task. What the board runs (the
 * callback, from prvDeleteTCB()) is read from the source, not run here.
 *
 * The owner is a per-task fact, so it is kept per thread: every task here is
 * a thread of the linux port. Counters are for ONE watched pointer: tasks of
 * earlier cases end in the background, and a total of all of them would move
 * under a case that reads it. */
static _Thread_local khal_task_slot_free_t t_slot_free;   /**< The owner this task gave its slot. */

/** @brief The task's function has returned: run the slot's owner, then tell a case that waits. */
static void fake_slot_ends(void)
{
    void *const p = khal_task_slot_get();

    if ( (NULL != p) && (NULL != t_slot_free) )
    {
        t_slot_free(p);
        if ( atomic_load(&s_slot_watched) == p )
        {
            (void)xSemaphoreGive(s_slot_ended);
        }
    }
}

void khal_task_slot_set_owned(void *p, khal_task_slot_free_t free_fn)
{
    t_slot_free = free_fn;
    if ( NULL != free_fn )
    {
        atomic_fetch_add(&s_slot_owned, 1U);
    }
    khal_task_slot_set(p);
}

void fake_task_slot_watch(void *p)
{
    if ( NULL == s_slot_ended )
    {
        s_slot_ended = xSemaphoreCreateBinary();
    }
    while ( xSemaphoreTake(s_slot_ended, 0U) == pdTRUE )
    {
    }
    atomic_store(&s_slot_watched, p);
    atomic_store(&s_slot_watch_frees, 0U);
}

unsigned fake_task_slot_watch_frees(void)
{
    return atomic_load(&s_slot_watch_frees);
}

bool fake_task_slot_wait_ended(uint32_t ms)
{
    return (NULL != s_slot_ended) && (xSemaphoreTake(s_slot_ended, pdMS_TO_TICKS(ms)) == pdTRUE);
}

uint32_t fake_task_slot_owned(void)
{
    return atomic_load(&s_slot_owned);
}

/* A plain counter, not a mock: most cases here have no reason to care who
 * allocates PSRAM, but the mirror's pause/resume behaviour is specifically
 * about whether a buffer is held or released, and there is otherwise no way
 * for a host case to observe that a free() genuinely happened. */
static unsigned s_psram_allocs;
static unsigned s_psram_frees;

void fake_psram_reset(void)
{
    s_psram_allocs = 0U;
    s_psram_frees  = 0U;
}

unsigned fake_psram_alloc_count(void)
{
    return s_psram_allocs;
}

unsigned fake_psram_free_count(void)
{
    return s_psram_frees;
}

void fake_psram_refuse(unsigned n)
{
    s_psram_refusals = n;
}

void fake_psram_refuse_after(unsigned ok, unsigned n)
{
    s_psram_grants   = ok;
    s_psram_refusals = n;
}

void *khal_alloc_psram(size_t align, size_t size)
{
    (void)align;
    if ( 0U < s_psram_grants )
    {
        --s_psram_grants;
    }
    else if ( 0U < s_psram_refusals )
    {
        --s_psram_refusals;
        return NULL;
    }
    else
    {
        /* Nothing armed: the host's heap answers. */
    }
    psram_hdr_t *const h = calloc(1U, sizeof(psram_hdr_t) + size);
    if ( NULL == h )
    {
        return NULL;
    }
    h->size = size;
    s_psram_held += size;
    ++s_psram_allocs;
    return &h[1];
}

/* A resize is not counted: it is neither an allocation nor a release, and
 * the one thing the counters answer - "is a buffer currently held" - must not
 * move because a block changed size. From NULL it is an allocation, as C's
 * realloc() is, so it is counted and can be refused like one. */
void *khal_realloc_psram(void *p, size_t size)
{
    if ( NULL == p )
    {
        if ( 0U < s_psram_refusals )
        {
            --s_psram_refusals;
            return NULL;
        }
        psram_hdr_t *const h = malloc(sizeof(psram_hdr_t) + size);
        if ( NULL == h )
        {
            return NULL;
        }
        h->size = size;
        s_psram_held += size;
        ++s_psram_allocs;
        return &h[1];
    }
    psram_hdr_t *const old = (psram_hdr_t *)p - 1;
    size_t const was = old->size;
    psram_hdr_t *const h = realloc(old, sizeof(psram_hdr_t) + size);
    if ( NULL == h )
    {
        return NULL;
    }
    h->size = size;
    psram_unhold(was);
    s_psram_held += size;
    return &h[1];
}

void khal_free(void *p)
{
    if ( NULL != p )
    {
        if ( atomic_load(&s_slot_watched) == p )
        {
            atomic_fetch_add(&s_slot_watch_frees, 1U);
        }
        psram_hdr_t *const h = (psram_hdr_t *)p - 1;

        ++s_psram_frees;
        psram_unhold(h->size);
        free(h);
    }
}

/* kdrv_apps.c calls this on the paths that verify a written image, which
 * test_appmgr_ao.c's CRC case reaches: bootloader_support has no host build
 * to take the symbol from, and this stub is what stands in for it. Always
 * ESP_OK, so a case that wants a REJECTED image has to make the CRC
 * disagree rather than the image header. */
void fake_image_verify_set_result(int r)
{
    s_image_verify_result = (esp_err_t)r;
}

esp_err_t esp_image_verify(esp_image_load_mode_t mode,
                           const esp_partition_pos_t *part_pos,
                           esp_image_metadata_t *data)
{
    (void)mode;
    (void)part_pos;
    (void)data;
    return s_image_verify_result;
}
