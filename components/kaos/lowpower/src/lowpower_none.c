/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_none.c
 * @brief The low-power facade of a build without CONFIG_KODE_LOWPOWER.
 */
#include "kaos.h"

#include "kode_log.h"

#define TAG "kaos.lowpower"

/** @brief What lowpower_last_refusal() answers in this build. */
#define NOT_BUILT  "not built"

void lowpower_request(kode_lp_state_t target, uint8_t src)
{
    (void)target;
    (void)src;
    KLOG_W(TAG, "low power not built: the request is refused");
}

bool lowpower_request_rest(uint8_t src)
{
    (void)src;
    KLOG_W(TAG, "low power not built: the request to rest is refused");
    return false;
}

void lowpower_input_note(uint8_t src)
{
    (void)src;
    /* Nothing counts idle time here. */
}

const char *lowpower_last_refusal(void)
{
    return NOT_BUILT;
}
