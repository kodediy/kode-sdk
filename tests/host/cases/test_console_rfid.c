/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_rfid.c
 * @brief `rfid` and its `rfid_*` verbs: the 125 kHz LF front end, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fake_kdrv_rfid_lf.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "krfid_wave.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks - repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

static void console_up(void)
{
    const QEvt *e;

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    owners_up();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);

    /* AO_Rfid's queue is the mailbox `rfid` posts into; emptied here so a
     * request left by an earlier case cannot be mistaken for this one's. */
    qf_support_give_queue(AO_Rfid);
    while ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    fake_klog_reset();
    fake_neighbours_reset();
    fake_time_reset();
}

static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

static bool replied(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Take the status request the console posted to AO_Rfid, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted_info(RfidInfoReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(RFID_INFO_REQ_SIG, e->sig);
    *out = *(const RfidInfoReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Rfid's status answer, the way info_serve() posts it. */
static void answer_info(bool field, bool have, const uint8_t id[5],
                        uint8_t rf_div, uint32_t age_ms, uint32_t reads,
                        uint32_t writes, uint32_t windows)
{
    RfidInfoEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super   = (QEvt)QEVT_INITIALIZER(RFID_INFO_RSP_SIG);
    a.field   = field;
    a.have    = have;
    if ( NULL != id )
    {
        (void)memcpy(a.id, id, sizeof(a.id));
    }
    a.rf_div  = rf_div;
    a.age_ms  = age_ms;
    a.reads   = reads;
    a.writes  = writes;
    a.windows = windows;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- rfid ----------------------------------------------------------------- */

TEST_CASE("rfid asks AO_Rfid and prints its status once answered",
          "[console_rfid]")
{
    RfidInfoReqEvt q;
    uint8_t        id[5] = { 0x12U, 0x34U, 0x56U, 0x78U, 0x90U };

    console_up();
    type_line("rfid");

    TEST_ASSERT_TRUE(posted_info(&q));
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.who,
                                  "the answer must come back here");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Rfid answered");

    answer_info(true, true, id, 64U, 1234U, 3U, 1U, 5U);

    TEST_ASSERT_TRUE(replied(">|rfid=field up=1"));
    TEST_ASSERT_TRUE(replied(">|rfid=last state=seen id=\"12:34:56:78:90\" rf=64 "
                             "age_ms=1234"));
    TEST_ASSERT_TRUE(replied(">|rfid=counts reads=3 writes=1 windows=5"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* Every hardware status verb in this console answers whatever state it finds,
 * the field never having come up included - a board with no Antenna PCB reads
 * flat windows and remembers no tag, and `rfid` must still get a clean answer.
 * This is also the "prints a snapshot line in the idle state" case. */
TEST_CASE("rfid prints a clean snapshot when nothing has been read",
          "[console_rfid]")
{
    RfidInfoReqEvt q;

    console_up();
    type_line("rfid");
    TEST_ASSERT_TRUE(posted_info(&q));

    answer_info(false, false, NULL, 0U, 0U, 0U, 0U, 0U);

    TEST_ASSERT_TRUE(replied(">|rfid=field up=0"));
    TEST_ASSERT_TRUE(replied(">|rfid=last state=none"));
    TEST_ASSERT_TRUE(replied(">|rfid=counts reads=0 writes=0 windows=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE_MESSAGE(replied("state=seen"),
                              "an id was printed with nothing decoded");
}

TEST_CASE("another chip's answer does not close an rfid question",
          "[console_rfid]")
{
    RfidInfoReqEvt q;
    IrInfoEvt      stray;

    console_up();
    type_line("rfid");
    TEST_ASSERT_TRUE(posted_info(&q));

    (void)memset(&stray, 0, sizeof(stray));
    stray.super = (QEvt)QEVT_INITIALIZER(IR_INFO_RSP_SIG);
    QASM_DISPATCH(AO_Console, &stray.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the infrared answer closed it");

    answer_info(false, false, NULL, 0U, 0U, 0U, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- rfid_help ------------------------------------------------------------ */

TEST_CASE("rfid_help lists every field and closes with ok, in one pass",
          "[console_rfid]")
{
    console_up();
    type_line("rfid_help");

    TEST_ASSERT_TRUE(replied(">|field=\"field\" desc="));
    TEST_ASSERT_TRUE(replied(">|field=\"last\" desc="));
    TEST_ASSERT_TRUE(replied(">|field=\"counts\" desc="));
    /* Three rows sit well under one KCLI_SLICE, so like ir_help this answers
     * whole in the same run-to-completion step - no pump needed. */
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- the four verbs are in the table -------------------------------------- */

/* `rfid` and `rfid_help` reach their handlers above. These two prove
 * `rfid_read` and `rfid_write` are known verbs that dispatch - a bad value on
 * each is answered by the gate in front of the handler, never a `code=1`
 * unknown-verb - so all four are in the command table without a real read or
 * clone being driven (which would open a window nothing here would close). */
TEST_CASE("rfid_read and rfid_write are known verbs, not unknown ones",
          "[console_rfid]")
{
    console_up();

    /* A window below its floor is refused by the argument bounds, before the
     * handler ever energises anything - and it is code=3, not code=1. */
    type_line("rfid_read ms=99");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=1"),
                              "rfid_read was not found in the table");

    /* A clone with no id= is a missing required key, named. */
    fake_klog_reset();
    type_line("rfid_write");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=id"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=1"),
                              "rfid_write was not found in the table");
}

/* ---- rfid_write id= parsing ----------------------------------------------- */

/* The id is ten hex digits, five bytes. The handler parses it - a KARG_STRING
 * only proves a value is present - so a value that is not ten hex digits is
 * refused there, naming the key, and no clone is ever submitted. */
TEST_CASE("rfid_write id= rejects a value that is not ten hex digits",
          "[console_rfid]")
{
    console_up();

    /* Not hex at all. */
    type_line("rfid_write id=\"nothexdig\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=id"));

    /* Too few digits. */
    fake_klog_reset();
    type_line("rfid_write id=\"AB:CD:EF:01\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=id"));

    /* Too many digits. */
    fake_klog_reset();
    type_line("rfid_write id=\"0123456789AB\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=id"));

    /* A digit that is neither hex nor a colon. */
    fake_klog_reset();
    type_line("rfid_write id=\"12:34:56:78:9Z\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=id"));
}

/* ---- rfid_field ----------------------------------------------------------- */

/**
 * @brief QP ticks that outlast every deadline AO_Rfid can have armed: the
 *        production row's whole budget, longer than any read's or capture's,
 *        and one over for the AO's rounding up to whole ticks.
 */
#define RFID_QUIET_TICKS \
    (QF_SUPPORT_STEPS((unsigned)KODE_RFID_TEST_BUDGET_MS) + 1U)

/** @brief Samples in a staged capture window: 20 ms at 80 kS/s. */
#define WIN_SAMPLES  1600U

/**
 * @brief What the busy case's read lends AO_Rfid: static, never a case's own
 *        stack, because a read left in flight by a failed case is ended later
 *        by its deadline and writes through these (kaos.h: a stack frame will
 *        not do).
 */
static uint8_t s_read_id[5];
static uint8_t s_read_rf;
static bool    s_read_have;

/**
 * @brief Bring AO_Rfid up over a fresh fake, after console_up().
 *
 * Any deadline a failed case left armed is let expire first, so the
 * QTimeEvt_ctorX() inside Rfid_ctor() never meets a still-linked timer - the
 * test_rfid_ao.c rule. The initial transition starts the AO's worker over a
 * fresh fake queue and hands the driver its worker check again, which
 * fake_rfid_reset() forgot.
 */
static void rfid_live(void)
{
    const QEvt *e = NULL;

    fake_rfid_reset();
    fake_khal_worker_reset();
    (void)qf_support_tick(AO_Rfid, RFID_QUIET_TICKS);
    Rfid_ctor();
    qf_support_give_queue(AO_Rfid);
    AO_Rfid->prio  = KODE_PRIO_RFID;
    AO_Rfid->pthre = KODE_PRIO_RFID;
    qf_support_register(AO_Rfid);
    while ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    QASM_INIT(AO_Rfid, (void *)0, 0U);
    fake_klog_reset();
}

/**
 * @brief Take the capture request the console posted to AO_Rfid, if any.
 * @return The request, still owned: the caller dispatches or collects it.
 */
static const QEvt *posted_capture(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) != pdTRUE )
    {
        return NULL;
    }
    TEST_ASSERT_EQUAL_INT(RFID_FIELD_REQ_SIG, e->sig);
    return e;
}

/** @brief Hand AO_Rfid a request the console posted, then collect it. */
static void rfid_takes(const QEvt *e)
{
    QASM_DISPATCH(AO_Rfid, e, 0U);
    QF_gc(e);
}

/** @brief Let the worker run its jobs, then AO_Rfid dispatch what they posted. */
static void rfid_serves(void)
{
    const QEvt *e = NULL;

    while ( fake_khal_worker_run_next() )
    {
        /* each job's done event is now in AO_Rfid's queue */
    }
    for ( unsigned i = 0U; 8U > i; ++i )
    {
        if ( xQueueReceive(AO_Rfid->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QASM_DISPATCH(AO_Rfid, e, 0U);
        QF_gc(e);
    }
}

/** @brief Hand the console whatever AO_Rfid posted it. */
static void console_hears(void)
{
    const QEvt *e = NULL;

    for ( unsigned i = 0U; 8U > i; ++i )
    {
        if ( xQueueReceive(AO_Console->eQueue, &e, 0U) != pdTRUE )
        {
            break;
        }
        QASM_DISPATCH(AO_Console, e, 0U);
        QF_gc(e);
    }
}

/** @brief How many replies so far contain @p needle. */
static unsigned replies_with(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            n += 1U;
        }
    }
    return n;
}

/** @brief A window with something in it: bursts of 3000 over a floor of 1000. */
static void stage_window(uint16_t *s)
{
    for ( size_t i = 0U; (size_t)WIN_SAMPLES > i; i++ )
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

TEST_CASE("rfid_field reads a window through AO_Rfid and prints its statistics",
          "[console_rfid]")
{
    static uint16_t        samples[WIN_SAMPLES];
    krfid_field_t          want;
    char                   rec[96];
    const QEvt            *e;
    const RfidFieldEvt    *q;

    console_up();
    rfid_live();
    stage_window(samples);
    krfid_wave_stats(samples, (size_t)WIN_SAMPLES, &want);

    type_line("rfid_field ms=50");

    e = posted_capture();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "nothing was asked of AO_Rfid");
    q = (const RfidFieldEvt *)e;
    TEST_ASSERT_EQUAL_UINT32(50U, q->ms);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q->to,
                                  "the answer must come back here");
    /* An Active Object is answered by value, and the waveform never goes to
     * the console: nothing is lent. */
    TEST_ASSERT_NULL(q->out);
    TEST_ASSERT_NULL(q->wave);
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)q->cap);
    TEST_ASSERT_NULL(q->n);
    TEST_ASSERT_NULL(q->rc);
    TEST_ASSERT_NULL(q->done);
    TEST_ASSERT_NULL(q->ack);
    rfid_takes(e);

    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_run_next(),
                             "the window was not handed to the worker");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, fake_rfid_capture_calls(),
                                  "the window was not read as a capture");
    TEST_ASSERT_EQUAL_INT(0, fake_rfid_read_calls());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Rfid answered");

    rfid_serves();
    console_hears();

    (void)snprintf(rec, sizeof(rec),
                   ">|rfid min=%u mean=%u peak=%u ac=%u n=%u",
                   (unsigned)want.min, (unsigned)want.mean,
                   (unsigned)want.peak, (unsigned)want.ac, WIN_SAMPLES);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, replies_with(">|rfid "),
                                   "not exactly one record");
    TEST_ASSERT_TRUE_MESSAGE(replied(rec), rec);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("rfid_field is answered busy while the front end reads a tag",
          "[console_rfid]")
{
    RfidReqEvt  rd;
    const QEvt *e;

    console_up();
    rfid_live();

    type_line("rfid_field ms=50");
    e = posted_capture();
    TEST_ASSERT_NOT_NULL(e);

    /* A read arrives first: its job is with the worker, not yet run, so the
     * front end is still held when the capture is looked at. */
    fake_rfid_set_window((int)KODE_TIMEOUT, NULL, 0U);
    rd.super  = (QEvt)QEVT_INITIALIZER(RFID_READ_REQ_SIG);
    rd.id     = s_read_id;
    rd.rf_div = &s_read_rf;
    rd.have   = &s_read_have;
    rd.done   = NULL;
    rd.gen    = 1U;
    rd.ack    = NULL;
    rd.ms     = 500U;
    rd.who    = NULL;
    rd.ask    = NULL;
    rd.rsp    = NULL;
    QASM_DISPATCH(AO_Rfid, &rd.super, 0U);

    rfid_takes(e);
    console_hears();

    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|rfid "), "a refusal printed a window");
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending(),
                                   "the refused capture reached the worker");

    /* The read ends as it would have, and leaves AO_Rfid idle. */
    rfid_serves();
    TEST_ASSERT_EQUAL_INT(1, fake_rfid_read_calls());
}

TEST_CASE("rfid_field names a window the front end could not read",
          "[console_rfid]")
{
    const QEvt *e;

    console_up();
    rfid_live();
    /* The fake's default after a reset: the ADC delivered nothing. */

    type_line("rfid_field ms=50");
    e = posted_capture();
    TEST_ASSERT_NOT_NULL(e);
    rfid_takes(e);
    rfid_serves();
    console_hears();

    TEST_ASSERT_EQUAL_INT(1, fake_rfid_capture_calls());
    TEST_ASSERT_TRUE(replied(">err code=5 msg=error"));
    TEST_ASSERT_FALSE(replied(">|rfid "));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("rfid_field refuses a window out of bounds before asking AO_Rfid",
          "[console_rfid]")
{
    const QEvt     *e;
    RfidFieldRspEvt a;

    console_up();

    type_line("rfid_field ms=0");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
    TEST_ASSERT_NULL(posted_capture());

    fake_klog_reset();
    type_line("rfid_field ms=151");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
    TEST_ASSERT_NULL(posted_capture());

    /* The window is the whole point of the verb, so it has no default. */
    fake_klog_reset();
    type_line("rfid_field");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=ms"));
    TEST_ASSERT_NULL(posted_capture());

    /* The longest window the front end reads is taken as typed. */
    fake_klog_reset();
    type_line("rfid_field ms=150");
    e = posted_capture();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT32(150U, ((const RfidFieldEvt *)e)->ms);
    QF_gc(e);

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(RFID_FIELD_RSP_SIG);
    a.st    = (uint32_t)KODE_DENIED;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
}

TEST_CASE("only a field answer closes rfid_field, and only while it waits",
          "[console_rfid]")
{
    const QEvt     *e;
    RfidFieldRspEvt a;
    NfcFieldRspEvt  other;
    RfidInfoEvt     info;

    console_up();

    /* An answer nobody is waiting for prints nothing. */
    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(RFID_FIELD_RSP_SIG);
    a.st    = (uint32_t)KODE_OK;
    a.min   = 5U;
    a.mean  = 5U;
    a.peak  = 5U;
    a.n     = 16U;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_FALSE(replied(">|rfid"));
    TEST_ASSERT_FALSE(replied(">ok"));

    type_line("rfid_field ms=50");
    e = posted_capture();
    TEST_ASSERT_NOT_NULL(e);
    QF_gc(e);

    /* Neither the LF status answer nor the NFC window's closes it. */
    (void)memset(&info, 0, sizeof(info));
    info.super = (QEvt)QEVT_INITIALIZER(RFID_INFO_RSP_SIG);
    QASM_DISPATCH(AO_Console, &info.super, 0U);
    (void)memset(&other, 0, sizeof(other));
    other.super = (QEvt)QEVT_INITIALIZER(NFC_FIELD_RSP_SIG);
    other.st    = (uint32_t)KODE_OK;
    QASM_DISPATCH(AO_Console, &other.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "another answer closed it");
    TEST_ASSERT_FALSE(replied(">|rfid=field"));
    TEST_ASSERT_FALSE(replied(">|nfc "));

    a.min  = 1000U;
    a.mean = 1200U;
    a.peak = 3000U;
    a.ac   = 2000U;
    a.n    = 4000U;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
    TEST_ASSERT_TRUE(replied(">|rfid min=1000 mean=1200 peak=3000 ac=2000 "
                             "n=4000"));
    TEST_ASSERT_TRUE(replied(">ok"));
}
