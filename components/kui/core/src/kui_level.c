/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_level.c
 * @brief The level: a slider the finger drags, from 0 to KUI_LEVEL_MAX.
 */
#include "kui.h"

/** The track: the level's empty length. */
static lv_style_t s_track;
/** The fill: the level's full length, in the caller's colour. */
static lv_style_t s_fill;
/** The knob the finger holds. */
static lv_style_t s_knob;

/** @brief Initialise the three level styles, once. */
static void level_styles_init(void)
{
    lv_style_init(&s_track);
    lv_style_init(&s_fill);
    lv_style_init(&s_knob);
    lv_style_set_radius(&s_track, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_track, KUI_COLOR_REST);
    lv_style_set_bg_opa(&s_track, LV_OPA_COVER);
    lv_style_set_radius(&s_fill, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_fill, LV_OPA_COVER);
    lv_style_set_radius(&s_knob, LV_RADIUS_CIRCLE);
    lv_style_set_bg_color(&s_knob, KUI_COLOR_TEXT_ON_BG);
    lv_style_set_bg_opa(&s_knob, LV_OPA_COVER);
    lv_style_set_pad_all(&s_knob, KUI_LEVEL_KNOB_PAD);
}

/* cppcheck-suppress misra-c2012-8.7 ; a kit call for apps, never used inside the SDK */
lv_obj_t *kui_level_create(lv_obj_t *parent, lv_color_t colour)
{
    /* Set once the three styles are initialised. */
    static bool s_ready = false;

    if ( !s_ready )
    {
        level_styles_init();
        s_ready = true;
    }
    lv_obj_t *level = lv_slider_create(parent);
    lv_obj_set_name_static(level, "level_#");
    lv_obj_remove_style_all(level);
    lv_obj_set_size(level, KUI_PILL_W, KUI_LEVEL_H);
    lv_obj_add_style(level, &s_track, LV_PART_MAIN);
    lv_obj_add_style(level, &s_fill, LV_PART_INDICATOR);
    lv_obj_add_style(level, &s_knob, LV_PART_KNOB);
    lv_slider_set_range(level, 0, KUI_LEVEL_MAX);
    /* A control inside a scrolling parent would otherwise scroll it on a tap. */
    lv_obj_set_scroll_on_focus(level, false);
    kui_level_set_colour(level, colour);
    return level;
}

void kui_level_set_colour(lv_obj_t *level, lv_color_t colour)
{
    lv_obj_set_style_bg_color(level, colour, LV_PART_INDICATOR);
}
