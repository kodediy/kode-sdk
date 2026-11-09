/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_pcf8574.h
 * @brief The E5 station's relays: a PCF8574 at KBSP_STATION_PCF8574_ADDR on J3's bus.
 */
#ifndef KDRV_PCF8574_H_
#define KDRV_PCF8574_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDRV_PCF8574_R1_BIT  0x01U  /**< P0: relay R1; 0 is on.                    */
#define KDRV_PCF8574_R2_BIT  0x02U  /**< P1: relay R2, through R1; 0 is on.        */
#define KDRV_PCF8574_REST    0xFFU  /**< Every relay at rest, every pin an input.  */

/**
 * @brief Drive the eight pins to @p byte.
 *
 * R2 on with R1 at rest would feed the computer's 5 V through the Kode to the station's load.
 * @param byte The pins, P0 in bit 0; a relay is on while its bit is 0.
 * @return KODE_OK; KODE_DENIED for R2 on with R1 at rest (nothing is written)
 *         or with no bus; KODE_IO_ERROR when the chip did not take it.
 */
kode_status_t kdrv_pcf8574_write(uint8_t byte);

/**
 * @brief Read the eight pins.
 * @param byte Out: the pins, P0 in bit 0.
 * @return KODE_OK; KODE_DENIED for a NULL @p byte or with no bus;
 *         KODE_IO_ERROR when the chip did not answer.
 */
kode_status_t kdrv_pcf8574_read(uint8_t *byte);

/**
 * @brief Whether the station is there: the relay expander and the ADC both acknowledge.
 *
 * It rides the camera's J3 bus (kdrv_ov5640_bus()), so kdrv_ov5640_probe() comes first.
 * @return true when both answered; false with either missing or no bus.
 */
bool kdrv_station_present(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_PCF8574_H_ */
