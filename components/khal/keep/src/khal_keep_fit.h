/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_keep_fit.h
 * @brief Where the kept block sits and how the heap is cut to hold it, private to khal's keep.
 */
#ifndef KHAL_KEEP_FIT_H_
#define KHAL_KEEP_FIT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "khal_keep.h"

/* ---- The block ---- */

#define KHAL_KEEP_END_GAP    (16U * 1024U)                     /**< Under the top of the PSRAM: the heap stops about 4 KB short of it. */
#define KHAL_KEEP_LINE       63U                               /**< A cache line, less one. */
#define KHAL_KEEP_HOLD_BYTES (KHAL_KEEP_BYTES + (64U * 1024U)) /**< What is held: the block and its margin. */
#define KHAL_KEEP_SLACK_MIN  256U                              /**< The least left over the held piece: room for the heap to cut it. */
#define KHAL_KEEP_SLACK_MAX  4096U                             /**< The most left over the held piece: it still ends inside the block's gap. */
#define KHAL_KEEP_FILLS_MAX  16U                               /**< Fills tried before giving up. */

/* ---- The heap's rules (ESP-IDF's TLSF) ---- */

#define KHAL_KEEP_SL_LOG2    5U   /**< TLSF's second-level count, log2, for a pool over 256 KB (tlsf.c, tlsf_create()). */
#define KHAL_KEEP_SMALL      128U /**< TLSF's small block size for that pool, 1 << (5 + 2): a request under it keeps its size. */
#define KHAL_KEEP_ALIGN      4U   /**< TLSF's alignment: every request is a multiple of it. */
#define KHAL_KEEP_HEADER     4U   /**< What a block's header takes from the free block it is cut from. */

/**
 * @brief Where the block starts, for a PSRAM that ends at @p top. Every image agrees on this address.
 * @param top The first address past the PSRAM.
 * @return The block's first byte, cache-line aligned.
 */
uintptr_t khal_keep_target(uintptr_t top);

/**
 * @brief What the heap takes for a request: rounded up to its size class, as TLSF's mapping_search() does.
 * @param request_bytes The size asked for.
 * @return The size the heap allocates.
 */
size_t khal_keep_taken(size_t request_bytes);

/**
 * @brief The next fill for a free block of @p block_bytes, a size the heap takes exactly.
 * @param block_bytes The free block's exact size.
 * @return Bytes to fill; 0 when the block is too short or nothing is left to fill.
 */
size_t khal_keep_fill(size_t block_bytes);

/**
 * @brief Whether the held piece can be cut from a free block of @p block_bytes.
 * @param block_bytes The free block's exact size.
 * @return true when it is the held piece plus a slack between the two limits, both included.
 */
bool khal_keep_ready(size_t block_bytes);

/**
 * @brief Whether a piece of @p held_bytes at @p from holds the whole block.
 * @param top        The first address past the PSRAM.
 * @param from       The piece's first byte.
 * @param held_bytes The piece's size.
 * @return true when the block lies inside the piece.
 */
bool khal_keep_covers(uintptr_t top, uintptr_t from, size_t held_bytes);

#endif /* KHAL_KEEP_FIT_H_ */
