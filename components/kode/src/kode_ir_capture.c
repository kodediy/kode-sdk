/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_ir_capture.c
 * @brief The standard layer's infrared capture to a file and sends from a file.
 */
/* Apart from kode_ir.c, so an app that only sends, reads or listens links no file layer. */
#include "kode.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "knec.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_entry.h"
#include "kode_file.h"
#include "kode_ir.h"
#include "kode_ir_file.h"
#include "kode_sys_internal.h"

/** @brief Bytes of the capture task's stack. */
#define KODE_IR_CAPTURE_STACK_BYTES 8192U

/** @brief The name of the capture task. */
#define KODE_IR_CAPTURE_TASK_NAME "kode_ircap"

/** @brief What a capture keeps. */
typedef struct {
    kode_ir_signal_t *sig;                     /**< KODE_IR_FILE_SIGNALS_MAX signals, in PSRAM; NULL: none. */
    char              path[KODE_FILE_PATH_MAX];  /**< Where the stop writes, as typed.                       */
} capture_t;

/** @brief The capture. */
static capture_t s_cap;

/** @brief The capture's task exists, from before it starts to its last statement. */
static atomic_bool s_cap_running;

/** @brief The capture is to end; the task reads it between windows. */
static atomic_bool s_cap_stop;

/** @brief A stop is waiting for the task or writing the file. */
static atomic_bool s_cap_closing;

/** @brief Signals the capture holds. */
static atomic_uint_least32_t s_cap_count;

/** @brief The last status the receiver answered that was not a signal; KODE_OK: none. */
static atomic_uint_least32_t s_cap_fail;

/**
 * @brief The durations of a heard frame, mark first; a duration of 0 reads 1, and a last space of 0 is left out.
 * @param sym The symbols.
 * @param n   How many, 1..KDRV_IR_RX_MAX_SYMBOLS.
 * @param us  Out: 2 * n durations at most.
 * @return How many durations.
 */
static uint32_t durations_from_symbols(const kdrv_ir_symbol_t *sym, uint32_t n, uint16_t *us)
{
    uint32_t count = 0U;

    for ( uint32_t i = 0U; i < n; i++ )
    {
        us[count] = sym[i].mark_us;
        count++;
        us[count] = sym[i].space_us;
        count++;
    }
    if ( 0U == us[count - 1U] )
    {
        count--;
    }
    for ( uint32_t i = 0U; i < count; i++ )
    {
        if ( 0U == us[i] )
        {
            us[i] = 1U;
        }
    }
    return count;
}

/**
 * @brief Keep a heard frame as the capture's next signal, as NEC when it decodes and raw otherwise.
 * @param sym The symbols.
 * @param n   How many, 1..KDRV_IR_RX_MAX_SYMBOLS.
 */
static void capture_keep(const kdrv_ir_symbol_t *sym, uint32_t n)
{
    const uint32_t    at = (uint32_t)atomic_load(&s_cap_count);
    kode_ir_signal_t *s  = &s_cap.sig[at];
    uint16_t          addr = 0U;
    uint16_t          cmd  = 0U;

    (void)memset(s, 0, sizeof(*s));
    if ( knec_decode(sym, n, &addr, &cmd) )
    {
        s->address = addr;
        s->command = cmd;
    }
    else
    {
        s->raw        = true;
        s->carrier_hz = KDRV_IR_CARRIER_HZ;
        s->count      = durations_from_symbols(sym, n, s->us);
    }
    atomic_store(&s_cap_count, at + 1U);
}

/**
 * @brief Listen for one window with the path to itself, and keep what is heard.
 * @param sym Where the window writes.
 * @return false when the receiver failed in a way a next window will not mend.
 */
static bool capture_window(kdrv_ir_symbol_t *sym)
{
    uint32_t      n  = 0U;
    kode_status_t   rc = KODE_OK;
    bool            fatal = false;
    kode_ir_heard_t heard = KODE_IR_QUIET;

    /* The lock exists: the start took it. */
    if ( kode_ir_lock_take("kode_ir_capture_start") )
    {
        heard = kode_ir_listen(sym, &n, KODE_IR_CAPTURE_WINDOW_MS, &rc);
        kode_ir_leave();
    }
    if ( KODE_IR_HEARD == heard )
    {
        capture_keep(sym, n);
    }
    else if ( KODE_IR_FAILED == heard )
    {
        atomic_store(&s_cap_fail, (uint32_t)rc);
        /* A path someone else holds, or a radio that is off, may be free at the next window. */
        fatal = (KODE_DENIED != rc);
    }
    else
    {
        /* a quiet window */
    }
    return !fatal;
}

/** @brief The capture's task: window after window until told to stop, or full, or the receiver fails. */
static void capture_task(void *arg)
{
    kdrv_ir_symbol_t sym[KDRV_IR_RX_MAX_SYMBOLS];
    bool             going = true;

    (void)arg;
    while ( going && !atomic_load(&s_cap_stop) && (atomic_load(&s_cap_count) < KODE_IR_FILE_SIGNALS_MAX) )
    {
        going = capture_window(sym);
        if ( going )
        {
            /* Out of the path for a moment, so a call waiting for its turn gets it. */
            kode_block_delay_ms(KODE_RETRY_STEP_MS);
        }
    }
    atomic_store(&s_cap_running, false);
}

/** @brief Whether the capture's task has ended. */
static bool capture_ended(void *ctx)
{
    (void)ctx;
    return !atomic_load(&s_cap_running);
}

/**
 * @brief Take the memory of a capture and start its task, under the lock.
 * @param path The file's path, valid.
 * @return true when the capture runs; false with the error set.
 */
static bool capture_launch(const char *path)
{
    bool ok = false;

    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block holds signals */
    s_cap.sig = (kode_ir_signal_t *)khal_alloc_psram(sizeof(void *),
                                                     KODE_IR_FILE_SIGNALS_MAX * sizeof(kode_ir_signal_t));
    if ( NULL == s_cap.sig )
    {
        kode_err_set("out of memory");
    }
    else
    {
        (void)snprintf(s_cap.path, sizeof(s_cap.path), "%s", path);
        atomic_store(&s_cap_count, 0U);
        atomic_store(&s_cap_fail, (uint32_t)KODE_OK);
        atomic_store(&s_cap_stop, false);
        atomic_store(&s_cap_closing, false);
        atomic_store(&s_cap_running, true);
        kode_ir_capturing_set(true);
        if ( khal_task_start(&capture_task, KODE_IR_CAPTURE_TASK_NAME, KODE_IR_CAPTURE_STACK_BYTES,
                            KODE_PRIO_STANDARD_READER, NULL) == KODE_OK )
        {
            ok = true;
        }
        else
        {
            kode_ir_capturing_set(false);
            atomic_store(&s_cap_running, false);
            khal_free(s_cap.sig);
            s_cap.sig = NULL;
            kode_err_set("no memory for the capture's task");
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ir_capture_start(const char *path)
{
    bool started = false;

    if ( kode_ir_enter("kode_ir_capture_start") )
    {
        char vol[KODE_PATH_MAX];

        if ( !kode_file_path(path, vol, sizeof(vol)) )
        {
            /* The error is set. */
        }
        else if ( kode_ir_capturing() )
        {
            kode_err_set("an IR capture is running");
        }
        else
        {
            started = capture_launch(path);
        }
        kode_ir_leave();
    }
    return started;
}

/**
 * @brief Write the capture's file and let the capture go; the task has ended.
 * @return true when written; false with the error set (a capture that could not be written is kept).
 */
static bool capture_finish(void)
{
    const uint32_t count = (uint32_t)atomic_load(&s_cap_count);
    bool           ok    = false;
    bool           keep  = false;

    if ( 0U == count )
    {
        const kode_status_t fail = (kode_status_t)atomic_load(&s_cap_fail);

        if ( KODE_OK != fail )
        {
            kode_ir_fail(fail, "receiver");
        }
        else
        {
            kode_err_set("no infrared signal arrived");
        }
    }
    else
    {
        const uint32_t size = kode_ir_file_size_max();
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block holds text */
        char *const    text = (char *)khal_alloc_psram(sizeof(void *), size);

        keep = true;
        if ( NULL == text )
        {
            kode_err_set("out of memory");
        }
        else
        {
            const uint32_t len = kode_ir_file_format(s_cap.sig, count, text, size);

            if ( 0U == len )
            {
                kode_err_set("the capture could not be written as text");
            }
            else
            {
                /* cppcheck-suppress misra-c2012-11.3 ; the text is bytes to the file */
                ok = kode_file_put_bytes(s_cap.path, (const uint8_t *)text, len, false);
                keep = !ok;
            }
            khal_free(text);
        }
    }
    if ( !keep )
    {
        khal_free(s_cap.sig);
        s_cap.sig = NULL;
        kode_ir_capturing_set(false);
    }
    atomic_store(&s_cap_closing, false);
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ir_capture_stop(void)
{
    bool ok = false;

    if ( kode_ir_begin("kode_ir_capture_stop") )
    {
        if ( !kode_ir_capturing() )
        {
            kode_err_set("no IR capture is running");
        }
        else if ( atomic_exchange(&s_cap_closing, true) )
        {
            kode_err_set("the IR capture is already being stopped");
        }
        else if ( !kode_ir_capturing() )
        {
            /* Another stop finished between the first look and the exchange. */
            atomic_store(&s_cap_closing, false);
            kode_err_set("no IR capture is running");
        }
        else
        {
            /* The path is not taken to wait: the task takes it for every window. */
            atomic_store(&s_cap_stop, true);
            if ( !kode_retry(&capture_ended, NULL, KODE_IR_CAPTURE_STOP_MS) )
            {
                atomic_store(&s_cap_closing, false);
                kode_err_set("the IR capture is still ending; call kode_ir_capture_stop() again");
            }
            else if ( kode_ir_lock_take("kode_ir_capture_stop") )
            {
                ok = capture_finish();
                kode_ir_leave();
            }
            else
            {
                atomic_store(&s_cap_closing, false);
            }
        }
    }
    return ok;
}

/**
 * @brief Send one signal of a file.
 * @param s The signal, valid.
 * @return true when it went out; false with the error set.
 */
static bool signal_send(const kode_ir_signal_t *s)
{
    bool ok = false;

    if ( s->raw )
    {
        ok = kode_ir_raw_send(s->us, s->count, s->carrier_hz);
    }
    else
    {
        ok = kode_ir_nec_send(s->address, s->command);
    }
    return ok;
}

/**
 * @brief Send every signal of a file, with the path taken.
 * @param path The file's path.
 * @return true when all went out; false with the error set.
 */
static bool file_send(const char *path)
{
    uint32_t       len   = 0U;
    const uint8_t *bytes = kode_file_read_bytes(path, &len);
    bool           ok    = false;

    if ( NULL != bytes )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block holds signals */
        kode_ir_signal_t *const sig = (kode_ir_signal_t *)khal_alloc_psram(
            sizeof(void *), KODE_IR_FILE_SIGNALS_MAX * sizeof(kode_ir_signal_t));

        if ( NULL == sig )
        {
            kode_err_set("out of memory");
        }
        else
        {
            uint32_t    n    = 0U;
            uint32_t    line = 0U;
            const char *why  = NULL;

            /* cppcheck-suppress misra-c2012-11.3 ; the file's bytes are its text */
            if ( !kode_ir_file_parse((const char *)bytes, len, sig, KODE_IR_FILE_SIGNALS_MAX, &n, &line, &why) )
            {
                kode_err_set("%s: line %" PRIu32 ": %s", path, line, why);
            }
            else if ( 0U == n )
            {
                kode_err_set("%s: no signal in the file", path);
            }
            else
            {
                ok = true;
                for ( uint32_t i = 0U; ok && (i < n); i++ )
                {
                    if ( 0U < i )
                    {
                        kode_block_delay_ms(KODE_IR_FILE_GAP_MS);
                    }
                    ok = signal_send(&sig[i]);
                }
            }
            khal_free(sig);
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ir_send_file(const char *path)
{
    bool ok = false;

    if ( kode_ir_enter("kode_ir_send_file") )
    {
        ok = file_send(path);
        kode_ir_leave();
    }
    return ok;
}
