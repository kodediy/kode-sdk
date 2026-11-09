/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_home.c
 * @brief Going home: commands to AO_Boot, which defers, takes or hands back the way to kodeOS.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"

#include "kaos.h"

#define TAG "kode"

kode_status_t kode_sys_home_defer(void)
{
    if ( !boot_home_pending() )
    {
        return KODE_DENIED;
    }
    return kode_sys_cmd_post(AO_Boot, boot_home_defer_evt(), KQP_BOOT_CMD_MARGIN);
}

kode_status_t kode_sys_home_now(void)
{
    return kode_sys_cmd_post(AO_Boot, boot_home_now_evt(), KQP_BOOT_CMD_MARGIN);
}

kode_status_t kode_sys_exit_to_os(void)
{
    kode_status_t const rc = kode_sys_cmd_post(AO_Boot, boot_exit_evt(),
                                               KQP_BOOT_CMD_MARGIN);

    if ( KODE_OK == rc )
    {
        KLOG_N(TAG, "handing control back to kodeOS");
    }
    return rc;
}
