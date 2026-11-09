/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_pcf8574.c
 * @brief The E5 station's relay expander, the forbidden byte refused before the wire.
 */
#include "kdrv_pcf8574.h"

#include <stddef.h>

#include "kbsp_station.h"
#include "kdrv_station_i2c.h"
#include "kode_log.h"

#define TAG "kdrv.pcf8574"

#define RELAY_PAIR_MASK  (KDRV_PCF8574_R1_BIT | KDRV_PCF8574_R2_BIT) /**< The two relays the rule reads. */
#define RELAY_FORBIDDEN  KDRV_PCF8574_R1_BIT  /**< In the pair: R1 at rest (1), R2 on (0). */

kode_status_t kdrv_pcf8574_write(uint8_t byte)
{
    kode_status_t st = KODE_DENIED;

    if ( RELAY_FORBIDDEN == (byte & RELAY_PAIR_MASK) )
    {
        KLOG_W(TAG, "0x%02x refused: R2 on with R1 at rest", (unsigned)byte);
    }
    else
    {
        st = kdrv_station_i2c_write(KBSP_STATION_PCF8574_ADDR, &byte, sizeof(byte));
    }
    return st;
}

kode_status_t kdrv_pcf8574_read(uint8_t *byte)
{
    kode_status_t st = KODE_DENIED;

    if ( NULL != byte )
    {
        st = kdrv_station_i2c_read(KBSP_STATION_PCF8574_ADDR, byte, sizeof(*byte));
    }
    return st;
}

bool kdrv_station_present(void)
{
    const bool relays = kdrv_station_i2c_probe(KBSP_STATION_PCF8574_ADDR);
    const bool adc    = kdrv_station_i2c_probe(KBSP_STATION_ADS1115_ADDR);

    return relays && adc;
}
