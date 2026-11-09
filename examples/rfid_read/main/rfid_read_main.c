/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file rfid_read_main.c
 * @brief kodeOS example: read a 125 kHz tag from an Active Object.
 */
#include "kode_sys.h"

#include "rfid_read_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("RFID 125 kHz", "example");

void app_main(void)
{
    /* First, always: every AO kodeOS starts - the core, and AO_Rfid, which
     * this app links by calling it - is running when this returns, and
     * posting to one whose queue does not exist yet is a boot loop. */
    kode_init();

    RfidRead_ctor();
    RfidRead_start();
}
