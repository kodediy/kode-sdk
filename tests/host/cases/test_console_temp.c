/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_temp.c
 * @brief `temp`: the P4's die temperature, one record and `>ok`.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

static void console_up(void)
{
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

TEST_CASE("temp answers the die temperature in degrees, one decimal",
          "[console_temp]")
{
    console_up();
    type_line("temp");
    TEST_ASSERT_TRUE(replied(">|temp=p4 c=41.5"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_temp_set(KODE_OK, 400);
    fake_klog_reset();
    type_line("temp");
    TEST_ASSERT_TRUE_MESSAGE(replied(">|temp=p4 c=40.0"),
        "a whole degree still prints its tenth");
}

TEST_CASE("temp prints a reading below zero with its sign", "[console_temp]")
{
    console_up();
    fake_temp_set(KODE_OK, -5);
    type_line("temp");
    TEST_ASSERT_TRUE_MESSAGE(replied(">|temp=p4 c=-0.5"),
        "half a degree below zero keeps its minus sign");

    fake_temp_set(KODE_OK, -123);
    fake_klog_reset();
    type_line("temp");
    TEST_ASSERT_TRUE(replied(">|temp=p4 c=-12.3"));
}

TEST_CASE("temp refuses when the sensor does not answer", "[console_temp]")
{
    console_up();
    fake_temp_set(KODE_ERROR, 0);
    type_line("temp");
    TEST_ASSERT_TRUE(replied(">|temp=p4 c=-"));
    TEST_ASSERT_TRUE(replied(">err code=5 msg=temp"));
    TEST_ASSERT_FALSE(replied(">ok"));
}
