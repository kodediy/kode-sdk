/* SPDX-License-Identifier: GPL-3.0-or-later */
// Every second, one line from Serial.println() and one from Kode.printf().

const uint32_t LOG_BAUD = 115200;    // The Serial Monitor's speed, in bits per second.
const uint32_t LOG_EVERY_MS = 1000;  // How often the two lines go out.

int seconds = 0;  // How many times the two lines have gone out.

void setup() {
  Serial.begin(LOG_BAUD);
}

void loop() {
  seconds++;
  Serial.println("A line from Serial.println()");
  Kode.printf("A line from Kode.printf(), number %d", seconds);
  delay(LOG_EVERY_MS);
}
