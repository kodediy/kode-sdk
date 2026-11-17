/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_heard.c
 * @brief Heard BLE advertisements: a record built from a report's values and a hand-over ring.
 *
 * kdrv_ble_heard_feed() runs on the host's task and kdrv_ble_heard_next() on
 * the owner's, so one mutex guards the ring and the counts; it is held for
 * the copy and nothing else. The ring lives in PSRAM: internal RAM has no
 * room for a table of this size. This file holds no NimBLE header: the
 * callback that feeds it lives in kdrv_ble.c.
 */
#include "kdrv_ble_heard.h"

#include <stdbool.h>
#include <string.h>

#include "kdrv_ble.h"
#include "khal_sync.h"
#include "khal_system.h"

/** @brief Alignment of the ring's block. */
#define HEARD_RING_ALIGN         4U

/** @brief HCI address type: public. */
#define HEARD_HCI_ADDR_PUBLIC    0U

/** @brief HCI address type: public identity (resolved from a bond). */
#define HEARD_HCI_ADDR_PUBLIC_ID 2U

/** @brief HCI event type: ADV_IND. */
#define HEARD_HCI_EVT_CONN       0U

/** @brief HCI event type: ADV_DIRECT_IND. */
#define HEARD_HCI_EVT_DIRECT     1U

/** @brief HCI event type: ADV_SCAN_IND. */
#define HEARD_HCI_EVT_SCAN       2U

/** @brief HCI event type: ADV_NONCONN_IND. */
#define HEARD_HCI_EVT_NONCONN    3U

/** @brief HCI event type: SCAN_RSP. */
#define HEARD_HCI_EVT_SCAN_RSP   4U

/** @brief What the driver keeps between calls. */
static struct {
    bool              ready;   /**< kdrv_ble_heard_init() succeeded.        */
    khal_mutex_t      lock;    /**< Guards everything below.                */
    kdrv_ble_heard_t *ring;    /**< KDRV_BLE_HEARD_RING_LEN records, PSRAM. */
    uint8_t           head;    /**< Where the next record goes.             */
    uint8_t           tail;    /**< The oldest waiting record.              */
    uint8_t           used;    /**< Records waiting.                        */
    uint32_t          heard;   /**< Records offered since the last forget.  */
    uint32_t          dropped; /**< Records refused since the last forget.  */
} s_heard;

_Static_assert(KDRV_BLE_HEARD_RING_LEN <= 255U, "the ring's indexes are uint8_t");
_Static_assert(KDRV_BLE_HEARD_DATA_MAX <= 255U, "a record's length is a uint8_t");

/**
 * @brief Map an HCI address type to the two the interface knows.
 * @param hci The report's address type.
 * @return KDRV_BLE_ADDR_PUBLIC or KDRV_BLE_ADDR_RANDOM.
 */
static uint8_t map_addr_type(uint8_t hci)
{
    if ( (HEARD_HCI_ADDR_PUBLIC == hci) || (HEARD_HCI_ADDR_PUBLIC_ID == hci) )
    {
        return KDRV_BLE_ADDR_PUBLIC;
    }
    return KDRV_BLE_ADDR_RANDOM;
}

/**
 * @brief Map an HCI event type to a kdrv_ble_heard_pdu_t.
 * @param hci The report's event type.
 * @return The PDU kind; KDRV_BLE_HEARD_PDU_OTHER for a value with no name.
 */
static uint8_t map_pdu(uint8_t hci)
{
    uint8_t pdu = (uint8_t)KDRV_BLE_HEARD_PDU_OTHER;

    switch ( hci )
    {
    case HEARD_HCI_EVT_CONN:     pdu = (uint8_t)KDRV_BLE_HEARD_PDU_CONN;     break;
    case HEARD_HCI_EVT_DIRECT:   pdu = (uint8_t)KDRV_BLE_HEARD_PDU_DIRECT;   break;
    case HEARD_HCI_EVT_SCAN:     pdu = (uint8_t)KDRV_BLE_HEARD_PDU_SCAN;     break;
    case HEARD_HCI_EVT_NONCONN:  pdu = (uint8_t)KDRV_BLE_HEARD_PDU_NONCONN;  break;
    case HEARD_HCI_EVT_SCAN_RSP: pdu = (uint8_t)KDRV_BLE_HEARD_PDU_SCAN_RSP; break;
    default:                                                                 break;
    }
    return pdu;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, from the host stack's scan callback */
void kdrv_ble_heard_make(const uint8_t addr_le[KDRV_BLE_HEARD_ADDR_LEN], uint8_t hci_addr_type,
                         uint8_t hci_event_type, int8_t rssi, const uint8_t *data,
                         uint8_t len, kdrv_ble_heard_t *out)
{
    uint8_t keep = len;

    if ( (NULL == out) || (NULL == addr_le) )
    {
        return;
    }
    if ( KDRV_BLE_HEARD_DATA_MAX < keep )
    {
        keep = (uint8_t)KDRV_BLE_HEARD_DATA_MAX;
    }
    if ( NULL == data )
    {
        keep = 0U;
    }
    (void)memset(out, 0, sizeof(*out));
    for ( uint8_t i = 0U; KDRV_BLE_HEARD_ADDR_LEN > i; i++ )
    {
        out->addr[i] = addr_le[(KDRV_BLE_HEARD_ADDR_LEN - 1U) - i];
    }
    out->addr_type = map_addr_type(hci_addr_type);
    out->rssi      = rssi;
    out->pdu       = map_pdu(hci_event_type);
    out->len       = keep;
    if ( 0U < keep )
    {
        (void)memcpy(out->data, data, keep);
    }
}

kode_status_t kdrv_ble_heard_init(void)
{
    if ( s_heard.ready )
    {
        return KODE_OK;
    }
    if ( NULL == s_heard.lock )
    {
        s_heard.lock = khal_mutex_create();
    }
    if ( NULL == s_heard.ring )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_heard.ring = (kdrv_ble_heard_t *)khal_alloc_psram(HEARD_RING_ALIGN,
            (size_t)KDRV_BLE_HEARD_RING_LEN * sizeof(kdrv_ble_heard_t));
    }
    if ( (NULL == s_heard.lock) || (NULL == s_heard.ring) )
    {
        return KODE_NO_RESOURCES;
    }
    s_heard.ready = true;
    return KODE_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, from the host stack's scan callback */
bool kdrv_ble_heard_feed(const kdrv_ble_heard_t *rec)
{
    bool taken = false;

    if ( (!s_heard.ready) || (NULL == rec) )
    {
        return false;
    }
    khal_mutex_lock(s_heard.lock);
    s_heard.heard++;
    if ( KDRV_BLE_HEARD_RING_LEN <= s_heard.used )
    {
        s_heard.dropped++;
    }
    else
    {
        s_heard.ring[s_heard.head] = *rec;
        s_heard.head = (uint8_t)((s_heard.head + 1U) % KDRV_BLE_HEARD_RING_LEN);
        s_heard.used++;
        taken = true;
    }
    khal_mutex_unlock(s_heard.lock);
    return taken;
}

bool kdrv_ble_heard_next(kdrv_ble_heard_t *out)
{
    bool taken = false;

    if ( (!s_heard.ready) || (NULL == out) )
    {
        return false;
    }
    khal_mutex_lock(s_heard.lock);
    if ( 0U < s_heard.used )
    {
        *out = s_heard.ring[s_heard.tail];
        s_heard.tail = (uint8_t)((s_heard.tail + 1U) % KDRV_BLE_HEARD_RING_LEN);
        s_heard.used--;
        taken = true;
    }
    khal_mutex_unlock(s_heard.lock);
    return taken;
}

uint32_t kdrv_ble_heard_total(void)
{
    uint32_t n = 0U;

    if ( s_heard.ready )
    {
        khal_mutex_lock(s_heard.lock);
        n = s_heard.heard;
        khal_mutex_unlock(s_heard.lock);
    }
    return n;
}

uint32_t kdrv_ble_heard_dropped(void)
{
    uint32_t n = 0U;

    if ( s_heard.ready )
    {
        khal_mutex_lock(s_heard.lock);
        n = s_heard.dropped;
        khal_mutex_unlock(s_heard.lock);
    }
    return n;
}

void kdrv_ble_heard_forget(void)
{
    if ( !s_heard.ready )
    {
        return;
    }
    khal_mutex_lock(s_heard.lock);
    s_heard.head    = 0U;
    s_heard.tail    = 0U;
    s_heard.used    = 0U;
    s_heard.heard   = 0U;
    s_heard.dropped = 0U;
    khal_mutex_unlock(s_heard.lock);
}
