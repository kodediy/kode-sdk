/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file Blink.ino
 * @brief The RGB LED on and off, and a line on the console each time.
 */
#define BLINK_HALF_MS 500U /**< How long the LED stays on, and off. */

static uint32_t s_blinks; /**< How many times the LED has changed. */

void setup(void)
{
    Kode.begin();
    (void)Kode.led.setColor(KODE_BLUE);
}

void loop(void)
{
    (void)Kode.led.toggle();
    s_blinks++;
    Kode.printf("blink %lu, battery %u %%", (unsigned long)s_blinks, (unsigned)Kode.battery.level());
    delay(BLINK_HALF_MS);
}
