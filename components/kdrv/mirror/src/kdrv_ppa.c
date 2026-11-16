/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ppa.c
 * @brief The mirror's scaled copy of the glass: one buffer, one pass through kdrv_compose.
 */
#include "kdrv_ppa.h"

#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"

#include "kbsp.h"
#include "kdrv_compose.h"
#include "kode_log.h"

#define TAG "kdrv.ppa"

/* The larger of this chip's two cache line sizes - both must agree the
 * buffer's SIZE is a whole number of lines, not just its address, and
 * aligning to the larger of the two aligns to the smaller as well (each is
 * a power of two and one divides the other on every configuration this
 * board ships). Named symbols, not restated numbers, so a change to either
 * Kconfig value is picked up rather than silently going stale. */
#define PPA_CACHE_LINE  ((CONFIG_CACHE_L1_CACHE_LINE_SIZE \
                          > CONFIG_CACHE_L2_CACHE_LINE_SIZE) \
                         ? (size_t)CONFIG_CACHE_L1_CACHE_LINE_SIZE \
                         : (size_t)CONFIG_CACHE_L2_CACHE_LINE_SIZE)

/* A full panel of RGB565: the largest thing this is ever asked for, because
 * scale 1 runs at full size. */
#define PPA_PANEL_BYTES ((size_t)KBSP_LCD_H_RES * (size_t)KBSP_LCD_V_RES * 2U)

/* Rounded up to a whole number of cache lines - written as arithmetic, not
 * as the literal it works out to, on purpose. MALLOC_CAP_CACHE_ALIGNED
 * below only aligns this buffer's ADDRESS; the PPA driver separately
 * requires its SIZE to be a multiple of the line too ("out.buffer addr or
 * out.buffer_size not aligned to cache line size"), and 410*502*2 =
 * 411640 is not a multiple of 64. That exact literal shipped once already
 * and silently refused every pass on real hardware - a caller "correcting"
 * this back to the bare panel size would reintroduce the same fault. */
#define PPA_OUT_CAP  (((PPA_PANEL_BYTES + (PPA_CACHE_LINE - 1U)) \
                      / PPA_CACHE_LINE) * PPA_CACHE_LINE)

static bool                s_ppa_ready;  /**< The accelerator and the buffer are there. */
static uint8_t            *s_out;
/* A try-lock: khal_mutex_t only blocks, and AO_Display must never wait out another pass. */
static SemaphoreHandle_t   s_busy;   /**< Try-lock.                       */

kode_status_t kdrv_ppa_init(void)
{
    if ( s_ppa_ready )
    {
        return KODE_OK;
    }

    /* Guarded on s_busy rather than created unconditionally, and created
     * unlocked - same reasoning as kdrv_jpeg_init(): a caller retrying after
     * a failed allocate below must not leak a second mutex, and the first
     * kdrv_ppa_prepare()'s xSemaphoreTake() has to succeed without anyone
     * having given it first. */
    if ( NULL == s_busy )
    {
        s_busy = xSemaphoreCreateMutex();
        if ( NULL == s_busy )
        {
            KLOG_E(TAG, "no memory for the prepare guard");
            return KODE_NO_RESOURCES;
        }
    }

    if ( kdrv_compose_init() != KODE_OK )
    {
        return KODE_NO_RESOURCES;
    }

    if ( NULL == s_out )
    {
        s_out = (uint8_t *)heap_caps_malloc(
            PPA_OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    }
    if ( NULL == s_out )
    {
        KLOG_E(TAG, "no room for a %u KB scaled buffer",
               (unsigned)(PPA_OUT_CAP / 1024U));
        return KODE_NO_RESOURCES;
    }
    s_ppa_ready = true;
    return KODE_OK;
}

kode_status_t kdrv_ppa_prepare(const uint8_t *src, uint32_t w, uint32_t h,
                               uint32_t scale, uint8_t **dst,
                               uint32_t *dst_w, uint32_t *dst_h)
{
    if ( !s_ppa_ready )
    {
        return KODE_DENIED;
    }
    if ( (NULL == src) || (NULL == dst) || (NULL == dst_w) || (NULL == dst_h)
        || (0U == w) || (0U == h) || (KDRV_COMPOSE_SIDE_MAX < w) || (KDRV_COMPOSE_SIDE_MAX < h)
        || ((1U != scale) && (2U != scale)) )
    {
        return KODE_DENIED;
    }
    if ( (2U == scale) && ((2U > w) || (2U > h)) )
    {
        return KODE_DENIED;
    }

    uint32_t const ow = w / scale;
    uint32_t const oh = h / scale;
    if ( ((size_t)ow * oh * 2U) > PPA_OUT_CAP )
    {
        return KODE_DENIED;
    }

    /* Zero timeout: a refusal, not a wait. One output buffer
     * means a second caller in here at the same time would read s_out as the
     * first is still writing it. */
    if ( xSemaphoreTake(s_busy, 0U) != pdTRUE )
    {
        KLOG_W(TAG, "a pass is already running; refusing the second caller");
        return KODE_DENIED;
    }

    /* cppcheck-suppress misra-c2012-11.8 ; the pass only reads its input picture */
    kdrv_compose_pic_t const  in   = { .px = (uint8_t *)src,
                                       .bytes = w * h * KDRV_COMPOSE_PX_BYTES,
                                       .w = (uint16_t)w, .h = (uint16_t)h };
    kdrv_compose_pic_t const  out  = { .px = s_out, .bytes = (uint32_t)PPA_OUT_CAP,
                                       .w = (uint16_t)ow, .h = (uint16_t)oh };
    kdrv_compose_rect_t const from = { .x = 0U, .y = 0U, .w = (uint16_t)w, .h = (uint16_t)h };
    uint16_t const            s16  = (uint16_t)(KDRV_COMPOSE_SCALE_ONE / scale);

    kode_status_t const err = kdrv_compose_scale(&out, 0U, 0U, &in, &from, s16, s16, 0U);

    /* Given back before *dst is even set, the same shape kdrv_jpeg.c's
     * kdrv_jpeg_encode() used to have - and that shape is a real bug there,
     * because kdrv_jpeg_selftest() reads its borrowed buffer from a
     * DIFFERENT Active Object (AO_Autotest, inline via TEST_SYNC) than the
     * one still holding it, so AO_Display can run between the give and
     * the read. There is no analogous second reader here: this driver's
     * only caller today is mirror_try_start() in display_ao.c, and it reads
     * *dst synchronously, in the same AO step that made this call, with no
     * event-loop return in between - the run-to-completion guarantee QP
     * gives one Active Object against itself is what keeps this safe
     * without copying the guard's shape. Add a locked-encode split like
     * kdrv_jpeg.c's the day a second caller (a `test component=ppa`
     * selftest, say) shows up on another AO. */
    (void)xSemaphoreGive(s_busy);

    if ( KODE_OK != err )
    {
        KLOG_W(TAG, "the PPA refused a %ux%u pass at scale %u",
               (unsigned)w, (unsigned)h, (unsigned)scale);
        return KODE_ERROR;
    }

    *dst   = s_out;
    *dst_w = ow;
    *dst_h = oh;
    return KODE_OK;
}
