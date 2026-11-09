/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_cat_pill.c
 * @brief The category tab.
 */
#include "kui.h"

/** A tab at rest: an outline on the page. */
static lv_style_t s_tab;
/** The active tab: filled with Light Grey, no border, dark text: the one light thing on a page of dark rows. */
static lv_style_t s_tab_selected;
/** A tab held down. */
static lv_style_t s_tab_pressed;
/** Set once the three styles are initialised. */
static bool s_ready = false;

/** @brief Initialise the three tab styles, once. */
static void tab_styles_init(void)
{
    lv_style_init(&s_tab);
    lv_style_init(&s_tab_selected);
    lv_style_init(&s_tab_pressed);
    lv_style_set_height(&s_tab, KUI_CAT_H);
    lv_style_set_bg_opa(&s_tab, LV_OPA_TRANSP);
    lv_style_set_border_color(&s_tab, KUI_COLOR_TEXT_ON_BG);
    lv_style_set_border_width(&s_tab, KUI_BORDER_WIDTH);
    lv_style_set_border_opa(&s_tab, KUI_OPA_PCT(40));
    lv_style_set_text_color(&s_tab, KUI_COLOR_TEXT_ON_BG);
    lv_style_set_radius(&s_tab, KUI_RADIUS_GLASS);
    lv_style_set_pad_left(&s_tab, KUI_SPACE_LG);
    lv_style_set_pad_right(&s_tab, KUI_SPACE_LG);
    lv_style_set_bg_color(&s_tab_selected, KUI_COLOR_TEXT_ON_BG);
    lv_style_set_bg_opa(&s_tab_selected, LV_OPA_COVER);
    lv_style_set_border_opa(&s_tab_selected, LV_OPA_TRANSP);
    lv_style_set_text_color(&s_tab_selected, KUI_COLOR_TEXT);
    lv_style_set_recolor(&s_tab_pressed, KUI_COLOR_TEXT_ON_BG);
    lv_style_set_recolor_opa(&s_tab_pressed, KUI_OPA_PCT(20));
}

lv_obj_t *kui_cat_pill_create(lv_obj_t *parent, const char *title)
{
    if ( !s_ready )
    {
        tab_styles_init();
        s_ready = true;
    }
    lv_obj_t *tab = lv_button_create(parent);
    lv_obj_set_name_static(tab, "cat_pill_#");
    lv_obj_set_width(tab, LV_SIZE_CONTENT);
    lv_obj_add_style(tab, &s_tab, 0);
    lv_obj_add_style(tab, &s_tab_selected, LV_STATE_CHECKED);
    lv_obj_add_style(tab, &s_tab_pressed, LV_STATE_PRESSED);

    lv_obj_t *label = lv_label_create(tab);
    lv_obj_set_align(label, LV_ALIGN_CENTER);
    lv_label_set_text(label, title);
    lv_obj_set_style_text_font(label, &kui_font_caption, 0);
    return tab;
}
