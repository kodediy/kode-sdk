/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_test_dot_run.c
 * @brief `test component=all` as AO_TestDot runs it: the rows in order and the result.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "fake_station.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_buttons.h"
#include "khal_nvs.h"
#include "kqp.h"
#include "test_dot_support.h"

#if !CONFIG_KODE_TEST_DOT
#error "test_test_dot_run.c needs CONFIG_KODE_TEST_DOT: main/CMakeLists.txt spells it out"
#endif

#define TD_ROWS_TEXT   200U  /**< Room for the names of the rows asked at once.                  */
#define TD_STEP_MS     50U   /**< QP time a drive round gives the AO's rests when no job waits.   */
#define TD_ROUNDS      400U  /**< Most drive rounds: well past the camera part's every step.      */
#define TD_ROWS_AFTER  11U   /**< The rows a run asks for after the camera part.                  */
#define TD_BTN_STEPS   6U    /**< Steps of the guided button test.                               */
#define TD_TP_STEPS    2U    /**< Targets of the guided touch test.                              */
#define TD_PRESS_MS    90U   /**< How long a press in these cases lasts.                         */
#define TD_TARGET_X    80    /**< The touch target the cases use.                                */
#define TD_TARGET_Y    120
#define TD_MISS_X      300   /**< A tap well off it.                                             */
#define TD_MISS_Y      400

/** @brief Run jobs and let rests pass until a line holds @p needle. @return Whether one did. */
static bool drive_until(const char *needle)
{
    for ( unsigned i = 0U; TD_ROUNDS > i; i++ )
    {
        td_take();
        if ( td_said(needle) )
        {
            return true;
        }
        if ( !td_job() )
        {
            td_wait_ms(TD_STEP_MS);
        }
    }
    td_dump();
    return false;
}

/** @brief A guided test's verdict, as AO_Buttons or AO_Touch publishes it. */
static void verdict(const char *name, bool ok)
{
    TestResultEvt v;

    (void)memset(&v, 0, sizeof(v));
    v.super  = (QEvt)QEVT_INITIALIZER(TEST_RESULT_SIG);
    v.ok     = ok;
    v.origin = TEST_ORIGIN_TEST_DOT;
    (void)strncpy(v.name, name, sizeof(v.name) - 1U);
    td_dispatch(&v.super);
}

/** @brief Start a run and take it through the two rows before the camera. */
static void run_to_camera(void)
{
    char rows[TD_ROWS_TEXT];

    td_up();
    td_verb_plain((uint8_t)TEST_DOT_VERB_RUN);
    td_wake_ok();
    TEST_ASSERT_EQUAL_UINT(1U, td_rows_asked(rows, sizeof(rows)));
    TEST_ASSERT_EQUAL_STRING("sd", rows);
    td_row_answer(AUTOTEST_ROW_PASS);
    TEST_ASSERT_EQUAL_UINT(1U, td_rows_asked(rows, sizeof(rows)));
    TEST_ASSERT_EQUAL_STRING("imu", rows);
    td_row_answer(AUTOTEST_ROW_PASS);
}

/** @brief Answer every row after the camera, then both operator tests: the run is over. */
static void run_finish(void)
{
    char rows[TD_ROWS_TEXT];
    unsigned n = 0U;

    while ( (TD_ROWS_AFTER >= n) && (td_rows_asked(rows, sizeof(rows)) == 1U) )
    {
        td_row_answer(AUTOTEST_ROW_PASS);
        n++;
    }
    TEST_ASSERT_EQUAL_UINT(TD_ROWS_AFTER, n);
    td_wait_ms(KODE_TEST_DOT_OP_SETTLE_MS);
    verdict("buttons", true);
    td_wait_ms(KODE_TEST_DOT_OP_SETTLE_MS);
    verdict("tp", true);
    td_take();
}

/** @brief One step of the guided button test, as AO_Buttons publishes it for this client. */
static void btn_step(uint8_t phase, uint8_t step, kdrv_button_t button, uint8_t pressed)
{
    ButtonsTestEvt b;

    (void)memset(&b, 0, sizeof(b));
    b.super   = (QEvt)QEVT_INITIALIZER(BTN_TEST_SIG);
    b.phase   = phase;
    b.step    = step;
    b.total   = TD_BTN_STEPS;
    b.button  = (uint8_t)button;
    b.pressed = pressed;
    b.origin  = TEST_ORIGIN_TEST_DOT;
    if ( BTN_TEST_PROMPT != phase )
    {
        b.ms = TD_PRESS_MS;
    }
    td_dispatch(&b.super);
}

/** @brief One step of the guided touch test, as AO_Touch publishes it for this client. */
static void tp_step(uint8_t phase, uint8_t step, int16_t gx, int16_t gy)
{
    TouchTestEvt t;

    (void)memset(&t, 0, sizeof(t));
    t.super  = (QEvt)QEVT_INITIALIZER(TOUCH_TEST_SIG);
    t.phase  = phase;
    t.step   = step;
    t.total  = TD_TP_STEPS;
    t.tx     = TD_TARGET_X;
    t.ty     = TD_TARGET_Y;
    t.gx     = gx;
    t.gy     = gy;
    t.origin = TEST_ORIGIN_TEST_DOT;
    td_dispatch(&t.super);
}

/** @brief The bit of @p b in a step's `pressed`. */
static uint8_t bit_of(kdrv_button_t b)
{
    return (uint8_t)(1U << (unsigned)b);
}

TEST_CASE("test_rows_in_order_one_at_a_time", "[test_dot_run]")
{
    char rows[TD_ROWS_TEXT];
    bool seen_ok = false;

    run_to_camera();
    TEST_ASSERT_TRUE(drive_until("rec test=imu result=pass"));
    TEST_ASSERT_TRUE(td_said_at("rec test=sd result=pass") < td_said_at("rec test=imu result=pass"));

    /* The camera part asks for no row; its end opens the rest. */
    for ( unsigned i = 0U; (TD_ROUNDS > i) && !seen_ok; i++ )
    {
        td_take();
        seen_ok = td_said(">ok");
        if ( !seen_ok )
        {
            TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, td_rows_asked(rows, sizeof(rows)), "no row under the camera");
            if ( !td_job() )
            {
                td_wait_ms(TD_STEP_MS);
            }
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(seen_ok, "the camera part ends");
    TEST_ASSERT_TRUE(td_said("rec timing=camera"));
    TEST_ASSERT_TRUE_MESSAGE(td_said_at("screen 0x05") > td_said_at(">ok"), "the buttons after the line");
    TEST_ASSERT_EQUAL_UINT(1U, td_rows_asked(rows, sizeof(rows)));
    TEST_ASSERT_EQUAL_STRING("audio", rows);

    /* One at a time: the next only once this one ended. */
    td_row_answer(AUTOTEST_ROW_FAIL);
    TEST_ASSERT_EQUAL_UINT(1U, td_rows_asked(rows, sizeof(rows)));
    TEST_ASSERT_EQUAL_STRING("haptic", rows);
    TEST_ASSERT_EQUAL_UINT(0U, td_rows_asked(rows, sizeof(rows)));
    td_end();
    td_down();
}

TEST_CASE("test_line_closes_at_the_camera_end_and_poll_drains_the_rest", "[test_dot_run]")
{
    run_to_camera();
    TEST_ASSERT_TRUE(drive_until(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_said_n(">ok"), "the run's line answered once");

    td_clear();
    td_row_answer(AUTOTEST_ROW_PASS);   /* audio */
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said("test=audio"), "the line is closed: nothing goes out unasked");

    td_verb_plain((uint8_t)TEST_DOT_VERB_POLL);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec test=audio result=pass"));
    TEST_ASSERT_TRUE(td_said("rec poll state=running"));
    TEST_ASSERT_TRUE(td_said_at("rec test=audio") < td_said_at("rec poll state="));
    TEST_ASSERT_EQUAL_UINT(1U, td_said_n(">ok"));

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_POLL);
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said("test=audio"), "a record is drained once");
    TEST_ASSERT_TRUE(td_said("rec poll state=running"));
    td_end();
    td_down();
}

TEST_CASE("test_pass_needs_a_finished_run", "[test_dot_run]")
{
    TestDotReqEvt r;
    FactoryResetRspEvt d;

    td_up();
    (void)memset(&r, 0, sizeof(r));
    r.verb   = (uint8_t)TEST_DOT_VERB_RESULT;
    r.a.pass = true;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=4 msg=no_run"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG), "nothing erased");
    td_end();

    /* A whole run, then the pass. */
    run_to_camera();
    TEST_ASSERT_TRUE(drive_until(">ok"));
    run_finish();
    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_POLL);
    /* Paced by the pool's margin: the poll's own record comes last. */
    TEST_ASSERT_TRUE(drive_until("rec poll state=done"));
    TEST_ASSERT_TRUE_MESSAGE(td_said("rec timing=all"), "the run's end is in the poll ring");
    td_clear();
    td_verb(&r);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG), "the SHIP reset asked");
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said(">ok"), "the verb waits for the reset");

    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(FACTORY_RESET_RSP_SIG);
    d.step   = (uint8_t)KBOOT_RESET_STEP_DONE;
    d.status = (uint8_t)KODE_OK;
    td_dispatch(&d.super);
    td_take();
    TEST_ASSERT_TRUE_MESSAGE(td_said("screen 0x07 o"), "PASS on the glass");
    td_take();   /* the answer follows the screen shown */
    TEST_ASSERT_TRUE(td_said(">ok"));

    /* After DONE the lock is still taken, and the end. */
    td_clear();
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_LOCK;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE(td_said(">ok"));
    td_clear();
    td_end();
    TEST_ASSERT_TRUE(td_said(">ok"));
    td_down();
}

TEST_CASE("test_second_pass_is_busy", "[test_dot_run]")
{
    TestDotReqEvt r;

    run_to_camera();
    TEST_ASSERT_TRUE(drive_until(">ok"));
    run_finish();
    (void)memset(&r, 0, sizeof(r));
    r.verb   = (uint8_t)TEST_DOT_VERB_RESULT;
    r.a.pass = true;
    td_verb(&r);
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));
    td_clear();
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG), "one reset only");
    td_end();
    td_down();
}

TEST_CASE("test_fail_stays_on_the_glass", "[test_dot_run]")
{
    TestDotReqEvt r;

    run_to_camera();
    TEST_ASSERT_TRUE(drive_until(">ok"));
    run_finish();
    td_clear();
    (void)memset(&r, 0, sizeof(r));
    r.verb   = (uint8_t)TEST_DOT_VERB_RESULT;
    r.a.pass = false;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE_MESSAGE(td_said("screen 0x07 x"), "FAIL on the glass");
    td_take();   /* the answer follows the screen shown */
    TEST_ASSERT_TRUE(td_said(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG), "a fail erases nothing");

    /* The station says nothing more: nothing takes the FAIL away, not even
     * past the longest timer the AO arms (the run's watchdog). */
    td_clear();
    td_wait_ms(KODE_TEST_DOT_RUN_MS + KODE_TEST_DOT_BLOB_MS);
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said("screen"), "the FAIL screen stays");
    td_down();
}

TEST_CASE("test_reset_mid_run_arms_nothing", "[test_dot_run]")
{
    uint8_t v = 0U;

    run_to_camera();
    TEST_ASSERT_EQUAL_UINT(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));
    TEST_ASSERT_NOT_EQUAL_MESSAGE(KODE_OK, khal_nvs_get_u8(KODE_STATION_NVS_NS, KODE_TEST_DOT_LOCK_KEY, &v),
                                  "no lock stored");

    /* The reset: the AO built afresh, as a boot does. */
    td_up();
    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_take();
    TEST_ASSERT_TRUE(td_said("state=dormant locked=0 run=none"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_station_probes());
    td_down();
}

TEST_CASE("test_fail_during_a_run_ends_it_and_stays", "[test_dot_run]")
{
    char rows[TD_ROWS_TEXT];
    TestDotReqEvt r;
    unsigned n = 0U;

    run_to_camera();
    TEST_ASSERT_TRUE(drive_until(">ok"));
    while ( (TD_ROWS_AFTER >= n) && (td_rows_asked(rows, sizeof(rows)) == 1U) )
    {
        td_row_answer(AUTOTEST_ROW_PASS);
        n++;
    }
    td_wait_ms(KODE_TEST_DOT_OP_SETTLE_MS);   /* the buttons' test is asked */

    td_clear();
    (void)memset(&r, 0, sizeof(r));
    r.verb   = (uint8_t)TEST_DOT_VERB_RESULT;
    r.a.pass = false;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE_MESSAGE(td_said("screen 0x07 x"), "FAIL on the glass");
    td_take();   /* the answer follows the screen shown */
    TEST_ASSERT_TRUE(td_said(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_posted(AO_Buttons, BUTTONS_TEST_CANCEL_SIG), "the operator's test ends");
    TEST_ASSERT_EQUAL_UINT(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));

    /* The buttons' verdict arrives anyway, and the run's watchdog runs out:
     * nothing paints over the FAIL. */
    td_clear();
    verdict("buttons", true);
    td_wait_ms(KODE_TEST_DOT_RUN_MS + KODE_TEST_DOT_BLOB_MS);
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said("screen"), "the FAIL screen stays");

    /* A pass after the fail is refused and erases nothing. */
    td_clear();
    r.a.pass = true;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=5 msg=failed"));
    TEST_ASSERT_EQUAL_UINT(0U, td_posted(AO_Boot, FACTORY_RESET_REQ_SIG));
    td_end();
    td_down();
}

/* The operator's records read exactly as the console's `test component=buttons`
 * and `tp` print them (the station's words), and the verdicts are its: a give-up
 * is a fail (spec 8.3), a step nobody answered a timeout. */
TEST_CASE("test_operator_records_are_the_console_s_and_the_station_s_words", "[test_dot_run]")
{
    run_to_camera();
    TEST_ASSERT_TRUE(drive_until(">ok"));
    td_wait_ms(KODE_TEST_DOT_OP_SETTLE_MS);

    btn_step(BTN_TEST_PROMPT, 1U, KDRV_BTN_LEFT, 0U);
    btn_step(BTN_TEST_WRONG, 1U, KDRV_BTN_LEFT, bit_of(KDRV_BTN_DOWN));
    btn_step(BTN_TEST_DOUBLE, 1U, KDRV_BTN_LEFT, bit_of(KDRV_BTN_DOWN) | bit_of(KDRV_BTN_LEFT));
    btn_step(BTN_TEST_PASS, 1U, KDRV_BTN_LEFT, bit_of(KDRV_BTN_LEFT));
    btn_step(BTN_TEST_TIMEOUT, 2U, KDRV_BTN_BOT, 0U);
    btn_step(BTN_TEST_STUCK, 3U, KDRV_BTN_DOWN, bit_of(KDRV_BTN_DOWN));
    btn_step(BTN_TEST_HELD, 4U, KDRV_BTN_TOP, bit_of(KDRV_BTN_TOP));
    btn_step(BTN_TEST_DOUBLEFAIL, 5U, KDRV_BTN_RIGHT, bit_of(KDRV_BTN_RIGHT) | bit_of(KDRV_BTN_UP));
    btn_step(BTN_TEST_GIVEUP, 6U, KDRV_BTN_UP, bit_of(KDRV_BTN_LEFT));
    verdict("buttons", false);
    td_wait_ms(KODE_TEST_DOT_OP_SETTLE_MS);
    tp_step(TP_TEST_PROMPT, 1U, 0, 0);
    tp_step(TP_TEST_TIMEOUT, 1U, 0, 0);
    tp_step(TP_TEST_PROMPT, 2U, 0, 0);
    tp_step(TP_TEST_MISS, 2U, TD_MISS_X, TD_MISS_Y);
    tp_step(TP_TEST_PASS, 2U, TD_TARGET_X, TD_TARGET_Y);
    verdict("tp", false);
    td_take();

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_POLL);
    TEST_ASSERT_TRUE_MESSAGE(drive_until("rec poll state="), "the poll drains the ring");
    TEST_ASSERT_TRUE(td_said("rec step=1 of=6 press=pad-left"));
    TEST_ASSERT_TRUE(td_said("rec step=1 button=pad-left result=wrong pressed=pad-down ms=90"));
    TEST_ASSERT_TRUE(td_said("rec step=1 button=pad-left result=double pressed=pad-down+pad-left ms=90"));
    TEST_ASSERT_TRUE(td_said("rec step=1 button=pad-left result=pass ms=90"));
    TEST_ASSERT_TRUE(td_said("rec step=2 button=bottom result=timeout"));
    TEST_ASSERT_TRUE(td_said("rec step=3 button=pad-down result=stuck pressed=pad-down ms=90"));
    TEST_ASSERT_TRUE(td_said("rec step=4 button=top result=held pressed=top ms=90"));
    TEST_ASSERT_TRUE(td_said("rec step=5 button=pad-right result=doublefail pressed=pad-up+pad-right ms=90"));
    TEST_ASSERT_TRUE(td_said("rec step=6 button=pad-up result=giveup pressed=pad-left ms=90"));
    TEST_ASSERT_FALSE_MESSAGE(td_said("pressed=0x"), "names, never the raw byte");
    TEST_ASSERT_TRUE_MESSAGE(td_said("rec test=buttons result=fail ms="), "a give-up is a fail, timeout or not");
    TEST_ASSERT_TRUE(td_said("rec step=1 of=2 target=80,120"));
    TEST_ASSERT_TRUE(td_said("rec step=1 target=80,120 result=timeout"));
    TEST_ASSERT_FALSE_MESSAGE(td_said("result=timeout got="), "no tap, no got");
    TEST_ASSERT_FALSE(td_said("got=0,0 result=timeout"));
    TEST_ASSERT_TRUE(td_said("rec step=2 target=80,120 got=300,400 result=miss"));
    TEST_ASSERT_TRUE(td_said("rec step=2 target=80,120 got=80,120 result=pass"));
    TEST_ASSERT_TRUE_MESSAGE(td_said("rec test=tp result=timeout ms="), "a step nobody answered");
    td_end();
    td_down();
}
