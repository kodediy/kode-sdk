/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file imu_main.c
 * @brief kodeOS example: the motion sensors, read without blocking.
 */
#include "kode_sys.h"

#include "imu_ao.h"

/* What the launcher shows, and the category it files this app under. */
KODE_APP_MANIFEST("IMU", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls - AO_Imu, the owner of both motion sensors, among them -
     * in the one order that works. Returns rather than blocking. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    ImuDemo_ctor();
    ImuDemo_start();
}
