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

/** @brief Set the colour. (0,0,0) turns the LED off but remembers the colour. */
kode_status_t kdrv_led_rgb(uint8_t r, uint8_t g, uint8_t b);

/** @brief Drive the LED on (at the last colour set, or white) or off. */
kode_status_t kdrv_led_set(bool on);

/** @brief Invert the LED. */
kode_status_t kdrv_led_toggle(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_LED_H_ */
