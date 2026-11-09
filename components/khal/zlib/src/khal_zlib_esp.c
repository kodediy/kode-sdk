/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_zlib_esp.c
 * @brief khal_zlib_decompress on ESP-IDF: the ROM's inflater, which costs no flash.
 */
#include "khal_zlib.h"

#include <stddef.h>
#include <stdint.h>

#include "khal_system.h"
#include "miniz.h"

/* zlib header, flat output buffer; HAS_MORE_INPUT stops a cut stream instead of decoding zeros. */
#define INFLATE_FLAGS \
    ((mz_uint32)TINFL_FLAG_PARSE_ZLIB_HEADER | (mz_uint32)TINFL_FLAG_HAS_MORE_INPUT \
     | (mz_uint32)TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)

kode_status_t khal_zlib_decompress(const void *src, size_t src_len,
                           void *dst, size_t dst_cap, size_t *out_len)
{
    if ( NULL != out_len )
    {
        *out_len = 0U;
    }
    if ( (NULL == src) || (0U == src_len) || (NULL == dst) || (0U == dst_cap)
        || (NULL == out_len) )
    {
        return KODE_DENIED;
    }

    tinfl_decompressor *const inf =
        (tinfl_decompressor *)khal_alloc_psram(sizeof(uint32_t), sizeof(tinfl_decompressor));
    if ( NULL == inf )
    {
        return KODE_NO_RESOURCES;
    }
    tinfl_init(inf);

    /* Both sizes go in as what there is and come back as what was used. */
    size_t in_bytes = src_len;
    size_t out_bytes = dst_cap;
    tinfl_status const ts = tinfl_decompress(inf, (const mz_uint8 *)src, &in_bytes,
                                             (mz_uint8 *)dst, (mz_uint8 *)dst, &out_bytes,
                                             INFLATE_FLAGS);
    khal_free(inf);

    if ( (TINFL_STATUS_DONE == ts) && (in_bytes == src_len) )
    {
        *out_len = out_bytes;
        return KODE_OK;
    }
    if ( TINFL_STATUS_HAS_MORE_OUTPUT == ts )
    {
        /* dst is full and the stream has more to give. */
        return KODE_DENIED;
    }
    /* Not one whole stream: damaged, cut short, followed by other bytes, or a bad checksum. */
    return KODE_ERROR;
}
