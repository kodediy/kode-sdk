/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file rfal_port.h
 * @brief The seam ST's RFAL needs, declared here and implemented in kdrv.
 *
 * RFAL and the ST25R3916 chip driver reach their host through the
 * `platformXxx()` macros in rfal_platform.h. Those macros land here, and
 * kdrv_st25r3916.c is what defines these functions.
 *
 * The declaration is here rather than in kdrv's header on purpose: this
 * component must not include one, or `rfal` and `kdrv` would require each
 * other and the two could not be configured. Undefined until the final link
 * is exactly right - `rfal` is only ever in an image because `kdrv` asked
 * for it, so the definitions are always there when it matters.
 *
 * This is the only file in this component that is not STMicroelectronics'.
 */
#ifndef RFAL_PORT_H_
#define RFAL_PORT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Drive CS low. RFAL chains several transfers under one CS. */
void rfal_port_spi_select(void);

/** @brief Release CS. */
void rfal_port_spi_deselect(void);

/**
 * @brief One SPI transfer with CS already low.
 *
 * @param tx  Bytes to send, or NULL to clock out zeros.
 * @param rx  Where to put what comes back, or NULL to discard it.
 * @param len How many bytes. @p tx and @p rx are never the same buffer.
 */
void rfal_port_spi_txrx(const uint8_t *tx, uint8_t *rx, uint16_t len);

/** @brief Is the ST25R3916's interrupt line high? */
bool rfal_port_irq_pin_high(void);

/**
 * @brief Register what the chip driver wants called on its interrupt.
 *
 * @param cb The chip driver's handler. It reads registers over SPI, so it
 *           never runs in interrupt context here - see kdrv_st25r3916.c.
 */
void rfal_port_irq_set_callback(void (*cb)(void));

/** @brief Take the communication lock (recursive). */
void rfal_port_protect_comm(void);

/** @brief Give the communication lock back. */
void rfal_port_unprotect_comm(void);

/** @brief Take the worker lock (recursive). */
void rfal_port_protect_worker(void);

/** @brief Give the worker lock back. */
void rfal_port_unprotect_worker(void);

/** @brief Milliseconds since boot, for ST's timer.c. */
uint32_t rfal_port_tick_ms(void);

/** @brief Busy-free delay of @p ms milliseconds. */
void rfal_port_delay_ms(uint32_t ms);

/** @brief Where RFAL's own logging goes. printf-style. */
void rfal_port_log(const char *fmt, ...);

/**
 * @brief RFAL reached a state its authors call impossible.
 *
 * @param file Source file, from __FILE__.
 * @param line Line number, from __LINE__.
 */
void rfal_port_error(const char *file, int line);

#ifdef __cplusplus
}
#endif

#endif /* RFAL_PORT_H_ */
