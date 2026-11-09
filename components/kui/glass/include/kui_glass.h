/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_glass.h
 * @brief The glass of a Kode screen as plain integers: where content stays out of the corners.
 */
#ifndef KUI_GLASS_H
#define KUI_GLASS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** A screen's glass: the active area and the radius of its four corners. */
typedef struct {
    int32_t w;        /**< Active area width in pixels. */
    int32_t h;        /**< Active area height in pixels. */
    int32_t corner_r; /**< Corner radius in pixels; 0 for a square screen. */
} kui_glass_t;

/**
 * @brief The Kode Dot's glass, from the board description (kbsp).
 * @return 410 x 502 with a 114 px corner radius.
 */
kui_glass_t kui_glass_kode_dot(void);

/**
 * @brief How far in from the side the glass starts at a depth from the top or the bottom edge.
 *
 * Rounded up, so the answer is always safe: at depth 0 it is the whole corner
 * radius, from the radius on it is 0. The shape is symmetric, so the same
 * number is the distance to keep from the top and the bottom at a depth from
 * a side. With a 114 px radius: 4 -> 85, 18 -> 53, 53 -> 18, 80 -> 6.
 *
 * @param glass The glass; NULL answers 0.
 * @param depth Pixels from the top or the bottom edge; a negative depth is treated as 0.
 * @return The inset in pixels, 0 .. corner_r.
 */
int32_t kui_glass_inset(const kui_glass_t *glass, int32_t depth);

/**
 * @brief Whether a rectangle lies entirely on the visible glass.
 *
 * Exact: the rectangle is inside when it is on the screen and each of its
 * four corners keeps the inset its depth demands. x2 and y2 are inclusive,
 * as in an LVGL area.
 *
 * @param glass The glass; NULL answers false.
 * @param x1 Left column.
 * @param y1 Top row.
 * @param x2 Right column, inclusive.
 * @param y2 Bottom row, inclusive.
 * @return true when no pixel of the rectangle is hidden by the glass.
 */
bool kui_glass_rect_inside(const kui_glass_t *glass, int32_t x1, int32_t y1, int32_t x2, int32_t y2);

#ifdef __cplusplus
}
#endif

#endif /* KUI_GLASS_H */
