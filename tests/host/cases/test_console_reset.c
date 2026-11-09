/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_reset.c
 * @brief `factory_reset confirm=on`: the console asks AO_Boot and reports how far the reset got.
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

static void console_up(void)
{
    qf_support_init();
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
    qf_support_give_queue(AO_Boot);
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Empty AO_Boot's mailbox: the verb posts its request there. */
static void boot_mailbox_drain(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Boot);
    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

static void type_from(const char *text, uint8_t src)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

static void type_line(const char *text)
{
    type_from(text, (uint8_t)KODE_CONSOLE_SRC_SERIAL);
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

/** @brief The index of the first reply holding @p needle, or -1. */
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

/** @brief Whatever the console posted to AO_Boot, or a zero signal. */
static enum_t posted(void *out, size_t size)
{
    const QEvt *e = NULL;
    enum_t sig = 0;

    if ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        sig = e->sig;
        (void)memcpy(out, e, size);
        QF_gc(e);
    }
    return sig;
}

static bool nothing_posted(void)
{
    FactoryResetReqEvt q;
    return posted(&q, sizeof(q)) == 0;
}

/** @brief Deliver the answer AO_Boot gives the console. */
static void boot_answers(uint8_t step, uint8_t status)
{
    FactoryResetRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super  = (QEvt)QEVT_INITIALIZER(FACTORY_RESET_RSP_SIG);
    a.step   = step;
    a.status = status;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/** @brief Type the verb and take the request it posts. */
static void ask_and_take(FactoryResetReqEvt *q)
{
    type_line("factory_reset confirm=on");
    TEST_ASSERT_EQUAL_INT((int)FACTORY_RESET_REQ_SIG,
                          (int)posted(q, sizeof(*q)));
}

TEST_CASE("factory_reset without confirm asks nothing", "[console_reset]")
{
    console_up();
    boot_mailbox_drain();

    type_line("factory_reset");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=confirm"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("factory_reset confirm=off asks nothing", "[console_reset]")
{
    console_up();
    boot_mailbox_drain();

    type_line("factory_reset confirm=off");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=confirm"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("factory_reset is refused over BLE and asks nothing", "[console_reset]")
{
    console_up();
    boot_mailbox_drain();

    type_from("factory_reset confirm=on", (uint8_t)KODE_CONSOLE_SRC_BLE);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("factory_reset is refused over the channel and asks nothing",
          "[console_reset]")
{
    console_up();
    boot_mailbox_drain();

    type_from("factory_reset confirm=on", (uint8_t)KODE_CONSOLE_SRC_NET);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=net"));
    TEST_ASSERT_TRUE(nothing_posted());
}

TEST_CASE("factory_reset confirm=on asks AO_Boot for the restart end",
          "[console_reset]")
{
    FactoryResetReqEvt q;

    console_up();
    boot_mailbox_drain();

    ask_and_take(&q);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBOOT_RESET_RESTART, q.end);
    TEST_ASSERT_EQUAL_PTR(AO_Console, q.who);
    TEST_ASSERT_FALSE(replied(">ok"));

    /* Close the command: the console outlives the case. */
    boot_answers((uint8_t)KBOOT_RESET_STEP_FIND, (uint8_t)KODE_DENIED);
}

TEST_CASE("factory_reset prints done then ok when every step did",
          "[console_reset]")
{
    FactoryResetReqEvt q;

    console_up();
    boot_mailbox_drain();

    ask_and_take(&q);
    boot_answers((uint8_t)KBOOT_RESET_STEP_DONE, (uint8_t)KODE_OK);
    TEST_ASSERT_TRUE(replied(">|factory_reset result=done"));
    TEST_ASSERT_TRUE(reply_index(">ok") > reply_index(">|factory_reset result=done"));
    TEST_ASSERT_TRUE(reply_index(">|factory_reset result=done") >= 0);
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("factory_reset names the step that failed", "[console_reset]")
{
    FactoryResetReqEvt q;

    console_up();
    boot_mailbox_drain();

    ask_and_take(&q);
    boot_answers((uint8_t)KBOOT_RESET_STEP_WIPE, (uint8_t)KODE_ERROR);
    TEST_ASSERT_TRUE(replied(">|factory_reset step=wipe status=1"));
    TEST_ASSERT_TRUE(reply_index(">err code=5 msg=wipe")
                     > reply_index(">|factory_reset step=wipe status=1"));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("factory_reset names find when nothing was erased", "[console_reset]")
{
    FactoryResetReqEvt q;

    console_up();
    boot_mailbox_drain();

    ask_and_take(&q);
    boot_answers((uint8_t)KBOOT_RESET_STEP_FIND, (uint8_t)KODE_DENIED);
    TEST_ASSERT_TRUE(replied(">|factory_reset step=find status=3"));
    TEST_ASSERT_TRUE(replied(">err code=5 msg=find"));
}

TEST_CASE("factory_reset names the closing request when it failed",
          "[console_reset]")
{
    FactoryResetReqEvt q;

    console_up();
    boot_mailbox_drain();

    /* The restart end could not write the welcome app's boot request: the
     * board stays up, and the step that failed is the closing request, not
     * "done". */
    ask_and_take(&q);
    boot_answers((uint8_t)KBOOT_RESET_STEP_REQUEST, (uint8_t)KODE_ERROR);
    TEST_ASSERT_TRUE(replied(">|factory_reset step=request status=1"));
    TEST_ASSERT_TRUE(replied(">err code=5 msg=request"));
    TEST_ASSERT_FALSE(replied("result=done"));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("factory_reset frees the console for the next line", "[console_reset]")
{
    FactoryResetReqEvt q;

    console_up();
    boot_mailbox_drain();

    ask_and_take(&q);
    type_line("factory_reset confirm=on");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4"),
                             "a second line while one is in flight is busy");
    TEST_ASSERT_TRUE(nothing_posted());

    boot_answers((uint8_t)KBOOT_RESET_STEP_NVS, (uint8_t)KODE_ERROR);
    fake_klog_reset();
    type_line("factory_reset confirm=on");
    TEST_ASSERT_EQUAL_INT((int)FACTORY_RESET_REQ_SIG,
                          (int)posted(&q, sizeof(q)));

    /* Close the command: the console outlives the case. */
    boot_answers((uint8_t)KBOOT_RESET_STEP_DONE, (uint8_t)KODE_OK);
}

/* The console's deadline is the sum of the budgets, with the find counted
 * twice (an install asks it again) and the drain before the restart. */
TEST_CASE("factory_reset waits for every budget it can be given", "[console_reset]")
{
    FactoryResetReqEvt q;

    TEST_ASSERT_EQUAL_UINT32(
        (2U * KODE_RESET_FIND_MS) + KODE_RESET_INSTALL_MS + KODE_RESET_APPS_MS
            + KODE_RESET_LINK_MS + KODE_RESET_WIPE_MS + KODE_RESET_NVS_MS
            + KODE_RESTART_DRAIN_MS,
        KODE_RESET_CONSOLE_MS);

    console_up();
    boot_mailbox_drain();
    ask_and_take(&q);

    (void)qf_support_tick(AO_Console, QF_SUPPORT_STEPS(KODE_RESET_CONSOLE_MS) - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=6"), "not before the last budget");
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=6"), "and then no longer");
}

TEST_CASE("factory_reset on a dry event pool is refused and leaves the console free",
          "[console_reset]")
{
    const QEvt *held[KQP_SMALL_POOL_COUNT + 1U];
    unsigned    n = 0U;
    FactoryResetReqEvt q;

    console_up();
    boot_mailbox_drain();

    /* The request is an event of the small pool: a dry pool drops it. */
    while ( (unsigned)(KQP_SMALL_POOL_COUNT + 1U) > n )
    {
        FactoryResetReqEvt * const e =
            Q_NEW_X(FactoryResetReqEvt, 0U, FACTORY_RESET_REQ_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT((unsigned)KQP_SMALL_POOL_COUNT, n);

    type_line("factory_reset confirm=on");
    for ( unsigned i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
    TEST_ASSERT_TRUE(replied(">err code=5 msg=factory_reset"));
    TEST_ASSERT_TRUE(nothing_posted());

    fake_klog_reset();
    ask_and_take(&q);
    boot_answers((uint8_t)KBOOT_RESET_STEP_DONE, (uint8_t)KODE_OK);
    TEST_ASSERT_TRUE(replied(">ok"));
}
