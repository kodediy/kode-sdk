/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_canvas.c
 * @brief A canvas's pixels, the RGB565 rasterizer and the list of canvases.
 */
#include "kode.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_screen_priv.h"

/** @brief Shift of the red byte in 0xRRGGBB. */
#define RGB_RED_SHIFT 16U

/** @brief Shift of the green byte in 0xRRGGBB. */
#define RGB_GREEN_SHIFT 8U

/** @brief Mask of one colour byte. */
#define RGB_BYTE_MASK 0xFFU

/** @brief Bits a byte loses going to the 5 of red and blue. */
#define RGB565_DROP_5 3U

/** @brief Bits a byte loses going to the 6 of green. */
#define RGB565_DROP_6 2U

/** @brief Shift of red in RGB565. */
#define RGB565_RED_SHIFT 11U

/** @brief Shift of green in RGB565. */
#define RGB565_GREEN_SHIFT 5U

/** @brief Bits a 5-bit channel needs to become eight, and a 6-bit channel. */
#define RGB565_GAIN_5 3U
#define RGB565_GAIN_6 2U

/** @brief Bits a 5 or 6-bit channel is shifted down to refill the low bits of its byte. */
#define RGB565_REFILL_5 2U
#define RGB565_REFILL_6 4U

/** @brief Mask of the red and blue channels in RGB565, and of the green. */
#define RGB565_MASK_5 0x1FU
#define RGB565_MASK_6 0x3FU

/** @brief Alpha at which a pixel is written whole, and its half-way rounding. */
#define ALPHA_OPAQUE 255U
#define ALPHA_ROUND  127U

/** @brief Bytes of a picture's pixel. */
#define PICTURE_PIXEL_BYTES 4U

/** @brief One canvas: the list's link, its number, its pixels and whether a draw is unseen. */
struct kode_canvasbuf {
    kode_canvasbuf_t *next;    /**< The next canvas of the list; guarded by s_canvas_lock.   */
    kode_canvas_t     handle;  /**< Its number; fixed.                                */
    uint32_t          w;       /**< Width; fixed.                                     */
    uint32_t          h;       /**< Height; fixed.                                    */
    uint16_t         *px;      /**< A canvas: w * h pixels; fixed until freed. NULL for an image. */
    uint8_t          *argb;    /**< An image: w * h * 4 bytes; fixed until freed. NULL for a canvas. */
    atomic_bool       dirty;   /**< Drawn on since the screen last looked.            */
};

/** @brief What a draw call asks of the rasterizer. */
typedef struct {
    int32_t  x;       /**< A point, a corner or a centre: column.              */
    int32_t  y;       /**< Its row.                                            */
    int32_t  p;       /**< A line's last column, a width or a radius.          */
    int32_t  q;       /**< A line's last row or a height.                      */
    uint16_t color;   /**< RGB565.                                             */
    bool     filled;  /**< A rectangle or circle: the whole shape.             */
    const kode_png_t *png;  /**< A picture to blend at (x, y), else NULL.      */
} shape_t;

/** @brief How a shape lands on a grid. */
typedef void (*shape_fn_t)(const kode_raster_t *r, const shape_t *s);

/** @brief A line being walked along its longer axis. */
typedef struct {
    int64_t major0;   /**< Where it starts on the longer axis.                 */
    int64_t minor0;   /**< Where it starts on the other.                       */
    int64_t len;      /**< Steps along the longer axis.                        */
    int64_t span;     /**< Steps along the other, no more than len.            */
    int64_t smaj;     /**< 1 or -1: the way along the longer axis.             */
    int64_t smin;     /**< 1 or -1: the way along the other.                   */
    int64_t limit;    /**< The grid's extent along the longer axis.            */
    bool    steep;    /**< The longer axis is the rows.                        */
} canvas_walk_t;

/** @brief Guards s_head, s_gen and every draw; made by the first call that needs it. */
static khal_mutex_t s_canvas_lock;

/** @brief The canvases kept, newest first; guarded by s_canvas_lock. */
static kode_canvasbuf_t *s_head;

/** @brief The screen the draws are checked against; guarded by s_canvas_lock. */
static uint32_t s_gen;

/* ---- The rasterizer --------------------------------------------------------------- */

uint16_t kode_raster_rgb565(kode_color_t rgb)
{
    const uint32_t r = (rgb >> RGB_RED_SHIFT) & RGB_BYTE_MASK;
    const uint32_t g = (rgb >> RGB_GREEN_SHIFT) & RGB_BYTE_MASK;
    const uint32_t b = rgb & RGB_BYTE_MASK;

    return (uint16_t)(((r >> RGB565_DROP_5) << RGB565_RED_SHIFT)
                      | ((g >> RGB565_DROP_6) << RGB565_GREEN_SHIFT)
                      | (b >> RGB565_DROP_5));
}

/** @brief The smaller of two. */
static int64_t low_of(int64_t a, int64_t b)
{
    int64_t v = b;

    if ( a < b )
    {
        v = a;
    }
    return v;
}

/** @brief The larger of two. */
static int64_t high_of(int64_t a, int64_t b)
{
    int64_t v = b;

    if ( a > b )
    {
        v = a;
    }
    return v;
}

/** @brief 1 for a value of 0 or more, -1 below. */
static int64_t way_of(int64_t v)
{
    int64_t way = 1;

    if ( 0 > v )
    {
        way = -1;
    }
    return way;
}

/** @brief @p v without its sign. */
static int64_t abs_of(int64_t v)
{
    int64_t n = v;

    if ( 0 > v )
    {
        n = -v;
    }
    return n;
}

/** @brief Set a pixel if it is on the grid. */
static void px_set(const kode_raster_t *r, int64_t x, int64_t y, uint16_t c)
{
    if ( (0 <= x) && (0 <= y) && ((int64_t)r->w > x) && ((int64_t)r->h > y) )
    {
        r->px[((uint32_t)y * r->w) + (uint32_t)x] = c;
    }
}

/** @brief Set the pixels of row @p y from column @p xa to @p xb that are on the grid. */
static void row_set(const kode_raster_t *r, int64_t y, int64_t xa, int64_t xb, uint16_t c)
{
    if ( (0 <= y) && ((int64_t)r->h > y) )
    {
        const int64_t last = low_of(xb, (int64_t)r->w - 1);

        for ( int64_t x = high_of(xa, 0); x <= last; x++ )
        {
            r->px[((uint32_t)y * r->w) + (uint32_t)x] = c;
        }
    }
}

/** @brief Set the pixels of column @p x from row @p ya to @p yb that are on the grid. */
static void col_set(const kode_raster_t *r, int64_t x, int64_t ya, int64_t yb, uint16_t c)
{
    if ( (0 <= x) && ((int64_t)r->w > x) )
    {
        const int64_t last = low_of(yb, (int64_t)r->h - 1);

        for ( int64_t y = high_of(ya, 0); y <= last; y++ )
        {
            r->px[((uint32_t)y * r->w) + (uint32_t)x] = c;
        }
    }
}

void kode_raster_clear(const kode_raster_t *r, uint16_t c)
{
    for ( uint32_t i = 0U; (r->w * r->h) > i; i++ )
    {
        r->px[i] = c;
    }
}

void kode_raster_point(const kode_raster_t *r, int32_t x, int32_t y, uint16_t c)
{
    px_set(r, x, y, c);
}

/**
 * @brief Walk a line along its longer axis and set the pixels on the grid.
 *
 * Step k lands on the other axis at the rounded k * span / len, half way
 * going on: the same pixels as the textbook error-term loop, with the
 * steps that fall off the grid never taken.
 *
 * @param r The grid.
 * @param w The line.
 * @param c RGB565.
 */
static void line_walk(const kode_raster_t *r, const canvas_walk_t *w, uint16_t c)
{
    int64_t first = 0;
    int64_t last  = w->len;

    if ( 0 < w->smaj )
    {
        first = high_of(first, -w->major0);
        last  = low_of(last, w->limit - 1 - w->major0);
    }
    else
    {
        first = high_of(first, w->major0 - (w->limit - 1));
        last  = low_of(last, w->major0);
    }
    for ( int64_t k = first; k <= last; k++ )
    {
        int64_t off = 0;

        if ( 0 < w->len )
        {
            off = ((2 * w->span * k) + w->len) / (2 * w->len);
        }
        const int64_t major = w->major0 + (w->smaj * k);
        const int64_t minor = w->minor0 + (w->smin * off);

        if ( w->steep )
        {
            px_set(r, minor, major, c);
        }
        else
        {
            px_set(r, major, minor, c);
        }
    }
}

void kode_raster_line(const kode_raster_t *r, int32_t x0, int32_t y0,
                      int32_t x1, int32_t y1, uint16_t c)
{
    const int64_t dx = (int64_t)x1 - (int64_t)x0;
    const int64_t dy = (int64_t)y1 - (int64_t)y0;
    canvas_walk_t w;

    if ( abs_of(dx) >= abs_of(dy) )
    {
        w.steep  = false;
        w.major0 = x0;
        w.minor0 = y0;
        w.len    = abs_of(dx);
        w.span   = abs_of(dy);
        w.smaj   = way_of(dx);
        w.smin   = way_of(dy);
        w.limit  = (int64_t)r->w;
    }
    else
    {
        w.steep  = true;
        w.major0 = y0;
        w.minor0 = x0;
        w.len    = abs_of(dy);
        w.span   = abs_of(dx);
        w.smaj   = way_of(dy);
        w.smin   = way_of(dx);
        w.limit  = (int64_t)r->h;
    }
    line_walk(r, &w, c);
}

void kode_raster_rect(const kode_raster_t *r, int32_t x, int32_t y, int32_t w,
                      int32_t h, uint16_t c, bool filled)
{
    if ( (0 < w) && (0 < h) )
    {
        const int64_t xb = (int64_t)x + (int64_t)w - 1;
        const int64_t yb = (int64_t)y + (int64_t)h - 1;

        if ( filled )
        {
            const int64_t last = low_of(yb, (int64_t)r->h - 1);

            for ( int64_t row = high_of(y, 0); row <= last; row++ )
            {
                row_set(r, row, x, xb, c);
            }
        }
        else
        {
            row_set(r, y, x, xb, c);
            row_set(r, yb, x, xb, c);
            col_set(r, x, y, yb, c);
            col_set(r, xb, y, yb, c);
        }
    }
}

/** @brief The eight pixels the midpoint walk finds for (dx, dy) around the centre. */
static void circle_dots(const kode_raster_t *r, int64_t cx, int64_t cy,
                        int64_t dx, int64_t dy, uint16_t c)
{
    px_set(r, cx + dx, cy + dy, c);
    px_set(r, cx - dx, cy + dy, c);
    px_set(r, cx + dx, cy - dy, c);
    px_set(r, cx - dx, cy - dy, c);
    px_set(r, cx + dy, cy + dx, c);
    px_set(r, cx - dy, cy + dx, c);
    px_set(r, cx + dy, cy - dx, c);
    px_set(r, cx - dy, cy - dx, c);
}

/** @brief Row @p dy above and below the centre, from @p dx left to @p dx right. */
static void rows_pair(const kode_raster_t *r, int64_t cx, int64_t cy,
                      int64_t dx, int64_t dy, uint16_t c)
{
    row_set(r, cy + dy, cx - dx, cx + dx, c);
    row_set(r, cy - dy, cx - dx, cx + dx, c);
}

void kode_raster_circle(const kode_raster_t *r, int32_t cx, int32_t cy,
                        int32_t rad, uint16_t c, bool filled)
{
    int64_t x   = rad;
    int64_t y   = 0;
    int64_t err = 1 - (int64_t)rad;

    while ( x >= y )
    {
        const int64_t y_here = y;

        if ( filled )
        {
            rows_pair(r, cx, cy, x, y_here, c);
        }
        else
        {
            circle_dots(r, cx, cy, x, y_here, c);
        }
        y++;
        if ( 0 > err )
        {
            err += (2 * y) + 1;
        }
        else
        {
            if ( filled )
            {
                /* The widest y this x will see: its rows at the poles, once. */
                rows_pair(r, cx, cy, y_here, x, c);
            }
            x--;
            err += (2 * (y - x)) + 1;
        }
    }
}

/**
 * @brief Mix one source pixel over a grid pixel.
 *
 * Each channel is (src * a + dst * (255 - a) + 127) / 255, the grid's pixel
 * widened to eight bits per channel first; a pixel at alpha 255 is the
 * source whole and one at 0 is left alone.
 *
 * @param dst The grid's pixel.
 * @param r   Source red.
 * @param g   Source green.
 * @param b   Source blue.
 * @param a   Source alpha.
 */
static void px_blend(uint16_t *dst, uint32_t r, uint32_t g, uint32_t b, uint32_t a)
{
    if ( ALPHA_OPAQUE <= a )
    {
        *dst = (uint16_t)(((r >> RGB565_GAIN_5) << RGB565_RED_SHIFT)
                          | ((g >> RGB565_GAIN_6) << RGB565_GREEN_SHIFT) | (b >> RGB565_GAIN_5));
    }
    else if ( 0U < a )
    {
        const uint32_t d  = *dst;
        const uint32_t dr = (d >> RGB565_RED_SHIFT) & RGB565_MASK_5;
        const uint32_t dg = (d >> RGB565_GREEN_SHIFT) & RGB565_MASK_6;
        const uint32_t db = d & RGB565_MASK_5;
        const uint32_t er = (dr << RGB565_GAIN_5) | (dr >> RGB565_REFILL_5);
        const uint32_t eg = (dg << RGB565_GAIN_6) | (dg >> RGB565_REFILL_6);
        const uint32_t eb = (db << RGB565_GAIN_5) | (db >> RGB565_REFILL_5);
        const uint32_t inv = ALPHA_OPAQUE - a;
        const uint32_t mr = ((r * a) + (er * inv) + ALPHA_ROUND) / ALPHA_OPAQUE;
        const uint32_t mg = ((g * a) + (eg * inv) + ALPHA_ROUND) / ALPHA_OPAQUE;
        const uint32_t mb = ((b * a) + (eb * inv) + ALPHA_ROUND) / ALPHA_OPAQUE;

        *dst = (uint16_t)(((mr >> RGB565_GAIN_5) << RGB565_RED_SHIFT)
                          | ((mg >> RGB565_GAIN_6) << RGB565_GREEN_SHIFT) | (mb >> RGB565_GAIN_5));
    }
    else
    {
        /* clear: nothing to draw */
    }
}

/**
 * @brief The part of a @p w by @p h block at (@p x, @p y) that lies on the grid.
 * @param r  The grid.
 * @param x  Block's left column.
 * @param y  Block's top row.
 * @param w  Its width.
 * @param h  Its height.
 * @param c0 Out: first column of the block on the grid.
 * @param c1 Out: one past the last.
 * @param r0 Out: first row.
 * @param r1 Out: one past the last.
 */
static void block_clip(const kode_raster_t *r, int32_t x, int32_t y, uint32_t w, uint32_t h,
                       uint32_t *c0, uint32_t *c1, uint32_t *r0, uint32_t *r1)
{
    const int64_t left  = high_of(0, -(int64_t)x);
    const int64_t top   = high_of(0, -(int64_t)y);
    const int64_t right = low_of((int64_t)w, (int64_t)r->w - x);
    const int64_t bottom = low_of((int64_t)h, (int64_t)r->h - y);

    *c0 = 0U;
    *c1 = 0U;
    *r0 = 0U;
    *r1 = 0U;
    if ( (left < right) && (top < bottom) )
    {
        *c0 = (uint32_t)left;
        *c1 = (uint32_t)right;
        *r0 = (uint32_t)top;
        *r1 = (uint32_t)bottom;
    }
}

void kode_raster_blend_rgba(const kode_raster_t *r, int32_t x, int32_t y,
                            const uint8_t *rgba, uint32_t w, uint32_t h)
{
    uint32_t c0;
    uint32_t c1;
    uint32_t r0;
    uint32_t r1;

    block_clip(r, x, y, w, h, &c0, &c1, &r0, &r1);
    for ( uint32_t row = r0; row < r1; row++ )
    {
        for ( uint32_t col = c0; col < c1; col++ )
        {
            const uint8_t *const src = &rgba[((row * w) + col) * PICTURE_PIXEL_BYTES];
            const int64_t        at_x = (int64_t)x + (int64_t)col;
            const int64_t        at_y = (int64_t)y + (int64_t)row;

            px_blend(&r->px[((uint32_t)at_y * r->w) + (uint32_t)at_x], src[0], src[1], src[2], src[3]);
        }
    }
}

void kode_raster_blend_mask(const kode_raster_t *r, int32_t x, int32_t y,
                            const uint8_t *mask, uint32_t stride, uint32_t w,
                            uint32_t h, kode_color_t rgb)
{
    const uint32_t red   = (rgb >> RGB_RED_SHIFT) & RGB_BYTE_MASK;
    const uint32_t green = (rgb >> RGB_GREEN_SHIFT) & RGB_BYTE_MASK;
    const uint32_t blue  = rgb & RGB_BYTE_MASK;
    uint32_t       c0;
    uint32_t       c1;
    uint32_t       r0;
    uint32_t       r1;

    block_clip(r, x, y, w, h, &c0, &c1, &r0, &r1);
    for ( uint32_t row = r0; row < r1; row++ )
    {
        for ( uint32_t col = c0; col < c1; col++ )
        {
            const int64_t at_x = (int64_t)x + (int64_t)col;
            const int64_t at_y = (int64_t)y + (int64_t)row;

            px_blend(&r->px[((uint32_t)at_y * r->w) + (uint32_t)at_x], red, green, blue,
                     mask[(row * stride) + col]);
        }
    }
}

/* ---- The canvases kept for the screen ------------------------------------------------ */

/** @brief Make the mutex: what the first call runs. */
static bool lock_make(void)
{
    s_canvas_lock = khal_mutex_create();
    return NULL != s_canvas_lock;
}

/**
 * @brief Take the mutex, making it first if no call has.
 * @return true with the mutex held; false when there is no memory for it.
 */
static bool reg_lock(void)
{
    static khal_once_t once;  /* the one-shot the mutex is made by */
    const bool         ok = khal_once_try(&once, &lock_make);

    if ( ok )
    {
        khal_mutex_lock(s_canvas_lock);
    }
    return ok;
}

/** @brief Free a canvas's pixels and the canvas; it is in no list. */
static void canvas_free(kode_canvasbuf_t *b)
{
    khal_free(b->px);
    khal_free(b->argb);
    khal_free(b);
}

/**
 * @brief The canvas numbered @p handle; the caller holds the mutex.
 * @param handle A canvas number.
 * @return The canvas, or NULL.
 */
static kode_canvasbuf_t *reg_find(kode_canvas_t handle)
{
    kode_canvasbuf_t *found = NULL;

    for ( kode_canvasbuf_t *b = s_head; (NULL != b) && (NULL == found); b = b->next )
    {
        if ( b->handle == handle )
        {
            found = b;
        }
    }
    return found;
}

kode_canvasbuf_t *kode_canvasbuf_open(kode_canvas_t handle, uint32_t w, uint32_t h)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a canvas */
    kode_canvasbuf_t *b = (kode_canvasbuf_t *)khal_alloc_psram(sizeof(void *), sizeof(*b));

    if ( NULL != b )
    {
        b->argb = NULL;
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is pixels */
        b->px = (uint16_t *)khal_alloc_psram(KODE_CANVAS_ALIGN,
                                             (size_t)w * (size_t)h * KODE_CANVAS_PIXEL_BYTES);
        b->handle = handle;
        b->w      = w;
        b->h      = h;
        atomic_store(&b->dirty, false);
        if ( NULL == b->px )
        {
            khal_free(b);
            b = NULL;
        }
    }
    if ( NULL != b )
    {
        if ( reg_lock() )
        {
            b->next = s_head;
            s_head  = b;
            khal_mutex_unlock(s_canvas_lock);
        }
        else
        {
            canvas_free(b);
            b = NULL;
        }
    }
    return b;
}

kode_canvasbuf_t *kode_canvasbuf_adopt(kode_canvas_t handle, uint8_t *argb, uint32_t w, uint32_t h)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is an image */
    kode_canvasbuf_t *b = (kode_canvasbuf_t *)khal_alloc_psram(sizeof(void *), sizeof(*b));

    if ( NULL != b )
    {
        b->px     = NULL;
        b->argb   = argb;
        b->handle = handle;
        b->w      = w;
        b->h      = h;
        atomic_store(&b->dirty, false);
        if ( reg_lock() )
        {
            b->next = s_head;
            s_head  = b;
            khal_mutex_unlock(s_canvas_lock);
        }
        else
        {
            /* the pixels stay the caller's */
            khal_free(b);
            b = NULL;
        }
    }
    return b;
}

void kode_canvasbuf_drop(kode_canvasbuf_t *b)
{
    if ( reg_lock() )
    {
        kode_canvasbuf_t **link = &s_head;

        while ( (NULL != *link) && (*link != b) )
        {
            link = &(*link)->next;
        }
        if ( *link == b )
        {
            *link = b->next;
        }
        khal_mutex_unlock(s_canvas_lock);
        canvas_free(b);
    }
}

void kode_canvasbuf_screen_changed(uint32_t gen)
{
    if ( reg_lock() )
    {
        s_gen = gen & KODE_WIDGET_GEN_MASK;
        khal_mutex_unlock(s_canvas_lock);
    }
}

kode_canvasbuf_t *kode_canvasbuf_get(uint32_t gen, uint32_t place)
{
    kode_canvasbuf_t *b = NULL;

    if ( reg_lock() )
    {
        b = reg_find(kode_widget_make(gen, place));
        khal_mutex_unlock(s_canvas_lock);
    }
    return b;
}

uint8_t *kode_canvasbuf_image_view(const kode_canvasbuf_t *b, uint32_t *w, uint32_t *h)
{
    *w = 0U;
    *h = 0U;
    if ( NULL != b->argb )
    {
        *w = b->w;
        *h = b->h;
    }
    return b->argb;
}

bool kode_canvasbuf_blend_mask(kode_canvasbuf_t *b, int32_t x, int32_t y,
                               const uint8_t *mask, uint32_t stride, uint32_t w,
                               uint32_t h, kode_color_t color)
{
    const bool ok = reg_lock();

    if ( ok )
    {
        const kode_raster_t r = { b->px, b->w, b->h };

        kode_raster_blend_mask(&r, x, y, mask, stride, w, h, color);
        atomic_store(&b->dirty, true);
        khal_mutex_unlock(s_canvas_lock);
    }
    return ok;
}

kode_raster_t kode_canvasbuf_view(const kode_canvasbuf_t *b)
{
    const kode_raster_t view = { b->px, b->w, b->h };

    return view;
}

bool kode_canvasbuf_take_dirty(kode_canvasbuf_t *b)
{
    return atomic_exchange(&b->dirty, false);
}

void kode_canvasbuf_retire(uint32_t gen)
{
    kode_canvasbuf_t *dead = NULL;

    if ( reg_lock() )
    {
        kode_canvasbuf_t **link = &s_head;

        while ( NULL != *link )
        {
            kode_canvasbuf_t *const b = *link;

            if ( (b->handle >> KODE_WIDGET_PLACE_BITS) == (gen & KODE_WIDGET_GEN_MASK) )
            {
                *link   = b->next;
                b->next = dead;
                dead    = b;
            }
            else
            {
                link = &b->next;
            }
        }
        khal_mutex_unlock(s_canvas_lock);
    }
    /* Out of the list under the mutex: no draw can find these any more. */
    while ( NULL != dead )
    {
        kode_canvasbuf_t *const next = dead->next;

        canvas_free(dead);
        dead = next;
    }
}

/* ---- The draw calls ------------------------------------------------------------------ */

/** @brief Why a handle names no canvas of the screen. */
typedef enum {
    CANVAS_OK = 0,   /**< It does.                                       */
    CANVAS_NONE,     /**< Nothing, or not a canvas, has that number.     */
    CANVAS_OLD       /**< It was a canvas of an older screen.              */
} canvas_why_t;

/**
 * @brief The canvas numbered @p c, if it is one of the current screen; the caller holds the mutex.
 * @param c   The caller's canvas.
 * @param why Out: CANVAS_OK, or what is wrong.
 * @return The canvas; NULL with @p why set. An image is not a canvas.
 */
static kode_canvasbuf_t *canvas_find(kode_canvas_t c, canvas_why_t *why)
{
    kode_canvasbuf_t *b = reg_find(c);

    *why = CANVAS_OK;
    if ( (0U != c) && ((c >> KODE_WIDGET_PLACE_BITS) != s_gen) )
    {
        *why = CANVAS_OLD;
        b    = NULL;
    }
    else if ( (NULL == b) || (NULL == b->px) )
    {
        *why = CANVAS_NONE;
        b    = NULL;
    }
    else
    {
        /* a canvas of this screen */
    }
    return b;
}

/** @brief Set the task's error for a canvas that is not one. */
static void canvas_say(canvas_why_t why)
{
    if ( CANVAS_OLD == why )
    {
        kode_err_set("this canvas belongs to an old screen");
    }
    else if ( CANVAS_NONE == why )
    {
        kode_err_set("no such canvas");
    }
    else
    {
        /* nothing wrong */
    }
}

/**
 * @brief Find the canvas, rasterize and mark it, all under the mutex.
 * @param c  The caller's canvas.
 * @param fn How the shape lands.
 * @param s  The shape, already checked.
 * @return true when drawn; false with the task's error set.
 */
static bool draw_run(kode_canvas_t c, shape_fn_t fn, const shape_t *s)
{
    bool ok = reg_lock();

    if ( !ok )
    {
        kode_err_set("no memory for the canvas lock");
    }
    else
    {
        canvas_why_t            why = CANVAS_OK;
        kode_canvasbuf_t *const b   = canvas_find(c, &why);

        ok = (NULL != b);
        if ( ok )
        {
            const kode_raster_t r = { b->px, b->w, b->h };

            fn(&r, s);
            atomic_store(&b->dirty, true);
        }
        khal_mutex_unlock(s_canvas_lock);
        canvas_say(why);
    }
    return ok;
}

/**
 * @brief Whether @p c is a canvas of the current screen.
 * @param c The caller's canvas.
 * @return true when it is; false with the task's error set.
 */
static bool canvas_check(kode_canvas_t c)
{
    bool ok = reg_lock();

    if ( !ok )
    {
        kode_err_set("no memory for the canvas lock");
    }
    else
    {
        canvas_why_t why = CANVAS_OK;

        ok = (canvas_find(c, &why) != NULL);
        khal_mutex_unlock(s_canvas_lock);
        canvas_say(why);
    }
    return ok;
}

/**
 * @brief Whether all four numbers of a shape are within KODE_DRAW_RANGE.
 * @param s The shape.
 * @return true when they are; false with the task's error set.
 */
static bool shape_in_range(const shape_t *s)
{
    const bool ok = (-KODE_DRAW_RANGE <= s->x) && (KODE_DRAW_RANGE >= s->x)
                    && (-KODE_DRAW_RANGE <= s->y) && (KODE_DRAW_RANGE >= s->y)
                    && (-KODE_DRAW_RANGE <= s->p) && (KODE_DRAW_RANGE >= s->p)
                    && (-KODE_DRAW_RANGE <= s->q) && (KODE_DRAW_RANGE >= s->q);

    if ( !ok )
    {
        kode_err_set("a coordinate, size or radius is beyond %" PRId32 " either way",
                     (int32_t)KODE_DRAW_RANGE);
    }
    return ok;
}

bool kode_draw_coords_ok(int32_t x, int32_t y)
{
    const shape_t s = { x, y, 0, 0, 0U, true, NULL };

    return shape_in_range(&s);
}

/** @brief Fill the grid. */
static void shape_clear(const kode_raster_t *r, const shape_t *s)
{
    kode_raster_clear(r, s->color);
}

/** @brief A point. */
static void shape_point(const kode_raster_t *r, const shape_t *s)
{
    kode_raster_point(r, s->x, s->y, s->color);
}

/** @brief A line from (x, y) to (p, q). */
static void shape_line(const kode_raster_t *r, const shape_t *s)
{
    kode_raster_line(r, s->x, s->y, s->p, s->q, s->color);
}

/** @brief A rectangle at (x, y), p by q. */
static void shape_rect(const kode_raster_t *r, const shape_t *s)
{
    kode_raster_rect(r, s->x, s->y, s->p, s->q, s->color, s->filled);
}

/** @brief A circle around (x, y) of radius p. */
static void shape_circle(const kode_raster_t *r, const shape_t *s)
{
    kode_raster_circle(r, s->x, s->y, s->p, s->color, s->filled);
}

bool kode_canvas_fill(kode_canvas_t c, kode_color_t color)
{
    const shape_t s  = { 0, 0, 0, 0, kode_raster_rgb565(color), true, NULL };
    bool          ok = kode_enter("kode_canvas_fill");

    if ( ok )
    {
        ok = draw_run(c, &shape_clear, &s);
    }
    return ok;
}

bool kode_draw_pixel(kode_canvas_t c, int32_t x, int32_t y, kode_color_t color)
{
    const shape_t s  = { x, y, 0, 0, kode_raster_rgb565(color), true, NULL };
    bool          ok = kode_enter("kode_draw_pixel") && shape_in_range(&s);

    if ( ok )
    {
        ok = draw_run(c, &shape_point, &s);
    }
    return ok;
}

bool kode_draw_line(kode_canvas_t c, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                    kode_color_t color)
{
    const shape_t s  = { x0, y0, x1, y1, kode_raster_rgb565(color), true, NULL };
    bool          ok = kode_enter("kode_draw_line") && shape_in_range(&s);

    if ( ok )
    {
        ok = draw_run(c, &shape_line, &s);
    }
    return ok;
}

/**
 * @brief Draw a rectangle's outline or the whole of it.
 * @param call   The public call's name, for the error text.
 * @param c      The canvas.
 * @param x      Left column.
 * @param y      Top row.
 * @param w      Width in pixels.
 * @param h      Height in pixels.
 * @param color  The colour.
 * @param filled true for the whole rectangle.
 * @return true when drawn; false with the error set.
 */
static bool rect_draw(const char *call, kode_canvas_t c, int32_t x, int32_t y, int32_t w,
                      int32_t h, kode_color_t color, bool filled)
{
    const shape_t s  = { x, y, w, h, kode_raster_rgb565(color), filled, NULL };
    bool          ok = kode_enter(call) && shape_in_range(&s);

    if ( ok && ((0 > w) || (0 > h)) )
    {
        kode_err_set("a width or height is negative");
        ok = false;
    }
    if ( ok )
    {
        ok = draw_run(c, &shape_rect, &s);
    }
    return ok;
}

/**
 * @brief Draw a circle's outline or its disc.
 * @param call   The public call's name, for the error text.
 * @param c      The canvas.
 * @param x      Centre column.
 * @param y      Centre row.
 * @param r      Radius in pixels.
 * @param color  The colour.
 * @param filled true for the disc.
 * @return true when drawn; false with the error set.
 */
static bool circle_draw(const char *call, kode_canvas_t c, int32_t x, int32_t y, int32_t r,
                        kode_color_t color, bool filled)
{
    const shape_t s  = { x, y, r, 0, kode_raster_rgb565(color), filled, NULL };
    bool          ok = kode_enter(call) && shape_in_range(&s);

    if ( ok && (0 > r) )
    {
        kode_err_set("a radius is negative");
        ok = false;
    }
    if ( ok )
    {
        ok = draw_run(c, &shape_circle, &s);
    }
    return ok;
}

bool kode_draw_rect(kode_canvas_t c, int32_t x, int32_t y, int32_t w, int32_t h,
                    kode_color_t color)
{
    return rect_draw("kode_draw_rect", c, x, y, w, h, color, false);
}

bool kode_fill_rect(kode_canvas_t c, int32_t x, int32_t y, int32_t w, int32_t h,
                    kode_color_t color)
{
    return rect_draw("kode_fill_rect", c, x, y, w, h, color, true);
}

bool kode_draw_circle(kode_canvas_t c, int32_t x, int32_t y, int32_t r, kode_color_t color)
{
    return circle_draw("kode_draw_circle", c, x, y, r, color, false);
}

bool kode_fill_circle(kode_canvas_t c, int32_t x, int32_t y, int32_t r, kode_color_t color)
{
    return circle_draw("kode_fill_circle", c, x, y, r, color, true);
}

/** @brief A picture, blended with its top-left corner at (x, y). */
static void shape_png(const kode_raster_t *r, const shape_t *s)
{
    kode_raster_blend_rgba(r, s->x, s->y, s->png->rgba, s->png->w, s->png->h);
}

bool kode_draw_png(kode_canvas_t c, int32_t x, int32_t y, const char *path)
{
    kode_png_t    png = { 0U, 0U, NULL };
    bool          ok = kode_enter("kode_draw_png");

    if ( ok && (NULL == path) )
    {
        kode_err_set("path is NULL");
        ok = false;
    }
    ok = ok && kode_draw_coords_ok(x, y);
    /* A canvas that is gone is found before a file is read for it. */
    ok = ok && canvas_check(c);
    /* The file is read and decoded outside the lock; only the blend is inside. */
    ok = ok && kode_png_load("kode_draw_png", path, &png);
    if ( ok )
    {
        const shape_t s = { x, y, 0, 0, 0U, true, &png };

        ok = draw_run(c, &shape_png, &s);
    }
    kode_png_free(&png);
    return ok;
}
