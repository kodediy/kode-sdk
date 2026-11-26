/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_led.h
 * @brief "The status LED" - a KTD2026 RGB driver at 0x30.
 */
#ifndef KDRV_LED_H_
#define KDRV_LED_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bring the chip up. The LED starts off. */
kode_status_t kdrv_led_init(void);

/**
 * @brief Leave the KTD2026 dark, for a board with no owner for it.
 *
 * One probe, quiet, then the bring-up, which leaves the part dark.
 *
 * @return KODE_OK with the part dark; KODE_ERROR when nothing answered.
 */
kode_status_t kdrv_led_park(void);

/**
 * @brief Set the colour. (0,0,0) turns the LED off but remembers the colour.
 *
 * The colour and whether the LED is lit are kept only when the chip took the
 * write: one that did not land leaves both as they were.
 *
 * @param r Red, 0 to 255.
 * @param g Green, 0 to 255.
 * @param b Blue, 0 to 255.
 * @return KODE_OK, or the bus's error.
 */
kode_status_t kdrv_led_rgb(uint8_t r, uint8_t g, uint8_t b);

/** @brief Drive the LED on (at the last colour set, or white) or off. */
kode_status_t kdrv_led_set(bool on);

/** @brief Invert the LED. */
kode_status_t kdrv_led_toggle(void);

/**
 * @brief Whether the LED is lit, as the last write that landed left it.
 * @return true when lit; false when dark or never brought up.
 */
bool kdrv_led_is_on(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_LED_H_ */
