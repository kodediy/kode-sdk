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

/**
 * @brief What is known of a frame object when LVGL draws it: the facts the
 *        accelerator's copy depends on, gathered from LVGL by kscreen_compose.c.
 */
typedef struct {
    bool direct;   /**< The layer is the panel's framebuffer being drawn, unturned, in the accelerator's format. */
    bool plain;    /**< The object would be drawn as its picture is: whole opacity, no recolour, no transform, nothing of the app's that changes the draw. */
    bool hooked;   /**< A callback of the app's takes the object's DRAW_MAIN: a copy, which ends the event, would leave it out. */
    bool idle;     /**< No draw task is waiting or running in any layer of the display. */
    uint16_t pic_w;  /**< The picture's width. */
    uint16_t pic_h;  /**< Its height. */
    kscreen_geom_area_t obj;   /**< The object on the glass. */
    kscreen_geom_area_t clip;  /**< The area of the glass this draw writes. */
    kscreen_geom_area_t buf;   /**< The area of the glass the layer's buffer holds. */
} kscreen_geom_frame_t;

/** @brief Whether the accelerator copies a frame, and why not. */
typedef enum {
    KSCREEN_GEOM_FRAME_COPY = 0,  /**< It does. */
    KSCREEN_GEOM_FRAME_LAYER,     /**< The layer is not the unturned framebuffer, or not in its format. */
    KSCREEN_GEOM_FRAME_STYLED,    /**< The object is not drawn as its picture is. */
    KSCREEN_GEOM_FRAME_HOOKED,    /**< A callback of the app's takes its DRAW_MAIN. */
    KSCREEN_GEOM_FRAME_SIZE,      /**< The object is not its picture's size. */
    KSCREEN_GEOM_FRAME_PART,      /**< The area drawn is empty, or reaches past the object or the buffer. */
    KSCREEN_GEOM_FRAME_BUSY       /**< LVGL's draw units may still be writing the buffer. */
} kscreen_geom_frame_why_t;

/**
 * @brief Whether a frame is copied by the accelerator, and from where to where.
 *
 * The lasting reasons are asked first and the one that changes from one draw
 * to the next, @c idle, last, so a frame that can never be copied always says
 * the same thing.
 *
 * @param f    The facts.
 * @param from Receives the block of the picture to read; untouched on a refusal.
 * @param at   Receives the block of the buffer to write, the same size; untouched on a refusal.
 * @return KSCREEN_GEOM_FRAME_COPY, or the first reason it is not.
 */
kscreen_geom_frame_why_t kscreen_geom_frame_judge(const kscreen_geom_frame_t *f, kscreen_geom_block_t *from,
                                                  kscreen_geom_block_t *at);

/**
 * @brief Whether a frame hides every pixel under it in the area drawn.
 *
 * Covering is about the pixels the object leaves, not about who draws them:
 * @c direct, @c idle and @c buf are not asked, nor @c hooked - the app's
 * callback runs after the picture is drawn, over it.
 *
 * @param f The facts.
 * @return true for a plain object of its picture's size whose area lies inside it.
 */
bool kscreen_geom_frame_covers(const kscreen_geom_frame_t *f);

#ifdef __cplusplus
}
#endif

#endif /* KSCREEN_GEOM_H_ */
