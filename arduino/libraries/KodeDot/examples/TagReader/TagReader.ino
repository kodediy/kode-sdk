/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file TagReader.ino
 * @brief A screen with the battery and the last NFC tag; the top button buzzes.
 */
#include <KodeDot.h>

#define TAG_WAIT_MS 200U /**< How long one look for a tag lasts. */
#define TAG_TONE_HZ 880U /**< The tone a tag is answered with. */
#define TAG_TONE_MS 120U /**< How long that tone sounds. */

static KodeWidget s_battery; /**< The battery's line. */
static KodeWidget s_tag;     /**< The last tag's line. */

void setup(void)
{
    Dot.begin();
    (void)Dot.screen.begin("Tag reader");
    s_battery = Dot.screen.addText("Battery: --");
    s_tag     = Dot.screen.addText("No tag yet");
}

void loop(void)
{
    KodeTag tag = Dot.nfc.readId(TAG_WAIT_MS);

    if ( tag.ok )
    {
        (void)Dot.led.setColor(KODE_GREEN);
        (void)Dot.audio.tone(TAG_TONE_HZ, TAG_TONE_MS);
        (void)s_tag.setText("%s", tag.id);
    }

    if ( Dot.button.wasPressed("top") )
    {
        (void)Dot.haptic.play("click");
        (void)Dot.led.off();
    }

    (void)s_battery.setText("Battery: %u %%", (unsigned)Dot.battery.level());
}
