/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_app.h
 * @brief The app's GATT service without NimBLE: the table, values, subscriptions and the ring.
 */
#ifndef KDRV_BLE_APP_H_
#define KDRV_BLE_APP_H_

#include <stdbool.h>
#include <stdint.h>

#include "kdrv_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Bytes of a 128-bit UUID. */
#define KDRV_BLE_UUID128_BYTES  16U

/** @brief Notifications the ring holds while the link catches up. */
#define KDRV_BLE_APP_RING_SLOTS 8U

/**
 * @brief The characteristic flags one property earns, NimBLE's ble_gatt_chr_flags.
 *
 * kdrv_ble.c fills it from the console's own permission masks, so the app's
 * characteristics are held to exactly what the console's are.
 */
typedef struct {
    uint32_t read;    /**< For KDRV_BLE_APP_READ: the read flag and its security.  */
    uint32_t write;   /**< For KDRV_BLE_APP_WRITE: the write flag and its security. */
    uint32_t notify;  /**< For KDRV_BLE_APP_NOTIFY: the notify flag and its security. */
} kdrv_ble_app_perm_t;

/** @brief The app's service, parsed: what kdrv_ble.c registers. */
typedef struct {
    uint8_t  svc_uuid[KDRV_BLE_UUID128_BYTES];                        /**< In wire order. */
    uint8_t  count;                                                   /**< Characteristics. */
    uint8_t  uuid[KDRV_BLE_APP_CHARS_MAX][KDRV_BLE_UUID128_BYTES];    /**< In wire order. */
    uint32_t flags[KDRV_BLE_APP_CHARS_MAX];                           /**< NimBLE's flags. */
} kdrv_ble_app_table_t;

/**
 * @brief Judge and parse @p svc into @p out for one build of the table.
 *
 * Takes the values and the ring from PSRAM the first time a service is
 * served. A service declared but left out logs one line saying why.
 *
 * @param svc  The app's service; NULL for none.
 * @param perm The flags each property earns.
 * @param out  The parsed table; its count is 0 unless this answers true.
 * @return true when the service is served.
 */
bool kdrv_ble_app_build(const kdrv_ble_app_service_t *svc, const kdrv_ble_app_perm_t *perm,
                        kdrv_ble_app_table_t *out);

/**
 * @brief Fix the app's choice of service for the boot, and answer it.
 *
 * kdrv_ble.c calls it when it builds the table it registers. From then on
 * kdrv_ble_app_select() refuses, and kdrv_ble_app_service() answers what
 * this answered.
 *
 * @return The service the table is built from; NULL for none.
 */
const kdrv_ble_app_service_t *kdrv_ble_app_fix(void);

/**
 * @brief Forget the choice and the fix, as a boot does.
 *
 * The host suite's: on the board a boot is the only way back.
 */
void kdrv_ble_app_select_forget(void);

/**
 * @brief Bytes kdrv_ble.c keeps in the block for its NimBLE structures.
 *
 * Defined by kdrv_ble.c (by the host suite's fake there), read once, when
 * the block is taken.
 *
 * @return Their size; 0 for none.
 */
size_t kdrv_ble_app_host_bytes(void);

/**
 * @brief kdrv_ble.c's part of the block, kdrv_ble_app_host_bytes() of them,
 *        aligned for any type and zeroed when the block was taken.
 * @return It; NULL before the block is taken.
 */
void *kdrv_ble_app_host(void);

/** @brief Whether the ring holds a notification. @return true when it does. */
bool kdrv_ble_app_queued(void);

/** @brief Forget the subscriptions and empty the ring and the queues: a link that began or ended. */
void kdrv_ble_app_link_reset(void);

/**
 * @brief A central subscribed to characteristic @p chr's notifications, or left.
 * @param chr Its place in the table.
 * @param on  true when subscribed.
 */
void kdrv_ble_app_subscribed(uint8_t chr, bool on);

/**
 * @brief A central wrote @p len bytes to characteristic @p chr: hand them to
 *        the write callback, and keep them as the value once it took them.
 * @param chr  Its place in the table.
 * @param data The bytes.
 * @param len  Their count, at most KDRV_BLE_APP_VALUE_MAX.
 * @return true when the callback took them.
 */
bool kdrv_ble_app_written(uint8_t chr, const uint8_t *data, uint16_t len);

/**
 * @brief Copy characteristic @p chr's value out, for a read or a read blob.
 *
 * A queued characteristic gives the oldest value in its queue, or 0 bytes
 * for an empty one, and the read that reaches the value's last byte takes
 * it out and calls the taken callback: one that starts at @p offset and
 * carries @p span bytes. A value longer than one read is read in parts, so
 * each part gets the same value until the last.
 *
 * @param chr    Its place in the table.
 * @param offset Where the read starts in the value: 0, or a read blob's.
 * @param span   Bytes the read's answer carries: the link's MTU less 1.
 * @param out    Where it goes.
 * @param cap    Bytes @p out holds.
 * @return Its bytes, the whole value; 0 for none.
 */
uint16_t kdrv_ble_app_read(uint8_t chr, uint16_t offset, uint16_t span, uint8_t *out,
                           uint16_t cap);

/**
 * @brief The host task's buffer for one write or one read, KDRV_BLE_APP_VALUE_MAX bytes.
 * @return It; NULL before the first build that served a service.
 */
uint8_t *kdrv_ble_app_scratch(void);

/**
 * @brief Copy the oldest notification in the ring out, leaving it in place.
 *
 * It stays the oldest until kdrv_ble_app_pop(): producers only add behind
 * it, and only the host task, which drains, takes one out.
 *
 * @param chr  Receives its characteristic.
 * @param out  Receives its bytes; KDRV_BLE_APP_VALUE_MAX of them fit.
 * @param len  Receives their count.
 * @return false when the ring is empty.
 */
bool kdrv_ble_app_peek(uint8_t *chr, uint8_t *out, uint16_t *len);

/** @brief Take the oldest notification out of the ring. */
void kdrv_ble_app_pop(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_APP_H_ */
