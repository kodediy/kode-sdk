/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file buttons_led_main.c
 * @brief kodeOS example: the buttons drive the LED, through an Active Object.
 */
#include "kode_sys.h"

#include "buttons_led_ao.h"

/* Optional: what the launcher shows, and which categories it files this app
 * under - up to four, comma-separated, free strings ("game", "audio",
 * whatever groups yours). Leave it out and the app lands in "Other". */
KODE_APP_MANIFEST("Buttons + LED", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls, in the one order that works. Returns rather than
     * blocking: on ESP-IDF the scheduler is already running when app_main() is
     * entered, so QF_run() hands control straight back. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    ButtonsLed_ctor();
    ButtonsLed_start();
}
