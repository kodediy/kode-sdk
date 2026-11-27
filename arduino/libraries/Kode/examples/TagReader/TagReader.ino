/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file TagReader.ino
 * @brief A screen with the battery and the last NFC tag; the top button buzzes.
 */
#define TAG_WAIT_MS 200U /**< How long one look for a tag lasts. */
#define TAG_TONE_HZ 880U /**< The tone a tag is answered with. */
#define TAG_TONE_MS 120U /**< How long that tone sounds. */

static KodeWidget s_battery; /**< The battery's line. */
static KodeWidget s_tag;     /**< The last tag's line. */

void setup(void)
{
    Kode.begin();
    (void)Kode.screen.show("Tag reader");
    s_battery = Kode.screen.addText("Battery: --");
    s_tag     = Kode.screen.addText("No tag yet");
}

void loop(void)
{
    KodeTag tag = Kode.nfc.readId(TAG_WAIT_MS);

    if ( tag.ok )
    {
        (void)Kode.led.setColor(KODE_GREEN);
        (void)Kode.audio.tone(TAG_TONE_HZ, TAG_TONE_MS);
        (void)s_tag.setText("%s", tag.id);
    }

    if ( Kode.button.wasPressed("top") )
    {
        (void)Kode.haptic.play("click");
        (void)Kode.led.off();
    }

    (void)s_battery.setText("Battery: %u %%", (unsigned)Kode.battery.level());
}
