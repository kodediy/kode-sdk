/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_codec_dev_defaults.h
 * @brief The component's default interfaces, as kdrv_es8311.c builds them.
 */
#ifndef HOST_CODEC_ESP_CODEC_DEV_DEFAULTS_H_
#define HOST_CODEC_ESP_CODEC_DEV_DEFAULTS_H_

#include "esp_codec_dev.h"
#include "driver/i2s_std.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t port;                                /**< The bus.             */
    uint8_t addr;                                /**< The chip, eight-bit form. */
    void   *bus_handle;                          /**< The bus handle.      */
} audio_codec_i2c_cfg_t;

typedef struct {
    uint8_t           port;                      /**< The controller.      */
    i2s_chan_handle_t rx_handle;                 /**< The receive half.    */
    i2s_chan_handle_t tx_handle;                 /**< The transmit half.   */
    int               clk_src;                   /**< The clock source.    */
} audio_codec_i2s_cfg_t;

const audio_codec_ctrl_if_t *audio_codec_new_i2c_ctrl(audio_codec_i2c_cfg_t *cfg);
const audio_codec_data_if_t *audio_codec_new_i2s_data(audio_codec_i2s_cfg_t *cfg);
const audio_codec_gpio_if_t *audio_codec_new_gpio(void);

#ifdef __cplusplus
}
#endif

#endif /* HOST_CODEC_ESP_CODEC_DEV_DEFAULTS_H_ */
