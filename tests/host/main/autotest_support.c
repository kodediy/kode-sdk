/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file autotest_support.c
 * @brief See autotest_support.h.
 */
#include <stdio.h>
#include <string.h>

#include "qpc.h"

#include "autotest_support.h"
#include "kaos.h"
#include "kqp.h"
#include "owners_support.h"
#include "qf_support.h"

void autotest_up(void)
{
    qf_support_init();
    owners_up();   /* what the owners' starts register, the rows among it */
    if ( qf_support_claim_ao(AO_Autotest) )
    {
        Autotest_ctor();
        qf_support_give_queue(AO_Autotest);
        AO_Autotest->prio  = KODE_PRIO_AUTOTEST;
        AO_Autotest->pthre = KODE_PRIO_AUTOTEST;
        QActive_register_(AO_Autotest);
        QASM_INIT(AO_Autotest, (void *)0, 0U);
        qf_support_relay(AO_Autotest);
    }
}

void autotest_settle(void)
{
    if ( NULL != AO_Console->eQueue )
    {
        (void)qf_support_settle(AO_Console);
    }
    else
    {
        (void)qf_support_settle(AO_Autotest);
    }
}

/** @brief Steps enough for the PSRAM row over the fake's two blocks. */
#define AUTOTEST_PSRAM_STEPS  100U

void autotest_psram_run(void)
{
    if ( NULL != AO_Console->eQueue )
    {
        (void)qf_support_tick(AO_Console, AUTOTEST_PSRAM_STEPS);
    }
    else
    {
        (void)qf_support_tick(AO_Autotest, AUTOTEST_PSRAM_STEPS);
    }
}

void autotest_typed(const char *line)
{
    if ( (NULL != line) && (strncmp(line, "test", 4U) == 0)
        && (('\0' == line[4]) || (' ' == line[4])) )
    {
        autotest_settle();
    }
}

void autotest_verdict(const char *name, bool ok)
{
    TestResultEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    e.ok = ok;
    e.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &e.super, 0U);
    if ( NULL != AO_Console->eQueue )
    {
        QASM_DISPATCH(AO_Console, &e.super, 0U);
    }
    autotest_settle();
}
