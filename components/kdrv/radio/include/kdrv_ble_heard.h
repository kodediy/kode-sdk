/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_heard.h
 * @brief One heard BLE advertisement, and the ring that hands them from the host's callback to the radio's owner.
 */
#ifndef KDRV_BLE_HEARD_H_
#define KDRV_BLE_HEARD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Records waiting for the owner. */
#define KDRV_BLE_HEARD_RING_LEN  64U

/** @brief A legacy advertisement's payload, bytes. */
#define KDRV_BLE_HEARD_DATA_MAX  31U

/** @brief Bytes of an address. */
#define KDRV_BLE_HEARD_ADDR_LEN  6U

/** @brief What kind of packet it was (the HCI report's event type, renamed). */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps the record its size */
typedef enum {
    KDRV_BLE_HEARD_PDU_CONN = 0,   /**< ADV_IND: connectable, scannable.   */
    KDRV_BLE_HEARD_PDU_DIRECT,     /**< ADV_DIRECT_IND.                    */
    KDRV_BLE_HEARD_PDU_SCAN,       /**< ADV_SCAN_IND: scannable only.      */
    KDRV_BLE_HEARD_PDU_NONCONN,    /**< ADV_NONCONN_IND.                   */
    KDRV_BLE_HEARD_PDU_SCAN_RSP,   /**< SCAN_RSP: never in a passive scan. */
    KDRV_BLE_HEARD_PDU_OTHER       /**< Anything else the report says.     */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps the record its size */
} kdrv_ble_heard_pdu_t;

/** @brief One advertisement, as the controller reported it. */
typedef struct {
    uint8_t addr[KDRV_BLE_HEARD_ADDR_LEN]; /**< Big-endian, as printed: NimBLE's order reversed. */
    uint8_t addr_type;                     /**< KDRV_BLE_ADDR_PUBLIC or KDRV_BLE_ADDR_RANDOM.    */
    int8_t  rssi;                          /**< dBm.                                             */
    uint8_t pdu;                           /**< kdrv_ble_heard_pdu_t.                            */
    uint8_t len;                           /**< Bytes in data, 0..KDRV_BLE_HEARD_DATA_MAX.       */
    uint8_t data[KDRV_BLE_HEARD_DATA_MAX]; /**< The payload as on the air; zeros past len.       */
} kdrv_ble_heard_t;

/**
 * @brief Build a record from a report's plain values. Pure: it keeps nothing.
 *
 * The address is reversed into print order. HCI address types 0 and 2 (public,
 * public identity) become KDRV_BLE_ADDR_PUBLIC and the rest KDRV_BLE_ADDR_RANDOM.
 * HCI event types 0 to 4 become the five named PDUs in order; any other value
 * becomes KDRV_BLE_HEARD_PDU_OTHER. A payload longer than
 * KDRV_BLE_HEARD_DATA_MAX keeps its first KDRV_BLE_HEARD_DATA_MAX bytes; the
 * bytes past len are zeros.
 *
 * @param addr_le         The address in NimBLE's order (least significant byte first).
 * @param hci_addr_type   The report's address type.
 * @param hci_event_type  The report's event type.
 * @param rssi            dBm.
 * @param data            The payload; may be NULL when len is 0.
 * @param len             Payload bytes.
 * @param out             Receives the record; left alone when NULL.
 */
void kdrv_ble_heard_make(const uint8_t addr_le[KDRV_BLE_HEARD_ADDR_LEN], uint8_t hci_addr_type,
                         uint8_t hci_event_type, int8_t rssi, const uint8_t *data,
                         uint8_t len, kdrv_ble_heard_t *out);

/**
 * @brief Take the ring from PSRAM and its lock. Idempotent.
 * @return KODE_OK, or KODE_NO_RESOURCES when the ring or its lock cannot be had.
 */
kode_status_t kdrv_ble_heard_init(void);

/**
 * @brief Put a record in the ring. Runs on the host's task: one lock, held for the copy.
 *
 * A full ring refuses the new record and keeps the oldest ones; the refusal is
 * counted in kdrv_ble_heard_dropped(). Every offered record counts in
 * kdrv_ble_heard_total().
 *
 * @param rec The record; copied.
 * @return true when it was taken; false when the ring was full (counted), or
 *         before kdrv_ble_heard_init() (not counted).
 */
bool kdrv_ble_heard_feed(const kdrv_ble_heard_t *rec);

/**
 * @brief Take the oldest waiting record.
 * @param out Receives it.
 * @return false when none waits.
 */
bool kdrv_ble_heard_next(kdrv_ble_heard_t *out);

/**
 * @brief Records offered since the last kdrv_ble_heard_forget().
 * @return The count; 0 before the ring exists.
 */
uint32_t kdrv_ble_heard_total(void);

/**
 * @brief Records the ring refused since the last kdrv_ble_heard_forget().
 * @return The count; 0 before the ring exists.
 */
uint32_t kdrv_ble_heard_dropped(void);

/** @brief Empty the ring and both counts. */
void kdrv_ble_heard_forget(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_HEARD_H_ */
