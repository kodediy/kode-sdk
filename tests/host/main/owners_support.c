/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file owners_support.c
 * @brief See owners_support.h.
 */
#include "owners_support.h"

#include "kaos.h"
#include "kaos_claim.h"
#include "sdkconfig.h"

void owners_up(void)
{
    kaos_storage_register();
    kaos_appmgr_register();
    kaos_radio_register();
    kaos_net_register();
    kaos_ledmgr_register();
    kaos_ir_register();
    kaos_nfc_register();
    kaos_rfid_register();
    kaos_audio_register();
    kaos_imu_register();
    kaos_haptic_register();
    kaos_touch_register();
    kaos_display_register();
    kaos_autotest_register();
#if CONFIG_KODE_LOWPOWER
    kaos_lowpower_register();
#endif
#if CONFIG_KODE_SCHEDULER
    kaos_scheduler_register();
#endif
}

bool owners_lend(kbsp_res_t res, const void *holder)
{
    bool const held = (kbsp_owner_name(res) != NULL);

    kbsp_release(res, holder);
    return held;
}

void owners_return(kbsp_res_t res, const void *holder, const char *name, bool held)
{
    kbsp_release(res, &kaos_parked);
    if ( held )
    {
        (void)kbsp_claim(res, holder, name);
    }
}
