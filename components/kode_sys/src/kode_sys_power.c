/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_power.c
 * @brief The power commands, posted to AO_Power, which owns the PMIC (the battery is its copy),
 *        and the board's rest, posted to AO_LowPower.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kbsp_cell.h"
#include "kdrv_bq25896.h"
#include "kode_log.h"
#include "sdkconfig.h"

/* cppcheck-suppress misra-c2012-2.5 ; used where CONFIG_KODE_LOWPOWER is off, a region cppcheck does not read here */
#define TAG "kode.sys"

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

_Static_assert(KODE_SYS_REST_SCREEN == KODE_LP_PART_SCREEN, "the parts of a rest are AO_LowPower's");
_Static_assert(KODE_SYS_REST_PANEL == KODE_LP_PART_PANEL, "the parts of a rest are AO_LowPower's");
_Static_assert(KODE_SYS_REST_TOUCH == KODE_LP_PART_TOUCH, "the parts of a rest are AO_LowPower's");
_Static_assert(KODE_SYS_REST_SOUND == KODE_LP_PART_SOUND, "the parts of a rest are AO_LowPower's");
_Static_assert(KODE_SYS_REST_CARD == KODE_LP_PART_CARD, "the parts of a rest are AO_LowPower's");
_Static_assert(KODE_SYS_REST_RADIO == KODE_LP_PART_RADIO, "the parts of a rest are AO_LowPower's");
_Static_assert(KODE_SYS_REST_CPU == KODE_LP_PART_CPU, "the parts of a rest are AO_LowPower's");

/**
 * @brief Ask AO_LowPower for parts, as a command.
 * @param req What is asked.
 * @return KODE_OK when AO_LowPower's queue took it; KODE_NO_RESOURCES as the
 *         commands' rule; KODE_DENIED without CONFIG_KODE_LOWPOWER.
 */
static kode_status_t power_part_post(const lowpower_part_req_t *req)
{
#ifdef CONFIG_KODE_LOWPOWER
    return kode_sys_cmd_post(AO_LowPower, lowpower_part_req_evt(req, KODE_LOWPOWER_SRC_APP),
                             KQP_LOWPOWER_CMD_MARGIN);
#else
    /* The build has no AO_LowPower to ask. */
    (void)req;
    KLOG_W(TAG, "low power not built: the rest is refused");
    return KODE_DENIED;
#endif
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_rest(uint32_t parts, const kode_sys_rest_wake_t *wake)
{
    lowpower_part_req_t req = { 0 };

    req.parts = (uint8_t)(parts & (uint32_t)UINT8_MAX);
    if ( (uint32_t)UINT8_MAX < parts )
    {
        /* A bit the event has no room for is asked as a part there is not, so
         * AO_LowPower refuses the request by name and nothing is dropped. */
        req.parts |= (uint8_t)~KODE_LP_PART_ALL;
    }
    if ( NULL != wake )
    {
        req.wake_touch    = wake->touch;
        req.wake_level    = wake->pin_level;
        req.wake_pin      = wake->pin;
        req.wake_after_ms = wake->after_ms;
    }
    return power_part_post(&req);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_rest_end(void)
{
    lowpower_part_req_t req = { 0 };

    req.end = true;
    return power_part_post(&req);
}

uint16_t kode_block_rest_answers(kode_status_t *rc, uint8_t *parts, bool *open, const char **why)
{
    lowpower_part_last_t last = { 0 };

    lowpower_part_last(&last);
    if ( NULL != open )
    {
        *open = last.open;
    }
    if ( NULL != rc )
    {
        *rc = (kode_status_t)last.rc;
    }
    if ( NULL != parts )
    {
        *parts = last.parts;
    }
    if ( NULL != why )
    {
        *why = lowpower_part_why();
    }
    return last.answers;
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
