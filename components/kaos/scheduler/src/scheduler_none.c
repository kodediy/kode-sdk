/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file scheduler_none.c
 * @brief The scheduler's doors in a build without CONFIG_KODE_SCHEDULER.
 */
#include "kaos.h"

#include "kode_log.h"

#define TAG "kaos.sched"

bool sched_request_set(QActive *who, const char *name, const char *when,
                       const char *cmd)
{
    (void)who;
    (void)name;
    (void)when;
    (void)cmd;
    KLOG_W(TAG, "the scheduler is not built: the request to set an entry is refused");
    return false;
}

bool sched_request_set_ao(QActive *who, const char *name, const char *when,
                          QActive *target, enum_t sig)
{
    (void)who;
    (void)name;
    (void)when;
    (void)target;
    (void)sig;
    KLOG_W(TAG, "the scheduler is not built: the request to set an entry is refused");
    return false;
}

bool sched_request_remove(QActive *who, const char *name)
{
    (void)who;
    (void)name;
    KLOG_W(TAG, "the scheduler is not built: the request to remove an entry is refused");
    return false;
}

bool sched_request_list(QActive *who, const char *name)
{
    (void)who;
    (void)name;
    KLOG_W(TAG, "the scheduler is not built: the request to list the entries is refused");
    return false;
}

void sched_line_done(void)
{
    /* No line was handed out, so none is awaited. */
}

void sched_line_busy(void)
{
    /* No line was handed out, so none is refused. */
}

void sched_check_done(int64_t passed_s)
{
    (void)passed_s;
    /* No check is given to AO_Net, so none is awaited. */
}
