/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file arduino_port.c
 * @brief The variant `arduino`'s console port: the two functions an app defines, taking every write.
 */
#include "khal_console.h"

void khal_console_app_init(void)
{
    /* No port to bring up: this build is linked, never run. */
}

size_t khal_console_app_write(const char *buf, size_t len, uint32_t wait_ms)
{
    (void)buf;
    (void)wait_ms;
    return len;
}
