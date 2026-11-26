/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_keep_fit.c
 * @brief Where the kept block sits and how the heap is cut to hold it.
 */
#include "unity.h"

#include "khal_keep_fit.h"

#define TOP ((uintptr_t)0x4A000000U)   /* 32 MB of PSRAM from 0x48000000. */

#define SWEEP_TOP     (32U * 1024U * 1024U)   /* The largest block the model tries. */
#define SWEEP_STEP    99991U                  /* An odd step: a few hundred sizes. */
#define CAPTURE_BYTES 1540096U                /* The block the spike's sketch logged. */

/** @brief Cut a free block as the heap does until no fill is asked for; true when it ends ready. */
static bool cut_to_ready(size_t start)
{
    size_t   size  = start;
    uint32_t fills = 0U;
    size_t   fill  = khal_keep_fill(size);

    while ( 0U != fill )
    {
        TEST_ASSERT_EQUAL_UINT32((uint32_t)fill, (uint32_t)khal_keep_taken(fill));
        size -= fill + KHAL_KEEP_HEADER;
        ++fills;
        fill = khal_keep_fill(size);
    }
    TEST_ASSERT_TRUE(KHAL_KEEP_FILLS_MAX >= fills);
    return khal_keep_ready(size);
}

TEST_CASE("the kept block starts a megabyte and its gap under the PSRAM's top", "[khal_keep]")
{
    TEST_ASSERT_EQUAL_HEX32(0x49EFC000U, khal_keep_target(TOP));
}

TEST_CASE("a held piece that ends short of the block does not cover it", "[khal_keep]")
{
    TEST_ASSERT_FALSE(khal_keep_covers(TOP, (uintptr_t)0x49E82390U, CAPTURE_BYTES));
}

TEST_CASE("the held piece under the heap's end covers the block", "[khal_keep]")
{
    TEST_ASSERT_TRUE(khal_keep_covers(TOP, (uintptr_t)0x49EEEF00U, KHAL_KEEP_HOLD_BYTES));
}

TEST_CASE("a held piece that starts above the block does not cover it", "[khal_keep]")
{
    TEST_ASSERT_FALSE(khal_keep_covers(TOP, khal_keep_target(TOP) + 64U, KHAL_KEEP_HOLD_BYTES));
}

TEST_CASE("the heap takes a request rounded up to its class", "[khal_keep]")
{
    TEST_ASSERT_EQUAL_HEX32(0x1D00000U, khal_keep_taken(0x1CEFF00U));
    TEST_ASSERT_EQUAL_HEX32(KHAL_KEEP_HOLD_BYTES, khal_keep_taken(KHAL_KEEP_HOLD_BYTES));
}

TEST_CASE("no fill for a block too short or already ready, one just past the slack", "[khal_keep]")
{
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)khal_keep_fill((KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MIN) - 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)khal_keep_fill(KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MAX));
    TEST_ASSERT_NOT_EQUAL(0U, khal_keep_fill(KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MAX + 1U));
}

TEST_CASE("fills the heap takes exactly leave every block long enough ready for the held piece", "[khal_keep]")
{
    size_t const least = KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MIN;

    for ( size_t size = least; size <= SWEEP_TOP; size += SWEEP_STEP )
    {
        TEST_ASSERT_TRUE(cut_to_ready(size));
    }
    TEST_ASSERT_TRUE(cut_to_ready(SWEEP_TOP));
    TEST_ASSERT_TRUE(cut_to_ready(0x1E00000U));
    TEST_ASSERT_TRUE(cut_to_ready(CAPTURE_BYTES - 1U));
    TEST_ASSERT_TRUE(cut_to_ready(CAPTURE_BYTES));
    TEST_ASSERT_TRUE(cut_to_ready(CAPTURE_BYTES + 1U));
}
