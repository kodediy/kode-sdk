/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_absent.c
 * @brief A console verb whose owner is not linked answers `>err code=5 msg=absent`.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kqp.h"
#include "owners_support.h"
#include "qf_support.h"

/* The command watchdog, in QP ticks, repeated rather than imported for the
 * reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief What a refused verb answers. */
#define ABSENT_REPLY ">err code=5 msg=absent"

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

/** @brief Type @p line with @p start's owner, alone, absent; it must answer absent. */
static void absent_answers(kaos_start_fn_t start, const char *line)
{
    console_up();
    fake_kaos_linked_reset();
    fake_kaos_absent(start);
    type_line(line);
    TEST_ASSERT_TRUE_MESSAGE(replied(ABSENT_REPLY), line);
    /* Nothing went in flight: the next line is taken at once. */
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("the file verbs answer absent with no AO_Storage", "[console_absent]")
{
    absent_answers(&kaos_storage_start, "ls");
    absent_answers(&kaos_storage_start, "cat path=\"/storage/x\"");
    absent_answers(&kaos_storage_start, "file_get name=\"x\"");
}

TEST_CASE("install answers absent with no owner for its destination",
          "[console_absent]")
{
    absent_answers(&kaos_radio_start, "install src=\"sd:c5.bin\" dst=c5");
    absent_answers(&kaos_appmgr_start, "install src=\"sd:a.bin\" dst=app");
    absent_answers(&kaos_appmgr_start,
                   "install src=\"terminal\" dst=app size=4 crc32=1");
}

TEST_CASE("screenshot answers absent with no AO_Display, and to a file with no "
          "AO_Storage", "[console_absent]")
{
    absent_answers(&kaos_display_start, "screenshot");
    absent_answers(&kaos_storage_start, "screenshot dst=file");
    absent_answers(&kaos_net_start, "screenshot dst=http token=\"t\"");
}

TEST_CASE("time sync and appdata_ls answer absent with no owner", "[console_absent]")
{
    absent_answers(&kaos_net_start, "time sync=on");
    absent_answers(&kaos_appmgr_start, "appdata_ls");
}

TEST_CASE("an owner that is linked answers as before", "[console_absent]")
{
    console_up();
    fake_kaos_absent(&kaos_radio_start);
    type_line("screenshot");
    TEST_ASSERT_FALSE(replied(ABSENT_REPLY));
    TEST_ASSERT_EQUAL_UINT(1U, fake_neighbour_display_posts());
}
