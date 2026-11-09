/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_ir.c
 * @brief Infrared through AO_Ir: the NEC command, the requests, and the blocking
 *        calls behind one claim per kind.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kdrv_ir.h"

#include <stddef.h>
#include <string.h>

_Static_assert(KODE_SYS_IR_RAW_MAX_SYMBOLS == KDRV_IR_TX_MAX_SYMBOLS,
               "a raw frame the facade takes must be one the driver sends");
_Static_assert(KODE_SYS_IR_RX_MAX_SYMBOLS == KDRV_IR_RX_MAX_SYMBOLS,
               "a capture the facade takes must be one the driver hears");

/* ---- The answers' payloads against the drivers' types they carry ---- */
_Static_assert(sizeof(kode_sys_ir_symbol_t) == sizeof(kdrv_ir_symbol_t), "the symbol is the driver's");
_Static_assert(offsetof(kode_sys_ir_symbol_t, space_us) == offsetof(kdrv_ir_symbol_t, space_us),
               "the symbol's space where the driver's is");
_Static_assert(sizeof(kode_sys_ir_frame_t) == sizeof(kdrv_ir_frame_t), "the frame is the driver's");
_Static_assert(offsetof(kode_sys_ir_frame_t, nec) == offsetof(kdrv_ir_frame_t, nec),
               "the frame's NEC flag where the driver's is");

/* ---- Infrared state: created by kode_sys_ir_init(), before AO_Ir starts ---- */
static khal_sem_t   s_ir_capture_sem;   /**< kode_block_ir_capture_symbols() sleeps here.  */
static khal_sem_t   s_ir_last_sem;      /**< kode_block_ir_last() sleeps here.             */
static khal_mutex_t s_ir_lock;          /**< Guards the claim flags and the generation.  */
static bool         s_ir_capture_busy;  /**< A capture is in its own wait.               */
static bool         s_ir_last_busy;     /**< A last-frame read is waiting.               */
static khal_sem_t   s_ir_raw_sem;       /**< kode_block_ir_send_raw() sleeps here.         */
static uint32_t     s_ir_gen;           /**< Tags requests apart; never 0.               */

/* AO_Ir writes through this file's storage, never a caller's frame; a claim flag lends it one call at a time. */
static IrSymbol         s_ir_capture_buf[KDRV_IR_RX_MAX_SYMBOLS];   /**< Where a capture lands.      */
static size_t           s_ir_capture_n;   /**< Symbols in s_ir_capture_buf.  */
static uint32_t         s_ir_capture_ack; /**< Whose capture answer that is. */
static kdrv_ir_frame_t  s_ir_last_frame;  /**< The frame AO_Ir last heard.   */
static bool             s_ir_last_have;       /**< Whether AO_Ir heard a frame at all. */
static uint32_t         s_ir_last_ack;        /**< Whose last-frame answer that is.   */

/*
 * ---- Infrared ----
 * The blocking calls lend AO_Ir pointers into this file's storage and sleep on
 * one semaphore each, with a claim flag taken under a mutex (two cores). Every
 * request carries a generation that the answering AO stamps back, so an answer
 * a caller gave up on is discarded by kode_sys_answer_await().
 */

/** @brief How much longer than AO_Ir's deadline a capture waits: one QP tick, for its rounding. */
#define KODE_IR_CAPTURE_MARGIN_MS  (KODE_SYS_MS_PER_S / KQP_TICKS_PER_SEC)

/** @brief How long kode_block_ir_last() waits for an answer given inside one AO_Ir step: a liveness bound. */
#define KODE_IR_LAST_WAIT_MS  100U

void kode_sys_ir_init(void)
{
    s_ir_capture_sem  = khal_sem_create();
    s_ir_last_sem     = khal_sem_create();
    s_ir_raw_sem      = khal_sem_create();
    s_ir_lock         = khal_mutex_create();
    s_ir_capture_busy = false;
    s_ir_last_busy    = false;
    s_ir_gen          = 0U;
}

/**
 * @brief Claim @p busy for this caller, atomically against the other core, and draw its generation.
 * @param busy The flag to claim.
 * @param[out] gen This request's generation when the claim was taken; never zero.
 * @return true when this caller took it, and must release it again.
 */
static bool ir_claim(bool *busy, uint32_t *gen)
{
    bool took = false;

    khal_mutex_lock(s_ir_lock);
    if ( !*busy )
    {
        *busy     = true;
        took      = true;
        s_ir_gen += 1U;
        if ( 0U == s_ir_gen )
        {
            s_ir_gen = 1U;
        }
        *gen = s_ir_gen;
    }
    khal_mutex_unlock(s_ir_lock);
    return took;
}

/**
 * @brief Give back a flag taken by ir_claim().
 * @param busy The flag to release.
 */
static void ir_release(bool *busy)
{
    khal_mutex_lock(s_ir_lock);
    *busy = false;
    khal_mutex_unlock(s_ir_lock);
}

kode_status_t kode_sys_ir_send_nec(uint16_t addr, uint16_t cmd)
{
    return kode_sys_cmd_post(AO_Ir, ir_send_evt(addr, cmd), KQP_IR_CMD_MARGIN);
}

/** @brief How long a raw send may wait in AO_Ir before it starts; later it is KODE_TIMEOUT, never sent. */
#define KODE_SYS_IR_RAW_START_MS  (KDRV_IR_CALL_MAX_MS + KODE_IR_JOB_MARGIN_MS)
/** @brief How long kode_block_ir_send_raw() waits for AO_Ir: the start budget, this call's own time, and a tick. */
#define KODE_SYS_IR_RAW_WAIT_MS \
    ((KODE_SYS_IR_RAW_START_MS + KDRV_IR_CALL_MAX_MS + KODE_IR_JOB_MARGIN_MS) \
     + KODE_IR_CAPTURE_MARGIN_MS)

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_block_ir_send_raw(const kode_sys_ir_symbol_t *sym, size_t n,
                                     uint32_t carrier_hz)
{
    static bool     s_ir_raw_busy;
    static uint32_t s_ir_raw_rc;    /* the status AO_Ir answers with */
    static uint32_t s_ir_raw_ack;   /* whose raw answer that is      */
    static uint32_t s_ir_raw_owed;  /* a timed-out call's generation; 0 = none */
    uint32_t      gen = 0U;
    kode_status_t rc  = KODE_TIMEOUT;

    if ( (NULL == sym) || (0U == n) || (KODE_SYS_IR_RAW_MAX_SYMBOLS < n)
        || (0U == carrier_hz) )
    {
        return KODE_DENIED;
    }
    if ( !ir_claim(&s_ir_raw_busy, &gen) )
    {
        return KODE_DENIED;
    }
    /* A timed-out send is still unanswered: its answer is yet to write s_ir_raw_rc. */
    if ( (0U != s_ir_raw_owed) && !kode_sys_answer_is(&s_ir_raw_ack, s_ir_raw_owed) )
    {
        ir_release(&s_ir_raw_busy);
        return KODE_DENIED;
    }
    s_ir_raw_owed = 0U;

    /* AO_Ir copies the frame as the call starts, inside its start budget, which
     * this wait outlasts: once it returns, nothing reads @p sym again. */
    s_ir_raw_rc = (uint32_t)KODE_TIMEOUT;
    ir_send_raw_request(sym, (uint16_t)n, carrier_hz,
                        KODE_SYS_IR_RAW_START_MS, &s_ir_raw_rc, s_ir_raw_sem, gen,
                        &s_ir_raw_ack);
    if ( kode_sys_answer_await(s_ir_raw_sem, &s_ir_raw_ack, gen, KODE_SYS_IR_RAW_WAIT_MS) )
    {
        rc = (kode_status_t)s_ir_raw_rc;
    }
    else
    {
        s_ir_raw_owed = gen;
    }
    ir_release(&s_ir_raw_busy);
    return rc;
}

kode_status_t kode_block_ir_capture_symbols(kdrv_ir_symbol_t *out, size_t cap, size_t *n,
                              uint32_t ms)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( (NULL == out) || (0U == cap) )
    {
        return KODE_ERROR;
    }
    /* Before anything is posted, so a refused caller leaves no request behind. */
    if ( !ir_claim(&s_ir_capture_busy, &gen) )
    {
        return KODE_DENIED;
    }

    /* The driver truncates a longer capture at the same ceiling. */
    size_t want = cap;
    if ( KDRV_IR_RX_MAX_SYMBOLS < want )
    {
        want = KDRV_IR_RX_MAX_SYMBOLS;
    }
    s_ir_capture_n = 0U;
    ir_capture_request(s_ir_capture_buf, want, &s_ir_capture_n,
                       s_ir_capture_sem, ms, gen, &s_ir_capture_ack);

    /* AO_Ir clamps its deadline to KODE_SYS_IR_TIMER_MS_MAX; a larger wait would wrap in the tick arithmetic. */
    uint32_t wait_ms = ms;
    if ( KODE_SYS_IR_TIMER_MS_MAX < wait_ms )
    {
        wait_ms = KODE_SYS_IR_TIMER_MS_MAX;
    }
    wait_ms += KODE_IR_CAPTURE_MARGIN_MS;

    if ( kode_sys_answer_await(s_ir_capture_sem, &s_ir_capture_ack, gen, wait_ms) )
    {
        /* The count came from another AO: clamp it to what was asked for. */
        size_t got = s_ir_capture_n;
        if ( got > want )
        {
            got = want;
        }
        /* Field by field: the caller's kdrv_ir_symbol_t is not IrSymbol. */
        for ( size_t i = 0U; i < got; ++i )
        {
            out[i].mark_us  = s_ir_capture_buf[i].mark_us;
            out[i].space_us = s_ir_capture_buf[i].space_us;
        }
        if ( NULL != n )
        {
            *n = got;
        }
        rc = KODE_OK;
    }
    ir_release(&s_ir_capture_busy);
    return rc;
}

kode_status_t kode_block_ir_last(kdrv_ir_frame_t *out)
{
    kode_status_t rc  = KODE_TIMEOUT;
    uint32_t      gen = 0U;

    if ( NULL == out )
    {
        return KODE_ERROR;
    }
    if ( !ir_claim(&s_ir_last_busy, &gen) )
    {
        return KODE_DENIED;
    }

    /* AO_Ir fills this file's frame, not the caller's: the wait is bounded and can end first. */
    s_ir_last_have = false;
    ir_last_request((void *)&s_ir_last_frame, &s_ir_last_have, s_ir_last_sem,
                    gen, &s_ir_last_ack);

    if ( kode_sys_answer_await(s_ir_last_sem, &s_ir_last_ack, gen, KODE_IR_LAST_WAIT_MS) )
    {
        rc = KODE_DENIED;
        if ( s_ir_last_have )
        {
            *out = s_ir_last_frame;
            rc   = KODE_OK;
        }
    }
    ir_release(&s_ir_last_busy);
    return rc;
}

/* ---- kode_sys's requests: AO_Ir answers the asking AO ---- */

kode_status_t kode_sys_ir_send_raw(QActive *me, const kode_sys_ir_symbol_t *sym, size_t n,
                                   uint32_t carrier_hz)
{
    if ( (NULL == sym) || (0U == n) || (KODE_SYS_IR_RAW_MAX_SYMBOLS < n) || (0U == carrier_hz) )
    {
        return KODE_DENIED;
    }
    return ir_send_raw_ask(me, sym, (uint16_t)n, carrier_hz, KODE_SYS_IR_RAW_START_MS);
}

kode_status_t kode_sys_ir_capture(QActive *me, kode_sys_ir_symbol_t *out, size_t cap,
                                  uint32_t ms)
{
    if ( (NULL == out) || (0U == cap) || (0U == ms) )
    {
        return KODE_DENIED;
    }
    return ir_capture_ask(me, out, cap, ms);
}

kode_status_t kode_sys_ir_last(QActive *me)
{
    return ir_last_ask(me);
}
