/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_ps.c
 * @brief `ps` and `test component=mem`: the internal pool, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kaos_wire.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks, repeated rather than imported for the
 * reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/* `ps` is paced, one slice a tick. More ticks than it has slices on any
 * build, and fewer than the command watchdog. */
#define PS_TICKS  64U

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

TEST_CASE("ps prints the internal pool's low mark and its refusals",
          "[console_ps]")
{
    console_up();
    fake_heap_set_alloc_failures(3U);

    type_line("ps");
    for ( unsigned i = 0U; PS_TICKS > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }

    /* The four internal figures in one record and in this order: free now,
     * largest block now, lowest free since boot, refusals since boot. */
    TEST_ASSERT_TRUE(replied(
        "int_free=32768 int_max=16384 int_low=98304 alloc_fail=3"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_heap_set_alloc_failures(0U);
}

/** @brief Stack of the worker the record case starts. */
#define PS_WORKER_STACK  8192U

/** @brief What that worker's stack reads as never used. */
#define PS_WORKER_FREE   3000U

TEST_CASE("ps lists every started worker with its stack and what is left",
          "[console_ps]")
{
    KqpWorker w;

    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kqp_worker_start(&w, ao_spy(), "ps_probe", PS_WORKER_STACK, 4U, 1U,
                         true));
    fake_khal_worker_set_stack_free_for("ps_probe", PS_WORKER_FREE);

    type_line("ps");
    for ( unsigned i = 0U; PS_TICKS > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }

    TEST_ASSERT_TRUE(replied(">|worker name=\"ps_probe\" stack=8192 free=3000"));
    TEST_ASSERT_TRUE(replied("int_low=98304 alloc_fail=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ps on a board that refused nothing says alloc_fail=0",
          "[console_ps]")
{
    console_up();

    type_line("ps");
    for ( unsigned i = 0U; PS_TICKS > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }

    TEST_ASSERT_TRUE(replied("int_low=98304 alloc_fail=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("test component=mem passes on a board that kept its floor and refused nothing",
          "[console_ps]")
{
    console_up();
    fake_heap_set_internal_low(KODE_MEM_TEST_FLOOR_BYTES);
    fake_heap_set_alloc_failures(0U);

    type_line("test component=mem");

    /* Synchronous, like the crystals' row: nobody owns the heap. */
    TEST_ASSERT_TRUE(replied(">|test=mem result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_system_reset();
}

TEST_CASE("test component=mem fails when the pool went under its floor",
          "[console_ps]")
{
    console_up();
    fake_heap_set_internal_low(KODE_MEM_TEST_FLOOR_BYTES - 1U);

    type_line("test component=mem");

    TEST_ASSERT_TRUE(replied(">|test=mem result=fail"));
    TEST_ASSERT_FALSE(replied(">|test=mem result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_system_reset();
}

TEST_CASE("test component=mem fails when the heap refused an allocation",
          "[console_ps]")
{
    console_up();
    fake_heap_set_alloc_failures(1U);

    type_line("test component=mem");

    TEST_ASSERT_TRUE(replied(">|test=mem result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_system_reset();
}

/* ---- The PSRAM row ------------------------------------------------------- */

/** @brief Where the first line holding @p needle was printed, or -1. */
static int replied_at(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return (int)i;
        }
    }
    return -1;
}

/* The numbers come first and the verdict after them, so whoever reads the
 * record - the line, support over the Kode channel - has what the row found
 * by the time it says pass or fail. The fake's PSRAM is the reserve and two
 * blocks: 4096 KB free, 2048 KB tested. */
TEST_CASE("test component=psram prints its record before the verdict",
          "[console_ps]")
{
    console_up();
    fake_psram_reset();

    type_line("test component=psram");
    autotest_psram_run();

    int const rec = replied_at(">|psram tested_kb=2048 free_kb=4096 errors=0 "
                               "first=- wrote=- read=-");
    int const res = replied_at(">|test=psram result=pass");
    TEST_ASSERT_TRUE(0 <= rec);
    TEST_ASSERT_TRUE(0 <= res);
    TEST_ASSERT_TRUE_MESSAGE(rec < res, "the record comes before the verdict");
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A fault is printed with what diagnoses it: the first word, what it should
 * have held and what it held, each as eight hex digits. */
TEST_CASE("a psram fault prints the first address and both values",
          "[console_ps]")
{
    PsramTestEvt e;

    console_up();
    fake_psram_reset();
    type_line("test component=psram");

    (void)memset(&e, 0, sizeof(e));
    e.super     = (QEvt)QEVT_INITIALIZER(PSRAM_TEST_SIG);
    e.tested_kb = 29696U;
    e.free_kb   = 31744U;
    e.errors    = 3U;
    e.first     = 0x48001000U;
    e.wrote     = 0x48001000U;
    e.read      = 0x48001100U;
    QASM_DISPATCH(AO_Console, &e.super, 0U);

    TEST_ASSERT_TRUE(replied(">|psram tested_kb=29696 free_kb=31744 errors=3 "
                             "first=0x48001000 wrote=0x48001000 "
                             "read=0x48001100"));

    autotest_psram_run();   /* the row itself, to its end */
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* QP keeps its registry of started Active Objects private; a case reads and
 * writes it here to make one AO "not started" for the length of a ps. */
extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

TEST_CASE("ps lists the Active Objects that started and none that did not",
          "[console_ps]")
{
    char console_row[32];

    console_up();
    /* AO_Radio as an app that never linked it: no AO at its priority. */
    QActive * const radio = QActive_registry_[KODE_PRIO_RADIO];
    QActive_registry_[KODE_PRIO_RADIO] = NULL;

    type_line("ps");
    for ( unsigned i = 0U; PS_TICKS > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    QActive_registry_[KODE_PRIO_RADIO] = radio;

    (void)snprintf(console_row, sizeof(console_row), ">|prio=%u ao=Console ",
                   (unsigned)KODE_PRIO_CONSOLE);
    TEST_ASSERT_TRUE(replied(console_row));
    TEST_ASSERT_FALSE(replied(" ao=Radio "));
    TEST_ASSERT_TRUE(replied(">ok"));
}
