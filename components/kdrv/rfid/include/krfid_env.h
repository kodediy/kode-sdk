/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_env.h
 * @brief The 125 kHz envelope, folded out of the comparator's pulse widths.
 */
#ifndef KRFID_ENV_H_
#define KRFID_ENV_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What one tick of high time is worth in the envelope.
 *
 * A full period at 20 MHz (160 ticks) folds to 5120 and a half-period pulse to
 * 2560: a range like a 12-bit ADC's, which is what keeps the codec's constants
 * meaningful - its flat gate of 120 counts is ~3.75 ticks of width swing.
 */
#define KRFID_ENV_SCALE 32U

/** @brief Sweep/health stats of one folded window. */
typedef struct {
    int      bins;        /**< Samples written to the envelope.                  */
    int      bins_pulsed; /**< Bins that received at least one level-1 run.      */
    uint32_t w_min;       /**< Least summed high time of a pulsed bin, in ticks. */
    uint32_t w_mean;      /**< Mean summed high time over the pulsed bins.       */
    uint32_t w_max;       /**< Most summed high time of a pulsed bin, in ticks.  */
} krfid_env_stats_t;

/**
 * @brief Fold an RMT symbol stream into one envelope sample per carrier period.
 *
 * Three steps. Regrid: the time of every level-1 run is added to the bin its
 * rising edge falls in, so the fragments of a chattered pulse add back up to
 * one width, and a bin no run started in stays 0 - a dropped pulse. A bin is
 * one period wide and opens a quarter period before its period does: a run
 * rising t ticks into the stream goes to bin
 * (t + @p period_ticks / 4) / @p period_ticks. A carrier coherent with the RMT
 * clock puts every rising edge within a tick of a period multiple, and a
 * half-period pulse then sits in the middle of its bin, with a quarter period
 * either side for jitter at its rising edge and chatter at its falling one.
 * A sample is one whole period of stream, whose bin has by then seen every
 * run it can get; a partial period at the end of the stream is not a sample,
 * and the fold stops at @p max bins. Box average: a causal moving average
 * over @p box periods, over fewer at the start of the window where fewer
 * exist - C142's low-pass, in software. Scale: KRFID_ENV_SCALE per tick,
 * rounded to nearest, saturating at UINT16_MAX.
 *
 * The stats describe the regrid, before the average: the pulse ratio and the
 * width spread are what the reference sweep judges.
 *
 * Each RMT word holds two runs, low half first: bits [14:0] duration0, bit 15 level0,
 * bits [30:16] duration1, bit 31 level1. A zero duration ends it; the pulse is level 1.
 *
 * @param sym          The RMT words, in the layout above.
 * @param nsym         How many words; the stream may end earlier on a zero
 *                     duration.
 * @param period_ticks Ticks per carrier period (160 at 20 MHz for 125 kHz).
 * @param box          Box-average length in periods (4 = C142 in software; 0
 *                     and 1 both mean no averaging).
 * @param out          Out: the envelope, one sample per period. Entries past
 *                     the returned count are scratch.
 * @param max          Capacity of @p out, in samples.
 * @param st           Out: the stats, always filled. May be NULL.
 * @return The number of samples written: 0 for a stream shorter than one
 *         period, a NULL pointer, a zero period, or a @p max or @p nsym under 1.
 */
int krfid_env_from_symbols(const uint32_t *sym, int nsym, uint16_t period_ticks,
                           uint8_t box, uint16_t *out, int max,
                           krfid_env_stats_t *st);

#ifdef __cplusplus
}
#endif

#endif /* KRFID_ENV_H_ */
