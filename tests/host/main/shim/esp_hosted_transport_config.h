/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_hosted_transport_config.h
 * @brief The one transport call kdrv_c5.c makes, declared for the host.
 */
#ifndef KODE_HOST_SHIM_ESP_HOSTED_TRANSPORT_CONFIG_H_
#define KODE_HOST_SHIM_ESP_HOSTED_TRANSPORT_CONFIG_H_

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

/* Copied from esp-hosted 2.12.9, host/api/include/esp_hosted_transport_config.h. */
#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ESP_TRANSPORT_OK                = ESP_OK,
    ESP_TRANSPORT_ERR_INVALID_ARG   = ESP_ERR_INVALID_ARG,
    ESP_TRANSPORT_ERR_ALREADY_SET   = ESP_ERR_NOT_ALLOWED,
    ESP_TRANSPORT_ERR_INVALID_STATE = ESP_ERR_INVALID_STATE,
} esp_hosted_transport_err_t;

typedef struct {
    void *port;
    int   pin;
} gpio_pin_t;

struct esp_hosted_sdio_config {
    uint32_t   clock_freq_khz;
    uint8_t    bus_width;
    uint8_t    slot;
    gpio_pin_t pin_clk;
    gpio_pin_t pin_cmd;
    gpio_pin_t pin_d0;
    gpio_pin_t pin_d1;
    gpio_pin_t pin_d2;
    gpio_pin_t pin_d3;
    gpio_pin_t pin_reset;
    uint8_t    rx_mode;
    bool       block_mode;
    bool       iomux_enable;
    uint16_t   tx_queue_size;
    uint16_t   rx_queue_size;
};

esp_hosted_transport_err_t esp_hosted_sdio_get_config(struct esp_hosted_sdio_config **config);

#ifdef __cplusplus
}
#endif

#endif /* KODE_HOST_SHIM_ESP_HOSTED_TRANSPORT_CONFIG_H_ */
