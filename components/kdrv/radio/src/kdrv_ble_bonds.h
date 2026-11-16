/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_bonds.h
 * @brief The NimBLE-free half of kdrv_ble_bonds(): the count of bonded peers, kept where no lock is needed to read it.
 *
 * NimBLE's store is read under the host's lock, which a write holds for a
 * round trip to the C5. The driver counts the peers after every change of the
 * store and keeps the count here, so a reader never waits. A count made
 * outside the lock is kept only when the store has not changed since it
 * began: an older count never replaces a newer one.
 */
#ifndef KDRV_BLE_BONDS_H_
#define KDRV_BLE_BONDS_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Keep the count made after a change of the store.
 *
 * Called inside the hold of the store's lock that made the change, with the
 * peers counted inside that same hold: the lock is what puts these calls in
 * the order of the changes.
 *
 * @param n The peers the store holds now.
 * @return true when the count kept moved.
 */
bool kdrv_ble_bonds_changed(uint8_t n);

/**
 * @brief Begin a count made outside the store's lock.
 * @return The ticket to hand kdrv_ble_bonds_settle() with the count.
 */
uint32_t kdrv_ble_bonds_begin(void);

/**
 * @brief Keep a count made outside the store's lock, unless the store changed since it began.
 * @param ticket What kdrv_ble_bonds_begin() returned before the count.
 * @param n      The peers counted.
 * @return true when the count kept moved; false when it stood, or when a
 *         change of the store since the ticket made this count an old one.
 */
bool kdrv_ble_bonds_settle(uint32_t ticket, uint8_t n);

/**
 * @brief The count kept.
 * @return The peers the store held at its last change, or at the last count settled.
 */
uint8_t kdrv_ble_bonds_kept(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_BONDS_H_ */
