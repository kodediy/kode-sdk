/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_krfid_mod.c
 * @brief krfid_mod: the Manchester modulator of the tag emulation, and the
 *        EM4100 frame it plays, pinned to the rfid-lf branch's values.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "krfid_em4100.h"
#include "krfid_mod.h"

/** @brief The branch's frame for 12 34 00 00 00. */
#define FRAME_1234          0xFF8CA64800000008ULL

/** @brief FNV-1a over the branch's 4096 levels for it at RF/64. */
#define LEVELS_1234_FNV     0x021285C5UL

/** @brief FNV-1a offset basis. */
#define FNV_BASIS           2166136261UL

/** @brief FNV-1a prime. */
#define FNV_PRIME           16777619UL

/** @brief Bits of an EM4100 frame. */
#define FRAME_BITS          64U

/** @brief Bytes of an EM4100 frame. */
#define FRAME_BYTES         8U

/** @brief Room for the longest sequence any case asks for. */
#define LV_ROOM             4096U

/** @brief The scratch every case writes its levels into. */
static uint8_t s_lv[LV_ROOM];

/** @brief The branch's real card, the tag the pinned values are for. */
static const uint8_t s_id1234[5] = { 0x12U, 0x34U, 0x00U, 0x00U, 0x00U };

/**
 * @brief Split a frame into bytes, the first bit most significant.
 *
 * @param f   The 64-bit frame.
 * @param out Out: its 8 bytes.
 */
static void frame_bytes(uint64_t f, uint8_t out[FRAME_BYTES])
{
    uint8_t i;

    for ( i = 0U; FRAME_BYTES > i; ++i )
    {
        out[i] = (uint8_t)(f >> (8U * (uint8_t)(FRAME_BYTES - 1U - i)));
    }
}

/**
 * @brief FNV-1a of a run of levels.
 *
 * @param p The levels.
 * @param n How many.
 * @return The hash.
 */
static uint32_t fnv(const uint8_t *p, uint32_t n)
{
    uint32_t h = (uint32_t)FNV_BASIS;
    uint32_t i;

    for ( i = 0U; i < n; ++i )
    {
        h ^= p[i];
        h *= (uint32_t)FNV_PRIME;
    }
    return h;
}

/**
 * @brief Assert @p n levels equal @p v from @p at.
 *
 * @param at Where.
 * @param n  How many.
 * @param v  The level.
 */
static void levels_are(uint32_t at, uint32_t n, uint8_t v)
{
    uint32_t k;

    for ( k = 0U; k < n; ++k )
    {
        TEST_ASSERT_EQUAL_UINT8(v, s_lv[at + k]);
    }
}

TEST_CASE("krfid_mod: the frame of 12 34 00 00 00 is the one the branch built",
          "[krfid_mod]")
{
    TEST_ASSERT_TRUE(krfid_em4100_frame(s_id1234) == FRAME_1234);
}

/** @brief One branch vector: an ID and its 64-bit frame. */
typedef struct {
    uint8_t  id[5];   /**< The ID.        */
    uint64_t frame;   /**< Its EM4100 frame. */
} frame_vec_t;

TEST_CASE("krfid_mod: the EM4100 frames match the branch's vectors",
          "[krfid_mod]")
{
    static const frame_vec_t VEC[] = {
        { { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U }, 0xFF80000000000000ULL },
        { { 0x00U, 0x00U, 0x00U, 0x00U, 0x01U }, 0xFF80000000000062ULL },
        { { 0xF2U, 0x2CU, 0xF4U, 0xBCU, 0x37U }, 0xFFF8A5C7937C19F6ULL },
        { { 0xDDU, 0xEDU, 0x29U, 0xC0U, 0xC3U }, 0xFFEF7DD9658060D6ULL },
        { { 0xAAU, 0x48U, 0x01U, 0x00U, 0xB8U }, 0xFFD2898806005E3CULL },
    };
    uint32_t i;

    for ( i = 0U; (sizeof(VEC) / sizeof(VEC[0])) > i; ++i )
    {
        TEST_ASSERT_TRUE(krfid_em4100_frame(VEC[i].id) == VEC[i].frame);
    }
}

TEST_CASE("krfid_mod: Manchester, a 1 loads the first half of its bit, a 0 "
          "the second", "[krfid_mod]")
{
    uint8_t  f[1] = { 0x80U };   /* "10" */
    uint32_t n;

    n = krfid_mod_manchester(64U, f, 2U, s_lv, sizeof(s_lv));
    TEST_ASSERT_EQUAL_UINT32(128U, n);
    levels_are(0U, 32U, 1U);
    levels_are(32U, 32U, 0U);
    levels_are(64U, 32U, 0U);
    levels_are(96U, 32U, 1U);
}

TEST_CASE("krfid_mod: a Manchester bit at a short rate alternates every "
          "period", "[krfid_mod]")
{
    uint8_t  f[1] = { 0x80U };   /* "10" */
    uint32_t n;
    uint32_t k;

    n = krfid_mod_manchester(2U, f, 2U, s_lv, sizeof(s_lv));
    TEST_ASSERT_EQUAL_UINT32(4U, n);
    for ( k = 0U; 2U > k; ++k )
    {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(1U - k), s_lv[k]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)k, s_lv[2U + k]);
    }
}

TEST_CASE("krfid_mod: Manchester refuses what it cannot build", "[krfid_mod]")
{
    uint8_t f[FRAME_BYTES];

    (void)memset(f, 0x5A, sizeof(f));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_manchester(64U, NULL, FRAME_BITS, s_lv, sizeof(s_lv)));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_manchester(64U, f, FRAME_BITS, NULL, sizeof(s_lv)));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_manchester(64U, f, 0U, s_lv, sizeof(s_lv)));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_manchester(0U, f, FRAME_BITS, s_lv, sizeof(s_lv)));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_manchester(33U, f, FRAME_BITS, s_lv, sizeof(s_lv)));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_manchester(64U, f, FRAME_BITS, s_lv, 4095U));
    TEST_ASSERT_EQUAL_UINT32(4096U, krfid_mod_manchester(64U, f, FRAME_BITS, s_lv, 4096U));
    TEST_ASSERT_EQUAL_UINT32(1024U, krfid_mod_manchester(16U, f, FRAME_BITS, s_lv, sizeof(s_lv)));
}

TEST_CASE("krfid_mod: the EM4100 levels of 12 34 00 00 00 are the branch's, "
          "bit for bit", "[krfid_mod]")
{
    uint8_t  fb[FRAME_BYTES];
    uint8_t  ref[LV_ROOM];
    uint32_t n;

    n = krfid_mod_em4100(s_id1234, s_lv, sizeof(s_lv));
    TEST_ASSERT_EQUAL_UINT32(KRFID_MOD_EM4100_LEVELS, n);
    TEST_ASSERT_EQUAL_HEX32((uint32_t)LEVELS_1234_FNV, fnv(s_lv, n));

    /* The same levels from the two steps by hand. */
    frame_bytes(krfid_em4100_frame(s_id1234), fb);
    TEST_ASSERT_EQUAL_UINT32(
        KRFID_MOD_EM4100_LEVELS,
        krfid_mod_manchester(KRFID_MOD_EM4100_RF, fb, FRAME_BITS, ref, sizeof(ref)));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(ref, s_lv, KRFID_MOD_EM4100_LEVELS);
}

TEST_CASE("krfid_mod: the nine header ones of an EM4100 frame load the first "
          "half of every bit", "[krfid_mod]")
{
    uint32_t n = krfid_mod_em4100(s_id1234, s_lv, sizeof(s_lv));
    uint32_t b;

    TEST_ASSERT_EQUAL_UINT32(KRFID_MOD_EM4100_LEVELS, n);
    for ( b = 0U; 9U > b; ++b )
    {
        levels_are(b * KRFID_MOD_EM4100_RF, KRFID_MOD_EM4100_RF / 2U, 1U);
        levels_are((b * KRFID_MOD_EM4100_RF) + (KRFID_MOD_EM4100_RF / 2U),
                   KRFID_MOD_EM4100_RF / 2U, 0U);
    }
    /* The stop bit is a 0: released first, loaded second. */
    levels_are(63U * KRFID_MOD_EM4100_RF, KRFID_MOD_EM4100_RF / 2U, 0U);
    levels_are((63U * KRFID_MOD_EM4100_RF) + (KRFID_MOD_EM4100_RF / 2U),
               KRFID_MOD_EM4100_RF / 2U, 1U);
}

TEST_CASE("krfid_mod: the levels read back as the frame, half-bit pairs "
          "decoded", "[krfid_mod]")
{
    uint8_t  fb[FRAME_BYTES];
    uint64_t f = krfid_em4100_frame(s_id1234);
    uint32_t b;

    frame_bytes(f, fb);
    TEST_ASSERT_EQUAL_UINT32(KRFID_MOD_EM4100_LEVELS,
                             krfid_mod_em4100(s_id1234, s_lv, sizeof(s_lv)));
    for ( b = 0U; FRAME_BITS > b; ++b )
    {
        uint32_t const at   = b * KRFID_MOD_EM4100_RF;
        uint8_t  const want = (uint8_t)((f >> (FRAME_BITS - 1U - b)) & 1U);

        TEST_ASSERT_EQUAL_UINT8(want, s_lv[at]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(want ^ 1U), s_lv[at + (KRFID_MOD_EM4100_RF - 1U)]);
    }
}

TEST_CASE("krfid_mod: EM4100 refuses a NULL or a room below one frame",
          "[krfid_mod]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_em4100(NULL, s_lv, sizeof(s_lv)));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_em4100(s_id1234, NULL, sizeof(s_lv)));
    TEST_ASSERT_EQUAL_UINT32(0U, krfid_mod_em4100(s_id1234, s_lv,
                                                  KRFID_MOD_EM4100_LEVELS - 1U));
}

TEST_CASE("krfid_mod: the sequence loops seamlessly, every run a half bit or "
          "a whole one, the seam included", "[krfid_mod]")
{
    static const uint8_t RATES[3] = { 64U, 32U, 16U };
    uint32_t seed = 0x51A11E55U;
    uint32_t r;
    uint32_t t;

    for ( r = 0U; 3U > r; ++r )
    {
        uint32_t const rf = RATES[r];

        for ( t = 0U; 50U > t; ++t )
        {
            uint8_t  id[5];
            uint8_t  fb[FRAME_BYTES];
            uint32_t n;
            uint32_t k;
            uint32_t len   = 1U;
            bool     first = true;
            uint8_t  i;

            for ( i = 0U; 5U > i; ++i )
            {
                seed = (uint32_t)((seed * 1664525UL) + 1013904223UL);
                id[i] = (uint8_t)(seed >> 24);
            }
            frame_bytes(krfid_em4100_frame(id), fb);
            n = krfid_mod_manchester((uint8_t)rf, fb, FRAME_BITS, s_lv, sizeof(s_lv));
            TEST_ASSERT_EQUAL_UINT32(FRAME_BITS * rf, n);

            /* Four passes back to back: the seam sits between each pair. */
            for ( k = 1U; (4U * n) > k; ++k )
            {
                if ( s_lv[k % n] == s_lv[(k - 1U) % n] )
                {
                    ++len;
                    continue;
                }
                if ( !first )
                {
                    TEST_ASSERT_TRUE_MESSAGE(((rf / 2U) == len) || (len == rf),
                                             "a run of the wrong length");
                }
                first = false;
                len   = 1U;
            }
        }
    }
}
