/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kring.c
 * @brief The stream's ring: what goes in comes out, in order, and no more.
 */
#include <string.h>

#include "unity.h"

#include "kring.h"

/** @brief A small ring, so the cases can fill it and wrap it by hand. */
#define CAP 8U

static int16_t s_store[CAP];
static kring_t s_r;

TEST_CASE("an empty ring gives nothing, and a full one takes nothing more",
          "[kring]")
{
    int16_t in[CAP + 3U];
    int16_t out[CAP];

    kring_init(&s_r, s_store, CAP);
    TEST_ASSERT_EQUAL_UINT32(0U, kring_fill(&s_r));
    TEST_ASSERT_EQUAL_UINT32(0U, kring_take(&s_r, out, CAP));

    for ( size_t i = 0U; (CAP + 3U) > i; ++i )
    {
        in[i] = (int16_t)(i + 1U);
    }
    /* Only the room there is goes in, and the caller is told how much. */
    TEST_ASSERT_EQUAL_size_t(CAP, kring_put(&s_r, in, CAP + 3U, 1U));
    TEST_ASSERT_EQUAL_UINT32(CAP, kring_fill(&s_r));
    TEST_ASSERT_EQUAL_size_t(0U, kring_put(&s_r, in, 1U, 1U));

    TEST_ASSERT_EQUAL_UINT32(CAP, kring_take(&s_r, out, CAP));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, CAP);
    TEST_ASSERT_EQUAL_UINT32(0U, kring_fill(&s_r));
}

TEST_CASE("frames come out in order across the end of the array", "[kring]")
{
    int16_t in[6];
    int16_t out[6];

    kring_init(&s_r, s_store, CAP);
    for ( size_t i = 0U; 6U > i; ++i )
    {
        in[i] = (int16_t)(100 + (int)i);
    }
    /* Move the indices most of the way round first, so the next put wraps. */
    TEST_ASSERT_EQUAL_size_t(6U, kring_put(&s_r, in, 6U, 1U));
    TEST_ASSERT_EQUAL_UINT32(6U, kring_take(&s_r, out, 6U));

    for ( size_t i = 0U; 6U > i; ++i )
    {
        in[i] = (int16_t)(-200 - (int)i);
    }
    TEST_ASSERT_EQUAL_size_t(6U, kring_put(&s_r, in, 6U, 1U));
    TEST_ASSERT_EQUAL_UINT32(4U, kring_take(&s_r, out, 4U));
    TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, 4U);
    TEST_ASSERT_EQUAL_UINT32(2U, kring_take(&s_r, out, 6U));
    TEST_ASSERT_EQUAL_INT16(in[4], out[0]);
    TEST_ASSERT_EQUAL_INT16(in[5], out[1]);
}

TEST_CASE("a stereo pair goes in as its mean, with no overflow at full scale",
          "[kring]")
{
    int16_t const pairs[] = { 1000, 3000,   32767, 32767,   -32768, -32768,
                              32767, -32768 };
    int16_t out[4];

    kring_init(&s_r, s_store, CAP);
    TEST_ASSERT_EQUAL_size_t(4U, kring_put(&s_r, pairs, 4U, 2U));
    TEST_ASSERT_EQUAL_UINT32(4U, kring_take(&s_r, out, 4U));
    TEST_ASSERT_EQUAL_INT16(2000, out[0]);
    TEST_ASSERT_EQUAL_INT16(32767, out[1]);
    TEST_ASSERT_EQUAL_INT16(-32768, out[2]);
    TEST_ASSERT_EQUAL_INT16(0, out[3]);
}

TEST_CASE("a channel count other than one or two, or no storage, takes nothing",
          "[kring]")
{
    int16_t const in[4] = { 1, 2, 3, 4 };

    kring_init(&s_r, s_store, CAP);
    TEST_ASSERT_EQUAL_size_t(0U, kring_put(&s_r, in, 2U, 0U));
    TEST_ASSERT_EQUAL_size_t(0U, kring_put(&s_r, in, 1U, 4U));
    TEST_ASSERT_EQUAL_size_t(0U, kring_put(&s_r, NULL, 2U, 1U));

    kring_init(&s_r, NULL, CAP);
    TEST_ASSERT_EQUAL_size_t(0U, kring_put(&s_r, in, 2U, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, kring_fill(&s_r));
}

TEST_CASE("frames stay in order across the index modulus, not only the array",
          "[kring]")
{
    /* The slot is the index modulo CAP, but the indices themselves count
     * modulo 2*CAP, so a put lands right whichever way the counter last
     * wrapped. Cycle the ring far past 2*CAP one frame at a time and check
     * every frame comes back as it went in. */
    kring_init(&s_r, s_store, CAP);

    for ( int16_t v = 0; 100 > v; ++v )
    {
        int16_t out = -1;

        TEST_ASSERT_EQUAL_size_t(1U, kring_put(&s_r, &v, 1U, 1U));
        TEST_ASSERT_EQUAL_UINT32(1U, kring_fill(&s_r));
        TEST_ASSERT_EQUAL_UINT32(1U, kring_take(&s_r, &out, 1U));
        TEST_ASSERT_EQUAL_INT16(v, out);
        TEST_ASSERT_EQUAL_UINT32(0U, kring_fill(&s_r));
    }

    /* And a full ring across the same boundary: fill CAP, drain CAP, twice
     * around the modulus, values never colliding. */
    for ( int round = 0; 5 > round; ++round )
    {
        int16_t in[CAP];
        int16_t out[CAP];

        for ( size_t i = 0U; CAP > i; ++i )
        {
            in[i] = (int16_t)((round * 100) + (int)i);
        }
        TEST_ASSERT_EQUAL_size_t(CAP, kring_put(&s_r, in, CAP, 1U));
        TEST_ASSERT_EQUAL_UINT32(CAP, kring_fill(&s_r));
        TEST_ASSERT_EQUAL_UINT32(CAP, kring_take(&s_r, out, CAP));
        TEST_ASSERT_EQUAL_INT16_ARRAY(in, out, CAP);
    }
}
