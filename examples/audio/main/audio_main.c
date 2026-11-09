/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file audio_main.c
 * @brief kodeOS example: a note and a recording, through AO_Audio.
 */
#include "kode_sys.h"

#include "audio_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("Audio", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls - AO_Audio, and AO_Storage, which AO_Audio's recording
     * into a file links - and returns. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    AudioDemo_ctor();
    AudioDemo_start();
}
