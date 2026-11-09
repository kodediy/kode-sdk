/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_krfid_wave.c
 * @brief krfid_wave: the LF capture's stats and its peak-per-bucket decimation.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "krfid_wave.h"

#define RAMP_N   1600U   /* a 20 ms capture at 80 kS/s               */
#define WAVE_CAP 200U    /* the points the scope's wave carries      */

static uint16_t ramp[RAMP_N];
static uint16_t wave[WAVE_CAP];

TEST_CASE("stats give min, mean, peak and the mean absolute deviation",
          "[krfid_wave]")
{
    static const uint16_t s[3] = { 100U, 300U, 200U };
    krfid_field_t f;

    (void)memset(&f, 0xA5, sizeof(f));
    krfid_wave_stats(s, 3U, &f);

    TEST_ASSERT_EQUAL_UINT16(100U, f.min);
    TEST_ASSERT_EQUAL_UINT16(200U, f.mean);
    TEST_ASSERT_EQUAL_UINT16(300U, f.peak);
    TEST_ASSERT_EQUAL_UINT16(66U, f.ac);    /* (100 + 100 + 0) / 3, truncated */
    TEST_ASSERT_EQUAL_UINT16(3U, f.n);
}

TEST_CASE("stats of an empty window are all zero", "[krfid_wave]")
{
    static const uint16_t s[1] = { 4095U };
    krfid_field_t f;

    (void)memset(&f, 0xA5, sizeof(f));
    krfid_wave_stats(s, 0U, &f);

    TEST_ASSERT_EQUAL_UINT16(0U, f.min);
    TEST_ASSERT_EQUAL_UINT16(0U, f.mean);
    TEST_ASSERT_EQUAL_UINT16(0U, f.peak);
    TEST_ASSERT_EQUAL_UINT16(0U, f.ac);
    TEST_ASSERT_EQUAL_UINT16(0U, f.n);
}

TEST_CASE("decimation keeps each bucket's peak, the buckets proportional",
          "[krfid_wave]")
{
    static const uint16_t s[10] = { 0U, 5U, 1U, 9U, 2U, 3U, 7U, 4U, 8U, 6U };
    /* Bucket p is [p * 10 / 4, (p + 1) * 10 / 4): [0,2) [2,5) [5,7) [7,10),
     * so {0,5} {1,9,2} {3,7} {4,8,6}. */
    static const uint16_t want[4] = { 5U, 9U, 7U, 8U };
    uint16_t out[4];
    size_t n;

    (void)memset(out, 0, sizeof(out));
    n = krfid_wave_decimate(s, 10U, out, 4U);

    TEST_ASSERT_EQUAL_size_t(4U, n);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want, out, 4U);
}

TEST_CASE("a window smaller than the room is copied point for point",
          "[krfid_wave]")
{
    static const uint16_t s[3] = { 7U, 4095U, 0U };
    uint16_t out[8];
    size_t n;

    (void)memset(out, 0, sizeof(out));
    n = krfid_wave_decimate(s, 3U, out, 8U);

    TEST_ASSERT_EQUAL_size_t(3U, n);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(s, out, 3U);
    TEST_ASSERT_EQUAL_UINT16(0U, out[3]);   /* nothing past the count */
}

TEST_CASE("no room writes no points", "[krfid_wave]")
{
    static const uint16_t s[3] = { 1U, 2U, 3U };
    uint16_t out[1] = { 0xBEEFU };

    TEST_ASSERT_EQUAL_size_t(0U, krfid_wave_decimate(s, 3U, out, 0U));
    TEST_ASSERT_EQUAL_UINT16(0xBEEFU, out[0]);
    TEST_ASSERT_EQUAL_size_t(0U, krfid_wave_decimate(s, 3U, NULL, 4U));
}

TEST_CASE("a 1600-sample ramp decimates to 200 points ending on its last",
          "[krfid_wave]")
{
    size_t i;
    size_t n;

    for ( i = 0U; RAMP_N > i; i++ )
    {
        ramp[i] = (uint16_t)i;
    }
    (void)memset(wave, 0, sizeof(wave));
    n = krfid_wave_decimate(ramp, RAMP_N, wave, WAVE_CAP);

    TEST_ASSERT_EQUAL_size_t(WAVE_CAP, n);
    TEST_ASSERT_EQUAL_UINT16(7U, wave[0]);          /* the first bucket's peak */
    TEST_ASSERT_EQUAL_UINT16(1599U, wave[WAVE_CAP - 1U]);
}

TEST_CASE("a 399-sample ramp into 200 keeps every bucket to two samples",
          "[krfid_wave]")
{
    size_t i;
    size_t n;

    for ( i = 0U; 399U > i; i++ )
    {
        ramp[i] = (uint16_t)i;
    }
    (void)memset(wave, 0, sizeof(wave));
    n = krfid_wave_decimate(ramp, 399U, wave, WAVE_CAP);

    TEST_ASSERT_EQUAL_size_t(WAVE_CAP, n);
    /* On a ramp each point is its bucket's last index, so the step between two
     * points is the next bucket's width: one or two samples, never the tail
     * of the window squeezed into the last point. */
    TEST_ASSERT_EQUAL_UINT16(0U, wave[0]);           /* bucket [0, 1) */
    for ( i = 1U; WAVE_CAP > i; i++ )
    {
        const uint32_t width = (uint32_t)wave[i] - (uint32_t)wave[i - 1U];

        TEST_ASSERT_TRUE((1U <= width) && (2U >= width));
    }
    TEST_ASSERT_EQUAL_UINT16(396U, wave[WAVE_CAP - 2U]);
    TEST_ASSERT_EQUAL_UINT16(398U, wave[WAVE_CAP - 1U]);
}
