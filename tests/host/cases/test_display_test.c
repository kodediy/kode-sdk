/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_display_test.c
 * @brief The `display` row: what the panel is painted, what is asked, and what an answer does.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkdrv_display.h"
#include "Mockkdrv_jpeg.h"
#include "Mockkdrv_ppa.h"
#include "kdrv_buttons.h"
#include "kdrv_tca6408a.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_board.h"
#include "kqp.h"
#include "qf_support.h"

/* QP's subscriber sets, one per published signal. Package-private to QP and
 * declared here rather than reached through qp_pkg.h, which hides it behind
 * QP_IMPL: one case below has to ask whether an AO is still subscribed, and
 * nothing public answers that. */
extern QSubscrList * QActive_subscrList_;

/** @brief The operator's deadline for one field in whole QP ticks, and one
 *         past it. */
#define STEP_TICKS    ((KODE_DISPLAY_TEST_STEP_MS / 10U) + 1U)

/** @brief The opening screen and the renderer's last frame, in whole steps. */
#define INTRO_STEPS   (KODE_DISPLAY_TEST_INTRO_MS / 10U)
#define SETTLE_STEPS  (KODE_DISPLAY_TEST_SETTLE_MS / 10U)

/** @brief Enough steps to run out a row left anywhere: its opening, its last
 *         frame and one field's deadline, with room. */
#define DRAIN_STEPS   (INTRO_STEPS + SETTLE_STEPS + STEP_TICKS + 3U)

/** @brief How many bands one field costs: 502 rows, 16 at a time. */
#define BAND_ROWS     ((int32_t)KODE_DISPLAY_TEST_BAND_ROWS)
#define BANDS         ((((int32_t)KBSP_LCD_V_RES) + BAND_ROWS - 1) / BAND_ROWS)

/* The five fields as native RGB565, pinned against literals rather than
 * against the very definitions the row is meant to be checked against. */
#define RGB_WHITE     0xFFFFU
#define RGB_RED       0xF800U
#define RGB_GREEN     0x07E0U
#define RGB_BLUE      0x001FU
#define RGB_BLACK     0x0000U

/** @brief The brightness the glass is left at before every row here, so the
 *         restore on the way out has a number to be wrong about. */
#define BRIGHT_BEFORE 42U

/* ---- What reached the driver ---------------------------------------------- */

#define FLUSH_MAX 64

static int32_t  s_y1[FLUSH_MAX];
static int32_t  s_y2[FLUSH_MAX];
static int32_t  s_x1[FLUSH_MAX];
static int32_t  s_x2[FLUSH_MAX];
static bool     s_last[FLUSH_MAX];
static uint16_t s_colour[FLUSH_MAX];
static bool     s_mixed[FLUSH_MAX];
static unsigned s_flushes;
static unsigned s_waits;
static unsigned s_aborts;
static uint8_t  s_driven;
static unsigned s_drives;

static kode_status_t on_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                              const void *pixels, bool last, int calls)
{
    (void)calls;
    if ( (unsigned)FLUSH_MAX > s_flushes )
    {
        const uint16_t *px = (const uint16_t *)pixels;
        size_t const    n  = (size_t)(x2 - x1) * (size_t)(y2 - y1);
        bool            mixed = false;

        for ( size_t i = 1U; i < n; ++i )
        {
            if ( px[i] != px[0] )
            {
                mixed = true;
                break;
            }
        }
        s_x1[s_flushes]     = x1;
        s_x2[s_flushes]     = x2;
        s_y1[s_flushes]     = y1;
        s_y2[s_flushes]     = y2;
        s_last[s_flushes]   = last;
        s_colour[s_flushes] = px[0];
        s_mixed[s_flushes]  = mixed;
    }
    ++s_flushes;
    return KODE_OK;
}

/** @brief Whether a completion stands in the driver's semaphore that the
 *         renderer has not collected - what a zero-timeout take finds. */
static bool s_standing;

static bool on_flush_wait(uint32_t ms, int calls)
{
    (void)calls;
    if ( 0U == ms )
    {
        /* Not a band's wait: the row looking for a token that is already
         * there, before it paints over it. */
        bool const had = s_standing;
        s_standing = false;
        return had;
    }
    ++s_waits;
    return true;
}

static void on_flush_abort(int calls)
{
    (void)calls;
    ++s_aborts;
}

static kode_status_t on_brightness(uint8_t pct, int calls)
{
    (void)calls;
    s_driven = pct;
    ++s_drives;
    return KODE_OK;
}

/** @brief Forget everything the driver was told. */
static void driver_forget(void)
{
    s_flushes = 0U;
    s_waits   = 0U;
    s_aborts  = 0U;
    s_drives  = 0U;
    s_driven  = 0xFFU;
    s_standing = false;
}

/* ---- Bringing the AO up ---------------------------------------------------- */

/**
 * @brief Bring AO_Display up.
 *
 * test_display_ao.c's display_up_once(), copied down to the supply
 * handshake - qf_support_claim_ao() is what keeps three files from
 * registering the same AO three times.
 */
static void display_test_up_once(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_subscribe(DISPLAY_STATE_SIG);
    ao_spy_subscribe(DISPLAY_TEST_SIG);
    ao_spy_subscribe(DISPLAY_REPAINT_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);
    ao_spy_drain();

    if ( qf_support_claim_ao(AO_Display) )
    {
        kdrv_display_init_IgnoreAndReturn(KODE_OK);
        kdrv_jpeg_init_IgnoreAndReturn(KODE_OK);
        kdrv_ppa_init_IgnoreAndReturn(KODE_OK);
        kdrv_display_brightness_IgnoreAndReturn(KODE_OK);

        Display_ctor();
        qf_support_give_queue(AO_Display);
        AO_Display->prio  = KODE_PRIO_DISPLAY;
        AO_Display->pthre = KODE_PRIO_DISPLAY;
        QActive_register_(AO_Display);
        QASM_INIT(AO_Display, (void *)0, 0U);

        /* The panel's supply is P7 of the expander, asked for by event and
         * answered twice before the panel is touched. test_display_ao.c
         * asserts on that handshake; here it is only walked. */
        IoexpFieldEvt a;
        (void)memset(&a, 0, sizeof(a));
        a.super = (QEvt)QEVT_INITIALIZER(IOEXP_FIELD_RSP_SIG);
        a.op    = FIELD_OP_SET;
        a.rc    = (uint8_t)KODE_OK;
        a.field = (uint16_t)KDRV_TCA_O7;
        a.value = 1;
        QASM_DISPATCH(AO_Display, &a.super, 0U);
        a.field = (uint16_t)KDRV_TCA_C7;
        a.value = 0;
        QASM_DISPATCH(AO_Display, &a.super, 0U);
    }

    kdrv_display_flush_StubWithCallback(&on_flush);
    kdrv_display_flush_wait_StubWithCallback(&on_flush_wait);
    kdrv_display_flush_abort_StubWithCallback(&on_flush_abort);
    kdrv_display_brightness_StubWithCallback(&on_brightness);

    /* A case that failed mid-row was abandoned by Unity where it stood, so
     * this AO can arrive here inside the row: paint whatever was going up
     * and let the deadline behind it pass. */
    (void)qf_support_pump(AO_Display, (unsigned)BANDS + 8U);
    (void)qf_support_tick(AO_Display, DRAIN_STEPS);

    /* A number for the row to restore, and a state event that carries it. */
    BrightnessEvt b;
    (void)memset(&b, 0, sizeof(b));
    b.super   = (QEvt)QEVT_INITIALIZER(DISPLAY_BRIGHTNESS_SIG);
    b.percent = (uint8_t)BRIGHT_BEFORE;
    b.persist = false;
    QASM_DISPATCH(AO_Display, &b.super, 0U);

    driver_forget();
    fake_klog_reset();
    ao_spy_drain();
}

/**
 * @brief Run out whatever the case left in flight and give the events back.
 *
 * A row still waiting for an answer holds an armed QTimeEvt, and an armed
 * one left behind is what corrupts QF's list for whichever file constructs
 * an AO next. Every case here therefore ends with the row over, either
 * because it finished or because its deadline was allowed to pass.
 */
static void display_test_down(void)
{
    (void)qf_support_pump(AO_Display, (unsigned)BANDS + 8U);
    (void)qf_support_tick(AO_Display, DRAIN_STEPS);
    ao_spy_drain();
}

/* ---- Driving the row ------------------------------------------------------- */

/** @brief Ask for the row the way `test component=display` does. */
static void row_request(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_TEST_REQ_SIG);

    QASM_DISPATCH(AO_Display, &req, 0U);
}

/** @brief One edge of one button, as AO_Buttons publishes it. */
static void button(kdrv_button_t id, bool pressed)
{
    ButtonEvt e = { .super = QEVT_INITIALIZER(BUTTON_EVT_SIG),
                    .id = 0U, .pressed = false };

    e.id      = (uint8_t)id;
    e.pressed = pressed;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/** @brief The operator says yes: a press of BUTTON_TOP, and its release. */
static void answer_yes(void)
{
    button(KDRV_BTN_TOP, true);
    button(KDRV_BTN_TOP, false);
}

/** @brief The operator says no: a press of BUTTON_BOT, and its release. */
static void answer_no(void)
{
    button(KDRV_BTN_BOT, true);
    button(KDRV_BTN_BOT, false);
}

/** @brief What @p field is painted as. */
static uint16_t field_colour(uint8_t field)
{
    static const uint16_t RGB[DISPLAY_FIELD_COUNT] = {
        RGB_WHITE, RGB_RED, RGB_GREEN, RGB_BLUE, RGB_BLACK
    };

    return RGB[field];
}

/**
 * @brief Let the AO paint one whole field, and say what reached the driver.
 *
 * One band per self-posted step, so the pump has to carry the chain: what is
 * asserted is that the bands cover the panel exactly once, in @p colour, and
 * that every one of them was waited for.
 *
 * @param colour The RGB565 the band should be filled with.
 */
static void paint_field(uint16_t colour)
{
    unsigned const ran = qf_support_pump(AO_Display, (unsigned)BANDS + 8U);

    /* One more at most: the look at the glass may have a tick queued. */
    TEST_ASSERT_UINT_WITHIN_MESSAGE(1U, (unsigned)BANDS, ran,
                                    "the field was not painted one band per step");
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)BANDS, s_flushes,
                                   "the bands do not add up to a panel");
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)BANDS, s_waits,
                                   "a band was handed to the DMA and never waited for");

    for ( int32_t i = 0; BANDS > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT32(0, s_x1[i]);
        TEST_ASSERT_EQUAL_INT32((int32_t)KBSP_LCD_H_RES, s_x2[i]);
        TEST_ASSERT_EQUAL_INT32(i * BAND_ROWS, s_y1[i]);
        TEST_ASSERT_EQUAL_UINT16_MESSAGE(colour, s_colour[i],
                                         "the panel was painted the wrong colour");
        TEST_ASSERT_FALSE_MESSAGE(s_mixed[i], "the band was not one colour");
    }
    TEST_ASSERT_EQUAL_INT32_MESSAGE((int32_t)KBSP_LCD_V_RES, s_y2[BANDS - 1],
                                    "the last rows of the panel were never painted");
    TEST_ASSERT_TRUE_MESSAGE(s_last[BANDS - 1],
                             "the last band did not say it was the last");
    driver_forget();
}

/** @brief Take the next event the spy holds and insist it is @p sig. */
static const QEvt *spy_expect(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the row said nothing at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)sig, (int)e->sig, "the wrong record");
    return e;
}

/** @brief Take the next record and insist on its step, phase and field. */
static void record_expect(uint8_t field, uint8_t phase)
{
    const DisplayTestEvt *r = (const DisplayTestEvt *)spy_expect(DISPLAY_TEST_SIG);

    TEST_ASSERT_EQUAL_UINT8_MESSAGE(phase, r->phase, "the wrong kind of record");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(field, r->field, "a record about the wrong field");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)(field + 1U), r->step,
                                    "a question and its outcome share the field's step");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)DISPLAY_FIELD_COUNT, r->total);
}

/**
 * @brief Insist the row ended, with @p ok, and gave back everything it took:
 *        the closing record, the verdict, the repaint, and the glass at the
 *        brightness the user chose.
 */
static void verdict_expect(uint8_t field, bool ok)
{
    record_expect(field, (uint8_t)DISPLAY_TEST_DONE);

    const TestResultEvt *v = (const TestResultEvt *)spy_expect(TEST_RESULT_SIG);

    TEST_ASSERT_EQUAL_STRING("display", v->name);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)ok, (int)v->ok, "the wrong verdict");
    (void)spy_expect(DISPLAY_REPAINT_SIG);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)BRIGHT_BEFORE, s_driven,
        "the glass was left at the row's own brightness");
}

/**
 * @brief Run the steps until the renderer's last frame is over and the first
 *        field is about to go up, and no further: the first band is posted,
 *        not painted.
 */
static void fields_wait(void)
{
    driver_forget();
    for ( unsigned i = 0U; (0U == s_drives) && ((SETTLE_STEPS + 2U) > i); ++i )
    {
        (void)qf_support_pump(AO_Display, 1U);
    }
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_DISPLAY_TEST_BRIGHT, s_driven,
        "the fields are shown at whatever the user had the glass at");
}

/**
 * @brief Ask for the row and start its fields the way the operator does:
 *        the opening screen, then BUTTON_TOP.
 */
static void row_start(void)
{
    row_request();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_INTRO);
    answer_yes();
    fields_wait();
}

/**
 * @brief Start the row and confirm every field before @p field.
 *
 * Leaves the question about @p field outstanding, so the case decides what
 * the operator says.
 *
 * @param field DISPLAY_FIELD_*
 */
static void row_to_field(uint8_t field)
{
    row_start();
    driver_forget();
    for ( uint8_t f = (uint8_t)DISPLAY_FIELD_WHITE; ; ++f )
    {
        TEST_ASSERT_NULL_MESSAGE(ao_spy_take(),
            "the question went out before the field was on the glass");
        paint_field(field_colour(f));
        record_expect(f, (uint8_t)DISPLAY_TEST_ASK);
        if ( f == field )
        {
            break;
        }
        answer_yes();
        record_expect(f, (uint8_t)DISPLAY_TEST_PASS);
    }
}

/* ---- 0. The opening screen ------------------------------------------------- */

/** @brief Ask the renderer's way for a 10 x 10 corner of one colour. */
static void renderer_flush(void)
{
    static uint8_t px[10U * 10U * sizeof(uint16_t)];
    FlushEvt f;

    (void)memset(&f, 0, sizeof(f));
    f.super  = (QEvt)QEVT_INITIALIZER(DISPLAY_FLUSH_SIG);
    f.x2     = 10;
    f.y2     = 10;
    f.pixels = px;
    f.last   = true;
    QASM_DISPATCH(AO_Display, &f.super, 0U);
}

/* The fields carry no words, so the row opens on the renderer's glass for it
 * to say what the pad will mean: its frames still reach the panel, nothing
 * is painted, the brightness is the user's, and only BUTTON_TOP moves on. */
TEST_CASE("the row opens on the renderer's glass and paints nothing",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_request();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_INTRO);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_drives,
        "the glass was driven to the row's brightness before any field");

    renderer_flush();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_flushes,
        "the opening screen never reached the panel");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_aborts,
        "a frame that went to the panel was also released by hand");

    answer_no();
    button(KDRV_BTN_UP, true);
    (void)qf_support_pump(AO_Display, 8U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "the opening screen took an answer");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_flushes,
        "a field went up with nobody having started it");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* An operator who reads and does not press is not stuck there: the fields
 * start on their own once KODE_DISPLAY_TEST_INTRO_MS and the renderer's last
 * frame have passed, and not a step sooner. */
TEST_CASE("the fields start on their own after the opening screen",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_request();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_INTRO);
    (void)qf_support_tick(AO_Display, INTRO_STEPS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_flushes, "the fields started before their time");
    TEST_ASSERT_NULL(ao_spy_take());

    (void)qf_support_tick(AO_Display, SETTLE_STEPS + 3U);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_DISPLAY_TEST_BRIGHT, s_driven,
        "the fields are shown at whatever the user had the glass at");
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)BANDS, s_flushes,
        "the first field was not painted once the opening screen ended");
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_ASK);

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* A frame the renderer began on the opening screen may still be on its way
 * when BUTTON_TOP is pressed. It must land before the first band, never on
 * the field: held, not painted, and released once the field is up. */
TEST_CASE("a frame after the opening screen is held until the field is up",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_request();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_INTRO);
    answer_yes();
    driver_forget();
    renderer_flush();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_flushes,
        "a frame after the opening screen reached the panel");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_aborts,
        "the renderer was released before the field, and draws on over it");

    fields_wait();
    (void)qf_support_pump(AO_Display, (unsigned)BANDS + 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)BANDS, s_flushes,
        "the held frame took a band's place on the glass");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_aborts,
        "the held frame was never released, and the renderer sits out its deadline");
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_ASK);

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* ---- 1. What the row does to a renderer ------------------------------------ */

/* AO_Gui waits for a flush inside its own render call with a deadline of its
 * own, so a rectangle arriving mid-row cannot be deferred and must not reach
 * the panel either - it would paint the launcher over the field being asked
 * about. Completed and dropped is the only answer that is both. */
TEST_CASE("a flush during the row is completed without a transfer",
          "[display_test]")
{
    FlushEvt f;

    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_to_field((uint8_t)DISPLAY_FIELD_WHITE);

    (void)memset(&f, 0, sizeof(f));
    f.super = (QEvt)QEVT_INITIALIZER(DISPLAY_FLUSH_SIG);
    f.x2    = 10;
    f.y2    = 10;
    QASM_DISPATCH(AO_Display, &f.super, 0U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_aborts,
        "the renderer was left waiting for a completion nobody owed it");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_flushes,
        "the launcher was painted over the field being asked about");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* The driver's completion is one binary semaphore, shared by the renderer's
 * wait and the row's band waits, and LVGL collects a completion only when it
 * starts its NEXT flush - so between frames a token stands there that is
 * already spoken for. A band painted over it loses its own signal on a full
 * semaphore and takes the standing one, and the renderer's next frame sits
 * out its whole deadline: `the panel never reported a flush done in 500 ms`,
 * seen on a board once a field. The row therefore takes a standing token into
 * keeping before a field goes up, and pays it back when the last band has
 * landed. */
TEST_CASE("a completion the renderer has not collected is kept for it, not spent on a band",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_start();
    driver_forget();
    (void)qf_support_pump(AO_Display, (unsigned)BANDS + 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_aborts,
        "a completion was handed over that nobody was owed");
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_ASK);

    /* The renderer draws while the question is out and leaves its last
     * completion standing; the next field must not spend it. */
    driver_forget();
    s_standing = true;
    answer_yes();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_PASS);
    TEST_ASSERT_FALSE_MESSAGE(s_standing, "the token was left where a band takes it");
    (void)qf_support_pump(AO_Display, (unsigned)BANDS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_aborts,
        "the token went back with bands still to wait for");
    (void)qf_support_pump(AO_Display, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_aborts,
        "the renderer's completion was spent, and its next frame will sit out its deadline");
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)BANDS, s_waits,
        "looking for a standing token was counted as a band's wait");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* The same debt from the other side: a flush the renderer asks for while a
 * field is going up is not answered between two bands, where the next band's
 * wait would take the answer. */
TEST_CASE("a flush while a field is going up is released once the field is up",
          "[display_test]")
{
    FlushEvt f;

    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_start();
    driver_forget();

    (void)memset(&f, 0, sizeof(f));
    f.super = (QEvt)QEVT_INITIALIZER(DISPLAY_FLUSH_SIG);
    f.x2    = 10;
    f.y2    = 10;
    QASM_DISPATCH(AO_Display, &f.super, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_aborts,
        "the renderer was released between two bands, where a band takes its token");

    /* All but the last band: still owed. */
    (void)qf_support_pump(AO_Display, (unsigned)BANDS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_aborts,
        "the renderer was released with bands still to wait for");

    (void)qf_support_pump(AO_Display, 8U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)BANDS, s_flushes, "the field did not finish");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_aborts,
        "the renderer was never released, and will sit out its deadline");
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_ASK);

    /* And one that arrives with the question out is released at once. */
    QASM_DISPATCH(AO_Display, &f.super, 0U);
    TEST_ASSERT_EQUAL_UINT(2U, s_aborts);

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* ---- 2. The band arithmetic ------------------------------------------------ */

/* 502 rows do not divide by 16. A row that painted 31 whole bands and stopped
 * would leave the last six rows of the panel showing the launcher, on every
 * field an operator is asked to call uniform. */
TEST_CASE("the bands cover all 502 rows, the last one short", "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_start();
    driver_forget();
    (void)qf_support_pump(AO_Display, (unsigned)BANDS + 8U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(32U, (unsigned)BANDS, "502 rows, 16 at a time");
    TEST_ASSERT_EQUAL_UINT(32U, s_flushes);
    TEST_ASSERT_EQUAL_INT32(0, s_y1[0]);
    TEST_ASSERT_EQUAL_INT32(496, s_y1[BANDS - 1]);
    TEST_ASSERT_EQUAL_INT32_MESSAGE(502, s_y2[BANDS - 1],
        "the last six rows of the glass were never painted");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* ---- 3. The whole row ------------------------------------------------------ */

/* The order is the contract: a field painted whole, then its question, then
 * the answer, then the next field - white, red, green, blue, black - and the
 * glass handed back at the end. */
TEST_CASE("five confirmed fields pass and the glass is given back",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_to_field((uint8_t)DISPLAY_FIELD_BLACK);
    answer_yes();
    record_expect((uint8_t)DISPLAY_FIELD_BLACK, (uint8_t)DISPLAY_TEST_PASS);
    verdict_expect((uint8_t)DISPLAY_FIELD_BLACK, true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_flushes,
        "the row painted something after it had already answered");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* ---- 4. The two ways a field fails ------------------------------------------ */

/* Red is the field both are shown on, so that "the row stops there" has a
 * field before it that passed and three after it that must never go up. */
TEST_CASE("a field the operator refuses fails the row and ends it",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_to_field((uint8_t)DISPLAY_FIELD_RED);
    answer_no();
    record_expect((uint8_t)DISPLAY_FIELD_RED, (uint8_t)DISPLAY_TEST_FAIL);
    verdict_expect((uint8_t)DISPLAY_FIELD_RED, false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_pump(AO_Display, 8U),
        "the row carried on to green past a red it had already failed");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* An operator cannot ship a unit nobody looked at, so an empty bench is a
 * failed verdict - and the row ends there rather than waiting out four more
 * fields for somebody who is not coming. */
TEST_CASE("a field nobody answers is a timeout and ends the row",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_to_field((uint8_t)DISPLAY_FIELD_RED);
    (void)qf_support_tick(AO_Display, STEP_TICKS - 2U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "the row gave up before its deadline");
    (void)qf_support_tick(AO_Display, 2U);
    record_expect((uint8_t)DISPLAY_FIELD_RED, (uint8_t)DISPLAY_TEST_TIMEOUT);
    verdict_expect((uint8_t)DISPLAY_FIELD_RED, false);

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* Every field has the whole of its deadline. The one that timed the field
 * before it was stopped when that field was answered, and must not be what
 * ends this one early. */
TEST_CASE("an answer gives the next field a deadline of its own",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_to_field((uint8_t)DISPLAY_FIELD_WHITE);
    (void)qf_support_tick(AO_Display, STEP_TICKS - 2U);
    answer_yes();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_PASS);
    paint_field(RGB_RED);
    record_expect((uint8_t)DISPLAY_FIELD_RED, (uint8_t)DISPLAY_TEST_ASK);

    (void)qf_support_tick(AO_Display, STEP_TICKS - 2U);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(),
        "red was timed by what was left of white's deadline");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* ---- 5. What is not an answer ------------------------------------------------ */

/* A press only means something once the question is out. One that lands
 * while the field is still going up is an operator answering the field
 * before it, twice - and taking it would confirm a colour nobody has seen. */
TEST_CASE("a press while a field is being painted is not an answer",
          "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_start();
    driver_forget();
    answer_yes();
    answer_no();
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "a field nobody has seen was answered for");
    paint_field(RGB_WHITE);
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_ASK);

    /* And once it is out: a release and the four directions are still not
     * answers. */
    button(KDRV_BTN_TOP, false);
    button(KDRV_BTN_BOT, false);
    button(KDRV_BTN_UP, true);
    button(KDRV_BTN_DOWN, true);
    button(KDRV_BTN_LEFT, true);
    button(KDRV_BTN_RIGHT, true);
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "something that is not an answer was taken for one");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, qf_support_pump(AO_Display, 8U),
        "the row moved on without an answer");

    answer_yes();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_PASS);

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* ---- 6. What the row holds ------------------------------------------------- */

/* The row reads the pad, and only the row: outside it AO_Display has no
 * business with the buttons. The subscription is taken on the way in and
 * given back on the way out. The glass is never one of its inputs. */
TEST_CASE("the row holds the pad only for its own length", "[display_test]")
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    TEST_ASSERT_FALSE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[BUTTON_EVT_SIG].set,
                         (uint_fast8_t)AO_Display->prio),
        "the pad is not the panel owner's outside the row");
    TEST_ASSERT_FALSE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[TOUCH_EVT_SIG].set,
                         (uint_fast8_t)AO_Display->prio),
        "the glass is not the panel owner's either, outside the row");

    row_to_field((uint8_t)DISPLAY_FIELD_WHITE);
    TEST_ASSERT_TRUE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[BUTTON_EVT_SIG].set,
                         (uint_fast8_t)AO_Display->prio),
        "the row cannot hear the operator's answer");
    TEST_ASSERT_FALSE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[TOUCH_EVT_SIG].set,
                         (uint_fast8_t)AO_Display->prio),
        "the row listens to the glass: its answer is the pad's alone");

    answer_no();
    record_expect((uint8_t)DISPLAY_FIELD_WHITE, (uint8_t)DISPLAY_TEST_FAIL);
    verdict_expect((uint8_t)DISPLAY_FIELD_WHITE, false);

    TEST_ASSERT_FALSE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[BUTTON_EVT_SIG].set,
                         (uint_fast8_t)AO_Display->prio),
        "the row kept the pad after it ended");
    TEST_ASSERT_FALSE_MESSAGE(
        QPSet_hasElement(&QActive_subscrList_[TOUCH_EVT_SIG].set,
                         (uint_fast8_t)AO_Display->prio),
        "the row left a subscription to the glass behind");

    display_test_down();
    Mockkdrv_display_Destroy();
}

/* `testing` is a substate of `active`, and the whole point of that is
 * everything it does not override. A state request is answered from the same
 * event as ever - an AO starting mid-row would otherwise wait forever for a
 * panel that is right there. */
TEST_CASE("a state request during the row is still answered", "[display_test]")
{
    static const QEvt ask = QEVT_INITIALIZER(DISPLAY_STATE_REQ_SIG);

    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    display_test_up_once();

    row_to_field((uint8_t)DISPLAY_FIELD_WHITE);
    ao_spy_drain();

    QASM_DISPATCH(AO_Display, &ask, 0U);
    const DisplayStateEvt *s = (const DisplayStateEvt *)spy_expect(DISPLAY_STATE_SIG);
    TEST_ASSERT_TRUE(s->up);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)BRIGHT_BEFORE, s->bright,
        "the row's own brightness was announced as the user's");

    display_test_down();
    Mockkdrv_display_Destroy();
}
