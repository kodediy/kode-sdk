/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_compose_check.c
 * @brief Whether a pass fits its pictures: pure arithmetic, run on the host.
 */
#include "kdrv_compose_check.h"

#include <stddef.h>

/**
 * @brief Whether @p pic can be read: pixels, a size within the limit, and room for them.
 * @param pic The picture.
 * @return true when it can.
 */
static bool pic_readable(const kdrv_compose_pic_t *pic)
{
    bool ok = false;

    if ( (NULL != pic) && (NULL != pic->px) && (0U != pic->w) && (0U != pic->h)
        && (KDRV_COMPOSE_SIDE_MAX >= pic->w) && (KDRV_COMPOSE_SIDE_MAX >= pic->h) )
    {
        /* In 64 bits: the two sides and the pixel's bytes can pass 32. */
        uint64_t const need = (uint64_t)pic->w * (uint64_t)pic->h * KDRV_COMPOSE_PX_BYTES;

        ok = (need <= (uint64_t)pic->bytes);
    }
    return ok;
}

/**
 * @brief Whether @p pic can be written: readable, and whole cache lines.
 * @param pic The picture.
 * @return true when it can.
 */
static bool pic_writable(const kdrv_compose_pic_t *pic)
{
    bool ok = pic_readable(pic);

    if ( ok )
    {
        /* cppcheck-suppress misra-c2012-11.4 ; the address is judged, never used: a cache line's multiple or not */
        uintptr_t const at = (uintptr_t)pic->px;

        ok = (0U == (at % KDRV_COMPOSE_ALIGN)) && (0U == (pic->bytes % KDRV_COMPOSE_ALIGN));
    }
    return ok;
}

/**
 * @brief Whether a block of @p w by @p h at @p x, @p y lies inside @p pic.
 * @param pic The picture, readable.
 * @param x   First column.
 * @param y   First row.
 * @param w   Columns.
 * @param h   Rows.
 * @return true when it is not empty and fits.
 */
static bool block_fits(const kdrv_compose_pic_t *pic, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return (0U != w) && (0U != h) && (x < pic->w) && (y < pic->h)
        && (w <= ((uint32_t)pic->w - x)) && (h <= ((uint32_t)pic->h - y));
}

/**
 * @brief Whether a scale is one the accelerator takes.
 * @param sixteenths The scale.
 * @return true inside KDRV_COMPOSE_SCALE_MIN..KDRV_COMPOSE_SCALE_MAX.
 */
static bool scale_valid(uint16_t sixteenths)
{
    return (KDRV_COMPOSE_SCALE_MIN <= sixteenths) && (KDRV_COMPOSE_SCALE_MAX >= sixteenths);
}

uint32_t kdrv_compose_scaled(uint32_t pixels, uint16_t sixteenths)
{
    return (pixels * (uint32_t)sixteenths) / KDRV_COMPOSE_SCALE_ONE;
}

void kdrv_compose_landed(const kdrv_compose_rect_t *from, uint16_t sx, uint16_t sy, uint8_t turns,
                         uint32_t *w, uint32_t *h)
{
    uint32_t const across = kdrv_compose_scaled(from->w, sx);
    uint32_t const down   = kdrv_compose_scaled(from->h, sy);

    if ( 0U == (turns % 2U) )
    {
        *w = across;
        *h = down;
    }
    else
    {
        *w = down;
        *h = across;
    }
}

#if defined(CONFIG_KODE_COMPOSE)
kode_status_t kdrv_compose_check_fill(const kdrv_compose_pic_t *dst, const kdrv_compose_rect_t *at)
{
    kode_status_t rc = KODE_DENIED;

    if ( (NULL != at) && pic_writable(dst) && block_fits(dst, at->x, at->y, at->w, at->h) )
    {
        rc = KODE_OK;
    }
    return rc;
}
#endif

kode_status_t kdrv_compose_check_scale(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                       const kdrv_compose_pic_t *src,
                                       const kdrv_compose_rect_t *from, uint16_t sx, uint16_t sy,
                                       uint8_t turns)
{
    kode_status_t rc = KODE_DENIED;

    if ( (NULL != from) && pic_writable(dst) && pic_readable(src) && (dst->px != src->px)
        && scale_valid(sx) && scale_valid(sy) && (KDRV_COMPOSE_TURNS > turns)
        && block_fits(src, from->x, from->y, from->w, from->h) )
    {
        uint32_t w = 0U;
        uint32_t h = 0U;

        kdrv_compose_landed(from, sx, sy, turns, &w, &h);
        if ( block_fits(dst, x, y, w, h) )
        {
            rc = KODE_OK;
        }
    }
    return rc;
}

#if defined(CONFIG_KODE_COMPOSE)
kode_status_t kdrv_compose_check_blend(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                       const kdrv_compose_pic_t *fg,
                                       const kdrv_compose_rect_t *from)
{
    kode_status_t rc = KODE_DENIED;

    if ( (NULL != from) && pic_writable(dst) && pic_readable(fg) && (dst->px != fg->px)
        && block_fits(fg, from->x, from->y, from->w, from->h)
        && block_fits(dst, x, y, from->w, from->h) )
    {
        rc = KODE_OK;
    }
    return rc;
}
#endif
