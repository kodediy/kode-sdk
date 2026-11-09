/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_wifi_status.c
 * @brief The Wi-Fi status, read from the driver.
 */
/* Names no door of AO_Radio, so reading the state links nothing that starts the radio. */
#include "kode_sys.h"

#include "kdrv_wifi.h"
#include "sdkconfig.h"

#include <string.h>

_Static_assert((uint32_t)KDRV_WIFI_IDLE == KODE_SYS_WIFI_STATE_IDLE, "Wi-Fi states follow the driver's");
_Static_assert((uint32_t)KDRV_WIFI_CONNECTING == KODE_SYS_WIFI_STATE_CONNECTING, "Wi-Fi states follow the driver's");
_Static_assert((uint32_t)KDRV_WIFI_GOT_IP == KODE_SYS_WIFI_STATE_GOT_IP, "Wi-Fi states follow the driver's");
_Static_assert((uint32_t)KDRV_WIFI_DISCONNECTED == KODE_SYS_WIFI_STATE_DISCONNECTED, "Wi-Fi states follow the driver's");

#if CONFIG_KODEOS_WIFI

kode_status_t kode_sys_wifi_status(kode_sys_wifi_status_t *out)
{
    kdrv_wifi_state_t state;
    uint32_t          ip4 = 0U;

    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    (void)memset(out, 0, sizeof(*out));
    state      = kdrv_wifi_state(&ip4);
    out->state = (uint8_t)state;
    if ( KDRV_WIFI_GOT_IP == state )
    {
        out->ip4 = ip4;
    }
    if ( (KDRV_WIFI_GOT_IP == state) || (KDRV_WIFI_CONNECTING == state) )
    {
        kdrv_wifi_ssid_copy(out->ssid, sizeof(out->ssid));
    }
    return KODE_OK;
}

#else  /* !CONFIG_KODEOS_WIFI */

kode_status_t kode_sys_wifi_status(kode_sys_wifi_status_t *out)
{
    (void)out;
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_WIFI */
