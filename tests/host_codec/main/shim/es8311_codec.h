/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file es8311_codec.h
 * @brief The ES8311 chip driver's configuration, as kdrv_es8311.c fills it.
 */
#ifndef HOST_CODEC_ES8311_CODEC_H_
#define HOST_CODEC_ES8311_CODEC_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    float pa_voltage;                            /**< The amplifier's supply.   */
    float codec_dac_voltage;                     /**< The converter's supply.   */
    float pa_gain;                               /**< The amplifier's gain.     */
} esp_codec_dev_hw_gain_t;

typedef struct {
    const audio_codec_ctrl_if_t *ctrl_if;        /**< The control bus.          */
    const audio_codec_gpio_if_t *gpio_if;        /**< The codec's pins.         */
    int                          codec_mode;     /**< Directions in use.        */
    int16_t                      pa_pin;         /**< The amplifier's enable.   */
    bool                         pa_reverted;    /**< Its polarity.             */
    bool                         master_mode;    /**< The chip is the master.   */
    bool                         use_mclk;       /**< A master clock is given.  */
    bool                         digital_mic;    /**< A digital microphone.     */
    bool                         invert_mclk;    /**< Master clock polarity.    */
    bool                         invert_sclk;    /**< Bit clock polarity.       */
    esp_codec_dev_hw_gain_t      hw_gain;        /**< The analog gain staging.  */
    bool                         no_dac_ref;     /**< No DAC reference slot.    */
    uint16_t                     mclk_div;       /**< Master clock multiple.    */
} es8311_codec_cfg_t;

const audio_codec_if_t *es8311_codec_new(es8311_codec_cfg_t *cfg);

#ifdef __cplusplus
}
#endif

#endif /* HOST_CODEC_ES8311_CODEC_H_ */
