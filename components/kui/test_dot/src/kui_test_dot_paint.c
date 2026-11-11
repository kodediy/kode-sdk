/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_test_dot_paint.c
 * @brief The E5 painter: the station's kgui_e5pat.c primitives, the same arithmetic in the same order.
 */
#include "kui_test_dot_paint.h"

#include <math.h>

#define PX_MID       0.5f        /**< From a pixel's corner to its centre. */
#define PI_F         3.14159265f /**< Pi, as kgui_e5pat.c spells it. */
#define DEG_HALF_TURN 180.0f     /**< Degrees in half a turn. */
#define R5_MASK      0xF8U       /**< The red and blue bits RGB565 keeps of a byte. */
#define G6_MASK      0xFCU       /**< The green bits RGB565 keeps of a byte. */
#define R_SHIFT      11U         /**< Red's place in RGB565. */
#define G_SHIFT      5U          /**< Green's place in RGB565. */
#define B5_DROP      3U          /**< Bits a byte of blue loses. */
#define G6_PLACE     3U          /**< From a byte of green to its place. */
#define R8_PLACE     8U          /**< From a byte of red to its place. */
#define BYTE_MAX     255U        /**< A full channel byte. */
#define C5_MAX       31U         /**< A full 5-bit channel. */
#define C6_MAX       63U         /**< A full 6-bit channel. */
#define RGB_R_SHIFT  16U         /**< Red's place in 0xRRGGBB. */
#define RGB_G_SHIFT  8U          /**< Green's place in 0xRRGGBB. */
#define AA_MARGIN    2           /**< Pixels around a shape its edge can still touch. */
#define TRI_SS       3           /**< A triangle's supersampling per axis. */
#define TRI_SS_N     9.0f        /**< TRI_SS squared. */
#define ARC_STEP_DEG 5.0f        /**< An arc's segment, in degrees. */
#define ARC_SLACK    0.01f       /**< So an arc reaches its last angle. */

/* The glyphs, in fractions of their size, as kgui_e5pat.c draws them. */
#define CHECK_LX     0.45f       /**< Check: the short stroke's left end. */
#define CHECK_MX     0.1f        /**< Check: the corner, left of the centre. */
#define CHECK_MY     0.35f       /**< Check: the corner, below the centre. */
#define CHECK_RX     0.5f        /**< Check: the long stroke's right end. */
#define CROSS_ARM    0.38f       /**< Cross: half a stroke's reach. */
#define RETRY_A0_DEG (-60.0f)    /**< Repeat: where the arc starts and its head sits. */
#define RETRY_A1_DEG 250.0f      /**< Repeat: where the arc ends. */
#define HEAD_TIP_X   1.4f        /**< Repeat's head: the tip, in stroke widths. */
#define HEAD_TIP_Y   0.3f        /**< Repeat's head: the tip, up. */
#define HEAD_L_X     0.9f        /**< Repeat's head: the left barb, back. */
#define HEAD_L_Y     1.8f        /**< Repeat's head: the left barb, up. */
#define HEAD_R_X     0.3f        /**< Repeat's head: the right barb, back. */
#define HEAD_R_Y     1.2f        /**< Repeat's head: the right barb, down. */

/* Hands off (kgui_e5pat.c, glyph_no_touch()). */
#define HAND_X       205.0f      /**< The hand's centre x. */
#define HAND_Y       262.0f      /**< The hand's centre y. */
#define HAND_K       0.95f       /**< The hand's scale. */
#define PALM_DY      19.0f       /**< The palm, below the centre. */
#define PALM_HX      30.0f       /**< Half the palm's width. */
#define PALM_HY      29.0f       /**< Half the palm's height. */
#define PALM_R       16.0f       /**< The palm's corner radius. */
#define FINGERS      4           /**< Fingers. */
#define FINGER_HX    7.0f        /**< Half a finger's width, and its radius. */
#define FINGER_BASE  10.0f       /**< Where the fingers start, above the centre. */
#define FINGER_JOIN  5.0f        /**< How far a finger sinks into the palm. */
#define THUMB_BASE_X (-30.0f)    /**< The thumb's base, x. */
#define THUMB_BASE_Y 4.0f        /**< The thumb's base, y. */
#define THUMB_TIP_X  (-56.0f)    /**< The thumb's tip, x. */
#define THUMB_TIP_Y  (-22.0f)    /**< The thumb's tip, y. */
#define THUMB_TOP_X  (-44.0f)    /**< The thumb's upper edge, x. */
#define THUMB_TOP_Y  (-32.0f)    /**< The thumb's upper edge, y. */
#define THUMB_IN_X   (-22.0f)    /**< The thumb's inner edge, x. */
#define THUMB_IN_Y   (-10.0f)    /**< The thumb's inner edge, y. */
#define SIGN_X       205.0f      /**< The prohibition ring's centre x. */
#define SIGN_Y       252.0f      /**< The prohibition ring's centre y. */
#define SIGN_R       86.0f       /**< The ring's radius. */
#define SIGN_HW      6.0f        /**< Half the ring's and the bar's width. */
#define BAR_X0       140.0f      /**< The bar's upper end, x. */
#define BAR_Y0       187.0f      /**< The bar's upper end, y. */
#define BAR_X1       270.0f      /**< The bar's lower end, x. */
#define BAR_Y1       317.0f      /**< The bar's lower end, y. */

kui_td_pt_t kui_td_pt(float x, float y)
{
    kui_td_pt_t p;
    p.x = x;
    p.y = y;
    return p;
}

/**
 * @brief RGB565 of three bytes, as kgui_e5pat.c's c565().
 * @param r Red.
 * @param g Green.
 * @param b Blue.
 * @return The value.
 */
static uint16_t c565(uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t const v = (((uint32_t)r & R5_MASK) << R8_PLACE) | (((uint32_t)g & G6_MASK) << G6_PLACE)
                       | ((uint32_t)b >> B5_DROP);
    return (uint16_t)v;
}

/**
 * @brief One channel of 0xRRGGBB.
 * @param rgb   The colour.
 * @param shift The channel's place.
 * @return Its byte.
 */
static uint8_t channel(uint32_t rgb, uint32_t shift)
{
    return (uint8_t)((rgb >> shift) & BYTE_MAX);
}

uint16_t kui_td_565(uint32_t rgb)
{
    return c565(channel(rgb, RGB_R_SHIFT), channel(rgb, RGB_G_SHIFT), channel(rgb, 0U));
}

/**
 * @brief The pixel at a place on the glass.
 * @param g The pixels.
 * @param x Column, inside the glass.
 * @param y Row, inside the glass.
 * @return Where it is.
 */
static uint16_t *at(const kui_td_px_t *g, int32_t x, int32_t y)
{
    return &g->px[((size_t)y * g->stride_px) + (size_t)x];
}

/**
 * @brief Whether a place is on the glass.
 * @param x Column.
 * @param y Row.
 * @return true inside it.
 */
static bool inside(int32_t x, int32_t y)
{
    return (0 <= x) && (KUI_TD_W_PX > x) && (0 <= y) && (KUI_TD_H_PX > y);
}

void kui_td_fill(const kui_td_px_t *g, uint32_t rgb)
{
    uint16_t const c = kui_td_565(rgb);
    for ( int32_t y = 0; KUI_TD_H_PX > y; y++ )
    {
        uint16_t *row = at(g, 0, y);
        for ( int32_t x = 0; KUI_TD_W_PX > x; x++ )
        {
            row[x] = c;
        }
    }
}

void kui_td_rect(const kui_td_px_t *g, int32_t x0, int32_t y0, int32_t w, int32_t h, uint32_t rgb)
{
    uint16_t const c = kui_td_565(rgb);
    for ( int32_t y = y0; (y0 + h) > y; y++ )
    {
        for ( int32_t x = x0; (x0 + w) > x; x++ )
        {
            if ( inside(x, y) )
            {
                *at(g, x, y) = c;
            }
        }
    }
}

/**
 * @brief Lay a colour over one pixel with a coverage, as kgui_e5pat.c's blend().
 * @param g   The pixels.
 * @param x   Column.
 * @param y   Row.
 * @param rgb The colour.
 * @param cov How much of the pixel it covers; at or under 0 nothing, over 1 is 1.
 */
static void blend(const kui_td_px_t *g, int32_t x, int32_t y, uint32_t rgb, float cov)
{
    float a = cov;
    if ( (!inside(x, y)) || (0.0f >= a) )
    {
        return;
    }
    if ( 1.0f < a )
    {
        a = 1.0f;
    }
    uint16_t *p = at(g, x, y);
    uint32_t const v = (uint32_t)*p;
    uint32_t const r8 = (((v >> R_SHIFT) & C5_MAX) * BYTE_MAX) / C5_MAX;
    uint32_t const g8 = (((v >> G_SHIFT) & C6_MAX) * BYTE_MAX) / C6_MAX;
    uint32_t const b8 = ((v & C5_MAX) * BYTE_MAX) / C5_MAX;
    float const pr = (float)r8;
    float const pg = (float)g8;
    float const pb = (float)b8;
    float const r = (float)channel(rgb, RGB_R_SHIFT);
    float const gr = (float)channel(rgb, RGB_G_SHIFT);
    float const b = (float)channel(rgb, 0U);
    float const nr = pr + ((r - pr) * a) + PX_MID;
    float const ng = pg + ((gr - pg) * a) + PX_MID;
    float const nb = pb + ((b - pb) * a) + PX_MID;
    *p = c565((uint8_t)nr, (uint8_t)ng, (uint8_t)nb);
}

void kui_td_disc(const kui_td_px_t *g, kui_td_pt_t c, float rad, uint32_t rgb)
{
    float const top = c.y - rad;
    float const bottom = c.y + rad;
    float const left = c.x - rad;
    float const right = c.x + rad;
    for ( int32_t y = (int32_t)top - 1; ((int32_t)bottom + 1) >= y; y++ )
    {
        for ( int32_t x = (int32_t)left - 1; ((int32_t)right + 1) >= x; x++ )
        {
            float const dx = ((float)x + PX_MID) - c.x;
            float const dy = ((float)y + PX_MID) - c.y;
            blend(g, x, y, rgb, (rad - sqrtf((dx * dx) + (dy * dy))) + PX_MID);
        }
    }
}

void kui_td_stroke(const kui_td_px_t *g, kui_td_pt_t a, kui_td_pt_t b, float hw, uint32_t rgb)
{
    float const dx = b.x - a.x;
    float const dy = b.y - a.y;
    float const l2 = (dx * dx) + (dy * dy);
    int32_t const y_lo = ((int32_t)fminf(a.y, b.y) - (int32_t)hw) - AA_MARGIN;
    int32_t const y_hi = ((int32_t)fmaxf(a.y, b.y) + (int32_t)hw) + AA_MARGIN;
    int32_t const x_lo = ((int32_t)fminf(a.x, b.x) - (int32_t)hw) - AA_MARGIN;
    int32_t const x_hi = ((int32_t)fmaxf(a.x, b.x) + (int32_t)hw) + AA_MARGIN;
    for ( int32_t y = y_lo; y <= y_hi; y++ )
    {
        for ( int32_t x = x_lo; x <= x_hi; x++ )
        {
            float const px = (float)x + PX_MID;
            float const py = (float)y + PX_MID;
            float t = 0.0f;
            if ( 0.0f < l2 )
            {
                t = (((px - a.x) * dx) + ((py - a.y) * dy)) / l2;
            }
            t = fminf(1.0f, fmaxf(0.0f, t));
            float const ex = (a.x + (t * dx)) - px;
            float const ey = (a.y + (t * dy)) - py;
            blend(g, x, y, rgb, (hw - sqrtf((ex * ex) + (ey * ey))) + PX_MID);
        }
    }
}

void kui_td_ring(const kui_td_px_t *g, kui_td_pt_t c, float rad, float hw, uint32_t rgb)
{
    float const top = (c.y - rad) - hw;
    float const bottom = (c.y + rad) + hw;
    float const left = (c.x - rad) - hw;
    float const right = (c.x + rad) + hw;
    int32_t const y_lo = (int32_t)top - AA_MARGIN;
    int32_t const y_hi = (int32_t)bottom + AA_MARGIN;
    int32_t const x_lo = (int32_t)left - AA_MARGIN;
    int32_t const x_hi = (int32_t)right + AA_MARGIN;
    for ( int32_t y = y_lo; y <= y_hi; y++ )
    {
        for ( int32_t x = x_lo; x <= x_hi; x++ )
        {
            float const dx = ((float)x + PX_MID) - c.x;
            float const dy = ((float)y + PX_MID) - c.y;
            blend(g, x, y, rgb, (hw - fabsf(sqrtf((dx * dx) + (dy * dy)) - rad)) + PX_MID);
        }
    }
}

/**
 * @brief Signed distance from a point to a rounded rectangle, as kgui_e5pat.c's sd_rrect().
 * @param p   The point.
 * @param c   The rectangle's centre.
 * @param hx  Half its width.
 * @param hy  Half its height.
 * @param rad Its corner radius.
 * @return The distance; negative inside.
 */
static float sd_rrect(kui_td_pt_t p, kui_td_pt_t c, float hx, float hy, float rad)
{
    float const qx = (fabsf(p.x - c.x) - hx) + rad;
    float const qy = (fabsf(p.y - c.y) - hy) + rad;
    float const ox = fmaxf(qx, 0.0f);
    float const oy = fmaxf(qy, 0.0f);
    return (sqrtf((ox * ox) + (oy * oy)) + fminf(fmaxf(qx, qy), 0.0f)) - rad;
}

void kui_td_rrect(const kui_td_px_t *g, kui_td_pt_t c, float hx, float hy, float rad, uint32_t rgb, float ow)
{
    float const top = c.y - hy;
    float const bottom = c.y + hy;
    float const left = c.x - hx;
    float const right = c.x + hx;
    for ( int32_t y = (int32_t)top - AA_MARGIN; ((int32_t)bottom + AA_MARGIN) >= y; y++ )
    {
        for ( int32_t x = (int32_t)left - AA_MARGIN; ((int32_t)right + AA_MARGIN) >= x; x++ )
        {
            float const d = sd_rrect(kui_td_pt((float)x + PX_MID, (float)y + PX_MID), c, hx, hy, rad);
            float a = fminf(1.0f, fmaxf(0.0f, PX_MID - d));
            if ( 0.0f < ow )
            {
                a *= fminf(1.0f, fmaxf(0.0f, (d + ow) + PX_MID));
            }
            blend(g, x, y, rgb, a);
        }
    }
}

/**
 * @brief An arc, degrees clockwise from 3 o'clock, as kgui_e5pat.c's aa_arc().
 * @param g   The pixels.
 * @param c   Its centre.
 * @param rad Its radius.
 * @param a0  Its first angle.
 * @param a1  Its last angle.
 * @param hw  Half its width.
 * @param rgb The colour.
 */
static void arc(const kui_td_px_t *g, kui_td_pt_t c, float rad, float a0, float a1, float hw, uint32_t rgb)
{
    kui_td_pt_t prev = kui_td_pt(0.0f, 0.0f);
    bool have = false;
    /* The same angles as kgui_e5pat.c's float counter: a0, a0 + step, ... up to a1 (whole degrees add exactly). */
    float const span = ((a1 + ARC_SLACK) - a0) / ARC_STEP_DEG;
    int32_t const n = (int32_t)span + 1;
    for ( int32_t i = 0; i < n; i++ )
    {
        float const a = a0 + ((float)i * ARC_STEP_DEG);
        float const rr = (a * PI_F) / DEG_HALF_TURN;
        kui_td_pt_t const p = kui_td_pt(c.x + (rad * cosf(rr)), c.y + (rad * sinf(rr)));
        if ( have )
        {
            kui_td_stroke(g, prev, p, hw, rgb);
        }
        prev = p;
        have = true;
    }
}

/**
 * @brief Whether a sample is in a triangle, edges included, as kgui_e5pat.c's aa_tri() decides it.
 * @param p The sample.
 * @param a A corner.
 * @param b A corner.
 * @param c A corner.
 * @return true inside.
 */
static bool in_tri(kui_td_pt_t p, kui_td_pt_t a, kui_td_pt_t b, kui_td_pt_t c)
{
    float const d0 = ((p.x - b.x) * (a.y - b.y)) - ((a.x - b.x) * (p.y - b.y));
    float const d1 = ((p.x - c.x) * (b.y - c.y)) - ((b.x - c.x) * (p.y - c.y));
    float const d2 = ((p.x - a.x) * (c.y - a.y)) - ((c.x - a.x) * (p.y - a.y));
    bool const neg = (0.0f > d0) || (0.0f > d1) || (0.0f > d2);
    bool const pos = (0.0f < d0) || (0.0f < d1) || (0.0f < d2);
    return !(neg && pos);
}

void kui_td_tri(const kui_td_px_t *g, kui_td_pt_t a, kui_td_pt_t b, kui_td_pt_t c, uint32_t rgb)
{
    int32_t const xa = (int32_t)fminf(a.x, fminf(b.x, c.x)) - 1;
    int32_t const xb = (int32_t)fmaxf(a.x, fmaxf(b.x, c.x)) + 1;
    int32_t const ya = (int32_t)fminf(a.y, fminf(b.y, c.y)) - 1;
    int32_t const yb = (int32_t)fmaxf(a.y, fmaxf(b.y, c.y)) + 1;
    for ( int32_t y = ya; y <= yb; y++ )
    {
        for ( int32_t x = xa; x <= xb; x++ )
        {
            int32_t n = 0;
            for ( int32_t sy = 0; TRI_SS > sy; sy++ )
            {
                for ( int32_t sx = 0; TRI_SS > sx; sx++ )
                {
                    kui_td_pt_t const s = kui_td_pt((float)x + (((float)sx + PX_MID) / (float)TRI_SS),
                                             (float)y + (((float)sy + PX_MID) / (float)TRI_SS));
                    if ( in_tri(s, a, b, c) )
                    {
                        n++;
                    }
                }
            }
            blend(g, x, y, rgb, (float)n / TRI_SS_N);
        }
    }
}

void kui_td_glyph_check(const kui_td_px_t *g, kui_td_pt_t c, float size, float w, uint32_t rgb)
{
    kui_td_pt_t const corner = kui_td_pt(c.x - (size * CHECK_MX), c.y + (size * CHECK_MY));
    kui_td_stroke(g, kui_td_pt(c.x - (size * CHECK_LX), c.y), corner, w / 2.0f, rgb);
    kui_td_stroke(g, corner, kui_td_pt(c.x + (size * CHECK_RX), c.y - (size * CHECK_MY)), w / 2.0f, rgb);
}

void kui_td_glyph_cross(const kui_td_px_t *g, kui_td_pt_t c, float size, float w, uint32_t rgb)
{
    float const a = size * CROSS_ARM;
    kui_td_stroke(g, kui_td_pt(c.x - a, c.y - a), kui_td_pt(c.x + a, c.y + a), w / 2.0f, rgb);
    kui_td_stroke(g, kui_td_pt(c.x - a, c.y + a), kui_td_pt(c.x + a, c.y - a), w / 2.0f, rgb);
}

void kui_td_glyph_retry(const kui_td_px_t *g, kui_td_pt_t c, float rad, float w, uint32_t rgb)
{
    arc(g, c, rad, RETRY_A0_DEG, RETRY_A1_DEG, w / 2.0f, rgb);
    float const a = (RETRY_A0_DEG * PI_F) / DEG_HALF_TURN;
    float const ex = c.x + (rad * cosf(a));
    float const ey = c.y + (rad * sinf(a));
    kui_td_tri(g, kui_td_pt(ex + (HEAD_TIP_X * w), ey - (HEAD_TIP_Y * w)), kui_td_pt(ex - (HEAD_L_X * w), ey - (HEAD_L_Y * w)),
               kui_td_pt(ex - (HEAD_R_X * w), ey + (HEAD_R_Y * w)), rgb);
}

/**
 * @brief An open hand, as kgui_e5pat.c's glyph_hand().
 * @param g   The pixels.
 * @param c   Its centre.
 * @param k   Its scale.
 * @param rgb The colour.
 */
static void hand(const kui_td_px_t *g, kui_td_pt_t c, float k, uint32_t rgb)
{
    static const float FX[FINGERS] = { -24.0f, -8.0f, 8.0f, 24.0f };  /* each finger's x from the centre */
    static const float FH[FINGERS] = { 40.0f, 50.0f, 50.0f, 42.0f };  /* each finger's length */
    kui_td_rrect(g, kui_td_pt(c.x, c.y + (PALM_DY * k)), PALM_HX * k, PALM_HY * k, PALM_R * k, rgb, 0.0f);
    for ( int32_t i = 0; FINGERS > i; i++ )
    {
        float const half = FH[i] / 2.0f;
        kui_td_rrect(g, kui_td_pt(c.x + (FX[i] * k), c.y - (((FINGER_BASE + half) - FINGER_JOIN) * k)), FINGER_HX * k,
                     (half + FINGER_JOIN) * k, FINGER_HX * k, rgb, 0.0f);
    }
    kui_td_pt_t const base = kui_td_pt(c.x + (THUMB_BASE_X * k), c.y + (THUMB_BASE_Y * k));
    kui_td_pt_t const top = kui_td_pt(c.x + (THUMB_TOP_X * k), c.y + (THUMB_TOP_Y * k));
    kui_td_tri(g, base, kui_td_pt(c.x + (THUMB_TIP_X * k), c.y + (THUMB_TIP_Y * k)), top, rgb);
    kui_td_tri(g, base, top, kui_td_pt(c.x + (THUMB_IN_X * k), c.y + (THUMB_IN_Y * k)), rgb);
}

void kui_td_glyph_no_touch(const kui_td_px_t *g, uint32_t hand_rgb, uint32_t sign)
{
    hand(g, kui_td_pt(HAND_X, HAND_Y), HAND_K, hand_rgb);
    kui_td_ring(g, kui_td_pt(SIGN_X, SIGN_Y), SIGN_R, SIGN_HW, sign);
    kui_td_stroke(g, kui_td_pt(BAR_X0, BAR_Y0), kui_td_pt(BAR_X1, BAR_Y1), SIGN_HW, sign);
}

void kui_td_marks(const kui_td_px_t *g)
{
    static const int32_t AT[KUI_TD_MARKS][2] = {
        { KUI_TD_MARK_LEFT_X_PX, KUI_TD_MARK_TOP_Y_PX },
        { KUI_TD_MARK_RIGHT_X_PX, KUI_TD_MARK_TOP_Y_PX },
        { KUI_TD_MARK_RIGHT_X_PX, KUI_TD_MARK_BOTTOM_Y_PX },
        { KUI_TD_MARK_LEFT_X_PX, KUI_TD_MARK_BOTTOM_Y_PX },
    };
    int32_t const half = KUI_TD_MARK_SIZE_PX / 2;
    for ( uint32_t k = 0U; KUI_TD_MARKS > k; k++ )
    {
        kui_td_rect(g, AT[k][0] - half, AT[k][1] - half, KUI_TD_MARK_SIZE_PX, KUI_TD_MARK_SIZE_PX, KUI_TD_MARK_RGB);
    }
}

void kui_td_witnesses(const kui_td_px_t *g, bool rim)
{
    /* Top left, top right, bottom left, bottom right. */
    static const int32_t WITNESS_AT[KUI_TD_WITNESSES][2] = {
        { KUI_TD_WITNESS_TL_X_PX, KUI_TD_WITNESS_TOP_Y_PX },
        { KUI_TD_WITNESS_TR_X_PX, KUI_TD_WITNESS_TOP_Y_PX },
        { KUI_TD_WITNESS_BL_X_PX, KUI_TD_WITNESS_BOT_Y_PX },
        { KUI_TD_WITNESS_BR_X_PX, KUI_TD_WITNESS_BOT_Y_PX },
    };
    for ( uint32_t i = 0U; KUI_TD_WITNESSES > i; i++ )
    {
        kui_td_pt_t const c = kui_td_pt((float)WITNESS_AT[i][0], (float)WITNESS_AT[i][1]);
        if ( rim )
        {
            kui_td_disc(g, c, KUI_TD_WITNESS_RIM_R_PX, KUI_TD_RGB_BLACK);
        }
        kui_td_disc(g, c, KUI_TD_WITNESS_R_PX, KUI_TD_WITNESS_RGB);
    }
}
