/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ads1115.h
 * @brief The E5 station's ADC: an ADS1115 at KBSP_STATION_ADS1115_ADDR on J3's bus.
 */
#ifndef KDRV_ADS1115_H_
#define KDRV_ADS1115_H_

#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDRV_ADS1115_CHANNELS   4U      /**< Inputs AIN0..AIN3.                               */
#define KDRV_ADS1115_FSR_MV     4096    /**< The range, +-4.096 V: the full-scale code's mV.   */
#define KDRV_ADS1115_FULL_CODE  32768   /**< Codes from zero to full scale (2^15).             */

/**
 * @brief Convert @p ch once against ground and give the result in mV.
 *
 * It rides the camera's J3 bus (kdrv_ov5640_bus()), so kdrv_ov5640_probe() comes first.
 * @param ch The input, 0..KDRV_ADS1115_CHANNELS - 1.
 * @param mv Out: the voltage at the pin, in mV; negative only from offset near 0 V.
 * @return KODE_OK; KODE_DENIED for a bad @p ch, a NULL @p mv or no bus;
 *         KODE_IO_ERROR when the chip did not answer.
 */
kode_status_t kdrv_ads1115_read_mv(uint8_t ch, int32_t *mv);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_ADS1115_H_ */
