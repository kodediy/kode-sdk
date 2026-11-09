/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file time_stub.c
 * @brief khal_time_ms() for a binary that tests budgets, not time.
 */
#include "khal_time.h"
#include "time_stub.h"

/** @brief What khal_time_ms() reports. */
static uint32_t s_now_ms;

void time_stub_set_ms(uint32_t ms)
{
    s_now_ms = ms;
}

void time_stub_advance_ms(uint32_t ms)
{
    s_now_ms += ms;
}

uint32_t khal_time_ms(void)
{
    return s_now_ms;
}
