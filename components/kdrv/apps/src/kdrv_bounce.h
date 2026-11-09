/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_bounce.h
 * @brief Flash writes from internal RAM: the one place they are made so.
 */
#ifndef KDRV_BOUNCE_H
#define KDRV_BOUNCE_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"

/** Internal RAM one write borrows: eight 256-byte flash pages. */
#define KDRV_BOUNCE_BYTES  2048U

/**
 * @brief Write one piece to flash.
 * @param ctx The caller's own context.
 * @param at  How far into this write the piece starts, in bytes.
 * @param src The piece.
 * @param len Its length.
 * @return ESP_OK, or the flash's error.
 */
typedef esp_err_t (*kdrv_bounce_put_t)(void *ctx, size_t at, const void *src,
                                       size_t len);

/**
 * @brief Write @p len bytes through a bounce of internal RAM.
 *
 * Stops at the first piece the flash refuses.
 *
 * @param put  Writes one piece.
 * @param ctx  Passed to @p put untouched.
 * @param data The bytes, anywhere in memory.
 * @param len  How many.
 * @return ESP_OK, or the first error @p put returned.
 */
static inline esp_err_t kdrv_bounce_write(kdrv_bounce_put_t put, void *ctx,
                                          const void *data, size_t len)
{
    const uint8_t * const src = (const uint8_t *)data;
    /* esp_flash_write() is fast only from internal RAM. The bounce is borrowed per call, never
     * a static; with none to lend, the write goes from data as it is: slower, still correct. */
    uint8_t * const bounce = (uint8_t *)heap_caps_malloc(
        (size_t)KDRV_BOUNCE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    esp_err_t err = ESP_OK;

    if ( NULL == bounce )
    {
        err = put(ctx, 0U, data, len);
    }
    else
    {
        size_t at = 0U;
        while ( (ESP_OK == err) && (at < len) )
        {
            size_t n = len - at;
            if ( (size_t)KDRV_BOUNCE_BYTES < n )
            {
                n = (size_t)KDRV_BOUNCE_BYTES;
            }
            (void)memcpy(bounce, &src[at], n);
            err = put(ctx, at, bounce, n);
            at += n;
        }
        heap_caps_free(bounce);
    }
    return err;
}

#endif /* KDRV_BOUNCE_H */
