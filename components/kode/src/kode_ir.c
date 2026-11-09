/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_ir.c
 * @brief The standard layer's infrared: sends and reads, by protocol name or raw.
 */
#include "kode.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "knec.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_ir.h"
#include "kode_ir_file.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_tag.h"
#include "kqp.h"

_Static_assert(KODE_IR_RAW_MAX == (2U * KDRV_IR_RX_MAX_SYMBOLS),
               "the raw result and the receiver's longest capture have drifted apart");
_Static_assert(KODE_IR_RAW_MAX == (2U * KODE_SYS_IR_RAW_MAX_SYMBOLS),
               "the raw frame and the transmitter's longest frame have drifted apart");

/** @brief Durations of one symbol: a mark and a space. */
#define KODE_IR_DURATIONS_PER_SYMBOL 2U

/** @brief The protocol name a NEC frame reads as. */
#define KODE_IR_NEC_NAME "NEC"

/** @brief The protocol name of a frame that is none the board decodes. */
#define KODE_IR_UNKNOWN_NAME "unknown"

/** @brief The text of a refused read while a capture runs. */
#define KODE_IR_CAPTURING_TEXT "an IR capture is running; kode_ir_capture_stop() ends it"

/** @brief The lock every public infrared call holds; made on the first call. */
static khal_mutex_t s_ir_lock;

/** @brief A capture exists, from its start to the end of the stop that wrote it; set under the lock. */
static atomic_bool s_ir_capturing;

/** @brief Make the infrared lock. @return true when it exists. */
static bool ir_lock_make(void)
{
    s_ir_lock = khal_mutex_create();
    return NULL != s_ir_lock;
}

bool kode_ir_lock_take(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = khal_once_try(&lock_once, &ir_lock_make);

    if ( ok )
    {
        khal_mutex_lock(s_ir_lock);
    }
    else
    {
        kode_err_set("%s: no memory for the infrared lock", name);
    }
    return ok;
}

bool kode_ir_begin(const char *name)
{
    bool ok = kode_enter(name);

#if !CONFIG_KODEOS_RADIO
    if ( ok )
    {
        kode_err_set("this firmware has no infrared");
        ok = false;
    }
#endif
    return ok;
}

bool kode_ir_enter(const char *name)
{
    bool ok = kode_ir_begin(name);

    if ( ok )
    {
        ok = kode_ir_lock_take(name);
    }
    return ok;
}

/**
 * @brief The prologue of a read: as kode_ir_enter(), but refused while a capture runs.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool ir_read_enter(const char *name)
{
    bool ok = kode_ir_begin(name);

    if ( ok && atomic_load(&s_ir_capturing) )
    {
        kode_err_set("%s", KODE_IR_CAPTURING_TEXT);
        ok = false;
    }
    if ( ok )
    {
        ok = kode_ir_lock_take(name);
        /* A capture that began while this read waited for its turn. */
        if ( ok && atomic_load(&s_ir_capturing) )
        {
            khal_mutex_unlock(s_ir_lock);
            kode_err_set("%s", KODE_IR_CAPTURING_TEXT);
            ok = false;
        }
    }
    return ok;
}

void kode_ir_leave(void)
{
    khal_mutex_unlock(s_ir_lock);
}

bool kode_ir_capturing(void)
{
    return atomic_load(&s_ir_capturing);
}

void kode_ir_capturing_set(bool on)
{
    atomic_store(&s_ir_capturing, on);
}

/** @brief What one try of a send carries. */
typedef struct {
    const kode_sys_ir_symbol_t *sym;         /**< The frame.                  */
    size_t                      n;           /**< Its symbols.                */
    uint32_t                    carrier_hz;  /**< Its carrier.                */
    kode_status_t               rc;          /**< The last answer.            */
} send_try_t;

/** @brief One try of a send: true unless another holder has the path. */
static bool send_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the send_try_t frame_send() passed */
    send_try_t *const t = (send_try_t *)ctx;

    t->rc = kode_block_ir_send_raw(t->sym, t->n, t->carrier_hz);
    return KODE_DENIED != t->rc;
}

void kode_ir_fail(kode_status_t rc, const char *part)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("infrared is busy, or the radio is off");
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("out of memory");
        break;
    case KODE_TIMEOUT:
        kode_err_set("the infrared %s did not answer", part);
        break;
    default:
        kode_err_set("the infrared %s could not be used", part);
        break;
    }
}

/**
 * @brief Send a frame, waiting its turn; the error is set on failure.
 * @param sym        The symbols, mark first.
 * @param n          How many, 1..KODE_SYS_IR_RAW_MAX_SYMBOLS.
 * @param carrier_hz The carrier, above 0.
 * @return true when the frame went out.
 */
static bool frame_send(const kode_sys_ir_symbol_t *sym, size_t n, uint32_t carrier_hz)
{
    send_try_t t = { sym, n, carrier_hz, KODE_ERROR };

    /* Every argument was checked before: a DENIED here can only mean busy. */
    (void)kode_retry(&send_attempt, &t, KODE_IR_BUSY_MS);
    if ( KODE_OK != t.rc )
    {
        kode_ir_fail(t.rc, "transmitter");
    }
    return KODE_OK == t.rc;
}

bool kode_ir_nec_send(uint32_t address, uint32_t command)
{
    kdrv_ir_symbol_t enc[KNEC_SYMBOLS];
    size_t           n  = 0U;
    bool             ok = false;

    if ( knec_encode((uint16_t)address, (uint16_t)command, enc, KNEC_SYMBOLS, &n) == KODE_OK )
    {
        kode_sys_ir_symbol_t sym[KNEC_SYMBOLS];

        /* The encoder speaks the driver's symbol, the send the facade's. */
        for ( size_t i = 0U; i < n; i++ )
        {
            sym[i].mark_us  = enc[i].mark_us;
            sym[i].space_us = enc[i].space_us;
        }
        ok = frame_send(sym, n, KDRV_IR_CARRIER_HZ);
    }
    else
    {
        kode_err_set("the frame could not be encoded");
    }
    return ok;
}

bool kode_ir_send(const char *protocol, uint32_t address, uint32_t command)
{
    bool ok = false;

    if ( kode_ir_enter("kode_ir_send") )
    {
        if ( NULL == protocol )
        {
            kode_err_set("no protocol given");
        }
        else if ( !kode_name_is(protocol, "nec") )
        {
            kode_err_set("protocol '%s' not supported", protocol);
        }
        else if ( KODE_IR_FILE_VALUE_MAX < address )
        {
            kode_err_set("address %" PRIu32 " is out of range (0 to %" PRIu32 ")",
                         address, (uint32_t)KODE_IR_FILE_VALUE_MAX);
        }
        else if ( KODE_IR_FILE_VALUE_MAX < command )
        {
            kode_err_set("command %" PRIu32 " is out of range (0 to %" PRIu32 ")",
                         command, (uint32_t)KODE_IR_FILE_VALUE_MAX);
        }
        else
        {
            ok = kode_ir_nec_send(address, command);
        }
        kode_ir_leave();
    }
    return ok;
}

/**
 * @brief Pair durations as marks and spaces; an odd last mark gets a space of 0.
 * @param us  The durations.
 * @param n   How many, 1..KODE_IR_RAW_MAX.
 * @param sym Out: (n + 1) / 2 symbols.
 * @return How many symbols.
 */
static size_t symbols_from_durations(const uint16_t *us, uint32_t n, kode_sys_ir_symbol_t *sym)
{
    const size_t count = ((size_t)n + 1U) / KODE_IR_DURATIONS_PER_SYMBOL;

    for ( size_t i = 0U; i < count; i++ )
    {
        const size_t at = i * KODE_IR_DURATIONS_PER_SYMBOL;

        sym[i].mark_us  = us[at];
        sym[i].space_us = 0U;
        if ( (at + 1U) < (size_t)n )
        {
            sym[i].space_us = us[at + 1U];
        }
    }
    return count;
}

bool kode_ir_raw_send(const uint16_t *us, uint32_t n, uint32_t carrier_hz)
{
    kode_sys_ir_symbol_t sym[KODE_SYS_IR_RAW_MAX_SYMBOLS];
    const size_t         count = symbols_from_durations(us, n, sym);

    return frame_send(sym, count, carrier_hz);
}

bool kode_ir_send_raw(const uint16_t *us, uint32_t n, uint32_t carrier_hz)
{
    bool ok = false;

    if ( kode_ir_enter("kode_ir_send_raw") )
    {
        if ( NULL == us )
        {
            kode_err_set("no durations given");
        }
        else if ( (0U == n) || (KODE_IR_RAW_MAX < n) )
        {
            kode_err_set("a raw frame holds 1 to %" PRIu32 " durations, not %" PRIu32,
                         (uint32_t)KODE_IR_RAW_MAX, n);
        }
        else if ( 0U == carrier_hz )
        {
            kode_err_set("the carrier must be above 0 Hz");
        }
        else
        {
            ok = kode_ir_raw_send(us, n, carrier_hz);
        }
        kode_ir_leave();
    }
    return ok;
}

/** @brief What one try of a listen carries. */
typedef struct {
    kdrv_ir_symbol_t *sym;     /**< Where the frame goes.                   */
    size_t            got;     /**< Symbols heard.                          */
    uint32_t          start;   /**< When the read began, in ms.             */
    uint32_t          ms;      /**< The whole time, or KODE_FOREVER.        */
    uint32_t          began;   /**< When this try began, in ms.             */
    uint32_t          window;  /**< How long this try listened, in ms.      */
    kode_status_t     rc;      /**< The last answer.                        */
} listen_try_t;

/** @brief One try of a listen window, sized by what is left of the read: true unless another holder has the receiver. */
static bool listen_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the listen_try_t kode_ir_listen() passed */
    listen_try_t *const t = (listen_try_t *)ctx;

    t->began  = kode_sys_time_ms();
    t->window = kode_tag_window(t->start, t->ms, KODE_SYS_IR_TIMER_MS_MAX);
    t->got    = 0U;
    /* No time left is a quiet window, or busy when a holder was the last answer: never a window of 0 ms. */
    if ( 0U == t->window )
    {
        if ( KODE_DENIED != t->rc )
        {
            t->rc = KODE_OK;
        }
    }
    else
    {
        t->rc = kode_block_ir_capture_symbols(t->sym, KDRV_IR_RX_MAX_SYMBOLS, &t->got, t->window);
    }
    return KODE_DENIED != t->rc;
}

kode_ir_heard_t kode_ir_listen(kdrv_ir_symbol_t *sym, uint32_t *n, uint32_t ms, kode_status_t *failure)
{
    listen_try_t    t      = { sym, 0U, kode_sys_time_ms(), ms, 0U, 0U, KODE_ERROR };
    kode_ir_heard_t result = KODE_IR_QUIET;
    bool         going  = true;

    while ( going )
    {
        /* The wait for a holder is part of the time asked for, never past it. */
        const uint32_t busy_ms = kode_tag_window(t.start, ms, KODE_IR_BUSY_MS);

        (void)kode_retry(&listen_attempt, &t, busy_ms);
        if ( KODE_OK != t.rc )
        {
            kode_ir_fail(t.rc, "receiver");
            if ( NULL != failure )
            {
                *failure = t.rc;
            }
            result   = KODE_IR_FAILED;
            going    = false;
        }
        else if ( 0U < t.got )
        {
            *n     = (uint32_t)t.got;
            result = KODE_IR_HEARD;
            going  = false;
        }
        else if ( (KODE_FOREVER != ms) && ((kode_sys_time_ms() - t.start) >= ms) )
        {
            kode_err_set("no infrared signal in %" PRIu32 " ms", ms);
            going = false;
        }
        else if ( (kode_sys_time_ms() - t.began) < t.window )
        {
            /* AO_Ir answered empty before its time (it was busy): do not spin on it. */
            kode_block_delay_ms(KODE_RETRY_STEP_MS);
        }
        else
        {
            /* The window ran its length and heard nothing: ask for what is left. */
        }
    }
    return result;
}

kode_ir_code_t kode_ir_receive(uint32_t ms)
{
    kode_ir_code_t r = { false, "", 0U, 0U };

    if ( ir_read_enter("kode_ir_receive") )
    {
        kdrv_ir_symbol_t sym[KDRV_IR_RX_MAX_SYMBOLS];
        uint32_t         n = 0U;

        if ( kode_ir_listen(sym, &n, ms, NULL) == KODE_IR_HEARD )
        {
            uint16_t addr = 0U;
            uint16_t cmd  = 0U;

            r.ok = true;
            if ( knec_decode(sym, n, &addr, &cmd) )
            {
                (void)snprintf(r.protocol, sizeof(r.protocol), "%s", KODE_IR_NEC_NAME);
                r.address = addr;
                r.command = cmd;
            }
            else
            {
                (void)snprintf(r.protocol, sizeof(r.protocol), "%s", KODE_IR_UNKNOWN_NAME);
            }
        }
        kode_ir_leave();
    }
    return r;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_ir_received(void)
{
    return KODE_EVENT_IR_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_ir_code_t kode_ir_last(void)
{
    kode_ir_code_t r = { false, "", 0U, 0U };

    if ( kode_ir_enter("kode_ir_last") )
    {
        kdrv_ir_frame_t     f  = { 0U, 0U, 0U, false };
        const kode_status_t rc = kode_block_ir_last(&f);

        if ( KODE_OK == rc )
        {
            r.ok = true;
            if ( f.nec )
            {
                (void)snprintf(r.protocol, sizeof(r.protocol), "%s", KODE_IR_NEC_NAME);
                r.address = f.addr;
                r.command = f.cmd;
            }
            else
            {
                (void)snprintf(r.protocol, sizeof(r.protocol), "%s", KODE_IR_UNKNOWN_NAME);
            }
        }
        else if ( KODE_DENIED == rc )
        {
            kode_err_set("no infrared signal heard yet");
        }
        else
        {
            kode_ir_fail(rc, "receiver");
        }
        kode_ir_leave();
    }
    return r;
}

const kode_ir_raw_t *kode_ir_receive_raw(uint32_t ms)
{
    /* What a read that cannot keep a result answers: no durations. */
    static const kode_ir_raw_t none = { 0U };
    const kode_ir_raw_t       *result = &none;

    if ( ir_read_enter("kode_ir_receive_raw") )
    {
        struct kode_tag_state *const state = kode_tag_state_ensure();

        if ( NULL == state )
        {
            kode_err_set("out of memory");
        }
        else
        {
            kdrv_ir_symbol_t sym[KDRV_IR_RX_MAX_SYMBOLS];
            uint32_t         n = 0U;

            state->ir_raw.count      = 0U;
            state->ir_raw.carrier_hz = 0U;
            result = &state->ir_raw;
            if ( kode_ir_listen(sym, &n, ms, NULL) == KODE_IR_HEARD )
            {
                for ( uint32_t i = 0U; i < n; i++ )
                {
                    state->ir_raw.us[i * KODE_IR_DURATIONS_PER_SYMBOL] = sym[i].mark_us;
                    state->ir_raw.us[(i * KODE_IR_DURATIONS_PER_SYMBOL) + 1U] = sym[i].space_us;
                }
                state->ir_raw.count      = n * KODE_IR_DURATIONS_PER_SYMBOL;
                state->ir_raw.carrier_hz = KDRV_IR_CARRIER_HZ;
            }
        }
        kode_ir_leave();
    }
    return result;
}
