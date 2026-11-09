/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_power.c
 * @brief The power commands, posted to AO_Power, which owns the PMIC (the battery is its copy),
 *        and the board's rest, posted to AO_LowPower.
 */
#include "kode_sys.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kbsp_cell.h"
#include "kdrv_bq25896.h"
#include "sdkconfig.h"

_Static_assert((uint32_t)KDRV_BQ_CHRG_NONE == KODE_SYS_CHARGE_NONE, "charge states follow the PMIC's");
_Static_assert((uint32_t)KDRV_BQ_CHRG_PRE == KODE_SYS_CHARGE_PRE, "charge states follow the PMIC's");
_Static_assert((uint32_t)KDRV_BQ_CHRG_FAST == KODE_SYS_CHARGE_FAST, "charge states follow the PMIC's");
_Static_assert((uint32_t)KDRV_BQ_CHRG_DONE == KODE_SYS_CHARGE_DONE, "charge states follow the PMIC's");

_Static_assert(KODE_SYS_CHARGE_LIMIT_MAX_MA == KBSP_CELL_ICHG_MAX_MA, "the charge ceiling is the pack's");
_Static_assert(KODE_SYS_BOOST_MIN_MV == KBSP_CELL_BOOST_MIN_MV, "the boost floor is the board's");

/**
 * @brief Ask AO_LowPower for a state, as a command.
 * @param target The state wanted.
 * @return KODE_OK when AO_LowPower's queue took it; KODE_NO_RESOURCES as the
 *         commands' rule; KODE_DENIED without CONFIG_KODE_LOWPOWER.
 */
static kode_status_t power_rest_post(kode_lp_state_t target)
{
#if CONFIG_KODE_LOWPOWER
    return kode_sys_cmd_post(AO_LowPower, lowpower_req_evt(target, KODE_LOWPOWER_SRC_APP),
                             KQP_LOWPOWER_CMD_MARGIN);
#else
    /* Logs `low power not built`: the build has no AO_LowPower to ask. */
    lowpower_request(target, KODE_LOWPOWER_SRC_APP);
    return KODE_DENIED;
#endif
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_power_sleep(void)
{
    return power_rest_post(KODE_LP_ASLEEP);
}

kode_status_t kode_sys_power_off(void)
{
    return power_rest_post(KODE_LP_OFF);
}

kode_status_t kode_sys_battery_read(kode_sys_battery_t *out)
{
    kode_status_t   rc = KODE_DENIED;
    power_reading_t r;

    if ( NULL != out )
    {
        if ( power_reading(&r) )
        {
            out->vbat_mv  = r.vbat_mv;
            out->soc_pct  = r.soc_pct;
            out->charging = r.charging;
            out->vbus     = r.vbus;
            out->gauge_ok = r.gauge_ok;
            rc = KODE_OK;
        }
    }
    return rc;
}

kode_status_t kode_sys_power_boost(bool on)
{
    int32_t v = 0;
    if ( on )
    {
        v = 1;
    }
    return kode_sys_cmd_post(AO_Power, power_ctrl_evt(POWER_CTRL_BOOST, v), KQP_POWER_CMD_MARGIN);
}

kode_status_t kode_sys_power_charge(bool on)
{
    int32_t v = 0;
    if ( on )
    {
        v = 1;
    }
    return kode_sys_cmd_post(AO_Power, power_ctrl_evt(POWER_CTRL_CHARGE, v), KQP_POWER_CMD_MARGIN);
}

kode_status_t kode_sys_power_charge_limit(uint16_t ma)
{
    return kode_sys_cmd_post(AO_Power, power_ctrl_evt(POWER_CTRL_ICHG, (int32_t)ma),
                             KQP_POWER_CMD_MARGIN);
}
