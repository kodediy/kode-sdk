/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file power_main.c
 * @brief kodeOS example: read the battery and switch the board off, through an Active Object.
 */
#include "kode_sys.h"

#include "power_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Power", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls. AO_Power is part of the core: the PMIC and the gauge
     * have an owner in every app. Returns rather than blocking. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    PowerDemo_ctor();
    PowerDemo_start();
}
