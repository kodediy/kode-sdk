/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_autotest_ao.c
 * @brief AO_Autotest: the walk over the self-test rows.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "Mockkdrv_net.h"
#include "Mockkhal_clk.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_clocks.h"
#include "khal_system.h"
#include "kqp.h"
#include "krows.h"
#include "owners_support.h"
#include "lowpower_cases.h"
#include "qf_support.h"

#define FOREIGN_ORIGIN 7U   /**< The number of a client other than the autotest. */

#define BUDGET_TICKS  QF_SUPPORT_STEPS(KODE_TEST_ROW_BUDGET_MS)

/** @brief One good read of the clock block, as the crystals' row asks it. */
static void expect_clocks(void)
{
    static uint32_t x32k;
    static uint32_t rc_slow;
    static uint32_t tree;

    x32k    = 32768000U;
    rc_slow = 121300000U;
    tree    = 400000000U;

    Mockkhal_clk_Init();
    khal_clk_main_hz_ExpectAndReturn(40000000U);
    khal_clk_slow_src_ExpectAndReturn(KHAL_CLK_SLOW_XTAL32K);

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_XTAL32K,
                                     KDRV_CLOCKS_CYCLES_FAST, NULL, KODE_OK);
    khal_clk_measure_IgnoreArg_milli_hz();
    khal_clk_measure_ReturnThruPtr_milli_hz(&x32k);

    khal_clk_measure_ExpectAndReturn(KHAL_CLK_SRC_RC_SLOW,
                                     KDRV_CLOCKS_CYCLES_FAST, NULL, KODE_OK);
    khal_clk_measure_IgnoreArg_milli_hz();
    khal_clk_measure_ReturnThruPtr_milli_hz(&rc_slow);

    for ( int i = 0; 4 > i; ++i )
    {
        khal_clk_tree_hz_ExpectAndReturn(KHAL_CLK_TREE_CPU, NULL, KODE_OK);
        khal_clk_tree_hz_IgnoreArg_clk();
        khal_clk_tree_hz_IgnoreArg_hz();
        khal_clk_tree_hz_ReturnThruPtr_hz(&tree);
    }
}

static void up(void)
{
    autotest_up();
    (void)ao_spy();
    ao_spy_subscribe(AUTOTEST_BEGIN_SIG);
    ao_spy_subscribe(AUTOTEST_ROW_SIG);
    ao_spy_subscribe(AUTOTEST_END_SIG);
    ao_spy_subscribe(AUTOTEST_HOLD_SIG);
    ao_spy_subscribe(AUTOTEST_REPORT_SIG);
    (void)qf_support_pump(AO_Autotest, 64U);
    ao_spy_drain();
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Leave nothing behind: the requests the rows posted sit in their
 *         owners' queues, and the file that brings an owner up would
 *         dispatch them as its own. */
static void done(void)
{
    /* The spy stops listening too. A whole `test` in a later file publishes
     * two of these a row, and a spy nobody drains would overflow on them. */
    QActive_unsubscribe(ao_spy(), AUTOTEST_BEGIN_SIG);
    QActive_unsubscribe(ao_spy(), AUTOTEST_ROW_SIG);
    QActive_unsubscribe(ao_spy(), AUTOTEST_END_SIG);
    QActive_unsubscribe(ao_spy(), AUTOTEST_HOLD_SIG);
    QActive_unsubscribe(ao_spy(), AUTOTEST_REPORT_SIG);
    ao_spy_drain();
    fake_neighbours_reset();
}

static void run(uint8_t group, const char *name)
{
    autotest_run_request(AUTOTEST_CLIENT_GUI, group, name);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/**
 * @brief Answer for a row's owner the way an owner does: on the bus.
 *
 * Published rather than dispatched, so that the verdict reaches AO_Autotest
 * only if AO_Autotest is listening for it.
 */
static void verdict(const char *name, bool ok)
{
    TestResultEvt * const e = Q_NEW(TestResultEvt, TEST_RESULT_SIG);

    (void)snprintf(e->name, sizeof(e->name), "%s", name);
    e->ok = ok;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, (void *)0);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/** @brief The spy's next event of this file's three signals; other files
 *         leave the spy subscribed to signals of their own. */
static const QEvt *take_ours(void)
{
    const QEvt *e = ao_spy_take();

    while ( (NULL != e) && (AUTOTEST_BEGIN_SIG != e->sig)
           && (AUTOTEST_ROW_SIG != e->sig) && (AUTOTEST_END_SIG != e->sig)
           && (AUTOTEST_HOLD_SIG != e->sig) && (AUTOTEST_REPORT_SIG != e->sig) )
    {
        e = ao_spy_take();
    }
    return e;
}

/** @brief How many events wait, unread, in AO_Autotest's own queue. */
static unsigned queued(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_Autotest->eQueue);
}

static const AutotestBeginEvt *take_begin(void)
{
    const QEvt * const e = take_ours();

    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(AUTOTEST_BEGIN_SIG, e->sig);
    return (const AutotestBeginEvt *)e;
}

static const AutotestRowEvt *take_row(void)
{
    const QEvt * const e = take_ours();

    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_SIG, e->sig);
    return (const AutotestRowEvt *)e;
}

static const AutotestEndEvt *expect_end(void)
{
    const QEvt * const e = take_ours();

    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL(AUTOTEST_END_SIG, e->sig);
    return (const AutotestEndEvt *)e;
}

static void expect_nothing(void)
{
    TEST_ASSERT_NULL(take_ours());
}

static void expect_row(const char *name, uint8_t state)
{
    const AutotestRowEvt * const r = take_row();

    TEST_ASSERT_EQUAL_STRING(name, r->name);
    TEST_ASSERT_EQUAL(state, r->state);
    ao_spy_release();        /* read, and a long run would empty the pool */
}

TEST_CASE("a row judged on the spot: begin, running, verdict, end",
          "[autotest]")
{
    up();
    expect_clocks();
    run(AUTOTEST_G_NONE, "clocks");

    const AutotestBeginEvt * const b = take_begin();
    TEST_ASSERT_EQUAL(AUTOTEST_TAKEN, b->status);
    TEST_ASSERT_EQUAL(1U, b->rows);
    expect_row("clocks", AUTOTEST_ROW_RUNNING);
    expect_row("clocks", AUTOTEST_ROW_PASS);
    (void)expect_end();
    expect_nothing();
    done();
}

TEST_CASE("a name no row answers to is refused and nothing runs",
          "[autotest]")
{
    up();
    run(AUTOTEST_G_NONE, "nosuch");

    TEST_ASSERT_EQUAL(AUTOTEST_UNKNOWN, take_begin()->status);
    expect_nothing();
    done();
}

TEST_CASE("a row with an owner is waited for, then the run ends",
          "[autotest]")
{
    up();
    run(AUTOTEST_G_NONE, "ioexp");

    TEST_ASSERT_EQUAL(AUTOTEST_TAKEN, take_begin()->status);
    const AutotestRowEvt * const r = take_row();
    TEST_ASSERT_EQUAL_STRING("ioexp", r->name);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_RUNNING, r->state);
    TEST_ASSERT_EQUAL(KODE_TEST_ROW_BUDGET_MS, r->budget_ms);
    expect_nothing();   /* waiting on the owner */

    verdict("ioexp", false);
    expect_row("ioexp", AUTOTEST_ROW_FAIL);
    (void)expect_end();
    done();
}

/** @brief The power test request the row in flight posted to AO_Power, if
 *         any; whatever else waits in that queue is thrown away. */
static bool posted_power(PowerTestReqEvt *out)
{
    const QEvt *e = NULL;
    bool found = false;

    while ( xQueueReceive(AO_Power->eQueue, &e, 0U) == pdTRUE )
    {
        if ( POWER_TEST_REQ_SIG == e->sig )
        {
            *out  = *(const PowerTestReqEvt *)e;
            found = true;
        }
        QF_gc(e);
    }
    return found;
}

/* The E5 final test's rows run under load on a cell at any charge, so it
 * asks the power chips only to answer and measure; any other client gets
 * the whole row. */
TEST_CASE("the power rows are judged in full, and only answering for the final test",
          "[autotest]")
{
    PowerTestReqEvt q;

    up();
    (void)posted_power(&q);                    /* what earlier cases left */

    run(AUTOTEST_G_NONE, "pmic");
    TEST_ASSERT_TRUE(posted_power(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_PMIC, q.chip);
    TEST_ASSERT_EQUAL_UINT8(POWER_DEPTH_FULL, q.depth);
    verdict("pmic", true);
    ao_spy_drain();

    autotest_run_request(AUTOTEST_CLIENT_TEST_DOT, AUTOTEST_G_NONE, "gauge");
    (void)qf_support_pump(AO_Autotest, 64U);
    TEST_ASSERT_TRUE(posted_power(&q));
    TEST_ASSERT_EQUAL_UINT8(POWER_CHIP_GAUGE, q.chip);
    TEST_ASSERT_EQUAL_UINT8(POWER_DEPTH_ANSWER, q.depth);
    verdict("gauge", true);
    ao_spy_drain();
    done();
}

TEST_CASE("a second run while one is in flight is busy", "[autotest]")
{
    up();
    run(AUTOTEST_G_NONE, "ioexp");
    ao_spy_drain();

    run(AUTOTEST_G_NONE, "clocks");
    TEST_ASSERT_EQUAL(AUTOTEST_BUSY, take_begin()->status);
    expect_nothing();

    verdict("ioexp", true);                  /* the first run is untouched */
    expect_row("ioexp", AUTOTEST_ROW_PASS);
    (void)expect_end();
    done();
}

TEST_CASE("a silent owner is one timeout and the run goes on", "[autotest]")
{
    up();
    run(AUTOTEST_G_NONE, "ioexp");
    ao_spy_drain();

    (void)qf_support_tick(AO_Autotest, BUDGET_TICKS - 1U);
    expect_nothing();   /* still inside its budget */
    (void)qf_support_tick(AO_Autotest, 2U);
    expect_row("ioexp", AUTOTEST_ROW_TIMEOUT);
    (void)expect_end();
    done();
}

TEST_CASE("main runs its six rows in the table's order", "[autotest]")
{
    static const char *const ORDER[] =
        { "pmic", "gauge", "ioexp", "clocks", "imu", "mag" };

    up();
    expect_clocks();
    run(AUTOTEST_G_MAIN, NULL);

    TEST_ASSERT_EQUAL(6U, take_begin()->rows);
    for ( size_t i = 0U; 6U > i; ++i )
    {
        expect_row(ORDER[i], AUTOTEST_ROW_RUNNING);
        if ( strcmp(ORDER[i], "clocks") != 0 )
        {
            expect_nothing();   /* one row at a time */
            verdict(ORDER[i], true);
        }
        expect_row(ORDER[i], AUTOTEST_ROW_PASS);
    }
    (void)expect_end();
    done();
}

TEST_CASE("a verdict from a row given up on is nobody's", "[autotest]")
{
    up();
    expect_clocks();
    run(AUTOTEST_G_MAIN, NULL);
    ao_spy_drain();

    (void)qf_support_tick(AO_Autotest, BUDGET_TICKS + 1U);
    expect_row("pmic", AUTOTEST_ROW_TIMEOUT);
    expect_row("gauge", AUTOTEST_ROW_RUNNING);

    verdict("pmic", true);                   /* late */
    expect_nothing();   /* and gauge is still waited for */

    /* The run is finished off so the next case starts idle. */
    verdict("gauge", true);
    verdict("ioexp", true);
    verdict("imu", true);
    verdict("mag", true);
    done();
}

TEST_CASE("a deadline already queued does not time out the next row",
          "[autotest]")
{
    up();
    expect_clocks();
    run(AUTOTEST_G_MAIN, NULL);              /* pmic in flight */
    ao_spy_drain();

    /* The budget runs out and its event is queued, undispatched: a pump
     * that may deliver nothing is a tick and no more. */
    for ( unsigned t = 0U; BUDGET_TICKS >= t; ++t )
    {
        (void)qf_support_pump(AO_Autotest, 0U);
    }
    /* The verdict overtakes it. */
    TestResultEvt e;
    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    (void)snprintf(e.name, sizeof(e.name), "%s", "pmic");
    e.ok = true;
    e.origin = TEST_ORIGIN_AUTOTEST;
    QASM_DISPATCH(AO_Autotest, &e.super, 0U);
    (void)qf_support_pump(AO_Autotest, 64U); /* now the stale deadline */

    expect_row("pmic", AUTOTEST_ROW_PASS);
    expect_row("gauge", AUTOTEST_ROW_RUNNING);
    expect_nothing();   /* gauge was not timed out */

    verdict("gauge", true);
    verdict("ioexp", true);
    verdict("imu", true);
    verdict("mag", true);
    done();
}

/* Verdicts are listened for only while a run is in flight. An idle sequencer
 * sits above every owner, and a result that is somebody else's - `sd_verify`
 * publishes one - is no reason to wake it. */
TEST_CASE("an idle sequencer is not woken by a verdict on the bus",
          "[autotest]")
{
    up();
    verdict("sd_verify", true);
    TEST_ASSERT_EQUAL(0U, queued());

    run(AUTOTEST_G_NONE, "ioexp");
    verdict("ioexp", true);
    ao_spy_drain();

    TestResultEvt * const e = Q_NEW(TestResultEvt, TEST_RESULT_SIG);
    (void)snprintf(e->name, sizeof(e->name), "%s", "ioexp");
    e->ok = true;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, (void *)0);
    TEST_ASSERT_EQUAL_MESSAGE(0U, queued(), "still listening after the run ended");
    done();
}

/* ---- The `user` profile ---------------------------------------------------- */

/** @brief Answer for an owner that has nothing to judge. */
static void skip(const char *name)
{
    TestResultEvt * const e = Q_NEW(TestResultEvt, TEST_SKIP_SIG);

    (void)snprintf(e->name, sizeof(e->name), "%s", name);
    e->ok = false;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, (void *)0);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/**
 * @brief The card's first step failing, as AO_Storage publishes it.
 *
 * @param capacity_mb What the card said it holds: 0 when nothing mounted.
 * @param total       How many phases the script says it has.
 */
static void sd_identify_failed(uint32_t capacity_mb, uint8_t total)
{
    StorageTestEvt * const e = Q_NEW(StorageTestEvt, STORAGE_TEST_SIG);

    (void)memset(((uint8_t *)e) + sizeof(QEvt), 0, sizeof(*e) - sizeof(QEvt));
    e->phase       = SD_TEST_IDENTIFY;
    e->total       = total;
    e->ok          = false;
    e->capacity_mb = capacity_mb;
    QACTIVE_PUBLISH(&e->super, (void *)0);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/**
 * @brief One later phase of the card row, as AO_Storage publishes it.
 *
 * @param phase SD_TEST_WRITE or SD_TEST_VERIFY.
 * @param ok    How it went.
 */
static void sd_phase(uint8_t phase, bool ok)
{
    StorageTestEvt * const e = Q_NEW(StorageTestEvt, STORAGE_TEST_SIG);

    (void)memset(((uint8_t *)e) + sizeof(QEvt), 0, sizeof(*e) - sizeof(QEvt));
    e->phase       = phase;
    e->total       = SD_TEST_STEPS;
    e->ok          = ok;
    e->capacity_mb = 29819U;
    QACTIVE_PUBLISH(&e->super, (void *)0);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/** @brief The card's first step, as AO_Storage publishes it with no card. */
static void sd_no_card(void)
{
    sd_identify_failed(0U, SD_TEST_STEPS);
}

/** @brief Give the owners of the user profile somewhere to be asked. */
static void user_owners(void)
{
    qf_support_give_queue(AO_Storage);
    qf_support_give_queue(AO_Nfc);
    qf_support_give_queue(AO_Rfid);
    qf_support_give_queue(AO_Audio);
    qf_support_give_queue(AO_Buttons);
    qf_support_give_queue(AO_LedMgr);
}

/** @brief Empty what the user profile's rows posted to their owners. */
static void user_owners_drain(void)
{
    QActive * const OWNERS[] = { AO_Storage, AO_Nfc, AO_Rfid, AO_Audio,
                                 AO_Buttons, AO_LedMgr };

    for ( size_t i = 0U; (sizeof(OWNERS) / sizeof(OWNERS[0])) > i; ++i )
    {
        const QEvt *e = NULL;

        while ( xQueueReceive(OWNERS[i]->eQueue, &e, 0U) == pdTRUE )
        {
            QF_gc(e);
        }
    }
}

/** @brief Expect one row to start and answer it for its owner. */
static void row_passes(const char *name, const char *answer)
{
    expect_row(name, AUTOTEST_ROW_RUNNING);
    verdict(answer, true);
    expect_row(name, AUTOTEST_ROW_PASS);
}

/* The final station's card is its own and a good one. A megabyte it refuses,
 * or gives back changed, fails the row there even though the slot's own
 * phases passed; for any other client the owner's verdict stands. */
TEST_CASE("a card that will not take the write fails the row at the final station only",
          "[autotest]")
{
    up();
    qf_support_give_queue(AO_Storage);

    autotest_run_request(AUTOTEST_CLIENT_TEST_DOT, AUTOTEST_G_NONE, "sd");
    (void)qf_support_pump(AO_Autotest, 64U);
    TEST_ASSERT_EQUAL(AUTOTEST_TAKEN, take_begin()->status);
    expect_row("sd", AUTOTEST_ROW_RUNNING);
    sd_phase(SD_TEST_WRITE, false);
    verdict("sd", true);
    expect_row("sd", AUTOTEST_ROW_FAIL);
    (void)expect_end();

    autotest_run_request(AUTOTEST_CLIENT_TEST_DOT, AUTOTEST_G_NONE, "sd");
    (void)qf_support_pump(AO_Autotest, 64U);
    TEST_ASSERT_EQUAL(AUTOTEST_TAKEN, take_begin()->status);
    expect_row("sd", AUTOTEST_ROW_RUNNING);
    sd_phase(SD_TEST_WRITE, true);
    sd_phase(SD_TEST_VERIFY, false);
    verdict("sd", true);
    expect_row("sd", AUTOTEST_ROW_FAIL);
    (void)expect_end();

    autotest_run_request(AUTOTEST_CLIENT_TEST_DOT, AUTOTEST_G_NONE, "sd");
    (void)qf_support_pump(AO_Autotest, 64U);
    TEST_ASSERT_EQUAL(AUTOTEST_TAKEN, take_begin()->status);
    expect_row("sd", AUTOTEST_ROW_RUNNING);
    sd_phase(SD_TEST_WRITE, true);
    sd_phase(SD_TEST_VERIFY, true);
    verdict("sd", true);
    expect_row("sd", AUTOTEST_ROW_PASS);
    (void)expect_end();

    run(AUTOTEST_G_NONE, "sd");
    TEST_ASSERT_EQUAL(AUTOTEST_TAKEN, take_begin()->status);
    expect_row("sd", AUTOTEST_ROW_RUNNING);
    sd_phase(SD_TEST_WRITE, false);
    verdict("sd", true);
    expect_row("sd", AUTOTEST_ROW_PASS);
    (void)expect_end();

    {
        const QEvt *e = NULL;

        while ( xQueueReceive(AO_Storage->eQueue, &e, 0U) == pdTRUE )
        {
            QF_gc(e);
        }
    }
    done();
}

/* The first block as the owner of a unit runs it: nobody at the glass, the
 * card slot empty, and each row of an owner that has two answering under the
 * owner's one name. Then the run waits, and goes on only when told to. */
TEST_CASE("user runs the unattended rows, holds, and goes on when told",
          "[autotest]")
{
    up();
    user_owners();
    expect_clocks();
    fake_nvs_reset();
    run(AUTOTEST_G_USER, NULL);

    const AutotestBeginEvt * const b = take_begin();
    TEST_ASSERT_EQUAL(AUTOTEST_CLIENT_GUI, b->client);
    TEST_ASSERT_EQUAL(20U, b->rows);

    row_passes("ioexp", "ioexp");
    expect_row("clocks", AUTOTEST_ROW_RUNNING);
    expect_row("clocks", AUTOTEST_ROW_PASS);
    row_passes("imu", "imu");
    row_passes("mag", "mag");
    row_passes("haptic", "haptic");

    /* No card: the owner fails at the first step, and that is nothing to
     * judge rather than a fault. */
    expect_row("sd_user", AUTOTEST_ROW_RUNNING);
    sd_no_card();
    verdict("sd", false);
    expect_row("sd_user", AUTOTEST_ROW_SKIPPED);

    row_passes("c5_user", "c5");
    row_passes("ir", "ir");
    row_passes("nfc_field", "nfc");
    row_passes("rfid_field", "rfid");
    row_passes("audio", "audio");

    const QEvt * const h = take_ours();
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL(AUTOTEST_HOLD_SIG, h->sig);
    TEST_ASSERT_EQUAL(AUTOTEST_CLIENT_GUI, ((const AutotestHoldEvt *)h)->client);
    expect_nothing();                        /* waiting, and asking nobody */

    autotest_continue(AUTOTEST_CLIENT_CONSOLE);   /* not its client */
    (void)qf_support_pump(AO_Autotest, 64U);
    expect_nothing();

    autotest_continue(AUTOTEST_CLIENT_GUI);
    (void)qf_support_pump(AO_Autotest, 64U);

    /* No charger: nothing to judge. */
    expect_row("power_plug", AUTOTEST_ROW_RUNNING);
    skip("power");
    expect_row("power_plug", AUTOTEST_ROW_SKIPPED);
    row_passes("buttons", "buttons");
    row_passes("led", "led");
    row_passes("display", "display");
    row_passes("tp", "tp");
    expect_row("nfc_card", AUTOTEST_ROW_RUNNING);
    skip("nfc");
    expect_row("nfc_card", AUTOTEST_ROW_SKIPPED);
    expect_row("rfid_card", AUTOTEST_ROW_RUNNING);
    verdict("rfid", true);
    expect_row("rfid_card", AUTOTEST_ROW_PASS);
    expect_row("psram", AUTOTEST_ROW_RUNNING);
    (void)qf_support_tick(AO_Autotest, 100U);
    expect_row("psram", AUTOTEST_ROW_PASS);
    expect_row("mem", AUTOTEST_ROW_RUNNING);
    expect_row("mem", AUTOTEST_ROW_PASS);

    const AutotestEndEvt * const d = expect_end();
    TEST_ASSERT_EQUAL(AUTOTEST_CLIENT_GUI, d->client);
    TEST_ASSERT_FALSE(d->cancelled);
    TEST_ASSERT_EQUAL(17U, d->tally[AUTOTEST_ROW_PASS]);
    TEST_ASSERT_EQUAL(3U, d->tally[AUTOTEST_ROW_SKIPPED]);
    TEST_ASSERT_EQUAL(0U, d->tally[AUTOTEST_ROW_FAIL]);
    TEST_ASSERT_EQUAL(0U, d->tally[AUTOTEST_ROW_TIMEOUT]);

    /* The report of that run is kept, and comes back row for row. */
    autotest_report_request(AUTOTEST_CLIENT_GUI);
    (void)qf_support_pump(AO_Autotest, 64U);
    unsigned rows = 0U;
    unsigned skipped = 0U;
    const AutotestReportEvt *r = (const AutotestReportEvt *)take_ours();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(AUTOTEST_ID_IOEXP, r->id);
    while ( (NULL != r) && !r->last )
    {
        TEST_ASSERT_EQUAL(AUTOTEST_REPORT_SIG, r->super.sig);
        rows += 1U;
        if ( AUTOTEST_ROW_SKIPPED == r->state )
        {
            skipped += 1U;
        }
        r = (const AutotestReportEvt *)take_ours();
    }
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(20U, rows);
    TEST_ASSERT_EQUAL(3U, skipped);
    TEST_ASSERT_EQUAL(20U, r->rows);
    TEST_ASSERT_EQUAL(0U, r->problems);

    user_owners_drain();
    done();
}

TEST_CASE("a cancel ends the run after the row in flight, and keeps no report",
          "[autotest]")
{
    up();
    user_owners();
    expect_clocks();
    fake_nvs_reset();
    run(AUTOTEST_G_USER, NULL);
    (void)take_begin();
    expect_row("ioexp", AUTOTEST_ROW_RUNNING);

    /* Not its client: the run goes on past the row in flight. */
    autotest_cancel_request(AUTOTEST_CLIENT_CONSOLE);
    (void)qf_support_pump(AO_Autotest, 64U);
    verdict("ioexp", true);
    expect_row("ioexp", AUTOTEST_ROW_PASS);
    expect_row("clocks", AUTOTEST_ROW_RUNNING);
    expect_row("clocks", AUTOTEST_ROW_PASS);
    expect_row("imu", AUTOTEST_ROW_RUNNING);

    autotest_cancel_request(AUTOTEST_CLIENT_GUI);
    (void)qf_support_pump(AO_Autotest, 64U);
    expect_nothing();                        /* the row is not torn down */

    verdict("imu", true);
    expect_row("imu", AUTOTEST_ROW_PASS);
    const AutotestEndEvt * const d = expect_end();
    TEST_ASSERT_TRUE(d->cancelled);
    TEST_ASSERT_EQUAL(3U, d->tally[AUTOTEST_ROW_PASS]);

    autotest_report_request(AUTOTEST_CLIENT_GUI);
    (void)qf_support_pump(AO_Autotest, 64U);
    const AutotestReportEvt * const r = (const AutotestReportEvt *)take_ours();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(r->last);
    TEST_ASSERT_EQUAL(0U, r->rows);          /* nothing was ever kept */

    user_owners_drain();
    done();
}

/* A fail whose row saw a step go unanswered, and none answered no, was never
 * judged: that is a timeout, not a fault. */
TEST_CASE("a row nobody answered is a timeout, and one answered no is a fail",
          "[autotest]")
{
    LedTestEvt *s;

    up();
    user_owners();
    run(AUTOTEST_G_NONE, "led");
    (void)take_begin();
    expect_row("led", AUTOTEST_ROW_RUNNING);
    s = Q_NEW(LedTestEvt, LED_TEST_SIG);
    s->step = 1U; s->total = 3U; s->phase = (uint8_t)LED_TEST_TIMEOUT; s->colour = 0U;
    QACTIVE_PUBLISH(&s->super, (void *)0);
    verdict("led", false);
    expect_row("led", AUTOTEST_ROW_TIMEOUT);
    TEST_ASSERT_EQUAL(1U, expect_end()->tally[AUTOTEST_ROW_TIMEOUT]);

    run(AUTOTEST_G_NONE, "led");
    (void)take_begin();
    expect_row("led", AUTOTEST_ROW_RUNNING);
    s = Q_NEW(LedTestEvt, LED_TEST_SIG);
    s->step = 1U; s->total = 3U; s->phase = (uint8_t)LED_TEST_FAIL; s->colour = 0U;
    QACTIVE_PUBLISH(&s->super, (void *)0);
    verdict("led", false);
    expect_row("led", AUTOTEST_ROW_FAIL);
    TEST_ASSERT_EQUAL(1U, expect_end()->tally[AUTOTEST_ROW_FAIL]);

    user_owners_drain();
    done();
}

/** @brief Publish one step of the guided button script, as AO_Buttons does. */
static void button_step(uint8_t phase, uint8_t origin)
{
    ButtonsTestEvt * const s = Q_NEW(ButtonsTestEvt, BTN_TEST_SIG);

    (void)memset(((uint8_t *)s) + sizeof(QEvt), 0, sizeof(*s) - sizeof(QEvt));
    s->phase  = phase;
    s->step   = 1U;
    s->total  = 6U;
    s->origin = origin;
    QACTIVE_PUBLISH(&s->super, (void *)0);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/* The guided scripts can also be asked for by another client. A step of that
 * run says no, and the row the autotest is running at that moment must not
 * carry the answer: with its own steps all passing the row passes, and with
 * one of its own timing out it is a timeout, not the other run's fail. */
TEST_CASE("test_foreign_step_leaves_row_alone", "[autotest]")
{
    up();
    user_owners();
    run(AUTOTEST_G_NONE, "buttons");
    (void)take_begin();
    expect_row("buttons", AUTOTEST_ROW_RUNNING);
    button_step(BTN_TEST_GIVEUP, FOREIGN_ORIGIN);
    button_step(BTN_TEST_PASS, TEST_ORIGIN_AUTOTEST);
    verdict("buttons", true);
    expect_row("buttons", AUTOTEST_ROW_PASS);
    (void)expect_end();

    run(AUTOTEST_G_NONE, "buttons");
    (void)take_begin();
    expect_row("buttons", AUTOTEST_ROW_RUNNING);
    button_step(BTN_TEST_GIVEUP, FOREIGN_ORIGIN);
    button_step(BTN_TEST_TIMEOUT, TEST_ORIGIN_AUTOTEST);
    verdict("buttons", false);
    expect_row("buttons", AUTOTEST_ROW_TIMEOUT);
    (void)expect_end();

    user_owners_drain();
    done();
}

/* A step that failed the unit, followed by steps nobody answered: the row is
 * a fail, not a timeout. Every phase the script fails a step with counts. */
TEST_CASE("test_doublefail_then_timeouts_is_fail", "[autotest]")
{
    uint8_t const failing[] = { BTN_TEST_DOUBLEFAIL, BTN_TEST_HELD, BTN_TEST_GIVEUP };

    up();
    user_owners();
    for ( unsigned i = 0U; (sizeof(failing) / sizeof(failing[0])) > i; ++i )
    {
        run(AUTOTEST_G_NONE, "buttons");
        (void)take_begin();
        expect_row("buttons", AUTOTEST_ROW_RUNNING);
        button_step(failing[i], TEST_ORIGIN_AUTOTEST);
        button_step(BTN_TEST_TIMEOUT, TEST_ORIGIN_AUTOTEST);
        button_step(BTN_TEST_TIMEOUT, TEST_ORIGIN_AUTOTEST);
        verdict("buttons", false);
        expect_row("buttons", AUTOTEST_ROW_FAIL);
        (void)expect_end();
    }

    user_owners_drain();
    done();
}

/** @brief Publish one step of the guided touch script, as AO_Touch does. */
static void touch_step(uint8_t phase, uint8_t step)
{
    TouchTestEvt * const s = Q_NEW(TouchTestEvt, TOUCH_TEST_SIG);

    (void)memset(((uint8_t *)s) + sizeof(QEvt), 0, sizeof(*s) - sizeof(QEvt));
    s->phase  = phase;
    s->step   = step;
    s->total  = 2U;
    s->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&s->super, (void *)0);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/* A tap off the target is a forgiven slip and does not mark the row; the
 * third one is published as a fail, which does. Step 2 passing afterwards
 * does not clear it, and the verdict is false: the row is a fail. */
TEST_CASE("test_tp_third_miss_is_fail", "[autotest]")
{
    up();
    user_owners();
    run(AUTOTEST_G_NONE, "tp");
    (void)take_begin();
    expect_row("tp", AUTOTEST_ROW_RUNNING);
    touch_step(TP_TEST_MISS, 1U);
    touch_step(TP_TEST_MISS, 1U);
    touch_step(TP_TEST_FAIL, 1U);
    touch_step(TP_TEST_PASS, 2U);
    verdict("tp", false);
    expect_row("tp", AUTOTEST_ROW_FAIL);
    (void)expect_end();

    user_owners_drain();
    done();
}

/* Two misses and then the hit are forgiven: the row passes. */
TEST_CASE("test_tp_forgiven_misses_pass", "[autotest]")
{
    up();
    user_owners();
    run(AUTOTEST_G_NONE, "tp");
    (void)take_begin();
    expect_row("tp", AUTOTEST_ROW_RUNNING);
    touch_step(TP_TEST_MISS, 1U);
    touch_step(TP_TEST_MISS, 1U);
    touch_step(TP_TEST_PASS, 1U);
    touch_step(TP_TEST_PASS, 2U);
    verdict("tp", true);
    expect_row("tp", AUTOTEST_ROW_PASS);
    (void)expect_end();

    user_owners_drain();
    done();
}

/** @brief A verdict on the bus for the run of @p origin, as AO_Buttons publishes it. */
static void verdict_from(uint8_t origin, const char *name, bool ok)
{
    TestResultEvt * const e = Q_NEW(TestResultEvt, TEST_RESULT_SIG);

    (void)snprintf(e->name, sizeof(e->name), "%s", name);
    e->ok     = ok;
    e->origin = origin;
    QACTIVE_PUBLISH(&e->super, (void *)0);
    (void)qf_support_pump(AO_Autotest, 64U);
}

/* The verdict of another client's run is not the row's either: the row stays
 * open, and ends on the verdict that is its own. */
TEST_CASE("test_foreign_verdict_leaves_row_alone", "[autotest]")
{
    up();
    user_owners();
    run(AUTOTEST_G_NONE, "buttons");
    (void)take_begin();
    expect_row("buttons", AUTOTEST_ROW_RUNNING);

    verdict_from(FOREIGN_ORIGIN, "buttons", false);
    expect_nothing();                        /* the row is still open */

    verdict_from(TEST_ORIGIN_AUTOTEST, "buttons", true);
    expect_row("buttons", AUTOTEST_ROW_PASS);
    (void)expect_end();

    user_owners_drain();
    done();
}

/** @brief Did klog see a line containing @p needle since the reset? */
static bool logged(const char *needle)
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

/* A verdict of another run is dropped, and the drop is said at debug level,
 * with the name it carried and the origin it came from. */
TEST_CASE("test_foreign_verdict_is_logged_and_dropped", "[autotest]")
{
    up();
    user_owners();
    run(AUTOTEST_G_NONE, "buttons");
    (void)take_begin();
    expect_row("buttons", AUTOTEST_ROW_RUNNING);

    fake_klog_reset();
    verdict_from(FOREIGN_ORIGIN, "buttons", true);
    expect_nothing();                        /* the row is not marked */
    TEST_ASSERT_TRUE_MESSAGE(
        logged("verdict of 'buttons' from origin 7 is not the row's; ignored"),
        "the drop is logged");

    verdict_from(TEST_ORIGIN_AUTOTEST, "buttons", true);
    expect_row("buttons", AUTOTEST_ROW_PASS);
    (void)expect_end();

    user_owners_drain();
    done();
}

/* The factory's card row is not the owner's: with no card it fails. */
TEST_CASE("the factory card row fails on an empty slot", "[autotest]")
{
    up();
    user_owners();
    run(AUTOTEST_G_NONE, "sd");
    (void)take_begin();
    expect_row("sd", AUTOTEST_ROW_RUNNING);
    sd_no_card();
    verdict("sd", false);
    expect_row("sd", AUTOTEST_ROW_FAIL);
    (void)expect_end();
    user_owners_drain();
    done();
}

/* An empty slot is nothing to judge. A card that is there and cannot be used
 * is a fault - the slot on one data line reads exactly like this - and so is
 * a first step that belongs to `sd_verify`, which numbers its phases from 1
 * as well. */
TEST_CASE("the owner's card row fails on a card that is there and unusable",
          "[autotest]")
{
    up();
    user_owners();
    run(AUTOTEST_G_NONE, "sd_user");
    (void)take_begin();
    expect_row("sd_user", AUTOTEST_ROW_RUNNING);
    sd_identify_failed(29000U, SD_TEST_STEPS);
    verdict("sd", false);
    expect_row("sd_user", AUTOTEST_ROW_FAIL);
    (void)expect_end();

    run(AUTOTEST_G_NONE, "sd_user");
    (void)take_begin();
    expect_row("sd_user", AUTOTEST_ROW_RUNNING);
    sd_identify_failed(0U, SD_TEST_STEPS + 1U);
    verdict("sd", false);
    expect_row("sd_user", AUTOTEST_ROW_FAIL);
    (void)expect_end();

    user_owners_drain();
    done();
}

/* A row run again by name, from the page that shows the last run: its new
 * verdict replaces its old one in the kept report, and the rest of the
 * report - the other rows, the time of the run - stays as it was. */
TEST_CASE("a row run again by name replaces its verdict in the kept report",
          "[autotest]")
{
    /* The kept record as the sequencer stores it: version, rows, ids,
     * states, then the run's time on a four-byte boundary. */
    uint8_t blob[2U + (2U * AUTOTEST_RUN_MAX) + 2U + 4U];

    (void)memset(blob, 0, sizeof(blob));
    blob[0] = 1U;                              /* version */
    blob[1] = 2U;                              /* rows    */
    blob[2] = AUTOTEST_ID_IOEXP;
    blob[3] = AUTOTEST_ID_CLOCKS;
    blob[2U + AUTOTEST_RUN_MAX]      = AUTOTEST_ROW_PASS;
    blob[2U + AUTOTEST_RUN_MAX + 1U] = AUTOTEST_ROW_PASS;
    blob[sizeof(blob) - 4U] = 0x2AU;           /* when = 42 */

    up();
    fake_nvs_reset();
    fake_nvs_seed("ktest", "last", blob, sizeof(blob));

    run(AUTOTEST_G_NONE, "ioexp");
    (void)take_begin();
    expect_row("ioexp", AUTOTEST_ROW_RUNNING);
    verdict("ioexp", false);
    expect_row("ioexp", AUTOTEST_ROW_FAIL);
    (void)expect_end();

    autotest_report_request(AUTOTEST_CLIENT_GUI);
    (void)qf_support_pump(AO_Autotest, 64U);
    const AutotestReportEvt *r = (const AutotestReportEvt *)take_ours();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(AUTOTEST_ID_IOEXP, r->id);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_FAIL, r->state);
    r = (const AutotestReportEvt *)take_ours();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL(AUTOTEST_ID_CLOCKS, r->id);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_PASS, r->state);
    r = (const AutotestReportEvt *)take_ours();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(r->last);
    TEST_ASSERT_EQUAL(2U, r->rows);
    TEST_ASSERT_EQUAL(1U, r->problems);
    TEST_ASSERT_EQUAL(42U, r->when);

    fake_nvs_reset();
    done();
}

/* ---- The PSRAM row ------------------------------------------------------- */

/** @brief Steps enough for any psram run the cases set up to end in. */
#define PSRAM_STEPS_MAX  200U

/** @brief Steps past the row's own budget, so its timer has fired. */
#define PSRAM_BUDGET_STEPS  (QF_SUPPORT_STEPS(KODE_PSRAM_TEST_BUDGET_MS) + 10U)

/** @brief What one psram run published, in the order it was published. */
typedef struct {
    bool         record;      /**< A PsramTestEvt was seen.             */
    PsramTestEvt rec;         /**< Its copy.                            */
    bool         record_first;/**< It came before the row's verdict.    */
    uint8_t      verdict;     /**< The row's closing state, or 0xFF.    */
    bool         ended;       /**< The run's end was published.         */
} psram_seen_t;

/** @brief Start `test component=psram` with the spy on the row's record. */
static void psram_start(void)
{
    up();
    fake_psram_reset();
    ao_spy_subscribe(PSRAM_TEST_SIG);
    run(AUTOTEST_G_NONE, "psram");
    (void)take_begin();
    expect_row("psram", AUTOTEST_ROW_RUNNING);
}

/** @brief Read everything the spy caught since the row started. */
static psram_seen_t psram_collect(void)
{
    psram_seen_t s;
    const QEvt *e = ao_spy_take();

    (void)memset(&s, 0, sizeof(s));
    s.verdict = 0xFFU;
    while ( NULL != e )
    {
        if ( PSRAM_TEST_SIG == e->sig )
        {
            s.record = true;
            s.rec    = *(const PsramTestEvt *)e;
            s.record_first = (0xFFU == s.verdict);
        }
        else if ( AUTOTEST_ROW_SIG == e->sig )
        {
            const AutotestRowEvt * const r = (const AutotestRowEvt *)e;
            if ( (strcmp(r->name, "psram") == 0)
                && (AUTOTEST_ROW_RUNNING != r->state) )
            {
                s.verdict = r->state;
            }
        }
        else if ( AUTOTEST_END_SIG == e->sig )
        {
            s.ended = true;
        }
        else
        {
            /* Another file's signal. */
        }
        ao_spy_release();
        e = ao_spy_take();
    }
    return s;
}

/** @brief Leave the spy as up() found it. */
static void psram_done(void)
{
    QActive_unsubscribe(ao_spy(), PSRAM_TEST_SIG);
    done();
}

TEST_CASE("the psram row tests the free PSRAM down to its reserve and passes",
          "[autotest]")
{
    size_t const free_b = KODE_PSRAM_TEST_RESERVE_BYTES
                        + (3U * KODE_PSRAM_TEST_BLOCK_BYTES);

    fake_psram_set_free(free_b);
    psram_start();
    (void)qf_support_tick(AO_Autotest, PSRAM_STEPS_MAX);

    psram_seen_t const s = psram_collect();
    TEST_ASSERT_TRUE(s.record);
    TEST_ASSERT_TRUE_MESSAGE(s.record_first, "the record comes before the verdict");
    TEST_ASSERT_EQUAL_UINT32(3U * (KODE_PSRAM_TEST_BLOCK_BYTES / 1024U), s.rec.tested_kb);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(free_b / 1024U), s.rec.free_kb);
    TEST_ASSERT_EQUAL_UINT32(0U, s.rec.errors);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_PASS, s.verdict);
    TEST_ASSERT_TRUE(s.ended);
    TEST_ASSERT_EQUAL_UINT(3U, fake_psram_alloc_count());
    TEST_ASSERT_EQUAL_UINT(3U, fake_psram_free_count());
    psram_done();
}

TEST_CASE("with too little PSRAM free the row fails having tested nothing",
          "[autotest]")
{
    fake_psram_set_free(KODE_PSRAM_TEST_RESERVE_BYTES
                        + KODE_PSRAM_TEST_BLOCK_BYTES - 1U);
    psram_start();
    (void)qf_support_tick(AO_Autotest, 2U);

    psram_seen_t const s = psram_collect();
    TEST_ASSERT_TRUE(s.record);
    TEST_ASSERT_EQUAL_UINT32(0U, s.rec.tested_kb);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_FAIL, s.verdict);
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
    psram_done();
}

TEST_CASE("a refused block ends the allocation; the blocks already held are tested",
          "[autotest]")
{
    fake_psram_set_free(KODE_PSRAM_TEST_RESERVE_BYTES
                        + (4U * KODE_PSRAM_TEST_BLOCK_BYTES));
    fake_psram_refuse_after(2U, 1U);
    psram_start();
    (void)qf_support_tick(AO_Autotest, PSRAM_STEPS_MAX);

    psram_seen_t const s = psram_collect();
    TEST_ASSERT_EQUAL_UINT32(2U * (KODE_PSRAM_TEST_BLOCK_BYTES / 1024U), s.rec.tested_kb);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_PASS, s.verdict);
    TEST_ASSERT_EQUAL_UINT(2U, fake_psram_alloc_count());
    TEST_ASSERT_EQUAL_UINT(2U, fake_psram_free_count());
    psram_done();
}

/* The budget can run out with the walk mid-way and its next step already
 * queued. The blocks go back at once, and the step that arrives after them
 * must find nothing to touch. */
TEST_CASE("a psram row that runs out of budget frees every block", "[autotest]")
{
    static const QEvt expiry = QEVT_INITIALIZER(AUTOTEST_BUDGET_SIG);

    fake_psram_set_free(KODE_PSRAM_TEST_RESERVE_BYTES
                        + (3U * KODE_PSRAM_TEST_BLOCK_BYTES));
    psram_start();
    (void)qf_support_tick(AO_Autotest, 6U);   /* three blocks taken, a pass begun */
    TEST_ASSERT_EQUAL_UINT(3U, fake_psram_alloc_count());

    QASM_DISPATCH(AO_Autotest, &expiry, 0U);
    (void)qf_support_pump(AO_Autotest, 64U);

    psram_seen_t s = psram_collect();
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_TIMEOUT, s.verdict);
    TEST_ASSERT_FALSE_MESSAGE(s.record, "a row given up on reports nothing");
    TEST_ASSERT_EQUAL_UINT(3U, fake_psram_free_count());

    /* The step left in the queue, and the row's real timer behind it. */
    (void)qf_support_tick(AO_Autotest, PSRAM_BUDGET_STEPS);
    s = psram_collect();
    TEST_ASSERT_FALSE(s.record);
    TEST_ASSERT_EQUAL_UINT(3U, fake_psram_free_count());
    psram_done();
}

/* On a board a refused allocation is counted against the whole boot
 * (`alloc_fail`), and the `mem` row fails on it. So the row asks for a block
 * only when the heap can give one whole, and never meets a refusal. */
TEST_CASE("a PSRAM with no block whole enough is not asked for one",
          "[autotest]")
{
    fake_psram_set_free(KODE_PSRAM_TEST_RESERVE_BYTES
                        + (3U * KODE_PSRAM_TEST_BLOCK_BYTES));
    fake_psram_set_largest(KODE_PSRAM_TEST_BLOCK_BYTES - 1U);
    psram_start();
    (void)qf_support_tick(AO_Autotest, 4U);

    psram_seen_t const s = psram_collect();
    TEST_ASSERT_EQUAL_UINT32(0U, s.rec.tested_kb);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_FAIL, s.verdict);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_psram_alloc_count(),
                                   "no allocation the heap would refuse");
    psram_done();
}

/* The system goes on allocating while the row takes its blocks. The reserve
 * is what is left at each step, not the start's free less the row's own. */
TEST_CASE("the reserve holds against what is free at each step, not at the start",
          "[autotest]")
{
    void *other;

    fake_psram_set_free(KODE_PSRAM_TEST_RESERVE_BYTES
                        + (3U * KODE_PSRAM_TEST_BLOCK_BYTES));
    psram_start();
    (void)qf_support_tick(AO_Autotest, 1U);
    TEST_ASSERT_EQUAL_UINT(1U, fake_psram_alloc_count());

    /* Somebody else takes a block and a half. */
    other = khal_alloc_psram(sizeof(uint32_t), KODE_PSRAM_TEST_BLOCK_BYTES
                             + (KODE_PSRAM_TEST_BLOCK_BYTES / 2U));
    TEST_ASSERT_NOT_NULL(other);
    (void)qf_support_tick(AO_Autotest, PSRAM_STEPS_MAX);

    psram_seen_t const s = psram_collect();
    TEST_ASSERT_EQUAL_UINT32(KODE_PSRAM_TEST_BLOCK_BYTES / 1024U, s.rec.tested_kb);
    TEST_ASSERT_EQUAL(AUTOTEST_ROW_PASS, s.verdict);
    TEST_ASSERT_TRUE_MESSAGE(khal_psram_free() >= KODE_PSRAM_TEST_RESERVE_BYTES,
                             "the reserve was never taken");
    khal_free(other);
    psram_done();
}

/* ---- The rows come from their owners ------------------------------------- */

/** @brief Empty the queues of the owners `all` asks that no neighbour reset
 *         empties, so the file that brings one up does not dispatch them. */
static void drain_owners(void)
{
    QActive * const owners[] = {
        AO_Storage, AO_Nfc, AO_Rfid, AO_Audio, AO_Buttons, AO_LedMgr,
    };

    for ( size_t i = 0U; (sizeof(owners) / sizeof(owners[0])) > i; ++i )
    {
        const QEvt *e = NULL;

        while ( (NULL != owners[i]->eQueue)
               && (xQueueReceive(owners[i]->eQueue, &e, 0U) == pdTRUE) )
        {
            QF_gc(e);
        }
    }
}

TEST_CASE("main keeps its order when the owners register in another order",
          "[autotest]")
{
    static const char *const ORDER[] = {
        "pmic", "gauge", "ioexp", "clocks", "imu", "mag",
    };
    size_t const rows = sizeof(ORDER) / sizeof(ORDER[0]);

    up();
    krows_reset();
    kaos_display_register();
    kaos_touch_register();
    kaos_haptic_register();
    kaos_imu_register();
    kaos_audio_register();
    kaos_rfid_register();
    kaos_nfc_register();
    kaos_ir_register();
    kaos_ledmgr_register();
    kaos_net_register();
    kaos_radio_register();
    kaos_storage_register();
    kaos_autotest_register();
    expect_clocks();
    run(AUTOTEST_G_MAIN, NULL);

    TEST_ASSERT_EQUAL((uint32_t)rows, (uint32_t)take_begin()->rows);
    for ( size_t i = 0U; i < rows; ++i )
    {
        const AutotestRowEvt * const r = take_row();

        TEST_ASSERT_EQUAL_STRING(ORDER[i], r->name);
        TEST_ASSERT_EQUAL(AUTOTEST_ROW_RUNNING, r->state);
        ao_spy_release();
        if ( strcmp(ORDER[i], "clocks") != 0 )
        {
            verdict(ORDER[i], true);
        }
        TEST_ASSERT_EQUAL_STRING(ORDER[i], take_row()->name);
        ao_spy_release();
    }
    (void)expect_end();
    krows_reset();
    owners_up();
    drain_owners();
    done();
}

/* `all` is the E5 final test, AO_TestDot's: no row answers to it as a group,
 * and every row the station's run names is reachable alone by name. */
TEST_CASE("no row is in a group called all any more; each answers by name",
          "[autotest]")
{
    static const char *const BY_NAME[] = {
        "haptic", "sd", "c5", "ir", "nfc", "rfid", "audio", "buttons",
        "led", "display", "tp", "psram", "mem",
    };

    up();
    krows_reset();
    kaos_autotest_register();
    kaos_display_register();
    kaos_touch_register();
    kaos_haptic_register();
    kaos_imu_register();
    kaos_audio_register();
    kaos_rfid_register();
    kaos_nfc_register();
    kaos_ir_register();
    kaos_ledmgr_register();
    kaos_net_register();
    kaos_radio_register();
    kaos_storage_register();
    for ( size_t i = 0U; (sizeof(BY_NAME) / sizeof(BY_NAME[0])) > i; ++i )
    {
        const krow_t *r = NULL;

        for ( size_t k = 0U; krows_count() > k; ++k )
        {
            if ( strcmp(krows_at(k)->name, BY_NAME[i]) == 0 )
            {
                r = krows_at(k);
            }
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(r, BY_NAME[i]);
        TEST_ASSERT_EQUAL_HEX8_MESSAGE(AUTOTEST_G_NONE, r->groups, BY_NAME[i]);
    }
    krows_reset();
    owners_up();
    drain_owners();
    done();
}

TEST_CASE("a row whose owner registered nothing is not in the run", "[autotest]")
{
    static const char *const ORDER[] = { "pmic", "gauge", "ioexp", "clocks" };
    size_t const rows = sizeof(ORDER) / sizeof(ORDER[0]);

    up();
    krows_reset();
    kaos_autotest_register();   /* AO_Imu not linked: no imu, no mag */
    run(AUTOTEST_G_NONE, "imu");
    TEST_ASSERT_EQUAL(AUTOTEST_UNKNOWN, take_begin()->status);
    expect_nothing();

    expect_clocks();
    run(AUTOTEST_G_MAIN, NULL);
    TEST_ASSERT_EQUAL((uint32_t)rows, (uint32_t)take_begin()->rows);
    for ( size_t i = 0U; i < rows; ++i )
    {
        expect_row(ORDER[i], AUTOTEST_ROW_RUNNING);
        if ( strcmp(ORDER[i], "clocks") != 0 )
        {
            verdict(ORDER[i], true);
        }
        expect_row(ORDER[i], AUTOTEST_ROW_PASS);
    }
    (void)expect_end();
    krows_reset();
    owners_up();
    done();
}

/** @brief A row's verdict for the registry cases; never run. */
static bool row_never_run(void)
{
    return true;
}

TEST_CASE("a row whose label or problem is longer than the screen's line is refused, whole",
          "[autotest]")
{
    static const krow_t LONG_LABEL[] = {
        { "longlabel", AUTOTEST_ID_LED, row_never_run, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "eighteen chars!!!x", "fine", AUTOTEST_ORDER_LED },
    };
    static const krow_t LONG_PROBLEM[] = {
        { "longproblem", AUTOTEST_ID_LED, row_never_run, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "fine", "twenty-three chars !!!x", AUTOTEST_ORDER_LED },
    };
    static const krow_t AT_LIMITS[] = {
        { "atlimits", AUTOTEST_ID_LED, row_never_run, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "seventeen chars!!", "twenty-two chars !!!!!", AUTOTEST_ORDER_LED },
    };

    up();
    krows_reset();
    TEST_ASSERT_EQUAL_UINT((size_t)KROW_LABEL_MAX + 1U, strlen(LONG_LABEL[0].label));
    TEST_ASSERT_EQUAL_UINT((size_t)KROW_PROBLEM_MAX + 1U, strlen(LONG_PROBLEM[0].problem));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, krows_add(LONG_LABEL, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, krows_add(LONG_PROBLEM, 1U));
    TEST_ASSERT_EQUAL_UINT(0U, krows_count());
    TEST_ASSERT_EQUAL_INT(KODE_OK, krows_add(AT_LIMITS, 1U));
    TEST_ASSERT_EQUAL_UINT(1U, krows_count());
    krows_reset();
    owners_up();
    done();
}


/* ---- The board's rest ------------------------------------------------------- */

TEST_CASE("the sequencer is ready for the round except while a row runs",
          "[autotest][lowpower]")
{
    up();
    lp_round(AO_Autotest, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    run(AUTOTEST_G_NONE, "ioexp");
    ao_spy_drain();
    lp_round(AO_Autotest, KODE_LP_OFF);
    lp_expect_answer(true, "test row");
    lp_expect_no_more();

    verdict("ioexp", true);
    expect_row("ioexp", AUTOTEST_ROW_PASS);
    (void)expect_end();
    lp_round(AO_Autotest, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    done();
}
