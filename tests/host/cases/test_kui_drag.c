/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_drag.c
 * @brief kui_drag: a page that follows a finger with resistance and lands on a spring.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "kui_drag.h"

/** The span every case uses, in pixels. */
#define SPAN 502
/** Where every case grabs the page when it starts closed. */
#define GRAB_CLOSED 100
/** Where every case grabs the page when it starts open. */
#define GRAB_OPEN 300
/** A time after which the grab's own sample is outside the speed window. */
#define LATER_MS 1000U
/** The time the spring is given to land, from the first release to the last pixel. */
#define LAND_BY_MS 320U
/** The step between two samples of a steady finger. */
#define SAMPLE_MS 10U

/** A closed page grabbed at GRAB_CLOSED at t=0. */
static void grab_closed(kui_drag_t *d)
{
    kui_drag_init(d, SPAN);
    kui_drag_grab(d, GRAB_CLOSED, 0U);
}

/** An open page grabbed at GRAB_OPEN at t=0. */
static void grab_open(kui_drag_t *d)
{
    kui_drag_init(d, SPAN);
    kui_drag_set(d, true);
    kui_drag_grab(d, GRAB_OPEN, 0U);
}

TEST_CASE("resistance holds a third, then lets the page catch the finger", "[kui_drag]")
{
    kui_drag_t d;

    grab_closed(&d);
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_FOLLOW, kui_drag_phase(&d));
    kui_drag_follow(&d, 148, SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(17, kui_drag_offset(&d));
    kui_drag_follow(&d, 204, 2U * SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(70, kui_drag_offset(&d));
    kui_drag_follow(&d, 260, 3U * SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(160, kui_drag_offset(&d));
    kui_drag_follow(&d, 400, 4U * SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(300, kui_drag_offset(&d));
}

TEST_CASE("resistance is the same pulling back from open", "[kui_drag]")
{
    kui_drag_t d;

    grab_open(&d);
    kui_drag_follow(&d, 252, SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(485, kui_drag_offset(&d));
}

TEST_CASE("the page stays between its ends", "[kui_drag]")
{
    kui_drag_t d;

    grab_closed(&d);
    kui_drag_follow(&d, -100, SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_offset(&d));

    grab_open(&d);
    kui_drag_follow(&d, 500, SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(SPAN, kui_drag_offset(&d));
}

TEST_CASE("halfway opens, short of it goes back", "[kui_drag]")
{
    kui_drag_t d;

    grab_closed(&d);
    kui_drag_follow(&d, 351, LATER_MS);
    TEST_ASSERT_TRUE(kui_drag_release(&d, LATER_MS + SAMPLE_MS, false));
    TEST_ASSERT_EQUAL_INT32(251, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_speed_px_s(&d, LATER_MS + SAMPLE_MS));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_SETTLE, kui_drag_phase(&d));

    grab_closed(&d);
    kui_drag_follow(&d, 350, LATER_MS);
    TEST_ASSERT_FALSE(kui_drag_release(&d, LATER_MS + SAMPLE_MS, false));
    TEST_ASSERT_EQUAL_INT32(250, kui_drag_offset(&d));
}

TEST_CASE("a fling decides by its direction", "[kui_drag]")
{
    kui_drag_t d;

    grab_closed(&d);
    kui_drag_follow(&d, 150, 50U);
    TEST_ASSERT_EQUAL_INT32(1000, kui_drag_speed_px_s(&d, 50U));
    TEST_ASSERT_TRUE(kui_drag_release(&d, 50U, false));
    TEST_ASSERT_EQUAL_INT32(18, kui_drag_offset(&d));

    grab_open(&d);
    kui_drag_follow(&d, 250, 50U);
    TEST_ASSERT_EQUAL_INT32(-1000, kui_drag_speed_px_s(&d, 50U));
    TEST_ASSERT_FALSE(kui_drag_release(&d, 50U, false));
    TEST_ASSERT_EQUAL_INT32(484, kui_drag_offset(&d));
}

TEST_CASE("by position, a fling does not count", "[kui_drag]")
{
    kui_drag_t d;

    grab_closed(&d);
    kui_drag_follow(&d, 150, 50U);
    TEST_ASSERT_FALSE(kui_drag_release(&d, 50U, true));
}

TEST_CASE("a finger that rested has no speed", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t t;

    grab_closed(&d);
    for ( t = SAMPLE_MS; 5U * SAMPLE_MS >= t; t += SAMPLE_MS )
    {
        kui_drag_follow(&d, GRAB_CLOSED + (int32_t)t, t);
    }
    kui_drag_follow(&d, 150, 150U);
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_speed_px_s(&d, 160U));
    TEST_ASSERT_FALSE(kui_drag_release(&d, 160U, false));
}

TEST_CASE("the speed sees only the last eighty milliseconds", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t t;
    int32_t pos = GRAB_CLOSED;

    grab_closed(&d);
    for ( t = SAMPLE_MS; 100U >= t; t += SAMPLE_MS )
    {
        pos += 20;
        kui_drag_follow(&d, pos, t);
    }
    TEST_ASSERT_INT32_WITHIN(13, 2000, kui_drag_speed_px_s(&d, 100U));
    for ( t = 100U + SAMPLE_MS; 180U >= t; t += SAMPLE_MS )
    {
        pos += 5;
        kui_drag_follow(&d, pos, t);
    }
    TEST_ASSERT_INT32_WITHIN(13, 500, kui_drag_speed_px_s(&d, 180U));
}

TEST_CASE("the spring lands inside 320 ms and never passes the end", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t t;
    int32_t before = 0;
    bool landed = false;

    kui_drag_init(&d, SPAN);
    kui_drag_aim(&d, true, 0U);
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_SETTLE, kui_drag_phase(&d));
    for ( t = SAMPLE_MS; (LAND_BY_MS >= t) && !landed; t += SAMPLE_MS )
    {
        landed = kui_drag_step(&d, t);
        TEST_ASSERT_TRUE(kui_drag_offset(&d) >= before);
        TEST_ASSERT_TRUE(kui_drag_offset(&d) <= SPAN);
        before = kui_drag_offset(&d);
    }
    TEST_ASSERT_TRUE(landed);
    TEST_ASSERT_EQUAL_INT32(SPAN, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(&d));
    TEST_ASSERT_TRUE(kui_drag_is_open(&d));
}

TEST_CASE("the spring starts at the finger's speed", "[kui_drag]")
{
    kui_drag_t rest;
    kui_drag_t fast;

    /* Both are released at offset 300; only the speed differs. */
    grab_closed(&rest);
    kui_drag_follow(&rest, 400, LATER_MS);
    TEST_ASSERT_TRUE(kui_drag_release(&rest, LATER_MS + SAMPLE_MS, false));
    TEST_ASSERT_FALSE(kui_drag_step(&rest, LATER_MS + 2U * SAMPLE_MS));

    grab_closed(&fast);
    kui_drag_follow(&fast, 385, LATER_MS - SAMPLE_MS);
    kui_drag_follow(&fast, 400, LATER_MS);
    TEST_ASSERT_EQUAL_INT32(1500, kui_drag_speed_px_s(&fast, LATER_MS));
    TEST_ASSERT_TRUE(kui_drag_release(&fast, LATER_MS, false));
    TEST_ASSERT_FALSE(kui_drag_step(&fast, LATER_MS + SAMPLE_MS));

    /* Ten milliseconds after each release: 305.4 against 317.1. */
    TEST_ASSERT_TRUE(kui_drag_offset(&fast) >= (kui_drag_offset(&rest) + 10));
}

TEST_CASE("a page caught in flight follows the finger one to one", "[kui_drag]")
{
    kui_drag_t d;
    int32_t held;

    kui_drag_init(&d, SPAN);
    kui_drag_aim(&d, true, 0U);
    (void)kui_drag_step(&d, 100U);
    kui_drag_catch(&d, 100U);
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_HELD, kui_drag_phase(&d));
    held = kui_drag_offset(&d);
    TEST_ASSERT_TRUE((0 < held) && (SPAN > held));

    TEST_ASSERT_FALSE(kui_drag_step(&d, 150U));
    TEST_ASSERT_EQUAL_INT32(held, kui_drag_offset(&d));

    kui_drag_grab(&d, 200, 160U);
    kui_drag_follow(&d, 248, 170U);
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_FOLLOW, kui_drag_phase(&d));
    TEST_ASSERT_EQUAL_INT32(held + 48, kui_drag_offset(&d));
}

TEST_CASE("a caught page let go without a claim goes on to where it was going", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t t;
    bool landed = false;

    kui_drag_init(&d, SPAN);
    kui_drag_aim(&d, true, 0U);
    (void)kui_drag_step(&d, 100U);
    kui_drag_catch(&d, 100U);
    TEST_ASSERT_TRUE(kui_drag_release(&d, 110U, false));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_SETTLE, kui_drag_phase(&d));
    for ( t = 120U; ((110U + LAND_BY_MS) >= t) && !landed; t += SAMPLE_MS )
    {
        landed = kui_drag_step(&d, t);
    }
    TEST_ASSERT_TRUE(landed);
    TEST_ASSERT_EQUAL_INT32(SPAN, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(&d));
    TEST_ASSERT_TRUE(kui_drag_is_open(&d));
}

TEST_CASE("finish lands at once", "[kui_drag]")
{
    kui_drag_t d;

    kui_drag_init(&d, SPAN);
    kui_drag_aim(&d, true, 0U);
    (void)kui_drag_step(&d, 50U);
    kui_drag_finish(&d);
    TEST_ASSERT_EQUAL_INT32(SPAN, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(&d));
    TEST_ASSERT_TRUE(kui_drag_is_open(&d));

    kui_drag_aim(&d, false, 100U);
    (void)kui_drag_step(&d, 150U);
    kui_drag_catch(&d, 150U);
    kui_drag_finish(&d);
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(&d));
    TEST_ASSERT_FALSE(kui_drag_is_open(&d));
}

/**
 * Drags a page to pos, asks what a release at t_rel would do, releases there
 * and checks that the prediction and the release agree on the expected answer.
 */
static void check_prediction(bool start_open, int32_t grab, int32_t pos, uint32_t t_pos,
                             uint32_t t_rel, bool by_position, bool expect_open)
{
    kui_drag_t d;
    bool predicted;

    kui_drag_init(&d, SPAN);
    kui_drag_set(&d, start_open);
    kui_drag_grab(&d, grab, 0U);
    kui_drag_follow(&d, pos, t_pos);
    predicted = kui_drag_would_open(&d, t_rel, by_position);
    TEST_ASSERT_EQUAL_INT((int)expect_open, (int)predicted);
    TEST_ASSERT_EQUAL_INT((int)predicted, (int)kui_drag_release(&d, t_rel, by_position));
}

TEST_CASE("would-open matches the release", "[kui_drag]")
{
    /* Slow and short of halfway: back. */
    check_prediction(false, GRAB_CLOSED, 200, LATER_MS, LATER_MS + SAMPLE_MS, false, false);
    /* Slow and past halfway: open. */
    check_prediction(false, GRAB_CLOSED, 400, LATER_MS, LATER_MS + SAMPLE_MS, false, true);
    /* A fast flick up that is nowhere near halfway: open by its speed, back by position. */
    check_prediction(false, GRAB_CLOSED, 180, 40U, 40U, false, true);
    check_prediction(false, GRAB_CLOSED, 180, 40U, 40U, true, false);
    /* A fast flick back from open, still past halfway: closed by its speed, open by position. */
    check_prediction(true, 400, 340, 40U, 40U, false, false);
    check_prediction(true, 400, 340, 40U, 40U, true, true);
}

TEST_CASE("would-open outside a drag is the target", "[kui_drag]")
{
    kui_drag_t d;

    kui_drag_init(&d, SPAN);
    TEST_ASSERT_FALSE(kui_drag_would_open(&d, 0U, false));
    kui_drag_set(&d, true);
    TEST_ASSERT_TRUE(kui_drag_would_open(&d, 0U, false));
    kui_drag_aim(&d, false, 0U);
    TEST_ASSERT_FALSE(kui_drag_would_open(&d, 10U, true));
}

/** Drags 50 px in 32 ms starting at base and records what the release says. */
static void drag_from(uint32_t base, bool *opens, int32_t *offset, int32_t *speed)
{
    kui_drag_t d;
    uint32_t const t = base + 32U;

    kui_drag_init(&d, SPAN);
    kui_drag_grab(&d, GRAB_CLOSED, base);
    kui_drag_follow(&d, 150, t);
    *speed = kui_drag_speed_px_s(&d, t);
    *opens = kui_drag_release(&d, t, false);
    *offset = kui_drag_offset(&d);
}

TEST_CASE("time wraps", "[kui_drag]")
{
    bool opens_a;
    bool opens_b;
    int32_t offset_a;
    int32_t offset_b;
    int32_t speed_a;
    int32_t speed_b;

    drag_from(0U, &opens_a, &offset_a, &speed_a);
    drag_from(0xFFFFFFF0u, &opens_b, &offset_b, &speed_b);
    TEST_ASSERT_TRUE(opens_a);
    TEST_ASSERT_EQUAL_INT((int)opens_a, (int)opens_b);
    TEST_ASSERT_EQUAL_INT32(offset_a, offset_b);
    TEST_ASSERT_EQUAL_INT32(speed_a, speed_b);
    TEST_ASSERT_TRUE(1000 < speed_b);
}

TEST_CASE("a stamp from the future does not move it back", "[kui_drag]")
{
    kui_drag_t d;

    grab_closed(&d);
    kui_drag_follow(&d, 300, LATER_MS);
    TEST_ASSERT_FALSE(kui_drag_release(&d, LATER_MS, false));
    TEST_ASSERT_EQUAL_INT32(200, kui_drag_offset(&d));
    TEST_ASSERT_FALSE(kui_drag_step(&d, LATER_MS - SAMPLE_MS));
    TEST_ASSERT_EQUAL_INT32(200, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_SETTLE, kui_drag_phase(&d));
}

TEST_CASE("a sample stamped ahead of the clock is inside the speed window", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t t;

    /* The finger moves 80 px in 80 ms, a flick of 1000 px/s, but the clock is at 79: the newest sample is stamped
     * one millisecond ahead of it and counts as taken now, so the speed is still there. */
    grab_closed(&d);
    for ( t = SAMPLE_MS; 8U * SAMPLE_MS >= t; t += SAMPLE_MS )
    {
        kui_drag_follow(&d, GRAB_CLOSED + (int32_t)t, t);
    }
    TEST_ASSERT_EQUAL_INT32(1000, kui_drag_speed_px_s(&d, 79U));
    TEST_ASSERT_TRUE(kui_drag_would_open(&d, 79U, false));
    /* Short of halfway by position: only the speed opens it. */
    TEST_ASSERT_FALSE(kui_drag_would_open(&d, 79U, true));
    TEST_ASSERT_TRUE(kui_drag_release(&d, 79U, false));

    /* A whole sample period ahead is the same. */
    grab_closed(&d);
    for ( t = SAMPLE_MS; 8U * SAMPLE_MS >= t; t += SAMPLE_MS )
    {
        kui_drag_follow(&d, GRAB_CLOSED + (int32_t)t, t);
    }
    TEST_ASSERT_EQUAL_INT32(1000, kui_drag_speed_px_s(&d, 70U));
}

TEST_CASE("aiming a settling page keeps its speed, aiming a held one starts from rest", "[kui_drag]")
{
    kui_drag_t d;
    int32_t at_turn;

    /* Turned around while still rising, it rises on before it comes back. */
    kui_drag_init(&d, SPAN);
    kui_drag_aim(&d, true, 0U);
    (void)kui_drag_step(&d, 100U);
    at_turn = kui_drag_offset(&d);
    kui_drag_aim(&d, false, 100U);
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_SETTLE, kui_drag_phase(&d));
    (void)kui_drag_step(&d, 110U);
    TEST_ASSERT_TRUE(kui_drag_offset(&d) > at_turn);

    /* From a held page there is no speed to keep: it goes straight back. */
    kui_drag_init(&d, SPAN);
    kui_drag_aim(&d, true, 0U);
    (void)kui_drag_step(&d, 100U);
    kui_drag_catch(&d, 100U);
    kui_drag_aim(&d, false, 100U);
    (void)kui_drag_step(&d, 110U);
    TEST_ASSERT_TRUE(kui_drag_offset(&d) < at_turn);
}

TEST_CASE("a finger landing on a settling page takes it where it is", "[kui_drag]")
{
    kui_drag_t d;

    kui_drag_init(&d, SPAN);
    kui_drag_aim(&d, true, 0U);
    kui_drag_grab(&d, 200, 100U);
    kui_drag_follow(&d, 200, 110U);
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_FOLLOW, kui_drag_phase(&d));
    /* The spring was at 359 px at t=100. */
    TEST_ASSERT_EQUAL_INT32(359, kui_drag_offset(&d));
}

TEST_CASE("a finger grabbing a settling page follows it one to one", "[kui_drag]")
{
    kui_drag_t caught;
    kui_drag_t direct;

    /* The same page, caught first and grabbed straight, at the same moment: 359 px, 143 short of the end. */
    kui_drag_init(&caught, SPAN);
    kui_drag_aim(&caught, true, 0U);
    kui_drag_catch(&caught, 100U);
    kui_drag_grab(&caught, 200, 100U);
    kui_drag_follow(&caught, 210, 110U);

    kui_drag_init(&direct, SPAN);
    kui_drag_aim(&direct, true, 0U);
    kui_drag_grab(&direct, 200, 100U);
    kui_drag_follow(&direct, 210, 110U);

    /* Ten pixels of finger are ten pixels of page, with no resistance on either. */
    TEST_ASSERT_EQUAL_INT32(369, kui_drag_offset(&direct));
    TEST_ASSERT_EQUAL_INT32(kui_drag_offset(&caught), kui_drag_offset(&direct));
}

TEST_CASE("a missing page is ignored", "[kui_drag]")
{
    kui_drag_init(NULL, SPAN);
    kui_drag_set(NULL, true);
    kui_drag_grab(NULL, 0, 0U);
    kui_drag_catch(NULL, 0U);
    kui_drag_follow(NULL, 0, 0U);
    kui_drag_aim(NULL, true, 0U);
    kui_drag_finish(NULL);
    TEST_ASSERT_FALSE(kui_drag_release(NULL, 0U, false));
    TEST_ASSERT_FALSE(kui_drag_step(NULL, 0U));
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_offset(NULL));
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_speed_px_s(NULL, 0U));
    TEST_ASSERT_FALSE(kui_drag_would_open(NULL, 0U, false));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(NULL));
    TEST_ASSERT_FALSE(kui_drag_is_open(NULL));
}

TEST_CASE("the resistance fades along a smoothstep, not a ramp", "[kui_drag]")
{
    kui_drag_t d;

    /* d = 76, a quarter of the way from 48 to 160: 76 * (0.35 + 0.65 * 0.15625) = 34.3, a ramp would give 39. */
    grab_closed(&d);
    kui_drag_follow(&d, 176, SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(34, kui_drag_offset(&d));
}

TEST_CASE("a fling past the end lands when the page crosses it", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t age;
    bool landed = false;

    /* Released at 484 px and 1 px/ms toward open: the spring is 18 px short, crosses the end at 33 ms and would
     * come to rest only at 210 ms, so the crossing is what lands it. */
    grab_closed(&d);
    kui_drag_follow(&d, 544, LATER_MS - 40U);
    kui_drag_follow(&d, 584, LATER_MS);
    TEST_ASSERT_EQUAL_INT32(484, kui_drag_offset(&d));
    TEST_ASSERT_TRUE(kui_drag_release(&d, LATER_MS, false));
    for ( age = 1U; (LAND_BY_MS >= age) && !landed; age++ )
    {
        landed = kui_drag_step(&d, LATER_MS + age);
        TEST_ASSERT_TRUE(kui_drag_offset(&d) <= SPAN);
    }
    age--;
    TEST_ASSERT_TRUE(landed);
    TEST_ASSERT_TRUE((33U <= age) && (34U >= age));
    TEST_ASSERT_EQUAL_INT32(SPAN, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(&d));
}

TEST_CASE("a page that comes to rest lands before the time is up", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t age;
    bool landed = false;

    /* Released at rest 7 px short of the end: |x| falls under half a pixel at 172 ms, the limit is 320. */
    grab_closed(&d);
    kui_drag_follow(&d, 595, LATER_MS);
    TEST_ASSERT_EQUAL_INT32(495, kui_drag_offset(&d));
    TEST_ASSERT_TRUE(kui_drag_release(&d, LATER_MS + SAMPLE_MS, false));
    for ( age = 1U; (LAND_BY_MS >= age) && !landed; age++ )
    {
        landed = kui_drag_step(&d, LATER_MS + SAMPLE_MS + age);
    }
    age--;
    TEST_ASSERT_TRUE(landed);
    TEST_ASSERT_TRUE((150U <= age) && (200U >= age));
    TEST_ASSERT_EQUAL_INT32(SPAN, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(&d));
}

TEST_CASE("the window takes a sample eighty milliseconds old and not one of eighty-one", "[kui_drag]")
{
    kui_drag_t d;
    uint32_t t;

    /* The finger moves 40 px at the start and then rests. Eighty milliseconds back from t=100 is the grab. */
    kui_drag_init(&d, SPAN);
    kui_drag_grab(&d, GRAB_CLOSED, 20U);
    for ( t = 40U; 100U >= t; t += 20U )
    {
        kui_drag_follow(&d, 140, t);
    }
    TEST_ASSERT_EQUAL_INT32(500, kui_drag_speed_px_s(&d, 100U));

    /* The same, with the grab one millisecond older: it is outside, the rest of the window is still. */
    kui_drag_init(&d, SPAN);
    kui_drag_grab(&d, GRAB_CLOSED, 19U);
    for ( t = 40U; 100U >= t; t += 20U )
    {
        kui_drag_follow(&d, 140, t);
    }
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_speed_px_s(&d, 100U));
}

/**
 * Grabs a page at the start (open or closed) at t=0, drags it to pos by t=40 and
 * releases it there by speed. Answers whether it opens, and writes the speed it
 * was released at to *speed.
 */
static bool fling_to(bool start_open, int32_t grab, int32_t pos, int32_t *speed)
{
    kui_drag_t d;

    kui_drag_init(&d, SPAN);
    kui_drag_set(&d, start_open);
    kui_drag_grab(&d, grab, 0U);
    kui_drag_follow(&d, pos, 40U);
    *speed = kui_drag_speed_px_s(&d, 40U);
    return kui_drag_release(&d, 40U, false);
}

TEST_CASE("the fling is 800 px/s: below it the position decides, above it the direction", "[kui_drag]")
{
    int32_t speed = 0;

    /* From closed and a few pixels in, either way the position says back. */
    TEST_ASSERT_FALSE(fling_to(false, GRAB_CLOSED, 128, &speed));
    TEST_ASSERT_EQUAL_INT32(700, speed);
    TEST_ASSERT_TRUE(fling_to(false, GRAB_CLOSED, 136, &speed));
    TEST_ASSERT_EQUAL_INT32(900, speed);

    /* From open and a few pixels back, either way the position says stay. */
    TEST_ASSERT_TRUE(fling_to(true, 400, 372, &speed));
    TEST_ASSERT_EQUAL_INT32(-700, speed);
    TEST_ASSERT_FALSE(fling_to(true, 400, 364, &speed));
    TEST_ASSERT_EQUAL_INT32(-900, speed);
}

/** What a page released at base does 32 ms and a whole landing later. */
typedef struct {
    int32_t off_early;   /**< The offset at the first step. */
    bool landed_early;   /**< Whether the first step landed it. */
    bool landed_late;    /**< Whether the step at the landing time did. */
    int32_t off_late;    /**< The offset after it. */
} spring_trace_t;

/** Grabs, follows and releases at the stamp base, then steps 32 ms and 320 ms on. */
static void spring_from(uint32_t base, spring_trace_t *out)
{
    kui_drag_t d;

    kui_drag_init(&d, SPAN);
    kui_drag_grab(&d, GRAB_CLOSED, base);
    kui_drag_follow(&d, 400, base);
    (void)kui_drag_release(&d, base, false);
    out->landed_early = kui_drag_step(&d, base + 32U);
    out->off_early = kui_drag_offset(&d);
    out->landed_late = kui_drag_step(&d, base + LAND_BY_MS);
    out->off_late = kui_drag_offset(&d);
}

TEST_CASE("the spring runs the same across the wrap of the clock", "[kui_drag]")
{
    spring_trace_t plain;
    spring_trace_t wrapped;

    spring_from(0U, &plain);
    spring_from(0xFFFFFFF0u, &wrapped);
    /* 300 px, 202 px short of the end: 32 ms on it has moved to 339. */
    TEST_ASSERT_FALSE(plain.landed_early);
    TEST_ASSERT_TRUE((300 < plain.off_early) && (SPAN > plain.off_early));
    TEST_ASSERT_EQUAL_INT((int)plain.landed_early, (int)wrapped.landed_early);
    TEST_ASSERT_EQUAL_INT32(plain.off_early, wrapped.off_early);
    TEST_ASSERT_TRUE(plain.landed_late);
    TEST_ASSERT_EQUAL_INT((int)plain.landed_late, (int)wrapped.landed_late);
    TEST_ASSERT_EQUAL_INT32(SPAN, wrapped.off_late);
}

TEST_CASE("a negative span is a page with no room", "[kui_drag]")
{
    kui_drag_t d;

    kui_drag_init(&d, -5);
    kui_drag_grab(&d, GRAB_CLOSED, 0U);
    kui_drag_follow(&d, 300, SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_offset(&d));
    (void)kui_drag_release(&d, 2U * SAMPLE_MS, false);
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_offset(&d));
    (void)kui_drag_step(&d, 3U * SAMPLE_MS);
    TEST_ASSERT_EQUAL_INT32(0, kui_drag_offset(&d));
    TEST_ASSERT_EQUAL_INT(KUI_DRAG_IDLE, kui_drag_phase(&d));
}
