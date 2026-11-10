/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_worker.c
 * @brief A worker queue with no task: jobs wait until a case runs them.
 */
#include <stdlib.h>
#include <string.h>

#include "fake_khal_worker.h"
#include "khal_worker.h"

/** @brief Most jobs the fake's queue can be created to hold: the radio's
 *         service worker asks for two slots per KODE service, so 16 holds
 *         eight services. */
#define FAKE_WORKER_DEPTH_MAX 16U

/** @brief Largest job the fake can copy. */
#define FAKE_WORKER_JOB_MAX 64U

/** @brief Most workers the fake keeps: one per name an Active Object gives,
 *         as many as kqp's own registry holds (KQP_WORKERS_MAX). */
#define FAKE_WORKER_SLOTS 16U

/** @brief Longest worker name the fake keeps, with its terminator. */
#define FAKE_WORKER_NAME_MAX 24U

/** @brief The fake's worker: its configuration and its queue of job copies. */
struct khal_worker_s {
    bool              used;                                         /**< Created at least once.   */
    bool              refuse;                                       /**< Submits answer no room.  */
    bool              psram;                                        /**< The last creation's stack. */
    uint32_t          stack_free;                                   /**< What the stack reads as unused. */
    char              name[FAKE_WORKER_NAME_MAX];                   /**< What it was called.      */
    khal_worker_run_t run;                                          /**< Runs a job.              */
    size_t            job_size;                                     /**< Bytes of one job.        */
    unsigned          depth;                                        /**< Jobs the queue holds.    */
    unsigned          head;                                         /**< Index of the oldest job. */
    unsigned          count;                                        /**< Jobs queued.             */
    unsigned char     jobs[FAKE_WORKER_DEPTH_MAX][FAKE_WORKER_JOB_MAX]; /**< The job copies.      */
};

/* A worker is found by its name. A case that builds its Active Object afresh
 * creates the same worker again and gets its slot back; an Active Object that
 * is built once per binary keeps its slot (and the pointer it holds) across
 * every reset, which empties the queues and nothing else. */
static struct khal_worker_s  s_workers[FAKE_WORKER_SLOTS];
static struct khal_worker_s *s_last;      /**< The one created most recently. */
static struct khal_worker_s *s_in_job;    /**< The one whose job is running.  */

void fake_khal_worker_reset(void)
{
    for ( unsigned i = 0U; FAKE_WORKER_SLOTS > i; ++i )
    {
        s_workers[i].head   = 0U;
        s_workers[i].count  = 0U;
        s_workers[i].refuse = false;
    }
    s_last   = NULL;
    s_in_job = NULL;
}

/** @brief The slot called @p name, or NULL. */
static struct khal_worker_s *find(char const *name)
{
    if ( NULL == name )
    {
        return NULL;
    }
    for ( unsigned i = 0U; FAKE_WORKER_SLOTS > i; ++i )
    {
        if ( s_workers[i].used
            && (strncmp(s_workers[i].name, name, FAKE_WORKER_NAME_MAX) == 0) )
        {
            return &s_workers[i];
        }
    }
    return NULL;
}

/** @brief Run @p w's oldest job on the caller's thread. */
static bool run_next(struct khal_worker_s *w)
{
    unsigned char job[FAKE_WORKER_JOB_MAX];

    if ( (NULL == w) || (0U == w->count) )
    {
        return false;
    }
    (void)memcpy(job, w->jobs[w->head], w->job_size);
    w->head = (w->head + 1U) % w->depth;
    --w->count;
    s_in_job = w;
    w->run(job);
    s_in_job = NULL;
    return true;
}

bool fake_khal_worker_run_next(void)
{
    return run_next(s_last);
}

bool fake_khal_worker_run_next_for(char const *name)
{
    return run_next(find(name));
}

unsigned fake_khal_worker_pending(void)
{
    if ( NULL == s_last )
    {
        return 0U;
    }
    return s_last->count;
}

unsigned fake_khal_worker_pending_for(char const *name)
{
    struct khal_worker_s const *const w = find(name);

    if ( NULL == w )
    {
        return 0U;
    }
    return w->count;
}

void fake_khal_worker_refuse_for(char const *name, bool refuse)
{
    struct khal_worker_s *const w = find(name);

    if ( NULL != w )
    {
        w->refuse = refuse;
    }
}

bool fake_khal_worker_last_psram(void)
{
    if ( NULL == s_last )
    {
        return false;
    }
    return s_last->psram;
}

bool fake_khal_worker_last_psram_for(char const *name)
{
    struct khal_worker_s const *const w = find(name);

    if ( NULL == w )
    {
        return false;
    }
    return w->psram;
}

kode_status_t khal_worker_create(khal_worker_t *out, char const *name,
                                 uint32_t stack_bytes, uint8_t prio,
                                 uint8_t depth, size_t job_size,
                                 khal_worker_run_t run, bool psram_stack)
{
    struct khal_worker_s *w;

    (void)prio;
    if ( (NULL == out) || (NULL == name) || (NULL == run) || (0U == depth) ||
        (FAKE_WORKER_DEPTH_MAX < depth) || (0U == job_size) ||
        (FAKE_WORKER_JOB_MAX < job_size) )
    {
        return KODE_DENIED;
    }
    w = find(name);
    if ( NULL == w )
    {
        for ( unsigned i = 0U; (FAKE_WORKER_SLOTS > i) && (NULL == w); ++i )
        {
            if ( !s_workers[i].used )
            {
                w = &s_workers[i];
            }
        }
    }
    if ( NULL == w )
    {
        return KODE_NO_RESOURCES;
    }
    (void)memset(w, 0, sizeof(*w));
    (void)strncpy(w->name, name, FAKE_WORKER_NAME_MAX - 1U);
    w->used     = true;
    w->run      = run;
    w->job_size = job_size;
    w->depth    = depth;
    w->psram    = psram_stack;
    w->stack_free = stack_bytes;
    s_last      = w;
    *out        = w;
    return KODE_OK;
}

kode_status_t khal_worker_submit(khal_worker_t w, void const *job)
{
    if ( (NULL == w) || (NULL == job) || !w->used )
    {
        return KODE_DENIED;
    }
    if ( w->refuse || (w->count >= w->depth) )
    {
        return KODE_NO_RESOURCES;
    }
    (void)memcpy(w->jobs[(w->head + w->count) % w->depth], job, w->job_size);
    ++w->count;
    return KODE_OK;
}

bool khal_worker_is_self(khal_worker_t w)
{
    return (NULL != w) && (s_in_job == w);
}

uint32_t khal_worker_stack_free(khal_worker_t w)
{
    if ( (NULL == w) || !w->used )
    {
        return 0U;
    }
    return w->stack_free;
}

void fake_khal_worker_set_stack_free_for(char const *name, uint32_t bytes)
{
    struct khal_worker_s *const w = find(name);

    if ( NULL != w )
    {
        w->stack_free = bytes;
    }
}
