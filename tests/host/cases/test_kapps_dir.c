/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kapps_dir.c
 * @brief Which directory copy a boot believes: the A/B choice, on the host.
 */
#include "unity.h"

#include <stdint.h>

#include "kapps_fmt.h"

/** @brief A generation that has been through a few commits. */
#define GEN_OLD 7U
/** @brief The generation after it. */
#define GEN_NEW 8U

TEST_CASE("a copy that alone is whole is the one used", "[kapps_dir]")
{
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_A, kapps_dir_pick(true, GEN_OLD, false, GEN_NEW));
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_B, kapps_dir_pick(false, GEN_NEW, true, GEN_OLD));
}

/* A commit writes the copy that is not active with gen + 1, so of two whole
 * copies the newer one is the last commit that finished. */
TEST_CASE("of two whole copies the newer generation is used", "[kapps_dir]")
{
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_A, kapps_dir_pick(true, GEN_NEW, true, GEN_OLD));
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_B, kapps_dir_pick(true, GEN_OLD, true, GEN_NEW));
}

/* Never written by kodeOS, which always moves gen on; a tie still has to
 * name one copy, and A is the one a format writes. */
TEST_CASE("two whole copies of one generation use A", "[kapps_dir]")
{
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_A, kapps_dir_pick(true, GEN_OLD, true, GEN_OLD));
}

/* The generation of a copy that is not whole means nothing: a torn write can
 * leave any number there, a large one included, and it must not win. */
TEST_CASE("a broken copy loses whatever generation it claims", "[kapps_dir]")
{
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_A, kapps_dir_pick(true, GEN_OLD, false, UINT32_MAX));
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_B, kapps_dir_pick(false, UINT32_MAX, true, GEN_OLD));
}

TEST_CASE("with no whole copy the region is formatted", "[kapps_dir]")
{
    TEST_ASSERT_EQUAL(KAPPS_DIR_PICK_FORMAT, kapps_dir_pick(false, GEN_NEW, false, GEN_OLD));
}
