/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kgoertzel.c
 * @brief The one-bin kernel two detectors share, proved on synthetic sines.
 */
#include <math.h>

#include "unity.h"

#include "kgoertzel.h"

TEST_CASE("a full-scale sine on an integer bin accumulates (n/2)^2 of power", "[kgoertzel]")
{
    kgoertzel_t g;
    kgoertzel_init_bin(&g, 32U, 4096U);
    for ( uint32_t i = 0U; 4096U > i; ++i )
    {
        kgoertzel_feed(&g, sinf((2.0f * (float)M_PI * 32.0f * (float)i) / 4096.0f));
    }
    TEST_ASSERT_FLOAT_WITHIN(0.01f * 2048.0f * 2048.0f, 2048.0f * 2048.0f, kgoertzel_power(&g));
}

TEST_CASE("init from a frequency and a rate is the same coefficient as from a bin", "[kgoertzel]")
{
    kgoertzel_t a, b;
    kgoertzel_init(&a, 235.0f, 1920.0f);
    kgoertzel_init_bin(&b, 235U, 1920U);
    TEST_ASSERT_FLOAT_WITHIN(1e-6f, b.coeff, a.coeff);
}

TEST_CASE("a sine far from the bin leaves next to nothing in it", "[kgoertzel]")
{
    kgoertzel_t g;
    kgoertzel_init_bin(&g, 32U, 4096U);
    for ( uint32_t i = 0U; 4096U > i; ++i )
    {
        kgoertzel_feed(&g, sinf((2.0f * (float)M_PI * 200.0f * (float)i) / 4096.0f));
    }
    TEST_ASSERT_TRUE(kgoertzel_power(&g) < 1.0f);
}
