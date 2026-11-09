/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_em4100.h
 * @brief The 125 kHz EM4100 / EM4102 codec, in symbols.
 */
#ifndef KRFID_EM4100_H_
#define KRFID_EM4100_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the decoder saw while recovering a frame. */
typedef struct {
    int  pkpk_raw;        /**< Peak-to-peak of the window (raw ADC counts).       */
    int  halfbit_samples; /**< Detected Manchester half-bit width (samples).      */
    int  bitrate_div;     /**< Inferred RF/div (16/32/64), 0 if unknown.          */
    int  phase;           /**< Manchester pairing phase used (0/1).               */
    bool inverted;        /**< Bit polarity used.                                 */
    int  frame_start_bit; /**< Index of the header inside the recovered stream.   */
} krfid_em4100_diag_t;

/** @brief The most samples one decode pass reads; a longer window is cut here. */
#define KRFID_EM4100_MAX_SAMPLES 12000

/**
 * @brief The decoder's scratch space, one array per pipeline stage.
 *
 * It is 84 KB, so the codec does not hold one itself: the caller lends it for
 * the length of a decode and decides where it lives. Its contents mean nothing
 * between calls and need no initialising.
 */
typedef struct {
    uint8_t dig[KRFID_EM4100_MAX_SAMPLES]; /**< Digitised samples (0/1).            */
    int     run[KRFID_EM4100_MAX_SAMPLES]; /**< Run lengths.                        */
    uint8_t hb[KRFID_EM4100_MAX_SAMPLES];  /**< Reconstructed half-bit levels.      */
    uint8_t bit[KRFID_EM4100_MAX_SAMPLES]; /**< Decoded bits (0/1, 2 = violation).  */
} krfid_em4100_work_t;

/**
 * @brief Decode an EM4100 tag from demodulated ADC samples.
 *
 * @param samples   Demodulated envelope, raw ADC counts (RFID_RF_IN).
 * @param n         Number of samples; at most KRFID_EM4100_MAX_SAMPLES are read.
 * @param sample_hz Sampling rate the samples were taken at.
 * @param work      Scratch space for this pass. Must not be NULL.
 * @param[out] id   5-byte ID on success (40-bit, most-significant nibble first).
 * @param[out] diag Diagnostics, always filled even on failure. May be NULL.
 * @return 0 on success; -1 when there is no tag (the signal is too flat); -2
 *         when a tag is present but no valid frame could be recovered.
 */
int krfid_em4100_decode(const uint16_t *samples, int n, uint32_t sample_hz,
                        krfid_em4100_work_t *work,
                        uint8_t id[5], krfid_em4100_diag_t *diag);

/**
 * @brief Encode a 5-byte ID into the 64-bit EM4100 frame.
 *
 * The frame is what a T5577 is programmed with (block 1 = the top 32 bits,
 * block 2 = the low 32 bits) so a rewritable tag re-reads as the same ID.
 *
 * @param id The 5-byte ID, most-significant nibble first.
 * @return The 64-bit frame: 9 header ones, ten data-plus-row-parity rows, four
 *         column parities and a zero stop bit.
 */
uint64_t krfid_em4100_frame(const uint8_t id[5]);

#ifdef __cplusplus
}
#endif

#endif /* KRFID_EM4100_H_ */
