/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kcompass.c
 * @brief kcompass_heading(): the compass's arithmetic on synthetic fields.
 *
 * Each case describes a pose on the board's axes (x right, y top, z out of the
 * glass) and turns it into what the chips read through the inverse of
 * kbsp_board.h's axis map, so the cases hold for any map.
 */
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "unity.h"

#include "kbsp_board.h"
#include "kcompass.h"

#define AXES        3U       /**< Components of a vector.                             */
#define FIELD_H_MG  250.0F   /**< The field's horizontal part, mG: Madrid's 25 uT.    */
#define FIELD_V_MG  370.0F   /**< Its downward part, mG: Madrid's 37 uT.              */
#define PI_F        3.14159265F
#define HALF_TURN   180.0F   /**< Degrees in half a turn.                             */
#define FULL_TURN   360.0F   /**< Degrees in a turn.                                  */
#define TOL_DEG     0.5F     /**< The heading's tolerance.                            */
#define BOTH_DEG    20.0F    /**< The tilt the case puts on both axes at once.        */
#define FALLING_G   0.1F     /**< An acceleration too small to say which way is up.   */
#define SHAKEN_G    3.0F     /**< One too large to.                                   */
#define WHY_LEN     96U      /**< Room for a failing pose's description.              */

/** @brief The tilts the cases put on one axis, degrees. */
static const float s_tilts[] = { 2.0F, 5.0F, 10.0F, 20.0F };

/** @brief The headings they are put at, degrees clockwise from north. */
static const float s_headings[] = { 0.0F, 45.0F, 90.0F, 135.0F, 200.0F, 270.0F, 315.0F };

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
 * @brief What the QMI8658A reads for a vector on the board's axes.
 * @param board The vector, board's axes.
 * @param[out] chip The same vector, the chip's axes.
 */
static void imu_chip(const float board[AXES], float chip[AXES])
{
    chip[KBSP_IMU_AXIS_X] = (float)KBSP_IMU_SIGN_X * board[0];
    chip[KBSP_IMU_AXIS_Y] = (float)KBSP_IMU_SIGN_Y * board[1];
    chip[KBSP_IMU_AXIS_Z] = (float)KBSP_IMU_SIGN_Z * board[2];
}

/**
 * @brief What the QMC5883P reads for a vector on the board's axes.
 * @param board The vector, board's axes.
 * @param[out] chip The same vector, the chip's axes.
 */
static void mag_chip(const float board[AXES], float chip[AXES])
{
    chip[KBSP_MAG_AXIS_X] = (float)KBSP_MAG_SIGN_X * board[0];
    chip[KBSP_MAG_AXIS_Y] = (float)KBSP_MAG_SIGN_Y * board[1];
    chip[KBSP_MAG_AXIS_Z] = (float)KBSP_MAG_SIGN_Z * board[2];
}

/**
 * @brief What the chips read for a Dot at rest in Madrid's field.
 * @param heading Degrees clockwise from north the top points to.
 * @param pitch   Degrees the top is raised.
 * @param roll    Degrees the right side is lowered.
 * @param[out] field The field, mG, the QMC5883P's axes.
 * @param[out] grav  The acceleration, g, the QMI8658A's axes.
 */
static void dot_sees(float heading, float pitch, float roll, float field[AXES], float grav[AXES])
{
    /* The world: x east, y north, z up. */
    const float world_b[AXES] = { 0.0F, FIELD_H_MG, -FIELD_V_MG };
    const float world_a[AXES] = { 0.0F, 0.0F, 1.0F };
    const float x0[AXES] = { cosf(rad(heading)), -sinf(rad(heading)), 0.0F };
    const float y0[AXES] = { sinf(rad(heading)), cosf(rad(heading)), 0.0F };
    const float z0[AXES] = { 0.0F, 0.0F, 1.0F };
    float       y1[AXES];
    float       z1[AXES];
    float       x2[AXES];
    float       z2[AXES];
    float       board_b[AXES];
    float       board_a[AXES];

    for ( uint32_t i = 0U; AXES > i; i++ )
    {
        y1[i] = (cosf(rad(pitch)) * y0[i]) + (sinf(rad(pitch)) * z0[i]);
        z1[i] = (-sinf(rad(pitch)) * y0[i]) + (cosf(rad(pitch)) * z0[i]);
        x2[i] = (cosf(rad(roll)) * x0[i]) - (sinf(rad(roll)) * z1[i]);
        z2[i] = (sinf(rad(roll)) * x0[i]) + (cosf(rad(roll)) * z1[i]);
    }
    board_b[0] = dot(world_b, x2);
    board_b[1] = dot(world_b, y1);
    board_b[2] = dot(world_b, z2);
    board_a[0] = dot(world_a, x2);
    board_a[1] = dot(world_a, y1);
    board_a[2] = dot(world_a, z2);
    mag_chip(board_b, field);
    imu_chip(board_a, grav);
}

/** @brief How far @p got is from @p want round the circle, degrees, -180 to 180. */
static float off_deg(float want, float got)
{
    float d = got - want;

    if ( HALF_TURN < d )
    {
        d -= FULL_TURN;
    }
    if ( -HALF_TURN > d )
    {
        d += FULL_TURN;
    }
    return d;
}

/** @brief Check the heading for one pose. */
static void expect_heading(float heading, float pitch, float roll)
{
    float field[AXES];
    float grav[AXES];
    float got = -1.0F;
    char  why[WHY_LEN];

    dot_sees(heading, pitch, roll, field, grav);
    TEST_ASSERT_EQUAL_INT(KCOMPASS_OK, kcompass_heading(field, grav, &got));
    (void)snprintf(why, sizeof why, "heading %.0f pitch %.0f roll %.0f gave %.1f",
                   (double)heading, (double)pitch, (double)roll, (double)got);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(TOL_DEG, 0.0F, off_deg(heading, got), why);
    TEST_ASSERT_TRUE_MESSAGE(0.0F <= got, why);
    TEST_ASSERT_TRUE_MESSAGE(FULL_TURN > got, why);
}

TEST_CASE("the maps put the QMC5883P a quarter turn from the QMI8658A about z", "[kcompass]")
{
    static const float board[AXES][AXES] = { { 1.0F, 0.0F, 0.0F }, { 0.0F, 1.0F, 0.0F }, { 0.0F, 0.0F, 1.0F } };

    for ( uint32_t k = 0U; AXES > k; k++ )
    {
        float imu[AXES];
        float mag[AXES];

        imu_chip(board[k], imu);
        mag_chip(board[k], mag);
        /* As measured on a board: the QMC5883P's x is the QMI8658A's y, its y the QMI8658A's -x. */
        TEST_ASSERT_EQUAL_FLOAT(imu[1], mag[0]);
        TEST_ASSERT_EQUAL_FLOAT(-imu[0], mag[1]);
        TEST_ASSERT_EQUAL_FLOAT(imu[2], mag[2]);
    }
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

TEST_CASE("heading: the top raised or lowered 2, 5, 10 and 20 degrees", "[kcompass]")
{
    for ( uint32_t h = 0U; (sizeof s_headings / sizeof s_headings[0]) > h; h++ )
    {
        for ( uint32_t t = 0U; (sizeof s_tilts / sizeof s_tilts[0]) > t; t++ )
        {
            expect_heading(s_headings[h], s_tilts[t], 0.0F);
            expect_heading(s_headings[h], -s_tilts[t], 0.0F);
        }
    }
}

TEST_CASE("heading: the right side lowered or raised 2, 5, 10 and 20 degrees", "[kcompass]")
{
    for ( uint32_t h = 0U; (sizeof s_headings / sizeof s_headings[0]) > h; h++ )
    {
        for ( uint32_t t = 0U; (sizeof s_tilts / sizeof s_tilts[0]) > t; t++ )
        {
            expect_heading(s_headings[h], 0.0F, s_tilts[t]);
            expect_heading(s_headings[h], 0.0F, -s_tilts[t]);
        }
    }
}

TEST_CASE("heading: pitched and rolled at once", "[kcompass]")
{
    expect_heading(0.0F, BOTH_DEG, BOTH_DEG);
    expect_heading(90.0F, -BOTH_DEG, BOTH_DEG);
    expect_heading(180.0F, BOTH_DEG, -BOTH_DEG);
    expect_heading(270.0F, -BOTH_DEG, -BOTH_DEG);
}

TEST_CASE("heading: a field with no horizontal part has none", "[kcompass]")
{
    const float board_b[AXES] = { 0.0F, 0.0F, -FIELD_V_MG };
    const float board_a[AXES] = { 0.0F, 0.0F, 1.0F };
    float       field[AXES];
    float       grav[AXES];
    float       got = -1.0F;

    mag_chip(board_b, field);
    imu_chip(board_a, grav);
    TEST_ASSERT_EQUAL_INT(KCOMPASS_WEAK, kcompass_heading(field, grav, &got));
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, got);
}

TEST_CASE("heading: a Dot in free fall or shaken hard has no up", "[kcompass]")
{
    const float board_b[AXES] = { 0.0F, FIELD_H_MG, -FIELD_V_MG };
    const float board_falling[AXES] = { 0.0F, 0.0F, FALLING_G };
    const float board_shaken[AXES] = { 0.0F, 0.0F, SHAKEN_G };
    float       field[AXES];
    float       falling[AXES];
    float       shaken[AXES];
    float       got = -1.0F;

    mag_chip(board_b, field);
    imu_chip(board_falling, falling);
    imu_chip(board_shaken, shaken);
    TEST_ASSERT_EQUAL_INT(KCOMPASS_MOVING, kcompass_heading(field, falling, &got));
    TEST_ASSERT_EQUAL_INT(KCOMPASS_MOVING, kcompass_heading(field, shaken, &got));
    TEST_ASSERT_EQUAL_FLOAT(-1.0F, got);
}
