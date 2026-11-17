/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_image_walk.h
 * @brief An image's header chain walked by its own bytes: the one place it happens.
 *
 * esp_image_verify(), and esp_ota_end() which calls it, hash every segment by
 * the length its header declares and compare the total with the partition
 * only afterwards. A header that declares more than the image holds keeps
 * them reading flash for as long as the lengths say, in the caller's own
 * step. Whoever is about to hand them an image that arrived from outside
 * walks its chain first and refuses one that leaves its bytes.
 */
#ifndef KDRV_IMAGE_WALK_H
#define KDRV_IMAGE_WALK_H

#include <stdint.h>

#include "esp_image_format.h"
#include "esp_partition.h"

/** @brief Bytes of the SHA-256 an image may carry after its checksum. */
#define KDRV_IMAGE_HASH_LEN   32U

/** @brief The boundary the checksum byte pads an image out to, less one. */
#define KDRV_IMAGE_PAD_MASK   15U

/**
 * @brief Walk an image's header chain by its own bytes: no mmap, no hash.
 *
 * Magic, segment count, each segment's declared length, the checksum pad and
 * the optional appended hash, read through esp_partition_read(). A length
 * check, not a cryptographic one: it says whether the image the header
 * describes fits in the bytes it was given, and how long it is.
 *
 * @param p     The partition the image lies in.
 * @param base  Where the image starts, partition-relative.
 * @param limit How many bytes from @p base the image may take.
 * @return The image length (header + every segment + checksum pad + the
 *         optional appended hash), or 0 if the walk does not hold together:
 *         a bad magic or segment count, or a length that leaves @p limit.
 */
static inline uint32_t kdrv_image_walk_len(const esp_partition_t *p, uint32_t base,
                                           uint32_t limit)
{
    esp_image_header_t header;
    if ( (esp_partition_read(p, base, &header, sizeof(header)) != ESP_OK)
        || (ESP_IMAGE_HEADER_MAGIC != header.magic)
        || (0U == header.segment_count)
        || (ESP_IMAGE_MAX_SEGMENTS < header.segment_count) )
    {
        return 0U;
    }

    uint32_t off = sizeof(esp_image_header_t);
    for ( uint8_t i = 0U; i < header.segment_count; ++i )
    {
        esp_image_segment_header_t seg;
        if ( (off > limit)
            || (esp_partition_read(p, base + off, &seg, sizeof(seg)) != ESP_OK) )
        {
            return 0U;
        }
        /* Bound the addition, not its result. `data_len` comes straight off
         * flash and is not to be trusted: a value near UINT32_MAX wraps `off`
         * back to a small number, and a check made afterwards waves that
         * through. Such a segment does not run off the end of its bytes, it
         * runs around them. `off <= limit` holds above, so both subtractions
         * here are safe. */
        uint32_t const room = limit - off;
        if ( ((uint32_t)sizeof(seg) > room)
            || ((room - (uint32_t)sizeof(seg)) < seg.data_len) )
        {
            return 0U;
        }
        off += (uint32_t)sizeof(seg) + seg.data_len;
    }
    /* The trailing 1-byte checksum pads the image out to a 16-byte boundary,
     * then the optional SHA-256 "simple hash" follows it. */
    off = (off | KDRV_IMAGE_PAD_MASK) + 1U;
    if ( 0U != header.hash_appended )
    {
        off += KDRV_IMAGE_HASH_LEN;
    }
    if ( off > limit )
    {
        return 0U;
    }
    return off;
}

#endif /* KDRV_IMAGE_WALK_H */
