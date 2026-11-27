/* SPDX-License-Identifier: GPL-3.0-or-later */
// The RGB LED on and off, and a line in the board's log each time.

const uint32_t BLINK_HALF_MS = 500;  // How long the LED stays on, and off.

int blinks = 0;  // How many times the LED has changed.

void setup() {
  Kode.led.setColor(KODE_PURE_BLUE);
}

void loop() {
  Kode.led.toggle();
  blinks++;
  Kode.printf("blink %d, battery %d %%", blinks, Kode.battery.level());
  delay(BLINK_HALF_MS);
}
