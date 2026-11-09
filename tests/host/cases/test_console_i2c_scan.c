/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_i2c_scan.c
 * @brief `i2c_scan`: the sweep is probed in short steps, the records are not.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "unity.h"

#include "qpc.h"

#include "Mockkhal_i2c.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kaos_wire.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks, repeated rather than imported for the
 * reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/* One step per tick is paced by a 10 ms gap, which is one QP tick here. More
 * ticks than the sweep has steps (14 at eight addresses a step), and fewer
 * than the command watchdog. */
#define SCAN_TICKS  64U

/* The sweep's first and last address, and the two that answer: one in the
 * first chunk (0x08..0x0F) and one in the second (0x10..0x17). */
#define ADDR_FIRST    0x08U
#define ADDR_LAST     0x77U
#define ADDR_CHUNK_1  0x0AU
#define ADDR_CHUNK_2  0x12U

static unsigned s_probed;
static unsigned s_last_probed;

static bool on_probe(uint16_t addr, uint32_t timeout_ms, int cmock_num_calls)
{
    (void)timeout_ms; (void)cmock_num_calls;
    s_probed += 1U;
    s_last_probed = addr;
    return (ADDR_CHUNK_1 == addr) || (ADDR_CHUNK_2 == addr);
}

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

/** @brief How many replies contain @p needle. */
static unsigned reply_count(const char *needle)
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

/** @brief The index of the first reply containing @p needle, or -1. */
static int reply_index(const char *needle)
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

TEST_CASE("test_i2c_scan_records_unchanged_across_steps", "[console_i2c_scan]")
{
    console_up();
    Mockkhal_i2c_Init();
    khal_i2c_init_IgnoreAndReturn(KODE_OK);
    khal_i2c_probe_StubWithCallback(&on_probe);
    s_probed = 0U;
    s_last_probed = 0U;

    type_line("i2c_scan");

    /* The first step probed one chunk and no more: the device in the first
     * chunk is out, the one in the second is not yet, and the command is
     * still open. */
    TEST_ASSERT_EQUAL_UINT(8U, s_probed);
    TEST_ASSERT_TRUE(replied(">|addr=0x0A"));
    TEST_ASSERT_FALSE(replied(">|addr=0x12"));
    TEST_ASSERT_FALSE(replied(">ok"));

    for ( unsigned i = 0U; SCAN_TICKS > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }

    /* Every address once, the second chunk's device reported, the records in
     * address order, the count after them and a single `>ok` last. */
    TEST_ASSERT_EQUAL_UINT((ADDR_LAST - ADDR_FIRST) + 1U, s_probed);
    TEST_ASSERT_EQUAL_UINT(ADDR_LAST, s_last_probed);
    TEST_ASSERT_EQUAL_UINT(1U, reply_count(">|addr=0x0A"));
    TEST_ASSERT_EQUAL_UINT(1U, reply_count(">|addr=0x12"));
    TEST_ASSERT_EQUAL_UINT(2U, reply_count(">|addr="));
    TEST_ASSERT_TRUE(reply_index(">|addr=0x0A") < reply_index(">|addr=0x12"));
    TEST_ASSERT_TRUE(reply_index(">|addr=0x12") < reply_index(">|found=2"));
    TEST_ASSERT_TRUE(reply_index(">|found=2") < reply_index(">ok"));
    TEST_ASSERT_EQUAL_UINT(1U, reply_count(">ok"));

    Mockkhal_i2c_Destroy();
}
