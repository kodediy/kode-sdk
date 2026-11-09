/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file standard_ao_support.c
 * @brief Waiting for the standard layer's Active Object to take its queue empty.
 */
#include "standard_ao_support.h"

#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "kode_standard_ao.h"

/** @brief Looks at the queue before the wait gives up. */
#define STANDARD_AO_WAIT_LOOKS 4000U

/** @brief Ticks slept between two looks, and twice more once it is empty. */
#define STANDARD_AO_POLL_TICKS 1U

void standard_ao_support_drained(void)
{
    for ( uint32_t i = 0U; (STANDARD_AO_WAIT_LOOKS > i)
                          && (uxQueueMessagesWaiting(AO_KodeStandard->eQueue) != 0U); i++ )
    {
        vTaskDelay(STANDARD_AO_POLL_TICKS);
    }
    vTaskDelay(STANDARD_AO_POLL_TICKS);
    vTaskDelay(STANDARD_AO_POLL_TICKS);
}
