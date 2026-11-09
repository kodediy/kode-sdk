/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_wifi.c
 * @brief The Wi-Fi commands, posted to AO_Radio. The status is
 *        kode_sys_wifi_status.c's: reading it must not link AO_Radio.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kdrv_wifi.h"
#include "sdkconfig.h"

#if CONFIG_KODEOS_WIFI

_Static_assert(KODE_SYS_WIFI_SSID_MAX == sizeof(((WifiJoinEvt *)0)->ssid),
               "the facade's name limit is the join event's field");
_Static_assert(KODE_SYS_WIFI_PASS_MAX == sizeof(((WifiJoinEvt *)0)->pass),
               "the facade's password limit is the join event's field");

kode_status_t kode_sys_wifi_connect(const char *ssid, const char *pass)
{
    QEvt const *e;

    if ( NULL == ssid )
    {
        e = wifi_auto_evt();
    }
    else
    {
        const char *secret = "";

        if ( NULL != pass )
        {
            secret = pass;
        }
        if ( !kode_sys_text_fits(ssid, KODE_SYS_WIFI_SSID_MAX)
            || (('\0' != secret[0]) && !kode_sys_text_fits(secret, KODE_SYS_WIFI_PASS_MAX)) )
        {
            return KODE_DENIED;
        }
        e = wifi_join_evt(ssid, secret, true);
    }
    return kode_sys_cmd_post(AO_Radio, e, KQP_RADIO_CMD_MARGIN);
}

kode_status_t kode_sys_wifi_disconnect(void)
{
    return kode_sys_cmd_post(AO_Radio, wifi_leave_evt(), KQP_RADIO_CMD_MARGIN);
}

#else  /* !CONFIG_KODEOS_WIFI - the surface still links, and refuses */

kode_status_t kode_sys_wifi_connect(const char *ssid, const char *pass)
{
    (void)ssid; (void)pass;
    return KODE_DENIED;
}

kode_status_t kode_sys_wifi_disconnect(void)
{
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_WIFI */
