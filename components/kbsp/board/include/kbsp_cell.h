/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_cell.h
 * @brief The battery as the board sees it: shunt, charge profile, OCV curve.
 */
#ifndef KBSP_CELL_H_
#define KBSP_CELL_H_

#define KBSP_CELL_RSENSE_MOHM     10U    /**< R75, the gauge's shunt          */
#define KBSP_CELL_R_INT_MOHM      180U   /**< For OCV = V - I * R_int         */

/* ---- Charge profile (WW603048, 1000 mAh) --------------------------------- */
#define KBSP_CELL_CAPACITY_MAH    1000U
#define KBSP_CELL_VCUT_MV         3000U  /**< Discharge cut-off               */
#define KBSP_CELL_VTRIP_MV        4280U  /**< Protection board's OV trip      */
#define KBSP_CELL_VREG_MV         4192U  /**< Charge voltage, 8 mV under nominal */
#define KBSP_CELL_ICHG_MA         512U   /**< Fast charge, 0.5C               */
#define KBSP_CELL_IPRECHG_MA      64U    /**< Pre-charge, the PMIC's floor    */
#define KBSP_CELL_ITERM_MA        64U    /**< Termination, the PMIC's floor   */
#define KBSP_CELL_ICHG_MAX_MA     896U   /**< Most `charge_ma` may ask for    */

/* ---- A cell run flat ----------------------------------------------------- */
/* On battery, under the first the 3.3 V rail is about to fall out of regulation
 * and the board goes to ship mode rather than brown out. The second is the same
 * call for a boot that follows a brownout: that cell has just shown it cannot
 * carry the board, so it gets a lower bar to stop the loop. Both are estimates
 * from the OCV table below, to be checked on a real cell. */
#define KBSP_CELL_CUT_MV          3300U  /**< Under this, on battery: ship mode */
#define KBSP_CELL_DIP_CUT_MV      3500U  /**< The same, right after a brownout  */

/* ---- Boost ---------------------------------------------------------------- */
#define KBSP_CELL_BOOST_LIM_CODE  0U     /**< BQ25896 BOOST_LIM: 0.5 A        */
#define KBSP_CELL_BOOST_HOT_CODE  1U     /**< BQ25896 BHOT: about 55 C        */
#define KBSP_CELL_BOOST_MIN_MV    3600U  /**< Under this, `boost on` is refused */
#define KBSP_CELL_BOOST_STOP_MV   3300U  /**< Under this, a running boost stops */

/* ---- Open-circuit voltage to percentage (feed an IR-compensated voltage) - */
#define KBSP_CELL_OCV_POINTS      13U
#define KBSP_CELL_OCV_MV  { 3300U, 3500U, 3600U, 3680U, 3740U, 3800U, 3850U, \
                            3920U, 3980U, 4040U, 4100U, 4150U, 4200U }
#define KBSP_CELL_OCV_PCT { 0U, 5U, 11U, 19U, 30U, 42U, 53U, 65U, 75U, 84U, \
                            90U, 96U, 100U }

#endif /* KBSP_CELL_H_ */
