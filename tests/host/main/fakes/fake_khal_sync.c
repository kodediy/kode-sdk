/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_sync.c
 * @brief Locks that lock nothing, because a host case is one thread.
 */
#include <stdatomic.h>

#include "fakes.h"
#include "khal_sync.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* Distinct real addresses: a create() is never NULL, and a mutex cannot pass for a semaphore. */
static int s_mutex_token;
static int s_sem_token;

/* Every semaphore made, fake or real; never reset, so a case reads a difference. */
static atomic_uint s_sem_creates;

static bool     s_real;
static bool     s_mutex_real;
static void    *s_wait_hook_owner;

static unsigned s_signals;
static bool     s_token;
static uint32_t s_last_wait_ms;
static void   (*s_wait_hook)(void);

/* One semaphore only: tasks of earlier cases end in the background and delete their own. */
static _Atomic(khal_sem_t) s_watched;       /**< The semaphore fake_sem_watch() counts deletions of. */

static SemaphoreHandle_t s_env;             /**< The environment lock, made on its first take. */
static unsigned          s_env_depth;       /**< Takes not yet released; only the holder moves it. */
static void (* _Atomic   s_env_hook)(void); /**< Runs once inside the next outermost release. */
static atomic_uint         s_watch_deletes; /**< How many times it was deleted. */

void fake_sync_reset(void)
{
    s_signals      = 0U;
    s_token        = false;
    s_last_wait_ms = 0U;
    s_wait_hook    = NULL;
    s_real         = false;
    s_mutex_real   = false;
    fake_sem_watch(NULL);
    atomic_store(&s_env_hook, NULL);
}

void fake_env_release_hook(void (*hook)(void))
{
    atomic_store(&s_env_hook, hook);
}

/* Always a real recursive mutex, the kind the C library's own lock is on the board. */
void khal_env_lock(void)
{
    if ( NULL == s_env )
    {
        s_env = xSemaphoreCreateRecursiveMutex();
    }
    (void)xSemaphoreTakeRecursive(s_env, portMAX_DELAY);
    ++s_env_depth;
}

void khal_env_unlock(void)
{
    if ( 1U == s_env_depth )
    {
        void (* const hook)(void) = atomic_exchange(&s_env_hook, NULL);

        if ( NULL != hook )
        {
            hook();
        }
    }
    --s_env_depth;
    (void)xSemaphoreGiveRecursive(s_env);
}

void fake_sem_watch(khal_sem_t s)
{
    atomic_store(&s_watched, s);
    atomic_store(&s_watch_deletes, 0U);
}

unsigned fake_sem_watch_deletes(void)
{
    return atomic_load(&s_watch_deletes);
}

void fake_sync_real(bool on)
{
    s_real = on;
}

void fake_mutex_real(bool on)
{
    s_mutex_real = on;
}

void fake_sem_reset(void)
{
    s_signals      = 0U;
    s_token        = false;
    s_last_wait_ms = 0U;
    s_wait_hook    = NULL;
}

void fake_sem_wait_hook(void (*hook)(void))
{
    s_wait_hook       = hook;
    s_wait_hook_owner = xTaskGetCurrentTaskHandle();
}

unsigned fake_sem_signals(void)
{
    return s_signals;
}

unsigned fake_sem_creates(void)
{
    return atomic_load(&s_sem_creates);
}

uint32_t fake_sem_last_wait_ms(void)
{
    return s_last_wait_ms;
}

khal_mutex_t khal_mutex_create(void)
{
    if ( s_real || s_mutex_real )
    {
        return (khal_mutex_t)xSemaphoreCreateMutex();
    }
    return &s_mutex_token;
}

void khal_mutex_lock(khal_mutex_t m)
{
    if ( (NULL != m) && (m != &s_mutex_token) )
    {
        (void)xSemaphoreTake((SemaphoreHandle_t)m, portMAX_DELAY);
    }
}

void khal_mutex_unlock(khal_mutex_t m)
{
    if ( (NULL != m) && (m != &s_mutex_token) )
    {
        (void)xSemaphoreGive((SemaphoreHandle_t)m);
    }
}

khal_sem_t khal_sem_create(void)
{
    atomic_fetch_add(&s_sem_creates, 1U);
    if ( s_real )
    {
        return (khal_sem_t)xSemaphoreCreateBinary();
    }
    return &s_sem_token;
}

bool khal_sem_wait_timeout(khal_sem_t s, uint32_t ms)
{
    if ( (NULL != s) && (s != &s_sem_token) )
    {
        /* A real semaphore: the wait is a real one. The hook runs only for
         * the task that armed it, so a task that sleeps on its own semaphore
         * (the one under test keeps one) cannot take it. */
        if ( (0U < ms) && (NULL != s_wait_hook)
            && (xTaskGetCurrentTaskHandle() == s_wait_hook_owner) )
        {
            void (* const hook)(void) = s_wait_hook;
            s_wait_hook = NULL;
            hook();
        }
        return xSemaphoreTake((SemaphoreHandle_t)s, pdMS_TO_TICKS(ms)) == pdTRUE;
    }

    s_last_wait_ms = ms;

    /* Whatever another task does while this caller is asleep - and a
     * zero-length poll is not a sleep. On the target it never yields the
     * core, so nothing else can run inside one, and this fake does not
     * pretend otherwise. Cleared before it runs, so a hook that calls
     * straight back in here stops after one pass instead of recursing. */
    if ( (0U < ms) && (NULL != s_wait_hook) )
    {
        void (* const hook)(void) = s_wait_hook;
        s_wait_hook = NULL;
        hook();
    }

    if ( s_token )
    {
        s_token = false;
        return true;
    }
    /* Nothing here can arrive later, so an ungathered token is the only way
     * a wait succeeds - which makes "did somebody complete this flush?" the
     * question a case is actually asking. */
    return false;
}

void khal_sem_delete(khal_sem_t s)
{
    if ( NULL == s )
    {
        return;
    }
    if ( atomic_load(&s_watched) == s )
    {
        atomic_fetch_add(&s_watch_deletes, 1U);
    }
    if ( s != &s_sem_token )
    {
        vSemaphoreDelete((SemaphoreHandle_t)s);
    }
}

void khal_sem_signal(khal_sem_t s)
{
    if ( (NULL != s) && (s != &s_sem_token) )
    {
        (void)xSemaphoreGive((SemaphoreHandle_t)s);
        return;
    }
    ++s_signals;
    s_token = true;   /* binary: a second give over this one is dropped */
}
