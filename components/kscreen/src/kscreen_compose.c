/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kscreen_compose.c
 * @brief Pictures composed by the pixel accelerator, and the object that shows one.
 */
#include "kscreen.h"

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kbsp_board.h"
#include "kdrv_compose.h"
#include "khal_system.h"
#include "kode_log.h"
/* Private in 9.6, and only these two: lvgl_private.h brings a driver's TAG()
 * with it. The display's layers and an event's filter, then a cover check's
 * answer. */
#include "src/display/lv_display_private.h"
#include "src/core/lv_obj_event_private.h"

#include "kscreen_geom.h"
#include "kscreen_priv.h"

/* The frame's copy relies on LVGL 9.6's layer and task lists: read it again before moving LVGL. */
#if (LVGL_VERSION_MAJOR != 9) || (LVGL_VERSION_MINOR != 6)
#error "kscreen's frame object is written against LVGL 9.6's draw pipeline"
#endif

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
static kode_status_t compose_run(display_compose_t const *job)
{
    /* The renderer's place in AO_Display's answers. One renderer per image,
     * and only its thread composes - an app's passes and its frames' copies
     * alike - so it needs no lock. */
    static display_compose_ask_t ask;

    return display_compose_run(&ask, job, KSCREEN_COMPOSE_TIMEOUT_MS, NULL);
}

/**
 * @brief Run one of the app's passes, and say when it was not done.
 * @param job The pass.
 * @return The pass's answer.
 */
static kode_status_t compose_pass(display_compose_t const *job)
{
    kode_status_t const rc = compose_run(job);

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

/* ---- The frame object ------------------------------------------------------
 * An image object showing a picture. Two callbacks make it cheap: it says it
 * covers what is under it, so LVGL draws nothing there, and, where LVGL
 * would only copy its rows into the panel's framebuffer, the accelerator
 * copies them instead. The arithmetic of both is kscreen_geom's. */

/**
 * @brief An LVGL area as kscreen_geom takes it.
 * @param a The area.
 * @return The same corners.
 */
static kscreen_geom_area_t geom_area(const lv_area_t *a)
{
    kscreen_geom_area_t const g = { a->x1, a->y1, a->x2, a->y2 };

    return g;
}

#if defined(CONFIG_KODE_COMPOSE)
/* Declared here because frame_hooks() tells it from an app's own. */
static void frame_draw_cb(lv_event_t *e);
#endif

/**
 * @brief What the app's own callbacks on a frame do with its drawing.
 *
 * In LVGL 9.6 DRAW_MAIN reaches the object's own callbacks alone: it neither
 * bubbles to the parent nor trickles to the children, whatever the flags.
 *
 * @param obj   The frame.
 * @param taken Receives true when one of them takes its DRAW_MAIN (added for
 *              it or for LV_EVENT_ALL): a copy ends the event before it runs.
 * @return true when one of them can change how LVGL draws the picture: a
 *         PREPROCESS callback on DRAW_MAIN runs before LVGL draws it, and
 *         draw task events hand the app the picture's own task.
 */
static bool frame_hooks(lv_obj_t *obj, bool *taken)
{
    uint32_t const n       = lv_obj_get_event_count(obj);
    uint32_t const flags   = (uint32_t)LV_EVENT_PREPROCESS | (uint32_t)LV_EVENT_MARKED_DELETING;
    bool           changes = lv_obj_is_send_draw_task_events(obj);

    *taken = changes;
    for ( uint32_t i = 0U; i < n; i++ )
    {
        const lv_event_dsc_t * const d = lv_obj_get_event_dsc(obj, i);
#if defined(CONFIG_KODE_COMPOSE)
        bool const ours = (NULL != d) && (&frame_draw_cb == d->cb);
#else
        bool const ours = false;
#endif

        /* One being removed is skipped by LVGL already. */
        if ( (NULL != d) && !ours && (0U == (d->filter & (uint32_t)LV_EVENT_MARKED_DELETING)) )
        {
            uint32_t const code = d->filter & ~flags;

            if ( ((uint32_t)LV_EVENT_ALL == code) || ((uint32_t)LV_EVENT_DRAW_MAIN == code) )
            {
                *taken = true;
                if ( 0U != (d->filter & (uint32_t)LV_EVENT_PREPROCESS) )
                {
                    changes = true;
                }
            }
        }
    }
    return changes;
}

/**
 * @brief Whether a frame would be drawn as its picture is, pixel for pixel.
 * @param obj   The frame.
 * @param pic   Its picture.
 * @param layer The layer it is drawn in, whose opacity and recolour it takes;
 *              NULL takes them from its parents' styles instead.
 * @param taken Receives frame_hooks()'s answer: a callback of the app's takes
 *              its DRAW_MAIN. Asked only of a frame otherwise plain.
 * @return true for its own picture, whole, and nothing of the app's that
 *         changes the draw: no opacity, recolour, colour key, shadow, mask,
 *         transform, offset, blend or radius.
 */
static bool frame_plain(lv_obj_t *obj, const kscreen_pic_t *pic, lv_layer_t *layer, bool *taken)
{
    lv_draw_image_dsc_t dsc;
    display_pic_t       view;
    bool                plain = false;

    *taken = false;
    /* Filled as lv_image fills it before it draws. */
    lv_draw_image_dsc_init(&dsc);
    dsc.base.layer = layer;
    lv_obj_init_draw_image_dsc(obj, LV_PART_MAIN, &dsc);
    if ( (lv_image_get_src(obj) == (const void *)&pic->buf) && pic_view(pic, &view)
        && ((uint32_t)LV_OPA_COVER == (uint32_t)dsc.opa) && ((uint32_t)LV_OPA_TRANSP == (uint32_t)dsc.recolor_opa)
        && (NULL == dsc.colorkey) && ((uint32_t)LV_OPA_TRANSP == (uint32_t)dsc.base.drop_shadow_opa)
        && (NULL == lv_image_get_bitmap_map_src(obj))
        && (0 == lv_image_get_rotation(obj)) && (LV_SCALE_NONE == lv_image_get_scale_x(obj))
        && (LV_SCALE_NONE == lv_image_get_scale_y(obj)) && (0 == lv_image_get_offset_x(obj))
        && (0 == lv_image_get_offset_y(obj)) && (LV_BLEND_MODE_NORMAL == lv_image_get_blend_mode(obj))
        && (0 == lv_obj_get_style_radius(obj, LV_PART_MAIN)) )
    {
        plain = !frame_hooks(obj, taken);
    }
    return plain;
}

/**
 * @brief The frame's covering, for LV_EVENT_COVER_CHECK after LVGL's own.
 *
 * An image with no background never says it covers, so LVGL drew the whole
 * screen under the picture before the picture. In every mode, turned or not.
 *
 * @param e The event; its user data is the picture.
 */
static void frame_cover_cb(lv_event_t *e)
{
    lv_obj_t * const obj = lv_event_get_current_target_obj(e);
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands the user data back as void * */
    const kscreen_pic_t * const pic = (const kscreen_pic_t *)lv_event_get_user_data(e);
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands the event's parameter back as void * */
    lv_cover_check_info_t * const info = (lv_cover_check_info_t *)lv_event_get_param(e);

    /* A clipped corner says MASKED, which nothing may undo. */
    if ( (NULL != obj) && (NULL != pic) && (NULL != info) && (NULL != info->area)
        && (LV_COVER_RES_MASKED != info->res) )
    {
        kscreen_geom_frame_t f;
        lv_area_t            coords;
        bool                 taken;

        lv_obj_get_coords(obj, &coords);
        f.direct = false;
        f.idle   = false;
        f.plain  = frame_plain(obj, pic, NULL, &taken);
        f.hooked = taken;
        f.pic_w  = (uint16_t)pic->buf.header.w;
        f.pic_h  = (uint16_t)pic->buf.header.h;
        f.obj    = geom_area(&coords);
        f.clip   = geom_area(info->area);
        f.buf    = f.obj;
        if ( kscreen_geom_frame_covers(&f) )
        {
            /* lv_event_set_cover_res() only ever weakens the answer. */
            info->res = LV_COVER_RES_COVER;
        }
    }
}

#if defined(CONFIG_KODE_COMPOSE)

/**
 * @brief Whether a layer is the panel's framebuffer as LVGL draws it now,
 *        unturned and in the accelerator's format.
 * @param layer The layer.
 * @return true when it is.
 */
static bool layer_direct(lv_layer_t const *layer)
{
    bool direct = false;

#if !LV_DRAW_TRANSFORM_USE_MATRIX
    /* A matrix build may draw the buffer turned: never a copy. */
    lv_display_t * const        disp = layer->display;
    lv_draw_buf_t const * const buf  = layer->draw_buf;

    if ( (NULL != disp) && (NULL != buf) && (NULL != buf->data)
        && (LV_DISPLAY_RENDER_MODE_DIRECT == lv_display_get_render_mode(disp))
        && (LV_DISPLAY_ROTATION_0 == lv_display_get_rotation(disp))
        && (lv_display_get_buf_active(disp) == buf)
        && ((uint32_t)LV_COLOR_FORMAT_RGB565 == (uint32_t)buf->header.cf)
        && kscreen_geom_packed((uint16_t)buf->header.w, (uint32_t)buf->header.stride)
        && (lv_area_get_width(&layer->buf_area) == (int32_t)buf->header.w)
        && (lv_area_get_height(&layer->buf_area) == (int32_t)buf->header.h) )
    {
        direct = true;
    }
#else
    (void)layer;
#endif
    return direct;
}

/**
 * @brief Whether no draw task waits or runs in any layer of a display.
 *
 * LVGL draws a frame as tiles, one per draw unit, and a task stays on its
 * layer's list until it is done. A copy that ran while a task of the tile
 * before still drew could meet it in a cache line the two rows share.
 *
 * @param disp The display.
 * @return true when every list is empty.
 */
static bool layers_idle(lv_display_t const *disp)
{
    const lv_layer_t *l    = disp->layer_head;
    bool              idle = true;

    while ( (NULL != l) && idle )
    {
        idle = (NULL == l->draw_task_head);
        l    = l->next;
    }
    return idle;
}

/**
 * @brief The renderer a frame's display belongs to.
 * @param obj The frame.
 * @return The kscreen_t, or NULL for a display kscreen did not open.
 */
static kscreen_t *frame_screen(lv_obj_t const *obj)
{
    kscreen_t *s = NULL;

    if ( NULL != obj )
    {
        s = kscreen_of(lv_obj_get_display(obj));
    }
    return s;
}

/**
 * @brief A reason in words.
 * @param why The reason.
 * @return Words for the log.
 */
static const char *frame_why_text(kscreen_geom_frame_why_t why)
{
    const char *text;

    switch ( why )
    {
        case KSCREEN_GEOM_FRAME_COPY: {
            text = "copied by the accelerator";
            break;
        }
        case KSCREEN_GEOM_FRAME_LAYER: {
            text = "drawn by LVGL: not into the unturned framebuffer";
            break;
        }
        case KSCREEN_GEOM_FRAME_STYLED: {
            text = "drawn by LVGL: styled or transformed, or the app may change its draw";
            break;
        }
        case KSCREEN_GEOM_FRAME_HOOKED: {
            text = "drawn by LVGL: a callback of the app's takes its DRAW_MAIN";
            break;
        }
        case KSCREEN_GEOM_FRAME_SIZE: {
            text = "drawn by LVGL: not its picture's size";
            break;
        }
        case KSCREEN_GEOM_FRAME_PART: {
            text = "drawn by LVGL: an area past the object or the buffer";
            break;
        }
        default: {
            text = "drawn by LVGL: LVGL was still drawing";
            break;
        }
    }
    return text;
}

/**
 * @brief Say a lasting reason when it changes. LVGL still drawing changes
 *        from one draw to the next; the counters say how often.
 * @param why The reason.
 */
static void frame_tell(kscreen_geom_frame_why_t why)
{
    /* The reason said last, as a number; UINT32_MAX before the first. */
    static uint32_t frame_said = UINT32_MAX;

    if ( (KSCREEN_GEOM_FRAME_BUSY != why) && ((uint32_t)why != frame_said) )
    {
        frame_said = (uint32_t)why;
        KLOG_D(TAG, "frame: %s", frame_why_text(why));
    }
}

/**
 * @brief The frame's drawing, for LV_EVENT_DRAW_MAIN before LVGL's own.
 *
 * In direct mode LVGL would only copy the picture's rows into the
 * framebuffer. When nothing else is drawing it, the accelerator copies them
 * and the event stops there; otherwise, or when the pass is not done, LVGL
 * draws the frame as before.
 *
 * @param e The event; its user data is the picture.
 */
static void frame_draw_cb(lv_event_t *e)
{
    /* Off for the rest of the boot once a pass is not done. */
    static bool off = false;
    lv_obj_t * const   obj   = lv_event_get_current_target_obj(e);
    lv_layer_t * const layer = lv_event_get_layer(e);
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands the user data back as void * */
    const kscreen_pic_t * const pic = (const kscreen_pic_t *)lv_event_get_user_data(e);
    kode_status_t rc = KODE_DENIED;

    if ( (NULL != obj) && (NULL != layer) && (NULL != pic) && !off )
    {
        kscreen_geom_frame_t f;
        kscreen_geom_block_t from = { 0U, 0U, 0U, 0U };
        kscreen_geom_block_t at   = { 0U, 0U, 0U, 0U };
        lv_area_t            coords;
        display_compose_t    job;
        bool                 taken;

        lv_obj_get_coords(obj, &coords);
        (void)memset(&job, 0, sizeof(job));
        f.direct = layer_direct(layer);
        f.plain  = frame_plain(obj, pic, layer, &taken);
        f.hooked = taken;
        f.idle   = f.direct && layers_idle(layer->display);
        f.pic_w  = (uint16_t)pic->buf.header.w;
        f.pic_h  = (uint16_t)pic->buf.header.h;
        f.obj    = geom_area(&coords);
        f.clip   = geom_area(&layer->_clip_area);
        f.buf    = geom_area(&layer->buf_area);

        kscreen_geom_frame_why_t const why = kscreen_geom_frame_judge(&f, &from, &at);

        frame_tell(why);
        if ( (KSCREEN_GEOM_FRAME_COPY == why) && pic_view(pic, &job.src) )
        {
            job.op        = DISPLAY_COMPOSE_SCALE;
            job.scale_x   = (uint16_t)KSCREEN_SCALE_ONE;
            job.scale_y   = (uint16_t)KSCREEN_SCALE_ONE;
            job.dst.px    = layer->draw_buf->data;
            job.dst.bytes = layer->draw_buf->data_size;
            job.dst.w     = (uint16_t)layer->draw_buf->header.w;
            job.dst.h     = (uint16_t)layer->draw_buf->header.h;
            job.x         = from.x;
            job.y         = from.y;
            job.w         = from.w;
            job.h         = from.h;
            job.dst_x     = at.x;
            job.dst_y     = at.y;
            rc = compose_run(&job);
            /* Any answer but KODE_OK ends the copies for the boot. A pass that
             * never ended may still be writing the framebuffer LVGL now
             * draws. Any other refusal - the accelerator's, a full queue, a
             * timeout of half a second - would cost the same again at every
             * tile of every frame, and nothing says the next pass fares
             * better; a timed-out pass never runs, so there is nothing to
             * wait out either. */
            if ( KODE_IO_ERROR == rc )
            {
                off = true;
                KLOG_E(TAG, "frame: a copy never ended; LVGL draws frames from now on");
            }
            else if ( KODE_OK != rc )
            {
                off = true;
                KLOG_W(TAG, "frame: the accelerator answered %d; LVGL draws frames from now on",
                       (int)rc);
            }
            else
            {
                /* copied: LVGL's own drawing is skipped */
            }
        }
    }

    kscreen_t * const s = frame_screen(obj);

    if ( KODE_OK == rc )
    {
        lv_event_stop_processing(e);
    }
    if ( NULL != s )
    {
        if ( KODE_OK == rc )
        {
            s->frame_copied++;
        }
        else
        {
            s->frame_drawn++;
        }
    }
}

#endif /* CONFIG_KODE_COMPOSE */

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
        /* cppcheck-suppress misra-c2012-11.8 ; LVGL keeps user data as void *; the callbacks only read the picture */
        (void)lv_obj_add_event_cb(frame, &frame_cover_cb, LV_EVENT_COVER_CHECK, (void *)pic);
#if defined(CONFIG_KODE_COMPOSE)
        /* Before LVGL's own handler, so a copy can stop it. */
        uint32_t const first = (uint32_t)LV_EVENT_DRAW_MAIN | (uint32_t)LV_EVENT_PREPROCESS;

        /* cppcheck-suppress [misra-c2012-10.5,misra-c2012-11.8] ; LVGL's PREPROCESS is a flag OR'ed into the code; the callback only reads the picture */
        (void)lv_obj_add_event_cb(frame, &frame_draw_cb, (lv_event_code_t)first, (void *)pic);
#endif
    }
    return frame;
}
