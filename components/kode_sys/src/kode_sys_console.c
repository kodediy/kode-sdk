/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_console.c
 * @brief The console's door: whether its output keeps up, as AO_Log counts it.
 */
#include "kode_sys.h"

#include "kaos.h"

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_sys_console_congested(void)
{
    return console_congested();
}
