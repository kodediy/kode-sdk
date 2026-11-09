/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ir.h
 * @brief The P4's side of the IR block: SFH4045N emitters and a TSOP75338 receiver on the C5.
 */
#ifndef KDRV_IR_H_
#define KDRV_IR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The only carrier this board's own receiver can hear, in Hz. */
#define KDRV_IR_CARRIER_HZ      38000U

/** @brief Most symbols one transmission may carry. */
#define KDRV_IR_TX_MAX_SYMBOLS    256U

/** @brief Most symbols one capture may hold; longer captures are truncated. */
#define KDRV_IR_RX_MAX_SYMBOLS    256U

/** @brief How long esp-hosted waits for the C5's answer to one RPC: its
 *         DEFAULT_RPC_RSP_TIMEOUT, 5 s (rpc_slave_if.h:61). Every call that
 *         is a request over the link - a frame, an arm, the request of a line
 *         reading - may spend it, and may queue behind a longer RPC on the
 *         shared channel. */
#define KDRV_IR_RPC_WAIT_MS     5000U

/** @brief How long a line reading then waits for the C5's reading after its
 *         request was accepted. */
#define KDRV_IR_REPLY_MS         500U

/** @brief The longest one call of this driver can legitimately take: a line
 *         reading's request and then its reply wait. */
#define KDRV_IR_CALL_MAX_MS     (KDRV_IR_RPC_WAIT_MS + KDRV_IR_REPLY_MS)

/** @brief One mark (carrier on) and the space after it, in microseconds. */
typedef struct {
    uint16_t mark_us;
    uint16_t space_us;
} kdrv_ir_symbol_t;

/** @brief What one received frame amounted to. */
typedef struct {
    uint16_t addr;      /**< NEC address, when @c nec.                     */
    uint16_t cmd;       /**< NEC command, when @c nec.                     */
    uint16_t symbols;   /**< How many symbols arrived, decoded or not.     */
    bool     nec;       /**< Whether the frame decoded as NEC.             */
} kdrv_ir_frame_t;

/**
 * @brief Called from interrupt context when a capture is ready.
 *
 * What it may do is post a static event and return. The symbols stay in the
 * driver until kdrv_ir_rx_take() copies them out, and nothing further is
 * received until kdrv_ir_rx_arm() asks for it - so the owner has all the time
 * it needs and no capture is overwritten under it.
 *
 * @param ctx The context handed to kdrv_ir_init().
 */
typedef void (*kdrv_ir_rx_cb_t)(void *ctx);

/**
 * @brief Bring the block up: listen for the C5's answers.
 *
 * Arms nothing and makes no request over the link, so it never waits on the
 * C5; the caller arms the receiver with kdrv_ir_rx_arm() when it wants to
 * listen. The RMT channels themselves are the C5's, opened by its firmware
 * the first time this side asks - a microsecond a tick, the carrier applied
 * to the transmitter at KDRV_IR_CARRIER_HZ. The link is normally still down
 * when this runs, so bring-up succeeds without it and every later call
 * answers KODE_DENIED until it is up. Calling it twice is harmless.
 *
 * @param cb  Called on the link's thread when a capture is ready - it may
 *            post and must return; may be NULL, in which case captures are
 *            collected but nobody is told.
 * @param ctx Handed back to @p cb untouched.
 * @return KODE_OK, or KODE_ERROR in a firmware with no link (KODEOS_RADIO).
 */
kode_status_t kdrv_ir_init(kdrv_ir_rx_cb_t cb, void *ctx);

/**
 * @brief Read the receiver's output level at rest, as the C5 sees it.
 *
 * The TSOP75338 is active low, so a working receiver rests HIGH and pulls low
 * while it hears a carrier. Its output is the C5's pin (KBSP_C5_IO_IR_RX),
 * not one of ours, so this is a request over the link and a wait for the
 * C5's answer: it BLOCKS for up to the reply timeout, and a board whose link
 * is down answers at once with a refusal.
 *
 * What a high reading proves is that nothing is holding the line down - a
 * receiver stuck low, which is also a receiver that can never signal a mark.
 * What it does not prove is that a receiver is fitted at all: a pin with
 * nothing on it reads high the same way. That question belongs to
 * kdrv_ir_rx_present().
 *
 * @param[out] high true when the line rests high. Written only on KODE_OK.
 * @return KODE_OK when the C5 read the line; KODE_DENIED for a NULL argument,
 *         before kdrv_ir_init(), or with no link to ask on; KODE_TIMEOUT when
 *         the C5 did not answer (a slave without the infrared service);
 *         KODE_ERROR when it answered that it could not read the line.
 */
kode_status_t kdrv_ir_rx_idle(bool *high);

/**
 * @brief Judge whether a receiver is fitted, by asking the C5 to discharge
 *        the line and watch it come back.
 *
 * A working TSOP75338 and a bare pin both rest high, so no level tells them
 * apart. What does is how the line recovers after being pulled down: a fitted
 * receiver restores it through its own pull-up within microseconds, and a pin
 * with nothing on it is still low a millisecond later. The pin is the C5's,
 * so the probe runs in the C5's infrared service, and how it drives the line
 * - and what it costs a capture the C5 has in progress - is that firmware's
 * to state. From here it is one request and one answer, the same shape and
 * the same wait as kdrv_ir_rx_idle().
 *
 * @param[out] present true when something holds the line up. Written only on
 *                     KODE_OK.
 * @return As kdrv_ir_rx_idle().
 */
kode_status_t kdrv_ir_rx_present(bool *present);

/**
 * @brief Transmit @p n symbols with a carrier at @p carrier_hz.
 *
 * Returns as soon as the request is on the link: the C5's RMT drives the
 * emitters and the C5 reports the end as an event. Never blocks for the frame.
 *
 * The symbols travel in one request to the C5, and its transmitter holds one
 * frame at a time, so only one transmission may be in
 * flight: a second call before the first completes is refused rather than
 * allowed to overwrite it.
 * The busy flag is checked and then set, with no lock: one task at a time may call this.
 *
 * No duration reaches the wire as zero: the RMT reads a zero duration as its
 * own end marker rather than as a zero-length pulse. A zero space - which is
 * how a codec says the frame ends here - is sent as the longest the peripheral
 * can express, which is also the gap a receiver needs to see the frame end; a
 * zero mark, which means nothing, becomes one microsecond. Anything longer
 * than one RMT duration field holds is clamped to it.
 *
 * @param sym        The symbols, mark first.
 * @param n          How many, at most KDRV_IR_TX_MAX_SYMBOLS.
 * @param carrier_hz Carrier frequency; 38000 for everything this board's own
 *                   receiver can hear.
 * @return KODE_OK when queued, KODE_DENIED for a bad argument, for a
 *         transmission still in flight, or before kdrv_ir_init().
 */
kode_status_t kdrv_ir_send(const kdrv_ir_symbol_t *sym, size_t n,
                           uint32_t carrier_hz);

/**
 * @brief Copy the capture that is waiting into @p out.
 *
 * A capture longer than @p cap is truncated, never wrapped: half a frame that
 * looks like a whole one is worse than a frame that is missing.
 *
 * @param out The caller's buffer.
 * @param cap How many symbols it holds.
 * @return How many symbols were copied; 0 when nothing is waiting.
 */
size_t kdrv_ir_rx_take(kdrv_ir_symbol_t *out, size_t cap);

/**
 * @brief Ask for the next capture.
 *
 * The receiver takes one capture per arming, so this is what the owner calls
 * to begin with and after it has read the last one. It is a request over the
 * link, so a caller on an Active Object's step makes it from a worker.
 *
 * Arming a receiver that is already armed is refused with KODE_ERROR: the
 * peripheral has a capture in flight and will not take a second job over it.
 * The owner arms after kdrv_ir_rx_take(), not on a timer.
 *
 * @return KODE_OK when the receiver is listening, KODE_DENIED before
 *         kdrv_ir_init(), KODE_ERROR when the peripheral refused.
 */
kode_status_t kdrv_ir_rx_arm(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_IR_H_ */
