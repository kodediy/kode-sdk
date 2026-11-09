/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file identity_stub.c
 * @brief The board's identity, fixed, for a binary that is not testing it.
 */
#include <stdlib.h>

#include "khal_identity.h"
#include "khal_system.h"
#include "host_spies.h"

bool khal_identity_valid(void)
{
    return true;
}

const char *khal_identity_serial(void)
{
    return "KD-TEST-0001";
}

/**
 * @brief The wildcard address ESP_PING_DEFAULT_CONFIG() dereferences.
 *
 * Declared by the ip_addr shim; it exists so the macro has something to copy
 * from, and every byte of it is overwritten before the config is used.
 */
#include "lwip/ip_addr.h"
const ip_addr_t kode_shim_ip_any;

/**
 * @brief PSRAM, as the host has it: the heap.
 *
 * kdrv_net takes its POST body and its MQTT session from here once and keeps
 * them. The host has no second kind of memory to tell apart.
 */
#define PSRAM_STUB_SEEN  8U   /**< Blocks remembered for psram_stub_block(). */

static struct {
    void  *at;
    size_t size;
} s_psram_seen[PSRAM_STUB_SEEN];

void *khal_alloc_psram(size_t align, size_t size)
{
    (void)align;
    void *const p = calloc(1U, size);
    for ( unsigned i = 0U; (NULL != p) && (PSRAM_STUB_SEEN > i); ++i )
    {
        if ( NULL == s_psram_seen[i].at )
        {
            s_psram_seen[i].at   = p;
            s_psram_seen[i].size = size;
            break;
        }
    }
    return p;
}

const unsigned char *psram_stub_block(size_t min_size, size_t *size)
{
    for ( unsigned i = 0U; PSRAM_STUB_SEEN > i; ++i )
    {
        if ( (NULL != s_psram_seen[i].at) && (s_psram_seen[i].size >= min_size) )
        {
            if ( NULL != size )
            {
                *size = s_psram_seen[i].size;
            }
            return (const unsigned char *)s_psram_seen[i].at;
        }
    }
    return NULL;
}
