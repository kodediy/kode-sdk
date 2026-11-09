/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file device.c
 * @brief Two displays. See device.h.
 */

/*********************
 *      INCLUDES
 *********************/

#include "device.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL2/SDL.h>

#include "kbsp_board.h"
#include "kui_tokens.h"
#include "lodepng.h"
#include "lvgl_private.h" /* lv_hit_test_info_t's fields are private in 9.6; a round switch sets res */

/*********************
 *      DEFINES
 *********************/

/* From device/kode-dot.json, the reference for these numbers, in picture
 * pixels, continuous (pixel i spans [i, i+1]): the D-pad centred at
 * (198.8, 801.4), span 254, arms 74 wide, so each arm reaches 127 from the
 * centre; the buttons 119 across, centred at (461.5, 731.3) (top) and
 * (403.2, 871.4) (bot). Rounded to whole pixels. */
#define DPAD_ARM_X   162 /**< The vertical arms' left edge. */
#define DPAD_ARM_Y   764 /**< The horizontal arms' top edge. */
#define DPAD_ARM_W   74
#define DPAD_ARM_L   90 /**< From the centre square's edge to the arm's end. */
#define DPAD_UP_Y    674
#define DPAD_DOWN_Y  838
#define DPAD_LEFT_X  72
#define DPAD_RIGHT_X 236
#define BUTTON_D     119
#define TOP_X        402
#define TOP_Y        672
#define BOT_X        344
#define BOT_Y        812
#define PRESSED_OPA  LV_OPA_30

/**********************
 *  STATIC PROTOTYPES
 **********************/

static bool load_picture(void);
static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map);
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data);
static void touch_from_mouse(const SDL_Event *ev);
static void region_event_cb(lv_event_t *e);
static void round_hit_test_cb(lv_event_t *e);
static void region(lv_obj_t *parent, kui_sim_button_t id, int32_t x, int32_t y, int32_t w, int32_t h, bool round);
static void create_regions(lv_obj_t *scr);
static int SDLCALL sdl_watch(void *user, SDL_Event *ev);

/**********************
 *  STATIC VARIABLES
 **********************/

/* generated from device/kode-dot.png by embed.cmake */
extern const unsigned char kode_dot_png[];
extern const size_t kode_dot_png_len;

static const char *const button_names[KUI_SIM_BTN_COUNT] = { "up", "down", "left", "right", "top", "bot" };

static lv_display_t *g_ui_disp;
static lv_display_t *g_frame_disp;
static lv_draw_buf_t *g_ui_buf;
static lv_obj_t *g_ui_img;
static lv_draw_buf_t *g_picture;
static float g_zoom = 1.0f;
static kui_sim_button_cb_t g_button_cb;
static void *g_button_user;

/** The touch panel, kept by sdl_watch from the mouse events as they are
 * queued. Sampling the mouse once a read period (33 ms) lost any click
 * shorter than that, a trackpad tap or a synthetic click. */
static struct {
    int32_t x, y;    /**< The last point inside the hole, in UI pixels. */
    bool inside;     /**< The pointer is over the hole now. */
    bool down;       /**< The left button went down inside the hole and is still down. */
    bool unreported; /**< A press LVGL has not read yet: kept until it has, however short the press. */
} g_touch;

/**********************
 *   GLOBAL FUNCTIONS
 **********************/

void kui_sim_device_set_button_cb(kui_sim_button_cb_t cb, void *user)
{
    g_button_cb = cb;
    g_button_user = user;
}

const char *kui_sim_device_button_name(kui_sim_button_t id)
{
    if ( KUI_SIM_BTN_COUNT > id )
    {
        return button_names[id];
    }
    return "?";
}

void kui_sim_device_button(kui_sim_button_t id, bool pressed)
{
    const char *state = "release";
    if ( pressed )
    {
        state = "press";
    }
    (void)printf("[button] %s %s\n", kui_sim_device_button_name(id), state);
    if ( NULL != g_button_cb )
    {
        g_button_cb(id, pressed, g_button_user);
    }
}

int kui_sim_device_parse_press(const char *list, kui_sim_button_t *out, int max)
{
    int n = 0;
    const char *p = list;
    for ( ;; )
    {
        const char *comma = strchr(p, ',');
        size_t len = strlen(p);
        if ( NULL != comma )
        {
            len = (size_t)(comma - p);
        }
        int found = -1;
        for ( int i = 0; KUI_SIM_BTN_COUNT > i; i++ )
        {
            if ( (strlen(button_names[i]) == len) && (strncmp(button_names[i], p, len) == 0) )
            {
                found = i;
            }
        }
        if ( 0 > found )
        {
            return -1;
        }
        if ( n >= max )
        {
            return -2;
        }
        out[n] = (kui_sim_button_t)found;
        n++;
        if ( NULL == comma )
        {
            return n;
        }
        p = comma + 1;
    }
}

void kui_sim_device_watch_install(void)
{
    (void)SDL_AddEventWatch(sdl_watch, NULL);
}

lv_display_t *kui_sim_device_create(float zoom)
{
    if ( !load_picture() )
    {
        return NULL;
    }
    g_zoom = zoom;

    /* The window and its mouse first: lv_indev_create() binds an indev to
     * the default display of the moment, and the SDL mouse must belong to
     * the window, where the buttons are. */
    g_frame_disp = lv_sdl_window_create(KUI_SIM_DEVICE_W, KUI_SIM_DEVICE_H);
    if ( NULL == g_frame_disp )
    {
        (void)fprintf(stderr, "kui_sim: cannot create the %dx%d SDL window\n", KUI_SIM_DEVICE_W, KUI_SIM_DEVICE_H);
        return NULL;
    }
    lv_sdl_window_set_title(g_frame_disp, "kui_sim");
    if ( 1.0f != zoom )
    {
        lv_sdl_window_set_zoom(g_frame_disp, zoom);
    }
    (void)lv_sdl_mouse_create();

    lv_obj_t *scr = lv_display_get_screen_active(g_frame_disp);
    lv_obj_remove_style_all(scr);
    lv_obj_set_scrollable(scr, false);
    lv_obj_set_clickable(scr, false); /* only the switches are */
    lv_obj_set_style_bg_color(scr, lv_color_hex(KUI_PALETTE_SOFT_BLACK), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    /* The UI display: the default from here on, so kui_init() and the
     * screens find it. */
    g_ui_disp = lv_display_create((int32_t)KBSP_LCD_H_RES, (int32_t)KBSP_LCD_V_RES);
    lv_display_set_color_format(g_ui_disp, LV_COLOR_FORMAT_RGB565);
    g_ui_buf = lv_draw_buf_create((int32_t)KBSP_LCD_H_RES, (int32_t)KBSP_LCD_V_RES, LV_COLOR_FORMAT_RGB565,
                                   LV_STRIDE_AUTO);
    if ( NULL == g_ui_buf )
    {
        (void)fprintf(stderr, "kui_sim: cannot allocate the %dx%d draw buffer of the UI display\n",
                       (int)KBSP_LCD_H_RES, (int)KBSP_LCD_V_RES);
        return NULL;
    }
    lv_display_set_draw_buffers(g_ui_disp, g_ui_buf, NULL);
    lv_display_set_render_mode(g_ui_disp, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(g_ui_disp, ui_flush_cb);
    lv_display_set_default(g_ui_disp);

    g_ui_img = lv_image_create(scr);
    lv_image_set_src(g_ui_img, g_ui_buf);
    lv_obj_set_pos(g_ui_img, KUI_SIM_DEVICE_SCREEN_X, KUI_SIM_DEVICE_SCREEN_Y);
    lv_obj_set_clickable(g_ui_img, false);

    lv_obj_t *picture = lv_image_create(scr);
    lv_image_set_src(picture, g_picture);
    lv_obj_set_pos(picture, 0, 0);
    lv_obj_set_clickable(picture, false);

    create_regions(scr);

    lv_indev_t *touch = lv_indev_create();
    lv_indev_set_type(touch, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(touch, touch_read_cb);
    lv_indev_set_display(touch, g_ui_disp);

    return g_ui_disp;
}

lv_obj_t *kui_sim_device_frame_screen(void)
{
    if ( NULL != g_frame_disp )
    {
        return lv_display_get_screen_active(g_frame_disp);
    }
    return NULL;
}

/**********************
 *   STATIC FUNCTIONS
 **********************/

/**
 * @brief Decode the device picture into g_picture.
 *
 * LVGL's lodepng is patched to decode into an lv_draw_buf_t passed off as
 * the unsigned char *, holding R,G,B,A where LVGL's ARGB8888 is B,G,R,A in
 * memory. Decoded once, here, so no image decoder and no cache
 * (LV_CACHE_DEF_SIZE is 0) sits between the picture and every frame.
 *
 * @return false when the picture does not decode.
 */
static bool load_picture(void)
{
    lv_draw_buf_t *decoded = NULL;
    unsigned w = 0;
    unsigned h = 0;
    unsigned err = lodepng_decode32((unsigned char **)&decoded, &w, &h, kode_dot_png, kode_dot_png_len);
    if ( (0U != err) || ((unsigned)KUI_SIM_DEVICE_W != w) || ((unsigned)KUI_SIM_DEVICE_H != h) )
    {
        const char *why = "wrong size";
        if ( 0U != err )
        {
            why = lodepng_error_text(err);
        }
        (void)fprintf(stderr, "kui_sim: the device picture does not decode (%s, %ux%u)\n", why, w, h);
        if ( NULL != decoded )
        {
            lv_draw_buf_destroy(decoded);
        }
        return false;
    }
    uint8_t *px = decoded->data;
    for ( size_t i = 0U; ((size_t)w * h * 4U) > i; i += 4U )
    {
        uint8_t r = px[i];
        px[i] = px[i + 2U];
        px[i + 2U] = r;
    }
    g_picture = decoded;
    return true;
}

/**
 * @brief Flush the UI display's buffer into the frame's image.
 *
 * DIRECT mode with one full-size buffer: the buffer always holds the whole
 * frame, so flushing is telling the frame display which part of the image
 * changed.
 */
static void ui_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    LV_UNUSED(px_map);
    lv_area_t a = *area;
    lv_area_move(&a, KUI_SIM_DEVICE_SCREEN_X, KUI_SIM_DEVICE_SCREEN_Y);
    lv_obj_invalidate_area(g_ui_img, &a);
    lv_display_flush_ready(disp);
}

/**
 * @brief The touch indev's read callback.
 *
 * The mouse over the screen hole is the touch panel: pressed only inside
 * it, at the last point inside it. A press that has not been read yet is
 * reported once even when the button is already up, so every press reaches
 * LVGL as at least one PRESSED read followed by a RELEASED one.
 */
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    LV_UNUSED(indev);
    data->point.x = g_touch.x;
    data->point.y = g_touch.y;
    bool pressed = false;
    if ( (g_touch.down && g_touch.inside) || g_touch.unreported )
    {
        pressed = true;
    }
    if ( pressed )
    {
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
    g_touch.unreported = false;
}

/**
 * @brief Turn a mouse event into the touch panel's state.
 *
 * Window pixels, minus the zoom, minus the hole. A press that begins
 * outside the hole belongs to the frame (the switches, through LVGL's own
 * SDL mouse) and never becomes a touch.
 */
static void touch_from_mouse(const SDL_Event *ev)
{
    if ( NULL == g_frame_disp )
    {
        return; /* no --device: no hole and no touch indev */
    }
    int32_t wx;
    int32_t wy;
    if ( SDL_MOUSEMOTION == ev->type )
    {
        wx = ev->motion.x;
        wy = ev->motion.y;
    }
    else if ( SDL_BUTTON_LEFT == ev->button.button )
    {
        wx = ev->button.x;
        wy = ev->button.y;
    }
    else
    {
        return;
    }
    int32_t x = (int32_t)((float)wx / g_zoom) - KUI_SIM_DEVICE_SCREEN_X;
    int32_t y = (int32_t)((float)wy / g_zoom) - KUI_SIM_DEVICE_SCREEN_Y;
    g_touch.inside = (0 <= x) && (0 <= y) && ((int32_t)KBSP_LCD_H_RES > x) && ((int32_t)KBSP_LCD_V_RES > y);
    if ( g_touch.inside )
    {
        g_touch.x = x;
        g_touch.y = y;
    }
    if ( (SDL_MOUSEBUTTONDOWN == ev->type) && g_touch.inside )
    {
        g_touch.down = true;
        g_touch.unreported = true;
    }
    else if ( SDL_MOUSEBUTTONUP == ev->type )
    {
        g_touch.down = false;
    }
}

/** @brief A region is a switch drawn on the picture: transparent until pressed, when it darkens a little. */
static void region_event_cb(lv_event_t *e)
{
    kui_sim_button_t id = (kui_sim_button_t)(uintptr_t)lv_event_get_user_data(e);
    lv_event_code_t code = lv_event_get_code(e);
    if ( LV_EVENT_PRESSED == code )
    {
        kui_sim_device_button(id, true);
    }
    else if ( (LV_EVENT_RELEASED == code) || (LV_EVENT_PRESS_LOST == code) )
    {
        kui_sim_device_button(id, false);
    }
}

/**
 * @brief The hit test of a round switch.
 *
 * LVGL hit-tests an object's rectangle whatever its radius, so a round
 * switch answers LV_EVENT_HIT_TEST itself: the point, a pixel whose centre
 * is at +0.5, must lie in the disc the region's width spans. Doubled, so it
 * stays in integers: 2*dx = 2*px + 1 - (x1 + x2 + 1).
 */
static void round_hit_test_cb(lv_event_t *e)
{
    lv_hit_test_info_t *info = lv_event_get_hit_test_info(e);
    lv_area_t c;
    lv_obj_get_coords(lv_event_get_current_target_obj(e), &c);
    int32_t dx2 = (2 * info->point->x) - c.x1 - c.x2;
    int32_t dy2 = (2 * info->point->y) - c.y1 - c.y2;
    int32_t d = lv_area_get_width(&c);
    info->res = ((dx2 * dx2) + (dy2 * dy2)) <= (d * d);
}

/**
 * @brief Create one switch region on the picture.
 * @param parent The picture's screen.
 * @param id     The switch.
 * @param x      Left edge, in picture pixels.
 * @param y      Top edge, in picture pixels.
 * @param w      Width, in picture pixels.
 * @param h      Height, in picture pixels.
 * @param round  Round hit test (the two round buttons) rather than a plain rectangle (the D-pad's arms).
 */
static void region(lv_obj_t *parent, kui_sim_button_t id, int32_t x, int32_t y, int32_t w, int32_t h, bool round)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_pos(r, x, y);
    lv_obj_set_size(r, w, h);
    lv_obj_set_clickable(r, true);
    lv_obj_set_scrollable(r, false);
    lv_obj_set_click_focusable(r, false);
    int32_t radius = 0;
    if ( round )
    {
        radius = LV_RADIUS_CIRCLE;
    }
    lv_obj_set_style_radius(r, radius, 0);
    lv_obj_set_style_bg_color(r, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(r, PRESSED_OPA, LV_STATE_PRESSED);
    lv_obj_add_event_cb(r, region_event_cb, LV_EVENT_PRESSED, (void *)(uintptr_t)id);
    lv_obj_add_event_cb(r, region_event_cb, LV_EVENT_RELEASED, (void *)(uintptr_t)id);
    lv_obj_add_event_cb(r, region_event_cb, LV_EVENT_PRESS_LOST, (void *)(uintptr_t)id);
    if ( round )
    {
        lv_obj_set_adv_hittest(r, true);
        lv_obj_add_event_cb(r, round_hit_test_cb, LV_EVENT_HIT_TEST, NULL);
    }
}

/** @brief The four arms without the centre square: the Kode Dot has no centre switch. */
static void create_regions(lv_obj_t *scr)
{
    region(scr, KUI_SIM_BTN_UP, DPAD_ARM_X, DPAD_UP_Y, DPAD_ARM_W, DPAD_ARM_L, false);
    region(scr, KUI_SIM_BTN_DOWN, DPAD_ARM_X, DPAD_DOWN_Y, DPAD_ARM_W, DPAD_ARM_L, false);
    region(scr, KUI_SIM_BTN_LEFT, DPAD_LEFT_X, DPAD_ARM_Y, DPAD_ARM_L, DPAD_ARM_W, false);
    region(scr, KUI_SIM_BTN_RIGHT, DPAD_RIGHT_X, DPAD_ARM_Y, DPAD_ARM_L, DPAD_ARM_W, false);
    region(scr, KUI_SIM_BTN_TOP, TOP_X, TOP_Y, BUTTON_D, BUTTON_D, true);
    region(scr, KUI_SIM_BTN_BOT, BOT_X, BOT_Y, BUTTON_D, BUTTON_D, true);
}

/**
 * @brief SDL's event watch: switches and the touch panel, seen as each event is queued.
 *
 * Seen before the LVGL SDL driver drains the queue: that driver, with no
 * keyboard indev, drops key events, and between two reads of the touch
 * indev a whole click can come and go. A held key auto-repeats in SDL; a
 * held switch does not, so repeats are ignored. SDL runs a watch on the
 * thread that queues the event. Here that is the main thread, inside
 * lv_timer_handler: LVGL's SDL driver calls SDL_PollEvent from an lv_timer
 * (lv_sdl_window.c), and that pumps Cocoa's mouse and key events into the
 * queue. So g_touch needs no lock against touch_read_cb, and the hook may
 * call LVGL, as it would from a region's event.
 */
static int SDLCALL sdl_watch(void *user, SDL_Event *ev)
{
    LV_UNUSED(user);
    if ( (SDL_MOUSEMOTION == ev->type) || (SDL_MOUSEBUTTONDOWN == ev->type) || (SDL_MOUSEBUTTONUP == ev->type) )
    {
        touch_from_mouse(ev);
        return 1;
    }
    if ( (SDL_KEYDOWN != ev->type) && (SDL_KEYUP != ev->type) )
    {
        return 1;
    }
    if ( 0U != ev->key.repeat )
    {
        return 1;
    }
    int id;
    switch ( ev->key.keysym.sym )
    {
        case SDLK_UP:
            id = KUI_SIM_BTN_UP;
            break;
        case SDLK_DOWN:
            id = KUI_SIM_BTN_DOWN;
            break;
        case SDLK_LEFT:
            id = KUI_SIM_BTN_LEFT;
            break;
        case SDLK_RIGHT:
            id = KUI_SIM_BTN_RIGHT;
            break;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
            id = KUI_SIM_BTN_TOP;
            break;
        case SDLK_ESCAPE:
            id = KUI_SIM_BTN_BOT;
            break;
        default:
            return 1;
    }
    kui_sim_device_button((kui_sim_button_t)id, SDL_KEYDOWN == ev->type);
    return 1;
}
