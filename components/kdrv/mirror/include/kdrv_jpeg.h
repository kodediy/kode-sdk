/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_jpeg.h
 * @brief The ESP32-P4's JPEG encoder, as a device with one owner.
 */
#ifndef KDRV_JPEG_H_
#define KDRV_JPEG_H_

#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocate the engine, its output buffer, and its encode guard.
 *        Idempotent.
 * @return KODE_NO_RESOURCES when the engine, the output buffer, or the
 *         encode guard (see kdrv_jpeg_encode()) will not allocate.
 */
kode_status_t kdrv_jpeg_init(void);

/**
 * @brief Compress a rectangle of RGB565 into JPEG.
 *
 * The INPUT needs no alignment - the driver imposes none, which is what lets
 * a band of the panel's framebuffer be encoded where it lies instead of
 * copied first. The
 * OUTPUT does, and this file owns a buffer that satisfies it.
 *
 * @param rgb565  w*h*2 bytes, row-major. Borrowed for the call only.
 * @param w       Width in pixels. Any value; it need not be a multiple of 16.
 * @param h       Height in pixels.
 * @param quality 1..100.
 * @param out     Receives this driver's output buffer. Borrowed, and valid
 *                only until the next call - there is one engine and one
 *                buffer, so a second caller would be reading the first's
 *                frame as it was overwritten. That second caller is refused
 *                rather than let read a half-written buffer: see @return.
 * @param out_len Receives the JPEG's length in bytes.
 * @return KODE_DENIED before kdrv_jpeg_init(), for a NULL rgb565/out/out_len,
 *         for w or h of 0, for quality outside 1..100, or when another call
 *         is already encoding - the last is a normal, expected outcome under
 *         contention, not a fault. KODE_ERROR if the engine refused,
 *         KODE_NO_RESOURCES if the result did not fit.
 */
kode_status_t kdrv_jpeg_encode(const uint8_t *rgb565, uint32_t w, uint32_t h,
                               uint32_t quality, uint8_t **out,
                               uint32_t *out_len);

/**
 * @brief `test component=jpeg`: encode a known rectangle and read it back.
 *
 * Here rather than in the console for the reason kdrv_storage_selftest()'s
 * body is here: what it checks is the format this layer produces, and nothing
 * above kdrv should need to know a JPEG's magic bytes to have an opinion.
 *
 * @return KODE_OK if the SOI/EOI framing checks out, KODE_DENIED if the
 *         encoder was busy with someone else's call rather than broken -
 *         a caller should tell an operator to retry, not that the codec
 *         failed - KODE_ERROR or KODE_NO_RESOURCES otherwise.
 */
kode_status_t kdrv_jpeg_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_JPEG_H_ */
