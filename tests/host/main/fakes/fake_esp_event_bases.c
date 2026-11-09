/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_esp_event_bases.c
 * @brief The two event bases the generated mocks cannot supply.
 */
#include "esp_event.h"

const char *WIFI_EVENT = "WIFI_EVENT";
const char *IP_EVENT   = "IP_EVENT";
