/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_station_i2c.h
 * @brief The E5 station's transfers on J3's bus, private to kdrv_station.
 */
#ifndef KDRV_STATION_I2C_H_
#define KDRV_STATION_I2C_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* J3's bus is the camera's (kdrv_ov5640_bus()): none before kdrv_ov5640_probe() or after close. */
#define KDRV_STATION_I2C_MS  200U  /**< The longest one transfer may take, in ms. */

/**
 * @brief Write @p n bytes to the chip at @p addr.
 * @param addr The chip's 7-bit address.
 * @param tx   The bytes.
 * @param n    How many.
 * @return KODE_OK; KODE_DENIED with no bus or a NULL @p tx; KODE_IO_ERROR when
 *         the transfer did not complete; KODE_ERROR when IDF refused the device.
 */
kode_status_t kdrv_station_i2c_write(uint8_t addr, const uint8_t *tx, size_t n);

/**
 * @brief Read @p n bytes from the chip at @p addr, with no register first.
 * @param addr The chip's 7-bit address.
 * @param rx   Where the bytes go.
 * @param n    How many.
 * @return As kdrv_station_i2c_write().
 */
kode_status_t kdrv_station_i2c_read(uint8_t addr, uint8_t *rx, size_t n);

/**
 * @brief Write @p ntx bytes, then read @p nrx, in one transfer with a repeated start.
 * @param addr The chip's 7-bit address.
 * @param tx   The bytes written first (a register's address).
 * @param ntx  How many.
 * @param rx   Where the bytes read go.
 * @param nrx  How many.
 * @return As kdrv_station_i2c_write().
 */
kode_status_t kdrv_station_i2c_write_read(uint8_t addr, const uint8_t *tx, size_t ntx, uint8_t *rx,
                                          size_t nrx);

/**
 * @brief Whether a chip acknowledges at @p addr.
 * @param addr The chip's 7-bit address.
 * @return true when it acknowledged; false with no bus or no acknowledge.
 */
bool kdrv_station_i2c_probe(uint8_t addr);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_STATION_I2C_H_ */
