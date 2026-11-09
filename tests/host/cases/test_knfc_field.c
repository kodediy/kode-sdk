/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_field.c
 * @brief knfc_field: the listen window's running stats, folded burst by burst.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "knfc_field.h"

TEST_CASE("bursts fold into one min, mean, peak and mod", "[knfc_field]")
{
    static const uint8_t first[3]  = { 10U, 20U, 30U };
    static const uint8_t second[1] = { 5U };
    knfc_field_acc_t a;
    knfc_field_t f;

    knfc_field_reset(&a);
    knfc_field_fold(&a, first, sizeof(first), false);
    knfc_field_fold(&a, second, sizeof(second), false);
    knfc_field_close(&a, &f);

    TEST_ASSERT_EQUAL_UINT8(5U, f.min);
    TEST_ASSERT_EQUAL_UINT8(30U, f.peak);
    TEST_ASSERT_EQUAL_UINT8(16U, f.mean);   /* 65 / 4, truncated */
    TEST_ASSERT_EQUAL_UINT8(25U, f.mod);
    TEST_ASSERT_FALSE(f.ext);
    TEST_ASSERT_EQUAL_UINT16(4U, f.n);
}

TEST_CASE("the external field stays set once a burst saw it", "[knfc_field]")
{
    static const uint8_t burst[2] = { 40U, 50U };
    knfc_field_acc_t a;
    knfc_field_t f;

    knfc_field_reset(&a);
    knfc_field_fold(&a, burst, sizeof(burst), false);
    knfc_field_fold(&a, burst, sizeof(burst), true);
    knfc_field_fold(&a, burst, sizeof(burst), false);
    knfc_field_close(&a, &f);

    TEST_ASSERT_TRUE(f.ext);
    TEST_ASSERT_EQUAL_UINT16(6U, f.n);
}

TEST_CASE("an empty burst still carries the detector", "[knfc_field]")
{
    knfc_field_acc_t a;
    knfc_field_t f;

    knfc_field_reset(&a);
    knfc_field_fold(&a, NULL, 0U, true);
    (void)memset(&f, 0xA5, sizeof(f));
    knfc_field_close(&a, &f);

    TEST_ASSERT_TRUE(f.ext);
    TEST_ASSERT_EQUAL_UINT16(0U, f.n);
    TEST_ASSERT_EQUAL_UINT8(0U, f.min);
    TEST_ASSERT_EQUAL_UINT8(0U, f.mean);
    TEST_ASSERT_EQUAL_UINT8(0U, f.peak);
    TEST_ASSERT_EQUAL_UINT8(0U, f.mod);
}

TEST_CASE("a window nothing was folded into closes all zero", "[knfc_field]")
{
    knfc_field_acc_t a;
    knfc_field_t f;

    knfc_field_reset(&a);
    (void)memset(&f, 0xA5, sizeof(f));
    knfc_field_close(&a, &f);

    TEST_ASSERT_EQUAL_UINT8(0U, f.min);
    TEST_ASSERT_EQUAL_UINT8(0U, f.mean);
    TEST_ASSERT_EQUAL_UINT8(0U, f.peak);
    TEST_ASSERT_EQUAL_UINT8(0U, f.mod);
    TEST_ASSERT_FALSE(f.ext);
    TEST_ASSERT_EQUAL_UINT16(0U, f.n);
}

TEST_CASE("400 samples of 200 do not overflow the mean", "[knfc_field]")
{
    /* 400 x 200 is 80000, past what a 16-bit sum holds, so a sum that
     * narrowed would fail here and not only at the count's ceiling. */
    uint8_t burst[400];
    knfc_field_acc_t a;
    knfc_field_t f;

    (void)memset(burst, 200, sizeof(burst));
    knfc_field_reset(&a);
    knfc_field_fold(&a, burst, sizeof(burst), false);
    knfc_field_close(&a, &f);

    TEST_ASSERT_EQUAL_UINT8(200U, f.mean);
    TEST_ASSERT_EQUAL_UINT8(200U, f.min);
    TEST_ASSERT_EQUAL_UINT8(200U, f.peak);
    TEST_ASSERT_EQUAL_UINT8(0U, f.mod);
    TEST_ASSERT_EQUAL_UINT16(400U, f.n);
}

TEST_CASE("NULL arguments are ignored, and a NULL window closes all zero",
          "[knfc_field]")
{
    static const uint8_t burst[2] = { 7U, 9U };
    knfc_field_acc_t a;
    knfc_field_acc_t before;
    knfc_field_t f;

    (void)memset(&f, 0xA5, sizeof(f));
    knfc_field_close(NULL, &f);
    TEST_ASSERT_EQUAL_UINT8(0U, f.min);
    TEST_ASSERT_EQUAL_UINT8(0U, f.mean);
    TEST_ASSERT_EQUAL_UINT8(0U, f.peak);
    TEST_ASSERT_EQUAL_UINT8(0U, f.mod);
    TEST_ASSERT_FALSE(f.ext);
    TEST_ASSERT_EQUAL_UINT16(0U, f.n);

    knfc_field_reset(&a);
    knfc_field_fold(&a, burst, sizeof(burst), false);
    /* Copied byte for byte, padding included, so the comparison below sees
     * only what the NULL calls could have written. */
    (void)memcpy(&before, &a, sizeof(a));
    knfc_field_reset(NULL);
    knfc_field_fold(NULL, burst, sizeof(burst), true);
    knfc_field_close(&a, NULL);
    TEST_ASSERT_EQUAL_MEMORY(&before, &a, sizeof(a));
}

TEST_CASE("the count stops at its ceiling instead of wrapping", "[knfc_field]")
{
    static uint8_t burst[4096];
    knfc_field_acc_t a;
    knfc_field_t f;
    uint32_t i;

    (void)memset(burst, 255, sizeof(burst));
    knfc_field_reset(&a);
    /* 17 bursts of 4096 is 69632 samples, past the 65535 a uint16_t holds. */
    for ( i = 0U; 17U > i; i++ )
    {
        knfc_field_fold(&a, burst, sizeof(burst), false);
    }
    knfc_field_close(&a, &f);

    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, f.n);
    TEST_ASSERT_EQUAL_UINT8(255U, f.mean);
    TEST_ASSERT_EQUAL_UINT8(255U, f.peak);
}
