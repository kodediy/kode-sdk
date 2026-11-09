/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_worker.h
 * @brief A task that runs one job at a time from a queue, for work that waits.
 */
#ifndef KHAL_WORKER_H_
#define KHAL_WORKER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief A worker: a task and the queue that feeds it. */
typedef struct khal_worker_s *khal_worker_t;

/**
 * @brief What a worker does with a job.
 * @param job The worker's own copy of the job, valid until the function
 *            returns; it holds the bytes that were handed to
 *            khal_worker_submit().
 */
typedef void (*khal_worker_run_t)(void const *job);

/**
 * @brief Create a worker: a task pinned to no core, and a queue of jobs.
 *
 * Both are taken once and never released. The queue copies @p job_size bytes
 * per job, so a job is a small struct of values, never a pointer the caller
 * must keep alive for it.
 *
 * @param out         Receives the worker.
 * @param name        Task name; the task list prints it.
 * @param stack_bytes Stack of the task, in bytes.
 * @param prio        FreeRTOS priority of the task.
 * @param depth       Jobs the queue holds besides the one being run.
 * @param job_size    Bytes of one job.
 * @param run         Runs each job, on the worker's task.
 * @param psram_stack true takes the task's stack from PSRAM, false from
 *                    internal RAM; the control block is always internal.
 * @return KODE_OK; KODE_DENIED for a NULL argument or a zero size or depth;
 *         KODE_NO_RESOURCES when the memory or the task cannot be had.
 * @warning A task whose stack is in PSRAM must not run while the flash cache
 *          is off. Its jobs must not touch the internal flash through SPI1
 *          (esp_partition, esp_flash, spi_flash, NVS, the internal FAT, the
 *          BLE bond store): a job that does belongs on a worker created with
 *          @p psram_stack false.
 */
kode_status_t khal_worker_create(khal_worker_t *out, char const *name,
                                 uint32_t stack_bytes, uint8_t prio,
                                 uint8_t depth, size_t job_size,
                                 khal_worker_run_t run, bool psram_stack);

/**
 * @brief Queue a job for the worker. Never blocks.
 * @param w   The worker.
 * @param job The job; @c job_size bytes are copied out of it.
 * @return KODE_OK; KODE_NO_RESOURCES when the queue is full (kode_status_t
 *         has no BUSY on purpose); KODE_DENIED for a NULL argument.
 */
kode_status_t khal_worker_submit(khal_worker_t w, void const *job);

/**
 * @brief The stack the worker's task has never used, at its lowest point so far.
 *
 * What is left of the stack at the deepest the task has been, in bytes. It is
 * the number to read before trusting a stack size: the task runs for the
 * life of the board, and the figure only goes down.
 *
 * @param w The worker.
 * @return Unused stack bytes; 0 for NULL.
 */
uint32_t khal_worker_stack_free(khal_worker_t w);

/**
 * @brief Whether the caller is the worker's own task.
 * @param w The worker.
 * @return true on the worker's task, false anywhere else or for NULL.
 */
bool khal_worker_is_self(khal_worker_t w);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_WORKER_H_ */
