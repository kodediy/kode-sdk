/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_worker.h
 * @brief The one way an Active Object runs work that waits, outside its step.
 */
#ifndef KQP_WORKER_H_
#define KQP_WORKER_H_

#include <stdbool.h>
#include <stdint.h>

#include "qpc.h"
#include "kode_defs.h"
#include "khal_worker.h"
#include "kqp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The function a job runs, on the worker's task.
 * @param arg   The pointer that was handed to kqp_worker_submit(); the
 *              function owns whatever it points to for the job's duration.
 * @param value Receives the one word the AO is told besides the status.
 * @return The status the AO is told.
 */
typedef kode_status_t (*KqpJobFn)(void *arg, uint32_t *value);

/**
 * @brief The event a worker posts when a job returns.
 *
 * Sixteen bytes, so it comes from the small pool: the status is a byte, as
 * the other events that carry a kode_status_t keep it, and the AO reads it
 * back as `(kode_status_t)e->status`.
 */
typedef struct {
    QEvt     super;  /**< Its signal is the one chosen at submit.            */
    uint16_t seq;    /**< Sequence number kqp_worker_submit() gave.          */
    uint8_t  status; /**< The kode_status_t the job's function returned.     */
    uint32_t value;  /**< What the job's function wrote.                     */
} KqpWorkerDoneEvt;

/** @brief An Active Object's worker: the task, its owner and the next sequence number. */
typedef struct {
    khal_worker_t w;        /**< The task and its queue (khal).               */
    QActive      *ao;       /**< Receives every done event.                    */
    uint16_t      next_seq; /**< Sequence number of the next job; it wraps.    */
} KqpWorker;

/**
 * @brief Bring a worker up for @p ao.
 *
 * Call it once, before the AO is started or from its initial transition,
 * never from a state handler that can run twice.
 *
 * @param me          The worker to fill in.
 * @param ao          The Active Object that receives the done events.
 * @param name        Task name.
 * @param stack_bytes Stack of the task, in bytes.
 * @param prio        FreeRTOS priority of the task.
 * @param depth       Jobs that may wait besides the one running.
 * @param psram_stack true puts the task's stack in PSRAM; see
 *                    khal_worker_create() for what a job may then not touch.
 * @return KODE_OK; KODE_DENIED for a NULL argument; KODE_NO_RESOURCES when
 *         the task cannot be had.
 */
kode_status_t kqp_worker_start(KqpWorker *me, QActive *ao, char const *name,
                               uint32_t stack_bytes, uint8_t prio,
                               uint8_t depth, bool psram_stack);

/**
 * @brief Queue a job. Never blocks.
 *
 * When @p fn returns, a KqpWorkerDoneEvt with signal @p done_sig is posted to
 * the worker's Active Object. The event is posted even if the AO has lost
 * interest; telling a late answer from a current one by @p seq is the AO's
 * job. Posting waits, without a bound, for a pool block and for room in the
 * AO's queue (the AO frees both as it runs), and the worker takes no other
 * job meanwhile.
 * Only the worker's own Active Object submits to it: @p seq_out is counted per worker.
 *
 * @param me       The worker.
 * @param fn       The function to run.
 * @param arg      Passed to @p fn; it must stay valid until the done event
 *                 has been received, even if the AO has stopped waiting for
 *                 the job: the function may still be reading it.
 * @param done_sig Signal of the done event.
 * @param seq_out  Receives the job's sequence number; may be NULL.
 * @return KODE_OK; KODE_NO_RESOURCES when the queue is full (nothing was
 *         queued and the sequence number was not used); KODE_DENIED for a
 *         NULL @p me or @p fn.
 */
kode_status_t kqp_worker_submit(KqpWorker *me, KqpJobFn fn, void *arg,
                                enum_t done_sig, uint16_t *seq_out);

/** @brief What the registry knows of one started worker. */
typedef struct {
    char     name[KQP_WORKER_NAME_MAX]; /**< Task name.                        */
    uint32_t stack_bytes;               /**< Stack the task was given.         */
    uint32_t free_bytes;                /**< Stack it has never used, now.     */
} KqpWorkerInfo;

/**
 * @brief How many started workers the registry holds.
 * @return A count of at most KQP_WORKERS_MAX.
 */
uint8_t kqp_worker_count(void);

/**
 * @brief Describe the @p idx-th started worker, in the order they started.
 *
 * The unused stack is read now, so the number is the lowest the stack has
 * been since boot.
 *
 * @param idx Which worker, from 0.
 * @param out Receives the description.
 * @return true; false when @p idx is past the last worker or @p out is NULL.
 */
bool kqp_worker_info(uint8_t idx, KqpWorkerInfo *out);

/**
 * @brief Whether the caller is this worker's own task.
 * @param me The worker.
 * @return true on the worker's task, false anywhere else.
 */
bool kqp_worker_is_self(KqpWorker const *me);

#ifdef __cplusplus
}
#endif

#endif /* KQP_WORKER_H_ */
