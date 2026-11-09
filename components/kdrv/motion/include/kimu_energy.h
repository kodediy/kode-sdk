/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kimu_energy.h
 * @brief Energy at one frequency in an accelerometer window, computed with no chip beneath it.
 */
#ifndef KIMU_ENERGY_H_
#define KIMU_ENERGY_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Fewer samples than this is not a window. */
#define KIMU_ENERGY_MIN_N   32U

/** @brief What one window amounted to. */
typedef struct {
    int32_t bin_x10;   /**< Amplitude of a sinusoid at the asked frequency on
                        *   the loudest axis, 0.1 mg.                        */
    int32_t rms_x10;   /**< RMS over the three axes with their means removed,
                        *   0.1 mg.                                          */
    uint8_t axis;      /**< 0 x, 1 y, 2 z: the axis that carried the bin.    */
} kimu_energy_t;

/**
 * @brief Measure @p n interleaved x,y,z samples at @p hz.
 *
 * @param xyz        Raw LSB, x0 y0 z0 x1 y1 z1 ...
 * @param n          Samples per axis, at least KIMU_ENERGY_MIN_N.
 * @param hz         The frequency to measure.
 * @param fs_hz      The sample rate the chip batched at.
 * @param ug_per_lsb Micro-g per LSB at the full scale used (244 at +-8 g).
 * @param out        Filled on KODE_OK, untouched otherwise.
 * @return KODE_OK, or KODE_DENIED for a NULL or a window too short.
 */
kode_status_t kimu_energy(const int16_t *xyz, size_t n, uint16_t hz, uint16_t fs_hz,
                          uint16_t ug_per_lsb, kimu_energy_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KIMU_ENERGY_H_ */
