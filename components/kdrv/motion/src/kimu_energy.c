/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kimu_energy.c
 * @brief One Goertzel per axis under a Hann window, means removed first.
 */
#include "kimu_energy.h"

#include <math.h>

#include "kgoertzel.h"

kode_status_t kimu_energy(const int16_t *xyz, size_t n, uint16_t hz, uint16_t fs_hz,
                          uint16_t ug_per_lsb, kimu_energy_t *out)
{
    kgoertzel_t g[3];
    float       mean[3];
    float       sumsq = 0.0f;
    float       best  = -1.0f;
    size_t      i;
    uint8_t     a;

    if ( (NULL == xyz) || (NULL == out) || ((size_t)KIMU_ENERGY_MIN_N > n) )
    {
        return KODE_DENIED;
    }
    /* Means first: gravity is a 1 g DC term whose leakage would otherwise ride into the bin. */
    for ( a = 0U; 3U > a; ++a )
    {
        float s = 0.0f;

        for ( i = 0U; i < n; ++i )
        {
            s += (float)xyz[(3U * i) + a];
        }
        mean[a] = s / (float)n;
    }
    /* One pass over the samples, three detectors fed from it. The window
     * coefficient depends on the sample index alone, so computing it inside
     * an axis loop costs three cosines per sample to produce the same three
     * numbers - and there is a cosine per sample in a 256-sample window that
     * a production row waits on. */
    for ( a = 0U; 3U > a; ++a )
    {
        kgoertzel_init(&g[a], (float)hz, (float)fs_hz);
    }
    for ( i = 0U; i < n; ++i )
    {
        float const w = 0.5f - (0.5f * cosf((KGOERTZEL_TWO_PI_F * (float)i)
                                            / (float)n));

        for ( a = 0U; 3U > a; ++a )
        {
            float const x = (float)xyz[(3U * i) + a] - mean[a];

            sumsq += x * x;
            kgoertzel_feed(&g[a], x * w);
        }
    }
    out->axis = 0U;
    for ( a = 0U; 3U > a; ++a )
    {
        float const p = kgoertzel_power(&g[a]);

        if ( p > best )
        {
            best = p;
            out->axis = a;
        }
    }
    if ( 0.0f > best )
    {
        best = 0.0f;
    }
    {
        float const amp_lsb = (4.0f * sqrtf(best)) / (float)n;
        float const rms_lsb = sqrtf(sumsq / (float)n);
        float const lsb_to_x10 = (float)ug_per_lsb / 100.0f;   /* ug -> 0.1 mg */

        out->bin_x10 = (int32_t)lrintf(amp_lsb * lsb_to_x10);
        out->rms_x10 = (int32_t)lrintf(rms_lsb * lsb_to_x10);
    }
    return KODE_OK;
}
