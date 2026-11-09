/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_flex.c
 * @brief The flex box every layout here is built from.
 */
#include <stdbool.h>

#include "kui.h"

/** Content-sized flex column, shared by every box. */
static lv_style_t s_box;
/** Set once s_box is initialised. */
static bool s_box_ready = false;

lv_obj_t *kui_flex_create(lv_obj_t *parent, lv_flex_flow_t flow, int32_t gap, uint8_t grow,
                          lv_flex_align_t main_place, lv_flex_align_t cross_place,
                          lv_flex_align_t track_place)
{
    if ( !s_box_ready )
    {
        lv_style_init(&s_box);
        lv_style_set_width(&s_box, LV_SIZE_CONTENT);
        lv_style_set_height(&s_box, LV_SIZE_CONTENT);
        lv_style_set_layout(&s_box, LV_LAYOUT_FLEX);
        lv_style_set_flex_flow(&s_box, LV_FLEX_FLOW_COLUMN);
        s_box_ready = true;
    }
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    if ( LV_FLEX_FLOW_ROW == flow )
    {
        lv_obj_set_name_static(box, "row_#");
    }
    else
    {
        lv_obj_set_name_static(box, "column_#");
    }
    lv_obj_add_style(box, &s_box, 0);
    lv_obj_add_style(box, &kui_style_scrollbar, LV_PART_SCROLLBAR);
    lv_obj_set_flex_grow(box, grow);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_set_style_pad_row(box, gap, 0);
    lv_obj_set_style_pad_column(box, gap, 0);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_style_flex_main_place(box, main_place, 0);
    lv_obj_set_style_flex_cross_place(box, cross_place, 0);
    lv_obj_set_style_flex_track_place(box, track_place, 0);
    return box;
}
