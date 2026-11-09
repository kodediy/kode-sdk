/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file device.h
 * @brief The Kode Dot around the simulator's screen: the picture, the six switches, and where the UI sits in it.
 */
#ifndef KUI_SIM_DEVICE_H_INCLUDED
#define KUI_SIM_DEVICE_H_INCLUDED /* not KUI_SIM_DEVICE_H: that is the picture height below */

#include <stdbool.h>

#include "lvgl.h"

/* device/kode-dot.png: 602x979 at 12.39 px/mm, the active area cut out at
 * (96,100). See device/kode-dot.json. */
#define KUI_SIM_DEVICE_W        602
#define KUI_SIM_DEVICE_H        979
#define KUI_SIM_DEVICE_SCREEN_X 96
#define KUI_SIM_DEVICE_SCREEN_Y 100

/** The six switches, numbered as kdrv's kdrv_button_t. */
typedef enum {
    KUI_SIM_BTN_UP = 0,
    KUI_SIM_BTN_DOWN,
    KUI_SIM_BTN_LEFT,
    KUI_SIM_BTN_RIGHT,
    KUI_SIM_BTN_TOP, /**< The blue round button, upper right (BUTTON_TOP, GPIO35). */
    KUI_SIM_BTN_BOT, /**< The red round button, below it (BUTTON_BOT, expander P0). */
    KUI_SIM_BTN_COUNT
} kui_sim_button_t;

/** @brief One edge of a switch: the picture, the keyboard, or an injected press. */
typedef void (*kui_sim_button_cb_t)(kui_sim_button_t id, bool pressed, void *user);

/**
 * @brief Install the hook every switch edge is handed to.
 *
 * Called as "[button] top press" is printed; the simulator installs
 * keys.c's default. The hook runs on the main thread, so it may call LVGL:
 * from inside lv_timer_handler when the edge comes from the picture or the
 * keyboard.
 *
 * @param cb   The hook; NULL clears it.
 * @param user Passed through to `cb` unchanged.
 */
void kui_sim_device_set_button_cb(kui_sim_button_cb_t cb, void *user);

/**
 * @brief Report one edge of a switch.
 * @param id      The switch.
 * @param pressed true for a press, false for its release.
 */
void kui_sim_device_button(kui_sim_button_t id, bool pressed);

/**
 * @brief A switch's name, as printed and as kui_sim_device_parse_press() reads it.
 * @param id The switch.
 * @return "up", "down", "left", "right", "top", "bot", or "?" when `id` is out of range.
 */
const char *kui_sim_device_button_name(kui_sim_button_t id);

/**
 * @brief Parse a comma-separated list of switch names.
 *
 * An empty name ("", "up,", ",up", "up,,down") matches no switch, so it is
 * refused as an unknown one is.
 *
 * @param list The names, comma-separated.
 * @param out  Filled with the switches named, in order.
 * @param max  How many `out` holds.
 * @return The count, or -1 for an unknown name, or -2 when there are more than `max`.
 */
int kui_sim_device_parse_press(const char *list, kui_sim_button_t *out, int max);

/**
 * @brief Watch SDL's events as they are queued.
 *
 * Arrow keys, Return and Escape become the six switches, in any mode, and
 * under --device the left mouse button over the screen hole becomes the
 * touch panel, so a click shorter than a read period still reaches LVGL.
 * Call after the SDL window exists.
 */
void kui_sim_device_watch_install(void);

/**
 * @brief Create the UI display and the SDL window with the picture around it.
 * @param zoom The window's zoom factor.
 * @return The UI display (410x502, the default), or NULL when the picture
 *         does not decode or the window or the UI's draw buffer cannot be
 *         made (said on stderr).
 */
lv_display_t *kui_sim_device_create(float zoom);

/**
 * @brief The window's screen: picture, UI and buttons.
 * @return What a framed --snapshot captures.
 */
lv_obj_t *kui_sim_device_frame_screen(void);

#endif /* KUI_SIM_DEVICE_H_INCLUDED */
