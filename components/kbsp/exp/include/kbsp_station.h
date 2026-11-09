/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kbsp_station.h
 * @brief The E5 final-test station on J3: its two chips and the camera module's four pin maps.
 */
#ifndef KBSP_STATION_H_
#define KBSP_STATION_H_

#include <stdint.h>

#include "kbsp_exp.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The station's chips, on J3's own I2C pair with the camera's SCCB ---- */
#define KBSP_STATION_PCF8574_ADDR  0x20U    /**< PCF8574: the four relays.                  */
#define KBSP_STATION_ADS1115_ADDR  0x48U    /**< ADS1115: the pads' voltages.                */
#define KBSP_STATION_I2C_HZ        100000U  /**< The clock of J3's bus, SCCB included, in Hz. */

/* ---- The camera module (OV5640 over DVP) --------------------------------- */
#define KBSP_CAM_SCCB_ADDR   0x3CU  /**< The OV5640's SCCB address.                  */
#define KBSP_CAM_DATA_LINES  8U     /**< DVP data lines, D0..D7.                     */
#define KBSP_CAM_CTRL_LINES  6U     /**< SCL, SDA, PWDN, VSYNC, HREF and PCLK.       */
#define KBSP_CAM_LINES       (KBSP_CAM_CTRL_LINES + KBSP_CAM_DATA_LINES) /**< Every line. */
#define KBSP_CAM_SIDES       2U     /**< The module plugs in upright or turned.      */
#define KBSP_CAM_VERSIONS    2U     /**< Module versions v1.0 and v1.1.              */
#define KBSP_CAM_MAP_COUNT   (KBSP_CAM_SIDES * KBSP_CAM_VERSIONS) /**< One map per version and side. */

#define KBSP_CAM_MAP_V10_UPRIGHT  0U  /**< v1.0, plugged upright.  */
#define KBSP_CAM_MAP_V10_TURNED   1U  /**< v1.0, plugged turned.   */
#define KBSP_CAM_MAP_V11_UPRIGHT  2U  /**< v1.1, plugged upright.  */
#define KBSP_CAM_MAP_V11_TURNED   3U  /**< v1.1, plugged turned.   */

/** @brief Where each of the camera's lines sits, as J3 positions (1..KBSP_EXP_COUNT). */
typedef struct {
    uint8_t scl;                        /**< SCCB clock.                         */
    uint8_t sda;                        /**< SCCB data.                          */
    uint8_t pwdn;                       /**< Power-down, active high; pulled down on the module. */
    uint8_t vsync;                      /**< Frame sync.                         */
    uint8_t href;                       /**< Line valid.                         */
    uint8_t pclk;                       /**< Pixel clock, from the module.       */
    uint8_t d[KBSP_CAM_DATA_LINES];     /**< D0..D7.                             */
} kbsp_cam_map_t;

_Static_assert(KBSP_CAM_LINES == KBSP_EXP_COUNT, "the camera takes the whole header");
_Static_assert(KBSP_CAM_MAP_COUNT == 4U, "four maps: v1.0 and v1.1, upright and turned");
_Static_assert(sizeof(kbsp_cam_map_t) == KBSP_CAM_LINES, "one byte per line, nothing else");

/** @brief The four maps, indexed by KBSP_CAM_MAP_*. */
extern const kbsp_cam_map_t KBSP_CAM_MAPS[KBSP_CAM_MAP_COUNT];

#ifdef __cplusplus
}
#endif

#endif /* KBSP_STATION_H_ */
