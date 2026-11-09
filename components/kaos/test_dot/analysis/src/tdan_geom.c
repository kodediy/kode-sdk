/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file tdan_geom.c
 * @brief The homography, the four marks of the black photo, the pose and the luma.
 */
#include <math.h>
#include <string.h>

#include "tdan.h"
#include "tdan_priv.h"

#define PI_D              3.14159265358979323846   /**< pi. */
#define DEG_PER_PI        180.0     /**< Degrees in pi radians. */
#define PANEL_MID_ROW     251.0     /**< The panel's middle row, where the turn and the scale are read. */
#define H_DEGENERATE      1e-12     /**< A pivot under this: the four points do not make a homography. */
#define H_UNKNOWNS        8         /**< Unknowns of a homography with h[8] = 1. */
#define H_COLS            9         /**< Columns of its augmented system. */

#define MARK_PX           24.0      /**< The squares of the marks pattern, panel px. */
#define MARK_MIN_PEAK     110       /**< Below this nothing counts as a mark (the bezel and reflections stay under ~90). */
#define MARK_PEAK_FLOOR   100       /**< A dim half still shows its marks, never under this. */
#define MARK_PEAK_SHARE   0.45      /**< ... at this share of the brightest one. */
#define MARK_SEARCH_K     4.0       /**< Half side of the search box, in marks. */
#define MARK_TIGHT_K      1.4       /**< Half side of the tight box, in marks. */
#define MARK_EXCLUDE_K    2.5       /**< Around a seed already tried, in marks, nothing seeds again. */
#define MARK_HALF_MAX     0.5       /**< The centroid takes what is above half of the box's maximum. */
#define MARK_AREA_MIN     0.35      /**< A lit square covers at least this share of its area ... */
#define MARK_AREA_MAX     2.5       /**< ... and at most this. */
#define MARK_SCALE_TOL    0.15      /**< The found scale may differ from the nominal one this much. */
#define MARK_SEEDS        12        /**< Seeds the whole-photo search tries. */
#define MARK_MAX_TURN_DEG 20.0      /**< The whole-photo search's layout holds below this turn. */
#define MARK_MAX_PERSP    0.08      /**< ... and this perspective. */
#define MARK_RIGHT_COS    0.25      /**< Three squares: the corner's edges must be this close to square. */
#define MARK_COS_EPS      1e-9      /**< Keeps the cosine finite for coincident squares. */
#define MARK_COS_START    1e30      /**< Larger than any cosine. */
#define MISSING_POSE      4         /**< `missing` when the marks were found but the pose failed. */
#define MISSING_TRIANGLE  3         /**< `missing` when three squares make no right angle. */

#define LUMA_R            77U       /**< Red weight of the luma, of 256. */
#define LUMA_G            150U      /**< Green weight. */
#define LUMA_B            29U       /**< Blue weight. */
#define LUMA_SHIFT        8U        /**< The weights' scale, 2^8. */

static tdan_yield_fn_t s_yield;   /**< The hook the long loops call; NULL on the host. */

void tdan_set_yield(tdan_yield_fn_t f)
{
    s_yield = f;
}

void tdan_yield_point(uint32_t i, uint32_t mask)
{
    if ( (NULL != s_yield) && ((i & mask) == mask) )
    {
        s_yield();
    }
}

int32_t tdan_iabs(int32_t v)
{
    if ( 0 > v )
    {
        return -v;
    }
    return v;
}

float tdan_bilinear(const uint8_t *l, int32_t w, int32_t h, float u, float v)
{
    float uu = u;
    float vv = v;
    if ( 0.0f > uu )
    {
        uu = 0.0f;
    }
    if ( 0.0f > vv )
    {
        vv = 0.0f;
    }
    if ( ((float)(w - 1) - TDAN_SAMPLE_EDGE) < uu )
    {
        uu = (float)(w - 1) - TDAN_SAMPLE_EDGE;
    }
    if ( ((float)(h - 1) - TDAN_SAMPLE_EDGE) < vv )
    {
        vv = (float)(h - 1) - TDAN_SAMPLE_EDGE;
    }
    const int32_t x = (int32_t)uu;
    const int32_t y = (int32_t)vv;
    const float fx = uu - (float)x;
    const float fy = vv - (float)y;
    const uint8_t *p = &l[((size_t)y * (size_t)w) + (size_t)x];
    return ((1.0f - fy) * (((1.0f - fx) * (float)p[0]) + (fx * (float)p[1])))
           + (fy * (((1.0f - fx) * (float)p[w]) + (fx * (float)p[w + 1])));
}

void tdan_work_new_photo(tdan_work_t *wk)
{
    wk->warped = false;
}

void tdan_h_apply(const tdan_h_t *hm, double x, double y, double *u, double *v)
{
    const double *h = hm->h;
    const double w = (h[6] * x) + (h[7] * y) + h[8];
    *u = ((h[0] * x) + (h[1] * y) + h[2]) / w;
    *v = ((h[3] * x) + (h[4] * y) + h[5]) / w;
}

bool tdan_h_from_points(const double p[TDAN_MARK_COORDS], const double q[TDAN_MARK_COORDS], tdan_h_t *out)
{
    double a[H_UNKNOWNS][H_COLS];
    for ( int32_t k = 0; TDAN_MARK_COUNT > k; k++ )
    {
        const double x = p[2 * k];
        const double y = p[(2 * k) + 1];
        const double u = q[2 * k];
        const double v = q[(2 * k) + 1];
        const double r1[H_COLS] = { x, y, 1, 0, 0, 0, -u * x, -u * y, u };
        const double r2[H_COLS] = { 0, 0, 0, x, y, 1, -v * x, -v * y, v };
        (void)memcpy(a[2 * k], r1, sizeof(r1));
        (void)memcpy(a[(2 * k) + 1], r2, sizeof(r2));
    }
    /* Gauss-Jordan with partial pivoting. */
    for ( int32_t c = 0; H_UNKNOWNS > c; c++ )
    {
        int32_t piv = c;
        for ( int32_t r = c + 1; H_UNKNOWNS > r; r++ )
        {
            if ( fabs(a[r][c]) > fabs(a[piv][c]) )
            {
                piv = r;
            }
        }
        if ( H_DEGENERATE > fabs(a[piv][c]) )
        {
            return false;
        }
        if ( piv != c )
        {
            double t[H_COLS];
            (void)memcpy(t, a[c], sizeof(t));
            (void)memcpy(a[c], a[piv], sizeof(t));
            (void)memcpy(a[piv], t, sizeof(t));
        }
        for ( int32_t r = 0; H_UNKNOWNS > r; r++ )
        {
            if ( r != c )
            {
                const double f = a[r][c] / a[c][c];
                for ( int32_t k = c; H_COLS > k; k++ )
                {
                    a[r][k] -= f * a[c][k];
                }
            }
        }
    }
    for ( int32_t k = 0; H_UNKNOWNS > k; k++ )
    {
        out->h[k] = a[k][H_UNKNOWNS] / a[k][k];
    }
    out->h[H_UNKNOWNS] = 1.0;
    return true;
}

void tdan_mark_centres(double out[TDAN_MARK_COORDS])
{
    static const double C[TDAN_MARK_COORDS] = {
        TDAN_MARK_LEFT_X, TDAN_MARK_TOP_Y, TDAN_MARK_RIGHT_X, TDAN_MARK_TOP_Y,
        TDAN_MARK_RIGHT_X, TDAN_MARK_BOTTOM_Y, TDAN_MARK_LEFT_X, TDAN_MARK_BOTTOM_Y
    };
    (void)memcpy(out, C, sizeof(C));
}

/**
 * @brief One pass of the centroid: pixels above @p thr inside the box, weighted by how far above.
 * @param luma  The photo.
 * @param w     Width.
 * @param h     Height.
 * @param x0    Box left.
 * @param y0    Box top.
 * @param x1    Box right.
 * @param y1    Box bottom.
 * @param thr   The threshold.
 * @param cx    Centroid x out.
 * @param cy    Centroid y out.
 * @param count Pixels above out.
 * @return false when nothing is above.
 */
static bool centroid_box(const uint8_t *luma, int32_t w, int32_t h, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                         double thr, double *cx, double *cy, int32_t *count)
{
    double sw = 0.0;
    double sx = 0.0;
    double sy = 0.0;
    int32_t n = 0;
    for ( int32_t y = y0; y <= y1; y++ )
    {
        for ( int32_t x = x0; x <= x1; x++ )
        {
            if ( (0 > x) || (0 > y) || (x >= w) || (y >= h) )
            {
                continue;
            }
            const double v = (double)luma[((size_t)y * (size_t)w) + (size_t)x];
            if ( v > thr )
            {
                const double wt = v - thr;
                sw += wt;
                sx += wt * (double)x;
                sy += wt * (double)y;
                n++;
            }
        }
    }
    if ( 0.0 >= sw )
    {
        return false;
    }
    *cx = sx / sw;
    *cy = sy / sw;
    *count = n;
    return true;
}

/**
 * @brief Look for each mark in a wide box around where @p h0 predicts it (coarse centroid, then a tight one).
 * @param luma    The black photo.
 * @param w       Width.
 * @param h       Height.
 * @param h0      The nominal homography.
 * @param hm      The homography out.
 * @param found   The marks out.
 * @param missing The first mark not found out.
 * @param minpeak The weakest peak that is a mark in this photo.
 * @return false when a mark is not there or the pose is off.
 */
static bool locate_near(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *h0, tdan_h_t *hm,
                        double found[TDAN_MARK_COORDS], int32_t *missing, int32_t minpeak)
{
    double pc[TDAN_MARK_COORDS];
    tdan_mark_centres(pc);
    double s0;
    double a0;
    double p0;
    tdan_pose(h0, &a0, &s0, &p0);
    const int32_t rs = (int32_t)(MARK_PX * s0 * MARK_SEARCH_K);
    const int32_t r = (int32_t)(MARK_PX * s0 * MARK_TIGHT_K) + 1;
    const double area = MARK_PX * MARK_PX * s0 * s0;
    if ( NULL != missing )
    {
        *missing = -1;
    }
    for ( int32_t k = 0; TDAN_MARK_COUNT > k; k++ )
    {
        double pu;
        double pv;
        tdan_h_apply(h0, pc[2 * k], pc[(2 * k) + 1], &pu, &pv);
        int32_t mx = 0;
        for ( int32_t y = (int32_t)pv - rs; ((int32_t)pv + rs) >= y; y++ )
        {
            for ( int32_t x = (int32_t)pu - rs; ((int32_t)pu + rs) >= x; x++ )
            {
                if ( (0 > x) || (0 > y) || (x >= w) || (y >= h) )
                {
                    continue;
                }
                const int32_t v = (int32_t)luma[((size_t)y * (size_t)w) + (size_t)x];
                if ( v > mx )
                {
                    mx = v;
                }
            }
        }
        double cx = 0.0;
        double cy = 0.0;
        int32_t n = 0;
        if ( (mx < minpeak)
             || !centroid_box(luma, w, h, (int32_t)pu - rs, (int32_t)pv - rs, (int32_t)pu + rs, (int32_t)pv + rs,
                              MARK_HALF_MAX * (double)mx, &cx, &cy, &n) )
        {
            if ( NULL != missing )
            {
                *missing = k;
            }
            return false;
        }
        double fx = cx;
        double fy = cy;
        int32_t nf = 0;
        if ( !centroid_box(luma, w, h, (int32_t)cx - r, (int32_t)cy - r, (int32_t)cx + r, (int32_t)cy + r,
                           MARK_HALF_MAX * (double)mx, &fx, &fy, &nf)
             || ((double)nf < (MARK_AREA_MIN * area)) || ((double)nf > (MARK_AREA_MAX * area)) )
        {
            if ( NULL != missing )
            {
                *missing = k;
            }
            return false;
        }
        found[2 * k] = fx;
        found[(2 * k) + 1] = fy;
    }
    if ( !tdan_h_from_points(pc, found, hm) )
    {
        return false;
    }
    double s1;
    double a1;
    double p1;
    tdan_pose(hm, &a1, &s1, &p1);
    if ( fabs((s1 / s0) - 1.0) > MARK_SCALE_TOL )
    {
        if ( NULL != missing )
        {
            *missing = MISSING_POSE;
        }
        return false;
    }
    return true;
}

/**
 * @brief Swap two indices.
 * @param a One.
 * @param b The other.
 */
static void swap_idx(int32_t *a, int32_t *b)
{
    const int32_t t = *a;
    *a = *b;
    *b = t;
}

/**
 * @brief Order four squares as the marks: by y, the upper pair by x, the lower pair by x; marks 0 TL, 1 TR, 2 BR, 3 BL.
 * @param px The squares' x.
 * @param py The squares' y.
 * @param mk The marks out, (x, y) each.
 */
static void order_marks(const double px[TDAN_MARK_COUNT], const double py[TDAN_MARK_COUNT], double mk[TDAN_MARK_COORDS])
{
    int32_t id[4] = { 0, 1, 2, 3 };
    for ( int32_t i = 0; TDAN_MARK_COUNT > i; i++ )
    {
        for ( int32_t j = i + 1; TDAN_MARK_COUNT > j; j++ )
        {
            if ( py[id[j]] < py[id[i]] )
            {
                swap_idx(&id[i], &id[j]);
            }
        }
    }
    int32_t up0 = id[0];
    int32_t up1 = id[1];
    int32_t lo0 = id[2];
    int32_t lo1 = id[3];
    if ( px[up0] > px[up1] )
    {
        swap_idx(&up0, &up1);
    }
    if ( px[lo0] > px[lo1] )
    {
        swap_idx(&lo0, &lo1);
    }
    const int32_t ord[4] = { up0, up1, lo1, lo0 };
    for ( int32_t k = 0; TDAN_MARK_COUNT > k; k++ )
    {
        mk[2 * k] = px[ord[k]];
        mk[(2 * k) + 1] = py[ord[k]];
    }
}

/**
 * @brief Rescue: look for the marks in the whole photo, brightest unexplained pixel first, up to MARK_SEEDS seeds;
 *        with three squares the fourth is the parallelogram's completion.
 * @param luma    The black photo.
 * @param w       Width.
 * @param h       Height.
 * @param h0      The nominal homography (its scale).
 * @param hm      The homography out.
 * @param found   The marks out.
 * @param missing Squares found when too few, or why the pose failed.
 * @param nmarks  Squares seen: 4 or 3.
 * @param minpeak The weakest peak that is a mark in this photo.
 * @return false when not found.
 */
static bool locate_field(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *h0, tdan_h_t *hm,
                         double found[TDAN_MARK_COORDS], int32_t *missing, int32_t *nmarks, int32_t minpeak)
{
    double pc[TDAN_MARK_COORDS];
    tdan_mark_centres(pc);
    double s0;
    double a0;
    double p0;
    tdan_pose(h0, &a0, &s0, &p0);
    const double area = MARK_PX * MARK_PX * s0 * s0;
    const int32_t r = (int32_t)(MARK_PX * s0 * MARK_TIGHT_K) + 1;
    const int32_t ex = (int32_t)(MARK_PX * s0 * MARK_EXCLUDE_K);
    int32_t nex = 0;
    int32_t exx[MARK_SEEDS];
    int32_t exy[MARK_SEEDS];
    int32_t good = 0;
    double gx[4];
    double gy[4];
    for ( int32_t seed = 0; (MARK_SEEDS > seed) && (TDAN_MARK_COUNT > good); seed++ )
    {
        int32_t mx = 0;
        int32_t bx = -1;
        int32_t by = -1;
        for ( int32_t y = 0; y < h; y++ )
        {
            const uint8_t *row = &luma[(size_t)y * (size_t)w];
            for ( int32_t x = 0; x < w; x++ )
            {
                const int32_t v = (int32_t)row[x];
                if ( v <= mx )
                {
                    continue;
                }
                bool skip = false;
                for ( int32_t e = 0; e < nex; e++ )
                {
                    if ( (tdan_iabs(x - exx[e]) < ex) && (tdan_iabs(y - exy[e]) < ex) )
                    {
                        skip = true;
                        break;
                    }
                }
                if ( !skip )
                {
                    mx = v;
                    bx = x;
                    by = y;
                }
            }
        }
        if ( (0 > bx) || (mx < minpeak) )
        {
            break;
        }
        exx[nex] = bx;
        exy[nex] = by;
        nex++;
        double fx;
        double fy;
        int32_t nf = 0;
        if ( !centroid_box(luma, w, h, bx - r, by - r, bx + r, by + r, MARK_HALF_MAX * (double)mx, &fx, &fy, &nf) )
        {
            continue;
        }
        int32_t nf2 = 0;
        double f2x = fx;
        double f2y = fy;
        if ( !centroid_box(luma, w, h, (int32_t)fx - r, (int32_t)fy - r, (int32_t)fx + r, (int32_t)fy + r,
                           MARK_HALF_MAX * (double)mx, &f2x, &f2y, &nf2) )
        {
            continue;
        }
        /* A square cut by the photo's edge has a biased centroid: never a mark. */
        const bool cut = ((f2x - (double)r) < 0.0) || ((f2y - (double)r) < 0.0) || ((f2x + (double)r) >= (double)w)
                         || ((f2y + (double)r) >= (double)h);
        if ( cut || ((double)nf2 > (MARK_AREA_MAX * area)) || ((double)nf2 < (MARK_AREA_MIN * area)) )
        {
            continue;
        }
        exx[nex - 1] = (int32_t)f2x;
        exy[nex - 1] = (int32_t)f2y;
        gx[good] = f2x;
        gy[good] = f2y;
        good++;
    }
    if ( 3 > good )
    {
        if ( NULL != missing )
        {
            *missing = good;
        }
        return false;
    }
    int32_t idx[4] = { 0, 1, 2, 3 };
    for ( int32_t i = 0; i < good; i++ )
    {
        for ( int32_t j = i + 1; j < good; j++ )
        {
            if ( gy[idx[j]] < gy[idx[i]] )
            {
                swap_idx(&idx[i], &idx[j]);
            }
        }
    }
    double mk[TDAN_MARK_COORDS];
    if ( TDAN_MARK_COUNT == good )
    {
        const double qx[4] = { gx[idx[0]], gx[idx[1]], gx[idx[2]], gx[idx[3]] };
        const double qy[4] = { gy[idx[0]], gy[idx[1]], gy[idx[2]], gy[idx[3]] };
        order_marks(qx, qy, mk);
    }
    else
    {
        /* Three squares: the right-angle vertex is the one whose two edges are closest to square. */
        const int32_t tri[3] = { idx[0], idx[1], idx[2] };
        double best = MARK_COS_START;
        int32_t corner = idx[0];
        int32_t o1 = idx[1];
        int32_t o2 = idx[2];
        for ( int32_t t = 0; 3 > t; t++ )
        {
            const int32_t v = tri[t];
            const int32_t p = tri[(t + 1) % 3];
            const int32_t q = tri[(t + 2) % 3];
            const double ux = gx[p] - gx[v];
            const double uy = gy[p] - gy[v];
            const double vx = gx[q] - gx[v];
            const double vy = gy[q] - gy[v];
            const double cosv = fabs((ux * vx) + (uy * vy)) / ((hypot(ux, uy) * hypot(vx, vy)) + MARK_COS_EPS);
            if ( cosv < best )
            {
                best = cosv;
                corner = v;
                o1 = p;
                o2 = q;
            }
        }
        if ( MARK_RIGHT_COS < best )
        {
            if ( NULL != missing )
            {
                *missing = MISSING_TRIANGLE;
            }
            return false;
        }
        const double fx4 = (gx[o1] + gx[o2]) - gx[corner];
        const double fy4 = (gy[o1] + gy[o2]) - gy[corner];
        const double qx[4] = { gx[corner], gx[o1], gx[o2], fx4 };
        const double qy[4] = { gy[corner], gy[o1], gy[o2], fy4 };
        order_marks(qx, qy, mk);
    }
    if ( !tdan_h_from_points(pc, mk, hm) )
    {
        return false;
    }
    double s1;
    double a1;
    double p1;
    tdan_pose(hm, &a1, &s1, &p1);
    if ( (fabs((s1 / s0) - 1.0) > MARK_SCALE_TOL) || (fabs(a1) > MARK_MAX_TURN_DEG) || (MARK_MAX_PERSP < p1) )
    {
        if ( NULL != missing )
        {
            *missing = MISSING_POSE;
        }
        return false;
    }
    (void)memcpy(found, mk, sizeof(mk));
    *nmarks = good;
    return true;
}

bool tdan_locate_marks(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *h0, tdan_h_t *hm,
                       double found[TDAN_MARK_COORDS], int32_t *missing, int32_t *nmarks, bool *rescued)
{
    if ( NULL != nmarks )
    {
        *nmarks = TDAN_MARK_COUNT;
    }
    if ( NULL != rescued )
    {
        *rescued = false;
    }
    /* The weakest peak that is a mark in this photo: a dim half (shadow) still shows its marks. */
    int32_t g = 0;
    for ( size_t i = 0U; ((size_t)w * (size_t)h) > i; i++ )
    {
        if ( (int32_t)luma[i] > g )
        {
            g = (int32_t)luma[i];
        }
    }
    int32_t minpeak = MARK_MIN_PEAK;
    if ( MARK_MIN_PEAK <= g )
    {
        minpeak = (int32_t)(MARK_PEAK_SHARE * (double)g);
        if ( MARK_PEAK_FLOOR > minpeak )
        {
            minpeak = MARK_PEAK_FLOOR;
        }
        if ( MARK_MIN_PEAK < minpeak )
        {
            minpeak = MARK_MIN_PEAK;
        }
    }
    if ( locate_near(luma, w, h, h0, hm, found, missing, minpeak) )
    {
        return true;
    }
    int32_t nm = 0;
    if ( locate_field(luma, w, h, h0, hm, found, missing, &nm, minpeak) )
    {
        if ( NULL != nmarks )
        {
            *nmarks = nm;
        }
        if ( NULL != rescued )
        {
            *rescued = true;
        }
        return true;
    }
    if ( NULL != nmarks )
    {
        *nmarks = 0;
    }
    return false;
}

void tdan_pose(const tdan_h_t *hm, double *angle_deg, double *scale, double *persp)
{
    double u0;
    double v0;
    double u1;
    double v1;
    double u2;
    double v2;
    double u3;
    double v3;
    tdan_h_apply(hm, 0, PANEL_MID_ROW, &u0, &v0);
    tdan_h_apply(hm, TDAN_PW, PANEL_MID_ROW, &u1, &v1);
    *angle_deg = (atan2(v1 - v0, u1 - u0) * DEG_PER_PI) / PI_D;
    *scale = hypot(u1 - u0, v1 - v0) / TDAN_PW;
    tdan_h_apply(hm, 0, 0, &u2, &v2);
    tdan_h_apply(hm, TDAN_PW, 0, &u3, &v3);
    const double top = hypot(u3 - u2, v3 - v2);
    tdan_h_apply(hm, 0, TDAN_PH, &u2, &v2);
    tdan_h_apply(hm, TDAN_PW, TDAN_PH, &u3, &v3);
    const double bot = hypot(u3 - u2, v3 - v2);
    *persp = fabs((top / bot) - 1.0);
}

void tdan_luma(const uint16_t *rgb565, int32_t w, int32_t h, uint8_t *out)
{
    for ( size_t i = 0U; ((size_t)w * (size_t)h) > i; i++ )
    {
        uint32_t c[TDAN_CHANNELS];
        tdan_rgb565_expand(rgb565[i], c);
        out[i] = (uint8_t)(((LUMA_R * c[TDAN_CH_R]) + (LUMA_G * c[TDAN_CH_G]) + (LUMA_B * c[TDAN_CH_B])) >> LUMA_SHIFT);
    }
}

void tdan_rgb565_expand(uint16_t px, uint32_t rgb[TDAN_CHANNELS])
{
    const uint32_t v = px;
    const uint32_t r = (v >> TDAN_RGB565_R_SHIFT) & TDAN_RGB565_5BIT;
    const uint32_t g = (v >> TDAN_RGB565_G_SHIFT) & TDAN_RGB565_6BIT;
    const uint32_t b = v & TDAN_RGB565_5BIT;
    rgb[TDAN_CH_R] = (r << TDAN_EXPAND5_UP) | (r >> TDAN_EXPAND5_DOWN);
    rgb[TDAN_CH_G] = (g << TDAN_EXPAND6_UP) | (g >> TDAN_EXPAND6_DOWN);
    rgb[TDAN_CH_B] = (b << TDAN_EXPAND5_UP) | (b >> TDAN_EXPAND5_DOWN);
}
