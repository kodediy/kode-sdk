/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kscreen_geom.h
 * @brief A composed picture's arithmetic: sizes, areas, points and the turn. No LVGL in it.
 */
#ifndef KSCREEN_GEOM_H_
#define KSCREEN_GEOM_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KSCREEN_GEOM_TURNS  4U  /**< Quarter turns in a whole one: a glass is turned 0 to 3. */

/** @brief An area of a picture, both corners inside it, as LVGL counts them. */
typedef struct {
    int32_t x1;  /**< First column. */
    int32_t y1;  /**< First row. */
    int32_t x2;  /**< Last column. */
    int32_t y2;  /**< Last row. */
} kscreen_geom_area_t;

/** @brief A block of a picture: where it starts and how big it is. */
typedef struct {
    uint16_t x;  /**< First column. */
    uint16_t y;  /**< First row. */
    uint16_t w;  /**< Columns. */
    uint16_t h;  /**< Rows. */
} kscreen_geom_block_t;

/**
 * @brief The bytes to allocate for a picture: its rows, rounded up to whole cache lines.
 * @param w Width in pixels.
 * @param h Height in pixels.
 * @return The size, or 0 for a side of nothing or longer than the accelerator takes.
 */
uint32_t kscreen_geom_bytes(uint16_t w, uint16_t h);

/**
 * @brief An area of a picture as the block a pass names.
 * @param pic_w The picture's width.
 * @param pic_h Its height.
 * @param area  The area; NULL is the whole picture.
 * @param out   Receives the block.
 * @return true when the area is not empty and lies inside the picture.
 */
bool kscreen_geom_block(uint16_t pic_w, uint16_t pic_h, const kscreen_geom_area_t *area,
                        kscreen_geom_block_t *out);

/**
 * @brief Whether a point lies inside a picture.
 * @param pic_w The picture's width.
 * @param pic_h Its height.
 * @param x     The point's column.
 * @param y     Its row.
 * @return true when it does.
 */
bool kscreen_geom_point(uint16_t pic_w, uint16_t pic_h, int32_t x, int32_t y);

/**
 * @brief Whether a picture's rows follow one another with nothing between them.
 * @param w      The picture's width in pixels.
 * @param stride The bytes from one row to the next.
 * @return true when a row is exactly its pixels.
 */
bool kscreen_geom_packed(uint16_t w, uint32_t stride);

/**
 * @brief Whether two pictures are a pair a dim can take: two of them, of one size.
 * @param dst   The pixels written.
 * @param src   The pixels read.
 * @param dst_w The written picture's width.
 * @param dst_h Its height.
 * @param src_w The read picture's width.
 * @param src_h Its height.
 * @return true when they are not the same picture and their sizes agree.
 */
bool kscreen_geom_pair(const void *dst, const void *src, uint16_t dst_w, uint16_t dst_h,
                       uint16_t src_w, uint16_t src_h);

/**
 * @brief How the frame in the panel's memory is turned to read as the glass shows it.
 *
 * A glass turned by @p rot quarter turns is drawn turned that way into the
 * panel's memory, counter-clockwise. The copy turns it back.
 *
 * @param rot The glass's turn: 0 upright, 1 for 90 degrees, 2 for 180, 3 for 270.
 * @return Quarter turns counter-clockwise for the copy, 0 to 3.
 */
uint8_t kscreen_geom_grab_turns(uint8_t rot);

/**
 * @brief Whether a picture has the size of the glass as it is turned.
 * @param rot   The glass's turn, 0 to 3.
 * @param pic_w The picture's width.
 * @param pic_h Its height.
 * @return true for the panel's size upright or at 180 degrees, and for its
 *         sides swapped at 90 or 270.
 */
bool kscreen_geom_grab_fits(uint8_t rot, uint16_t pic_w, uint16_t pic_h);

#ifdef __cplusplus
}
#endif

#endif /* KSCREEN_GEOM_H_ */
