/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_entry.c
 * @brief kode_enter(), kode_err_set(), kode_error(), kode_retry() and the shared name comparison.
 */
#include "kode_entry.h"

/* cppcheck-suppress misra-c2012-17.1 ; a printf-style error text needs a variable argument list, bounded by vsnprintf() */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kode.h"
#include "kode_file.h"
#include "kode_http.h"
#include "kode_log.h"
#include "kode_tag.h"
#include "kode_task.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kqp.h"
#include "kaos.h"

/* The three modules that keep state per task, named weakly (KAOS_WEAK,
 * kaos.h): a module the app never called made no state, and freeing it must
 * not link the module - with it, its owner. A state that exists names its
 * module linked. */
KAOS_WEAK void kode_file_state_free(struct kode_file_state *s);
KAOS_WEAK void kode_http_state_free(struct kode_http_state *s);
KAOS_WEAK void kode_tag_state_free(struct kode_tag_state *s);

#define TAG "kode"

/** @brief The ellipsis a cut text ends in, as UTF-8. */
#define KODE_ELLIPSIS "\xE2\x80\xA6"

/** @brief Bytes of KODE_ELLIPSIS. */
#define KODE_ELLIPSIS_LEN 3U

/** @brief Mask that isolates the marker bits of a UTF-8 byte. */
#define UTF8_CONT_MASK 0xC0U

/** @brief What those bits read on a continuation byte (10xxxxxx). */
#define UTF8_CONT_BITS 0x80U

/** @brief The distance between an ASCII capital and its lower-case letter. */
#define KODE_ASCII_CASE_GAP ('a' - 'A')

/** @brief What kode_error() answers when the task has no text. */
static const char s_no_error[] = "";

/**
 * @brief Free a task's state when the task ends.
 *
 * Runs in the task that deletes this one or in the idle task (the slot's free
 * function), so it takes no lock, waits for nothing and logs nothing. It does
 * not touch the list of waiting tasks: a task is never deleted while it waits
 * inside a call, and kode.h says so.
 *
 * @param p The task's kode_task_t.
 */
static void kode_task_free(void *p)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the slot holds the kode_task_t kode_task_ensure() put there */
    kode_task_t *const task = (kode_task_t *)p;

    if ( NULL != task )
    {
        if ( NULL != task->file )
        {
            kode_file_state_free(task->file);
        }
        if ( NULL != task->http )
        {
            kode_http_state_free(task->http);
        }
        khal_free(task->setting);
        if ( NULL != task->tag )
        {
            kode_tag_state_free(task->tag);
        }
        khal_sem_delete(task->wait_sem);
        khal_free(task);
    }
}

kode_task_t *kode_task_get(void)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the slot holds the kode_task_t kode_task_ensure() put there */
    return (kode_task_t *)khal_task_slot_get();
}

kode_task_t *kode_task_ensure(void)
{
    kode_task_t *task = kode_task_get();

    if ( NULL == task )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a kode_task_t */
        task = (kode_task_t *)khal_alloc_psram(sizeof(void *), sizeof(*task));
        if ( NULL != task )
        {
            (void)memset(task, 0, sizeof(*task));
            khal_task_slot_set_owned(task, &kode_task_free);
        }
    }
    return task;
}

const char *kode_error(void)
{
    const kode_task_t *const task = kode_task_get();

    if ( NULL == task )
    {
        return s_no_error;
    }
    return task->error;
}

/**
 * @brief Cut @p buf, which holds a text longer than it can, to end in "…".
 *
 * The cut never leaves half a UTF-8 character before the ellipsis.
 *
 * @param buf A buffer of KODE_ERROR_MAX bytes, full.
 */
static void error_cut(char *buf)
{
    size_t at = KODE_ERROR_MAX - 1U - KODE_ELLIPSIS_LEN;

    while ( (0U < at) && (((uint8_t)buf[at] & UTF8_CONT_MASK) == UTF8_CONT_BITS) )
    {
        --at;
    }
    (void)memcpy(&buf[at], KODE_ELLIPSIS, KODE_ELLIPSIS_LEN);
    buf[at + KODE_ELLIPSIS_LEN] = '\0';
}

void kode_err_set(const char *fmt, ...)
{
    kode_task_t *const task = kode_task_ensure();

    if ( NULL == task )
    {
        return;
    }
    char *const buf = task->error;

    /* cppcheck-suppress misra-c2012-17.1 ; the list is handed straight to vsnprintf() */
    va_list ap;
    /* cppcheck-suppress misra-c2012-17.1 ; the same list */
    va_start(ap, fmt);
    const int32_t n = (int32_t)vsnprintf(buf, KODE_ERROR_MAX, fmt, ap);
    /* cppcheck-suppress misra-c2012-17.1 ; the same list */
    va_end(ap);

    if ( 0 > n )
    {
        buf[0] = '\0';
    }
    else if ( (size_t)n >= KODE_ERROR_MAX )
    {
        error_cut(buf);
    }
    else
    {
        /* It fit whole. */
    }
}

bool kode_enter(const char *name)
{
    const char *ao = NULL;
    const char *who = s_no_error;

    if ( NULL != name )
    {
        who = name;
    }
    if ( kqp_current_is_ao(&ao) )
    {
        kode_err_set("%s was called from an Active Object (%s); kode.h calls "
                     "block - call kode_sys_* instead", who, ao);
        KLOG_E(TAG, "%s", kode_error());
        return false;
    }
    kode_init();

    kode_task_t *const task = kode_task_get();

    if ( NULL != task )
    {
        task->error[0] = '\0';
    }
    return true;
}

char kode_ascii_lower(char c)
{
    char lower = c;

    if ( ('A' <= c) && ('Z' >= c) )
    {
        lower = (char)(c + KODE_ASCII_CASE_GAP);
    }
    return lower;
}

bool kode_name_is(const char *name, const char *word)
{
    size_t i = 0U;

    while ( ('\0' != name[i]) && ('\0' != word[i])
           && (kode_ascii_lower(name[i]) == word[i]) )
    {
        i++;
    }
    return ('\0' == name[i]) && ('\0' == word[i]);
}

bool kode_has_prefix(const char *text, const char *prefix)
{
    size_t i = 0U;

    while ( ('\0' != prefix[i]) && ('\0' != text[i])
           && (kode_ascii_lower(text[i]) == prefix[i]) )
    {
        i++;
    }
    return '\0' == prefix[i];
}

bool kode_retry(bool (*attempt)(void *ctx), void *ctx, uint32_t bound_ms)
{
    if ( NULL == attempt )
    {
        return false;
    }

    const uint32_t start = kode_sys_time_ms();
    bool           done  = attempt(ctx);

    while ( !done )
    {
        uint32_t nap = KODE_RETRY_STEP_MS;

        if ( KODE_FOREVER != bound_ms )
        {
            const uint32_t spent = kode_sys_time_ms() - start;

            if ( spent >= bound_ms )
            {
                break;
            }
            if ( (bound_ms - spent) < nap )
            {
                nap = bound_ms - spent;
            }
        }
        kode_block_delay_ms(nap);
        done = attempt(ctx);
    }
    return done;
}
