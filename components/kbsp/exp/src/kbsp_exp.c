/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_exp.c
 * @brief The expansion header's table. ADC channel = GPIO - 16 (ADC1).
 */
#include "kbsp_exp.h"

#include "kbsp_pins.h"

#include <stdint.h>

#define NO_ADC  ((uint8_t)KBSP_EXP_NO_ADC)

const kbsp_exp_t KBSP_EXP[KBSP_EXP_COUNT] = {
    { (uint8_t)KBSP_PIN_EXP_1,  NO_ADC,      true  }, /* GPIO8  */
    { (uint8_t)KBSP_PIN_EXP_2,  NO_ADC,      true  }, /* GPIO9  */
    { (uint8_t)KBSP_PIN_EXP_3,  NO_ADC,      true  }, /* GPIO10 */
    { (uint8_t)KBSP_PIN_EXP_4,  NO_ADC,      true  }, /* GPIO11 */
    { (uint8_t)KBSP_PIN_EXP_5,  NO_ADC,      true  }, /* GPIO12 */
    { (uint8_t)KBSP_PIN_EXP_6,  NO_ADC,      true  }, /* GPIO13 */
    { (uint8_t)KBSP_PIN_EXP_7,  NO_ADC,      true  }, /* GPIO14 */
    { (uint8_t)KBSP_PIN_EXP_8,  (uint8_t)5U, false }, /* GPIO21 */
    { (uint8_t)KBSP_PIN_EXP_9,  (uint8_t)3U, false }, /* GPIO19 */
    { (uint8_t)KBSP_PIN_EXP_10, (uint8_t)4U, false }, /* GPIO20 */
    { (uint8_t)KBSP_PIN_EXP_11, (uint8_t)1U, false }, /* GPIO17 */
    { (uint8_t)KBSP_PIN_EXP_12, (uint8_t)2U, false }, /* GPIO18 */
    { (uint8_t)KBSP_PIN_EXP_13, NO_ADC,      true  }, /* GPIO15 */
    { (uint8_t)KBSP_PIN_EXP_14, (uint8_t)0U, false }, /* GPIO16 */
};

#ifdef CONFIG_ESP_CONSOLE_UART
/* The table leaves out UART0 alone for a UART console: a console on another
 * port would be handed to an app. */
_Static_assert(CONFIG_ESP_CONSOLE_UART_NUM == 0,
               "a UART console must be UART0, the one KBSP_EXP_CTRL leaves out");
#endif

const kbsp_exp_ctrl_t KBSP_EXP_CTRL[KBSP_EXP_CTRL_COUNT] = {
    { KBSP_EXP_I2C,  (uint8_t)KBSP_EXP_I2C_ID },
    { KBSP_EXP_SPI,  (uint8_t)KBSP_EXP_SPI_ID },
#ifndef CONFIG_ESP_CONSOLE_UART
    { KBSP_EXP_UART, 0U },   /* the console is the USB's, not UART0 */
#endif
    { KBSP_EXP_UART, 1U },
    { KBSP_EXP_UART, 2U },
    { KBSP_EXP_UART, 3U },
    { KBSP_EXP_UART, (uint8_t)KBSP_EXP_UART_LAST },
};
