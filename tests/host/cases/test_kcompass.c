/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kcompass.c
 * @brief kcompass_heading(): the compass's arithmetic on synthetic fields.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "kbsp_board.h"
#include "kcompass.h"

_Static_assert((KBSP_MAG_AXIS_X == 0U) && (KBSP_MAG_AXIS_Y == 1U) && (KBSP_MAG_AXIS_Z == 2U)
                   && (KBSP_IMU_AXIS_X == 0U) && (KBSP_IMU_AXIS_Y == 1U) && (KBSP_IMU_AXIS_Z == 2U)
                   && (KBSP_MAG_SIGN_X == 1) && (KBSP_MAG_SIGN_Y == 1)
                   && (KBSP_MAG_SIGN_Z == 1) && (KBSP_IMU_SIGN_X == 1)
                   && (KBSP_IMU_SIGN_Y == 1) && (KBSP_IMU_SIGN_Z == 1),
               "these vectors are made for the identity axis map");

#define AXES        3U       /**< Components of a vector.                       */
#define FIELD_H_MG  200.0F   /**< The field's horizontal part, mG.              */
#define FIELD_V_MG  400.0F   /**< Its downward part, mG.                        */
#define PI_F        3.14159265F
#define HALF_TURN   180.0F   /**< Degrees in half a turn.                       */
#define FULL_TURN   360.0F   /**< Degrees in a turn.                            */
#define TOL_DEG     2.0F     /**< The heading's tolerance.                      */
#define TILT_DEG    30.0F    /**< The tilt the case puts on the Dot.            */

/** @brief Degrees to radians. */
static float rad(float deg)
{
    return (deg * PI_F) / HALF_TURN;
}

/** @brief The dot product of two vectors. */
static float dot(const float a[AXES], const float b[AXES])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/**
 * @brief What the chips report for a Dot pointing at @p heading.
 * @param heading Degrees clockwise from north.
 * @param pitch   Degrees the top is raised.
 * @param roll    Degrees the right side is lowered.
 * @param[out] field The field, mG.
 * @param[out] accel The acceleration, g.
 */
static void dot_sees(float heading, float pitch, float bank, float field[AXES], float grav[AXES])
{
    const float world_b[AXES] = { 0.0F, FIELD_H_MG, -FIELD_V_MG };
    const float world_a[AXES] = { 0.0F, 0.0F, 1.0F };
    const float x0[AXES] = { cosf(rad(heading)), -sinf(rad(heading)), 0.0F };
    const float y0[AXES] = { sinf(rad(heading)), cosf(rad(heading)), 0.0F };
    const float z0[AXES] = { 0.0F, 0.0F, 1.0F };
    float       y1[AXES];
    float       z1[AXES];
    float       x2[AXES];
    float       z2[AXES];

    for ( uint32_t i = 0U; AXES > i; i++ )
    {
        y1[i] = (cosf(rad(pitch)) * y0[i]) + (sinf(rad(pitch)) * z0[i]);
        z1[i] = (-sinf(rad(pitch)) * y0[i]) + (cosf(rad(pitch)) * z0[i]);
        x2[i] = (cosf(rad(bank)) * x0[i]) - (sinf(rad(bank)) * z1[i]);
        z2[i] = (sinf(rad(bank)) * x0[i]) + (cosf(rad(bank)) * z1[i]);
    }
    field[0] = dot(world_b, x2);
    field[1] = dot(world_b, y1);
    field[2] = dot(world_b, z2);
    grav[0] = dot(world_a, x2);
    grav[1] = dot(world_a, y1);
    grav[2] = dot(world_a, z2);
}

/** @brief Whether two directions, in degrees, are within @p tol of each other round the circle. */
static bool near_deg(float want, float got, float tol)
{
    float d = fabsf(want - got);

    if ( HALF_TURN < d )
    {
        d = FULL_TURN - d;
    }
    return d <= tol;
}

/** @brief Check the heading for one pose. */
static void expect_heading(float heading, float pitch, float bank)
{
    float field[AXES];
    float grav[AXES];
    float got = -1.0F;

    dot_sees(heading, pitch, bank, field, grav);
    TEST_ASSERT_EQUAL_INT(KCOMPASS_OK, kcompass_heading(field, grav, &got));
    TEST_ASSERT_TRUE_MESSAGE(near_deg(heading, got, TOL_DEG), "the heading is off");
    TEST_ASSERT_TRUE(0.0F <= got);
    TEST_ASSERT_TRUE(FULL_TURN > got);
}

TEST_CASE("heading: north, east, south and west with the Dot flat", "[kcompass]")
{
    expect_heading(0.0F, 0.0F, 0.0F);
    expect_heading(90.0F, 0.0F, 0.0F);
    expect_heading(180.0F, 0.0F, 0.0F);
    expect_heading(270.0F, 0.0F, 0.0F);
    expect_heading(45.0F, 0.0F, 0.0F);
    expect_heading(359.0F, 0.0F, 0.0F);
}

TEST_CASE("heading: the same four with the Dot pitched 30 degrees", "[kcompass]")
{
    expect_heading(0.0F, TILT_DEG, 0.0F);
    expect_heading(90.0F, TILT_DEG, 0.0F);
    expect_heading(180.0F, -TILT_DEG, 0.0F);
    expect_heading(270.0F, -TILT_DEG, 0.0F);
}

TEST_CASE("heading: and rolled 30 degrees, and both at once", "[kcompass]")
{
    expect_heading(0.0F, 0.0F, TILT_DEG);
    expect_heading(90.0F, 0.0F, -TILT_DEG);
    expect_heading(180.0F, TILT_DEG, TILT_DEG);
    expect_heading(270.0F, -TILT_DEG, TILT_DEG);
}

TEST_CASE("heading: a field with no horizontal part has none", "[kcompass]")
{
    const float field[AXES] = { 0.0F, 0.0F, -FIELD_V_MG };
    const float grav[AXES] = { 0.0F, 0.0F, 1.0F };
    float       got = -1.0F;

    TEST_ASSERT_EQUAL_INT(KCOMPASS_WEAK, kcompass_heading(field, grav, &got));
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, got);
}

TEST_CASE("heading: a Dot in free fall or shaken hard has no up", "[kcompass]")
{
    const float field[AXES] = { 0.0F, FIELD_H_MG, -FIELD_V_MG };
    const float falling[AXES] = { 0.0F, 0.0F, 0.1F };
    const float shaken[AXES] = { 0.0F, 0.0F, 3.0F };
    float       got = -1.0F;

    TEST_ASSERT_EQUAL_INT(KCOMPASS_MOVING, kcompass_heading(field, falling, &got));
    TEST_ASSERT_EQUAL_INT(KCOMPASS_MOVING, kcompass_heading(field, shaken, &got));
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, got);
}
