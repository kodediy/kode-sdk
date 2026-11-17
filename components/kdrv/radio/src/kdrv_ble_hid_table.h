/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_hid_table.h
 * @brief The HID service without NimBLE: what kdrv_ble.c registers, drains and keeps.
 */
#ifndef KDRV_BLE_HID_TABLE_H_
#define KDRV_BLE_HID_TABLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kdrv_ble_hid.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDRV_BLE_HID_SVC_UUID     0x1812U /**< The Bluetooth SIG's Human Interface Device service. */
#define KDRV_BLE_HID_ID_MOUSE     1U      /**< The mouse's report ID in the map.               */
#define KDRV_BLE_HID_ID_KEYBOARD  2U      /**< The keyboard's report ID in the map.            */
#define KDRV_BLE_HID_MAP_MAX      128U    /**< Bytes that hold the longest report map.         */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_ble.c, which is a file this check does not join to this one */
#define KDRV_BLE_HID_CCCDS        2U      /**< Subscriptions one bond spends on HID: an input
                                           *   report per kind.                               */
#define KDRV_BLE_HID_APPEAR_HID   0x03C0U /**< Appearance: a generic HID device.               */
#define KDRV_BLE_HID_APPEAR_KEYS  0x03C1U /**< Appearance: a keyboard.                         */
#define KDRV_BLE_HID_APPEAR_MOUSE 0x03C2U /**< Appearance: a mouse.                            */
/* cppcheck-suppress misra-c2012-2.5 ; used by kdrv_ble.c, which is a file this check does not join to this one */
#define KDRV_BLE_HID_PRINT_SEED   2166136261U /**< Where a table's print starts: FNV-1a's basis. */
#define KDRV_BLE_HID_PRINT_NONE   0U      /**< The print of a table without the HID service.   */
#define KDRV_BLE_HID_HOSTS_MAX    3U      /**< Hosts remembered: one for each bond.            */
#define KDRV_BLE_HID_ADDR_LEN     7U      /**< Bytes of a device's identity address: its type,
                                           *   then its six bytes.                            */
#define KDRV_BLE_HID_NVS_NS       "kble_hid"  /**< The NVS namespace of what the HID service keeps between boots. */
#define KDRV_BLE_HID_NVS_HOSTS    "hosts"     /**< Its key for the bonds made for the HID service.                */

/* ---- What a bond was made for -----------------------------------------------
 * A bond is made for the console or for the HID service, and for one of the
 * two alone: a computer paired as a mouse's host is not thereby let into the
 * console, and the phone paired for the console is not typed at by an app.
 * The mark is what the board was when the bond was written: a board serving
 * HID makes a host's bond, any other makes a console's. It is kept with the
 * bond, by the device's identity address, as the list of the hosts: a device
 * in it has a HID bond, any other a console bond. A device that wants the
 * other use pairs again while the board is the other thing. */

/** @brief The devices whose bond was made for the HID service, oldest first, as NVS keeps them. */
typedef struct {
    uint8_t count;                                                /**< How many; KDRV_BLE_HID_HOSTS_MAX at most. */
    uint8_t addr[KDRV_BLE_HID_HOSTS_MAX][KDRV_BLE_HID_ADDR_LEN];  /**< Each one's identity address.             */
} kdrv_ble_hid_hosts_t;

/**
 * @brief Whether a device still has a bond; kdrv_ble_hid_hosts_sift() asks it of each host.
 * @param addr The device's identity address, KDRV_BLE_HID_ADDR_LEN bytes.
 * @param ctx  What kdrv_ble_hid_hosts_sift() was given.
 * @return true when the bond store holds one for it.
 */
typedef bool (*kdrv_ble_hid_bonded_t)(const uint8_t *addr, void *ctx);

_Static_assert(sizeof(kdrv_ble_hid_hosts_t)
               == (1U + (KDRV_BLE_HID_HOSTS_MAX * KDRV_BLE_HID_ADDR_LEN)),
               "the hosts are stored as their count and their addresses, with no padding");

/** @brief What to do with a device whose link has just been encrypted. */
typedef enum {
    KDRV_BLE_HID_HOST_STAY = 0,  /**< Nothing: it is welcome.                              */
    KDRV_BLE_HID_HOST_HANG_UP    /**< Hang up: it came back for a HID service that is not served. */
} kdrv_ble_hid_host_do_t;

/**
 * @brief Fix the choice of kinds for the boot, and take the ring.
 *
 * kdrv_ble.c calls it when it builds the table it registers. From then on
 * kdrv_ble_hid_select() refuses. A ring PSRAM refuses serves no kind.
 *
 * @return The kinds the table is built with; 0 for none.
 */
uint8_t kdrv_ble_hid_fix(void);

/**
 * @brief Forget the choice, the fix and the counts, as a boot does.
 *
 * The host suite's: on the board a boot is the only way back.
 */
void kdrv_ble_hid_select_forget(void);

/**
 * @brief Copy the report map of @p kinds out: one collection per kind.
 * @param kinds The kinds served.
 * @param out   Where it goes.
 * @param cap   Bytes @p out holds; KDRV_BLE_HID_MAP_MAX holds every map.
 * @return Its bytes; 0 for no kind, or a map that does not fit @p cap.
 */
size_t kdrv_ble_hid_report_map(uint8_t kinds, uint8_t *out, size_t cap);

/**
 * @brief The appearance a board of @p kinds advertises.
 * @param kinds The kinds served.
 * @return A mouse's, a keyboard's, a generic HID device's for both; 0 for none.
 */
uint16_t kdrv_ble_hid_appearance(uint8_t kinds);

/** @brief Forget the subscriptions and empty the ring: a link that ended. */
void kdrv_ble_hid_link_reset(void);

/**
 * @brief A host subscribed to the input report of @p kind, or left it.
 * @param kind KDRV_BLE_HID_MOUSE or KDRV_BLE_HID_KEYBOARD.
 * @param on   true when subscribed.
 */
void kdrv_ble_hid_subscribed(uint8_t kind, bool on);

/** @brief Whether a report waits for the link. @return true when one does. */
bool kdrv_ble_hid_queued(void);

/**
 * @brief Copy the next report to send out, leaving it in place: an owed
 *        report of zeroes whose kind has nothing ahead of it, else the
 *        oldest in the ring.
 * @param kind Receives its kind.
 * @param out  Receives its bytes; KDRV_BLE_HID_REPORT_MAX of them fit.
 * @param len  Receives their count.
 * @return false when nothing waits.
 */
bool kdrv_ble_hid_peek(uint8_t *kind, uint8_t *out, uint16_t *len);

/**
 * @brief Take out the report the last kdrv_ble_hid_peek() gave, and free its slot.
 * @param sent true when the link took it: it becomes what a read of that
 *             kind's report gets; false counts it dropped.
 */
void kdrv_ble_hid_pop(bool sent);

/**
 * @brief Copy what a read of the input report of @p kind gets: the last one sent on this link.
 * @param kind KDRV_BLE_HID_MOUSE or KDRV_BLE_HID_KEYBOARD.
 * @param out  Where it goes; KDRV_BLE_HID_REPORT_MAX bytes fit.
 * @return Its bytes: a report of zeroes before the first; 0 for no such kind.
 */
uint16_t kdrv_ble_hid_last(uint8_t kind, uint8_t *out);

/** @brief Keep the keyboard LEDs the host wrote. @param leds The output report's byte. */
void kdrv_ble_hid_leds_set(uint8_t leds);

/** @brief The keyboard LEDs the host wrote last. @return The output report's byte; 0 before the first. */
uint8_t kdrv_ble_hid_leds(void);

/**
 * @brief Whether a new device may pair in an app: the time to pair runs and HID is served.
 * @return true while a pairing may go on.
 */
bool kdrv_ble_hid_pair_open(void);

/**
 * @brief Add @p len bytes to a table's print (FNV-1a, 32 bits).
 * @param print The print so far; KDRV_BLE_HID_PRINT_SEED to start.
 * @param data  The bytes.
 * @param len   Their count.
 * @return The print with them in.
 */
uint32_t kdrv_ble_hid_print_add(uint32_t print, const void *data, size_t len);

/**
 * @brief The print a table is kept under: never KDRV_BLE_HID_PRINT_NONE for a table with HID.
 * @param print  What kdrv_ble_hid_print_add() added up to.
 * @param served Whether the table carries the HID service.
 * @return KDRV_BLE_HID_PRINT_NONE without HID; the print, moved off that value, with it.
 */
uint32_t kdrv_ble_hid_print_end(uint32_t print, bool served);

/**
 * @brief Whether the subscriptions the bonds stored belong to another table.
 * @param kept The print the last boot left; KDRV_BLE_HID_PRINT_NONE for none.
 * @param now  This boot's table's print.
 * @return true when they must be forgotten. Never between two tables without HID.
 */
bool kdrv_ble_hid_prune_due(uint32_t kept, uint32_t now);

/**
 * @brief Whether a stored subscription goes when the table changed.
 * @param handle  The value handle it was stored for.
 * @param changed Service Changed's value handle; 0 when the table has none.
 * @return true for every one but Service Changed's, which tells its bond to
 *         read the table again.
 */
bool kdrv_ble_hid_prune_takes(uint16_t handle, uint16_t changed);

/**
 * @brief Whether @p hosts is a record this build wrote.
 * @param hosts What the store gave.
 * @param len   Its bytes.
 * @return true when its size and its count are right.
 */
bool kdrv_ble_hid_hosts_ok(const kdrv_ble_hid_hosts_t *hosts, size_t len);

/**
 * @brief Whether the bond of @p addr was made for the HID service.
 * @param hosts The hosts.
 * @param addr  KDRV_BLE_HID_ADDR_LEN bytes: an identity address.
 * @return true for a HID bond; false for a console bond, which every other is.
 */
bool kdrv_ble_hid_hosts_has(const kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr);

/**
 * @brief Mark the bond of @p addr as made for the HID service; with no room, the oldest gives its place.
 * @param hosts The hosts.
 * @param addr  KDRV_BLE_HID_ADDR_LEN bytes: an identity address.
 * @return true when @p hosts changed and is to be stored.
 */
bool kdrv_ble_hid_hosts_add(kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr);

/**
 * @brief Take the HID mark off @p addr: its bond is a console's, or is gone.
 * @param hosts The hosts.
 * @param addr  KDRV_BLE_HID_ADDR_LEN bytes: an identity address.
 * @return true when @p hosts changed and is to be stored.
 */
bool kdrv_ble_hid_hosts_drop(kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr);

/**
 * @brief Mark a bond that has just been written, by what the board is now.
 *
 * A board that serves HID makes a host's bond; any other makes a console's,
 * and takes the mark off a device that was a host and paired again.
 *
 * @param hosts  The hosts.
 * @param addr   The device's identity address, KDRV_BLE_HID_ADDR_LEN bytes.
 * @param served Whether this boot serves the HID service.
 * @return true when @p hosts changed and is to be stored.
 */
bool kdrv_ble_hid_bond_note(kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr, bool served);

/**
 * @brief Forget the hosts that have no bond any more.
 * @param hosts  The hosts.
 * @param bonded Says whether a device still has a bond.
 * @param ctx    Handed to @p bonded.
 * @return true when @p hosts changed and is to be stored.
 */
bool kdrv_ble_hid_hosts_sift(kdrv_ble_hid_hosts_t *hosts, kdrv_ble_hid_bonded_t bonded, void *ctx);

/**
 * @brief Read the hosts the last boot kept.
 * @param hosts Receives them; none when the store holds no record this build wrote.
 */
void kdrv_ble_hid_hosts_load(kdrv_ble_hid_hosts_t *hosts);

/**
 * @brief Keep the hosts for the next boot; none is no record at all.
 * @param hosts The hosts.
 */
void kdrv_ble_hid_hosts_store(const kdrv_ble_hid_hosts_t *hosts);

/**
 * @brief Whether a device may reach an attribute, by what its bond was made for.
 *
 * The console's service is a console bond's alone, and the HID service a HID
 * bond's alone. Asked only for the attributes of those two services, in a
 * table that serves HID.
 *
 * @param hid_attr Whether the attribute is one of the HID service's.
 * @param hid_bond Whether the device's bond was made for the HID service.
 * @return true when it may.
 */
bool kdrv_ble_hid_access_ok(bool hid_attr, bool hid_bond);

/**
 * @brief What to do with a device whose link has just been encrypted.
 *
 * A device with a HID bond comes back by itself whenever it sees the board.
 * While HID is not served it would hold the one link for nothing, so it is
 * hung up on.
 *
 * @param hosts  The hosts.
 * @param addr   The device's identity address, KDRV_BLE_HID_ADDR_LEN bytes.
 * @param served Whether this boot serves the HID service.
 * @return What to do.
 */
kdrv_ble_hid_host_do_t kdrv_ble_hid_host_judge(const kdrv_ble_hid_hosts_t *hosts,
                                               const uint8_t *addr, bool served);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_BLE_HID_TABLE_H_ */
