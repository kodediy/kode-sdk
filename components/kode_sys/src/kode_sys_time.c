/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_time.c
 * @brief The time, the board's name and the wall clock, in UTC and in local
 *        time: read in the caller's task.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"

/* cppcheck-suppress misra-c2012-21.10 ; khal_time_local() answers in the C library's struct tm */
#include <time.h>

#include "khal_identity.h"
#include "khal_time.h"

/** @brief Microseconds in a second. */
#define KODE_SYS_US_PER_S 1000000ULL

/** @brief The year struct tm counts its years from. */
#define KODE_SYS_TM_YEAR_BASE  1900
/** @brief struct tm counts months from 0; kode_sys_time_t from 1. */
#define KODE_SYS_TM_MONTH_BASE 1

void kode_block_delay_ms(uint32_t ms)
{
    khal_delay_ms(ms);
}

uint32_t kode_sys_time_ms(void)
{
    return khal_time_ms();
}

const char *kode_sys_device_id(void)
{
    return khal_identity_serial();
}

kode_status_t kode_sys_clock_read(int64_t *unix_s)
{
    kode_status_t st = KODE_DENIED;

    if ( NULL != unix_s )
    {
        st = KODE_ERROR;
        if ( khal_time_wall_known() )
        {
            *unix_s = (int64_t)(khal_time_wall_us() / KODE_SYS_US_PER_S);
            st = KODE_OK;
        }
    }
    return st;
}

kode_status_t kode_sys_clock_local(kode_sys_time_t *out)
{
    kode_status_t st = KODE_DENIED;
    int64_t       now = 0;
    struct tm     t;

    if ( NULL != out )
    {
        st = KODE_ERROR;
        if ( (kode_sys_clock_read(&now) == KODE_OK) && khal_time_local(now, &t) )
        {
            out->year    = (uint16_t)(t.tm_year + KODE_SYS_TM_YEAR_BASE);
            out->month   = (uint8_t)(t.tm_mon + KODE_SYS_TM_MONTH_BASE);
            out->day     = (uint8_t)t.tm_mday;
            out->hour    = (uint8_t)t.tm_hour;
            out->minute  = (uint8_t)t.tm_min;
            out->second  = (uint8_t)t.tm_sec;
            out->weekday = (uint8_t)t.tm_wday;
            st = KODE_OK;
        }
    }
    return st;
}
