/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_haptic.c
 * @brief The haptic doors: posted to AO_Haptic, the range judged here.
 */
#include "kode_sys.h"

#include "kaos.h"

kode_status_t kode_sys_haptic_play_buzz(uint16_t ms, uint8_t gain_pct)
{
    /* The range is the facade's promise; whether the driver is free is AO_Haptic's answer. */
    if ( (0U == ms) || (KODE_SYS_HAPTIC_MAX_MS < ms) )
    {
        return KODE_DENIED;
    }
    if ( KODE_SYS_HAPTIC_GAIN_MAX < gain_pct )
    {
        return KODE_DENIED;
    }
    return haptic_play_request(ms, gain_pct);
}

_Static_assert(KODE_SYS_HAPTIC_SEGMENTS_MAX == KODE_HAPTIC_SEGMENTS_MAX,
               "the facade's segment ceiling is AO_Haptic's");

kode_status_t kode_sys_haptic_play_pattern(const uint16_t *seg_ms, uint8_t n,
                                           uint8_t gain_pct)
{
    if ( KODE_SYS_HAPTIC_GAIN_MAX < gain_pct )
    {
        return KODE_DENIED;
    }
    return haptic_pattern_request(seg_ms, n, gain_pct);
}

kode_status_t kode_sys_haptic_hold(uint8_t gain_pct)
{
    if ( KODE_SYS_HAPTIC_GAIN_MAX < gain_pct )
    {
        return KODE_DENIED;
    }
    return haptic_hold_request(gain_pct);
}

bool kode_sys_haptic_is_holding(void)
{
    return haptic_state() == HAPTIC_STATE_HOLDING;
}

void kode_sys_haptic_stop(void)
{
    haptic_stop_request();
}
