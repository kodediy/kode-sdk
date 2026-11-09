/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_td_sim.c
 * @brief The E5 test's screens and patterns as the simulator's screens, one name each.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "kui_sim.h"
#include "kui_test_dot.h"

/** One screen of the simulator: a builder's request. */
typedef struct {
    bool pattern;          /**< A pattern, not a screen. */
    uint8_t kind;          /**< kui_td_screen_t or kui_td_pattern_t. */
    const char *arg;       /**< A screen's states, or NULL. */
    uint8_t value;         /**< A pattern's arg. */
} td_view_t;

/**
 * @brief Build one view on a screen of its own.
 * @param v The view.
 * @return The screen.
 */
static lv_obj_t *build(const td_view_t *v)
{
    /* The timers before the build: any other one is the screen's own motion. */
    lv_timer_t *last = lv_timer_get_next(NULL);
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    if ( v->pattern )
    {
        kui_td_pattern_build(scr, (kui_td_pattern_t)v->kind, v->value);
    }
    else
    {
        size_t len = 0U;
        if ( NULL != v->arg )
        {
            len = strlen(v->arg);
        }
        kui_td_screen_build(scr, (kui_td_screen_t)v->kind, (const uint8_t *)v->arg, len);
    }
    /* A still picture is the first frame: LVGL puts a new timer first, so pause those before the old head. */
    lv_timer_t *t = lv_timer_get_next(NULL);
    while ( (NULL != t) && (t != last) )
    {
        lv_timer_pause(t);
        t = lv_timer_get_next(t);
    }
    return scr;
}

/** Every view, by the name the command line uses. */
static const td_view_t V_CONNECTING = { false, KUI_TD_SCREEN_CONNECTING, NULL, 0U };            /**< connecting */
static const td_view_t V_AUTO = { false, KUI_TD_SCREEN_AUTO, NULL, 0U };                        /**< auto, all pending */
static const td_view_t V_AUTO_RUN = { false, KUI_TD_SCREEN_AUTO, "oooooxorppppppppp", 0U };     /**< auto, mid-run */
static const td_view_t V_PADS = { false, KUI_TD_SCREEN_PADS, NULL, 0U };                        /**< pads */
static const td_view_t V_CAMERA = { false, KUI_TD_SCREEN_CAMERA, NULL, 0U };                    /**< camera */
static const td_view_t V_LED = { false, KUI_TD_SCREEN_LED, NULL, 0U };                          /**< led */
static const td_view_t V_BUTTONS = { false, KUI_TD_SCREEN_BUTTONS, NULL, 0U };                  /**< buttons, nothing asked */
static const td_view_t V_BUTTONS_STEP = { false, KUI_TD_SCREEN_BUTTONS, "oocprp", 0U };         /**< buttons, mid-test */
static const td_view_t V_BUTTONS_FAIL = { false, KUI_TD_SCREEN_BUTTONS, "ooxooo", 0U };         /**< buttons, done with one failed */
static const td_view_t V_TOUCH = { false, KUI_TD_SCREEN_TOUCH, NULL, 0U };                      /**< touch, first target */
static const td_view_t V_TOUCH_SECOND = { false, KUI_TD_SCREEN_TOUCH, "oc", 0U };               /**< touch, second target */
static const td_view_t V_PASS = { false, KUI_TD_SCREEN_RESULT, "o", 0U };                       /**< result, PASS */
static const td_view_t V_FAIL = { false, KUI_TD_SCREEN_RESULT, "x", 0U };                       /**< result, FAIL */
static const td_view_t V_P_BLACK = { true, KUI_TD_PAT_COLOUR, NULL, KUI_TD_COLOUR_BLACK };      /**< pattern: black */
static const td_view_t V_P_WHITE = { true, KUI_TD_PAT_COLOUR, NULL, KUI_TD_COLOUR_WHITE };      /**< pattern: white */
static const td_view_t V_P_RED = { true, KUI_TD_PAT_COLOUR, NULL, KUI_TD_COLOUR_RED };          /**< pattern: red */
static const td_view_t V_P_GREEN = { true, KUI_TD_PAT_COLOUR, NULL, KUI_TD_COLOUR_GREEN };      /**< pattern: green */
static const td_view_t V_P_BLUE = { true, KUI_TD_PAT_COLOUR, NULL, KUI_TD_COLOUR_BLUE };        /**< pattern: blue */
static const td_view_t V_P_GREY = { true, KUI_TD_PAT_GREY, NULL, KUI_TD_GREY_RUN };             /**< pattern: grey 128 */
static const td_view_t V_P_MARKS = { true, KUI_TD_PAT_MARKS, NULL, 0U };                        /**< pattern: marks */
static const td_view_t V_P_OK = { true, KUI_TD_PAT_OK, NULL, 0U };                              /**< pattern: ok */
static const td_view_t V_P_FAIL = { true, KUI_TD_PAT_FAIL, NULL, 0U };                          /**< pattern: fail */
static const td_view_t V_P_LINES = { true, KUI_TD_PAT_LINES, NULL, KUI_TD_COLOUR_WHITE };       /**< pattern: lines */
static const td_view_t V_P_DOTS = { true, KUI_TD_PAT_DOTS, NULL, KUI_TD_COLOUR_WHITE };         /**< pattern: dots */
static const td_view_t V_P_OFF = { true, KUI_TD_PAT_OFF, NULL, 0U };                            /**< pattern: off */

/** @brief connecting @return The screen. */
static lv_obj_t *get_connecting(void) { return build(&V_CONNECTING); }
/** @brief auto @return The screen. */
static lv_obj_t *get_auto(void) { return build(&V_AUTO); }
/** @brief auto_run @return The screen. */
static lv_obj_t *get_auto_run(void) { return build(&V_AUTO_RUN); }
/** @brief pads @return The screen. */
static lv_obj_t *get_pads(void) { return build(&V_PADS); }
/** @brief camera @return The screen. */
static lv_obj_t *get_camera(void) { return build(&V_CAMERA); }
/** @brief led @return The screen. */
static lv_obj_t *get_led(void) { return build(&V_LED); }
/** @brief buttons @return The screen. */
static lv_obj_t *get_buttons(void) { return build(&V_BUTTONS); }
/** @brief buttons_step @return The screen. */
static lv_obj_t *get_buttons_step(void) { return build(&V_BUTTONS_STEP); }
/** @brief buttons_fail @return The screen. */
static lv_obj_t *get_buttons_fail(void) { return build(&V_BUTTONS_FAIL); }
/** @brief touch @return The screen. */
static lv_obj_t *get_touch(void) { return build(&V_TOUCH); }
/** @brief touch_second @return The screen. */
static lv_obj_t *get_touch_second(void) { return build(&V_TOUCH_SECOND); }
/** @brief result_pass @return The screen. */
static lv_obj_t *get_pass(void) { return build(&V_PASS); }
/** @brief result_fail @return The screen. */
static lv_obj_t *get_fail(void) { return build(&V_FAIL); }
/** @brief pat_black @return The screen. */
static lv_obj_t *get_p_black(void) { return build(&V_P_BLACK); }
/** @brief pat_white @return The screen. */
static lv_obj_t *get_p_white(void) { return build(&V_P_WHITE); }
/** @brief pat_red @return The screen. */
static lv_obj_t *get_p_red(void) { return build(&V_P_RED); }
/** @brief pat_green @return The screen. */
static lv_obj_t *get_p_green(void) { return build(&V_P_GREEN); }
/** @brief pat_blue @return The screen. */
static lv_obj_t *get_p_blue(void) { return build(&V_P_BLUE); }
/** @brief pat_grey @return The screen. */
static lv_obj_t *get_p_grey(void) { return build(&V_P_GREY); }
/** @brief pat_marks @return The screen. */
static lv_obj_t *get_p_marks(void) { return build(&V_P_MARKS); }
/** @brief pat_ok @return The screen. */
static lv_obj_t *get_p_ok(void) { return build(&V_P_OK); }
/** @brief pat_fail @return The screen. */
static lv_obj_t *get_p_fail(void) { return build(&V_P_FAIL); }
/** @brief pat_lines @return The screen. */
static lv_obj_t *get_p_lines(void) { return build(&V_P_LINES); }
/** @brief pat_dots @return The screen. */
static lv_obj_t *get_p_dots(void) { return build(&V_P_DOTS); }
/** @brief pat_off @return The screen. */
static lv_obj_t *get_p_off(void) { return build(&V_P_OFF); }

const kui_sim_screen_t kui_sim_screens[] = {
    { "connecting", get_connecting },     { "auto", get_auto },
    { "auto_run", get_auto_run },         { "pads", get_pads },
    { "camera", get_camera },             { "led", get_led },
    { "buttons", get_buttons },           { "buttons_step", get_buttons_step },
    { "buttons_fail", get_buttons_fail }, { "touch", get_touch },
    { "touch_second", get_touch_second }, { "result_pass", get_pass },
    { "result_fail", get_fail },          { "pat_black", get_p_black },
    { "pat_white", get_p_white },         { "pat_red", get_p_red },
    { "pat_green", get_p_green },         { "pat_blue", get_p_blue },
    { "pat_grey", get_p_grey },           { "pat_marks", get_p_marks },
    { "pat_ok", get_p_ok },               { "pat_fail", get_p_fail },
    { "pat_lines", get_p_lines },         { "pat_dots", get_p_dots },
    { "pat_off", get_p_off },
};

const uint32_t kui_sim_screen_count = (uint32_t)(sizeof(kui_sim_screens) / sizeof(kui_sim_screens[0]));

void kui_sim_setup(void)
{
}

bool kui_sim_set(const char *subject, const char *value)
{
    (void)subject;
    (void)value;
    return false;
}
