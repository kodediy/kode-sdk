/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_compose.c
 * @brief The P4's pixel accelerator: one blocking hardware pass per call.
 */
#include "kdrv_compose.h"

#include <stddef.h>
#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"

#include "sdkconfig.h"

#include "kdrv_display.h"
#include "kode_log.h"

#include "kdrv_compose_check.h"

#define TAG "kdrv.compose"

_Static_assert((KDRV_COMPOSE_ALIGN >= (uint32_t)CONFIG_CACHE_L1_CACHE_LINE_SIZE)
               && (KDRV_COMPOSE_ALIGN >= (uint32_t)CONFIG_CACHE_L2_CACHE_LINE_SIZE),
               "an output's alignment covers both cache line sizes");
_Static_assert((KDRV_COMPOSE_SIDE_MAX >= KBSP_LCD_H_RES) && (KDRV_COMPOSE_SIDE_MAX >= KBSP_LCD_V_RES),
               "a picture may be the glass, upright or turned");
_Static_assert((KDRV_COMPOSE_ALIGN == KDRV_DISPLAY_CACHE_LINE)
               && (KDRV_COMPOSE_PX_BYTES == KDRV_DISPLAY_BYTES_PER_PIXEL),
               "a framebuffer is a picture this door can write");

#define COMPOSE_CLIENTS      3U     /**< One client per kind of pass. */
#define COMPOSE_PENDING      1U     /**< Passes a client may hold: every pass here blocks. */
#define COMPOSE_SCALE_ONE_F  ((float)KDRV_COMPOSE_SCALE_ONE)  /**< The same size, as the driver's float. */

/** @brief The kinds of pass, in the order their clients are kept. */
enum {
    COMPOSE_SRM = 0,  /**< Scale. */
    COMPOSE_BLEND,    /**< Blend. */
    COMPOSE_FILL      /**< Fill. */
};

/**
 * @brief The client of one kind of pass, registered the first time it is asked for.
 * @param kind COMPOSE_SRM, COMPOSE_BLEND or COMPOSE_FILL.
 * @return The client, or NULL, logged, when the accelerator gave none.
 */
static ppa_client_handle_t client_get(size_t kind)
{
    static const ppa_operation_t KINDS[COMPOSE_CLIENTS] = {
        [COMPOSE_SRM]   = PPA_OPERATION_SRM,
        [COMPOSE_BLEND] = PPA_OPERATION_BLEND,
        [COMPOSE_FILL]  = PPA_OPERATION_FILL,
    };
    static ppa_client_handle_t s_client[COMPOSE_CLIENTS];   /* each NULL until its first pass */

    if ( NULL == s_client[kind] )
    {
        ppa_client_config_t cfg;

        (void)memset(&cfg, 0, sizeof(cfg));
        cfg.oper_type             = KINDS[kind];
        cfg.max_pending_trans_num = COMPOSE_PENDING;
        if ( ppa_register_client(&cfg, &s_client[kind]) != ESP_OK )
        {
            KLOG_E(TAG, "the PPA would not register client %u", (unsigned)kind);
            s_client[kind] = NULL;
        }
    }
    return s_client[kind];
}

kode_status_t kdrv_compose_init(void)
{
    kode_status_t rc = KODE_OK;

    if ( NULL == client_get(COMPOSE_SRM) )
    {
        rc = KODE_NO_RESOURCES;
    }
    return rc;
}

/**
 * @brief Write back what the CPU still holds of the rows a scale is about to write.
 *
 * For a scale the driver only drops the cache's copy of every whole row its
 * output block touches, and of the cache lines those rows end in. Pixels the
 * CPU drew beside the block and has not written back would go with it. A
 * fill and a blend are written back by the driver itself.
 *
 * @param dst  The picture written, already checked.
 * @param y    The block's first row.
 * @param rows How many rows it has.
 */
static void out_rows_write_back(const kdrv_compose_pic_t *dst, uint32_t y, uint32_t rows)
{
    uint32_t const row   = (uint32_t)dst->w * KDRV_COMPOSE_PX_BYTES;
    uint32_t const first = ((y * row) / KDRV_COMPOSE_ALIGN) * KDRV_COMPOSE_ALIGN;
    uint32_t       end   = ((((y + rows) * row) + (KDRV_COMPOSE_ALIGN - 1U))
                            / KDRV_COMPOSE_ALIGN) * KDRV_COMPOSE_ALIGN;

    if ( end > dst->bytes )
    {
        end = dst->bytes;
    }
    if ( esp_cache_msync(&dst->px[first], (size_t)end - (size_t)first,
                         ESP_CACHE_MSYNC_FLAG_DIR_C2M) != ESP_OK )
    {
        KLOG_W(TAG, "rows %u..%u were not written back before a pass", (unsigned)y,
               (unsigned)(y + rows));
    }
}

/**
 * @brief Describe @p dst as a pass's output, its block starting at @p x, @p y.
 * @param out Filled in.
 * @param dst The picture written.
 * @param x   The block's first column.
 * @param y   The block's first row.
 */
static void out_of(ppa_out_pic_blk_config_t *out, const kdrv_compose_pic_t *dst, uint32_t x,
                   uint32_t y)
{
    out->buffer         = dst->px;
    out->buffer_size    = dst->bytes;
    out->pic_w          = dst->w;
    out->pic_h          = dst->h;
    out->block_offset_x = x;
    out->block_offset_y = y;
}

/**
 * @brief Describe a block of @p src as a pass's input.
 * @param in  Filled in.
 * @param src The picture read.
 * @param x   The block's first column.
 * @param y   The block's first row.
 * @param w   Its columns.
 * @param h   Its rows.
 */
static void in_of(ppa_in_pic_blk_config_t *in, const kdrv_compose_pic_t *src, uint32_t x,
                  uint32_t y, uint32_t w, uint32_t h)
{
    in->buffer         = src->px;
    in->pic_w          = src->w;
    in->pic_h          = src->h;
    in->block_w        = w;
    in->block_h        = h;
    in->block_offset_x = x;
    in->block_offset_y = y;
}

/**
 * @brief Whether a pass may be handed to the accelerator, and to which client.
 * @param checked The arithmetic's verdict.
 * @param kind    The kind of pass.
 * @param client  Receives the kind's client when KODE_OK is returned.
 * @return @p checked when it refuses; KODE_NO_RESOURCES with no client; else KODE_OK.
 */
static kode_status_t pass_ready(kode_status_t checked, size_t kind, ppa_client_handle_t *client)
{
    kode_status_t rc = checked;

    if ( KODE_OK == rc )
    {
        *client = client_get(kind);
        if ( NULL == *client )
        {
            rc = KODE_NO_RESOURCES;
        }
    }
    return rc;
}

#if defined(CONFIG_KODE_COMPOSE)
kode_status_t kdrv_compose_fill(const kdrv_compose_pic_t *dst, const kdrv_compose_rect_t *at,
                                uint32_t argb8888)
{
    ppa_client_handle_t client = NULL;
    kode_status_t rc = pass_ready(kdrv_compose_check_fill(dst, at), COMPOSE_FILL, &client);

    if ( KODE_OK == rc )
    {
        ppa_fill_oper_config_t cfg;

        (void)memset(&cfg, 0, sizeof(cfg));
        out_of(&cfg.out, dst, at->x, at->y);
        cfg.out.fill_cm         = PPA_FILL_COLOR_MODE_RGB565;
        cfg.fill_block_w        = at->w;
        cfg.fill_block_h        = at->h;
        cfg.fill_argb_color.val = argb8888;
        cfg.mode                = PPA_TRANS_MODE_BLOCKING;

        /* The driver writes the block's rows back before it fills them. */
        if ( ppa_do_fill(client, &cfg) != ESP_OK )
        {
            KLOG_W(TAG, "the PPA refused a %ux%u fill", (unsigned)at->w, (unsigned)at->h);
            rc = KODE_ERROR;
        }
    }
    return rc;
}
#endif /* CONFIG_KODE_COMPOSE */

kode_status_t kdrv_compose_scale(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                 const kdrv_compose_pic_t *src, const kdrv_compose_rect_t *from,
                                 uint16_t sx, uint16_t sy, uint8_t turns)
{
    /* The driver's angles, by quarter turns counter-clockwise. */
    static const ppa_srm_rotation_angle_t ANGLES[KDRV_COMPOSE_TURNS] = {
        PPA_SRM_ROTATION_ANGLE_0, PPA_SRM_ROTATION_ANGLE_90,
        PPA_SRM_ROTATION_ANGLE_180, PPA_SRM_ROTATION_ANGLE_270,
    };
    ppa_client_handle_t client = NULL;
    kode_status_t rc = pass_ready(kdrv_compose_check_scale(dst, x, y, src, from, sx, sy, turns),
                                  COMPOSE_SRM, &client);

    if ( KODE_OK == rc )
    {
        ppa_srm_oper_config_t cfg;
        uint32_t              out_w = 0U;
        uint32_t              out_h = 0U;

        (void)memset(&cfg, 0, sizeof(cfg));
        in_of(&cfg.in, src, from->x, from->y, from->w, from->h);
        cfg.in.srm_cm      = PPA_SRM_COLOR_MODE_RGB565;
        out_of(&cfg.out, dst, x, y);
        cfg.out.srm_cm     = PPA_SRM_COLOR_MODE_RGB565;
        cfg.rotation_angle = ANGLES[turns];
        cfg.scale_x        = (float)sx / COMPOSE_SCALE_ONE_F;
        cfg.scale_y        = (float)sy / COMPOSE_SCALE_ONE_F;
        cfg.mode           = PPA_TRANS_MODE_BLOCKING;

        kdrv_compose_landed(from, sx, sy, turns, &out_w, &out_h);
        out_rows_write_back(dst, y, out_h);
        if ( ppa_do_scale_rotate_mirror(client, &cfg) != ESP_OK )
        {
            KLOG_W(TAG, "the PPA refused a %ux%u scale at %u/16 by %u/16, %u turn(s)",
                   (unsigned)from->w, (unsigned)from->h, (unsigned)sx, (unsigned)sy,
                   (unsigned)turns);
            rc = KODE_ERROR;
        }
    }
    return rc;
}

#if defined(CONFIG_KODE_COMPOSE)
kode_status_t kdrv_compose_blend(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                 const kdrv_compose_pic_t *fg, const kdrv_compose_rect_t *from,
                                 uint8_t opa)
{
    ppa_client_handle_t client = NULL;
    kode_status_t rc = pass_ready(kdrv_compose_check_blend(dst, x, y, fg, from), COMPOSE_BLEND,
                                  &client);

    if ( KODE_OK == rc )
    {
        ppa_blend_oper_config_t cfg;

        /* The background is the destination itself: the driver writes its
         * rows back before the pass, so nothing beside the block is lost. */
        (void)memset(&cfg, 0, sizeof(cfg));
        in_of(&cfg.in_bg, dst, x, y, from->w, from->h);
        cfg.in_bg.blend_cm       = PPA_BLEND_COLOR_MODE_RGB565;
        cfg.bg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        cfg.bg_alpha_fix_val     = KDRV_COMPOSE_OPA_MAX;
        in_of(&cfg.in_fg, fg, from->x, from->y, from->w, from->h);
        cfg.in_fg.blend_cm       = PPA_BLEND_COLOR_MODE_RGB565;
        cfg.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
        cfg.fg_alpha_fix_val     = opa;
        out_of(&cfg.out, dst, x, y);
        cfg.out.blend_cm         = PPA_BLEND_COLOR_MODE_RGB565;
        cfg.mode                 = PPA_TRANS_MODE_BLOCKING;

        if ( ppa_do_blend(client, &cfg) != ESP_OK )
        {
            KLOG_W(TAG, "the PPA refused a %ux%u blend", (unsigned)from->w, (unsigned)from->h);
            rc = KODE_ERROR;
        }
    }
    return rc;
}
#endif /* CONFIG_KODE_COMPOSE */
