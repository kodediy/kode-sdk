/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_frames.h
 * @brief The P4's end of the Wi-Fi frames service: the C5's batches decoded and held for the radio's owner.
 */
#ifndef KDRV_FRAMES_H_
#define KDRV_FRAMES_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kode_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Records waiting for the owner. */
#define KDRV_FRAMES_RING_LEN  64U

/** @brief One frame, as the C5 summarised it. */
typedef struct {
    uint8_t  channel;                    /**< The radio's primary channel when it was heard. */
    int8_t   rssi;                       /**< dBm.                                           */
    uint8_t  fc[2];                      /**< Frame control, as on the air.                  */
    uint8_t  addr1[6];                   /**< Address 1, as on the air.                      */
    uint8_t  addr2[6];                   /**< Address 2; zeros when the frame has none.      */
    uint16_t len;                        /**< The frame's length on the air.                 */
    uint8_t  head[KODE_SNIFF_HEAD_LEN];  /**< After the MAC header; zeros past the end.      */
} kdrv_frame_t;

/** @brief What one batch said about its period. */
typedef struct {
    uint8_t  channel;    /**< The radio's primary channel in the period.            */
    bool     joined;     /**< The station holds an association.                     */
    int8_t   rssi_max;   /**< KODE_SNIFF_RSSI_NONE when the period heard nothing.   */
    uint32_t heard;      /**< Frames heard since START.                             */
    uint16_t mgmt;       /**< Management frames heard in the period.                */
    uint16_t data;       /**< Data frames heard in the period.                      */
} kdrv_frames_tick_t;

/** @brief An answer to START, STOP or STATE, read. */
typedef struct {
    bool    present;     /**< false: the C5 has no such service.                    */
    bool    on;          /**< The service is listening.                             */
    uint8_t channel;     /**< The radio's primary channel now.                      */
    bool    joined;      /**< The station holds an association.                     */
    uint8_t why;         /**< KODE_SNIFF_WHY_*.                                     */
} kdrv_frames_state_t;

/**
 * @brief Take the ring from PSRAM. Idempotent.
 * @return KODE_OK, or KODE_NO_RESOURCES when the ring or its lock cannot be had.
 */
kode_status_t kdrv_frames_init(void);

/**
 * @brief Decode a batch event's payload into the ring and the tick.
 *
 * Runs on esp-hosted's thread: one lock, held for the copy and nothing else.
 * Records that do not fit the ring are counted in kdrv_frames_dropped(), never
 * overwritten. Only the newest tick is kept.
 *
 * @param payload kode_sniff_batch_t, then its records; valid only during the call.
 * @param len     Payload bytes.
 * @return true when the batch was taken; false before kdrv_frames_init(), for
 *         a payload shorter than its header plus its records, or for a count
 *         over KODE_SNIFF_BATCH_MAX (nothing is queued then).
 */
bool kdrv_frames_feed(const uint8_t *payload, size_t len);

/**
 * @brief Read an answer to START, STOP or STATE. Pure: it keeps nothing.
 *
 * KODE_ST_OK and KODE_ST_REFUSED carry a kode_sniff_state_t and read as
 * present. Any other status, or a payload shorter than the state, reads as a
 * C5 without the service (an older image answers KODE_ST_BAD_REQUEST).
 *
 * @param status  KODE_ST_*.
 * @param payload The answer's payload; may be NULL when len is 0.
 * @param len     Payload bytes.
 * @param out     Receives the state.
 */
void kdrv_frames_answer(uint8_t status, const uint8_t *payload, size_t len,
                        kdrv_frames_state_t *out);

/**
 * @brief Take the newest batch's summary, once.
 * @param out Receives it.
 * @return false when no batch arrived since the last call.
 */
bool kdrv_frames_tick_take(kdrv_frames_tick_t *out);

/**
 * @brief Take the oldest waiting frame.
 * @param out Receives it.
 * @return false when none waits.
 */
bool kdrv_frames_next(kdrv_frame_t *out);

/** @brief Records the ring refused since the last kdrv_frames_forget(). */
uint32_t kdrv_frames_dropped(void);

/** @brief Empty the ring, the tick and the count. */
void kdrv_frames_forget(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_FRAMES_H_ */
