/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_worker_esp.c
 * @brief khal_worker on ESP-IDF: a FreeRTOS task fed by a queue of job copies.
 */
#include "khal_worker.h"

#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"

/** @brief Heap capabilities of everything a PSRAM worker owns. */
#define WORKER_CAPS_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

/** @brief Heap capabilities of everything an internal-RAM worker owns. */
#define WORKER_CAPS_INTERNAL (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

/** @brief The worker: its queue, its task and the buffer a job is copied into. */
struct khal_worker_s {
    QueueHandle_t     queue; /**< Jobs waiting, one copy each.          */
    TaskHandle_t      task;  /**< The task that runs them.              */
    khal_worker_run_t run;   /**< What a job is handed to.              */
    void             *job;   /**< The task's copy of the job being run. */
};

/**
 * @brief The task: take a job, run it, take the next.
 * @param arg The worker.
 */
static void worker_task(void *arg)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the task entry gets a void *: it is the worker handed to xTaskCreate */
    struct khal_worker_s *const w = (struct khal_worker_s *)arg;

    for ( ;; )
    {
        if ( xQueueReceive(w->queue, w->job, portMAX_DELAY) == pdTRUE )
        {
            w->run(w->job);
        }
    }
}

kode_status_t khal_worker_create(khal_worker_t *out, char const *name,
                                 uint32_t stack_bytes, uint8_t prio,
                                 uint8_t depth, size_t job_size,
                                 khal_worker_run_t run, bool psram_stack)
{
    if ( (NULL == out) || (NULL == name) || (NULL == run) ||
         (0U == stack_bytes) || (0U == depth) || (0U == job_size) )
    {
        return KODE_DENIED;
    }

    /* cppcheck-suppress [misra-c2012-10.1,misra-c2012-12.2] ; IDF's MALLOC_CAP_* are signed int flags, OR'd as heap_caps_* take them */
    uint32_t caps = WORKER_CAPS_INTERNAL;
    if ( psram_stack )
    {
        /* cppcheck-suppress [misra-c2012-10.1,misra-c2012-12.2] ; IDF's MALLOC_CAP_* are signed int flags, OR'd as heap_caps_* take them */
        caps = WORKER_CAPS_PSRAM;
    }

    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is the worker */
    struct khal_worker_s *const w = heap_caps_calloc(1U, sizeof(*w), caps);
    if ( NULL == w )
    {
        return KODE_NO_RESOURCES;
    }
    w->run = run;
    w->job = heap_caps_calloc(1U, job_size, caps);
    w->queue = xQueueCreateWithCaps(depth, job_size, caps);
    if ( (NULL == w->job) || (NULL == w->queue) )
    {
        if ( NULL != w->queue )
        {
            vQueueDeleteWithCaps(w->queue);
        }
        heap_caps_free(w->job);
        heap_caps_free(w);
        return KODE_NO_RESOURCES;
    }

    if ( xTaskCreatePinnedToCoreWithCaps(worker_task, name, stack_bytes, w, prio,
                                         &w->task, tskNO_AFFINITY, caps) != pdPASS )
    {
        vQueueDeleteWithCaps(w->queue);
        heap_caps_free(w->job);
        heap_caps_free(w);
        return KODE_NO_RESOURCES;
    }

    *out = w;
    return KODE_OK;
}

kode_status_t khal_worker_submit(khal_worker_t w, void const *job)
{
    if ( (NULL == w) || (NULL == job) )
    {
        return KODE_DENIED;
    }
    if ( xQueueSendToBack(w->queue, job, 0U) != pdTRUE )
    {
        return KODE_NO_RESOURCES;
    }
    return KODE_OK;
}

bool khal_worker_is_self(khal_worker_t w)
{
    if ( NULL == w )
    {
        return false;
    }
    return xTaskGetCurrentTaskHandle() == w->task;
}

uint32_t khal_worker_stack_free(khal_worker_t w)
{
    if ( NULL == w )
    {
        return 0U;
    }
    /* The mark is counted in stack words; the port defines their size. */
    UBaseType_t const words = uxTaskGetStackHighWaterMark(w->task);

    return (uint32_t)(words * sizeof(StackType_t));
}
