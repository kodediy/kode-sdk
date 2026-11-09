/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_wave.h
 * @brief The LF capture's arithmetic: a window's stats and its peak-per-bucket decimation.
 */
#ifndef KRFID_WAVE_H_
#define KRFID_WAVE_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One capture window's statistics. All zero when it held nothing. */
typedef struct {
    uint16_t min;   /**< Lowest ADC count in the window.                   */
    uint16_t mean;  /**< Integer mean of the counts.                       */
    uint16_t peak;  /**< Highest ADC count in the window.                  */
    uint16_t ac;    /**< Mean absolute deviation from the mean: modulation. */
    uint16_t n;     /**< Samples the window held before decimation.        */
} krfid_field_t;

/**
 * @brief Describe a window of 12-bit samples.
 *
 * The mean is the integer quotient of the sum by the count, and @c ac is the
 * integer mean of |sample - mean| against that integer mean. Both quotients
 * truncate. At most UINT16_MAX samples are taken, the first ones, so the count
 * fits @c n exactly and neither sum can overflow its 32 bits whatever the
 * sample values; a window from kdrv_rfid_lf never comes near that.
 *
 * @param s   The samples. May be NULL only when @p n is 0.
 * @param n   How many samples @p s holds; 0 writes an all-zero answer.
 * @param out Where the stats go. NULL is ignored.
 */
void krfid_wave_stats(const uint16_t *s, size_t n, krfid_field_t *out);

/**
 * @brief Squeeze @p n samples into at most @p cap points, peak per bucket.
 *
 * When the window fits (@p n <= @p cap) every sample is copied as it is.
 * Otherwise the window is cut into @p cap buckets, bucket p covering the
 * samples [p * n / cap, (p + 1) * n / cap), and each point is the largest
 * sample in its bucket. The buckets are uneven by one sample at most, so the
 * points stay evenly spaced in time and the last one is the peak of the
 * window's tail. Nothing past the returned count is written.
 *
 * @param s    The samples. NULL writes nothing and returns 0.
 * @param n    How many samples @p s holds.
 * @param wave Out: the points. NULL writes nothing and returns 0.
 * @param cap  Points @p wave holds; 0 writes nothing and returns 0.
 * @return The number of points written, min(@p n, @p cap), or 0 as above.
 */
size_t krfid_wave_decimate(const uint16_t *s, size_t n, uint16_t *wave,
                           size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* KRFID_WAVE_H_ */
