/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_scan.c
 * @brief The blocking Wi-Fi and BLE scans' claim and wait: one at a time, stale answers skipped.
 */
#include "kode_sys_priv.h"

#include "khal_system.h"

bool kode_sys_scan_make(kode_sys_scan_t *s)
{
    s->sem  = khal_sem_create();
    s->lock = khal_mutex_create();
    if ( (NULL == s->sem) || (NULL == s->lock) )
    {
        return false;
    }
    s->wait.done = s->sem;
    s->wait.cap  = s->cap;
    return true;
}

void kode_sys_scan_release(kode_sys_scan_t *s)
{
    khal_mutex_lock(s->lock);
    s->busy = false;
    khal_mutex_unlock(s->lock);
}

kode_status_t kode_sys_scan_enter(kode_sys_scan_t *s, khal_once_try_body_t make, uint32_t ms)
{
    uint32_t stale = 0U;

    if ( !khal_once_try(&s->made, make) )
    {
        return KODE_NO_RESOURCES;
    }
    khal_mutex_lock(s->lock);
    if ( s->busy )
    {
        khal_mutex_unlock(s->lock);
        return KODE_NO_RESOURCES;
    }
    s->busy = true;
    stale   = s->stale;
    khal_mutex_unlock(s->lock);

    /* Kept for the boot: an abandoned request holds a pointer into it. */
    if ( NULL == s->wait.rows )
    {
        s->wait.rows = khal_alloc_psram(sizeof(uint32_t), s->row_size * (size_t)s->cap);
        if ( NULL == s->wait.rows )
        {
            kode_sys_scan_release(s);
            return KODE_NO_RESOURCES;
        }
    }

    if ( 0U == stale )
    {
        return KODE_OK;
    }
    if ( !kode_sys_answer_await(s->sem, &s->wait.ack, stale, ms) )
    {
        /* Still owed: the latch stays, and holding the claim would add nothing. */
        kode_sys_scan_release(s);
        return KODE_NO_RESOURCES;
    }
    khal_mutex_lock(s->lock);
    s->stale = 0U;
    khal_mutex_unlock(s->lock);
    return KODE_OK;
}

uint32_t kode_sys_scan_gen(kode_sys_scan_t *s)
{
    khal_mutex_lock(s->lock);
    s->gen += 1U;
    if ( 0U == s->gen )
    {
        s->gen = 1U;
    }
    uint32_t const gen = s->gen;
    khal_mutex_unlock(s->lock);
    return gen;
}

kode_status_t kode_sys_scan_await(kode_sys_scan_t *s, uint32_t gen, uint32_t ms)
{
    kode_status_t rc;

    if ( !kode_sys_answer_await(s->sem, &s->wait.ack, gen, ms) )
    {
        khal_mutex_lock(s->lock);
        s->stale = gen;
        khal_mutex_unlock(s->lock);
        rc = KODE_TIMEOUT;
    }
    else
    {
        rc = (kode_status_t)s->wait.rc;
    }
    return rc;
}
