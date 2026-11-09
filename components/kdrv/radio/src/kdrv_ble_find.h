/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_find.h
 * @brief The NimBLE-free half of kdrv_ble_scan_find(): which advert matches, and what is kept.
 */
#ifndef KDRV_BLE_FIND_H_
#define KDRV_BLE_FIND_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kdrv_ble.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest name a find looks for: what one advert holds beside
 *         its length and type bytes. */
#define KDRV_BLE_FIND_NAME_MAX  (KDRV_BLE_ADV_MAX - 2U)

/** @brief Bytes of a BLE device address. */
#define KDRV_BLE_FIND_ADDR_LEN  6U

/** @brief One find: the name wanted, whether it is listening, what it heard. */
typedef struct {
    char            name[KDRV_BLE_FIND_NAME_MAX + 1U]; /**< The prefix wanted.   */
    volatile bool   on;    /**< Listening: advertisements are judged.            */
    /* Set last, by the host task: the worker reads dev only once it sees it. */
    volatile bool   hit;   /**< Heard; @c dev holds the first match.             */
    kdrv_ble_dev_t  dev;   /**< The first match: address, rssi, name.            */
} kdrv_ble_find_t;

/**
 * @brief Start a find for @p name.
 * @param f    The find; cleared.
 * @param name The name to look for; a prefix of the advertised one matches.
 * @return false for NULL, an empty name or one longer than KDRV_BLE_FIND_NAME_MAX.
 */
bool kdrv_ble_find_begin(kdrv_ble_find_t *f, const char *name);

/**
 * @brief Judge one advertisement or scan response.
 * @param f         The find.
 * @param addr_le   The advertiser's address, least significant byte first
 *                  (NimBLE's order); kept most significant first.
 * @param addr_type KDRV_BLE_ADDR_PUBLIC or KDRV_BLE_ADDR_RANDOM.
 * @param rssi      The reading, dBm.
 * @param name      The local name the packet carries, not terminated; NULL for none.
 * @param name_len  Its bytes.
 * @return true on the first match, the one that ends the listen; false for
 *         everything else, later matches included.
 */
bool kdrv_ble_find_offer(kdrv_ble_find_t *f, const uint8_t *addr_le, uint8_t addr_type,
                         int8_t rssi, const uint8_t *name, size_t name_len);

/**
 * @brief Stop judging.
 * @param f The find.
 */
void kdrv_ble_find_end(kdrv_ble_find_t *f);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_FIND_H_ */
