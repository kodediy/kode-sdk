/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ppa.h
 * @brief The ESP32-P4's Pixel Processing Accelerator, as a device.
 */
#ifndef KDRV_PPA_H_
#define KDRV_PPA_H_

#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Register the client and allocate the output buffer. Idempotent.
 * @return KODE_NO_RESOURCES when the client, the output buffer, or the
 *         prepare guard (see kdrv_ppa_prepare()) will not allocate.
 */
kode_status_t kdrv_ppa_init(void);

/**
 * @brief Copy a picture of RGB565 into this driver's buffer, halving it if
 *        asked.
 *
 * @param src     w*h*2 bytes, row-major, top-down. Borrowed for the call.
 * @param w       Source width in pixels.
 * @param h       Source height in pixels.
 * @param scale   1 for a same-size mirror, 2 to also halve both axes.
 *                No other value is accepted.
 * @param dst     Receives this driver's output buffer, top-down. Borrowed,
 *                and valid only until the next call - one client, one
 *                buffer.
 * @param dst_w   Receives w/scale.
 * @param dst_h   Receives h/scale.
 * @return KODE_DENIED before kdrv_ppa_init(), for a bad argument, for a
 *         scale other than 1 or 2, for a result that would not fit the
 *         buffer, or when another call is already running - the last is a
 *         normal, expected outcome under contention (one client, one output
 *         buffer), not a fault. KODE_ERROR when the hardware refused.
 */
kode_status_t kdrv_ppa_prepare(const uint8_t *src, uint32_t w, uint32_t h,
                               uint32_t scale, uint8_t **dst,
                               uint32_t *dst_w, uint32_t *dst_h);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_PPA_H_ */
