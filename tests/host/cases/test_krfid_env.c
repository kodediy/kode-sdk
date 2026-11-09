/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_krfid_env.c
 * @brief The pulse-width fold, checked against synthesised comparator streams.
 */
#include <string.h>
#include <stdint.h>

#include "unity.h"

#include "krfid_em4100.h"
#include "krfid_env.h"

#define PERIOD      160U    /* ticks per carrier period at 20 MHz          */
#define W_FULL      80U     /* the pulse under a high half-bit             */
#define W_DIP       60U     /* the pulse under a low one, narrower         */
#define W_NONE      0U      /* the pulse under a low one, dropped (keying) */
#define BOX         4U      /* C142, in software                           */
#define RF64_PER_HB 32      /* carrier periods per half-bit at RF/64       */
#define RF32_PER_HB 16      /* carrier periods per half-bit at RF/32       */
#define FULL_ENV    (PERIOD * KRFID_ENV_SCALE)   /* a whole period high: 5120 */
#define ENV_HZ      125000U /* one sample per carrier period               */
#define W_JIT       96U     /* the pulse on the board's coherent carrier   */
#define JIT_PULSES  1000U   /* its periods, each with a jittered edge      */
#define SYM_MAX     13000
#define ENV_MAX     13000

static uint32_t sym[SYM_MAX];
static uint16_t env[ENV_MAX];
static uint16_t env2[ENV_MAX];

/* The decoder keeps no scratch of its own; on the host a static one will do. */
static krfid_em4100_work_t work;

static const uint8_t ID_A[5] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
static const uint8_t ID_B[5] = {0xF0, 0x0D, 0xCA, 0xFE, 0x01};

/* Words are packed here from krfid_env.h's documented layout, never with the fold's own code. */
/* One run as half a word: duration in [14:0], level in bit 15. */
static uint32_t run_half(uint32_t level, uint32_t dur)
{
    return (dur & 0x7FFFU) | ((level & 1U) << 15);
}

/* One whole word, the low half first. */
static uint32_t word(uint32_t d0, uint32_t l0, uint32_t d1, uint32_t l1)
{
    return run_half(l0, d0) | (run_half(l1, d1) << 16);
}

/* The stream under construction. A run stays open while its level repeats -
 * the pad does not change, so RMT sees one run - and a closed run fills the
 * next half of the next word. */
static int      s_nsym;   /* whole words written                    */
static int      s_half;   /* 1 when sym[s_nsym] holds its low half  */
static uint32_t s_level;  /* the open run's level                   */
static uint32_t s_dur;    /* the open run's length, 0 = none open   */

static void synth_reset(void)
{
    memset(sym, 0, sizeof sym);
    s_nsym  = 0;
    s_half  = 0;
    s_level = 0U;
    s_dur   = 0U;
}

static void synth_close(void)
{
    if ( 0U == s_dur )
    {
        return;
    }
    if ( 0 == s_half )
    {
        sym[s_nsym] = run_half(s_level, s_dur);
        s_half = 1;
    }
    else
    {
        sym[s_nsym] |= run_half(s_level, s_dur) << 16;
        s_nsym++;
        s_half = 0;
    }
    s_dur = 0U;
}

static void synth_run(uint32_t level, uint32_t dur)
{
    if ( 0U == dur )
    {
        return;
    }
    if ( (0U != s_dur) && (level == s_level) )
    {
        s_dur += dur;
        return;
    }
    synth_close();
    s_level = level;
    s_dur   = dur;
}

/* One carrier period: a pulse of @p w ticks, then the rest of it low. */
static void synth_period(uint32_t w)
{
    synth_run(1U, w);
    synth_run(0U, PERIOD - w);
}

/* Close the stream; a half-filled last word counts, its zero high half is
 * the end marker RMT writes. */
static int synth_end(void)
{
    synth_close();
    return s_nsym + s_half;
}

/* @p periods identical pulses of @p w ticks. */
static int synth_carrier(int periods, uint32_t w)
{
    synth_reset();
    for ( int i = 0; i < periods; i++ )
    {
        synth_period(w);
    }
    return synth_end();
}

/* A tag answering with @p id: @p reps frames' worth of bits starting at frame
 * bit @p start, Manchester (bit 1 = high then low half-bit, bit 0 = low then
 * high), @p per_hb carrier periods per half-bit. */
static int synth_tag(const uint8_t id[5], int per_hb, uint32_t w_hi,
                     uint32_t w_lo, int reps, int start)
{
    uint64_t const frame = krfid_em4100_frame(id);

    synth_reset();
    for ( int b = start; (start + (64 * reps)) > b; b++ )
    {
        uint32_t first;
        uint32_t second;

        if ( ((frame >> (63 - (b % 64))) & 1U) != 0U )
        {
            first  = w_hi;
            second = w_lo;
        }
        else
        {
            first  = w_lo;
            second = w_hi;
        }
        for ( int k = 0; k < per_hb; k++ )
        {
            synth_period(first);
        }
        for ( int k = 0; k < per_hb; k++ )
        {
            synth_period(second);
        }
    }
    return synth_end();
}

/* Fold `sym`, decode the envelope, and assert the id and the rate. */
static void expect_decodes(const uint8_t want[5], int nsym, int div,
                           krfid_env_stats_t *st)
{
    krfid_em4100_diag_t d;
    uint8_t got[5];
    int n = krfid_env_from_symbols(sym, nsym, PERIOD, BOX, env, ENV_MAX, st);

    TEST_ASSERT_GREATER_THAN_INT(0, n);
    TEST_ASSERT_EQUAL_INT(0, krfid_em4100_decode(env, n, ENV_HZ, &work, got, &d));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, got, 5);
    TEST_ASSERT_EQUAL_INT(div, d.bitrate_div);
}

TEST_CASE("krfid_env: a clean carrier folds to a flat 2560", "[krfid_env]")
{
    krfid_env_stats_t st;
    int const nsym = synth_carrier(1000, W_FULL);

    /* Passed with a zero-filled tail, as a forced RMT stop leaves it: the
     * first zero duration ends the stream, not the count. */
    TEST_ASSERT_EQUAL_INT(1000, nsym);
    TEST_ASSERT_EQUAL_INT(1000, krfid_env_from_symbols(sym, nsym + 50, PERIOD, BOX,
                                                       env, ENV_MAX, &st));
    for ( int i = 0; 1000 > i; i++ )
    {
        TEST_ASSERT_EQUAL_UINT16(2560U, env[i]);
    }
    TEST_ASSERT_EQUAL_INT(1000, st.bins);
    TEST_ASSERT_EQUAL_INT(1000, st.bins_pulsed);
    TEST_ASSERT_EQUAL_UINT32(W_FULL, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(W_FULL, st.w_mean);
    TEST_ASSERT_EQUAL_UINT32(W_FULL, st.w_max);
}

TEST_CASE("krfid_env: edges a tick either side of a period multiple keep their own bin", "[krfid_env]")
{
    krfid_env_stats_t st;
    uint32_t t = 0U;   /* where the stream has got to, in ticks */

    /* A carrier coherent with the RMT clock puts every rising edge on a
     * period multiple, give or take a tick of jitter. Here the even edges
     * come one tick late and the odd ones one tick early, so consecutive
     * edges straddle k * PERIOD; the stream opens on a one-tick low run for
     * the first edge to be late too. Each pulse must still fold into a bin
     * of its own - never two in one bin and none in the next. */
    synth_reset();
    for ( uint32_t k = 0U; JIT_PULSES > k; k++ )
    {
        uint32_t edge = k * PERIOD;

        if ( (k % 2U) == 0U )
        {
            edge += 1U;
        }
        else
        {
            edge -= 1U;
        }
        synth_run(0U, edge - t);
        synth_run(1U, W_JIT);
        t = edge + W_JIT;
    }
    synth_run(0U, (JIT_PULSES * PERIOD) - t);
    int const nsym = synth_end();

    TEST_ASSERT_EQUAL_INT((int)JIT_PULSES, krfid_env_from_symbols(sym, nsym, PERIOD, 1U,
                                                                  env, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_INT((int)JIT_PULSES, st.bins);
    TEST_ASSERT_EQUAL_INT(st.bins, st.bins_pulsed);
    TEST_ASSERT_EQUAL_UINT32(W_JIT, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(W_JIT, st.w_max);
    for ( int i = 0; i < st.bins; i++ )
    {
        TEST_ASSERT_EQUAL_UINT16(W_JIT * KRFID_ENV_SCALE, env[i]);
    }
}

TEST_CASE("krfid_env: the fold stops at max bins", "[krfid_env]")
{
    krfid_env_stats_t st;
    int const nsym = synth_carrier(1000, W_FULL);

    TEST_ASSERT_EQUAL_INT(100, krfid_env_from_symbols(sym, nsym, PERIOD, BOX,
                                                      env, 100, &st));
    TEST_ASSERT_EQUAL_INT(100, st.bins);
    TEST_ASSERT_EQUAL_INT(100, st.bins_pulsed);
}

TEST_CASE("krfid_env: the average is causal and clamped at the start", "[krfid_env]")
{
    krfid_env_stats_t st;

    synth_reset();
    synth_period(80U);
    synth_period(40U);
    synth_period(70U);
    synth_period(60U);
    synth_period(20U);
    int const nsym = synth_end();

    /* Box 0 and box 1 are both no averaging: the widths, scaled. */
    TEST_ASSERT_EQUAL_INT(5, krfid_env_from_symbols(sym, nsym, PERIOD, 0U,
                                                    env, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_UINT16(2560U, env[0]);
    TEST_ASSERT_EQUAL_UINT16(1280U, env[1]);
    TEST_ASSERT_EQUAL_UINT16(2240U, env[2]);
    TEST_ASSERT_EQUAL_UINT16(1920U, env[3]);
    TEST_ASSERT_EQUAL_UINT16(640U, env[4]);
    TEST_ASSERT_EQUAL_INT(5, krfid_env_from_symbols(sym, nsym, PERIOD, 1U,
                                                    env2, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_UINT16_ARRAY(env, env2, 5);

    /* Box 4: over what exists at the start, over four from the fourth on,
     * rounded to nearest - 190 ticks over 3 bins is 2026.7, so 2027. */
    TEST_ASSERT_EQUAL_INT(5, krfid_env_from_symbols(sym, nsym, PERIOD, BOX,
                                                    env, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_UINT16(2560U, env[0]);
    TEST_ASSERT_EQUAL_UINT16(1920U, env[1]);
    TEST_ASSERT_EQUAL_UINT16(2027U, env[2]);
    TEST_ASSERT_EQUAL_UINT16(2000U, env[3]);
    TEST_ASSERT_EQUAL_UINT16(1520U, env[4]);

    /* The stats describe the widths, not the average. */
    TEST_ASSERT_EQUAL_INT(5, st.bins_pulsed);
    TEST_ASSERT_EQUAL_UINT32(20U, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(54U, st.w_mean);
    TEST_ASSERT_EQUAL_UINT32(80U, st.w_max);
}

TEST_CASE("krfid_env: width modulation at RF/64 decodes to the frame's id", "[krfid_env]")
{
    krfid_env_stats_t st;
    int const nsym = synth_tag(ID_A, RF64_PER_HB, W_FULL, W_DIP, 3, 0);
    uint16_t lo = UINT16_MAX;
    uint16_t hi = 0U;

    expect_decodes(ID_A, nsym, 64, &st);

    /* Bimodal: nothing outside the two widths the tag produces. */
    for ( int i = 0; i < st.bins; i++ )
    {
        if ( env[i] < lo )
        {
            lo = env[i];
        }
        if ( env[i] > hi )
        {
            hi = env[i];
        }
    }
    TEST_ASSERT_EQUAL_UINT16(W_DIP * KRFID_ENV_SCALE, lo);
    TEST_ASSERT_EQUAL_UINT16(W_FULL * KRFID_ENV_SCALE, hi);

    /* Every period pulsed; Manchester has as many high half-bits as low. */
    TEST_ASSERT_EQUAL_INT(3 * 64 * 2 * RF64_PER_HB, st.bins);
    TEST_ASSERT_EQUAL_INT(st.bins, st.bins_pulsed);
    TEST_ASSERT_EQUAL_UINT32(W_DIP, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(70U, st.w_mean);
    TEST_ASSERT_EQUAL_UINT32(W_FULL, st.w_max);
}

TEST_CASE("krfid_env: width modulation at RF/32 decodes to the frame's id", "[krfid_env]")
{
    krfid_env_stats_t st;
    int const nsym = synth_tag(ID_B, RF32_PER_HB, W_FULL, W_DIP, 5, 23);

    expect_decodes(ID_B, nsym, 32, &st);
    TEST_ASSERT_EQUAL_INT(st.bins, st.bins_pulsed);
}

TEST_CASE("krfid_env: keying (dropped pulses) at RF/64 decodes", "[krfid_env]")
{
    static const uint8_t id[5] = {0xAB, 0xCD, 0xEF, 0x01, 0x23};
    krfid_env_stats_t st;
    int const nsym = synth_tag(id, RF64_PER_HB, W_FULL, W_NONE, 3, 41);

    expect_decodes(id, nsym, 64, &st);

    /* Half the periods carry a pulse, every one of them whole. */
    TEST_ASSERT_EQUAL_INT(st.bins, 2 * st.bins_pulsed);
    TEST_ASSERT_EQUAL_UINT32(W_FULL, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(W_FULL, st.w_mean);
    TEST_ASSERT_EQUAL_UINT32(W_FULL, st.w_max);
}

TEST_CASE("krfid_env: a chattered pulse sums back to one width", "[krfid_env]")
{
    krfid_env_stats_t st;

    /* One 80-tick pulse in three fragments, the gaps inside its period, then
     * one clean pulse in the next bin: five level-1 runs, two pulses. */
    memset(sym, 0, sizeof sym);
    sym[0] = word(30U, 1U, 10U, 0U);
    sym[1] = word(30U, 1U, 10U, 0U);
    sym[2] = word(20U, 1U, 100U, 0U);
    sym[3] = word(80U, 1U, 80U, 0U);

    TEST_ASSERT_EQUAL_INT(2, krfid_env_from_symbols(sym, 4, PERIOD, 1U,
                                                    env, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_UINT16(2560U, env[0]);
    TEST_ASSERT_EQUAL_UINT16(2560U, env[1]);
    TEST_ASSERT_EQUAL_INT(2, st.bins);
    TEST_ASSERT_EQUAL_INT(2, st.bins_pulsed);
    TEST_ASSERT_EQUAL_UINT32(80U, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(80U, st.w_max);
}

TEST_CASE("krfid_env: a stream under one period or a bad argument gives nothing", "[krfid_env]")
{
    krfid_env_stats_t st;

    memset(sym, 0, sizeof sym);
    sym[0] = word(80U, 1U, 40U, 0U);   /* 120 ticks: no whole period */

    memset(&st, 0xA5, sizeof st);
    TEST_ASSERT_EQUAL_INT(0, krfid_env_from_symbols(sym, 1, PERIOD, BOX,
                                                    env, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_INT(0, st.bins);
    TEST_ASSERT_EQUAL_INT(0, st.bins_pulsed);
    TEST_ASSERT_EQUAL_UINT32(0U, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(0U, st.w_mean);
    TEST_ASSERT_EQUAL_UINT32(0U, st.w_max);

    /* A stream that ends on its first run. */
    sym[0] = 0U;
    TEST_ASSERT_EQUAL_INT(0, krfid_env_from_symbols(sym, 10, PERIOD, BOX,
                                                    env, ENV_MAX, &st));

    (void)synth_carrier(10, W_FULL);
    TEST_ASSERT_EQUAL_INT(0, krfid_env_from_symbols(sym, 10, PERIOD, BOX,
                                                    NULL, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_INT(0, st.bins);
    TEST_ASSERT_EQUAL_INT(0, krfid_env_from_symbols(sym, 10, PERIOD, BOX,
                                                    env, 0, &st));
    TEST_ASSERT_EQUAL_INT(0, krfid_env_from_symbols(NULL, 10, PERIOD, BOX,
                                                    env, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_INT(0, krfid_env_from_symbols(sym, 0, PERIOD, BOX,
                                                    env, ENV_MAX, &st));
    TEST_ASSERT_EQUAL_INT(0, krfid_env_from_symbols(sym, 10, 0U, BOX,
                                                    env, ENV_MAX, &st));

    /* The stats are optional. */
    TEST_ASSERT_EQUAL_INT(10, krfid_env_from_symbols(sym, 10, PERIOD, BOX,
                                                     env, ENV_MAX, NULL));
}

TEST_CASE("krfid_env: inverted level bits fold to the complement", "[krfid_env]")
{
    krfid_env_stats_t st;
    int const nsym = synth_tag(ID_A, RF64_PER_HB, W_FULL, W_DIP, 3, 0);
    int const n = krfid_env_from_symbols(sym, nsym, PERIOD, BOX, env, ENV_MAX, NULL);

    /* The fold counts level 1 and nothing else: with every bit flipped it
     * measures the gaps, a full period minus each pulse. */
    for ( int i = 0; i < nsym; i++ )
    {
        sym[i] ^= 0x80008000U;
    }
    TEST_ASSERT_EQUAL_INT(n, krfid_env_from_symbols(sym, nsym, PERIOD, BOX,
                                                    env2, ENV_MAX, &st));
    for ( int i = 0; i < n; i++ )
    {
        TEST_ASSERT_EQUAL_UINT16(FULL_ENV - env[i], env2[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(PERIOD - W_FULL, st.w_min);
    TEST_ASSERT_EQUAL_UINT32(90U, st.w_mean);
    TEST_ASSERT_EQUAL_UINT32(PERIOD - W_DIP, st.w_max);

    /* The codec tries both polarities, so the complement still reads. */
    expect_decodes(ID_A, nsym, 64, &st);
}
