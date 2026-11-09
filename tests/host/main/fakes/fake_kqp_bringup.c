/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_kqp_bringup.c
 * @brief The two kqp symbols kode_init()'s bring-up names, and nothing more.
 */
#include <stdio.h>
#include <stdlib.h>

#include "kqp.h"

/** @brief Stop the run: a case reached the bring-up without arming it. */
static void reached(const char *what)
{
    (void)fprintf(stderr,
                  "fake_kqp_bringup: %s was called: a case reached the real bring-up "
                  "without arming kode_sys_init_once\n", what);
    abort();
}

void kqp_init(void)
{
    reached("kqp_init");
}

void QF_onStartup(void)
{
    reached("QF_onStartup");
}
