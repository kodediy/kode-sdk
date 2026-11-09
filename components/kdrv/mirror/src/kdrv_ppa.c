/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ppa.c
 * @brief The P4's pixel accelerator: one hardware pass that scales.
 */
#include "kdrv_ppa.h"

#include <string.h>

#include "driver/ppa.h"
#include "esp_heap_caps.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "sdkconfig.h"

#include "kbsp.h"
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

static ppa_client_handle_t s_client;
static uint8_t            *s_out;
/* A try-lock: khal_mutex_t only blocks, and AO_Display must never wait out another pass. */
static SemaphoreHandle_t   s_busy;   /**< Try-lock.                       */

kode_status_t kdrv_ppa_init(void)
{
    if ( NULL != s_client )
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

    ppa_client_config_t const cfg = {
        .oper_type             = PPA_OPERATION_SRM,
        /* One, because every operation here is blocking: the caller is an AO
         * step that wants the pixels before it can encode them, so a queue
         * would only add latency to a wait that has to happen anyway. */
        .max_pending_trans_num = 1U,
    };
    if ( ppa_register_client(&cfg, &s_client) != ESP_OK )
    {
        KLOG_E(TAG, "the PPA would not register a client");
        s_client = NULL;
        return KODE_NO_RESOURCES;
    }

    s_out = (uint8_t *)heap_caps_malloc(
        PPA_OUT_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
    if ( NULL == s_out )
    {
        KLOG_E(TAG, "no room for a %u KB scaled buffer",
               (unsigned)(PPA_OUT_CAP / 1024U));
        (void)ppa_unregister_client(s_client);
        s_client = NULL;
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

kode_status_t kdrv_ppa_prepare(const uint8_t *src, uint32_t w, uint32_t h,
                               uint32_t scale, uint8_t **dst,
                               uint32_t *dst_w, uint32_t *dst_h)
{
    if ( (NULL == s_client) || (NULL == s_out) )
    {
        return KODE_DENIED;
    }
    if ( (NULL == src) || (NULL == dst) || (NULL == dst_w) || (NULL == dst_h)
        || (0U == w) || (0U == h) || ((1U != scale) && (2U != scale)) )
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

    /* Zero timeout: a refusal, not a wait. One client and one output buffer
     * mean a second caller in here at the same time would read s_out as the
     * first is still writing it. */
    if ( xSemaphoreTake(s_busy, 0U) != pdTRUE )
    {
        KLOG_W(TAG, "a pass is already running; refusing the second caller");
        return KODE_DENIED;
    }

    float const factor = (2U == scale) ? 0.5F : 1.0F;

    ppa_srm_oper_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.in.buffer         = src;
    cfg.in.pic_w          = w;
    cfg.in.pic_h          = h;
    cfg.in.block_w        = w;
    cfg.in.block_h        = h;
    cfg.in.block_offset_x = 0U;
    cfg.in.block_offset_y = 0U;
    cfg.in.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

    cfg.out.buffer         = s_out;
    cfg.out.buffer_size    = (uint32_t)PPA_OUT_CAP;
    cfg.out.pic_w          = ow;
    cfg.out.pic_h          = oh;
    cfg.out.block_offset_x = 0U;
    cfg.out.block_offset_y = 0U;
    cfg.out.srm_cm         = PPA_SRM_COLOR_MODE_RGB565;

    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x        = factor;
    cfg.scale_y        = factor;
    cfg.mirror_x        = false;
    cfg.mirror_y        = false;
    cfg.mode            = PPA_TRANS_MODE_BLOCKING;

    esp_err_t const err = ppa_do_scale_rotate_mirror(s_client, &cfg);

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

    if ( ESP_OK != err )
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
