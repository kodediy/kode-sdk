/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ktone.c
 * @brief The tone and the detector, proved without a board.
 */
#include <math.h>
#include <string.h>

#include "unity.h"

#include "ktone.h"

/** @brief One window's worth of samples, reused by every case. */
static int16_t s_buf[KTONE_WIN_N];
static ktone_det_t     s_det;
static ktone_measure_t s_m;

/**
 * @brief Fill the buffer with candidate @p i and measure a whole window.
 *
 * @param i   Which candidate.
 * @param amp Peak amplitude.
 */
static void measure_tone(uint8_t i, int16_t amp)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, ktone_fill(i, 0U, amp, s_buf, KTONE_WIN_N));
    ktone_reset(&s_det);
    ktone_feed(&s_det, s_buf, KTONE_WIN_N);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ktone_result(&s_det, &s_m));
}

/** @brief Fill the buffer with an arbitrary frequency, in double precision. */
static void fill_hz(double hz, int16_t amp)
{
    size_t n;

    for ( n = 0U; (size_t)KTONE_WIN_N > n; ++n )
    {
        double const ph = (2.0 * M_PI * hz * (double)n) / (double)KTONE_RATE_HZ;
        s_buf[n] = (int16_t)lrint((double)amp * sin(ph));
    }
}

TEST_CASE("a full-scale tone at each candidate reads 0 dBFS at that bin",
          "[ktone]")
{
    for ( uint8_t i = 0U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        measure_tone(i, 32767);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(i, s_m.peak_bin,
                                       "the loudest bin is the one emitted");
        TEST_ASSERT_INT_WITHIN_MESSAGE(10, 0, s_m.bin_x10[i],
                                       "a full-scale sine is the reference");
    }
}

TEST_CASE("and leaks at least 60 dB down into every other candidate",
          "[ktone]")
{
    /* The leakage-free claim, and the case that would catch a window length
     * or a bin index that is not an exact multiple of the rate over it. */
    for ( uint8_t i = 0U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        measure_tone(i, 32767);
        TEST_ASSERT_TRUE_MESSAGE(
            (s_m.bin_x10[i] - ktone_other_max(&s_m, i)) >= 600,
            "a tone reached a bin it was not emitted at");
    }
}

TEST_CASE("the second harmonic of any candidate lights none of them",
          "[ktone]")
{
    /* The one-octave property, asserted rather than argued: twice the lowest
     * is above the highest, so distortion cannot flatter a neighbour. */
    for ( uint8_t i = 0U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        fill_hz(2.0 * (double)ktone_bin_hz(i), 32000);
        ktone_reset(&s_det);
        ktone_feed(&s_det, s_buf, KTONE_WIN_N);
        TEST_ASSERT_EQUAL_INT(KODE_OK, ktone_result(&s_det, &s_m));
        for ( uint8_t b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
        {
            TEST_ASSERT_TRUE_MESSAGE(-350 > s_m.bin_x10[b],
                                     "a harmonic lit a candidate");
        }
    }
}

TEST_CASE("the candidates are one increasing set inside a single octave",
          "[ktone]")
{
    uint16_t const lo = ktone_bin_hz(0U);
    uint16_t const hi = ktone_bin_hz((uint8_t)KTONE_BIN_N - 1U);

    TEST_ASSERT_TRUE_MESSAGE((2U * lo) > hi, "the set spans over an octave");
    TEST_ASSERT_TRUE_MESSAGE(400U <= lo, "below the output filter's corner");
    TEST_ASSERT_TRUE_MESSAGE(3400U >= hi, "past the speaker's tested band");
    for ( uint8_t i = 1U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        TEST_ASSERT_TRUE(ktone_bin_hz(i) > ktone_bin_hz(i - 1U));
    }
    TEST_ASSERT_EQUAL_UINT(0U, ktone_bin_hz((uint8_t)KTONE_BIN_N));
}

TEST_CASE("the generator's phase does not drift, however long it runs",
          "[ktone]")
{
    /* An integer index modulo the window, not a float accumulator: one part
     * in a hundred thousand of drift is enough to smear the bin the detector
     * is looking for. */
    for ( uint8_t i = 0U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        for ( uint32_t k = 0U; 40U > k; ++k )
        {
            uint32_t const n = k * 7919U;
            int16_t const  v = ktone_sample(i, n, KTONE_TONE_AMP_Q15);
            TEST_ASSERT_EQUAL_INT16(
                v, ktone_sample(i, n + KTONE_WIN_N, KTONE_TONE_AMP_Q15));
            TEST_ASSERT_EQUAL_INT16(
                v, ktone_sample(i, n + (1000U * KTONE_WIN_N),
                                KTONE_TONE_AMP_Q15));
        }
    }
}

TEST_CASE("the row's own amplitude reads -12.0 dBFS", "[ktone]")
{
    /* A power of two, so the figure is exact rather than rounded - and it is
     * the number the bench's 22 mW was computed from. */
    measure_tone(0U, (int16_t)KTONE_TONE_AMP_Q15);
    TEST_ASSERT_INT_WITHIN_MESSAGE(3, -120, s_m.bin_x10[0],
                                   "the emitted level is not what it says");
}

TEST_CASE("the nearest candidate is what a frequency snaps to", "[ktone]")
{
    TEST_ASSERT_EQUAL_UINT(0U, ktone_bin_nearest(0U));
    TEST_ASSERT_EQUAL_UINT((uint8_t)KTONE_BIN_N - 1U,
                           ktone_bin_nearest(20000U));
    for ( uint8_t i = 0U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT(i, ktone_bin_nearest(ktone_bin_hz(i)));
    }
}

TEST_CASE("silence and a railed input are both told apart from a signal",
          "[ktone]")
{
    (void)memset(s_buf, 0, sizeof(s_buf));
    ktone_reset(&s_det);
    ktone_feed(&s_det, s_buf, KTONE_WIN_N);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ktone_result(&s_det, &s_m));
    TEST_ASSERT_TRUE_MESSAGE(s_m.stuck, "an all-zero window is not moving");
    TEST_ASSERT_EQUAL_UINT(0U, s_m.clipped);
    TEST_ASSERT_EQUAL_INT(KTONE_FLOOR_STUCK, ktone_floor_judge(&s_m));

    for ( size_t n = 0U; (size_t)KTONE_WIN_N > n; ++n )
    {
        s_buf[n] = 32767;
    }
    ktone_reset(&s_det);
    ktone_feed(&s_det, s_buf, KTONE_WIN_N);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ktone_result(&s_det, &s_m));
    TEST_ASSERT_TRUE(s_m.stuck);
    TEST_ASSERT_EQUAL_UINT(KTONE_WIN_N, s_m.clipped);

    measure_tone(3U, 16384);
    TEST_ASSERT_FALSE_MESSAGE(s_m.stuck, "a sine is not a stuck sample");
    TEST_ASSERT_INT_WITHIN_MESSAGE(20, -60, s_m.peak_x10,
                                   "half of full scale is six decibels down");
}

TEST_CASE("a window that was never fed leaves its measurement untouched",
          "[ktone]")
{
    /* A failed measurement must never become a plausible reading of a
     * microphone, which is why the caller's struct is left alone. */
    ktone_measure_t m;
    (void)memset(&m, 0x5A, sizeof(m));
    ktone_reset(&s_det);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ktone_result(&s_det, &m));
    TEST_ASSERT_EQUAL_INT16(0x5A5A, m.rms_x10);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, ktone_result(NULL, &m));
}

TEST_CASE("the floor judges only what needs no calibration", "[ktone]")
{
    ktone_measure_t m;

    (void)memset(&m, 0, sizeof(m));
    m.rms_x10 = -600;
    TEST_ASSERT_EQUAL_INT(KTONE_FLOOR_OK, ktone_floor_judge(&m));

    m.clipped = 1U;
    TEST_ASSERT_EQUAL_INT(KTONE_FLOOR_CLIPPED, ktone_floor_judge(&m));
    m.clipped = 0U;

    /* A bench too loud is a refusal to judge, not a verdict on the board. */
    m.rms_x10 = (int16_t)(KTONE_FLOOR_MAX_DBFS_X10 + 1);
    TEST_ASSERT_EQUAL_INT(KTONE_FLOOR_LOUD, ktone_floor_judge(&m));

    /* A measurement that is not there is reported as the worst of the three,
     * so a caller cannot read a missing window as a quiet one. */
    TEST_ASSERT_EQUAL_INT(KTONE_FLOOR_STUCK, ktone_floor_judge(NULL));
    TEST_ASSERT_NOT_NULL(ktone_floor_reason(KTONE_FLOOR_LOUD));
}

TEST_CASE("a window passes only when all three tests pass", "[ktone]")
{
    int16_t const floor = -800;

    measure_tone(5U, (int16_t)KTONE_TONE_AMP_Q15);
    TEST_ASSERT_TRUE_MESSAGE(ktone_window_ok(&s_m, 5U, floor),
                             "a clean tone over a quiet floor is a pass");

    TEST_ASSERT_FALSE_MESSAGE(ktone_window_ok(&s_m, 4U, floor),
                              "a tone at another bin is not this one's");
    TEST_ASSERT_FALSE_MESSAGE(
        ktone_window_ok(&s_m, 5U, s_m.bin_x10[5]),
        "a tone level with its own floor stands over nothing");
    TEST_ASSERT_FALSE(ktone_window_ok(NULL, 5U, floor));
    TEST_ASSERT_FALSE(ktone_window_ok(&s_m, (uint8_t)KTONE_BIN_N, floor));
}

TEST_CASE("noise alone does not clear the margin over the other candidates",
          "[ktone]")
{
    /* Deterministic rather than random: a suite that fails once in a hundred
     * runs is worse than no suite. */
    uint32_t seed = 12345U;

    for ( size_t n = 0U; (size_t)KTONE_WIN_N > n; ++n )
    {
        seed = (seed * 1103515245U) + 12345U;
        s_buf[n] = (int16_t)((int32_t)((seed >> 16) & 0x3FFU) - 512);
    }
    ktone_reset(&s_det);
    ktone_feed(&s_det, s_buf, KTONE_WIN_N);
    TEST_ASSERT_EQUAL_INT(KODE_OK, ktone_result(&s_det, &s_m));

    for ( uint8_t i = 0U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        TEST_ASSERT_FALSE_MESSAGE(
            ktone_window_ok(&s_m, i, (int16_t)KTONE_SILENCE_DBFS_X10),
            "noise was taken for a tone");
    }
}
