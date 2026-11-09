/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_sync.c
 * @brief A mutex that counts how deep it is held, because a host case is one thread.
 */
#include <stdint.h>

#include "khal_sync.h"

int32_t g_fake_mutex_depth;   /**< +1 per lock, -1 per unlock: 0 when every lock was given back. */

static uint8_t s_mutex_token; /**< Its address is the one mutex handed out. */

khal_mutex_t khal_mutex_create(void)
{
    return &s_mutex_token;
}

void khal_mutex_lock(khal_mutex_t m)
{
    (void)m;
    ++g_fake_mutex_depth;
}

void khal_mutex_unlock(khal_mutex_t m)
{
    (void)m;
    --g_fake_mutex_depth;
}
