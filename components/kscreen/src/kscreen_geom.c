/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kscreen_geom.c
 * @brief A composed picture's arithmetic, run on the host: no LVGL and no accelerator.
 */
#include "kscreen_geom.h"

#include <stddef.h>

#include "kbsp_board.h"
#include "kdrv_compose.h"

_Static_assert(KSCREEN_GEOM_TURNS == KDRV_COMPOSE_TURNS, "a turn is counted as the accelerator counts it");

uint32_t kscreen_geom_bytes(uint16_t w, uint16_t h)
{
    uint32_t bytes = 0U;

    if ( (0U != w) && (0U != h) && (KDRV_COMPOSE_SIDE_MAX >= w) && (KDRV_COMPOSE_SIDE_MAX >= h) )
    {
        /* In 64 bits: two sides of 16 bits and the pixel's bytes can pass 32. */
        uint64_t const rows  = (uint64_t)w * (uint64_t)h * KDRV_COMPOSE_PX_BYTES;
        uint64_t const lines = (rows + (KDRV_COMPOSE_ALIGN - 1U)) / KDRV_COMPOSE_ALIGN;
        uint64_t const whole = lines * KDRV_COMPOSE_ALIGN;

        if ( (uint64_t)UINT32_MAX >= whole )
        {
            bytes = (uint32_t)whole;
        }
    }
    return bytes;
}

bool kscreen_geom_block(uint16_t pic_w, uint16_t pic_h, const kscreen_geom_area_t *area,
                        kscreen_geom_block_t *out)
{
    bool ok = true;

    if ( NULL == area )
    {
        out->x = 0U;
        out->y = 0U;
        out->w = pic_w;
        out->h = pic_h;
    }
    else if ( (0 > area->x1) || (0 > area->y1) || (area->x1 > area->x2) || (area->y1 > area->y2)
             || (area->x2 >= (int32_t)pic_w) || (area->y2 >= (int32_t)pic_h) )
    {
        ok = false;
    }
    else
    {
        /* Both corners are inside: the last column counts. */
        int32_t const w = (area->x2 - area->x1) + 1;
        int32_t const h = (area->y2 - area->y1) + 1;

        out->x = (uint16_t)area->x1;
        out->y = (uint16_t)area->y1;
        out->w = (uint16_t)w;
        out->h = (uint16_t)h;
    }
    return ok;
}

bool kscreen_geom_point(uint16_t pic_w, uint16_t pic_h, int32_t x, int32_t y)
{
    return (0 <= x) && (0 <= y) && (x < (int32_t)pic_w) && (y < (int32_t)pic_h);
}

bool kscreen_geom_packed(uint16_t w, uint32_t stride)
{
    return ((uint32_t)w * KDRV_COMPOSE_PX_BYTES) == stride;
}

bool kscreen_geom_pair(const void *dst, const void *src, uint16_t dst_w, uint16_t dst_h,
                       uint16_t src_w, uint16_t src_h)
{
    return (dst != src) && (dst_w == src_w) && (dst_h == src_h);
}

uint8_t kscreen_geom_grab_turns(uint8_t rot)
{
    uint8_t const turned = (uint8_t)(rot % KSCREEN_GEOM_TURNS);

    return (uint8_t)((KSCREEN_GEOM_TURNS - turned) % KSCREEN_GEOM_TURNS);
}

bool kscreen_geom_grab_fits(uint8_t rot, uint16_t pic_w, uint16_t pic_h)
{
    bool fits;

    if ( 0U == (rot % 2U) )
    {
        fits = ((uint16_t)KBSP_LCD_H_RES == pic_w) && ((uint16_t)KBSP_LCD_V_RES == pic_h);
    }
    else
    {
        fits = ((uint16_t)KBSP_LCD_V_RES == pic_w) && ((uint16_t)KBSP_LCD_H_RES == pic_h);
    }
    return fits;
}

/**
 * @brief Whether an area is not empty and lies inside another, edges included.
 * @param in    The area.
 * @param outer The one it must lie in.
 * @return true when it does.
 */
static bool area_inside(const kscreen_geom_area_t *in, const kscreen_geom_area_t *outer)
{
    return (in->x1 <= in->x2) && (in->y1 <= in->y2)
           && (in->x1 >= outer->x1) && (in->y1 >= outer->y1)
           && (in->x2 <= outer->x2) && (in->y2 <= outer->y2);
}

/**
 * @brief The reasons a frame's own pixels are not its picture's in the area drawn.
 * @param f The facts.
 * @return KSCREEN_GEOM_FRAME_COPY when they are, or STYLED, SIZE or PART.
 */
static kscreen_geom_frame_why_t frame_fit(const kscreen_geom_frame_t *f)
{
    kscreen_geom_frame_why_t why = KSCREEN_GEOM_FRAME_COPY;
    /* In 64 bits: two corners of 32 bits can be a side LVGL never gives. */
    int64_t const obj_w = ((int64_t)f->obj.x2 - (int64_t)f->obj.x1) + 1;
    int64_t const obj_h = ((int64_t)f->obj.y2 - (int64_t)f->obj.y1) + 1;

    if ( !f->plain )
    {
        why = KSCREEN_GEOM_FRAME_STYLED;
    }
    else if ( (obj_w != (int64_t)f->pic_w) || (obj_h != (int64_t)f->pic_h) )
    {
        why = KSCREEN_GEOM_FRAME_SIZE;
    }
    else if ( !area_inside(&f->clip, &f->obj) )
    {
        why = KSCREEN_GEOM_FRAME_PART;
    }
    else
    {
        /* Its pixels are the picture's, every one. */
    }
    return why;
}

kscreen_geom_frame_why_t kscreen_geom_frame_judge(const kscreen_geom_frame_t *f, kscreen_geom_block_t *from,
                                                  kscreen_geom_block_t *at)
{
    kscreen_geom_frame_why_t why = KSCREEN_GEOM_FRAME_LAYER;

    if ( f->direct )
    {
        why = frame_fit(f);
    }
    if ( (KSCREEN_GEOM_FRAME_COPY == why) && f->hooked )
    {
        why = KSCREEN_GEOM_FRAME_HOOKED;
    }
    if ( (KSCREEN_GEOM_FRAME_COPY == why) && !area_inside(&f->clip, &f->buf) )
    {
        why = KSCREEN_GEOM_FRAME_PART;
    }
    if ( (KSCREEN_GEOM_FRAME_COPY == why) && !f->idle )
    {
        why = KSCREEN_GEOM_FRAME_BUSY;
    }
    if ( KSCREEN_GEOM_FRAME_COPY == why )
    {
        /* Inside the object and the buffer, and the object is the picture's
         * size: every number below is small and not negative. */
        int32_t const w      = (f->clip.x2 - f->clip.x1) + 1;
        int32_t const h      = (f->clip.y2 - f->clip.y1) + 1;
        int32_t const from_x = f->clip.x1 - f->obj.x1;
        int32_t const from_y = f->clip.y1 - f->obj.y1;
        int32_t const at_x   = f->clip.x1 - f->buf.x1;
        int32_t const at_y   = f->clip.y1 - f->buf.y1;

        from->x = (uint16_t)from_x;
        from->y = (uint16_t)from_y;
        from->w = (uint16_t)w;
        from->h = (uint16_t)h;
        at->x   = (uint16_t)at_x;
        at->y   = (uint16_t)at_y;
        at->w   = (uint16_t)w;
        at->h   = (uint16_t)h;
    }
    return why;
}

bool kscreen_geom_frame_covers(const kscreen_geom_frame_t *f)
{
    return KSCREEN_GEOM_FRAME_COPY == frame_fit(f);
}
