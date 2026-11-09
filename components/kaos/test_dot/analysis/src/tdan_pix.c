/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file tdan_pix.c
 * @brief Dead and stuck-on pixels by local contrast on the screen grid, dead lines, zone cells.
 */
#include <math.h>
#include <string.h>

#include "tdan.h"
#include "tdan_priv.h"

#define NY               TDAN_PIX_ROWS   /**< Rows of the screen grid. */
#define PWC              TDAN_PW         /**< Columns of the screen grid. */
#define SAT_LEVEL        250.0f    /**< A screen pixel at or over this is saturated. */
#define HIST_BINS        256       /**< Grey levels of a column's histogram. */
#define EDGE_REF_SKIP    6         /**< Columns past the unjudged ones before the edge reference starts. */
#define EDGE_REF_COLS    10        /**< Columns whose median is the edge reference. */
#define EDGE_REF_FAR     16        /**< The right reference starts this far from the unjudged columns. */
#define EDGE_FALLOFF     0.93f     /**< An edge column under this share of the reference is the glass's fall-off. */
#define RING_MIN         6         /**< A ring median needs this many valid cells. */
#define RING_CELLS       16        /**< Cells of the ring at Chebyshev distance 2. */
#define RING_R           2         /**< The ring's distance. */
#define RING_SIDE_STEP   4         /**< The ring's middle rows take only their two ends. */
#define BLOCK_PX         32        /**< Side of a noise block, cells. */
#define BY               8         /**< Noise blocks down the band, at most. */
#define BX               13        /**< Noise blocks across, at most. */
#define BLOCKS_X         ((PWC - (2 * TDAN_MX)) / BLOCK_PX)   /**< Noise blocks across the judged columns. */
#define BLOCKS_Y         ((TDAN_B1 - TDAN_B0) / BLOCK_PX)     /**< Noise blocks down the band. */
_Static_assert((1 <= BLOCKS_X) && (BX >= BLOCKS_X), "the noise blocks across fit the table");
_Static_assert((1 <= BLOCKS_Y) && (BY >= BLOCKS_Y), "the noise blocks down fit the table");
#define BLOCK_BINS       512       /**< Bins of a block's value histogram. */
#define BLOCK_DEV_BINS   256       /**< Bins of a block's deviation histogram. */
#define BLOCK_MIN_N      64        /**< A block with fewer valid cells keeps the floor. */
#define MAD_TO_SIGMA     1.4826f   /**< Robust sigma per median absolute deviation. */
#define REL_RANGE        0.25f     /**< The relative contrast's histogram range. */
#define REL_FLOOR        0.006f    /**< The relative contrast's noise floor. */
#define ABS_RANGE        16.0f     /**< The absolute contrast's histogram range (black photo). */
#define ABS_FLOOR        0.5f      /**< The absolute contrast's noise floor. */
#define CAND_MARGIN      2         /**< Dead candidates are searched this far inside the judged columns. */
#define ZONE_R           3         /**< Around a candidate, cells darker than ZONE_DARK are counted this far. */
#define ZONE_DARK        (-0.05f)  /**< A darker cell around a candidate. */
#define ZONE_EDGE        12        /**< Beside the glass edge a region is not trusted as a zone. */
#define REF_FLOOR        1.0f      /**< References under this level are taken as this. */
#define STUCK_MARK_PX    16        /**< Maxima this near a mark's centre are the mark. */
#define STUCK_MERGE_PX   4         /**< Weak stuck maxima this near are one pixel. */
#define LINE_WIN         24        /**< Cells averaged along a line. */
#define LINE_MX          3         /**< Columns beyond the unjudged ones not searched for lines. */
#define LINE_EDGE        3         /**< Rows from the band's ends not searched for lines. */
/* The column search's window fits the grid and its first centre is past the edge rows, so neither needs a test. */
_Static_assert(LINE_WIN <= (NY - (2 * TDAN_PIX_EXT)), "a line window fits the band");
_Static_assert((LINE_WIN / 2) >= LINE_EDGE, "a line window's first centre is past the edge rows");
#define LINE_EXTENT      (-0.05f)  /**< A line's extent: its cells under this. */
#define WL_WIN           48        /**< Wide lines: rows (columns) averaged per window. */
#define WL_STEP          8         /**< Wide lines: the windows' step. */
#define WL_REF_NEAR      8         /**< Wide lines: the reference starts this far to each side ... */
#define WL_REF           14        /**< ... and ends this far. */
#define WL_REF_N         14.0f     /**< Lines in the reference. */
#define WL_COL_X0        40        /**< Wide columns: first centre judged (the glass edge is not). */
#define WL_COL_X1        370       /**< Wide columns: centre that ends the search. */
#define WL_ROW_X0        24        /**< Wide rows: first window column, and the margin at the right. */
#define WL_ROW_EDGE      16        /**< Wide rows: centres this far inside the band. */
#define CELL_SAMPLES     4.0f      /**< Bilinear samples per screen pixel. */
#define CELL_NEAR        0.25f     /**< A sample's offset inside its pixel ... */
#define CELL_FAR         0.75f     /**< ... and the other one. */
#define PERCENT          100.0f    /**< Cells are percent of the median. */
#define CELL_MAX         255.0f    /**< A cell's top. */
#define MEDIAN_MIN       1e-3f     /**< A median under this is taken as 1. */
#define WARP_SIGMA       6.0f      /**< Blur of the warped band for the golden cells, grid px. */
#define WARP_STEP        2         /**< The golden cells take every second grid px. */
#define BLUR_PASSES      3         /**< Box blurs that approximate a Gaussian. */
#define BLUR_MIN_SIGMA   0.3f      /**< Below this the blur is a copy. */

/* ---- The warped band (the golden cells) ---- */

/**
 * @brief Horizontal box blur of radius @p r, edges clamped.
 * @param in  The grid.
 * @param out The result.
 * @param w   Width.
 * @param h   Height.
 * @param r   Radius.
 */
static void box_h(const float *in, float *out, int32_t w, int32_t h, int32_t r)
{
    const float inv = 1.0f / (float)((2 * r) + 1);
    for ( int32_t y = 0; y < h; y++ )
    {
        tdan_yield_point((uint32_t)y, TDAN_YIELD_EVERY_16);
        const float *s = &in[(size_t)y * (size_t)w];
        float *o = &out[(size_t)y * (size_t)w];
        float acc = s[0] * (float)(r + 1);
        for ( int32_t x = 1; x <= r; x++ )
        {
            int32_t xs = w - 1;
            if ( x < w )
            {
                xs = x;
            }
            acc += s[xs];
        }
        for ( int32_t x = 0; x < w; x++ )
        {
            o[x] = acc * inv;
            int32_t add = x + r + 1;
            int32_t sub = x - r;
            if ( add >= w )
            {
                add = w - 1;
            }
            if ( 0 >= sub )
            {
                sub = 0;
            }
            acc += s[add] - s[sub];
        }
    }
}

/**
 * @brief Vertical box blur of radius @p r, row by row (walking down a column misses the cache at every step).
 * @param in  The grid.
 * @param out The result.
 * @param acc One row of scratch.
 * @param w   Width.
 * @param h   Height.
 * @param r   Radius.
 */
static void box_v(const float *in, float *out, float *acc, int32_t w, int32_t h, int32_t r)
{
    const float inv = 1.0f / (float)((2 * r) + 1);
    for ( int32_t x = 0; x < w; x++ )
    {
        acc[x] = in[x] * (float)(r + 1);
    }
    for ( int32_t y = 1; y <= r; y++ )
    {
        int32_t ys = h - 1;
        if ( y < h )
        {
            ys = y;
        }
        const float *s = &in[(size_t)ys * (size_t)w];
        for ( int32_t x = 0; x < w; x++ )
        {
            acc[x] += s[x];
        }
    }
    for ( int32_t y = 0; y < h; y++ )
    {
        tdan_yield_point((uint32_t)y, TDAN_YIELD_EVERY_16);
        float *o = &out[(size_t)y * (size_t)w];
        for ( int32_t x = 0; x < w; x++ )
        {
            o[x] = acc[x] * inv;
        }
        int32_t add = y + r + 1;
        int32_t sub = y - r;
        if ( add >= h )
        {
            add = h - 1;
        }
        if ( 0 >= sub )
        {
            sub = 0;
        }
        const float *sa = &in[(size_t)add * (size_t)w];
        const float *ss = &in[(size_t)sub * (size_t)w];
        for ( int32_t x = 0; x < w; x++ )
        {
            acc[x] += sa[x] - ss[x];
        }
    }
}

/**
 * @brief Three box blurs whose widths approximate a Gaussian of @p sigma (Kovesi).
 * @param in    The grid.
 * @param out   The result.
 * @param tmp   A grid of scratch.
 * @param acc   A row of scratch.
 * @param w     Width.
 * @param h     Height.
 * @param sigma The Gaussian's sigma, grid px.
 */
static void blur(const float *in, float *out, float *tmp, float *acc, int32_t w, int32_t h, float sigma)
{
    if ( BLUR_MIN_SIGMA > sigma )
    {
        if ( out != in )
        {
            (void)memcpy(out, in, (size_t)w * (size_t)h * sizeof(float));
        }
        return;
    }
    const int32_t n = BLUR_PASSES;
    const float wi = sqrtf(((12.0f * sigma * sigma) / (float)n) + 1.0f);
    int32_t wl = (int32_t)floorf(wi);
    if ( 0 == (wl & 1) )
    {
        wl--;
    }
    const int32_t wu = wl + 2;
    const float m = roundf(((((12.0f * sigma * sigma) - (float)(n * wl * wl)) - ((4.0f * (float)n) * (float)wl))
                            - (3.0f * (float)n))
                           / ((-4.0f * (float)wl) - 4.0f));
    const float *src = in;
    for ( int32_t i = 0; i < n; i++ )
    {
        int32_t width = wu;
        if ( (int32_t)m > i )
        {
            width = wl;
        }
        const int32_t r = width / 2;
        box_h(src, tmp, w, h, r);
        box_v(tmp, out, acc, w, h, r);
        src = out;
    }
}

/**
 * @brief Warp the photo into the band grid (once per photo, into plane a) and blur it into @p dst.
 * @param luma  The photo.
 * @param w     Width.
 * @param h     Height.
 * @param hm    The homography.
 * @param wk    The work memory: a the warp, b and d scratch.
 * @param sigma The blur, grid px.
 * @param dst   The result (plane c).
 */
static void warp_blur(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk, float sigma,
                      float *dst)
{
    float *a = wk->a;
    if ( !wk->warped )
    {
        const float k = 1.0f / (float)TDAN_S;
        for ( int32_t v = 0; TDAN_WH > v; v++ )
        {
            tdan_yield_point((uint32_t)v, TDAN_YIELD_EVERY_8);
            const double py = (double)TDAN_B0 + (((double)v + 0.5) / (double)TDAN_S);
            const float x0 = 0.5f * k;
            /* Numerators and denominator along the row: value = base + u * step. The x = 0 term stays so the
             * rounding is the measured one. */
            const float bu = (float)((hm->h[0] * 0.0) + (hm->h[1] * py) + hm->h[2]);
            const float bv = (float)((hm->h[4] * py) + hm->h[5]);
            const float bd = (float)((hm->h[7] * py) + 1.0);
            const float su = (float)hm->h[0] * k;
            const float sv = (float)hm->h[3] * k;
            const float sd = (float)hm->h[6] * k;
            const float cu = bu + ((float)hm->h[0] * x0);
            const float cv = bv + ((float)hm->h[3] * x0);
            const float cd = bd + ((float)hm->h[6] * x0);
            for ( int32_t u = 0; TDAN_WW > u; u++ )
            {
                const float d = cd + ((float)u * sd);
                const float inv = 1.0f / d;
                a[((size_t)v * (size_t)TDAN_WW) + (size_t)u] =
                    tdan_bilinear(luma, w, h, (cu + ((float)u * su)) * inv, (cv + ((float)u * sv)) * inv);
            }
        }
        wk->warped = true;
    }
    if ( 0.0f < sigma )
    {
        blur(a, dst, wk->b, wk->d, TDAN_WW, TDAN_WH, sigma);
    }
    else
    {
        (void)memcpy(dst, a, TDAN_PLANE_FLOATS * sizeof(float));
    }
}

/**
 * @brief Sort a few floats, ascending (insertion).
 * @param f The values.
 * @param n How many.
 */
static void sort_floats(float *f, int32_t n)
{
    for ( int32_t i = 1; i < n; i++ )
    {
        const float k = f[i];
        int32_t j = i - 1;
        while ( (0 <= j) && (f[j] > k) )
        {
            f[j + 1] = f[j];
            j--;
        }
        f[j + 1] = k;
    }
}

/**
 * @brief Cells as percent of the median of the inner cells.
 * @param m     The cell means, TDAN_ZR rows of TDAN_ZC.
 * @param flat  Room for the inner cells.
 * @param nflat How many inner cells.
 * @param cells The cells out.
 */
static void cells_over_median(const float *m, float *flat, int32_t nflat, uint8_t cells[TDAN_ZR][TDAN_ZC])
{
    sort_floats(flat, nflat);
    float med = 1.0f;
    if ( MEDIAN_MIN < flat[nflat / 2] )
    {
        med = flat[nflat / 2];
    }
    for ( int32_t cy = 0; TDAN_ZR > cy; cy++ )
    {
        for ( int32_t cx = 0; TDAN_ZC > cx; cx++ )
        {
            float v = (PERCENT * m[(cy * TDAN_ZC) + cx]) / med;
            if ( CELL_MAX < v )
            {
                v = CELL_MAX;
            }
            cells[cy][cx] = (uint8_t)(v + 0.5f);
        }
    }
}

/**
 * @brief Is (cx, cy) an inner cell (not on the grid's border).
 * @param cx Cell column.
 * @param cy Cell row.
 * @return true for an inner cell.
 */
static bool inner_cell(int32_t cx, int32_t cy)
{
    return (1 <= cx) && ((TDAN_ZC - 1) > cx) && (1 <= cy) && ((TDAN_ZR - 1) > cy);
}

void tdan_zone_cells(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                     uint8_t cells[TDAN_ZR][TDAN_ZC])
{
    float *g = wk->c;
    warp_blur(luma, w, h, hm, wk, WARP_SIGMA, g);
    /* Plane d is free once the blur is done. */
    float (*m)[TDAN_ZC] = (float (*)[TDAN_ZC])(void *)wk->d;
    float *flat = &wk->d[TDAN_ZR * TDAN_ZC];
    int32_t nflat = 0;
    for ( int32_t cy = 0; TDAN_ZR > cy; cy++ )
    {
        for ( int32_t cx = 0; TDAN_ZC > cx; cx++ )
        {
            double acc = 0.0;
            int32_t n = 0;
            for ( int32_t v = cy * TDAN_ZONE_PX * TDAN_S; ((cy + 1) * TDAN_ZONE_PX * TDAN_S) > v; v += WARP_STEP )
            {
                for ( int32_t u = cx * TDAN_ZONE_PX * TDAN_S;
                      (((cx + 1) * TDAN_ZONE_PX * TDAN_S) > u) && (TDAN_WW > u); u += WARP_STEP )
                {
                    acc += (double)g[((size_t)v * (size_t)TDAN_WW) + (size_t)u];
                    n++;
                }
            }
            m[cy][cx] = 0.0f;
            if ( 0 != n )
            {
                m[cy][cx] = (float)(acc / (double)n);
            }
            if ( inner_cell(cx, cy) )
            {
                flat[nflat] = m[cy][cx];
                nflat++;
            }
        }
    }
    cells_over_median(&m[0][0], flat, nflat, cells);
}

/* ---- The screen grid ---- */

/**
 * @brief P: the value of each screen pixel, the mean of 4 bilinear samples inside its cell.
 * @param luma The photo.
 * @param w    Width.
 * @param h    Height.
 * @param hm   The homography.
 * @param p    The grid out, NY x PWC.
 */
static void screen_cells(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, float *p)
{
    static const float OFF[2] = { CELL_NEAR, CELL_FAR };
    const float h0 = (float)hm->h[0];
    const float h1 = (float)hm->h[1];
    const float h2 = (float)hm->h[2];
    const float h3 = (float)hm->h[3];
    const float h4 = (float)hm->h[4];
    const float h5 = (float)hm->h[5];
    const float h6 = (float)hm->h[6];
    const float h7 = (float)hm->h[7];
    for ( int32_t cy = 0; NY > cy; cy++ )
    {
        tdan_yield_point((uint32_t)cy, TDAN_YIELD_EVERY_8);
        const float y0 = (float)((TDAN_B0 - TDAN_PIX_EXT) + cy);
        float bu[2];
        float bv[2];
        float bd[2];
        for ( int32_t b = 0; 2 > b; b++ )
        {
            const float yy = y0 + OFF[b];
            bu[b] = (h1 * yy) + h2;
            bv[b] = (h4 * yy) + h5;
            bd[b] = (h7 * yy) + 1.0f;
        }
        float *row = &p[(size_t)cy * (size_t)PWC];
        for ( int32_t cx = 0; PWC > cx; cx++ )
        {
            float acc = 0.0f;
            for ( int32_t b = 0; 2 > b; b++ )
            {
                for ( int32_t a = 0; 2 > a; a++ )
                {
                    const float x = (float)cx + OFF[a];
                    const float d = bd[b] + (h6 * x);
                    const float inv = 1.0f / d;
                    acc += tdan_bilinear(luma, w, h, (bu[b] + (h0 * x)) * inv, (bv[b] + (h3 * x)) * inv);
                }
            }
            row[cx] = acc * (1.0f / CELL_SAMPLES);
        }
    }
}

/**
 * @brief Mean of the two middle values of EDGE_REF_COLS sorted column medians.
 * @param col   The column medians.
 * @param first The first column taken.
 * @return The reference.
 */
static float edge_reference(const float *col, int32_t first)
{
    float t[EDGE_REF_COLS];
    for ( int32_t i = 0; EDGE_REF_COLS > i; i++ )
    {
        t[i] = col[first + i];
    }
    sort_floats(t, EDGE_REF_COLS);
    return 0.5f * (t[(EDGE_REF_COLS / 2) - 1] + t[EDGE_REF_COLS / 2]);
}

/**
 * @brief The columns that are not the glass's edge fall-off: valid references.
 * @param p   The screen grid.
 * @param ok  One flag per column out.
 * @param col PWC floats of scratch.
 */
static void valid_cols(const float *p, uint8_t *ok, float *col)
{
    for ( int32_t x = 0; PWC > x; x++ )
    {
        uint16_t hist[HIST_BINS];
        (void)memset(hist, 0, sizeof(hist));
        int32_t n = 0;
        for ( int32_t y = TDAN_PIX_EXT; (NY - TDAN_PIX_EXT) > y; y++ )
        {
            int32_t v = (int32_t)p[((size_t)y * (size_t)PWC) + (size_t)x];
            if ( 0 > v )
            {
                v = 0;
            }
            if ( (HIST_BINS - 1) < v )
            {
                v = HIST_BINS - 1;
            }
            hist[v]++;
            n++;
        }
        int32_t acc = 0;
        int32_t m = 0;
        for ( ; HIST_BINS > m; m++ )
        {
            acc += (int32_t)hist[m];
            if ( (acc * 2) >= n )
            {
                break;
            }
        }
        col[x] = (float)m;
    }
    const float l = edge_reference(col, TDAN_MX + EDGE_REF_SKIP);
    const float r = edge_reference(col, (PWC - TDAN_MX) - EDGE_REF_FAR);
    for ( int32_t x = 0; PWC > x; x++ )
    {
        uint8_t o = 1U;
        if ( ((TDAN_MX + EDGE_REF_SKIP) > x) && ((EDGE_FALLOFF * l) > col[x]) )
        {
            o = 0U;
        }
        if ( (((PWC - TDAN_MX) - EDGE_REF_SKIP) <= x) && ((EDGE_FALLOFF * r) > col[x]) )
        {
            o = 0U;
        }
        ok[x] = o;
    }
}

/**
 * @brief Median of the 16-cell ring at Chebyshev distance 2 (valid columns only).
 * @param p  The screen grid.
 * @param ok The valid columns.
 * @param x  Column.
 * @param y  Row.
 * @return The median; NAN with fewer than RING_MIN cells.
 */
static float ring_med(const float *p, const uint8_t *ok, int32_t x, int32_t y)
{
    float v[RING_CELLS];
    int32_t n = 0;
    for ( int32_t dy = -RING_R; RING_R >= dy; dy++ )
    {
        const int32_t yy = y + dy;
        if ( (0 > yy) || (NY <= yy) )
        {
            continue;
        }
        int32_t step = RING_SIDE_STEP;
        if ( (-RING_R == dy) || (RING_R == dy) )
        {
            step = 1;
        }
        for ( int32_t dx = -RING_R; RING_R >= dx; dx += step )
        {
            const int32_t xx = x + dx;
            if ( (0 > xx) || (PWC <= xx) || (0U == ok[xx]) )
            {
                continue;
            }
            const float val = p[((size_t)yy * (size_t)PWC) + (size_t)xx];
            int32_t j = n - 1;
            while ( (0 <= j) && (v[j] > val) )
            {
                v[j + 1] = v[j];
                j--;
            }
            v[j + 1] = val;
            n++;
        }
    }
    if ( RING_MIN > n )
    {
        return NAN;
    }
    if ( 0 != (n & 1) )
    {
        return v[n / 2];
    }
    return 0.5f * (v[(n / 2) - 1] + v[n / 2]);
}

/**
 * @brief The contrast plane: P / R - 1 (relative) or P - R (absolute), R the ring median.
 * @param p   The screen grid.
 * @param ok  The valid columns.
 * @param c   The plane out.
 * @param rel Relative.
 */
static void contrast(const float *p, const uint8_t *ok, float *c, bool rel)
{
    for ( int32_t y = 0; NY > y; y++ )
    {
        tdan_yield_point((uint32_t)y, TDAN_YIELD_EVERY_8);
        for ( int32_t x = 0; PWC > x; x++ )
        {
            const size_t i = ((size_t)y * (size_t)PWC) + (size_t)x;
            const float r = ring_med(p, ok, x, y);
            if ( 0 != isnan(r) )
            {
                c[i] = NAN;
            }
            else if ( rel )
            {
                c[i] = (p[i] / fmaxf(r, REF_FLOOR)) - 1.0f;
            }
            else
            {
                c[i] = p[i] - r;
            }
        }
    }
}

/**
 * @brief Robust sigma (1.4826 MAD) of the contrast per block of 32 x 32 cells over the judged zone.
 * @param c      The contrast plane.
 * @param range  The histogram's half range.
 * @param floor_ The noise floor.
 * @param sig    Each block's sigma out.
 * @param nby    Blocks down out.
 * @param nbx    Blocks across out.
 * @return The largest.
 */
static float block_sigma(const float *c, float range, float floor_, float sig[BY][BX], int32_t *nby, int32_t *nbx)
{
    const int32_t bx = BLOCKS_X;
    const int32_t by = BLOCKS_Y;
    *nbx = bx;
    *nby = by;
    float mx = 0.0f;
    for ( int32_t j = 0; j < by; j++ )
    {
        for ( int32_t i = 0; i < bx; i++ )
        {
            const int32_t x0 = TDAN_MX + (i * BLOCK_PX);
            const int32_t y0 = TDAN_PIX_EXT + (j * BLOCK_PX);
            int32_t x1 = x0 + BLOCK_PX;
            int32_t y1 = y0 + BLOCK_PX;
            if ( (bx - 1) == i )
            {
                x1 = PWC - TDAN_MX;
            }
            if ( (by - 1) == j )
            {
                y1 = TDAN_PIX_EXT + (TDAN_B1 - TDAN_B0);
            }
            uint16_t hist[BLOCK_BINS];
            (void)memset(hist, 0, sizeof(hist));
            int32_t n = 0;
            const float k = (float)BLOCK_BINS / (2.0f * range);
            for ( int32_t y = y0; y < y1; y++ )
            {
                for ( int32_t x = x0; x < x1; x++ )
                {
                    const float v = c[((size_t)y * (size_t)PWC) + (size_t)x];
                    if ( 0 != isnan(v) )
                    {
                        continue;
                    }
                    int32_t b = (int32_t)((v + range) * k);
                    if ( 0 > b )
                    {
                        b = 0;
                    }
                    if ( (BLOCK_BINS - 1) < b )
                    {
                        b = BLOCK_BINS - 1;
                    }
                    hist[b]++;
                    n++;
                }
            }
            float s = floor_;
            if ( BLOCK_MIN_N <= n )
            {
                int32_t acc = 0;
                int32_t m = 0;
                for ( ; BLOCK_BINS > m; m++ )
                {
                    acc += (int32_t)hist[m];
                    if ( (acc * 2) >= n )
                    {
                        break;
                    }
                }
                const float med = (((float)m + 0.5f) / k) - range;
                uint16_t h2[BLOCK_DEV_BINS];
                (void)memset(h2, 0, sizeof(h2));
                const float k2 = (float)BLOCK_DEV_BINS / range;
                for ( int32_t y = y0; y < y1; y++ )
                {
                    for ( int32_t x = x0; x < x1; x++ )
                    {
                        const float v = c[((size_t)y * (size_t)PWC) + (size_t)x];
                        if ( 0 != isnan(v) )
                        {
                            continue;
                        }
                        int32_t b = (int32_t)(fabsf(v - med) * k2);
                        if ( (BLOCK_DEV_BINS - 1) < b )
                        {
                            b = BLOCK_DEV_BINS - 1;
                        }
                        h2[b]++;
                    }
                }
                acc = 0;
                int32_t d = 0;
                for ( ; BLOCK_DEV_BINS > d; d++ )
                {
                    acc += (int32_t)h2[d];
                    if ( (acc * 2) >= n )
                    {
                        break;
                    }
                }
                s = (MAD_TO_SIGMA * ((float)d + 0.5f)) / k2;
                if ( s < floor_ )
                {
                    s = floor_;
                }
            }
            sig[j][i] = s;
            if ( s > mx )
            {
                mx = s;
            }
        }
    }
    return mx;
}

/**
 * @brief Deficit: minus the sum over 3x3 of min(c, 0), then its max over 3x3 (the pixel can sit one cell off).
 * @param c The contrast plane.
 * @param d The deficit out.
 * @param t A plane of scratch.
 */
static void deficit3(const float *c, float *d, float *t)
{
    for ( int32_t y = 0; NY > y; y++ )
    {
        tdan_yield_point((uint32_t)y, TDAN_YIELD_EVERY_16);
        for ( int32_t x = 0; PWC > x; x++ )
        {
            float s = 0.0f;
            for ( int32_t dy = -1; 1 >= dy; dy++ )
            {
                const int32_t yy = y + dy;
                if ( (0 > yy) || (NY <= yy) )
                {
                    continue;
                }
                for ( int32_t dx = -1; 1 >= dx; dx++ )
                {
                    const int32_t xx = x + dx;
                    if ( (0 > xx) || (PWC <= xx) )
                    {
                        continue;
                    }
                    const float v = c[((size_t)yy * (size_t)PWC) + (size_t)xx];
                    if ( (0 == isnan(v)) && (0.0f > v) )
                    {
                        s += v;
                    }
                }
            }
            t[((size_t)y * (size_t)PWC) + (size_t)x] = -s;
        }
    }
    /* Max over 3x3, separable. */
    for ( int32_t y = 0; NY > y; y++ )
    {
        for ( int32_t x = 0; PWC > x; x++ )
        {
            const size_t i = ((size_t)y * (size_t)PWC) + (size_t)x;
            float m = t[i];
            if ( (0 < x) && (t[i - 1U] > m) )
            {
                m = t[i - 1U];
            }
            if ( ((PWC - 1) > x) && (t[i + 1U] > m) )
            {
                m = t[i + 1U];
            }
            d[i] = m;
        }
    }
    for ( int32_t y = 0; NY > y; y++ )
    {
        for ( int32_t x = 0; PWC > x; x++ )
        {
            const size_t i = ((size_t)y * (size_t)PWC) + (size_t)x;
            float m = d[i];
            if ( (0 < y) && (d[i - (size_t)PWC] > m) )
            {
                m = d[i - (size_t)PWC];
            }
            if ( ((NY - 1) > y) && (d[i + (size_t)PWC] > m) )
            {
                m = d[i + (size_t)PWC];
            }
            t[i] = m;
        }
    }
    (void)memcpy(d, t, (size_t)NY * (size_t)PWC * sizeof(float));
}

/**
 * @brief Is (x, y) a local extreme of its 3x3 (NAN neighbours ignored).
 * @param c   The plane.
 * @param x   Column.
 * @param y   Row.
 * @param max true: a maximum; false: a minimum.
 * @return true for an extreme.
 */
static bool is_extreme3(const float *c, int32_t x, int32_t y, bool max)
{
    const float v = c[((size_t)y * (size_t)PWC) + (size_t)x];
    if ( 0 != isnan(v) )
    {
        return false;
    }
    for ( int32_t dy = -1; 1 >= dy; dy++ )
    {
        for ( int32_t dx = -1; 1 >= dx; dx++ )
        {
            const int32_t xx = x + dx;
            const int32_t yy = y + dy;
            if ( ((0 == dx) && (0 == dy)) || (0 > xx) || (0 > yy) || (PWC <= xx) || (NY <= yy) )
            {
                continue;
            }
            const float u = c[((size_t)yy * (size_t)PWC) + (size_t)xx];
            if ( 0 != isnan(u) )
            {
                continue;
            }
            if ( max && (u > v) )
            {
                return false;
            }
            if ( !max && (u < v) )
            {
                return false;
            }
        }
    }
    return true;
}

/**
 * @brief Keep a dead candidate of the white photo.
 * @param st The pixel state.
 * @param c  The contrast plane.
 * @param d  The deficit plane.
 * @param x  Column.
 * @param y  Grid row.
 */
static void keep_candidate(tdan_pix_t *st, const float *c, const float *d, int32_t x, int32_t y)
{
    /* Cells darker than 5 % around: a bigger region is a small zone, not a pixel. */
    int32_t zc = 0;
    for ( int32_t dy = -ZONE_R; ZONE_R >= dy; dy++ )
    {
        for ( int32_t dx = -ZONE_R; ZONE_R >= dx; dx++ )
        {
            const int32_t yy = y + dy;
            const int32_t xx = x + dx;
            if ( (0 <= yy) && (NY > yy) && (0 <= xx) && (PWC > xx) )
            {
                const float u = c[((size_t)yy * (size_t)PWC) + (size_t)xx];
                if ( (0 == isnan(u)) && (ZONE_DARK > u) )
                {
                    zc++;
                }
            }
        }
    }
    tdan_cand_t *q = &st->cand[st->ncand];
    q->x = (int16_t)x;
    q->y = (int16_t)((y - TDAN_PIX_EXT) + TDAN_B0);
    q->dw = d[((size_t)y * (size_t)PWC) + (size_t)x];
    /* Beside the glass edge a region is not trusted as a zone. */
    q->zone = zc;
    if ( ((TDAN_MX + ZONE_EDGE) > x) || (((PWC - TDAN_MX) - ZONE_EDGE) <= x) )
    {
        q->zone = 0;
    }
    st->ncand++;
}

int32_t tdan_pix_photo(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                       int32_t grey, tdan_pix_t *st)
{
    float *p = wk->c;
    float *c = wk->d;
    float *d = wk->b;
    float *t = wk->a;
    /* Plane a is scratch here: the warp, if any, is gone. */
    wk->warped = false;
    if ( 0 == grey )
    {
        (void)memset(st, 0, sizeof(*st));
    }
    screen_cells(luma, w, h, hm, p);
    int32_t sat = 0;
    int32_t nj = 0;
    for ( int32_t y = TDAN_PIX_EXT; (NY - TDAN_PIX_EXT) > y; y++ )
    {
        for ( int32_t x = TDAN_MX; (PWC - TDAN_MX) > x; x++ )
        {
            if ( SAT_LEVEL <= p[((size_t)y * (size_t)PWC) + (size_t)x] )
            {
                sat++;
            }
            nj++;
        }
    }
    st->sat[grey] = (float)sat / (float)nj;
    if ( 0 == grey )
    {
        valid_cols(p, st->okcol, t);
        st->have_ref = true;
    }
    else if ( !st->have_ref )
    {
        return TDAN_PIX_NOREF;
    }
    else
    {
        /* The grey reuses the white's glass-edge columns. */
    }
    contrast(p, st->okcol, c, true);
    {
        float sg[BY][BX];
        int32_t nby;
        int32_t nbx;
        st->sigmax[grey] = block_sigma(c, REL_RANGE, REL_FLOOR, sg, &nby, &nbx);
    }
    if ( TDAN_PIX_SAT_FRAC < st->sat[grey] )
    {
        return TDAN_PIX_SATURATED;
    }
    if ( TDAN_PIX_SIG_MAX < st->sigmax[grey] )
    {
        return TDAN_PIX_NOISY;
    }
    deficit3(c, d, t);
    if ( 0 == grey )
    {
        for ( int32_t y = TDAN_PIX_EXT; (NY - TDAN_PIX_EXT) > y; y++ )
        {
            for ( int32_t x = TDAN_MX + CAND_MARGIN; ((PWC - TDAN_MX) - CAND_MARGIN) > x; x++ )
            {
                const float v = c[((size_t)y * (size_t)PWC) + (size_t)x];
                if ( (0 != isnan(v)) || (-TDAN_PIX_T_C < v) || !is_extreme3(c, x, y, false) )
                {
                    continue;
                }
                if ( TDAN_PIX_MAXCAND > st->ncand )
                {
                    keep_candidate(st, c, d, x, y);
                }
            }
        }
    }
    else
    {
        st->ndead = 0;
        st->nwarn = 0;
        for ( int32_t i = 0; i < st->ncand; i++ )
        {
            const int32_t x = st->cand[i].x;
            const int32_t y = ((int32_t)st->cand[i].y - TDAN_B0) + TDAN_PIX_EXT;
            const float dg = d[((size_t)y * (size_t)PWC) + (size_t)x];
            const float dw = st->cand[i].dw;
            const float s = fminf(dw, dg);
            tdan_dead_t *q = NULL;
            int32_t zone = 0;
            if ( (TDAN_PIX_T_D <= s) && (TDAN_PIX_MAXDEAD > st->ndead) )
            {
                q = &st->dead[st->ndead];
                st->ndead++;
                zone = st->cand[i].zone;
            }
            else if ( (TDAN_PIX_T_WARN <= s) && (TDAN_PIX_MAXWARN > st->nwarn) )
            {
                q = &st->warn[st->nwarn];
                st->nwarn++;
            }
            else
            {
                /* Neither dead nor a weak spot. */
            }
            if ( NULL != q )
            {
                q->x = (int16_t)x;
                q->y = st->cand[i].y;
                q->dw = dw;
                q->dg = dg;
                q->zone = zone;
            }
        }
    }
    return TDAN_PIX_OK;
}

int32_t tdan_pix_stuck(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                       const tdan_pix_t *st, tdan_pt_t *out, int32_t maxn)
{
    float *p = wk->c;
    float *d = wk->d;
    wk->warped = false;
    if ( !st->have_ref )
    {
        return 0;
    }
    screen_cells(luma, w, h, hm, p);
    contrast(p, st->okcol, d, false);
    float sg[BY][BX];
    int32_t nby;
    int32_t nbx;
    (void)block_sigma(d, ABS_RANGE, ABS_FLOOR, sg, &nby, &nbx);
    double mc[TDAN_MARK_COORDS];
    tdan_mark_centres(mc);
    int32_t n = 0;
    for ( int32_t y = TDAN_PIX_EXT; (NY - TDAN_PIX_EXT) > y; y++ )
    {
        for ( int32_t x = TDAN_MX; (PWC - TDAN_MX) > x; x++ )
        {
            const float v = d[((size_t)y * (size_t)PWC) + (size_t)x];
            if ( (0 != isnan(v)) || (TDAN_PIX_ST_ABS > v) )
            {
                continue;
            }
            int32_t bi = (x - TDAN_MX) / BLOCK_PX;
            int32_t bj = (y - TDAN_PIX_EXT) / BLOCK_PX;
            if ( bi >= nbx )
            {
                bi = nbx - 1;
            }
            if ( bj >= nby )
            {
                bj = nby - 1;
            }
            if ( ((TDAN_PIX_ST_K * sg[bj][bi]) > v) || !is_extreme3(d, x, y, true) )
            {
                continue;
            }
            const int32_t py = (y - TDAN_PIX_EXT) + TDAN_B0;
            bool near = false;
            for ( int32_t k = 0; TDAN_MARK_COUNT > k; k++ )
            {
                if ( (STUCK_MARK_PX >= tdan_iabs(x - (int32_t)mc[2 * k]))
                     && (STUCK_MARK_PX >= tdan_iabs(py - (int32_t)mc[(2 * k) + 1])) )
                {
                    near = true;
                }
            }
            if ( near )
            {
                continue;
            }
            if ( n < maxn )
            {
                out[n].x = x;
                out[n].y = py;
                out[n].v = v;
                n++;
            }
        }
    }
    /* One weak stuck pixel gives two maxima 2 px apart on the real Kode: of the weak maxima (the stronger one under
     * TDAN_PIX_ST_MERGE) within 4 px of each other only the strongest stays; strong ones are never merged. */
    int32_t k = 0;
    for ( int32_t i = 0; i < n; i++ )
    {
        bool weaker = false;
        for ( int32_t m = 0; (m < n) && !weaker; m++ )
        {
            if ( (m != i) && (STUCK_MERGE_PX >= tdan_iabs(out[m].x - out[i].x))
                 && (STUCK_MERGE_PX >= tdan_iabs(out[m].y - out[i].y)) && (TDAN_PIX_ST_MERGE > out[m].v)
                 && ((out[m].v > out[i].v) || (!(out[m].v < out[i].v) && !(out[m].v > out[i].v) && (m < i))) )
            {
                weaker = true;
            }
        }
        if ( !weaker )
        {
            out[k] = out[i];
            k++;
        }
    }
    return k;
}

/**
 * @brief A contrast value, NAN read as 0.
 * @param v The value.
 * @return @p v, or 0 for NAN.
 */
static float or_zero(float v)
{
    if ( 0 != isnan(v) )
    {
        return 0.0f;
    }
    return v;
}

void tdan_pix_lines(const tdan_work_t *wk, tdan_line_t *vl, tdan_line_t *hl)
{
    const float *c = wk->d;
    const int32_t win = LINE_WIN;
    const int32_t y0 = TDAN_PIX_EXT;
    const int32_t y1 = NY - TDAN_PIX_EXT;
    const int32_t x0 = TDAN_MX + LINE_MX;
    const int32_t x1 = (PWC - TDAN_MX) - LINE_MX;
    const int32_t edge = LINE_EDGE;
    /* Columns: the mean of c down `win` cells, the most negative window. */
    float best = 0.0f;
    int32_t bu = x0;
    int32_t bv = y0 + edge;
    for ( int32_t x = x0; x < x1; x++ )
    {
        tdan_yield_point((uint32_t)x, TDAN_YIELD_EVERY_32);
        float acc = 0.0f;
        for ( int32_t k = 0; k < win; k++ )
        {
            acc += or_zero(c[((size_t)(y0 + k) * (size_t)PWC) + (size_t)x]);
        }
        for ( int32_t cy = y0 + (win / 2); (cy + (win / 2)) <= y1; cy++ )
        {
            if ( (y0 + (win / 2)) < cy )
            {
                const float a = or_zero(c[((size_t)((cy + (win / 2)) - 1) * (size_t)PWC) + (size_t)x]);
                const float b = or_zero(c[((size_t)((cy - (win / 2)) - 1) * (size_t)PWC) + (size_t)x]);
                acc += a - b;
            }
            if ( (y1 - edge) > cy )
            {
                const float m = acc / (float)win;
                if ( m < best )
                {
                    best = m;
                    bu = x;
                    bv = cy;
                }
            }
        }
    }
    vl->level = best;
    vl->pos = bu;
    vl->a0 = (bv - TDAN_PIX_EXT) + TDAN_B0;
    vl->a1 = vl->a0;
    {
        int32_t lo = NY;
        int32_t hi = -1;
        for ( int32_t y = y0; y < y1; y++ )
        {
            const float v = c[((size_t)y * (size_t)PWC) + (size_t)bu];
            if ( (0 == isnan(v)) && (LINE_EXTENT > v) )
            {
                if ( y < lo )
                {
                    lo = y;
                }
                if ( y > hi )
                {
                    hi = y;
                }
            }
        }
        if ( 0 <= hi )
        {
            vl->a0 = (lo - TDAN_PIX_EXT) + TDAN_B0;
            vl->a1 = (hi - TDAN_PIX_EXT) + TDAN_B0;
        }
    }
    /* Rows. */
    best = 0.0f;
    int32_t bhv = y0 + edge;
    int32_t bhu = x0 + (win / 2);
    for ( int32_t y = y0 + edge; (y1 - edge) > y; y++ )
    {
        tdan_yield_point((uint32_t)y, TDAN_YIELD_EVERY_32);
        float acc = 0.0f;
        for ( int32_t k = 0; k < win; k++ )
        {
            acc += or_zero(c[((size_t)y * (size_t)PWC) + (size_t)(x0 + k)]);
        }
        for ( int32_t cx = x0 + (win / 2); (cx + (win / 2)) <= x1; cx++ )
        {
            if ( (x0 + (win / 2)) < cx )
            {
                const float a = or_zero(c[((size_t)y * (size_t)PWC) + (size_t)((cx + (win / 2)) - 1)]);
                const float b = or_zero(c[((size_t)y * (size_t)PWC) + (size_t)((cx - (win / 2)) - 1)]);
                acc += a - b;
            }
            const float m = acc / (float)win;
            if ( m < best )
            {
                best = m;
                bhv = y;
                bhu = cx;
            }
        }
    }
    hl->level = best;
    hl->pos = (bhv - TDAN_PIX_EXT) + TDAN_B0;
    hl->a0 = bhu;
    hl->a1 = bhu;
    {
        int32_t lo = PWC;
        int32_t hi = -1;
        for ( int32_t x = x0; x < x1; x++ )
        {
            const float v = c[((size_t)bhv * (size_t)PWC) + (size_t)x];
            if ( (0 == isnan(v)) && (LINE_EXTENT > v) )
            {
                if ( x < lo )
                {
                    lo = x;
                }
                if ( x > hi )
                {
                    hi = x;
                }
            }
        }
        if ( 0 <= hi )
        {
            hl->a0 = lo;
            hl->a1 = hi;
        }
    }
}

/**
 * @brief A wide line's level: the mean of 3 lines against the lines 8 to 14 px to each side.
 * @param m The line means.
 * @param x The centre.
 * @return mean / reference - 1.
 */
static float wl_level(const float *m, int32_t x)
{
    const float mid = (m[x - 1] + m[x] + m[x + 1]) * (1.0f / 3.0f);
    float ref = 0.0f;
    for ( int32_t i = WL_REF_NEAR; WL_REF >= i; i++ )
    {
        ref += m[x - i] + m[x + i];
    }
    ref *= 1.0f / WL_REF_N;
    return (mid / fmaxf(ref, REF_FLOOR)) - 1.0f;
}

void tdan_pix_wide_lines(tdan_work_t *wk, tdan_line_t *vl, tdan_line_t *hl)
{
    const float *p = wk->c;
    const int32_t y0 = TDAN_PIX_EXT;
    const int32_t nr = TDAN_B1 - TDAN_B0;
    /* Plane a is scratch after tdan_pix_photo. */
    float *m = wk->a;
    vl->level = 0;
    vl->pos = 0;
    vl->a0 = TDAN_B0;
    vl->a1 = TDAN_B0;
    hl->level = 0;
    hl->pos = 0;
    hl->a0 = 0;
    hl->a1 = 0;
    /* Columns: windows of 48 rows every 8. */
    for ( int32_t s = 0; (s + WL_WIN) <= nr; s += WL_STEP )
    {
        tdan_yield_point((uint32_t)(s / WL_STEP), TDAN_YIELD_EVERY_4);
        for ( int32_t x = 0; PWC > x; x++ )
        {
            m[x] = 0;
        }
        for ( int32_t r = 0; WL_WIN > r; r++ )
        {
            const float *row = &p[(size_t)(y0 + s + r) * (size_t)PWC];
            for ( int32_t x = 0; PWC > x; x++ )
            {
                m[x] += row[x];
            }
        }
        for ( int32_t x = 0; PWC > x; x++ )
        {
            m[x] *= 1.0f / (float)WL_WIN;
        }
        for ( int32_t x = WL_COL_X0; WL_COL_X1 > x; x++ )
        {
            const float lv = wl_level(m, x);
            if ( lv < vl->level )
            {
                vl->level = lv;
                vl->pos = x;
                vl->a0 = TDAN_B0 + s;
                vl->a1 = TDAN_B0 + s + WL_WIN;
            }
        }
    }
    /* Rows: windows of 48 columns every 8, centres 16 rows inside the band. */
    for ( int32_t s = WL_ROW_X0; (s + WL_WIN) <= (PWC - WL_ROW_X0); s += WL_STEP )
    {
        tdan_yield_point((uint32_t)(s / WL_STEP), TDAN_YIELD_EVERY_4);
        for ( int32_t r = 0; r < nr; r++ )
        {
            const float *row = &p[((size_t)(y0 + r) * (size_t)PWC) + (size_t)s];
            float a = 0.0f;
            for ( int32_t x = 0; WL_WIN > x; x++ )
            {
                a += row[x];
            }
            m[r] = a * (1.0f / (float)WL_WIN);
        }
        for ( int32_t r = WL_ROW_EDGE; (nr - WL_ROW_EDGE) > r; r++ )
        {
            const float lv = wl_level(m, r);
            if ( lv < hl->level )
            {
                hl->level = lv;
                hl->pos = TDAN_B0 + r;
                hl->a0 = s;
                hl->a1 = s + WL_WIN;
            }
        }
    }
}

void tdan_pix_zone_cells(tdan_work_t *wk, uint8_t cells[TDAN_ZR][TDAN_ZC])
{
    const float *p = wk->c;
    /* Plane a is scratch after tdan_pix_photo. */
    float (*m)[TDAN_ZC] = (float (*)[TDAN_ZC])(void *)wk->a;
    float *flat = &wk->a[TDAN_ZR * TDAN_ZC];
    int32_t nflat = 0;
    for ( int32_t cy = 0; TDAN_ZR > cy; cy++ )
    {
        for ( int32_t cx = 0; TDAN_ZC > cx; cx++ )
        {
            float acc = 0.0f;
            int32_t n = 0;
            for ( int32_t y = cy * TDAN_ZONE_PX; ((cy + 1) * TDAN_ZONE_PX) > y; y++ )
            {
                for ( int32_t x = cx * TDAN_ZONE_PX; (((cx + 1) * TDAN_ZONE_PX) > x) && (PWC > x); x++ )
                {
                    acc += p[((size_t)(y + TDAN_PIX_EXT) * (size_t)PWC) + (size_t)x];
                    n++;
                }
            }
            m[cy][cx] = 0.0f;
            if ( 0 != n )
            {
                m[cy][cx] = acc / (float)n;
            }
            if ( inner_cell(cx, cy) )
            {
                flat[nflat] = m[cy][cx];
                nflat++;
            }
        }
    }
    cells_over_median(&m[0][0], flat, nflat, cells);
}

void tdan_analyse_photo(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                        int32_t grey, tdan_pix_t *st, tdan_photo_t *out)
{
    uint8_t zc[TDAN_ZR][TDAN_ZC];
    out->status = tdan_pix_photo(luma, w, h, hm, wk, grey, st);
    tdan_pix_lines(wk, &out->vl, &out->hl);
    tdan_pix_wide_lines(wk, &out->wvl, &out->whl);
    tdan_pix_zone_cells(wk, zc);
    out->nzones = tdan_zones_from_cells(zc, grey, wk, out->zones, TDAN_ZONES_MAX);
    out->nbz = 0;
    if ( 0 != grey )
    {
        out->nbz = tdan_zones_bright_from_cells(zc, wk, out->bz, TDAN_ZONES_MAX);
    }
}
