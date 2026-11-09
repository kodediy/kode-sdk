/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_swatch.c
 * @brief The swatch: a round colour to tap, ringed when it is the chosen one.
 */
#include "kui.h"

/** A swatch at rest: a filled circle. */
static lv_style_t s_swatch;
/** The chosen swatch: the cursor ring round it. */
static lv_style_t s_swatch_checked;
/** A swatch held down. */
static lv_style_t s_swatch_pressed;

/** @brief Initialise the three swatch styles, once. */
static void swatch_styles_init(void)
{
    lv_style_init(&s_swatch);
    lv_style_init(&s_swatch_checked);
    lv_style_init(&s_swatch_pressed);
    lv_style_set_radius(&s_swatch, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_swatch, LV_OPA_COVER);
    lv_style_set_border_width(&s_swatch_checked, KUI_RING_W);
    lv_style_set_border_color(&s_swatch_checked, KUI_COLOR_FOCUS);
    lv_style_set_border_opa(&s_swatch_checked, LV_OPA_COVER);
    lv_style_set_border_post(&s_swatch_checked, true);
    lv_style_set_recolor(&s_swatch_pressed, KUI_COLOR_TEXT_ON_BG);
    lv_style_set_recolor_opa(&s_swatch_pressed, KUI_OPA_PCT(20));
}

/* cppcheck-suppress misra-c2012-8.7 ; a kit call for apps, never used inside the SDK */
lv_obj_t *kui_swatch_create(lv_obj_t *parent, lv_color_t colour)
{
    /* Set once the three styles are initialised. */
    static bool s_ready = false;

    if ( !s_ready )
    {
        swatch_styles_init();
        s_ready = true;
    }
    lv_obj_t *swatch = lv_obj_create(parent);
    lv_obj_set_name_static(swatch, "swatch_#");
    lv_obj_remove_style_all(swatch);
    lv_obj_set_size(swatch, KUI_SWATCH, KUI_SWATCH);
    lv_obj_add_style(swatch, &s_swatch, 0);
    lv_obj_add_style(swatch, &s_swatch_checked, LV_STATE_CHECKED);
    lv_obj_add_style(swatch, &s_swatch_pressed, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(swatch, colour, 0);
    lv_obj_set_scrollable(swatch, false);
    lv_obj_set_scroll_on_focus(swatch, false);
    return swatch;
}
