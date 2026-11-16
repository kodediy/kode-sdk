/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kscreen_compose.c
 * @brief Pictures composed by the pixel accelerator, and the object that shows one.
 */
#include "kscreen.h"

#include <stddef.h>
#include <string.h>

#include "kaos.h"
#include "kbsp_board.h"
#include "kdrv_compose.h"
#include "khal_system.h"
#include "kode_log.h"

#include "kscreen_geom.h"

#define TAG "kscreen"

/** @brief How long a pass may take to be answered: the flush's own bound. */
#define KSCREEN_COMPOSE_TIMEOUT_MS 500U

/** @brief Black, as a fill takes it (0xAARRGGBB): what a dimmed picture fades to. */
#define KSCREEN_BLACK_FILL        0xFF000000U

_Static_assert((KSCREEN_SCALE_ONE == KDRV_COMPOSE_SCALE_ONE) && (KSCREEN_SCALE_MIN == KDRV_COMPOSE_SCALE_MIN)
               && (KSCREEN_SCALE_MAX == KDRV_COMPOSE_SCALE_MAX),
               "a scale is counted as the accelerator counts it");

/**
 * @brief Hand one pass to the accelerator's owner and wait for it.
 *
 * The one place that knows who runs a pass: AO_Display, by request.
 *
 * @param job The pass.
 * @return The pass's answer.
 */
static kode_status_t compose_pass(display_compose_t const *job)
{
    /* The renderer's place in AO_Display's answers. One renderer per image,
     * and only its thread composes, so it needs no lock. */
    static display_compose_ask_t ask;
    kode_status_t const rc = display_compose_run(&ask, job, KSCREEN_COMPOSE_TIMEOUT_MS, NULL);

    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "compose pass %u was not done (status %d)", (unsigned)job->op, (int)rc);
    }
    return rc;
}

/**
 * @brief A picture as a pass names it.
 * @param pic The picture; may be NULL.
 * @param out Filled in.
 * @return true for a picture kscreen_pic_create() made, its rows still packed.
 */
static bool pic_view(const kscreen_pic_t *pic, display_pic_t *out)
{
    bool ok = false;

    /* A draw buffer reshaped since it was made may pad its rows; the
     * accelerator reads them with nothing between. */
    if ( (NULL != pic) && (NULL != pic->buf.data)
        && ((uint32_t)LV_COLOR_FORMAT_RGB565 == (uint32_t)pic->buf.header.cf)
        && kscreen_geom_packed((uint16_t)pic->buf.header.w, (uint32_t)pic->buf.header.stride) )
    {
        out->px    = pic->buf.data;
        out->bytes = pic->buf.data_size;
        out->w     = (uint16_t)pic->buf.header.w;
        out->h     = (uint16_t)pic->buf.header.h;
        ok = true;
    }
    return ok;
}

/**
 * @brief An area of a picture as a pass's block: first column and row, columns, rows.
 * @param pic  The picture's view.
 * @param area The area, inclusive as LVGL's are; NULL is the whole picture.
 * @param job  Receives the block in @c x, @c y, @c w and @c h.
 * @return true when the area is not empty and lies inside the picture.
 */
static bool area_block(display_pic_t const *pic, const lv_area_t *area, display_compose_t *job)
{
    kscreen_geom_area_t        in  = { 0, 0, 0, 0 };
    const kscreen_geom_area_t *ask = NULL;
    kscreen_geom_block_t       blk = { 0U, 0U, 0U, 0U };
    bool                       ok;

    if ( NULL != area )
    {
        in.x1 = area->x1;
        in.y1 = area->y1;
        in.x2 = area->x2;
        in.y2 = area->y2;
        ask   = &in;
    }
    ok = kscreen_geom_block(pic->w, pic->h, ask, &blk);
    if ( ok )
    {
        job->x = blk.x;
        job->y = blk.y;
        job->w = blk.w;
        job->h = blk.h;
    }
    return ok;
}

/**
 * @brief A point of a picture as a pass's landing point.
 * @param pic The picture's view.
 * @param x   Its column.
 * @param y   Its row.
 * @param job Receives @c dst_x and @c dst_y.
 * @return true when the point is inside the picture.
 */
static bool point_inside(display_pic_t const *pic, int32_t x, int32_t y, display_compose_t *job)
{
    bool const ok = kscreen_geom_point(pic->w, pic->h, x, y);

    if ( ok )
    {
        job->dst_x = (uint16_t)x;
        job->dst_y = (uint16_t)y;
    }
    return ok;
}

/**
 * @brief Fill in a pass that lands an area of one picture on a point of another.
 * @param job  Receives both pictures, the block and the landing point.
 * @param dst  The picture written.
 * @param x    Column of @p dst the area lands on.
 * @param y    Row of @p dst the area lands on.
 * @param src  The picture read.
 * @param from The area of @p src; NULL is all of it.
 * @return true when both are pictures, the area is in @p src and the point in @p dst.
 */
static bool block_onto(display_compose_t *job, const kscreen_pic_t *dst, int32_t x, int32_t y,
                       const kscreen_pic_t *src, const lv_area_t *from)
{
    bool ok = pic_view(dst, &job->dst);

    if ( ok )
    {
        ok = pic_view(src, &job->src);
    }
    if ( ok )
    {
        ok = area_block(&job->src, from, job);
    }
    if ( ok )
    {
        ok = point_inside(&job->dst, x, y, job);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; the API an app composes with; no app is in this build */
kscreen_pic_t *kscreen_pic_create(uint16_t w, uint16_t h)
{
    uint32_t const row   = (uint32_t)w * KDRV_COMPOSE_PX_BYTES;
    uint32_t const bytes = kscreen_geom_bytes(w, h);
    kscreen_pic_t *pic   = NULL;

    if ( 0U == bytes )
    {
        KLOG_E(TAG, "no picture of %u by %u: a side is 1 to %u pixels", (unsigned)w, (unsigned)h,
               (unsigned)KDRV_COMPOSE_SIDE_MAX);
    }
    else if ( lv_draw_buf_width_to_stride((uint32_t)w, LV_COLOR_FORMAT_RGB565) != row )
    {
        /* The accelerator reads rows with nothing between them. */
        KLOG_E(TAG, "LVGL pads RGB565 rows in this build; no picture");
    }
    else
    {
        void * const px   = khal_alloc_psram(KDRV_COMPOSE_ALIGN, (size_t)bytes);
        bool         made = false;

        /* cppcheck-suppress misra-c2012-11.5 ; khal hands memory back as void * */
        pic = (kscreen_pic_t *)khal_alloc_psram(sizeof(void *), sizeof(*pic));
        if ( (NULL != pic) && (NULL != px) )
        {
            made = (lv_draw_buf_init(&pic->buf, (uint32_t)w, (uint32_t)h, LV_COLOR_FORMAT_RGB565,
                                     row, px, bytes) == LV_RESULT_OK);
        }
        if ( !made )
        {
            KLOG_E(TAG, "no PSRAM for a picture of %u by %u (%u bytes)", (unsigned)w,
                   (unsigned)h, (unsigned)bytes);
            khal_free(px);    /* takes NULL */
            khal_free(pic);
            pic = NULL;
        }
    }
    return pic;
}

/* cppcheck-suppress misra-c2012-8.7 ; the API an app composes with; no app is in this build */
void kscreen_pic_delete(kscreen_pic_t *pic)
{
    if ( NULL != pic )
    {
        khal_free(pic->buf.data);
        khal_free(pic);
    }
}

/* cppcheck-suppress [misra-c2012-8.7,constParameterPointer] ; an app's call, none in this build; its pixels are written */
kode_status_t kscreen_pic_fill(kscreen_pic_t *dst, const lv_area_t *area, lv_color_t colour)
{
    display_compose_t job;
    kode_status_t     rc = KODE_DENIED;
    bool              ok = false;

    (void)memset(&job, 0, sizeof(job));
    job.op     = DISPLAY_COMPOSE_FILL;
    job.colour = lv_color_to_u32(colour);
    if ( pic_view(dst, &job.dst) )
    {
        ok = area_block(&job.dst, area, &job);
    }
    if ( ok )
    {
        rc = compose_pass(&job);
    }
    return rc;
}

/* cppcheck-suppress [misra-c2012-8.7,constParameterPointer] ; an app's call, none in this build; its pixels are written */
kode_status_t kscreen_pic_scale(kscreen_pic_t *dst, int32_t x, int32_t y, const kscreen_pic_t *src,
                                const lv_area_t *from, uint16_t sixteenths)
{
    display_compose_t job;
    kode_status_t     rc = KODE_DENIED;

    (void)memset(&job, 0, sizeof(job));
    job.op      = DISPLAY_COMPOSE_SCALE;
    job.scale_x = sixteenths;
    job.scale_y = sixteenths;
    if ( block_onto(&job, dst, x, y, src, from) )
    {
        rc = compose_pass(&job);
    }
    return rc;
}

/* cppcheck-suppress [misra-c2012-8.7,constParameterPointer] ; an app's call, none in this build; its pixels are written */
kode_status_t kscreen_pic_blend(kscreen_pic_t *dst, int32_t x, int32_t y, const kscreen_pic_t *fg,
                                const lv_area_t *from, lv_opa_t opa)
{
    display_compose_t job;
    kode_status_t     rc = KODE_DENIED;

    (void)memset(&job, 0, sizeof(job));
    job.op  = DISPLAY_COMPOSE_BLEND;
    job.opa = (uint8_t)opa;
    if ( block_onto(&job, dst, x, y, fg, from) )
    {
        rc = compose_pass(&job);
    }
    return rc;
}

/* cppcheck-suppress [misra-c2012-8.7,constParameterPointer] ; an app's call, none in this build; its pixels are written */
kode_status_t kscreen_pic_dim(kscreen_pic_t *dst, const kscreen_pic_t *src, lv_opa_t level)
{
    display_compose_t job;
    kode_status_t     rc = KODE_DENIED;

    /* The accelerator does not multiply: black first, then the source over
     * it at the level asked. */
    (void)memset(&job, 0, sizeof(job));
    job.op     = DISPLAY_COMPOSE_FILL;
    job.colour = KSCREEN_BLACK_FILL;
    /* Judged before anything is painted: a picture is never dimmed onto
     * itself, and the black must not reach it first. */
    if ( block_onto(&job, dst, 0, 0, src, NULL)
        && kscreen_geom_pair(job.dst.px, job.src.px, job.dst.w, job.dst.h, job.src.w, job.src.h) )
    {
        rc = compose_pass(&job);
        if ( KODE_OK == rc )
        {
            job.op  = DISPLAY_COMPOSE_BLEND;
            job.opa = (uint8_t)level;
            rc = compose_pass(&job);
        }
    }
    return rc;
}

/* cppcheck-suppress [misra-c2012-8.7,constParameterPointer] ; an app's call, none in this build; its pixels are written */
kode_status_t kscreen_pic_grab(const kscreen_t *s, kscreen_pic_t *dst)
{
    display_compose_t job;
    kode_status_t     rc = KODE_DENIED;

    (void)memset(&job, 0, sizeof(job));
    job.op      = DISPLAY_COMPOSE_SCALE;
    job.glass   = true;
    job.w       = (uint16_t)KBSP_LCD_H_RES;
    job.h       = (uint16_t)KBSP_LCD_V_RES;
    job.scale_x = (uint16_t)KSCREEN_SCALE_ONE;
    job.scale_y = (uint16_t)KSCREEN_SCALE_ONE;
    if ( (NULL != s) && (NULL != s->disp) && pic_view(dst, &job.dst) )
    {
        uint8_t const rot = (uint8_t)s->rot;

        /* The panel's memory holds the frame as the panel is wired; the
         * same pass turns it to what the glass shows. */
        if ( kscreen_geom_grab_fits(rot, job.dst.w, job.dst.h) )
        {
            job.turns = kscreen_geom_grab_turns(rot);
            rc = compose_pass(&job);
        }
    }
    return rc;
}

/* cppcheck-suppress misra-c2012-8.7 ; the API an app composes with; no app is in this build */
lv_obj_t *kscreen_frame_create(lv_obj_t *parent, const kscreen_pic_t *pic)
{
    lv_obj_t *frame = NULL;

    if ( (NULL != parent) && (NULL != pic) )
    {
        frame = lv_image_create(parent);
    }
    if ( NULL != frame )
    {
        /* The picture is its own image, used where it lies: LVGL copies its
         * rows as it draws and keeps no copy of its own. */
        lv_image_set_src(frame, &pic->buf);
    }
    return frame;
}
