/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_ble_heard.c
 * @brief The BLE advertisement stream through AO_Radio: an AO's start and stop.
 */
#include "kode_sys.h"

#include "kaos.h"
#include "sdkconfig.h"

#ifdef CONFIG_KODEOS_BLE

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_heard(QActive *me, bool on)
{
    if ( NULL == me )
    {
        return KODE_DENIED;
    }
    if ( !ble_heard_request(me, on) )
    {
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

#else  /* !CONFIG_KODEOS_BLE - the surface still links, and refuses */

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_heard(QActive *me, bool on)
{
    (void)me; (void)on;
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_BLE */
