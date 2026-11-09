/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_worker.h
 * @brief A worker for an Active Object of your own: runs what waits, off the AO's step.
 */
#ifndef KODE_WORKER_H_
#define KODE_WORKER_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kqp.h"
#include "kqp_worker.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief An Active Object's worker. */
typedef KqpWorker kode_worker_t;

/**
 * @brief A job: `kode_status_t fn(void *arg, uint32_t *value)`, run on the worker's task.
 *
 * @c arg is what kode_worker_submit() was handed; the job writes one word to
 * @c value and returns the status the AO is told.
 */
typedef KqpJobFn kode_worker_fn_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief A job's answer, under the signal given at submit: @c seq, the job's
 *        sequence number; @c status, the kode_status_t the job returned; @c value,
 *        the word it wrote.
 */
typedef KqpWorkerDoneEvt kode_worker_done_t;

/**
 * @brief Bring a worker up for @p ao, its stack in PSRAM.
 * @note Call once, before the AO is started or from its initial transition.
 * @param me          The worker to fill in.
 * @param ao          The Active Object that receives the done events.
 * @param name        Task name.
 * @param stack_bytes Stack of the task, in bytes.
 * @param prio        FreeRTOS priority of the task.
 * @param depth       Jobs that may wait besides the one running.
 * @return KODE_OK; KODE_DENIED for a NULL argument; KODE_NO_RESOURCES when
 *         the task cannot be had.
 */
static inline kode_status_t kode_worker_start(kode_worker_t *me, QActive *ao,
                                              char const *name,
                                              uint32_t stack_bytes,
                                              uint8_t prio, uint8_t depth)
{
    return kqp_worker_start(me, ao, name, stack_bytes, prio, depth, true);
}

/**
 * @brief Queue a job; one kode_worker_done_t is posted to the AO when @p fn returns.
 * @note Never blocks. The done event is posted even when the AO has stopped
 *       waiting; telling a late one by its @c seq is the AO's job. Its post
 *       waits without a bound for a pool block and room in the AO's queue, and
 *       the worker takes no other job meanwhile: an AO that stops reading its
 *       queue stalls its worker.
 * Only the AO the worker was started for submits to it: @c seq is counted per worker.
 * @param me       The worker.
 * @param fn       The job; no ESP-IDF flash, partition or NVS call, nor kode_sys_nvs_get().
 * @param arg      LENT: passed to @p fn; keep it valid until the done event
 *                 arrives, even after the AO stopped waiting for the job.
 * @param done_sig Signal of the kode_worker_done_t posted when @p fn returns.
 * @param seq_out  Receives the job's sequence number; may be NULL.
 * @return KODE_OK; KODE_NO_RESOURCES when the queue is full, nothing queued;
 *         KODE_DENIED for a NULL @p me or @p fn.
 */
static inline kode_status_t kode_worker_submit(kode_worker_t *me, kode_worker_fn_t fn,
                                               void *arg, enum_t done_sig,
                                               uint16_t *seq_out)
{
    return kqp_worker_submit(me, fn, arg, done_sig, seq_out);
}

#ifdef __cplusplus
}
#endif

#endif /* KODE_WORKER_H_ */
