/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file esp_crt_bundle.h
 * @brief The one function kdrv_net.c uses from the root certificate bundle.
 */
#ifndef KODE_HOST_SHIM_ESP_CRT_BUNDLE_H_
#define KODE_HOST_SHIM_ESP_CRT_BUNDLE_H_

#include "esp_err.h"

esp_err_t esp_crt_bundle_attach(void *conf);

#endif /* KODE_HOST_SHIM_ESP_CRT_BUNDLE_H_ */
