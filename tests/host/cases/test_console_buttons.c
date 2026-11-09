/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_buttons.c
 * @brief `test component=buttons`: the guided button test, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kdrv_buttons.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

#define FOREIGN_ORIGIN 7U   /**< The number of a client other than the autotest. */

/* The command watchdog and the button row's own budget, in QP ticks.
 * Repeated rather than imported for the same reason test_console_flow.c
 * repeats it: six steps, each worst case four attempts (two wrong presses
 * and a double forgiven, then the right one) of twenty seconds to press and
 * three to let go, and five seconds of margin. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)
#define BTN_BUDGET_TICKS   ((((6U * 4U * (20000U + 3000U)) + 5000U) * 100U) / 1000U)

static void console_up(void)
{
    const QEvt *e;

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
    qf_support_give_queue(AO_Buttons);
    while ( xQueueReceive(AO_Buttons->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
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

/** @brief Was the guided test asked of AO_Buttons? */
static bool posted(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Buttons->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(BUTTONS_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief One step of the script, as AO_Buttons publishes it for the run of @p origin, @p ms into its press. */
static void step_timed(uint8_t origin, uint8_t phase, uint8_t n, kdrv_button_t button, uint8_t pressed,
                       uint16_t ms)
{
    ButtonsTestEvt t;

    t.super   = (QEvt)QEVT_INITIALIZER(BTN_TEST_SIG);
    t.phase   = phase;
    t.step    = n;
    t.total   = 6U;
    t.button  = (uint8_t)button;
    t.pressed = pressed;
    t.origin  = origin;
    t.ms      = ms;
    QASM_DISPATCH(AO_Autotest, &t.super, 0U);
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

/** @brief One step of the script, as AO_Buttons publishes it for the run of @p origin. */
static void step_from(uint8_t origin, uint8_t phase, uint8_t n, kdrv_button_t button, uint8_t pressed)
{
    step_timed(origin, phase, n, button, pressed, 0U);
}

/** @brief One step of the script, as AO_Buttons publishes it for the autotest's own run. */
static void step(uint8_t phase, uint8_t n, kdrv_button_t button, uint8_t pressed)
{
    step_from(TEST_ORIGIN_AUTOTEST, phase, n, button, pressed);
}

/** @brief Deliver the verdict to the sequencer and the console. */
static void verdict(bool ok)
{
    TestResultEvt r;

    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", "buttons");
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

TEST_CASE("test component=buttons asks AO_Buttons and prints each prompt",
          "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());
    TEST_ASSERT_FALSE(replied(">ok"));

    step(BTN_TEST_PROMPT, 1U, KDRV_BTN_LEFT, 0U);
    TEST_ASSERT_TRUE(replied(">|step=1 of=6 press=pad-left"));
    TEST_ASSERT_FALSE(replied(">ok"));

    verdict(true);
    TEST_ASSERT_TRUE(replied(">|test=buttons result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* Another client's run is not this command's: its steps print nothing. */
TEST_CASE("test_foreign_steps_print_nothing", "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    step_from(FOREIGN_ORIGIN, BTN_TEST_PROMPT, 1U, KDRV_BTN_LEFT, 0U);
    step_from(FOREIGN_ORIGIN, BTN_TEST_PASS, 1U, KDRV_BTN_LEFT, 0x04U);
    step_from(FOREIGN_ORIGIN, BTN_TEST_GIVEUP, 2U, KDRV_BTN_BOT, 0x04U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|step="), "a foreign step prints no record");

    step(BTN_TEST_PROMPT, 1U, KDRV_BTN_LEFT, 0U);
    TEST_ASSERT_TRUE_MESSAGE(replied(">|step=1 of=6 press=pad-left"), "its own step still does");
    verdict(true);
}

/* Nor does the verdict of another client's run close the command. */
TEST_CASE("test_foreign_verdict_closes_nothing", "[console_buttons]")
{
    TestResultEvt r;

    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", "buttons");
    r.ok     = false;
    r.origin = FOREIGN_ORIGIN;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=buttons result="), "no row record");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the command is still in flight");

    verdict(true);
    TEST_ASSERT_TRUE(replied(">|test=buttons result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* Every other command dies after five seconds without its answer. An
 * operator finding the right button takes longer than that, so the row
 * runs on its own budget - the whole script's worst case - and the
 * watchdog stays quiet through the prompts. */
TEST_CASE("the row's budget keeps the command alive past the five-second watchdog",
          "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    step(BTN_TEST_PROMPT, 1U, KDRV_BTN_LEFT, 0U);
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 100U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=6"), "the budget covers the first step");

    step(BTN_TEST_PASS, 1U, KDRV_BTN_LEFT, 0x04U);
    step(BTN_TEST_PROMPT, 2U, KDRV_BTN_BOT, 0U);
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 100U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=6"), "and the second");

    /* But a run that stalls for good is timed out at the budget, not
     * never - as one record, with the command closed, not as an error. */
    (void)qf_support_tick(AO_Console, BTN_BUDGET_TICKS - (2U * (CMD_TIMEOUT_TICKS + 100U)) - 1U);
    TEST_ASSERT_FALSE(replied(">|test=buttons"));
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE(replied(">|test=buttons result=timeout"));
    TEST_ASSERT_FALSE(replied(">err code=6"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("every outcome of a step prints as a record", "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    step_timed(TEST_ORIGIN_AUTOTEST, BTN_TEST_PASS, 1U, KDRV_BTN_LEFT, 0x04U, 250U);
    TEST_ASSERT_TRUE(replied(">|step=1 button=pad-left result=pass ms=250"));

    step(BTN_TEST_GIVEUP, 2U, KDRV_BTN_BOT,
         (uint8_t)((1U << KDRV_BTN_UP) | (1U << KDRV_BTN_DOWN)));
    TEST_ASSERT_TRUE(replied(">|step=2 button=bottom result=giveup pressed=pad-up+pad-down"));

    step(BTN_TEST_TIMEOUT, 3U, KDRV_BTN_DOWN, 0U);
    TEST_ASSERT_TRUE(replied(">|step=3 button=pad-down result=timeout"));

    step(BTN_TEST_STUCK, 4U, KDRV_BTN_TOP, (uint8_t)(1U << KDRV_BTN_TOP));
    TEST_ASSERT_TRUE(replied(">|step=4 button=top result=stuck pressed=top"));

    step(BTN_TEST_DONE, 6U, KDRV_BTN_TOP, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|step=6"), "done is not a step of its own");

    verdict(false);
    TEST_ASSERT_TRUE(replied(">|test=buttons result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A forgiven slip prints as a record of its own, naming what was pressed. */
TEST_CASE("test_wrong_record", "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    step(BTN_TEST_WRONG, 1U, KDRV_BTN_LEFT, (uint8_t)(1U << KDRV_BTN_DOWN));
    TEST_ASSERT_TRUE(replied(">|step=1 button=pad-left result=wrong pressed=pad-down ms="));

    step(BTN_TEST_DOUBLE, 1U, KDRV_BTN_LEFT, (uint8_t)((1U << KDRV_BTN_LEFT) | (1U << KDRV_BTN_DOWN)));
    TEST_ASSERT_TRUE(replied(">|step=1 button=pad-left result=double pressed=pad-down+pad-left ms="));

    step(BTN_TEST_DOUBLEFAIL, 1U, KDRV_BTN_LEFT, (uint8_t)((1U << KDRV_BTN_LEFT) | (1U << KDRV_BTN_DOWN)));
    TEST_ASSERT_TRUE(replied("result=doublefail pressed=pad-down+pad-left ms="));

    step(BTN_TEST_HELD, 2U, KDRV_BTN_BOT, (uint8_t)(1U << KDRV_BTN_BOT));
    TEST_ASSERT_TRUE(replied(">|step=2 button=bottom result=held pressed=bottom ms="));

    verdict(false);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The third wrong press ends the run, and the verdict that follows is a fail. */
TEST_CASE("test_giveup_record", "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    step(BTN_TEST_GIVEUP, 1U, KDRV_BTN_LEFT, (uint8_t)(1U << KDRV_BTN_DOWN));
    TEST_ASSERT_TRUE(replied(">|step=1 button=pad-left result=giveup pressed=pad-down ms="));
    step(BTN_TEST_DONE, 6U, KDRV_BTN_UP, 0U);
    verdict(false);
    TEST_ASSERT_TRUE(replied(">|test=buttons result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* An unattended run: every step went unanswered, and AO_Buttons can only say
 * "not a pass". Nobody pressed anything wrong, so the row was never judged,
 * and printing it as a fail would mark a good board bad. */
TEST_CASE("a row whose steps only went unanswered is a timeout, not a fail",
          "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    step(BTN_TEST_TIMEOUT, 1U, KDRV_BTN_LEFT, 0U);
    step(BTN_TEST_TIMEOUT, 2U, KDRV_BTN_BOT, 0U);
    step(BTN_TEST_DONE, 6U, KDRV_BTN_TOP, 0U);
    verdict(false);

    TEST_ASSERT_TRUE(replied(">|test=buttons result=timeout"));
    TEST_ASSERT_FALSE(replied(">|test=buttons result=fail"));
    TEST_ASSERT_TRUE(replied(">|summary pass=0 fail=1 timeout=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* One wrong answer outweighs any number of silent steps: the operator was
 * there, and the pad said something other than what was asked. */
TEST_CASE("a step answered wrong keeps the row a fail, whatever else timed out",
          "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    step(BTN_TEST_TIMEOUT, 1U, KDRV_BTN_LEFT, 0U);
    step(BTN_TEST_GIVEUP, 2U, KDRV_BTN_BOT, (uint8_t)(1U << KDRV_BTN_UP));
    verdict(false);

    TEST_ASSERT_TRUE(replied(">|test=buttons result=fail"));
    TEST_ASSERT_TRUE(replied(">|summary pass=0 fail=1 timeout=0"));
}

TEST_CASE("the run ends with one summary record before >ok", "[console_buttons]")
{
    console_up();
    type_line("test component=buttons");
    TEST_ASSERT_TRUE(posted());

    verdict(true);
    TEST_ASSERT_TRUE(replied(">|summary pass=1 fail=0 timeout=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
}
