/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_klog_sink.c
 * @brief A capture sink for the real klog.
 */
#include <stdio.h>
#include <string.h>

#include "fakes.h"
#include "kode_log.h"

/* `ps` alone emits one record per AO, one per resource in kbsp's table and
 * one per configured expansion position, which passed thirty-two lines long
 * before any of them could be asserted about. The cap only bounds what is
 * KEPT - s_count below counts every line either way - so raising it changes
 * no case's arithmetic, only how far into a burst one can read. */
#define CAP_LINES 80
#define CAP_LEN   288

static char     s_lines[CAP_LINES][CAP_LEN];
static uint8_t  s_srcs[CAP_LINES];
static unsigned s_count;

/**
 * @brief Receives one formatted line from klog.
 * @param line NUL-terminated, colour and newline included.
 * @param len  Its length, terminator excluded.
 * @param src  The tag it was sent with.
 */
static void capture(const char *line, size_t len, uint8_t src)
{
    (void)len;
    if ( CAP_LINES > s_count )
    {
        (void)snprintf(s_lines[s_count], CAP_LEN, "%s", line);
        s_srcs[s_count] = src;
    }
    ++s_count;   /* keeps counting past the cap, so a case can see overflow */
}

void fake_klog_reset(void)
{
    s_count = 0U;
    (void)memset(s_lines, 0, sizeof(s_lines));
    (void)memset(s_srcs, (int)KODE_LOG_SRC_NONE, sizeof(s_srcs));
    /* Not optional: with no sink, klog sends every line to esp_rom_printf on the suite's output. */
    kode_log_set_sink(&capture);
    kode_log_set_level(NULL, KODE_LOG_DEBUG);
}

unsigned fake_klog_count(void)
{
    return s_count;
}

const char *fake_klog_line(unsigned idx)
{
    return (idx < s_count) && (CAP_LINES > idx) ? s_lines[idx] : NULL;
}

unsigned char fake_klog_src(unsigned idx)
{
    return (idx < s_count) && (CAP_LINES > idx) ? s_srcs[idx] : (uint8_t)KODE_LOG_SRC_NONE;
}

const char *fake_klog_last(void)
{
    return (0U < s_count) ? fake_klog_line(s_count - 1U) : NULL;
}

void fake_klog_detach(void)
{
    kode_log_set_sink(NULL);
}
