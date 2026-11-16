/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_display.c
 * @brief The screen's commands: posted to AO_Display, whose name is what links
 *        the owner into an app that asks.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"

#include <string.h>

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

kode_status_t kode_sys_screen_sleep(bool sleep)
{
    DisplaySleepReqEvt *e   = Q_NEW_X(DisplaySleepReqEvt, 0U, DISPLAY_SLEEP_REQ_SIG);
    QEvt const         *evt = NULL;

    if ( NULL != e )
    {
        e->sleep = sleep;
        evt      = &e->super;
    }
    return kode_sys_cmd_post(AO_Display, evt, KQP_DISPLAY_CMD_MARGIN);
}

kode_status_t kode_sys_screen_parked(uint32_t seq)
{
    kode_status_t rc = KODE_DENIED;

    /* No park is numbered 0: that is a round's hand-over, and taking it from
     * an app would settle a round the app was never asked about. */
    if ( 0U != seq )
    {
        DisplayGlyphsEvt *e   = Q_NEW_X(DisplayGlyphsEvt, 0U, DISPLAY_GLYPHS_SIG);
        QEvt const       *evt = NULL;

        if ( NULL != e )
        {
            /* No glyphs: a strip of NULL says only that the renderer has stopped. */
            e->strip    = NULL;
            e->glyph_w  = 0U;
            e->glyph_h  = 0U;
            e->x        = 0U;
            e->y        = 0U;
            e->colon_w  = 0U;
            e->rest_pct = 0U;
            e->h12      = false;
            (void)memset(e->adv, 0, sizeof(e->adv));
            e->park     = seq;
            evt         = &e->super;
        }
        rc = kode_sys_cmd_post(AO_Display, evt, KQP_DISPLAY_CMD_MARGIN);
    }
    return rc;
}

bool kode_sys_screen_is_asleep(void)
{
    return display_asleep();
}

uint16_t kode_block_screen_sleep_answers(kode_status_t *rc, const char **why)
{
    display_sleep_last_t last = { 0 };

    display_sleep_last(&last);
    if ( NULL != rc )
    {
        *rc = (kode_status_t)last.rc;
    }
    if ( NULL != why )
    {
        *why = display_sleep_why_text(last.why);
    }
    return last.answers;
}
