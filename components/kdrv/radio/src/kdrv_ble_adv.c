/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_adv.c
 * @brief What a scan response carries, packed as BLE lays it out, and the board's default name.
 */
#include "kdrv_ble.h"

#include <string.h>

#include "khal_identity.h"

/** @brief What one AD structure spends before its payload: a length, a type. */
#define BLE_AD_HEAD   2U

/** @brief AD type: the complete local name. */
#define BLE_AD_NAME   0x09U

/** @brief AD type: manufacturer-specific data, its company id first. */
#define BLE_AD_MFG    0xFFU

/* ---- Connection parameters ----
 * Intervals in 1.25 ms units, timeouts in 10 ms units, as BLE gives them. */
#define CONN_FAST_ITVL        12U    /**< 15 ms, the floor macOS and iOS accept.       */
#define CONN_FAST_TIMEOUT     400U   /**< 4 s.                                          */
#define CONN_REST_ITVL_MIN    240U   /**< 300 ms.                                       */
#define CONN_REST_ITVL_MAX    264U   /**< 330 ms: min + 30 ms, on Apple's rule of + 15. */
#define CONN_REST_LATENCY     4U     /**< 330 ms x 5 = 1.65 s between answers at most.  */
#define CONN_REST_TIMEOUT     600U   /**< 6 s, Apple's longest.                         */

/* Apple's rules for an accessory, in the same units, scaled by 4 so that
 * 1.25 ms (5/4) and 10 ms (40/4) are whole numbers. */
#define CONN_Q_ITVL           5U     /**< One interval unit, in quarter ms.  */
#define CONN_Q_TIMEOUT        40U    /**< One timeout unit, in quarter ms.   */
#define CONN_Q_15MS           60U    /**< 15 ms.                             */
#define CONN_Q_2S             8000U  /**< 2 s.                               */
#define CONN_Q_6S             24000U /**< 6 s.                               */
#define CONN_LATENCY_MAX      30U    /**< The most events a board may skip.  */
#define CONN_TIMEOUT_FACTOR   3U     /**< The timeout covers three misses.   */

/** @brief The name of a board that does not know its serial. */
#define BLE_NAME_UNKNOWN  "KODE-DOT"

/**
 * @brief The length of @p s, counted no further than past @p max.
 * @param s   A string; NULL counts 0.
 * @param max The longest length worth counting.
 * @return The length, or @p max + 1 when it is longer.
 */
static size_t adv_strlen(const char *s, size_t max)
{
    size_t n = 0U;

    if ( NULL != s )
    {
        while ( (n <= max) && ('\0' != s[n]) )
        {
            ++n;
        }
    }
    return n;
}

/**
 * @brief Write one AD structure at @p at.
 * @param out  The packet.
 * @param at   Where the structure starts.
 * @param type Its AD type.
 * @param src  Its payload.
 * @param n    The payload's bytes.
 * @return Where the next structure starts.
 */
static size_t adv_put(uint8_t *out, size_t at, uint8_t type, const void *src, size_t n)
{
    out[at]      = (uint8_t)(n + 1U);   /* the type and the payload */
    out[at + 1U] = type;
    (void)memcpy(&out[at + BLE_AD_HEAD], src, n);
    return at + BLE_AD_HEAD + n;
}

kode_status_t kdrv_ble_adv_pack(const char *name, const uint8_t *data, size_t len,
                                uint8_t *out, size_t cap, size_t *out_len)
{
    size_t const name_len = adv_strlen(name, KDRV_BLE_ADV_MAX);
    size_t       need     = 0U;
    size_t       at       = 0U;

    if ( (NULL == out) || (NULL == out_len) )
    {
        return KODE_DENIED;
    }
    if ( (0U < len) && ((NULL == data) || (KDRV_BLE_MFG_ID_LEN > len)) )
    {
        return KODE_DENIED;
    }
    if ( KDRV_BLE_ADV_MAX < len )
    {
        return KODE_NO_RESOURCES;   /* checked first: the sum below cannot wrap */
    }
    if ( 0U < name_len )
    {
        need += BLE_AD_HEAD + name_len;
    }
    if ( 0U < len )
    {
        need += BLE_AD_HEAD + len;
    }
    if ( (need > cap) || (KDRV_BLE_ADV_MAX < need) )
    {
        return KODE_NO_RESOURCES;
    }
    if ( 0U < name_len )
    {
        at = adv_put(out, at, (uint8_t)BLE_AD_NAME, name, name_len);
    }
    if ( 0U < len )
    {
        at = adv_put(out, at, (uint8_t)BLE_AD_MFG, data, len);
    }
    *out_len = at;
    return KODE_OK;
}

/*
 * The canonical serial and not the printed one: the name is read by
 * `packages/device-link` and by the mobile app at least as often as by eyes,
 * and grouping the digits is a decision for whoever paints the screen, which
 * parses the name anyway. Putting the separators on the air would spend three
 * bytes of a 31-byte packet on a presentation choice, and leave one identifier
 * travelling in two shapes across two machine interfaces. A board that does
 * not know its serial says so with the old constant.
 */
const char *kdrv_ble_serial_name(void)
{
    const char *name = BLE_NAME_UNKNOWN;

    if ( khal_identity_valid() )
    {
        name = khal_identity_serial();
    }
    return name;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, which is a file this check does not join to this one */
void kdrv_ble_conn_params(bool rest, kdrv_ble_conn_params_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    if ( rest )
    {
        out->itvl_min = (uint16_t)CONN_REST_ITVL_MIN;
        out->itvl_max = (uint16_t)CONN_REST_ITVL_MAX;
        out->latency  = (uint16_t)CONN_REST_LATENCY;
        out->timeout  = (uint16_t)CONN_REST_TIMEOUT;
    }
    else
    {
        out->itvl_min = (uint16_t)CONN_FAST_ITVL;
        out->itvl_max = (uint16_t)CONN_FAST_ITVL;
        out->latency  = 0U;
        out->timeout  = (uint16_t)CONN_FAST_TIMEOUT;
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; the contract the host tests hold both sets to, called from there */
bool kdrv_ble_conn_params_ok(const kdrv_ble_conn_params_t *p)
{
    bool ok = false;

    if ( NULL != p )
    {
        uint32_t const min_q   = (uint32_t)p->itvl_min * CONN_Q_ITVL;
        uint32_t const max_q   = (uint32_t)p->itvl_max * CONN_Q_ITVL;
        uint32_t const tout_q  = (uint32_t)p->timeout * CONN_Q_TIMEOUT;
        uint32_t const cycle_q = max_q * ((uint32_t)p->latency + 1U);
        bool const both_15     = (CONN_Q_15MS == min_q) && (CONN_Q_15MS == max_q);

        ok = (CONN_Q_15MS <= min_q)
             && (both_15 || ((min_q + CONN_Q_15MS) <= max_q))
             && (CONN_LATENCY_MAX >= (uint32_t)p->latency)
             && (CONN_Q_2S >= cycle_q)
             && (CONN_Q_2S <= tout_q) && (CONN_Q_6S >= tout_q)
             && ((cycle_q * CONN_TIMEOUT_FACTOR) < tout_q);
    }
    return ok;
}
