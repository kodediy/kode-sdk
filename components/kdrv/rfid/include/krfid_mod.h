/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_mod.h
 * @brief Manchester modulation of a frame into one load level per carrier period.
 */
#ifndef KRFID_MOD_H_
#define KRFID_MOD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Carrier periods per EM4100 bit (RF/64). */
#define KRFID_MOD_EM4100_RF       64U

/** @brief Levels of one EM4100 frame at RF/64: 64 bits of 64 periods. */
#define KRFID_MOD_EM4100_LEVELS   4096U

/**
 * @brief Modulate a frame as Manchester: a 1 loads the coil for the first
 *        half of its bit, a 0 for the second.
 *
 * @param rf    Carrier periods per bit; even.
 * @param frame The frame, first bit most significant.
 * @param nbits Its length in bits.
 * @param lv    Out: one level per carrier period, 0 or 1.
 * @param cap   How many levels @p lv holds.
 * @return The levels written, one whole frame; 0 for a NULL pointer, an empty
 *         frame, an odd or zero @p rf, or a frame that does not fit @p cap.
 */
uint32_t krfid_mod_manchester(uint8_t rf, const uint8_t *frame,
                              uint16_t nbits, uint8_t *lv, uint32_t cap);

/**
 * @brief Modulate an EM4100 tag at RF/64: krfid_em4100_frame() of the ID,
 *        then krfid_mod_manchester().
 *
 * @param id  The 5-byte ID, most-significant nibble first.
 * @param lv  Out: one level per carrier period.
 * @param cap How many levels @p lv holds.
 * @return KRFID_MOD_EM4100_LEVELS; 0 for a NULL pointer or a @p cap below it.
 */
uint32_t krfid_mod_em4100(const uint8_t id[5], uint8_t *lv, uint32_t cap);

#ifdef __cplusplus
}
#endif

#endif /* KRFID_MOD_H_ */
