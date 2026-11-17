/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_compose_none.c
 * @brief display_compose_run() in a build without CONFIG_KODE_COMPOSE.
 */
#include "kaos.h"

#include "kode_log.h"

#define TAG "kaos.display"

kode_status_t display_compose_run(display_compose_ask_t *ask, display_compose_t const *job,
                                  uint32_t timeout_ms, uint32_t *us)
{
    (void)ask;
    (void)job;
    (void)timeout_ms;
    (void)us;
    KLOG_W(TAG, "compose not built (CONFIG_KODE_COMPOSE): the pass is refused");
    return KODE_DENIED;
}
