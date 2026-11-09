/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file network_main.c
 * @brief kodeOS example: the network from an Active Object, without blocking.
 */
#include "kode_sys.h"

#include "network_ao.h"

/* Optional: what the launcher shows, and which categories it files this app
 * under. Leave it out and the app lands in "Other". */
KODE_APP_MANIFEST("Network", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls - AO_Net among them, and AO_Radio with it, because the
     * AO asks for the network - in the one order that works. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    Network_ctor();
    Network_start();
}
