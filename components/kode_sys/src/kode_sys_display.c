/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_display.c
 * @brief The screen's command: posted to AO_Display, whose name is what links
 *        the owner into an app that asks.
 */
#include "kode_sys.h"

#include "kode_sys_priv.h"

#include "kaos.h"

kode_status_t kode_sys_screen_brightness(uint8_t pct)
{
    kode_status_t rc = KODE_DENIED;

    /* The range is the facade's promise: 0 is dark, and between dark and the
     * lowest lit level the glass would only look broken. */
    if ( (0U == pct) || ((KODE_SYS_SCREEN_BRIGHT_MIN <= pct) && (KODE_SYS_SCREEN_BRIGHT_MAX >= pct)) )
    {
        BrightnessEvt *e   = Q_NEW_X(BrightnessEvt, 0U, DISPLAY_BRIGHTNESS_SIG);
        QEvt const    *evt = NULL;

        if ( NULL != e )
        {
            e->percent = pct;
            e->persist = false;
            evt        = &e->super;
        }
        rc = kode_sys_cmd_post(AO_Display, evt, KQP_DISPLAY_CMD_MARGIN);
    }
    return rc;
}
