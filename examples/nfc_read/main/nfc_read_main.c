/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file nfc_read_main.c
 * @brief kodeOS example: the buttons read an NFC tag, through an Active Object.
 */
#include "kode_sys.h"

#include "nfc_read_ao.h"

/* Optional: what the launcher shows, and which categories it files this app
 * under - up to four, comma-separated, free strings. */
KODE_APP_MANIFEST("NFC reader", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls - AO_Nfc among them. kode_init() first, always: posting
     * to an AO whose queue does not exist yet is a boot loop. */
    kode_init();

    NfcRead_ctor();
    NfcRead_start();
}
