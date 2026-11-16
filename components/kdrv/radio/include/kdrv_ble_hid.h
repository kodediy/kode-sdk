/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_hid.h
 * @brief The board as a Bluetooth mouse and keyboard: the choice, the input reports and the pairing door.
 */
#ifndef KDRV_BLE_HID_H_
#define KDRV_BLE_HID_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- HID over GATT ---------------------------------------------------------
 * Served only when asked: kdrv_ble_hid_select() names the kinds before
 * kdrv_ble_start() registers the GATT table, and the table then carries the
 * HID service (0x1812) with the driver's own report map, Battery and Device
 * Information, after the console's and the app's services. The advert
 * carries 0x1812 and the appearance beside the console's UUID, and gives up
 * its transmit power for them (kdrv_ble_adv_shape()). A board that never
 * asks keeps the table and the advert it had.
 *
 * A bond is made for the console or for the HID service, and opens that one
 * alone: a bond written while the board serves HID is a host's, and the
 * console's service refuses it; any other is the console's, and the HID
 * service refuses it, so no report reaches the phone paired for the console.
 * The refusal is ATT's Insufficient Authorization. A device that wants the
 * other use pairs again while the board is the other thing.
 *
 * A host comes back by itself whenever it sees the board. In a boot that
 * serves no HID it is hung up on once its link is encrypted, so the one link
 * there is stays free.
 *
 * AO_Radio owns the reports: a caller claims a slot of the ring with
 * kdrv_ble_hid_claim(), from any thread, and posts the report to AO_Radio,
 * which alone calls kdrv_ble_hid_push() and kdrv_ble_hid_kick(). NimBLE's
 * host task notifies. A link that ends empties the ring.
 *
 * The report of zeroes, which lets every key or button go, needs no slot:
 * kdrv_ble_hid_owe() has it sent once the kind's reports ahead of it have
 * gone, so a key is never left down for want of room.
 */

#define KDRV_BLE_HID_MOUSE       0x01U  /**< A mouse: three buttons, x, y and a wheel.          */
#define KDRV_BLE_HID_KEYBOARD    0x02U  /**< A keyboard: eight modifiers and six keys.          */
#define KDRV_BLE_HID_KINDS       (KDRV_BLE_HID_MOUSE | KDRV_BLE_HID_KEYBOARD) /**< Every kind.  */
#define KDRV_BLE_HID_MOUSE_LEN   4U     /**< Bytes of a mouse report: buttons, x, y, wheel.     */
#define KDRV_BLE_HID_KEYS_LEN    8U     /**< Bytes of a keyboard report: modifiers, 0, six keys. */
#define KDRV_BLE_HID_KEYS_MAX    6U     /**< Keys one keyboard report holds down.               */
#define KDRV_BLE_HID_KEYS_AT     2U     /**< Where the keys start in a keyboard report.         */
#define KDRV_BLE_HID_REPORT_MAX  8U     /**< Bytes of the longest input report.                 */
#define KDRV_BLE_HID_RING_SLOTS  16U    /**< Reports that wait for the link.                    */
#define KDRV_BLE_HID_BUTTONS     0x07U  /**< The mouse's buttons: bit 0 left, 1 right, 2 middle. */
#define KDRV_BLE_HID_STEP_MAX    127    /**< The longest move or turn one mouse report carries. */
#define KDRV_BLE_HID_KEY_MAX     0x65U  /**< The last key the report map declares: usage 101.   */

/**
 * @brief Choose the kinds of HID device the board is; 0 for none.
 *
 * A choice, read once, when kdrv_ble_start() registers the GATT table. Once
 * that table is registered the choice is fixed for the boot, and this
 * refuses and changes nothing. Any thread.
 *
 * @param kinds KDRV_BLE_HID_MOUSE, KDRV_BLE_HID_KEYBOARD, both, or 0.
 * @return KODE_OK when the choice is kept; KODE_DENIED for a kind that does
 *         not exist, once the table is registered, or in a build without
 *         the console's pairing, which HID needs.
 */
kode_status_t kdrv_ble_hid_select(uint8_t kinds);

/**
 * @brief The kinds chosen, which are the kinds served once the table is registered.
 * @return KDRV_BLE_HID_MOUSE, KDRV_BLE_HID_KEYBOARD, both, or 0. Any thread.
 */
uint8_t kdrv_ble_hid_kinds(void);

/**
 * @brief Whether the GATT table is registered with the HID service in it.
 * @return true from the table's registration on, when a kind was chosen. Any thread.
 */
bool kdrv_ble_hid_served(void);

/**
 * @brief Whether a host is subscribed to the input reports of every kind served.
 * @return true when a report claimed now reaches the host. Any thread.
 */
bool kdrv_ble_hid_ready(void);

/**
 * @brief Write a mouse report. Movement and wheel are relative.
 *
 * A step of -128, which the report map does not allow, is written as -127.
 *
 * @param buttons KDRV_BLE_HID_BUTTONS' bits; the others are dropped.
 * @param dx      Right, -127 to 127.
 * @param dy      Down, -127 to 127.
 * @param wheel   Up, -127 to 127.
 * @param out     Receives the report's KDRV_BLE_HID_MOUSE_LEN bytes.
 */
void kdrv_ble_hid_mouse_pack(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel,
                             uint8_t out[KDRV_BLE_HID_MOUSE_LEN]);

/**
 * @brief Write a keyboard report: the keys held down now.
 * @param modifiers The eight modifier bits, left Control first.
 * @param keys      KDRV_BLE_HID_KEYS_MAX usage IDs, 0 for none; NULL for no key down.
 * @param out       Receives the report's KDRV_BLE_HID_KEYS_LEN bytes.
 */
void kdrv_ble_hid_keys_pack(uint8_t modifiers, const uint8_t *keys,
                            uint8_t out[KDRV_BLE_HID_KEYS_LEN]);

/**
 * @brief Claim @p slots of the ring for as many reports of @p kind, all or none.
 *
 * Every slot claimed is given back once: by kdrv_ble_hid_push(), which every
 * claimed report reaches, or by kdrv_ble_hid_unclaim() when the reports never
 * leave their caller. Any thread.
 *
 * @param kind  KDRV_BLE_HID_MOUSE or KDRV_BLE_HID_KEYBOARD.
 * @param slots How many, 1 to KDRV_BLE_HID_RING_SLOTS.
 * @return KODE_OK with the slots held; KODE_DENIED when that kind is not
 *         served, no host is subscribed to it or @p slots is out of range;
 *         KODE_NO_RESOURCES when fewer are free, or while the kind's report
 *         of zeroes is owed (kdrv_ble_hid_owe()).
 */
kode_status_t kdrv_ble_hid_claim(uint8_t kind, uint8_t slots);

/**
 * @brief Give back the slots of reports that were never pushed. Any thread.
 * @param kind  The kind claimed.
 * @param slots How many.
 */
void kdrv_ble_hid_unclaim(uint8_t kind, uint8_t slots);

/**
 * @brief Owe the host the report of zeroes of @p kind: every key or button up.
 *
 * It takes no slot, so it is never refused for want of room. The host task
 * sends it once every report of that kind claimed before it has gone, and
 * until then the kind takes no new report. Any thread; the drain is woken
 * with kdrv_ble_hid_kick().
 *
 * @param kind KDRV_BLE_HID_MOUSE or KDRV_BLE_HID_KEYBOARD.
 * @return KODE_OK when it is owed; KODE_DENIED when that kind is not served
 *         or no host is subscribed to it, and then nothing is held down.
 */
kode_status_t kdrv_ble_hid_owe(uint8_t kind);

/**
 * @brief Queue one claimed report for the host task. AO_Radio's alone.
 *
 * A report whose host left since the claim, or that is malformed, is dropped
 * and counted; its slot is free again either way.
 *
 * @param kind The kind claimed.
 * @param data The report: KDRV_BLE_HID_MOUSE_LEN or KDRV_BLE_HID_KEYS_LEN bytes.
 * @param len  Its bytes.
 */
void kdrv_ble_hid_push(uint8_t kind, const uint8_t *data, size_t len);

/**
 * @brief Wake the host task for the reports queued. AO_Radio's alone.
 *
 * Defined by kdrv_ble.c (by the host suite's fake there).
 */
void kdrv_ble_hid_kick(void);

/**
 * @brief The reports notified and the reports dropped since boot.
 * @param sent    Receives the reports the link took; may be NULL.
 * @param dropped Receives the reports queued and never sent; may be NULL.
 */
void kdrv_ble_hid_counts(uint32_t *sent, uint32_t *dropped);

/**
 * @brief Say whether the time to pair a new device runs. AO_Radio's alone.
 *
 * In an app a new device pairs only while this is open and the HID service
 * is served; kodeOS decides by its advert, as it does without HID.
 *
 * @param open true while the time runs.
 */
void kdrv_ble_hid_pair_window(bool open);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_HID_H_ */
