/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_wifi_scan.c
 * @brief The Wi-Fi scan through AO_Radio: an AO's request, and the blocking scan behind one claim.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kdrv_wifi.h"
#include "sdkconfig.h"

#include <stddef.h>
#include <string.h>

#if CONFIG_KODEOS_WIFI

_Static_assert(KODE_SYS_WIFI_SCAN_MAX == KDRV_WIFI_SCAN_MAX,
               "the facade's scan ceiling and the driver's table have drifted apart");
_Static_assert(KODE_SYS_WIFI_SCAN_MAX <= (uint32_t)UINT8_MAX,
               "a waiter counts its rows in a byte");
_Static_assert(offsetof(kode_sys_wifi_scan_answer_t, rc) == offsetof(StatusRspEvt, rc),
               "the scan's answer carries its rc where every answer does");
_Static_assert(sizeof(((kode_sys_wifi_scan_answer_t *)NULL)->rc) == sizeof(((StatusRspEvt *)NULL)->rc),
               "the scan's answer's rc is every answer's");

/** @brief The blocking scan's claim and storage (kode_sys_priv.h says how). */
static kode_sys_scan_t s_wifi_scan = { .row_size = sizeof(kdrv_wifi_ap_t),
                                       .cap      = (uint8_t)KODE_SYS_WIFI_SCAN_MAX };

_Static_assert(KODE_SYS_WIFI_SSID_MAX == sizeof(((kdrv_wifi_ap_t *)NULL)->ssid),
               "the facade's name and the driver's have drifted apart");

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_wifi_scan(QActive *me)
{
    return wifi_scan_ask(me);
}

/** @brief Make the semaphore and the lock. @return true when both exist. */
static bool wifi_scan_make(void)
{
    return kode_sys_scan_make(&s_wifi_scan);
}

/**
 * @brief Offer the rows AO_Radio wrote, converted, until @p cb says stop.
 * @param cb  The caller's callback.
 * @param ctx Its context.
 */
static void wifi_scan_offer(kode_sys_wifi_ap_cb cb, void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the rows are the kdrv_wifi_ap_t[] kode_sys_scan_enter() took for this scan */
    const kdrv_wifi_ap_t *const rows = (const kdrv_wifi_ap_t *)s_wifi_scan.wait.rows;
    uint8_t                     n    = s_wifi_scan.wait.got;

    if ( (uint8_t)KODE_SYS_WIFI_SCAN_MAX < n )
    {
        n = (uint8_t)KODE_SYS_WIFI_SCAN_MAX;
    }
    for ( uint8_t i = 0U; i < n; ++i )
    {
        kode_sys_wifi_ap_t ap;

        (void)memcpy(ap.ssid, rows[i].ssid, sizeof(ap.ssid));
        ap.ssid[sizeof(ap.ssid) - 1U] = '\0';
        ap.rssi    = rows[i].rssi;
        ap.channel = rows[i].channel;
        ap.open    = KDRV_WIFI_AUTH_OPEN == rows[i].authmode;
        if ( !cb(&ap, ctx) )
        {
            break;
        }
    }
}

kode_status_t kode_block_wifi_scan(kode_sys_wifi_ap_cb cb, void *ctx, uint32_t ms)
{
    if ( NULL == cb )
    {
        return KODE_ERROR;
    }
    kode_status_t rc = kode_sys_scan_enter(&s_wifi_scan, &wifi_scan_make, ms);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    uint32_t const gen = kode_sys_scan_gen(&s_wifi_scan);

    wifi_scan_request_wait(&s_wifi_scan.wait, gen);
    rc = kode_sys_scan_await(&s_wifi_scan, gen, ms);
    if ( KODE_OK == rc )
    {
        wifi_scan_offer(cb, ctx);
    }
    kode_sys_scan_release(&s_wifi_scan);
    return rc;
}

#else  /* !CONFIG_KODEOS_WIFI - the surface still links, and refuses */

kode_status_t kode_sys_wifi_scan(QActive *me)
{
    (void)me;
    return KODE_DENIED;
}

kode_status_t kode_block_wifi_scan(kode_sys_wifi_ap_cb cb, void *ctx, uint32_t ms)
{
    (void)cb; (void)ctx; (void)ms;
    return KODE_DENIED;
}

#endif /* CONFIG_KODEOS_WIFI */
