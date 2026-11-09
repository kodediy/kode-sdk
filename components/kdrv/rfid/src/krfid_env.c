/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_env.c
 * @brief The pulse-width fold (see krfid_env.h). Host-testable, no IDF deps.
 */
#include "krfid_env.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#define RUN_BITS       16U      /* one run's share of a word: the low half first */
#define RUN_HALF_MASK  0xFFFFU  /* the run's half of the word                    */
#define RUN_DUR_MASK   0x7FFFU  /* its 15-bit duration, in ticks                 */
#define RUN_LEVEL_BIT  15U      /* its level, right above the duration           */

/**
 * @brief Add @p b to @p a, saturating at UINT16_MAX.
 * @param a The bin's time so far, in ticks.
 * @param b The run's duration, in ticks.
 * @return The sum, or UINT16_MAX when it does not fit.
 */
static uint16_t add_sat(uint16_t a, uint32_t b)
{
    uint32_t const s = (uint32_t)a + b;

    if ( (uint32_t)UINT16_MAX < s )
    {
        return UINT16_MAX;
    }
    return (uint16_t)s;
}

/**
 * @brief Walk the runs and add each level-1 run's time to its starting bin.
 *
 * The position is kept as a period and an offset inside it, never as an
 * absolute tick count, so no length of stream can overflow it. Each bin opens
 * a quarter period before its period does (see krfid_env_from_symbols()).
 *
 * @param sym    The RMT words.
 * @param nsym   How many.
 * @param period Ticks per bin; not 0.
 * @param acc    Out: the time above threshold per bin, in ticks.
 * @param cap    Capacity of @p acc, in bins; not 0.
 * @return The number of whole periods the stream covers, at most @p cap.
 */
static int regrid(const uint32_t *sym, int nsym, uint32_t period,
                  uint16_t *acc, uint32_t cap)
{
    uint32_t const lead = period / 4U;  /* how early a bin opens, in ticks */
    uint32_t per = 0U;   /* the period the next run starts in  */
    uint32_t off = 0U;   /* how far into that period, in ticks */
    bool     end = false;
    int      w;

    (void)memset(acc, 0, (size_t)cap * sizeof(acc[0]));
    for ( w = 0; (w < nsym) && (!end); ++w )
    {
        uint32_t half;

        for ( half = 0U; (2U > half) && (!end); ++half )
        {
            uint32_t const run = (sym[w] >> (half * RUN_BITS)) & RUN_HALF_MASK;
            uint32_t const dur = run & RUN_DUR_MASK;

            if ( 0U == dur )
            {
                end = true;
            }
            else
            {
                /* The run rises at t = per * period + off, and its bin is
                 * (t + lead) / period: off and lead are both under a period,
                 * so that is this period's bin or the next one's. An edge a
                 * tick either side of a period multiple lands a quarter
                 * period inside its bin, not on the boundary, where jitter
                 * would put two pulses in one bin and none in the next. A
                 * run whose bin is past the buffer belongs to no sample the
                 * fold returns. */
                uint32_t const idx = per + ((off + lead) / period);

                if ( ((run >> RUN_LEVEL_BIT) != 0U) && (idx < cap) )
                {
                    acc[idx] = add_sat(acc[idx], dur);
                }
                off += dur;
                per += off / period;
                off %= period;
                if ( per >= cap )
                {
                    per = cap;
                    end = true;
                }
            }
        }
    }
    return (int)per;
}

/**
 * @brief Describe the regridded window: the pulse ratio and the width spread.
 * @param acc The time above threshold per bin, in ticks. Read only when
 *            @p n is over 0.
 * @param n   How many bins.
 * @param st  Out: the stats.
 */
static void stats_fill(const uint16_t *acc, int n, krfid_env_stats_t *st)
{
    uint64_t sum    = 0U;
    uint32_t lo     = UINT32_MAX;
    uint32_t hi     = 0U;
    int      pulsed = 0;
    int      i;

    for ( i = 0; i < n; ++i )
    {
        uint32_t const w = acc[i];

        if ( 0U != w )
        {
            ++pulsed;
            sum += w;
            if ( w < lo )
            {
                lo = w;
            }
            if ( w > hi )
            {
                hi = w;
            }
        }
    }
    st->bins        = n;
    st->bins_pulsed = pulsed;
    if ( 0 == pulsed )
    {
        st->w_min  = 0U;
        st->w_mean = 0U;
        st->w_max  = 0U;
    }
    else
    {
        st->w_min  = lo;
        st->w_mean = (uint32_t)(sum / (uint64_t)pulsed);
        st->w_max  = hi;
    }
}

/**
 * @brief Box-average and scale the window in place, from its end backwards.
 * @param env The window: ticks per bin in, envelope samples out.
 * @param n   How many bins.
 * @param len The average's length in bins; not 0.
 */
static void box_scale(uint16_t *env, int n, uint32_t len)
{
    int i;

    /* Backwards: the average is causal, so each sample reads only bins not yet overwritten. */
    for ( i = n - 1; 0 <= i; --i )
    {
        uint32_t const idx   = (uint32_t)i;
        uint32_t       sum   = 0U;
        uint32_t       count = 0U;
        uint32_t       v;

        while ( (count < len) && (count <= idx) )
        {
            sum += env[idx - count];
            ++count;
        }
        v = ((sum * KRFID_ENV_SCALE) + (count / 2U)) / count;
        if ( (uint32_t)UINT16_MAX < v )
        {
            v = UINT16_MAX;
        }
        env[idx] = (uint16_t)v;
    }
}

int krfid_env_from_symbols(const uint32_t *sym, int nsym, uint16_t period_ticks,
                           uint8_t box, uint16_t *out, int max,
                           krfid_env_stats_t *st)
{
    uint32_t len = box;
    int      n   = 0;

    if ( (NULL != sym) && (NULL != out) && (0 < nsym) && (0 < max) &&
        (0U < period_ticks) )
    {
        n = regrid(sym, nsym, period_ticks, out, (uint32_t)max);
    }
    if ( NULL != st )
    {
        stats_fill(out, n, st);
    }
    if ( 0U == len )
    {
        len = 1U;
    }
    box_scale(out, n, len);
    return n;
}
