/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_buttons.h
 * @brief The six buttons: what a byte of expander pins means, and the one pin off the expander.
 */
#ifndef KDRV_BUTTONS_H_
#define KDRV_BUTTONS_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The six buttons, by what they are for rather than where they are. */
typedef enum {
    KDRV_BTN_UP = 0,   /**< pad up      (expander P1) */
    KDRV_BTN_DOWN,     /**< pad down    (expander P3) */
    KDRV_BTN_LEFT,     /**< pad left    (expander P2) */
    KDRV_BTN_RIGHT,    /**< pad right   (expander P4) */
    KDRV_BTN_TOP,      /**< the upper individual button (GPIO35) */
    KDRV_BTN_BOT,      /**< the lower individual button (expander P0) */
    KDRV_BTN_COUNT
} kdrv_button_t;

/** @brief Make GPIO35 an input with the P4's pull-up. */
kode_status_t kdrv_buttons_init(void);

/**
 * @brief Arm GPIO35's interrupt on any edge and call @p cb from it.
 *
 * The interrupt disables itself before @p cb runs and stays off until
 * kdrv_buttons_top_rearm(): a bouncing contact therefore posts once per
 * arming, and the owner decides when it has looked.
 *
 * @param cb  Runs in interrupt context; may post a static event and no more.
 * @return KODE_DENIED for a NULL callback, KODE_ERROR when the pin could not
 *         be armed.
 */
kode_status_t kdrv_buttons_top_attach(void (*cb)(void *arg), void *arg);

/** @brief Re-enable GPIO35's interrupt after the owner has sampled it. */
void kdrv_buttons_top_rearm(void);

/**
 * @brief Arm GPIO35 as a light-sleep wake, or take that away.
 *
 * Withdrawn in the interrupt, so a held button cannot wake the chip in a loop.
 *
 * @param armed true before the board sleeps, false when it wakes.
 * @return KODE_OK, or the pin's refusal.
 */
kode_status_t kdrv_buttons_top_sleep_wake(bool armed);

/** @brief Is BUTTON_TOP down right now? (GPIO35 low.) */
bool kdrv_buttons_top_pressed(void);

/**
 * @brief One sample of all six, from the expander's byte and GPIO35.
 *
 * @param exp_levels  The TCA6408A's input register: bit n is Pn, high at rest.
 * @param top_pressed kdrv_buttons_top_pressed(), read at the same moment.
 * @return Bit (1 << kdrv_button_t) set means that button is down. P5..P7 are
 *         not buttons and are ignored whatever they read.
 */
uint8_t kdrv_buttons_decode(uint8_t exp_levels, bool top_pressed);

/** @brief The name of @p b, for logs and for the console. */
const char *kdrv_button_name(kdrv_button_t b);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BUTTONS_H_ */
