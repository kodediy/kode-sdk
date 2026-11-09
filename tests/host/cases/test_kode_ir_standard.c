/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_ir_standard.c
 * @brief The standard layer's infrared: send and read by protocol or raw, and the last frame.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kdrv_ir.h"
#include "kdrv_storage.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "knec.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_ir.h"
#include "kode_ir_file.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"
#include "kode_tag.h"
#include "kode_task.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief The frame the read cases have AO_Ir hear. */
#define HEARD_ADDR 0x0012U
#define HEARD_CMD  0x0034U

/** @brief A carrier other than the receiver's. */
#define TEST_CARRIER_HZ 56000U

/** @brief Symbols of the longest raw frame. */
#define TEST_RAW_SYMBOLS KODE_SYS_IR_RAW_MAX_SYMBOLS

/** @brief Windows a read script can hold. */
#define TEST_WINDOWS_MAX 40U

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of a test task: above the case's own, so it runs the moment it is let go. */
#define TEST_TASK_PRIO 10U

/** @brief The longest a case waits for a task, in ms. */
#define TEST_WAIT_MS 4000U

/* Stand-ins only record: an assertion there would skip the extended layer's claim release. */
/** @brief What a stand-in running inside a wait could not do, if anything. */
static const char *s_hook_fault;

/** @brief Fail the case with whatever a stand-in could not do, if anything. */
static void ir_hook_ok(void)
{
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
}

/** @brief Give AO_Ir somewhere to receive, forget whatever it held, and make the locks real. */
static void ir_fresh(void)
{
    /* The IR lock is made by the first call of the binary: make it a real one. */
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    qf_support_init();
    qf_support_give_queue(AO_Ir);
    fake_sem_reset();
    fake_time_reset();
    s_hook_fault = NULL;

    for ( ;; )
    {
        const QEvt *e = NULL;

        if ( xQueueReceive(AO_Ir->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
}

/** @brief How many requests are waiting for AO_Ir. */
static uint32_t ir_pending(void)
{
    return (uint32_t)uxQueueMessagesWaiting(AO_Ir->eQueue);
}

/**
 * @brief Take the head of AO_Ir's queue from inside a wait hook.
 * @param sig The signal the head must carry.
 * @return The event, to QF_gc(); NULL, with s_hook_fault set, when there is none.
 */
static const QEvt *ir_hook_head(enum_t sig)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Ir->eQueue, &e, 0U) != pdTRUE )
    {
        s_hook_fault = "the stand-in found nothing in AO_Ir's queue";
        return NULL;
    }
    if ( (enum_t)e->sig != sig )
    {
        s_hook_fault = "the stand-in found the wrong request at the head";
        QF_gc(e);
        return NULL;
    }
    return e;
}

/* ---- The raw send's stand-in ------------------------------------------------ */

static kode_sys_ir_symbol_t s_raw_seen[TEST_RAW_SYMBOLS];  /**< The frame AO_Ir was lent.   */
static uint32_t         s_raw_seen_n;                  /**< Its symbols.                */
static uint32_t         s_raw_seen_hz;                 /**< Its carrier.                */
static uint32_t         s_raw_calls;                   /**< Requests answered.          */
static kode_status_t    s_raw_rc;                      /**< What the stand-in answers.  */

/** @brief Sends the stand-in keeps a record of. */
#define TEST_SENDS_MAX 4U

static uint32_t s_sent_n[TEST_SENDS_MAX];     /**< Symbols of each send.                 */
static uint32_t s_sent_hz[TEST_SENDS_MAX];    /**< Carrier of each send.                 */
static uint16_t s_sent_mark[TEST_SENDS_MAX];  /**< First mark of each send.              */
static uint32_t s_sent_at[TEST_SENDS_MAX];    /**< Fake clock when each was answered, ms. */

/** @brief Forget what the raw stand-in saw; it answers OK again. */
static void raw_reset(void)
{
    (void)memset(s_raw_seen, 0, sizeof(s_raw_seen));
    s_raw_seen_n  = 0U;
    s_raw_seen_hz = 0U;
    s_raw_calls   = 0U;
    s_raw_rc      = KODE_OK;
    (void)memset(s_sent_n, 0, sizeof(s_sent_n));
    (void)memset(s_sent_hz, 0, sizeof(s_sent_hz));
    (void)memset(s_sent_mark, 0, sizeof(s_sent_mark));
    (void)memset(s_sent_at, 0, sizeof(s_sent_at));
}

/** @brief AO_Ir answering the raw send at the head of its queue. */
static void ir_answers_head_raw(void)
{
    const QEvt * const e = ir_hook_head((enum_t)IR_SEND_RAW_REQ_SIG);

    if ( NULL == e )
    {
        return;
    }
    const IrSendRawEvt * const req = (const IrSendRawEvt *)e;

    if ( (NULL != req->sym) && (TEST_RAW_SYMBOLS >= req->n) )
    {
        (void)memcpy(s_raw_seen, req->sym, (size_t)req->n * sizeof(kode_sys_ir_symbol_t));
        s_raw_seen_n = req->n;
    }
    else
    {
        s_hook_fault = "the raw request carried no frame";
    }
    s_raw_seen_hz = req->carrier_hz;
    if ( TEST_SENDS_MAX > s_raw_calls )
    {
        s_sent_n[s_raw_calls]    = s_raw_seen_n;
        s_sent_hz[s_raw_calls]   = req->carrier_hz;
        s_sent_mark[s_raw_calls] = s_raw_seen[0].mark_us;
        s_sent_at[s_raw_calls]   = kode_sys_time_ms();
    }
    s_raw_calls++;
    if ( NULL != req->rc )
    {
        *req->rc = (uint32_t)s_raw_rc;
    }
    if ( NULL != req->ack )
    {
        *req->ack = req->gen;
    }
    khal_sem_signal(req->done);
    QF_gc(e);
}

/* ---- The capture's stand-in ------------------------------------------------- */

/** @brief One window of a script: what AO_Ir answers and how long it took. */
typedef struct {
    uint32_t n;           /**< Symbols to answer with; 0 is a window that heard nothing. */
    uint32_t advance_ms;  /**< Time the window takes on the fake clock.                  */
    const kdrv_ir_symbol_t *sym;  /**< What it hears; NULL: s_script_sym.                */
} window_t;

static kdrv_ir_symbol_t s_script_sym[KDRV_IR_RX_MAX_SYMBOLS];  /**< What a window that hears, hears. */
static window_t         s_script[TEST_WINDOWS_MAX];            /**< The windows, in order.           */
static uint32_t         s_script_count;                        /**< How many there are.              */
static uint32_t         s_script_at;                           /**< The next one to answer.          */
static uint32_t         s_asked_ms[TEST_WINDOWS_MAX];          /**< The deadline each request carried. */

/** @brief Forget the script. */
static void script_reset(void)
{
    (void)memset(s_script_sym, 0, sizeof(s_script_sym));
    (void)memset(s_script, 0, sizeof(s_script));
    (void)memset(s_asked_ms, 0, sizeof(s_asked_ms));
    s_script_count = 0U;
    s_script_at    = 0U;
}

/**
 * @brief AO_Ir answering the capture at the head of its queue with a window.
 * @param w What the window hears and how long it takes.
 */
static void answer_window(const window_t *w)
{
    const QEvt * const e = ir_hook_head((enum_t)IR_CAPTURE_REQ_SIG);

    if ( NULL == e )
    {
        return;
    }
    const IrCaptureEvt * const req = (const IrCaptureEvt *)e;
    kdrv_ir_symbol_t * const   out = (kdrv_ir_symbol_t *)req->out;

    if ( NULL == out )
    {
        s_hook_fault = "a capture request with nowhere to write";
        QF_gc(e);
        return;
    }
    s_asked_ms[s_script_at % TEST_WINDOWS_MAX] = req->ms;
    fake_time_advance_ms(w->advance_ms);
    const kdrv_ir_symbol_t *heard = s_script_sym;

    if ( NULL != w->sym )
    {
        heard = w->sym;
    }
    for ( uint32_t i = 0U; (i < w->n) && (i < req->cap); i++ )
    {
        out[i] = heard[i];
    }
    if ( NULL != req->n )
    {
        *req->n = w->n;
    }
    if ( NULL != req->ack )
    {
        *req->ack = req->gen;
    }
    khal_sem_signal(req->done);
    QF_gc(e);
}

/** @brief AO_Ir answering the capture at the head of its queue with the next window of the script. */
static void script_answer_next(void)
{
    if ( s_script_at >= s_script_count )
    {
        s_hook_fault = "a capture request past the script";
        return;
    }
    const window_t w = s_script[s_script_at];

    answer_window(&w);
    s_script_at++;
}

/** @brief The next window of the script, and arming for the one after. */
static void ir_answers_window(void)
{
    script_answer_next();
    if ( s_script_at < s_script_count )
    {
        fake_sem_wait_hook(&ir_answers_window);
    }
}

/**
 * @brief Script one more window and arm the stand-in for the first.
 * @param n          Symbols the window hears (from s_script_sym).
 * @param advance_ms Time it takes.
 */
static void script_window(uint32_t n, uint32_t advance_ms)
{
    s_script[s_script_count].n          = n;
    s_script[s_script_count].advance_ms = advance_ms;
    s_script_count++;
    fake_sem_wait_hook(&ir_answers_window);
}

/** @brief Put the NEC frame of the heard address and command in the script's symbols. */
static uint32_t script_nec(void)
{
    size_t n = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, knec_encode(HEARD_ADDR, HEARD_CMD, s_script_sym,
                                               KDRV_IR_RX_MAX_SYMBOLS, &n));
    return (uint32_t)n;
}

/* ---- kode_ir_send ----------------------------------------------------------- */

TEST_CASE("a NEC send puts the encoded frame on the raw send at 38 kHz", "[kode_ir_standard]")
{
    kdrv_ir_symbol_t want[KNEC_SYMBOLS];
    size_t           n = 0U;

    ir_fresh();
    raw_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, knec_encode(0x04U, 0x08U, want, KNEC_SYMBOLS, &n));
    fake_sem_wait_hook(&ir_answers_head_raw);

    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send("nec", 0x04U, 0x08U), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(KNEC_SYMBOLS, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT32(KDRV_IR_CARRIER_HZ, s_raw_seen_hz);
    TEST_ASSERT_EQUAL_MEMORY(want, s_raw_seen, KNEC_SYMBOLS * sizeof(kdrv_ir_symbol_t));
}

TEST_CASE("the protocol name is read in any case", "[kode_ir_standard]")
{
    ir_fresh();
    raw_reset();
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send("NEC", 1U, 2U), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(1U, s_raw_calls);
}

TEST_CASE("the largest address and command are sent", "[kode_ir_standard]")
{
    ir_fresh();
    raw_reset();
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send("nec", 65535U, 65535U), kode_error());
    ir_hook_ok();
}

TEST_CASE("a protocol the board does not encode is refused and nothing is sent", "[kode_ir_standard]")
{
    ir_fresh();
    raw_reset();
    TEST_ASSERT_FALSE(kode_ir_send("sony", 1U, 2U));
    TEST_ASSERT_EQUAL_STRING("protocol 'sony' not supported", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());
}

TEST_CASE("no protocol is refused by name", "[kode_ir_standard]")
{
    ir_fresh();
    TEST_ASSERT_FALSE(kode_ir_send(NULL, 1U, 2U));
    TEST_ASSERT_EQUAL_STRING("no protocol given", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());
}

TEST_CASE("an address or command over 65535 is refused and nothing is sent", "[kode_ir_standard]")
{
    ir_fresh();
    TEST_ASSERT_FALSE(kode_ir_send("nec", 70000U, 1U));
    TEST_ASSERT_EQUAL_STRING("address 70000 is out of range (0 to 65535)", kode_error());
    TEST_ASSERT_FALSE(kode_ir_send("nec", 1U, 65536U));
    TEST_ASSERT_EQUAL_STRING("command 65536 is out of range (0 to 65535)", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());
}

TEST_CASE("a transmitter that does not answer is named, and the next send waits then calls it busy",
          "[kode_ir_standard]")
{
    ir_fresh();
    raw_reset();
    /* Nothing answers: the wait runs out and the send stays unanswered. */
    TEST_ASSERT_FALSE(kode_ir_send("nec", 1U, 2U));
    TEST_ASSERT_EQUAL_STRING("the infrared transmitter did not answer", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, ir_pending());

    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_ir_send("nec", 1U, 2U));
    TEST_ASSERT_EQUAL_STRING("infrared is busy, or the radio is off", kode_error());
    TEST_ASSERT_TRUE((kode_sys_time_ms() - before) >= KODE_IR_BUSY_MS);
    TEST_ASSERT_EQUAL_UINT32(1U, ir_pending());

    /* AO_Ir answers the old one at last; the path is free again. */
    ir_answers_head_raw();
    ir_hook_ok();
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send("nec", 1U, 2U), kode_error());
    ir_hook_ok();
}

TEST_CASE("what the transmitter answers is said in words", "[kode_ir_standard]")
{
    ir_fresh();
    raw_reset();
    s_raw_rc = KODE_NO_RESOURCES;
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_FALSE(kode_ir_send("nec", 1U, 2U));
    ir_hook_ok();
    TEST_ASSERT_EQUAL_STRING("out of memory", kode_error());

    s_raw_rc = KODE_IO_ERROR;
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_FALSE(kode_ir_send("nec", 1U, 2U));
    ir_hook_ok();
    TEST_ASSERT_EQUAL_STRING("the infrared transmitter could not be used", kode_error());
}

/* ---- kode_ir_send_raw ------------------------------------------------------- */

TEST_CASE("a raw send of an even count pairs the durations as marks and spaces", "[kode_ir_standard]")
{
    static const uint16_t us[] = { 9000U, 4500U, 560U, 1690U };

    ir_fresh();
    raw_reset();
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send_raw(us, 4U, TEST_CARRIER_HZ), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(2U, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT32(TEST_CARRIER_HZ, s_raw_seen_hz);
    TEST_ASSERT_EQUAL_UINT16(9000U, s_raw_seen[0].mark_us);
    TEST_ASSERT_EQUAL_UINT16(4500U, s_raw_seen[0].space_us);
    TEST_ASSERT_EQUAL_UINT16(560U, s_raw_seen[1].mark_us);
    TEST_ASSERT_EQUAL_UINT16(1690U, s_raw_seen[1].space_us);
}

TEST_CASE("a raw send of an odd count ends on a mark with a space of 0", "[kode_ir_standard]")
{
    static const uint16_t us[] = { 9000U, 4500U, 560U };

    ir_fresh();
    raw_reset();
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send_raw(us, 3U, KDRV_IR_CARRIER_HZ), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(2U, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT16(560U, s_raw_seen[1].mark_us);
    TEST_ASSERT_EQUAL_UINT16(0U, s_raw_seen[1].space_us);
}

TEST_CASE("a raw send of one duration is one mark", "[kode_ir_standard]")
{
    static const uint16_t us[] = { 560U };

    ir_fresh();
    raw_reset();
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send_raw(us, 1U, KDRV_IR_CARRIER_HZ), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(1U, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT16(0U, s_raw_seen[0].space_us);
}

TEST_CASE("a raw send of the most durations is sent whole", "[kode_ir_standard]")
{
    static uint16_t us[KODE_IR_RAW_MAX];

    ir_fresh();
    raw_reset();
    for ( uint32_t i = 0U; KODE_IR_RAW_MAX > i; i++ )
    {
        us[i] = (uint16_t)(i + 1U);
    }
    fake_sem_wait_hook(&ir_answers_head_raw);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send_raw(us, KODE_IR_RAW_MAX, KDRV_IR_CARRIER_HZ), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(TEST_RAW_SYMBOLS, s_raw_seen_n);
    TEST_ASSERT_EQUAL_UINT16(KODE_IR_RAW_MAX - 1U, s_raw_seen[TEST_RAW_SYMBOLS - 1U].mark_us);
    TEST_ASSERT_EQUAL_UINT16(KODE_IR_RAW_MAX, s_raw_seen[TEST_RAW_SYMBOLS - 1U].space_us);
}

TEST_CASE("a raw send with no durations, too many, no array or no carrier is refused", "[kode_ir_standard]")
{
    static uint16_t us[KODE_IR_RAW_MAX + 1U];

    ir_fresh();
    raw_reset();
    TEST_ASSERT_FALSE(kode_ir_send_raw(us, 0U, KDRV_IR_CARRIER_HZ));
    TEST_ASSERT_EQUAL_STRING("a raw frame holds 1 to 512 durations, not 0", kode_error());
    TEST_ASSERT_FALSE(kode_ir_send_raw(us, KODE_IR_RAW_MAX + 1U, KDRV_IR_CARRIER_HZ));
    TEST_ASSERT_EQUAL_STRING("a raw frame holds 1 to 512 durations, not 513", kode_error());
    TEST_ASSERT_FALSE(kode_ir_send_raw(NULL, 4U, KDRV_IR_CARRIER_HZ));
    TEST_ASSERT_EQUAL_STRING("no durations given", kode_error());
    TEST_ASSERT_FALSE(kode_ir_send_raw(us, 4U, 0U));
    TEST_ASSERT_EQUAL_STRING("the carrier must be above 0 Hz", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());
}

/* ---- kode_ir_receive ----------------------------------------------------------- */

TEST_CASE("a heard NEC frame reads as its protocol, address and command", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    script_window(script_nec(), 0U);

    const kode_ir_code_t r = kode_ir_receive(1000U);

    ir_hook_ok();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING("NEC", r.protocol);
    TEST_ASSERT_EQUAL_UINT32(HEARD_ADDR, r.address);
    TEST_ASSERT_EQUAL_UINT32(HEARD_CMD, r.command);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1000U, s_asked_ms[0]);
}

TEST_CASE("a frame that is not NEC reads ok as unknown with address and command 0", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    s_script_sym[0].mark_us  = 2400U;
    s_script_sym[0].space_us = 600U;
    s_script_sym[1].mark_us  = 1200U;
    s_script_sym[1].space_us = 600U;
    s_script_sym[2].mark_us  = 600U;
    s_script_sym[2].space_us = 0U;
    script_window(3U, 0U);

    const kode_ir_code_t r = kode_ir_receive(1000U);

    ir_hook_ok();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING("unknown", r.protocol);
    TEST_ASSERT_EQUAL_UINT32(0U, r.address);
    TEST_ASSERT_EQUAL_UINT32(0U, r.command);
}

TEST_CASE("a read that hears nothing in its time says so", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    script_window(0U, 100U);

    const kode_ir_code_t r = kode_ir_receive(100U);

    ir_hook_ok();
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("", r.protocol);
    TEST_ASSERT_EQUAL_UINT32(0U, r.address);
    TEST_ASSERT_EQUAL_UINT32(0U, r.command);
    TEST_ASSERT_EQUAL_STRING("no infrared signal in 100 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(100U, s_asked_ms[0]);
}

TEST_CASE("a window that came back empty and early waits a step, then asks for what is left",
          "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    script_window(0U, 300U);
    script_window(script_nec(), 0U);

    const kode_ir_code_t r = kode_ir_receive(1000U);

    ir_hook_ok();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, kode_error());
    TEST_ASSERT_EQUAL_UINT32(1000U, s_asked_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(1000U - 300U - KODE_RETRY_STEP_MS, s_asked_ms[1]);
}

TEST_CASE("a long read asks the receiver in windows of the longest it holds", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    script_window(0U, KODE_SYS_IR_TIMER_MS_MAX);
    script_window(script_nec(), 0U);

    const kode_ir_code_t r = kode_ir_receive(KODE_SYS_IR_TIMER_MS_MAX + 1000U);

    ir_hook_ok();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, kode_error());
    TEST_ASSERT_EQUAL_UINT32(KODE_SYS_IR_TIMER_MS_MAX, s_asked_ms[0]);
    TEST_ASSERT_EQUAL_UINT32(1000U, s_asked_ms[1]);
}

TEST_CASE("a read with no limit asks for the longest window and takes the first frame", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    script_window(script_nec(), 0U);

    const kode_ir_code_t r = kode_ir_receive(KODE_FOREVER);

    ir_hook_ok();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, kode_error());
    TEST_ASSERT_EQUAL_UINT32(KODE_SYS_IR_TIMER_MS_MAX, s_asked_ms[0]);
}

TEST_CASE("a receiver that does not answer is named", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    /* No stand-in: the wait runs out. */
    const kode_ir_code_t r = kode_ir_receive(100U);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("the infrared receiver did not answer", kode_error());
    /* The request AO_Ir still holds. */
    TEST_ASSERT_EQUAL_UINT32(1U, ir_pending());
}

TEST_CASE("a read with no time answers at once, hearing nothing", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    script_window(0U, 0U);

    const kode_ir_code_t r = kode_ir_receive(0U);

    ir_hook_ok();
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("no infrared signal in 0 ms", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_asked_ms[0]);
}

static SemaphoreHandle_t s_holder_in;    /**< Given when the holder sits in its wait.   */
static SemaphoreHandle_t s_holder_go;    /**< Lets the holder's wait end.               */
static SemaphoreHandle_t s_holder_done;  /**< Given when the holder's call returned.    */

/** @brief The holder's wait: say so, then sit in it until let go. */
static void holder_sits(void)
{
    (void)xSemaphoreGive(s_holder_in);
    (void)xSemaphoreTake(s_holder_go, pdMS_TO_TICKS(TEST_WAIT_MS));
}

/** @brief The time the holder's capture asks for: how a request nobody waits for any more is told. */
#define TEST_HOLDER_MS 1U

/** @brief A task that holds the receiver: a capture whose wait does not end. */
static void holder_task(void *arg)
{
    kdrv_ir_symbol_t sym[1];
    size_t           n = 0U;

    (void)arg;
    fake_sem_wait_hook(&holder_sits);
    (void)kode_block_ir_capture_symbols(sym, 1U, &n, TEST_HOLDER_MS);
    (void)xSemaphoreGive(s_holder_done);
}

TEST_CASE("a read behind a holder of the receiver gives up within its time, calling it busy",
          "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    s_holder_in   = xSemaphoreCreateBinary();
    s_holder_go   = xSemaphoreCreateBinary();
    s_holder_done = xSemaphoreCreateBinary();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&holder_task, "t_irhold",
                                                    TEST_STACK_BYTES, TEST_TASK_PRIO, NULL));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_holder_in, pdMS_TO_TICKS(TEST_WAIT_MS)));

    const uint32_t before = kode_sys_time_ms();
    const kode_ir_code_t r = kode_ir_receive(100U);
    const uint32_t spent = kode_sys_time_ms() - before;

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("infrared is busy, or the radio is off", kode_error());
    TEST_ASSERT_TRUE_MESSAGE(100U >= spent, "the read kept trying after its time");

    /* And a long read waits for the holder no longer than a send does. */
    const uint32_t before_long = kode_sys_time_ms();
    const kode_ir_raw_t *const raw = kode_ir_receive_raw(KODE_FOREVER);
    const uint32_t spent_long = kode_sys_time_ms() - before_long;

    TEST_ASSERT_NOT_NULL(raw);
    TEST_ASSERT_EQUAL_UINT32(0U, raw->count);
    TEST_ASSERT_EQUAL_STRING("infrared is busy, or the radio is off", kode_error());
    TEST_ASSERT_TRUE(KODE_IR_BUSY_MS >= spent_long);
    TEST_ASSERT_TRUE(KODE_IR_BUSY_MS - KODE_RETRY_STEP_MS <= spent_long);

    (void)xSemaphoreGive(s_holder_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_holder_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_holder_in);
    vSemaphoreDelete(s_holder_go);
    vSemaphoreDelete(s_holder_done);
}

/* ---- kode_ir_last ----------------------------------------------------------- */

static kdrv_ir_frame_t s_last_frame;  /**< What the last-frame stand-in says AO_Ir heard. */
static bool            s_last_have;   /**< Whether it heard anything.                     */

/** @brief AO_Ir answering the last-frame request at the head of its queue. */
static void ir_answers_last(void)
{
    const QEvt * const e = ir_hook_head((enum_t)IR_LAST_REQ_SIG);

    if ( NULL == e )
    {
        return;
    }
    const IrLastEvt * const req = (const IrLastEvt *)e;

    if ( s_last_have )
    {
        *(kdrv_ir_frame_t *)req->out = s_last_frame;
    }
    *req->have = s_last_have;
    if ( NULL != req->ack )
    {
        *req->ack = req->gen;
    }
    khal_sem_signal(req->done);
    QF_gc(e);
}

TEST_CASE("the last frame reads as its protocol without waiting for one", "[kode_ir_standard]")
{
    kode_ir_code_t r;

    ir_fresh();
    s_last_frame = (kdrv_ir_frame_t){ HEARD_ADDR, HEARD_CMD, 34U, true };
    s_last_have  = true;
    fake_sem_wait_hook(&ir_answers_last);
    r = kode_ir_last();
    ir_hook_ok();
    TEST_ASSERT_TRUE_MESSAGE(r.ok, kode_error());
    TEST_ASSERT_EQUAL_STRING("NEC", r.protocol);
    TEST_ASSERT_EQUAL_UINT32(HEARD_ADDR, r.address);
    TEST_ASSERT_EQUAL_UINT32(HEARD_CMD, r.command);

    s_last_frame = (kdrv_ir_frame_t){ 0U, 0U, 9U, false };
    fake_sem_wait_hook(&ir_answers_last);
    r = kode_ir_last();
    ir_hook_ok();
    TEST_ASSERT_TRUE(r.ok);
    TEST_ASSERT_EQUAL_STRING("unknown", r.protocol);
    TEST_ASSERT_EQUAL_UINT32(0U, r.address);
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());
}

TEST_CASE("the last frame before any was heard is not ok and says so", "[kode_ir_standard]")
{
    kode_ir_code_t r;

    ir_fresh();
    s_last_have = false;
    fake_sem_wait_hook(&ir_answers_last);
    r = kode_ir_last();
    ir_hook_ok();
    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("no infrared signal heard yet", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());
}

/* ---- kode_ir_receive_raw ------------------------------------------------------- */

TEST_CASE("a raw read gives every symbol as a mark and a space at 38 kHz", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    s_script_sym[0].mark_us  = 9000U;
    s_script_sym[0].space_us = 4500U;
    s_script_sym[1].mark_us  = 560U;
    s_script_sym[1].space_us = 1690U;
    script_window(2U, 0U);

    const kode_ir_raw_t *const raw = kode_ir_receive_raw(1000U);

    ir_hook_ok();
    TEST_ASSERT_NOT_NULL(raw);
    TEST_ASSERT_EQUAL_UINT32(4U, raw->count);
    TEST_ASSERT_EQUAL_UINT32(KDRV_IR_CARRIER_HZ, raw->carrier_hz);
    TEST_ASSERT_EQUAL_UINT16(9000U, raw->us[0]);
    TEST_ASSERT_EQUAL_UINT16(4500U, raw->us[1]);
    TEST_ASSERT_EQUAL_UINT16(560U, raw->us[2]);
    TEST_ASSERT_EQUAL_UINT16(1690U, raw->us[3]);
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a raw read of the longest capture fills every duration", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    for ( uint32_t i = 0U; KDRV_IR_RX_MAX_SYMBOLS > i; i++ )
    {
        s_script_sym[i].mark_us  = (uint16_t)(i + 1U);
        s_script_sym[i].space_us = (uint16_t)(i + 1000U);
    }
    script_window(KDRV_IR_RX_MAX_SYMBOLS, 0U);

    const kode_ir_raw_t *const raw = kode_ir_receive_raw(1000U);

    ir_hook_ok();
    TEST_ASSERT_NOT_NULL(raw);
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_RAW_MAX, raw->count);
    TEST_ASSERT_EQUAL_UINT16(KDRV_IR_RX_MAX_SYMBOLS, raw->us[KODE_IR_RAW_MAX - 2U]);
    TEST_ASSERT_EQUAL_UINT16(KDRV_IR_RX_MAX_SYMBOLS + 999U, raw->us[KODE_IR_RAW_MAX - 1U]);
}

TEST_CASE("a raw read that hears nothing is not NULL: count 0 and the reason", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    script_window(0U, 50U);

    const kode_ir_raw_t *const raw = kode_ir_receive_raw(50U);

    ir_hook_ok();
    TEST_ASSERT_NOT_NULL(raw);
    TEST_ASSERT_EQUAL_UINT32(0U, raw->count);
    TEST_ASSERT_EQUAL_UINT32(0U, raw->carrier_hz);
    TEST_ASSERT_EQUAL_STRING("no infrared signal in 50 ms", kode_error());
}

TEST_CASE("a raw read keeps its block for the task and a new read replaces it", "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    s_script_sym[0].mark_us  = 100U;
    s_script_sym[0].space_us = 200U;
    script_window(1U, 0U);
    const kode_ir_raw_t *const first = kode_ir_receive_raw(1000U);

    ir_hook_ok();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_UINT16(100U, first->us[0]);

    script_reset();
    s_script_sym[0].mark_us  = 300U;
    s_script_sym[0].space_us = 400U;
    script_window(1U, 0U);
    const kode_ir_raw_t *const second = kode_ir_receive_raw(1000U);

    ir_hook_ok();
    TEST_ASSERT_EQUAL_PTR(first, second);
    TEST_ASSERT_EQUAL_UINT16(300U, second->us[0]);
}

/* ---- The tag state of a task that read raw ---------------------------------- */

/** @brief What the task of a task-end case leaves for the case. */
typedef struct {
    SemaphoreHandle_t    done;      /**< Given when the task has noted what it holds. */
    void                *slot;      /**< The task's khal slot (its kode_task_t).      */
    bool                 refuse;    /**< PSRAM refuses the tag block, after the task's own struct. */
    bool                 found;     /**< The read returned a result.                  */
    uint32_t             count;     /**< Its count.                                   */
    uint32_t             allocs;    /**< PSRAM blocks the task took.                  */
    char                 error[KODE_ERROR_MAX];  /**< The task's error after the read. */
} ender_t;

/** @brief A task that reads raw once and ends. */
static void raw_task_ends(void *arg)
{
    ender_t *const e = (ender_t *)arg;
    const uint32_t before = (uint32_t)fake_psram_alloc_count();

    if ( e->refuse )
    {
        fake_psram_refuse_after(1U, 1U);
    }
    /* Nothing answers: the read fails, and the block it took is what matters. */
    const kode_ir_raw_t *const raw = kode_ir_receive_raw(10U);

    fake_psram_refuse(0U);
    e->found = (NULL != raw);
    if ( NULL != raw )
    {
        e->count = raw->count;
    }
    (void)strncpy(e->error, kode_error(), sizeof(e->error) - 1U);
    e->allocs = (uint32_t)fake_psram_alloc_count() - before;
    e->slot   = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

/** @brief Run the task to its end and wait for the slot's clean-up. */
static void run_raw_task(ender_t *e)
{
    e->done = xSemaphoreCreateBinary();
    fake_task_slot_watch(NULL);

    const uint32_t allocs_before = (uint32_t)fake_psram_alloc_count();
    const uint32_t frees_before  = (uint32_t)fake_psram_free_count();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&raw_task_ends, "t_irraw",
                                                    TEST_STACK_BYTES, 1U, e));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(e->done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_NOT_NULL(e->slot);
    TEST_ASSERT_TRUE(fake_task_slot_wait_ended(TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_task_slot_watch_frees());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)fake_psram_alloc_count() - allocs_before,
                             (uint32_t)fake_psram_free_count() - frees_before);
    fake_task_slot_watch(NULL);
    vSemaphoreDelete(e->done);
}

TEST_CASE("a task that read raw infrared frees its block when it ends", "[kode_ir_standard]")
{
    ender_t e = { NULL, NULL, false, false, 1U, 0U, { '\0' } };

    ir_fresh();
    script_reset();
    run_raw_task(&e);
    TEST_ASSERT_TRUE(e.found);
    TEST_ASSERT_EQUAL_UINT32(0U, e.count);
    /* The task's struct and the tag block. */
    TEST_ASSERT_EQUAL_UINT32(2U, e.allocs);
}

TEST_CASE("a raw read whose block PSRAM refuses still answers, empty, and leaks nothing",
          "[kode_ir_standard]")
{
    ender_t e = { NULL, NULL, true, false, 1U, 0U, { '\0' } };

    ir_fresh();
    script_reset();
    run_raw_task(&e);
    TEST_ASSERT_TRUE(e.found);
    TEST_ASSERT_EQUAL_UINT32(0U, e.count);
    TEST_ASSERT_EQUAL_STRING("out of memory", e.error);
    TEST_ASSERT_EQUAL_UINT32(1U, e.allocs);
}

/* ---- Two tasks sending ------------------------------------------------------- */

static SemaphoreHandle_t s_second_go;                      /**< Lets the second task begin.                    */
static SemaphoreHandle_t s_second_done;                    /**< Given when its send has returned.              */
static bool              s_second_ok;                      /**< What its send answered.                        */
static char              s_second_error[KODE_ERROR_MAX];   /**< Its error afterwards.                          */
static uint32_t          s_pending_at_first;               /**< Requests queued when the first was answered.   */

/** @brief The second task: wait to be let go, then send a NEC frame. */
static void second_sender(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_second_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    s_second_ok = kode_ir_send("nec", 2U, 2U);
    (void)strncpy(s_second_error, kode_error(), sizeof(s_second_error) - 1U);
    (void)xSemaphoreGive(s_second_done);
}

/** @brief The first send's wait: let the second task go, answer the first, arm for the second. */
static void first_answered_second_released(void)
{
    (void)xSemaphoreGive(s_second_go);
    ir_answers_head_raw();
    s_pending_at_first = ir_pending();
    fake_sem_wait_hook(&ir_answers_head_raw);
}

TEST_CASE("two tasks sending at once are served one after the other", "[kode_ir_standard]")
{
    ir_fresh();
    raw_reset();
    s_second_ok = false;
    s_second_error[0] = '\0';
    s_pending_at_first = 0xFFFFU;
    s_second_go   = xSemaphoreCreateBinary();
    s_second_done = xSemaphoreCreateBinary();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&second_sender, "t_irsend2",
                                                    TEST_STACK_BYTES, TEST_TASK_PRIO, NULL));

    fake_sem_wait_hook(&first_answered_second_released);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send("nec", 1U, 1U), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_second_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_second_go);
    vSemaphoreDelete(s_second_done);

    TEST_ASSERT_TRUE_MESSAGE(s_second_ok, s_second_error);
    TEST_ASSERT_EQUAL_UINT32(2U, s_raw_calls);
    /* The second had posted nothing while the first was in flight. */
    TEST_ASSERT_EQUAL_UINT32(0U, s_pending_at_first);
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());
}

/* ---- The capture, its file and the file's playback ---------------------------- */

/** @brief Bytes of the largest file a case reads back from the host. */
#define TEST_FILE_BYTES 4096U

/** @brief The text of the capture file the cases expect for a NEC frame and a raw frame. */
#define TEST_CAPTURE_TEXT \
    "# kode ir capture v1\n" \
    "\n" \
    "nec 0x0012 0x0034\n" \
    "\n" \
    "raw 38000\n" \
    "2400 600 1200 600 600\n"

static TaskHandle_t      s_case_task;           /**< The task the case runs in.                         */
static bool              s_park;                /**< The capture window after the script parks the task. */
static bool              s_release_next_delay;  /**< The case's next delay lets the parked window go.   */
static bool              s_yield_case;          /**< The case's delays let the other tasks run.         */
static bool              s_yield_each;          /**< Every task's delays let the others run.            */
static SemaphoreHandle_t s_world_in;            /**< Given when a window is parked.                     */
static SemaphoreHandle_t s_world_go;            /**< Lets the parked window end.                        */

/** @brief Let AO_Storage answer what is queued, and its worker. */
static void world_storage(void)
{
    (void)qf_support_pump(AO_Storage, 8U);
    while ( fake_khal_worker_run_next_for("storage_sd") )
    {
        (void)qf_support_pump(AO_Storage, 8U);
    }
}

/** @brief The window the script is done with: park the task inside it until let go, then hear nothing. */
static void world_park(void)
{
    static const window_t quiet = { 0U, KODE_IR_CAPTURE_WINDOW_MS, NULL };

    s_park = false;
    (void)xSemaphoreGive(s_world_in);
    (void)xSemaphoreTake(s_world_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    answer_window(&quiet);
}

/** @brief Whatever waits on AO_Ir or AO_Storage is answered, from inside the wait, and the hook stays armed. */
static void world_hook(void)
{
    const QEvt *head = NULL;

    world_storage();
    /* A request whose caller has gone is dropped, as AO_Ir drops what its owner no longer waits for. */
    while ( (xQueuePeek(AO_Ir->eQueue, &head, 0U) == pdTRUE) && ((enum_t)head->sig == (enum_t)IR_CAPTURE_REQ_SIG)
           && (((const IrCaptureEvt *)head)->ms == TEST_HOLDER_MS) )
    {
        QF_gc(ir_hook_head((enum_t)IR_CAPTURE_REQ_SIG));
    }
    if ( xQueuePeek(AO_Ir->eQueue, &head, 0U) == pdTRUE )
    {
        if ( (enum_t)head->sig == (enum_t)IR_SEND_RAW_REQ_SIG )
        {
            ir_answers_head_raw();
        }
        else if ( (enum_t)head->sig != (enum_t)IR_CAPTURE_REQ_SIG )
        {
            s_hook_fault = "AO_Ir was asked for something no case expects";
        }
        else if ( s_script_at < s_script_count )
        {
            script_answer_next();
        }
        else if ( s_park )
        {
            world_park();
        }
        else
        {
            /* AO_Ir never answers: the caller's wait runs out. The request goes, as a restart would drop it. */
            QF_gc(ir_hook_head((enum_t)IR_CAPTURE_REQ_SIG));
        }
    }
    fake_sem_wait_hook(&world_hook);
}

/** @brief The clock hook of the call under test: let a parked window go, and the other tasks run. */
static void world_delay(void)
{
    if ( xTaskGetCurrentTaskHandle() == s_case_task )
    {
        if ( s_release_next_delay )
        {
            s_release_next_delay = false;
            (void)xSemaphoreGive(s_world_go);
        }
        if ( s_yield_case )
        {
            vTaskDelay(1U);
        }
    }
    else if ( s_yield_each )
    {
        vTaskDelay(1U);
    }
    else
    {
        /* another task's pause: nothing to do */
    }
    fake_time_delay_hook(&world_delay);
}

/** @brief A clean world: AO_Ir and AO_Storage that answer, empty media, a script of nothing. */
static void world_fresh(void)
{
    ir_fresh();
    (void)kdrv_storage_close();
    (void)ao_spy();
    ao_spy_drain();
    qf_support_reap_timers();
    Storage_ctor();
    qf_support_give_queue(AO_Storage);
    AO_Storage->prio  = KODE_PRIO_STORAGE;
    AO_Storage->pthre = KODE_PRIO_STORAGE;
    QASM_INIT(AO_Storage, (void *)0, 0U);
    kbsp_release(KBSP_RES_SDMMC, AO_Storage);
    kbsp_release(KBSP_RES_FATFS, AO_Storage);
    for ( ;; )
    {
        const QEvt *e = NULL;

        if ( xQueueReceive(AO_Storage->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QF_gc(e);
    }
    fake_media_reset();
    fake_time_reset();
    fake_sem_reset();
    script_reset();
    raw_reset();
    if ( NULL == s_world_in )
    {
        s_world_in = xSemaphoreCreateBinary();
        s_world_go = xSemaphoreCreateBinary();
    }
    while ( xSemaphoreTake(s_world_in, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_world_go, 0U) == pdTRUE )
    {
    }
    s_case_task    = xTaskGetCurrentTaskHandle();
    s_park         = false;
    s_release_next_delay = false;
    s_yield_case   = false;
    s_yield_each    = false;
    fake_sem_wait_hook(&world_hook);
    fake_time_delay_hook(&world_delay);
}

/** @brief The case is over: nothing is hooked, and a capture that still runs is ended. */
static void world_end(void)
{
    s_release_next_delay = true;
    s_yield_case   = true;
    fake_time_delay_hook(&world_delay);
    fake_sem_wait_hook(&world_hook);
    (void)kode_ir_capture_stop();
    fake_sem_wait_hook(NULL);
    fake_time_delay_hook(NULL);
}

/**
 * @brief The size of a file on the internal volume; UINT32_MAX when it is not there.
 * @param name The file's name in the volume.
 * @param buf  Receives up to TEST_FILE_BYTES of it, NUL-terminated.
 */
static uint32_t host_file(const char *name, char *buf)
{
    char     path[256];
    uint32_t size = UINT32_MAX;
    FILE    *f;

    (void)snprintf(path, sizeof(path), "%s/%s", fake_media_fs_dir(), name);
    f = fopen(path, "rb");
    if ( NULL != f )
    {
        const size_t got = fread(buf, 1U, TEST_FILE_BYTES - 1U, f);

        buf[got] = '\0';
        size     = (uint32_t)got;
        (void)fclose(f);
    }
    return size;
}

/** @brief The NEC frame of the heard address and command, as a window hears it. */
static kdrv_ir_symbol_t s_nec_sym[KNEC_SYMBOLS];

/** @brief A frame that is not NEC: three symbols, the last ending on a mark. */
static const kdrv_ir_symbol_t s_odd_sym[3] = { { 2400U, 600U }, { 1200U, 600U }, { 600U, 0U } };

/** @brief Script a window that hears @p sym. */
static void script_hears(const kdrv_ir_symbol_t *sym, uint32_t n)
{
    s_script[s_script_count].n          = n;
    s_script[s_script_count].advance_ms = 0U;
    s_script[s_script_count].sym        = sym;
    s_script_count++;
}

/** @brief Script a NEC frame and then one that is not. */
static void script_nec_then_odd(void)
{
    size_t n = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, knec_encode(HEARD_ADDR, HEARD_CMD, s_nec_sym, KNEC_SYMBOLS, &n));
    script_hears(s_nec_sym, (uint32_t)n);
    script_hears(s_odd_sym, 3U);
}

/** @brief Start a capture on the symbols of the case and wait until its task is parked in a window. */
static void capture_parked(const char *path)
{
    s_park = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_capture_start(path), kode_error());
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_world_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
}

/** @brief Stop the capture of a parked task: the clock lets it go and the others run. */
static bool capture_stopped(void)
{
    s_release_next_delay = true;
    s_yield_case   = true;
    return kode_ir_capture_stop();
}


TEST_CASE("a capture keeps what it hears and stop writes the file", "[kode_ir_capture]")
{
    char text[TEST_FILE_BYTES];

    world_fresh();
    script_nec_then_odd();
    capture_parked("flash/cap.txt");
    TEST_ASSERT_TRUE(kode_ir_capturing());
    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    ir_hook_ok();
    TEST_ASSERT_FALSE(kode_ir_capturing());
    TEST_ASSERT_EQUAL_UINT32(sizeof(TEST_CAPTURE_TEXT) - 1U, host_file("cap.txt", text));
    TEST_ASSERT_EQUAL_STRING(TEST_CAPTURE_TEXT, text);
    world_end();
}

TEST_CASE("a read during a capture is refused at once and says how to end it", "[kode_ir_capture]")
{
    world_fresh();
    script_nec_then_odd();
    capture_parked("flash/cap.txt");

    const uint32_t before = kode_sys_time_ms();
    const kode_ir_code_t r = kode_ir_receive(1000U);

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("an IR capture is running; kode_ir_capture_stop() ends it", kode_error());
    TEST_ASSERT_EQUAL_UINT32(before, kode_sys_time_ms());

    const kode_ir_raw_t *const raw = kode_ir_receive_raw(1000U);

    TEST_ASSERT_NOT_NULL(raw);
    TEST_ASSERT_EQUAL_UINT32(0U, raw->count);
    TEST_ASSERT_EQUAL_STRING("an IR capture is running; kode_ir_capture_stop() ends it", kode_error());

    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    ir_hook_ok();
    world_end();
}

TEST_CASE("a second start while a capture runs is refused", "[kode_ir_capture]")
{
    world_fresh();
    script_nec_then_odd();
    capture_parked("flash/cap.txt");
    TEST_ASSERT_FALSE(kode_ir_capture_start("flash/other.txt"));
    TEST_ASSERT_EQUAL_STRING("an IR capture is running", kode_error());
    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    ir_hook_ok();
    world_end();
}

static SemaphoreHandle_t s_twice_done;  /**< Given when the second stop has returned.   */
static bool              s_twice_ok;    /**< What the second stop answered.             */
static char              s_twice_error[KODE_ERROR_MAX];  /**< Its error afterwards.    */

/** @brief A task that stops the capture once. */
static void twice_task(void *arg)
{
    (void)arg;
    s_twice_ok = kode_ir_capture_stop();
    (void)strncpy(s_twice_error, kode_error(), sizeof(s_twice_error) - 1U);
    (void)xSemaphoreGive(s_twice_done);
}

/** @brief The first stop's wait: a second stop is made and answered, then the window is let go. */
static void stop_twice_hook(void)
{
    if ( xTaskGetCurrentTaskHandle() == s_case_task )
    {
        (void)khal_task_start(&twice_task, "t_irstop2", TEST_STACK_BYTES, TEST_TASK_PRIO, NULL);
        (void)xSemaphoreTake(s_twice_done, pdMS_TO_TICKS(TEST_WAIT_MS));
        (void)xSemaphoreGive(s_world_go);
        vTaskDelay(1U);
        fake_time_delay_hook(&world_delay);
    }
    else
    {
        fake_time_delay_hook(&stop_twice_hook);
    }
}

TEST_CASE("a second stop while one waits is refused and the first still finishes", "[kode_ir_capture]")
{
    char text[TEST_FILE_BYTES];

    world_fresh();
    script_nec_then_odd();
    capture_parked("flash/twice.txt");
    s_twice_done  = xSemaphoreCreateBinary();
    s_twice_ok    = true;
    s_twice_error[0] = '\0';
    s_yield_case  = true;
    fake_time_delay_hook(&stop_twice_hook);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_capture_stop(), kode_error());
    ir_hook_ok();
    TEST_ASSERT_FALSE(s_twice_ok);
    TEST_ASSERT_EQUAL_STRING("the IR capture is already being stopped", s_twice_error);
    TEST_ASSERT_EQUAL_UINT32(sizeof(TEST_CAPTURE_TEXT) - 1U, host_file("twice.txt", text));
    TEST_ASSERT_FALSE(kode_ir_capturing());
    /* And a stop with nothing running says so. */
    TEST_ASSERT_FALSE(kode_ir_capture_stop());
    TEST_ASSERT_EQUAL_STRING("no IR capture is running", kode_error());
    vSemaphoreDelete(s_twice_done);
    world_end();
}

TEST_CASE("a start with a bad path fails and starts nothing", "[kode_ir_capture]")
{
    world_fresh();
    TEST_ASSERT_FALSE(kode_ir_capture_start("usb/x"));
    TEST_ASSERT_EQUAL_STRING("bad path 'usb/x': start with sd/ or flash/", kode_error());
    TEST_ASSERT_FALSE(kode_ir_capturing());
    TEST_ASSERT_FALSE(kode_ir_capture_start(NULL));
    TEST_ASSERT_FALSE(kode_ir_capturing());
    TEST_ASSERT_EQUAL_UINT32(0U, ir_pending());

    /* No task ran: the memory a capture takes was never taken. */
    const uint32_t allocs = (uint32_t)fake_psram_alloc_count();

    TEST_ASSERT_FALSE(kode_ir_capture_start("flash/../x"));
    TEST_ASSERT_EQUAL_UINT32(allocs, (uint32_t)fake_psram_alloc_count());
    TEST_ASSERT_FALSE(kode_ir_capture_stop());
    TEST_ASSERT_EQUAL_STRING("no IR capture is running", kode_error());
    world_end();
}

TEST_CASE("a stop with nothing heard says so and writes no file", "[kode_ir_capture]")
{
    char text[TEST_FILE_BYTES];

    world_fresh();
    capture_parked("flash/none.txt");
    TEST_ASSERT_FALSE(capture_stopped());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_STRING("no infrared signal arrived", kode_error());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_file("none.txt", text));
    TEST_ASSERT_FALSE(kode_ir_capturing());

    /* A new capture starts, and the memory of the first came back. */
    const uint32_t held = (uint32_t)(fake_psram_alloc_count() - fake_psram_free_count());

    script_reset();
    script_nec_then_odd();
    capture_parked("flash/none.txt");
    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    TEST_ASSERT_EQUAL_UINT32(held, (uint32_t)(fake_psram_alloc_count() - fake_psram_free_count()));
    TEST_ASSERT_EQUAL_UINT32(sizeof(TEST_CAPTURE_TEXT) - 1U, host_file("none.txt", text));
    world_end();
}

TEST_CASE("a receiver that does not answer ends the capture, and stop says so", "[kode_ir_capture]")
{
    char text[TEST_FILE_BYTES];

    world_fresh();
    /* No script, no park: the first window is never answered. */
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_capture_start("flash/dead.txt"), kode_error());
    s_yield_case = true;
    TEST_ASSERT_FALSE(kode_ir_capture_stop());
    TEST_ASSERT_EQUAL_STRING("the infrared receiver did not answer", kode_error());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_file("dead.txt", text));
    TEST_ASSERT_FALSE(kode_ir_capturing());
    ir_hook_ok();
    world_end();
}

TEST_CASE("a stop that cannot reach the window in time gives up within its bound, and a stop later works",
          "[kode_ir_capture]")
{
    char text[TEST_FILE_BYTES];

    world_fresh();
    script_nec_then_odd();
    capture_parked("flash/slow.txt");

    /* The window stays open: the wait ends at its bound, not before and not after. */
    const uint32_t before = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_ir_capture_stop());

    const uint32_t spent = kode_sys_time_ms() - before;

    TEST_ASSERT_TRUE(KODE_IR_CAPTURE_STOP_MS >= spent);
    TEST_ASSERT_TRUE((KODE_IR_CAPTURE_STOP_MS - KODE_RETRY_STEP_MS) <= spent);
    TEST_ASSERT_EQUAL_STRING("the IR capture is still ending; call kode_ir_capture_stop() again", kode_error());
    TEST_ASSERT_TRUE(kode_ir_capturing());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_file("slow.txt", text));

    /* The window ends: the same stop now finishes the job. */
    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    ir_hook_ok();
    TEST_ASSERT_TRUE(host_file("slow.txt", text) != UINT32_MAX);
    TEST_ASSERT_EQUAL_STRING(TEST_CAPTURE_TEXT, text);
    TEST_ASSERT_FALSE(kode_ir_capturing());
    world_end();
}

TEST_CASE("a capture ends by itself with a file's worth of signals, and stop writes them all",
          "[kode_ir_capture]")
{
    static kode_ir_signal_t sig[KODE_IR_FILE_SIGNALS_MAX];
    char                    text[TEST_FILE_BYTES];
    size_t                  n = 0U;
    uint32_t                got = 0U;
    uint32_t                bad = 0U;
    const char             *why = NULL;

    world_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, knec_encode(HEARD_ADDR, HEARD_CMD, s_nec_sym, KNEC_SYMBOLS, &n));
    for ( uint32_t i = 0U; (KODE_IR_FILE_SIGNALS_MAX + 2U) > i; i++ )
    {
        script_hears(s_nec_sym, (uint32_t)n);
    }
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_capture_start("flash/full.txt"), kode_error());
    s_yield_case = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_capture_stop(), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_FILE_SIGNALS_MAX, s_script_at);
    TEST_ASSERT_TRUE(host_file("full.txt", text) != UINT32_MAX);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_file_parse(text, (uint32_t)strlen(text), sig, KODE_IR_FILE_SIGNALS_MAX,
                                                &got, &bad, &why), why);
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_FILE_SIGNALS_MAX, got);
    world_end();
}

TEST_CASE("a failed write keeps the capture, and a stop after the cause is gone writes it",
          "[kode_ir_capture]")
{
    char text[TEST_FILE_BYTES];

    world_fresh();
    script_nec_then_odd();
    capture_parked("flash/nodir/cap.txt");
    TEST_ASSERT_FALSE(capture_stopped());
    ir_hook_ok();
    TEST_ASSERT_TRUE(kode_ir_capturing());
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, host_file("nodir/cap.txt", text));

    fake_media_put_fs_dir("nodir");
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_capture_stop(), kode_error());
    TEST_ASSERT_FALSE(kode_ir_capturing());
    TEST_ASSERT_EQUAL_UINT32(sizeof(TEST_CAPTURE_TEXT) - 1U, host_file("nodir/cap.txt", text));
    TEST_ASSERT_EQUAL_STRING(TEST_CAPTURE_TEXT, text);
    world_end();
}

static SemaphoreHandle_t s_sender_done;  /**< Given when the sender task's send has returned.   */
static bool              s_sender_ok;    /**< What it answered.                                 */

/** @brief A task that sends a NEC frame once. */
static void sender_task(void *arg)
{
    (void)arg;
    s_sender_ok = kode_ir_send("nec", 1U, 2U);
    (void)xSemaphoreGive(s_sender_done);
}

TEST_CASE("a send during a capture waits for the window, goes out between windows, and the capture goes on",
          "[kode_ir_capture]")
{
    char text[TEST_FILE_BYTES];

    world_fresh();
    script_nec_then_odd();
    /* The two scripted frames are heard; the next window parks. */
    capture_parked("flash/send.txt");
    s_sender_done = xSemaphoreCreateBinary();
    s_sender_ok   = false;
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&sender_task, "t_irsend", TEST_STACK_BYTES, TEST_TASK_PRIO, NULL));
    /* It cannot go while the window is open: the path is held for that window. */
    TEST_ASSERT_EQUAL_INT(pdFALSE, xSemaphoreTake(s_sender_done, pdMS_TO_TICKS(100U)));
    TEST_ASSERT_EQUAL_UINT32(0U, s_raw_calls);

    (void)xSemaphoreGive(s_world_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_sender_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE(s_sender_ok);
    TEST_ASSERT_EQUAL_UINT32(1U, s_raw_calls);
    TEST_ASSERT_TRUE(kode_ir_capturing());
    vSemaphoreDelete(s_sender_done);

    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    ir_hook_ok();
    TEST_ASSERT_TRUE(host_file("send.txt", text) != UINT32_MAX);
    TEST_ASSERT_EQUAL_STRING(TEST_CAPTURE_TEXT, text);
    world_end();
}

TEST_CASE("a capture rides out a receiver held from outside the layer, and hears once it is free",
          "[kode_ir_capture]")
{
    char   text[TEST_FILE_BYTES];
    size_t n = 0U;

    world_fresh();
    s_holder_in   = xSemaphoreCreateBinary();
    s_holder_go   = xSemaphoreCreateBinary();
    s_holder_done = xSemaphoreCreateBinary();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&holder_task, "t_irhold", TEST_STACK_BYTES,
                                                    TEST_TASK_PRIO, NULL));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_holder_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    fake_sem_wait_hook(&world_hook);
    TEST_ASSERT_EQUAL_INT(KODE_OK, knec_encode(HEARD_ADDR, HEARD_CMD, s_nec_sym, KNEC_SYMBOLS, &n));
    script_hears(s_nec_sym, (uint32_t)n);
    s_park       = true;
    s_yield_each  = true;
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_capture_start("flash/held.txt"), kode_error());

    /* Whole windows go by refused; the capture is still there and has heard nothing. */
    vTaskDelay(pdMS_TO_TICKS(700U));
    TEST_ASSERT_TRUE(kode_sys_time_ms() >= KODE_IR_CAPTURE_WINDOW_MS);
    TEST_ASSERT_EQUAL_UINT32(0U, s_script_at);

    (void)xSemaphoreGive(s_holder_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_holder_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_world_in, pdMS_TO_TICKS(TEST_WAIT_MS)));
    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    ir_hook_ok();
    TEST_ASSERT_TRUE(host_file("held.txt", text) != UINT32_MAX);
    TEST_ASSERT_EQUAL_STRING("# kode ir capture v1\n\nnec 0x0012 0x0034\n", text);
    vSemaphoreDelete(s_holder_in);
    vSemaphoreDelete(s_holder_go);
    vSemaphoreDelete(s_holder_done);
    world_end();
}

/* ---- Playing a file back ----------------------------------------------------- */

/** @brief Put a text on the internal volume under @p name. */
static void file_put(const char *name, const char *text)
{
    fake_media_put_fs_path(name, text, strlen(text));
}

TEST_CASE("a file is sent signal by signal in order, one gap apart", "[kode_ir_file_send]")
{
    world_fresh();
    file_put("tv.txt", TEST_CAPTURE_TEXT);
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send_file("flash/tv.txt"), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(2U, s_raw_calls);
    /* The NEC frame first, at 38 kHz, 34 symbols. */
    TEST_ASSERT_EQUAL_UINT32(KNEC_SYMBOLS, s_sent_n[0]);
    TEST_ASSERT_EQUAL_UINT32(KDRV_IR_CARRIER_HZ, s_sent_hz[0]);
    /* Then the raw one: five durations are three symbols, the last ending on a mark. */
    TEST_ASSERT_EQUAL_UINT32(3U, s_sent_n[1]);
    TEST_ASSERT_EQUAL_UINT32(38000U, s_sent_hz[1]);
    TEST_ASSERT_EQUAL_UINT16(2400U, s_sent_mark[1]);
    TEST_ASSERT_EQUAL_UINT32(KODE_IR_FILE_GAP_MS, s_sent_at[1] - s_sent_at[0]);
    world_end();
}

TEST_CASE("a file with one wrong line sends nothing and names the line", "[kode_ir_file_send]")
{
    world_fresh();
    file_put("bad.txt", "# kode ir capture v1\n\nnec 1 2\n\nnec 3\n");
    TEST_ASSERT_FALSE(kode_ir_send_file("flash/bad.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/bad.txt: line 5: nec needs an address and a command", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_raw_calls);
    world_end();
}

TEST_CASE("a file with more signals than a file may carry is refused whole", "[kode_ir_file_send]")
{
    char text[TEST_FILE_BYTES];
    uint32_t at = 0U;

    world_fresh();
    for ( uint32_t i = 0U; (KODE_IR_FILE_SIGNALS_MAX + 1U) > i; i++ )
    {
        at += (uint32_t)snprintf(&text[at], sizeof(text) - at, "nec %" PRIu32 " 1\n\n", i);
    }
    file_put("many.txt", text);
    TEST_ASSERT_FALSE(kode_ir_send_file("flash/many.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/many.txt: line 65: a file holds at most 32 signals", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_raw_calls);
    world_end();
}

TEST_CASE("an empty file and a file of comments hold no signal", "[kode_ir_file_send]")
{
    world_fresh();
    file_put("empty.txt", "");
    file_put("notes.txt", "# kode ir capture v1\n\n# nothing\n");
    TEST_ASSERT_FALSE(kode_ir_send_file("flash/empty.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/empty.txt: no signal in the file", kode_error());
    TEST_ASSERT_FALSE(kode_ir_send_file("flash/notes.txt"));
    TEST_ASSERT_EQUAL_STRING("flash/notes.txt: no signal in the file", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_raw_calls);
    world_end();
}

TEST_CASE("a file that is not there, or a bad path, says what the file calls say", "[kode_ir_file_send]")
{
    char want[KODE_ERROR_MAX];

    world_fresh();
    TEST_ASSERT_NULL(kode_file_read_bytes("flash/nope.txt", NULL));
    (void)snprintf(want, sizeof(want), "%s", kode_error());
    TEST_ASSERT_TRUE(strlen(want) > 0U);
    TEST_ASSERT_FALSE(kode_ir_send_file("flash/nope.txt"));
    TEST_ASSERT_EQUAL_STRING(want, kode_error());

    TEST_ASSERT_NULL(kode_file_read_bytes("usb/x", NULL));
    (void)snprintf(want, sizeof(want), "%s", kode_error());
    TEST_ASSERT_FALSE(kode_ir_send_file("usb/x"));
    TEST_ASSERT_EQUAL_STRING(want, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_raw_calls);
    world_end();
}

TEST_CASE("a send that fails stops the file there and says why", "[kode_ir_file_send]")
{
    world_fresh();
    file_put("tv.txt", TEST_CAPTURE_TEXT);
    s_raw_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_FALSE(kode_ir_send_file("flash/tv.txt"));
    ir_hook_ok();
    TEST_ASSERT_EQUAL_STRING("out of memory", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_raw_calls);
    world_end();
}

TEST_CASE("a capture's file plays back as it was heard", "[kode_ir_file_send]")
{
    world_fresh();
    script_nec_then_odd();
    capture_parked("flash/loop.txt");
    TEST_ASSERT_TRUE_MESSAGE(capture_stopped(), kode_error());
    raw_reset();
    s_yield_case = false;
    TEST_ASSERT_TRUE_MESSAGE(kode_ir_send_file("flash/loop.txt"), kode_error());
    ir_hook_ok();
    TEST_ASSERT_EQUAL_UINT32(2U, s_raw_calls);
    TEST_ASSERT_EQUAL_UINT32(KNEC_SYMBOLS, s_sent_n[0]);
    TEST_ASSERT_EQUAL_UINT32(3U, s_sent_n[1]);
    world_end();
}

/* ---- A read that waits for a holder after an early answer -------------------- */

/** @brief The clock hook of the overstay case: a holder takes the receiver while the read pauses. */
static void holder_arrives(void)
{
    if ( xTaskGetCurrentTaskHandle() == s_case_task )
    {
        (void)khal_task_start(&holder_task, "t_irhold", TEST_STACK_BYTES, TEST_TASK_PRIO, NULL);
        (void)xSemaphoreTake(s_holder_in, pdMS_TO_TICKS(TEST_WAIT_MS));
    }
}

TEST_CASE("a read whose window ended early and then met a holder does not overstay its time",
          "[kode_ir_standard]")
{
    ir_fresh();
    script_reset();
    s_case_task   = xTaskGetCurrentTaskHandle();
    s_holder_in   = xSemaphoreCreateBinary();
    s_holder_go   = xSemaphoreCreateBinary();
    s_holder_done = xSemaphoreCreateBinary();
    /* The first window comes back empty after 300 ms of its 1000; the read pauses; a holder arrives. */
    script_window(0U, 300U);
    fake_time_delay_hook(&holder_arrives);

    const uint32_t before = kode_sys_time_ms();
    const kode_ir_code_t r = kode_ir_receive(1000U);
    const uint32_t spent = kode_sys_time_ms() - before;

    TEST_ASSERT_FALSE(r.ok);
    TEST_ASSERT_EQUAL_STRING("infrared is busy, or the radio is off", kode_error());
    TEST_ASSERT_TRUE_MESSAGE(1000U >= spent, "the read waited for the holder past its time");
    TEST_ASSERT_TRUE((1000U - KODE_RETRY_STEP_MS) <= spent);

    (void)xSemaphoreGive(s_holder_go);
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_holder_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    fake_time_delay_hook(NULL);
    vSemaphoreDelete(s_holder_in);
    vSemaphoreDelete(s_holder_go);
    vSemaphoreDelete(s_holder_done);
}
