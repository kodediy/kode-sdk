/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kui_spring.c
 * @brief kui_spring: a damped spring in closed form, from a start to a target.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <math.h>

#include "unity.h"

#include "kui_spring.h"

/** Where every case starts, in pixels. */
#define START_PX 0.0F
/** Where every case goes, in pixels. */
#define TARGET_PX 100.0F
/** The period every case uses, in milliseconds: the drag's. */
#define PERIOD_MS 250U
/** The damping of the underdamped cases that bounce visibly. */
#define ZETA_HALF 0.5F
/** The damping of the underdamped case that bounces a hair. */
#define ZETA_NINE_TENTHS 0.9F
/** A time long past any spring's landing, in milliseconds. */
#define LONG_AFTER_MS 2500U
/** How far the scans for a peak look, in milliseconds. */
#define SCAN_MS 600U
/** How close a float result comes to a hand value given to six digits, in pixels. */
#define HAND_POS_TOL_PX 0.001F
/** How close a float result comes to a hand speed given to six digits, in pixels per millisecond. */
#define HAND_VEL_TOL 0.00001F
/** How close a retarget keeps the position, in pixels. */
#define KEEP_TOL 0.0001F
/** The limits a spring at rest is judged by in the drag: half a pixel... */
#define REST_POS_PX 0.5F
/** ...and 0.05 px/ms. */
#define REST_VEL 0.05F
/** Pi, to the digits a float holds. */
#define TEST_PI 3.14159265F
/** A full turn, to the digits a float holds. */
#define TEST_TAU 6.28318531F
/** The time the clock wrap cases start before the wrap, in milliseconds. */
#define WRAP_T0 0xFFFFFFF0U

/** The number of rows of a table. */
#define ROWS(a) (sizeof(a) / sizeof((a)[0]))

/** What the closed form says at one time. */
typedef struct {
    uint32_t t_ms;  /**< The time since the start. */
    float pos;      /**< The position, in pixels. */
    float vel;      /**< The speed, in pixels per millisecond. */
} hand_row_t;

/** Critical, from rest at 0 toward 100. */
static const hand_row_t CRITICAL_REST[] = {
    {50U, 35.77396F, 0.898875F},
    {125U, 82.10256F, 0.341203F},
    {250U, 98.63991F, 0.029489F},
};

/** Critical, starting at 1 px/ms toward the target: k = v0 + w x0 = -1.51327. */
static const hand_row_t CRITICAL_TOWARD[] = {
    {50U, 50.00443F, 0.825833F},
    {125U, 87.50430F, 0.248657F},
    {250U, 99.10677F, 0.019623F},
};

/** Critical, starting at 1 px/ms away from the target: k = -1 + w x0 = -3.51327. */
static const hand_row_t CRITICAL_AWAY[] = {
    {50U, 21.54348F, 0.971916F},
    {125U, 76.70082F, 0.433750F},
    {250U, 98.17305F, 0.039356F},
};

/** Underdamped with zeta 0.5, from rest. */
static const hand_row_t UNDER_HALF[] = {
    {50U, 47.96121F, 1.371464F},
    {125U, 114.06997F, 0.246487F},
    {250U, 98.98221F, -0.093535F},
};

/** Underdamped with zeta 0.9, from rest. */
static const hand_row_t UNDER_NINE_TENTHS[] = {
    {50U, 37.74893F, 0.969026F},
    {125U, 86.84741F, 0.334238F},
    {250U, 100.03870F, 0.007912F},
};

/** Starts a spring at t=0 on the common start, target and period. */
static void begin(kui_spring_t *s, float vel, float zeta)
{
    kui_spring_start(s, START_PX, vel, TARGET_PX, PERIOD_MS, zeta, 0U);
}

/** The position of a spring at a time. */
static float pos_at(const kui_spring_t *s, uint32_t t_ms)
{
    float pos = 0.0F;
    float vel = 0.0F;

    kui_spring_at(s, t_ms, &pos, &vel);
    return pos;
}

/** The speed of a spring at a time. */
static float vel_at(const kui_spring_t *s, uint32_t t_ms)
{
    float pos = 0.0F;
    float vel = 0.0F;

    kui_spring_at(s, t_ms, &pos, &vel);
    return vel;
}

/** Holds a spring to the rows of a hand-worked table. */
static void check_rows(const kui_spring_t *s, const hand_row_t *rows, size_t n)
{
    size_t i;

    for ( i = 0U; i < n; i++ )
    {
        TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, rows[i].pos, pos_at(s, rows[i].t_ms));
        TEST_ASSERT_FLOAT_WITHIN(HAND_VEL_TOL, rows[i].vel, vel_at(s, rows[i].t_ms));
    }
}

TEST_CASE("a spring started at rest reaches its target and never passes it", "[kui_spring]")
{
    kui_spring_t s;
    float pos = 0.0F;
    float vel = 0.0F;
    float before = START_PX;
    uint32_t t;

    begin(&s, 0.0F, KUI_SPRING_CRITICAL);
    kui_spring_at(&s, 0U, &pos, &vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, START_PX, pos);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, vel);
    for ( t = 1U; LONG_AFTER_MS >= t; t++ )
    {
        kui_spring_at(&s, t, &pos, &vel);
        TEST_ASSERT_TRUE(TARGET_PX >= pos);
        TEST_ASSERT_TRUE(pos >= before);
        TEST_ASSERT_TRUE(0.0F <= vel);
        before = pos;
    }
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, TARGET_PX, pos);
    TEST_ASSERT_FLOAT_WITHIN(HAND_VEL_TOL, 0.0F, vel);
}

TEST_CASE("a spring is at the start at the start, and the fields say where it goes", "[kui_spring]")
{
    kui_spring_t s;

    kui_spring_start(&s, 30.0F, 0.25F, TARGET_PX, PERIOD_MS, ZETA_HALF, 77U);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, TARGET_PX, s.target);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, -70.0F, s.x0);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.25F, s.v0);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, ZETA_HALF, s.zeta);
    TEST_ASSERT_EQUAL_UINT32(77U, s.t0);
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, TEST_TAU / (float)PERIOD_MS, s.w);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 30.0F, pos_at(&s, 77U));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.25F, vel_at(&s, 77U));
}

TEST_CASE("the critical closed form, at three times", "[kui_spring]")
{
    kui_spring_t s;

    begin(&s, 0.0F, KUI_SPRING_CRITICAL);
    check_rows(&s, CRITICAL_REST, ROWS(CRITICAL_REST));
}

TEST_CASE("a start speed toward the target runs ahead of a start from rest", "[kui_spring]")
{
    kui_spring_t s;
    kui_spring_t rest;
    float vel = 0.0F;
    float pos = 0.0F;
    uint32_t t;

    begin(&s, 1.0F, KUI_SPRING_CRITICAL);
    begin(&rest, 0.0F, KUI_SPRING_CRITICAL);
    kui_spring_at(&s, 0U, &pos, &vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, START_PX, pos);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 1.0F, vel);
    check_rows(&s, CRITICAL_TOWARD, ROWS(CRITICAL_TOWARD));
    for ( t = 1U; LONG_AFTER_MS >= t; t++ )
    {
        TEST_ASSERT_TRUE(pos_at(&s, t) >= pos_at(&rest, t));
        TEST_ASSERT_TRUE(pos_at(&s, t) <= TARGET_PX);
    }
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, TARGET_PX, pos_at(&s, LONG_AFTER_MS));
}

TEST_CASE("a start speed away from the target overshoots the start and comes back", "[kui_spring]")
{
    kui_spring_t s;
    float lowest = START_PX;
    float before;
    uint32_t lowest_at = 0U;
    uint32_t t;

    begin(&s, -1.0F, KUI_SPRING_CRITICAL);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, -1.0F, vel_at(&s, 0U));
    check_rows(&s, CRITICAL_AWAY, ROWS(CRITICAL_AWAY));

    /* It goes back past the start first: the speed turns at 11.3 ms, 5.16 px behind it. */
    for ( t = 1U; SCAN_MS >= t; t++ )
    {
        if ( pos_at(&s, t) < lowest )
        {
            lowest = pos_at(&s, t);
            lowest_at = t;
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, -5.15799F, lowest);
    TEST_ASSERT_EQUAL_UINT32(11U, lowest_at);

    /* From the turn it only goes on toward the target, and lands on it without crossing. */
    before = lowest;
    for ( t = lowest_at + 1U; LONG_AFTER_MS >= t; t++ )
    {
        TEST_ASSERT_TRUE(pos_at(&s, t) >= before);
        TEST_ASSERT_TRUE(pos_at(&s, t) <= TARGET_PX);
        before = pos_at(&s, t);
    }
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, TARGET_PX, before);
}

/**
 * Retargets a spring in flight and checks what the retarget owes: the position
 * and the speed it had, the period and the damping it had, a landing on the
 * new target, and the same trajectory for a retarget onto its own target.
 */
static void check_retarget(float zeta)
{
    kui_spring_t s;
    kui_spring_t orig;
    float const moved = 20.0F;
    float const w = TEST_TAU / (float)PERIOD_MS;
    uint32_t const t_ms = 80U;
    float pos_before = 0.0F;
    float vel_before = 0.0F;
    float pos = 0.0F;
    float vel = 0.0F;

    begin(&s, 0.4F, zeta);
    begin(&orig, 0.4F, zeta);
    kui_spring_at(&s, t_ms, &pos_before, &vel_before);

    kui_spring_retarget(&s, moved, t_ms);
    kui_spring_at(&s, t_ms, &pos, &vel);
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, pos_before, pos);
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, vel_before, vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, moved, s.target);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, zeta, s.zeta);
    TEST_ASSERT_FLOAT_WITHIN(1e-6F, w, s.w);
    TEST_ASSERT_EQUAL_UINT32(t_ms, s.t0);

    /* It goes on at the speed it had: one millisecond on is a speed's worth on, less a little acceleration. */
    TEST_ASSERT_FLOAT_WITHIN(0.05F, pos + vel, pos_at(&s, t_ms + 1U));

    /* Sent back to where it was going at the same time, it keeps them too. */
    kui_spring_retarget(&s, TARGET_PX, t_ms);
    kui_spring_at(&s, t_ms, &pos, &vel);
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, pos_before, pos);
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, vel_before, vel);

    /* Sent away and back to its own target, it flies the spring that was never touched. */
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, pos_at(&orig, t_ms + 120U), pos_at(&s, t_ms + 120U));
    TEST_ASSERT_FLOAT_WITHIN(HAND_VEL_TOL, vel_at(&orig, t_ms + 120U), vel_at(&s, t_ms + 120U));

    /* And a spring retargeted elsewhere lands there. */
    kui_spring_retarget(&s, moved, t_ms + 40U);
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, moved, pos_at(&s, t_ms + LONG_AFTER_MS));
}

TEST_CASE("a retarget in flight keeps the position and the speed", "[kui_spring]")
{
    check_retarget(KUI_SPRING_CRITICAL);
    check_retarget(ZETA_HALF);
}

TEST_CASE("a retarget at the start keeps the start", "[kui_spring]")
{
    kui_spring_t s;

    kui_spring_start(&s, 30.0F, -0.5F, TARGET_PX, PERIOD_MS, KUI_SPRING_CRITICAL, 1000U);
    kui_spring_retarget(&s, 10.0F, 1000U);
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, 30.0F, pos_at(&s, 1000U));
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, -0.5F, vel_at(&s, 1000U));

    /* A time before the start is the start: the retarget keeps that, not a position from the past. */
    kui_spring_start(&s, 30.0F, -0.5F, TARGET_PX, PERIOD_MS, KUI_SPRING_CRITICAL, 1000U);
    kui_spring_retarget(&s, 10.0F, 900U);
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, 30.0F, pos_at(&s, 900U));
    TEST_ASSERT_FLOAT_WITHIN(KEEP_TOL, -0.5F, vel_at(&s, 900U));
}

/**
 * Scans a spring from rest for its highest point and holds it to the textbook:
 * the overshoot of a second-order system is e^(-pi zeta / sqrt(1 - zeta^2)) of
 * the distance travelled, and it comes at pi / wd, half a turn of the damped
 * frequency.
 */
static void check_overshoot(float zeta)
{
    kui_spring_t s;
    float const wd = (TEST_TAU / (float)PERIOD_MS) * sqrtf(1.0F - (zeta * zeta));
    float const textbook_px = (TARGET_PX - START_PX) * expf(-((TEST_PI * zeta) / sqrtf(1.0F - (zeta * zeta))));
    float const peak_ms = TEST_PI / wd;
    float highest = START_PX;
    uint32_t highest_at = 0U;
    uint32_t t;

    begin(&s, 0.0F, zeta);
    for ( t = 0U; SCAN_MS >= t; t++ )
    {
        if ( pos_at(&s, t) > highest )
        {
            highest = pos_at(&s, t);
            highest_at = t;
        }
    }
    TEST_ASSERT_FLOAT_WITHIN(0.002F, TARGET_PX + textbook_px, highest);
    TEST_ASSERT_TRUE(((float)highest_at >= (peak_ms - 1.0F)) && ((float)highest_at <= (peak_ms + 1.0F)));

    /* The speed changes sign at the peak, and it settles on the target. */
    TEST_ASSERT_TRUE(vel_at(&s, highest_at - 2U) > 0.0F);
    TEST_ASSERT_TRUE(vel_at(&s, highest_at + 2U) < 0.0F);
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, TARGET_PX, pos_at(&s, LONG_AFTER_MS));
}

TEST_CASE("an underdamped spring overshoots by the textbook amount", "[kui_spring]")
{
    /* zeta 0.5: 16.303 px past the target at 144.3 ms. zeta 0.9: 0.152 px past it at 286.8 ms. */
    check_overshoot(ZETA_HALF);
    check_overshoot(ZETA_NINE_TENTHS);
}

TEST_CASE("the underdamped closed form, at three times", "[kui_spring]")
{
    kui_spring_t s;

    begin(&s, 0.0F, ZETA_HALF);
    check_rows(&s, UNDER_HALF, ROWS(UNDER_HALF));
    begin(&s, 0.0F, ZETA_NINE_TENTHS);
    check_rows(&s, UNDER_NINE_TENTHS, ROWS(UNDER_NINE_TENTHS));
}

TEST_CASE("a damping outside its range is the nearest one", "[kui_spring]")
{
    kui_spring_t critical;
    kui_spring_t over;
    kui_spring_t none;
    kui_spring_t below;

    begin(&critical, 0.3F, KUI_SPRING_CRITICAL);
    begin(&over, 0.3F, 2.0F * KUI_SPRING_CRITICAL);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, KUI_SPRING_CRITICAL, over.zeta);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, pos_at(&critical, 90U), pos_at(&over, 90U));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, vel_at(&critical, 90U), vel_at(&over, 90U));

    /* No damping at all is a swing that goes on: back at the start after a period, at the far side after half. */
    begin(&none, 0.0F, 0.0F);
    begin(&below, 0.0F, -1.0F);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, below.zeta);
    TEST_ASSERT_FLOAT_WITHIN(0.01F, 2.0F * TARGET_PX, pos_at(&none, PERIOD_MS / 2U));
    TEST_ASSERT_FLOAT_WITHIN(0.01F, START_PX, pos_at(&none, PERIOD_MS));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, pos_at(&none, 62U), pos_at(&below, 62U));
}

TEST_CASE("a damping that is not a number is no damping, an infinite one is the critical", "[kui_spring]")
{
    kui_spring_t none;
    kui_spring_t nan_zeta;
    kui_spring_t critical;
    kui_spring_t over;
    kui_spring_t under;
    uint32_t t;

    begin(&none, 0.0F, 0.0F);
    begin(&nan_zeta, 0.0F, NAN);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, nan_zeta.zeta);
    for ( t = 0U; PERIOD_MS >= t; t += 31U )
    {
        TEST_ASSERT_FLOAT_WITHIN(0.0F, pos_at(&none, t), pos_at(&nan_zeta, t));
        TEST_ASSERT_FLOAT_WITHIN(0.0F, vel_at(&none, t), vel_at(&nan_zeta, t));
    }

    begin(&critical, 0.3F, KUI_SPRING_CRITICAL);
    begin(&over, 0.3F, INFINITY);
    begin(&under, 0.3F, -INFINITY);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, KUI_SPRING_CRITICAL, over.zeta);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, pos_at(&critical, 90U), pos_at(&over, 90U));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, under.zeta);
}

TEST_CASE("a period of nothing is a spring that is already there", "[kui_spring]")
{
    kui_spring_t s;
    float pos = 1.0F;
    float vel = 1.0F;

    kui_spring_start(&s, START_PX, 0.7F, TARGET_PX, 0U, ZETA_HALF, 5U);
    kui_spring_at(&s, 5U, &pos, &vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, TARGET_PX, pos);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, TARGET_PX, pos_at(&s, 105U));
    TEST_ASSERT_TRUE(kui_spring_rested(&s, 5U, REST_POS_PX, REST_VEL));
}

TEST_CASE("a retarget of a spring with no period puts it at the new target at once", "[kui_spring]")
{
    kui_spring_t s;
    float pos = 1.0F;
    float vel = 1.0F;

    kui_spring_start(&s, START_PX, 0.7F, TARGET_PX, 0U, ZETA_HALF, 5U);
    kui_spring_retarget(&s, 50.0F, 10U);
    kui_spring_at(&s, 10U, &pos, &vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 50.0F, pos);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 50.0F, pos_at(&s, 110U));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 50.0F, pos_at(&s, LONG_AFTER_MS));
    TEST_ASSERT_TRUE(kui_spring_rested(&s, 10U, REST_POS_PX, REST_VEL));

    /* And again, from a time the clock has not reached. */
    kui_spring_retarget(&s, 70.0F, 4U);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 70.0F, pos_at(&s, 4U));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 70.0F, pos_at(&s, 200U));
}

TEST_CASE("rested holds at its thresholds and not past them", "[kui_spring]")
{
    kui_spring_t s;

    /* At the start the closed form is its own start, exactly, so the thresholds can be met to the bit. */
    kui_spring_start(&s, TARGET_PX + REST_POS_PX, REST_VEL, TARGET_PX, PERIOD_MS, KUI_SPRING_CRITICAL, 0U);
    TEST_ASSERT_TRUE(kui_spring_rested(&s, 0U, REST_POS_PX, REST_VEL));
    TEST_ASSERT_FALSE(kui_spring_rested(&s, 0U, 0.49F, REST_VEL));
    TEST_ASSERT_FALSE(kui_spring_rested(&s, 0U, REST_POS_PX, 0.049F));
    TEST_ASSERT_TRUE(kui_spring_rested(&s, 0U, 0.51F, 0.051F));

    /* Either side of the target, either way of travel. */
    kui_spring_start(&s, TARGET_PX - REST_POS_PX, -REST_VEL, TARGET_PX, PERIOD_MS, KUI_SPRING_CRITICAL, 0U);
    TEST_ASSERT_TRUE(kui_spring_rested(&s, 0U, REST_POS_PX, REST_VEL));
    TEST_ASSERT_FALSE(kui_spring_rested(&s, 0U, 0.49F, REST_VEL));
    TEST_ASSERT_FALSE(kui_spring_rested(&s, 0U, REST_POS_PX, 0.049F));
}

TEST_CASE("a spring from rest comes to rest, a crossing spring is near its target but not at rest", "[kui_spring]")
{
    kui_spring_t s;
    uint32_t t = 0U;

    /* Critical from rest: |x| falls under half a pixel at 296 ms (0.507 px at 295). */
    begin(&s, 0.0F, KUI_SPRING_CRITICAL);
    TEST_ASSERT_FALSE(kui_spring_rested(&s, 0U, REST_POS_PX, REST_VEL));
    while ( (LONG_AFTER_MS > t) && !kui_spring_rested(&s, t, REST_POS_PX, REST_VEL) )
    {
        t++;
    }
    TEST_ASSERT_EQUAL_UINT32(296U, t);
    TEST_ASSERT_TRUE(kui_spring_rested(&s, LONG_AFTER_MS, REST_POS_PX, REST_VEL));

    /* zeta 0.5 passes through the target at about 96.5 ms at 0.75 px/ms: 0.17 px away, and too fast to be at rest. */
    begin(&s, 0.0F, ZETA_HALF);
    TEST_ASSERT_FLOAT_WITHIN(0.2F, TARGET_PX, pos_at(&s, 96U));
    TEST_ASSERT_FALSE(kui_spring_rested(&s, 96U, REST_POS_PX, REST_VEL));
    TEST_ASSERT_TRUE(kui_spring_rested(&s, 96U, REST_POS_PX, 1.0F));
}

/** What a spring started at base does at an offset from it. */
typedef struct {
    float pos;     /**< Where it is. */
    float vel;     /**< How fast it moves. */
    bool rested;   /**< Whether it is at rest by the drag's limits. */
} trace_t;

/** Starts a spring at base and reads it dt later. */
static void trace_from(uint32_t base, uint32_t dt, trace_t *out)
{
    kui_spring_t s;

    kui_spring_start(&s, START_PX, 0.4F, TARGET_PX, PERIOD_MS, ZETA_HALF, base);
    kui_spring_at(&s, base + dt, &out->pos, &out->vel);
    out->rested = kui_spring_rested(&s, base + dt, REST_POS_PX, REST_VEL);
}

TEST_CASE("time wraps", "[kui_spring]")
{
    static const uint32_t DT[] = {0U, 1U, 15U, 16U, 17U, 32U, 125U, 250U, 400U, LONG_AFTER_MS};
    trace_t plain;
    trace_t wrapped;
    size_t i;

    for ( i = 0U; ROWS(DT) > i; i++ )
    {
        trace_from(0U, DT[i], &plain);
        trace_from(WRAP_T0, DT[i], &wrapped);
        TEST_ASSERT_FLOAT_WITHIN(0.0F, plain.pos, wrapped.pos);
        TEST_ASSERT_FLOAT_WITHIN(0.0F, plain.vel, wrapped.vel);
        TEST_ASSERT_EQUAL_INT((int)plain.rested, (int)wrapped.rested);
    }
    TEST_ASSERT_TRUE(wrapped.rested);
}

TEST_CASE("a retarget runs the same across the wrap of the clock", "[kui_spring]")
{
    kui_spring_t plain;
    kui_spring_t wrapped;
    uint32_t dt;

    kui_spring_start(&plain, START_PX, 0.4F, TARGET_PX, PERIOD_MS, ZETA_HALF, 0U);
    kui_spring_start(&wrapped, START_PX, 0.4F, TARGET_PX, PERIOD_MS, ZETA_HALF, WRAP_T0);
    kui_spring_retarget(&plain, 20.0F, 20U);
    kui_spring_retarget(&wrapped, 20.0F, WRAP_T0 + 20U);
    TEST_ASSERT_EQUAL_UINT32(4U, wrapped.t0);
    for ( dt = 0U; SCAN_MS >= dt; dt += 7U )
    {
        TEST_ASSERT_FLOAT_WITHIN(0.0F, pos_at(&plain, 20U + dt), pos_at(&wrapped, WRAP_T0 + 20U + dt));
        TEST_ASSERT_FLOAT_WITHIN(0.0F, vel_at(&plain, 20U + dt), vel_at(&wrapped, WRAP_T0 + 20U + dt));
    }
}

TEST_CASE("the age of a stamp is an unsigned difference, and a stamp ahead of the clock is now", "[kui_spring]")
{
    /* Plain. */
    TEST_ASSERT_EQUAL_UINT32(60U, kui_age_ms(100U, 160U));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_age_ms(100U, 100U));

    /* Across the wrap: 0xFFFFFFF0 to 0x10 is 32 ms. */
    TEST_ASSERT_EQUAL_UINT32(32U, kui_age_ms(WRAP_T0, 0x10U));
    TEST_ASSERT_EQUAL_UINT32(32U, kui_age_ms(WRAP_T0, WRAP_T0 + 32U));

    /* The oldest stamp is just under half the counter; one more millisecond and it is ahead of the clock. */
    TEST_ASSERT_EQUAL_UINT32(0x7FFFFFFFU, kui_age_ms(0U, 0x7FFFFFFFU));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_age_ms(0U, 0x80000000U));

    /* Ahead of the clock, plain and across the wrap: taken now, not 4 billion ms ago. */
    TEST_ASSERT_EQUAL_UINT32(0U, kui_age_ms(1000U, 999U));
    TEST_ASSERT_EQUAL_UINT32(0U, kui_age_ms(5U, 0xFFFFFFFBU));
}

TEST_CASE("a time before the start is the start", "[kui_spring]")
{
    kui_spring_t s;
    kui_spring_t wrapped;

    kui_spring_start(&s, 30.0F, 0.25F, TARGET_PX, PERIOD_MS, KUI_SPRING_CRITICAL, 1000U);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 30.0F, pos_at(&s, 999U));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.25F, vel_at(&s, 999U));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 30.0F, pos_at(&s, 0U));

    /* The same across the wrap: five milliseconds ahead of the clock is not an age of 4 billion. */
    kui_spring_start(&wrapped, 30.0F, 0.25F, TARGET_PX, PERIOD_MS, KUI_SPRING_CRITICAL, 5U);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 30.0F, pos_at(&wrapped, 0xFFFFFFFBU));
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.25F, vel_at(&wrapped, 0xFFFFFFFBU));
    TEST_ASSERT_FALSE(kui_spring_rested(&wrapped, 0xFFFFFFFBU, REST_POS_PX, REST_VEL));
}

TEST_CASE("a missing spring is ignored", "[kui_spring]")
{
    kui_spring_t s;
    float pos = 1.0F;
    float vel = 1.0F;

    kui_spring_start(NULL, START_PX, 0.0F, TARGET_PX, PERIOD_MS, KUI_SPRING_CRITICAL, 0U);
    kui_spring_retarget(NULL, TARGET_PX, 0U);
    kui_spring_at(NULL, 0U, &pos, &vel);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, pos);
    TEST_ASSERT_FLOAT_WITHIN(0.0F, 0.0F, vel);
    kui_spring_at(NULL, 0U, NULL, NULL);
    TEST_ASSERT_FALSE(kui_spring_rested(NULL, 0U, REST_POS_PX, REST_VEL));

    /* A reader that wants one of the two leaves the other out. */
    begin(&s, 0.0F, KUI_SPRING_CRITICAL);
    pos = -1.0F;
    vel = -1.0F;
    kui_spring_at(&s, 50U, &pos, NULL);
    TEST_ASSERT_FLOAT_WITHIN(HAND_POS_TOL_PX, 35.77396F, pos);
    kui_spring_at(&s, 50U, NULL, &vel);
    TEST_ASSERT_FLOAT_WITHIN(HAND_VEL_TOL, 0.898875F, vel);
    kui_spring_at(&s, 50U, NULL, NULL);
}
