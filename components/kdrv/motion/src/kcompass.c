/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kcompass.c
 * @brief kcompass_heading(): see kcompass.h.
 */
#include "kcompass.h"

#include <math.h>
#include <stdint.h>

#include "kbsp_board.h"
#include "kdrv_qmc5883p.h"

/** @brief The least acceleration, g, that still says which way is up. */
#define KCOMPASS_G_MIN 0.5F

/** @brief The most acceleration, g, that still says which way is up. */
#define KCOMPASS_G_MAX 1.5F

/** @brief The least horizontal field, mG, that still says which way is north. */
#define KCOMPASS_FIELD_MIN_MG 20.0F

/** @brief Degrees in one radian. */
#define KCOMPASS_DEG_PER_RAD 57.2957795F

/** @brief Degrees in a turn. */
#define KCOMPASS_TURN 360.0F

/** @brief The dot product of two vectors of KDRV_QMC5883P_AXES. */
static float vec_dot(const float a[KDRV_QMC5883P_AXES], const float b[KDRV_QMC5883P_AXES])
{
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

kcompass_rc_t kcompass_heading(const float field_mg[KDRV_QMC5883P_AXES],
                               const float accel_g[KDRV_QMC5883P_AXES], float *deg)
{
    const float b[KDRV_QMC5883P_AXES] = {
        (float)KBSP_MAG_SIGN_X * field_mg[KBSP_MAG_AXIS_X],
        (float)KBSP_MAG_SIGN_Y * field_mg[KBSP_MAG_AXIS_Y],
        (float)KBSP_MAG_SIGN_Z * field_mg[KBSP_MAG_AXIS_Z]
    };
    const float a[KDRV_QMC5883P_AXES] = {
        (float)KBSP_IMU_SIGN_X * accel_g[KBSP_IMU_AXIS_X],
        (float)KBSP_IMU_SIGN_Y * accel_g[KBSP_IMU_AXIS_Y],
        (float)KBSP_IMU_SIGN_Z * accel_g[KBSP_IMU_AXIS_Z]
    };
    const float    g  = sqrtf(vec_dot(a, a));
    kcompass_rc_t rc = KCOMPASS_MOVING;

    if ( (KCOMPASS_G_MIN <= g) && (KCOMPASS_G_MAX >= g) )
    {
        const float up[KDRV_QMC5883P_AXES] = { a[0] / g, a[1] / g, a[2] / g };
        const float along = vec_dot(b, up);
        const float h[KDRV_QMC5883P_AXES] = {
            b[0] - (along * up[0]), b[1] - (along * up[1]), b[2] - (along * up[2])
        };

        rc = KCOMPASS_WEAK;
        if ( sqrtf(vec_dot(h, h)) >= KCOMPASS_FIELD_MIN_MG )
        {
            /* North is h; east is h x up; the top (board y) is measured on both. */
            const float east_y = (h[2] * up[0]) - (h[0] * up[2]);
            float       turn   = atan2f(east_y, h[1]) * KCOMPASS_DEG_PER_RAD;

            if ( 0.0F > turn )
            {
                turn += KCOMPASS_TURN;
            }
            if ( KCOMPASS_TURN <= turn )
            {
                turn -= KCOMPASS_TURN;
            }
            *deg = turn;
            rc   = KCOMPASS_OK;
        }
    }
    return rc;
}
