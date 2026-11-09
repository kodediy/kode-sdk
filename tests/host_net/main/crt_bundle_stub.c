/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file crt_bundle_stub.c
 * @brief esp_crt_bundle_attach(), which HTTPS needs and this binary does not.
 */
#include "esp_crt_bundle.h"

esp_err_t esp_crt_bundle_attach(void *conf)
{
    (void)conf;
    return ESP_OK;
}
