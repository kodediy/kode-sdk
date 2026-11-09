/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file keys.h
 * @brief The default meaning of the switches: a keypad that walks the focus ring.
 */
#ifndef KUI_SIM_KEYS_H
#define KUI_SIM_KEYS_H

#include "lvgl.h"

/**
 * @brief Install the keypad indev on the UI display, with its own group.
 * @param ui_disp The UI display.
 */
void kui_sim_keys_install(lv_display_t *ui_disp);

/**
 * @brief Drop the edges not read yet, release a key the keypad still holds, and empty the group.
 *
 * Call before deleting a screen; the next press rebuilds the group from the
 * new one.
 */
void kui_sim_keys_reset(void);

#endif /* KUI_SIM_KEYS_H */
