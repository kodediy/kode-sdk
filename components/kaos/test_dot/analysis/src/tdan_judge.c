/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file tdan_judge.c
 * @brief Zones against the golden photos, colour, ripple, and the verdicts with their limits.
 */
#include <math.h>
#include <string.h>

#include "tdan.h"
#include "tdan_gold.h"
#include "tdan_priv.h"

#define ZONE_MIN_CELLS    4        /**< Joined cells that make a zone. */
#define ZONE_DARK_START   9.0f     /**< Larger than any dark ratio. */
#define ZONE_NEIGHBOURS   4        /**< A cell joins its four neighbours. */

#define COLOUR_INSET      20       /**< The colour's sampled band stays this far inside the judged one. */
#define COLOUR_STEP       2        /**< The colour samples every second panel px. */

#define RIPPLE_COL        205.0    /**< The ripple's column: the panel's middle. */
#define RIPPLE_ROW0       145.0    /**< The ripple's first row. */
#define RIPPLE_ROW1       360.0    /**< The ripple's last row. */
#define RIPPLE_MIN_ROWS   60       /**< Fewer photo rows: no ripple. */
#define RIPPLE_HALF_COLS  150      /**< Photo columns averaged to each side of the middle. */
#define RIPPLE_MIN_COLS   100      /**< Fewer: no ripple. */
#define RIPPLE_DET_MIN    1e-9     /**< A smaller determinant: no fit. */
#define RIPPLE_PEAK       1.41421356   /**< Peak over standard deviation of a sine. */
#define RIPPLE_MEAN_MIN   1.0      /**< A darker mean is taken as this. */

#define SMALL_ZONE_HALF   5        /**< A small zone born from a dead candidate: this many px to each side. */
#define SMALL_ZONE_CELLS  4        /**< ... counted as this many cells ... */
#define SMALL_ZONE_RATIO  0.5f     /**< ... at this ratio. */
#define LINE_SAME_PX      2        /**< White and grey see the same line within this. */
#define LINE_ZONE_PX      8        /**< A line this near a zone is on it. */
#define LINE_THIN_PX      24       /**< A zone this narrow along a line is the line; a wider one swallows it. */
#define DEAD_ZONE_PX      8        /**< A dead pixel this near a zone is the zone's. */
#define DEAD_LINE_PX      2        /**< A dead pixel this near a line ... */
#define DEAD_LINE_END_PX  3        /**< ... and within its extent plus this is the line's. */
#define CONFIRM_PX        3        /**< The confirming pass finds a defect again within this. */
#define STREAK_PX         8        /**< Stuck-on pixels this near are chained ... */
#define STREAK_LINKS      2        /**< ... with this many neighbours ... */
#define STREAK_MIN        5        /**< ... and this many or more, half of them chained, are a reflection. */

#define WHITE_DEAD_LEVEL  8.0      /**< A white channel under this did not light. */
#define WHITE_CLIP_LEVEL  250.0    /**< A white channel at this is clipped. */
#define CHROMA_EPS        1e-9     /**< A smaller sum has no chromaticity. */
#define ONE_THIRD         (1.0 / 3.0)   /**< A neutral channel's share. */
#define BAND_CORNERS      4        /**< Corners of the judged band. */

/** @brief The chromaticity of grey, red, green and blue relative to the white, measured on the fixture. */
static const double REF_CHROMA[TDAN_COLOURS][TDAN_CHANNELS] = {
    { 0.3333, 0.3333, 0.3333 },
    { 0.3333, 0.3333, 0.3333 },
    { 0.9388, 0.0120, 0.0492 },
    { 0.0000, 1.0000, 0.0000 },
    { 0.0037, 0.1078, 0.8884 },
};

/** @brief The zone search's scratch, carved from work plane a. */
typedef struct {
    uint8_t flag[TDAN_ZR][TDAN_ZC];          /**< The cell is off the golden photo. */
    uint8_t seen[TDAN_ZR][TDAN_ZC];          /**< The cell was joined already. */
    float   ratio[TDAN_ZR][TDAN_ZC];         /**< The cell over the golden one. */
    int32_t stack[TDAN_ZR * TDAN_ZC][2];     /**< Cells waiting to be joined. */
} zone_scratch_t;

_Static_assert(sizeof(zone_scratch_t) <= (TDAN_PLANE_FLOATS * sizeof(float)),
               "the zone search's scratch must fit one work plane");

/**
 * @brief Join the flagged cells into zones of ZONE_MIN_CELLS or more.
 * @param zs     The scratch, flag and ratio filled.
 * @param bright Bright patches (the highest ratio kept) or dark zones (the lowest).
 * @param out    The zones out.
 * @param maxn   Room in @p out.
 * @return How many.
 */
static int32_t join_zones(zone_scratch_t *zs, bool bright, tdan_zone_t *out, int32_t maxn)
{
    static const int32_t D[ZONE_NEIGHBOURS][2] = { { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } };
    int32_t n = 0;
    for ( int32_t cy = 0; TDAN_ZR > cy; cy++ )
    {
        for ( int32_t cx = 0; TDAN_ZC > cx; cx++ )
        {
            if ( (0U == zs->flag[cy][cx]) || (0U != zs->seen[cy][cx]) )
            {
                continue;
            }
            int32_t sp = 0;
            int32_t cnt = 0;
            int32_t x0 = cx;
            int32_t x1 = cx;
            int32_t y0 = cy;
            int32_t y1 = cy;
            float mr = ZONE_DARK_START;
            if ( bright )
            {
                mr = 0.0f;
            }
            zs->stack[sp][0] = cx;
            zs->stack[sp][1] = cy;
            sp++;
            zs->seen[cy][cx] = 1U;
            while ( 0 != sp )
            {
                sp--;
                const int32_t x = zs->stack[sp][0];
                const int32_t y = zs->stack[sp][1];
                cnt++;
                if ( x < x0 )
                {
                    x0 = x;
                }
                if ( x > x1 )
                {
                    x1 = x;
                }
                if ( y < y0 )
                {
                    y0 = y;
                }
                if ( y > y1 )
                {
                    y1 = y;
                }
                if ( (bright && (zs->ratio[y][x] > mr)) || (!bright && (zs->ratio[y][x] < mr)) )
                {
                    mr = zs->ratio[y][x];
                }
                for ( int32_t k = 0; ZONE_NEIGHBOURS > k; k++ )
                {
                    const int32_t xx = x + D[k][0];
                    const int32_t yy = y + D[k][1];
                    if ( (0 <= xx) && (0 <= yy) && (TDAN_ZC > xx) && (TDAN_ZR > yy) && (0U != zs->flag[yy][xx])
                         && (0U == zs->seen[yy][xx]) )
                    {
                        zs->seen[yy][xx] = 1U;
                        zs->stack[sp][0] = xx;
                        zs->stack[sp][1] = yy;
                        sp++;
                    }
                }
            }
            if ( (ZONE_MIN_CELLS <= cnt) && (n < maxn) )
            {
                out[n].x0 = x0 * TDAN_ZONE_PX;
                out[n].y0 = TDAN_B0 + (y0 * TDAN_ZONE_PX);
                out[n].x1 = (x1 * TDAN_ZONE_PX) + (TDAN_ZONE_PX - 1);
                out[n].y1 = TDAN_B0 + (y1 * TDAN_ZONE_PX) + (TDAN_ZONE_PX - 1);
                out[n].cells = cnt;
                out[n].min_ratio = mr;
                n++;
            }
        }
    }
    return n;
}

/**
 * @brief Fill the scratch's ratios against @p gold and flag the cells past the limit (the border columns never).
 * @param cells  The photo's cells.
 * @param gold   The golden cells.
 * @param bright Flag over TDAN_K_BRIGHT (else under TDAN_K_UNI).
 * @param zs     The scratch.
 */
static void flag_cells(const uint8_t cells[TDAN_ZR][TDAN_ZC], const uint8_t gold[TDAN_ZR][TDAN_ZC], bool bright,
                       zone_scratch_t *zs)
{
    (void)memset(zs->flag, 0, sizeof(zs->flag));
    (void)memset(zs->seen, 0, sizeof(zs->seen));
    for ( int32_t cy = 0; TDAN_ZR > cy; cy++ )
    {
        for ( int32_t cx = 0; TDAN_ZC > cx; cx++ )
        {
            zs->ratio[cy][cx] = 1.0f;
            if ( 0U != gold[cy][cx] )
            {
                zs->ratio[cy][cx] = (float)cells[cy][cx] / (float)gold[cy][cx];
            }
            const bool judged = (1 <= cx) && ((TDAN_ZC - 1) > cx);
            if ( judged && ((bright && (TDAN_K_BRIGHT < zs->ratio[cy][cx]))
                            || (!bright && (TDAN_K_UNI > zs->ratio[cy][cx]))) )
            {
                zs->flag[cy][cx] = 1U;
            }
        }
    }
}

int32_t tdan_zones_from_cells(const uint8_t cells[TDAN_ZR][TDAN_ZC], int32_t pattern, tdan_work_t *wk,
                              tdan_zone_t *out, int32_t maxn)
{
    zone_scratch_t *zs = (zone_scratch_t *)(void *)wk->a;
    if ( 0 == pattern )
    {
        flag_cells(cells, TDAN_GOLD_WHITE, false, zs);
    }
    else
    {
        flag_cells(cells, TDAN_GOLD_GREY, false, zs);
    }
    return join_zones(zs, false, out, maxn);
}

int32_t tdan_zones_bright_from_cells(const uint8_t cells[TDAN_ZR][TDAN_ZC], tdan_work_t *wk, tdan_zone_t *out,
                                     int32_t maxn)
{
    zone_scratch_t *zs = (zone_scratch_t *)(void *)wk->a;
    flag_cells(cells, TDAN_GOLD_GREY, true, zs);
    return join_zones(zs, true, out, maxn);
}

void tdan_colour(const uint16_t *rgb565, int32_t w, int32_t h, const tdan_h_t *hm, double rgb[TDAN_CHANNELS])
{
    /* Float throughout: the P4 has no double FPU, the double version took 0.1 s per photo. */
    double acc[TDAN_CHANNELS] = { 0.0, 0.0, 0.0 };
    int32_t n = 0;
    const float h0 = (float)hm->h[0];
    const float h1 = (float)hm->h[1];
    const float h2 = (float)hm->h[2];
    const float h3 = (float)hm->h[3];
    const float h4 = (float)hm->h[4];
    const float h5 = (float)hm->h[5];
    const float h6 = (float)hm->h[6];
    const float h7 = (float)hm->h[7];
    for ( int32_t y = TDAN_B0 + COLOUR_INSET; (TDAN_B1 - COLOUR_INSET) >= y; y += COLOUR_STEP )
    {
        float ra[TDAN_CHANNELS] = { 0.0f, 0.0f, 0.0f };
        tdan_yield_point((uint32_t)y, TDAN_YIELD_EVERY_16);
        for ( int32_t x = TDAN_MX + COLOUR_INSET; ((TDAN_PW - TDAN_MX) - COLOUR_INSET) >= x; x += COLOUR_STEP )
        {
            const float fx0 = (float)x + 0.5f;
            const float fy0 = (float)y + 0.5f;
            const float d = (h6 * fx0) + (h7 * fy0) + 1.0f;
            float u = ((h0 * fx0) + (h1 * fy0) + h2) / d;
            float v = ((h3 * fx0) + (h4 * fy0) + h5) / d;
            if ( 0.0f > u )
            {
                u = 0.0f;
            }
            if ( 0.0f > v )
            {
                v = 0.0f;
            }
            if ( ((float)(w - 1) - TDAN_SAMPLE_EDGE) < u )
            {
                u = (float)(w - 1) - TDAN_SAMPLE_EDGE;
            }
            if ( ((float)(h - 1) - TDAN_SAMPLE_EDGE) < v )
            {
                v = (float)(h - 1) - TDAN_SAMPLE_EDGE;
            }
            const int32_t ix = (int32_t)u;
            const int32_t iy = (int32_t)v;
            const float fx = u - (float)ix;
            const float fy = v - (float)iy;
            const size_t at = ((size_t)iy * (size_t)w) + (size_t)ix;
            const uint16_t q[TDAN_BILINEAR_TAPS] = { rgb565[at], rgb565[at + 1U], rgb565[at + (size_t)w], rgb565[at + (size_t)w + 1U] };
            const float wt[TDAN_BILINEAR_TAPS] = { (1.0f - fx) * (1.0f - fy), fx * (1.0f - fy), (1.0f - fx) * fy, fx * fy };
            for ( int32_t k = 0; TDAN_BILINEAR_TAPS > k; k++ )
            {
                uint32_t c[TDAN_CHANNELS];
                tdan_rgb565_expand(q[k], c);
                for ( int32_t ch = 0; TDAN_CHANNELS > ch; ch++ )
                {
                    ra[ch] += wt[k] * (float)c[ch];
                }
            }
            n++;
        }
        for ( int32_t k = 0; TDAN_CHANNELS > k; k++ )
        {
            acc[k] += (double)ra[k];
        }
    }
    int32_t nn = 1;
    if ( 0 != n )
    {
        nn = n;
    }
    for ( int32_t k = 0; TDAN_CHANNELS > k; k++ )
    {
        rgb[k] = acc[k] / (double)nn;
    }
}

/**
 * @brief Mean of one photo row over [c0, c1).
 * @param luma The photo.
 * @param w    Width.
 * @param row  The row.
 * @param c0   First column.
 * @param c1   Column that ends the span.
 * @return The mean.
 */
static double row_mean(const uint8_t *luma, int32_t w, int32_t row, int32_t c0, int32_t c1)
{
    double y = 0.0;
    for ( int32_t x = c0; x < c1; x++ )
    {
        y += (double)luma[((size_t)row * (size_t)w) + (size_t)x];
    }
    y /= (double)(c1 - c0);
    return y;
}

float tdan_ripple(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm)
{
    double u0;
    double v0;
    double u1;
    double v1;
    tdan_h_apply(hm, RIPPLE_COL, RIPPLE_ROW0, &u0, &v0);
    tdan_h_apply(hm, RIPPLE_COL, RIPPLE_ROW1, &u1, &v1);
    int32_t a = (int32_t)v1;
    int32_t b = (int32_t)v0;
    if ( v0 < v1 )
    {
        a = (int32_t)v0;
        b = (int32_t)v1;
    }
    if ( 0 > a )
    {
        a = 0;
    }
    if ( (h - 1) < b )
    {
        b = h - 1;
    }
    if ( RIPPLE_MIN_ROWS > (b - a) )
    {
        return 0.0f;
    }
    int32_t c0 = (int32_t)u0 - RIPPLE_HALF_COLS;
    int32_t c1 = (int32_t)u0 + RIPPLE_HALF_COLS;
    if ( 0 > c0 )
    {
        c0 = 0;
    }
    if ( c1 > w )
    {
        c1 = w;
    }
    if ( RIPPLE_MIN_COLS > (c1 - c0) )
    {
        return 0.0f;
    }
    const int32_t n = b - a;
    double s0 = 0.0;
    double s1 = 0.0;
    double s2 = 0.0;
    double s3 = 0.0;
    double s4 = 0.0;
    double t0 = 0.0;
    double t1 = 0.0;
    double t2 = 0.0;
    double mean = 0.0;
    /* The row means and the sums of the quadratic fit, t scaled to -1..1. */
    for ( int32_t i = 0; i < n; i++ )
    {
        const double y = row_mean(luma, w, a + i, c0, c1);
        const double t = ((2.0 * (double)i) / (double)(n - 1)) - 1.0;
        s0 += 1.0;
        s1 += t;
        s2 += t * t;
        s3 += t * t * t;
        s4 += t * t * t * t;
        t0 += y;
        t1 += y * t;
        t2 += y * t * t;
        mean += y;
    }
    mean /= (double)n;
    /* Solve [s0 s1 s2; s1 s2 s3; s2 s3 s4] [p0 p1 p2]^T = [t0 t1 t2]^T by Cramer. */
    const double det = ((s0 * ((s2 * s4) - (s3 * s3))) - (s1 * ((s1 * s4) - (s3 * s2)))) + (s2 * ((s1 * s3) - (s2 * s2)));
    if ( RIPPLE_DET_MIN > fabs(det) )
    {
        return 0.0f;
    }
    const double p0 = (((t0 * ((s2 * s4) - (s3 * s3))) - (s1 * ((t1 * s4) - (s3 * t2)))) + (s2 * ((t1 * s3) - (s2 * t2))))
                      / det;
    const double p1 = (((s0 * ((t1 * s4) - (s3 * t2))) - (t0 * ((s1 * s4) - (s3 * s2)))) + (s2 * ((s1 * t2) - (t1 * s2))))
                      / det;
    const double p2 = (((s0 * ((s2 * t2) - (t1 * s3))) - (s1 * ((s1 * t2) - (t1 * s2)))) + (t0 * ((s1 * s3) - (s2 * s2))))
                      / det;
    double ss = 0.0;
    for ( int32_t i = 0; i < n; i++ )
    {
        const double y = row_mean(luma, w, a + i, c0, c1);
        const double t = ((2.0 * (double)i) / (double)(n - 1)) - 1.0;
        const double r = y - (p0 + (p1 * t) + (p2 * t * t));
        ss += r * r;
    }
    double m = RIPPLE_MEAN_MIN;
    if ( RIPPLE_MEAN_MIN < mean )
    {
        m = mean;
    }
    return (float)((RIPPLE_PEAK * sqrt(ss / (double)n)) / m);
}

void tdan_judge_frame(const tdan_h_t *hm, bool located, int32_t nmarks, int32_t w, int32_t h, tdan_frame_t *out)
{
    static const double CORNER[BAND_CORNERS][2] = { { 0, TDAN_B0 }, { TDAN_PW, TDAN_B0 }, { TDAN_PW, TDAN_B1 }, { 0, TDAN_B1 } };
    tdan_pose(hm, &out->angle_deg, &out->scale, &out->persp);
    out->inside = true;
    for ( int32_t k = 0; BAND_CORNERS > k; k++ )
    {
        double u;
        double v;
        tdan_h_apply(hm, CORNER[k][0], CORNER[k][1], &u, &v);
        if ( (TDAN_INSIDE_MARGIN_PX > u) || (TDAN_INSIDE_MARGIN_PX > v) || ((double)(w - TDAN_INSIDE_MARGIN_PX) < u)
             || ((double)(h - TDAN_INSIDE_MARGIN_PX) < v) )
        {
            out->inside = false;
        }
    }
    /* A square cut by the window (or one hidden): the position is known, the verdict is not. */
    out->partial = located && (TDAN_MARK_COUNT > nmarks);
    const bool seen = located && !out->partial;
    out->framing = TDAN_NO_VERDICT;
    out->straight = TDAN_NO_VERDICT;
    if ( seen )
    {
        out->framing = TDAN_FAIL;
        if ( out->inside )
        {
            out->framing = TDAN_PASS;
        }
        /* The turn is recorded, not judged: only the perspective and a sanity limit. */
        out->straight = TDAN_FAIL;
        if ( (TDAN_MAX_ANGLE_DEG >= fabs(out->angle_deg)) && (TDAN_MAX_PERSP >= out->persp) )
        {
            out->straight = TDAN_PASS;
        }
    }
}

/**
 * @brief Is (x, y) within CONFIRM_PX of a point of @p l.
 * @param l The points.
 * @param n How many.
 * @param x Column.
 * @param y Row.
 * @return true when near one.
 */
static bool near_any(const tdan_pt_t *l, int32_t n, int32_t x, int32_t y)
{
    for ( int32_t i = 0; i < n; i++ )
    {
        if ( (CONFIRM_PX >= tdan_iabs(l[i].x - x)) && (CONFIRM_PX >= tdan_iabs(l[i].y - y)) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Point @p i of the dead list followed by the stuck list.
 * @param d  The dead pixels.
 * @param nd How many.
 * @param st The stuck-on pixels.
 * @param i  The index across both.
 * @return The point.
 */
static const tdan_pt_t *pt_of(const tdan_pt_t *d, int32_t nd, const tdan_pt_t *st, int32_t i)
{
    if ( i < nd )
    {
        return &d[i];
    }
    return &st[i - nd];
}

/**
 * @brief Two defects (dead or stuck) closer than TDAN_PIX_GROUP_PX in x and y are a group.
 * @param d  The dead pixels.
 * @param nd How many.
 * @param st The stuck-on pixels.
 * @param ns How many.
 * @return true for a group.
 */
static bool pix_group(const tdan_pt_t *d, int32_t nd, const tdan_pt_t *st, int32_t ns)
{
    for ( int32_t i = 0; (nd + ns) > i; i++ )
    {
        const tdan_pt_t *a = pt_of(d, nd, st, i);
        for ( int32_t k = i + 1; (nd + ns) > k; k++ )
        {
            const tdan_pt_t *b = pt_of(d, nd, st, k);
            if ( (TDAN_PIX_GROUP_PX > tdan_iabs(a->x - b->x)) && (TDAN_PIX_GROUP_PX > tdan_iabs(a->y - b->y)) )
            {
                return true;
            }
        }
    }
    return false;
}

/**
 * @brief Do two zones overlap.
 * @param a One.
 * @param b The other.
 * @return true when they share a pixel.
 */
static bool zones_overlap(const tdan_zone_t *a, const tdan_zone_t *b)
{
    return (a->x0 <= b->x1) && (b->x0 <= a->x1) && (a->y0 <= b->y1) && (b->y0 <= a->y1);
}

/**
 * @brief The zones that count: dark in white and grey, the small ones born from dead candidates, the bright patches.
 * @param in     What was measured.
 * @param small  The small zones.
 * @param nsmall How many.
 * @param zones  The zones out, TDAN_ZONES_MAX.
 * @return How many.
 */
static int32_t gather_zones(const tdan_screen_in_t *in, const tdan_zone_t *small, int32_t nsmall, tdan_zone_t *zones)
{
    int32_t nz = 0;
    if ( (NULL != in->white) && (NULL != in->grey) )
    {
        for ( int32_t k = 0; k < in->white->nzones; k++ )
        {
            for ( int32_t m = 0; m < in->grey->nzones; m++ )
            {
                if ( zones_overlap(&in->white->zones[k], &in->grey->zones[m]) && (TDAN_ZONES_MAX > nz) )
                {
                    zones[nz] = in->white->zones[k];
                    nz++;
                }
            }
        }
    }
    for ( int32_t k = 0; (k < nsmall) && (TDAN_ZONES_MAX > nz); k++ )
    {
        zones[nz] = small[k];
        nz++;
    }
    if ( NULL != in->grey )
    {
        for ( int32_t k = 0; (k < in->grey->nbz) && (TDAN_ZONES_MAX > nz); k++ )
        {
            zones[nz] = in->grey->bz[k];
            nz++;
        }
    }
    return nz;
}

/**
 * @brief Is a dead pixel the zone's or the line's, not a pixel of its own.
 * @param p      The dead pixel.
 * @param zones  The zones.
 * @param nz     How many.
 * @param out    The lines that count.
 * @return true when it belongs to one.
 */
static bool dead_explained(const tdan_pt_t *p, const tdan_zone_t *zones, int32_t nz, const tdan_screen_t *out)
{
    bool in = false;
    for ( int32_t z = 0; z < nz; z++ )
    {
        if ( ((zones[z].x0 - DEAD_ZONE_PX) <= p->x) && ((zones[z].x1 + DEAD_ZONE_PX) >= p->x)
             && ((zones[z].y0 - DEAD_ZONE_PX) <= p->y) && ((zones[z].y1 + DEAD_ZONE_PX) >= p->y) )
        {
            in = true;
        }
    }
    if ( out->vline && (DEAD_LINE_PX >= tdan_iabs(p->x - out->vl.pos)) && ((out->vl.a0 - DEAD_LINE_END_PX) <= p->y)
         && ((out->vl.a1 + DEAD_LINE_END_PX) >= p->y) )
    {
        in = true;
    }
    if ( out->hline && (DEAD_LINE_PX >= tdan_iabs(p->y - out->hl.pos)) && ((out->hl.a0 - DEAD_LINE_END_PX) <= p->x)
         && ((out->hl.a1 + DEAD_LINE_END_PX) >= p->x) )
    {
        in = true;
    }
    return in;
}

/**
 * @brief The lines: the same column (row) in white and grey, deep enough, narrow or wide.
 * @param in  What was measured.
 * @param out vline, hline, vl and hl out.
 */
static void judge_lines(const tdan_screen_in_t *in, tdan_screen_t *out)
{
    out->vline = false;
    out->hline = false;
    bool nvline = false;
    bool nhline = false;
    (void)memset(&out->vl, 0, sizeof(out->vl));
    (void)memset(&out->hl, 0, sizeof(out->hl));
    if ( (NULL == in->white) || (NULL == in->grey) )
    {
        return;
    }
    const tdan_photo_t *wp = in->white;
    const tdan_photo_t *gp = in->grey;
    nvline = (TDAN_LINE_C > wp->vl.level) && (TDAN_LINE_C > gp->vl.level)
             && (LINE_SAME_PX >= tdan_iabs(wp->vl.pos - gp->vl.pos));
    nhline = (TDAN_LINE_C > wp->hl.level) && (TDAN_LINE_C > gp->hl.level)
             && (LINE_SAME_PX >= tdan_iabs(wp->hl.pos - gp->hl.pos));
    /* The glass blurs a 1 px dead column into a 12 px dip: the wide ones, each deep enough. */
    const bool wvline = (TDAN_WLINE_COL_W > wp->wvl.level) && (TDAN_WLINE_COL_G > gp->wvl.level)
                        && (LINE_SAME_PX >= tdan_iabs(wp->wvl.pos - gp->wvl.pos));
    const bool whline = (TDAN_WLINE_ROW > wp->whl.level) && (TDAN_WLINE_ROW > gp->whl.level)
                        && (LINE_SAME_PX >= tdan_iabs(wp->whl.pos - gp->whl.pos));
    out->vline = nvline || wvline;
    out->hline = nhline || whline;
    out->vl = wp->wvl;
    if ( nvline )
    {
        out->vl = wp->vl;
    }
    out->hl = wp->whl;
    if ( nhline )
    {
        out->hl = wp->hl;
    }
}

void tdan_judge_screen(const tdan_screen_in_t *in, tdan_screen_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    /* Dead pixels: dark in white AND grey; a region around one is a small zone. */
    tdan_pt_t dead[TDAN_PIX_MAXDEAD];
    int32_t nd = 0;
    tdan_zone_t small[TDAN_ZONES_MAX];
    int32_t nsmall = 0;
    if ( (NULL != in->white) && (NULL != in->grey) )
    {
        for ( int32_t i = 0; (i < in->pix->ndead) && (TDAN_PIX_MAXDEAD > nd); i++ )
        {
            const tdan_dead_t *q = &in->pix->dead[i];
            /* Not there the second time: not a pixel. */
            if ( in->confirming && !near_any(in->first_dead, in->nfirst_dead, q->x, q->y) )
            {
                continue;
            }
            if ( TDAN_PIX_ZONE_CELLS < q->zone )
            {
                if ( TDAN_ZONES_MAX > nsmall )
                {
                    small[nsmall].x0 = (int32_t)q->x - SMALL_ZONE_HALF;
                    small[nsmall].x1 = (int32_t)q->x + SMALL_ZONE_HALF;
                    small[nsmall].y0 = (int32_t)q->y - SMALL_ZONE_HALF;
                    small[nsmall].y1 = (int32_t)q->y + SMALL_ZONE_HALF;
                    small[nsmall].cells = SMALL_ZONE_CELLS;
                    small[nsmall].min_ratio = SMALL_ZONE_RATIO;
                    nsmall++;
                }
                continue;
            }
            dead[nd].x = q->x;
            dead[nd].y = q->y;
            dead[nd].v = -fminf(q->dw, q->dg);
            nd++;
        }
    }
    out->nsmall = nsmall;
    judge_lines(in, out);
    tdan_zone_t zones[TDAN_ZONES_MAX];
    int32_t nz = gather_zones(in, small, nsmall, zones);
    /* A thin dark band along a line IS the line (a 1-2 px line darkens its 10 px cells): the band is dropped. Only a
     * wide dark area swallows a line. */
    bool vl_ok = out->vline;
    bool hl_ok = out->hline;
    for ( int32_t z = 0; z < nz; z++ )
    {
        const int32_t zw = (zones[z].x1 - zones[z].x0) + 1;
        const int32_t zh = (zones[z].y1 - zones[z].y0) + 1;
        const bool on_v = out->vline && ((zones[z].x0 - LINE_ZONE_PX) <= out->vl.pos)
                          && ((zones[z].x1 + LINE_ZONE_PX) >= out->vl.pos);
        const bool on_h = out->hline && ((zones[z].y0 - LINE_ZONE_PX) <= out->hl.pos)
                          && ((zones[z].y1 + LINE_ZONE_PX) >= out->hl.pos);
        if ( on_v && (LINE_THIN_PX < zw) )
        {
            vl_ok = false;
        }
        if ( on_h && (LINE_THIN_PX < zh) )
        {
            hl_ok = false;
        }
        if ( (on_v && (LINE_THIN_PX >= zw) && vl_ok) || (on_h && (LINE_THIN_PX >= zh) && hl_ok) )
        {
            zones[z].cells = 0;
        }
    }
    for ( int32_t z = 0; z < nz; z++ )
    {
        if ( 0 < zones[z].cells )
        {
            out->zone[out->nzones] = zones[z];
            out->nzones++;
        }
    }
    out->vline = vl_ok;
    out->hline = hl_ok;
    /* What lies in or beside a zone or on a line is theirs, not a dead pixel. A dead pixel at the glass's left edge
     * in a pass whose light needed retakes is a warning: the false FAILs seen there were light on the edge. */
    for ( int32_t i = 0; i < nd; i++ )
    {
        if ( dead_explained(&dead[i], out->zone, out->nzones, out) )
        {
            continue;
        }
        if ( (0 < in->retakes) && (TDAN_EDGE_WARN_X > dead[i].x) )
        {
            out->edge[out->nedge] = dead[i];
            out->nedge++;
        }
        else
        {
            out->dead[out->ndead] = dead[i];
            out->ndead++;
        }
    }
    /* Stuck-on: the confirming pass keeps only those that came back. */
    for ( int32_t i = 0; (i < in->nstuck) && (TDAN_MAXP > out->nstuck); i++ )
    {
        if ( in->confirming && !near_any(in->first_stuck, in->nfirst_stuck, in->stuck[i].x, in->stuck[i].y) )
        {
            continue;
        }
        out->stuck[out->nstuck] = in->stuck[i];
        out->nstuck++;
    }
    int32_t ws = TDAN_PIX_OK;
    int32_t gs = TDAN_PIX_OK;
    if ( NULL != in->white )
    {
        ws = in->white->status;
    }
    if ( NULL != in->grey )
    {
        gs = in->grey->status;
    }
    out->group = pix_group(out->dead, out->ndead, out->stuck, out->nstuck);
    const bool bad = (TDAN_PIX_ALLOW_DEAD < out->ndead) || (TDAN_PIX_ALLOW_STUCK < out->nstuck) || out->group;
    out->confirm = !in->confirming && (TDAN_PIX_OK == ws) && (TDAN_PIX_OK == gs) && (bad || (0 < nsmall));
    out->pix_status = gs;
    if ( TDAN_PIX_OK != ws )
    {
        out->pix_status = ws;
    }
    if ( TDAN_PIX_OK != out->pix_status )
    {
        out->pixels = TDAN_NO_VERDICT;
    }
    else
    {
        /* Many stuck-on pixels strung together are not pixels: a reflection or a lamp the subtraction left. */
        int32_t chained = 0;
        for ( int32_t i = 0; i < out->nstuck; i++ )
        {
            int32_t nb = 0;
            for ( int32_t k = 0; k < out->nstuck; k++ )
            {
                if ( (k != i) && (STREAK_PX >= tdan_iabs(out->stuck[i].x - out->stuck[k].x))
                     && (STREAK_PX >= tdan_iabs(out->stuck[i].y - out->stuck[k].y)) )
                {
                    nb++;
                }
            }
            if ( STREAK_LINKS <= nb )
            {
                chained++;
            }
        }
        out->streak = (STREAK_MIN <= out->nstuck) && ((chained * 2) >= out->nstuck);
        out->pixels = TDAN_PASS;
        if ( bad )
        {
            out->pixels = TDAN_FAIL;
        }
    }
    out->lines = TDAN_PASS;
    if ( out->vline || out->hline )
    {
        out->lines = TDAN_FAIL;
    }
    out->zones = TDAN_PASS;
    if ( 0 < out->nzones )
    {
        out->zones = TDAN_FAIL;
    }
}

void tdan_judge_colour(const tdan_colour_in_t *in, tdan_colour_t *out)
{
    (void)memset(out, 0, sizeof(*out));
    bool cok = true;
    bool cnov = false;
    double wbv[TDAN_CHANNELS] = { 1.0, 1.0, 1.0 };
    /* Each channel of grey, red, green and blue is divided by the same channel of the white, so the light's tint and
     * the camera's gains cancel; only gross faults are looked for. */
    if ( in->ok[TDAN_WHITE] )
    {
        for ( int32_t k = 0; TDAN_CHANNELS > k; k++ )
        {
            wbv[k] = in->rgb[TDAN_WHITE][k];
            if ( WHITE_DEAD_LEVEL > wbv[k] )
            {
                wbv[k] = WHITE_DEAD_LEVEL;
                out->white_dead = true;
            }
        }
    }
    else
    {
        cnov = true;
    }
    for ( int32_t i = TDAN_GREY; TDAN_COLOURS > i; i++ )
    {
        if ( !in->ok[i] )
        {
            cnov = true;
            continue;
        }
        double n3[3];
        double sum = 0.0;
        for ( int32_t k = 0; TDAN_CHANNELS > k; k++ )
        {
            n3[k] = fmax(in->rgb[i][k], 0.0) / wbv[k];
            sum += n3[k];
        }
        double d = 0.0;
        for ( int32_t k = 0; TDAN_CHANNELS > k; k++ )
        {
            out->rel[i][k] = 0.0;
            if ( CHROMA_EPS < sum )
            {
                out->rel[i][k] = n3[k] / sum;
            }
            d += fabs(out->rel[i][k] - REF_CHROMA[i][k]);
        }
        out->dist[i] = d;
        if ( TDAN_CHROMA_TOL < d )
        {
            cok = false;
        }
        if ( in->retake[i] )
        {
            out->retakes++;
        }
    }
    /* The white on its own (relative colours hide a weak channel that dims everything alike): its chromaticity,
     * the ripple of its rows (flicker) and the exposure it needed (brightness). */
    if ( in->ok[TDAN_WHITE] )
    {
        const double ws = in->rgb[TDAN_WHITE][0] + in->rgb[TDAN_WHITE][1] + in->rgb[TDAN_WHITE][2];
        double wd = 0.0;
        for ( int32_t k = 0; TDAN_CHANNELS > k; k++ )
        {
            double share = 0.0;
            if ( CHROMA_EPS < ws )
            {
                share = in->rgb[TDAN_WHITE][k] / ws;
            }
            wd += fabs(share - ONE_THIRD);
        }
        out->wchroma = wd;
        if ( (TDAN_WHITE_CHROMA_TOL < wd) || (TDAN_RIPPLE_MAX < in->ripple) || (TDAN_EXP_RATIO_MAX < in->exp_ratio)
             || ((1.0 / TDAN_EXP_RATIO_MAX) > in->exp_ratio) )
        {
            cok = false;
        }
        const double level = fmax(in->rgb[TDAN_WHITE][0], fmax(in->rgb[TDAN_WHITE][1], in->rgb[TDAN_WHITE][2]));
        out->white_clipped = WHITE_CLIP_LEVEL <= level;
    }
    if ( out->white_dead )
    {
        cok = false;
    }
    if ( out->white_clipped || (cnov && cok) )
    {
        out->colour = TDAN_NO_VERDICT;
    }
    else if ( cok )
    {
        out->colour = TDAN_PASS;
    }
    else
    {
        out->colour = TDAN_FAIL;
    }
}
