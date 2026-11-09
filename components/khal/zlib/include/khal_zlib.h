/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_zlib.h
 * @brief One whole zlib stream inflated into a buffer, with the ROM's decoder.
 */
#ifndef KHAL_ZLIB_H_
#define KHAL_ZLIB_H_

#include <stddef.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Inflate one whole zlib stream into a buffer.
 *
 * @param src     The stream.
 * @param src_len Bytes in @p src: the stream's own length, with nothing
 *                after it.
 * @param dst     Where the payload goes. It does not overlap @p src.
 * @param dst_cap Room in @p dst, in bytes: at least the payload's size.
 * @param out_len Receives the bytes written to @p dst: the payload's size on
 *                KODE_OK, 0 otherwise.
 * @return KODE_OK when @p src is one whole stream, it inflated whole and its
 *         checksum matches. KODE_DENIED when a pointer is NULL, a length is
 *         0, or the stream holds more than @p dst_cap bytes.
 *         KODE_NO_RESOURCES when PSRAM has no room for the decoder's state.
 *         KODE_ERROR when @p src is not one whole stream: it is damaged, it
 *         ends before the stream does, other bytes follow the stream, or the
 *         checksum does not match. Damage can also answer KODE_DENIED, when
 *         what it decodes to overflows @p dst before the decoder meets it.
 *         After a failure the contents of @p dst are not defined.
 */
kode_status_t khal_zlib_decompress(const void *src, size_t src_len,
                           void *dst, size_t dst_cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_ZLIB_H_ */
