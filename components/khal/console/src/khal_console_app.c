/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_console_app.c
 * @brief The console's writes to a cable port the app provides: one call per write, the rest counted.
 */
#include "khal_console_app.h"

#include <stdbool.h>

#include "khal_console.h"

static uint32_t s_dropped;   /**< Bytes the app's port did not take. */

void khal_console_app_put(const char *buf, size_t len)
{
    static bool stalled;   /* The last write was taken in part: nobody is reading. */
    uint32_t    wait_ms = KHAL_CONSOLE_TX_WAIT_MS;

    if ( 0U == len )
    {
        return;
    }
    if ( stalled )
    {
        wait_ms = 0U;
    }
    size_t const taken = khal_console_app_write(buf, len, wait_ms);

    /* An answer of len or more took the whole write. */
    stalled = (taken < len);
    if ( stalled )
    {
        s_dropped += (uint32_t)(len - taken);
    }
}

uint32_t khal_console_app_dropped(void)
{
    return s_dropped;
}
