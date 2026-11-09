/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_exp_bus.h
 * @brief The buses an app runs on the expansion header J3: an I2C bus, an SPI bus and a UART.
 */
#ifndef KDRV_EXP_BUS_H_
#define KDRV_EXP_BUS_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDRV_EXP_I2C_ADDR_MAX  0x7FU  /**< The highest 7-bit address.                     */
#define KDRV_EXP_I2C_PROBE_MS  100U   /**< How long one probe waits for its answer, in ms. */

#define KDRV_EXP_SPI_XFER_MAX  64U    /**< Bytes one SPI transfer moves: the controller's FIFO, with no DMA. */
#define KDRV_EXP_SPI_HZ_MIN    100000U /**< Slowest SPI clock, Hz: below it IDF's dividers run out near 78 kHz. */
#define KDRV_EXP_SPI_MODE_MAX  3U     /**< The highest SPI mode (CPOL 1, CPHA 1).                          */

#define KDRV_EXP_UART_RX_BYTES 1024U  /**< The UART's receive ring, in bytes.                              */

/* No lock here: the one caller (kode_bus.c) holds its own lock for a bus around every call. */

/**
 * @brief Open the I2C bus on @p ctrl, its data on @p sda_gpio and its clock on @p scl_gpio.
 * @param ctrl     The controller kode_sys_exp_ctrl() handed out, in IDF's numbering (I2C1 is 1).
 * @param sda_gpio The GPIO behind the data position.
 * @param scl_gpio The GPIO behind the clock position.
 * @param hz       The clock, in Hz; the caller has judged it.
 * @return KODE_OK. KODE_DENIED while a bus is open, for the board's own
 *         controller or one the chip does not have, or a zero @p hz.
 *         KODE_ERROR when IDF's driver refused.
 */
kode_status_t kdrv_exp_i2c_open(uint8_t ctrl, uint32_t sda_gpio, uint32_t scl_gpio, uint32_t hz);

/**
 * @brief One transfer with the device at @p addr: write @p out, then read into @p in, under one start.
 * @param addr    The device's 7-bit address.
 * @param out     The bytes to write; NULL when @p out_len is 0.
 * @param out_len How many to write; 0 reads only.
 * @param in      Where the bytes read go; NULL when @p in_len is 0.
 * @param in_len  How many to read; 0 writes only.
 * @param ms      The longest the transfer may take.
 * @return KODE_OK. KODE_DENIED with no bus open, an address over
 *         KDRV_EXP_I2C_ADDR_MAX, nothing to write or read, or a length with no
 *         buffer. KODE_IO_ERROR when the transfer did not complete: IDF's
 *         driver answers one code (ESP_ERR_INVALID_RESPONSE) for a missing
 *         acknowledge and for a bus that timed out (SDA or SCL held low, no
 *         pull-ups), and its own ESP_ERR_TIMEOUT means only that its bus lock
 *         was not free, which the one caller's lock rules out. KODE_ERROR
 *         otherwise.
 */
kode_status_t kdrv_exp_i2c_xfer(uint8_t addr, const uint8_t *out, size_t out_len, uint8_t *in,
                                size_t in_len, uint32_t ms);

/**
 * @brief Whether a device answers at @p addr.
 *
 * An address counts only when it answers two probes running: after a NACK,
 * IDF's driver can take the late completion of the transfer before for this
 * one's (espressif/esp-idf#13547). Each probe waits KDRV_EXP_I2C_PROBE_MS.
 * @param addr The 7-bit address.
 * @return KODE_OK when a device answered. KODE_IO_ERROR when none did.
 *         KODE_TIMEOUT when the bus timed out (SDA or SCL held low, no
 *         pull-ups): a probe, unlike a transfer, tells the two apart.
 *         KODE_DENIED with no bus open or an address over KDRV_EXP_I2C_ADDR_MAX.
 *         KODE_ERROR otherwise.
 */
kode_status_t kdrv_exp_i2c_probe(uint8_t addr);

/**
 * @brief Close the I2C bus and free its controller; the pins stay as the matrix left them.
 * @return KODE_OK, also when no bus is open. KODE_ERROR when IDF's driver
 *         refused; the bus is forgotten all the same.
 */
kode_status_t kdrv_exp_i2c_close(void);

/**
 * @brief Open the SPI bus on @p host with one device on @p cs.
 * @param host The host kode_sys_exp_ctrl() handed out, in IDF's numbering (SPI3 is 2).
 * @param sck  The GPIO behind the clock position.
 * @param mosi The GPIO behind the data-out position.
 * @param miso The GPIO behind the data-in position.
 * @param cs   The GPIO behind the chip-select position, low while a transfer runs.
 * @param hz   The clock, in Hz, KDRV_EXP_SPI_HZ_MIN or more; the caller has
 *             judged the top. From KDRV_EXP_SPI_HZ_MIN up the bus runs at
 *             the nearest clock the controller's dividers make. Below it IDF
 *             6.1 falls back to an 80 MHz
 *             source divided by at most 1024, so the bus would run near
 *             78 kHz whatever was asked.
 * @param mode 0 to KDRV_EXP_SPI_MODE_MAX: bit 1 the clock's idle level (CPOL),
 *             bit 0 the edge data is sampled on (CPHA).
 * @return KODE_OK. KODE_DENIED while a bus is open, for the flash's host, the
 *         board's own or one the chip does not have, @p hz under
 *         KDRV_EXP_SPI_HZ_MIN or a mode
 *         over KDRV_EXP_SPI_MODE_MAX. KODE_ERROR when IDF's driver refused (a
 *         host something else initialised, no memory, a pin it cannot route).
 */
kode_status_t kdrv_exp_spi_open(uint8_t host, uint32_t sck, uint32_t mosi, uint32_t miso,
                                uint32_t cs, uint32_t hz, uint8_t mode);

/**
 * @brief One full-duplex transfer: @p len bytes out of @p out while @p len come in to @p in.
 *
 * The chip select goes low for the whole transfer. The controller clocks
 * whether or not a device answers, so a transfer cannot time out: with nothing
 * on the bus, @p in reads what the data-in line floated at.
 * @param out The bytes to send.
 * @param in  Where the bytes received go; NULL sends only.
 * @param len 1 to KDRV_EXP_SPI_XFER_MAX.
 * @return KODE_OK. KODE_DENIED with no bus open, a NULL @p out, or a @p len
 *         of 0 or over KDRV_EXP_SPI_XFER_MAX. KODE_ERROR when IDF's driver
 *         refused.
 */
kode_status_t kdrv_exp_spi_xfer(const uint8_t *out, uint8_t *in, size_t len);

/**
 * @brief Close the SPI bus and free its host; the pins stay as the matrix left them.
 * @return KODE_OK, also when no bus is open. KODE_ERROR when IDF's driver
 *         refused; the bus is forgotten all the same.
 */
kode_status_t kdrv_exp_spi_close(void);

/**
 * @brief Open the UART @p port, sending on @p tx and receiving on @p rx: 8 data bits, no parity, 1 stop bit, no flow control.
 *
 * IDF allocates the receive ring here. While the image does not set
 * CONFIG_UART_ISR_IN_IRAM, the UART's interrupt cannot run during a flash
 * write, and bytes that arrive then are lost once the controller's 128-byte
 * FIFO is full; with it set, IDF takes the ring and the driver's state from
 * internal RAM instead of the default heap.
 * @param port The UART kode_sys_exp_ctrl() handed out, in IDF's numbering.
 * @param tx   The GPIO behind the transmit position.
 * @param rx   The GPIO behind the receive position.
 * @param baud The rate; the caller has judged it.
 * @return KODE_OK. KODE_DENIED while a UART is open, for a port the chip does
 *         not have or the console's, or a zero @p baud. KODE_ERROR when IDF's
 *         driver refused (a port something else installed, no memory, a rate
 *         its divider cannot make).
 */
kode_status_t kdrv_exp_uart_open(uint8_t port, uint32_t tx, uint32_t rx, uint32_t baud);

/**
 * @brief Send @p len bytes: returns once the last of them is in the controller's FIFO.
 *
 * The wait is the time the bytes take at the rate, less the 128 the FIFO
 * holds; with no flow control the line never holds it back.
 * @param d    The bytes.
 * @param len  How many; at least one.
 * @param sent Out: how many were queued, @p len on KODE_OK and 0 otherwise.
 * @return KODE_OK. KODE_DENIED with no UART open, a NULL pointer or a zero
 *         @p len. KODE_ERROR when IDF's driver refused.
 */
kode_status_t kdrv_exp_uart_write(const uint8_t *d, size_t len, size_t *sent);

/**
 * @brief Take what the UART received, waiting up to @p ms for it.
 *
 * Returns what is in the ring at once when that fills @p cap; otherwise waits
 * for more until @p cap is full or @p ms have gone by since the call, so the
 * wait never passes @p ms (to the tick). 0 takes only what has already come.
 * @param d   Where the bytes go.
 * @param cap Room in @p d; at least one.
 * @param ms  The longest wait, in ms.
 * @param got Out: how many bytes are in @p d; 0 when nothing came.
 * @return KODE_OK, also when nothing came. KODE_DENIED with no UART open, a
 *         NULL pointer or a zero @p cap. KODE_ERROR when IDF's driver refused.
 */
kode_status_t kdrv_exp_uart_read(uint8_t *d, size_t cap, uint32_t ms, size_t *got);

/**
 * @brief Close the UART and free its ring; the pins stay as the matrix left them.
 * @return KODE_OK, also when no UART is open. KODE_ERROR when IDF's driver
 *         refused; the UART is forgotten all the same.
 */
kode_status_t kdrv_exp_uart_close(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_EXP_BUS_H_ */
