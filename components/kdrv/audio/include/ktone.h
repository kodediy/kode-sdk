/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ktone.h
 * @brief The tone the speaker emits and the detector that finds it again.
 */
#ifndef KTONE_H_
#define KTONE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kgoertzel.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The format, which is the board's and not a parameter ---------------- */

/**
 * @brief The one sample rate this board runs.
 *
 * Measured, not chosen. At 16 kHz the reconstruction image at 15 kHz comes
 * back at -3.8 dBc and is plainly audible; at 48 kHz every image is at or
 * below -52 dBc. kdrv_es8311.c asserts that its own rate agrees with this.
 */
#define KTONE_RATE_HZ           48000U

/** @brief One analysis window: 4096 frames, 85.3 ms, 11.7 Hz of noise
 *         bandwidth. Long enough to reject a noisy bench by 26 dB more than
 *         a quarter of it would, and short enough that a row of six windows
 *         still fits comfortably inside its budget. */
#define KTONE_WIN_N              4096U

/** @brief One I2S transfer: 256 frames, 5.33 ms. Sixteen of them make exactly
 *         one window, so a window never straddles a partial block. */
#define KTONE_BLOCK_FRAMES        256U

/** @brief How many frequencies a tone may be drawn from. */
#define KTONE_BIN_N                 8U

/**
 * @brief The lowest tone this board can honestly make.
 *
 * The output filter's corner is near 390 Hz and the speaker cannot radiate
 * below it, so a tone down there is heat and nothing else. Whoever takes a
 * frequency from a person refuses under this rather than snapping upward: a
 * caller asking for 100 Hz wants 100 Hz, and quietly playing them something
 * else would be worse than saying no.
 */
#define KTONE_HZ_MIN              400U
/** @brief The highest. Where the speaker manufacturer's own test stops. */
#define KTONE_HZ_MAX             3400U

/* ---- The tone ------------------------------------------------------------ */

/**
 * @brief The peak amplitude the production row emits, out of 32768.
 *
 * 8192 is -12.04 dBFS exactly - a power of two, so the figure is exact rather
 * than rounded. The bench measured 352 mW at full scale with this board's
 * final gain network, so this is about 22 mW: audible across a production
 * bay, thirteen decibels below the hardware's own thermal ceiling, and far
 * enough from clipping that the detector sees a clean tone rather than one
 * whose second harmonic it has to reason about.
 *
 * It has a floor as well as a ceiling, and the floor is the one that is not
 * yet measured: the tone must be loud enough that blocking the acoustic path
 * drops the target bin below KTONE_SNR_DB_X10. Until the bench has run that
 * check, a pass could in principle be electrical crosstalk rather than sound.
 */
#define KTONE_TONE_AMP_Q15       8192

/* ---- What the detector refuses to be fooled by --------------------------- */

/**
 * @brief A sample at or past this is a clipped input, not a noise floor.
 *
 * Deliberately just short of full scale: an ADC pinned to a rail reports
 * 32767, and so does one whose input stage is saturated, and neither is a
 * measurement of anything.
 */
#define KTONE_CLIP_LSB          32700

/**
 * @brief Above this broadband level the row refuses to judge at all.
 *
 * A REFUSAL GATE, not a margin, and the distinction is what sets the number.
 * Step 2's two tests are measured against step 1's floor AT EACH BIN, of that
 * same run - so a louder room raises its own bar and cannot hand out a pass
 * it did not earn. What this ceiling protects against is narrower: a room so
 * loud that the capture is no longer a measurement of anything, where saying
 * "the board is bad" would be a lie about the board.
 *
 * Set for a production floor rather than a quiet bench, deliberately. A row
 * that refuses to judge is a board that does not ship, and a factory is not
 * quiet; the per-bin margins are what keep that honest rather than this.
 *
 * A GUESS all the same, in the sense that no bay has been recorded yet. The
 * bench moves it if a real one reads above it.
 *
 * There is deliberately NO matching KTONE_FLOOR_MIN. Judging a microphone by
 * how quiet it is needs a threshold above the ES8311 ADC's own electrical
 * floor and below a real bench's ambient, and separating those two needs a
 * board with the microphone depopulated. Until one has been measured, this
 * file judges only what needs no calibration.
 */
#define KTONE_FLOOR_MAX_DBFS_X10 (-200)

/**
 * @brief How far the target bin must stand over the loudest of the others.
 *
 * A GUESS; expect 30 to 50 dB in practice. With exact-bin tones in a
 * rectangular window the ideal ratio is unbounded, so this is slack for room
 * modes, the microphone's response tilt and a bench that is not silent - and
 * it is still far more than any accidental match could produce.
 */
#define KTONE_BIN_MARGIN_DB_X10    120

/**
 * @brief How far the target bin must stand over the floor AT THAT SAME BIN.
 *
 * A GUESS. Per-bin rather than broadband on purpose: a bench humming at one
 * of these frequencies then raises its own bar instead of lowering ours.
 */
#define KTONE_SNR_DB_X10           200

/** @brief What a bin with no energy at all reports, rather than an infinity. */
#define KTONE_SILENCE_DBFS_X10   (-1200)

/* ---- Types --------------------------------------------------------------- */

/**
 * @brief The detector's running state. Opaque in practice; reset before use.
 *
 * All eight bins advance together over one pass of the samples, and the
 * broadband figures come out of the same pass. Single-precision float,
 * because the P4 has an FPU and fixed point would buy nothing here but a
 * class of overflow reasoning.
 */
typedef struct {
    kgoertzel_t bin[KTONE_BIN_N]; /**< One bin per candidate, each pointed at
                                   *   its frequency at reset. Kept here
                                   *   rather than in a file static so this
                                   *   module has no mutable global and the
                                   *   host suite can run two detectors in
                                   *   one case without them interfering.   */
    uint64_t sumsq;            /**< Broadband energy. Integer, so exact.     */
    uint32_t count;            /**< Samples fed.                             */
    uint16_t clipped;          /**< Samples at or past KTONE_CLIP_LSB.       */
    int16_t  peak;             /**< Largest magnitude seen, clamped.         */
    int16_t  first;            /**< The first sample, for the stuck test.    */
    bool     varied;           /**< Whether any sample differed from it.     */
} ktone_det_t;

/**
 * @brief What one window amounted to. Every level is tenth-dBFS.
 *
 * A bin is referred to a full-scale sine and the RMS to full scale, so one clean
 * tone reads 3 dB lower as RMS than in its bin.
 */
typedef struct {
    int16_t  bin_x10[KTONE_BIN_N]; /**< Each candidate's level.              */
    int16_t  rms_x10;              /**< Broadband RMS over the same samples. */
    int16_t  peak_x10;             /**< The largest sample.                  */
    uint16_t clipped;              /**< How many reached full scale.         */
    uint8_t  peak_bin;             /**< Which candidate came out loudest.    */
    bool     stuck;                /**< Every sample identical: a dead bus
                                    *   reads exactly this, and so does an
                                    *   ADC pinned to a rail.                */
} ktone_measure_t;

/** @brief Why step 1 refused, or that it did not. */
typedef enum {
    KTONE_FLOOR_OK = 0,   /**< Nothing indisputable is wrong.                */
    KTONE_FLOOR_STUCK,    /**< Every sample identical: no ADC, or no bus.    */
    KTONE_FLOOR_CLIPPED,  /**< The input is railed rather than listening.    */
    KTONE_FLOOR_LOUD,     /**< The bench is too loud to judge in.            */
} ktone_floor_t;

/* ---- The tone ------------------------------------------------------------ */

/**
 * @brief The frequency of candidate @p i, in whole hertz, for logs and records.
 *
 * @param i Which candidate, 0 to KTONE_BIN_N - 1.
 * @return Its frequency rounded to the nearest hertz, or 0 when @p i is out
 *         of range.
 */
uint16_t ktone_bin_hz(uint8_t i);

/**
 * @brief The candidate nearest @p hz.
 *
 * What the console's tone verb snaps to. The snapped value is what callers
 * report back rather than what was asked for: a bench that types 1500 and
 * measures 1535 on a scope must not be left thinking the firmware lied.
 *
 * @param hz The frequency asked for.
 * @return The index of the nearest candidate. Never out of range.
 */
uint8_t ktone_bin_nearest(uint16_t hz);

/**
 * @brief One sample of candidate @p i's tone at frame index @p n.
 *
 * The phase is an integer index modulo KTONE_WIN_N and never a float
 * accumulator. Because each candidate completes a whole number of cycles in
 * one window, that index is exact for as long as the tone runs; a float
 * accumulator drifts, and one part in a hundred thousand is enough to smear
 * the bin the detector is looking for.
 *
 * @param i   Which candidate.
 * @param n   The frame index, counting from the start of the tone.
 * @param amp Peak amplitude, out of 32768.
 * @return The sample, or 0 when @p i is out of range.
 */
int16_t ktone_sample(uint8_t i, uint32_t n, int16_t amp);

/**
 * @brief Fill @p out with @p frames samples of candidate @p i's tone.
 *
 * @param i      Which candidate.
 * @param n0     The frame index the first sample carries.
 * @param amp    Peak amplitude, out of 32768.
 * @param out    Where to write, mono.
 * @param frames How many.
 * @return KODE_OK, or KODE_DENIED for a NULL @p out or an @p i out of range.
 */
kode_status_t ktone_fill(uint8_t i, uint32_t n0, int16_t amp,
                         int16_t *out, size_t frames);

/* ---- The detector -------------------------------------------------------- */

/**
 * @brief Start a window. Must be called before the first ktone_feed().
 *
 * @param det The detector to reset. NULL is a no-op.
 */
void ktone_reset(ktone_det_t *det);

/**
 * @brief Advance every bin and every broadband figure over @p n samples.
 *
 * May be called any number of times; the window is however many samples were
 * fed between a reset and a result. Feeding exactly KTONE_WIN_N is what makes
 * the leakage-free claim true.
 *
 * @param det  The detector.
 * @param mono The samples, one channel.
 * @param n    How many.
 */
void ktone_feed(ktone_det_t *det, const int16_t *mono, size_t n);

/**
 * @brief Read the window out.
 *
 * @param det The detector, unchanged by this call.
 * @param out Where to write. Left untouched when @p det is empty or either
 *            pointer is NULL, so a failed measurement never becomes a
 *            plausible one.
 * @return KODE_OK, or KODE_DENIED.
 */
kode_status_t ktone_result(const ktone_det_t *det, ktone_measure_t *out);

/* ---- The judgements ------------------------------------------------------ */

/**
 * @brief Judge a window that was captured while nothing was emitted.
 *
 * Only what needs no calibration; see KTONE_FLOOR_MAX_DBFS_X10 for what is
 * deliberately not judged here and why.
 *
 * @param m The window.
 * @return Why it refused, or KTONE_FLOOR_OK.
 */
ktone_floor_t ktone_floor_judge(const ktone_measure_t *m);

/**
 * @brief What to call a floor verdict in a log line.
 *
 * @param f The verdict.
 * @return A short lower-case phrase. Never NULL.
 */
const char *ktone_floor_reason(ktone_floor_t f);

/**
 * @brief The loudest candidate other than @p bin.
 *
 * @param m   The window.
 * @param bin The candidate to exclude.
 * @return That level in tenth-dBFS, or KTONE_SILENCE_DBFS_X10.
 */
int16_t ktone_other_max(const ktone_measure_t *m, uint8_t bin);

/**
 * @brief Did the tone at @p bin come back?
 *
 * All three of these must hold: the bin stands KTONE_SNR_DB_X10 above
 * @p floor_x10, it stands KTONE_BIN_MARGIN_DB_X10 above the loudest of the
 * others, and it is the loudest. The third is redundant given the second and
 * is checked anyway, because it is the sentence a log prints and an operator
 * reads.
 *
 * @param m         The window.
 * @param bin       Which candidate was emitted.
 * @param floor_x10 What that same bin read when nothing was emitted.
 * @return true when the tone came back.
 */
bool ktone_window_ok(const ktone_measure_t *m, uint8_t bin, int16_t floor_x10);

#ifdef __cplusplus
}
#endif

#endif /* KTONE_H_ */
