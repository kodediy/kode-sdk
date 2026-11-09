/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_lamp.c
 * @brief The lamp: a disc that glows with a colour, in a holder with room for the glow.
 */
#include "kui.h"

/** The disc: a filled circle with a glow that is hidden while the lamp is off. */
static lv_style_t s_lamp_disc;

/** @brief Initialise the disc's style, once. */
static void lamp_style_init(void)
{
    lv_style_init(&s_lamp_disc);
    lv_style_set_radius(&s_lamp_disc, LV_RADIUS_CIRCLE);
    lv_style_set_bg_opa(&s_lamp_disc, LV_OPA_COVER);
    lv_style_set_shadow_width(&s_lamp_disc, KUI_LAMP_GLOW);
    lv_style_set_shadow_spread(&s_lamp_disc, KUI_LAMP_GLOW_SPREAD);
}

/* cppcheck-suppress misra-c2012-8.7 ; a kit call for apps, never used inside the SDK */
lv_obj_t *kui_lamp_create(lv_obj_t *parent, int32_t diameter)
{
    /* Set once the style is initialised. */
    static bool s_ready = false;

    if ( !s_ready )
    {
        lamp_style_init();
        s_ready = true;
    }
    /* LVGL cuts a child's shadow at its parent's edge: the holder's padding is the glow's room. */
    lv_obj_t *lamp = lv_obj_create(parent);
    lv_obj_set_name_static(lamp, "lamp_#");
    lv_obj_remove_style_all(lamp);
    /* cppcheck-suppress [misra-c2012-10.1,misra-c2012-12.2] ; LV_SIZE_CONTENT is LVGL's own shifted constant */
    lv_obj_set_size(lamp, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(lamp, KUI_LAMP_GLOW, 0);
    lv_obj_set_scrollable(lamp, false);
    lv_obj_set_clickable(lamp, false);

    lv_obj_t *disc = lv_obj_create(lamp);
    lv_obj_set_name(disc, "disc");
    lv_obj_remove_style_all(disc);
    lv_obj_set_size(disc, diameter, diameter);
    lv_obj_add_style(disc, &s_lamp_disc, 0);
    lv_obj_set_scrollable(disc, false);
    lv_obj_set_clickable(disc, false);
    kui_lamp_set(lamp, KUI_COLOR_OFF, false);
    return lamp;
}

/* cppcheck-suppress constParameterPointer ; the lamp changes: its disc is restyled */
void kui_lamp_set(lv_obj_t *lamp, lv_color_t colour, bool on)
{
    lv_obj_t *disc = lv_obj_find_by_name(lamp, "disc");

    if ( NULL == disc )
    {
        return;
    }
    if ( on )
    {
        lv_obj_set_style_bg_color(disc, colour, 0);
        lv_obj_set_style_shadow_color(disc, colour, 0);
        lv_obj_set_style_shadow_opa(disc, LV_OPA_COVER, 0);
    }
    else
    {
        lv_obj_set_style_bg_color(disc, KUI_COLOR_OFF, 0);
        lv_obj_set_style_shadow_opa(disc, LV_OPA_TRANSP, 0);
    }
}
