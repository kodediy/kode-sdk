/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_tp.c
 * @brief `tp`, `tp_help` and `test component=tp`: the touch controller's fields and guided test.
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
#include "kaos_wire.h"
#include "kdrv_cst820.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

#define FOREIGN_ORIGIN 7U   /**< The number of a client other than the autotest. */

/* The command watchdog and the touch row's own budget, in QP ticks.
 * Repeated rather than imported for the same reason test_console_flow.c
 * repeats it: two targets, each with three tries of twenty seconds, and
 * five seconds of margin. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)
#define TP_BUDGET_TICKS    ((((2U * 3U * 20000U) + 5000U) * 100U) / 1000U)
/* What it takes to expire whatever an earlier file left in flight. Not the
 * watchdog: a `test` row waits on its own budget, and the card's is the
 * better part of a minute. */
#define ANY_BUDGET_TICKS   ((200000U * 100U) / 1000U)

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
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);
    fake_klog_reset();
    /* AO_Touch's queue is the mailbox the verbs post into; emptied here so a
     * request left by an earlier case cannot be mistaken for this one's. */
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
 * @brief Take the request the console posted to AO_Touch, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted(TouchFieldReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Touch->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(TOUCH_FIELD_REQ_SIG, e->sig);
    *out = *(const TouchFieldReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Touch's answer to the console, the way it posts it. */
static void answer(uint8_t op, uint16_t field, kode_status_t rc, int32_t value)
{
    TouchFieldEvt a;

    a.super = (QEvt)QEVT_INITIALIZER(TOUCH_FIELD_RSP_SIG);
    a.op    = op;
    a.field = field;
    a.rc    = (uint8_t)rc;
    a.value = value;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- The verb -------------------------------------------------------------- */

TEST_CASE("tp with no keys asks for the whole chip and closes on the answer",
          "[console_tp]")
{
    TouchFieldReqEvt q;

    console_up();
    type_line("tp");
    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_DUMP, q.op);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.who, "the answer must come back here");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the owner answered");

    answer(FIELD_OP_DUMP, 0U, KODE_OK, 0);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|chip="), "a dump is log lines, not a record");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("tp field= asks the owner for that field and answers a record",
          "[console_tp]")
{
    TouchFieldReqEvt q;

    console_up();
    type_line("tp field=\"auto_sleep_time\"");
    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_GET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_CST_AUTO_SLEEP_TIME, q.field);

    answer(FIELD_OP_GET, (uint16_t)KDRV_CST_AUTO_SLEEP_TIME, KODE_OK, 2);
    TEST_ASSERT_TRUE(replied(">|chip=tp field=\"auto_sleep_time\" value=2"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("tp field= value= asks for a write and answers the readback",
          "[console_tp]")
{
    TouchFieldReqEvt q;

    console_up();
    type_line("tp field=\"dis_auto_sleep\" value=1");
    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_SET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_CST_DIS_AUTO_SLEEP, q.field);
    TEST_ASSERT_EQUAL_INT32(1, q.value);

    answer(FIELD_OP_SET, (uint16_t)KDRV_CST_DIS_AUTO_SLEEP, KODE_OK, 1);
    TEST_ASSERT_TRUE(replied(">|chip=tp field=\"dis_auto_sleep\" value=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A name the driver never printed is refused here: nothing is asked of the
 * owner, and the answer names the key rather than the verb. */
TEST_CASE("a field nobody printed is refused before the owner is asked",
          "[console_tp]")
{
    TouchFieldReqEvt q;

    console_up();
    type_line("tp field=\"p9\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
    TEST_ASSERT_FALSE(posted(&q));

    fake_klog_reset();
    type_line("tp field=\"ichg_ma\"");      /* the PMIC's, not the touch's */
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
    TEST_ASSERT_FALSE(posted(&q));
}

/* The owner's verdicts, on the wire: a field that cannot be written is a bad
 * value naming the field; a chip that did not answer - asleep, on this part
 * - is a rejection naming the verb. */
TEST_CASE("a refusal from the owner names the field, a failure names the verb",
          "[console_tp]")
{
    TouchFieldReqEvt q;

    console_up();
    type_line("tp field=\"chip_id\" value=1");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_SET, (uint16_t)KDRV_CST_CHIP_ID, KODE_DENIED, 1);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=chip_id"));

    fake_klog_reset();
    type_line("tp field=\"x\"");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_GET, (uint16_t)KDRV_CST_X, KODE_ERROR, 0);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=tp"));
}

/* Four chips share one answer shape and one inflight slot. Somebody else's
 * answer arriving while `tp` waits - a power chip's, published for an app
 * that asked; the expander's, posted to us by mistake - must not close it. */
TEST_CASE("another chip's answer does not close a touch question", "[console_tp]")
{
    TouchFieldReqEvt q;
    PowerFieldEvt    stray_power;
    IoexpFieldEvt    stray_ioexp;

    console_up();
    type_line("tp field=\"fingers\"");
    TEST_ASSERT_TRUE(posted(&q));

    stray_power.super = (QEvt)QEVT_INITIALIZER(POWER_FIELD_SIG);
    stray_power.chip  = POWER_CHIP_PMIC;
    stray_power.op    = FIELD_OP_GET;
    stray_power.field = 0U;
    stray_power.rc    = (uint8_t)KODE_OK;
    stray_power.value = 1;
    QASM_DISPATCH(AO_Console, &stray_power.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "a power chip's answer");

    stray_ioexp.super = (QEvt)QEVT_INITIALIZER(IOEXP_FIELD_RSP_SIG);
    stray_ioexp.op    = FIELD_OP_GET;
    stray_ioexp.field = 0U;
    stray_ioexp.rc    = (uint8_t)KODE_OK;
    stray_ioexp.value = 1;
    QASM_DISPATCH(AO_Console, &stray_ioexp.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the expander's answer");

    answer(FIELD_OP_GET, (uint16_t)KDRV_CST_FINGERS, KODE_OK, 1);
    TEST_ASSERT_TRUE(replied(">|chip=tp field=\"fingers\" value=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- tp_help ----------------------------------------------------------------- */

TEST_CASE("tp_help lists every field, paced, and closes with ok", "[console_tp]")
{
    unsigned remaining;
    unsigned ticks_to_last_slice;

    console_up();
    type_line("tp_help");

    /* The first field is in the first slice, emitted before any tick. */
    TEST_ASSERT_TRUE(replied(">|field=\"gesture\" unit=code min=0 max=255 rw=ro"));
    TEST_ASSERT_TRUE(replied("desc="));
    TEST_ASSERT_EQUAL_UINT((unsigned)KAOS_WIRE_SLICE, fake_klog_count());

    for ( unsigned i = 0U; (unsigned)KDRV_CST_FIELD_MAX > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_CST_FIELD_MAX + 1U, fake_klog_count());

    /* Replay with the capture window reset before the last slice, so the
     * text of the table's last field is checked, not just the count. */
    console_up();
    type_line("tp_help");
    remaining = (unsigned)KDRV_CST_FIELD_MAX - (unsigned)KAOS_WIRE_SLICE;
    ticks_to_last_slice =
        (remaining + (unsigned)KAOS_WIRE_SLICE - 1U) / (unsigned)KAOS_WIRE_SLICE;
    for ( unsigned i = 0U; (ticks_to_last_slice - 1U) > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    fake_klog_reset();
    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(replied(">|field=\"dis_auto_sleep\" unit=bits min=0 max=255 rw=rw"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- The guided test --------------------------------------------------------- */

/** @brief Was `test component=tp` asked of AO_Touch? */
static bool posted_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Touch->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(TOUCH_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief One step of the script, as AO_Touch publishes it for the run of @p origin. */
static void step_from(uint8_t origin, uint8_t phase, uint8_t n, int16_t tx, int16_t ty, int16_t gx, int16_t gy)
{
    TouchTestEvt t;

    t.super = (QEvt)QEVT_INITIALIZER(TOUCH_TEST_SIG);
    t.phase = phase;
    t.step  = n;
    t.total = 2U;
    t.tx    = tx;
    t.ty    = ty;
    t.gx    = gx;
    t.gy    = gy;
    t.origin = origin;
    QASM_DISPATCH(AO_Autotest, &t.super, 0U);
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

/** @brief One step of the script, as AO_Touch publishes it for the autotest's own run. */
static void step(uint8_t phase, uint8_t n, int16_t tx, int16_t ty, int16_t gx, int16_t gy)
{
    step_from(TEST_ORIGIN_AUTOTEST, phase, n, tx, ty, gx, gy);
}

/** @brief Deliver the verdict to the sequencer and the console. */
static void verdict(bool ok)
{
    TestResultEvt r;

    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", "tp");
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

/* Another client's run is not this command's: its steps print nothing. */
TEST_CASE("test_foreign_steps_print_nothing", "[console_tp]")
{
    console_up();
    type_line("test component=tp");
    TEST_ASSERT_TRUE(posted_test());

    step_from(FOREIGN_ORIGIN, TP_TEST_PROMPT, 1U, 80, 215, 0, 0);
    step_from(FOREIGN_ORIGIN, TP_TEST_PASS, 1U, 80, 215, 85, 210);
    step_from(FOREIGN_ORIGIN, TP_TEST_MISS, 2U, 335, 285, 120, 60);
    step_from(FOREIGN_ORIGIN, TP_TEST_FAIL, 2U, 335, 285, 120, 60);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|step="), "a foreign step prints no record");

    step(TP_TEST_PROMPT, 1U, 80, 215, 0, 0);
    TEST_ASSERT_TRUE_MESSAGE(replied(">|step=1 of=2 target=80,215"), "its own step still does");
    verdict(true);
}

TEST_CASE("test component=tp asks AO_Touch, prints each step and closes on its verdict",
          "[console_tp]")
{
    console_up();
    type_line("test component=tp");
    TEST_ASSERT_TRUE(posted_test());
    TEST_ASSERT_FALSE(replied(">ok"));

    step(TP_TEST_PROMPT, 1U, 80, 215, 0, 0);
    TEST_ASSERT_TRUE(replied(">|step=1 of=2 target=80,215"));

    /* An operator finding the target takes longer than the five seconds
     * every other answer gets: the row runs on its own budget. */
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=6"), "the watchdog waits for the operator");

    step(TP_TEST_PASS, 1U, 80, 215, 85, 210);
    TEST_ASSERT_TRUE(replied(">|step=1 target=80,215 got=85,210 result=pass"));

    step(TP_TEST_PROMPT, 2U, 335, 285, 0, 0);
    step(TP_TEST_FAIL, 2U, 335, 285, 120, 60);
    TEST_ASSERT_TRUE(replied(">|step=2 target=335,285 got=120,60 result=fail"));

    step(TP_TEST_TIMEOUT, 2U, 335, 285, 0, 0);
    TEST_ASSERT_TRUE(replied(">|step=2 target=335,285 result=timeout"));

    fake_klog_reset();
    step(TP_TEST_DONE, 2U, 335, 285, 0, 0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "done prints nothing; the verdict follows");
    TEST_ASSERT_FALSE(replied(">ok"));

    verdict(false);
    TEST_ASSERT_TRUE(replied(">|test=tp result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A tap off the target is told as a miss, with where it landed; the same
 * target is asked again, and the verdict is not touched by it. */
TEST_CASE("test_miss_record", "[console_tp]")
{
    console_up();
    type_line("test component=tp");
    TEST_ASSERT_TRUE(posted_test());

    step(TP_TEST_PROMPT, 1U, 80, 215, 0, 0);
    step(TP_TEST_MISS, 1U, 80, 215, 160, 215);
    TEST_ASSERT_TRUE(replied(">|step=1 target=80,215 got=160,215 result=miss"));
    TEST_ASSERT_FALSE(replied(">|test=tp"));
    verdict(true);
}

/* The budget is the whole run's worst case, two targets of three tries of twenty
 * seconds and a margin, and a run that stalls past it is one timeout record and a
 * closed command - not a console stuck on `busy` for an operator who went
 * to lunch. */
TEST_CASE("a touch test nobody finishes is a timeout after its budget, and the command closes",
          "[console_tp]")
{
    console_up();
    type_line("test component=tp");
    TEST_ASSERT_TRUE(posted_test());
    step(TP_TEST_PROMPT, 1U, 80, 215, 0, 0);

    (void)qf_support_tick(AO_Console, TP_BUDGET_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=tp"), "not yet: the operator has 125 s");
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE(replied(">|test=tp result=timeout"));
    TEST_ASSERT_FALSE(replied(">err code=6"));
    TEST_ASSERT_TRUE(replied(">ok"));
}
