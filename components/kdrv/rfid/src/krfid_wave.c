/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_wave.c
 * @brief The LF capture's stats and decimation (see krfid_wave.h). Host-testable, no IDF deps.
 */
#include "krfid_wave.h"

/**
 * @brief The largest sample in s[first, last).
 * @param s     The samples.
 * @param first The bucket's first index.
 * @param last  One past its last index; greater than @p first.
 * @return The bucket's peak.
 */
static uint16_t bucket_peak(const uint16_t *s, size_t first, size_t last)
{
    uint16_t peak = s[first];
    size_t   i;

    for ( i = first + 1U; i < last; i++ )
    {
        if ( s[i] > peak )
        {
            peak = s[i];
        }
    }
    return peak;
}

void krfid_wave_stats(const uint16_t *s, size_t n, krfid_field_t *out)
{
    krfid_field_t f = { 0U, 0U, 0U, 0U, 0U };
    size_t   count = n;
    size_t   i;
    uint32_t sum = 0U;
    uint32_t dev = 0U;

    if ( NULL == out )
    {
        return;
    }
    if ( (NULL != s) && (0U < n) )
    {
        if ( (size_t)UINT16_MAX < count )
        {
            count = (size_t)UINT16_MAX;
        }
        f.min = UINT16_MAX;
        for ( i = 0U; i < count; i++ )
        {
            const uint16_t v = s[i];

            if ( v < f.min )
            {
                f.min = v;
            }
            if ( v > f.peak )
            {
                f.peak = v;
            }
            sum += (uint32_t)v;
        }
        f.mean = (uint16_t)(sum / (uint32_t)count);
        for ( i = 0U; i < count; i++ )
        {
            const uint16_t v = s[i];

            if ( v >= f.mean )
            {
                dev += (uint32_t)v - (uint32_t)f.mean;
            }
            else
            {
                dev += (uint32_t)f.mean - (uint32_t)v;
            }
        }
        f.ac = (uint16_t)(dev / (uint32_t)count);
        f.n  = (uint16_t)count;
    }
    *out = f;
}

size_t krfid_wave_decimate(const uint16_t *s, size_t n, uint16_t *wave,
                           size_t cap)
{
    size_t p;

    if ( (NULL == s) || (NULL == wave) || (0U == cap) )
    {
        return 0U;
    }
    if ( n <= cap )
    {
        for ( p = 0U; p < n; p++ )
        {
            wave[p] = s[p];
        }
        return n;
    }
    /* Bucket p is s[p * n / cap, (p + 1) * n / cap). With n > cap every
     * bucket holds at least one sample and they differ by one at most, the
     * last one ends exactly on n, and the product is taken in 64 bits so it
     * cannot wrap whatever size_t is. */
    for ( p = 0U; p < cap; p++ )
    {
        const size_t first = (size_t)(((uint64_t)p * (uint64_t)n) /
                                      (uint64_t)cap);
        const size_t last  = (size_t)((((uint64_t)p + 1U) * (uint64_t)n) /
                                      (uint64_t)cap);

        wave[p] = bucket_peak(s, first, last);
    }
    return cap;
}
