/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knec.h
 * @brief The NEC codec, in symbols.
 */
#ifndef KNEC_H_
#define KNEC_H_

#include "kdrv_ir.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Leader, 32 bits, and the stop mark. */
#define KNEC_SYMBOLS            34U

/**
 * @brief How much longer a mark comes back than it was sent, in microseconds.
 *
 * The TSOP75338's AGC holds its output low past the end of a burst, so every
 * received mark is longer and every space shorter by about this much. A
 * recording is never corrected - a recording should stay a recording - so the
 * decoder's window is widened asymmetrically instead.
 *
 * The figure is sized for other people's remotes rather than for this board's
 * own echo, which is the smaller of the two problems: the Dot's emitter into
 * the Dot's receiver widens a mark by 11 to 38 microseconds, while a
 * third-party transmitter varies far more and is what the decoder must also
 * take. Being generous costs no ambiguity here - at 200 us a bit mark's upper
 * bound is 900 us, against the 1690 us of a '1' space.
 */
#define KNEC_MARK_EXCESS_US       200U

/**
 * @brief Encode one NEC frame.
 *
 * Address and command are written LSB first, sixteen bits each and no
 * inversion, so any pair of values round-trips - including 0x0000 and 0xFFFF,
 * which a standard-NEC inversion check would reject.
 *
 * @param addr     The address.
 * @param cmd      The command.
 * @param[out] out Where to write the symbols.
 * @param cap      How many @p out holds.
 * @param[out] n   How many were written.
 * @return KODE_OK, or KODE_DENIED when @p cap is under KNEC_SYMBOLS or a
 *         pointer is NULL.
 */
kode_status_t knec_encode(uint16_t addr, uint16_t cmd,
                          kdrv_ir_symbol_t *out, size_t cap, size_t *n);

/**
 * @brief Decode @p n symbols as NEC.
 *
 * Marks are accepted up to KNEC_MARK_EXCESS_US beyond their window and spaces
 * that far below theirs, which is the AGC's distortion and not tolerance for
 * a sloppy transmitter.
 *
 * @param sym       The symbols, mark first.
 * @param n         How many.
 * @param[out] addr The address, written only on success.
 * @param[out] cmd  The command, written only on success.
 * @return true when the frame decoded.
 */
bool knec_decode(const kdrv_ir_symbol_t *sym, size_t n,
                 uint16_t *addr, uint16_t *cmd);

#ifdef __cplusplus
}
#endif

#endif /* KNEC_H_ */
