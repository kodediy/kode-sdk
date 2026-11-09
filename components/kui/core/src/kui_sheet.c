/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_sheet.c
 * @brief The sheet: a card that rises over a transparent root.
 */
#include "kui.h"

/**
 * @brief A hidden, empty, left-aligned line of the card, PILL_W wide at x 32.
 * @param card  The card.
 * @param name  Its name.
 * @param y     Its row.
 * @param style Its style.
 */
static void card_line(lv_obj_t *card, const char *name, int32_t y, lv_style_t *style)
{
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_name(l, name);
    lv_obj_set_x(l, 32);
    lv_obj_set_y(l, y);
    lv_obj_set_width(l, KUI_PILL_W);
    lv_label_set_text(l, "");
    lv_obj_set_hidden(l, true);
    lv_obj_add_style(l, style, 0);
}

/**
 * @brief A hidden, empty, centred line of the card.
 * @param card  The card.
 * @param name  Its name.
 * @param y     Its row.
 * @param style Its style.
 */
static void card_centred(lv_obj_t *card, const char *name, int32_t y, lv_style_t *style)
{
    lv_obj_t *l = lv_label_create(card);
    lv_obj_set_name(l, name);
    lv_obj_set_align(l, LV_ALIGN_TOP_MID);
    lv_obj_set_y(l, y);
    lv_label_set_text(l, "");
    lv_obj_set_hidden(l, true);
    lv_obj_add_style(l, style, 0);
}

/**
 * @brief The one answer button with its word, centred in the actions row.
 * @param row The actions row.
 */
static void action(lv_obj_t *row)
{
    lv_obj_t *b = lv_button_create(row);
    lv_obj_set_name(b, "btn");
    lv_obj_set_width(b, KUI_ACTION_W);
    lv_obj_set_height(b, KUI_ACTION_H);
    lv_obj_add_style(b, &kui_style_action, 0);
    lv_obj_add_style(b, &kui_style_action_pressed, LV_STATE_PRESSED);
    lv_obj_add_style(b, &kui_style_action_ring, LV_STATE_CHECKED);

    lv_obj_t *t = lv_label_create(b);
    lv_obj_set_name(t, "btn_text");
    lv_obj_set_align(t, LV_ALIGN_CENTER);
    lv_label_set_text(t, "");
    lv_obj_add_style(t, &kui_style_action_text, 0);
}

lv_obj_t *kui_sheet_create(lv_obj_t *parent)
{
    lv_obj_t *root = lv_obj_create(parent);
    lv_obj_set_name_static(root, "sheet_#");
    lv_obj_set_width(root, 410);
    lv_obj_set_height(root, 502);
    lv_obj_set_style_bg_opa(root, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(root, 0, 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_add_style(root, &kui_style_scrollbar_hidden, LV_PART_SCROLLBAR);

    /* The whole glass: the card's own corners are the glass's corners. */
    lv_obj_t *card = lv_obj_create(root);
    lv_obj_set_name(card, "card");
    lv_obj_set_x(card, 0);
    lv_obj_set_y(card, 0);
    lv_obj_set_width(card, 410);
    lv_obj_set_height(card, 502);
    lv_obj_add_style(card, &kui_style_sheet_card, 0);
    lv_obj_add_style(card, &kui_style_scrollbar_hidden, LV_PART_SCROLLBAR);

    lv_obj_t *handle = lv_obj_create(card);
    lv_obj_set_name(handle, "handle");
    lv_obj_set_align(handle, LV_ALIGN_TOP_MID);
    lv_obj_set_y(handle, 14);
    lv_obj_set_width(handle, 36);
    lv_obj_set_height(handle, 5);
    lv_obj_add_style(handle, &kui_style_handle, 0);

    lv_obj_t *disc = lv_obj_create(card);
    lv_obj_set_name(disc, "hdr_disc");
    lv_obj_set_x(disc, 32);
    lv_obj_set_y(disc, 56);
    lv_obj_set_width(disc, KUI_HDR_DISC);
    lv_obj_set_height(disc, KUI_HDR_DISC);
    lv_obj_add_style(disc, &kui_style_hdr_disc, 0);

    lv_obj_t *img = lv_image_create(disc);
    lv_obj_set_name(img, "hdr_img");
    lv_obj_set_align(img, LV_ALIGN_CENTER);
    lv_obj_set_hidden(img, true);

    lv_obj_t *letter = lv_label_create(disc);
    lv_obj_set_name(letter, "hdr_text");
    lv_obj_set_align(letter, LV_ALIGN_CENTER);
    lv_label_set_text(letter, "");
    lv_obj_add_style(letter, &kui_style_hdr_text, 0);

    /* Fixed heights: a long name is dotted on one line instead of wrapping
     * out of its place beside the disc. */
    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_name(title, "hdr_title");
    lv_obj_set_x(title, 160);
    lv_obj_set_y(title, 80);
    lv_obj_set_width(title, 218);
    lv_obj_set_height(title, 35);
    lv_label_set_text(title, "");
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_style(title, &kui_style_hdr_title, 0);

    lv_obj_t *sub = lv_label_create(card);
    lv_obj_set_name(sub, "hdr_sub");
    lv_obj_set_x(sub, 160);
    lv_obj_set_y(sub, 121);
    lv_obj_set_width(sub, 218);
    lv_obj_set_height(sub, 22);
    lv_label_set_text(sub, "");
    lv_label_set_long_mode(sub, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_add_style(sub, &kui_style_hdr_sub, 0);

    lv_obj_t *body = kui_flex_create(card, LV_FLEX_FLOW_COLUMN, KUI_SPACE_LG, 0U,
                                     LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_name(body, "body");
    lv_obj_set_x(body, 32);
    lv_obj_set_y(body, 188);
    lv_obj_set_width(body, KUI_PILL_W);
    lv_obj_set_height(body, 264);
    lv_obj_set_style_pad_all(body, 0, 0);
    lv_obj_add_style(body, &kui_style_scrollbar_hidden, LV_PART_SCROLLBAR);

    card_line(card, "line1", 188, &kui_style_sheet_line);
    card_line(card, "line2", 218, &kui_style_sheet_line);
    card_line(card, "line3", 248, &kui_style_sheet_line);
    card_line(card, "note", 262, &kui_style_sheet_note);
    card_centred(card, "code", 226, &kui_style_sheet_code);
    card_centred(card, "code_l1", 296, &kui_style_sheet_line_c);
    card_centred(card, "code_l2", 324, &kui_style_sheet_line_c);

    lv_obj_t *actions = lv_obj_create(card);
    lv_obj_set_name(actions, "actions");
    lv_obj_set_x(actions, 32);
    lv_obj_set_y(actions, 395);
    lv_obj_set_width(actions, KUI_PILL_W);
    lv_obj_set_height(actions, KUI_ACTION_H);
    lv_obj_set_hidden(actions, true);
    lv_obj_set_flex_flow(actions, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_flex_main_place(actions, LV_FLEX_ALIGN_CENTER, 0);
    lv_obj_set_style_bg_opa(actions, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(actions, 0, 0);
    lv_obj_set_style_pad_all(actions, 0, 0);
    action(actions);
    return root;
}
