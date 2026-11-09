/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_ioexp.c
 * @brief `ioexp` and `ioexp_help`: the expander's field table, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "Mockkhal_clk.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kaos_wire.h"
#include "kdrv_tca6408a.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

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
    /* AO_IoExp's queue is the mailbox the verbs post into; emptied here so a
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
 * @brief Take the request the console posted to AO_IoExp, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted(IoexpFieldReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_IoExp->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(IOEXP_FIELD_REQ_SIG, e->sig);
    *out = *(const IoexpFieldReqEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_IoExp's answer to the console, the way it posts it. */
static void answer(uint8_t op, uint16_t field, kode_status_t rc, int32_t value)
{
    IoexpFieldEvt a;

    a.super = (QEvt)QEVT_INITIALIZER(IOEXP_FIELD_RSP_SIG);
    a.op    = op;
    a.field = field;
    a.rc    = (uint8_t)rc;
    a.value = value;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- The verb -------------------------------------------------------------- */

TEST_CASE("ioexp with no keys asks for the whole chip and closes on the answer",
          "[console_ioexp]")
{
    IoexpFieldReqEvt q;

    console_up();
    type_line("ioexp");
    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_DUMP, q.op);
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.who, "the answer must come back here");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the owner answered");

    answer(FIELD_OP_DUMP, 0U, KODE_OK, 0);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|chip="), "a dump is log lines, not a record");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ioexp field= asks the owner for that field and answers a record",
          "[console_ioexp]")
{
    IoexpFieldReqEvt q;

    console_up();
    type_line("ioexp field=\"config\"");
    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_GET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_TCA_CONFIG, q.field);

    answer(FIELD_OP_GET, (uint16_t)KDRV_TCA_CONFIG, KODE_OK, 127);
    TEST_ASSERT_TRUE(replied(">|chip=ioexp field=\"config\" value=127"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ioexp field= value= asks for a write and answers the readback",
          "[console_ioexp]")
{
    IoexpFieldReqEvt q;

    console_up();
    type_line("ioexp field=\"o7\" value=0");
    TEST_ASSERT_TRUE(posted(&q));
    TEST_ASSERT_EQUAL_UINT8(FIELD_OP_SET, q.op);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KDRV_TCA_O7, q.field);
    TEST_ASSERT_EQUAL_INT32(0, q.value);

    answer(FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, KODE_OK, 0);
    TEST_ASSERT_TRUE(replied(">|chip=ioexp field=\"o7\" value=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A name the driver never printed is refused here: nothing is asked of the
 * owner, and the answer names the key rather than the verb. */
TEST_CASE("a pin nobody printed is refused before the owner is asked",
          "[console_ioexp]")
{
    IoexpFieldReqEvt q;

    console_up();
    type_line("ioexp field=\"p9\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
    TEST_ASSERT_FALSE(posted(&q));

    fake_klog_reset();
    type_line("ioexp field=\"ichg_ma\"");      /* the PMIC's, not the expander's */
    TEST_ASSERT_TRUE(replied(">err code=3 msg=field"));
    TEST_ASSERT_FALSE(posted(&q));
}

/* The owner's verdicts, on the wire: a value the chip cannot hold is a bad
 * value naming the field; a chip that did not answer is a rejection. */
TEST_CASE("a refusal from the owner names the field, a failure names the verb",
          "[console_ioexp]")
{
    IoexpFieldReqEvt q;

    console_up();
    type_line("ioexp field=\"input\" value=1");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_SET, (uint16_t)KDRV_TCA_INPUT, KODE_DENIED, 1);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=input"));

    fake_klog_reset();
    type_line("ioexp field=\"i6\"");
    TEST_ASSERT_TRUE(posted(&q));
    answer(FIELD_OP_GET, (uint16_t)KDRV_TCA_I6, KODE_ERROR, 0);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=ioexp"));
}

/* Three chips share one answer shape and one inflight slot. A PMIC answer
 * arriving while `ioexp` waits - an app asked for it - must not close it. */
TEST_CASE("a power chip's answer does not close an expander question",
          "[console_ioexp]")
{
    IoexpFieldReqEvt q;
    PowerFieldEvt    stray;

    console_up();
    type_line("ioexp field=\"i0\"");
    TEST_ASSERT_TRUE(posted(&q));

    stray.super = (QEvt)QEVT_INITIALIZER(POWER_FIELD_SIG);
    stray.chip  = POWER_CHIP_PMIC;
    stray.op    = FIELD_OP_GET;
    stray.field = 0U;
    stray.rc    = (uint8_t)KODE_OK;
    stray.value = 1;
    QASM_DISPATCH(AO_Console, &stray.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "somebody else's answer");

    answer(FIELD_OP_GET, (uint16_t)KDRV_TCA_I0, KODE_OK, 1);
    TEST_ASSERT_TRUE(replied(">|chip=ioexp field=\"i0\" value=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- ioexp_help -------------------------------------------------------------- */

TEST_CASE("ioexp_help lists every field, paced, and closes with ok",
          "[console_ioexp]")
{
    unsigned remaining;
    unsigned ticks_to_last_slice;

    console_up();
    type_line("ioexp_help");

    /* The first field is in the first slice, emitted before any tick. */
    TEST_ASSERT_TRUE(replied(">|field=\"input\" unit=bits min=0 max=255 rw=ro"));
    TEST_ASSERT_TRUE(replied("desc="));
    TEST_ASSERT_EQUAL_UINT((unsigned)KAOS_WIRE_SLICE, fake_klog_count());

    for ( unsigned i = 0U; (unsigned)KDRV_TCA_FIELD_MAX > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_TCA_FIELD_MAX + 1U, fake_klog_count());

    /* Replay with the capture window reset before the last slice, so the
     * text of the table's last field is checked, not just the count. */
    console_up();
    type_line("ioexp_help");
    remaining = (unsigned)KDRV_TCA_FIELD_MAX - (unsigned)KAOS_WIRE_SLICE;
    ticks_to_last_slice =
        (remaining + (unsigned)KAOS_WIRE_SLICE - 1U) / (unsigned)KAOS_WIRE_SLICE;
    for ( unsigned i = 0U; (ticks_to_last_slice - 1U) > i; ++i )
    {
        (void)qf_support_tick(AO_Console, 1U);
    }
    fake_klog_reset();
    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(replied(">|field=\"c7\" unit=bool min=0 max=1 rw=rw"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- Self-test --------------------------------------------------------------- */

/** @brief Was `test component=ioexp` asked of AO_IoExp? */
static bool posted_test(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_IoExp->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(IOEXP_TEST_REQ_SIG, e->sig);
    QF_gc(e);
    return true;
}

/** @brief Deliver a self-test verdict to the sequencer and the console. */
static void verdict(const char *name, bool ok)
{
    TestResultEvt r;

    r.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(r.name, sizeof(r.name), "%s", name);
    r.ok = ok;
    r.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &r.super, 0U);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    autotest_settle();
}

TEST_CASE("test component=ioexp asks AO_IoExp and closes on its verdict",
          "[console_ioexp]")
{
    console_up();
    type_line("test component=ioexp");
    TEST_ASSERT_TRUE(posted_test());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_IoExp answered");
    verdict("ioexp", true);
    TEST_ASSERT_TRUE(replied(">|test=ioexp result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The row's silence is AO_Autotest's to report, inside the row's own budget.
 * The console's watchdog is armed past that, so the one record a silent
 * owner costs is the row's, and the command still closes with its summary. */
TEST_CASE("a silent row is one timeout record, and the command still closes",
          "[console_ioexp]")
{
    console_up();
    type_line("test component=ioexp");
    TEST_ASSERT_TRUE(posted_test());

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=ioexp"), "judged inside its budget");
    (void)qf_support_tick(AO_Console, 2U);
    TEST_ASSERT_TRUE(replied(">|test=ioexp result=timeout"));
    TEST_ASSERT_TRUE(replied(">|summary pass=0 fail=1 timeout=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">err"), "the console's own watchdog fired");
}

/* One run at a time, whoever asked. A `test` typed while somebody else's run
 * is in flight is refused, and none of that run's rows are printed as its
 * own. */
TEST_CASE("test while another client's run is in flight is busy",
          "[console_ioexp]")
{
    console_up();
    autotest_run_request(AUTOTEST_CLIENT_GUI, AUTOTEST_G_NONE, "ioexp");
    autotest_settle();
    TEST_ASSERT_TRUE(posted_test());

    type_line("test component=ioexp");
    TEST_ASSERT_TRUE(replied(">err code=4"));
    TEST_ASSERT_FALSE(posted_test());

    verdict("ioexp", true);                           /* the other run ends */
    TEST_ASSERT_FALSE_MESSAGE(replied(">|test=ioexp"), "printed a run it did not ask for");
}

/* The answer to a run request is published, so the refusal of somebody
 * else's request reaches the console too. It is not the console's answer. */
TEST_CASE("another client's refusal does not end the console's run",
          "[console_ioexp]")
{
    console_up();
    type_line("test component=ioexp");
    TEST_ASSERT_TRUE(posted_test());

    autotest_run_request(AUTOTEST_CLIENT_GUI, AUTOTEST_G_NONE, "clocks");
    autotest_settle();
    TEST_ASSERT_FALSE(replied(">err"));

    verdict("ioexp", true);
    TEST_ASSERT_TRUE(replied(">|test=ioexp result=pass"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A run that is taken says how many rows it holds, and a record is owed for
 * each. An event lost on the way from AO_Autotest would otherwise leave a
 * summary that is one row short and still says `>ok` - a `fail=0` over a row
 * nobody printed. */
TEST_CASE("a run that ends short of its rows is not ok", "[console_ioexp]")
{
    AutotestEndEvt d;

    console_up();
    type_line("test component=ioexp");        /* one row promised */
    TEST_ASSERT_TRUE(posted_test());

    /* The end, with the row's record never having arrived. */
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(AUTOTEST_END_SIG);
    d.client = AUTOTEST_CLIENT_CONSOLE;
    QASM_DISPATCH(AO_Console, &d.super, 0U);
    TEST_ASSERT_TRUE(replied(">|summary pass=0 fail=0 timeout=0"));
    TEST_ASSERT_TRUE(replied(">err code=5"));
    TEST_ASSERT_FALSE(replied(">ok"));

    verdict("ioexp", true);                   /* the real run ends, unheard */
}

/* Every answer of AO_Autotest carries the client it is for. A run the GUI
 * asked for publishes the same rows, and none of them is the console's. */
TEST_CASE("the rows of another client's run are not printed",
          "[console_ioexp]")
{
    console_up();
    type_line("test component=ioexp");
    TEST_ASSERT_TRUE(posted_test());
    verdict("ioexp", true);
    TEST_ASSERT_TRUE(replied(">ok"));
    fake_klog_reset();

    autotest_run_request(AUTOTEST_CLIENT_GUI, AUTOTEST_G_NONE, "ioexp");
    autotest_settle();
    type_line("ping");
    verdict("ioexp", true);
    TEST_ASSERT_FALSE(replied(">|test=ioexp"));
    TEST_ASSERT_FALSE(replied(">|summary"));
}

/* `user` from the console: the hold is answered at once, because whoever
 * typed the command is the one reading the prompts; a row with nothing to
 * judge is `skipped`, and the summary names how many only when there are
 * any. */
TEST_CASE("test component=user goes through its hold and counts the skipped",
          "[console_ioexp]")
{
    static const char *const BLOCK1[] =
        { "ioexp", "imu", "mag", "haptic", "sd", "c5", "ir", "nfc", "rfid",
          "audio" };
    static const char *const BLOCK2[] =
        { "buttons", "led", "display", "tp", "nfc", "rfid" };

    console_up();
    qf_support_give_queue(AO_Storage);
    qf_support_give_queue(AO_Nfc);
    qf_support_give_queue(AO_Rfid);
    qf_support_give_queue(AO_Audio);
    qf_support_give_queue(AO_Buttons);
    qf_support_give_queue(AO_LedMgr);
    qf_support_give_queue(AO_Power);
    Mockkhal_clk_Init();
    khal_clk_main_hz_IgnoreAndReturn(40000000U);
    khal_clk_slow_src_IgnoreAndReturn(KHAL_CLK_SLOW_XTAL32K);
    khal_clk_measure_IgnoreAndReturn(KODE_ERROR);
    khal_clk_tree_hz_IgnoreAndReturn(KODE_ERROR);

    type_line("test component=user");
    for ( size_t i = 0U; (sizeof(BLOCK1) / sizeof(BLOCK1[0])) > i; ++i )
    {
        verdict(BLOCK1[i], true);
    }
    TEST_ASSERT_TRUE(replied(">|test=audio result=pass"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">|hold=continue"), "the hold was not answered");

    /* The charger's row: nobody plugs one in. */
    {
        TestResultEvt r;

        r.super = (QEvt)QEVT_INITIALIZER(TEST_SKIP_SIG);
        (void)snprintf(r.name, sizeof(r.name), "%s", "power");
        r.ok = false;
        r.origin = TEST_ORIGIN_AUTOTEST;
        QASM_DISPATCH(AO_Autotest, &r.super, 0U);
        autotest_settle();
    }
    TEST_ASSERT_TRUE(replied(">|test=power_plug result=skipped"));
    for ( size_t i = 0U; (sizeof(BLOCK2) / sizeof(BLOCK2[0])) > i; ++i )
    {
        verdict(BLOCK2[i], true);
    }
    TEST_ASSERT_TRUE(replied(">|test=rfid_card result=pass"));
    autotest_psram_run();
    TEST_ASSERT_TRUE(replied(">|test=psram result=pass"));
    TEST_ASSERT_TRUE(replied(">|test=clocks result=fail"));
    TEST_ASSERT_TRUE(replied(">|summary pass=18 fail=1 timeout=0 skipped=1"));
    TEST_ASSERT_TRUE(replied(">ok"));

    Mockkhal_clk_Destroy();
    fake_neighbours_reset();
}

/* The kept report from the wire: nothing kept is one record that says so. */
TEST_CASE("test_report with no run kept says none", "[console_ioexp]")
{
    console_up();
    fake_nvs_reset();
    type_line("test_report");
    autotest_settle();
    TEST_ASSERT_TRUE(replied(">|report=none"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* Every run ends with a block a person reads at a glance: a headline that
 * says the outcome, the failed rows first with what to tell somebody, then
 * the rest, then a count - log lines, after the records a script reads. */
TEST_CASE("a test ends with a verdict block that names what failed",
          "[console_ioexp]")
{
    console_up();
    type_line("test component=ioexp");
    TEST_ASSERT_TRUE(posted_test());
    verdict("ioexp", false);
    TEST_ASSERT_TRUE(replied("TEST: 1 FAILED"));
    TEST_ASSERT_TRUE(replied("FAIL       ioexp"));
    TEST_ASSERT_TRUE(replied("Lines not answering"));
    TEST_ASSERT_TRUE(replied("0 pass, 1 fail, 0 no answer, 0 skipped"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_line("test component=ioexp");
    TEST_ASSERT_TRUE(posted_test());
    verdict("ioexp", true);
    TEST_ASSERT_TRUE(replied("TEST: ALL PASSED"));
    TEST_ASSERT_TRUE(replied("pass       ioexp"));
}
