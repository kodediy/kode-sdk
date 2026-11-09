/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_mem.c
 * @brief LVGL's heap, pinned to PSRAM.
 */

#include "lvgl.h"

#if LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM

#include "khal_system.h"

/**
 * @brief Alignment every block from here carries.
 *
 * Eight bytes: enough for any scalar LVGL puts at the head of a block, and
 * above LV_DRAW_BUF_ALIGN (4) so a buffer that ends up here satisfies the
 * draw path too. Larger would round up hundreds of tiny allocations and
 * waste more than it aligns.
 */
#define KUI_MEM_ALIGN  8U

/**
 * @brief Nothing to prepare: the heap is the system's, not a pool of ours.
 */
void lv_mem_init(void)
{
    /* Intentionally empty. */
}

/**
 * @brief Nothing to tear down, for the same reason.
 */
void lv_mem_deinit(void)
{
    /* Intentionally empty. */
}

/**
 * @brief Allocate @p size bytes of PSRAM for LVGL.
 *
 * PSRAM or NULL, never internal RAM: DMA buffers and task stacks need all of it.
 *
 * @param size Bytes wanted.
 * @return The block, aligned to KUI_MEM_ALIGN and zeroed, or NULL. Zeroed
 *         because khal_alloc_psram() has calloc semantics; LVGL never
 *         assumes it, and the cost is a memset on blocks that are tens of
 *         bytes.
 */
void *lv_malloc_core(size_t size)
{
    return khal_alloc_psram((size_t)KUI_MEM_ALIGN, size);
}

/**
 * @brief Resize an LVGL block, keeping it in PSRAM.
 *
 * @param p Block from lv_malloc_core(), or NULL.
 * @param new_size Bytes wanted now.
 * @return The block, or NULL - in which case @p p is still valid and still
 *         the caller's to free.
 */
void *lv_realloc_core(void *p, size_t new_size)
{
    return khal_realloc_psram(p, new_size);
}

/**
 * @brief Release a block taken by lv_malloc_core() or lv_realloc_core().
 *
 * @param p The block, or NULL.
 */
void lv_free_core(void *p)
{
    khal_free(p);
}

/**
 * @brief Report what LVGL can still take.
 *
 * The heap is shared, so most of this structure has no honest answer: LVGL's
 * blocks are not distinguishable from anyone else's once they are in it.
 * Free bytes is the one figure that is true, and it is the one the monitor
 * is ever read for.
 *
 * @param mon_p Filled with zeroes except for the free size.
 */
void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    if ( NULL != mon_p )
    {
        (void)lv_memzero(mon_p, sizeof(*mon_p));
        mon_p->free_size = khal_heap_free();
    }
}

/**
 * @brief Integrity check, which a shared heap does not offer.
 *
 * @return LV_RESULT_OK always: the system heap has its own checker and this
 *         hook has no view into it.
 */
lv_result_t lv_mem_test_core(void)
{
    return LV_RESULT_OK;
}

#endif /* LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM */
