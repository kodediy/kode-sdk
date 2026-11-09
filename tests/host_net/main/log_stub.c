/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file log_stub.c
 * @brief klog, silenced, for a binary that is not testing klog.
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "kode_log.h"
#include "host_spies.h"

#define LOG_STUB_LINES     16U    /**< Lines kept, the oldest overwritten. */
#define LOG_STUB_LINE_MAX  320U   /**< One formatted line, terminator included. */

static char     s_lines[LOG_STUB_LINES][LOG_STUB_LINE_MAX];
static unsigned s_next;

void kode_log_init(void) {}

void kode_log_va(kode_log_lvl_t lvl, const char *tag, const char *fmt, va_list ap)
{
    (void)lvl; (void)tag;
    (void)vsnprintf(s_lines[s_next], sizeof(s_lines[0]), fmt, ap);
    s_next = (s_next + 1U) % LOG_STUB_LINES;
}

void kode_log(kode_log_lvl_t lvl, const char *tag, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    kode_log_va(lvl, tag, fmt, ap);
    va_end(ap);
}

bool log_stub_saw(const char *needle)
{
    for ( unsigned i = 0U; LOG_STUB_LINES > i; ++i )
    {
        if ( strstr(s_lines[i], needle) != NULL )
        {
            return true;
        }
    }
    return false;
}

void log_stub_forget(void)
{
    (void)memset(s_lines, 0, sizeof(s_lines));
    s_next = 0U;
}
void kode_log_set_level(const char *tag, kode_log_lvl_t min)
{ (void)tag; (void)min; }

/* The holds are recorded, not enforced: a case asks whether kdrv_net held the
 * HTTP client's tag at the moment that matters (the request going out). */
static const char    *s_held[KODE_LOG_HOLD_MAX];
static kode_log_lvl_t s_held_min[KODE_LOG_HOLD_MAX];
static bool           s_refuse;

bool kode_log_hold(const char *tag, kode_log_lvl_t min)
{
    if ( (NULL == tag) || s_refuse )
    {
        return false;
    }
    for ( unsigned i = 0U; KODE_LOG_HOLD_MAX > i; ++i )
    {
        if ( (NULL == s_held[i]) || (strcmp(s_held[i], tag) == 0) )
        {
            s_held[i]     = tag;
            s_held_min[i] = min;
            return true;
        }
    }
    return false;
}

void kode_log_release(const char *tag)
{
    for ( unsigned i = 0U; (NULL != tag) && (KODE_LOG_HOLD_MAX > i); ++i )
    {
        if ( (NULL != s_held[i]) && (strcmp(s_held[i], tag) == 0) )
        {
            s_held[i] = NULL;
        }
    }
}

bool log_stub_held(const char *tag, kode_log_lvl_t *min)
{
    for ( unsigned i = 0U; KODE_LOG_HOLD_MAX > i; ++i )
    {
        if ( (NULL != s_held[i]) && (strcmp(s_held[i], tag) == 0) )
        {
            if ( NULL != min )
            {
                *min = s_held_min[i];
            }
            return true;
        }
    }
    return false;
}

void log_stub_refuse_holds(bool refuse)
{
    s_refuse = refuse;
}
