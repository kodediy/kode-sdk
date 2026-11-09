/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_i2c.h
 * @brief The Kode Dot's one I2C bus: created once, a handle per device.
 */
#ifndef KHAL_I2C_H_
#define KHAL_I2C_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ESP-IDF's handle types, named without its headers so this one builds on the host. */
struct i2c_master_bus_t;
struct i2c_master_dev_t;

/** @brief A device on the shared bus. Opaque; khal_i2c_esp.c owns the type. */
typedef struct i2c_master_dev_t *khal_i2c_dev_t;

/** @brief The longest burst khal_i2c_write_regs() takes. */
#define KHAL_I2C_BURST_MAX  64U

/**
 * @brief Bring up the bus. Idempotent.
 * @return KODE_OK, or KODE_ERROR when the controller would not start.
 */
kode_status_t khal_i2c_init(void);

/**
 * @brief The raw bus handle, for vendor components that build their own device on it.
 * @return NULL before a successful khal_i2c_init().
 */
struct i2c_master_bus_t *khal_i2c_bus(void);

/**
 * @brief Whether anything acknowledges @p addr.
 *
 * Asks twice and counts the address only when both answer: a single probe of
 * an empty address can come back answered (espressif/esp-idf#13547).
 *
 * @param addr       A 7-bit address.
 * @param timeout_ms How long to wait for each answer.
 * @return true when a device answered both times.
 */
bool khal_i2c_probe(uint16_t addr, uint32_t timeout_ms);

/**
 * @brief Attach a 7-bit device at the board's bus speed.
 * @param addr The device's address.
 * @param out  Receives the handle.
 * @return KODE_OK, or KODE_ERROR before khal_i2c_init() or when the driver refused it.
 */
kode_status_t khal_i2c_add(uint16_t addr, khal_i2c_dev_t *out);

/**
 * @brief Detach a device khal_i2c_add() handed out, and free it.
 *
 * Owed on every path that walks out after the add succeeded: a forgotten
 * handle leaks the driver's device, and a retry adds a second one.
 *
 * @param dev The device; NULL is accepted and does nothing.
 * @return KODE_OK, or KODE_ERROR when the driver would not let it go.
 */
kode_status_t khal_i2c_remove(khal_i2c_dev_t dev);

/**
 * @brief Write one register.
 * @param dev The device.
 * @param reg The register.
 * @param val The byte.
 * @return KODE_OK, or KODE_ERROR when the bus said no.
 */
kode_status_t khal_i2c_write_reg(khal_i2c_dev_t dev, uint8_t reg, uint8_t val);

/**
 * @brief Read @p len bytes starting at @p reg.
 * @param dev The device.
 * @param reg The first register.
 * @param buf Where the bytes go.
 * @param len How many.
 * @return KODE_OK, or KODE_ERROR when the bus said no.
 */
kode_status_t khal_i2c_read_reg(khal_i2c_dev_t dev, uint8_t reg,
                                uint8_t *buf, size_t len);

/**
 * @brief Write @p len bytes starting at @p reg, in one transaction.
 * @param dev The device.
 * @param reg The first register; a FIFO that does not increment takes them all.
 * @param buf The bytes.
 * @param len How many, 1 to KHAL_I2C_BURST_MAX.
 * @return KODE_OK; KODE_DENIED for a bad length; KODE_ERROR when the bus said no.
 */
kode_status_t khal_i2c_write_regs(khal_i2c_dev_t dev, uint8_t reg,
                                  const uint8_t *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_I2C_H_ */
