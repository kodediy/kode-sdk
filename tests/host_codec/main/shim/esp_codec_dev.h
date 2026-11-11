/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_codec_dev.h
 * @brief The part of the codec component that kdrv_es8311.c uses.
 *
 * The component's own sources include the I2S driver and do not build for the
 * host. Names, field names and signatures are the component's.
 */
#ifndef HOST_CODEC_ESP_CODEC_DEV_H_
#define HOST_CODEC_ESP_CODEC_DEV_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ESP_CODEC_DEV_OK                0    /**< The call worked.            */
#define ESP_CODEC_DEV_WORK_MODE_BOTH    3    /**< Both directions.            */
#define ESP_CODEC_DEV_TYPE_IN_OUT       3    /**< An input and an output.     */

typedef struct audio_codec_ctrl_if_t audio_codec_ctrl_if_t;   /**< The control bus.  */
typedef struct audio_codec_data_if_t audio_codec_data_if_t;   /**< The data bus.     */
typedef struct audio_codec_gpio_if_t audio_codec_gpio_if_t;   /**< The codec's pins. */
typedef struct audio_codec_if_t      audio_codec_if_t;        /**< The chip driver.  */
typedef struct codec_dev_t          *esp_codec_dev_handle_t;  /**< The device.       */

struct audio_codec_ctrl_if_t {
    int (*read_reg)(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                    void *data, int data_len);   /**< Read a register.   */
    int (*write_reg)(const audio_codec_ctrl_if_t *ctrl, int reg, int reg_len,
                     void *data, int data_len);  /**< Write a register.  */
};

struct audio_codec_if_t {
    int (*enable)(const audio_codec_if_t *h, bool enable);   /**< Power the chip. */
};

typedef struct {
    uint8_t bits_per_sample;                     /**< Sample width.        */
    uint8_t channel;                             /**< Slots.               */
    uint16_t channel_mask;                       /**< Which slots.         */
    uint32_t sample_rate;                        /**< Frames per second.   */
    int mclk_multiple;                           /**< Master clock multiple. */
} esp_codec_dev_sample_info_t;

typedef struct {
    int   vol;                                   /**< A volume step.       */
    float db_value;                              /**< Its decibels.        */
} esp_codec_dev_vol_map_t;

typedef struct {
    esp_codec_dev_vol_map_t *vol_map;            /**< The points.          */
    int                      count;              /**< How many.            */
} esp_codec_dev_vol_curve_t;

typedef struct {
    int                       dev_type;          /**< Input, output or both. */
    const audio_codec_if_t   *codec_if;          /**< The chip driver.     */
    const audio_codec_data_if_t *data_if;        /**< The data bus.        */
} esp_codec_dev_cfg_t;

esp_codec_dev_handle_t esp_codec_dev_new(const esp_codec_dev_cfg_t *cfg);
int esp_codec_dev_open(esp_codec_dev_handle_t dev, esp_codec_dev_sample_info_t *fs);
int esp_codec_dev_close(esp_codec_dev_handle_t dev);
int esp_codec_dev_write(esp_codec_dev_handle_t dev, void *data, int len);
int esp_codec_dev_read(esp_codec_dev_handle_t dev, void *data, int len);
int esp_codec_dev_set_out_vol(esp_codec_dev_handle_t dev, int volume);
int esp_codec_dev_set_in_gain(esp_codec_dev_handle_t dev, float db);
int esp_codec_dev_set_out_mute(esp_codec_dev_handle_t dev, bool mute);
int esp_codec_dev_set_in_mute(esp_codec_dev_handle_t dev, bool mute);
int esp_codec_dev_set_vol_curve(esp_codec_dev_handle_t dev,
                                esp_codec_dev_vol_curve_t *curve);
int esp_codec_set_disable_when_closed(esp_codec_dev_handle_t dev, bool disable);

#ifdef __cplusplus
}
#endif

#endif /* HOST_CODEC_ESP_CODEC_DEV_H_ */
