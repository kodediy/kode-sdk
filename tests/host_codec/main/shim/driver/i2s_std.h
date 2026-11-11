/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file i2s_std.h
 * @brief The part of ESP-IDF's I2S standard-mode driver that kdrv_es8311.c uses.
 *
 * The real header does not build for the host. The names, the field names and
 * the signatures are IDF's own; the macros fill only the fields the driver
 * reads.
 */
#ifndef HOST_CODEC_DRIVER_I2S_STD_H_
#define HOST_CODEC_DRIVER_I2S_STD_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef int gpio_num_t;                          /**< IDF's pin number type.   */

#define I2S_NUM_2  2                             /**< The chip's last controller. */

typedef struct i2s_channel_obj_t *i2s_chan_handle_t;  /**< One direction of a controller. */

typedef enum {
    I2S_ROLE_MASTER = 0,                         /**< The chip drives the clocks. */
    I2S_ROLE_SLAVE,                              /**< The chip follows them.      */
} i2s_role_t;

typedef struct {
    int        id;                               /**< The controller.             */
    i2s_role_t role;                             /**< Master or slave.            */
    uint32_t   dma_desc_num;                     /**< Descriptors in the ring.    */
    uint32_t   dma_frame_num;                    /**< Frames in one descriptor.   */
    bool       auto_clear;                       /**< Silence on an underrun.     */
} i2s_chan_config_t;

#define I2S_CHANNEL_DEFAULT_CONFIG(i2s_num, i2s_role) \
    { .id = (i2s_num), .role = (i2s_role) }

typedef int i2s_mclk_multiple_t;                 /**< The master clock's multiple of the rate. */

typedef struct {
    uint32_t            sample_rate_hz;          /**< Frames per second.          */
    i2s_mclk_multiple_t mclk_multiple;           /**< Master clock over the rate. */
} i2s_std_clk_config_t;

#define I2S_STD_CLK_DEFAULT_CONFIG(rate) \
    { .sample_rate_hz = (rate), .mclk_multiple = 256 }

#define I2S_DATA_BIT_WIDTH_16BIT  16             /**< 16-bit samples.             */
#define I2S_SLOT_MODE_STEREO       2             /**< Two slots.                  */

typedef struct {
    int data_bit_width;                          /**< Bits per sample.            */
    int slot_mode;                               /**< Mono or stereo.             */
} i2s_std_slot_config_t;

#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits, mode) \
    { .data_bit_width = (bits), .slot_mode = (mode) }

typedef struct {
    bool mclk_inv;                               /**< Master clock inverted.      */
    bool bclk_inv;                               /**< Bit clock inverted.         */
    bool ws_inv;                                 /**< Word select inverted.       */
} i2s_gpio_invert_t;

typedef struct {
    gpio_num_t        mclk;                      /**< Master clock pin.           */
    gpio_num_t        bclk;                      /**< Bit clock pin.              */
    gpio_num_t        ws;                        /**< Word select pin.            */
    gpio_num_t        dout;                      /**< Data out pin.               */
    gpio_num_t        din;                       /**< Data in pin.                */
    i2s_gpio_invert_t invert_flags;              /**< Polarities.                 */
} i2s_std_gpio_config_t;

typedef struct {
    i2s_std_clk_config_t  clk_cfg;               /**< The clock.                  */
    i2s_std_slot_config_t slot_cfg;              /**< The slots.                  */
    i2s_std_gpio_config_t gpio_cfg;              /**< The pins.                   */
} i2s_std_config_t;

esp_err_t i2s_new_channel(const i2s_chan_config_t *chan_cfg,
                          i2s_chan_handle_t *tx_handle, i2s_chan_handle_t *rx_handle);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t handle, const i2s_std_config_t *std_cfg);
esp_err_t i2s_channel_reconfig_std_gpio(i2s_chan_handle_t handle,
                                        const i2s_std_gpio_config_t *gpio_cfg);

#ifdef __cplusplus
}
#endif

#endif /* HOST_CODEC_DRIVER_I2S_STD_H_ */
