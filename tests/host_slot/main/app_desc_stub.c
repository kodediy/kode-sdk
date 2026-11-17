/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file app_desc_stub.c
 * @brief The running image's description, for a binary the linux target
 *        links none into.
 */
#include "esp_app_desc.h"

/** @brief What the running image says it is: the project an update must name. */
static const esp_app_desc_t s_desc = {
    .project_name = "kodeos",
    .version      = "0.0.0",
};

const esp_app_desc_t *esp_app_get_description(void)
{
    return &s_desc;
}
