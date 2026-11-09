/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_lvgl_main.c
 * @brief kodeOS example: LVGL on the glass, from an Active Object of your own.
 */
#include "kode_sys.h"

#include "display_lvgl_ao.h"

/* What the launcher shows, and which categories it files this app under. */
KODE_APP_MANIFEST("Display", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls - AO_Display among them, which kscreen links and which
     * already owns the panel when this returns. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops, and this AO's first act asks AO_Display. */
    kode_init();

    DisplayLvgl_ctor();
    DisplayLvgl_start();
}
