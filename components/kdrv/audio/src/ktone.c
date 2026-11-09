/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ktone.c
 * @brief The tone generator and the Goertzel detector.
 */
#include "ktone.h"

#include <math.h>
#include <string.h>

/**
 * @brief The eight candidates, as Goertzel bin indices.
 *
 * Each one is an exact multiple of KTONE_RATE_HZ / KTONE_WIN_N, which is what
 * makes a tone at that frequency land in one bin and nowhere else. The set
 * spans 1957 / 1019 = 1.92, under an octave, so twice the lowest is above the
 * highest and no member's harmonics can reach another member.
 */
static const uint16_t KTONE_BIN_K[KTONE_BIN_N] = {
    87U, 97U, 107U, 119U, 131U, 143U, 155U, 167U
};

/** @brief Full scale, as the divisor that normalises a sample to +/- 1. */
#define KTONE_FULL_SCALE_F   32768.0f

/**
 * @brief A decibel figure as the tenth-dB integer everything else carries.
 *
 * Clamped at both ends: silence would otherwise be an infinity, and no real
 * reading is anywhere near the top of the range.
 *
 * @param db The figure.
 * @return It, times ten, rounded.
 */
static int16_t to_x10(float db)
{
    float v = db * 10.0f;

    if ( !((float)KTONE_SILENCE_DBFS_X10 < v) )
    {
        /* Written as a negated greater-than so that a NaN - which no
         * finite input produces, but a zero power would without the guard
         * below - lands on silence rather than propagating. */
        return (int16_t)KTONE_SILENCE_DBFS_X10;
    }
    if ( 0.0f < v )
    {
        /* Nothing here can exceed full scale, so anything positive is a
         * rounding artefact at exactly 0 dBFS. */
        v = 0.0f;
    }
    return (int16_t)lrintf(v);
}

uint16_t ktone_bin_hz(uint8_t i)
{
    uint32_t hz;

    if ( (uint8_t)KTONE_BIN_N <= i )
    {
        return 0U;
    }
    /* KTONE_RATE_HZ / KTONE_WIN_N is 48000 / 4096 = 375 / 32 exactly, so the
     * frequency is integer arithmetic with a rounding term and never a float. */
    hz = (((uint32_t)KTONE_BIN_K[i] * 375U) + 16U) / 32U;
    return (uint16_t)hz;
}

uint8_t ktone_bin_nearest(uint16_t hz)
{
    uint8_t  best = 0U;
    uint32_t best_d = UINT32_MAX;
    uint8_t  i;

    for ( i = 0U; (uint8_t)KTONE_BIN_N > i; ++i )
    {
        uint16_t f = ktone_bin_hz(i);
        uint32_t d;

        if ( f > hz )
        {
            d = (uint32_t)(f - hz);
        }
        else
        {
            d = (uint32_t)(hz - f);
        }
        if ( d < best_d )
        {
            best_d = d;
            best   = i;
        }
    }
    return best;
}

int16_t ktone_sample(uint8_t i, uint32_t n, int16_t amp)
{
    uint32_t phase;
    float    v;

    if ( (uint8_t)KTONE_BIN_N <= i )
    {
        return 0;
    }
    /* (k * n) mod N, computed as (k * (n mod N)) mod N so the product cannot
     * grow with n: the two are equal for all n, and the second never leaves
     * a range of about 700 000 however long the tone runs. */
    phase = ((n % KTONE_WIN_N) * (uint32_t)KTONE_BIN_K[i]) % KTONE_WIN_N;

    v = sinf((KGOERTZEL_TWO_PI_F * (float)phase) / (float)KTONE_WIN_N);
    return (int16_t)lrintf(v * (float)amp);
}

kode_status_t ktone_fill(uint8_t i, uint32_t n0, int16_t amp,
                         int16_t *out, size_t frames)
{
    size_t f;

    if ( (NULL == out) || ((uint8_t)KTONE_BIN_N <= i) )
    {
        return KODE_DENIED;
    }
    for ( f = 0U; f < frames; ++f )
    {
        out[f] = ktone_sample(i, n0 + (uint32_t)f, amp);
    }
    return KODE_OK;
}

void ktone_reset(ktone_det_t *det)
{
    uint8_t b;

    if ( NULL == det )
    {
        return;
    }
    (void)memset(det, 0, sizeof(*det));

    for ( b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
    {
        kgoertzel_init_bin(&det->bin[b], KTONE_BIN_K[b], KTONE_WIN_N);
    }
}

void ktone_feed(ktone_det_t *det, const int16_t *mono, size_t n)
{
    size_t s;

    if ( (NULL == det) || (NULL == mono) )
    {
        return;
    }
    for ( s = 0U; s < n; ++s )
    {
        int16_t  v = mono[s];
        int32_t  mag = (int32_t)v;
        float    x;
        uint8_t  b;

        if ( 0U == det->count )
        {
            det->first = v;
        }
        else if ( v != det->first )
        {
            det->varied = true;
        }
        else
        {
            /* Same as the first sample. Nothing to note; the branch is here
             * so that the chain has no implicit fall-through. */
        }

        if ( 0 > mag )
        {
            mag = -mag;
        }
        if ( 32767 < mag )
        {
            /* INT16_MIN has no positive twin. Report the largest that fits. */
            mag = 32767;
        }
        if ( (int32_t)det->peak < mag )
        {
            det->peak = (int16_t)mag;
        }
        if ( (int32_t)KTONE_CLIP_LSB <= mag )
        {
            if ( UINT16_MAX > det->clipped )
            {
                det->clipped += 1U;
            }
        }

        /* Integer, so the broadband energy is exact: the largest term is
         * about 1.07e9 and a full window of them is 4.4e12, well inside 64
         * bits, where a float accumulation would have started losing bits. */
        det->sumsq += (uint64_t)((int32_t)v * (int32_t)v);

        x = (float)v / KTONE_FULL_SCALE_F;

        for ( b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
        {
            kgoertzel_feed(&det->bin[b], x);
        }
        det->count += 1U;
    }
}

kode_status_t ktone_result(const ktone_det_t *det, ktone_measure_t *out)
{
    float   ref_db;
    float   mean;
    uint8_t b;

    if ( (NULL == det) || (NULL == out) || (0U == det->count) )
    {
        /* A window that was never fed leaves @p out untouched: a failed
         * measurement must never become a plausible reading of a microphone. */
        return KODE_DENIED;
    }
    (void)memset(out, 0, sizeof(*out));

    /* A full-scale sine at a bin accumulates (count / 2) there, so its power
     * is the square of that. Referring every bin to it makes such a tone read
     * 0.0 dBFS, which is the reference ktone.h documents. */
    ref_db = 20.0f * log10f((float)det->count / 2.0f);

    for ( b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
    {
        float p = kgoertzel_power(&det->bin[b]);

        if ( 0.0f < p )
        {
            out->bin_x10[b] = to_x10((10.0f * log10f(p)) - ref_db);
        }
        else
        {
            out->bin_x10[b] = (int16_t)KTONE_SILENCE_DBFS_X10;
        }
        if ( out->bin_x10[b] > out->bin_x10[out->peak_bin] )
        {
            out->peak_bin = b;
        }
    }

    mean = (float)det->sumsq / (float)det->count;
    if ( 0.0f < mean )
    {
        out->rms_x10 = to_x10(10.0f * log10f(mean
                                             / (KTONE_FULL_SCALE_F
                                                * KTONE_FULL_SCALE_F)));
    }
    else
    {
        out->rms_x10 = (int16_t)KTONE_SILENCE_DBFS_X10;
    }

    if ( 0 < det->peak )
    {
        out->peak_x10 = to_x10(20.0f * log10f((float)det->peak
                                              / KTONE_FULL_SCALE_F));
    }
    else
    {
        out->peak_x10 = (int16_t)KTONE_SILENCE_DBFS_X10;
    }

    out->clipped = det->clipped;
    out->stuck   = !det->varied;
    return KODE_OK;
}

ktone_floor_t ktone_floor_judge(const ktone_measure_t *m)
{
    if ( NULL == m )
    {
        /* A measurement that is not there is not a bench problem. Report the
         * worst of the three, so a caller can never read a missing window as
         * a quiet one. */
        return KTONE_FLOOR_STUCK;
    }
    if ( m->stuck )
    {
        return KTONE_FLOOR_STUCK;
    }
    if ( 0U < m->clipped )
    {
        return KTONE_FLOOR_CLIPPED;
    }
    if ( (int16_t)KTONE_FLOOR_MAX_DBFS_X10 < m->rms_x10 )
    {
        return KTONE_FLOOR_LOUD;
    }
    return KTONE_FLOOR_OK;
}

const char *ktone_floor_reason(ktone_floor_t f)
{
    switch ( f )
    {
        case KTONE_FLOOR_OK:
            return "ok";
        case KTONE_FLOOR_STUCK:
            return "every sample identical; no ADC or no I2S";
        case KTONE_FLOOR_CLIPPED:
            return "the input is railed rather than listening";
        case KTONE_FLOOR_LOUD:
            return "the bench is too loud to judge in";
        default:
            return "?";
    }
}

int16_t ktone_other_max(const ktone_measure_t *m, uint8_t bin)
{
    int16_t best = (int16_t)KTONE_SILENCE_DBFS_X10;
    uint8_t b;

    if ( NULL == m )
    {
        return best;
    }
    for ( b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
    {
        if ( b == bin )
        {
            continue;
        }
        if ( m->bin_x10[b] > best )
        {
            best = m->bin_x10[b];
        }
    }
    return best;
}

bool ktone_window_ok(const ktone_measure_t *m, uint8_t bin, int16_t floor_x10)
{
    int32_t level;
    int32_t other;

    if ( (NULL == m) || ((uint8_t)KTONE_BIN_N <= bin) )
    {
        return false;
    }
    level = (int32_t)m->bin_x10[bin];
    other = (int32_t)ktone_other_max(m, bin);

    if ( ((int32_t)floor_x10 + (int32_t)KTONE_SNR_DB_X10) > level )
    {
        return false;
    }
    if ( (other + (int32_t)KTONE_BIN_MARGIN_DB_X10) > level )
    {
        return false;
    }
    if ( m->peak_bin != bin )
    {
        /* Redundant given the test above, and checked anyway: this is the
         * sentence the log prints and an operator reads. */
        return false;
    }
    return true;
}
