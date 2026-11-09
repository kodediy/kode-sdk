/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_task_psram.h
 * @brief Force-included into a library: every xTaskCreate() in it takes its stack from PSRAM.
 */
#ifndef KHAL_TASK_PSRAM_H_
#define KHAL_TASK_PSRAM_H_

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"

/* Such a task must not run while the flash cache is off: its stack is in PSRAM. */
/* After task.h, whose guard keeps the declaration from ever seeing this. */
#define xTaskCreate(fn_, name_, depth_, arg_, prio_, handle_)                 \
    xTaskCreatePinnedToCoreWithCaps((fn_), (name_), (depth_), (arg_),         \
                                    (prio_), (handle_), tskNO_AFFINITY,       \
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

#endif /* KHAL_TASK_PSRAM_H_ */
