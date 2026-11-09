/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_panic.c
 * @brief The panic printer, swallowed and counted.
 */
#include <stdarg.h>

#include "fakes.h"
#include "khal_system.h"

static unsigned s_panics;

void fake_panic_reset(void)
{
    s_panics = 0U;
}

unsigned fake_panic_count(void)
{
    return s_panics;
}

void khal_panic_print(const char *fmt, ...)
{
    (void)fmt;
    ++s_panics;
}
