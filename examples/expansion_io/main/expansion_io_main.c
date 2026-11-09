/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file expansion_io_main.c
 * @brief kodeOS example: the expansion header J3, driven from an Active Object.
 */
#include "kode_sys.h"

#include "expansion_io_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Expansion header", "example");

void app_main(void)
{
    /* First, always: posting to an AO before its queue exists is a boot loop. */
    kode_init();

    ExpansionIo_ctor();
    ExpansionIo_start();
}
