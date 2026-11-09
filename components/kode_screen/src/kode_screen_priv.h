/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_screen_priv.h
 * @brief What the halves of kode_screen share. Private; never installed.
 */
#ifndef KODE_SCREEN_PRIV_H_
#define KODE_SCREEN_PRIV_H_

#include <stdbool.h>
#include <stdint.h>

#include "kdrv_qmc5883p.h"
#include "kode.h"
#include "kode_internal.h"

/** @brief Bits of a widget number that hold its place on the screen, counted from 1. */
#define KODE_WIDGET_PLACE_BITS 8U

/** @brief Mask of those bits. */
#define KODE_WIDGET_PLACE_MASK (((uint32_t)1U << KODE_WIDGET_PLACE_BITS) - 1U)

/** @brief Mask of a screen's generation before it is shifted into a widget. */
#define KODE_WIDGET_GEN_MASK (UINT32_MAX >> KODE_WIDGET_PLACE_BITS)

/** @brief Alignment of a canvas's pixels: a cache line of the PSRAM. */
#define KODE_CANVAS_ALIGN 64U

#define KODE_TEXT_RGB_MASK    0x00FFFFFFU  /**< A request's colour word, a text's or a paint's: 0xRRGGBB in its low bytes. */
#define KODE_TEXT_ALIGN_SHIFT 24U          /**< Where the alignment sits in that word: its top byte.      */

/* ---- What each widget takes in the screen's body, px ---- */

#define KODE_SCREEN_GAP_H     12U  /**< Between two widgets: the kit's KUI_SPACE_LG.              */
#define KODE_SCREEN_LINE_H    29U  /**< A text, or a value's unit: one line of the body face.   */
#define KODE_SCREEN_NUMBER_H  48U  /**< A value's number: one line of the big face.             */
#define KODE_SCREEN_BUTTON_H  80U  /**< A button: the kit's KUI_PILL_H.                          */

/** @brief Bytes of one canvas pixel (RGB565). */
#define KODE_CANVAS_PIXEL_BYTES 2U

/**
 * @brief The number of the widget at @p place of screen @p gen.
 * @param gen   The screen's generation.
 * @param place The widget's place on the screen, from 0.
 * @return The widget (a canvas handle for a canvas).
 */
static inline uint32_t kode_widget_make(uint32_t gen, uint32_t place)
{
    return ((gen & KODE_WIDGET_GEN_MASK) << KODE_WIDGET_PLACE_BITS) | (place + 1U);
}

/** @brief The screen's half of the standard layer's Active Object (kode_screen.c). */
extern const kode_screen_ops_t kode_screen_ops;

/**
 * @brief Say that the button at @p place of screen @p gen was tapped.
 *
 * Called by the screen's half on the Active Object's thread. Takes no lock
 * (the caller's half may hold it while it waits for that very thread), and a
 * tap meant for an older screen, or for a place past the last, counts for
 * nothing.
 *
 * @param gen   The screen's generation, as the screen request carried it.
 * @param place The button's place on the screen, from 0.
 */
void kode_widget_tapped(uint32_t gen, uint32_t place);

/* ---- The compass ---- */

#define KODE_COMPASS_STEP_MS     50U    /**< Between two readings: the part converts at 50 Hz.           */
#define KODE_COMPASS_SPAN_MIN_MG 200    /**< The widest span must reach this: the Dot was turned.        */
#define KODE_COMPASS_GROWTH_MG   10     /**< A span grown by more than this found a new extreme, mG.     */
#define KODE_COMPASS_SETTLE_MS   3000U  /**< No span has grown for this long: the sweep is complete.     */
#define KODE_COMPASS_AGREE_PCT   80U    /**< Every span is at least this per cent of the widest.         */
#define KODE_COMPASS_ROUND_PCT   20U    /**< Every corrected reading's length is within this per cent of the others'. */
#define KODE_COMPASS_READS_SLACK 8U     /**< Readings past the time limit's count that the clock may allow. */
#define KODE_COMPASS_READS_MAX   ((KODE_MAG_CALIBRATE_MS / KODE_COMPASS_STEP_MS) + KODE_COMPASS_READS_SLACK) /**< Readings a sweep keeps. */

/** @brief What the compass calibration reads and stores; kode_mag_calibrate() gives the real ones. */
typedef struct {
    bool (*read)(int32_t mg[KDRV_QMC5883P_AXES]);    /**< One raw reading, mG; false with the error set. */
    bool (*store)(const kdrv_qmc5883p_cal_t *corr);   /**< Keep the result; false with the error set.     */
} kode_compass_io_t;

/**
 * @brief Run the guided calibration on the glass with @p io.
 *
 * @param io The readings and the store.
 * @return true when the calibration was stored; false with the task's error set.
 */
bool kode_compass_run(const kode_compass_io_t *io);

/* ---- The rasterizer: RGB565 pixels, no lock, no LVGL ---- */

/** @brief A pixel grid: @c w by @c h halfwords, row after row. */
typedef struct {
    uint16_t *px;  /**< The pixels.  */
    uint32_t  w;   /**< Width.       */
    uint32_t  h;   /**< Height.      */
} kode_raster_t;

/**
 * @brief A colour as the canvas holds it.
 * @param rgb 0xRRGGBB; the top byte is ignored.
 * @return RGB565.
 */
uint16_t kode_raster_rgb565(kode_color_t rgb);

/**
 * @brief Fill the whole grid.
 * @param r The grid.
 * @param c RGB565.
 */
void kode_raster_clear(const kode_raster_t *r, uint16_t c);

/**
 * @brief Set one pixel; one outside the grid is skipped.
 * @param r The grid.
 * @param x Column.
 * @param y Row.
 * @param c RGB565.
 */
void kode_raster_point(const kode_raster_t *r, int32_t x, int32_t y, uint16_t c);

/**
 * @brief Draw a Bresenham line from the first point to the second.
 *
 * Only the part inside the grid. Both points are within +-KODE_DRAW_RANGE.
 *
 * @param r  The grid.
 * @param x0 First point's column.
 * @param y0 First point's row.
 * @param x1 Last point's column.
 * @param y1 Last point's row.
 * @param c  RGB565.
 */
void kode_raster_line(const kode_raster_t *r, int32_t x0, int32_t y0,
                      int32_t x1, int32_t y1, uint16_t c);

/**
 * @brief Draw a rectangle, filled or its one-pixel outline.
 *
 * Coordinates and sizes are within +-KODE_DRAW_RANGE; a size of 0 draws nothing.
 *
 * @param r      The grid.
 * @param x      Left column.
 * @param y      Top row.
 * @param w      Width, 0 or more.
 * @param h      Height, 0 or more.
 * @param c      RGB565.
 * @param filled true for the whole rectangle.
 */
void kode_raster_rect(const kode_raster_t *r, int32_t x, int32_t y, int32_t w,
                      int32_t h, uint16_t c, bool filled);

/**
 * @brief Draw a midpoint circle, filled or its one-pixel outline.
 *
 * Coordinates and radius are within +-KODE_DRAW_RANGE.
 *
 * @param r      The grid.
 * @param cx     Centre column.
 * @param cy     Centre row.
 * @param rad    Radius, 0 or more; 0 is one pixel.
 * @param c      RGB565.
 * @param filled true for the disc.
 */
void kode_raster_circle(const kode_raster_t *r, int32_t cx, int32_t cy,
                        int32_t rad, uint16_t c, bool filled);

/**
 * @brief Blend a picture of straight-alpha RGBA bytes over the grid.
 *
 * Only the part inside the grid. Each channel is
 * (src * a + dst * (255 - a) + 127) / 255, with the grid's pixel widened to
 * eight bits per channel and packed back; a 0 alpha leaves the pixel alone.
 *
 * @param r    The grid.
 * @param x    Column of the picture's left edge; within +-KODE_DRAW_RANGE.
 * @param y    Row of its top edge; within +-KODE_DRAW_RANGE.
 * @param rgba @p w * @p h pixels of four bytes, red first, rows top to bottom.
 * @param w    Width of the picture.
 * @param h    Height of the picture.
 */
void kode_raster_blend_rgba(const kode_raster_t *r, int32_t x, int32_t y,
                            const uint8_t *rgba, uint32_t w, uint32_t h);

/**
 * @brief Blend one colour over the grid through an eight-bit mask.
 *
 * The same arithmetic as kode_raster_blend_rgba() with the mask as alpha.
 *
 * @param r      The grid.
 * @param x      Column of the mask's left edge; within +-KODE_DRAW_RANGE.
 * @param y      Row of its top edge; within +-KODE_DRAW_RANGE.
 * @param mask   Rows of @p w bytes, @p stride bytes apart, top to bottom.
 * @param stride Bytes from one row of the mask to the next, at least @p w.
 * @param w      Width of the mask.
 * @param h      Height of the mask.
 * @param rgb    The colour, 0xRRGGBB.
 */
void kode_raster_blend_mask(const kode_raster_t *r, int32_t x, int32_t y,
                            const uint8_t *mask, uint32_t stride, uint32_t w,
                            uint32_t h, kode_color_t rgb);

/**
 * @brief Whether a position is within +-KODE_DRAW_RANGE.
 * @param x Column.
 * @param y Row.
 * @return true when it is; false with the task's error set.
 */
bool kode_draw_coords_ok(int32_t x, int32_t y);

/* ---- A PNG, decoded in the caller's task ---- */

/** @brief A decoded PNG: straight-alpha RGBA, rows top to bottom. */
typedef struct {
    uint32_t w;      /**< Width.                                                   */
    uint32_t h;      /**< Height.                                                  */
    uint8_t *rgba;   /**< w * h * 4 bytes of PSRAM, red first; kode_png_free().    */
} kode_png_t;

/** @brief What reading a PNG found. */
typedef enum {
    KODE_PNG_OK = 0,       /**< Decoded.                                            */
    KODE_PNG_NOT_PNG,      /**< Does not start with the PNG signature.              */
    KODE_PNG_TOO_BIG,      /**< Wider or taller than the glass.                     */
    KODE_PNG_DAMAGED,      /**< Cut short, or its data does not add up.             */
    KODE_PNG_UNSUPPORTED,  /**< Valid, but interlaced.                              */
    KODE_PNG_NO_MEMORY     /**< PSRAM has no room for it.                           */
} kode_png_result_t;

/**
 * @brief Read a PNG's signature and header and nothing else.
 *
 * @param data The file.
 * @param len  Its size.
 * @param w    Out: the width, also for KODE_PNG_TOO_BIG; 0 otherwise.
 * @param h    Out: the height, likewise.
 * @return KODE_PNG_OK when it is a PNG no bigger than the glass;
 *         KODE_PNG_NOT_PNG, KODE_PNG_TOO_BIG, or KODE_PNG_DAMAGED when the
 *         header is cut short or has a zero side.
 */
kode_png_result_t kode_png_size(const uint8_t *data, uint32_t len, uint32_t *w, uint32_t *h);

/**
 * @brief Decode a PNG into RGBA.
 *
 * Untrusted input: the size is checked before anything is allocated, every
 * chunk is bounds-checked, and the inflated data must be exactly the size the
 * header implies. Handles 1 to 16 bits of gray, gray and alpha, RGB, palette
 * (with its transparency) and RGBA, not interlaced; a 16-bit sample keeps its
 * high byte. A gray or RGB colour key is ignored.
 *
 * @param data The file.
 * @param len  Its size.
 * @param out  Out: the picture on KODE_PNG_OK; on KODE_PNG_TOO_BIG only the
 *             size; zeroed at entry, so empty otherwise.
 * @return The result.
 */
kode_png_result_t kode_png_decode(const uint8_t *data, uint32_t len, kode_png_t *out);

/**
 * @brief Set the task's error for a PNG that did not decode.
 * @param path The file's path, as the caller typed it.
 * @param r    The result, not KODE_PNG_OK.
 * @param w    The PNG's width, for KODE_PNG_TOO_BIG.
 * @param h    Its height.
 */
void kode_png_say(const char *path, kode_png_result_t r, uint32_t w, uint32_t h);

/**
 * @brief Read a PNG file and decode it.
 * @param name The public call that reads, for its errors.
 * @param path "sd/..." or "flash/...".
 * @param out  Out: the picture.
 * @return true on success; false with the task's error set.
 */
bool kode_png_load(const char *name, const char *path, kode_png_t *out);

/**
 * @brief Give back a picture's pixels.
 * @param p The picture; its pixels may be NULL.
 */
void kode_png_free(kode_png_t *p);

/**
 * @brief Swap red and blue in place: RGBA bytes become LVGL's ARGB8888 (B, G, R, A).
 * @param p The picture.
 */
void kode_png_swap_rb(kode_png_t *p);

/**
 * @brief Say that the screen has answered a text a caller is waiting for.
 *
 * Called by the screen's half on the Active Object's thread, for every
 * KODE_SCREEN_OP_DRAW_TEXT it takes, drawn or dropped. Takes no lock.
 *
 * @param wait  The request's num[3], as the caller made it.
 * @param drawn true when the text was drawn; false when it was dropped, and
 *              the call fails "the text could not be drawn".
 */
void kode_widget_text_done(uint32_t wait, bool drawn);

/**
 * @brief Add a decoded picture to the screen as an image widget.
 *
 * kode_screen_add_image() after the file is read: the pixels become the screen's on
 * success and are freed here on failure.
 *
 * @param path Names the picture in an error.
 * @param png  The picture; its pixels are taken on every outcome.
 * @return The widget; 0 with the error set.
 */
kode_widget_t kode_screen_image_add(const char *path, kode_png_t *png);

/* ---- The canvases kept for the screen ---- */

/** @brief A canvas's pixels, kept until the screen has let go of them. */
typedef struct kode_canvasbuf kode_canvasbuf_t;

/**
 * @brief Take a canvas's black pixels and keep them under @p handle.
 * @param handle The canvas's number: kode_widget_make() of its screen and place.
 * @param w      Width, 1 to KODE_CANVAS_WIDTH_MAX.
 * @param h      Height, 1 to KODE_CANVAS_HEIGHT_MAX.
 * @return The canvas; NULL when PSRAM has no room.
 */
kode_canvasbuf_t *kode_canvasbuf_open(kode_canvas_t handle, uint32_t w, uint32_t h);

/**
 * @brief Keep a picture's pixels under @p handle: an image the screen shows.
 *
 * An image cannot be drawn on.
 *
 * @param handle The widget's number.
 * @param argb   @p w * @p h * 4 bytes from khal_alloc_psram() with
 *               KODE_CANVAS_ALIGN, as LVGL's ARGB8888 holds them.
 * @param w      Width.
 * @param h      Height.
 * @return The image, which now owns @p argb; NULL when there is no room for
 *         its record, and @p argb is still the caller's.
 */
kode_canvasbuf_t *kode_canvasbuf_adopt(kode_canvas_t handle, uint8_t *argb, uint32_t w, uint32_t h);

/**
 * @brief Free a canvas or image whose post was refused.
 * @param b The buffer from kode_canvasbuf_open() or kode_canvasbuf_adopt().
 */
void kode_canvasbuf_drop(kode_canvasbuf_t *b);

/**
 * @brief Say which screen the canvases belong to now; any other fails its draws.
 * @param gen The new screen's generation.
 */
void kode_canvasbuf_screen_changed(uint32_t gen);

/**
 * @brief Find the canvas at @p place of screen @p gen (valid until that screen is retired).
 * @param gen   The screen's generation.
 * @param place The canvas's place on the screen, from 0.
 * @return The canvas; NULL when there is none.
 */
kode_canvasbuf_t *kode_canvasbuf_get(uint32_t gen, uint32_t place);

/**
 * @brief The pixels of an image.
 * @param b A buffer the screen holds.
 * @param w Out: the width, 0 for a canvas.
 * @param h Out: the height, 0 for a canvas.
 * @return The ARGB8888 bytes; NULL for a canvas.
 */
uint8_t *kode_canvasbuf_image_view(const kode_canvasbuf_t *b, uint32_t *w, uint32_t *h);

/**
 * @brief Blend a colour through a mask into a canvas, as the screen's text does.
 *
 * Takes the same lock a draw does, so it waits for a draw in progress and a
 * draw waits for it; each holds it for one blend only. Marks the canvas drawn.
 *
 * @param b      A canvas the screen holds.
 * @param x      Column of the mask's left edge.
 * @param y      Row of its top edge.
 * @param mask   Eight-bit coverage, rows @p stride bytes apart.
 * @param stride Bytes from a row to the next.
 * @param w      Width.
 * @param h      Height.
 * @param color  The colour, 0xRRGGBB.
 * @return true when blended; false when there is no memory for the lock.
 */
bool kode_canvasbuf_blend_mask(kode_canvasbuf_t *b, int32_t x, int32_t y,
                               const uint8_t *mask, uint32_t stride, uint32_t w,
                               uint32_t h, kode_color_t color);

/**
 * @brief The pixels of a canvas.
 * @param b A canvas the screen holds.
 * @return Its grid; tasks may change the pixels under the reader, a halfword at a time.
 */
kode_raster_t kode_canvasbuf_view(const kode_canvasbuf_t *b);

/**
 * @brief Whether the canvas was drawn on since the last call; clears the mark (no lock).
 *
 * @param b A canvas the screen holds.
 * @return true when something was drawn.
 */
bool kode_canvasbuf_take_dirty(kode_canvasbuf_t *b);

/**
 * @brief Free every canvas of screen @p gen, once the screen's objects for it are deleted.
 * @param gen The generation of the screen the screen has left.
 */
void kode_canvasbuf_retire(uint32_t gen);

#endif /* KODE_SCREEN_PRIV_H_ */
