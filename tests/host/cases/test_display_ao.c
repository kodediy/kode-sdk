/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_display_ao.c
 * @brief AO_Display's control path: the number that reaches the glass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkdrv_display.h"
#include "kdrv_tca6408a.h"
#include "Mockkdrv_jpeg.h"
#include "Mockkdrv_ppa.h"

#include "ao_spy.h"
#include "config_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kqp.h"
#include "kset.h"
#include "qf_support.h"

#if !CONFIG_KODE_LOWPOWER
#error "test_display_ao.c needs CONFIG_KODE_LOWPOWER: main/CMakeLists.txt spells it out"
#endif

/* ---- What the driver was asked, in order ---------------------------------- */

/** @brief One thing the driver was asked, for the order cases. */
typedef enum {
    T_BRIGHT = 1,   /**< kdrv_display_brightness(); the level is traced too.   */
    T_LINK_DOWN,    /**< kdrv_display_link_set(false).                         */
    T_LINK_UP,      /**< kdrv_display_link_set(true).                          */
    T_SLEEP_IN,     /**< kdrv_display_panel_sleep(true).                       */
    T_SLEEP_OUT,    /**< kdrv_display_panel_sleep(false).                      */
    T_GRAM,         /**< kdrv_display_gram_write().                            */
    T_RESET_LOW     /**< kdrv_display_reset_low().                             */
} trace_t;

/** @brief Longest sequence a case traces. */
#define TRACE_MAX   16U

static int      s_trace[TRACE_MAX];
static unsigned s_trace_n;

/** @brief Record one call. */
static void trace(trace_t what)
{
    if ( TRACE_MAX > s_trace_n )
    {
        s_trace[s_trace_n] = (int)what;
        ++s_trace_n;
    }
}

/** @brief What kdrv_display_brightness() was last given. */
static uint8_t  s_driven;
static unsigned s_drives;

static kode_status_t on_brightness(uint8_t pct, int calls)
{
    (void)calls;
    s_driven = pct;
    ++s_drives;
    trace(T_BRIGHT);
    return KODE_OK;
}

/** @brief The expander's owner answers one of the two supply requests. */
static void supply_answered(kdrv_tca6408a_field_t f, int32_t value)
{
    IoexpFieldEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(IOEXP_FIELD_RSP_SIG);
    a.op    = FIELD_OP_SET;
    a.field = (uint16_t)f;
    a.rc    = (uint8_t)KODE_OK;
    a.value = value;
    QASM_DISPATCH(AO_Display, &a.super, 0U);
}

/**
 * @brief Bring AO_Display up, once for the whole binary.
 *
 * Once, like the other polling AOs, though for a different reason: the initial
 * transition asks for the panel's supply and, once it has it, reads the store
 * and announces the panel - and re-running it would re-announce a panel every
 * subscriber already knows about.
 *
 * The supply is P7 of the expander, and the expander has an owner: the AO asks
 * AO_IoExp for the output level and then the direction, by event, and touches
 * the panel only when both answers are back. That handshake is what this
 * helper walks, asserting as it goes, because whichever case runs first is
 * the one that sees it.
 *
 * @param stored The brightness an earlier boot left behind, or 0 for none.
 */
static void display_up_once(uint8_t stored)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    ao_spy_subscribe(DISPLAY_STATE_SIG);


    /* qf_support_claim_ao() rather than a local static bool: this AO now has
     * a second file asking for it (test_display_mirror.c), and two once-flags
     * that cannot see each other is exactly the "two registrations" failure
     * qf_support.h's own doc comment describes - it is what the console's
     * cases hit first, and the fix is the same one used there. */
    if ( qf_support_claim_ao(AO_Display) )
    {
        if ( 0U != stored )
        {
            (void)khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, stored);
        }
        /* Claimed and started alongside the panel, but nothing here is about
         * the encoder: what the ignore buys is a return value for the call
         * the bring-up makes, not a behaviour any case asserts on. */
        kdrv_jpeg_init_IgnoreAndReturn(KODE_OK);
        /* Same reasoning as the encoder just above: nothing here is about
         * the PPA, only about the bring-up getting a return value for the
         * call it makes. */
        kdrv_ppa_init_IgnoreAndReturn(KODE_OK);
        kdrv_display_brightness_StubWithCallback(&on_brightness);

        unsigned const asked = fake_neighbour_ioexp_posts();
        Display_ctor();
        qf_support_give_queue(AO_Display);
        AO_Display->prio  = KODE_PRIO_DISPLAY;
        AO_Display->pthre = KODE_PRIO_DISPLAY;
        QActive_register_(AO_Display);
        QASM_INIT(AO_Display, (void *)0, 0U);

        /* The initial transition asked for O7 and touched nothing else: no
         * kdrv_display_init is expected yet, and the mock would say so. */
        TEST_ASSERT_EQUAL_UINT_MESSAGE(asked + 1U, fake_neighbour_ioexp_posts(),
                                       "the supply is asked from the expander's owner");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                       "nothing is announced before the supply is up");

        /* AO_Gui asks for the panel's state the moment kode_init() returns,
         * which is exactly now, while the supply is still being asked for.
         * The question is kept and answered once the panel is up. */
        static const QEvt early = QEVT_INITIALIZER(DISPLAY_STATE_REQ_SIG);
        QASM_DISPATCH(AO_Display, &early, 0U);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                       "a state asked for too early is deferred, not refused");

        supply_answered(KDRV_TCA_O7, 1);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(asked + 2U, fake_neighbour_ioexp_posts(),
                                       "then the direction is asked for");
        TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

        kdrv_display_init_ExpectAndReturn(KODE_OK);
        supply_answered(KDRV_TCA_C7, 0);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "the announcement");
        /* The recalled question is re-queued, not re-dispatched: the AO takes
         * it as its next event, which under direct dispatch is a pump. */
        TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Display, 4U));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, ao_spy_count(),
                                       "and the answer to the early question");
        kdrv_display_init_IgnoreAndReturn(KODE_OK);
    }

    kdrv_display_brightness_StubWithCallback(&on_brightness);
    /* The look at the glass runs on its own timer: quiet unless a case asks. */
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    fake_klog_reset();
    ao_spy_drain();
}

/** @brief Ask for a brightness the way the GUI and AppMgr do. */
static void ask_brightness(uint8_t pct, bool persist)
{
    BrightnessEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super   = (QEvt)QEVT_INITIALIZER(DISPLAY_BRIGHTNESS_SIG);
    e.percent = pct;
    e.persist = persist;

    s_drives = 0U;
    s_driven = 0xFFU;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/** @brief What the panel state announcement currently says, via a re-ask. */
static const DisplayStateEvt *announced(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_STATE_REQ_SIG);

    ao_spy_drain();
    QASM_DISPATCH(AO_Display, &req, 0U);

    const QEvt * const e = ao_spy_take();
    if ( (NULL == e) || (DISPLAY_STATE_SIG != e->sig) )
    {
        return NULL;
    }
    return (DisplayStateEvt const *)e;
}

/* ---- The rest: stand-ins for the driver's three new calls ------------------ */

/** @brief What the link calls answer. */
static kode_status_t s_link_down_rc;
static kode_status_t s_link_up_rc;

/** @brief What the sleep-in and the reset line's drive answer. */
static kode_status_t s_sleep_in_rc;
static kode_status_t s_reset_low_rc;

/** @brief Requests AO_IoExp's queue held when the reset line went low. */
static unsigned s_posts_at_reset;

/** @brief Answers AO_LowPower already held when the link went down / the
 *         minute was written: the order of "rest, then answer". */
static unsigned s_ready_at_link;
static unsigned s_ready_at_gram;

/** @brief Flushes the driver was handed / completed by hand. */
static unsigned s_flush_sent;
static unsigned s_flush_aborted;

/** @brief The last rectangle written into the panel's memory. */
#define GRAM_PX_MAX   128U
static unsigned s_grams;
static unsigned s_gram_fail;   /* the next calls that fail, then they succeed */
static uint16_t s_gram_x;
static uint16_t s_gram_y;
static uint16_t s_gram_w;
static uint16_t s_gram_h;
static uint16_t s_gram_px[GRAM_PX_MAX];

static unsigned lp_queued(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_LowPower->eQueue);
}

static kode_status_t on_link(bool up, int calls)
{
    kode_status_t rc = s_link_up_rc;

    (void)calls;
    if ( up )
    {
        trace(T_LINK_UP);
    }
    else
    {
        trace(T_LINK_DOWN);
        s_ready_at_link = lp_queued();
        rc = s_link_down_rc;
    }
    return rc;
}

static kode_status_t on_panel_sleep(bool sleep, int calls)
{
    kode_status_t rc = KODE_OK;

    (void)calls;
    if ( sleep )
    {
        trace(T_SLEEP_IN);
        rc = s_sleep_in_rc;
    }
    else
    {
        trace(T_SLEEP_OUT);
    }
    return rc;
}

static kode_status_t on_reset_low(int calls)
{
    (void)calls;
    trace(T_RESET_LOW);
    s_posts_at_reset = fake_neighbour_ioexp_posts();
    return s_reset_low_rc;
}

static kode_status_t on_gram(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                             const uint16_t *px, int calls)
{
    (void)calls;
    trace(T_GRAM);
    ++s_grams;
    if ( 0U < s_gram_fail )
    {
        --s_gram_fail;
        return KODE_TIMEOUT;
    }
    s_ready_at_gram = lp_queued();
    s_gram_x = x;
    s_gram_y = y;
    s_gram_w = w;
    s_gram_h = h;
    (void)memset(s_gram_px, 0, sizeof(s_gram_px));
    if ( ((unsigned)w * (unsigned)h) <= GRAM_PX_MAX )
    {
        (void)memcpy(s_gram_px, px, (size_t)w * h * sizeof(uint16_t));
    }
    return KODE_OK;
}

static kode_status_t on_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                              const void *px, bool last, int calls)
{
    (void)x1; (void)y1; (void)x2; (void)y2; (void)px; (void)last; (void)calls;
    ++s_flush_sent;
    return KODE_OK;
}

static void on_flush_abort(int calls)
{
    (void)calls;
    ++s_flush_aborted;
}

/** @brief Arrange the rest's stand-ins; call after display_up_once(). */
static void rest_mocks(void)
{
    s_trace_n       = 0U;
    s_link_down_rc  = KODE_OK;
    s_link_up_rc    = KODE_OK;
    s_sleep_in_rc   = KODE_OK;
    s_reset_low_rc  = KODE_OK;
    s_posts_at_reset = 0xFFU;
    s_ready_at_link = 0xFFU;
    s_ready_at_gram = 0xFFU;
    s_flush_sent    = 0U;
    s_flush_aborted = 0U;
    s_grams         = 0U;
    s_gram_fail     = 0U;
    kdrv_display_link_set_StubWithCallback(&on_link);
    kdrv_display_panel_sleep_StubWithCallback(&on_panel_sleep);
    kdrv_display_reset_low_StubWithCallback(&on_reset_low);
    kdrv_display_gram_write_StubWithCallback(&on_gram);
    kdrv_display_flush_StubWithCallback(&on_flush);
    kdrv_display_flush_abort_StubWithCallback(&on_flush_abort);
    ao_spy_subscribe(DISPLAY_REPAINT_SIG);
    ao_spy_drain();
}

/* ---- The rest: rounds, answers and the glyph strip ------------------------- */

static uint32_t s_round_no;

/** @brief AO_LowPower opens a round for @p target. */
static void round_for(kode_lp_state_t target)
{
    LowPowerRoundEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(LOWPOWER_ROUND_SIG);
    e.target = (uint8_t)target;
    e.round  = ++s_round_no;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/** @brief AO_LowPower says the board is AWAKE (again), leaving @p prev. */
static void state_awake(kode_lp_state_t prev)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_AWAKE;
    e.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    e.prev  = (uint8_t)prev;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/** @brief AO_LowPower says the board is ASLEEP. */
static void state_asleep(void)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_ASLEEP;
    e.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    e.prev  = (uint8_t)KODE_LP_AWAKE;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/** @brief The minute is over: AO_LowPower asks for the next one. */
static void minute_asked(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_MINUTE_REQ_SIG);

    QASM_DISPATCH(AO_Display, &req, 0U);
}

/** @brief The next answer must be for the open round, with this verdict. */
static void expect_answer(bool busy, const char *why)
{
    LowPowerReadyEvt a;

    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_ready_take(&a), "the round was not answered");
    TEST_ASSERT_EQUAL_UINT32(s_round_no, a.round);
    TEST_ASSERT_EQUAL_INT(busy, a.busy);
    if ( NULL != why )
    {
        TEST_ASSERT_EQUAL_STRING(why, a.why);
    }
}

/** @brief Exactly once per round: nothing more is waiting. */
static void expect_no_more_answers(void)
{
    LowPowerReadyEvt a;

    TEST_ASSERT_FALSE_MESSAGE(fake_lowpower_ready_take(&a), "a second answer for one round");
}

/** @brief The trace is exactly @p n entries, @p want. */
static void expect_trace_n(const trace_t *want, unsigned n)
{
    TEST_ASSERT_EQUAL_UINT(n, s_trace_n);
    for ( unsigned i = 0U; i < n; ++i )
    {
        TEST_ASSERT_EQUAL_INT((int)want[i], s_trace[i]);
    }
}

/** @brief The trace is exactly these calls, in this order. */
#define EXPECT_TRACE(...)                                                   \
    do {                                                                    \
        const trace_t want_[] = { __VA_ARGS__ };                            \
        expect_trace_n(want_, (unsigned)(sizeof(want_) / sizeof(want_[0]))); \
    } while (false)

/* The strip: ten digits of GLYPH_W, then the colon of COLON_W, GLYPH_H rows.
 * Every pixel says where it came from, so a composed rectangle can be read
 * back cell by cell. */
#define GLYPH_W    4U
#define GLYPH_H    4U
#define COLON_W    2U
#define STRIP_W    ((10U * GLYPH_W) + COLON_W)
#define GLYPH_X    10U
#define GLYPH_Y    20U
#define REST_PCT   15U

static uint16_t s_strip[STRIP_W * GLYPH_H];

/** @brief The pixel the strip holds for @p cell (10 is the colon), row, column. */
static uint16_t glyph_px(unsigned cell, unsigned row, unsigned col)
{
    return (uint16_t)(((cell + 1U) << 8) | (row << 4) | col);
}

static unsigned cell_w(unsigned cell)
{
    unsigned w = GLYPH_W;

    if ( 10U == cell )
    {
        w = COLON_W;
    }
    return w;
}

static void strip_fill(void)
{
    for ( unsigned row = 0U; GLYPH_H > row; ++row )
    {
        unsigned at = row * STRIP_W;

        for ( unsigned cell = 0U; 10U >= cell; ++cell )
        {
            for ( unsigned col = 0U; cell_w(cell) > col; ++col )
            {
                s_strip[at] = glyph_px(cell, row, col);
                ++at;
            }
        }
    }
}

/** @brief The cell number that stands for a blank cell in expect_cells(). */
#define CELL_BLANK  11U

/** @brief The renderer hands the glyphs over, as kgui does before it answers.
 *         @p adv is its spacing, NULL for fixed cells. */
static void hand_spaced(const uint16_t *strip, uint16_t x, bool h12, const uint8_t *adv)
{
    DisplayGlyphsEvt g;

    (void)memset(&g, 0, sizeof(g));
    g.strip    = strip;
    g.glyph_w  = (uint16_t)GLYPH_W;
    g.glyph_h  = (uint16_t)GLYPH_H;
    g.colon_w  = (uint16_t)COLON_W;
    g.x        = x;
    g.y        = (uint16_t)GLYPH_Y;
    g.rest_pct = (uint8_t)REST_PCT;
    g.h12      = h12;
    if ( NULL != adv )
    {
        (void)memcpy(g.adv, adv, sizeof(g.adv));
    }
    display_glyphs_hand(&g);
    (void)qf_support_settle(AO_Display);
}

/** @brief The hand-over with fixed cells. */
static void hand_dial(const uint16_t *strip, uint16_t x, bool h12)
{
    hand_spaced(strip, x, h12, NULL);
}

/** @brief The renderer hands the glyphs over for a 24 h clock. */
static void hand(const uint16_t *strip, uint16_t x)
{
    hand_dial(strip, x, false);
}

/** @brief The last written rectangle is these five cells of the strip; a
 *         CELL_BLANK stands for the page's black. */
static void expect_cells(const unsigned cells[5])
{
    TEST_ASSERT_EQUAL_UINT16(GLYPH_X, s_gram_x);
    TEST_ASSERT_EQUAL_UINT16(GLYPH_Y, s_gram_y);
    TEST_ASSERT_EQUAL_UINT16((4U * GLYPH_W) + COLON_W, s_gram_w);
    TEST_ASSERT_EQUAL_UINT16(GLYPH_H, s_gram_h);
    for ( unsigned row = 0U; GLYPH_H > row; ++row )
    {
        unsigned at = row * s_gram_w;

        for ( unsigned k = 0U; 5U > k; ++k )
        {
            unsigned w = GLYPH_W;

            if ( CELL_BLANK != cells[k] )
            {
                w = cell_w(cells[k]);
            }
            for ( unsigned col = 0U; col < w; ++col )
            {
                uint16_t want = 0U;

                if ( CELL_BLANK != cells[k] )
                {
                    want = glyph_px(cells[k], row, col);
                }
                TEST_ASSERT_EQUAL_HEX16(want, s_gram_px[at]);
                ++at;
            }
        }
    }
}

/** @brief The last written rectangle is the time @p hh:@p mm, from the strip. */
static void expect_time(unsigned hh, unsigned mm)
{
    const unsigned cells[5] = { hh / 10U, hh % 10U, 10U, mm / 10U, mm % 10U };

    expect_cells(cells);
}

/** @brief 2026-10-28 09:41:00 UTC, in seconds. */
#define EPOCH_0941   1793180460ULL
#define US_PER_S     1000000ULL
#define S_PER_MIN    60ULL
#define S_PER_HOUR   3600ULL

/** @brief A flush reaches AO_Display: the proof that a renderer exists. */
static void renderer_flushes(void)
{
    FlushEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(DISPLAY_FLUSH_SIG);
    e.x2    = 10;
    e.y2    = 10;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/* ---- Bring-up -------------------------------------------------------------- */

/* The panel is this AO's; its supply pin is the expander's owner's, and is
 * asked for by event - display_up_once() walks that handshake and asserts on
 * it, so this case is what sees it when it runs first. */
TEST_CASE("AO_Display owns the panel, and asks the expander's owner for its supply",
          "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    TEST_ASSERT_EQUAL_STRING("AO_Display", kbsp_owner_name(KBSP_RES_PANEL));
    const char * const who = kbsp_owner_name(KBSP_RES_IOEXP);
    TEST_ASSERT_TRUE_MESSAGE((NULL == who) || (strcmp(who, "AO_Display") != 0),
                             "the expander is not this AO's to own");

    Mockkdrv_display_Destroy();
}

/* A renderer that hears nothing cannot tell a panel that failed from one that
 * has not answered yet, and would wait for the second forever. So the state is
 * announced either way - and answered again to whoever asks late, which is
 * what lets AO_Gui start after kode_init() returns. */
TEST_CASE("the panel's state is answered to whoever asks late", "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    const DisplayStateEvt * const e = announced();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "an AO starting late would wait forever");
    TEST_ASSERT_TRUE(e->up);

    Mockkdrv_display_Destroy();
}

/* ---- The floor, and the one case it must not apply to ---------------------- */

/* A chosen brightness has a floor. Without it a slider dragged to the bottom
 * blacks the glass, and the control for getting it back is on the glass. */
TEST_CASE("a chosen brightness cannot black the glass", "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(0U, true);

    TEST_ASSERT_EQUAL_UINT(1U, s_drives);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KSET_BRIGHT_MIN, s_driven,
        "the slider just turned the screen off with the slider on it");

    Mockkdrv_display_Destroy();
}

/* THE other one. The floor exists so that a black can never be stored and come
 * back at every boot with the control for undoing it on the glass. A transient
 * value is never stored, so it passes through as asked: a caller that asks for
 * zero without persisting it gets the black it asked for. */
TEST_CASE("a deliberate blank is not floored", "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(0U, false);

    TEST_ASSERT_EQUAL_UINT(1U, s_drives);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, s_driven,
        "a transient zero was clamped: the floor is for what is stored");

    Mockkdrv_display_Destroy();
}

/* The top end too: a caller that asks for more than the scale has must not
 * drive the panel past it. */
TEST_CASE("a chosen brightness is capped at full", "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(250U, true);

    TEST_ASSERT_EQUAL_UINT8(100U, s_driven);

    Mockkdrv_display_Destroy();
}

/* An ordinary value passes through untouched, or the clamp would be a bug of
 * its own. */
TEST_CASE("an ordinary brightness reaches the glass unchanged", "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(42U, true);
    TEST_ASSERT_EQUAL_UINT8(42U, s_driven);

    ask_brightness(42U, false);
    TEST_ASSERT_EQUAL_UINT8(42U, s_driven);

    Mockkdrv_display_Destroy();
}

/* ---- The announcement, and what a slider is seeded from -------------------- */

/* The clamp runs BEFORE the state event is updated, so the number a late
 * subscriber is told is the number the panel is actually driven at. A settings
 * slider reads this to know where to put its handle; a stale field seeds it
 * with a lie the user then drags away from. */
TEST_CASE("what is announced is what is driven, after the clamp",
          "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(1U, true);          /* below the floor */

    const DisplayStateEvt * const e = announced();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(s_driven, e->bright,
        "the slider would come up somewhere the glass is not");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSET_BRIGHT_MIN, e->bright);

    Mockkdrv_display_Destroy();
}

/* A transient blank is reported honestly as the zero it really is. Nothing
 * asks for the state during one, and inventing a floor here would mean the
 * announcement and the glass disagreed for as long as the blank lasted. */
TEST_CASE("a transient blank is announced as the zero it is", "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(0U, false);

    const DisplayStateEvt * const e = announced();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_UINT8(0U, e->bright);

    /* And putting it back restores both. */
    ask_brightness(70U, false);
    const DisplayStateEvt * const back = announced();
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_UINT8(70U, back->bright);

    Mockkdrv_display_Destroy();
}

/* ---- The store ------------------------------------------------------------- */

/* One write per gesture. The slider sends its whole travel with persist=false
 * and only the release lands here; a write per event would be dozens of flash
 * writes for one drag of a finger. */
TEST_CASE("only a chosen brightness is written down", "[display_ao]")
{
    uint8_t stored = 0U;

    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(64U, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, &stored));
    TEST_ASSERT_EQUAL_UINT8(64U, stored);

    /* The travel of the next gesture: driven, never stored. */
    ask_brightness(11U, false);
    TEST_ASSERT_EQUAL_UINT8(11U, s_driven);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, &stored));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(64U, stored,
        "every step of a slider drag reached the flash");

    Mockkdrv_display_Destroy();
}

/* What reaches the glass and what lands in the store are always the same
 * number, which is why the clamp is applied here rather than trusted from
 * every caller. A store holding 1 would come back as 1 on the next boot. */
TEST_CASE("the store holds the clamped value, not the asked one",
          "[display_ao]")
{
    uint8_t stored = 0U;

    Mockkdrv_display_Init();
    display_up_once(0U);

    ask_brightness(0U, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, &stored));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSET_BRIGHT_MIN, stored);
    TEST_ASSERT_EQUAL_UINT8(stored, s_driven);

    Mockkdrv_display_Destroy();
}

/* ---- config key="bright" ---------------------------------------------------- */

/** @brief A brightness a case stores or asks for, off both ends of the scale. */
#define CONFIG_BRIGHT_PROBE_PCT  55U

/** @brief Leave the store and the spy as the next case expects them. */
static void config_bright_end(void)
{
    fake_nvs_reset();
    ao_spy_drain();
    Mockkdrv_display_Destroy();
}

/* `config` is how a host learns the brightness without a screen: the answer
 * must come from the store, and say whether it did, or a board nobody has ever
 * adjusted would claim a choice it never made. */
TEST_CASE("config bright reads the stored brightness, or the default with "
          "nothing stored", "[display_ao]")
{
    const KodeConfigAnsEvt *a;
    char                    want[8];

    Mockkdrv_display_Init();
    display_up_once(0U);
    fake_nvs_reset();

    a = config_ask(AO_Display, &(config_ask_t){ .key = KSET_KEY_BRIGHT });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    (void)snprintf(want, sizeof(want), "%u", (unsigned)KSET_BRIGHT_DEF);
    TEST_ASSERT_EQUAL_STRING(want, a->value);
    TEST_ASSERT_FALSE(a->stored);

    (void)khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, CONFIG_BRIGHT_PROBE_PCT);
    a = config_ask(AO_Display, &(config_ask_t){ .key = KSET_KEY_BRIGHT });
    TEST_ASSERT_NOT_NULL(a);
    (void)snprintf(want, sizeof(want), "%u", (unsigned)CONFIG_BRIGHT_PROBE_PCT);
    TEST_ASSERT_EQUAL_STRING(want, a->value);
    TEST_ASSERT_TRUE(a->stored);

    config_bright_end();
}

/* A set is a chosen brightness: it must reach the glass, the store and the
 * announcement as one number, exactly as the slider's release does. */
TEST_CASE("config bright set drives the glass, stores it and answers it",
          "[display_ao]")
{
    const KodeConfigAnsEvt *a;
    uint8_t                 stored = 0U;
    char                    want[8];

    Mockkdrv_display_Init();
    display_up_once(0U);
    fake_nvs_reset();
    s_driven = 0xFFU;

    a = config_ask(AO_Display, &(config_ask_t){ .key = KSET_KEY_BRIGHT, .set = true,
                                                .i = (int32_t)CONFIG_BRIGHT_PROBE_PCT });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    (void)snprintf(want, sizeof(want), "%u", (unsigned)CONFIG_BRIGHT_PROBE_PCT);
    TEST_ASSERT_EQUAL_STRING(want, a->value);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_UINT8(CONFIG_BRIGHT_PROBE_PCT, s_driven);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, &stored));
    TEST_ASSERT_EQUAL_UINT8(CONFIG_BRIGHT_PROBE_PCT, stored);
    TEST_ASSERT_EQUAL_UINT8(CONFIG_BRIGHT_PROBE_PCT, announced()->bright);

    config_bright_end();
}

/* The console keeps a value off the scale from ever reaching the owner; one
 * that arrives anyway is held to the scale and the answer says what the glass
 * really got, so the host never reads back a number the panel is not at. */
TEST_CASE("config bright set past either end of the scale answers the clamped "
          "value", "[display_ao]")
{
    const KodeConfigAnsEvt *a;
    char                    want[8];

    Mockkdrv_display_Init();
    display_up_once(0U);
    fake_nvs_reset();

    a = config_ask(AO_Display, &(config_ask_t){ .key = KSET_KEY_BRIGHT, .set = true,
                                                .i = 0 });
    TEST_ASSERT_NOT_NULL(a);
    (void)snprintf(want, sizeof(want), "%u", (unsigned)KSET_BRIGHT_MIN);
    TEST_ASSERT_EQUAL_STRING(want, a->value);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSET_BRIGHT_MIN, s_driven);

    a = config_ask(AO_Display, &(config_ask_t){ .key = KSET_KEY_BRIGHT, .set = true,
                                                .i = (int32_t)(KSET_PCT_MAX + 1U) });
    TEST_ASSERT_NOT_NULL(a);
    (void)snprintf(want, sizeof(want), "%u", (unsigned)KSET_PCT_MAX);
    TEST_ASSERT_EQUAL_STRING(want, a->value);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSET_PCT_MAX, s_driven);

    config_bright_end();
}

/* One key, one owner: AO_Display must stay silent for another AO's setting. */
TEST_CASE("another owner's key is not AO_Display's to answer", "[display_ao]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);

    TEST_ASSERT_NULL(config_ask(AO_Display, &(config_ask_t){ .key = KSET_KEY_VOL }));

    config_bright_end();
}

/* ---- The rest: a board nobody has drawn on ---------------------------------- */

/* These come before every flush in this file on purpose: AO_Display counts a
 * renderer from its first flush and never forgets it, so the cases below see
 * the glass as a headless firmware does. The cases after the flushing section
 * see it as kodeOS does. */

/* A round for ASLEEP: the glass is dimmed to the level the interface gave, or
 * to the default when it gave none, and only then does the link go; the
 * answer comes last. */
TEST_CASE("a round for ASLEEP with no renderer dims the glass, drops the link, "
          "then answers", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();

    round_for(KODE_LP_ASLEEP);

    EXPECT_TRACE(T_BRIGHT, T_LINK_DOWN);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_LOWPOWER_DIM_PCT, s_driven,
        "no glyphs were handed, so the default dim");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_ready_at_link,
        "the answer was given before the glass had rested");
    expect_answer(false, NULL);
    expect_no_more_answers();

    state_awake(KODE_LP_ASLEEP);
    Mockkdrv_display_Destroy();
}

/* The way back: link up, the user's brightness told, and a repaint asked so
 * the renderer takes the new framebuffers. Twice is the same as once: an
 * abandoned round publishes AWAKE again. */
TEST_CASE("AWAKE restores the glass once, however often it is said",
          "[display_ao][lowpower]")
{
    uint8_t user;

    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    user = announced()->bright;
    ao_spy_drain();

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    s_trace_n = 0U;

    state_awake(KODE_LP_ASLEEP);
    EXPECT_TRACE(T_LINK_UP, T_BRIGHT);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(user, s_driven, "the user's brightness is back");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "one repaint is asked");
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    ao_spy_drain();
    state_awake(KODE_LP_AWAKE);
    EXPECT_TRACE(T_LINK_UP, T_BRIGHT);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the second AWAKE asks for nothing");

    Mockkdrv_display_Destroy();
}

/* An AWAKE that cannot put the glass back is the last one this board hears
 * while it stays awake, so the glass is asked again on a short timer until the
 * link answers; then the repaint goes out once and the timer is done. */
TEST_CASE("a restore the link refused is tried again until it answers",
          "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    ao_spy_drain();

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);

    s_link_up_rc = KODE_ERROR;
    s_trace_n = 0U;
    state_awake(KODE_LP_ASLEEP);
    EXPECT_TRACE(T_LINK_UP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a repaint was asked of a dark glass");

    s_trace_n = 0U;
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(KODE_DISPLAY_RESTORE_RETRY_MS) + 1U);
    EXPECT_TRACE(T_LINK_UP);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    s_link_up_rc = KODE_OK;
    s_trace_n = 0U;
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(KODE_DISPLAY_RESTORE_RETRY_MS) + 1U);
    EXPECT_TRACE(T_LINK_UP, T_BRIGHT);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "one repaint is asked");
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    ao_spy_drain();
    s_trace_n = 0U;
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(KODE_DISPLAY_RESTORE_RETRY_MS) * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_trace_n, "the retry went on after the glass came back");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    Mockkdrv_display_Destroy();
}

/* A round that opens while the glass is still being put back ends the retry:
 * the AWAKE that closes that round restores it. */
TEST_CASE("a new round ends the restore retry", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    s_link_up_rc = KODE_ERROR;
    state_awake(KODE_LP_ASLEEP);

    round_for(KODE_LP_ASLEEP);
    s_link_up_rc = KODE_OK;
    s_trace_n = 0U;
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(KODE_DISPLAY_RESTORE_RETRY_MS) * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_trace_n, "the retry ran into a round");

    state_awake(KODE_LP_ASLEEP);
    Mockkdrv_display_Destroy();
}

/* OFF: the panel in sleep-in (which darkens it itself) and the link down. */
TEST_CASE("a round for OFF sleeps the panel and drops the link",
          "[display_ao][lowpower]")
{
    uint8_t user;

    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    user = announced()->bright;
    ao_spy_drain();

    round_for(KODE_LP_OFF);
    EXPECT_TRACE(T_SLEEP_IN, T_LINK_DOWN);
    expect_answer(false, NULL);
    expect_no_more_answers();

    /* An abandoned round: the glass is woken, lit and repainted. */
    s_trace_n = 0U;
    state_awake(KODE_LP_AWAKE);
    EXPECT_TRACE(T_LINK_UP, T_SLEEP_OUT, T_BRIGHT);
    TEST_ASSERT_EQUAL_UINT8(user, s_driven);
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    Mockkdrv_display_Destroy();
}

/** @brief AO_LowPower says the board is going OFF: the grace, then deep sleep. */
static void state_off(void)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_OFF;
    e.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    e.prev  = (uint8_t)KODE_LP_AWAKE;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/* OFF left the panel asleep with its supply on, an estimated 0.1-1 mA for as
 * long as the board was off. Once OFF is certain, the shared reset line goes
 * low and only then is P7 asked low: the panel's power-off order. */
TEST_CASE("STATE(OFF) after the sleep-in drives the reset line low, then asks for the supply off",
          "[display_ao][lowpower]")
{
    unsigned asked;

    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();

    round_for(KODE_LP_OFF);
    EXPECT_TRACE(T_SLEEP_IN, T_LINK_DOWN);
    expect_answer(false, NULL);
    asked = fake_neighbour_ioexp_posts();

    state_off();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(asked + 1U, fake_neighbour_ioexp_posts(),
                                   "P7 is asked low once OFF is certain");
    /* The glass is dark and asleep already: the reset line is all that moves. */
    EXPECT_TRACE(T_SLEEP_IN, T_LINK_DOWN, T_RESET_LOW);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(asked, s_posts_at_reset,
                                   "the reset line goes low before P7 is asked");

    /* The board never comes back from here; the next case needs a glass. */
    state_awake(KODE_LP_AWAKE);
    ao_spy_drain();
    Mockkdrv_display_Destroy();
}

/* A reset line that cannot be driven (the driver refuses it) leaves the order
 * as it was before: the supply still goes. */
TEST_CASE("STATE(OFF) asks for the supply off even when the reset line is refused",
          "[display_ao][lowpower]")
{
    unsigned asked;

    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    s_reset_low_rc = KODE_DENIED;

    round_for(KODE_LP_OFF);
    expect_answer(false, NULL);
    asked = fake_neighbour_ioexp_posts();

    state_off();
    EXPECT_TRACE(T_SLEEP_IN, T_LINK_DOWN, T_RESET_LOW);
    TEST_ASSERT_EQUAL_UINT(asked + 1U, fake_neighbour_ioexp_posts());

    state_awake(KODE_LP_AWAKE);
    ao_spy_drain();
    Mockkdrv_display_Destroy();
}

/* A panel that did not sleep is still lit: losing its supply then is a flash
 * on the glass, so the AO leaves the supply alone, and the shared reset line
 * with it (the hold drives that line low at the deep sleep, not this AO). The
 * round for ASLEEP never lets go of either. */
TEST_CASE("the panel keeps its supply when it did not sleep, and for ASLEEP",
          "[display_ao][lowpower]")
{
    unsigned asked;

    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();

    /* ASLEEP, the glass at rest: no reset line, no supply. */
    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    asked = fake_neighbour_ioexp_posts();
    s_trace_n = 0U;
    state_asleep();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_trace_n, "ASLEEP touched the reset line");
    TEST_ASSERT_EQUAL_UINT(asked, fake_neighbour_ioexp_posts());
    state_awake(KODE_LP_ASLEEP);
    ao_spy_drain();

    /* A link that will not go down: not at the OFF rest, so P7 stays. */
    s_link_down_rc = KODE_ERROR;
    round_for(KODE_LP_ASLEEP);
    expect_answer(true, "display link");
    asked = fake_neighbour_ioexp_posts();
    s_trace_n = 0U;
    state_off();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_trace_n, "the reset line moved under a lit glass");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(asked, fake_neighbour_ioexp_posts(),
                                   "a glass not at the OFF rest keeps P7");
    s_link_down_rc = KODE_OK;
    state_awake(KODE_LP_AWAKE);
    ao_spy_drain();

    /* A sleep-in that failed: the panel is lit, so OFF leaves both alone. */
    s_sleep_in_rc = KODE_TIMEOUT;
    round_for(KODE_LP_OFF);
    expect_answer(true, "display panel");
    asked = fake_neighbour_ioexp_posts();
    s_trace_n = 0U;
    state_off();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_trace_n, "the reset line moved under a lit panel");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(asked, fake_neighbour_ioexp_posts(),
                                   "a panel that did not sleep keeps P7");

    s_sleep_in_rc = KODE_OK;
    state_awake(KODE_LP_AWAKE);
    ao_spy_drain();
    Mockkdrv_display_Destroy();
}

/* ASLEEP to OFF without lighting: the commands ride the link, so it comes up
 * for the sleep-in and goes down again. */
TEST_CASE("OFF from the ASLEEP rest brings the link up for the sleep-in",
          "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    s_trace_n = 0U;

    round_for(KODE_LP_OFF);
    EXPECT_TRACE(T_LINK_UP, T_SLEEP_IN, T_LINK_DOWN);
    expect_answer(false, NULL);
    expect_no_more_answers();

    s_trace_n = 0U;
    state_awake(KODE_LP_OFF);
    EXPECT_TRACE(T_LINK_UP, T_SLEEP_OUT, T_BRIGHT);

    Mockkdrv_display_Destroy();
}

/* A link that will not go down is a glass that did not rest: the round is
 * refused with the reason. The driver leaves the link down when it answers an
 * error (framebuffers freed), so AWAKE asks for it up whatever this AO saw:
 * link_set(true) is a no-op where the link is up. */
TEST_CASE("a link that will not go down refuses the round", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    s_link_down_rc = KODE_ERROR;

    round_for(KODE_LP_ASLEEP);
    expect_answer(true, "display link");
    expect_no_more_answers();

    s_trace_n = 0U;
    state_awake(KODE_LP_AWAKE);
    EXPECT_TRACE(T_LINK_UP, T_BRIGHT);
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    Mockkdrv_display_Destroy();
}

/* The same on the way to OFF: the panel is already in sleep-in when the link
 * answers an error, and the wake must bring the link up first or the panel
 * could not be woken at all. */
TEST_CASE("a link error on the way to OFF still wakes the link and the panel",
          "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    s_link_down_rc = KODE_ERROR;

    round_for(KODE_LP_OFF);
    EXPECT_TRACE(T_SLEEP_IN, T_LINK_DOWN);
    expect_answer(true, "display link");
    expect_no_more_answers();

    s_trace_n = 0U;
    state_awake(KODE_LP_AWAKE);
    EXPECT_TRACE(T_LINK_UP, T_SLEEP_OUT, T_BRIGHT);
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    /* And the glass is whole again: a repeat does nothing. */
    s_trace_n = 0U;
    state_awake(KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(0U, s_trace_n);

    Mockkdrv_display_Destroy();
}

/* The minute is written only into a glass that is resting, and only from
 * glyphs: without them the panel's memory is left as it is. */
TEST_CASE("the minute without glyphs writes nothing", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    fake_time_wall_set(EPOCH_0941 * US_PER_S);

    minute_asked();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_grams, "the glass is awake");

    round_for(KODE_LP_ASLEEP);
    expect_answer(false, NULL);
    minute_asked();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_grams, "no glyphs were handed");

    state_awake(KODE_LP_ASLEEP);
    Mockkdrv_display_Destroy();
}

/* ---- Flushing -------------------------------------------------------------- */

/* A flush that never started queues no DMA, so no completion interrupt is
 * coming - and the renderer is blocked on exactly that completion. Completing
 * it by hand is the difference between one bad rectangle and a GUI that is
 * frozen for the rest of the boot.
 *
 * Asserted through kdrv_display_flush_abort() and not by counting semaphore
 * gives: the semaphore is the driver's own now, and "who may complete a
 * flush" is the whole point of routing this through one door. Exactly once,
 * because a second completion is what a binary semaphore swallows without a
 * word and what leaves the wait after it with nothing to take. */
TEST_CASE("a flush that failed completes it exactly once", "[display_ao]")
{
    FlushEvt e;

    Mockkdrv_display_Init();
    display_up_once(0U);

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(DISPLAY_FLUSH_SIG);
    e.x2    = 10;
    e.y2    = 10;

    kdrv_display_flush_ExpectAnyArgsAndReturn(KODE_ERROR);
    kdrv_display_flush_abort_Expect();
    QASM_DISPATCH(AO_Display, &e.super, 0U);

    Mockkdrv_display_Verify();
    Mockkdrv_display_Destroy();
}

/* A flush that started must NOT be completed here: the DSI interrupt will do
 * it when the transfer finishes, and completing it twice lets the renderer
 * run ahead of the panel and hand it a buffer it is still reading.
 *
 * No expectation for the abort is the assertion: CMock fails any call that
 * was not asked for, so "it must not happen" needs nothing written down. */
TEST_CASE("a flush that started is left to the interrupt", "[display_ao]")
{
    FlushEvt e;

    Mockkdrv_display_Init();
    display_up_once(0U);

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(DISPLAY_FLUSH_SIG);
    e.x2    = 10;
    e.y2    = 10;

    kdrv_display_flush_ExpectAnyArgsAndReturn(KODE_OK);
    QASM_DISPATCH(AO_Display, &e.super, 0U);

    Mockkdrv_display_Verify();
    Mockkdrv_display_Destroy();
}

/* ---- The rest: a board with a renderer on it --------------------------------- */

/* From the first flush on, AO_Display assumes someone is drawing. The link
 * takes the framebuffers with it, so it stays up, and the glass stays lit,
 * until the renderer hands over its glyphs - its word that it has stopped.
 * Until then the renderer is drawing the page the glass rests on, and its
 * flushes are sent; after the glyphs a flush is completed without being
 * sent. */
TEST_CASE("a renderer is parked before the link goes: its last page is flipped, "
          "the glyphs release the glass", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();

    renderer_flushes();
    TEST_ASSERT_EQUAL_UINT(1U, s_flush_sent);

    round_for(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_trace_n, "the glass moved before the renderer stopped");
    TEST_ASSERT_FALSE_MESSAGE(lp_queued() > 0U, "answered before the renderer stopped");

    renderer_flushes();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_flush_sent, "the renderer's page was dropped while parking");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_flush_aborted, "a flush was completed unsent while parking");

    hand(s_strip, (uint16_t)GLYPH_X);
    EXPECT_TRACE(T_BRIGHT, T_LINK_DOWN);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)REST_PCT, s_driven, "the interface's own dim level");
    TEST_ASSERT_EQUAL_UINT(0U, s_ready_at_link);
    expect_answer(false, NULL);
    expect_no_more_answers();

    renderer_flushes();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_flush_sent, "a flush reached the resting glass");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_flush_aborted, "a flush after the glyphs was not completed unsent");

    state_awake(KODE_LP_ASLEEP);
    renderer_flushes();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_flush_sent, "flushes flow again once AWAKE");
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    Mockkdrv_display_Destroy();
}

/* A renderer that never hands over refuses the sleep, by name, before the
 * round gives up on everyone - and the glass was never touched. */
TEST_CASE("no glyphs in time: the round is refused and the link stays up",
          "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();

    round_for(KODE_LP_ASLEEP);
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(KODE_LOWPOWER_GLYPHS_WAIT_MS) - 1U);
    TEST_ASSERT_FALSE_MESSAGE(lp_queued() > 0U, "refused before the wait was over");
    (void)qf_support_tick(AO_Display, 2U);

    expect_answer(true, "renderer not parked");
    expect_no_more_answers();
    TEST_ASSERT_EQUAL_UINT(0U, s_trace_n);

    /* Late glyphs belong to a round that is over. */
    hand(s_strip, (uint16_t)GLYPH_X);
    TEST_ASSERT_EQUAL_UINT(0U, s_trace_n);
    expect_no_more_answers();

    state_awake(KODE_LP_AWAKE);
    EXPECT_TRACE(T_LINK_UP, T_BRIGHT);
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    Mockkdrv_display_Destroy();
}

/* A glass that is lent (a screenshot is being read) refuses the round at
 * once, before anything moved. The renderer parks on every round all the same
 * and waits for the repaint, so the AWAKE that follows must send it. */
static uint8_t        s_lent_px[4];

static kode_status_t on_capture_begin(const uint8_t **img, size_t *len, int calls)
{
    (void)calls;
    *img       = s_lent_px;
    *len       = sizeof(s_lent_px);
    return KODE_OK;
}

TEST_CASE("a round refused before the glass moved still ends in a repaint",
          "[display_ao][lowpower]")
{
    DisplayCaptureReqEvt req;
    static const QEvt end = QEVT_INITIALIZER(DISPLAY_CAPTURE_END_SIG);

    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    kdrv_display_capture_begin_StubWithCallback(&on_capture_begin);
    kdrv_display_capture_painted_IgnoreAndReturn(true);
    kdrv_display_capture_end_Ignore();
    (void)memset(&req, 0, sizeof(req));
    req.super = (QEvt)QEVT_INITIALIZER(DISPLAY_CAPTURE_REQ_SIG);
    req.who   = AO_Display;   /* the answer lands on this AO and is not its business */
    QASM_DISPATCH(AO_Display, &req.super, 0U);

    round_for(KODE_LP_ASLEEP);
    expect_answer(true, "glass lent");
    expect_no_more_answers();
    TEST_ASSERT_EQUAL_UINT(0U, s_trace_n);

    /* The renderer parked and handed over all the same; nothing is done. */
    hand(s_strip, (uint16_t)GLYPH_X);
    TEST_ASSERT_EQUAL_UINT(0U, s_trace_n);

    ao_spy_drain();
    state_awake(KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(), "the parked renderer was never told it may draw");
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    /* Said once: an AWAKE that repeats has nothing more to say. */
    ao_spy_drain();
    state_awake(KODE_LP_AWAKE);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    QASM_DISPATCH(AO_Display, &end, 0U);
    Mockkdrv_display_Destroy();
}

/* The wait ends with the hand-over: a timer left running would refuse the
 * next round, or answer this one twice. */
TEST_CASE("glyphs in time end the wait", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);

    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(KODE_LOWPOWER_GLYPHS_WAIT_MS) * 2U);
    expect_no_more_answers();

    state_awake(KODE_LP_ASLEEP);
    Mockkdrv_display_Destroy();
}

/* A NULL strip is a renderer that is parked and has nothing to show: the
 * glass rests dimmed to the default and dark. */
TEST_CASE("a hand-over with no strip parks the renderer and the glass rests dark",
          "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    renderer_flushes();
    fake_time_wall_set(EPOCH_0941 * US_PER_S);

    round_for(KODE_LP_ASLEEP);
    hand(NULL, (uint16_t)GLYPH_X);
    EXPECT_TRACE(T_BRIGHT, T_LINK_DOWN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOWPOWER_DIM_PCT, s_driven);
    expect_answer(false, NULL);

    minute_asked();
    TEST_ASSERT_EQUAL_UINT(0U, s_grams);

    state_awake(KODE_LP_ASLEEP);
    Mockkdrv_display_Destroy();
}

/* The panel takes only even windows; a hand-over it could not write is no
 * hand-over, and the glass is told so once rather than at every minute. */
TEST_CASE("glyphs the panel cannot take are no glyphs", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    fake_time_wall_set(EPOCH_0941 * US_PER_S);

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)(GLYPH_X + 1U));
    EXPECT_TRACE(T_BRIGHT, T_LINK_DOWN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOWPOWER_DIM_PCT, s_driven);
    expect_answer(false, NULL);
    minute_asked();
    TEST_ASSERT_EQUAL_UINT(0U, s_grams);

    state_awake(KODE_LP_ASLEEP);
    Mockkdrv_display_Destroy();
}

/* The minute: four digits and the colon composed into ONE rectangle and
 * written once. The clock is read when the request arrives, not when the
 * glyphs did. */
TEST_CASE("the minute writes hh:mm as one rectangle", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    (void)setenv("TZ", "UTC0", 1);
    tzset();

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_grams, "no clock to write");

    fake_time_wall_set(EPOCH_0941 * US_PER_S);
    minute_asked();
    TEST_ASSERT_EQUAL_UINT(1U, s_grams);
    expect_time(9U, 41U);

    fake_time_wall_set((EPOCH_0941 + S_PER_MIN) * US_PER_S);
    minute_asked();
    TEST_ASSERT_EQUAL_UINT(2U, s_grams);
    expect_time(9U, 42U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(), fake_psram_free_count(),
        "the rectangle's buffer was kept");

    state_awake(KODE_LP_ASLEEP);
    minute_asked();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_grams, "the minute of a glass that is lit");
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/** @brief From a figure to the next in the spaced cases: every digit is a pixel
 *         tighter than its cell, the colon a pixel tighter than its own. */
#define SPACED_DIGIT  (GLYPH_W - 1U)
#define SPACED_COLON  (COLON_W - 1U)

/** @brief The last written rectangle holds, column by column on every row, the
 *         column @p from[i] of the cell @p cell[i], and black where they end. */
static void expect_columns(const unsigned *cell, const unsigned *from, unsigned n)
{
    TEST_ASSERT_EQUAL_UINT16((4U * GLYPH_W) + COLON_W, s_gram_w);
    for ( unsigned row = 0U; GLYPH_H > row; ++row )
    {
        for ( unsigned col = 0U; col < s_gram_w; ++col )
        {
            uint16_t want = 0U;

            if ( (col < n) && (CELL_BLANK != cell[col]) )
            {
                want = glyph_px(cell[col], row, from[col]);
            }
            TEST_ASSERT_EQUAL_HEX16(want, s_gram_px[(row * s_gram_w) + col]);
        }
    }
}

/* The clock of the rest keeps the renderer's spacing: each figure starts where
 * the one before it says, and what a figure does not draw leaves what is under
 * it. Nine's first column is left empty here so the overlap shows. A renderer
 * that hands no spacing keeps the fixed cells: every other case in this file. */
TEST_CASE("the renderer's spacing reaches the glass, and a hole in a figure shows what is under it",
          "[display_ao][lowpower]")
{
    uint8_t adv[11];

    (void)memset(adv, SPACED_DIGIT, sizeof(adv));
    adv[10] = (uint8_t)SPACED_COLON;
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    for ( unsigned row = 0U; GLYPH_H > row; ++row )
    {
        s_strip[(row * STRIP_W) + (9U * GLYPH_W)] = 0U;
    }
    renderer_flushes();
    (void)setenv("TZ", "UTC0", 1);
    tzset();

    round_for(KODE_LP_ASLEEP);
    hand_spaced(s_strip, (uint16_t)GLYPH_X, false, adv);
    expect_answer(false, NULL);

    /* 09:41: 0 at 0, 9 at 3, the colon at 6, 4 at 7, 1 at 10. */
    fake_time_wall_set(EPOCH_0941 * US_PER_S);
    minute_asked();
    {
        static const unsigned cell[14] = { 0U, 0U, 0U, 0U, 9U, 9U, 10U, 4U, 4U, 4U, 1U, 1U, 1U, 1U };
        static const unsigned from[14] = { 0U, 1U, 2U, 3U, 1U, 2U, 0U, 0U, 1U, 2U, 0U, 1U, 2U, 3U };

        expect_columns(cell, from, 14U);
    }

    state_awake(KODE_LP_ASLEEP);
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/* On the 12 h dial a lone figure takes the place of the tens: no gap before it. */
TEST_CASE("with the renderer's spacing a lone hour on the 12 h dial starts at the left",
          "[display_ao][lowpower]")
{
    uint8_t adv[11];

    (void)memset(adv, SPACED_DIGIT, sizeof(adv));
    adv[10] = (uint8_t)SPACED_COLON;
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    (void)setenv("TZ", "UTC0", 1);
    tzset();

    round_for(KODE_LP_ASLEEP);
    hand_spaced(s_strip, (uint16_t)GLYPH_X, true, adv);
    expect_answer(false, NULL);

    /* 09:41 reads 9:41: 9 at 0, the colon at 3, 4 at 4, 1 at 7. */
    fake_time_wall_set(EPOCH_0941 * US_PER_S);
    minute_asked();
    {
        static const unsigned cell[11] = { 9U, 9U, 9U, 10U, 4U, 4U, 4U, 1U, 1U, 1U, 1U };
        static const unsigned from[11] = { 0U, 1U, 2U, 0U, 0U, 1U, 2U, 0U, 1U, 2U, 3U };

        expect_columns(cell, from, 11U);
    }

    state_awake(KODE_LP_ASLEEP);
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/* The 12 h dial: the hour reads 1 to 12 with no AM or PM, and an hour of one
 * figure leaves the tens cell on the page's black, as the face's clock does. */
TEST_CASE("the 12 h dial reads 1 to 12 and blanks a lone figure", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    (void)setenv("TZ", "UTC0", 1);
    tzset();

    round_for(KODE_LP_ASLEEP);
    hand_dial(s_strip, (uint16_t)GLYPH_X, true);
    expect_answer(false, NULL);

    /* 09:41 reads 9:41. */
    fake_time_wall_set(EPOCH_0941 * US_PER_S);
    minute_asked();
    {
        const unsigned want[5] = { CELL_BLANK, 9U, 10U, 4U, 1U };
        expect_cells(want);
    }

    /* 13:05 reads 1:05. */
    fake_time_wall_set((EPOCH_0941 + (S_PER_HOUR * 3ULL) + (S_PER_MIN * 24ULL)) * US_PER_S);
    minute_asked();
    {
        const unsigned want[5] = { CELL_BLANK, 1U, 10U, 0U, 5U };
        expect_cells(want);
    }

    /* 12:30 reads 12:30: noon is 12, with both figures. */
    fake_time_wall_set((EPOCH_0941 + (S_PER_HOUR * 2ULL) + (S_PER_MIN * 49ULL)) * US_PER_S);
    minute_asked();
    {
        const unsigned want[5] = { 1U, 2U, 10U, 3U, 0U };
        expect_cells(want);
    }

    /* 00:30 reads 12:30 too: midnight is 12, not 0. */
    fake_time_wall_set((EPOCH_0941 - (S_PER_HOUR * 9ULL) - (S_PER_MIN * 11ULL)) * US_PER_S);
    minute_asked();
    {
        const unsigned want[5] = { 1U, 2U, 10U, 3U, 0U };
        expect_cells(want);
    }

    state_awake(KODE_LP_ASLEEP);
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/* A strip handed for the 24 h clock reads an hour as it is, with its zero. */
TEST_CASE("the 24 h clock keeps its leading zero", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    (void)setenv("TZ", "UTC0", 1);
    tzset();

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);

    fake_time_wall_set((EPOCH_0941 - (S_PER_HOUR * 9ULL) - (S_PER_MIN * 11ULL)) * US_PER_S);
    minute_asked();
    expect_time(0U, 30U);

    state_awake(KODE_LP_ASLEEP);
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/* Local time, through the zone the face keeps in TZ. */
TEST_CASE("the time on the glass is local", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    (void)setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);

    fake_time_wall_set(EPOCH_0941 * US_PER_S);
    minute_asked();
    expect_time(10U, 41U);

    state_awake(KODE_LP_ASLEEP);
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/* A minute that did not reach the panel (a task stopped the cache under the
 * write) is written once more a moment later, in the same minute. */
TEST_CASE("a minute that failed is written again after a pause", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    (void)setenv("TZ", "UTC0", 1);
    tzset();

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);

    fake_time_wall_set(EPOCH_0941 * US_PER_S);
    s_grams     = 0U;
    s_gram_fail = 1U;
    uint64_t const t0 = khal_time_us();
    minute_asked();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_grams, "the failed write was not repeated");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64_MESSAGE(
        t0 + ((uint64_t)KODE_LOWPOWER_MINUTE_RETRY_MS * 1000U), khal_time_us(),
        "no pause before the second try");
    expect_time(9U, 41U);

    state_awake(KODE_LP_ASLEEP);
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/* Two tries and no more: the third would only be the next minute's. */
TEST_CASE("a minute that failed twice is left for the next one", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    fake_time_wall_set(EPOCH_0941 * US_PER_S);

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);

    s_grams     = 0U;
    s_gram_fail = 5U;
    minute_asked();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_grams, "more than two tries in a minute");

    s_gram_fail = 0U;
    s_grams     = 0U;
    minute_asked();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_grams, "the next minute writes at once");

    state_awake(KODE_LP_ASLEEP);
    Mockkdrv_display_Destroy();
}

/* The glass would otherwise show whatever was drawn last until the next
 * minute: the time is written as the glass rests, before the answer. */
TEST_CASE("a clock that is set is written as the glass rests", "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();
    (void)setenv("TZ", "UTC0", 1);
    tzset();
    fake_time_wall_set(EPOCH_0941 * US_PER_S);

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    EXPECT_TRACE(T_BRIGHT, T_LINK_DOWN, T_GRAM);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_ready_at_gram, "answered before the time was written");
    expect_time(9U, 41U);
    expect_answer(false, NULL);

    state_awake(KODE_LP_ASLEEP);
    unsetenv("TZ");
    tzset();
    Mockkdrv_display_Destroy();
}

/* A link that would not come up leaves the glass as it is, and the next
 * AWAKE tries again rather than lighting a panel with no link behind it. */
TEST_CASE("a link that will not come up is tried again at the next AWAKE",
          "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);
    s_trace_n = 0U;

    s_link_up_rc = KODE_ERROR;
    state_awake(KODE_LP_ASLEEP);
    EXPECT_TRACE(T_LINK_UP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a repaint onto a glass with no link");

    s_link_up_rc = KODE_OK;
    state_awake(KODE_LP_ASLEEP);
    EXPECT_TRACE(T_LINK_UP, T_LINK_UP, T_BRIGHT);
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    Mockkdrv_display_Destroy();
}

/** @brief AO_LowPower says the radio alone is awake: the glass is not. */
static void state_radio(void)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_RADIO;
    e.cause = (uint8_t)KODE_LP_CAUSE_TIMER;
    e.prev  = (uint8_t)KODE_LP_ASLEEP;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/* A radio wake keeps the glass asleep: no link, no brightness, no repaint
 * asked, and the AWAKE that follows still restores it. */
TEST_CASE("STATE(RADIO) leaves the rested glass alone; AWAKE still restores it",
          "[display_ao][lowpower]")
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    strip_fill();
    renderer_flushes();

    round_for(KODE_LP_ASLEEP);
    hand(s_strip, (uint16_t)GLYPH_X);
    expect_answer(false, NULL);
    s_trace_n = 0U;
    ao_spy_drain();

    state_radio();
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(KODE_DISPLAY_RESTORE_RETRY_MS) * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_trace_n, "the radio wake drove the glass");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "the radio wake asked for a repaint");

    state_awake(KODE_LP_ASLEEP);
    EXPECT_TRACE(T_LINK_UP, T_BRIGHT);
    TEST_ASSERT_EQUAL_INT((int)DISPLAY_REPAINT_SIG, (int)ao_spy_next_sig());

    Mockkdrv_display_Destroy();
}

/* ---- The look at the glass, once a second ---------------------------------- */

static kdrv_display_link_t s_look_link;

static void on_look_link(kdrv_display_link_t *out, int calls)
{
    (void)calls;
    *out = s_look_link;
}

static void look_setup(void)
{
    Mockkdrv_display_Init();
    display_up_once(0U);
    rest_mocks();
    (void)memset(&s_look_link, 0, sizeof(s_look_link));
    kdrv_display_link_StubWithCallback(&on_look_link);
    /* One look by hand: a restart a last case landed from left the timer
     * unarmed, as it would never be on a board, where nothing comes after. And
     * with the video not expected, it forgets what that case left. */
    {
        static const QEvt look = QEVT_INITIALIZER(DISPLAY_HEALTH_SIG);

        QASM_DISPATCH(AO_Display, &look, 0U);
    }
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(2000U));
    kdrv_display_video_expected_IgnoreAndReturn(true);
    s_look_link.frames = 100U;
    (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(1100U));
    fake_klog_reset();
}

static bool fake_klog_saw(const char *what)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( NULL != strstr(fake_klog_line(i), what) )
        {
            return true;
        }
    }
    return false;
}

static void look_seconds(uint32_t n, uint32_t frames_per_s, uint32_t dpi_per_s)
{
    for ( uint32_t i = 0U; n > i; ++i )
    {
        s_look_link.frames += frames_per_s;
        s_look_link.dpi    += dpi_per_s;
        (void)qf_support_tick(AO_Display, QF_SUPPORT_STEPS(1000U));
    }
}

TEST_CASE("a glass that scans and raises no errors is left alone", "[display_ao]")
{
    unsigned const before = fake_system_restarts();

    look_setup();
    look_seconds(20U, 66U, 0U);
    TEST_ASSERT_EQUAL_UINT(before, fake_system_restarts());
    TEST_ASSERT_FALSE(fake_klog_saw("the glass"));
    Mockkdrv_display_Destroy();
}

TEST_CASE("a few stray errors are not a lost glass", "[display_ao]")
{
    unsigned const before = fake_system_restarts();

    look_setup();
    look_seconds(20U, 66U, 2U);
    TEST_ASSERT_EQUAL_UINT(before, fake_system_restarts());
    TEST_ASSERT_FALSE(fake_klog_saw("the glass"));
    Mockkdrv_display_Destroy();
}

/* The two ways a glass is lost, judged with the restarts already spent: a
 * restart lands out of the middle of a dispatch and leaves the AO unusable for
 * the cases after it. The restart itself is seen on the board. */
TEST_CASE("a scan-out that stopped counting is a lost glass, and three restarts end it",
          "[display_ao]")
{
    unsigned const before = fake_system_restarts();

    look_setup();
    kdrv_display_recoveries_StopIgnore();
    kdrv_display_recoveries_IgnoreAndReturn(3U);
    look_seconds(5U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT(before, fake_system_restarts());
    TEST_ASSERT_TRUE(fake_klog_saw("the glass stays lost"));
    Mockkdrv_display_Destroy();
}

TEST_CASE("pixel-path errors on most frames are a lost glass", "[display_ao]")
{
    unsigned const before = fake_system_restarts();

    look_setup();
    kdrv_display_recoveries_StopIgnore();
    kdrv_display_recoveries_IgnoreAndReturn(3U);
    look_seconds(5U, 66U, 60U);
    TEST_ASSERT_EQUAL_UINT(before, fake_system_restarts());
    TEST_ASSERT_TRUE(fake_klog_saw("the glass stays lost"));
    Mockkdrv_display_Destroy();
}
