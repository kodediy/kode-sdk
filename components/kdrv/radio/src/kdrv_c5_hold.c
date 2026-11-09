/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_c5_hold.c
 * @brief kdrv_radio_c5_hold() for a firmware built without the link (KODEOS_RADIO=n): C5_EN alone.
 */
#include "kdrv_radio.h"
#include "kdrv_radio_note_reg.h"

#include "kbsp.h"
#include "khal_gpio.h"

static bool s_held;  /**< C5_EN is driven low by this file. */

kode_status_t kdrv_radio_c5_hold(bool hold)
{
    kode_status_t const rc = khal_gpio_output((uint8_t)KBSP_PIN_C5_EN, !hold);

    if ( KODE_OK == rc )
    {
        s_held = hold;
        if ( hold )
        {
            /* A held C5 is off, not parked: whatever note was left is no longer true. */
            kdrv_radio_note_reg_write(0U);
        }
    }
    return rc;
}

bool kdrv_radio_c5_held(void)
{
    return s_held;
}
