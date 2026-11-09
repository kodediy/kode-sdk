/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_keep_esp.c
 * @brief khal_keep.h: the top of the PSRAM, held out of the heap.
 */
#include "khal_keep.h"

#include <stdint.h>

#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"

#include "kode_log.h"

#define TAG "khal.keep"

#define KEEP_END_GAP  (16U * 1024U) /**< Under the top of the PSRAM: the heap stops about 4 KB short of it. */
#define KEEP_MARGIN   (64U * 1024U) /**< What the held piece has over the block. */
#define KEEP_LINE     63U           /**< A cache line, less one. */

static uint8_t *s_keep;

void khal_keep_init(void)
{
    /* cppcheck-suppress misra-c2012-10.1 ; MALLOC_CAP_SPIRAM is ESP-IDF's own shifted constant */
    /* cppcheck-suppress misra-c2012-12.2 ; the same constant: a shift of a signed 1 by 10 */
    uint32_t const caps = MALLOC_CAP_SPIRAM;

    if ( (NULL != s_keep) || !esp_psram_is_initialized() )
    {
        return;
    }
    uintptr_t const top    = (uintptr_t)SOC_EXTRAM_LOW + esp_psram_get_size();
    uintptr_t const target = (top - KHAL_KEEP_BYTES - KEEP_END_GAP) & ~(uintptr_t)KEEP_LINE;
    size_t const    want   = (size_t)KHAL_KEEP_BYTES + KEEP_MARGIN;
    size_t const    large  = heap_caps_get_largest_free_block(caps);

    if ( large <= want )
    {
        KLOG_W(TAG, "no room to set the kept block aside");
        return;
    }
    /* TLSF serves the front of a free block: fill all but the top, take the rest, free the fill. */
    void *const below = heap_caps_malloc(large - want, caps);
    size_t const rest = heap_caps_get_largest_free_block(caps);
    void *const held  = heap_caps_malloc(rest, caps);

    if ( NULL != below )
    {
        heap_caps_free(below);
    }
    /* cppcheck-suppress misra-c2012-11.6 ; the block's place is judged as an address */
    uintptr_t const from = (uintptr_t)held;

    if ( (NULL == held) || (from > target)
        || ((from + rest) < (target + KHAL_KEEP_BYTES)) )
    {
        /* Not where it has to be: no copy this boot, and nothing else lost. */
        KLOG_W(TAG, "the kept block did not land at the top (0x%08lx, %lu B)",
               (unsigned long)from, (unsigned long)rest);
        if ( NULL != held )
        {
            heap_caps_free(held);
        }
        return;
    }
    /* cppcheck-suppress misra-c2012-11.4 ; the block sits at an address the PSRAM's size decides */
    s_keep = (uint8_t *)target;
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
    size_t const start = off & ~(size_t)KEEP_LINE;
    size_t       end   = (off + len + KEEP_LINE) & ~(size_t)KEEP_LINE;

    if ( KHAL_KEEP_BYTES < end )
    {
        end = KHAL_KEEP_BYTES;
    }
    (void)esp_cache_msync(&s_keep[start], end - start,
                          ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
}
