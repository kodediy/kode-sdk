/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ov5640.h
 * @brief The E5 station's camera module: an OV5640 on J3, over DVP, with its SCCB on J3's I2C.
 */
#ifndef KDRV_OV5640_H_
#define KDRV_OV5640_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The loan and the frames ---------------------------------------------- */
#define KDRV_OV5640_TAG          "test_dot" /**< The loan's name in khal_exp's books.          */
#define KDRV_OV5640_FIELD_W      800U       /**< The whole field, scaled by the sensor: width.  */
#define KDRV_OV5640_FIELD_H      600U       /**< The whole field, scaled by the sensor: height. */
#define KDRV_OV5640_SENSOR_W     2592U      /**< The sensor's array, native pixels: width.      */
#define KDRV_OV5640_SENSOR_H     1944U      /**< The sensor's array, native pixels: height.     */
#define KDRV_OV5640_BUFS         3U         /**< Frame buffers in PSRAM while the camera is open. */

/* ---- The settings ---------------------------------------------------------- */
#define KDRV_OV5640_AWB_CHANNELS 3U         /**< White-balance gains: R, G, B.                  */
#define KDRV_OV5640_AWB_X1       0x400U     /**< A white-balance gain of x1.                     */
#define KDRV_OV5640_GAIN_X1      16U        /**< An analog gain of x1 (gain is x16).             */
#define KDRV_OV5640_VCM_MAX      1023U      /**< The lens motor's highest DAC code.              */

/** @brief What a frame carries. */
typedef enum {
    KDRV_OV5640_RGB565 = 0,  /**< Two bytes a pixel, big-endian, the ISP's colour.     */
    KDRV_OV5640_RAW8,        /**< One byte a pixel, the array's Bayer data.            */
} kdrv_ov5640_fmt_t;

/** @brief What the camera captures. */
typedef struct {
    bool              window;  /**< false: the whole field, KDRV_OV5640_FIELD_W x _H; true: a native window. */
    uint16_t          x;       /**< Window: first column of the array.                 */
    uint16_t          y;       /**< Window: first row of the array.                    */
    uint16_t          w;       /**< Window: width; x + w at most KDRV_OV5640_SENSOR_W.  */
    uint16_t          h;       /**< Window: height; y + h at most KDRV_OV5640_SENSOR_H. */
    kdrv_ov5640_fmt_t fmt;     /**< The pixels.                                        */
} kdrv_ov5640_mode_t;

/** @brief The settings the caller asked for; written again after any reset of the sensor. */
typedef struct {
    bool     exp_set;                           /**< Manual exposure, exp_lines.          */
    bool     gain_set;                          /**< Manual gain, gain.                   */
    bool     awb_set;                           /**< Manual white balance, awb.           */
    bool     isp_off;                           /**< The ISP's corrections off.           */
    bool     vcm_set;                           /**< The lens motor at vcm.               */
    uint32_t exp_lines;                         /**< Exposure, in lines.                  */
    uint16_t gain;                              /**< Gain, KDRV_OV5640_GAIN_X1 = x1.       */
    uint16_t awb[KDRV_OV5640_AWB_CHANNELS];     /**< R, G, B gains, KDRV_OV5640_AWB_X1 = x1. */
    uint16_t vcm;                               /**< Lens DAC, 0..KDRV_OV5640_VCM_MAX.    */
    bool     vflip;                             /**< The sensor flips the image vertically. */
    bool     mirror;                            /**< The sensor mirrors the image.        */
} kdrv_ov5640_cfg_t;

/* ---- The calls ------------------------------------------------------------- */

/**
 * @brief Take J3 and find the camera: the plug side, then the module version.
 *
 * Looks on both sides' SCCB pair for pull-ups and a sensor that answers at
 * KBSP_CAM_SCCB_ADDR, until one does or @p budget_ms have passed, then reads
 * the version from where PWDN is pulled down. Found, the loan stays and J3's
 * I2C bus is up (kdrv_ov5640_bus()). Called again while the loan is held, it
 * answers the map it found.
 * J3's supply is the caller's to switch on first. One thread makes every call while the loan
 * lasts: the driver keeps no lock.
 * @param budget_ms The longest the search may take, in ms.
 * @param map       Out: the map found, an index of KBSP_CAM_MAPS.
 * @return KODE_OK; KODE_TIMEOUT when nothing answered, J3 given back;
 *         KODE_DENIED for a NULL @p map or J3 held by someone else;
 *         KODE_NO_RESOURCES when J3's I2C controller is taken; KODE_ERROR
 *         when IDF's I2C driver refused, J3 given back.
 */
kode_status_t kdrv_ov5640_probe(uint32_t budget_ms, uint8_t *map);

/**
 * @brief Start capturing in @p mode, or change the mode of a camera already open.
 *
 * The first open brings the sensor up and takes KDRV_OV5640_BUFS frame
 * buffers from PSRAM; they stay until kdrv_ov5640_close(). A failed open
 * leaves the camera stopped and the loan held: the caller opens again or closes.
 * @param mode The capture; copied.
 * @return KODE_OK; KODE_DENIED before a probe or for a window off the array;
 *         KODE_NO_RESOURCES with no PSRAM for the buffers; KODE_IO_ERROR when
 *         the sensor did not take its registers; KODE_ERROR when the DVP refused.
 */
kode_status_t kdrv_ov5640_open(const kdrv_ov5640_mode_t *mode);

/**
 * @brief Sensor off, buffers freed, J3's bus down and J3 given back. Safe at any time.
 */
void kdrv_ov5640_close(void);

/**
 * @brief Hold the newest frame, after letting @p skip frames go by.
 *
 * A sensor that lost its settings (a reset, a brown-out of the module) gets
 * them written again and the frame after is taken instead. The frame stays
 * the caller's until kdrv_ov5640_release() or the next grab.
 * @param skip       Frames to let go by first.
 * @param timeout_ms The longest wait for each frame, in ms.
 * @param frame      Out: the frame's first byte.
 * @param len        Out: its length in bytes.
 * @return KODE_OK; KODE_DENIED when the camera is not capturing or a pointer
 *         is NULL; KODE_TIMEOUT when no frame came in time.
 */
kode_status_t kdrv_ov5640_grab(uint32_t skip, uint32_t timeout_ms, const uint8_t **frame,
                               size_t *len);

/**
 * @brief Give the held frame back to the DVP.
 */
void kdrv_ov5640_release(void);

/**
 * @brief Keep @p cfg and write it to the sensor now if the camera is open.
 *
 * A change the sensor takes only with its mode's registers (the flips, or a
 * correction going back to automatic) rebuilds the capture.
 * @param cfg The settings; copied.
 * @return KODE_OK, also while closed (the settings wait for the open);
 *         KODE_DENIED for a NULL @p cfg; KODE_IO_ERROR or KODE_ERROR as
 *         kdrv_ov5640_open() when the write or the rebuild failed.
 */
kode_status_t kdrv_ov5640_apply(const kdrv_ov5640_cfg_t *cfg);

/**
 * @brief J3's I2C bus, the one the SCCB runs on, for the station's chips (kdrv only).
 * @return IDF's bus handle from a successful probe until the close; NULL otherwise.
 */
void *kdrv_ov5640_bus(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_OV5640_H_ */
