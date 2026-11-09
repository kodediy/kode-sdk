/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_clocks.c
 * @brief `clocks` and `test component=clocks`: the crystals, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "unity.h"

#include "qpc.h"

#include "Mockkdrv_net.h"
#include "Mockkhal_clk.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kaos_wire.h"
#include "kdrv_clocks.h"
#include "khal_nvs.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

#define NOMINAL_MHZ   32768000U
#define MAIN_HZ       40000000U

static void console_up(void)
{
    qf_support_init();
    autotest_up();
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
    /* Whatever an earlier file left in flight - test_console_parse.c ends
     * with a command deliberately waiting - is timed out here, so every case
     * starts at an idle prompt. */
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    fake_klog_reset();
    fake_neighbours_reset();
}

static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
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
 * @brief Arrange one board's worth of answers for a single read.
 *
 * @param[in] mux      What the slow-clock mux reports.
 * @param[in] x32k_rc  What the counter returns for the crystal.
 * @param[in] cycles   Which count the caller under test asks for; the verb
 *                     and the test row deliberately differ.
 */
static void expect_read(khal_clk_slow_t mux, kode_status_t x32k_rc,
                        uint32_t cycles)
{
    static uint32_t x32k;
    static uint32_t rc_slow;
    static uint32_t tree;

    x32k    = NOMINAL_MHZ;
    rc_slow = 121300000U;
    tree    = 400000000U;

    khal_clk_main_hz_ExpectAndReturn(MAIN_HZ);
    khal_clk_slow_src_ExpectAndReturn(mux);

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_XTAL32K, cycles, NULL,
                                     x32k_rc);
    khal_clk_measure_IgnoreArg_milli_hz();
    if ( KODE_OK == x32k_rc )
    {
        khal_clk_measure_ReturnThruPtr_milli_hz(&x32k);
    }

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_RC_SLOW, cycles, NULL,
                                     KODE_OK);
    khal_clk_measure_IgnoreArg_milli_hz();
    khal_clk_measure_ReturnThruPtr_milli_hz(&rc_slow);

    for ( int i = 0; 4 > i; ++i )
    {
        khal_clk_tree_hz_ExpectAndReturn(KHAL_CLK_TREE_CPU, NULL, KODE_OK);
        khal_clk_tree_hz_IgnoreArg_clk();
        khal_clk_tree_hz_IgnoreArg_hz();
        khal_clk_tree_hz_ReturnThruPtr_hz(&tree);
    }
}

TEST_CASE("test component=clocks answers its verdict without waiting",
          "[console_clocks]")
{
    console_up();
    Mockkhal_clk_Init();
    expect_read(KHAL_CLK_SLOW_XTAL32K, KODE_OK, KDRV_CLOCKS_CYCLES_FAST);

    type_line("test component=clocks");

    /* Synchronous: the record and the terminator are both already out, with
     * nothing owed by any other AO. */
    TEST_ASSERT_TRUE(replied(">|test=clocks result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

TEST_CASE("a board that fell back to the RC fails the row", "[console_clocks]")
{
    console_up();
    Mockkhal_clk_Init();
    expect_read(KHAL_CLK_SLOW_RC, KODE_TIMEOUT, KDRV_CLOCKS_CYCLES_FAST);

    type_line("test component=clocks");

    TEST_ASSERT_TRUE(replied(">|test=clocks result=fail"));
    TEST_ASSERT_FALSE(replied(">|test=clocks result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

TEST_CASE("the clocks verb prints both crystals and the tree",
          "[console_clocks]")
{
    console_up();
    Mockkhal_clk_Init();
    /* The verb takes the fine count, the row the fast one. If that ever
     * silently became the same number this expectation is what notices. */
    expect_read(KHAL_CLK_SLOW_XTAL32K, KODE_OK, KDRV_CLOCKS_CYCLES_FINE);

    type_line("clocks");

    TEST_ASSERT_TRUE(replied(">|clk=x1 nominal_hz=40000000"));
    TEST_ASSERT_TRUE(replied(">|clk=slow_mux src=xtal32k"));
    TEST_ASSERT_TRUE(replied(">|clk=x3 state=ok millihz=32768000 ppm=+0"));
    TEST_ASSERT_TRUE(replied(">|clk=x1_from_x3 hz=40000000"));
    TEST_ASSERT_TRUE(replied(">|clk=rc_slow state=ok"));
    TEST_ASSERT_TRUE(replied(">|clk=cpu hz="));
    TEST_ASSERT_TRUE(replied(">|clk=rtc_fast hz="));
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

TEST_CASE("a silent crystal prints no ppm and no derived X1",
          "[console_clocks]")
{
    console_up();
    Mockkhal_clk_Init();
    expect_read(KHAL_CLK_SLOW_XTAL32K, KODE_TIMEOUT, KDRV_CLOCKS_CYCLES_FINE);

    type_line("clocks");

    /* Neither number exists when the counter timed out, and printing a stale
     * one would be worse than printing nothing. */
    TEST_ASSERT_TRUE(replied(">|clk=x3 state=silent"));
    TEST_ASSERT_FALSE(replied("ppm="));
    TEST_ASSERT_FALSE(replied("x1_from_x3"));
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkhal_clk_Verify();
    Mockkhal_clk_Destroy();
}

/* ---- The `time` verb ---------------------------------------------------
 * Ownerless and synchronous, like `clocks` above it: the answer is on the
 * wire before the handler returns, so there is no owner queue to read and no
 * verdict to deliver by hand.
 */

TEST_CASE("time on a board that was never told the time says so",
          "[console_clocks]")
{
    console_up();
    fake_time_wall_set(0U);

    type_line("time");

    /* No date, and in particular not 1970: a clock that does not know is a
     * different answer from a clock that knows it is January 1970. */
    TEST_ASSERT_TRUE(replied(">|time state=unset"));
    TEST_ASSERT_FALSE(replied("utc="));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("time set= puts the clock and time reads it back",
          "[console_clocks]")
{
    console_up();
    fake_time_wall_set(0U);

    type_line("time set=\"2026-09-03T21:09:44Z\"");
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_line("time");
    TEST_ASSERT_TRUE(
        replied(">|time state=known utc=\"2026-09-03T21:09:44Z\" source=set"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A time put back after a power cut is late by however long the board was
 * off. It says so, and says where it came from, until something replaces it. */
TEST_CASE("time says approx while the clock is a restored one",
          "[console_clocks]")
{
    console_up();
    fake_nvs_reset();
    fake_time_wall_set(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          khal_nvs_set_u64("kclock", "wall", 1790000000ULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_clocks_wall_restore());

    type_line("time");
    TEST_ASSERT_TRUE(replied(
        ">|time state=approx utc=\"2026-09-21T14:13:20Z\" source=saved"));
    TEST_ASSERT_TRUE(replied(">ok"));

    kdrv_clocks_wall_note_sntp();   /* leave the source as a case expects */
}

TEST_CASE("a timestamp the board will not accept is an error, not a guess",
          "[console_clocks]")
{
    console_up();
    fake_time_wall_set(0U);

    type_line("time set=\"yesterday\"");

    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_FALSE(replied(">ok"));

    /* And the clock is still unset, so the next reader is not misled. */
    fake_klog_reset();
    type_line("time");
    TEST_ASSERT_TRUE(replied(">|time state=unset"));
}

TEST_CASE("a day that does not exist and a year the board cannot hold are errors",
          "[console_clocks]")
{
    static const char *const bad[] = {
        "time set=\"2026-02-29T12:00:00Z\"",
        "time set=\"2026-02-30T00:00:00Z\"",
        "time set=\"2106-02-07T06:28:16Z\"",
        "time set=\"9999-12-31T23:59:59Z\"",
    };

    console_up();
    for ( size_t i = 0U; (sizeof(bad) / sizeof(bad[0])) > i; ++i )
    {
        fake_time_wall_set(0U);
        fake_klog_reset();
        type_line(bad[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err"), bad[i]);
        TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), bad[i]);
    }

    fake_klog_reset();
    type_line("time");
    TEST_ASSERT_TRUE(replied(">|time state=unset"));
}

TEST_CASE("time sync= on a board with no address refuses rather than hanging",
          "[console_clocks]")
{
    /* The success path needs a network and belongs on the bench. What the
     * host can judge is the refusal, which is the branch an operator
     * actually hits - and hitting it must not leave the command in flight
     * waiting for an answer that is never coming. */
    console_up();
    fake_time_wall_set(0U);

    Mockkdrv_net_Init();
    /* The client only exists once there is an address, so with none it
     * refuses - and the console must turn that into an answer rather than
     * leave the command in flight waiting for one that is never coming. */
    kdrv_net_sntp_sync_now_ExpectAndReturn(KODE_DENIED);

    type_line("time sync=on");

    TEST_ASSERT_TRUE(replied(">err"));
    TEST_ASSERT_FALSE(replied(">ok"));

    Mockkdrv_net_Verify();
    Mockkdrv_net_Destroy();
}

TEST_CASE("time sync= says ok when the client took the request",
          "[console_clocks]")
{
    console_up();
    fake_time_wall_set(0U);

    Mockkdrv_net_Init();
    kdrv_net_sntp_sync_now_ExpectAndReturn(KODE_OK);

    type_line("time sync=on");

    /* The answer is "asked", not "got": the server replies on its own thread
     * whenever it feels like it, and the log line is where that lands. */
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));

    Mockkdrv_net_Verify();
    Mockkdrv_net_Destroy();
}
