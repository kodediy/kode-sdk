/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_krfid_em4100.c
 * @brief The pure EM4100 codec, checked against synthesised Manchester windows.
 */
#include <string.h>
#include <stdint.h>

#include "unity.h"

#include "krfid_em4100.h"

#define WIN_MAX 20000

static uint16_t win[WIN_MAX];

/* The decoder keeps no scratch of its own; on the host a static one will do. */
static krfid_em4100_work_t work;

/* Build the 64 raw frame bits from a 5-byte ID, header first. */
static void build_frame(const uint8_t id[5], uint8_t out[64])
{
    uint8_t nib[10];
    for ( int i = 0; 5 > i; i++ ) { nib[2 * i] = id[i] >> 4; nib[2 * i + 1] = id[i] & 0xF; }
    int p = 0;
    for ( int i = 0; 9 > i; i++ ) out[p++] = 1;                 /* header */
    for ( int r = 0; 10 > r; r++ )
    {
        int d0 = (nib[r] >> 3) & 1, d1 = (nib[r] >> 2) & 1, d2 = (nib[r] >> 1) & 1, d3 = nib[r] & 1;
        out[p++] = d0; out[p++] = d1; out[p++] = d2; out[p++] = d3;
        out[p++] = d0 ^ d1 ^ d2 ^ d3;                        /* row parity */
    }
    for ( int c = 0; 4 > c; c++ )
    {
        int par = 0;
        for ( int r = 0; 10 > r; r++ ) par ^= (nib[r] >> (3 - c)) & 1;
        out[p++] = par;                                      /* column parity */
    }
    out[p++] = 0;                                            /* stop */
}

/* Split a 64-bit frame into its 64 bits, most-significant first. */
static void frame_to_bits(uint64_t frame, uint8_t out[64])
{
    for ( int i = 0; 64 > i; i++ ) out[i] = (uint8_t)((frame >> (63 - i)) & 1U);
}

/* A small, repeatable LCG for additive noise. */
static uint32_t lcg = 12345;
static int noise(int amp)
{
    lcg = lcg * 1103515245u + 12345u;
    if ( 0 == amp )
    {
        return 0;
    }
    return (int)((lcg >> 16) % (2 * amp + 1)) - amp;
}

/* Generate Manchester samples from 64 frame bits: bit1 -> [hi,lo], bit0 -> [lo,hi],
 * hb samples per half-bit, `reps` whole frames, optional leading junk and noise. */
static int gen_bits(const uint8_t fr[64], int hb, int hi, int lo, int reps, int nz, int start_off)
{
    int n = 0;
    for ( int i = 0; i < start_off && WIN_MAX > n; i++ ) win[n++] = lo + noise(nz);
    for ( int rep = 0; rep < reps; rep++ )
    {
        for ( int b = 0; 64 > b; b++ )
        {
            int h0, h1;
            if ( fr[b] ) { h0 = hi; h1 = lo; }
            else       { h0 = lo; h1 = hi; }
            for ( int k = 0; k < hb && WIN_MAX > n; k++ ) win[n++] = h0 + noise(nz);
            for ( int k = 0; k < hb && WIN_MAX > n; k++ ) win[n++] = h1 + noise(nz);
        }
    }
    return n;
}

static int gen(const uint8_t id[5], int hb, int hi, int lo, int reps, int nz, int start_off)
{
    uint8_t fr[64];
    build_frame(id, fr);
    return gen_bits(fr, hb, hi, lo, reps, nz, start_off);
}

/* Decode `win`[0..n) and assert the recovered ID matches; -1 for no expected div. */
static void expect_id(const uint8_t want[5], int n, int expect_div)
{
    uint8_t got[5];
    krfid_em4100_diag_t d;
    int r = krfid_em4100_decode(win, n, 100000, &work, got, &d);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, got, 5);
    if ( 0 <= expect_div )
    {
        TEST_ASSERT_EQUAL_INT(expect_div, d.bitrate_div);
    }
}

static const uint8_t ID_A[5] = {0x1A, 0x2B, 0x3C, 0x4D, 0x5E};
static const uint8_t ID_B[5] = {0xF0, 0x0D, 0xCA, 0xFE, 0x01};

TEST_CASE("krfid: a clean RF/32 window decodes to the known id", "[krfid_em4100]")
{
    expect_id(ID_A, gen(ID_A, 13, 3000, 400, 6, 0, 0), 32);
}

TEST_CASE("krfid: a clean RF/64 window decodes to the known id", "[krfid_em4100]")
{
    expect_id(ID_A, gen(ID_A, 26, 3000, 400, 3, 0, 0), 64);
}

TEST_CASE("krfid: RF/32 survives noise and a partial start", "[krfid_em4100]")
{
    expect_id(ID_A, gen(ID_A, 13, 2600, 700, 6, 180, 37), 32);
}

TEST_CASE("krfid: RF/64 survives noise, a second id", "[krfid_em4100]")
{
    expect_id(ID_B, gen(ID_B, 26, 3000, 500, 3, 200, 11), 64);
}

TEST_CASE("krfid: a coarse RF/32 window at ~10 samples/half-bit decodes", "[krfid_em4100]")
{
    /* 80 kSPS reality: RF/32 lands near 10 samples/half-bit. Assert the id, not
     * the div - at hb 10 the snap rounds to the RF/32 boundary. */
    expect_id(ID_A, gen(ID_A, 10, 3000, 400, 7, 130, 7), -1);
}

TEST_CASE("krfid: an even coarser hb-8 window still recovers the id", "[krfid_em4100]")
{
    expect_id(ID_B, gen(ID_B, 8, 2800, 500, 7, 90, 3), -1);
}

TEST_CASE("krfid: a flat window reports no tag", "[krfid_em4100]")
{
    krfid_em4100_diag_t d;
    uint8_t got[5];
    for ( int i = 0; 6000 > i; i++ ) win[i] = 1700 + noise(40);
    TEST_ASSERT_EQUAL_INT(-1, krfid_em4100_decode(win, 6000, 100000, &work, got, &d));
}

TEST_CASE("krfid: an all-zero window reports no tag", "[krfid_em4100]")
{
    krfid_em4100_diag_t d;
    uint8_t got[5];
    memset(win, 0, sizeof win);
    TEST_ASSERT_EQUAL_INT(-1, krfid_em4100_decode(win, 6000, 100000, &work, got, &d));
}

TEST_CASE("krfid: the encoded frame carries a valid header, parity and stop", "[krfid_em4100]")
{
    const uint8_t id[5] = {0x12, 0x34, 0x56, 0x78, 0x90};
    uint64_t f = krfid_em4100_frame(id);

    for ( int i = 0; 9 > i; i++ )
    {                    /* 9-bit header, all 1 */
        TEST_ASSERT_EQUAL_INT(1, (int)((f >> (63 - i)) & 1U));
    }

    uint8_t nib[10];
    uint8_t col[4] = {0};
    for ( int r = 0; 10 > r; r++ )
    {                   /* rows: 4 data + even row parity */
        int base = 54 - 5 * r;
        int d0 = (int)((f >> base) & 1U);
        int d1 = (int)((f >> (base - 1)) & 1U);
        int d2 = (int)((f >> (base - 2)) & 1U);
        int d3 = (int)((f >> (base - 3)) & 1U);
        int rp = (int)((f >> (base - 4)) & 1U);
        TEST_ASSERT_EQUAL_INT(d0 ^ d1 ^ d2 ^ d3, rp);
        nib[r] = (uint8_t)((d0 << 3) | (d1 << 2) | (d2 << 1) | d3);
        col[0] ^= d0; col[1] ^= d1; col[2] ^= d2; col[3] ^= d3;
    }
    for ( int c = 0; 4 > c; c++ )
    {                    /* 4 even column parities at bits 4..1 */
        TEST_ASSERT_EQUAL_INT(col[c], (int)((f >> (4 - c)) & 1U));
    }
    TEST_ASSERT_EQUAL_INT(0, (int)(f & 1U));         /* stop bit */

    for ( int i = 0; 5 > i; i++ )
    {                    /* the 40 data bits are the ID */
        uint8_t b = (uint8_t)((nib[2 * i] << 4) | nib[2 * i + 1]);
        TEST_ASSERT_EQUAL_UINT8(id[i], b);
    }
}

TEST_CASE("krfid: a frame round-trips through the decoder to the same id", "[krfid_em4100]")
{
    const uint8_t id[5] = {0xAB, 0xCD, 0xEF, 0x01, 0x23};
    uint8_t fr[64];
    frame_to_bits(krfid_em4100_frame(id), fr);
    int n = gen_bits(fr, 13, 3000, 400, 6, 0, 0);
    expect_id(id, n, 32);
}
