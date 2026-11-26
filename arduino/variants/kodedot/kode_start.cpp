/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_start.cpp
 * @brief The SDK started before setup(), and its console written through Serial.
 */
#include <stddef.h>
#include <stdint.h>

#include "Arduino.h"
#include "khal_console.h"
#include "kode_init.h"

#define KODE_SERIAL_LOOK_MS 1U /**< How long a console write sleeps between two looks for room in Serial. */

/**
 * @brief Arduino's start-up hook, run at the end of initArduino() on the main task: the SDK is up before setup().
 */
extern "C" void initVariant(void)
{
    kode_init();
}

/* Contract in khal_console.h. Serial is the cable port; the SDK reads nothing from it. */
extern "C" void khal_console_app_init(void)
{
    Serial.begin();
}

/* Contract in khal_console.h. Writes only what fits in Serial's ring, so Serial.write() never waits for room
 * and the sketch's own Serial settings stay as they are. It stops when len is taken, no host has the port
 * open (or Serial is not begun), or wait_ms have gone by. The one wait it cannot bound is HWCDC's own lock:
 * while the sketch's own write holds it, a look or a write waits for it up to the sketch's timeout, so the
 * pass that ends past wait_ms can run over by that much. */
extern "C" size_t khal_console_app_write(const char *buf, size_t len, uint32_t wait_ms)
{
    const uint32_t start = static_cast<uint32_t>(millis());
    size_t taken = 0U;
    int32_t room = 0;
    size_t fits = 0U;
    bool more = (NULL != buf) && (0U != len) && Serial.isConnected();

    while ( more )
    {
        room = Serial.availableForWrite();
        if ( 0 < room )
        {
            fits = static_cast<size_t>(room);
            if ( (len - taken) < fits )
            {
                fits = len - taken;
            }
            taken += Serial.write(reinterpret_cast<const uint8_t *>(&buf[taken]), fits);
        }
        if ( (len <= taken) || !Serial.isConnected()
             || (wait_ms <= (static_cast<uint32_t>(millis()) - start)) )
        {
            more = false;
        }
        else if ( 0 >= room )
        {
            delay(KODE_SERIAL_LOOK_MS);
        }
        else
        {
            /* Room was there and taken: look again at once. */
        }
    }
    return taken;
}
