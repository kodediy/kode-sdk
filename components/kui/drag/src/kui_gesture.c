/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_gesture.c
 * @brief The recognizer's rules, by integer arithmetic on the distance from the landing.
 */
#include "kui_gesture.h"

#include <stddef.h>

/**
 * @brief The absolute value of a coordinate difference.
 *
 * @param v The difference.
 * @return |v|.
 */
static int32_t gesture_abs(int32_t v)
{
    if ( 0 > v )
    {
        return -v;
    }
    return v;
}

/**
 * @brief The direction a sample went, from the signed offsets from the landing.
 *
 * The vertical axis wins equal offsets; the decision never asks about them,
 * because equal axes are a diagonal.
 *
 * @param dx Signed horizontal offset from the landing.
 * @param dy Signed vertical offset from the landing.
 * @return One KUI_GESTURE_DIR_* bit.
 */
static uint8_t gesture_direction(int32_t dx, int32_t dy)
{
    if ( gesture_abs(dy) >= gesture_abs(dx) )
    {
        if ( 0 > dy )
        {
            return (uint8_t)KUI_GESTURE_DIR_UP;
        }
        return (uint8_t)KUI_GESTURE_DIR_DOWN;
    }
    if ( 0 > dx )
    {
        return (uint8_t)KUI_GESTURE_DIR_LEFT;
    }
    return (uint8_t)KUI_GESTURE_DIR_RIGHT;
}

/**
 * @brief Takes the one decision of a press at the sample that reached the claim distance.
 *
 * Claims the drag when the axis is clear and the zone takes the direction,
 * otherwise notes why not. Nothing here becomes the state but a claim.
 *
 * @param g The gesture, PENDING with no reason noted.
 * @param x Column of the sample.
 * @param y Row of the sample.
 * @param a Distance from the landing on the farther axis.
 * @param b Distance from the landing on the nearer axis.
 */
static void gesture_decide(kui_gesture_t *g, int32_t x, int32_t y, int32_t a, int32_t b)
{
    if ( (a * KUI_GESTURE_AXIS_DEN) < (b * KUI_GESTURE_AXIS_NUM) )
    {
        g->reason = KUI_GESTURE_DIAGONAL;
        return;
    }

    uint8_t const dir = gesture_direction(x - g->x0, y - g->y0);
    if ( 0U != (uint8_t)(g->allowed & dir) )
    {
        g->dir = dir;
        g->cx = x;
        g->cy = y;
        g->state = KUI_GESTURE_CLAIMED;
    }
    else if ( 0U == g->allowed )
    {
        g->reason = KUI_GESTURE_ZONE;
    }
    else
    {
        g->reason = KUI_GESTURE_AXIS;
    }
}

void kui_gesture_press(kui_gesture_t *g, int32_t x, int32_t y, uint8_t allowed, bool blocked)
{
    if ( NULL == g )
    {
        return;
    }
    g->x0 = x;
    g->y0 = y;
    g->cx = x;
    g->cy = y;
    g->travel = 0;
    g->allowed = allowed;
    g->dir = 0U;
    g->reason = KUI_GESTURE_IDLE;
    g->state = KUI_GESTURE_PENDING;
    if ( blocked )
    {
        g->state = KUI_GESTURE_BLOCKED;
    }
}

kui_gesture_state_t kui_gesture_move(kui_gesture_t *g, int32_t x, int32_t y)
{
    if ( NULL == g )
    {
        return KUI_GESTURE_IDLE;
    }
    if ( (KUI_GESTURE_IDLE == g->state) || (KUI_GESTURE_TAP == g->state) )
    {
        return g->state;
    }

    int32_t const ax = gesture_abs(x - g->x0);
    int32_t const ay = gesture_abs(y - g->y0);
    int32_t a = ax;
    int32_t b = ay;
    if ( ay > ax )
    {
        a = ay;
        b = ax;
    }
    if ( a > g->travel )
    {
        g->travel = a;
    }

    if ( KUI_GESTURE_PENDING != g->state )
    {
        return g->state;
    }
    if ( (KUI_GESTURE_IDLE == g->reason) && (a >= KUI_GESTURE_CLAIM_PX) )
    {
        gesture_decide(g, x, y, a, b);
    }
    if ( (KUI_GESTURE_PENDING == g->state) && (KUI_GESTURE_IDLE != g->reason)
         && (g->travel > KUI_GESTURE_TAP_PX) )
    {
        g->state = g->reason;
    }
    return g->state;
}

kui_gesture_state_t kui_gesture_release(kui_gesture_t *g, int32_t x, int32_t y)
{
    kui_gesture_state_t const s = kui_gesture_move(g, x, y);
    if ( KUI_GESTURE_PENDING != s )
    {
        return s;
    }
    g->state = KUI_GESTURE_TAP;
    return g->state;
}

const char *kui_gesture_name(kui_gesture_state_t s)
{
    switch ( s )
    {
    case KUI_GESTURE_IDLE:
        return "idle";
    case KUI_GESTURE_PENDING:
        return "pending";
    case KUI_GESTURE_CLAIMED:
        return "claim";
    case KUI_GESTURE_TAP:
        return "tap";
    case KUI_GESTURE_DIAGONAL:
        return "diagonal";
    case KUI_GESTURE_AXIS:
        return "axis";
    case KUI_GESTURE_ZONE:
        return "zone";
    case KUI_GESTURE_BLOCKED:
        return "blocked";
    default:
        return "?";
    }
}
