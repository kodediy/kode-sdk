/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_frames.c
 * @brief `wifi_frames`, from the wire: the window, the record and the refusals.
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

/** @brief The window and channel the cases ask for. */
#define FRAMES_MS   200U
#define FRAMES_CH   6U

/** @brief The verb's longest wait for AO_Radio's answer, ms: above AO_Radio's own 3 s. */
#define FRAMES_WAIT_MS  5000U

/** @brief How long after a watchdog an answer of that run may still come, ms: the send guard and the wait. */
#define FRAMES_LATE_MS  (KODE_RADIO_SVC_SEND_GUARD_MS + FRAMES_WAIT_MS)

/** @brief How long the verb asks for a stop AO_Radio has no room for, ms: one second. */
#define RADIO_FRAMES_STOP_TRIES_MS  1000U

/** @brief Bring the console up with a clean log and AO_Radio's queue empty. */
static void console_up(void)
{
    const QEvt *e;

    /* The verb keeps the time of the last run the watchdog ended, across
     * cases, and the clock starts again with each case: every case begins
     * later than the last one's longest wait. */
    static uint32_t s_epoch_ms;

    s_epoch_ms += 2U * FRAMES_LATE_MS;
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
    /* A command a case left open is closed by the watchdog; a `wifi_frames`
     * has none, and AO_Radio's answer closes it. */
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    {
        WifiFramesStateEvt gone;

        (void)memset(&gone, 0, sizeof(gone));
        gone.super = (QEvt)QEVT_INITIALIZER(WIFI_FRAMES_STATE_SIG);
        gone.why   = (uint8_t)KWIFI_FRAMES_NO_LINK;
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
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return l;
        }
    }
    return NULL;
}

/** @brief What the console posted to AO_Radio: the last frames request, and how many. */
typedef struct {
    unsigned  starts;   /**< Starts seen.                            */
    unsigned  stops;    /**< Stops seen.                             */
    unsigned  count;    /**< Requests of WIFI_FRAMES_REQ_SIG seen.   */
    bool      on;       /**< The last one's wish.                    */
    uint8_t   channel;  /**< Its channel.                            */
    QActive  *who;      /**< Its asker.                              */
} posted_t;

/** @brief Read and recycle everything AO_Radio holds. */
static posted_t posted_requests(void)
{
    posted_t    p;
    const QEvt *e = NULL;

    (void)memset(&p, 0, sizeof(p));
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)WIFI_FRAMES_REQ_SIG == e->sig )
        {
            const WifiFramesReqEvt *const r = (const WifiFramesReqEvt *)(const void *)e;

            p.count   += 1U;
            if ( r->on )
            {
                p.starts += 1U;
            }
            else
            {
                p.stops += 1U;
            }
            p.on       = r->on;
            p.channel  = r->channel;
            p.who      = r->who;
        }
        QF_gc(e);
    }
    return p;
}

/** @brief AO_Radio answers the request: listening or not, and why. */
static void radio_says(bool on, uint8_t ch, bool joined, kwifi_frames_why_t why)
{
    WifiFramesStateEvt s;

    (void)memset(&s, 0, sizeof(s));
    s.super   = (QEvt)QEVT_INITIALIZER(WIFI_FRAMES_STATE_SIG);
    s.on      = on;
    s.channel = ch;
    s.joined  = joined;
    s.why     = (uint8_t)why;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
}

/** @brief AO_Radio posts one period's counts. */
static void radio_ticks(uint8_t ch, bool joined, int8_t rssi_max, uint32_t heard,
                        uint16_t mgmt, uint16_t data, uint32_t dropped)
{
    WifiFramesTickEvt t;

    (void)memset(&t, 0, sizeof(t));
    t.super    = (QEvt)QEVT_INITIALIZER(WIFI_FRAMES_TICK_SIG);
    t.channel  = ch;
    t.joined   = joined;
    t.rssi_max = rssi_max;
    t.heard    = heard;
    t.mgmt     = mgmt;
    t.data     = data;
    t.dropped  = dropped;
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

/** @brief AO_Radio posts one frame. */
static void radio_frame(void)
{
    WifiFrameEvt f;

    (void)memset(&f, 0, sizeof(f));
    f.super   = (QEvt)QEVT_INITIALIZER(WIFI_FRAME_SIG);
    f.channel = FRAMES_CH;
    f.rssi    = -50;
    (void)memset(f.addr1, 0xA1, sizeof(f.addr1));
    (void)memset(f.addr2, 0xA2, sizeof(f.addr2));
    QASM_DISPATCH(AO_Console, &f.super, 0U);
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

/** @brief AO_Radio answers the stop: the C5 has stopped. */
static void radio_stops(void)
{
    radio_says(false, FRAMES_CH, false, KWIFI_FRAMES_OK);
}

TEST_CASE("wifi_frames listens, stops when the window ends and answers one record",
          "[console_frames]")
{
    console_up();
    type_line("wifi_frames ch=6 ms=200");

    posted_t p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.count);
    TEST_ASSERT_TRUE(p.on);
    TEST_ASSERT_EQUAL_UINT8(FRAMES_CH, p.channel);
    TEST_ASSERT_TRUE(AO_Console == p.who);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the verb answered before its window");

    radio_says(true, FRAMES_CH, false, KWIFI_FRAMES_OK);
    radio_ticks(FRAMES_CH, false, -60, 4U, 2U, 1U, 0U);
    radio_frame();
    radio_frame();
    radio_ticks(FRAMES_CH, false, -35, 9U, 3U, 4U, 1U);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the verb answered before its window");

    window_passes(FRAMES_MS);

    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.count);
    TEST_ASSERT_FALSE_MESSAGE(p.on, "the window's end did not ask to stop");
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the verb answered before the stop was");
    TEST_ASSERT_NULL_MESSAGE(line_with(">|frames"), "the verb answered before the stop was");

    radio_stops();
    TEST_ASSERT_NOT_NULL(line_with(">|frames ch=6 joined=0 heard=9 handed=2 dropped=1 "
                                   "mgmt=5 data=5 rssi_max=-35"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("wifi_frames prints - for the level of a window that heard nothing", "[console_frames]")
{
    console_up();
    type_line("wifi_frames ch=6 ms=200");
    (void)posted_requests();
    radio_says(true, FRAMES_CH, true, KWIFI_FRAMES_OK);
    window_passes(FRAMES_MS);
    radio_stops();

    TEST_ASSERT_NOT_NULL(line_with(">|frames ch=6 joined=1 heard=0 handed=0 dropped=0 "
                                   "mgmt=0 data=0 rssi_max=-"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("wifi_frames prints nothing but counts and levels", "[console_frames]")
{
    const char *rec;

    console_up();
    type_line("wifi_frames ch=6 ms=200");
    (void)posted_requests();
    radio_says(true, FRAMES_CH, false, KWIFI_FRAMES_OK);
    radio_ticks(FRAMES_CH, false, -40, 1U, 1U, 0U, 0U);
    radio_frame();
    window_passes(FRAMES_MS);
    radio_stops();

    rec = line_with(">|frames");
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_NULL_MESSAGE(strstr(rec, "a1"), "an address reached the record");
    TEST_ASSERT_NULL_MESSAGE(strstr(rec, "A1"), "an address reached the record");
    TEST_ASSERT_NULL_MESSAGE(line_with("a1:a1"), "an address reached the console");
}

TEST_CASE("wifi_frames names each reason AO_Radio refuses with", "[console_frames]")
{
    static const struct {
        kwifi_frames_why_t why;
        const char        *line;
    } REFUSALS[] = {
        { KWIFI_FRAMES_NO_LINK,    ">err code=5 msg=no_link"    },
        { KWIFI_FRAMES_NO_SERVICE, ">err code=5 msg=no_service" },
        { KWIFI_FRAMES_WIFI_OFF,   ">err code=5 msg=wifi_off"   },
        { KWIFI_FRAMES_JOINED,     ">err code=5 msg=joined"     },
        { KWIFI_FRAMES_CHANNEL,    ">err code=5 msg=channel"    },
        { KWIFI_FRAMES_MEMORY,     ">err code=5 msg=memory"     },
        { KWIFI_FRAMES_RESTING,    ">err code=5 msg=resting"    },
        { KWIFI_FRAMES_BUSY,       ">err code=4 msg=busy"       },
    };

    for ( unsigned i = 0U; (sizeof(REFUSALS) / sizeof(REFUSALS[0])) > i; ++i )
    {
        console_up();
        type_line("wifi_frames ch=6 ms=200");
        (void)posted_requests();
        radio_says(false, 0U, false, REFUSALS[i].why);

        TEST_ASSERT_NOT_NULL_MESSAGE(line_with(REFUSALS[i].line), REFUSALS[i].line);
        TEST_ASSERT_NULL(line_with(">ok"));

        /* The console is free again: the next command is taken. */
        fake_klog_reset();
        type_line("wifi_frames ch=6 ms=200");
        TEST_ASSERT_EQUAL_UINT(1U, posted_requests().count);
        radio_says(false, 0U, false, KWIFI_FRAMES_NO_LINK);
    }
}

TEST_CASE("wifi_frames refuses a channel or a window out of range with code 3", "[console_frames]")
{
    console_up();
    type_line("wifi_frames ch=200 ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ch"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    fake_klog_reset();
    type_line("wifi_frames ch=6 ms=50");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ms"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    fake_klog_reset();
    type_line("wifi_frames ch=6 ms=10001");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ms"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);
}

TEST_CASE("wifi_frames ends with the reason when the link goes in the middle of its window",
          "[console_frames]")
{
    console_up();
    type_line("wifi_frames ch=6 ms=200");
    (void)posted_requests();
    radio_says(true, FRAMES_CH, false, KWIFI_FRAMES_OK);
    radio_ticks(FRAMES_CH, false, -40, 1U, 1U, 0U, 0U);
    radio_says(false, 0U, false, KWIFI_FRAMES_NO_LINK);

    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=no_link"));
    TEST_ASSERT_NULL(line_with(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);
}

/** @brief Start a run, let it listen and let its window run out; the stop is not answered yet. */
static void run_to_the_stop(void)
{
    type_line("wifi_frames ch=6 ms=200");
    radio_says(true, FRAMES_CH, false, KWIFI_FRAMES_OK);
    window_passes(FRAMES_MS);
}

TEST_CASE("a wifi_frames typed before the last one's stop is answered is refused busy, and none is lost",
          "[console_frames]")
{
    posted_t first;
    posted_t second;

    console_up();
    run_to_the_stop();
    first = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, first.starts);
    TEST_ASSERT_EQUAL_UINT(1U, first.stops);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the first run closed before its stop was answered");

    /* The new line comes while the stop is out: the console is still the first run's. */
    type_line("wifi_frames ch=6 ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    radio_stops();
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    /* The second run is whole: its own answer, its own window, its own stop. */
    fake_klog_reset();
    run_to_the_stop();
    second = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, second.starts);
    TEST_ASSERT_EQUAL_UINT(1U, second.stops);
    radio_stops();
    TEST_ASSERT_NOT_NULL(line_with(">|frames ch=6"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_NULL(line_with(">err"));
}

TEST_CASE("a stop's answer that comes late is not read as the next run's refusal", "[console_frames]")
{
    console_up();
    run_to_the_stop();
    (void)posted_requests();
    radio_stops();
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    /* The next run starts, and an answer of the stop before it - the same
     * kind of event, delivered late - reaches it. A refusal is never OK. */
    fake_klog_reset();
    type_line("wifi_frames ch=6 ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);
    radio_stops();
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "a stop's answer ended the next run");

    radio_says(true, FRAMES_CH, false, KWIFI_FRAMES_OK);
    window_passes(FRAMES_MS);
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().stops);
    radio_stops();
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_NULL(line_with(">err"));
}

TEST_CASE("a stop AO_Radio has no room for is asked again, and the run ends with its record",
          "[console_frames]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    console_up();
    type_line("wifi_frames ch=6 ms=200");
    (void)posted_requests();
    radio_says(true, FRAMES_CH, false, KWIFI_FRAMES_OK);
    for ( unsigned guard = 0U;
         (64U > guard) && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    window_passes(FRAMES_MS);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the run closed with the stop not asked");

    /* Room again: the next slice asks, and the answer closes the run. */
    {
        const QEvt *e = NULL;

        while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
        {
            QF_gc(e);
        }
    }
    window_passes(FRAMES_MS);
    posted_t p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
    radio_stops();
    TEST_ASSERT_NOT_NULL(line_with(">|frames ch=6"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("a stop AO_Radio never has room for ends the run saying so", "[console_frames]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    console_up();
    type_line("wifi_frames ch=6 ms=200");
    (void)posted_requests();
    radio_says(true, FRAMES_CH, false, KWIFI_FRAMES_OK);
    for ( unsigned guard = 0U;
         (64U > guard) && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    window_passes(FRAMES_MS + (2U * RADIO_FRAMES_STOP_TRIES_MS));

    TEST_ASSERT_NOT_NULL(line_with(">|frames ch=6"));
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err code=5 msg=stop"),
                                 "the run ended without saying the C5 may still listen");
    TEST_ASSERT_NULL(line_with(">ok"));
    {
        const QEvt *e = NULL;

        while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
        {
            QF_gc(e);
        }
    }
    /* The console is free, and the same asker may start again. */
    fake_klog_reset();
    type_line("wifi_frames ch=6 ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);
    radio_says(false, 0U, false, KWIFI_FRAMES_NO_LINK);   /* leave the console free for the next file */
}

/** @brief How many console lines hold @p needle. */
static unsigned lines_with(const char *needle)
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

/** @brief Let the console's watchdog run out. */
static void watchdog_runs_out(void)
{
    (void)qf_support_tick(AO_Console, WINDOW_TICKS(FRAMES_WAIT_MS));
}

TEST_CASE("a start AO_Radio never answers frees the console by itself, with one error",
          "[console_frames]")
{
    posted_t p;

    console_up();
    type_line("wifi_frames ch=6 ms=200");
    TEST_ASSERT_EQUAL_UINT(1U, posted_requests().starts);
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(">err"));

    watchdog_runs_out();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, lines_with(">err"), "one terminal line");
    TEST_ASSERT_NOT_NULL(line_with(">err code=6 msg=wifi_frames"));
    TEST_ASSERT_NULL(line_with(">ok"));
    /* The start may still be taken late: it is stopped behind itself. */
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
    /* The console is free again. */
    fake_klog_reset();
    type_line("wifi_frames ch=200 ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ch"));
}

TEST_CASE("a stop AO_Radio never answers frees the console by itself, after the record",
          "[console_frames]")
{
    posted_t p;

    console_up();
    run_to_the_stop();
    p = posted_requests();
    TEST_ASSERT_EQUAL_UINT(1U, p.starts);
    TEST_ASSERT_EQUAL_UINT(1U, p.stops);
    TEST_ASSERT_NULL(line_with(">ok"));

    watchdog_runs_out();

    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">|frames ch=6"), "the record comes first");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, lines_with(">err"), "one terminal line");
    TEST_ASSERT_NOT_NULL(line_with(">err code=6 msg=wifi_frames"));
    TEST_ASSERT_NULL(line_with(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, posted_requests().count, "a stop was posted twice");
    fake_klog_reset();
    type_line("wifi_frames ch=200 ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ch"));
}

TEST_CASE("a stop's answer after the watchdog is never taken by the next run",
          "[console_frames]")
{
    unsigned starts = 0U;
    unsigned stops  = 0U;
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
    type_line("wifi_frames ch=6 ms=200");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT(0U, posted_requests().count);

    /* The late answer lands on an idle console and is dropped. */
    radio_says(false, 0U, false, KWIFI_FRAMES_NO_LINK);
    TEST_ASSERT_NULL(line_with(">ok"));

    /* Past the longest an answer can take, the next run is whole, and no stream is left on. */
    fake_time_advance_ms(FRAMES_LATE_MS + 1U);
    fake_klog_reset();
    run_to_the_stop();
    p = posted_requests();
    starts += p.starts;
    stops  += p.stops;
    radio_stops();
    TEST_ASSERT_NOT_NULL(line_with(">|frames ch=6"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_NULL(line_with(">err"));
    TEST_ASSERT_EQUAL_UINT(2U, starts);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(starts, stops, "a start was left without its stop");
}
