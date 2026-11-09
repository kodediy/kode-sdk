/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_jpeg.c
 * @brief The worker's JPEG encodes, asked of AO_Display, which owns the engine.
 */
#include <stddef.h>
#include <stdint.h>

#include "kaos.h"
#include "khal_sync.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp_config.h"

#include "test_dot_priv.h"

/** @brief This file's log tag. */
#define TAG "kaos.test_dot"

/* Only the worker encodes, one at a time, so these need no lock. */
static khal_sem_t s_done;  /**< AO_Display's signal, made on the first encode and kept. */
static uint32_t   s_gen;   /**< The tag of the encode waited on now.                     */
static uint32_t   s_ack;   /**< The tag AO_Display answered last.                         */
static uint32_t   s_len;   /**< The JPEG's length, as answered.                           */
static uint32_t   s_rc;    /**< The answer's kode_status_t.                               */

/**
 * @brief Wait for the answer to tag @p gen, and for no other.
 *
 * A wait that ran out leaves a signal owed: it wakes the next wait, which
 * finds a foreign tag and goes on for what is left of its own time.
 *
 * @param gen The tag.
 * @return true once AO_Display answered @p gen; false when the time ran out.
 */
static bool td_jpeg_await(uint32_t gen)
{
    uint32_t const start = khal_time_ms();
    uint32_t left = KODE_TEST_DOT_JPEG_MS;
    bool mine = false;

    while ( !mine && khal_sem_wait_timeout(s_done, left) )
    {
        uint32_t const spent = khal_time_ms() - start;

        if ( __atomic_load_n(&s_ack, __ATOMIC_ACQUIRE) == gen )
        {
            mine = true;
        }
        else if ( KODE_TEST_DOT_JPEG_MS <= spent )
        {
            left = 0U;
        }
        else
        {
            left = KODE_TEST_DOT_JPEG_MS - spent;
        }
    }
    return mine;
}

kode_status_t td_jpeg_encode(const uint16_t *src, uint32_t w, uint32_t h, uint32_t quality, uint8_t *dst,
                             uint32_t cap, uint32_t *len)
{
    display_jpeg_t job;
    kode_status_t rc = KODE_TIMEOUT;
    uint32_t const gen = s_gen + 1U;

    *len = 0U;
    if ( NULL == s_done )
    {
        s_done = khal_sem_create();
    }
    if ( NULL == s_done )
    {
        return KODE_NO_RESOURCES;
    }
    job.src     = (const uint8_t *)(const void *)src;
    job.w       = (uint16_t)w;
    job.h       = (uint16_t)h;
    job.quality = (uint8_t)quality;
    job.dst     = dst;
    job.cap     = cap;
    __atomic_store_n(&s_gen, gen, __ATOMIC_RELEASE);
    display_jpeg_request(&job, &s_len, &s_rc, s_done, gen, &s_gen, &s_ack);
    if ( td_jpeg_await(gen) )
    {
        rc = (kode_status_t)s_rc;
        if ( KODE_OK == rc )
        {
            *len = s_len;
        }
    }
    else
    {
        /* Moved on, so AO_Display drops the request unread if it reaches it
         * later. One it was encoding at this very moment still writes the
         * buffer: an encode takes milliseconds against this wait's seconds. */
        __atomic_store_n(&s_gen, gen + 1U, __ATOMIC_RELEASE);
        KLOG_W(TAG, "camera: AO_Display did not encode within %u ms", (unsigned)KODE_TEST_DOT_JPEG_MS);
    }
    return rc;
}
