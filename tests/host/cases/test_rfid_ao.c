/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_rfid_ao.c
 * @brief AO_Rfid: a field that lives only while asked, and a worker under it.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "fake_kdrv_rfid_lf.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kdrv_rfid_lf.h"
#include "khal_sync.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_cert.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "krfid_wave.h"
#include "lowpower_cases.h"
#include "qf_support.h"

#ifndef CONFIG_KODEOS_NFC_CERT
#error "main/CMakeLists.txt must define CONFIG_KODEOS_NFC_CERT=1 for this file"
#endif

/**
 * @brief QP ticks that outlast every deadline AO_Rfid arms here: the
 *        production row's whole budget, longer than any read's or capture's.
 *        The one tick over covers the AO's rounding up to whole ticks.
 */
#define RFID_QUIET_TICKS \
    (QF_SUPPORT_STEPS((unsigned)KODE_RFID_TEST_BUDGET_MS) + 1U)

/** @brief Samples in a staged capture window: 20 ms at 80 kS/s. */
#define WIN_SAMPLES  1600U

/** @brief Points the requester's waveform buffer holds. */
#define WAVE_POINTS  200U

/** @brief What memory the AO must not touch is filled with. */
#define UNTOUCHED    0xA5U

/** @brief What a lent status holds until AO_Rfid writes it: no kode_status_t. */
#define RC_UNWRITTEN 0xA5U

/** @brief Everything one requester lends AO_Rfid, whatever it asks for. */
typedef struct {
    uint8_t       id[5];             /**< A read's ID.                      */
    uint8_t       rf;                /**< A read's bit rate.                */
    bool          have;              /**< A read's flag.                    */
    bool          ok;                /**< A clone's read-back.              */
    uint32_t      ack;               /**< Where the generation comes back.  */
    uint32_t      rc;                /**< A capture's or a certification
                                      *   request's status.                 */
    krfid_field_t got;               /**< A capture's statistics.           */
    size_t        n;                 /**< A capture's point count.          */
    uint16_t      wave[WAVE_POINTS]; /**< A capture's waveform.             */
} lent_t;

/**
 * @brief What the cases lend AO_Rfid: static, never a case's own stack, which
 *        is kaos.h's rule that a stack frame will not do.
 *
 * A case that fails with a request in flight leaves the AO holding these
 * pointers, and the next rfid_up() ends that request by its deadline, which
 * writes through them. Into a dead stack frame that write lands on whatever
 * the next case keeps there, a return address included. rfid_up() clears the
 * lot after the leftover deadlines have fired, so each case starts from
 * zeros. Three slots: no case has more requesters than that.
 */
static lent_t s_lent[3];

/* ---- Bringing the AO up --------------------------------------------------- */

/**
 * @brief Register AO_Rfid once, and construct it afresh for every case.
 *
 * The queue is drained and any leftover timer expired before the AO is
 * re-constructed, so a QTimeEvt_ctorX() on a still-linked one cannot corrupt
 * QF's list (the test_haptic_ao.c / test_imu_ao.c rule). RFID_QUIET_TICKS
 * outlasts every deadline a case arms - a 500 ms read's is 150 ticks - so a
 * case that fails with a request in flight does not take the next one down
 * with it. A deadline that fires here answers that request through what the
 * failed case lent, which is why that memory is s_lent's and is cleared only
 * afterwards. Each status is then painted RC_UNWRITTEN, which no
 * kode_status_t is, so a case reads the exact status AO_Rfid wrote - zero
 * would read as KODE_OK.
 */
static void rfid_up(void)
{
    const QEvt *stray = NULL;

    fake_rfid_reset();
    fake_khal_worker_reset();
    qf_support_init();

    if ( NULL != AO_Rfid->eQueue )
    {
        while ( xQueueReceive(AO_Rfid->eQueue, &stray, 0U) == pdTRUE )
        {
            QF_gc(stray);
        }
    }
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);   /* leftover deadlines */
    (void)memset(s_lent, 0, sizeof(s_lent));
    for ( size_t i = 0U; (sizeof(s_lent) / sizeof(s_lent[0])) > i; i++ )
    {
        s_lent[i].rc = RC_UNWRITTEN;
    }

    (void)ao_spy();
    ao_spy_subscribe(RFID_TAG_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);

    Rfid_ctor();
    qf_support_give_queue(AO_Rfid);
    AO_Rfid->prio  = KODE_PRIO_RFID;
    AO_Rfid->pthre = KODE_PRIO_RFID;
    qf_support_register(AO_Rfid);
    while ( xQueueReceive(AO_Rfid->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    fake_klog_reset();
    QASM_INIT(AO_Rfid, (void *)0, 0U);
    ao_spy_drain();
    fake_sem_reset();
}

/** @brief Run every job the worker holds, each on the "worker's task". */
static void worker_runs(void)
{
    while ( fake_khal_worker_run_next() )
    {
        /* each job's done event is now in the AO's queue */
    }
}

/**
 * @brief Let the worker and the AO work through everything waiting for them,
 *        the row included: jobs run, their outcomes are dispatched, the next
 *        job a row submits runs, and so on until nothing is left.
 */
static void pump(void)
{
    unsigned moved;

    do
    {
        moved = 0U;
        while ( fake_khal_worker_run_next() )
        {
            moved += 1U;
        }
        moved += qf_support_settle(AO_Rfid);
    } while ( 0U < moved );
}

/** @brief Take the next event the spy holds and insist it is @p sig. */
static const QEvt *spy_expect(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the AO said nothing at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sig, (int)e->sig, "the wrong answer");
    return e;
}

/* ---- Driving the AO ------------------------------------------------------- */

/** @brief Ask for a read the way a posted request arrives. */
static void read_req(uint8_t *id, uint8_t *rf_div, bool *have, khal_sem_t done,
                     uint32_t ms, uint32_t gen, uint32_t *ack)
{
    RfidReqEvt q;

    q.super  = (QEvt)QEVT_INITIALIZER(RFID_READ_REQ_SIG);
    q.id     = id;
    q.rf_div = rf_div;
    q.have   = have;
    q.done   = done;
    q.gen    = gen;
    q.ack    = ack;
    q.ms     = ms;
    q.who    = NULL;
    q.ask    = NULL;
    q.rsp    = NULL;
    QASM_DISPATCH(AO_Rfid, &q.super, 0U);
}

/** @brief Ask for a clone the way a posted request arrives. */
static void write_req(const uint8_t *id, bool *ok, khal_sem_t done,
                      uint32_t ms, uint32_t gen, uint32_t *ack)
{
    RfidWriteReqEvt q;

    q.super = (QEvt)QEVT_INITIALIZER(RFID_WRITE_REQ_SIG);
    q.id    = id;
    q.ok    = ok;
    q.done  = done;
    q.gen   = gen;
    q.ack   = ack;
    q.ms    = ms;
    q.who   = NULL;
    q.ask   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Rfid, &q.super, 0U);
}

/** @brief Start the production row. */
static void test_req(void)
{
    static const ReaderTestReqEvt req = {
        QEVT_INITIALIZER(RFID_TEST_REQ_SIG), READER_TEST_ROW
    };

    QASM_DISPATCH(AO_Rfid, &req.super, 0U);
}

/** @brief The `rfid` snapshot as the console would read it, right now. */
static RfidInfoEvt info_now(void)
{
    RfidInfoReqEvt     q;
    const RfidInfoEvt *e;
    RfidInfoEvt        copy;

    ao_spy_drain();   /* so the response is the next thing the spy holds */
    q.super = (QEvt)QEVT_INITIALIZER(RFID_INFO_REQ_SIG);
    q.who   = ao_spy();
    QASM_DISPATCH(AO_Rfid, &q.super, 0U);
    e    = (const RfidInfoEvt *)spy_expect(RFID_INFO_RSP_SIG);
    copy = *e;
    ao_spy_drain();
    return copy;
}

/** @brief Ask for a capture the way a posted request arrives. */
static void capture_req(void *out, uint16_t *wave, size_t cap, size_t *n,
                        uint32_t *rc, khal_sem_t done, uint32_t ms,
                        uint32_t gen, uint32_t *ack, QActive *to)
{
    RfidFieldEvt q;

    q.super = (QEvt)QEVT_INITIALIZER(RFID_FIELD_REQ_SIG);
    q.out   = out;
    q.wave  = wave;
    q.cap   = cap;
    q.n     = n;
    q.rc    = rc;
    q.done  = done;
    q.ms    = ms;
    q.gen   = gen;
    q.ack   = ack;
    q.to    = to;
    q.ask   = NULL;
    q.rsp   = NULL;
    QASM_DISPATCH(AO_Rfid, &q.super, 0U);
}

/** @brief A window with something in it: bursts of 3000 over a 1000 floor. */
static void stage_window(uint16_t *s)
{
    size_t i;

    for ( i = 0U; (size_t)WIN_SAMPLES > i; i++ )
    {
        if ( (i % 16U) < 3U )
        {
            s[i] = 3000U;
        }
        else
        {
            s[i] = (uint16_t)(1000U + (i % 7U));
        }
    }
    fake_rfid_set_capture((int)KODE_OK, s, (size_t)WIN_SAMPLES);
}

/** @brief Insist a capture's answer carries no statistics at all. */
static void expect_empty(const RfidFieldRspEvt *r)
{
    TEST_ASSERT_EQUAL_UINT16(0U, r->min);
    TEST_ASSERT_EQUAL_UINT16(0U, r->mean);
    TEST_ASSERT_EQUAL_UINT16(0U, r->peak);
    TEST_ASSERT_EQUAL_UINT16(0U, r->ac);
    TEST_ASSERT_EQUAL_UINT16(0U, r->n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (unsigned)r->points,
        "a failed answer reported waveform points");
}

/**
 * @brief Run the worker's next job and take its outcome back out of the AO's
 *        queue and keep it: as far as AO_Rfid can tell, the worker is still
 *        inside its job until the case hands the outcome back with deliver().
 * @return The outcome, a KqpWorkerDoneEvt.
 */
static const QEvt *hold_outcome(void)
{
    const QEvt *held = NULL;

    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_run_next(),
        "no job was with the worker");
    TEST_ASSERT_TRUE_MESSAGE(
        xQueueReceive(AO_Rfid->eQueue, &held, 0U) == pdTRUE,
        "the worker's outcome was not queued");
    TEST_ASSERT_EQUAL_INT((int)RFID_WORKER_DONE_SIG, (int)held->sig);
    return held;
}

/** @brief The worker reports at last: hand AO_Rfid an outcome held back. */
static void deliver(const QEvt *held)
{
    QASM_DISPATCH(AO_Rfid, held, 0U);
    QF_gc(held);
}

/** @brief Whether @p out still holds only UNTOUCHED bytes. */
static bool out_untouched(const krfid_field_t *out);

/* ---- Ownership ------------------------------------------------------------ */

TEST_CASE("AO_Rfid owns the LF front end", "[rfid_ao]")
{
    rfid_up();
    TEST_ASSERT_EQUAL_STRING("AO_Rfid", kbsp_owner_name(KBSP_RES_RFID));
}

/* ---- The field lives only while asked ------------------------------------- */

TEST_CASE("rfid: field down until asked, up during a read, down after",
          "[rfid_ao]")
{
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    lent_t * const rd      = &s_lent[0];
    RfidInfoEvt    s;

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    rd->rf   = -1;
    rd->have = true;

    s = info_now();
    TEST_ASSERT_FALSE_MESSAGE(s.field, "the field is up with nobody asking");
    TEST_ASSERT_FALSE_MESSAGE(s.have, "a tag was remembered before any read");

    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 7U, &rd->ack);

    /* The outcome is queued, not yet dispatched, so the field is still up. */
    s = info_now();
    TEST_ASSERT_TRUE_MESSAGE(s.field, "the field did not come up for a read");

    pump();

    s = info_now();
    TEST_ASSERT_FALSE_MESSAGE(s.field, "the field stayed up after the read");

    TEST_ASSERT_TRUE_MESSAGE(rd->have, "the read did not report a tag");
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, rd->id, 5);
    TEST_ASSERT_EQUAL_INT_MESSAGE(64, rd->rf,
        "the bit rate was not passed back");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7U, rd->ack,
        "the answer was not stamped with the request's generation");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
        "the requester was woken more or less than once");
}

/* ---- What a decode leads to ----------------------------------------------- */

TEST_CASE("rfid: a decoded window publishes the id and lowers the field",
          "[rfid_ao]")
{
    uint8_t           want[5] = { 0x12U, 0x34U, 0x56U, 0x78U, 0x90U };
    lent_t * const    rd      = &s_lent[0];
    const RfidTagEvt *t;
    RfidInfoEvt       s;

    rfid_up();
    fake_rfid_set_window(0, want, 64U);

    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 3U, &rd->ack);
    pump();

    t = (const RfidTagEvt *)spy_expect(RFID_TAG_SIG);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, t->id, 5);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(64U, t->rf_div, "the published bit rate is wrong");
    ao_spy_drain();

    s = info_now();
    TEST_ASSERT_FALSE_MESSAGE(s.field, "the field stayed up after the decode");
    TEST_ASSERT_TRUE(s.have);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s.reads, "the decode was not counted");
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, s.id, 5);
    TEST_ASSERT_EQUAL_UINT8(64U, s.rf_div);
}

/* ---- One request at a time ------------------------------------------------ */

TEST_CASE("rfid: a second request is DENIED and posts nothing", "[rfid_ao]")
{
    uint8_t        want[5] = { 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x01U };
    lent_t * const first   = &s_lent[0];
    lent_t * const second  = &s_lent[1];

    rfid_up();
    fake_rfid_set_window(0, want, 64U);

    /* The first request is in flight: its job is with the worker. */
    read_req(first->id, &first->rf, &first->have, khal_sem_create(), 500U, 1U,
             &first->ack);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending(),
        "the request did not submit one job");

    fake_sem_reset();
    second->have = true;
    read_req(second->id, &second->rf, &second->have, khal_sem_create(), 500U,
             2U, &second->ack);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
        "the refused request was not answered");
    TEST_ASSERT_FALSE_MESSAGE(second->have, "a refused read reported a tag");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U, second->ack,
        "the refusal carried the wrong generation");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending(),
        "the refused request reached the worker anyway");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the refused request published something");

    /* Leave the AO idle: complete the first request. */
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_read_calls(),
        "the refused request ran a second job");
    ao_spy_drain();
}

/* ---- A stale worker outcome ------------------------------------------------ */

TEST_CASE("rfid: a worker answer for a retired request is ignored", "[rfid_ao]")
{
    KqpWorkerDoneEvt e;
    RfidInfoEvt      s;

    rfid_up();

    /* Nothing is in flight. A worker outcome arrives anyway - a late event
     * for a job a deadline already retired. Its request is gone, so it must
     * be discarded rather than attributed to whatever asks next. */
    fake_sem_reset();
    e.super  = (QEvt)QEVT_INITIALIZER(RFID_WORKER_DONE_SIG);
    e.seq    = 0U;
    e.status = (uint8_t)KODE_OK;
    e.value  = 0U;
    QASM_DISPATCH(AO_Rfid, &e.super, 0U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a stale worker answer was published");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
        "a stale worker answer woke a requester that was not there");

    s = info_now();
    TEST_ASSERT_FALSE(s.field);
    TEST_ASSERT_FALSE_MESSAGE(s.have, "a stale answer was recorded as the last tag");
    TEST_ASSERT_EQUAL_UINT32(0U, s.reads);
}

TEST_CASE("rfid: an answer carrying another job's sequence number is dropped",
          "[rfid_ao]")
{
    uint8_t          want[5] = { 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU };
    lent_t * const   rd      = &s_lent[0];
    const QEvt      *held;
    KqpWorkerDoneEvt stale;
    RfidInfoEvt      s;

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 4U, &rd->ack);
    held = hold_outcome();

    /* An answer of the previous job's sequence number: not this job's. */
    stale.super  = (QEvt)QEVT_INITIALIZER(RFID_WORKER_DONE_SIG);
    stale.seq    = (uint16_t)(((const KqpWorkerDoneEvt *)held)->seq + 1U);
    stale.status = (uint8_t)KODE_OK;
    stale.value  = 0U;
    QASM_DISPATCH(AO_Rfid, &stale.super, 0U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
        "an answer of another sequence number ended the request");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "an answer of another sequence number published a tag");
    s = info_now();
    TEST_ASSERT_TRUE_MESSAGE(s.field, "a stale answer freed the front end");

    /* The job's own answer still ends it. */
    deliver(held);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_TRUE(rd->have);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, rd->id, 5);
    TEST_ASSERT_EQUAL_UINT32(4U, rd->ack);
    ao_spy_drain();
    s = info_now();
    TEST_ASSERT_FALSE(s.field);
}

TEST_CASE("rfid: a read is a job on the worker and its answer is one event",
          "[rfid_ao]")
{
    uint8_t                 want[5] = { 0x21U, 0x22U, 0x23U, 0x24U, 0x25U };
    lent_t * const          rd      = &s_lent[0];
    const KqpWorkerDoneEvt *done;
    const QEvt             *held;

    rfid_up();
    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_last_psram(),
        "the LF worker's stack is not in PSRAM");
    fake_rfid_set_window(0, want, 64U);

    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 9U, &rd->ack);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending(),
        "the read was not submitted as a job");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U,
        (uint32_t)uxQueueMessagesWaiting(AO_Rfid->eQueue),
        "an answer arrived before the job ran");
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());

    held = hold_outcome();
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_read_calls());
    done = (const KqpWorkerDoneEvt *)held;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, done->status);
    TEST_ASSERT_EQUAL_UINT(0U, fake_sem_signals());

    deliver(held);
    TEST_ASSERT_TRUE(rd->have);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, rd->id, 5);
    TEST_ASSERT_EQUAL_INT(64, rd->rf);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    ao_spy_drain();
}

/* ---- The write boost ------------------------------------------------------ */

TEST_CASE("rfid: a read keeps VO4 low, a clone boosts it", "[rfid_ao]")
{
    uint8_t        want[5] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U };
    uint8_t        wid[5]  = { 0xABU, 0xCDU, 0xEFU, 0x01U, 0x23U };
    lent_t * const rd      = &s_lent[0];
    lent_t * const cl      = &s_lent[1];

    rfid_up();
    fake_rfid_set_window(0, want, 64U);

    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 1U, &rd->ack);
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_read_calls(),
        "the read was not sent as a read job");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_clone_calls(), "a read cloned");
    TEST_ASSERT_FALSE_MESSAGE(fake_rfid_last_boost(), "a read boosted VO4");
    ao_spy_drain();

    write_req(wid, &cl->ok, khal_sem_create(), 500U, 2U, &cl->ack);
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_clone_calls(),
        "the clone was not sent as a clone job");
    TEST_ASSERT_TRUE_MESSAGE(fake_rfid_last_boost(), "the clone did not boost VO4");
    TEST_ASSERT_TRUE_MESSAGE(cl->ok,
        "the read-back did not match the ID written");
    TEST_ASSERT_EQUAL_UINT32(2U, cl->ack);
    ao_spy_drain();
}

/* ---- The production row --------------------------------------------------- */

TEST_CASE("rfid row: flat windows fail after KODE_RFID_TEST_WINDOWS",
          "[rfid_ao]")
{
    const TestResultEvt *r;

    rfid_up();
    /* The fake's default window is flat; leave it. */

    test_req();
    pump();

    r = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("rfid", r->name);
    TEST_ASSERT_FALSE_MESSAGE(r->ok, "a flat antenna passed the row");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KODE_RFID_TEST_WINDOWS,
        fake_rfid_read_calls(), "the row did not try every window before giving up");
    ao_spy_drain();
}

TEST_CASE("rfid row: KODE_RFID_TEST_TRIES identical decodes pass, and stop early",
          "[rfid_ao]")
{
    uint8_t              want[5] = { 0x0AU, 0x0BU, 0x0CU, 0x0DU, 0x0EU };
    const TestResultEvt *r;

    rfid_up();
    fake_rfid_set_window(0, want, 64U);

    test_req();
    pump();

    r = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("rfid", r->name);
    TEST_ASSERT_TRUE_MESSAGE(r->ok, "a stable ID did not pass the row");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KODE_RFID_TEST_TRIES, fake_rfid_read_calls(),
        "the row did not stop at the first stable ID");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the row published a tag instead of only its verdict");
    ao_spy_drain();
}

/* ---- A capture leaves the carrier down ------------------------------------ */

TEST_CASE("rfid capture: the carrier stays down and the worker fills the "
          "requester's memory", "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    static uint16_t want_wave[WAVE_POINTS];
    lent_t * const  cp = &s_lent[0];
    krfid_field_t   want;
    size_t          want_n;
    RfidInfoEvt     s;
    const RfidFieldRspEvt *r;

    rfid_up();
    stage_window(samples);
    krfid_wave_stats(samples, (size_t)WIN_SAMPLES, &want);
    want_n = krfid_wave_decimate(samples, (size_t)WIN_SAMPLES, want_wave,
                                 (size_t)WAVE_POINTS);

    (void)memset(&cp->got, (int)UNTOUCHED, sizeof(cp->got));
    cp->n = 999U;
    capture_req(&cp->got, cp->wave, (size_t)WAVE_POINTS, &cp->n, &cp->rc,
                khal_sem_create(), 20U, 5U, &cp->ack, ao_spy());

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending(),
        "the capture was not handed to the worker as a job");
    worker_runs();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_capture_calls(),
        "the capture was not run as a CAPTURE job");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_read_calls(),
        "the capture was sent as a read");

    /* The outcome is queued, not yet dispatched: the request is live, and
     * what the front end hears must be somebody else's field. */
    s = info_now();
    TEST_ASSERT_FALSE_MESSAGE(s.field, "a capture raised the carrier");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(RC_UNWRITTEN, cp->rc,
        "the requester was answered early");

    pump();

    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    TEST_ASSERT_EQUAL_UINT16(want.min, r->min);
    TEST_ASSERT_EQUAL_UINT16(want.mean, r->mean);
    TEST_ASSERT_EQUAL_UINT16(want.peak, r->peak);
    TEST_ASSERT_EQUAL_UINT16(want.ac, r->ac);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)WIN_SAMPLES, r->n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)want_n, (unsigned)r->points,
        "the answer's point count is not what went into the wave");
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_OK, cp->rc,
        "the requester was not told the window was read");
    TEST_ASSERT_EQUAL_MEMORY(&want, &cp->got, sizeof(cp->got));
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)want_n, (unsigned)cp->n,
        "the requester's point count is wrong");
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want_wave, cp->wave, want_n);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(5U, cp->ack,
        "the answer was not stamped with the request's generation");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_sem_signals(),
        "the requester was woken more or less than once");

    s = info_now();
    TEST_ASSERT_FALSE(s.field);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s.reads,
        "a capture counted as a read");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, s.windows,
        "a capture counted as an empty read window");
}

TEST_CASE("rfid capture: an Active Object that lends no memory is still told "
          "the statistics", "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    krfid_field_t   want;
    const RfidFieldRspEvt *r;

    rfid_up();
    stage_window(samples);
    krfid_wave_stats(samples, (size_t)WIN_SAMPLES, &want);

    capture_req(NULL, NULL, 0U, NULL, NULL, NULL, 20U, 0U, NULL, ao_spy());
    pump();

    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    TEST_ASSERT_EQUAL_UINT16(want.min, r->min);
    TEST_ASSERT_EQUAL_UINT16(want.mean, r->mean);
    TEST_ASSERT_EQUAL_UINT16(want.peak, r->peak);
    TEST_ASSERT_EQUAL_UINT16(want.ac, r->ac);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)WIN_SAMPLES, r->n);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (unsigned)r->points,
        "points reported for a capture that asked for no waveform");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
        "a semaphore nobody lent was signalled");
    ao_spy_drain();
}

TEST_CASE("rfid capture: a zero window is refused with KODE_ERROR and reaches "
          "no worker", "[rfid_ao]")
{
    lent_t * const cp = &s_lent[0];
    krfid_field_t  sentinel;
    const RfidFieldRspEvt *r;

    rfid_up();
    (void)memset(&cp->got, (int)UNTOUCHED, sizeof(cp->got));
    (void)memset(&sentinel, (int)UNTOUCHED, sizeof(sentinel));
    cp->wave[0] = 7U;
    cp->n       = 999U;
    cp->rc      = RC_UNWRITTEN;

    capture_req(&cp->got, cp->wave, 4U, &cp->n, &cp->rc, khal_sem_create(),
                0U, 9U, &cp->ack, ao_spy());

    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    expect_empty(r);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_ERROR, cp->rc,
        "a refused zero window was not told why");
    TEST_ASSERT_EQUAL_UINT32(9U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_capture_calls(),
        "a zero window reached the worker");
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &cp->got, sizeof(cp->got));
    TEST_ASSERT_EQUAL_UINT(999U, (unsigned)cp->n);
    TEST_ASSERT_EQUAL_UINT16(7U, cp->wave[0]);
}

TEST_CASE("rfid capture: a read in flight refuses it with KODE_DENIED",
          "[rfid_ao]")
{
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    lent_t * const rd      = &s_lent[0];
    lent_t * const cp      = &s_lent[1];
    const RfidFieldRspEvt *r;

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 1U, &rd->ack);

    fake_sem_reset();
    cp->rc = RC_UNWRITTEN;
    capture_req(NULL, NULL, 0U, NULL, &cp->rc, khal_sem_create(), 20U, 2U,
                &cp->ack, ao_spy());

    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, r->st);
    expect_empty(r);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, cp->rc);
    TEST_ASSERT_EQUAL_UINT32(2U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_capture_calls());

    /* Leave the AO idle: the read completes as it would have. */
    pump();
    TEST_ASSERT_TRUE_MESSAGE(rd->have,
        "the refusal disturbed the read in flight");
    TEST_ASSERT_EQUAL_UINT32(1U, rd->ack);
    ao_spy_drain();
}

TEST_CASE("rfid capture: a capture in flight holds the front end against a "
          "read and a second capture", "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    lent_t * const  cp  = &s_lent[0];
    lent_t * const  rd  = &s_lent[1];
    lent_t * const  cp2 = &s_lent[2];
    const RfidFieldRspEvt *r;

    rfid_up();
    stage_window(samples);
    capture_req(&cp->got, NULL, 0U, NULL, &cp->rc, khal_sem_create(), 20U,
                1U, &cp->ack, NULL);

    fake_sem_reset();
    rd->have = true;
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 2U, &rd->ack);
    TEST_ASSERT_FALSE_MESSAGE(rd->have, "a read ran under a capture");
    TEST_ASSERT_EQUAL_UINT32(2U, rd->ack);
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());

    cp2->rc = RC_UNWRITTEN;
    capture_req(NULL, NULL, 0U, NULL, &cp2->rc, khal_sem_create(), 20U, 3U,
                &cp2->ack, ao_spy());
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, r->st);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, cp2->rc);
    TEST_ASSERT_EQUAL_UINT32(3U, cp2->ack);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_sem_signals(),
        "the two refusals were not both answered");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending(),
        "the refused requests reached the worker");

    /* The first capture still ends as it would have. */
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_capture_calls(),
        "the second capture reached the worker");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, cp->rc);
    TEST_ASSERT_EQUAL_UINT32(1U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(3U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a capture that named no Active Object posted an answer");
}

TEST_CASE("rfid capture: a failed window answers KODE_ERROR and writes nothing",
          "[rfid_ao]")
{
    lent_t * const cp = &s_lent[0];
    krfid_field_t  sentinel;
    const RfidFieldRspEvt *r;

    rfid_up();
    /* The fake's default: the ADC delivered nothing. */
    (void)memset(&cp->got, (int)UNTOUCHED, sizeof(cp->got));
    (void)memset(&sentinel, (int)UNTOUCHED, sizeof(sentinel));
    cp->wave[0] = 7U;
    cp->n       = 999U;
    cp->rc      = RC_UNWRITTEN;

    capture_req(&cp->got, cp->wave, 4U, &cp->n, &cp->rc, khal_sem_create(),
                20U, 4U, &cp->ack, ao_spy());
    pump();

    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    expect_empty(r);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(1, fake_rfid_capture_calls());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_ERROR, cp->rc,
        "a failed window was not reported as one");
    TEST_ASSERT_EQUAL_UINT32(4U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_MEMORY(&sentinel, &cp->got, sizeof(cp->got));
    TEST_ASSERT_EQUAL_UINT(999U, (unsigned)cp->n);
    TEST_ASSERT_EQUAL_UINT16(7U, cp->wave[0]);
}

TEST_CASE("rfid capture: only the worker may take the window", "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    lent_t * const  cp = &s_lent[0];
    krfid_field_t   got;

    /* Before the AO has started: no worker check is set, so none may. */
    fake_rfid_reset();
    stage_window(samples);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KODE_DENIED,
        (int)kdrv_rfid_lf_field_capture(20U, &got, NULL, 0U, NULL),
        "a capture ran before the AO named its worker");

    rfid_up();
    stage_window(samples);
    (void)memset(&got, (int)UNTOUCHED, sizeof(got));

    /* The AO has started and named its worker; this thread is not it. */
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KODE_DENIED,
        (int)kdrv_rfid_lf_field_capture(20U, &got, NULL, 0U, NULL),
        "a capture ran on a task that is not the AO's worker");
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_capture_calls());
    TEST_ASSERT_TRUE_MESSAGE(out_untouched(&got),
        "a refused capture wrote the statistics");

    /* The same window, as a job: the worker's own task takes it. */
    capture_req(&cp->got, NULL, 0U, NULL, &cp->rc, khal_sem_create(), 20U, 1U,
                &cp->ack, NULL);
    pump();
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_capture_calls());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, cp->rc);
}

TEST_CASE("rfid capture: a worker that never answers is ended by a deadline "
          "sized to the capped window", "[rfid_ao]")
{
    lent_t * const cp = &s_lent[0];
    RfidInfoEvt    s;
    const QEvt    *held;
    const RfidFieldRspEvt *r;
    /* KDRV_RFID_LF_CAPTURE_MS_MAX plus the AO's 1000 ms of slack, in 10 ms
     * ticks. The request asks for four times the cap, which uncapped would
     * wait 1600 ms: late enough to tell apart, and soon enough that a failure
     * here leaves no deadline RFID_QUIET_TICKS cannot expire. */
    const unsigned dl_ticks =
        QF_SUPPORT_STEPS(KDRV_RFID_LF_CAPTURE_MS_MAX + 1000U);

    rfid_up();
    cp->rc = RC_UNWRITTEN;
    capture_req(NULL, NULL, 0U, NULL, &cp->rc, khal_sem_create(),
                4U * KDRV_RFID_LF_CAPTURE_MS_MAX, 6U, &cp->ack, ao_spy());
    held = hold_outcome();

    (void)qf_support_tick(AO_Rfid, dl_ticks - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the deadline fired before the capped window's slack ran out");
    TEST_ASSERT_EQUAL_UINT(0U, fake_sem_signals());

    (void)qf_support_tick(AO_Rfid, 1U);
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    expect_empty(r);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, cp->rc);
    TEST_ASSERT_EQUAL_UINT32(6U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());

    /* The worker reports late; the outcome answers nobody a second time. */
    deliver(held);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a late outcome answered the retired capture again");
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());

    /* The front end is free again. */
    s = info_now();
    TEST_ASSERT_FALSE(s.field);
    capture_req(NULL, NULL, 0U, NULL, NULL, NULL, 20U, 0U, NULL, ao_spy());
    pump();
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_ERROR, r->st,
        "the AO stayed busy after the worker reported");
    TEST_ASSERT_EQUAL_INT(2, fake_rfid_capture_calls());
    ao_spy_drain();
}

TEST_CASE("rfid capture: a retired capture's job refuses a capture, a read "
          "and the row until the worker reports", "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    lent_t * const  cp  = &s_lent[0];
    lent_t * const  cp2 = &s_lent[1];
    lent_t * const  rd  = &s_lent[2];
    const QEvt     *held;
    const RfidFieldRspEvt *r;
    const TestResultEvt   *t;

    rfid_up();
    stage_window(samples);
    capture_req(&cp->got, cp->wave, (size_t)WAVE_POINTS, &cp->n, &cp->rc,
                khal_sem_create(), 20U, 1U, &cp->ack, ao_spy());
    held = hold_outcome();

    /* The deadline answers the requester; the worker is still in its window
     * and may still write what the requester lent. */
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, cp->rc);
    TEST_ASSERT_EQUAL_UINT32(1U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());

    /* So a second capture is refused, and its memory reaches no worker. */
    cp2->rc = RC_UNWRITTEN;
    capture_req(&cp2->got, cp2->wave, (size_t)WAVE_POINTS, &cp2->n,
                &cp2->rc, khal_sem_create(), 20U, 2U, &cp2->ack, ao_spy());
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_DENIED, r->st,
        "a capture was taken while the worker still held the last one");
    expect_empty(r);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, cp2->rc);
    TEST_ASSERT_EQUAL_UINT32(2U, cp2->ack);
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_capture_calls());

    /* So is a read. */
    rd->have = true;
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 3U,
             &rd->ack);
    TEST_ASSERT_FALSE_MESSAGE(rd->have, "a read ran under a retired capture");
    TEST_ASSERT_EQUAL_UINT32(3U, rd->ack);
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());

    /* And the row fails at once, reading nothing. */
    test_req();
    t = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("rfid", t->name);
    TEST_ASSERT_FALSE(t->ok);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());
    TEST_ASSERT_EQUAL_UINT(3U, fake_sem_signals());

    /* The worker reports at last: its outcome answers nobody... */
    deliver(held);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a late outcome answered somebody");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, fake_sem_signals(),
        "a late outcome woke a requester");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_ERROR, cp->rc,
        "a late outcome told the retired requester its window was read");
    TEST_ASSERT_EQUAL_UINT32(1U, cp->ack);

    /* ...and the front end takes the next request. */
    capture_req(&cp2->got, cp2->wave, (size_t)WAVE_POINTS, &cp2->n,
                &cp2->rc, khal_sem_create(), 20U, 4U, &cp2->ack, ao_spy());
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending());
    pump();
    TEST_ASSERT_EQUAL_INT(2, fake_rfid_capture_calls());
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, cp2->rc);
    TEST_ASSERT_EQUAL_UINT32(4U, cp2->ack);
}

TEST_CASE("rfid: a read retired by its deadline keeps the carrier up until "
          "the worker reports, and its late decode is discarded", "[rfid_ao]")
{
    uint8_t        want[5]  = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    uint8_t        blank[5] = { 0U, 0U, 0U, 0U, 0U };
    lent_t * const rd       = &s_lent[0];
    const QEvt    *held;
    RfidInfoEvt    s;

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 7U,
             &rd->ack);
    held = hold_outcome();

    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    TEST_ASSERT_FALSE(rd->have);
    TEST_ASSERT_EQUAL_UINT32(7U, rd->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    s = info_now();
    TEST_ASSERT_TRUE_MESSAGE(s.field,
        "the carrier was reported down while the worker may still hold it");
    TEST_ASSERT_EQUAL_UINT32(1U, s.windows);

    /* The worker reports a decode after the requester has gone. */
    deliver(held);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a late decode was published");
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(blank, rd->id, 5,
        "a late decode was written into a retired read's buffer");
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    s = info_now();
    TEST_ASSERT_FALSE(s.field);
    TEST_ASSERT_FALSE_MESSAGE(s.have, "a late decode became the last tag");
    TEST_ASSERT_EQUAL_UINT32(0U, s.reads);
}

TEST_CASE("rfid row: a row ended by its budget fails, and its window keeps "
          "the front end until the worker reports", "[rfid_ao]")
{
    const QEvt          *held;
    const TestResultEvt *t;
    const RfidFieldRspEvt *r;

    rfid_up();
    test_req();
    held = hold_outcome();

    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    t = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("rfid", t->name);
    TEST_ASSERT_FALSE(t->ok);
    ao_spy_drain();

    capture_req(NULL, NULL, 0U, NULL, NULL, NULL, 20U, 0U, NULL, ao_spy());
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_DENIED, r->st,
        "a capture was taken while the row's window was still with the worker");
    ao_spy_drain();

    deliver(held);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_read_calls(),
        "a late window started the finished row again");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a late window gave the row a second verdict");

    capture_req(NULL, NULL, 0U, NULL, NULL, NULL, 20U, 0U, NULL, ao_spy());
    pump();
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_capture_calls());
    ao_spy_drain();
}

TEST_CASE("rfid row: a test request during a capture fails at once and the "
          "capture is still answered", "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    lent_t * const  cp = &s_lent[0];
    krfid_field_t   want;
    const TestResultEvt   *t;
    const RfidFieldRspEvt *r;

    rfid_up();
    stage_window(samples);
    krfid_wave_stats(samples, (size_t)WIN_SAMPLES, &want);
    capture_req(&cp->got, NULL, 0U, NULL, &cp->rc, khal_sem_create(), 20U,
                5U, &cp->ack, ao_spy());

    /* The capture's outcome is queued, not yet taken, when the row is
     * asked for. */
    test_req();
    t = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_EQUAL_STRING("rfid", t->name);
    TEST_ASSERT_FALSE_MESSAGE(t->ok, "a row ran under a capture");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_read_calls(),
        "the refused row read a window anyway");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
        "the refusal ended the capture");

    pump();
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_OK, r->st,
        "the capture in flight was abandoned by the refused row");
    TEST_ASSERT_EQUAL_UINT16(want.min, r->min);
    TEST_ASSERT_EQUAL_UINT16(want.mean, r->mean);
    TEST_ASSERT_EQUAL_UINT16(want.peak, r->peak);
    TEST_ASSERT_EQUAL_UINT16(want.ac, r->ac);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, cp->rc);
    TEST_ASSERT_EQUAL_UINT32(5U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
}

TEST_CASE("rfid capture: the certification carrier is refused busy and the "
          "capture keeps its deadline", "[rfid_ao]")
{
    lent_t * const cp = &s_lent[0];
    lent_t * const ct = &s_lent[1];
    RfidInfoEvt    s;
    const QEvt    *held;
    const RfidFieldRspEvt *r;

    rfid_up();
    cp->rc = RC_UNWRITTEN;
    capture_req(NULL, NULL, 0U, NULL, &cp->rc, khal_sem_create(), 20U, 6U,
                &cp->ack, ao_spy());
    /* The worker is taken to be inside its window still: nothing but the
     * deadline can end the capture now, and only if the stop below left it
     * armed. */
    held = hold_outcome();

    /* A stop is refused: it would free the ADC handle and the read buffers
     * under the worker and disarm the capture's deadline. */
    ct->rc = 0xFFU;
    rfid_cert_request(0U, &ct->rc, khal_sem_create(), 21U, &ct->ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Rfid, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ct->rc);
    TEST_ASSERT_EQUAL_UINT32(21U, ct->ack);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_field_off_calls(),
        "the refused stop took the front end down under the worker");

    /* So is a carrier: it would energise the coil the capture listens with. */
    ct->rc = 0xFFU;
    rfid_cert_request(1U, &ct->rc, khal_sem_create(), 22U, &ct->ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Rfid, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ct->rc);
    TEST_ASSERT_EQUAL_UINT32(22U, ct->ack);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_sem_signals(),
        "the two refusals were not both answered");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a refusal answered the capture's requester");
    s = info_now();
    TEST_ASSERT_FALSE_MESSAGE(s.field,
        "a certification carrier came up under a capture");

    /* The capture still ends on its own deadline and answers its requester. */
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, cp->rc);
    TEST_ASSERT_EQUAL_UINT32(6U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(3U, fake_sem_signals());

    /* The worker is still in its window after the deadline, so a stop is
     * still refused: it would free the buffers under it all the same. */
    ct->rc = 0xFFU;
    rfid_cert_request(0U, &ct->rc, khal_sem_create(), 23U, &ct->ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Rfid, 4U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_DENIED, ct->rc,
        "a stop was taken while the worker still held a retired window");
    TEST_ASSERT_EQUAL_UINT32(23U, ct->ack);
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_field_off_calls());

    /* Once the worker reports, a stop is success and takes the front end
     * down. */
    deliver(held);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ct->rc = 0xFFU;
    rfid_cert_request(0U, &ct->rc, khal_sem_create(), 24U, &ct->ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Rfid, 4U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, ct->rc);
    TEST_ASSERT_EQUAL_UINT32(24U, ct->ack);
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_field_off_calls());
}

TEST_CASE("rfid_capture_request posts the request and the answer comes back "
          "on both channels", "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    lent_t * const  cp = &s_lent[0];
    krfid_field_t   want;
    const RfidFieldRspEvt *r;

    rfid_up();
    stage_window(samples);
    krfid_wave_stats(samples, (size_t)WIN_SAMPLES, &want);

    rfid_capture_request(&cp->got, cp->wave, (size_t)WAVE_POINTS, &cp->n,
                         &cp->rc, khal_sem_create(), 20U, 8U, &cp->ack,
                         ao_spy());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U,
        (unsigned)uxQueueMessagesWaiting(AO_Rfid->eQueue),
        "the request was not posted to AO_Rfid");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_capture_calls(),
        "the request was served before AO_Rfid dispatched it");

    pump();

    r = (const RfidFieldRspEvt *)spy_expect(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    TEST_ASSERT_EQUAL_UINT16(want.peak, r->peak);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)WAVE_POINTS, (unsigned)r->points,
        "a 1600-sample window did not fill a 200-point wave");
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, cp->rc);
    TEST_ASSERT_EQUAL_MEMORY(&want, &cp->got, sizeof(cp->got));
    TEST_ASSERT_EQUAL_UINT((unsigned)WAVE_POINTS, (unsigned)cp->n);
    TEST_ASSERT_EQUAL_UINT32(8U, cp->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
}

/* ---- The facade: kode_block_rfid_read -------------------------------------------
 * The blocking calls an app rests on. Each posts a request to AO_Rfid and
 * sleeps on a semaphore; on the single-threaded host the AO runs from inside
 * that sleep, armed with fake_sem_wait_hook() - which is where it runs on the
 * board too, sitting above everything an app is likely to have. The fake's
 * submit() is synchronous, so one pump carries a request through to its answer
 * - the caller's have/ok flag and its ack are filled - and the wait returns
 * with the result in hand.
 */

/**
 * @brief Let AO_Rfid run from inside a facade wait, then stand in for kode_init.
 *
 * pump() carries the request through to its outcome, but that outcome does NOT
 * signal the facade's semaphore: kode_init() creates s_rfid_sem on the target
 * and no host case runs it, so the facade lends AO_Rfid a NULL semaphore and
 * req_release() does not signal one it was handed as NULL. The
 * khal_sem_signal(NULL) here is that missing wake, on the fake's binary token,
 * and stands in for kode_init() and nothing else - the test_kode_ir.c idiom.
 */
static void rfid_runs_while_asleep(void)
{
    pump();
    khal_sem_signal(NULL);
}

TEST_CASE("kode_block_rfid_read returns the decoded id via the facade", "[rfid_ao]")
{
    uint8_t       want[5] = { 0x12U, 0x34U, 0x56U, 0x78U, 0x90U };
    uint8_t       id[5];
    uint8_t       rf = 0U;
    kode_status_t st;

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    fake_sem_wait_hook(&rfid_runs_while_asleep);

    (void)memset(id, 0, sizeof(id));
    st = kode_block_rfid_read(id, &rf, 500U);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, st, "a decoded read did not report OK");
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, id, 5);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(64U, rf, "the bit rate was not passed back");
    ao_spy_drain();   /* the decode also published RFID_TAG_SIG */
}

TEST_CASE("kode_block_rfid_read rejects a NULL id, a NULL rf_div or a zero ms",
          "[rfid_ao]")
{
    uint8_t id[5];
    uint8_t rf = 0U;

    rfid_up();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_rfid_read(NULL, &rf, 500U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_rfid_read(id, NULL, 500U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_rfid_read(id, &rf, 0U));

    /* Refused before anything is posted: nothing reached AO_Rfid's queue, the
     * worker was never called, and no waiter was ever woken. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U,
        (unsigned)uxQueueMessagesWaiting(AO_Rfid->eQueue),
        "a refused facade call posted a request anyway");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_read_calls(),
        "a refused facade call reached the worker");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
        "a refused facade call woke a waiter");
}

/* ---- The facade: kode_block_rfid_write ---------------------------------------
 * The clone's blocking call. The ID is copied into the facade's own buffer
 * before the request is posted, so the caller's array is never lent.
 */

/** @brief What the write cases ask to be written. */
static const uint8_t s_write_want[5] = { 0xA1U, 0xB2U, 0xC3U, 0xD4U, 0xE5U };

/** @brief The caller's array, which a hook scribbles on while the call waits. */
static uint8_t s_write_caller[5];

/** @brief The status a second caller got while the first waited. */
static kode_status_t s_write_second;

/** @brief Change the caller's array, then let AO_Rfid run. */
static void rfid_runs_after_caller_scribbles(void)
{
    (void)memset(s_write_caller, 0x5AU, sizeof(s_write_caller));
    pump();
    khal_sem_signal(NULL);
}

/** @brief Ask again from inside the wait, then let AO_Rfid run. */
static void rfid_runs_after_second_caller(void)
{
    s_write_second = kode_block_rfid_write(s_write_want, 500U);
    pump();
    khal_sem_signal(NULL);
}

TEST_CASE("kode_block_rfid_write is KODE_OK when the read-back matches", "[rfid_ao]")
{
    uint8_t written[5];

    rfid_up();
    (void)memcpy(s_write_caller, s_write_want, sizeof(s_write_caller));
    fake_sem_wait_hook(&rfid_runs_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_write(s_write_caller, 500U));
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_clone_calls());
    TEST_ASSERT_TRUE_MESSAGE(fake_rfid_last_boost(), "a write ran without the boost");
    fake_rfid_last_clone_id(written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_write_want, written, 5);
    ao_spy_drain();
}

TEST_CASE("kode_block_rfid_write lends its own copy of the id", "[rfid_ao]")
{
    uint8_t written[5];

    rfid_up();
    (void)memcpy(s_write_caller, s_write_want, sizeof(s_write_caller));
    fake_sem_wait_hook(&rfid_runs_after_caller_scribbles);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_write(s_write_caller, 500U));
    fake_rfid_last_clone_id(written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(s_write_want, written, 5,
        "the caller's array, changed while the call waited, was what got written");
    ao_spy_drain();
}

TEST_CASE("kode_block_rfid_write is KODE_TIMEOUT when the read-back differs",
          "[rfid_ao]")
{
    rfid_up();
    fake_rfid_set_clone_mismatch(true);
    fake_sem_wait_hook(&rfid_runs_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_rfid_write(s_write_want, 500U));
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_clone_calls());
    ao_spy_drain();
}

TEST_CASE("kode_block_rfid_write is KODE_TIMEOUT when nothing answers", "[rfid_ao]")
{
    rfid_up();
    /* No stand-in: the request waits in AO_Rfid's queue and the call gives up. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_rfid_write(s_write_want, 10U));
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_clone_calls());
}

TEST_CASE("kode_block_rfid_write rejects a NULL id or a zero ms", "[rfid_ao]")
{
    rfid_up();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_rfid_write(NULL, 500U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_rfid_write(s_write_want, 0U));

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U,
        (unsigned)uxQueueMessagesWaiting(AO_Rfid->eQueue),
        "a refused write posted a request anyway");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_clone_calls(),
        "a refused write reached the worker");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_sem_signals(),
        "a refused write woke a waiter");
}

TEST_CASE("kode_block_rfid_write is KODE_DENIED while another RFID call waits",
          "[rfid_ao]")
{
    rfid_up();
    s_write_second = KODE_OK;
    fake_sem_wait_hook(&rfid_runs_after_second_caller);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_write(s_write_want, 500U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_write_second,
        "a second call was let through while the first waited");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_clone_calls(),
        "the refused call reached the worker");
    ao_spy_drain();
}

/* ---- Memory a request lends --------------------------------------------------
 * The statistics and the waveform a capture fills are the requester's, lent
 * for one request: painted before it, and compared after, they say who wrote
 * what and when.
 */

/** @brief Paint what a facade call must not touch. */
static void paint(krfid_field_t *out, uint16_t *wave, size_t cap, size_t *n)
{
    (void)memset(out, (int)UNTOUCHED, sizeof(*out));
    for ( size_t i = 0U; i < cap; ++i )
    {
        wave[i] = 0x5A5AU;
    }
    *n = 999U;
}

/** @brief Whether @p wave still holds only paint(). */
static bool wave_untouched(const uint16_t *wave, size_t cap)
{
    for ( size_t i = 0U; i < cap; ++i )
    {
        if ( 0x5A5AU != wave[i] )
        {
            return false;
        }
    }
    return true;
}

/** @brief Whether @p out still holds only paint(). */
static bool out_untouched(const krfid_field_t *out)
{
    krfid_field_t sentinel;

    (void)memset(&sentinel, (int)UNTOUCHED, sizeof(sentinel));
    return memcmp(out, &sentinel, sizeof(sentinel)) == 0;
}

/**
 * @brief AO_Rfid runs while the facade sleeps, with a read still in flight:
 *        the capture request the facade posted meets the front end while the
 *        read's job is still with the worker, and the job runs after.
 *
 * The AO's queue holds the capture request only, so pump() dispatches it
 * first, busy; the read's job then runs and its outcome ends the read.
 */
static void rfid_busy_while_asleep(void)
{
    pump();
    khal_sem_signal(NULL);
}

TEST_CASE("kode_block_rfid_read is KODE_TIMEOUT when AO_Rfid turns it away busy",
          "[rfid_ao]")
{
    static uint16_t samples[WIN_SAMPLES];
    lent_t * const  cp = &s_lent[0];
    uint8_t         id[5];
    uint8_t         rf = 0U;

    rfid_up();
    stage_window(samples);
    /* An Active Object's capture holds the front end: its outcome is queued,
     * and rfid_busy_while_asleep() takes it out before the read is looked
     * at. The facade learns only that nothing was decoded. */
    capture_req(&cp->got, NULL, 0U, NULL, &cp->rc, khal_sem_create(), 20U, 1U,
                &cp->ack, NULL);
    fake_sem_wait_hook(&rfid_busy_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_rfid_read(id, &rf, 500U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_read_calls(),
        "a read ran under a capture");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_OK, cp->rc,
        "the capture in flight was disturbed");
}

/* ---- RFID_PULL between jobs ------------------------------------------------ */

/* Held low, RFID_PULL sinks about 0.33 mA through R11 for as long as it stays
 * low. It is let go once a job is over and nothing follows it, and never while
 * the receiver is in use. */
TEST_CASE("rfid: a read with nothing to follow lets RFID_PULL go once",
          "[rfid_ao]")
{
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    lent_t * const rd      = &s_lent[0];

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 8U, &rd->ack);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_rest_calls(),
                                  "let go while the read was in flight");
    pump();
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_rest_calls());
    ao_spy_drain();
}

/* A row goes on to its next window with the receiver still in use: one
 * release, at the row's end, not one per window. */
TEST_CASE("rfid row: the windows keep RFID_PULL and the row's end lets it go",
          "[rfid_ao]")
{
    rfid_up();
    test_req();
    pump();
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_rest_calls());
    ao_spy_drain();
}

/* A certification carrier is the receiver in use for as long as it is held;
 * turning it off is the end of that use. */
TEST_CASE("rfid: a certification carrier holds RFID_PULL, and its end lets it go",
          "[rfid_ao]")
{
    lent_t * const ct = &s_lent[1];

    rfid_up();
    ct->rc = 0xFFU;
    rfid_cert_request(1U, &ct->rc, khal_sem_create(), 30U, &ct->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, ct->rc);
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_rest_calls());

    ct->rc = 0xFFU;
    rfid_cert_request(0U, &ct->rc, khal_sem_create(), 31U, &ct->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, ct->rc);
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_rest_calls());
}

/* A carrier that would not come up was the receiver in use for the moment it
 * was tried: the driver drove RFID_PULL low on the way, so the failure lets it
 * go as the carrier's end does. */
TEST_CASE("rfid: a certification carrier that will not come up lets RFID_PULL go",
          "[rfid_ao]")
{
    lent_t * const ct = &s_lent[1];

    rfid_up();
    fake_rfid_set_cert_rc((int)KODE_ERROR);
    ct->rc = 0xFFU;
    rfid_cert_request(1U, &ct->rc, khal_sem_create(), 32U, &ct->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, ct->rc);
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_rest_calls());
    fake_rfid_set_cert_rc((int)KODE_OK);
}

/* ---- Emulating an EM4100 tag ----------------------------------------------
 * RFID_EMU_REQ_SIG submits an EMULATE job and RFID_EMU_STOP_REQ_SIG an EMU_STOP
 * one. The loop the first starts runs on the PARLIO with nobody waiting, so
 * the AO holds the front end for as long as it runs: rfid_is_emulating() is
 * true from the EMULATE job's OK to the EMU_STOP job's end, and every other
 * request is turned away with KODE_DENIED meanwhile.
 */

/** @brief The tag the emulation cases play. */
static const uint8_t s_emu_want[5] = { 0x12U, 0x34U, 0x00U, 0x00U, 0x00U };

/** @brief Start an emulation and carry it through to the job's outcome. */
static void emulate_up(lent_t * const l, uint32_t gen)
{
    rfid_emulate_request(s_emu_want, &l->rc, khal_sem_create(), gen, &l->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, l->rc);
}

/** @brief Stop it again and carry that through. */
static void emulate_down(lent_t * const l, uint32_t gen)
{
    rfid_emulate_stop_request(&l->rc, khal_sem_create(), gen, &l->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, l->rc);
}

TEST_CASE("rfid emulate: a request submits an EMULATE job with the id and the "
          "getter turns true on its OK", "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];
    uint8_t        got[5];
    RfidInfoEvt    s;

    rfid_up();
    TEST_ASSERT_FALSE(rfid_is_emulating());

    rfid_emulate_request(s_emu_want, &em->rc, khal_sem_create(), 11U, &em->ack);
    TEST_ASSERT_FALSE_MESSAGE(rfid_is_emulating(),
        "the getter was true before the job answered");
    pump();

    TEST_ASSERT_TRUE(rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emulate_calls());
    fake_rfid_last_emulate_id(got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_emu_want, got, 5);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, em->rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(11U, em->ack,
        "the answer was not stamped with the request's generation");
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    s = info_now();
    TEST_ASSERT_FALSE_MESSAGE(s.field, "an emulation raised the reader's carrier");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, fake_rfid_rest_calls(),
        "RFID_PULL was let go while the loop drives it");
}

TEST_CASE("rfid emulate: the id travels in the event, not by pointer",
          "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];
    uint8_t        mine[5];
    uint8_t        got[5];

    rfid_up();
    (void)memcpy(mine, s_emu_want, sizeof(mine));
    rfid_emulate_request(mine, &em->rc, khal_sem_create(), 12U, &em->ack);
    (void)memset(mine, 0x5AU, sizeof(mine));   /* the caller moves on */
    pump();

    fake_rfid_last_emulate_id(got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(s_emu_want, got, 5,
        "the job read the caller's array, scribbled on after the request");
}

TEST_CASE("rfid emulate: a NULL id is refused before anything is posted",
          "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];

    rfid_up();
    rfid_emulate_request(NULL, &em->rc, khal_sem_create(), 13U, &em->ack);

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, em->rc);
    TEST_ASSERT_EQUAL_UINT32(13U, em->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Rfid->eQueue));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emulate_calls());
    TEST_ASSERT_FALSE(rfid_is_emulating());
}

TEST_CASE("rfid emulate: every other request is DENIED while the loop runs, "
          "and nothing reaches the driver", "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];
    lent_t * const rd = &s_lent[1];
    lent_t * const ot = &s_lent[2];
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    const QEvt    *e;

    rfid_up();
    emulate_up(em, 20U);
    ao_spy_drain();

    /* A read. */
    fake_rfid_set_window(0, want, 64U);
    rd->have = true;
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 21U, &rd->ack);
    TEST_ASSERT_FALSE(rd->have);
    TEST_ASSERT_EQUAL_UINT32(21U, rd->ack);

    /* A clone. */
    ot->ok = true;
    write_req(s_emu_want, &ot->ok, khal_sem_create(), 500U, 22U, &ot->ack);
    TEST_ASSERT_FALSE(ot->ok);
    TEST_ASSERT_EQUAL_UINT32(22U, ot->ack);

    /* A capture. */
    capture_req(&ot->got, NULL, 0U, NULL, &ot->rc, khal_sem_create(), 20U, 23U,
                &ot->ack, NULL);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ot->rc);
    TEST_ASSERT_EQUAL_UINT32(23U, ot->ack);

    /* A certification carrier. */
    ot->rc = RC_UNWRITTEN;
    rfid_cert_request(1U, &ot->rc, khal_sem_create(), 24U, &ot->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ot->rc);

    /* A second emulation. */
    ot->rc = RC_UNWRITTEN;
    rfid_emulate_request(s_emu_want, &ot->rc, khal_sem_create(), 25U, &ot->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ot->rc);

    /* The production row: a failing verdict, no window run. */
    test_req();
    pump();
    e = spy_expect(TEST_RESULT_SIG);
    TEST_ASSERT_FALSE(((const TestResultEvt *)e)->ok);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_clone_calls());
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_capture_calls());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, fake_rfid_emulate_calls(),
        "a second emulation reached the driver");
    TEST_ASSERT_TRUE_MESSAGE(rfid_is_emulating(),
        "a refused request ended the emulation");
    TEST_ASSERT_TRUE(fake_rfid_emulating());

    emulate_down(em, 26U);
}

TEST_CASE("rfid emulate: a stop submits an EMU_STOP job, the getter is false "
          "after it ends and RFID_PULL is let go", "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];

    rfid_up();
    emulate_up(em, 30U);
    TEST_ASSERT_TRUE(rfid_is_emulating());

    em->rc = RC_UNWRITTEN;
    rfid_emulate_stop_request(&em->rc, khal_sem_create(), 31U, &em->ack);
    TEST_ASSERT_TRUE_MESSAGE(rfid_is_emulating(),
        "the getter fell before the stop job ended");
    pump();

    TEST_ASSERT_FALSE(rfid_is_emulating());
    TEST_ASSERT_FALSE(fake_rfid_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emu_stop_calls());
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, em->rc);
    TEST_ASSERT_EQUAL_UINT32(31U, em->ack);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_rest_calls(),
        "the front end was not handed back to rest after the stop");
}

TEST_CASE("rfid emulate: the front end serves again once the loop is stopped",
          "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];
    lent_t * const rd = &s_lent[1];
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };

    rfid_up();
    emulate_up(em, 40U);
    emulate_down(em, 41U);

    fake_rfid_set_window(0, want, 64U);
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 42U, &rd->ack);
    pump();
    TEST_ASSERT_TRUE(rd->have);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(want, rd->id, 5);
    ao_spy_drain();
}

TEST_CASE("rfid emulate: a stop with nothing running is DENIED and submits "
          "nothing", "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];

    rfid_up();
    em->rc = RC_UNWRITTEN;
    rfid_emulate_stop_request(&em->rc, khal_sem_create(), 50U, &em->ack);
    pump();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, em->rc);
    TEST_ASSERT_EQUAL_UINT32(50U, em->ack);
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());
    TEST_ASSERT_FALSE(rfid_is_emulating());
}

TEST_CASE("rfid emulate: an EMULATE job that fails leaves the getter false and "
          "the front end free", "[rfid_ao]")
{
    static const uint32_t FAILS[3] = {
        (uint32_t)KODE_NO_RESOURCES, (uint32_t)KODE_ERROR, (uint32_t)KODE_DENIED
    };
    lent_t * const em = &s_lent[0];
    lent_t * const rd = &s_lent[1];
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    uint32_t       i;

    rfid_up();
    for ( i = 0U; 3U > i; ++i )
    {
        em->rc = RC_UNWRITTEN;
        fake_rfid_set_emulate_rc(FAILS[i]);
        rfid_emulate_request(s_emu_want, &em->rc, khal_sem_create(), 60U + i,
                             &em->ack);
        pump();
        TEST_ASSERT_EQUAL_UINT32(FAILS[i], em->rc);
        TEST_ASSERT_EQUAL_UINT32(60U + i, em->ack);
        TEST_ASSERT_FALSE(rfid_is_emulating());
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(i + 1U, (uint32_t)fake_rfid_rest_calls(),
            "a failed start did not hand the front end back to rest");
    }

    fake_rfid_set_window(0, want, 64U);
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 70U, &rd->ack);
    pump();
    TEST_ASSERT_TRUE_MESSAGE(rd->have, "a read after a failed start was refused");
    ao_spy_drain();
}

TEST_CASE("rfid emulate: a stop that arrives before the start has answered is "
          "DENIED", "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];
    lent_t * const st = &s_lent[1];

    rfid_up();
    rfid_emulate_request(s_emu_want, &em->rc, khal_sem_create(), 80U, &em->ack);
    st->rc = RC_UNWRITTEN;
    rfid_emulate_stop_request(&st->rc, khal_sem_create(), 81U, &st->ack);
    (void)qf_support_settle(AO_Rfid);   /* both dispatched, no job has run */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, st->rc);
    pump();

    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, em->rc);
    TEST_ASSERT_TRUE(rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());
    emulate_down(em, 82U);
}

TEST_CASE("rfid emulate: a deadline that answers first is KODE_TIMEOUT, and the "
          "loop the worker started late is still known", "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];
    lent_t * const rd = &s_lent[1];
    const QEvt    *late;

    rfid_up();
    em->rc = RC_UNWRITTEN;
    rfid_emulate_request(s_emu_want, &em->rc, khal_sem_create(), 90U, &em->ack);
    (void)qf_support_settle(AO_Rfid);   /* the request is dispatched, the job queued */
    late = hold_outcome();
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_TIMEOUT, em->rc);
    TEST_ASSERT_EQUAL_UINT32(90U, em->ack);

    /* The worker reports at last: the loop runs, and the getter says so. */
    deliver(late);
    TEST_ASSERT_TRUE_MESSAGE(rfid_is_emulating(),
        "a loop started behind a timed-out request was forgotten");

    rd->have = true;
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 91U, &rd->ack);
    TEST_ASSERT_FALSE_MESSAGE(rd->have, "a read ran under the loop");
    emulate_down(em, 92U);
    TEST_ASSERT_FALSE(rfid_is_emulating());
}

TEST_CASE("rfid emulate: a stop whose deadline fires first is KODE_TIMEOUT, "
          "and its late outcome ends the loop and frees the front end",
          "[rfid_ao]")
{
    lent_t * const em = &s_lent[0];
    lent_t * const rd = &s_lent[1];
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    const QEvt    *late;

    rfid_up();
    emulate_up(em, 100U);

    em->rc = RC_UNWRITTEN;
    rfid_emulate_stop_request(&em->rc, khal_sem_create(), 101U, &em->ack);
    (void)qf_support_settle(AO_Rfid);
    late = hold_outcome();
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_TIMEOUT, em->rc);
    TEST_ASSERT_EQUAL_UINT32(101U, em->ack);
    TEST_ASSERT_TRUE_MESSAGE(rfid_is_emulating(),
        "the getter fell before the stop job's outcome arrived");

    /* The worker reports at last: the loop is over and the front end free. */
    deliver(late);
    TEST_ASSERT_FALSE(rfid_is_emulating());
    TEST_ASSERT_FALSE(fake_rfid_emulating());
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_rest_calls());

    fake_rfid_set_window(0, want, 64U);
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 102U, &rd->ack);
    pump();
    TEST_ASSERT_TRUE_MESSAGE(rd->have, "a read after the late stop was refused");
    ao_spy_drain();
}

/* ---- The facade: kode_sys_rfid_emulate_* ---------------------------------- */

/** @brief The status a second, concurrent facade call got. */
static kode_status_t s_emu_second;

/** @brief Ask again from inside the wait; nothing answers either call. */
static void rfid_emulate_again(void)
{
    s_emu_second = kode_block_rfid_emulate_start(s_emu_want);
}

/** @brief The caller's array, scribbled on while the call waits. */
static uint8_t s_emu_caller[5];

/** @brief Change the caller's array, then let AO_Rfid run. */
static void rfid_runs_after_emu_caller_scribbles(void)
{
    (void)memset(s_emu_caller, 0x5AU, sizeof(s_emu_caller));
    pump();
    khal_sem_signal(NULL);
}

TEST_CASE("kode_block_rfid_emulate_start is KODE_OK with the loop running, stop "
          "ends it", "[rfid_ao]")
{
    uint8_t got[5];

    rfid_up();
    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_start(s_emu_want));
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emulate_calls());
    fake_rfid_last_emulate_id(got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_emu_want, got, 5);

    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_stop());
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emu_stop_calls());
    ao_spy_drain();
}

TEST_CASE("kode_block_rfid_emulate_start lends no pointer of the caller's",
          "[rfid_ao]")
{
    uint8_t got[5];

    rfid_up();
    (void)memcpy(s_emu_caller, s_emu_want, sizeof(s_emu_caller));
    fake_sem_wait_hook(&rfid_runs_after_emu_caller_scribbles);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_start(s_emu_caller));
    fake_rfid_last_emulate_id(got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(s_emu_want, got, 5,
        "the caller's array, changed while the call waited, was what got played");
    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_stop());
}

TEST_CASE("kode_block_rfid_emulate_start rejects a NULL id before posting",
          "[rfid_ao]")
{
    rfid_up();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_rfid_emulate_start(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Rfid->eQueue));
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emulate_calls());
    TEST_ASSERT_EQUAL_UINT(0U, fake_sem_signals());
}

TEST_CASE("kode_block_rfid_emulate_start is KODE_DENIED while the front end is "
          "held", "[rfid_ao]")
{
    rfid_up();
    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_start(s_emu_want));

    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_rfid_emulate_start(s_emu_want));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_rfid_emulate_calls());
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_stop());
}

TEST_CASE("kode_block_rfid_emulate_start is KODE_DENIED while another RFID call "
          "waits", "[rfid_ao]")
{
    const QEvt *e = NULL;

    rfid_up();
    s_emu_second = KODE_OK;
    fake_sem_wait_hook(&rfid_emulate_again);

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_rfid_emulate_start(s_emu_want));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, s_emu_second,
                                  "the second caller is turned away");

    /* Leave AO_Rfid idle: the first request is withdrawn unserved. */
    while ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

TEST_CASE("kode_block_rfid_emulate_stop is KODE_DENIED when none was emulating",
          "[rfid_ao]")
{
    rfid_up();
    fake_sem_wait_hook(&rfid_runs_while_asleep);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_rfid_emulate_stop());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());
    ao_spy_drain();
}

TEST_CASE("kode_block_rfid_emulate_start reports a start the driver refused",
          "[rfid_ao]")
{
    rfid_up();
    fake_sem_wait_hook(&rfid_runs_while_asleep);
    fake_rfid_set_emulate_rc((uint32_t)KODE_NO_RESOURCES);

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_block_rfid_emulate_start(s_emu_want));
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    ao_spy_drain();
}

TEST_CASE("kode_block_rfid_read is KODE_TIMEOUT while the loop holds the front "
          "end", "[rfid_ao]")
{
    uint8_t id[5];
    uint8_t rf = 0U;

    rfid_up();
    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_start(s_emu_want));

    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_rfid_read(id, &rf, 500U));
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());
    fake_sem_wait_hook(&rfid_runs_while_asleep);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_rfid_emulate_stop());
    ao_spy_drain();
}

/* ---- kode_sys's requests and the certification ones: answered to the asking AO ---
 * They post and return; the answer is an event posted to the AO that asked, the
 * spy here. An ID travels by value. The one buffer an ask lends is a capture's
 * waveform: the worker writes it in place, so AO_Rfid answers only once the
 * worker has let go of it - at its deadline too - and never writes it after.
 */

/** @brief A static event AO_Rfid ignores, to fill its queue with. */
static const QEvt s_rfid_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief Most blocks a case takes out of a pool to leave it dry. */
#define DRY_MAX  128U

/** @brief The next thing the spy holds, which must be @p sig. */
static const void *ask_take(enum_t sig)
{
    return (const void *)spy_expect(sig);
}

/** @brief AO_Rfid up, its queue one a margin post can read, both askers' mailboxes empty. */
static void ask_up(void)
{
    rfid_up();
    qf_support_margin_queue(AO_Rfid);
    ao_spy_drain();
    ao_spy_other_drain();
}

/** @brief Leave nothing owed behind a case. */
static void ask_down(void)
{
    ao_spy_drain();
    ao_spy_other_drain();
}

/** @brief The ID every ask below reads, clones or plays. */
static const uint8_t s_ask_id[KODE_SYS_RFID_ID_BYTES] = { 0x12U, 0x34U, 0x56U, 0x78U, 0x90U };

TEST_CASE("kode_sys_rfid_read answers the asking AO once, with the id and the bit rate",
          "[rfid_ao][ask]")
{
    const kode_sys_rfid_answer_t *a;

    ask_up();
    fake_rfid_set_window(0, s_ask_id, 64U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_rfid_read(ao_spy(), 500U),
                                  "a second request of the kind waits for the first's answer");
    pump();

    (void)spy_expect(RFID_TAG_SIG);   /* the decode is published as ever, apart from the answer */
    a = (const kode_sys_rfid_answer_t *)ask_take(RFID_OP_RSP_SIG);
    TEST_ASSERT_FALSE(a->write);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_ask_id, a->id, KODE_SYS_RFID_ID_BYTES);
    TEST_ASSERT_EQUAL_UINT8(64U, a->rf_div);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once");
    ao_spy_release();

    fake_rfid_set_window(1, NULL, 0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U),
                                  "the answer opens the door again");
    pump();
    a = (const kode_sys_rfid_answer_t *)ask_take(RFID_OP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "an empty window publishes nothing");
    ask_down();
}

TEST_CASE("kode_sys_rfid_write clones a copy of the id and answers whether the read-back matched",
          "[rfid_ao][ask]")
{
    uint8_t                       mine[KODE_SYS_RFID_ID_BYTES];
    uint8_t                       written[KODE_SYS_RFID_ID_BYTES];
    const kode_sys_rfid_answer_t *a;

    ask_up();
    (void)memcpy(mine, s_ask_id, sizeof(mine));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_write(ao_spy(), mine, 500U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_rfid_read(ao_spy(), 500U),
                                  "a read and a clone share the door");
    (void)memset(mine, 0x5AU, sizeof(mine));   /* the caller moves on */
    pump();
    fake_rfid_last_clone_id(written);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(s_ask_id, written, KODE_SYS_RFID_ID_BYTES,
                                          "the caller's array, changed after the request, was written");
    TEST_ASSERT_TRUE(fake_rfid_last_boost());
    a = (const kode_sys_rfid_answer_t *)ask_take(RFID_OP_RSP_SIG);
    TEST_ASSERT_TRUE(a->write);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_ask_id, a->id, KODE_SYS_RFID_ID_BYTES);
    ao_spy_release();

    fake_rfid_set_clone_mismatch(true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_write(ao_spy(), s_ask_id, 500U));
    pump();
    a = (const kode_sys_rfid_answer_t *)ask_take(RFID_OP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KODE_TIMEOUT, a->rc, "the read-back differed");
    ask_down();
}

TEST_CASE("two AOs reading are both answered: the busy front end refuses the second",
          "[rfid_ao][ask]")
{
    const kode_sys_rfid_answer_t *a;
    const QEvt                   *o;

    ask_up();
    fake_rfid_set_window(0, s_ask_id, 64U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_read(ao_spy_other(), 500U),
                                  "the door is the asker's, not the kind's");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_read(ao_spy_other(), 500U));
    pump();

    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered too");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)RFID_OP_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ((const kode_sys_rfid_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_NULL(ao_spy_other_take());
    (void)spy_expect(RFID_TAG_SIG);
    a = (const kode_sys_rfid_answer_t *)ask_take(RFID_OP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ask_down();
}

TEST_CASE("a read ask retired by its deadline is answered KODE_TIMEOUT once",
          "[rfid_ao][ask]")
{
    const kode_sys_rfid_answer_t *a;
    const QEvt                   *late;

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U));
    (void)qf_support_settle(AO_Rfid);
    late = hold_outcome();   /* the worker is still in its window */
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    a = (const kode_sys_rfid_answer_t *)ask_take(RFID_OP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_TIMEOUT, a->rc);
    ao_spy_release();

    deliver(late);   /* the worker reports at last, and answers nobody a second time */
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U), "the door was open again");
    pump();
    (void)ask_take(RFID_OP_RSP_SIG);
    ask_down();
}

TEST_CASE("kode_sys_rfid_emulate_start plays a copy of the id and _stop ends it, each answered",
          "[rfid_ao][ask]")
{
    uint8_t                           mine[KODE_SYS_RFID_ID_BYTES];
    uint8_t                           got[KODE_SYS_RFID_ID_BYTES];
    const kode_sys_rfid_emu_answer_t *a;

    ask_up();
    (void)memcpy(mine, s_ask_id, sizeof(mine));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_start(ao_spy(), mine));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_rfid_emulate_stop(ao_spy()),
                                  "one emulation request at a time");
    (void)memset(mine, 0x5AU, sizeof(mine));   /* the caller moves on */
    pump();
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_FALSE(a->stop);
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    fake_rfid_last_emulate_id(got);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(s_ask_id, got, KODE_SYS_RFID_ID_BYTES);
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_emulate_stop(ao_spy()),
                                  "the answer opens the door again");
    pump();
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->stop);
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ask_down();
}

TEST_CASE("an emulation stop from another asker is refused and the opener's tag keeps playing",
          "[rfid_ao][ask]")
{
    lent_t * const                    wt = &s_lent[0];
    const kode_sys_rfid_emu_answer_t *a;
    const QEvt                       *o;

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_start(ao_spy(), s_ask_id));
    pump();
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    ao_spy_release();

    /* Another AO, then a waiting task: neither opened it, so neither ends it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_stop(ao_spy_other()));
    pump();
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the foreign stop is answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)RFID_EMU_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED,
                           ((const kode_sys_rfid_emu_answer_t *)(const void *)o)->rc);
    TEST_ASSERT_TRUE(((const kode_sys_rfid_emu_answer_t *)(const void *)o)->stop);
    QF_gc(o);
    wt->rc = RC_UNWRITTEN;
    rfid_emulate_stop_request(&wt->rc, khal_sem_create(), 120U, &wt->ack);
    pump();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, wt->rc);
    TEST_ASSERT_TRUE_MESSAGE(kode_sys_rfid_is_emulating(), "a foreign stop ended the opener's tag");
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());

    /* The opener's own stop ends it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_stop(ao_spy()));
    pump();
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(a->stop);
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    ask_down();
}

TEST_CASE("an emulation ask whose deadline passes with the worker out is answered with what the job came to",
          "[rfid_ao][ask]")
{
    const kode_sys_rfid_emu_answer_t *a;
    const QEvt                       *late;

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_start(ao_spy(), s_ask_id));
    (void)qf_support_settle(AO_Rfid);
    late = hold_outcome();   /* the worker is still bringing the loop up */
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "answered KODE_TIMEOUT while the tag may still start");

    deliver(late);   /* the loop is up after all */
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_FALSE(a->stop);
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered once");
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_stop(ao_spy()));
    pump();
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_OK, a->rc, "the opener ends the tag it was told plays");
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    ask_down();
}

TEST_CASE("emulation asks are answered with the status: a refusing driver, nothing playing, a held front end",
          "[rfid_ao][ask]")
{
    const kode_sys_rfid_emu_answer_t *a;

    ask_up();
    fake_rfid_set_emulate_rc((uint32_t)KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_start(ao_spy(), s_ask_id));
    pump();
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_ERROR, a->rc);
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    ao_spy_release();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_stop(ao_spy()));
    pump();
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_DENIED, a->rc, "nothing was playing");
    TEST_ASSERT_TRUE(a->stop);
    ao_spy_release();

    /* A read in flight holds the front end: the start is refused, answered. */
    fake_rfid_set_emulate_rc((uint32_t)KODE_OK);
    fake_rfid_set_window(0, s_ask_id, 64U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_read(ao_spy_other(), 500U));
    (void)qf_support_settle(AO_Rfid);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_start(ao_spy(), s_ask_id));
    (void)qf_support_settle(AO_Rfid);
    a = (const kode_sys_rfid_emu_answer_t *)ask_take(RFID_EMU_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, a->rc);
    TEST_ASSERT_FALSE(a->stop);
    pump();
    ask_down();
}

TEST_CASE("the certification capture fills the lent waveform, then answers once, and never writes it again",
          "[rfid_ao][ask]")
{
    static uint16_t                      samples[WIN_SAMPLES];
    static uint16_t                      want_wave[WAVE_POINTS];
    lent_t * const                       cp = &s_lent[0];
    krfid_field_t                        want;
    size_t                               want_n;
    uint16_t                             after[WAVE_POINTS];
    const kode_cert_rfid_field_answer_t *r;

    ask_up();
    stage_window(samples);
    krfid_wave_stats(samples, (size_t)WIN_SAMPLES, &want);
    want_n = krfid_wave_decimate(samples, (size_t)WIN_SAMPLES, want_wave, (size_t)WAVE_POINTS);
    paint(&cp->got, cp->wave, (size_t)WAVE_POINTS, &cp->n);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_rfid_field_read(ao_spy(), cp->wave, (size_t)WAVE_POINTS, 20U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED,
        kode_cert_rfid_field_read(ao_spy(), cp->wave, (size_t)WAVE_POINTS, 20U),
        "a second window of the asker's waits for the first's answer");
    TEST_ASSERT_TRUE_MESSAGE(wave_untouched(cp->wave, (size_t)WAVE_POINTS), "written before the worker ran");
    pump();

    r = (const kode_cert_rfid_field_answer_t *)ask_take(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    TEST_ASSERT_EQUAL_UINT16(want.min, r->min);
    TEST_ASSERT_EQUAL_UINT16(want.mean, r->mean);
    TEST_ASSERT_EQUAL_UINT16(want.peak, r->peak);
    TEST_ASSERT_EQUAL_UINT16(want.ac, r->ac);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)WIN_SAMPLES, r->n);
    TEST_ASSERT_EQUAL_UINT((uint32_t)want_n, (uint32_t)r->points);
    TEST_ASSERT_EQUAL_UINT16_ARRAY(want_wave, cp->wave, want_n);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_release();

    /* A later window by somebody else is not written into the answered buffer. */
    (void)memcpy(after, cp->wave, sizeof(after));
    capture_req(NULL, NULL, 0U, NULL, NULL, NULL, 20U, 0U, NULL, ao_spy());
    pump();
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT16_ARRAY_MESSAGE(after, cp->wave, WAVE_POINTS, "the answered wave was written again");

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_cert_rfid_field_read(ao_spy(), NULL, 0U, 20U),
                                  "statistics only, and the answer opened the door");
    pump();
    r = (const kode_cert_rfid_field_answer_t *)ask_take(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, r->st);
    TEST_ASSERT_EQUAL_UINT(0U, (uint32_t)r->points);
    ask_down();
}

TEST_CASE("a capture ask whose deadline passes with the worker in the window is answered only when it returns",
          "[rfid_ao][ask]")
{
    static uint16_t                      samples[WIN_SAMPLES];
    lent_t * const                       cp = &s_lent[0];
    const QEvt                          *late;
    const kode_cert_rfid_field_answer_t *r;

    ask_up();
    stage_window(samples);
    paint(&cp->got, cp->wave, (size_t)WAVE_POINTS, &cp->n);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_rfid_field_read(ao_spy(), cp->wave, (size_t)WAVE_POINTS, 20U));
    (void)qf_support_settle(AO_Rfid);
    late = hold_outcome();   /* the worker is inside its window, with the lent wave */
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "answered while the worker still holds the lent wave");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_cert_rfid_field_read(ao_spy(), NULL, 0U, 20U),
                                  "the asker still waits for its answer");

    deliver(late);   /* the worker lets go of it at last */
    r = (const kode_cert_rfid_field_answer_t *)ask_take(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_TIMEOUT, r->st);
    TEST_ASSERT_EQUAL_UINT(0U, (uint32_t)r->points);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered once");
    ask_down();
}

TEST_CASE("a capture ask is answered with the status and leaves the lent waveform alone when it is refused or fails",
          "[rfid_ao][ask]")
{
    lent_t * const                       cp = &s_lent[0];
    uint8_t                              want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    const kode_cert_rfid_field_answer_t *r;

    ask_up();
    paint(&cp->got, cp->wave, (size_t)WAVE_POINTS, &cp->n);

    /* The front end is held by a read: the window is refused, answered. */
    fake_rfid_set_window(0, want, 64U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_read(ao_spy_other(), 500U));
    (void)qf_support_settle(AO_Rfid);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_rfid_field_read(ao_spy(), cp->wave, (size_t)WAVE_POINTS, 20U));
    (void)qf_support_settle(AO_Rfid);
    r = (const kode_cert_rfid_field_answer_t *)ask_take(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, r->st);
    expect_empty((const RfidFieldRspEvt *)r);
    TEST_ASSERT_TRUE_MESSAGE(wave_untouched(cp->wave, (size_t)WAVE_POINTS), "a refused window wrote the wave");
    ao_spy_release();
    pump();
    ao_spy_drain();
    ao_spy_other_drain();

    /* The ADC delivered nothing: the window failed. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_rfid_field_read(ao_spy(), cp->wave, (size_t)WAVE_POINTS, 20U));
    pump();
    r = (const kode_cert_rfid_field_answer_t *)ask_take(RFID_FIELD_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_ERROR, r->st);
    expect_empty((const RfidFieldRspEvt *)r);
    TEST_ASSERT_TRUE_MESSAGE(wave_untouched(cp->wave, (size_t)WAVE_POINTS), "a failed window wrote the wave");
    ask_down();
}

TEST_CASE("the certification carrier is commanded and dropped, and every refusal is an answer",
          "[rfid_ao][ask]")
{
    const kode_cert_carrier_answer_t *a;

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_rfid_carrier(ao_spy(), KODE_CERT_RFID_CARRIER_ON));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_carrier(ao_spy(), KODE_CERT_RFID_CARRIER_OFF));
    (void)qf_support_settle(AO_Rfid);
    a = (const kode_cert_carrier_answer_t *)ask_take(RFID_CERT_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE_MESSAGE(info_now().field, "the carrier was not raised");
    ao_spy_release();

    /* Another request now finds the front end held. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_read(ao_spy_other(), 500U));
    (void)qf_support_settle(AO_Rfid);
    {
        const QEvt *o = ao_spy_other_take();

        TEST_ASSERT_NOT_NULL(o);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, ((const kode_sys_rfid_answer_t *)(const void *)o)->rc);
        QF_gc(o);
    }

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_rfid_carrier(ao_spy(), KODE_CERT_RFID_CARRIER_OFF));
    (void)qf_support_settle(AO_Rfid);
    a = (const kode_cert_carrier_answer_t *)ask_take(RFID_CERT_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_FALSE(info_now().field);
    ao_spy_release();

    fake_rfid_set_cert_rc((int)KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_cert_rfid_carrier(ao_spy(), KODE_CERT_RFID_CARRIER_MOD));
    (void)qf_support_settle(AO_Rfid);
    a = (const kode_cert_carrier_answer_t *)ask_take(RFID_CERT_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_ERROR, a->rc, "a carrier the driver refused");
    ask_down();
}

TEST_CASE("a plain task's or a badly asked RFID request is refused at once and posts nothing",
          "[rfid_ao][ask]")
{
    uint16_t wave[4];

    ask_up();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_read(NULL, 500U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_read(ao_spy(), 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_write(NULL, s_ask_id, 500U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_write(ao_spy(), NULL, 500U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_write(ao_spy(), s_ask_id, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_emulate_start(NULL, s_ask_id));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_emulate_start(ao_spy(), NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_rfid_emulate_stop(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_field_read(NULL, NULL, 0U, 20U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_field_read(ao_spy(), NULL, 0U, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_field_read(ao_spy(), NULL, 0U,
                                                                 KODE_CERT_RFID_FIELD_MS_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_field_read(ao_spy(), wave, 0U, 20U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_carrier(NULL, KODE_CERT_RFID_CARRIER_ON));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_carrier(ao_spy(), 3));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_cert_rfid_carrier(ao_spy(), -1));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (uint32_t)uxQueueMessagesWaiting(AO_Rfid->eQueue),
                                   "a refused request posted anyway");

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U), "a refusal takes no door");
    pump();
    (void)ask_take(RFID_OP_RSP_SIG);
    ask_down();
}

TEST_CASE("a full AO_Rfid queue refuses kode_sys's requests with no resources",
          "[rfid_ao][ask]")
{
    const QEvt *e;

    ask_up();
    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Rfid, &s_rfid_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_rfid_read(ao_spy(), 500U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_rfid_write(ao_spy(), s_ask_id, 500U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_rfid_emulate_start(ao_spy(), s_ask_id));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_cert_rfid_field_read(ao_spy(), NULL, 0U, 20U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_cert_rfid_carrier(ao_spy(), KODE_CERT_RFID_CARRIER_ON));
    while ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }

    fake_rfid_set_window(1, NULL, 0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U),
                                  "nothing was posted, so the door is open");
    pump();
    (void)ask_take(RFID_OP_RSP_SIG);
    ask_down();
}

TEST_CASE("a dry pool refuses kode_sys's RFID request with no resources and posts nothing",
          "[rfid_ao][ask]")
{
    static RfidOpRspEvt *held[DRY_MAX];
    uint32_t             n = 0U;

    ask_up();
    while ( DRY_MAX > n )
    {
        held[n] = Q_NEW_X(RfidOpRspEvt, 0U, RFID_OP_RSP_SIG);
        if ( NULL == held[n] )
        {
            break;
        }
        n++;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(DRY_MAX, n, "the pool never ran dry");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_rfid_read(ao_spy(), 500U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Rfid->eQueue));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        QF_gc(&held[i]->super);
    }

    fake_rfid_set_window(1, NULL, 0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U),
                                  "nothing was posted, so the door is open");
    pump();
    (void)ask_take(RFID_OP_RSP_SIG);
    ask_down();
}

TEST_CASE("an RFID ask whose request finds its pool dry gives the answer back and posts nothing",
          "[rfid_ao][ask]")
{
    static qf_support_dry_t dry;
    uint16_t                before;

    ask_up();
    before = qf_support_pool_dry_second(&dry, (uint16_t)sizeof(RfidReqEvt), (uint16_t)sizeof(RfidOpRspEvt));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(0U, before, "the pools could not be set up");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_rfid_read(ao_spy(), 500U));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(before, qf_support_pool_free(&dry), "the answer went back to its pool");
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Rfid->eQueue));
    qf_support_pool_give_back(&dry);

    fake_rfid_set_window(1, NULL, 0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_rfid_read(ao_spy(), 500U),
                                  "nothing was posted, so the door is open");
    pump();
    (void)ask_take(RFID_OP_RSP_SIG);
    ask_down();
}

/** @brief The body of the full-queue case, run in a child: the asker's queue is full, then AO_Rfid answers into it. */
static void full_asker_body(void *arg)
{
    (void)arg;
    (void)qf_support_queue_stuff(ao_spy_other(), &s_rfid_filler);
    (void)kode_sys_rfid_emulate_stop(ao_spy_other());   /* nothing plays: refused, answered */
    pump();
}

TEST_CASE("an answer into an asker's full queue is QP's assert, not a drop",
          "[rfid_ao][ask]")
{
    ask_up();

    /* QP asserts with Q_onError(), which aborts this binary, so the post runs
     * in a child and the parent reads how it died. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)ASK_CHILD_ABORTED,
                                    (uint32_t)ask_support_fork_run(&full_asker_body, NULL),
                                  "the child was not stopped by QP's assert");
    ask_down();
}

TEST_CASE("an app AO's stop of a tag a waiting task opened is refused, and the task's ends it",
          "[rfid_ao][ask]")
{
    lent_t * const wt = &s_lent[0];
    const QEvt    *o;

    ask_up();
    emulate_up(wt, 31U);   /* a waiting task's: no asking AO opened it */
    TEST_ASSERT_TRUE(kode_sys_rfid_is_emulating());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_rfid_emulate_stop(ao_spy_other()));
    pump();
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the app's stop is answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)RFID_EMU_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED,
                           ((const kode_sys_rfid_emu_answer_t *)(const void *)o)->rc);
    QF_gc(o);
    TEST_ASSERT_TRUE_MESSAGE(kode_sys_rfid_is_emulating(), "an app ended the task's tag");
    TEST_ASSERT_EQUAL_UINT32(0U, fake_rfid_emu_stop_calls());

    emulate_down(wt, 32U);
    TEST_ASSERT_FALSE(kode_sys_rfid_is_emulating());
    ask_down();
}

/* ---- The board's rest ------------------------------------------------------- */

TEST_CASE("rfid: a round lets the receiver's pull go and answers once", "[rfid_ao][lowpower]")
{
    rfid_up();

    lp_round(AO_Rfid, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_rest_calls(), "the front end was not rested");
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    lp_round(AO_Rfid, KODE_LP_OFF);
    TEST_ASSERT_EQUAL_INT(2, fake_rfid_rest_calls());
    lp_expect_answer(false, NULL);
}

/* Nothing is restored on AWAKE: every way into the receiver drives the pull
 * itself. */
TEST_CASE("rfid: after the rest AWAKE does nothing and the next read runs",
          "[rfid_ao][lowpower]")
{
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    lent_t * const rd      = &s_lent[0];

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    lp_round(AO_Rfid, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    lp_awake(AO_Rfid);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_rest_calls(), "AWAKE touched the front end");

    rd->have = false;
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 7U, &rd->ack);
    pump();
    TEST_ASSERT_TRUE_MESSAGE(rd->have, "the read after the rest did not run");
}

/* A job with the worker is a field somebody is reading. */
TEST_CASE("rfid: a read in flight refuses the round and is not cut", "[rfid_ao][lowpower]")
{
    uint8_t        want[5] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    lent_t * const rd      = &s_lent[0];

    rfid_up();
    fake_rfid_set_window(0, want, 64U);
    rd->have = false;
    read_req(rd->id, &rd->rf, &rd->have, khal_sem_create(), 500U, 7U, &rd->ack);

    lp_round(AO_Rfid, KODE_LP_ASLEEP);
    lp_expect_answer(true, "rfid busy");
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_rest_calls());

    pump();
    TEST_ASSERT_TRUE(rd->have);

    lp_round(AO_Rfid, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
}
