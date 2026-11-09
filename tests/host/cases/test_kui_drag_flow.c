/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_drag_flow.c
 * @brief kui_gesture and kui_drag together: the console's swipe on a page pulled down from the top.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "kui_drag.h"
#include "kui_gesture.h"

/** The span of the page, in pixels: the panel's height. */
#define FLOW_SPAN 502
/** The column every swipe stays on. */
#define FLOW_X 205
/** The row every swipe starts on. */
#define FLOW_Y0 30
/** The clock when the swipe's press lands. */
#define FLOW_T0 5000U
/** The period of the touch sensor's polls, and so of the swipe's moves. */
#define FLOW_POLL_MS 10U
/** How far ahead of its stamp the release is decided, at the most: one poll period. */
#define FLOW_LEAD_MAX_MS 9U

/** One swipe and what the page does with it. */
typedef struct {
    const char *name;   /**< Which row, for the failure message. */
    int32_t y1;         /**< Where the finger lifts. */
    uint32_t ms;        /**< How long it takes to get there. */
    uint32_t hold;      /**< How long it rests there before it lifts. */
    int32_t claim_cy;   /**< The row of the sample that claims the drag. */
    int32_t offset;     /**< The page's offset when the finger lifts. */
    int32_t speed;      /**< The finger's speed at the lift, in pixels per second. */
    bool opens;         /**< Whether the release opens the page. */
} flow_row_t;

/** The five swipes: slow and short, slow and long, a flick, a long rest past halfway, a long rest short of it. */
static const flow_row_t ROWS[] = {
    { "y1=180 ms=600",          180, 600U, 0U,   43, 127, 250,  false },
    { "y1=320 ms=900",          320, 900U, 0U,   43, 277, 325,  true  },
    { "y1=110 ms=80",           110, 80U,  0U,   50, 22,  1000, true  },
    { "y1=320 ms=400 hold=300", 320, 400U, 300U, 45, 275, 0,    true  },
    { "y1=200 ms=400 hold=300", 200, 400U, 300U, 43, 157, 0,    false },
};

/**
 * The coordinate a fraction of the way from one point to another, as the
 * console's swipe plays it: to the nearest pixel, a tie going away from the
 * start, so a swipe and its mirror image land on mirrored pixels.
 */
static int32_t flow_lerp(int32_t from, int32_t to, int32_t num, int32_t den)
{
    int32_t const scaled = (to - from) * num;
    int32_t const half = den / 2;

    if ( 0 <= scaled )
    {
        return from + ((scaled + half) / den);
    }
    return from - (((-scaled) + half) / den);
}

/**
 * Plays a swipe from (FLOW_X, FLOW_Y0) to (FLOW_X, y1) through the recognizer
 * and the follower, and checks every outcome of the row. The release is
 * decided lead ms before its stamp, as when the planned stamps run ahead of
 * the clock.
 */
static void flow_replay(const flow_row_t *row, uint32_t lead)
{
    kui_gesture_t g;
    kui_drag_t d;
    uint32_t const moves = (row->ms + FLOW_POLL_MS - 1U) / FLOW_POLL_MS;
    uint32_t const t_lift = FLOW_T0 + row->ms + row->hold;
    uint32_t k;
    bool claimed = false;
    bool opens;

    kui_gesture_press(&g, FLOW_X, FLOW_Y0, KUI_GESTURE_DIR_DOWN, false);
    kui_drag_init(&d, FLOW_SPAN);
    for ( k = 1U; k <= moves; k++ )
    {
        uint32_t at = k * FLOW_POLL_MS;
        int32_t y;

        if ( at > row->ms )
        {
            at = row->ms;
        }
        y = flow_lerp(FLOW_Y0, row->y1, (int32_t)at, (int32_t)row->ms);
        (void)kui_gesture_move(&g, FLOW_X, y);
        if ( KUI_GESTURE_CLAIMED == g.state )
        {
            if ( claimed )
            {
                kui_drag_follow(&d, y, FLOW_T0 + at);
            }
            else
            {
                claimed = true;
                kui_drag_grab(&d, g.cy, FLOW_T0 + at);
            }
        }
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(KUI_GESTURE_CLAIMED, g.state, row->name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(row->claim_cy, g.cy, row->name);

    /* The lift: measured by the recognizer like a move, and fed to the follower before the release. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(KUI_GESTURE_CLAIMED, kui_gesture_release(&g, FLOW_X, row->y1), row->name);
    kui_drag_follow(&d, row->y1, t_lift);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(row->offset, kui_drag_offset(&d), row->name);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(row->speed, kui_drag_speed_px_s(&d, t_lift - lead), row->name);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)row->opens, (int)kui_drag_would_open(&d, t_lift - lead, false), row->name);
    opens = kui_drag_release(&d, t_lift - lead, false);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)row->opens, (int)opens, row->name);
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_SETTLE, kui_drag_phase(&d));
}

TEST_CASE("a swipe through the recognizer and the follower opens or goes back as pinned", "[kui_drag_flow]")
{
    size_t i;
    uint32_t lead;

    /* Once with the release decided at its stamp, once a poll period before it. */
    for ( lead = 0U; FLOW_LEAD_MAX_MS >= lead; lead += FLOW_LEAD_MAX_MS )
    {
        for ( i = 0U; (sizeof(ROWS) / sizeof(ROWS[0])) > i; i++ )
        {
            flow_replay(&ROWS[i], lead);
        }
    }
}
