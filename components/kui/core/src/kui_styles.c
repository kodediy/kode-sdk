/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_styles.c
 * @brief The styles the kit's components share, and the ones kgui names directly.
 */
#include "kui.h"
#include "kui_internal.h"

lv_style_t kui_style_screen;
lv_style_t kui_style_scrollbar;
lv_style_t kui_style_scrollbar_hidden;
lv_style_t kui_style_pill;
lv_style_t kui_style_pill_pressed;
lv_style_t kui_style_pill_off;
lv_style_t kui_style_ring;
lv_style_t kui_style_ring_dark;
lv_style_t kui_style_fill;
lv_style_t kui_style_slot;
lv_style_t kui_style_slot_text;
lv_style_t kui_style_pill_name;
lv_style_t kui_style_pill_sub;
lv_style_t kui_style_sheet_card;
lv_style_t kui_style_handle;
lv_style_t kui_style_handle_on_bg;
lv_style_t kui_style_hdr_disc;
lv_style_t kui_style_hdr_text;
lv_style_t kui_style_hdr_title;
lv_style_t kui_style_hdr_sub;
lv_style_t kui_style_hdr_title_on_bg;
lv_style_t kui_style_sheet_line;
lv_style_t kui_style_sheet_line_c;
lv_style_t kui_style_sheet_note;
lv_style_t kui_style_sheet_code;
lv_style_t kui_style_action;
lv_style_t kui_style_action_text;
lv_style_t kui_style_action_pressed;
lv_style_t kui_style_action_ring;

/**
 * @brief A focus ring: an outline RING_W wide, RING_GAP off the object, outside it.
 * @param s     The style.
 * @param color The ring's colour.
 */
static void ring(lv_style_t *s, lv_color_t color)
{
    lv_style_init(s);
    lv_style_set_outline_width(s, KUI_RING_W);
    lv_style_set_outline_pad(s, KUI_RING_GAP);
    lv_style_set_outline_color(s, color);
    lv_style_set_outline_opa(s, LV_OPA_COVER);
}

/**
 * @brief A solid rounded surface with no border.
 * @param s      The style.
 * @param color  Its colour.
 * @param opa    Its opacity.
 * @param radius Its corner radius.
 */
static void surface(lv_style_t *s, lv_color_t color, lv_opa_t opa, int32_t radius)
{
    lv_style_init(s);
    lv_style_set_bg_color(s, color);
    lv_style_set_bg_opa(s, opa);
    lv_style_set_radius(s, radius);
    lv_style_set_border_width(s, 0);
}

/**
 * @brief Text in a face and a colour, at an opacity.
 * @param s     The style.
 * @param font  The face.
 * @param color The colour.
 * @param opa   The text opacity; a style sets only the properties it names, and LV_OPA_COVER is
 *              already every part's default, so it makes no call.
 */
static void text(lv_style_t *s, const lv_font_t *font, lv_color_t color, lv_opa_t opa)
{
    lv_style_init(s);
    lv_style_set_text_font(s, font);
    lv_style_set_text_color(s, color);
    if ( LV_OPA_COVER != opa )
    {
        lv_style_set_text_opa(s, opa);
    }
}

void kui_styles_init(void)
{
    lv_style_init(&kui_style_screen);
    lv_style_set_bg_color(&kui_style_screen, KUI_COLOR_BG);
    lv_style_set_bg_opa(&kui_style_screen, LV_OPA_COVER);
    lv_style_set_text_color(&kui_style_screen, KUI_COLOR_TEXT_ON_BG);
    lv_style_set_text_font(&kui_style_screen, &kui_font_body);

    lv_style_init(&kui_style_scrollbar);
    lv_style_set_width(&kui_style_scrollbar, KUI_SPACE_SM);
    lv_style_set_radius(&kui_style_scrollbar, KUI_SPACE_SM);
    lv_style_set_bg_color(&kui_style_scrollbar, KUI_COLOR_TRACK);
    lv_style_set_bg_opa(&kui_style_scrollbar, KUI_OPA_PCT(80));
    lv_style_set_pad_all(&kui_style_scrollbar, KUI_SPACE_SM);

    lv_style_init(&kui_style_scrollbar_hidden);
    lv_style_set_width(&kui_style_scrollbar_hidden, 0);
    lv_style_set_bg_opa(&kui_style_scrollbar_hidden, LV_OPA_TRANSP);

    surface(&kui_style_pill, KUI_COLOR_SURFACE, LV_OPA_COVER, KUI_RADIUS_GLASS);
    lv_style_set_pad_all(&kui_style_pill, 0);
    lv_style_set_shadow_width(&kui_style_pill, 0);

    lv_style_init(&kui_style_pill_pressed);
    lv_style_set_bg_color(&kui_style_pill_pressed, KUI_COLOR_REST);

    lv_style_init(&kui_style_pill_off);
    lv_style_set_bg_color(&kui_style_pill_off, KUI_COLOR_OFF);

    ring(&kui_style_ring, KUI_COLOR_FOCUS);
    ring(&kui_style_ring_dark, KUI_COLOR_TEXT);

    surface(&kui_style_fill, KUI_COLOR_LEVEL, LV_OPA_COVER, 0);

    surface(&kui_style_slot, KUI_COLOR_TEXT_MUTED, LV_OPA_COVER, KUI_RADIUS_GLASS);
    lv_style_set_pad_all(&kui_style_slot, 0);

    text(&kui_style_slot_text, &kui_font_caption, KUI_COLOR_TEXT, LV_OPA_COVER);
    text(&kui_style_pill_name, &kui_font_name, KUI_COLOR_TEXT_ON_BG, LV_OPA_COVER);
    text(&kui_style_pill_sub, &kui_font_caption, KUI_COLOR_TEXT_MUTED, LV_OPA_COVER);

    surface(&kui_style_sheet_card, KUI_COLOR_SHEET, LV_OPA_COVER, KUI_RADIUS_GLASS);
    lv_style_set_pad_all(&kui_style_sheet_card, 0);

    surface(&kui_style_handle, KUI_COLOR_TEXT_ON_BG, KUI_OPA_PCT(35), KUI_RADIUS_GLASS);
    surface(&kui_style_handle_on_bg, KUI_COLOR_TEXT_ON_BG, KUI_OPA_PCT(35), KUI_RADIUS_GLASS);

    surface(&kui_style_hdr_disc, KUI_COLOR_SURFACE, LV_OPA_COVER, KUI_RADIUS_GLASS);
    lv_style_set_pad_all(&kui_style_hdr_disc, 0);

    text(&kui_style_hdr_text, &kui_font_title, KUI_COLOR_TEXT_ON_BG, LV_OPA_COVER);
    text(&kui_style_hdr_title, &kui_font_title, KUI_COLOR_TEXT_ON_BG, LV_OPA_COVER);
    text(&kui_style_hdr_sub, &kui_font_caption, KUI_COLOR_TEXT_MUTED, LV_OPA_COVER);
    text(&kui_style_hdr_title_on_bg, &kui_font_heading, KUI_COLOR_TEXT_ON_BG, LV_OPA_COVER);
    text(&kui_style_sheet_line, &kui_font_body, KUI_COLOR_TEXT_ON_BG, LV_OPA_COVER);
    text(&kui_style_sheet_line_c, &kui_font_body, KUI_COLOR_TEXT_ON_BG, LV_OPA_COVER);
    lv_style_set_text_align(&kui_style_sheet_line_c, LV_TEXT_ALIGN_CENTER);
    text(&kui_style_sheet_note, &kui_font_caption, KUI_COLOR_TEXT_MUTED, LV_OPA_COVER);
    text(&kui_style_sheet_code, &kui_font_code, KUI_COLOR_TEXT_ON_BG, LV_OPA_COVER);
    lv_style_set_text_letter_space(&kui_style_sheet_code, 4);

    surface(&kui_style_action, KUI_COLOR_ACTION, LV_OPA_COVER, KUI_RADIUS_GLASS);
    lv_style_set_pad_all(&kui_style_action, 0);
    lv_style_set_shadow_width(&kui_style_action, 0);

    text(&kui_style_action_text, &kui_font_button, KUI_COLOR_ON_ACCENT, LV_OPA_COVER);

    lv_style_init(&kui_style_action_pressed);
    lv_style_set_recolor(&kui_style_action_pressed, KUI_COLOR_ON_ACCENT);
    lv_style_set_recolor_opa(&kui_style_action_pressed, KUI_OPA_PCT(20));

    ring(&kui_style_action_ring, KUI_COLOR_FOCUS);
}
