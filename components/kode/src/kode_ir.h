/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_ir.h
 * @brief kode.h's infrared lock, sends, listen and capture state, shared by
 *        kode_ir.c and kode_ir_capture.c. Private to this component.
 */
#ifndef KODE_IR_H_
#define KODE_IR_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode.h"
#include "kdrv_ir.h"

/** @brief How long one window of the background capture listens, ms. */
#define KODE_IR_CAPTURE_WINDOW_MS 500U

/** @brief The longest kode_ir_capture_stop() waits for the capture to end, ms: a window and a wait for the path. */
#define KODE_IR_CAPTURE_STOP_MS (KODE_IR_CAPTURE_WINDOW_MS + KODE_IR_BUSY_MS)

/** @brief How a listen ended. */
typedef enum {
    KODE_IR_HEARD,   /**< A frame was heard.                     */
    KODE_IR_QUIET,   /**< The time passed and nothing was heard. */
    KODE_IR_FAILED   /**< The receiver could not be used.        */
} kode_ir_heard_t;

/**
 * @brief Whether a background capture runs.
 * @return true while one does.
 */
bool kode_ir_capturing(void);

/**
 * @brief Say that a background capture exists, or no longer does; under the lock.
 * @param on true from its start to the end of the stop that wrote it.
 */
void kode_ir_capturing_set(bool on);

/**
 * @brief kode_enter(), and a refusal in a firmware with no link to the C5, which has no infrared.
 * @param name The public name of the call.
 * @return true when the call may go on; false with the error set.
 */
bool kode_ir_begin(const char *name);

/**
 * @brief Take the infrared lock, after kode_enter(); made on the first call.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
bool kode_ir_lock_take(const char *name);

/**
 * @brief The prologue of a send: kode_ir_begin(), then the lock; kode_ir_leave() gives it back.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
bool kode_ir_enter(const char *name);

/** @brief Give the turn back, at the end of a call kode_ir_enter() or kode_ir_lock_take() let in. */
void kode_ir_leave(void);

/**
 * @brief Say why a call to the infrared path failed.
 * @param rc   The sys answer.
 * @param part "transmitter" or "receiver".
 */
void kode_ir_fail(kode_status_t rc, const char *part);

/**
 * @brief Send a NEC frame, waiting its turn, with the lock held; the error is set on failure.
 * @param address 0 to KODE_IR_FILE_VALUE_MAX.
 * @param command 0 to KODE_IR_FILE_VALUE_MAX.
 * @return true when the frame went out.
 */
bool kode_ir_nec_send(uint32_t address, uint32_t command);

/**
 * @brief Send a raw frame, waiting its turn, with the lock held; the error is set on failure.
 * @param us         The durations, mark first.
 * @param n          How many, 1..KODE_IR_RAW_MAX.
 * @param carrier_hz The carrier, above 0.
 * @return true when the frame went out.
 */
bool kode_ir_raw_send(const uint16_t *us, uint32_t n, uint32_t carrier_hz);

/**
 * @brief Wait for one frame, with the lock held, asking AO_Ir window after window until @p ms;
 *        the error is set on failure.
 * @param sym     Out: KDRV_IR_RX_MAX_SYMBOLS symbols.
 * @param n       Out: how many were heard; written only when this answers KODE_IR_HEARD.
 * @param ms      The time to wait, or KODE_FOREVER.
 * @param failure Out: the sys status of a failure; written only when this answers KODE_IR_FAILED;
 *                may be NULL.
 * @return How it ended.
 */
kode_ir_heard_t kode_ir_listen(kdrv_ir_symbol_t *sym, uint32_t *n, uint32_t ms, kode_status_t *failure);

#endif /* KODE_IR_H_ */
