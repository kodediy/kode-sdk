/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ads1115.c
 * @brief The E5 station's ADC: one single-shot conversion, counts to mV.
 */
#include "kdrv_ads1115.h"

#include <stddef.h>

#include "kbsp_station.h"
#include "kdrv_station_i2c.h"
#include "khal_time.h"

/* Registers and the configuration word (TI SBAS444, 8.6). */
#define REG_CONVERSION   0x00U    /**< The last conversion, signed, high byte first. */
#define REG_CONFIG       0x01U    /**< The configuration register.                  */
#define CFG_OS_START     0x8000U  /**< OS: start one conversion.                    */
#define CFG_MUX_SHIFT    12U      /**< MUX[2:0]'s place.                            */
#define CFG_MUX_SINGLE   0x4U     /**< MUX 100b: AIN0 to GND; AINn is 100b + n.     */
#define CFG_PGA_4V096    0x0200U  /**< PGA 001b: +-4.096 V.                         */
#define CFG_MODE_SINGLE  0x0100U  /**< MODE: single shot, then power down.          */
#define CFG_DR_128SPS    0x0080U  /**< DR 100b: 128 samples a second.               */
#define CFG_COMP_OFF     0x0003U  /**< COMP_QUE 11b: comparator off, ALERT high-Z.  */
#define BYTE_BITS        8U       /**< A register byte's width.                     */
#define CONV_BYTES       2U       /**< A conversion: two bytes.                    */
#define CONV_WAIT_MS     20U      /**< A conversion at 128 SPS takes 7.8 ms; the station waits 20. */

/** @brief The configuration word that starts one conversion of @p ch against ground. */
static uint16_t config_for(uint8_t ch)
{
    const uint16_t mux = (uint16_t)((CFG_MUX_SINGLE + (uint16_t)ch) << CFG_MUX_SHIFT);

    return (uint16_t)(CFG_OS_START | mux | CFG_PGA_4V096 | CFG_MODE_SINGLE | CFG_DR_128SPS | CFG_COMP_OFF);
}

kode_status_t kdrv_ads1115_read_mv(uint8_t ch, int32_t *mv)
{
    kode_status_t st = KODE_DENIED;

    if ( (KDRV_ADS1115_CHANNELS > ch) && (NULL != mv) )
    {
        const uint16_t cfg      = config_for(ch);
        const uint8_t  write[]  = { REG_CONFIG, (uint8_t)(cfg >> BYTE_BITS), (uint8_t)cfg };
        const uint8_t  reg      = REG_CONVERSION;
        uint8_t        raw[CONV_BYTES] = { 0U, 0U };

        st = kdrv_station_i2c_write(KBSP_STATION_ADS1115_ADDR, write, sizeof(write));
        if ( KODE_OK == st )
        {
            khal_delay_ms(CONV_WAIT_MS);
            st = kdrv_station_i2c_write_read(KBSP_STATION_ADS1115_ADDR, &reg, sizeof(reg), raw,
                                             sizeof(raw));
        }
        if ( KODE_OK == st )
        {
            const int16_t counts = (int16_t)(uint16_t)(((uint16_t)raw[0] << BYTE_BITS) | raw[1]);

            *mv = ((int32_t)counts * KDRV_ADS1115_FSR_MV) / KDRV_ADS1115_FULL_CODE;
        }
    }
    return st;
}
