/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_c5.c
 * @brief `c5`, `c5_help` and `test component=c5`: the coprocessor's fields and test, from the wire.
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
#include "kaos_wire.h"
#include "kcli_proto.h"
#include "kdrv_c5.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog and the row's own budget, in QP ticks, repeated
 * rather than imported for the reason test_console_flow.c repeats them. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)
#define C5_BUDGET_TICKS    ((KODE_C5_TEST_BUDGET_MS * 100U) / 1000U)
#define ANY_BUDGET_TICKS   ((200000U * 100U) / 1000U)

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
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);

    /* AO_Radio's queue is the mailbox the verbs post into; emptied here so
     * a request left by an earlier case cannot be mistaken for this one's. */
    qf_support_give_queue(AO_Radio);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
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

/**
 * @brief Take the field request the console posted to AO_Radio, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted(RadioFieldReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(RADIO_FIELD_REQ_SIG, e->sig);
    *out = *(const RadioFieldReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Radio's answer to the console, the way it posts it. */
static void answer(uint8_t op, uint16_t field, kode_status_t rc, int32_t value)
{
    RadioFieldEvt a;

    a.super = (QEvt)QEVT_INITIALIZER(RADIO_FIELD_RSP_SIG);
    a.op    = op;
    a.field = field;
    a.rc    = (uint8_t)rc;
    a.value = value;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- The verb -------------------------------------------------------------- */

TEST_CASE("c5 with no keys asks AO_Radio for the whole chip and closes on the answer",
          "[console_c5]")
{
    RadioFieldReqEvt q;

    console_up();
    type_line("c5");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_DUMP, q.op);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.who,
                                  "the answer must come back here");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the owner answered");

    answer(FIELD_OP_DUMP, 0U, KODE_OK, 0);
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|chip=c5"),
                              "a dump has no one field to print");
}

TEST_CASE("c5 field= asks the owner for that field and answers a record",
          "[console_c5]")
{
    RadioFieldReqEvt q;

    console_up();
    type_line("c5 field=\"chip_id\"");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_GET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_C5_CHIP_ID, q.field);

    answer(FIELD_OP_GET, (uint16_t)KDRV_C5_CHIP_ID, KODE_OK, 23);
    TEST_ASSERT_TRUE(replied(">|chip=c5 field=\"chip_id\" value=23"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The table has no knobs, so the owner refuses every write - and the console
 * still asks, because the table is the owner's to know and not this AO's. */
TEST_CASE("c5 field= value= asks for a write, and the refusal names the field",
          "[console_c5]")
{
    RadioFieldReqEvt q;

    console_up();
    type_line("c5 field=\"sd_cd\" value=1");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_SET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_C5_SD_CD, q.field);
    TEST_ASSERT_EQUAL_INT32(1, q.value);

    answer(FIELD_OP_SET, (uint16_t)KDRV_C5_SD_CD, KODE_DENIED, 0);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=sd_cd"));
}

TEST_CASE("a field nobody printed is refused before the owner is asked",
          "[console_c5]")
{
    RadioFieldReqEvt q;

    console_up();
    type_line("c5 field=\"ichg_ma\"");

    TEST_ASSERT_FALSE_MESSAGE(posted(&q), "the owner was asked anyway");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
}

/* A refusal names the field - a row the snapshot did not fill - and a
 * failure names the verb. */
TEST_CASE("a refusal names the field, a failure names the verb", "[console_c5]")
{
    RadioFieldReqEvt q;

    console_up();
    type_line("c5 field=\"chip_id\"");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_GET, (uint16_t)KDRV_C5_CHIP_ID, KODE_DENIED, 0);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=chip_id"));

    console_up();
    type_line("c5 field=\"mac_hi\"");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_GET, (uint16_t)KDRV_C5_MAC_HI, KODE_ERROR, 0);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=c5"));
}

TEST_CASE("another chip's answer does not close a c5 question", "[console_c5]")
{
    RadioFieldReqEvt q;
    StorageFieldEvt  stray;

    console_up();
    type_line("c5 field=\"sdio_width\"");
    TEST_ASSERT_TRUE(posted(&q));

    stray.super = (QEvt)QEVT_INITIALIZER(STORAGE_FIELD_RSP_SIG);
    stray.op    = FIELD_OP_GET;
    stray.field = 0U;
    stray.rc    = (uint8_t)KODE_OK;
    stray.value = 4;
    QASM_DISPATCH(AO_Console, &stray.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the card's answer closed it");

    answer(FIELD_OP_GET, (uint16_t)KDRV_C5_SDIO_WIDTH, KODE_OK, 4);
    TEST_ASSERT_TRUE(replied(">|chip=c5 field=\"sdio_width\" value=4"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- c5_help --------------------------------------------------------------- */

TEST_CASE("c5_help lists every field, paced, and closes with ok", "[console_c5]")
{
    unsigned remaining;
    unsigned ticks_to_last_slice;

    console_up();
    type_line("c5_help");

    TEST_ASSERT_TRUE(replied(">|field=\"valid\" unit=bool min=0 max=1 rw=ro"));
    TEST_ASSERT_TRUE(replied("desc="));
    TEST_ASSERT_EQUAL_UINT((unsigned)KAOS_WIRE_SLICE, fake_klog_count());

    for ( unsigned i = 0U; (unsigned)KDRV_C5_FIELD_MAX > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_C5_FIELD_MAX + 1U, fake_klog_count());

    console_up();
    type_line("c5_help");
    remaining = (unsigned)KDRV_C5_FIELD_MAX - (unsigned)KAOS_WIRE_SLICE;
    ticks_to_last_slice =
        (remaining + (unsigned)KAOS_WIRE_SLICE - 1U) / (unsigned)KAOS_WIRE_SLICE;
    for ( unsigned i = 0U; (ticks_to_last_slice - 1U) > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    fake_klog_reset();
    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(replied(">|field=\"sd_cd\" unit=bool min=0 max=1 rw=ro"));
    TEST_ASSERT_TRUE(replied(">|field=\"pwr_ext\" unit=bool min=0 max=1 rw=ro"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- exp_power ---------------------------------------------------------------- */

/**
 * @brief Take the supply request the console posted to AO_Radio, if any.
 * @param[out] on What it asked for.
 * @return false when nothing was posted.
 */
static bool posted_power(bool *on)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(EXP_POWER_REQ_SIG, e->sig);
    *on = ((const ExpPowerEvt *)e)->on;
    QF_gc(e);
    return true;
}

TEST_CASE("exp_power state= asks AO_Radio for J3's supply and accepts at once",
          "[console_c5]")
{
    bool on = false;

    console_up();
    type_line("exp_power state=on");
    TEST_ASSERT_TRUE(posted_power(&on));
    TEST_ASSERT_TRUE(on);
    TEST_ASSERT_TRUE(replied(">ok"));

    console_up();
    type_line("exp_power state=off");
    TEST_ASSERT_TRUE(posted_power(&on));
    TEST_ASSERT_FALSE(on);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("exp_power with no state, or a quoted one, asks nobody",
          "[console_c5]")
{
    bool on = false;

    console_up();
    type_line("exp_power");
    TEST_ASSERT_FALSE(posted_power(&on));
    TEST_ASSERT_TRUE(replied("msg=state"));

    console_up();
    type_line("exp_power state=\"on\"");
    TEST_ASSERT_FALSE(posted_power(&on));
    TEST_ASSERT_TRUE(replied(">err code=3 msg=state"));
}

/* ---- The row ---------------------------------------------------------------- */

/** @brief Was `test component=c5` asked of AO_Radio? */
static bool posted_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(RADIO_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Deliver the row's verdict. */
static void verdict(bool ok)
{
    TestResultEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", "c5");
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

TEST_CASE("test component=c5 asks AO_Radio and closes on its verdict",
          "[console_c5]")
{
    console_up();
    type_line("test component=c5");

    TEST_ASSERT_TRUE(posted_test());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the radio answered");

    verdict(true);
    TEST_ASSERT_TRUE(replied(">|test=c5 result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A link bring-up and a scan outlive the five seconds every other command
 * gets, so the row has a budget of its own. */
TEST_CASE("the c5 row waits its own budget before it is a timeout", "[console_c5]")
{
    console_up();
    type_line("test component=c5");
    TEST_ASSERT_TRUE(posted_test());

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=c5"),
                              "the row has longer than a chip's five seconds");
    TEST_ASSERT_FALSE(replied(">err code=6"));

    (void)qf_support_tick(AO_Console, C5_BUDGET_TICKS);
    TEST_ASSERT_TRUE(replied(">|test=c5 result=timeout"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"),
                             "a row's silence is a record, not the end of it");
}

/* The cache-only `radio` test judged a handshake somebody else had run; the
 * c5 row runs the handshake itself and judges the chip, so the old name is
 * gone rather than kept as a second way of asking less. */
TEST_CASE("test component=radio is no longer a name the console knows",
          "[console_c5]")
{
    console_up();
    type_line("test component=radio");

    TEST_ASSERT_FALSE(posted_test());
    TEST_ASSERT_TRUE(replied(">err code=3"));
}
