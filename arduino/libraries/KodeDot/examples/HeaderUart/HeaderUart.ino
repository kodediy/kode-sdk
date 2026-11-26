/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file HeaderUart.ino
 * @brief A line every second on Serial1: out at the header's position 1, in at position 2.
 */
#include <KodeDot.h>

#define UART_BAUD     115200U /**< The line's speed, in bits per second. */
#define UART_EVERY_MS 1000U   /**< How often a line goes out. */

static uint32_t s_lines; /**< How many lines have gone out. */

void setup(void)
{
    Dot.begin();
    Serial1.begin(UART_BAUD, SERIAL_8N1, EXP2, EXP1);
}

void loop(void)
{
    s_lines++;
    Serial1.print("line ");
    Serial1.println(s_lines);
    delay(UART_EVERY_MS);
}
