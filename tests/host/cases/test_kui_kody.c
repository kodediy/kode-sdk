/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_kody.c
 * @brief Kody's anchors as numbers: where the next one is, and whether a step passed it.
 */
#include "unity.h"

#include "kui_kody_anchor.h"

TEST_CASE("the next anchor of a 540-frame scene with one every 45", "[kui_kody]")
{
    TEST_ASSERT_EQUAL_UINT16(45, kui_kody_next_anchor(0, 45, 540));
    TEST_ASSERT_EQUAL_UINT16(45, kui_kody_next_anchor(44, 45, 540));
    TEST_ASSERT_EQUAL_UINT16(90, kui_kody_next_anchor(45, 45, 540));
    TEST_ASSERT_EQUAL_UINT16(495, kui_kody_next_anchor(494, 45, 540));
}

TEST_CASE("past the last anchor of the pass the next one is frame 0", "[kui_kody]")
{
    TEST_ASSERT_EQUAL_UINT16(0, kui_kody_next_anchor(495, 45, 540));
    TEST_ASSERT_EQUAL_UINT16(0, kui_kody_next_anchor(539, 45, 540));
    /* A 46-frame reaction ends on its second anchor, f45, and wraps after it. */
    TEST_ASSERT_EQUAL_UINT16(45, kui_kody_next_anchor(44, 45, 46));
    TEST_ASSERT_EQUAL_UINT16(0, kui_kody_next_anchor(45, 45, 46));
}

TEST_CASE("with no anchors the next stop is the end of the pass", "[kui_kody]")
{
    TEST_ASSERT_EQUAL_UINT16(56, kui_kody_next_anchor(0, 0, 57));
    TEST_ASSERT_EQUAL_UINT16(56, kui_kody_next_anchor(30, 0, 57));
    TEST_ASSERT_EQUAL_UINT16(56, kui_kody_next_anchor(56, 0, 57));
}

TEST_CASE("an animation with no frames has no anchor but 0", "[kui_kody]")
{
    TEST_ASSERT_EQUAL_UINT16(0, kui_kody_next_anchor(0, 45, 0));
    TEST_ASSERT_EQUAL_UINT16(0, kui_kody_next_anchor(0, 0, 0));
}

TEST_CASE("a forward step that passes the target has crossed it", "[kui_kody]")
{
    TEST_ASSERT_TRUE(kui_kody_crossed(40, 47, 45, 540));
    TEST_ASSERT_TRUE(kui_kody_crossed(0, 539, 300, 540));
}

TEST_CASE("a step that lands exactly on the target has crossed it", "[kui_kody]")
{
    TEST_ASSERT_TRUE(kui_kody_crossed(44, 45, 45, 540));
    /* The end of a one-shot: despierta_tren's last frame. */
    TEST_ASSERT_TRUE(kui_kody_crossed(50, 56, 56, 57));
}

TEST_CASE("a step that stops short, or starts on the target, has not crossed it", "[kui_kody]")
{
    TEST_ASSERT_FALSE(kui_kody_crossed(40, 44, 45, 540));
    TEST_ASSERT_FALSE(kui_kody_crossed(45, 50, 45, 540));
    TEST_ASSERT_FALSE(kui_kody_crossed(46, 89, 45, 540));
}

TEST_CASE("no step crosses nothing", "[kui_kody]")
{
    TEST_ASSERT_FALSE(kui_kody_crossed(10, 10, 10, 540));
    TEST_ASSERT_FALSE(kui_kody_crossed(10, 10, 45, 540));
}

TEST_CASE("a step across the end of the pass crosses what lies on either side", "[kui_kody]")
{
    /* 530 -> 539 -> 0 -> 3: frame 0 is the next anchor, and it is passed. */
    TEST_ASSERT_TRUE(kui_kody_crossed(530, 3, 0, 540));
    TEST_ASSERT_TRUE(kui_kody_crossed(530, 3, 3, 540));
    TEST_ASSERT_TRUE(kui_kody_crossed(530, 3, 535, 540));
    TEST_ASSERT_TRUE(kui_kody_crossed(530, 3, 539, 540));
    TEST_ASSERT_FALSE(kui_kody_crossed(530, 3, 4, 540));
    TEST_ASSERT_FALSE(kui_kody_crossed(530, 3, 530, 540));
    TEST_ASSERT_FALSE(kui_kody_crossed(530, 3, 520, 540));
}

TEST_CASE("a target that is not a frame of the pass is never crossed", "[kui_kody]")
{
    TEST_ASSERT_FALSE(kui_kody_crossed(530, 3, 540, 540));
    TEST_ASSERT_FALSE(kui_kody_crossed(0, 10, 5, 0));
}
