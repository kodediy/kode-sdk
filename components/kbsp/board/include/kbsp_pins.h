/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_pins.h
 * @brief The pin map: the only place a pin number or an I2C address is written.
 */
#ifndef KBSP_PINS_H_
#define KBSP_PINS_H_

/* ---- I2C: one shared system bus ------------------------------------------ */
#define KBSP_PIN_I2C_SDA       4U
#define KBSP_PIN_I2C_SCL       5U
#define KBSP_I2C_FREQ_HZ       400000U

#define KBSP_I2C_ADDR_LED      0x30U  /**< KTD2026 RGB LED driver (U8)      */
#define KBSP_I2C_ADDR_IOEXP    0x20U  /**< TCA6408A (U24)                   */
#define KBSP_I2C_ADDR_IMU      0x6AU  /**< QMI8658A (U10)                   */
#define KBSP_I2C_ADDR_MAG      0x2CU  /**< QMC5883P (U5)                    */
#define KBSP_I2C_ADDR_MAG_STRAY 0x0CU /**< QMC5883P (U5) after a power-up that
                                      *   loaded its address without bit 5 */
#define KBSP_I2C_ADDR_CODEC    0x18U  /**< ES8311                           */
#define KBSP_I2C_ADDR_CHARGER  0x6BU  /**< BQ25896 PMIC                     */
#define KBSP_I2C_ADDR_GAUGE    0x64U  /**< CW2217 fuel gauge                */
#define KBSP_I2C_ADDR_TOUCH    0x15U  /**< CST820, panel supplier A         */
#define KBSP_I2C_ADDR_HAPTIC   0x5AU  /**< AW86233 (U7, antenna PCB)        */

#define KBSP_PIN_IOEXP_INT     6U     /**< TCA6408A INT, active low          */
#define KBSP_PIN_DSP_TP_RST    7U     /**< Display + touch reset, active low */

/* ---- The expander's eight pins (TCA6408A, U24) --------------------------- */
#define KBSP_IOEXP_BIT_BTN_BOT   0U   /**< P0: BUTTON_BOT (SW2), also QON    */
#define KBSP_IOEXP_BIT_PAD_UP    1U   /**< P1: PAD_TOP (SW3)                 */
#define KBSP_IOEXP_BIT_PAD_LEFT  2U   /**< P2: PAD_LEFT (SW4)                */
#define KBSP_IOEXP_BIT_PAD_DOWN  3U   /**< P3: PAD_BOTTOM (SW5)              */
#define KBSP_IOEXP_BIT_PAD_RIGHT 4U   /**< P4: PAD_RIGHT (SW6)               */
#define KBSP_IOEXP_BIT_IMU_INT1  5U   /**< P5: QMI8658A INT1                 */
#define KBSP_IOEXP_BIT_TP_INT    6U   /**< P6: touch IRQ                     */
#define KBSP_IOEXP_BIT_DSP_PWR   7U   /**< P7: VCI_EN, the panel's supply    */
#define KBSP_IOEXP_DIR_INPUTS    0x7FU /**< Config register: P0..P6 in, P7 out */

#define KBSP_PIN_BUTTON_TOP    35U    /**< The sixth button, on a P4 pin    */

/* ---- microSD (SDMMC slot 0) ---------------------------------------------- */
#define KBSP_PIN_SD_D0         39U
#define KBSP_PIN_SD_D1         40U
#define KBSP_PIN_SD_D2         41U
#define KBSP_PIN_SD_D3         42U
#define KBSP_PIN_SD_CLK        43U
#define KBSP_PIN_SD_CMD        44U
#define KBSP_PIN_SD_PWR        22U    /**< Card supply through Q1; 0 = on   */

/* ---- USB ----------------------------------------------------------------- */
#define KBSP_PIN_USB_USJ_DM    24U
#define KBSP_PIN_USB_USJ_DP    25U
#define KBSP_PIN_DOCK_TX       26U    /**< Magnetic pad 2; the E5 pads loop's UART TX */
#define KBSP_PIN_DOCK_RX       27U    /**< Magnetic pad 3; the E5 pads loop's UART RX */

/* ---- Audio (ES8311 + NS4150B, I2S); needs rework KD-DVT-002 on a DVT ----- */
#define KBSP_PIN_I2S_MCLK      36U
#define KBSP_PIN_I2S_SCLK      34U
#define KBSP_PIN_I2S_ASDOUT    33U    /**< Codec -> P4 */
#define KBSP_PIN_I2S_LRCK      37U
#define KBSP_PIN_I2S_DSDIN     38U    /**< P4 -> codec */

/* ---- IR: wired to the ESP32-C5, numbered as the C5's IOs ----------------- */
#define KBSP_C5_IO_IR_TX       24
#define KBSP_C5_IO_IR_RX       23

/* ---- NFC (ST25R3916B on the antenna PCB, SPI mode 1) --------------------- */
#define KBSP_PIN_NFC_CS        28U
#define KBSP_PIN_NFC_IRQ       29U
#define KBSP_PIN_SPI_CLK       30U
#define KBSP_PIN_SPI_MOSI      31U
#define KBSP_PIN_SPI_MISO      32U

/* ---- RFID LF 125 kHz ----------------------------------------------------- */
#define KBSP_PIN_RFID_PULL     45U
#define KBSP_PIN_RFID_CARRIER  46U
#define KBSP_PIN_RFID_RF_IN    54U    /**< ADC2_CH5 */
#define KBSP_PIN_RFID_OUT      47U

/* ---- ESP32-C5 coprocessor (esp-hosted over SDIO) ------------------------- */
#define KBSP_PIN_C5_EN         23U    /**< HIGH = C5 running, LOW = reset    */
#define KBSP_PIN_C5_WAKE_P4    2U     /**< The C5 drives it high to wake the P4 (C5_WU_P4) */
#define KBSP_PIN_C5_SD_D0      49U
#define KBSP_PIN_C5_SD_D1      48U
#define KBSP_PIN_C5_SD_D2      53U
#define KBSP_PIN_C5_SD_D3      52U
#define KBSP_PIN_C5_SD_CLK     50U
#define KBSP_PIN_C5_SD_CMD     51U
#define KBSP_C5_IO_SD_CD       6      /**< C5 IO: the card slot's card-detect */
#define KBSP_C5_IO_PA_CTRL     4      /**< C5 IO: the NS4150B's enable       */
#define KBSP_C5_IO_PWR_EXT     5      /**< C5 IO: J3's +3.3V_EXT enable, on unless driven */

/* ---- Expansion header J3 (2x10): GPIO8..21, 68R in series each ----------- */
#define KBSP_PIN_EXP_1         8U
#define KBSP_PIN_EXP_2         9U
#define KBSP_PIN_EXP_3         10U
#define KBSP_PIN_EXP_4         11U
#define KBSP_PIN_EXP_5         12U
#define KBSP_PIN_EXP_6         13U
#define KBSP_PIN_EXP_7         14U
#define KBSP_PIN_EXP_8         21U
#define KBSP_PIN_EXP_9         19U
#define KBSP_PIN_EXP_10        20U
#define KBSP_PIN_EXP_11        17U
#define KBSP_PIN_EXP_12        18U
#define KBSP_PIN_EXP_13        15U
#define KBSP_PIN_EXP_14        16U

#endif /* KBSP_PINS_H_ */
