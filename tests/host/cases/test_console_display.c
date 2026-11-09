/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_display.c
 * @brief `display_info`, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "unity.h"

#include "qpc.h"

#include "Mockkdrv_display.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

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
    /* Whatever an earlier file left in flight is timed out here, so every
     * case starts at an idle prompt. */
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
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

TEST_CASE("display_info prints the link counters", "[console_display]")
{
    kdrv_display_link_t link;

    console_up();
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    (void)memset(&link, 0, sizeof(link));
    link.frames    = 594U;
    link.underruns = 3U;
    link.dpi       = 1U;
    link.phy       = 2U;
    link.st0       = 0x00010000U;
    link.st1       = 0x00080000U;
    link.missed    = 4U;
    kdrv_display_link_ExpectAnyArgs();
    kdrv_display_link_ReturnThruPtr_out(&link);

    type_line("display_info");
    TEST_ASSERT_TRUE(replied(">|display frames=594 underruns=3 dpi=1 ack=0 "
                             "phy=2 other=0 st0=0x00010000 st1=0x00080000 "
                             "missed=4"));
    TEST_ASSERT_TRUE(replied(">ok"));
    Mockkdrv_display_Verify();
    Mockkdrv_display_Destroy();
}

TEST_CASE("display_info prints zeros on a link that has seen nothing",
          "[console_display]")
{
    kdrv_display_link_t link;

    console_up();
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    (void)memset(&link, 0, sizeof(link));
    kdrv_display_link_ExpectAnyArgs();
    kdrv_display_link_ReturnThruPtr_out(&link);

    type_line("display_info");
    TEST_ASSERT_TRUE(replied(">|display frames=0 underruns=0 dpi=0 ack=0 "
                             "phy=0 other=0 st0=0x00000000 st1=0x00000000 "
                             "missed=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
    Mockkdrv_display_Verify();
    Mockkdrv_display_Destroy();
}
