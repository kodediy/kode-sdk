/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_test_dot_patterns.c
 * @brief The patterns the E5 camera photographs, pixel for pixel as the station's kgui_e5pat.c paints them.
 */
#include "kui_test_dot.h"
#include "kui_test_dot_paint.h"

/* The result icons (kgui_e5pat.c, result_icon()): offsets from the band's centre. */
#define OK_X0     27.0f  /**< Check: the short stroke's start, left. */
#define OK_Y0     2.0f   /**< Check: the short stroke's start, down. */
#define OK_XM     8.0f   /**< Check: the corner, left. */
#define OK_YM     21.0f  /**< Check: the corner, down. */
#define OK_X1     28.0f  /**< Check: the long stroke's end, right. */
#define OK_Y1     19.0f  /**< Check: the long stroke's end, up. */
#define FAIL_ARM  24.0f  /**< Cross: each stroke's reach in both axes. */
#define GREY_R_SHIFT 16U /**< Red's place in 0xRRGGBB. */
#define GREY_G_SHIFT 8U  /**< Green's place in 0xRRGGBB. */

_Static_assert((KUI_TD_MARK_LEFT_X_PX - (KUI_TD_MARK_SIZE_PX / 2)) >= 0, "a mark leaves the glass");
_Static_assert((KUI_TD_MARK_RIGHT_X_PX + (KUI_TD_MARK_SIZE_PX / 2)) <= KUI_TD_W_PX, "a mark leaves the glass");
_Static_assert((KUI_TD_MARK_TOP_Y_PX - (KUI_TD_MARK_SIZE_PX / 2)) >= KUI_TD_BAND_Y0_PX, "a mark leaves the band");
_Static_assert((KUI_TD_MARK_BOTTOM_Y_PX + (KUI_TD_MARK_SIZE_PX / 2)) <= KUI_TD_BAND_Y1_PX, "a mark leaves the band");

/**
 * @brief A green disc with a check or a red one with a cross, centred in the band, on black (kgui_e5pat.c, result_icon()).
 * @param g  The pixels.
 * @param ok true for the check.
 */
static void paint_icon(const kui_td_px_t *g, bool ok)
{
    kui_td_pt_t c;
    c.x = (float)KUI_TD_W_PX / 2.0f;
    int32_t const band_sum = KUI_TD_BAND_Y0_PX + KUI_TD_BAND_Y1_PX;
    c.y = (float)band_sum / 2.0f;
    kui_td_fill(g, KUI_TD_RGB_BLACK);
    if ( ok )
    {
        kui_td_pt_t a;
        kui_td_pt_t m;
        kui_td_pt_t b;
        a.x = c.x - OK_X0;
        a.y = c.y + OK_Y0;
        m.x = c.x - OK_XM;
        m.y = c.y + OK_YM;
        b.x = c.x + OK_X1;
        b.y = c.y - OK_Y1;
        kui_td_disc(g, c, KUI_TD_ICON_R_PX, KUI_TD_RGB_ICON_OK);
        kui_td_stroke(g, a, m, KUI_TD_ICON_STROKE_PX, KUI_TD_RGB_ICON_INK);
        kui_td_stroke(g, m, b, KUI_TD_ICON_STROKE_PX, KUI_TD_RGB_ICON_INK);
    }
    else
    {
        kui_td_pt_t a;
        kui_td_pt_t b;
        kui_td_disc(g, c, KUI_TD_ICON_R_PX, KUI_TD_RGB_ICON_FAIL);
        a.x = c.x - FAIL_ARM;
        a.y = c.y - FAIL_ARM;
        b.x = c.x + FAIL_ARM;
        b.y = c.y + FAIL_ARM;
        kui_td_stroke(g, a, b, KUI_TD_ICON_STROKE_PX, KUI_TD_RGB_ICON_INK);
        a.x = c.x + FAIL_ARM;
        b.x = c.x - FAIL_ARM;
        kui_td_stroke(g, a, b, KUI_TD_ICON_STROKE_PX, KUI_TD_RGB_ICON_INK);
    }
}

/**
 * @brief Lit rows every period on black (kgui_e5pat.c, K_LINES, horizontal).
 * @param g   The pixels.
 * @param rgb The lit colour.
 */
static void paint_lines(const kui_td_px_t *g, uint32_t rgb)
{
    kui_td_fill(g, KUI_TD_RGB_BLACK);
    for ( int32_t y = 0; KUI_TD_H_PX > y; y++ )
    {
        if ( (y % KUI_TD_LINES_PERIOD_PX) < KUI_TD_LINES_ON_PX )
        {
            kui_td_rect(g, 0, y, KUI_TD_W_PX, 1, rgb);
        }
    }
}

/**
 * @brief A lattice of dots on black (kgui_e5pat.c, K_DOTS).
 * @param g   The pixels.
 * @param rgb The dots' colour.
 */
static void paint_dots(const kui_td_px_t *g, uint32_t rgb)
{
    kui_td_fill(g, KUI_TD_RGB_BLACK);
    for ( int32_t y = 0; KUI_TD_H_PX > y; y += KUI_TD_DOTS_PITCH_PX )
    {
        for ( int32_t x = 0; KUI_TD_W_PX > x; x += KUI_TD_DOTS_PITCH_PX )
        {
            kui_td_rect(g, x, y, KUI_TD_DOTS_SIZE_PX, KUI_TD_DOTS_SIZE_PX, rgb);
        }
    }
}

/**
 * @brief Paint one pattern.
 * @param g   The pixels.
 * @param p   The pattern, below KUI_TD_PAT_OFF.
 * @param arg Its arg, already checked.
 */
static void pattern_paint(const kui_td_px_t *g, kui_td_pattern_t p, uint8_t arg)
{
    static const uint32_t COLOURS[KUI_TD_COLOUR_COUNT] = {
        KUI_TD_RGB_BLACK, KUI_TD_RGB_WHITE, KUI_TD_RGB_RED, KUI_TD_RGB_GREEN, KUI_TD_RGB_BLUE,
    };
    /* The witnesses go over every pattern but a black one and the marks (kgui_e5pat.c, apply_frame() while the camera works). */
    bool witnesses = false;
    switch ( p )
    {
        case KUI_TD_PAT_COLOUR:
            kui_td_fill(g, COLOURS[arg]);
            witnesses = (KUI_TD_COLOUR_BLACK != arg);
            break;
        case KUI_TD_PAT_GREY:
        {
            uint32_t const v = (uint32_t)arg;
            kui_td_fill(g, (v << GREY_R_SHIFT) | (v << GREY_G_SHIFT) | v);
            witnesses = true;
            break;
        }
        case KUI_TD_PAT_MARKS:
            /* Black, and the four white squares the locator finds (kgui_e5pat.c, K_MARKS). */
            kui_td_fill(g, KUI_TD_RGB_BLACK);
            kui_td_marks(g);
            break;
        case KUI_TD_PAT_OK:
            paint_icon(g, true);
            witnesses = true;
            break;
        case KUI_TD_PAT_FAIL:
            paint_icon(g, false);
            witnesses = true;
            break;
        case KUI_TD_PAT_LINES:
            paint_lines(g, COLOURS[arg]);
            witnesses = true;
            break;
        case KUI_TD_PAT_DOTS:
            paint_dots(g, COLOURS[arg]);
            witnesses = true;
            break;
        default:
            /* OFF and COUNT never reach here. */
            break;
    }
    if ( witnesses )
    {
        kui_td_witnesses(g, true);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; a kit call for apps, never used inside the SDK */
void kui_td_pattern_build(lv_obj_t *parent, kui_td_pattern_t p, uint8_t arg)
{
    bool const coloured = (KUI_TD_PAT_COLOUR == p) || (KUI_TD_PAT_LINES == p) || (KUI_TD_PAT_DOTS == p);
    if ( (NULL == parent) || ((uint32_t)KUI_TD_PAT_OFF <= (uint32_t)p) )
    {
        return;
    }
    if ( coloured && (KUI_TD_COLOUR_COUNT <= arg) )
    {
        return;
    }
    kui_td_canvas_t *c = kui_td_canvas_create(parent, KUI_TD_NAME_PATTERN, (uint8_t)p);
    if ( NULL == c )
    {
        return;
    }
    pattern_paint(&c->px, p, arg);
    kui_td_canvas_show(c);
}
