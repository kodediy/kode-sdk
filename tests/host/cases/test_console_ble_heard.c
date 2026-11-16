/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_ble_heard.c
 * @brief `ble_heard`, from the wire: the window, the record and the refusals.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kdrv_ble_heard.h"
#include "kode_wire.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks, repeated rather than imported for the
 * reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief QP ticks in @p ms milliseconds, plus the one that lets the timer fire. */
#define WINDOW_TICKS(ms_)  (((ms_) / SLICE_MS) + 1U)

/** @brief The console pacer's slice, ms (one QP tick at 100 Hz). */
#define SLICE_MS  10U

/** @brief The window the cases ask for. */
#define LISTEN_MS   200U

/** @brief The verb's longest wait for AO_Radio's answer, ms: above AO_Radio's own 3 s. */
#define LISTEN_WAIT_MS  5000U

/** @brief How long after a watchdog an answer of that run may still come, ms: the send guard and the wait. */
#define LISTEN_LATE_MS  (KODE_RADIO_SVC_SEND_GUARD_MS + LISTEN_WAIT_MS)

/** @brief How long the verb asks for a stop AO_Radio has no room for, ms: one second. */
#define LISTEN_STOP_TRIES_MS  1000U

/** @brief The address the cases' advertisements carry; it must never reach the console. */
#define ADV_ADDR_BYTE  0xA1U

/** @brief Advertisements a case hands a console that runs no `ble_heard`. */
#define BURST_RECORDS   12U

/** @brief How long the console holds its stop of a stream nobody reads before it asks again, ms. */
#define BURST_AGAIN_MS  1000U

/** @brief Bring the console up with a clean log and AO_Radio's queue empty. */
static void console_up(void)
{
    const QEvt *e;

    /* The verb keeps the time of the last run the watchdog ended, across
     * cases, and the clock starts again with each case: every case begins
     * later than the last one's longest wait. */
    static uint32_t s_epoch_ms;

    s_epoch_ms += 2U * LISTEN_LATE_MS;
    fake_time_advance_ms(s_epoch_ms);
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
    /* A command a case left open is closed by the watchdog. A `ble_heard`
     * stretches its own past this tick (console_extend()), so AO_Radio's
     * answer closes it: once for a start still waiting, again for its stop. */
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    {
        BleHeardStateEvt gone;

        (void)memset(&gone, 0, sizeof(gone));
        gone.super = (QEvt)QEVT_INITIALIZER(BLE_HEARD_STATE_SIG);
        gone.why   = (uint8_t)KBLE_HEARD_NO_LINK;
        QASM_DISPATCH(AO_Console, &gone.super, 0U);
        QASM_DISPATCH(AO_Console, &gone.super, 0U);
    }

    qf_support_give_queue(AO_Radio);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    fake_klog_reset();
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

/** @brief The first console line holding @p needle, or NULL. */
static const char *line_with(const char *needle)
{
    for ( uint32_t i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return l;
        }
    }
    return NULL;
}

/** @brief How many console lines hold @p needle. */
static uint32_t lines_with(const char *needle)
{
    uint32_t n = 0U;

    for ( uint32_t i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);

        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            n += 1U;
        }
    }
    return n;
}

/** @brief What the console posted to AO_Radio: the advertisement requests, and how many. */
typedef struct {
    uint32_t  starts;   /**< Starts seen.                  */
    uint32_t  stops;    /**< Stops seen.                   */
    uint32_t  count;    /**< Requests of BLE_HEARD_REQ_SIG.  */
    QActive  *who;      /**< The last one's asker.         */
} posted_t;

/** @brief Read and recycle everything AO_Radio holds. */
static posted_t posted_requests(void)
{
    posted_t    p;
    const QEvt *e = NULL;

    (void)memset(&p, 0, sizeof(p));
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)BLE_HEARD_REQ_SIG == e->sig )
        {
            const BleHeardReqEvt *const r = (const BleHeardReqEvt *)(const void *)e;

            p.count += 1U;
            if ( r->on )
            {
                p.starts += 1U;
            }
            else
            {
                p.stops += 1U;
            }
            p.who = r->who;
        }
        QF_gc(e);
    }
    return p;
}

/** @brief AO_Radio answers the request: listening or not, why, and the totals. */
static void radio_says(bool on, kble_heard_why_t why, uint32_t heard, uint32_t dropped)
{
    BleHeardStateEvt s;

    (void)memset(&s, 0, sizeof(s));
    s.super   = (QEvt)QEVT_INITIALIZER(BLE_HEARD_STATE_SIG);
    s.on      = on;
    s.why     = (uint8_t)why;
    s.heard   = heard;
    s.dropped = dropped;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
}

/** @brief AO_Radio posts one advertisement. */
static void radio_heard(kdrv_ble_heard_pdu_t pdu, int8_t rssi, uint32_t dropped)
{
    BleHeardEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(BLE_HEARD_SIG);
    (void)memset(a.addr, (int)ADV_ADDR_BYTE, sizeof(a.addr));
    a.pdu     = (uint8_t)pdu;
    a.rssi    = rssi;
    a.len     = 3U;
    a.data[0] = 0xD1U;
    a.data[1] = 0xD2U;
    a.data[2] = 0xD3U;
    a.dropped = dropped;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/** @brief Let the window run out, one slice of the console's pacer at a time. */
static void window_passes(uint32_t ms)
{
    for ( uint32_t t = 0U; WINDOW_TICKS(ms) > t; ++t )
    {
        fake_time_advance_ms(SLICE_MS);
        (void)qf_support_tick(AO_Console, 1U);
    }
}

/** @brief AO_Radio answers the stop: nothing listens, with the totals. */
static void radio_stops(uint32_t heard, uint32_t dropped)
{
    radio_says(false, KBLE_HEARD_OK, heard, dropped);
}

TEST_CASE("ble_heard listens, stops when the window ends and answers one record", "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=200");

    posted_t p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.count);
    TEST_ASSERT_EQUAL_UINT(1U, p.starts);
    TEST_ASSERT_TRUE(AO_Console == p.who);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the verb answered before its window");

    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -70, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -45, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_NONCONN, -60, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_SCAN, -80, 1U);
    radio_heard(KDRV_BLE_HEARD_PDU_DIRECT, -90, 1U);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the verb answered before its window");

    window_passes(LISTEN_MS);

    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.count);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, p.stops, "the window's end did not ask to stop");
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the verb answered before the stop was");
    TEST_ASSERT_NULL_MESSAGE(line_with(">|ble_heard"), "the verb answered before the stop was");

    radio_stops(7U, 2U);
    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=7 handed=5 dropped=2 conn=2 direct=1 scan=1 "
                                   "nonconn=1 rssi_max=-45"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("ble_heard prints - for the level of a window that heard nothing", "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    window_passes(LISTEN_MS);
    radio_stops(0U, 0U);

    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=0 handed=0 dropped=0 conn=0 direct=0 scan=0 "
                                   "nonconn=0 rssi_max=-"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("ble_heard leaves a level the controller did not give out of the strongest", "[console_ble_heard]")
{
    const char *record;
    const char *level;
    char        after;

    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, (int8_t)KBLE_HEARD_RSSI_NONE, 0U);
    window_passes(LISTEN_MS);
    radio_stops(1U, 0U);
    record = line_with("handed=1");
    TEST_ASSERT_NOT_NULL_MESSAGE(record, "the record was not counted");
    level = strstr(record, "rssi_max=-");
    TEST_ASSERT_NOT_NULL_MESSAGE(level, "a level nobody measured was printed");
    after = level[strlen("rssi_max=-")];
    TEST_ASSERT_TRUE_MESSAGE(('0' > after) || ('9' < after), "a level nobody measured was printed");

    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, (int8_t)KBLE_HEARD_RSSI_NONE, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    window_passes(LISTEN_MS);
    radio_stops(2U, 0U);
    TEST_ASSERT_NOT_NULL(line_with("rssi_max=-40"));
}

TEST_CASE("ble_heard prints nothing but counts and levels", "[console_ble_heard]")
{
    const char *rec;

    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    window_passes(LISTEN_MS);
    radio_stops(1U, 0U);

    rec = line_with(">|ble_heard");
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_NULL_MESSAGE(strstr(rec, "a1"), "an address reached the record");
    TEST_ASSERT_NULL_MESSAGE(strstr(rec, "A1"), "an address reached the record");
    TEST_ASSERT_NULL_MESSAGE(strstr(rec, "d1"), "a payload reached the record");
    TEST_ASSERT_NULL_MESSAGE(line_with("a1:a1"), "an address reached the console");
}

TEST_CASE("ble_heard names each reason AO_Radio refuses with", "[console_ble_heard]")
{
    static const struct {
        kble_heard_why_t why;
        const char    *line;
    } REFUSALS[] = {
        { KBLE_HEARD_NO_LINK,  ">err code=5 msg=no_link" },
        { KBLE_HEARD_BLE_OFF,  ">err code=5 msg=ble_off" },
        { KBLE_HEARD_MEMORY,   ">err code=5 msg=memory"  },
        { KBLE_HEARD_REFUSED,  ">err code=5 msg=refused" },
        { KBLE_HEARD_RESTING,  ">err code=5 msg=resting" },
        { KBLE_HEARD_CANCELLED,    ">err code=5 msg=cancelled"   },
        { KBLE_HEARD_BUSY,     ">err code=4 msg=busy"    },
    };

    for ( uint32_t i = 0U; (sizeof(REFUSALS) / sizeof(REFUSALS[0])) > i; ++i )
    {
        console_up();
        type_line("ble_heard ms=200");
        (void)posted_requests();
        radio_says(false, REFUSALS[i].why, 0U, 0U);

        TEST_ASSERT_NOT_NULL_MESSAGE(line_with(REFUSALS[i].line), REFUSALS[i].line);
        TEST_ASSERT_NULL(line_with(">ok"));

        /* The console is free again: the next command is taken. */
        fake_klog_reset();
        type_line("ble_heard ms=200");
        TEST_ASSERT_EQUAL_UINT(1U, posted_requests().count);
        radio_says(false, KBLE_HEARD_NO_LINK, 0U, 0U);
    }
}

TEST_CASE("ble_heard refuses a window out of range with code 3, and none with code 2", "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=50");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ms"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    fake_klog_reset();
    type_line("ble_heard ms=10001");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ms"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    fake_klog_reset();
    type_line("ble_heard");
    TEST_ASSERT_NOT_NULL(line_with(">err code=2 msg=ms"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);
}

TEST_CASE("ble_heard ends with the reason when Bluetooth goes in the middle of its window",
          "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    radio_says(false, KBLE_HEARD_BLE_OFF, 1U, 0U);

    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=ble_off"));
    TEST_ASSERT_NULL(line_with(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);
}

/** @brief Start a run, let it listen and let its window run out; the stop is not answered yet. */
static void run_to_the_stop(void)
{
    type_line("ble_heard ms=200");
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    window_passes(LISTEN_MS);
}

TEST_CASE("a ble_heard typed while the window runs or the stop waits is refused busy, and none is lost",
          "[console_ble_heard]")
{
    posted_t first;
    posted_t second;

    console_up();
    type_line("ble_heard ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);

    /* In the window the console is the run's. */
    type_line("ble_heard ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    fake_klog_reset();
    window_passes(LISTEN_MS);
    first = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, first.stops);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the first run closed before its stop was answered");

    type_line("ble_heard ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    radio_stops(0U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    /* The second run is whole: its own answer, its own window, its own stop. */
    fake_klog_reset();
    run_to_the_stop();
    second = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, second.starts);
    TEST_ASSERT_EQUAL_UINT(1U, second.stops);
    radio_stops(0U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=0"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_NULL(line_with(">err"));
}

TEST_CASE("a stop's answer that comes late is not read as the next run's refusal", "[console_ble_heard]")
{
    console_up();
    run_to_the_stop();
    (void)posted_requests();
    radio_stops(0U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    /* The next run starts, and an answer of the stop before it - the same
     * kind of event, delivered late - reaches it. A refusal is never OK. */
    fake_klog_reset();
    type_line("ble_heard ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);
    radio_stops(0U, 0U);
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "a stop's answer ended the next run");

    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    window_passes(LISTEN_MS);
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().stops);
    radio_stops(0U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_NULL(line_with(">err"));
}

/** @brief Fill AO_Radio's queue to its margin. */
static void radio_full(void)
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    for ( uint32_t guard = 0U;
         (64U > guard) && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
}

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_empty(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

TEST_CASE("a stop AO_Radio has no room for is asked again, and the run ends with its record",
          "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_full();
    window_passes(LISTEN_MS);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the run closed with the stop not asked");

    /* Room again: the next slice asks, and the answer closes the run. */
    radio_empty();
    window_passes(LISTEN_MS);
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().stops);
    radio_stops(0U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=0"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("a stop AO_Radio never has room for ends the run saying so", "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_full();
    window_passes(LISTEN_MS + (2U * LISTEN_STOP_TRIES_MS));

    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=0"));
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err code=5 msg=stop"),
                                 "the run ended without saying the passive scan may still be on");
    TEST_ASSERT_NULL(line_with(">ok"));
    radio_empty();
    /* The console is free, and the same asker may start again. */
    fake_klog_reset();
    type_line("ble_heard ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);
    radio_says(false, KBLE_HEARD_NO_LINK, 0U, 0U);
}

TEST_CASE("a stop the worker could not take is asked again, and is bounded", "[console_ble_heard]")
{
    posted_t p;

    /* AO_Radio says it still listens and that the stop was busy: the run asks again. */
    console_up();
    run_to_the_stop();
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
    radio_says(true, KBLE_HEARD_BUSY, 3U, 0U);
    TEST_ASSERT_NULL(line_with(">err"));
    window_passes(SLICE_MS);
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, p.stops, "the stop was not asked again");
    radio_stops(3U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=3"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    /* A worker that never takes it: the run ends with the record and `stop`. */
    console_up();
    run_to_the_stop();
    (void)posted_requests();
    for ( uint32_t i = 0U; ((LISTEN_STOP_TRIES_MS / SLICE_MS) + 2U) > i; ++i )
    {
        radio_says(true, KBLE_HEARD_BUSY, 3U, 0U);
        window_passes(SLICE_MS);
        (void)posted_requests();
    }
    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard"));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stop"));
    TEST_ASSERT_NULL(line_with(">ok"));
}

/** @brief Let the console's watchdog run out. */
static void watchdog_runs_out(void)
{
    (void)qf_support_tick(AO_Console, WINDOW_TICKS(LISTEN_WAIT_MS));
}

TEST_CASE("a start AO_Radio never answers frees the console by itself, with one error",
          "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(">err"));

    watchdog_runs_out();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, lines_with(">err"), "one terminal line");
    TEST_ASSERT_NOT_NULL(line_with(">err code=6 msg=ble_heard"));
    TEST_ASSERT_NULL(line_with(">ok"));
    /* The start may still be taken late: it is stopped behind itself. */
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().stops);
    fake_klog_reset();
    type_line("ble_heard ms=50");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ms"));
}

TEST_CASE("a stop AO_Radio never answers frees the console by itself, after the record",
          "[console_ble_heard]")
{
    posted_t p;

    console_up();
    run_to_the_stop();
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.starts);
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
    TEST_ASSERT_NULL(line_with(">ok"));

    watchdog_runs_out();

    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">|ble_heard"), "the record comes first");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, lines_with(">err"), "one terminal line");
    TEST_ASSERT_NOT_NULL(line_with(">err code=6 msg=ble_heard"));
    TEST_ASSERT_NULL(line_with(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, posted_requests().count, "a stop was posted twice");
}

TEST_CASE("a stop's answer after the watchdog is never taken by the next run", "[console_ble_heard]")
{
    uint32_t starts = 0U;
    uint32_t stops  = 0U;
    posted_t p;

    console_up();
    run_to_the_stop();
    p = posted_requests();
    starts += p.starts;
    stops  += p.stops;
    watchdog_runs_out();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, lines_with(">err"), "the watchdog did not close the run");

    /* The stop's answer may still be on its way, so the next run waits for it
     * to have landed or become impossible: it is refused, and posts nothing. */
    fake_klog_reset();
    type_line("ble_heard ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    /* The late answer lands on an idle console and is dropped. */
    radio_says(false, KBLE_HEARD_NO_LINK, 0U, 0U);
    TEST_ASSERT_NULL(line_with(">ok"));

    /* Past the longest an answer can take, the next run is whole. */
    fake_time_advance_ms(LISTEN_LATE_MS + 1U);
    fake_klog_reset();
    run_to_the_stop();
    p = posted_requests();
    starts += p.starts;
    stops  += p.stops;
    radio_stops(0U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=0"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_NULL(line_with(">err"));
    TEST_ASSERT_EQUAL_UINT(2U, starts);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(starts, stops, "a start was left without its stop");
}

TEST_CASE("an advertisement that reaches the console after a run whose stop never reached AO_Radio asks for the stop",
          "[console_ble_heard]")
{
    posted_t p;

    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_full();
    window_passes(LISTEN_MS + (2U * LISTEN_STOP_TRIES_MS));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stop"));
    radio_empty();

    /* AO_Radio still counts the console as the asker: its next record comes
     * here, to a console with no run, which asks for the stop again. */
    fake_klog_reset();
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.count);
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
    TEST_ASSERT_TRUE(AO_Console == p.who);
    TEST_ASSERT_NULL_MESSAGE(line_with(">"), "the console answered a run that is over");
}

TEST_CASE("a burst of advertisements with no run asks for one stop, and again a second later",
          "[console_ble_heard]")
{
    posted_t p;

    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_full();
    window_passes(LISTEN_MS + (2U * LISTEN_STOP_TRIES_MS));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stop"));
    radio_empty();

    /* A queue of records with nobody reading: one stop, not one a record. */
    for ( uint32_t i = 0U; BURST_RECORDS > i; ++i )
    {
        radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    }
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, p.stops, "the console asked for a stop a record");

    /* The stream is still there a second later: it is asked again, once. */
    fake_time_advance_ms(BURST_AGAIN_MS);
    for ( uint32_t i = 0U; BURST_RECORDS > i; ++i )
    {
        radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    }
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
}

TEST_CASE("a stop AO_Radio had no room for is asked again by the next advertisement", "[console_ble_heard]")
{
    posted_t p;

    console_up();
    type_line("ble_heard ms=200");
    (void)posted_requests();
    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    radio_full();
    window_passes(LISTEN_MS + (2U * LISTEN_STOP_TRIES_MS));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stop"));

    /* Still no room: the stop does not get in, and nothing is held against the next try. */
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    radio_empty();
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
}

TEST_CASE("an advertisement that reaches a run still waiting for its start asks for no stop",
          "[console_ble_heard]")
{
    console_up();
    type_line("ble_heard ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);

    /* A record of the console's earlier stream, ahead of the new start's answer. */
    radio_heard(KDRV_BLE_HEARD_PDU_CONN, -40, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    radio_says(true, KBLE_HEARD_OK, 0U, 0U);
    window_passes(LISTEN_MS);
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().stops);
    radio_stops(0U, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">|ble_heard heard=0 handed=0"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}
