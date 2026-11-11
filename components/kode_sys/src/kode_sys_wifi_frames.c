/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_wifi_frames.c
 * @brief The Wi-Fi frames request through AO_Radio: an AO's start and stop.
 */
#include "kode_sys.h"

#include "kaos.h"
#include "sdkconfig.h"

#if CONFIG_KODEOS_WIFI

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_wifi_frames(QActive *me, bool on, uint8_t channel)
{
    if ( NULL == me )
    {
        return KODE_DENIED;
    }
    if ( !wifi_frames_request(me, on, channel) )
    {
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

#else  /* !CONFIG_KODEOS_WIFI - the surface still links, and refuses */

kode_status_t kode_sys_wifi_frames(QActive *me, bool on, uint8_t channel)
{
    (void)me; (void)on; (void)channel;
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_WIFI */
