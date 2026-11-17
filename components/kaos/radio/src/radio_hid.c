/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_hid.c
 * @brief AO_Radio's HID door: the reports it queues for the host, and kodeOS's stored choice of kinds.
 */
#include "radio_hid.h"

#include <string.h>

#include "kaos.h"
#include "kdrv_ble.h"
#include "kdrv_ble_hid.h"
#include "khal_nvs.h"
#include "kode_config.h"
#include "kode_log.h"
#include "kqp_config.h"
#include "kset.h"
#include "radio_knocks.h"
#include "sdkconfig.h"

#define TAG "kaos.radio"

/** @brief Small blocks a report leaves free in its tier, for every other AO's events. */
#define RADIO_HID_POOL_MARGIN  16U

_Static_assert(KQP_BLE_HID_REPORT_MAX == KDRV_BLE_HID_REPORT_MAX,
               "a report's event carries the driver's longest report");
_Static_assert(RADIO_HID_POOL_MARGIN < KQP_SMALL_POOL_COUNT,
               "a report leaves part of its tier, not all of it");
_Static_assert(RADIO_HID_WORDS_N == (KDRV_BLE_HID_KINDS + 1U),
               "the setting has one word for each set of kinds");

const char *const RADIO_HID_WORDS[RADIO_HID_WORDS_N] = {
    [0U]                     = "off",
    [KDRV_BLE_HID_MOUSE]     = "mouse",
    [KDRV_BLE_HID_KEYBOARD]  = "keyboard",
    [KDRV_BLE_HID_KINDS]     = "both",
};

/** @brief Slots of the ring a tap takes: the report and the zeroes after it. */
#define RADIO_HID_TAP_SLOTS  2U

/**
 * @brief Whether a report lets everything go: every one of its bytes is 0.
 * @param data The report.
 * @param len  Its bytes.
 * @return true for a report of zeroes.
 */
static bool hid_all_up(const uint8_t *data, size_t len)
{
    bool up = true;

    for ( size_t i = 0U; (i < len) && up; ++i )
    {
        up = (0U == data[i]);
    }
    return up;
}

/**
 * @brief Claim the ring for one event and post it to AO_Radio.
 * @param kind  The kind.
 * @param data  The report.
 * @param len   Its bytes.
 * @param tap   Whether a report of zeroes follows it in the same event.
 * @return KODE_OK when posted; the claim's refusal; KODE_NO_RESOURCES when
 *         the pool or AO_Radio's queue had no room, with the slots given back.
 */
static kode_status_t hid_post(uint8_t kind, const uint8_t *data, size_t len, bool tap)
{
    uint8_t slots = 1U;

    if ( tap )
    {
        slots = (uint8_t)RADIO_HID_TAP_SLOTS;
    }
    kode_status_t rc = kdrv_ble_hid_claim(kind, slots);

    if ( KODE_OK == rc )
    {
        BleHidReportEvt *const e = Q_NEW_X(BleHidReportEvt, RADIO_HID_POOL_MARGIN,
                                           BLE_HID_REPORT_REQ_SIG);
        bool posted = false;

        if ( NULL != e )
        {
            e->kind = kind;
            e->tap  = tap;
            e->len  = (uint8_t)len;
            (void)memset(e->data, 0, sizeof(e->data));
            (void)memcpy(e->data, data, len);
            /* QP takes the event back when the queue is full. */
            posted = QACTIVE_POST_X(AO_Radio, &e->super, KQP_RADIO_CMD_MARGIN, NULL);
        }
        if ( !posted )
        {
            /* The report never left: its slots are free again. */
            kdrv_ble_hid_unclaim(kind, slots);
            rc = KODE_NO_RESOURCES;
        }
    }
    return rc;
}

kode_status_t ble_hid_report_post(uint8_t kind, const uint8_t *data, size_t len, bool tap)
{
    kode_status_t rc = KODE_DENIED;

    if ( (NULL != data) && (0U < len) && (KQP_BLE_HID_REPORT_MAX >= len) )
    {
        rc = hid_post(kind, data, len, tap);
        if ( (KODE_NO_RESOURCES == rc) && !tap && hid_all_up(data, len) )
        {
            /* Letting go is never refused for want of room: the driver owes
             * the host the report and sends it behind what is queued. The
             * wake-up is a knock, which a full queue owes and never loses. */
            rc = kdrv_ble_hid_owe(kind);
            if ( KODE_OK == rc )
            {
                radio_knocks_post(RADIO_KNOCK_HID, 0U);
            }
        }
    }
    return rc;
}

bool radio_hid_event(QEvt const * const e, bool moving)
{
    if ( (QSignal)BLE_HID_REPORT_REQ_SIG != e->sig )
    {
        return false;
    }
    BleHidReportEvt const * const r = Q_EVT_CAST(BleHidReportEvt);

    if ( 0U == r->len )
    {
        /* The wake-up's knock, taken before the drain is woken: a report of
         * zeroes owed from here on makes a knock of its own. */
        (void)radio_knocks_take(RADIO_KNOCK_HID, NULL);
    }
    else
    {
        kdrv_ble_hid_push(r->kind, r->data, r->len);
        if ( r->tap )
        {
            static const uint8_t released[KQP_BLE_HID_REPORT_MAX];   /* every key and button up */

            kdrv_ble_hid_push(r->kind, released, r->len);
        }
    }
    if ( !moving )
    {
        kdrv_ble_hid_kick();
    }
    return true;
}

void radio_hid_job_end(QActive const * const me)
{
    if ( kdrv_ble_hid_served() )
    {
        power_reading_t now;

        /* Asking twice is harmless; a level that did not move is not notified. */
        QActive_subscribe(me, POWER_EVT_SIG);
        if ( power_reading(&now) && now.gauge_ok )
        {
            kdrv_ble_battery_set(now.soc_pct);
        }
    }
    kdrv_ble_hid_kick();
}

#if !defined(CONFIG_KODEOS_BOOT_ROLE_APP)
/**
 * @brief The kinds the setting stores.
 * @param kinds Receives them; 0 when nothing valid is stored.
 * @return true when the store holds the setting.
 */
static bool hid_stored(uint8_t *kinds)
{
    uint8_t    v      = 0U;
    bool const stored = (khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_BLE_HID, &v) == KODE_OK);

    *kinds = 0U;
    if ( stored && ((uint8_t)KDRV_BLE_HID_KINDS >= v) )
    {
        *kinds = v;
    }
    return stored;
}

void radio_hid_boot(void)
{
    uint8_t kinds = 0U;

    if ( hid_stored(&kinds) && (0U != kinds) )
    {
        if ( kdrv_ble_hid_select(kinds) == KODE_OK )
        {
            KLOG_N(TAG, "ble_hid: %s (stored)", RADIO_HID_WORDS[kinds]);
        }
        else
        {
            KLOG_W(TAG, "ble_hid: %s is stored and the driver refused it", RADIO_HID_WORDS[kinds]);
        }
    }
}

bool radio_hid_config(QActive * const me, KodeConfigReqEvt const * const r)
{
    kode_status_t rc    = KODE_OK;
    uint8_t       kinds = 0U;

    if ( strcmp(r->key, KSET_KEY_BLE_HID) != 0 )
    {
        return false;
    }
    if ( r->set )
    {
        if ( (uint8_t)RADIO_HID_WORDS_N <= r->e )
        {
            rc = KODE_DENIED;
        }
        else if ( khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_BLE_HID, r->e) != KODE_OK )
        {
            rc = KODE_IO_ERROR;
        }
        else if ( kdrv_ble_hid_select(r->e) == KODE_OK )
        {
            KLOG_N(TAG, "ble_hid %s (stored): served when Bluetooth starts", RADIO_HID_WORDS[r->e]);
        }
        else
        {
            KLOG_N(TAG, "ble_hid %s (stored): it holds from the next boot, Bluetooth has "
                        "registered its table", RADIO_HID_WORDS[r->e]);
        }
    }
    bool const stored = hid_stored(&kinds);

    (void)kode_config_answer(me, r, rc, stored, RADIO_HID_WORDS[kinds]);
    return true;
}
#else  /* an app asks by code and reads no setting */
void radio_hid_boot(void)
{
}

bool radio_hid_config(QActive * const me, KodeConfigReqEvt const * const r)
{
    (void)me;
    (void)r;
    return false;
}
#endif /* !CONFIG_KODEOS_BOOT_ROLE_APP */
