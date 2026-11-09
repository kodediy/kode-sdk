/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_exp_power.c
 * @brief J3's supply: the one expansion door that asks an Active Object.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "sdkconfig.h"

/* No claim: the rail's enable is a pin of the C5, so this asks its owner, AO_Radio. */
kode_status_t kode_sys_exp_power(bool on)
{
#if CONFIG_KODEOS_RADIO
    return kode_sys_cmd_post(AO_Radio, exp_power_evt(on), KQP_RADIO_CMD_MARGIN);
#else
    (void)on;
    return KODE_DENIED;
#endif
}
