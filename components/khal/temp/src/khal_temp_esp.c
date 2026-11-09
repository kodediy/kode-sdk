/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_temp_esp.c
 * @brief The die temperature on ESP-IDF: the temperature sensor driver, started once and kept.
 */
#include "khal_temp.h"

#include <stddef.h>

#include "driver/temperature_sensor.h"
#include "esp_err.h"

#include "kode_log.h"

#define TAG               "khal.temp"   /**< This file's log tag. */
#define TEMP_RANGE_MIN_C  (-10)         /**< Lowest temperature the sensor is set up for, degrees C. */
#define TEMP_RANGE_MAX_C  80            /**< Highest temperature the sensor is set up for, degrees C. */
#define TEMP_DECI_PER_C   10.0f         /**< Tenths of a degree in one degree. */
#define TEMP_HALF_DECI    0.5f          /**< Half a tenth: rounds to the nearest one. */

static temperature_sensor_handle_t s_tsens;   /**< Installed and enabled by the first read. */

/**
 * @brief Install and enable the sensor, once.
 * @return KODE_OK, or KODE_ERROR with the reason logged.
 */
static kode_status_t temp_up(void)
{
    if ( NULL != s_tsens )
    {
        return KODE_OK;
    }
    temperature_sensor_config_t const cfg =
        TEMPERATURE_SENSOR_CONFIG_DEFAULT(TEMP_RANGE_MIN_C, TEMP_RANGE_MAX_C);
    temperature_sensor_handle_t h = NULL;
    esp_err_t err = temperature_sensor_install(&cfg, &h);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "sensor install: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    err = temperature_sensor_enable(h);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "sensor enable: %s", esp_err_to_name(err));
        (void)temperature_sensor_uninstall(h);
        return KODE_ERROR;
    }
    s_tsens = h;
    return KODE_OK;
}

kode_status_t khal_temp_read(int32_t *deci_c)
{
    if ( NULL == deci_c )
    {
        return KODE_DENIED;
    }
    if ( temp_up() != KODE_OK )
    {
        return KODE_ERROR;
    }
    float celsius = 0.0f;
    esp_err_t const err = temperature_sensor_get_celsius(s_tsens, &celsius);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "sensor read: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    float deci = celsius * TEMP_DECI_PER_C;
    if ( 0.0f <= deci )
    {
        deci += TEMP_HALF_DECI;
    }
    else
    {
        deci -= TEMP_HALF_DECI;
    }
    *deci_c = (int32_t)deci;
    return KODE_OK;
}
