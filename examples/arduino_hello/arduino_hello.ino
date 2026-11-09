/*
 * arduino_hello - the Kode Dot from the Arduino IDE.
 *
 * Board: Kode Dot. Upload writes this sketch into kodeOS's mailbox; kodeOS
 * adopts it on the next boot and runs it. It keeps running when the Serial
 * Monitor opens the port. Hold the bottom and left buttons for a second, or
 * power-cycle, to come back to kodeOS, where the sketch is now a pill in the
 * launcher.
 *
 * It prints on Serial and blinks the RGB LED, which is a KTD2026 on the
 * internal I2C bus - there is no LED on a GPIO, so this is what "blink"
 * looks like on this board.
 */
#include <Wire.h>

static const uint8_t KTD_REG_EN_RST = 0x00;
static const uint8_t KTD_REG_LED_EN = 0x04;
static const uint8_t KTD_REG_ISET3  = 0x08;   /* OUT3 current */
static const uint8_t KTD_RESET_FULL = 0x07;
static const uint8_t KTD_EN_NORMAL  = 0x18;
static const uint8_t KTD_ALL_OFF    = 0x00;
static const uint8_t KTD_BLUE_ON    = 0x10;   /* channel 3, 2 bits each */

static void ktdWrite(uint8_t reg, uint8_t value)
{
  Wire.beginTransmission(KODE_I2C_LED);
  Wire.write(reg);
  Wire.write(value);
  Wire.endTransmission();
}

void setup()
{
  Serial.begin(115200);
  Wire.begin(SDA, SCL, 400000);

  ktdWrite(KTD_REG_EN_RST, KTD_RESET_FULL);
  delay(1);
  ktdWrite(KTD_REG_EN_RST, KTD_EN_NORMAL);
  /* OUT1/OUT2/OUT3 are taken to be R/G/B; that is kdrv_led.c's assumption
   * (see its header comment), not something the schematic pins down. A Dot
   * that lights up a different colour is that mapping, not this sketch. */
  ktdWrite(KTD_REG_ISET3, 0x20);              /* a dim blue: 4 mA */
  ktdWrite(KTD_REG_LED_EN, KTD_ALL_OFF);

  pinMode(BUTTON_TOP, INPUT_PULLUP);
}

void loop()
{
  static bool on = false;
  on = !on;
  if ( on )
  {
    ktdWrite(KTD_REG_LED_EN, KTD_BLUE_ON);
  }
  else
  {
    ktdWrite(KTD_REG_LED_EN, KTD_ALL_OFF);
  }
  Serial.printf("hello from the Kode Dot: led=%d top_button=%d uptime=%lu ms\n",
                on, digitalRead(BUTTON_TOP), (unsigned long)millis());
  delay(500);
}
