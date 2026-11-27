/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file SerialLog.ino
 * @brief Every second, one line from Serial.println() and one from Kode.printf().
 */
#define LOG_BAUD     115200U /**< The Serial Monitor's speed, in bits per second. */
#define LOG_EVERY_MS 1000U   /**< How often the two lines go out. */

static uint32_t s_seconds; /**< How many times the two lines have gone out. */

void setup(void)
{
    Serial.begin(LOG_BAUD);
}

void loop(void)
{
    s_seconds++;
    Serial.println("A line from Serial.println()");
    Kode.printf("A line from Kode.printf(), number %lu", (unsigned long)s_seconds);
    delay(LOG_EVERY_MS);
}
