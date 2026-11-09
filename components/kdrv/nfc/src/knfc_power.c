/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_power.c
 * @brief The reader's rest/wake decision table. See knfc_power.h.
 */
#include "knfc_power.h"

knfc_power_action_t knfc_power_action(knfc_power_state_t from,
                                      knfc_power_state_t want)
{
    if ( from == want )
    {
        return KNFC_POWER_HOLD;
    }
    if ( KNFC_POWER_AWAKE == want )
    {
        return KNFC_POWER_WAKE;
    }
    return KNFC_POWER_REST;
}
