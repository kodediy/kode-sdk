/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_system.c
 * @brief The standard layer's system calls: delay, uptime, clock, device id, home, console, random.
 */
#include "kode.h"

/* cppcheck-suppress misra-c2012-17.1 ; kode_print() is printf-style, bounded by vsnprintf() */
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>

#include "khal_system.h"
#include "kode_entry.h"
#include "kode_log.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"

/** @brief The tag an app's kode_print() lines carry. */
#define KODE_PRINT_TAG "app"

/** @brief What kode_time_unix() answers while the clock is not set, or refused. */
#define KODE_CLOCK_UNSET (-1)

/* kode_time_t is kode_sys_time_t's twin, field for field. */
_Static_assert(sizeof(kode_time_t) == sizeof(kode_sys_time_t),
               "kode_time_t is kode_sys_time_t's size");
_Static_assert(offsetof(kode_time_t, year) == offsetof(kode_sys_time_t, year),
               "year is where kode_sys_time_t has it");
_Static_assert(offsetof(kode_time_t, month) == offsetof(kode_sys_time_t, month),
               "month is where kode_sys_time_t has it");
_Static_assert(offsetof(kode_time_t, day) == offsetof(kode_sys_time_t, day),
               "day is where kode_sys_time_t has it");
_Static_assert(offsetof(kode_time_t, hour) == offsetof(kode_sys_time_t, hour),
               "hour is where kode_sys_time_t has it");
_Static_assert(offsetof(kode_time_t, minute) == offsetof(kode_sys_time_t, minute),
               "minute is where kode_sys_time_t has it");
_Static_assert(offsetof(kode_time_t, second) == offsetof(kode_sys_time_t, second),
               "second is where kode_sys_time_t has it");
_Static_assert(offsetof(kode_time_t, weekday) == offsetof(kode_sys_time_t, weekday),
               "weekday is where kode_sys_time_t has it");

/** @brief What kode_device_id() answers when the call was refused. */
static const char s_no_id[] = "";

/** @brief How long kode_exit() sleeps between looks while AO_Boot leaves and resets. */
#define KODE_EXIT_WAIT_MS  1000U

void kode_delay(uint32_t ms)
{
    if ( kode_enter("kode_delay") )
    {
        kode_block_delay_ms(ms);
    }
}

uint32_t kode_uptime_ms(void)
{
    uint32_t ms = 0U;

    if ( kode_enter("kode_uptime_ms") )
    {
        ms = kode_sys_time_ms();
    }
    return ms;
}

int64_t kode_time_unix(void)
{
    int64_t unix_s = KODE_CLOCK_UNSET;

    if ( kode_enter("kode_time_unix") )
    {
        int64_t now = 0;

        if ( kode_sys_clock_read(&now) == KODE_OK )
        {
            unix_s = now;
        }
        else
        {
            kode_err_set("the clock is not set");
        }
    }
    return unix_s;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_time_local(kode_time_t *out)
{
    bool ok = false;

    if ( kode_enter("kode_time_local") )
    {
        kode_sys_time_t now;

        if ( NULL == out )
        {
            kode_err_set("time is NULL");
        }
        else if ( kode_sys_clock_local(&now) == KODE_OK )
        {
            out->year    = now.year;
            out->month   = now.month;
            out->day     = now.day;
            out->hour    = now.hour;
            out->minute  = now.minute;
            out->second  = now.second;
            out->weekday = now.weekday;
            ok = true;
        }
        else
        {
            kode_err_set("the clock is not set");
        }
    }
    return ok;
}

const char *kode_device_id(void)
{
    const char *id = s_no_id;

    if ( kode_enter("kode_device_id") )
    {
        id = kode_sys_device_id();
        if ( NULL == id )
        {
            id = s_no_id;
        }
    }
    return id;
}

void kode_exit(void)
{
    if ( kode_enter("kode_exit") )
    {
        kode_status_t rc = kode_sys_exit_to_os();

        /* Asked again until AO_Boot's queue takes it; AO_Boot then resets after
         * its bounded leave (KODE_LEAVE_BUDGET_MS at most), so this never returns. */
        for ( ;; )
        {
            kode_block_delay_ms(KODE_EXIT_WAIT_MS);
            if ( KODE_OK != rc )
            {
                rc = kode_sys_exit_to_os();
            }
        }
    }
}

void kode_print(const char *fmt, ...)
{
    if ( kode_enter("kode_print") )
    {
        if ( NULL == fmt )
        {
            kode_err_set("format is NULL");
        }
        else
        {
            char text[KODE_PRINT_MAX];
            /* cppcheck-suppress misra-c2012-17.1 ; the list is handed straight to vsnprintf() */
            va_list ap;
            /* cppcheck-suppress misra-c2012-17.1 ; the same list */
            va_start(ap, fmt);
            const int32_t n = (int32_t)vsnprintf(text, sizeof(text), fmt, ap);
            /* cppcheck-suppress misra-c2012-17.1 ; the same list */
            va_end(ap);

            if ( 0 > n )
            {
                kode_err_set("the format could not be printed");
            }
            else
            {
                /* A longer text is already cut to the buffer by vsnprintf(). */
                kode_log_always(KODE_LOG_NOTICE, KODE_PRINT_TAG, "%s", text);
            }
        }
    }
}

/**
 * @brief A draw from khal_random() brought into 0 to @p span, every number
 *        equally likely.
 * @param span The largest number it may answer.
 * @return The number.
 */
static uint32_t random_upto(uint32_t span)
{
    uint32_t draw = khal_random();

    if ( UINT32_MAX > span )
    {
        const uint32_t count = span + 1U;
        /* The largest multiple of count a draw can reach: a draw at or past it
         * is drawn again, or the low numbers would come up more often. */
        const uint32_t limit = UINT32_MAX - (UINT32_MAX % count);

        while ( draw >= limit )
        {
            draw = khal_random();
        }
        draw %= count;
    }
    return draw;
}

uint32_t kode_random(uint32_t min, uint32_t max)
{
    uint32_t n = min;

    if ( kode_enter("kode_random") )
    {
        if ( max < min )
        {
            kode_err_set("max %u is below min %u", (unsigned)max, (unsigned)min);
        }
        else
        {
            n = min + random_upto(max - min);
        }
    }
    return n;
}
