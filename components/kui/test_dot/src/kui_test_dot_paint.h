/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_test_dot_paint.h
 * @brief The E5 painter: RGB565 pixels written one by one, with the station's anti-aliasing, and the canvas that holds them.
 */
#ifndef KUI_TEST_DOT_PAINT_H
#define KUI_TEST_DOT_PAINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kui_test_dot.h"
#include "lvgl.h"

/* ---- Pixels (no LVGL: the painter is plain C) ---- */

/** The glass's pixels: KUI_TD_W_PX by KUI_TD_H_PX, RGB565. */
typedef struct {
    uint16_t *px;      /**< The first pixel of the first row. */
    size_t stride_px;  /**< Pixels from one row to the next. */
} kui_td_px_t;

/** A point of the glass, in pixels, with a fraction. */
typedef struct {
    float x;  /**< Column. */
    float y;  /**< Row. */
} kui_td_pt_t;

/**
 * @brief A point.
 * @param x Column.
 * @param y Row.
 * @return The point.
 */
kui_td_pt_t kui_td_pt(float x, float y);

/**
 * @brief A 0xRRGGBB colour in RGB565, as the panel shows it.
 * @param rgb The colour.
 * @return Its RGB565 value.
 */
uint16_t kui_td_565(uint32_t rgb);

/**
 * @brief Paint the whole glass one colour.
 * @param g   The pixels.
 * @param rgb The colour.
 */
void kui_td_fill(const kui_td_px_t *g, uint32_t rgb);

/**
 * @brief Paint a rectangle, clipped to the glass.
 * @param g   The pixels.
 * @param x0  Left column.
 * @param y0  Top row.
 * @param w   Width.
 * @param h   Height.
 * @param rgb The colour.
 */
void kui_td_rect(const kui_td_px_t *g, int32_t x0, int32_t y0, int32_t w, int32_t h, uint32_t rgb);

/**
 * @brief An anti-aliased filled disc.
 * @param g   The pixels.
 * @param c   Its centre.
 * @param rad Its radius.
 * @param rgb The colour.
 */
void kui_td_disc(const kui_td_px_t *g, kui_td_pt_t c, float rad, uint32_t rgb);

/**
 * @brief An anti-aliased stroke with round ends.
 * @param g   The pixels.
 * @param a   One end.
 * @param b   The other end.
 * @param hw  Half its width.
 * @param rgb The colour.
 */
void kui_td_stroke(const kui_td_px_t *g, kui_td_pt_t a, kui_td_pt_t b, float hw, uint32_t rgb);

/**
 * @brief An anti-aliased ring.
 * @param g   The pixels.
 * @param c   Its centre.
 * @param rad The radius of its middle line.
 * @param hw  Half its width.
 * @param rgb The colour.
 */
void kui_td_ring(const kui_td_px_t *g, kui_td_pt_t c, float rad, float hw, uint32_t rgb);

/**
 * @brief An anti-aliased rounded rectangle, filled, or an outline when @p ow is above 0.
 * @param g   The pixels.
 * @param c   Its centre.
 * @param hx  Half its width.
 * @param hy  Half its height.
 * @param rad Its corner radius.
 * @param rgb The colour.
 * @param ow  The outline's width; 0 fills it.
 */
void kui_td_rrect(const kui_td_px_t *g, kui_td_pt_t c, float hx, float hy, float rad, uint32_t rgb, float ow);

/**
 * @brief An anti-aliased filled triangle.
 * @param g   The pixels.
 * @param a   A corner.
 * @param b   A corner.
 * @param c   A corner.
 * @param rgb The colour.
 */
void kui_td_tri(const kui_td_px_t *g, kui_td_pt_t a, kui_td_pt_t b, kui_td_pt_t c, uint32_t rgb);

/**
 * @brief A check mark.
 * @param g    The pixels.
 * @param c    Its centre.
 * @param size Its size.
 * @param w    Its stroke's width.
 * @param rgb  The colour.
 */
void kui_td_glyph_check(const kui_td_px_t *g, kui_td_pt_t c, float size, float w, uint32_t rgb);

/**
 * @brief A cross.
 * @param g    The pixels.
 * @param c    Its centre.
 * @param size Its size.
 * @param w    Its strokes' width.
 * @param rgb  The colour.
 */
void kui_td_glyph_cross(const kui_td_px_t *g, kui_td_pt_t c, float size, float w, uint32_t rgb);

/**
 * @brief The repeat glyph: three quarters of a circle with an arrow head.
 * @param g   The pixels.
 * @param c   Its centre.
 * @param rad Its radius.
 * @param w   Its stroke's width.
 * @param rgb The colour.
 */
void kui_td_glyph_retry(const kui_td_px_t *g, kui_td_pt_t c, float rad, float w, uint32_t rgb);

/**
 * @brief Hands off: an open hand struck through by a prohibition sign, in the middle of the band.
 * @param g     The pixels.
 * @param hand  The hand's colour.
 * @param sign  The sign's colour.
 */
void kui_td_glyph_no_touch(const kui_td_px_t *g, uint32_t hand, uint32_t sign);

/**
 * @brief The four marks the locator finds, white squares at their centres.
 * @param g The pixels.
 */
void kui_td_marks(const kui_td_px_t *g);

/**
 * @brief The four witnesses.
 * @param g   The pixels.
 * @param rim true for the black rim a pattern needs under each.
 */
void kui_td_witnesses(const kui_td_px_t *g, bool rim);

/* ---- The canvas (LVGL) ---- */

#define KUI_TD_NAME_SCREEN   "test_dot_screen"   /**< The object name of a screen's canvas. */
#define KUI_TD_NAME_PATTERN  "test_dot_pattern"  /**< The object name of a pattern's canvas. */

/** A canvas of the glass and what it was built with; freed with the canvas. */
typedef struct {
    lv_obj_t *obj;                 /**< The canvas. */
    lv_draw_buf_t *buf;            /**< Its pixels, destroyed with it. */
    lv_timer_t *timer;             /**< What moves on it, or NULL; deleted with it. */
    kui_td_px_t px;                /**< The same pixels, for the painter. */
    uint8_t kind;                  /**< kui_td_screen_t or kui_td_pattern_t. */
    uint8_t arg[KUI_TD_ARG_MAX];   /**< The states it was built with, pending past its arg. */
    uint32_t step;                 /**< Ticks of its timer. */
} kui_td_canvas_t;

/**
 * @brief Make a whole-glass canvas on @p parent.
 * @param parent Its parent.
 * @param name   KUI_TD_NAME_SCREEN or KUI_TD_NAME_PATTERN.
 * @param kind   What it shows.
 * @return The canvas, or NULL when memory ran out (nothing is left on @p parent).
 */
kui_td_canvas_t *kui_td_canvas_create(lv_obj_t *parent, const char *name, uint8_t kind);

/**
 * @brief Find the canvas a builder made on @p parent.
 * @param parent The parent.
 * @param name   Its name.
 * @return The canvas, or NULL.
 */
kui_td_canvas_t *kui_td_canvas_find(const lv_obj_t *parent, const char *name);

/**
 * @brief Give the canvas a timer that calls @p cb every @p period_ms with the canvas.
 * @param c         The canvas.
 * @param period_ms The period.
 * @param cb        What it calls.
 * @return Nothing: without memory, the canvas stays still.
 */
void kui_td_canvas_animate(kui_td_canvas_t *c, uint32_t period_ms, lv_timer_cb_t cb);

/**
 * @brief Put what was painted on the glass.
 * @param c The canvas.
 */
void kui_td_canvas_show(const kui_td_canvas_t *c);

#endif /* KUI_TEST_DOT_PAINT_H */
