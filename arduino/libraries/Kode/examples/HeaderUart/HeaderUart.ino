/* SPDX-License-Identifier: GPL-3.0-or-later */
// A line every second on Serial1: out at the header's position 1, in at position 2.

const uint32_t UART_BAUD = 115200;    // The line's speed, in bits per second.
const uint32_t UART_EVERY_MS = 1000;  // How often a line goes out.

int lines = 0;  // How many lines have gone out.

void setup() {
  Serial1.begin(UART_BAUD, SERIAL_8N1, EXP2, EXP1);
}

void loop() {
  lines++;
  Serial1.print("line ");
  Serial1.println(lines);
  delay(UART_EVERY_MS);
}
