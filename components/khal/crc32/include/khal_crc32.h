/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_crc32.h
 * @brief Reflected CRC-32 (zlib's), chainable over chunks.
 */
#ifndef KHAL_CRC32_H_
#define KHAL_CRC32_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The seed a fresh CRC starts from. */
#define KHAL_CRC32_INIT  0U

/**
 * @brief Fold @p len bytes of @p buf into the running CRC @p crc.
 *
 * Chainable: the return value is the seed for the next chunk, so a stream
 * checksums without ever being held whole.
 *
 * @param crc Running value; KHAL_CRC32_INIT for the first call.
 * @param buf Bytes to fold in.
 * @param len How many.
 * @return The updated running value.
 */
uint32_t khal_crc32(uint32_t crc, const void *buf, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_CRC32_H_ */
