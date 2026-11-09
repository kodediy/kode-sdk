/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_dock_loop.h
 * @brief The E5 station's pads loop: frames sent out on one magnetic pad come back on the other.
 */
#ifndef KDRV_DOCK_LOOP_H_
#define KDRV_DOCK_LOOP_H_

#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDRV_DOCK_LOOP_FRAME_BYTES  16U      /**< Bytes in one frame.                         */
#define KDRV_DOCK_LOOP_BAUD_SLOW    9600U    /**< The first pass, pad 2 to pad 3.             */
#define KDRV_DOCK_LOOP_BAUD_FAST    115200U  /**< The second pass, and the reversed one.     */

/** @brief Why a loop did not come back whole. */
typedef enum {
    KDRV_DOCK_LOOP_OK = 0,      /**< Every frame came back.                             */
    KDRV_DOCK_LOOP_NO_ECHO,     /**< Not one byte came back: no bridge, no contact.     */
    KDRV_DOCK_LOOP_CORRUPT,     /**< Bytes came back, not every frame identical.        */
    KDRV_DOCK_LOOP_UART_BUSY    /**< No UART was free, or its driver would not install. */
} kdrv_dock_loop_why_t;

/** @brief What a loop measured. */
typedef struct {
    uint8_t  slow;   /**< Frames back at KDRV_DOCK_LOOP_BAUD_SLOW, TX on pad 2.      */
    uint8_t  fast;   /**< Frames back at KDRV_DOCK_LOOP_BAUD_FAST, TX on pad 2.      */
    uint8_t  rev;    /**< Frames back at KDRV_DOCK_LOOP_BAUD_FAST, TX on pad 3.      */
    uint8_t  why;    /**< kdrv_dock_loop_why_t.                                       */
    uint32_t ms;     /**< How long the three passes took.                             */
} kdrv_dock_loop_t;

/**
 * @brief Send @p count frames at each speed and count the echoes. Blocking: a
 *        worker's job.
 *
 * Without the bridge nothing comes back and each frame's wait is its own air
 * time plus a short margin, so the call never hangs.
 * The pads are the dock console's: the caller has them lent first (log_dock_lend_request()).
 *
 * @param count Frames at each speed, 1..255.
 * @param out   What came back.
 * @return KODE_OK when every frame of every pass came back identical;
 *         KODE_ERROR when one did not (out->why says how); KODE_NO_RESOURCES
 *         when no UART was free; KODE_DENIED for a NULL @p out or a zero
 *         @p count.
 */
kode_status_t kdrv_dock_loop_run(uint8_t count, kdrv_dock_loop_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_DOCK_LOOP_H_ */
