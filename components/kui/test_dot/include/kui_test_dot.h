/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_test_dot.h
 * @brief The E5 final test on the glass: the operator's screens and the patterns the camera shoots.
 */
#ifndef KUI_TEST_DOT_H
#define KUI_TEST_DOT_H

#include <stddef.h>
#include <stdint.h>

#include "kbsp_board.h"
#include "lvgl.h"

/* The station's analysis judges photos of these: their geometry and colours never move,
 * and none follows the kit's palette. */
/* ---- The glass ---- */

#define KUI_TD_W_PX          ((int32_t)KBSP_LCD_H_RES) /**< Canvas width: the whole panel. */
#define KUI_TD_H_PX          ((int32_t)KBSP_LCD_V_RES) /**< Canvas height: the whole panel. */
#define KUI_TD_BAND_Y0_PX    125                       /**< The free band of the protector: first row. */
#define KUI_TD_BAND_Y1_PX    380                       /**< The free band of the protector: the row past its end. */

/* ---- The marks: the locator's four white squares (the analysis' an_mark_centres()) ---- */

#define KUI_TD_MARKS            4U   /**< How many marks. */
#define KUI_TD_MARK_SIZE_PX     24   /**< Side of a mark. */
#define KUI_TD_MARK_LEFT_X_PX   42   /**< Centre x of the two left marks. */
#define KUI_TD_MARK_RIGHT_X_PX  368  /**< Centre x of the two right marks. */
#define KUI_TD_MARK_TOP_Y_PX    167  /**< Centre y of the two top marks. */
#define KUI_TD_MARK_BOTTOM_Y_PX 338  /**< Centre y of the two bottom marks. */
#define KUI_TD_MARK_RGB         0xFFFFFFU /**< A mark's colour. */

/* ---- The witnesses: four red dots outside the band while the camera photographs ---- */

#define KUI_TD_WITNESSES          4U        /**< How many witnesses. */
#define KUI_TD_WITNESS_TOP_Y_PX   72        /**< Centre y of the two top witnesses. */
#define KUI_TD_WITNESS_BOT_Y_PX   405       /**< Centre y of the two bottom witnesses. */
#define KUI_TD_WITNESS_TL_X_PX    50        /**< Centre x of the top left witness. */
#define KUI_TD_WITNESS_TR_X_PX    360       /**< Centre x of the top right witness. */
#define KUI_TD_WITNESS_BL_X_PX    55        /**< Centre x of the bottom left witness. */
#define KUI_TD_WITNESS_BR_X_PX    355       /**< Centre x of the bottom right witness. */
#define KUI_TD_WITNESS_R_PX       15.0f     /**< A witness' radius. */
#define KUI_TD_WITNESS_RIM_R_PX   20.0f     /**< Its black rim on a pattern, so it reads on white. */
#define KUI_TD_WITNESS_RGB        0xE12D2DU /**< A witness' red. */

/* ---- The colours a colour pattern takes, by index ---- */

#define KUI_TD_COLOUR_BLACK  0U  /**< Black: the dark photo. */
/* cppcheck-suppress misra-c2012-2.5 ; drawn by index in the kit's desktop simulator (sim/), which the firmware build does not compile */
#define KUI_TD_COLOUR_WHITE  1U  /**< White. */
/* cppcheck-suppress misra-c2012-2.5 ; drawn by index in the kit's desktop simulator (sim/), which the firmware build does not compile */
#define KUI_TD_COLOUR_RED    2U  /**< Full red. */
/* cppcheck-suppress misra-c2012-2.5 ; drawn by index in the kit's desktop simulator (sim/), which the firmware build does not compile */
#define KUI_TD_COLOUR_GREEN  3U  /**< Full green. */
/* cppcheck-suppress misra-c2012-2.5 ; drawn by index in the kit's desktop simulator (sim/), which the firmware build does not compile */
#define KUI_TD_COLOUR_BLUE   4U  /**< Full blue. */
#define KUI_TD_COLOUR_COUNT  5U  /**< How many colours; an index at or past it draws nothing. */

#define KUI_TD_RGB_BLACK  0x000000U /**< KUI_TD_COLOUR_BLACK. */
#define KUI_TD_RGB_WHITE  0xFFFFFFU /**< KUI_TD_COLOUR_WHITE. */
#define KUI_TD_RGB_RED    0xFF0000U /**< KUI_TD_COLOUR_RED. */
#define KUI_TD_RGB_GREEN  0x00FF00U /**< KUI_TD_COLOUR_GREEN. */
#define KUI_TD_RGB_BLUE   0x0000FFU /**< KUI_TD_COLOUR_BLUE. */

/* cppcheck-suppress misra-c2012-2.5 ; the grey the kit's desktop simulator (sim/) shows, which the firmware build does not compile */
#define KUI_TD_GREY_RUN   128U      /**< The grey level the run photographs (the analysis' golden tables). */

#define KUI_TD_RGB_ICON_OK    0x31C45AU /**< The OK pattern's disc (kgui_e5pat.c, result_icon()). */
#define KUI_TD_RGB_ICON_FAIL  0xC9303DU /**< The FAIL pattern's disc (kgui_e5pat.c, result_icon()). */
#define KUI_TD_RGB_ICON_INK   0x191919U /**< The check and the cross drawn on those discs. */

/* ---- The lines and dots patterns (bench patterns; their lit colour is an index above) ---- */

#define KUI_TD_LINES_ON_PX      1   /**< Lit rows in each period. */
#define KUI_TD_LINES_PERIOD_PX  2   /**< Rows in a period: every other row lit. */
#define KUI_TD_DOTS_PITCH_PX    10  /**< One dot every this many pixels, in both axes. */
#define KUI_TD_DOTS_SIZE_PX     1   /**< A dot's side. */

/* ---- The result icons of the ok and fail patterns ---- */

#define KUI_TD_ICON_R_PX        62.0f  /**< The disc's radius, centred in the band. */
#define KUI_TD_ICON_STROKE_PX   7.0f   /**< Half the width of the check's and the cross' strokes. */

/* ---- The states a screen's arg carries ---- */

#define KUI_TD_ST_PENDING  ((uint8_t)'p') /**< Not reached yet. */
#define KUI_TD_ST_RUNNING  ((uint8_t)'r') /**< The auto grid: running now. */
#define KUI_TD_ST_CURRENT  ((uint8_t)'c') /**< Buttons and touch: the one asked for now. */
#define KUI_TD_ST_AGAIN    ((uint8_t)'r') /**< Buttons: the same one again, after a wrong press. */
#define KUI_TD_ST_OK       ((uint8_t)'o') /**< Done and passed; the result's PASS. */
#define KUI_TD_ST_FAILED   ((uint8_t)'x') /**< Failed; the result's FAIL. */

#define KUI_TD_GRID_CELLS   17U  /**< The auto screen: one cell per automatic row. */
#define KUI_TD_PAD_KEYS     6U   /**< The buttons screen: up, down, left, right, top, bottom. */
#define KUI_TD_TARGETS      2U   /**< The touch screen: the touch test's two targets. */
#define KUI_TD_ARG_MAX      KUI_TD_GRID_CELLS /**< The longest arg a screen reads. */

/* ---- The builders ---- */

/** One screen per phase of the run. */
typedef enum {
    KUI_TD_SCREEN_CONNECTING, /**< The connectors going in, in a loop, and the marks. */
    KUI_TD_SCREEN_AUTO,       /**< Hands off, and the grid of the automatic rows. */
    KUI_TD_SCREEN_PADS,       /**< Black with the four witnesses. */
    KUI_TD_SCREEN_CAMERA,     /**< Black with the four witnesses, between two patterns. */
    KUI_TD_SCREEN_LED,        /**< Black: the camera looks at the LED. */
    KUI_TD_SCREEN_BUTTONS,    /**< The pad and the two round buttons where they sit. */
    KUI_TD_SCREEN_TOUCH,      /**< The touch test's targets. */
    KUI_TD_SCREEN_RESULT,     /**< A green band and a check, or a red band and a cross. */
    KUI_TD_SCREEN_COUNT       /**< How many screens; never built. */
} kui_td_screen_t;

/** One pattern the camera photographs. */
typedef enum {
    KUI_TD_PAT_COLOUR,  /**< A solid colour, by index; witnesses unless black. */
    KUI_TD_PAT_GREY,    /**< A solid grey of the given level, with witnesses. */
    KUI_TD_PAT_MARKS,   /**< Black and the four marks. */
    KUI_TD_PAT_OK,      /**< A green disc with a check, in the band, on black, with witnesses. */
    KUI_TD_PAT_FAIL,    /**< A red disc with a cross, in the band, on black, with witnesses. */
    KUI_TD_PAT_LINES,   /**< Lit rows on black, in a colour by index, with witnesses. */
    KUI_TD_PAT_DOTS,    /**< A lattice of dots on black, in a colour by index, with witnesses. */
    KUI_TD_PAT_OFF,     /**< Nothing: the host takes its pattern away. */
    KUI_TD_PAT_COUNT    /**< How many patterns; never built. */
} kui_td_pattern_t;

/**
 * @brief Build one operator screen as a canvas on @p parent.
 *
 * The arg is one byte per element, KUI_TD_ST_*; an element it does not reach,
 * or a byte it does not know, reads pending. AUTO: one byte per grid cell
 * (pending, running, ok, failed). BUTTONS: one byte per key, in the order of
 * KUI_TD_PAD_KEYS (pending, current, again, ok, failed); none is all pending.
 * TOUCH: one byte per target (pending, current, ok); none is the first one
 * current. RESULT: one byte, KUI_TD_ST_OK is PASS and anything else FAIL. The
 * other screens ignore it. Amber parts blink at 1 Hz and the connectors move,
 * on a timer the canvas owns and deletes with itself.
 * @param parent  Where the canvas goes; nothing else is touched. NULL builds nothing.
 * @param s       The screen; KUI_TD_SCREEN_COUNT or more builds nothing.
 * @param arg     The states, or NULL.
 * @param arg_len How many bytes @p arg holds.
 * @return Nothing: a canvas that cannot be made leaves @p parent as it was.
 */
void kui_td_screen_build(lv_obj_t *parent, kui_td_screen_t s, const uint8_t *arg, size_t arg_len);

/**
 * @brief Build one pattern as a canvas on @p parent.
 * @param parent Where the canvas goes; nothing else is touched. NULL builds nothing.
 * @param p      The pattern; KUI_TD_PAT_COUNT or more builds nothing.
 * @param arg    COLOUR, LINES and DOTS: a colour index (KUI_TD_COLOUR_*); GREY: the level, 0 to 255; the rest ignore it.
 * @return Nothing: a canvas that cannot be made leaves @p parent as it was.
 */
void kui_td_pattern_build(lv_obj_t *parent, kui_td_pattern_t p, uint8_t arg);

/**
 * @brief Change one cell of an auto screen without building it again.
 * @param screen The parent an auto screen was built on.
 * @param idx    The cell, below KUI_TD_GRID_CELLS.
 * @param state  KUI_TD_ST_PENDING, _RUNNING, _OK or _FAILED.
 * @return Nothing: any other screen, cell or state changes nothing.
 */
void kui_td_grid_set(lv_obj_t *screen, uint8_t idx, uint8_t state);

#endif /* KUI_TEST_DOT_H */
