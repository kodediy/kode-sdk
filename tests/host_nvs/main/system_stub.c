/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file system_stub.c
 * @brief The one khal_system call khal_nvs makes, for a host with no PSRAM.
 */
#include "khal_system.h"

bool khal_task_stack_in_psram(void)
{
    return false;
}
