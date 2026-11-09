/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_screen.c
 * @brief The screen on the standard layer's Active Object: kscreen, LVGL and the kit's sheet.
 */
#include "kode_screen_priv.h"

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#include "kode.h"
#include "kode_log.h"
#include "khal_system.h"
#include "kqp.h"
#include "kbsp_board.h"
#include "kscreen.h"
#include "kui.h"
#include "kui_gesture.h"

#define TAG "kode.screen"

#if !LV_USE_CANVAS
#error "kode_screen draws canvases with lv_canvas: set CONFIG_LV_USE_CANVAS"
#endif

_Static_assert((uint32_t)LV_DRAW_BUF_STRIDE_ALIGN <= KODE_CANVAS_PIXEL_BYTES,
               "an lv_canvas stride must be the canvas's width in pixels times two");
_Static_assert((KODE_CANVAS_ALIGN % (uint32_t)LV_DRAW_BUF_ALIGN) == 0U,
               "a canvas's pixels are aligned as LVGL asks of a draw buffer");
_Static_assert((KODE_CANVAS_WIDTH_MAX == KBSP_LCD_H_RES) && (KODE_CANVAS_HEIGHT_MAX == KBSP_LCD_V_RES),
               "the biggest canvas is the glass");
_Static_assert(KODE_CANVAS_BODY_W == (uint32_t)KUI_PILL_W, "a canvas fits the sheet's body");
_Static_assert(KODE_CANVAS_BODY_H_BARE == (KBSP_LCD_V_RES - (uint32_t)KUI_SCREEN_TOP - (uint32_t)KUI_SCREEN_BOTTOM),
               "a canvas fits the body of a screen with no header");
_Static_assert(KODE_SCREEN_GAP_H == (uint32_t)KUI_SPACE_LG, "the body's widgets are KUI_SPACE_LG apart");
_Static_assert(KODE_SCREEN_BUTTON_H == (uint32_t)KUI_PILL_H, "a button is a pill");

/** @brief Alignment of the screen's state in PSRAM. */
#define SCREEN_ALLOC_ALIGN 16U

/** @brief How far the title moves down when there is no line under it: half
 *         that line and its gap, so the title alone is centred on the disc. */
#define SCREEN_TITLE_ALONE_DY ((int32_t)15)

/** @brief Bytes of the disc's letter: one UTF-8 character and its NUL. */
#define SCREEN_MONO_BYTES 5U

/** @brief The top two bits of a UTF-8 continuation byte. */
#define UTF8_CONT_MASK 0xC0U

/** @brief Their value in a continuation byte. */
#define UTF8_CONT_BITS 0x80U

/** @brief From an ASCII lower-case letter to its capital. */
#define ASCII_CASE_GAP ((uint8_t)0x20U)

/** @brief What a widget is. */
#define WIDGET_TEXT   0U  /**< A line of text. */
#define WIDGET_VALUE  1U  /**< A value.        */
#define WIDGET_BUTTON 2U  /**< A button.       */
#define WIDGET_CANVAS 3U  /**< A canvas.       */
#define WIDGET_IMAGE  4U  /**< An image.       */

/** @brief A widget's colour while it keeps the kit's: no 0xRRGGBB reaches it. */
#define WIDGET_COLOR_KIT UINT32_MAX

/** @brief A button's fill at least this light (lv_color_luminance(), 0 to 255) takes dark letters. */
#define WIDGET_FILL_LIGHT 128U

/** @brief UTF-8: the lead byte's marker bits and masks, per length, and a continuation's. */
#define UTF8_LEAD2_MARK 0xC0U
#define UTF8_LEAD2_MASK 0xE0U
#define UTF8_LEAD2_DATA 0x1FU
#define UTF8_LEAD3_MARK 0xE0U
#define UTF8_LEAD3_MASK 0xF0U
#define UTF8_LEAD3_DATA 0x0FU
#define UTF8_LEAD4_MARK 0xF0U
#define UTF8_LEAD4_MASK 0xF8U
#define UTF8_LEAD4_DATA 0x07U
#define UTF8_ASCII_MAX  0x80U
#define UTF8_DATA_BITS  6U
#define UTF8_DATA_MASK  0x3FU

/** @brief What a byte that starts no character reads as: the replacement character. */
#define UTF8_BAD 0xFFFDU

/** @brief The highest code point searched for the body font's biggest glyph; the font holds nothing above. */
#define GLYPH_SCAN_MAX 0xFFU

/** @brief Alignment of the glyph coverage block. */
#define COVER_ALIGN 16U

/** @brief Bits in a byte. */
#define COVER_BYTE_BITS 8U

/** @brief The largest value of a coverage byte. */
#define COVER_FULL 255U

/** @brief The new-line that ends a line of text. */
#define TEXT_NEWLINE '\n'

/** @brief One button's press, from the landing to the release. */
typedef struct {
    uint32_t      place;    /**< The button's place on the screen.            */
    kui_gesture_t gesture;  /**< Whether the press was a tap.               */
} press_t;

/** @brief The screen, as the requests describe it and as LVGL holds it. */
typedef struct {
    kscreen_t screen;                                        /**< LVGL on the glass.          */
    bool      open;                                          /**< kscreen_open() worked.       */
    uint32_t  gen;                                           /**< The screen's generation.       */
    char      title[KODE_SCREEN_TEXT_MAX];                   /**< "" for no header.            */
    uint32_t  count;                                         /**< Widgets on the screen.          */
    uint8_t   kind[KODE_WIDGETS_MAX];                         /**< WIDGET_* of each.             */
    char      text[KODE_WIDGETS_MAX][KODE_SCREEN_TEXT_MAX];   /**< Each widget's text: a line, a number or a label. */
    char      unit[KODE_WIDGETS_MAX][KODE_VALUE_UNIT_MAX];    /**< A value's unit.              */
    uint32_t  color[KODE_WIDGETS_MAX];                        /**< Each widget's colour, 0xRRGGBB, or WIDGET_COLOR_KIT. */
    press_t   press[KODE_WIDGETS_MAX];                       /**< Each button's press.         */
    kode_canvasbuf_t *buf[KODE_WIDGETS_MAX];                  /**< A canvas's or an image's pixels, from its request until its screen is left. */
    lv_obj_t *root;                                          /**< The sheet; NULL until built. */
    lv_obj_t *card;                                          /**< The sheet's card.            */
    lv_obj_t *body;                                          /**< The column of widgets.        */
    lv_obj_t *widget[KODE_WIDGETS_MAX];                        /**< The label a change sets: a line, or a value's number; a button's pill; a canvas's lv_canvas. */
} screen_t;


/** @brief The screen; taken by prepare(), then the Active Object's thread alone touches it. */
static screen_t *s_screen;

/**
 * @brief Hide a child of the card by name.
 * @param card The sheet's card, or a pill.
 * @param name The child's name.
 */
static void child_hide(const lv_obj_t *card, const char *name)
{
    lv_obj_t *const obj = lv_obj_find_by_name(card, name);

    if ( NULL != obj )
    {
        lv_obj_set_hidden(obj, true);
    }
}

/**
 * @brief The disc's letter: the title's first character, an ASCII one in capitals.
 * @param title The title; not empty.
 * @param out   SCREEN_MONO_BYTES bytes.
 */
static void monogram(const char *title, char *out)
{
    size_t n = 1U;

    while ( ((SCREEN_MONO_BYTES - 1U) > n)
           && (((uint8_t)title[n] & UTF8_CONT_MASK) == UTF8_CONT_BITS) )
    {
        n++;
    }
    (void)memcpy(out, title, n);
    out[n] = '\0';

    const uint8_t c = (uint8_t)out[0];

    if ( ((uint8_t)'a' <= c) && ((uint8_t)'z' >= c) )
    {
        const uint8_t capital = c - ASCII_CASE_GAP;

        out[0] = (char)capital;
    }
}

/**
 * @brief Fill the header: the letter in the disc and the title beside it.
 * @param card  The sheet's card.
 * @param title The title; not empty.
 */
static void header_set(const lv_obj_t *card, const char *title)
{
    lv_obj_t *const letter = lv_obj_find_by_name(card, "hdr_text");
    lv_obj_t *const line   = lv_obj_find_by_name(card, "hdr_title");
    char            mono[SCREEN_MONO_BYTES];

    monogram(title, mono);
    if ( NULL != letter )
    {
        lv_label_set_text(letter, mono);
    }
    if ( NULL != line )
    {
        lv_label_set_text(line, title);
        lv_obj_set_y(line, lv_obj_get_style_y(line, LV_PART_MAIN) + SCREEN_TITLE_ALONE_DY);
    }
    child_hide(card, "hdr_sub");
}

/**
 * @brief The value's style: the kit's big digits, and the signs from the title face.
 *
 * The big face (kui_font_code) draws digits and a space only; LVGL asks a
 * face's fallback for any other character, so a copy of it that names the
 * title face draws a minus or a point there, smaller. Made on the first call,
 * on the Active Object's thread, and kept.
 *
 * @return The style, to go after kui_style_sheet_code.
 */
static const lv_style_t *value_style(void)
{
    static lv_font_t  font;
    static lv_style_t style;
    static bool       ready;

    if ( !ready )
    {
        font          = kui_font_code;
        font.fallback = &kui_font_title;
        lv_style_init(&style);
        lv_style_set_text_font(&style, &font);
        ready = true;
    }
    return &style;
}

/**
 * @brief A click on a button: a tap that did not travel is a tap.
 * @param e The event; its user data is the button's press_t.
 */
static void button_cb(lv_event_t *e)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the user data is the press_t widget_create() passed */
    press_t *const    b     = (press_t *)lv_event_get_user_data(e);
    const lv_indev_t *indev = lv_indev_active();
    lv_point_t        at    = { 0, 0 };

    if ( NULL != indev )
    {
        lv_indev_get_point(indev, &at);
    }
    switch ( lv_event_get_code(e) )
    {
        case LV_EVENT_PRESSED: {
            kui_gesture_press(&b->gesture, at.x, at.y, 0U, false);
            break;
        }
        case LV_EVENT_PRESSING: {
            (void)kui_gesture_move(&b->gesture, at.x, at.y);
            break;
        }
        case LV_EVENT_CLICKED: {
            if ( (NULL != s_screen)
                && (kui_gesture_release(&b->gesture, at.x, at.y) == KUI_GESTURE_TAP) )
            {
                kode_widget_tapped(s_screen->gen, b->place);
            }
            break;
        }
        default: {
            break;
        }
    }
}

/**
 * @brief Put a value on the glass: its number, the unit under it.
 * @param s The screen; s->body exists.
 * @param i The widget's place.
 */
static void value_create(screen_t *s, uint32_t i)
{
    lv_obj_t *const box = kui_flex_create(s->body, LV_FLEX_FLOW_COLUMN, 0, 0,
                                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *const num  = lv_label_create(box);
    lv_obj_t *const unit = lv_label_create(box);

    lv_obj_set_width(box, lv_pct(100));
    lv_obj_add_style(num, &kui_style_sheet_code, LV_PART_MAIN);
    lv_obj_add_style(num, value_style(), LV_PART_MAIN);
    lv_obj_set_width(num, lv_pct(100));
    lv_obj_set_style_text_align(num, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    lv_label_set_long_mode(num, LV_LABEL_LONG_MODE_CLIP);
    lv_label_set_text(num, s->text[i]);
    lv_obj_add_style(unit, &kui_style_sheet_line_c, LV_PART_MAIN);
    lv_obj_set_width(unit, lv_pct(100));
    lv_label_set_text(unit, s->unit[i]);
    lv_obj_set_hidden(unit, '\0' == s->unit[i][0]);
    s->widget[i] = num;
}

/**
 * @brief Put a button on the glass: a pill with only its label, centred.
 * @param s The screen; s->body exists.
 * @param i The widget's place.
 */
static void button_create(screen_t *s, uint32_t i)
{
    lv_obj_t *const button = kui_pill_create(s->body, s->text[i], "");

    child_hide(button, "slot");
    child_hide(button, "sub");

    lv_obj_t *const name = lv_obj_find_by_name(button, "name");

    if ( NULL != name )
    {
        lv_obj_set_x(name, 0);
        lv_obj_set_width(name, lv_pct(100));
        lv_obj_set_align(name, LV_ALIGN_CENTER);
        lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);
    }
    lv_obj_set_scroll_on_focus(button, false);
    s->press[i].place = i;
    (void)lv_obj_add_event_cb(button, &button_cb, LV_EVENT_ALL, &s->press[i]);
    s->widget[i] = button;
}

/**
 * @brief Give widget @p i the colour kept for it, if it has one.
 *
 * Set on the default state, so the kit's held style still greys a button
 * under a finger.
 *
 * @param s The screen; widget @p i is on the glass.
 * @param i The widget's place.
 */
static void widget_paint(screen_t *s, uint32_t i)
{
    if ( (WIDGET_COLOR_KIT != s->color[i]) && (NULL != s->widget[i]) )
    {
        const lv_color_t c = lv_color_hex(s->color[i]);

        if ( (uint8_t)WIDGET_BUTTON == s->kind[i] )
        {
            lv_obj_t *const name = lv_obj_find_by_name(s->widget[i], "name");
            lv_color_t      ink  = KUI_COLOR_TEXT_ON_BG;

            if ( lv_color_luminance(c) >= WIDGET_FILL_LIGHT )
            {
                ink = KUI_COLOR_TEXT;
            }
            lv_obj_set_style_bg_color(s->widget[i], c, LV_PART_MAIN);
            if ( NULL != name )
            {
                lv_obj_set_style_text_color(name, ink, LV_PART_MAIN);
            }
        }
        else
        {
            lv_obj_set_style_text_color(s->widget[i], c, LV_PART_MAIN);
        }
    }
}

/**
 * @brief Put a canvas or an image on the glass: over its pixels, among the
 *        widgets or, at the glass's own size, over the whole card.
 * @param s The screen; s->body exists.
 * @param i The widget's place.
 */
static void canvas_create(screen_t *s, uint32_t i)
{
    s->widget[i] = NULL;
    if ( NULL != s->buf[i] )
    {
        uint32_t           w      = 0U;
        uint32_t           h      = 0U;
        void              *pixels = kode_canvasbuf_image_view(s->buf[i], &w, &h);
        lv_color_format_t  cf     = LV_COLOR_FORMAT_ARGB8888;

        if ( NULL == pixels )
        {
            const kode_raster_t grid = kode_canvasbuf_view(s->buf[i]);

            pixels = grid.px;
            w      = grid.w;
            h      = grid.h;
            cf     = LV_COLOR_FORMAT_RGB565;
        }
        const bool whole = (KODE_CANVAS_WIDTH_MAX == w) && (KODE_CANVAS_HEIGHT_MAX == h);
        lv_obj_t  *parent = s->body;

        if ( whole && (NULL != s->card) )
        {
            parent = s->card;
        }
        lv_obj_t *const cv = lv_canvas_create(parent);

        lv_canvas_set_buffer(cv, pixels, (int32_t)w, (int32_t)h, cf);
        lv_obj_set_scrollable(cv, false);
        if ( whole && (NULL != s->card) )
        {
            lv_obj_set_floating(cv, true);
            lv_obj_set_pos(cv, 0, 0);
        }
        s->widget[i] = cv;
    }
}

/**
 * @brief Put widget @p i on the glass, under the ones before it.
 * @param s The screen; s->body exists.
 * @param i The widget's place.
 */
static void widget_create(screen_t *s, uint32_t i)
{
    if ( (uint8_t)WIDGET_VALUE == s->kind[i] )
    {
        value_create(s, i);
    }
    else if ( (uint8_t)WIDGET_BUTTON == s->kind[i] )
    {
        button_create(s, i);
    }
    else if ( ((uint8_t)WIDGET_CANVAS == s->kind[i]) || ((uint8_t)WIDGET_IMAGE == s->kind[i]) )
    {
        canvas_create(s, i);
    }
    else
    {
        lv_obj_t *const l = lv_label_create(s->body);

        lv_obj_add_style(l, &kui_style_sheet_line_c, LV_PART_MAIN);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
        lv_label_set_text(l, s->text[i]);
        s->widget[i] = l;
    }
    widget_paint(s, i);
}

/**
 * @brief Build the whole screen from what is kept, in place of the last one.
 * @param s The screen; LVGL is open.
 */
static void screen_build(screen_t *s)
{
    if ( NULL != s->root )
    {
        lv_obj_delete(s->root);
    }
    s->root = kui_sheet_create(lv_screen_active());

    lv_obj_t *const card = lv_obj_find_by_name(s->root, "card");

    s->card = card;
    s->body = NULL;
    if ( NULL != card )
    {
        s->body = lv_obj_find_by_name(card, "body");
        child_hide(card, "handle");
        if ( '\0' != s->title[0] )
        {
            header_set(card, s->title);
            if ( (NULL != s->body)
                && (lv_obj_get_style_height(s->body, LV_PART_MAIN) != (int32_t)KODE_CANVAS_BODY_H_TITLE) )
            {
                KLOG_E(TAG, "the sheet's body is not %" PRIu32 " px tall; a canvas may be cropped",
                       (uint32_t)KODE_CANVAS_BODY_H_TITLE);
            }
        }
        else
        {
            child_hide(card, "hdr_disc");
            child_hide(card, "hdr_title");
            child_hide(card, "hdr_sub");
            if ( NULL != s->body )
            {
                lv_obj_set_y(s->body, KUI_SCREEN_TOP);
                lv_obj_set_height(s->body, (int32_t)KODE_CANVAS_BODY_H_BARE);
            }
        }
    }
    if ( NULL == s->body )
    {
        KLOG_E(TAG, "the kit's sheet has no body; the screen stays empty");
    }
    else
    {
        lv_obj_set_flex_align(s->body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        for ( uint32_t i = 0U; i < s->count; i++ )
        {
            widget_create(s, i);
        }
    }
}

/**
 * @brief Keep a new widget under the others and put it on the glass once it is up.
 * @param s    The screen.
 * @param r    The request; r->widget is the place it must take.
 * @param kind WIDGET_*.
 */
static void widget_keep(screen_t *s, KodeScreenReqEvt const *r, uint8_t kind)
{
    const uint32_t i = r->widget;

    if ( (i == s->count) && (KODE_WIDGETS_MAX > i) )
    {
        (void)memcpy(s->text[i], r->text, sizeof(s->text[i]));
        s->text[i][KODE_SCREEN_TEXT_MAX - 1U] = '\0';
        (void)memset(s->unit[i], 0, sizeof(s->unit[i]));
        if ( (uint8_t)WIDGET_VALUE == kind )
        {
            /* The unit follows the number's NUL, inside the request. */
            const size_t from = strlen(s->text[i]) + 1U;

            for ( size_t n = 0U; ((from + n) < KODE_SCREEN_TEXT_MAX) && ((n + 1U) < sizeof(s->unit[i]))
                                && ('\0' != r->text[from + n]); n++ )
            {
                s->unit[i][n] = r->text[from + n];
            }
        }
        s->kind[i]  = kind;
        s->color[i] = WIDGET_COLOR_KIT;
        s->count++;
        if ( s->open && (NULL != s->body) )
        {
            widget_create(s, i);
        }
    }
    else
    {
        KLOG_W(TAG, "widget %" PRIu32 " is not the next one; dropped", i);
    }
}

/**
 * @brief Keep a new canvas or image under the others and put it on the glass once it is up.
 * @param s    The screen.
 * @param r    The request; r->widget is the place it must take, r->num[0] its screen.
 * @param kind WIDGET_CANVAS or WIDGET_IMAGE.
 */
static void canvas_keep(screen_t *s, KodeScreenReqEvt const *r, uint8_t kind)
{
    const uint32_t i = r->widget;

    if ( (i == s->count) && (KODE_WIDGETS_MAX > i) )
    {
        s->buf[i] = kode_canvasbuf_get((uint32_t)r->num[0], i);
        if ( NULL == s->buf[i] )
        {
            KLOG_W(TAG, "widget %" PRIu32 " has no pixels; it stays empty", i);
        }
        s->kind[i]  = kind;
        s->color[i] = WIDGET_COLOR_KIT;
        s->widget[i] = NULL;
        s->count++;
        if ( s->open && (NULL != s->body) )
        {
            widget_create(s, i);
        }
    }
    else
    {
        KLOG_W(TAG, "widget %" PRIu32 " is not the next one; dropped", i);
    }
}

/**
 * @brief Invalidate each canvas a draw marked since the last pass, once.
 * @param s The screen.
 */
static void canvases_invalidate(const screen_t *s)
{
    for ( uint32_t i = 0U; i < s->count; i++ )
    {
        kode_canvasbuf_t *const b = s->buf[i];

        if ( ((uint8_t)WIDGET_CANVAS == s->kind[i]) && (NULL != b) && (NULL != s->widget[i])
            && kode_canvasbuf_take_dirty(b) )
        {
            (void)lv_obj_invalidate(s->widget[i]);
        }
    }
}

/**
 * @brief The next character of a text.
 * @param t   The text.
 * @param end Where it ends (the byte after the last).
 * @param i   In: where the character starts; out: where the next one does.
 * @return The code point; UTF8_BAD for bytes that are no character.
 */
static uint32_t utf8_next(const char *t, size_t end, size_t *i)
{
    const uint32_t b0    = (uint8_t)t[*i];
    uint32_t       cp    = b0;
    uint32_t       extra = 0U;

    if ( UTF8_ASCII_MAX > b0 )
    {
        extra = 0U;
    }
    else if ( (b0 & UTF8_LEAD2_MASK) == UTF8_LEAD2_MARK )
    {
        cp    = b0 & UTF8_LEAD2_DATA;
        extra = 1U;
    }
    else if ( (b0 & UTF8_LEAD3_MASK) == UTF8_LEAD3_MARK )
    {
        cp    = b0 & UTF8_LEAD3_DATA;
        extra = 2U;
    }
    else if ( (b0 & UTF8_LEAD4_MASK) == UTF8_LEAD4_MARK )
    {
        cp    = b0 & UTF8_LEAD4_DATA;
        extra = 3U;
    }
    else
    {
        cp = UTF8_BAD;
    }
    *i += 1U;
    for ( uint32_t n = 0U; n < extra; n++ )
    {
        if ( (*i >= end) || (((uint32_t)(uint8_t)t[*i] & UTF8_CONT_MASK) != UTF8_CONT_BITS) )
        {
            cp = UTF8_BAD;
            break;
        }
        cp = (cp << UTF8_DATA_BITS) | ((uint32_t)(uint8_t)t[*i] & UTF8_DATA_MASK);
        *i += 1U;
    }
    return cp;
}

/**
 * @brief The width of a line of text in the kit's body font.
 * @param line The line.
 * @param n    Its bytes, without the new-line.
 * @return Pixels, the glyphs' advances with their kerning.
 */
static int32_t line_width(const char *line, size_t n)
{
    int32_t width = 0;
    size_t  i     = 0U;

    while ( i < n )
    {
        const uint32_t letter = utf8_next(line, n, &i);
        size_t         peek   = i;
        uint32_t       next   = 0U;
        lv_font_glyph_dsc_t g;

        if ( i < n )
        {
            next = utf8_next(line, n, &peek);
        }
        if ( lv_font_get_glyph_dsc(&kui_font_body, &g, letter, next) )
        {
            width += (int32_t)g.adv_w;
        }
    }
    return width;
}

/** @brief The block one glyph's coverage is unpacked into, one for all glyphs. */
typedef struct {
    uint8_t *px;     /**< w * h bytes of PSRAM; NULL when there is none.       */
    uint32_t w;      /**< Widest glyph box of the body font.                   */
    uint32_t h;      /**< Tallest.                                             */
} cover_t;

/** @brief The coverage block; taken by prepare(), then the Active Object's thread alone uses it. */
static cover_t s_cover;

/**
 * @brief Whether the body font and its fallbacks are fonts of plain bitmaps this code unpacks.
 * @return true when every one of them is.
 */
static bool fonts_plain(void)
{
    bool ok = true;

    for ( const lv_font_t *f = &kui_font_body; (NULL != f) && ok; f = f->fallback )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; a font whose bitmaps are lv_font_get_bitmap_fmt_txt()'s keeps an lv_font_fmt_txt_dsc_t */
        const lv_font_fmt_txt_dsc_t *const d = (const lv_font_fmt_txt_dsc_t *)f->dsc;

        ok = (0U != f->static_bitmap) && (f->get_glyph_bitmap == &lv_font_get_bitmap_fmt_txt)
             && (NULL != d) && ((uint32_t)d->bitmap_format == (uint32_t)LV_FONT_FMT_TXT_PLAIN);
    }
    return ok;
}

/**
 * @brief Take the block for glyph coverage, in the caller's task.
 *
 * Sized for the biggest glyph box the body font and its fallbacks hold, found
 * in their tables with no LVGL state, so text written before the panel is up,
 * and before lv_init(), draws. Fonts this code cannot unpack, or with no glyph,
 * leave no block: the screen goes on and a text on a canvas fails "could not be drawn".
 *
 * @return false when PSRAM has no room for it.
 */
static bool cover_prepare(void)
{
    bool ok = true;

    if ( NULL == s_cover.px )
    {
        if ( fonts_plain() )
        {
            s_cover.w = 0U;
            s_cover.h = 0U;
            for ( uint32_t cp = 0U; GLYPH_SCAN_MAX >= cp; cp++ )
            {
                lv_font_glyph_dsc_t g;

                if ( lv_font_get_glyph_dsc(&kui_font_body, &g, cp, 0U) )
                {
                    if ( g.box_w > s_cover.w )
                    {
                        s_cover.w = g.box_w;
                    }
                    if ( g.box_h > s_cover.h )
                    {
                        s_cover.h = g.box_h;
                    }
                }
            }
            if ( (s_cover.w * s_cover.h) == 0U )
            {
                KLOG_E(TAG, "the body font has no glyph; text on a canvas is dropped");
            }
            else
            {
                /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is bytes */
                s_cover.px = (uint8_t *)khal_alloc_psram(COVER_ALIGN, (size_t)s_cover.w * s_cover.h);
                ok         = (NULL != s_cover.px);
            }
        }
        else
        {
            KLOG_E(TAG, "the body font is not plain bitmaps; text on a canvas is dropped");
        }
    }
    return ok;
}

/**
 * @brief Unpack a glyph's packed bits to eight-bit coverage, rows tight.
 *
 * A glyph with no stride has its rows packed one after the other; with one,
 * each row starts on a byte.
 *
 * @param g   The glyph, found in the font.
 * @param raw Its bitmap, as the font stores it.
 * @param bpp Bits per pixel: 1, 2, 4 or 8.
 */
static void cover_unpack(const lv_font_glyph_dsc_t *g, const uint8_t *raw, uint32_t bpp)
{
    const uint32_t top = (1U << bpp) - 1U;

    for ( uint32_t row = 0U; row < g->box_h; row++ )
    {
        for ( uint32_t col = 0U; col < g->box_w; col++ )
        {
            uint32_t bit = ((row * g->box_w) + col) * bpp;

            if ( 0U != g->stride )
            {
                bit = (row * g->stride * COVER_BYTE_BITS) + (col * bpp);
            }
            const uint32_t from = COVER_BYTE_BITS - bpp - (bit % COVER_BYTE_BITS);
            const uint32_t v    = ((uint32_t)raw[bit / COVER_BYTE_BITS] >> from) & top;

            s_cover.px[(row * g->box_w) + col] = (uint8_t)((v * COVER_FULL) / top);
        }
    }
}

/**
 * @brief Blend one glyph's coverage into the canvas.
 * @param b     The canvas.
 * @param g     The glyph, found in the font.
 * @param pen_x Column where the glyph's advance starts.
 * @param top   Row of the line's top.
 * @param color The colour.
 */
static void glyph_blend(kode_canvasbuf_t *b, lv_font_glyph_dsc_t *g, int32_t pen_x, int32_t top,
                        kode_color_t color)
{
    const uint32_t bpp = (uint32_t)g->format;
    const bool     fits = (0U < g->box_w) && (0U < g->box_h) && (g->box_w <= s_cover.w)
                          && (g->box_h <= s_cover.h);
    const bool     packed = (1U == bpp) || (2U == bpp) || (4U == bpp) || (8U == bpp);

    if ( fits && packed )
    {
        /* The bits as the font keeps them: no draw buffer, no LVGL state. */
        /* cppcheck-suppress misra-c2012-11.5 ; a plain font's bitmap is bytes, returned as void * */
        const uint8_t *const raw = lv_font_get_glyph_static_bitmap(g);

        if ( NULL != raw )
        {
            const int32_t gx = pen_x + g->ofs_x;
            const int32_t gy = top + (kui_font_body.line_height - kui_font_body.base_line)
                               - (int32_t)g->box_h - g->ofs_y;

            cover_unpack(g, raw, bpp);
            (void)kode_canvasbuf_blend_mask(b, gx, gy, s_cover.px, g->box_w, g->box_w, g->box_h, color);
        }
    }
}

/**
 * @brief Write one line of text on a canvas.
 * @param b     The canvas.
 * @param line  The line.
 * @param n     Its bytes, without the new-line.
 * @param x     Column the alignment is measured from.
 * @param top   Row of the line's top.
 * @param color The colour.
 * @param align Where x sits on the line.
 */
static void line_draw(kode_canvasbuf_t *b, const char *line, size_t n, int32_t x, int32_t top,
                      kode_color_t color, kode_align_t align)
{
    int32_t pen = x;
    size_t  i   = 0U;

    if ( KODE_ALIGN_CENTER == align )
    {
        pen = x - (line_width(line, n) / 2);
    }
    else if ( KODE_ALIGN_RIGHT == align )
    {
        pen = x - line_width(line, n);
    }
    else
    {
        /* left: the pen starts at x */
    }
    while ( i < n )
    {
        const uint32_t letter = utf8_next(line, n, &i);
        size_t         peek   = i;
        uint32_t       next   = 0U;
        lv_font_glyph_dsc_t g;

        if ( i < n )
        {
            next = utf8_next(line, n, &peek);
        }
        if ( lv_font_get_glyph_dsc(&kui_font_body, &g, letter, next) )
        {
            glyph_blend(b, &g, pen, top, color);
            pen += (int32_t)g.adv_w;
        }
    }
}

/**
 * @brief Write a text on a canvas, a line at a time.
 * @param s The screen.
 * @param r The request: the canvas's whole number in widget, x, y, colour and alignment in num.
 */
static void text_request(screen_t *s, KodeScreenReqEvt const *r)
{
    const uint32_t place = r->widget & KODE_WIDGET_PLACE_MASK;
    const uint32_t gen   = r->widget >> KODE_WIDGET_PLACE_BITS;
    bool           drawn = false;

    if ( (gen == s->gen) && (1U <= place) && (place <= s->count)
        && ((uint8_t)WIDGET_CANVAS == s->kind[place - 1U]) && (NULL != s->buf[place - 1U])
        && (NULL != s_cover.px) )
    {
        kode_canvasbuf_t *const b     = s->buf[place - 1U];
        const char *const       text  = r->text;
        const uint32_t          shade = (uint32_t)r->num[2];
        const uint32_t          side  = shade >> KODE_TEXT_ALIGN_SHIFT;
        const kode_align_t      align = (kode_align_t)side;
        const kode_color_t      color = shade & KODE_TEXT_RGB_MASK;
        int32_t                 top   = r->num[1];
        size_t                  from  = 0U;
        bool                    more  = true;

        while ( more )
        {
            size_t to = from;

            while ( (KODE_SCREEN_TEXT_MAX > to) && ('\0' != text[to]) && (TEXT_NEWLINE != text[to]) )
            {
                to++;
            }
            line_draw(b, &text[from], to - from, r->num[0], top, color, align);
            more = (KODE_SCREEN_TEXT_MAX > to) && (TEXT_NEWLINE == text[to]);
            from = to + 1U;
            top += lv_font_get_line_height(&kui_font_body);
        }
        drawn = true;
    }
    else
    {
        KLOG_W(TAG, "no canvas %" PRIu32 " or no text block to write on; dropped", r->widget);
    }
    /* Drawn or dropped, the caller is waiting, and is told which. */
    kode_widget_text_done((uint32_t)r->num[3], drawn);
}

/**
 * @brief Apply one request to the screen kept, and to the glass once it is up.
 * @param s The screen.
 * @param r The request.
 */
static void request_apply(screen_t *s, KodeScreenReqEvt const *r)
{
    const uint32_t i = r->widget;

    switch ( r->op )
    {
        case KODE_SCREEN_OP_NEW: {
            const uint32_t left = s->gen;

            (void)memcpy(s->title, r->text, sizeof(s->title));
            s->title[KODE_SCREEN_TEXT_MAX - 1U] = '\0';
            s->gen   = (uint32_t)r->num[0];
            s->count = 0U;
            if ( s->open )
            {
                screen_build(s);
            }
            /* The old screen's lv_canvas objects are deleted: its pixels go. */
            kode_canvasbuf_retire(left);
            (void)memset(s->buf, 0, sizeof(s->buf));
            break;
        }
        case KODE_SCREEN_OP_TEXT: {
            widget_keep(s, r, (uint8_t)WIDGET_TEXT);
            break;
        }
        case KODE_SCREEN_OP_VALUE: {
            widget_keep(s, r, (uint8_t)WIDGET_VALUE);
            break;
        }
        case KODE_SCREEN_OP_BUTTON: {
            widget_keep(s, r, (uint8_t)WIDGET_BUTTON);
            break;
        }
        case KODE_SCREEN_OP_CANVAS: {
            canvas_keep(s, r, (uint8_t)WIDGET_CANVAS);
            break;
        }
        case KODE_SCREEN_OP_IMAGE: {
            canvas_keep(s, r, (uint8_t)WIDGET_IMAGE);
            break;
        }
        case KODE_SCREEN_OP_DRAW_TEXT: {
            text_request(s, r);
            break;
        }
        case KODE_SCREEN_OP_SET: {
            if ( (i < s->count) && (((uint8_t)WIDGET_TEXT == s->kind[i])
                                   || ((uint8_t)WIDGET_VALUE == s->kind[i])) )
            {
                (void)memcpy(s->text[i], r->text, sizeof(s->text[i]));
                s->text[i][KODE_SCREEN_TEXT_MAX - 1U] = '\0';
                if ( s->open && (NULL != s->body) )
                {
                    lv_label_set_text(s->widget[i], s->text[i]);
                }
            }
            else
            {
                KLOG_W(TAG, "no widget %" PRIu32 " to change; dropped", i);
            }
            break;
        }
        case KODE_SCREEN_OP_COLOR: {
            if ( (i < s->count) && ((uint8_t)WIDGET_CANVAS != s->kind[i])
                && ((uint8_t)WIDGET_IMAGE != s->kind[i]) )
            {
                s->color[i] = (uint32_t)r->num[0] & KODE_TEXT_RGB_MASK;
                if ( s->open && (NULL != s->body) )
                {
                    widget_paint(s, i);
                }
            }
            else
            {
                KLOG_W(TAG, "no widget %" PRIu32 " to paint; dropped", i);
            }
            break;
        }
        default: {
            KLOG_W(TAG, "unknown screen request %" PRIu32 "; dropped", (uint32_t)r->op);
            break;
        }
    }
}

/**
 * @brief The table's prepare(): take the screen's memory, in the caller's task.
 * @return true when the screen has its memory (also when it already had).
 */
static bool screen_prepare(void)
{
    if ( (lv_font_get_line_height(&kui_font_body) != (int32_t)KODE_SCREEN_LINE_H)
        || (lv_font_get_line_height(&kui_font_code) != (int32_t)KODE_SCREEN_NUMBER_H) )
    {
        KLOG_E(TAG, "the kit's faces are not %" PRIu32 " and %" PRIu32 " px tall; a canvas under text may scroll",
               (uint32_t)KODE_SCREEN_LINE_H, (uint32_t)KODE_SCREEN_NUMBER_H);
    }
    if ( NULL == s_screen )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a screen_t */
        s_screen = (screen_t *)khal_alloc_psram(SCREEN_ALLOC_ALIGN, sizeof(screen_t));
    }
    return (NULL != s_screen) && cover_prepare();
}

/**
 * @brief The table's attach(): start kscreen on the memory prepare() took.
 * @param ao The standard layer's Active Object.
 */
static void screen_attach(QActive *ao)
{
    kscreen_ctor(&s_screen->screen, ao, (enum_t)KODE_SCREEN_TICK_SIG);
    kscreen_start(&s_screen->screen);
}

/**
 * @brief The table's event(): kscreen's five signals and the screen's requests.
 * @param e The event.
 * @return true when handled.
 */
static bool screen_event(QEvt const *e)
{
    screen_t *const s       = s_screen;
    bool            handled = (NULL != s);

    if ( handled )
    {
        switch ( e->sig )
        {
            case DISPLAY_STATE_SIG: {
                if ( (!s->open)
                    && (kscreen_open(&s->screen, Q_EVT_CAST(DisplayStateEvt)) == KODE_OK) )
                {
                    s->open = true;
                    lv_obj_add_style(lv_screen_active(), &kui_style_screen, LV_PART_MAIN);
                    screen_build(s);
                }
                break;
            }
            case TOUCH_EVT_SIG: {
                kscreen_touch(&s->screen, Q_EVT_CAST(TouchEvt));
                break;
            }
            case DISPLAY_REPAINT_SIG: {
                kscreen_repaint(&s->screen);
                break;
            }
            case KODE_SCREEN_TICK_SIG: {
                canvases_invalidate(s);
                kscreen_tick(&s->screen);
                break;
            }
            case KODE_SCREEN_REQ_SIG: {
                request_apply(s, Q_EVT_CAST(KodeScreenReqEvt));
                break;
            }
            default: {
                handled = false;
                break;
            }
        }
    }
    return handled;
}

const kode_screen_ops_t kode_screen_ops = { &screen_prepare, &screen_attach, &screen_event };
