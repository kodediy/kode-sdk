/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_sd.c
 * @brief `sd`, `sd_help` and `test component=sd`: the card's fields and test, from the wire.
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
#include "kdrv_sd.h"
#include "kdrv_storage.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog and the card row's own budget, in QP ticks, repeated
 * rather than imported for the reason test_console_flow.c repeats them. The
 * third is what expires whatever an earlier file left in flight: a `test` row
 * waits on its own budget, and this one's is the better part of a minute. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)
#define SD_BUDGET_TICKS    ((KODE_SD_TEST_BUDGET_MS * 100U) / 1000U)
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

    /* AO_Storage's queue is the mailbox the verbs post into; emptied here so
     * a request left by an earlier case cannot be mistaken for this one's. */
    qf_support_give_queue(AO_Storage);
    while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
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
 * @brief Take the field request the console posted to AO_Storage, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted(StorageFieldReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Storage->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(STORAGE_FIELD_REQ_SIG, e->sig);
    *out = *(const StorageFieldReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Storage's answer to the console, the way it posts it. */
static void answer(uint8_t op, uint16_t field, kode_status_t rc, int32_t value)
{
    StorageFieldEvt a;

    a.super = (QEvt)QEVT_INITIALIZER(STORAGE_FIELD_RSP_SIG);
    a.op    = op;
    a.field = field;
    a.rc    = (uint8_t)rc;
    a.value = value;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- The verb -------------------------------------------------------------- */

TEST_CASE("sd with no keys asks for the whole card and closes on the answer",
          "[console_sd]")
{
    StorageFieldReqEvt q;

    console_up();
    type_line("sd");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_DUMP, q.op);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.who,
                                  "the answer must come back here");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the owner answered");

    answer(FIELD_OP_DUMP, 0U, KODE_OK, 0);
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">|chip=sd"),
                              "a dump has no one field to print");
}

TEST_CASE("sd field= asks the owner for that field and answers a record",
          "[console_sd]")
{
    StorageFieldReqEvt q;

    console_up();
    type_line("sd field=\"capacity_mb\"");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_GET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_SD_CAPACITY_MB, q.field);

    answer(FIELD_OP_GET, (uint16_t)KDRV_SD_CAPACITY_MB, KODE_OK, 30000);
    TEST_ASSERT_TRUE(replied(">|chip=sd field=\"capacity_mb\" value=30000"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("sd field= value= asks for a write and answers the readback",
          "[console_sd]")
{
    StorageFieldReqEvt q;

    console_up();
    type_line("sd field=\"width_req\" value=1");

    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_SET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_SD_WIDTH_REQ, q.field);
    TEST_ASSERT_EQUAL_INT32(1, q.value);

    answer(FIELD_OP_SET, (uint16_t)KDRV_SD_WIDTH_REQ, KODE_OK, 1);
    TEST_ASSERT_TRUE(replied(">|chip=sd field=\"width_req\" value=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The name is resolved against the driver's own table before anything is
 * asked of the owner, so a misspelling costs nothing and names itself. */
TEST_CASE("a field nobody printed is refused before the owner is asked",
          "[console_sd]")
{
    StorageFieldReqEvt q;

    console_up();
    type_line("sd field=\"ichg_ma\"");

    TEST_ASSERT_FALSE_MESSAGE(posted(&q), "the owner was asked anyway");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
}

TEST_CASE("a value with no field to put it in is missing, not wrong",
          "[console_sd]")
{
    StorageFieldReqEvt q;

    console_up();
    type_line("sd value=4");

    TEST_ASSERT_FALSE(posted(&q));
    TEST_ASSERT_TRUE(replied(">err code=2 msg=field"));
}

/* A refusal names the field - that is what tells a read-only field from a
 * card that is not there - and a failure names the verb. */
TEST_CASE("a refusal names the field, a failure names the verb", "[console_sd]")
{
    StorageFieldReqEvt q;

    console_up();
    type_line("sd field=\"capacity_mb\" value=1");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_SET, (uint16_t)KDRV_SD_CAPACITY_MB, KODE_DENIED, 0);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=capacity_mb"));

    console_up();
    type_line("sd field=\"total_mb\"");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_GET, (uint16_t)KDRV_SD_TOTAL_MB, KODE_ERROR, 0);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=sd"));
}

/* Five chips share one answer shape now, and an app may ask a power chip
 * while `sd` is waiting. Only the right chip's answer may close it. */
TEST_CASE("another chip's answer does not close a card question",
          "[console_sd]")
{
    StorageFieldReqEvt q;
    TouchFieldEvt      stray;

    console_up();
    type_line("sd field=\"bus_width\"");
    TEST_ASSERT_TRUE(posted(&q));

    stray.super = (QEvt)QEVT_INITIALIZER(TOUCH_FIELD_RSP_SIG);
    stray.op    = FIELD_OP_GET;
    stray.field = 0U;
    stray.rc    = (uint8_t)KODE_OK;
    stray.value = 7;
    QASM_DISPATCH(AO_Console, &stray.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the panel's answer closed it");

    answer(FIELD_OP_GET, (uint16_t)KDRV_SD_BUS_WIDTH, KODE_OK, 4);
    TEST_ASSERT_TRUE(replied(">|chip=sd field=\"bus_width\" value=4"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- sd_info and fs_info ----------------------------------------------------- */

/** @brief Deliver a STORAGE_INFO_SIG about @p medium, as a subscriber sees it. */
static void info_arrives(kdrv_medium_t medium, uint32_t total_kb)
{
    StorageInfoEvt i;

    (void)memset(&i, 0, sizeof(i));
    i.super       = (QEvt)QEVT_INITIALIZER(STORAGE_INFO_SIG);
    i.medium      = (uint8_t)medium;
    i.mounted     = true;
    i.usage_known = true;
    i.total_kb    = total_kb;
    i.free_kb     = 1U;
    QASM_DISPATCH(AO_Console, &i.super, 0U);
}

/* STORAGE_INFO_SIG is published, and the launcher asks about both media too:
 * About asks the card, Kody's pack the internal FAT. A question about one
 * medium closes on that medium's answer and no other. */
TEST_CASE("sd_info closes on the card's answer, not on the internal FAT's",
          "[console_sd]")
{
    console_up();
    type_line("sd_info");
    info_arrives(KDRV_MEDIUM_FS, 1988U);
    TEST_ASSERT_FALSE_MESSAGE(replied("medium=fs"),
                              "sd_info answered with the FAT's numbers");
    TEST_ASSERT_FALSE(replied(">ok"));
    info_arrives(KDRV_MEDIUM_SD, 30000U);
    TEST_ASSERT_TRUE(replied(">|medium=sd mounted=on total_kb=30000"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("fs_info closes on the internal FAT's answer, not on the card's",
          "[console_sd]")
{
    console_up();
    type_line("fs_info");
    info_arrives(KDRV_MEDIUM_SD, 30000U);
    TEST_ASSERT_FALSE_MESSAGE(replied("medium=sd"),
                              "fs_info answered with the card's numbers");
    info_arrives(KDRV_MEDIUM_FS, 1988U);
    TEST_ASSERT_TRUE(replied(">|medium=fs mounted=on total_kb=1988"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- sd_help --------------------------------------------------------------- */

TEST_CASE("sd_help lists every field, paced, and closes with ok", "[console_sd]")
{
    unsigned remaining;
    unsigned ticks_to_last_slice;

    console_up();
    type_line("sd_help");

    /* The first field is in the first slice, emitted before any tick. */
    TEST_ASSERT_TRUE(replied(">|field=\"mfg_id\" unit=code min=0 max=255 rw=ro"));
    TEST_ASSERT_TRUE(replied("desc="));
    TEST_ASSERT_EQUAL_UINT((unsigned)KAOS_WIRE_SLICE, fake_klog_count());

    for ( unsigned i = 0U; (unsigned)KDRV_SD_FIELD_MAX > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_SD_FIELD_MAX + 1U, fake_klog_count());

    /* Replay with the capture window reset before the last slice, so the
     * text of the table's last field is checked, not just the count. */
    console_up();
    type_line("sd_help");
    remaining = (unsigned)KDRV_SD_FIELD_MAX - (unsigned)KAOS_WIRE_SLICE;
    ticks_to_last_slice =
        (remaining + (unsigned)KAOS_WIRE_SLICE - 1U) / (unsigned)KAOS_WIRE_SLICE;
    for ( unsigned i = 0U; (ticks_to_last_slice - 1U) > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    fake_klog_reset();
    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(
        replied(">|field=\"check_req\" unit=bool min=0 max=1 rw=rw"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- The row ---------------------------------------------------------------- */

/** @brief Was `test component=sd` asked of AO_Storage? */
static bool posted_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Storage->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(STORAGE_CMD_SIG, e->sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)STORAGE_OP_SELFTEST,
                            ((const StorageCmdEvt *)e)->op);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MEDIUM_SD,
                            ((const StorageCmdEvt *)e)->medium);
    QF_gc(e);
    return true;
}

/** @brief Deliver one phase record, the way AO_Storage publishes it. */
static void phase(uint8_t which, bool ok)
{
    StorageTestEvt t;

    (void)memset(&t, 0, sizeof(t));
    t.super       = (QEvt)QEVT_INITIALIZER(STORAGE_TEST_SIG);
    t.phase       = which;
    t.total       = SD_TEST_STEPS;
    t.ok          = ok;
    t.width       = 4U;
    t.freq_khz    = 40000U;
    t.capacity_mb = 30000U;
    t.total_mb    = 29999U;
    t.kb_s        = 8000U;
    (void)snprintf(t.name, sizeof(t.name), "%s", "KODE1");
    QASM_DISPATCH(AO_Console, &t.super, 0U);
}

/** @brief Deliver the row's verdict. */
static void verdict(bool ok)
{
    TestResultEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", "sd");
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

/* Every phase prints its own numbers and only its own: a zero where a phase
 * measured nothing would read as a measurement. */
TEST_CASE("test component=sd prints one record per phase and closes on the "
          "verdict", "[console_sd]")
{
    console_up();
    type_line("test component=sd");

    TEST_ASSERT_TRUE(posted_test());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the card answered");

    phase(SD_TEST_IDENTIFY, true);
    TEST_ASSERT_TRUE(replied(">|step=1 of=5 sd=identify result=pass "
                             "name=\"KODE1\" width=4 freq_khz=40000 "
                             "capacity_mb=30000"));

    phase(SD_TEST_READ4, true);
    TEST_ASSERT_TRUE(replied(">|step=2 of=5 sd=read4 result=pass "
                             "width=4 kb=1024 kb_s=8000"));

    phase(SD_TEST_READ1, true);
    TEST_ASSERT_TRUE(replied(">|step=3 of=5 sd=read1 result=pass "
                             "width=4 kb=1024 kb_s=8000"));

    phase(SD_TEST_WRITE, true);
    TEST_ASSERT_TRUE(replied(">|step=4 of=5 sd=write result=pass "
                             "width=4 kb=1024 kb_s=8000"));

    /* A write the CARD refused: the record says so and the verdict below
     * still belongs to the slot. */
    phase(SD_TEST_VERIFY, false);
    TEST_ASSERT_TRUE(replied(">|step=5 of=5 sd=verify result=fail "
                             "width=4 kb=1024 kb_s=8000"));

    TEST_ASSERT_FALSE(replied(">ok"));
    verdict(false);
    TEST_ASSERT_TRUE(replied(">|test=sd result=fail"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The row moves four megabytes through the card, so it outlives the five
 * seconds every other command gets. */
TEST_CASE("the card row waits its own budget before it is a timeout",
          "[console_sd]")
{
    console_up();
    type_line("test component=sd");
    TEST_ASSERT_TRUE(posted_test());

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=sd"),
                              "the row has longer than a chip's five seconds");
    TEST_ASSERT_FALSE(replied(">err code=6"));

    (void)qf_support_tick(AO_Console, SD_BUDGET_TICKS);
    TEST_ASSERT_TRUE(replied(">|test=sd result=timeout"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"),
                             "a row's silence is a record, not the end of it");
}

/* A record that turns up with no `test` in flight belongs to nobody: the row
 * was given up on, and printing its phases afterwards would put records on
 * the wire outside any command. */
TEST_CASE("a phase record outside a test run is ignored", "[console_sd]")
{
    console_up();
    phase(SD_TEST_IDENTIFY, true);

    TEST_ASSERT_FALSE(replied(">|step=1"));
    TEST_ASSERT_FALSE(replied(">ok"));
}
