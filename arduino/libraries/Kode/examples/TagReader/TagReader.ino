/* SPDX-License-Identifier: GPL-3.0-or-later */
// A screen with the battery and the last NFC tag. A tag turns the LED green and sounds a tone;
// the top button gives a tap and turns the LED off.

const uint32_t TAG_WAIT_MS = 200;  // How long one look for a tag lasts.
const uint16_t TAG_TONE_HZ = 880;  // The tone a tag is answered with.
const uint32_t TAG_TONE_MS = 120;  // How long that tone sounds.

KodeWidget batteryLine;  // The battery's line.
KodeWidget tagLine;      // The last tag's line.
int shownLevel = -1;     // The battery level the screen shows.

void setup() {
  Kode.screen.show("Tag reader");
  batteryLine = Kode.screen.addText("Battery: --");
  tagLine = Kode.screen.addText("No tag yet");
}

void loop() {
  KodeTag tag = Kode.nfc.readId(TAG_WAIT_MS);
  if (tag.ok) {
    tagLine.setText("Tag %s", tag.id);
    Kode.led.setColor(KODE_PURE_GREEN);
    Kode.audio.tone(TAG_TONE_HZ, TAG_TONE_MS);
  }

  if (Kode.button.wasPressed("top")) {
    Kode.haptic.play("click");
    Kode.led.off();
  }

  int level = Kode.battery.level();
  if (level != shownLevel) {
    batteryLine.setText("Battery: %d %%", level);
    shownLevel = level;
  }
}
