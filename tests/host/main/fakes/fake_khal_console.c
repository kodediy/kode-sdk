/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_console.c
 * @brief The console transport, captured rather than mocked.
 */
#include <string.h>

#include "fakes.h"
#include "khal_console.h"

#define OUT_CAP  2048
#define RAW_CAP  512

static char     s_out[OUT_CAP + 1];
static size_t   s_out_len;
static unsigned s_inits;

static uint8_t  s_raw[RAW_CAP];
static size_t   s_raw_len;
static size_t   s_raw_taken;

static void (*s_pump)(void);
static void (*s_armed)(void);
static bool     s_raw_open;
static bool     s_raw_refuse;

/* The line reader's callback, captured rather than run: registering it does
 * not start a task here (the design note by khal_console_start_reader()
 * below explains why), but a case that wants log_on_byte()'s own retry
 * behaviour under test - not handle_byte()'s, which every other case in
 * this file dispatches straight into - needs a way to call it. */
static void (*s_on_byte)(uint8_t byte);

/* What khal_console_rest() was told last. */
static bool     s_resting;

/* The dock's pins: lent or held, how many calls, and what the next take answers. */
static bool          s_dock_lent;
static unsigned      s_dock_releases;
static unsigned      s_dock_takes;
static kode_status_t s_dock_take_rc;

void fake_console_reset(void)
{
    (void)memset(s_out, 0, sizeof(s_out));
    s_out_len    = 0U;
    s_inits      = 0U;
    s_raw_len    = 0U;
    s_raw_taken  = 0U;
    s_pump       = NULL;
    s_armed      = NULL;
    s_raw_open   = false;
    s_raw_refuse = false;
    s_on_byte    = NULL;
    s_dock_lent     = false;
    s_dock_releases = 0U;
    s_dock_takes    = 0U;
    s_dock_take_rc  = KODE_OK;
}

void fake_console_clear_out(void)
{
    (void)memset(s_out, 0, sizeof(s_out));
    s_out_len = 0U;
}

const char *fake_console_out(void)
{
    return s_out;
}

size_t fake_console_out_len(void)
{
    return s_out_len;
}

unsigned fake_console_inits(void)
{
    return s_inits;
}

void fake_console_feed_raw(const void *data, size_t len)
{
    if ( sizeof(s_raw) < len )
    {
        len = sizeof(s_raw);
    }
    (void)memcpy(s_raw, data, len);
    s_raw_len   = len;
    s_raw_taken = 0U;
}

void fake_console_raw_refuse(bool refuse)
{
    s_raw_refuse = refuse;
}

bool fake_console_raw_open(void)
{
    return s_raw_open;
}

void fake_console_raw_run_pump(void)
{
    if ( NULL != s_armed )
    {
        s_armed();
    }
    if ( NULL != s_pump )
    {
        s_pump();
    }
}

/* ---- The seam itself ------------------------------------------------------ */

void khal_console_init(void)
{
    ++s_inits;
}

void khal_console_write(const char *buf, size_t len)
{
    if ( (NULL == buf) || (0U == len) )
    {
        return;
    }
    /* Silently dropping the overflow rather than wrapping: a case that writes
     * more than the cap wants a bigger cap, not a confusing tail. */
    size_t const room = (size_t)OUT_CAP - s_out_len;
    size_t const n    = (len > room) ? room : len;
    (void)memcpy(&s_out[s_out_len], buf, n);
    s_out_len += n;
    s_out[s_out_len] = '\0';
}

void khal_console_rest(bool rest)
{
    s_resting = rest;
}

bool fake_console_resting(void)
{
    return s_resting;
}

kode_status_t khal_console_start_reader(void (*on_byte)(uint8_t byte),
                                        uint32_t prio)
{
    (void)prio;
    /* No task: an event source that blocks is exactly what a host suite must
     * not start. A case that wants bytes delivered dispatches them, which is
     * why the pointer is only captured, never called from here - a case
     * that specifically wants the reader's own allocation-retry behaviour
     * invokes it itself, synchronously, through fake_console_feed_byte(). */
    s_on_byte = on_byte;
    return KODE_OK;
}

void fake_console_feed_byte(int c)
{
    if ( NULL != s_on_byte )
    {
        s_on_byte((uint8_t)c);
    }
}

kode_status_t khal_console_raw_begin(void (*pump)(void), void (*on_armed)(void))
{
    if ( s_raw_refuse )
    {
        return KODE_ERROR;
    }
    s_pump     = pump;
    s_armed    = on_armed;
    s_raw_open = true;
    return KODE_OK;
}

void khal_console_raw_end(void)
{
    s_raw_open = false;
    s_pump     = NULL;
    s_armed    = NULL;
}

int32_t khal_console_read_raw(void *buf, size_t want, uint32_t timeout_ms)
{
    (void)timeout_ms;
    if ( (NULL == buf) || (0U == want) )
    {
        return 0;
    }
    size_t const left = s_raw_len - s_raw_taken;
    size_t const n    = (want > left) ? left : want;
    (void)memcpy(buf, &s_raw[s_raw_taken], n);
    s_raw_taken += n;
    return (int32_t)n;
}

kode_status_t khal_console_dock_release(void)
{
    s_dock_releases += 1U;
    s_dock_lent      = true;
    return KODE_OK;
}

kode_status_t khal_console_dock_take(void)
{
    s_dock_takes += 1U;
    if ( KODE_OK == s_dock_take_rc )
    {
        s_dock_lent = false;
    }
    return s_dock_take_rc;
}

bool fake_console_dock_lent(void)
{
    return s_dock_lent;
}

unsigned fake_console_dock_releases(void)
{
    return s_dock_releases;
}

unsigned fake_console_dock_takes(void)
{
    return s_dock_takes;
}

void fake_console_dock_take_rc(kode_status_t rc)
{
    s_dock_take_rc = rc;
}
