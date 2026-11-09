/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_board.h
 * @brief Board parameters that are not a pin: glass, rails, links, crystals.
 */
#ifndef KBSP_BOARD_H_
#define KBSP_BOARD_H_

/** @brief The product's name, as the boot banner prints it. */
#define KBSP_PRODUCT_NAME       "Kode Dot"

/** @brief The board revision, as the boot banner and `info` print it. */
#define KBSP_BOARD_REV          "DVT"

/* ---- The AMOLED (CO5300, 410x502, MIPI-DSI) ------------------------------ */
#define KBSP_LCD_H_RES          410U  /**< Width in pixels.                  */
#define KBSP_LCD_V_RES          502U  /**< Height in pixels.                 */
#define KBSP_LCD_CORNER_R_PX    114U  /**< Corner radius; anchor nothing to a corner. */

/* The touch test's two targets (AO_Touch's guided test; the E5 screen draws
 * them): inside the clear band of the screen protector (y 125 to 380), well
 * clear of its printed edges, of the R114 corner arcs and of the bezel, on
 * opposite sides so that one tap on each proves two places of the glass. */
#define KBSP_TOUCH_TEST_A_X_PX  80    /**< First target, x: left side, past the corner arcs. */
#define KBSP_TOUCH_TEST_A_Y_PX  215   /**< First target, y: 90 px under the band's top edge. */
#define KBSP_TOUCH_TEST_B_X_PX  335   /**< Second target, x: right side, past the corner arcs. */
#define KBSP_TOUCH_TEST_B_Y_PX  285   /**< Second target, y: 95 px over the band's bottom edge. */

#define KBSP_LCD_DPHY_LDO_CHAN  3     /**< P4 LDO channel that feeds the DPHY. */
#define KBSP_LCD_DPHY_LDO_MV    2500  /**< DPHY rail, up before any MIPI traffic. */

/* ---- microSD ------------------------------------------------------------- */
#define KBSP_SD_LDO_CHAN        4     /**< P4 LDO channel of the SDMMC IO domain. */
#define KBSP_SD_WIDTH           4U    /**< Default bus width; kdrv_sd can change it. */
#define KBSP_SD_FREQ_KHZ        40000U /**< Default clock, validated on this PCB. */

/* ---- The ESP32-C5 link (esp-hosted over SDIO) ---------------------------- */
#define KBSP_C5_SDIO_WIDTH      4U    /**< Bus width the link must negotiate.  */
#define KBSP_C5_SDIO_KHZ        40000U /**< Clock the link must negotiate.     */

/* ---- The two crystals ---------------------------------------------------- */
#define KBSP_XTAL_MAIN_HZ       40000000U /**< X1, the main crystal.          */
#define KBSP_XTAL_32K_MHZ       32768000U /**< X3, the RTC crystal, in mHz.   */
#define KBSP_XTAL_32K_TOL_MHZ     327680U /**< +/-1 %: catches a missing or dead X3. */

/* ---- The motion parts' axes -----------------------------------------------
 * The board's x (right), y (top) and z (out of the glass) as one chip axis
 * each: its index in the chip's reading (0 x, 1 y, 2 z) and its sign.
 * Identity until verified on a board.
 */
#define KBSP_IMU_AXIS_X  0U     /**< The QMI8658A axis that is the board's x. */
#define KBSP_IMU_AXIS_Y  1U     /**< The one that is y.                        */
#define KBSP_IMU_AXIS_Z  2U     /**< The one that is z.                        */
#define KBSP_IMU_SIGN_X  1   /**< +1 when it points the board's way, -1 when against. */
#define KBSP_IMU_SIGN_Y  1   /**< The same, for y.                          */
#define KBSP_IMU_SIGN_Z  1   /**< The same, for z.                          */
#define KBSP_MAG_AXIS_X  0U     /**< The QMC5883P axis that is the board's x.  */
#define KBSP_MAG_AXIS_Y  1U     /**< The one that is y.                        */
#define KBSP_MAG_AXIS_Z  2U     /**< The one that is z.                        */
#define KBSP_MAG_SIGN_X  1   /**< +1 when it points the board's way, -1 when against. */
#define KBSP_MAG_SIGN_Y  1   /**< The same, for y.                          */
#define KBSP_MAG_SIGN_Z  1   /**< The same, for z.                          */

/* ---- The controllers kodeOS owns ------------------------------------------
 * The chip routes most controllers to any pin through its GPIO matrix, so
 * which one a device uses is a choice this board makes, like a pin. The
 * numbers are IDF's (I2C_NUM_x, SPIx_HOST, ADC_UNIT_x), which for SPI and
 * the ADC are one less than the chip's names: SPI2 is SPI2_HOST, 1. Every
 * driver takes its number from here beside a _Static_assert; kbsp_exp.h
 * lists the ones an app may take for J3.
 *
 * The chip has one continuous ADC engine (DMA) for both units, and the RFID
 * driver uses it: it opens the engine for each read window and closes it
 * after. An app that held it would make every RFID read fail, so J3's analog
 * positions are read one sample at a time, through khal_exp.
 */
#define KBSP_I2C_BOARD          0U    /**< I2C0: the board bus (khal_i2c).      */
#define KBSP_SPI_NFC            1U    /**< SPI2_HOST: the ST25R3916B (AO_Nfc).  */
#define KBSP_I2S_CODEC          0U    /**< I2S0: the ES8311 (AO_Audio).         */
#define KBSP_ADC_EXP            0U    /**< ADC_UNIT_1: J3's analog positions (khal_exp). */
#define KBSP_ADC_RFID           1U    /**< ADC_UNIT_2: the RFID demodulated line. */

#endif /* KBSP_BOARD_H_ */
