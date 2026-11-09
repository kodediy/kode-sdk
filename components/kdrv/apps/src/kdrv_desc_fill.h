/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_desc_fill.h
 * @brief IDF's app descriptor into ours: the one place it happens.
 */
#ifndef KDRV_DESC_FILL_H
#define KDRV_DESC_FILL_H

#include <stdio.h>

#include "esp_app_desc.h"
#include "kdrv_slot.h"

/**
 * @brief Translate IDF's descriptor into ours.
 * @param out  Ours; every string terminated inside its array.
 * @param desc IDF's, as read off flash or from the running image.
 */
static inline void kdrv_desc_fill(kdrv_slot_desc_t *out, const esp_app_desc_t *desc)
{
    /* IDF's strings need not end in a NUL, so each is read with its array's width. */
    (void)snprintf(out->project, sizeof(out->project), "%.*s",
                   (int)sizeof(desc->project_name), desc->project_name);
    (void)snprintf(out->version, sizeof(out->version), "%.*s",
                   (int)sizeof(desc->version), desc->version);
    (void)snprintf(out->date, sizeof(out->date), "%.*s",
                   (int)sizeof(desc->date), desc->date);
    (void)snprintf(out->time, sizeof(out->time), "%.*s",
                   (int)sizeof(desc->time), desc->time);
    (void)snprintf(out->idf_ver, sizeof(out->idf_ver), "%.*s",
                   (int)sizeof(desc->idf_ver), desc->idf_ver);
}

#endif /* KDRV_DESC_FILL_H */
