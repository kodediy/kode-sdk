/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_main.c
 * @brief kodeOS example: the two places an app keeps things, through an Active Object.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kode.h"
#include "kode_sys.h"

#include "storage_ao.h"

#define TAG "storage"

/** The key the boot counter lives under. The store is shared, and a key is at
 *  most KODE_SETTING_KEY_MAX - 1 characters. */
#define DEMO_BOOTS_KEY   "storage_boots"

/** Room for a counter written out in decimal: ten digits and a terminator,
 *  rounded up. */
#define DEMO_NUM_LEN     12U

/** Base ten, for the counter's digits. */
#define DEMO_DECIMAL     10

/* What the launcher shows, and which category it files this app under. */
KODE_APP_MANIFEST("Storage", "example");

/**
 * @brief Read the boot counter, add one and store it back.
 *
 * kode_setting_get() answers "" when the key was never written, which is the
 * first boot of this app on this board and not a fault: strtoul() reads it as
 * 0.
 *
 * @return This run's boot number, counting from 1.
 */
static uint32_t bump_boot_count(void)
{
    char     text[DEMO_NUM_LEN];
    uint32_t boots;

    boots = (uint32_t)strtoul(kode_setting_get(DEMO_BOOTS_KEY), (char **)0,
                              DEMO_DECIMAL);
    boots++;

    (void)snprintf(text, sizeof(text), "%lu", (unsigned long)boots);
    if ( !kode_setting_set(DEMO_BOOTS_KEY, text) )
    {
        KLOG_W(TAG, "could not store the boot counter: %s", kode_error());
    }
    return boots;
}

void app_main(void)
{
    uint32_t boots;

    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls, AO_Storage among them. Returns rather than blocking:
     * on ESP-IDF the scheduler is already running when app_main() is
     * entered. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    /* Here, before the AO runs: kode_setting_*() block, and kode_sys.h has no
     * store write for an AO. */
    boots = bump_boot_count();

    StorageDemo_ctor(boots);
    StorageDemo_start();
}
