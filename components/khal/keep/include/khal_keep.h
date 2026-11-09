/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_keep.h
 * @brief A block of PSRAM that outlives a software reset, in every image.
 */
#ifndef KHAL_KEEP_H_
#define KHAL_KEEP_H_

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The block's size in bytes. */
#define KHAL_KEEP_BYTES (1024U * 1024U)

/**
 * @brief Take the block out of the heap. Once, before the first big allocation.
 */
void khal_keep_init(void);

/**
 * @brief The block, KHAL_KEEP_BYTES long, 64-byte aligned.
 *
 * A power-on leaves noise in it: trust only contents a magic, a version and a CRC vouch for.
 * @return NULL when this boot could not set it aside: the caller has no copy and
 *         takes the road it always took.
 */
void *khal_keep(void);

/**
 * @brief Write the cache lines of the block's bytes [off, off + len) back to the
 *        PSRAM.
 *
 * A software reset does not flush everything: what is still dirty in the cache
 * may be lost, and the PSRAM keeps the older bytes. Write the payload, sync it,
 * then write and sync what vouches for it.
 *
 * @param off First byte.
 * @param len How many.
 */
void khal_keep_sync(size_t off, size_t len);

/**
 * @brief Forget the PSRAM's timing, as a power-on would. The crystal's calibration
 *        stays: the running clock reads it. For the bench, right before a reset.
 */
void khal_keep_forget_warm(void);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_KEEP_H_ */
