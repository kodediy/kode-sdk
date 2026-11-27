// Derived from Espressif's arduino-esp32, release 4.0.0, variants/kodedot/pins_arduino.h.
#ifndef Pins_Arduino_h
#define Pins_Arduino_h

#include <stdint.h>

// The Kode Dot with the Kode SDK in its libraries. The board's own buses and pins (its I2C bus, the NFC
// reader, the buttons, the card, the RFID front end, the display) are the SDK's; a sketch reaches them
// through Kode. The names below are the expansion header's, J3: a position is the index of EXP<n>, as the
// SDK's Kode.pin numbers it, and PIN<n> is the connector's own pin number.
//
// BOARD_HAS_PIN_REMAP is never defined here: with it Arduino makes tone() a macro, which breaks
// Kode.audio.tone().

// Serial is the USB-Serial/JTAG. UART0 (Serial0) is on J3 positions 1 and 2:
// SOC_TX0/SOC_RX0 are what the core reads for it (the P4 defaults, GPIO37/38, are the codec's I2S pins here).
#define SOC_TX0 (gpio_num_t)8
#define SOC_RX0 (gpio_num_t)9
// UART1 has no default pins: position 3, GPIO10, is the ESP32-C5's download strap. Pass pins to Serial1.begin().
#define TX1 (gpio_num_t)(-1)
#define RX1 (gpio_num_t)(-1)
static const uint8_t TX = 8;
static const uint8_t RX = 9;

// Wire on J3 positions 4 and 5. The SDK keeps the other I2C controller for the board's bus.
static const uint8_t SDA = 11;
static const uint8_t SCL = 12;

// SPI on J3 positions 6 (SCK), 7 (MOSI), 13 (MISO) and 14 (SS). The SDK keeps the other SPI controller for
// the NFC reader.
static const uint8_t SCK = 13;
static const uint8_t MOSI = 14;
static const uint8_t MISO = 15;
static const uint8_t SS = 16;

// ADC1 pins on the expansion header
static const uint8_t A0 = 16;
static const uint8_t A1 = 17;
static const uint8_t A2 = 18;
static const uint8_t A3 = 19;
static const uint8_t A4 = 20;
static const uint8_t A5 = 21;

// Expansion header J3 (2x10), in connector order. Pins 1, 2, 19, 20 are power; 10 and 11 are GND
#define EXP1  8
#define EXP2  9
#define EXP3  10
#define EXP4  11
#define EXP5  12
#define EXP6  13
#define EXP7  14
#define EXP8  21
#define EXP9  19
#define EXP10 20
#define EXP11 17
#define EXP12 18
#define EXP13 15
#define EXP14 16
// The same pins by J3 pin number
#define PIN3  8
#define PIN4  9
#define PIN5  10
#define PIN6  11
#define PIN7  12
#define PIN8  13
#define PIN9  14
#define PIN12 21
#define PIN13 19
#define PIN14 20
#define PIN15 17
#define PIN16 18
#define PIN17 15
#define PIN18 16

// The ESP32-C5's SDIO link and its reset, so the core's esp-hosted never drives J3's pins (GPIO14-19) or
// GPIO54, the P4 dev board's. The radio is the SDK's: Arduino's WiFi and Bluetooth libraries are not supported.
#define BOARD_HAS_SDIO_ESP_HOSTED
#define BOARD_SDIO_ESP_HOSTED_CLK   50
#define BOARD_SDIO_ESP_HOSTED_CMD   51
#define BOARD_SDIO_ESP_HOSTED_D0    49
#define BOARD_SDIO_ESP_HOSTED_D1    48
#define BOARD_SDIO_ESP_HOSTED_D2    53
#define BOARD_SDIO_ESP_HOSTED_D3    52
// The ESP32-C5's enable line, which resets it.
#define BOARD_SDIO_ESP_HOSTED_RESET 23

// The SDK's object, Kode, in every sketch with no include, as Serial is: the library Kode's layer, by its
// path inside the platform, which is the same from this folder in the SDK's tree. Arduino's core includes
// this file inside extern "C", where the layer's templates are refused ("template with C linkage"), so the
// layer is given back C++ linkage. The core, its libraries and a sketch's own .cpp files build with the
// layer in sight; a .c file does not see it.
#ifdef __cplusplus
extern "C++" {
#include "../../libraries/Kode/src/KodeArduino.h"
}
#endif

#endif /* Pins_Arduino_h */
