/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_exp_esp.c
 * @brief khal_exp's port on ESP-IDF: ADC1 oneshot with a scheme per channel.
 */
#include "khal_exp_port.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_err.h"
#include "hal/spi_types.h"
#include "soc/soc_caps.h"

#include "kbsp_board.h"
#include "kbsp_exp.h"
#include "kode_log.h"

#define TAG "khal.exp"   /**< This file's log tag. */

/* The controllers an app may take for J3 (kbsp_exp.h) against the chip and
 * against kodeOS's own (kbsp_board.h): an app is never handed one that does
 * not exist or that a driver here already drives. */
_Static_assert(KBSP_EXP_I2C_ID < (uint32_t)SOC_HP_I2C_NUM,
               "KBSP_EXP_I2C_ID must name one of the chip's HP I2C controllers");
_Static_assert(KBSP_EXP_I2C_ID != KBSP_I2C_BOARD,
               "the app's I2C must not be the board bus");
_Static_assert((KBSP_EXP_SPI_ID > (uint32_t)SPI1_HOST)
               && (KBSP_EXP_SPI_ID < (uint32_t)SOC_SPI_PERIPH_NUM),
               "KBSP_EXP_SPI_ID must name a general SPI host, not the flash's");
_Static_assert(KBSP_EXP_SPI_ID != KBSP_SPI_NFC,
               "the app's SPI must not be the NFC reader's");
_Static_assert(KBSP_EXP_UART_LAST < (uint32_t)SOC_UART_HP_NUM,
               "KBSP_EXP_UART_LAST must name one of the chip's HP UARTs");

#define ADC_OPEN_BITS  8U   /**< Bits in s_adc_open: one per ADC channel. */
_Static_assert((uint32_t)SOC_ADC_MAX_CHANNEL_NUM <= ADC_OPEN_BITS,
               "s_adc_open is a byte: one bit per ADC channel");

#define EXP_ADC_UNIT    ((adc_unit_t)KBSP_ADC_EXP) /**< The unit every analog position is on. */
_Static_assert(KBSP_ADC_EXP == (uint32_t)ADC_UNIT_1,
               "J3's analog positions are ADC1's channels (kbsp_exp.c)");
#define EXP_ADC_ATTEN   ADC_ATTEN_DB_12       /**< Attenuation: the widest input range. */
#define EXP_ADC_BITS    ADC_BITWIDTH_12       /**< Conversion width. */
#define EXP_ADC_FS_MV   3100                  /**< The 12 dB range's nominal full scale, for a read with no scheme. */
#define EXP_ADC_FS_RAW  4095                  /**< The count at that full scale. */

static adc_oneshot_unit_handle_t s_adc;        /**< ADC1, created on the first open and kept. */
static uint8_t                   s_adc_open;   /**< Bit n set: channel n is open. */

#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
static adc_cali_handle_t s_cali[SOC_ADC_MAX_CHANNEL_NUM];   /**< Each channel's scheme, or NULL. */
#endif

/**
 * @brief Whether @p chan is a channel this unit has, and a bit in s_adc_open.
 * @param chan The ADC1 channel as kbsp's table spells it.
 * @return false when it is off the end of the unit.
 */
static bool chan_ok(uint8_t chan)
{
    return ((uint8_t)SOC_ADC_MAX_CHANNEL_NUM > chan);
}

/**
 * @brief Give @p chan its own calibration scheme, or leave it without one.
 * @param chan The ADC1 channel, already checked.
 */
static void cali_open(uint8_t chan)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if ( NULL != s_cali[chan] )
    {
        return;
    }
    const adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = EXP_ADC_UNIT,
        .chan     = (adc_channel_t)chan,
        .atten    = EXP_ADC_ATTEN,
        .bitwidth = EXP_ADC_BITS,
    };
    if ( adc_cali_create_scheme_curve_fitting(&cfg, &s_cali[chan]) != ESP_OK )
    {
        s_cali[chan] = NULL;
        KLOG_W(TAG, "ADC1 channel %u has no scheme; raw counts only",
               (unsigned)chan);
    }
#else
    (void)chan;
#endif
}

/**
 * @brief Take @p chan's calibration scheme back down.
 * @param chan The ADC1 channel, already checked.
 */
static void cali_close(uint8_t chan)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    if ( NULL != s_cali[chan] )
    {
        (void)adc_cali_delete_scheme_curve_fitting(s_cali[chan]);
        s_cali[chan] = NULL;
    }
#else
    (void)chan;
#endif
}

/**
 * @brief Turn a count into millivolts through @p chan's scheme.
 * @param chan The ADC1 channel, already checked.
 * @param raw  The count the conversion returned.
 * @param mv   Written only when this answers true.
 * @return false when the channel has no scheme, or the scheme refused.
 */
static bool cali_mv(uint8_t chan, int raw, int32_t *mv)
{
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    int volts = 0;

    if ( NULL == s_cali[chan] )
    {
        return false;
    }
    if ( adc_cali_raw_to_voltage(s_cali[chan], raw, &volts) != ESP_OK )
    {
        return false;
    }
    *mv = (int32_t)volts;
    return true;
#else
    (void)chan;
    (void)raw;
    (void)mv;
    return false;
#endif
}

kode_status_t khal_exp_port_adc_open(uint8_t chan)
{
    if ( !chan_ok(chan) )
    {
        KLOG_E(TAG, "ADC1 has no channel %u", (unsigned)chan);
        return KODE_DENIED;
    }
    if ( NULL == s_adc )
    {
        const adc_oneshot_unit_init_cfg_t unit = {
            .unit_id  = EXP_ADC_UNIT,
            .ulp_mode = ADC_ULP_MODE_DISABLE,
        };
        if ( adc_oneshot_new_unit(&unit, &s_adc) != ESP_OK )
        {
            s_adc = NULL;
            KLOG_E(TAG, "no ADC1 unit");
            return KODE_ERROR;
        }
    }
    const adc_oneshot_chan_cfg_t cfg = {
        .atten    = EXP_ADC_ATTEN,
        .bitwidth = EXP_ADC_BITS,
    };
    if ( adc_oneshot_config_channel(s_adc, (adc_channel_t)chan, &cfg)
        != ESP_OK )
    {
        KLOG_E(TAG, "ADC1 refused channel %u", (unsigned)chan);
        return KODE_ERROR;
    }
    cali_open(chan);
    s_adc_open |= (uint8_t)(1U << chan);
    return KODE_OK;
}

kode_status_t khal_exp_port_adc_read(uint8_t chan, uint16_t *raw, int32_t *mv,
                                     bool *calibrated)
{
    int count = 0;

    /* Filled first, so every return below leaves zeroes, not stack. */
    *raw        = 0U;
    *mv         = 0;
    *calibrated = false;
    if ( !chan_ok(chan) )
    {
        return KODE_DENIED;
    }
    if ( (NULL == s_adc) || (0U == (s_adc_open & (uint8_t)(1U << chan))) )
    {
        return KODE_DENIED;
    }
    if ( adc_oneshot_read(s_adc, (adc_channel_t)chan, &count) != ESP_OK )
    {
        return KODE_IO_ERROR;
    }
    *raw = (uint16_t)count;
    if ( cali_mv(chan, count, mv) )
    {
        *calibrated = true;
        return KODE_OK;
    }
    /* Uncalibrated: an estimate over the nominal full scale, and the caller is told so. */
    *mv = ((int32_t)count * EXP_ADC_FS_MV) / EXP_ADC_FS_RAW;
    return KODE_OK;
}

void khal_exp_port_adc_close(uint8_t chan)
{
    if ( !chan_ok(chan) )
    {
        return;
    }
    cali_close(chan);
    s_adc_open &= (uint8_t)~(1U << chan);
    /* The unit stays: deleting it would close every other open channel. */
}
