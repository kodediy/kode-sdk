/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_test_dot_ao.c
 * @brief AO_TestDot's life: dormant from boot, woken by the cable, everything given back after.
 */
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "fake_station.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_pcf8574.h"
#include "kqp.h"
#include "test_dot_support.h"

#if !CONFIG_KODE_TEST_DOT
#error "test_test_dot_ao.c needs CONFIG_KODE_TEST_DOT: main/CMakeLists.txt spells it out"
#endif

#define TD_BRIGHT_BEFORE  40U  /**< The glass's level before the wake, as AO_Display announces it. */
#define TD_FULL           100  /**< The level the wake asks for. */
#define TD_LOOP_FRAMES    20U  /**< Frames the loop sends at each speed. */
#define TD_STEP_MS        50U  /**< QP time a drive round gives the AO's rests when no job waits. */
#define TD_ROUNDS         400U /**< Most drive rounds: well past the camera part's every step. */
#define TD_PHOTO_NONE     99U  /**< A photo number past every one a pass keeps. */
#define TD_C5_FW_MAJOR    2U   /**< The esp-hosted version the C5 answers, and the host's. */
#define TD_C5_FW_MINOR    12U
#define TD_C5_FW_PATCH    9U
#define TD_C5_SDIO_LINES  4U   /**< Data lines the link came up on. */
#define TD_C5_SDIO_KHZ    40000U /**< Its clock. */
#define TD_RSSI_24        (-55) /**< What the 2.4 GHz network reads. */
#define TD_RSSI_5         (-60) /**< What the 5 GHz one reads. */
#define TD_RSSI_BLE       (-70) /**< What the beacon reads. */
#define TD_CH_24          6U    /**< The 2.4 GHz network's channel. */
#define TD_CH_5           36U   /**< The 5 GHz one's. */
#define TD_CARD_MS        120U  /**< What each part of the radio check took. */
#define TD_W24_MS         500U
#define TD_W5_MS          800U
#define TD_BLE_MS         1100U
#define TD_RADIO_MS       2600U
#define TD_SWEEP_MS       36000U /**< What one whole discovery sweep takes on the bench. */
#define TD_TIME_WIDEST    0xFFFFFFFFUL /**< A time at its type's widest. */
#define TD_RSSI_WIDEST    (-128) /**< A reading at its type's widest. */
#define TD_CH_WIDEST      255U   /**< A channel at its type's widest. */
#define TD_CHIP_WIDEST    0xFFFFU /**< A chip id at its type's widest. */

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

/** @brief `test_dot_place` with no time for the marks: one look, then the verdict. */
static void place_once(void)
{
    TestDotReqEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.verb              = (uint8_t)TEST_DOT_VERB_PLACE;
    r.a.place.win_x     = -1;
    r.a.place.win_y     = -1;
    r.a.place.mark_ms   = 0U;
    r.a.place.dx_mm10   = (uint16_t)KODE_TEST_DOT_PLACE_MM10;
    r.a.place.deg10     = (uint16_t)KODE_TEST_DOT_PLACE_DEG10;
    td_verb(&r);
}

/** @brief The level the glass announces, as AO_Display publishes it. */
static void display_says(uint8_t bright)
{
    DisplayStateEvt d;

    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(DISPLAY_STATE_SIG);
    d.up     = true;
    d.bright = bright;
    td_dispatch(&d.super);
}

/** @brief AO_Power's answer about the boost. */
static void boost_says(bool on)
{
    PowerBoostTestEvt b;

    (void)memset(&b, 0, sizeof(b));
    b.super  = (QEvt)QEVT_INITIALIZER(POWER_BOOST_TEST_SIG);
    b.status = (uint8_t)KODE_OK;
    b.boost  = on;
    td_dispatch(&b.super);
}

/** @brief AO_Log's answer about the dock's pins. */
static void dock_says(bool lent, kode_status_t st)
{
    LogDockEvt d;

    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(LOG_DOCK_SIG);
    d.lent   = lent;
    d.status = (uint8_t)st;
    td_dispatch(&d.super);
}

TEST_CASE("test_dormant_at_boot_claims_nothing", "[test_dot_ao]")
{
    bool seen = false;

    td_up();
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
    TEST_ASSERT_EQUAL_UINT(0U, fake_station_probes());
    TEST_ASSERT_EQUAL_UINT(0U, td_jobs());
    (void)td_rail_asked(&seen);
    TEST_ASSERT_FALSE_MESSAGE(seen, "J3's rail is left alone");
    TEST_ASSERT_EQUAL_INT(-1, td_relays_last());

    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec test_dot state=dormant locked=0 run=none"));
    TEST_ASSERT_TRUE(td_said("j3=free"));
    TEST_ASSERT_TRUE(td_said(">ok"));
    (void)td_rail_asked(&seen);
    TEST_ASSERT_FALSE_MESSAGE(seen, "info wakes nothing");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(), fake_psram_free_count(),
                                   "the records' FIFO is given back once it is empty");

    /* An end with nothing to give back answers at once. */
    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    td_take();
    TEST_ASSERT_TRUE(td_said(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, fake_station_closes());
    td_down();
}

TEST_CASE("test_check_refuses_without_camera_and_gives_back", "[test_dot_ao]")
{
    bool seen = false;

    td_up();
    fake_station_probe_rc(KODE_TIMEOUT);
    td_verb_plain((uint8_t)TEST_DOT_VERB_RUN);
    TEST_ASSERT_TRUE_MESSAGE(td_rail_asked(&seen), "the wake asks for J3's rail");
    TEST_ASSERT_TRUE(seen);

    td_rail_answer(true, KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(td_job(), "the probe");
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_probes());
    TEST_ASSERT_TRUE_MESSAGE(td_job(), "the close");
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_closes());
    TEST_ASSERT_FALSE_MESSAGE(td_rail_asked(&seen), "the rail off");
    TEST_ASSERT_TRUE(seen);

    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=4 msg=camera"));
    TEST_ASSERT_FALSE_MESSAGE(td_said("screen"), "nothing on the glass");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, td_relays_last(), "the relays were never touched");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(), fake_psram_free_count(),
                                   "nothing is held: the records' FIFO went back with the refusal");

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_take();
    TEST_ASSERT_TRUE(td_said("state=dormant"));
    TEST_ASSERT_TRUE(td_said("j3=free"));
    td_down();
}

TEST_CASE("test_rail_silence_is_camera", "[test_dot_ao]")
{
    bool seen = false;

    td_up();
    td_verb_plain((uint8_t)TEST_DOT_VERB_RELAYS);
    TEST_ASSERT_TRUE(td_rail_asked(&seen));
    td_wait_ms(KODE_TEST_DOT_RAIL_MS);
    TEST_ASSERT_TRUE_MESSAGE(td_job(), "the close");
    TEST_ASSERT_EQUAL_UINT(0U, fake_station_probes());
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_closes());
    TEST_ASSERT_FALSE(td_rail_asked(&seen));
    TEST_ASSERT_TRUE_MESSAGE(seen, "the rail asked off");
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=4 msg=camera"));
    td_down();
}

TEST_CASE("test_check_refuses_without_station_i2c", "[test_dot_ao]")
{
    bool seen = false;

    td_up();
    td_station_answers(false);
    td_verb_plain((uint8_t)TEST_DOT_VERB_ADC);
    td_rail_answer(true, KODE_OK);
    TEST_ASSERT_TRUE(td_job());   /* the probe */
    TEST_ASSERT_TRUE(td_job());   /* the station: nobody answers */
    TEST_ASSERT_TRUE(td_job());   /* the close */
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_closes());
    TEST_ASSERT_FALSE(td_rail_asked(&seen));
    TEST_ASSERT_TRUE(seen);
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=4 msg=station_i2c"));
    td_down();
}

TEST_CASE("test_place_wakes_and_all_finds_it_awake", "[test_dot_ao]")
{
    bool seen = false;

    td_up();
    place_once();
    td_wake_ok();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_PCF8574_REST, td_relays_last(), "the wake puts the relays at rest");
    TEST_ASSERT_TRUE_MESSAGE(td_job(), "one look");
    td_take();
    TEST_ASSERT_TRUE(td_said("rec place state=searching"));
    TEST_ASSERT_TRUE(td_said("rec place result=misplaced"));
    TEST_ASSERT_TRUE(td_said(">ok"));

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_RUN);
    (void)td_rail_asked(&seen);
    TEST_ASSERT_FALSE_MESSAGE(seen, "awake: no second rail request");
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_probes());
    td_end();
    td_down();
}

TEST_CASE("test_end_gives_everything_back", "[test_dot_ao]")
{
    TestDotReqEvt r;
    bool seen = false;

    td_up();
    place_once();
    td_wake_ok();
    TEST_ASSERT_EQUAL_INT_MESSAGE(TD_FULL, td_brightness(), "the wake asks for full brightness");
    display_says(TD_BRIGHT_BEFORE);   /* the level before, answered first */
    (void)td_job();
    td_take();
    TEST_ASSERT_TRUE(td_said(">ok"));
    TEST_ASSERT_TRUE_MESSAGE(fake_psram_alloc_count() > fake_psram_free_count(), "the camera's memory is taken");

    /* The boost on. */
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_BOOST;
    r.a.on = true;
    td_verb(&r);
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Power, POWER_BOOST_TEST_REQ_SIG));
    boost_says(true);

    /* A loop whose take-back the dock refused: the pins stay lent. */
    (void)memset(&r, 0, sizeof(r));
    r.verb    = (uint8_t)TEST_DOT_VERB_LOOP;
    r.a.count = TD_LOOP_FRAMES;
    td_verb(&r);
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Log, LOG_DOCK_REQ_SIG));
    dock_says(true, KODE_OK);
    TEST_ASSERT_TRUE(td_job());
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_loops());
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Log, LOG_DOCK_REQ_SIG));
    dock_says(true, KODE_ERROR);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec loop ok=1 b9600=20/20"));

    /* The end. */
    td_clear();
    unsigned const writes = td_relays_writes();
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_posted(AO_Power, POWER_BOOST_TEST_REQ_SIG), "the boost off");
    boost_says(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_posted(AO_Log, LOG_DOCK_REQ_SIG), "the dock back");
    dock_says(false, KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(td_job(), "the close");
    TEST_ASSERT_EQUAL_UINT(writes + 1U, td_relays_writes());
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_PCF8574_REST, td_relays_last(), "the relays at rest");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_station_closes(), "the camera closed");
    TEST_ASSERT_FALSE(td_rail_asked(&seen));
    TEST_ASSERT_TRUE_MESSAGE(seen, "the rail off");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)TD_BRIGHT_BEFORE, td_brightness(), "the level before");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(fake_psram_alloc_count(), fake_psram_free_count(), "every buffer freed");
    td_take();
    TEST_ASSERT_TRUE(td_said("led 0,0,0"));
    TEST_ASSERT_TRUE_MESSAGE(td_said("screen 0x7f"), "the glass given back");
    TEST_ASSERT_TRUE(td_said(">ok"));

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_take();
    TEST_ASSERT_TRUE(td_said("state=dormant locked=0 run=none"));
    TEST_ASSERT_TRUE(td_said("j3=free"));
    td_down();
}

TEST_CASE("test_lock_in_session_refuses_all_but_the_end", "[test_dot_ao]")
{
    TestDotReqEvt r;

    td_up();
    place_once();
    td_wake_ok();
    (void)td_job();
    td_take();

    td_clear();
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_LOCK;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE(td_said(">ok"));

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_verb_plain((uint8_t)TEST_DOT_VERB_PHOTOS);
    td_verb_plain((uint8_t)TEST_DOT_VERB_LOCK);
    td_take();
    TEST_ASSERT_EQUAL_UINT(3U, td_said_n(">err code=5 msg=locked"));

    td_clear();
    td_end();
    TEST_ASSERT_TRUE_MESSAGE(td_said(">ok"), "the end still gives everything back");
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_closes());

    /* The next boot reads the lock: nothing answers, the end included. */
    td_up_locked();
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_take();
    TEST_ASSERT_EQUAL_UINT(2U, td_said_n(">err code=5 msg=locked"));
    td_down();
}

TEST_CASE("test_end_drops_a_boost_answer_that_came_after_its_wait", "[test_dot_ao]")
{
    TestDotReqEvt r;
    bool seen = false;

    td_up();
    place_once();
    td_wake_ok();
    (void)td_job();
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_BOOST;
    r.a.on = true;
    td_verb(&r);
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Power, POWER_BOOST_TEST_REQ_SIG));
    boost_says(true);
    td_take();

    /* AO_Power answers late: the end stops waiting and sends the close. */
    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Power, POWER_BOOST_TEST_REQ_SIG));
    td_wait_ms(KODE_TEST_DOT_ANSWER_MS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_jobs(), "the close is out");

    /* The boost's answer now is not the one the end waits for. */
    boost_says(false);
    TEST_ASSERT_FALSE(td_rail_asked(&seen));
    TEST_ASSERT_FALSE_MESSAGE(seen, "the rail stays on under the close");
    TEST_ASSERT_TRUE_MESSAGE(fake_psram_alloc_count() > fake_psram_free_count(), "the camera's memory stays taken");
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said(">ok"), "the end is not over");

    TEST_ASSERT_TRUE_MESSAGE(td_job(), "the close");
    TEST_ASSERT_FALSE(td_rail_asked(&seen));
    TEST_ASSERT_TRUE_MESSAGE(seen, "the rail off, after the close");
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_closes());
    TEST_ASSERT_EQUAL_UINT(fake_psram_alloc_count(), fake_psram_free_count());
    td_take();
    TEST_ASSERT_EQUAL_UINT(1U, td_said_n(">ok"));
    td_down();
}

TEST_CASE("test_a_lost_boost_answer_times_the_verb_out", "[test_dot_ao]")
{
    TestDotReqEvt r;

    td_up();
    place_once();
    td_wake_ok();
    (void)td_job();
    td_clear();
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_BOOST;
    r.a.on = true;
    td_verb(&r);
    TEST_ASSERT_EQUAL_UINT(1U, td_posted(AO_Power, POWER_BOOST_TEST_REQ_SIG));
    td_wait_ms(KODE_TEST_DOT_ANSWER_MS);
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=6 msg=timeout"));

    /* The answer, late: no second reply. The end still turns the boost off. */
    td_clear();
    boost_says(true);
    td_take();
    TEST_ASSERT_FALSE(td_said(">"));
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_posted(AO_Power, POWER_BOOST_TEST_REQ_SIG), "the boost off");
    boost_says(false);
    (void)td_job();
    td_take();
    TEST_ASSERT_TRUE(td_said(">ok"));
    td_down();
}

TEST_CASE("test_a_lost_job_times_out_and_the_end_waits_for_it", "[test_dot_ao]")
{
    bool seen = false;

    td_up();
    place_once();
    td_wake_ok();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_jobs(), "the look is out");

    /* The worker says nothing: the verb is answered, not left busy. */
    td_clear();
    td_wait_ms(KODE_TEST_DOT_JOB_MS);
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=6 msg=timeout"));

    /* The end waits for the job, and past its bound answers anyway. */
    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, td_jobs(), "no close while the look holds the worker");
    td_wait_ms(KODE_TEST_DOT_JOB_MS);
    td_take();
    TEST_ASSERT_TRUE(td_said(">err code=6 msg=timeout"));
    TEST_ASSERT_FALSE(td_rail_asked(&seen));
    TEST_ASSERT_FALSE_MESSAGE(seen, "the rail stays on");
    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_take();
    TEST_ASSERT_TRUE_MESSAGE(td_said(">err code=4 msg=busy"), "busy until the job answers");

    /* The look answers at last: dropped, and the end goes on in the background. */
    td_clear();
    TEST_ASSERT_TRUE(td_job());
    TEST_ASSERT_TRUE_MESSAGE(td_job(), "the close");
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said("rec place result"), "the late look is not reported");
    TEST_ASSERT_FALSE_MESSAGE(td_said(">"), "the end's line was answered already");
    TEST_ASSERT_FALSE(td_rail_asked(&seen));
    TEST_ASSERT_TRUE_MESSAGE(seen, "the rail off");
    TEST_ASSERT_EQUAL_UINT(1U, fake_station_closes());
    TEST_ASSERT_EQUAL_UINT(fake_psram_alloc_count(), fake_psram_free_count());
    td_verb_plain((uint8_t)TEST_DOT_VERB_INFO);
    td_take();
    TEST_ASSERT_TRUE(td_said("state=dormant"));
    td_down();
}

TEST_CASE("test_screen_request_waits_for_the_host_answer", "[test_dot_ao]")
{
    TestDotReqEvt r;
    uint8_t kind = 0U;
    uint8_t seq  = 0U;
    uint8_t old  = 0U;

    td_up();
    td_screens_answer(false);
    (void)memset(&r, 0, sizeof(r));
    r.verb           = (uint8_t)TEST_DOT_VERB_SCREEN;
    r.a.screen.phase = (uint8_t)TEST_DOT_PHASE_RESULT;
    r.a.screen.pass  = false;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE(td_screen_last(&kind, &seq));
    TEST_ASSERT_EQUAL_UINT8(TEST_DOT_SCR_RESULT, kind);
    TEST_ASSERT_FALSE_MESSAGE(td_said(">"), "the verb waits for the host");
    td_wait_ms(KODE_TEST_DOT_SCREEN_MS);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec screen phase=result ok=0"));
    TEST_ASSERT_TRUE(td_said(">ok"));

    /* The same screen again: the first one's answer, late, is not its own. */
    td_clear();
    old             = seq;
    r.a.screen.pass = true;
    td_verb(&r);
    td_take();
    TEST_ASSERT_TRUE(td_screen_last(&kind, &seq));
    TEST_ASSERT_NOT_EQUAL(old, seq);
    test_dot_screen_done(kind, old);
    td_settle();
    td_take();
    TEST_ASSERT_FALSE_MESSAGE(td_said(">"), "the previous request's answer is ignored");
    test_dot_screen_done(kind, seq);
    td_settle();
    td_take();
    TEST_ASSERT_TRUE(td_said("rec screen phase=result ok=1"));
    TEST_ASSERT_TRUE(td_said(">ok"));
    td_down();
}

/* The station shows "connecting" before the place, while the AO is dormant:
 * the end gives the glass back even with nothing else to give. */
TEST_CASE("test_end_while_dormant_gives_the_glass_back", "[test_dot_ao]")
{
    TestDotReqEvt r;
    uint8_t kind = 0U;
    uint8_t seq  = 0U;

    td_up();
    (void)memset(&r, 0, sizeof(r));
    r.verb           = (uint8_t)TEST_DOT_VERB_SCREEN;
    r.a.screen.phase = (uint8_t)TEST_DOT_PHASE_CONNECTING;
    td_verb(&r);
    td_take();
    td_take();
    TEST_ASSERT_TRUE(td_said("screen 0x00"));
    TEST_ASSERT_TRUE(td_said(">ok"));

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    td_take();
    TEST_ASSERT_TRUE(td_screen_last(&kind, &seq));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(TEST_DOT_SCR_END, kind, "the glass given back");
    TEST_ASSERT_TRUE(td_said(">ok"));
    TEST_ASSERT_TRUE_MESSAGE(td_said_at("screen 0x7f") < td_said_at(">ok"), "before the answer");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_station_closes(), "nothing woken, nothing closed");
    td_down();
}

/* The station counts the photos from one, and its retry of one photo
 * (`only=`) expects no total. */
TEST_CASE("test_photos_count_from_one_and_a_retry_says_no_total", "[test_dot_ao]")
{
    TestDotReqEvt r;

    td_up();
    fake_station_frames(true);
    td_jpeg_encodes(true);
    (void)memset(&r, 0, sizeof(r));
    r.verb  = (uint8_t)TEST_DOT_VERB_CAMERA;
    r.a.cam = (uint8_t)TEST_DOT_CAM_FRAMING;
    td_verb(&r);
    td_wake_ok();
    TEST_ASSERT_TRUE_MESSAGE(drive_until(">"), "the framing part answers");

    td_clear();
    td_verb_plain((uint8_t)TEST_DOT_VERB_PHOTOS);
    TEST_ASSERT_TRUE(drive_until(">ok"));
    TEST_ASSERT_TRUE(td_said("rec photos total="));
    TEST_ASSERT_TRUE_MESSAGE(td_said("blob photo_01_"), "the first photo is number one");
    TEST_ASSERT_FALSE(td_said("blob photo_00_"));
    TEST_ASSERT_TRUE(td_said_at("rec photos total=") < td_said_at("blob photo_01_"));

    td_clear();
    (void)memset(&r, 0, sizeof(r));
    r.verb           = (uint8_t)TEST_DOT_VERB_PHOTOS;
    r.a.photos.one   = true;
    r.a.photos.only  = TEST_DOT_PHOTO_FIRST;
    td_verb(&r);
    TEST_ASSERT_TRUE(drive_until(">ok"));
    TEST_ASSERT_FALSE_MESSAGE(td_said("total="), "a retry says no total");
    TEST_ASSERT_TRUE(td_said("blob photo_01_"));
    TEST_ASSERT_TRUE(td_said("rec photos n=1 "));

    td_clear();
    r.a.photos.only = TD_PHOTO_NONE;
    td_verb(&r);
    TEST_ASSERT_TRUE(drive_until(">ok"));
    TEST_ASSERT_FALSE(td_said("total="));
    TEST_ASSERT_FALSE(td_said("blob "));
    TEST_ASSERT_TRUE(td_said("rec photos n=0 "));

    fake_station_frames(false);
    td_end();
    td_down();
}

/** @brief Fill @p rsp with every number at its widest and a timeout: the longest records the check can make. */
static void radio_widest(RadioProbeEvt *rsp)
{
    rsp->status     = (uint8_t)KODE_TIMEOUT;
    rsp->chip       = TD_CHIP_WIDEST;
    rsp->fw[0]      = UINT8_MAX;
    rsp->fw[1]      = UINT8_MAX;
    rsp->fw[2]      = UINT8_MAX;
    rsp->host[0]    = UINT8_MAX;
    rsp->host[1]    = UINT8_MAX;
    rsp->host[2]    = UINT8_MAX;
    rsp->sdio_width = UINT8_MAX;
    rsp->sdio_khz   = TD_TIME_WIDEST;
    rsp->rssi24     = TD_RSSI_WIDEST;
    rsp->ch24       = TD_CH_WIDEST;
    rsp->rssi5      = TD_RSSI_WIDEST;
    rsp->ch5        = TD_CH_WIDEST;
    rsp->rssi_rx    = TD_RSSI_WIDEST;
    rsp->card_ms    = TD_TIME_WIDEST;
    rsp->w24_ms     = TD_TIME_WIDEST;
    rsp->w5_ms      = TD_TIME_WIDEST;
    rsp->ble_ms     = TD_TIME_WIDEST;
    rsp->ms         = TD_TIME_WIDEST;
}

/** @brief Take the radio check out of AO_Radio's mailbox, answer it as AO_Radio does, with @p fw_patch for the C5's; @p widest stretches every number. */
static void radio_answer(uint8_t fw_patch, bool widest)
{
    const QEvt *e = NULL;

    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT(RADIO_PROBE_REQ_SIG, e->sig);
    RadioProbeEvt * const rsp = ((RadioProbeReqEvt const *)e)->rsp;
    rsp->status     = (uint8_t)KODE_OK;
    rsp->card       = true;
    rsp->chip       = RADIO_PROBE_CHIP_C5;
    rsp->fw[0]      = TD_C5_FW_MAJOR;
    rsp->fw[1]      = TD_C5_FW_MINOR;
    rsp->fw[2]      = fw_patch;
    rsp->host[0]    = TD_C5_FW_MAJOR;
    rsp->host[1]    = TD_C5_FW_MINOR;
    rsp->host[2]    = TD_C5_FW_PATCH;
    rsp->kode       = true;
    rsp->sdio_width = TD_C5_SDIO_LINES;
    rsp->sdio_khz   = TD_C5_SDIO_KHZ;
    rsp->seen24     = true;
    rsp->rssi24     = TD_RSSI_24;
    rsp->ch24       = TD_CH_24;
    rsp->seen5      = true;
    rsp->rssi5      = TD_RSSI_5;
    rsp->ch5        = TD_CH_5;
    rsp->ble_adv    = true;
    rsp->ble_rx     = true;
    rsp->rssi_rx    = TD_RSSI_BLE;
    rsp->card_ms    = TD_CARD_MS;
    rsp->w24_ms     = TD_W24_MS;
    rsp->w5_ms      = TD_W5_MS;
    rsp->ble_ms     = TD_BLE_MS;
    rsp->ms         = TD_RADIO_MS;
    if ( widest )
    {
        radio_widest(rsp);
    }
    QACTIVE_POST(AO_TestDot, &rsp->super, NULL);
    QF_gc(e);
    td_settle();
}

/* The card's record and the readings read as the station's `e5ficha` and
 * `e5radio` meant them: the chip by name and number, both versions, the
 * SDIO link, and what each part took. */
TEST_CASE("test_radio_records_carry_the_card_and_the_times", "[test_dot_ao]")
{
    TestDotReqEvt r;

    td_up();
    (void)memset(&r, 0, sizeof(r));
    r.verb = (uint8_t)TEST_DOT_VERB_RADIO;
    td_verb(&r);
    radio_answer(TD_C5_FW_PATCH, false);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec radio_card ok=1 chip=esp32c5 id=23 kode=1 fw=2.12.9 host=2.12.9 mac="));
    TEST_ASSERT_TRUE(td_said(" sdio=4x40000kHz"));
    TEST_ASSERT_TRUE(td_said("rec radio_time card_ms=120 w24_ms=500 w5_ms=800 ble_ms=1100 ms=2600"));
    TEST_ASSERT_TRUE(td_said("rec radio ok=1 seen24=1 rssi24=-55 ch24=6 seen5=1 rssi5=-60 ch5=36"));
    TEST_ASSERT_TRUE(td_said(" ble_adv=1 ble_rx=1 rssi_rx=-70 why=-"));
    TEST_ASSERT_TRUE(td_said(">ok"));

    /* A C5 at another version than the host's is a bad card. */
    td_clear();
    td_verb(&r);
    radio_answer((uint8_t)(TD_C5_FW_PATCH + 1U), false);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec radio_card ok=0 chip=esp32c5 id=23 kode=1 fw=2.12.10 host=2.12.9"));

    /* Every number at its widest: each record still ends whole, nothing cut. */
    td_clear();
    td_verb(&r);
    radio_answer(TD_C5_FW_PATCH, true);
    td_take();
    TEST_ASSERT_TRUE(td_said(" sdio=255x4294967295kHz"));
    TEST_ASSERT_TRUE(td_said(" ble_ms=4294967295 ms=4294967295"));
    TEST_ASSERT_TRUE(td_said(" rssi_rx=-128 why=timeout"));
    td_down();
}

/* A discovery sweeps every channel (~36 s on the bench), past the radio
 * check's 33 s: its own bound lets it finish rather than time it out. */
TEST_CASE("test_a_discovery_of_a_whole_sweep_is_not_a_timeout", "[test_dot_ao]")
{
    TestDotReqEvt r;

    td_up();
    (void)memset(&r, 0, sizeof(r));
    r.verb             = (uint8_t)TEST_DOT_VERB_RADIO;
    r.a.radio.discover = true;
    td_verb(&r);
    td_wait_ms(TD_SWEEP_MS);
    td_take();
    TEST_ASSERT_FALSE(td_said("why=timeout"));
    TEST_ASSERT_FALSE(td_said(">err"));
    radio_answer(TD_C5_FW_PATCH, false);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec radio ok=1"));
    TEST_ASSERT_TRUE(td_said(" why=-"));
    TEST_ASSERT_TRUE(td_said(">ok"));

    /* Its bound still holds: one that never answers is a timeout at it. */
    td_clear();
    td_verb(&r);
    td_wait_ms(KODE_TEST_DOT_DISCOVER_MS);
    td_take();
    TEST_ASSERT_TRUE(td_said("rec radio ok=0 why=timeout"));
    /* Its late answer is dropped. */
    td_clear();
    radio_answer(TD_C5_FW_PATCH, false);
    td_take();
    TEST_ASSERT_FALSE(td_said("rec radio ok=1"));
    td_down();
}
