/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_test_dot_screens.c
 * @brief The E5 operator's screens, one per phase, as the station's kgui_e5op.c and kgui_e5pat.c paint them; no text.
 */
#include <string.h>

#include "kui_test_dot.h"
#include "kui_test_dot_paint.h"
#include "kui_tokens.h"

/* The colour rule, in the kit's palette. */
#define C_TURN     KUI_PALETTE_BOLD_YELLOW  /**< Amber: the operator's turn. */
#define C_HANDS    KUI_PALETTE_LIGHT_GREY   /**< Light grey: hands off. */
#define C_PASS     KUI_PALETTE_OK_GREEN     /**< Green: done, passed. */
#define C_FAIL     KUI_PALETTE_DANGER_RED   /**< Red: failed, and a prohibition. */
#define C_INK      KUI_PALETTE_PAGE         /**< The ink of a glyph on a lit fill. */
#define C_PENDING  KUI_PALETTE_REST_GREY    /**< A key or a cell not reached yet. */
#define C_ARROW    KUI_PALETTE_MEDIUM_GREY  /**< The arrow on a key not reached yet. */
#define C_HUB      KUI_PALETTE_OFF_GREY     /**< The middle of the pad. */
#define C_TOP_BTN  KUI_PALETTE_KODY_BLUE    /**< The top round button, blue as on the Kode. */
#define C_BOT_BTN  KUI_PALETTE_DANGER_RED   /**< The bottom round button, red as on the Kode. */

/* Motion. */
#define BLINK_MS          500U  /**< Half a period of the 1 Hz blink. */
#define CONNECT_MS        300U  /**< One frame of the connectors' loop. */
#define CONNECT_FRAMES    3U    /**< Frames in the loop. */
#define BLINK_PHASES      2U    /**< On, off. */

/* The connecting screen (kgui_e5pat.c, sc_connect()). */
#define KODE_X        215.0f  /**< The Kode's outline: centre x. */
#define KODE_Y        240.0f  /**< The Kode's outline: centre y. */
#define KODE_HX       38.0f   /**< Half its width. */
#define KODE_HY       62.0f   /**< Half its height. */
#define KODE_R        12.0f   /**< Its corner radius. */
#define KODE_OW       4.0f    /**< Its outline's width. */
#define GLASS_DY      28.0f   /**< The glass, above the centre. */
#define GLASS_HX      28.0f   /**< Half the glass' width. */
#define GLASS_HY      24.0f   /**< Half the glass' height. */
#define GLASS_R       8.0f    /**< The glass' corner radius. */
#define GLASS_OW      3.0f    /**< The glass' outline width. */
#define PAD_H_DX      24      /**< The drawn pad's bar: left of the centre. */
#define PAD_H_DY      20      /**< The drawn pad's bar: below the centre. */
#define PAD_H_W       14      /**< The drawn pad's bar: width. */
#define PAD_H_H       6       /**< The drawn pad's bar: height. */
#define PAD_V_DX      20      /**< The drawn pad's post: left of the centre. */
#define PAD_V_DY      16      /**< The drawn pad's post: below the centre. */
#define PAD_V_W       6       /**< The drawn pad's post: width. */
#define PAD_V_H       14      /**< The drawn pad's post: height. */
#define BTN1_DX       16.0f   /**< The drawn top button: right of the centre. */
#define BTN1_DY       18.0f   /**< The drawn top button: below the centre. */
#define BTN2_DX       24.0f   /**< The drawn bottom button: right of the centre. */
#define BTN2_DY       30.0f   /**< The drawn bottom button: below the centre. */
#define BTN_DRAWN_R   5.0f    /**< The drawn buttons' radius. */
#define CAM_GAP       12.0f   /**< The camera's plug: its gap over the top edge. */
#define CAM_PLUG_HW   14      /**< Half the camera plug's width. */
#define CAM_PLUG_H    14      /**< The camera plug's height. */
#define CAM_CABLE_HW  4       /**< Half the camera cable's width. */
#define CAM_CABLE_UP  40      /**< The camera cable's top, above the plug. */
#define CAM_CABLE_H   26      /**< The camera cable's length. */
#define SD_GAP        6.0f    /**< The microSD tab: its gap off the left edge. */
#define SD_TAB_DX     26      /**< The tab's left side, from its tip. */
#define SD_TAB_UP     10      /**< The tab's top, above the centre. */
#define SD_TAB_SIDE   22      /**< The tab's side. */
#define SD_CHAMFER    4.0f    /**< The tab's chamfer. */
#define SD_CH_LOW     8.0f    /**< The chamfer's lower corner, below the centre. */
#define SD_CH_FOOT    12.0f   /**< The chamfer's foot, below the centre. */
#define SD_CABLE_DX   52      /**< The tab's cable: its far end, from the tip. */
#define SD_CABLE_HH   3       /**< Half the tab cable's height. */
#define SD_CABLE_W    26      /**< The tab cable's length. */
#define USB_UP        8.0f    /**< The USB-C plug: its offset up from the bottom edge. */
#define USB_GAP       8.0f    /**< The USB-C plug: its gap under the bottom edge. */
#define USB_SLIDE     0.6f    /**< How much of a frame's slide the USB-C takes. */
#define USB_DY        7.0f    /**< The USB-C plug's centre, below its top. */
#define USB_HX        9.0f    /**< Half the USB-C plug's width. */
#define USB_HY        7.0f    /**< Half the USB-C plug's height. */
#define USB_R         3.0f    /**< The USB-C plug's radius. */
#define USB_CABLE_DY  14      /**< The USB-C cable's top, below the plug's top. */
#define USB_CABLE_HW  4       /**< Half the USB-C cable's width. */
#define USB_CABLE_H   26      /**< The USB-C cable's length. */

/* The buttons screen (kgui_e5pat.c, sc_pad()). */
#define PAD_X         125.0f  /**< The pad's centre x. */
#define PAD_Y         250.0f  /**< The pad's centre y. */
#define PAD_K         46.0f   /**< A key's side, and the step between keys. */
#define HUB_GROW      2.0f    /**< The hub's half side past a key's. */
#define HUB_R         6.0f    /**< The hub's corner radius. */
#define KEY_R         8.0f    /**< A key's corner radius. */
#define KEY_RING_GROW 7.0f    /**< The amber frame's half side past a key's. */
#define KEY_RING_R    12.0f   /**< The amber frame's corner radius. */
#define KEY_RING_OW   3.0f    /**< The amber frame's width. */
#define KEY_CHECK     0.7f    /**< A key's check, in key sides. */
#define KEY_CROSS     0.8f    /**< A key's cross, in key sides. */
#define KEY_GLYPH_W   6.0f    /**< A key's check and cross stroke. */
#define RETRY_R       13.0f   /**< The repeat glyph's radius. */
#define RETRY_W       5.0f    /**< The repeat glyph's stroke. */
#define ARROW_Q       10.0f   /**< A key's arrow: half its reach. */
#define ARROW_BACK    0.6f    /**< A key's arrow: its base, in reaches. */
#define ROUND_RING    7.0f    /**< A round button's ring, past its radius. */
#define ROUND_RING_HW 3.5f    /**< Half the ring's width. */
#define ROUND_CHECK   1.5f    /**< A round button's check, in radii. */
#define ROUND_CHECK_W 7.0f    /**< Its stroke. */
#define ROUND_CROSS   1.6f    /**< A round button's cross, in radii. */
#define ROUND_CROSS_W 6.0f    /**< Its strokes. */
#define DIM_NUM       6U      /**< A round button not reached yet: 60 % ... */
#define DIM_DEN       10U     /**< ... of its colour. */
#define ARROWS        4U      /**< The pad's four keys. */
#define RGB_R_SHIFT   16U     /**< Red's place in 0xRRGGBB. */
#define RGB_G_SHIFT   8U      /**< Green's place in 0xRRGGBB. */
#define RGB_BYTE      0xFFU   /**< One channel of 0xRRGGBB. */

/* The touch screen (kgui_e5pat.c, E5S_TOUCH): the board's targets of AO_Touch's test (kbsp_board.h). */
_Static_assert((KBSP_TOUCH_TEST_A_Y_PX > KUI_TD_BAND_Y0_PX) && (KBSP_TOUCH_TEST_A_Y_PX < KUI_TD_BAND_Y1_PX),
               "the first touch target leaves the band");
_Static_assert((KBSP_TOUCH_TEST_B_Y_PX > KUI_TD_BAND_Y0_PX) && (KBSP_TOUCH_TEST_B_Y_PX < KUI_TD_BAND_Y1_PX),
               "the second touch target leaves the band");
#define TARGET_RING   55.0f   /**< The amber ring's radius. */
#define TARGET_RING_HW 3.0f   /**< Half its width. */
#define TARGET_DOT    26.0f   /**< The amber dot's radius. */
#define TARGET_DONE   30.0f   /**< A done target's green disc. */
#define TARGET_CHECK  44.0f   /**< Its check. */
#define TARGET_CHECK_W 6.0f   /**< The check's stroke. */

/* The result (kgui_e5pat.c, E5S_PASS and E5S_FAIL). */
#define RESULT_X      205.0f  /**< The glyph's centre x. */
#define RESULT_Y      252.0f  /**< The glyph's centre y. */
#define RESULT_SIZE   170.0f  /**< The glyph's size. */
#define RESULT_W      22.0f   /**< Its stroke. */

/* The auto grid: one row of cells under hands off, inside the band. */
#define GRID_Y        362     /**< The row's centre y. */
#define GRID_PITCH    22      /**< From one cell to the next. */
#define GRID_R        7       /**< A cell's radius. */

_Static_assert((((KUI_TD_W_PX / 2) - ((((int32_t)KUI_TD_GRID_CELLS - 1) / 2) * GRID_PITCH)) - GRID_R) > 0,
               "the grid leaves the glass");
_Static_assert((GRID_Y + GRID_R + 1) < KUI_TD_BAND_Y1_PX, "the grid leaves the band");

/**
 * @brief The Kode's outline, the marks and three connectors coming in (kgui_e5pat.c, sc_connect()).
 * @param g     The pixels.
 * @param frame The frame of the loop.
 */
static void paint_connecting(const kui_td_px_t *g, uint32_t frame)
{
    static const float SLIDE[CONNECT_FRAMES] = { 30.0f, 12.0f, 0.0f };  /* the connectors' distance, frame by frame */
    float const off = SLIDE[frame % CONNECT_FRAMES];
    float const kx = KODE_X;
    float const ky = KODE_Y;
    kui_td_marks(g);
    kui_td_rrect(g, kui_td_pt(kx, ky), KODE_HX, KODE_HY, KODE_R, C_HANDS, KODE_OW);
    kui_td_rrect(g, kui_td_pt(kx, ky - GLASS_DY), GLASS_HX, GLASS_HY, GLASS_R, C_HANDS, GLASS_OW);
    kui_td_rect(g, (int32_t)kx - PAD_H_DX, (int32_t)ky + PAD_H_DY, PAD_H_W, PAD_H_H, C_HANDS);
    kui_td_rect(g, (int32_t)kx - PAD_V_DX, (int32_t)ky + PAD_V_DY, PAD_V_W, PAD_V_H, C_HANDS);
    kui_td_disc(g, kui_td_pt(kx + BTN1_DX, ky + BTN1_DY), BTN_DRAWN_R, C_HANDS);
    kui_td_disc(g, kui_td_pt(kx + BTN2_DX, ky + BTN2_DY), BTN_DRAWN_R, C_HANDS);
    /* The camera's connector, down onto the top edge. */
    float const ty = ((ky - KODE_HY) - CAM_GAP) - off;
    kui_td_rect(g, (int32_t)kx - CAM_PLUG_HW, (int32_t)ty - CAM_PLUG_H, CAM_PLUG_HW * 2, CAM_PLUG_H, C_TURN);
    kui_td_rect(g, (int32_t)kx - CAM_CABLE_HW, (int32_t)ty - CAM_CABLE_UP, CAM_CABLE_HW * 2, CAM_CABLE_H, C_TURN);
    /* The microSD tab, in by the left edge, with its chamfered corner. */
    float const sx = ((kx - KODE_HX) - SD_GAP) - off;
    kui_td_rect(g, (int32_t)sx - SD_TAB_DX, (int32_t)ky - SD_TAB_UP, SD_TAB_SIDE, SD_TAB_SIDE, C_TURN);
    kui_td_tri(g, kui_td_pt(sx - SD_CHAMFER, ky - (float)SD_TAB_UP), kui_td_pt(sx, ky - (float)SD_TAB_UP), kui_td_pt(sx, ky + SD_CH_LOW),
               C_TURN);
    kui_td_tri(g, kui_td_pt(sx - SD_CHAMFER, ky - (float)SD_TAB_UP), kui_td_pt(sx, ky + SD_CH_LOW), kui_td_pt(sx - SD_CHAMFER, ky + SD_CH_FOOT),
               C_TURN);
    kui_td_rect(g, (int32_t)sx - SD_CABLE_DX, (int32_t)ky - SD_CABLE_HH, SD_CABLE_W, SD_CABLE_HH * 2, C_TURN);
    /* The USB-C, in by the bottom: already there, so green. */
    float const uy = (((ky - USB_UP) + KODE_HY) + USB_GAP) + (off * USB_SLIDE);
    kui_td_rrect(g, kui_td_pt(kx, uy + USB_DY), USB_HX, USB_HY, USB_R, C_PASS, 0.0f);
    kui_td_rect(g, (int32_t)kx - USB_CABLE_HW, (int32_t)uy + USB_CABLE_DY, USB_CABLE_HW * 2, USB_CABLE_H, C_PASS);
}

/**
 * @brief A colour at 60 %, as a round button not reached yet.
 * @param rgb The colour.
 * @return The dimmed colour.
 */
static uint32_t dim(uint32_t rgb)
{
    uint32_t const r = ((((rgb >> RGB_R_SHIFT) & RGB_BYTE) * DIM_NUM) / DIM_DEN) << RGB_R_SHIFT;
    uint32_t const gr = ((((rgb >> RGB_G_SHIFT) & RGB_BYTE) * DIM_NUM) / DIM_DEN) << RGB_G_SHIFT;
    uint32_t const b = ((rgb & RGB_BYTE) * DIM_NUM) / DIM_DEN;
    return r | gr | b;
}

/**
 * @brief Whether a key or a button wears the amber of the operator's turn.
 * @param st Its state.
 * @return true when it is asked for now or again.
 */
static bool asked(uint8_t st)
{
    return (KUI_TD_ST_CURRENT == st) || (KUI_TD_ST_AGAIN == st);
}

/**
 * @brief The arrow of a key not reached yet or asked for now.
 * @param g   The pixels.
 * @param i   The key: up, down, left, right.
 * @param c   Its centre.
 * @param rgb The colour.
 */
static void arrow(const kui_td_px_t *g, uint32_t i, kui_td_pt_t c, uint32_t rgb)
{
    float const q = ARROW_Q;
    float const b = q * ARROW_BACK;
    if ( 0U == i )
    {
        kui_td_tri(g, kui_td_pt(c.x, c.y - q), kui_td_pt(c.x - q, c.y + b), kui_td_pt(c.x + q, c.y + b), rgb);
    }
    else if ( 1U == i )
    {
        kui_td_tri(g, kui_td_pt(c.x, c.y + q), kui_td_pt(c.x - q, c.y - b), kui_td_pt(c.x + q, c.y - b), rgb);
    }
    else if ( 2U == i )
    {
        kui_td_tri(g, kui_td_pt(c.x - q, c.y), kui_td_pt(c.x + b, c.y - q), kui_td_pt(c.x + b, c.y + q), rgb);
    }
    else
    {
        kui_td_tri(g, kui_td_pt(c.x + q, c.y), kui_td_pt(c.x - b, c.y - q), kui_td_pt(c.x - b, c.y + q), rgb);
    }
}

/**
 * @brief The pad's four keys around the hub.
 * @param g     The pixels.
 * @param st    The keys' states, up, down, left, right.
 * @param on    The blink is on.
 */
static void pad_keys(const kui_td_px_t *g, const uint8_t *st, bool on)
{
    static const float DX[ARROWS] = { 0.0f, 0.0f, -1.0f, 1.0f };  /* each key's step from the hub, x */
    static const float DY[ARROWS] = { -1.0f, 1.0f, 0.0f, 0.0f };  /* each key's step from the hub, y */
    float const half = PAD_K / 2.0f;
    kui_td_rrect(g, kui_td_pt(PAD_X, PAD_Y), half + HUB_GROW, half + HUB_GROW, HUB_R, C_HUB, 0.0f);
    for ( uint32_t i = 0U; i < ARROWS; i++ )
    {
        kui_td_pt_t const c = kui_td_pt(PAD_X + (DX[i] * PAD_K), PAD_Y + (DY[i] * PAD_K));
        uint32_t fill = C_PENDING;
        if ( asked(st[i]) )
        {
            fill = C_TURN;
            if ( on )
            {
                kui_td_rrect(g, c, half + KEY_RING_GROW, half + KEY_RING_GROW, KEY_RING_R, C_TURN, KEY_RING_OW);
            }
        }
        else if ( KUI_TD_ST_OK == st[i] )
        {
            fill = C_PASS;
        }
        else if ( KUI_TD_ST_FAILED == st[i] )
        {
            fill = C_FAIL;
        }
        else
        {
            /* Pending. */
        }
        kui_td_rrect(g, c, half, half, KEY_R, fill, 0.0f);
        if ( KUI_TD_ST_OK == st[i] )
        {
            kui_td_glyph_check(g, c, PAD_K * KEY_CHECK, KEY_GLYPH_W, C_INK);
        }
        else if ( KUI_TD_ST_AGAIN == st[i] )
        {
            kui_td_glyph_retry(g, c, RETRY_R, RETRY_W, C_INK);
        }
        else if ( KUI_TD_ST_FAILED == st[i] )
        {
            kui_td_glyph_cross(g, c, PAD_K * KEY_CROSS, KEY_GLYPH_W, C_INK);
        }
        else if ( KUI_TD_ST_CURRENT == st[i] )
        {
            arrow(g, i, c, C_INK);
        }
        else
        {
            arrow(g, i, c, C_ARROW);
        }
    }
}

/**
 * @brief The two round buttons where they sit on the Kode, each in its own colour.
 * @param g     The pixels.
 * @param st    Their states, top then bottom.
 * @param on    The blink is on.
 */
static void pad_buttons(const kui_td_px_t *g, const uint8_t *st, bool on)
{
    static const float BX[2] = { 345.0f, 295.0f };         /* centre x, top then bottom */
    static const float BY[2] = { 215.0f, 290.0f };         /* centre y */
    static const float BR[2] = { 34.0f, 30.0f };           /* radius */
    static const uint32_t COLOUR[2] = { C_TOP_BTN, C_BOT_BTN };
    for ( uint32_t j = 0U; j < 2U; j++ )
    {
        kui_td_pt_t const c = kui_td_pt(BX[j], BY[j]);
        float const rr = BR[j];
        if ( asked(st[j]) )
        {
            if ( on )
            {
                kui_td_ring(g, c, rr + ROUND_RING, ROUND_RING_HW, C_TURN);
            }
            kui_td_disc(g, c, rr, COLOUR[j]);
            if ( KUI_TD_ST_AGAIN == st[j] )
            {
                kui_td_glyph_retry(g, c, RETRY_R, RETRY_W, C_HANDS);
            }
        }
        else if ( KUI_TD_ST_OK == st[j] )
        {
            kui_td_ring(g, c, rr + ROUND_RING, ROUND_RING_HW, C_PASS);
            kui_td_disc(g, c, rr, COLOUR[j]);
            kui_td_glyph_check(g, c, rr * ROUND_CHECK, ROUND_CHECK_W, C_HANDS);
        }
        else if ( KUI_TD_ST_FAILED == st[j] )
        {
            kui_td_disc(g, c, rr, C_FAIL);
            kui_td_glyph_cross(g, c, rr * ROUND_CROSS, ROUND_CROSS_W, C_INK);
        }
        else
        {
            kui_td_disc(g, c, rr, dim(COLOUR[j]));
        }
    }
}

/**
 * @brief The touch test's targets: a done one green with a check, the one asked amber with its ring.
 * @param g  The pixels.
 * @param st The targets' states.
 * @param on The blink is on.
 */
static void paint_touch(const kui_td_px_t *g, const uint8_t *st, bool on)
{
    static const kui_td_pt_t AT[KUI_TD_TARGETS] = {
        { (float)KBSP_TOUCH_TEST_A_X_PX, (float)KBSP_TOUCH_TEST_A_Y_PX },
        { (float)KBSP_TOUCH_TEST_B_X_PX, (float)KBSP_TOUCH_TEST_B_Y_PX },
    };
    for ( uint32_t i = 0U; i < KUI_TD_TARGETS; i++ )
    {
        if ( KUI_TD_ST_OK == st[i] )
        {
            kui_td_disc(g, AT[i], TARGET_DONE, C_PASS);
            kui_td_glyph_check(g, AT[i], TARGET_CHECK, TARGET_CHECK_W, C_INK);
        }
    }
    for ( uint32_t i = 0U; i < KUI_TD_TARGETS; i++ )
    {
        if ( KUI_TD_ST_CURRENT == st[i] )
        {
            if ( on )
            {
                kui_td_ring(g, AT[i], TARGET_RING, TARGET_RING_HW, C_TURN);
            }
            kui_td_disc(g, AT[i], TARGET_DOT, C_TURN);
        }
    }
}

/**
 * @brief A whole band of green with a check, or of red with a cross.
 * @param g    The pixels.
 * @param pass true for PASS.
 */
static void paint_result(const kui_td_px_t *g, bool pass)
{
    if ( pass )
    {
        kui_td_rect(g, 0, KUI_TD_BAND_Y0_PX, KUI_TD_W_PX, KUI_TD_BAND_Y1_PX - KUI_TD_BAND_Y0_PX, C_PASS);
        kui_td_glyph_check(g, kui_td_pt(RESULT_X, RESULT_Y), RESULT_SIZE, RESULT_W, C_INK);
    }
    else
    {
        kui_td_rect(g, 0, KUI_TD_BAND_Y0_PX, KUI_TD_W_PX, KUI_TD_BAND_Y1_PX - KUI_TD_BAND_Y0_PX, C_FAIL);
        kui_td_glyph_cross(g, kui_td_pt(RESULT_X, RESULT_Y), RESULT_SIZE, RESULT_W, C_INK);
    }
}

/**
 * @brief The centre of a grid cell.
 * @param idx The cell.
 * @return Its centre.
 */
static kui_td_pt_t cell_at(uint32_t idx)
{
    int32_t const mid = ((int32_t)KUI_TD_GRID_CELLS - 1) / 2;
    int32_t const x = (KUI_TD_W_PX / 2) + (((int32_t)idx - mid) * GRID_PITCH);
    return kui_td_pt((float)x, (float)GRID_Y);
}

/**
 * @brief The colour of a grid cell's state; an unknown one is pending.
 * @param st The state.
 * @return The colour.
 */
static uint32_t cell_colour(uint8_t st)
{
    uint32_t rgb = C_PENDING;
    if ( KUI_TD_ST_RUNNING == st )
    {
        rgb = C_HANDS;
    }
    else if ( KUI_TD_ST_OK == st )
    {
        rgb = C_PASS;
    }
    else if ( KUI_TD_ST_FAILED == st )
    {
        rgb = C_FAIL;
    }
    else
    {
        /* Pending, or a byte the grid does not know. */
    }
    return rgb;
}

/**
 * @brief One grid cell over black: what a whole build would leave there.
 * @param g   The pixels.
 * @param idx The cell.
 * @param st  Its state.
 */
static void paint_cell(const kui_td_px_t *g, uint32_t idx, uint8_t st)
{
    kui_td_pt_t const c = cell_at(idx);
    /* The disc touches one pixel past its radius on each side. */
    int32_t const reach = GRID_R + 1;
    kui_td_rect(g, (int32_t)c.x - reach, (int32_t)c.y - reach, (reach * 2) + 1, (reach * 2) + 1, KUI_TD_RGB_BLACK);
    kui_td_disc(g, c, (float)GRID_R, cell_colour(st));
}

/**
 * @brief Paint a screen from what its canvas holds.
 * @param c The canvas.
 */
static void screen_paint(kui_td_canvas_t *c)
{
    const kui_td_px_t *g = &c->px;
    bool const on = (0U == (c->step % BLINK_PHASES));
    kui_td_fill(g, KUI_TD_RGB_BLACK);
    switch ( (kui_td_screen_t)c->kind )
    {
        case KUI_TD_SCREEN_CONNECTING:
            paint_connecting(g, c->step);
            break;
        case KUI_TD_SCREEN_AUTO:
            kui_td_glyph_no_touch(g, C_HANDS, C_FAIL);
            for ( uint32_t i = 0U; i < KUI_TD_GRID_CELLS; i++ )
            {
                paint_cell(g, i, c->arg[i]);
            }
            break;
        case KUI_TD_SCREEN_PADS:
        case KUI_TD_SCREEN_CAMERA:
            kui_td_witnesses(g, false);
            break;
        case KUI_TD_SCREEN_BUTTONS:
            pad_keys(g, c->arg, on);
            pad_buttons(g, &c->arg[ARROWS], on);
            break;
        case KUI_TD_SCREEN_TOUCH:
            paint_touch(g, c->arg, on);
            break;
        case KUI_TD_SCREEN_RESULT:
            paint_result(g, KUI_TD_ST_OK == c->arg[0]);
            break;
        default:
            /* LED: black, so the camera sees the LED alone. */
            break;
    }
}

/**
 * @brief The screen's timer: the next frame of the loop, or the blink's other half.
 * @param t The timer; its user data is the canvas.
 */
static void on_tick(lv_timer_t *t)
{
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands back the canvas this file gave the timer */
    kui_td_canvas_t *c = (kui_td_canvas_t *)lv_timer_get_user_data(t);
    c->step++;
    screen_paint(c);
    kui_td_canvas_show(c);
}

/**
 * @brief How often a screen moves.
 * @param c The canvas, painted.
 * @return The period, or 0 for a screen that stays still.
 */
static uint32_t motion_ms(const kui_td_canvas_t *c)
{
    uint32_t ms = 0U;
    if ( (uint8_t)KUI_TD_SCREEN_CONNECTING == c->kind )
    {
        ms = CONNECT_MS;
    }
    else if ( (uint8_t)KUI_TD_SCREEN_BUTTONS == c->kind )
    {
        for ( uint32_t i = 0U; i < KUI_TD_PAD_KEYS; i++ )
        {
            if ( asked(c->arg[i]) )
            {
                ms = BLINK_MS;
            }
        }
    }
    else if ( (uint8_t)KUI_TD_SCREEN_TOUCH == c->kind )
    {
        for ( uint32_t i = 0U; i < KUI_TD_TARGETS; i++ )
        {
            if ( KUI_TD_ST_CURRENT == c->arg[i] )
            {
                ms = BLINK_MS;
            }
        }
    }
    else
    {
        /* The rest stay still. */
    }
    return ms;
}

/* cppcheck-suppress misra-c2012-8.7 ; a kit call for apps, never used inside the SDK */
void kui_td_screen_build(lv_obj_t *parent, kui_td_screen_t s, const uint8_t *arg, size_t arg_len)
{
    if ( (NULL == parent) || ((uint32_t)KUI_TD_SCREEN_COUNT <= (uint32_t)s) )
    {
        return;
    }
    kui_td_canvas_t *c = kui_td_canvas_create(parent, KUI_TD_NAME_SCREEN, (uint8_t)s);
    if ( NULL == c )
    {
        return;
    }
    size_t n = arg_len;
    if ( NULL == arg )
    {
        n = 0U;
    }
    if ( n > sizeof(c->arg) )
    {
        n = sizeof(c->arg);
    }
    if ( 0U != n )
    {
        (void)memcpy(c->arg, arg, n);
    }
    else if ( KUI_TD_SCREEN_TOUCH == s )
    {
        /* With no states, the touch screen asks for its first target. */
        c->arg[0] = KUI_TD_ST_CURRENT;
    }
    else
    {
        /* Every other screen starts all pending. */
    }
    screen_paint(c);
    kui_td_canvas_show(c);
    uint32_t const ms = motion_ms(c);
    if ( 0U != ms )
    {
        kui_td_canvas_animate(c, ms, on_tick);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; a kit call for apps, never used inside the SDK */
/* cppcheck-suppress constParameterPointer ; the screen changes: its canvas is repainted */
void kui_td_grid_set(lv_obj_t *screen, uint8_t idx, uint8_t state)
{
    kui_td_canvas_t *c = kui_td_canvas_find(screen, KUI_TD_NAME_SCREEN);
    bool const known = (KUI_TD_ST_PENDING == state) || (KUI_TD_ST_RUNNING == state) || (KUI_TD_ST_OK == state)
                       || (KUI_TD_ST_FAILED == state);
    if ( (NULL == c) || ((uint8_t)KUI_TD_SCREEN_AUTO != c->kind) || (KUI_TD_GRID_CELLS <= idx) || (!known) )
    {
        return;
    }
    c->arg[idx] = state;
    paint_cell(&c->px, idx, state);
    kui_td_canvas_show(c);
}
