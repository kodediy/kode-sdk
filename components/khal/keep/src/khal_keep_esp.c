/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_keep_esp.c
 * @brief khal_keep.h: the top of the PSRAM, held out of the heap.
 */
#include "khal_keep.h"

#include <stdbool.h>
#include <stdint.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#include "kode_log.h"

#include "khal_keep_fit.h"

#define TAG "khal.keep"

/** @brief A free block of the heap, as the walk saw it. */
typedef struct
{
    uintptr_t from;    /**< Its first byte. */
    size_t    bytes;   /**< Its exact size; 0 when none was found. */
} keep_free_t;

static uint8_t    *s_keep;
static keep_free_t s_last;   /**< The free block that ends highest: the heap walk writes it. */

/**
 * @brief An address, for judging where a block sits.
 * @param p The pointer.
 * @return Its address.
 */
static uintptr_t keep_addr(void const *p)
{
    /* cppcheck-suppress misra-c2012-11.6 ; the block's place is judged as an address */
    return (uintptr_t)p;
}

/**
 * @brief The heap walk's callback: keep the free block whose end is highest.
 *
 * Runs with the heap locked: it compares and stores, nothing else.
 *
 * @param heap_info  Unused.
 * @param block_info The block.
 * @param user_data  Unused.
 * @return true: the walk goes on.
 */
static bool keep_walk_top(walker_heap_into_t heap_info, walker_block_info_t block_info, void *user_data)
{
    uintptr_t const from = keep_addr(block_info.ptr);

    (void)heap_info;
    (void)user_data;
    if ( (!block_info.used) && ((s_last.from + s_last.bytes) < (from + block_info.size)) )
    {
        s_last.from  = from;
        s_last.bytes = block_info.size;
    }
    return true;
}

/**
 * @brief Walk the heap for the free block that ends highest, into s_last.
 *
 * The walk gives the block's exact size; the heap's largest-block answer is rounded down to its class.
 *
 * @param caps The heap's capabilities.
 */
static void keep_walk(uint32_t caps)
{
    s_last.from  = 0U;
    s_last.bytes = 0U;
    heap_caps_walk(caps, keep_walk_top, NULL);
}

void khal_keep_init(void)
{
    /* cppcheck-suppress misra-c2012-10.1 ; MALLOC_CAP_SPIRAM is ESP-IDF's own shifted constant */
    /* cppcheck-suppress misra-c2012-12.2 ; the same constant: a shift of a signed 1 by 10 */
    uint32_t const caps = MALLOC_CAP_SPIRAM;
    void          *fills[KHAL_KEEP_FILLS_MAX] = { NULL };
    size_t         count = 0U;
    void          *held  = NULL;

    if ( (NULL != s_keep) || !esp_psram_is_initialized() )
    {
        return;
    }
    uintptr_t const top = (uintptr_t)SOC_EXTRAM_LOW + esp_psram_get_size();

    keep_walk(caps);
    if ( (KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MIN) > s_last.bytes )
    {
        KLOG_W(TAG, "no room to set the kept block aside");
        return;
    }
    /* The heap cuts a request from the front of a free block and takes it at the request's class:
     * fills it takes exactly eat the top block from below until the held piece and its slack are left. */
    size_t fill = khal_keep_fill(s_last.bytes);

    while ( (0U != fill) && (KHAL_KEEP_FILLS_MAX > count) )
    {
        fills[count] = heap_caps_malloc(fill, caps);
        if ( NULL == fills[count] )
        {
            fill = 0U;
        }
        else
        {
            ++count;
            keep_walk(caps);
            fill = khal_keep_fill(s_last.bytes);
        }
    }
    if ( khal_keep_ready(s_last.bytes) )
    {
        /* Heap poisoning or heap task tracking adds bytes to every request: then this misses the top
         * and the warning below says so, never a wrong block. */
        held = heap_caps_malloc(KHAL_KEEP_HOLD_BYTES, caps);
    }
    for ( size_t i = 0U; i < count; ++i )
    {
        heap_caps_free(fills[i]);
    }
    uintptr_t const from = keep_addr(held);

    if ( (NULL == held) || !khal_keep_covers(top, from, KHAL_KEEP_HOLD_BYTES) )
    {
        /* Not where it has to be: no copy this boot, and nothing else lost. */
        KLOG_W(TAG, "the kept block did not land at the top (0x%08lx, top block %lu B)",
               (unsigned long)from, (unsigned long)s_last.bytes);
        if ( NULL != held )
        {
            heap_caps_free(held);
        }
        return;
    }
    /* cppcheck-suppress misra-c2012-11.4 ; the block sits at an address the PSRAM's size decides */
    s_keep = (uint8_t *)khal_keep_target(top);
}

void khal_keep_forget_warm(void)
{
    /* cppcheck-suppress misra-c2012-11.4 ; REG_WRITE is ESP-IDF's register access */
    REG_WRITE(LP_SYSTEM_REG_LP_STORE13_REG, 0U);
}

void *khal_keep(void)
{
    return s_keep;
}

void khal_keep_sync(size_t off, size_t len)
{
    if ( (NULL == s_keep) || (0U == len) || (KHAL_KEEP_BYTES <= off) )
    {
        return;
    }
    size_t const start = off & ~(size_t)KHAL_KEEP_LINE;
    size_t       end   = (off + len + KHAL_KEEP_LINE) & ~(size_t)KHAL_KEEP_LINE;

    if ( KHAL_KEEP_BYTES < end )
    {
        end = KHAL_KEEP_BYTES;
    }
    (void)esp_cache_msync(&s_keep[start], end - start,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}
