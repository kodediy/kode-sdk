/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_worker.c
 * @brief The runner of a worker's jobs and the post of their done events.
 */
#include "kqp_worker.h"

#include <stddef.h>
#include <string.h>

#include "kode_log.h"
#include "khal_time.h"
#include "kqp.h"

/** @brief Log tag of this file. */
#define KQP_WORKER_TAG "kqp.worker"

/** @brief How long the runner waits before trying to post again, in ms. */
#define KQP_WORKER_RETRY_MS 1U

/** @brief Failed attempts after which the wait is logged, once per job. */
#define KQP_WORKER_RETRIES_BEFORE_LOG 100U

/** @brief A job as the khal queue carries it: the call and where its answer goes. */
typedef struct {
    KqpJobFn fn;   /**< The function to run.                    */
    void    *arg;  /**< Its argument.                           */
    QActive *ao;   /**< Receives the done event.                */
    enum_t   sig;  /**< Signal of the done event.               */
    uint16_t seq;  /**< Sequence number to put in the event.    */
} KqpJob;

/** @brief A started worker as the registry keeps it. */
typedef struct {
    khal_worker_t w;                         /**< The task and its queue.   */
    uint32_t      stack_bytes;               /**< What it was given.        */
    char          name[KQP_WORKER_NAME_MAX]; /**< Its task name, clipped.   */
} KqpRegistered;

/** @brief The workers started so far, in order. Written by the AOs' initial
 *         transitions, which run before any console command can read it. */
static KqpRegistered s_registry[KQP_WORKERS_MAX];

/** @brief How many entries of s_registry are in use. */
static uint8_t s_registered;

/**
 * @brief Note a started worker so `ps` can list it.
 *
 * A name already listed is that worker again (an AO built a second time on
 * the host); it takes its entry over. A full registry leaves the worker
 * running and unlisted, and says so.
 *
 * @param w           The worker.
 * @param name        Its task name.
 * @param stack_bytes Its stack.
 */
static void registry_add(khal_worker_t w, char const *name, uint32_t stack_bytes)
{
    uint8_t slot = s_registered;

    for ( uint8_t i = 0U; i < s_registered; ++i )
    {
        if ( strncmp(s_registry[i].name, name, KQP_WORKER_NAME_MAX - 1U) == 0 )
        {
            slot = i;
        }
    }
    if ( (uint8_t)KQP_WORKERS_MAX <= slot )
    {
        KLOG_W(KQP_WORKER_TAG, "worker '%s' runs but the registry is full", name);
        return;
    }
    (void)memset(&s_registry[slot], 0, sizeof(s_registry[slot]));
    s_registry[slot].w           = w;
    s_registry[slot].stack_bytes = stack_bytes;
    (void)strncpy(s_registry[slot].name, name, KQP_WORKER_NAME_MAX - 1U);
    if ( slot == s_registered )
    {
        ++s_registered;
    }
}

/**
 * @brief Post the done event of @p job, waiting for room as long as it takes.
 *
 * A refused pool block or a full queue frees up as the AO runs, so the
 * answer is never dropped; only the wait is logged, once.
 *
 * @param job    The job that returned.
 * @param status What its function returned.
 * @param value  What its function wrote.
 */
static void post_done(KqpJob const *job, kode_status_t status, uint32_t value)
{
    bool     posted = false;
    uint32_t tries  = 0U;

    while ( !posted )
    {
        KqpWorkerDoneEvt *const e = Q_NEW_X(KqpWorkerDoneEvt,
                                            KQP_WORKER_POST_MARGIN,
                                            job->sig);

        if ( NULL != e )
        {
            e->seq    = job->seq;
            e->status = (uint8_t)status;
            e->value  = value;
            posted = QACTIVE_POST_X(job->ao, &e->super,
                                    KQP_WORKER_POST_MARGIN, (void *)0);
        }
        if ( !posted )
        {
            ++tries;
            if ( KQP_WORKER_RETRIES_BEFORE_LOG == tries )
            {
                KLOG_W(KQP_WORKER_TAG,
                       "done event %d seq %u waits for a pool block or queue room",
                       (int)job->sig, (unsigned)job->seq);
            }
            khal_delay_ms(KQP_WORKER_RETRY_MS);
        }
    }
}

/**
 * @brief Run one job and tell its Active Object.
 * @param job The worker's copy of a KqpJob.
 */
static void run_job(void const *job)
{
    /* cppcheck-suppress misra-c2012-11.5 ; khal_worker_run_t hands back a void const *: the copy of the KqpJob it was given */
    KqpJob const *const j = (KqpJob const *)job;
    uint32_t value = 0U;
    kode_status_t const status = j->fn(j->arg, &value);

    post_done(j, status, value);
}

kode_status_t kqp_worker_start(KqpWorker *me, QActive *ao, char const *name,
                               uint32_t stack_bytes, uint8_t prio,
                               uint8_t depth, bool psram_stack)
{
    if ( NULL == me )
    {
        return KODE_DENIED;
    }
    me->w = NULL;      /* nothing to submit to until the task exists */
    if ( (NULL == ao) || (NULL == name) )
    {
        return KODE_DENIED;
    }
    me->ao       = ao;
    me->next_seq = 0U;

    kode_status_t const st = khal_worker_create(&me->w, name, stack_bytes, prio,
                                                depth, sizeof(KqpJob), &run_job,
                                                psram_stack);
    if ( KODE_OK == st )
    {
        registry_add(me->w, name, stack_bytes);
    }
    return st;
}

uint8_t kqp_worker_count(void)
{
    return s_registered;
}

bool kqp_worker_info(uint8_t idx, KqpWorkerInfo *out)
{
    if ( (NULL == out) || (idx >= s_registered) )
    {
        return false;
    }
    (void)memset(out, 0, sizeof(*out));
    (void)strncpy(out->name, s_registry[idx].name, KQP_WORKER_NAME_MAX - 1U);
    out->stack_bytes = s_registry[idx].stack_bytes;
    out->free_bytes  = khal_worker_stack_free(s_registry[idx].w);
    return true;
}

kode_status_t kqp_worker_submit(KqpWorker *me, KqpJobFn fn, void *arg,
                                enum_t done_sig, uint16_t *seq_out)
{
    if ( (NULL == me) || (NULL == fn) )
    {
        return KODE_DENIED;
    }

    KqpJob const job = {
        .fn  = fn,
        .arg = arg,
        .ao  = me->ao,
        .sig = done_sig,
        .seq = me->next_seq,
    };
    kode_status_t const st = khal_worker_submit(me->w, &job);

    if ( KODE_OK != st )
    {
        return st;
    }
    me->next_seq = (uint16_t)(me->next_seq + 1U);
    if ( NULL != seq_out )
    {
        *seq_out = job.seq;
    }
    return KODE_OK;
}

bool kqp_worker_is_self(KqpWorker const *me)
{
    if ( NULL == me )
    {
        return false;
    }
    return khal_worker_is_self(me->w);
}
