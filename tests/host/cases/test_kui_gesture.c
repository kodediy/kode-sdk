/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_gesture.c
 * @brief kui_gesture: when a press is a tap, a claimed drag, or a named rejection.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "kui_gesture.h"

/** Where every case lands. */
#define LAND_X 100
/** Where every case lands. */
#define LAND_Y 100
/** A press that went this far along one axis is surely past every threshold. */
#define FAR_PX 20
/** Any value that is not a state. */
#define NOT_A_STATE 99

/** A fresh press at the common landing, input open. */
static void land(kui_gesture_t *g, uint8_t allowed)
{
    kui_gesture_press(g, LAND_X, LAND_Y, allowed, false);
}

/** Lands with every direction taken, moves to (x, y) and answers the direction claimed. */
static uint8_t claimed_dir(int32_t x, int32_t y)
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_VERT | KUI_GESTURE_DIR_HORZ);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_move(&g, x, y));
    return g.dir;
}

TEST_CASE("a press that barely moves is a tap", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, g.state);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 105, 103));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_TAP, kui_gesture_release(&g, 105, 103));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_TAP, g.state);
}

TEST_CASE("a tap may wander sixteen pixels where nothing drags", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, 0U);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 116, 100));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_ZONE, g.reason);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_TAP, kui_gesture_release(&g, 116, 100));
}

TEST_CASE("seventeen pixels where nothing drags is out of zone", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, 0U);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_ZONE, kui_gesture_move(&g, 117, 100));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_ZONE, kui_gesture_release(&g, 117, 100));
}

TEST_CASE("a drag is claimed at twelve pixels along its axis", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 100, 111));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_move(&g, 100, 112));
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_DOWN, g.dir);
    TEST_ASSERT_EQUAL_INT32(112, g.cy);
    TEST_ASSERT_EQUAL_INT32(100, g.cx);
}

TEST_CASE("one and a half to one is enough, a hair less is diagonal", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_move(&g, 108, 112));

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 109, 112));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_DIAGONAL, g.reason);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_DIAGONAL, kui_gesture_move(&g, 113, 118));
}

TEST_CASE("equal axes are diagonal", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_VERT | KUI_GESTURE_DIR_HORZ);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 112, 112));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_DIAGONAL, kui_gesture_move(&g, 120, 120));
}

TEST_CASE("a direction the zone does not take is axis", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_AXIS, kui_gesture_move(&g, 100, 80));
    TEST_ASSERT_EQUAL_UINT8(0U, g.dir);
}

TEST_CASE("each direction maps to its bit", "[kui_gesture]")
{
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_UP, claimed_dir(LAND_X, LAND_Y - FAR_PX));
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_DOWN, claimed_dir(LAND_X, LAND_Y + FAR_PX));
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_LEFT, claimed_dir(LAND_X - FAR_PX, LAND_Y));
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_RIGHT, claimed_dir(LAND_X + FAR_PX, LAND_Y));
}

TEST_CASE("a claim holds its axis whatever the finger does after", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_move(&g, 100, 112));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_move(&g, 200, 130));
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_DOWN, g.dir);
    TEST_ASSERT_EQUAL_INT32(112, g.cy);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_release(&g, 200, 130));
}

TEST_CASE("a diagonal never claims later", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 109, 112));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_DIAGONAL, kui_gesture_move(&g, 100, 140));
    TEST_ASSERT_EQUAL_UINT8(0U, g.dir);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_DIAGONAL, kui_gesture_release(&g, 100, 140));
}

TEST_CASE("a blocked press stays blocked", "[kui_gesture]")
{
    kui_gesture_t g;

    kui_gesture_press(&g, LAND_X, LAND_Y, KUI_GESTURE_DIR_DOWN, true);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_BLOCKED, g.state);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_BLOCKED, kui_gesture_move(&g, 100, 200));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_BLOCKED, kui_gesture_release(&g, 100, 200));
}

TEST_CASE("travel is measured from the landing", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_HORZ);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 104, 100));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 108, 100));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 111, 100));
    TEST_ASSERT_EQUAL_INT32(11, g.travel);
}

TEST_CASE("a rejection noted within sixteen pixels is still a tap", "[kui_gesture]")
{
    kui_gesture_t g;

    /* Diagonal at 12 px: noted, not final. */
    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 109, 112));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_TAP, kui_gesture_release(&g, 109, 112));

    /* A direction the zone does not take, at 12 px up. */
    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, kui_gesture_move(&g, 100, 88));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_AXIS, g.reason);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_TAP, kui_gesture_release(&g, 100, 88));
}

TEST_CASE("a release far from the last move is not a tap", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, 0U);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_ZONE, kui_gesture_release(&g, 130, 100));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_ZONE, g.state);
}

TEST_CASE("a release can complete a claim", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_release(&g, 100, 140));
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_DOWN, g.dir);
    TEST_ASSERT_EQUAL_INT32(140, g.cy);
    TEST_ASSERT_EQUAL_INT32(100, g.cx);
}

TEST_CASE("a press starts over whatever the structure held", "[kui_gesture]")
{
    kui_gesture_t g;

    land(&g, KUI_GESTURE_DIR_DOWN);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_CLAIMED, kui_gesture_move(&g, 100, 140));
    kui_gesture_press(&g, 50, 60, KUI_GESTURE_DIR_UP, false);

    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_PENDING, g.state);
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_IDLE, g.reason);
    TEST_ASSERT_EQUAL_INT32(50, g.x0);
    TEST_ASSERT_EQUAL_INT32(60, g.y0);
    TEST_ASSERT_EQUAL_INT32(0, g.travel);
    TEST_ASSERT_EQUAL_UINT8(KUI_GESTURE_DIR_UP, g.allowed);
    TEST_ASSERT_EQUAL_UINT8(0U, g.dir);
}

TEST_CASE("a gesture that was never pressed stays idle", "[kui_gesture]")
{
    kui_gesture_t g = { 0 };

    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_IDLE, kui_gesture_move(&g, 100, 200));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_IDLE, kui_gesture_release(&g, 100, 200));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_IDLE, kui_gesture_move(NULL, 100, 200));
    TEST_ASSERT_EQUAL_INT(KUI_GESTURE_IDLE, kui_gesture_release(NULL, 100, 200));
    kui_gesture_press(NULL, 100, 200, KUI_GESTURE_DIR_DOWN, false);
}

TEST_CASE("the names", "[kui_gesture]")
{
    TEST_ASSERT_EQUAL_STRING("idle", kui_gesture_name(KUI_GESTURE_IDLE));
    TEST_ASSERT_EQUAL_STRING("pending", kui_gesture_name(KUI_GESTURE_PENDING));
    TEST_ASSERT_EQUAL_STRING("claim", kui_gesture_name(KUI_GESTURE_CLAIMED));
    TEST_ASSERT_EQUAL_STRING("tap", kui_gesture_name(KUI_GESTURE_TAP));
    TEST_ASSERT_EQUAL_STRING("diagonal", kui_gesture_name(KUI_GESTURE_DIAGONAL));
    TEST_ASSERT_EQUAL_STRING("axis", kui_gesture_name(KUI_GESTURE_AXIS));
    TEST_ASSERT_EQUAL_STRING("zone", kui_gesture_name(KUI_GESTURE_ZONE));
    TEST_ASSERT_EQUAL_STRING("blocked", kui_gesture_name(KUI_GESTURE_BLOCKED));
    TEST_ASSERT_EQUAL_STRING("?", kui_gesture_name((kui_gesture_state_t)NOT_A_STATE));
}
