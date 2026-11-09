/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_pill.c
 * @brief The pill: the kit's one row shape.
 */
#include "kui.h"

lv_obj_t *kui_pill_create(lv_obj_t *parent, const char *title, const char *subtitle)
{
    lv_obj_t *pill = lv_button_create(parent);
    lv_obj_set_name_static(pill, "pill_#");
    lv_obj_set_width(pill, KUI_PILL_W);
    lv_obj_set_height(pill, KUI_PILL_H);
    lv_obj_add_style(pill, &kui_style_pill, 0);
    lv_obj_add_style(pill, &kui_style_pill_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(pill, &kui_style_pill_off, LV_STATE_USER_1);
    lv_obj_add_style(pill, &kui_style_ring, LV_STATE_CHECKED);

    lv_obj_t *fill = lv_obj_create(pill);
    lv_obj_set_name(fill, "fill");
    lv_obj_set_x(fill, 0);
    lv_obj_set_y(fill, 0);
    lv_obj_set_width(fill, 0);
    lv_obj_set_height(fill, lv_pct(100));
    lv_obj_set_hidden(fill, true);
    lv_obj_add_style(fill, &kui_style_fill, 0);

    lv_obj_t *slot = lv_obj_create(pill);
    lv_obj_set_name(slot, "slot");
    lv_obj_set_x(slot, 12);
    lv_obj_set_align(slot, LV_ALIGN_LEFT_MID);
    lv_obj_set_width(slot, KUI_ICON_SLOT);
    lv_obj_set_height(slot, KUI_ICON_SLOT);
    lv_obj_add_style(slot, &kui_style_slot, 0);

    lv_obj_t *slot_img = lv_image_create(slot);
    lv_obj_set_name(slot_img, "slot_img");
    lv_obj_set_align(slot_img, LV_ALIGN_CENTER);
    lv_obj_set_hidden(slot_img, true);

    lv_obj_t *slot_text = lv_label_create(slot);
    lv_obj_set_name(slot_text, "slot_text");
    lv_obj_set_align(slot_text, LV_ALIGN_CENTER);
    lv_label_set_text(slot_text, "");
    lv_obj_add_style(slot_text, &kui_style_slot_text, 0);

    lv_obj_t *name = lv_label_create(pill);
    lv_obj_set_name(name, "name");
    lv_obj_set_x(name, 84);
    lv_obj_set_y(name, 8);
    lv_obj_set_width(name, 250);
    lv_label_set_text(name, title);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_style(name, &kui_style_pill_name, 0);

    lv_obj_t *sub = lv_label_create(pill);
    lv_obj_set_name(sub, "sub");
    lv_obj_set_x(sub, 84);
    lv_obj_set_y(sub, 44);
    lv_obj_set_width(sub, 250);
    lv_label_set_text(sub, subtitle);
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_style(sub, &kui_style_pill_sub, 0);

    return pill;
}
