/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_identity.h
 * @brief The board's serial, derived from its C5's MAC.
 */
#ifndef KHAL_IDENTITY_H_
#define KHAL_IDENTITY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief NVS namespace of the cached C5 MAC; the factory reset keeps it. */
#define KHAL_IDENTITY_NVS_NS "kident"

/** @brief Bytes in a MAC address. */
#define KHAL_ID_MAC_LEN     6U

/** @brief Canonical serial plus NUL: "KODE" + 10 symbols + 1 check. */
#define KHAL_ID_LEN         16U

/** @brief Printed serial plus NUL: "KODE-XXXX-XXXX-XXX". */
#define KHAL_ID_PRETTY_LEN  19U

/**
 * @brief Name the board from the cached C5 MAC. Re-reads the cache each call.
 * @return KODE_OK, or KODE_ERROR when no C5 MAC was ever cached.
 */
kode_status_t khal_identity_init(void);

/**
 * @brief Name the board from its C5's base MAC, and cache the MAC.
 *
 * The C5's owner calls it when the link comes up. Another MAC than the one
 * in use renames the board.
 *
 * @param mac KHAL_ID_MAC_LEN bytes, most significant first.
 * @return KODE_OK; KODE_DENIED for NULL or six zero bytes; KODE_IO_ERROR
 *         when the cache could not be written (the name applies this boot).
 */
kode_status_t khal_identity_set_mac(const uint8_t mac[KHAL_ID_MAC_LEN]);

/**
 * @brief The canonical serial.
 * @return "KODE78YR5EEV0XS", or an empty string when unknown.
 */
const char *khal_identity_serial(void);

/**
 * @brief The printed serial.
 * @return "KODE-78YR-5EEV-0XS", or an empty string when unknown.
 */
const char *khal_identity_pretty(void);

/**
 * @brief Whether this board knows its own name.
 * @return true once the board has a serial.
 */
bool khal_identity_valid(void);

/**
 * @brief The C5's MAC behind the serial.
 * @param mac KHAL_ID_MAC_LEN bytes out, most significant first, output pointer.
 * @return KODE_OK, or KODE_ERROR when identity is unknown.
 */
kode_status_t khal_identity_mac(uint8_t mac[KHAL_ID_MAC_LEN]);

/**
 * @brief Encode any MAC into a canonical serial. Pure: no state, no hardware.
 * @param mac KHAL_ID_MAC_LEN bytes in, most significant first.
 * @param out Destination, NUL-terminated on KODE_OK.
 * @param cap Size of @p out; at least KHAL_ID_LEN.
 * @return KODE_OK, or KODE_ERROR on a null pointer or too small a buffer.
 */
kode_status_t khal_identity_encode(const uint8_t mac[KHAL_ID_MAC_LEN], char *out, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_IDENTITY_H_ */
