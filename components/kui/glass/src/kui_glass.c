/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_glass.c
 * @brief The glass's corners by integer arithmetic.
 */
#include "kui_glass.h"

#include <stddef.h>

#include "kbsp_board.h"
#include "kui_internal.h"

int32_t kui_isqrt_floor(int64_t n)
{
    if ( 0 >= n )
    {
        return 0;
    }
    int64_t x = n;
    int64_t y = (x + 1) / 2;
    while ( y < x )
    {
        x = y;
        y = (x + (n / x)) / 2;
    }
    return (int32_t)x;
}

kui_glass_t kui_glass_kode_dot(void)
{
    kui_glass_t const g = {
        (int32_t)KBSP_LCD_H_RES,
        (int32_t)KBSP_LCD_V_RES,
        (int32_t)KBSP_LCD_CORNER_R_PX,
    };
    return g;
}

int32_t kui_glass_inset(const kui_glass_t *glass, int32_t depth)
{
    if ( NULL == glass )
    {
        return 0;
    }
    int32_t const r = glass->corner_r;
    int32_t d = depth;
    if ( 0 > d )
    {
        d = 0;
    }
    if ( d >= r )
    {
        return 0;
    }
    int64_t const off = (int64_t)r - (int64_t)d;
    int64_t const rr = (int64_t)r * (int64_t)r;
    /* The inset rounds up, and ceil(r - s) is r - floor(s): an integer square root suffices. */
    return r - kui_isqrt_floor(rr - (off * off));
}

bool kui_glass_rect_inside(const kui_glass_t *glass, int32_t x1, int32_t y1, int32_t x2, int32_t y2)
{
    if ( NULL == glass )
    {
        return false;
    }
    if ( (0 > x1) || (0 > y1) || (x2 >= glass->w) || (y2 >= glass->h) || (x1 > x2) || (y1 > y2) )
    {
        return false;
    }
    int32_t const top = y1;
    int32_t const bottom = (glass->h - 1) - y2;
    int32_t const left = x1;
    int32_t const right = (glass->w - 1) - x2;

    /* A corner of the rectangle is on the glass when its distance from the
     * side is at least the inset at its depth; inset shrinks with depth, so
     * the rest of the rectangle beyond that corner is on the glass too. */
    if ( (kui_glass_inset(glass, top) > left) || (kui_glass_inset(glass, bottom) > left) )
    {
        return false;
    }
    if ( (kui_glass_inset(glass, top) > right) || (kui_glass_inset(glass, bottom) > right) )
    {
        return false;
    }
    return true;
}
