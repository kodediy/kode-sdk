/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kring.h
 * @brief The stream's ring: mono frames from one writer to one reader.
 */
#ifndef KRING_H_
#define KRING_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A ring of mono 16-bit frames. The fields are the module's own. */
typedef struct {
    int16_t  *buf;   /**< @c cap frames, owned by whoever built the ring.     */
    uint32_t  cap;   /**< How many frames it holds.                           */
    uint32_t  head;  /**< Frames put, modulo 2 * @c cap. The writer's only.   */
    uint32_t  tail;  /**< Frames taken, modulo 2 * @c cap. The reader's only. */
} kring_t;

/**
 * @brief Make @p r an empty ring over @p buf.
 *
 * Not safe against a writer or a reader already using @p r: call it before
 * either starts.
 *
 * @param r   The ring.
 * @param buf Storage for @p cap frames.
 * @param cap How many frames @p buf holds, below 2^30; 0 makes a ring nothing
 *            fits in.
 */
void kring_init(kring_t *r, int16_t *buf, uint32_t cap);

/**
 * @brief How many frames are waiting to be taken.
 * @param r The ring.
 * @return 0 .. cap.
 */
uint32_t kring_fill(const kring_t *r);

/**
 * @brief Put as many of @p n frames as there is room for. The writer's call.
 *
 * @param r        The ring.
 * @param frames   The caller's frames: @p n mono samples, or @p n stereo
 *                 pairs (left then right) when @p channels is 2.
 * @param n        How many frames.
 * @param channels 1 or 2. Anything else puts nothing.
 * @return How many frames went in, 0 .. @p n.
 */
size_t kring_put(kring_t *r, const int16_t *frames, size_t n, uint8_t channels);

/**
 * @brief Take up to @p max frames into @p out. The reader's call.
 *
 * @param r   The ring.
 * @param out Where the frames go.
 * @param max The most to take.
 * @return How many were taken, 0 .. @p max.
 */
uint32_t kring_take(kring_t *r, int16_t *out, uint32_t max);

#ifdef __cplusplus
}
#endif

#endif /* KRING_H_ */
