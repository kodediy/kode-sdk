/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui.h
 * @brief The Kode Dot's UI kit: tokens, styles, faces, glyphs and components.
 */
#ifndef KUI_H
#define KUI_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"
#include "kbsp_board.h"
#include "kui_tokens.h"
#include "kui_glyphs.h"
#include "kui_glass.h"
#include "kui_panel.h"

#ifdef __cplusplus
extern "C" {
#endif

/** The glass's corner radius, from kbsp. LVGL clamps a radius to half the shorter side, so on a pill it draws a capsule and on a card the glass's own curve. */
#define KUI_RADIUS_GLASS  ((int32_t)KBSP_LCD_CORNER_R_PX)

/** An opacity given in percent, rounded down as LVGL's own percentages are. */
#define KUI_OPA_PCT(p)    ((lv_opa_t)((255U * (uint32_t)(p)) / 100U))

/** A level's top value: a level runs from 0 to this, a percentage. */
#define KUI_LEVEL_MAX     100

/**
 * @brief Apply the simple theme to a display and initialise the shared styles.
 *
 * Idempotent: a second call does nothing.
 * Call it after lv_init() and the display, before any kui style is used. Every kui call runs
 * on the thread that drives LVGL; nothing here locks.
 *
 * @param disp The display the theme goes on; must not be NULL.
 */
void kui_init(lv_display_t *disp);

/**
 * @brief A transparent flex box with no padding: the one container every layout here uses.
 *
 * Its own style is removed (no theme background, no border), its size follows
 * its content until the caller sets one, and its scrollbar wears
 * kui_style_scrollbar.
 *
 * @param parent      The parent object.
 * @param flow        The flex flow.
 * @param gap         Pixels between two children, along both axes.
 * @param grow        Flex grow inside a flex parent; 0 keeps the content size.
 * @param main_place  Placement along the flow.
 * @param cross_place Placement across the flow.
 * @param track_place Placement of wrapped tracks.
 * @return The box.
 */
lv_obj_t *kui_flex_create(lv_obj_t *parent, lv_flex_flow_t flow, int32_t gap, uint8_t grow,
                          lv_flex_align_t main_place, lv_flex_align_t cross_place,
                          lv_flex_align_t track_place);

/**
 * @brief A pill: the kit's one row shape, a full-width capsule with a round slot, a name and a line under it.
 *
 * Children, by name: `fill` (a level bar, hidden), `slot` (the disc, holding
 * `slot_img`, hidden, and `slot_text`), `name` and `sub`. Pressed it wears
 * kui_style_pill_pressed, with LV_STATE_USER_1 kui_style_pill_off, checked
 * kui_style_ring.
 *
 * @param parent   The parent object.
 * @param title    The name line.
 * @param subtitle The line under it.
 * @return The pill, an lv_button named "pill_#".
 */
lv_obj_t *kui_pill_create(lv_obj_t *parent, const char *title, const char *subtitle);

/**
 * @brief A category tab: an outlined capsule, filled when checked.
 * @param parent The parent object.
 * @param title  The category's name.
 * @return The tab, an lv_button named "cat_pill_#".
 */
lv_obj_t *kui_cat_pill_create(lv_obj_t *parent, const char *title);

/**
 * @brief A sheet: a full-screen transparent root carrying a card that covers the glass.
 *
 * Every child is created empty and, apart from the header, hidden; the caller
 * fills and shows what a given sheet says. Children of `card`, by name:
 * `handle`, `hdr_disc` (holding `hdr_img` and `hdr_text`), `hdr_title`,
 * `hdr_sub`, `body`, `line1`..`line3`, `note`, `code`, `code_l1`, `code_l2`,
 * `actions` (holding `btn`/`btn_text`).
 *
 * @param parent The parent object.
 * @return The root, named "sheet_#".
 */
lv_obj_t *kui_sheet_create(lv_obj_t *parent);

/**
 * @brief A level the finger drags: a track, a fill in a colour and a knob, from 0 to KUI_LEVEL_MAX.
 *
 * It is a pill's width until the caller sets another. The fill follows the
 * value; LV_EVENT_VALUE_CHANGED says the finger moved it.
 *
 * @param parent The parent object.
 * @param colour The fill's colour.
 * @return The level, an lv_slider named "level_#".
 */
lv_obj_t *kui_level_create(lv_obj_t *parent, lv_color_t colour);

/**
 * @brief Repaint a level's fill.
 * @param level  A level from kui_level_create().
 * @param colour The fill's new colour.
 */
void kui_level_set_colour(lv_obj_t *level, lv_color_t colour);

/**
 * @brief A round colour to tap. LV_STATE_CHECKED rings it, the way a checked pill is ringed.
 * @param parent The parent object.
 * @param colour Its colour; the caller may repaint it with its own bg colour.
 * @return The swatch, an lv_obj named "swatch_#".
 */
lv_obj_t *kui_swatch_create(lv_obj_t *parent, lv_color_t colour);

/**
 * @brief A disc that glows with a colour, off until kui_lamp_set() lights it.
 *
 * The disc sits in a transparent holder padded by the glow's reach, so the
 * glow is never cut at the edge of the lamp's parent.
 *
 * @param parent   The parent object.
 * @param diameter The disc's diameter.
 * @return The holder, named "lamp_#"; its child "disc" is the disc.
 */
lv_obj_t *kui_lamp_create(lv_obj_t *parent, int32_t diameter);

/**
 * @brief Light a lamp with a colour, or turn it off.
 * @param lamp   A lamp from kui_lamp_create().
 * @param colour The disc's and the glow's colour while on.
 * @param on     false shows the off colour and no glow.
 */
void kui_lamp_set(lv_obj_t *lamp, lv_color_t colour, bool on);

/* The shared styles. */
extern lv_style_t kui_style_screen;           /**< A screen: the page, Light Grey type, the body face. */
extern lv_style_t kui_style_scrollbar;        /**< A thin rounded scrollbar in the track colour. */
extern lv_style_t kui_style_scrollbar_hidden; /**< A scrollbar that takes no room and draws nothing. */
extern lv_style_t kui_style_pill;             /**< A pill at rest. */
extern lv_style_t kui_style_pill_pressed;     /**< A pill held down. */
extern lv_style_t kui_style_pill_off;         /**< A pill whose switch is off. */
extern lv_style_t kui_style_ring;             /**< The focus ring: the cursor of the pad, yellow, outside its object. */
extern lv_style_t kui_style_ring_dark;        /**< The same ring in dark ink, for an object that sits on a light surface. */
extern lv_style_t kui_style_fill;             /**< A level's fill inside a pill. */
extern lv_style_t kui_style_slot;             /**< The disc on a pill's left. */
extern lv_style_t kui_style_slot_text;        /**< A letter or a count in the disc. */
extern lv_style_t kui_style_pill_name;        /**< A pill's name line. */
extern lv_style_t kui_style_pill_sub;         /**< A pill's second line. */
extern lv_style_t kui_style_sheet_card;       /**< A sheet's card. */
extern lv_style_t kui_style_handle;           /**< The grab handle at a card's top. */
extern lv_style_t kui_style_handle_on_bg;     /**< The grab handle on the page. */
extern lv_style_t kui_style_hdr_disc;         /**< The disc in a sheet's header. */
extern lv_style_t kui_style_hdr_text;         /**< A letter in the header disc (the title face). */
extern lv_style_t kui_style_hdr_title;        /**< A sheet's title. */
extern lv_style_t kui_style_hdr_sub;          /**< A sheet's subtitle. */
extern lv_style_t kui_style_hdr_title_on_bg;  /**< A title on the page. */
extern lv_style_t kui_style_sheet_line;       /**< A line of a speaking sheet. */
extern lv_style_t kui_style_sheet_line_c;     /**< The same line, centred. */
extern lv_style_t kui_style_sheet_note;       /**< A sheet's small print. */
extern lv_style_t kui_style_sheet_code;       /**< A code shown in large digits. */
extern lv_style_t kui_style_action;           /**< An action button at rest. */
extern lv_style_t kui_style_action_text;      /**< An action button's word. */
extern lv_style_t kui_style_action_pressed;   /**< An action button held down. */
extern lv_style_t kui_style_action_ring;      /**< The cursor ring on an action button. */

/* The faces, 4 bpp: eight in Outfit, the pairing code, the clock and the two
 * big figures in DM Sans, and the DM Sans digits the Outfit faces fall back on.
 * The brand sets numbers in DM Sans: an Outfit face holds no 0-9, so LVGL draws
 * each digit from its fallback. The sizes 28, 34, 40, 60 and 80 are the scale
 * of the page; "Latin-1" is 0xA0-0xFF. */
extern const lv_font_t kui_font_caption;   /**< Outfit Regular 22: ASCII less 0-9, Latin-1, and ←↑→↓. Falls back on kui_font_digits_22. */
extern const lv_font_t kui_font_body;      /**< Outfit Regular 28: ASCII less 0-9, and Latin-1. Falls back on kui_font_digits_28. */
extern const lv_font_t kui_font_button;    /**< Outfit Bold 26: ASCII less 0-9. Falls back on kui_font_digits_26. */
extern const lv_font_t kui_font_name;      /**< Outfit Bold 28: ASCII less 0-9, and Latin-1. Falls back on kui_font_digits_28. */
extern const lv_font_t kui_font_title;     /**< Outfit Bold 34: ASCII less 0-9, and Latin-1. Falls back on kui_font_digits_34. */
extern const lv_font_t kui_font_heading;   /**< Outfit ExtraBold 34, a screen's title: ASCII less 0-9. Falls back on kui_font_digits_34. */
extern const lv_font_t kui_font_display_40; /**< Outfit ExtraBold 40: ASCII less 0-9, and Latin-1. Falls back on kui_font_digits_40. */
extern const lv_font_t kui_font_title_60;  /**< Outfit ExtraBold 60: ASCII less 0-9. Falls back on kui_font_digits_60. */
extern const lv_font_t kui_font_code;      /**< DM Sans Bold 64: space, 0-9 and ←↑→↓ only. */
extern const lv_font_t kui_font_clock;     /**< DM Sans Bold 104: 0-9, the colon and the hyphen only, with no tabular figures. */
extern const lv_font_t kui_font_code_80;   /**< DM Sans Bold 80: space, %, +, -, ., /, 0-9 and the colon only. */
extern const lv_font_t kui_font_hero_160;  /**< DM Sans Bold 160: space, % and 0-9 only. */
extern const lv_font_t kui_font_digits_22; /**< DM Sans Bold 22: 0-9 only. */
extern const lv_font_t kui_font_digits_26; /**< DM Sans Bold 26: 0-9 only. */
extern const lv_font_t kui_font_digits_28; /**< DM Sans Bold 28: 0-9 only. */
extern const lv_font_t kui_font_digits_34; /**< DM Sans Bold 34: 0-9 only. */
extern const lv_font_t kui_font_digits_40; /**< DM Sans Bold 40: 0-9 only. */
extern const lv_font_t kui_font_digits_60; /**< DM Sans Bold 60: 0-9 only. */

/* The thirteen glyphs, 40 x 40 ARGB8888, and the wordmark. The line glyphs of
 * kui_glyphs.h, A8 and in more sizes, are the look of the page. */
extern const lv_image_dsc_t kui_glyph_wifi;    /**< Wi-Fi. */
extern const lv_image_dsc_t kui_glyph_ble;     /**< Bluetooth. */
extern const lv_image_dsc_t kui_glyph_sun;     /**< Brightness. */
extern const lv_image_dsc_t kui_glyph_battery; /**< Battery. */
extern const lv_image_dsc_t kui_glyph_power;   /**< Power off. */
extern const lv_image_dsc_t kui_glyph_info;    /**< About. */
extern const lv_image_dsc_t kui_glyph_check;   /**< A tick. */
extern const lv_image_dsc_t kui_glyph_lock;    /**< Secured. */
extern const lv_image_dsc_t kui_glyph_usb;     /**< USB power. */
extern const lv_image_dsc_t kui_glyph_bolt;    /**< Voltage. */
extern const lv_image_dsc_t kui_glyph_sd;      /**< The microSD card. */
extern const lv_image_dsc_t kui_glyph_radio;   /**< The radio. */
extern const lv_image_dsc_t kui_glyph_x;       /**< A cross. */
extern const lv_image_dsc_t kui_wordmark;      /**< The kode wordmark, 300 x 90 RGB565A8. */

#ifdef __cplusplus
}
#endif

#endif /* KUI_H */
