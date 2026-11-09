/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_temp.h
 * @brief The chip's own temperature, from its on-die sensor.
 */
#ifndef KHAL_TEMP_H_
#define KHAL_TEMP_H_

#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Read the die temperature. The first call installs and starts the sensor.
 * @param[out] deci_c Tenths of a degree Celsius, written only on KODE_OK.
 * @return KODE_OK; KODE_DENIED for a NULL @p deci_c; KODE_ERROR when the
 *         sensor would not start or answer. The reason is logged.
 */
kode_status_t khal_temp_read(int32_t *deci_c);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_TEMP_H_ */
