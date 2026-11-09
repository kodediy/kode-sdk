/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_glass.c
 * @brief The glass's rounded corners as numbers.
 */
#include "unity.h"

#include "kui_glass.h"

TEST_CASE("the Kode Dot's glass comes from kbsp", "[kui_glass]")
{
    kui_glass_t const g = kui_glass_kode_dot();

    TEST_ASSERT_EQUAL_INT32(410, g.w);
    TEST_ASSERT_EQUAL_INT32(502, g.h);
    TEST_ASSERT_EQUAL_INT32(114, g.corner_r);
}

TEST_CASE("inset at a depth matches the drawing, rounded up", "[kui_glass]")
{
    kui_glass_t const g = kui_glass_kode_dot();

    TEST_ASSERT_EQUAL_INT32(114, kui_glass_inset(&g, 0));
    TEST_ASSERT_EQUAL_INT32(85, kui_glass_inset(&g, 4));
    TEST_ASSERT_EQUAL_INT32(53, kui_glass_inset(&g, 18));
    TEST_ASSERT_EQUAL_INT32(33, kui_glass_inset(&g, 34));
    TEST_ASSERT_EQUAL_INT32(19, kui_glass_inset(&g, 52));
    TEST_ASSERT_EQUAL_INT32(18, kui_glass_inset(&g, 53));
    TEST_ASSERT_EQUAL_INT32(6, kui_glass_inset(&g, 80));
    TEST_ASSERT_EQUAL_INT32(0, kui_glass_inset(&g, 114));
    TEST_ASSERT_EQUAL_INT32(0, kui_glass_inset(&g, 300));
}

TEST_CASE("a depth above the glass is the whole corner", "[kui_glass]")
{
    kui_glass_t const g = kui_glass_kode_dot();

    TEST_ASSERT_EQUAL_INT32(114, kui_glass_inset(&g, -5));
}

TEST_CASE("a square screen has no inset", "[kui_glass]")
{
    kui_glass_t const g = { 320, 240, 0 };

    TEST_ASSERT_EQUAL_INT32(0, kui_glass_inset(&g, 0));
    TEST_ASSERT_TRUE(kui_glass_rect_inside(&g, 0, 0, 319, 239));
}

TEST_CASE("a full-width row clears the glass from y 53 to 448", "[kui_glass]")
{
    kui_glass_t const g = kui_glass_kode_dot();

    TEST_ASSERT_TRUE(kui_glass_rect_inside(&g, 18, 53, 391, 448));
    TEST_ASSERT_FALSE(kui_glass_rect_inside(&g, 17, 53, 391, 448));
    TEST_ASSERT_FALSE(kui_glass_rect_inside(&g, 18, 52, 391, 448));
    TEST_ASSERT_FALSE(kui_glass_rect_inside(&g, 18, 53, 392, 448));
    TEST_ASSERT_FALSE(kui_glass_rect_inside(&g, 18, 53, 391, 449));
}

TEST_CASE("a rectangle off the screen is not inside", "[kui_glass]")
{
    kui_glass_t const g = kui_glass_kode_dot();

    TEST_ASSERT_FALSE(kui_glass_rect_inside(&g, -1, 200, 100, 300));
    TEST_ASSERT_FALSE(kui_glass_rect_inside(&g, 200, 200, 410, 300));
    TEST_ASSERT_TRUE(kui_glass_rect_inside(&g, 150, 200, 250, 300));
}

TEST_CASE("NULL answers the safe way", "[kui_glass]")
{
    TEST_ASSERT_EQUAL_INT32(0, kui_glass_inset(NULL, 10));
    TEST_ASSERT_FALSE(kui_glass_rect_inside(NULL, 0, 0, 1, 1));
}
