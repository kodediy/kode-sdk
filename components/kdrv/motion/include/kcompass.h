/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kcompass.h
 * @brief A tilt-compensated heading from the field and gravity, on the board's axes.
 */
#ifndef KCOMPASS_H_
#define KCOMPASS_H_

#include "kdrv_qmc5883p.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Why a heading could not be worked out. */
typedef enum {
    KCOMPASS_OK = 0,    /**< There is one.                                     */
    KCOMPASS_MOVING,    /**< The Dot is accelerating: gravity is not up.       */
    KCOMPASS_WEAK       /**< The field has no horizontal part worth following. */
} kcompass_rc_t;

/**
 * @brief The direction the Dot's top points, tilt-compensated.
 *
 * Both vectors are on the chips' own axes; the axis map puts them on the
 * board's (x right, y top, z out of the glass). Gravity gives up, the field's
 * part across it gives north.
 *
 * @param field_mg The calibrated field, mG.
 * @param accel_g  The acceleration, g.
 * @param[out] deg Degrees clockwise from north, 0 up to 360; written on KCOMPASS_OK.
 * @return KCOMPASS_OK, or why not.
 */
kcompass_rc_t kcompass_heading(const float field_mg[KDRV_QMC5883P_AXES],
                               const float accel_g[KDRV_QMC5883P_AXES], float *deg);

#ifdef __cplusplus
}
#endif

#endif /* KCOMPASS_H_ */
