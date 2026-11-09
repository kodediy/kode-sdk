/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file console_actions_main.c
 * @brief kodeOS example: give the console a variable and two functions of your own.
 */
#include "kode_sys.h"

#include "console_actions_ao.h"

/* Optional: what the launcher shows, and which categories it files this app
 * under - up to four, comma-separated, free strings. Leave it out and the app
 * lands in "Other". */
KODE_APP_MANIFEST("Console actions", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls, in the one order that works. Returns rather than
     * blocking: on ESP-IDF the scheduler is already running when app_main() is
     * entered, so QF_run() hands control straight back. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    ConsoleActions_ctor();
    ConsoleActions_start();
}
