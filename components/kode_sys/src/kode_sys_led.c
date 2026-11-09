/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_led.c
 * @brief The status LED's commands: posted to AO_LedMgr, whose name is what
 *        links the owner into an app that asks.
 */
#include "kode_sys.h"

#include "kode_sys_priv.h"

#include "kaos.h"

/**
 * @brief Post a LedEvt-carrying signal to AO_LedMgr.
 * @param sig The signal.
 * @param on  The event's flag.
 * @return As kode_sys_cmd_post().
 */
static kode_status_t led_post(enum KodeSignals sig, bool on)
{
    LedEvt     *e   = Q_NEW_X(LedEvt, 0U, (QSignal)sig);
    QEvt const *evt = NULL;

    if ( NULL != e )
    {
        e->on = on;
        evt   = &e->super;
    }
    return kode_sys_cmd_post(AO_LedMgr, evt, KQP_LEDMGR_CMD_MARGIN);
}

kode_status_t kode_sys_led_set(bool on)
{
    return led_post(LED_SET_SIG, on);
}

kode_status_t kode_sys_led_toggle(void)
{
    return led_post(LED_TOGGLE_SIG, false);
}

kode_status_t kode_sys_led_set_rgb(uint8_t r, uint8_t g, uint8_t b)
{
    LedRgbEvt  *e   = Q_NEW_X(LedRgbEvt, 0U, LED_RGB_SIG);
    QEvt const *evt = NULL;

    if ( NULL != e )
    {
        e->r = r;
        e->g = g;
        e->b = b;
        evt  = &e->super;
    }
    return kode_sys_cmd_post(AO_LedMgr, evt, KQP_LEDMGR_CMD_MARGIN);
}
