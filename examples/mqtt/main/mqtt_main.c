/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mqtt_main.c
 * @brief kodeOS example: the buttons talk to an MQTT broker, through an Active Object.
 */
#include "kode_sys.h"

#include "mqtt_ao.h"

/* Optional: what the launcher shows, and which categories it files this app
 * under. Leave it out and the app lands in "Other". */
KODE_APP_MANIFEST("MQTT", "example");

void app_main(void)
{
    /* Brings up logging, QF, the event pools, the core AOs and every owner
     * this app calls, AO_Radio and AO_Net among them. Returns rather than
     * blocking. */
    /* kode_init() first, always: posting to an AO before its queue exists
     * asserts and boot-loops. */
    kode_init();

    MqttDemo_ctor();
    MqttDemo_start();
}
