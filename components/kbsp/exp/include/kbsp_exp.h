/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_exp.h
 * @brief The expansion header J3: fourteen positions and what each may be.
 */
#ifndef KBSP_EXP_H_
#define KBSP_EXP_H_

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KBSP_EXP_NO_ADC  0xFFU  /**< adc_chan of a position with no ADC. */

/** @brief One position of J3. Every position is digital and matrix-routed. */
typedef struct {
    uint8_t gpio;      /**< The P4 GPIO behind it.                       */
    uint8_t adc_chan;  /**< ADC1 channel (EXP8..12, 14) or KBSP_EXP_NO_ADC. */
    bool    lp;        /**< An LP pin (EXP1..7, 13): works with the chip asleep. */
} kbsp_exp_t;

#define KBSP_EXP_COUNT  14U  /**< Positions on the header. */

/** @brief The table, indexed by position minus one. */
extern const kbsp_exp_t KBSP_EXP[KBSP_EXP_COUNT];

/** @brief The kinds of controller an app may take for J3. */
typedef enum {
    KBSP_EXP_I2C = 0,   /**< An I2C controller.  */
    KBSP_EXP_SPI,       /**< A general SPI host. */
    KBSP_EXP_UART,      /**< A UART.             */
} kbsp_exp_bus_t;

/** @brief One controller an app may take, as IDF numbers it. */
typedef struct {
    kbsp_exp_bus_t bus;  /**< Its kind.                                    */
    uint8_t        id;   /**< What IDF takes: I2C_NUM_x, SPIx_HOST or UART_NUM_x. */
} kbsp_exp_ctrl_t;

#define KBSP_EXP_I2C_ID      1U  /**< I2C_NUM_1; I2C0 is the board bus.          */
#define KBSP_EXP_SPI_ID      2U  /**< SPI3_HOST; SPI1 is the flash, SPI2 the NFC reader. */
#define KBSP_EXP_UART_LAST   4U  /**< UART_NUM_4, the chip's last HP UART.       */
#ifdef CONFIG_ESP_CONSOLE_UART
#define KBSP_EXP_UART_FIRST  1U  /**< UART0 is the console's in this build.      */
#else
#define KBSP_EXP_UART_FIRST  0U  /**< The console is the USB's: UART0 is free.   */
#endif

/**
 * @brief How many controllers an app may take: I2C1, SPI3 and UART0..4, less
 *        UART0 while the console is a UART. The LP controllers reach only the
 *        LP pins and are not offered; kbsp_board.h names the rest, kodeOS's.
 */
#define KBSP_EXP_CTRL_ONE_EACH  2U  /**< The one I2C and the one SPI. */
#define KBSP_EXP_CTRL_COUNT  \
    (KBSP_EXP_CTRL_ONE_EACH + ((KBSP_EXP_UART_LAST - KBSP_EXP_UART_FIRST) + 1U))

/** @brief The controllers an app may take, by kind, lowest first. */
extern const kbsp_exp_ctrl_t KBSP_EXP_CTRL[KBSP_EXP_CTRL_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* KBSP_EXP_H_ */
