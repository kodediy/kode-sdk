/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_test_dot.c
 * @brief The station's console verbs: who may type them and what reaches AO_TestDot.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fake_station.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "qf_support.h"
#include "test_dot_support.h"

#if !CONFIG_KODE_TEST_DOT
#error "test_console_test_dot.c needs CONFIG_KODE_TEST_DOT: main/CMakeLists.txt spells it out"
#endif

/** @brief Past a station verb's budget, the longest of any command: whatever is left open times out. */
#define TDC_CLOSE_MS          (KODE_TEST_DOT_LINE_MS + 1000U)
/** @brief Most rounds of the two queues after a typed line. */
#define TDC_ROUNDS            16U

/** @brief Empty an Active Object's queue, so a case sees only its own events. */
static void mailbox_empty(QActive *ao)
{
    const QEvt *e = NULL;

    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The console up, then AO_TestDot built afresh (stored lock or not). */
static void up(bool locked)
{
    qf_support_init();
    autotest_up();
    (void)ao_spy();
    ao_spy_drain();
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
    (void)qf_support_tick(AO_Console, QF_SUPPORT_STEPS(TDC_CLOSE_MS));
    if ( locked )
    {
        td_up_locked();
    }
    else
    {
        td_up();
    }
    mailbox_empty(AO_Boot);
    fake_klog_reset();
}

/**
 * @brief End a case: the console's command closed, then AO_TestDot quiet.
 *
 * A station verb's budget is longer than any other command's, so a command
 * left open here would outlast the time-out the console files after this one
 * tick through, and they would read busy.
 */
static void down(void)
{
    (void)qf_support_tick(AO_Console, QF_SUPPORT_STEPS(TDC_CLOSE_MS));
    td_down();
}

/** @brief Both queues dispatched until neither holds anything. */
static void exchange(void)
{
    for ( unsigned i = 0U; TDC_ROUNDS > i; i++ )
    {
        td_settle();
        if ( qf_support_settle(AO_Console) == 0U )
        {
            break;
        }
    }
}

/** @brief A line typed on port @p src, then the exchange it starts. */
static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
}

/** @brief A line typed on the cable, and its exchange. */
static void type_line(const char *text)
{
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, text);
    exchange();
}

/** @brief How many of the log's lines hold @p needle. */
static unsigned replied_n(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            n++;
        }
    }
    return n;
}

/** @brief Whether a log line holds @p needle. */
static bool replied(const char *needle)
{
    return replied_n(needle) > 0U;
}

/** @brief The station requests waiting in AO_TestDot's queue; the queue is emptied. @p relays Out: the last one's byte. */
static unsigned requests(uint8_t *relays)
{
    unsigned n = 0U;
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_TestDot->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)TEST_DOT_REQ_SIG == e->sig )
        {
            n++;
            if ( NULL != relays )
            {
                *relays = ((const TestDotReqEvt *)e)->a.relays;
            }
        }
        QF_gc(e);
    }
    return n;
}

TEST_CASE("test_all_from_ble_or_channel_is_refused_cable", "[console_test_dot]")
{
    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "test component=all");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, requests(NULL), "nothing reaches AO_TestDot");

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "test component=all");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=net"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "test_dot_info");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    /* The cable reaches it. */
    fake_klog_reset();
    type_line("test_dot_info");
    TEST_ASSERT_TRUE(replied(">|test_dot state=dormant"));
    TEST_ASSERT_TRUE(replied(">ok"));
    down();
}

TEST_CASE("test_locked_refuses_every_station_verb", "[console_test_dot]")
{
    static const char *const LINES[] = {
        "test component=all", "test_dot_place", "test_dot_poll", "test_dot_screen phase=connecting",
        "test_dot_photos", "test_dot_blob name=\"location\"", "test_dot_relays state=0xFF", "test_dot_adc ch=0",
        "test_dot_loop", "test_dot_boost state=on", "test_dot_result verdict=fail", "test_dot_lock confirm=on",
        "test_dot_end", "test_dot_info", "test_dot_camera",
    };
    unsigned const n = (unsigned)(sizeof(LINES) / sizeof(LINES[0]));

    up(true);
    for ( unsigned i = 0U; i < n; i++ )
    {
        type_line(LINES[i]);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(n, replied_n(">err code=5 msg=locked"), "every verb refused");
    TEST_ASSERT_EQUAL_UINT(0U, replied_n(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_station_probes(), "nothing woke");

    /* The factory reset is not a station verb: it still runs. */
    fake_klog_reset();
    type_line("factory_reset confirm=on");
    TEST_ASSERT_FALSE(replied("msg=locked"));
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));
    down();
}

TEST_CASE("test_factory_reset_refused_while_a_run_goes", "[console_test_dot]")
{
    up(false);
    /* A run the station started; its line is not the console's here. */
    td_verb_plain((uint8_t)TEST_DOT_VERB_RUN);
    td_wake_ok();
    TEST_ASSERT_TRUE(test_dot_busy());

    type_line("factory_reset confirm=on");
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG), "nothing erased");

    td_end();
    TEST_ASSERT_FALSE(test_dot_busy());
    fake_klog_reset();
    type_line("factory_reset confirm=on");
    TEST_ASSERT_FALSE(replied("msg=busy"));
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));
    down();
}

TEST_CASE("test_relays_take_hex_or_decimal_and_refuse_the_forbidden_pair", "[console_test_dot]")
{
    uint8_t b = 0U;

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=0xFE");
    TEST_ASSERT_EQUAL_UINT(1U, requests(&b));
    TEST_ASSERT_EQUAL_HEX8(0xFEU, b);

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=254");
    TEST_ASSERT_EQUAL_UINT(1U, requests(&b));
    TEST_ASSERT_EQUAL_HEX8(0xFEU, b);

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=\"0xFE\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=state"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=0x1FF");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=state"));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_relays state=0xFD");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=5 msg=forbidden"), "R2 on through R1 off");
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));
    down();
}

/* Photos are numbered from one on the wire, as the station asks for
 * them again: there is no photo zero to ask for. */
TEST_CASE("test_photos_only_counts_from_one", "[console_test_dot]")
{
    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_photos only=0");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=only"));
    TEST_ASSERT_EQUAL_UINT(0U, requests(NULL));

    up(false);
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "test_dot_photos only=1");
    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_EQUAL_UINT(1U, requests(NULL));
    down();
}
