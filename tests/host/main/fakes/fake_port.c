/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_port.c
 * @brief The one FreeRTOS port macro the linux port does not declare.
 */
#include "fakes.h"

static bool s_can_yield = true;

void fake_port_reset(void)
{
    s_can_yield = true;
}

void fake_port_set_can_yield(bool can)
{
    s_can_yield = can;
}

int xPortCanYield(void)
{
    return s_can_yield ? 1 : 0;
}
