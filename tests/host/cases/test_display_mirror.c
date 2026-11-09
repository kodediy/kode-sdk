/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_display_mirror.c
 * @brief The mirror state machine: what leaves, and what must not.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"
#include "Mockkdrv_display.h"
#include "kdrv_tca6408a.h"
#include "Mockkdrv_jpeg.h"
#include "Mockkdrv_ppa.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kbsp_owners.h"
#include "kbsp_board.h"
#include "khal_sync.h"
#include "kmirror_proto.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief A frame small enough to leave in one record: 14-byte header plus
 *         these 4 bytes is 18, well under KMIRROR_PAY_BYTES (162). Content is
 *         never inspected - only mirror_try_start()'s own memcpy() of it
 *         needs it to be real, readable memory. */
static const uint8_t JPEG_SMALL[4] = { 0xDEU, 0xADU, 0xBEU, 0xEFU };

/** @brief A frame that cannot leave in one tick: 14 + 3000 = 3014 bytes,
 *         which needs 19 records at 162 bytes each and therefore at least
 *         two ticks at 12 records per tick. Zero-filled; never inspected. */
static uint8_t JPEG_BIG[3000];

/** @brief One byte past display_ao.c's KMIRROR_JPEG_CAP (32 KiB): too big
 *         for me->frame no matter how many times mirror_try_start() retries
 *         it. Zero-filled; never inspected - this is only ever used to
 *         drive the drop branch, never encoded into a wire record. */
static uint8_t JPEG_HUGE[(32U * 1024U) + 1U];

/** @brief display_ao.c's KMIRROR_ABORT_LINE, copied rather than exposed:
 *         that macro is private to the AO, and a wire constant is exactly
 *         the kind of value a case should pin against a literal rather than
 *         against the very definition it is meant to be checking.
 *         kode_log_reply() appends the newline - see its own comment. */
static const char MIRROR_ABORT_LINE[] = ">|mabort\n";

/** @brief display_ao.c's KMIRROR_PAUSE_LINE, copied for the same reason as
 *         the abort line above. A pause is a wire event, not only a log
 *         line: a host that is here has nothing left to acknowledge, so it
 *         cannot ack its way back and must be told to start again. */
static const char MIRROR_PAUSE_LINE[] = ">|mpause\n";

/**
 * @brief Bring AO_Display up, once for the whole binary.
 *
 * qf_support_claim_ao() rather than a local static bool: test_display_ao.c
 * already brings this AO up for its own cases, and two once-flags that
 * cannot see each other is exactly the "two registrations" failure
 * qf_support.h's own doc comment describes.
 */
static void display_mirror_up_once(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();

    /* Handed over before the initial transition; ignored rather than
     * expected for the same reason test_display_ao.c ignores it - which case
     * happens to construct the AO first is link order, and no case here is
     * about the semaphore changing hands. */

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
         * answered twice (level, then direction) before the panel is touched.
         * test_display_ao.c asserts on that handshake; here it is only walked
         * so the AO reaches Display_active. */
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

    fake_klog_reset();
    fake_psram_reset();
    ao_spy_drain();
}

/* ---- Driving the AO, in place of a host and a scheduler -------------------
 * The mirror's tick is a ONE-SHOT QTimeEvt the AO re-arms itself, every
 * pass - see mirror_pass()'s own comment for why. A one-shot only clears
 * QP's "already armed" bookkeeping (QTimeEvt.ctr) when it actually fires
 * through QF's tick machinery; dispatching DISPLAY_MIRROR_TICK_SIG by hand,
 * the way a plain QEvt would, skips that and leaves the timer looking armed
 * forever, so the AO's own re-arm at the end of the handler trips
 * Q_REQUIRE_INCRIT(450) on the very next tick. qf_support_tick() is built
 * for exactly this - it turns QF's real clock forward one step, lets the
 * timer disarm itself and post for real, then dispatches what landed in
 * AO_Display's own queue - which is what keeps this test honest about the
 * mechanism instead of only about the handler body.
 */

static void mirror_tick(void)
{
    (void)qf_support_tick(AO_Display, 1U);
}

/** @brief Whether the mirror was started at scale 2, which halves through
 *         the PPA - see expect_frame(). */
static bool s_halving;

static void mirror_start(uint8_t scale, uint8_t quality)
{
    MirrorStartEvt e;

    s_halving = (2U == scale);

    (void)memset(&e, 0, sizeof(e));
    e.super   = (QEvt)QEVT_INITIALIZER(DISPLAY_MIRROR_START_SIG);
    e.scale   = scale;
    e.quality = quality;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

static void mirror_stop(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_MIRROR_STOP_SIG);
    QASM_DISPATCH(AO_Display, &req, 0U);
}

static void mirror_ack(uint16_t seq)
{
    MirrorAckEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(DISPLAY_MIRROR_ACK_SIG);
    e.seq   = seq;
    QASM_DISPATCH(AO_Display, &e.super, 0U);
}

/**
 * @brief Arrange one successful dirty -> band -> PPA -> JPEG chain, for the
 *        very next "start" tick.
 *
 * @p dirty_y1/@p dirty_y2 are what kdrv_display_dirty_take() hands back; they
 * are ignored by the AO on the very first frame after mirror_start(), which
 * always forces the whole panel instead. The band and PPA buffers are never
 * read by production code under test - the PPA and the encoder are both
 * mocked - only @p jpeg is, because mirror_try_start() really does memcpy()
 * it into the frame it copies out.
 */
static void expect_frame(int32_t dirty_y1, int32_t dirty_y2,
                         const uint8_t *jpeg, uint32_t jlen)
{
    /* Every one of these has to outlive this function, not just the call
     * that fills it in. CMock's ReturnThruPtr_* does not copy the value now
     * - it remembers the ADDRESS and memcpy()s from it only when the real
     * call under test happens, which for this helper is always after
     * expect_frame() has already returned. An ordinary local's address is
     * stack space this function has given back by then, so the mock would
     * be reading whatever later calls (mirror_tick(), the dispatcher, lldb's
     * own frame handling) had since written there - which is exactly how
     * this file first shipped, and it read back a NULL jpg pointer and
     * segfaulted inside mirror_try_start()'s memcpy(). Static, like s_band
     * and s_ppa always were, is what makes the address still good later. */
    static uint8_t        s_band[4];
    static uint8_t        s_ppa[4];
    static int32_t        s_y1;
    static int32_t        s_y2;
    static const uint8_t *s_rows;
    static size_t         s_band_len;
    static uint8_t        *s_ppa_out;
    static uint32_t       s_ppa_w;
    static uint32_t       s_ppa_h;
    static uint8_t        *s_jpg;
    static uint32_t        s_jlen;

    s_y1       = dirty_y1;
    s_y2       = dirty_y2;
    s_rows     = s_band;
    s_band_len = sizeof(s_band);
    s_ppa_out  = s_ppa;
    s_ppa_w    = (uint32_t)KBSP_LCD_H_RES;
    s_ppa_h    = 1U;
    s_jpg      = (uint8_t *)jpeg;
    s_jlen     = jlen;

    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&s_y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&s_y2);

    kdrv_display_band_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);

    /* At scale 1 the band is encoded where it lies; only a halving pass
     * goes through the PPA. */
    if ( s_halving )
    {
        kdrv_ppa_prepare_ExpectAnyArgsAndReturn(KODE_OK);
        kdrv_ppa_prepare_ReturnThruPtr_dst(&s_ppa_out);
        kdrv_ppa_prepare_ReturnThruPtr_dst_w(&s_ppa_w);
        kdrv_ppa_prepare_ReturnThruPtr_dst_h(&s_ppa_h);
    }

    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);
}

/** @brief KMIRROR_WINDOW, display_ao.c's own ceiling - see its comment for
 *         why 4, not the spec's original 2. Duplicated here rather than
 *         shared: that #define is private to display_ao.c, and a wire
 *         constant belongs pinned against a literal in the case that
 *         exercises it, not against the very definition under test. */
#define TEST_MIRROR_WINDOW 4U

/**
 * @brief Send exactly TEST_MIRROR_WINDOW frames without any ack - the
 *        window genuinely exhausted, which is what every case below that
 *        drives a "no room left" or "a stall trips" scenario needs before
 *        the behaviour under test can be observed at all.
 */
static void fill_credit_window(void)
{
    expect_frame(0, 0, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
    mirror_tick();
    mirror_tick();                       /* frame 1, seq 0 (forced full) */

    for ( unsigned i = 0U; (TEST_MIRROR_WINDOW - 1U) > i; ++i )
    {
        expect_frame(50, 60, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
        mirror_tick();
        mirror_tick();                   /* frames 2..window, seq 1.. */
    }
}

/** @brief Arrange "nothing changed": dirty_take answers empty, and nothing
 *         downstream is ever called - the whole point of the credit and
 *         dirty-band checks running before any hardware is touched.
 *
 * Static for the same reason expect_frame()'s locals are - see its comment. */
static void expect_nothing_dirty(void)
{
    static int32_t s_y1;
    static int32_t s_y2;

    s_y1 = (int32_t)KBSP_LCD_V_RES;
    s_y2 = 0;

    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&s_y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&s_y2);
}

/**
 * @brief Undo mirror_send_slice()'s base64, for a case that wants to look at
 *        what actually left rather than only that something did.
 *
 * Test-only: production has nothing symmetrical to call, since the board
 * only ever encodes. Stops at the first '=' or unrecognised character,
 * which is what a padded tail needs.
 *
 * @param out Must hold at least ((strlen(in) / 4) * 3) bytes.
 * @return Bytes written.
 */
static size_t mirror_b64_decode(const char *in, uint8_t *out)
{
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t   o    = 0U;
    uint32_t buf  = 0U;
    int      bits = 0;

    for ( const char *p = in; '\0' != *p; ++p )
    {
        const char *pos = strchr(alphabet, *p);
        if ( NULL == pos )
        {
            break;                    /* '=' padding, or the line's end */
        }
        buf  = (buf << 6) | (uint32_t)(pos - alphabet);
        bits += 6;
        if ( 8 <= bits )
        {
            bits -= 8;
            out[o] = (uint8_t)((buf >> bits) & 0xFFU);
            ++o;
        }
    }
    return o;
}

/** @brief Every mock's Init(), called before any expectation is queued. */
static void mocks_init(void)
{
    Mockkdrv_display_Init();
    kdrv_display_video_expected_IgnoreAndReturn(false);
    kdrv_display_link_Ignore();
    kdrv_display_recoveries_IgnoreAndReturn(0U);
    kdrv_display_recoveries_set_Ignore();
    Mockkdrv_jpeg_Init();
    Mockkdrv_ppa_Init();
}

/** @brief Every mock's Verify() then Destroy(), called at the end of a case. */
static void mocks_done(void)
{
    Mockkdrv_display_Verify();
    Mockkdrv_jpeg_Verify();
    Mockkdrv_ppa_Verify();
    Mockkdrv_display_Destroy();
    Mockkdrv_jpeg_Destroy();
    Mockkdrv_ppa_Destroy();
}

/* ---- 1. An idle screen costs nothing, even mid-stream --------------------- */

TEST_CASE("after the first full frame, a tick with nothing dirty emits nothing",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    /* The dirty values here are irrelevant: this is the first frame after
     * mirror_start(), which always forces the whole panel. */
    expect_frame(0, 0, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
    mirror_tick();                       /* encode + copy; nothing sent yet */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "the tick that encodes a frame must not also emit any of it");

    mirror_tick();                       /* the one record this frame needs */
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING_LEN(">|m=", fake_klog_line(0), 4U);

    expect_nothing_dirty();
    mirror_tick();                       /* start again: nothing changed */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_klog_count(),
        "an idle screen must cost zero further bytes");

    mirror_stop();
    mocks_done();
}

/* ---- 2. The credit window, not a timer -------------------------------------
 * Four frames outstanding is exactly what the window allows without any ack
 * - KMIRROR_WINDOW, raised from the spec's original 2 (see display_ao.c's
 * own comment on why: only one frame buffer ever exists, so the window never
 * bounded memory, only how far the sequence may run ahead of the host's
 * acks). A fifth attempt must find no room and cost nothing. This is the
 * case that pins the new ceiling is genuinely wider than the old one, not
 * merely renamed. */
TEST_CASE("without any ack, at most four frames go out - the wider credit "
          "window", "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    fill_credit_window();
    TEST_ASSERT_EQUAL_UINT(TEST_MIRROR_WINDOW, fake_klog_count());

    /* Credit denies this before the driver is ever asked for a dirty band -
     * no expectation is queued for it, so an unexpected dirty_take() here
     * would fail the case on its own. */
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW, fake_klog_count(),
        "a fifth frame must wait for credit, not go out unacknowledged");

    mirror_stop();
    mocks_done();
}

/* ---- 3. An ack releases exactly one slot ----------------------------------- */

TEST_CASE("an ack releases exactly one credit slot", "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    fill_credit_window();                /* frames 1..4, seq 0..3 */
    TEST_ASSERT_EQUAL_UINT(TEST_MIRROR_WINDOW, fake_klog_count());

    mirror_tick();                       /* frame 5: denied, window still full */
    TEST_ASSERT_EQUAL_UINT(TEST_MIRROR_WINDOW, fake_klog_count());

    mirror_ack(0U);                      /* releases seq 0's slot, and only it */

    expect_frame(70, 80, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
    mirror_tick();
    mirror_tick();                       /* frame 5 now goes out */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW + 1U, fake_klog_count(),
        "one ack must free exactly one slot");

    mirror_tick();                       /* frame 6: denied again - one ack, one slot */
    TEST_ASSERT_EQUAL_UINT(TEST_MIRROR_WINDOW + 1U, fake_klog_count());

    mirror_stop();
    mocks_done();
}

/* ---- 4. mirror_stop is not merely "ignore the next frame" ------------------ */

TEST_CASE("mirror_stop disarms the tick, and a later ack changes nothing",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    expect_frame(0, 0, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
    mirror_tick();                       /* frame 1 encoded, not yet sent */

    mirror_stop();

    /* A real clock tick, through qf_support_tick() - and mirror_stop()
     * disarmed the QTimeEvt, so QF has nothing armed to fire and posts
     * nothing to AO_Display's queue. This is the genuine article, not a
     * stand-in: if mirror_stop() had left the timer armed, this tick would
     * fire it for real. Nothing is queued for kdrv_display_dirty_take()
     * here either, so the AO calling it would fail the case on its own; the
     * record count below is the assertion that matters. */
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "a tick arriving after stop must not push the frame that was mid-flight");

    mirror_ack(0U);
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "an ack after stop must not resume the stream");

    mocks_done();
}

/* ---- 5. THE case: a frame is sliced, not dumped ---------------------------- */

/* A 3014-byte frame needs 19 records at 162 bytes each and 12 records is one
 * tick's whole allowance, so this cannot finish in one send-tick without the
 * slicing having collapsed back into a single run-to-completion step. A test
 * that only checked the frame eventually arrived would pass either way -
 * this one specifically pins the record count after exactly one send-tick to
 * a number strictly short of the frame's total. */
TEST_CASE("a frame that needs several records is sliced across several ticks, "
          "not sent in one", "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    expect_frame(0, 0, JPEG_BIG, (uint32_t)sizeof(JPEG_BIG));
    mirror_tick();                       /* encode + copy only */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "the encode tick must not itself emit any records");

    mirror_tick();                       /* first send-tick: one slice's worth */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(12U, fake_klog_count(),
        "one tick sent the whole 19-record frame - the slicing has collapsed "
        "back into a single run-to-completion step");

    mirror_tick();                       /* second send-tick: the remainder */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(19U, fake_klog_count(),
        "the frame must be whole once every slice has gone out");

    mirror_stop();
    mocks_done();
}

/* ---- A busy codec is retried, not lost -------------------------------------
 * kdrv_jpeg_encode() answering KODE_DENIED is the ordinary shape of "someone
 * else - test component=jpeg, on AO_Console's own thread - holds the codec
 * right now", not a fault. The dirty rows this pass already took out of the
 * driver's own tracker have to survive the refusal, because a second
 * kdrv_display_dirty_take() would report "nothing changed" and lose them for
 * good. Only one dirty_take() expectation is queued below for both attempts,
 * so an accidental second call fails the case on its own. */
TEST_CASE("a busy encoder is retried next tick, not silently dropped",
          "[display_mirror]")
{
    static uint8_t  band[4];
    const uint8_t  *rows     = band;
    size_t          band_len = sizeof(band);
    int32_t         y1       = 0;
    int32_t         y2       = 0;

    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&y2);
    kdrv_display_band_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_display_band_ReturnThruPtr_rows(&rows);
    kdrv_display_band_ReturnThruPtr_len(&band_len);
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_DENIED);

    mirror_tick();                       /* the codec is busy; nothing to send */
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    /* Retried WITHOUT a second dirty_take() - see the comment above. */
    kdrv_display_band_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_display_band_ReturnThruPtr_rows(&rows);
    kdrv_display_band_ReturnThruPtr_len(&band_len);
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    uint8_t  *jpg  = (uint8_t *)JPEG_SMALL;
    uint32_t  jlen = (uint32_t)sizeof(JPEG_SMALL);
    kdrv_jpeg_encode_ReturnThruPtr_out(&jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&jlen);

    mirror_tick();                       /* retried, and this time it takes */
    mirror_tick();                       /* the one record leaves */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_klog_count(),
        "the band the first attempt took out of the driver must not be lost");

    mirror_stop();
    mocks_done();
}

/* At scale 1 the band is encoded straight out of the framebuffer, and every
 * case above runs there with no PPA expectation at all - which CMock, being
 * strict, turns into "the PPA is not called". This is the other half: at
 * scale 2 the band goes through the PPA to be halved before it is encoded. */
TEST_CASE("at scale 2 the band is halved by the PPA before it is encoded",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(2U, 70U);
    expect_frame(0, 0, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));

    mirror_tick();                       /* band, halve, encode, copy */
    mirror_tick();                       /* the one record leaves */
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    mirror_stop();
    mocks_done();
}

/* ---- The header on the wire is the header the band was computed from ----- */

/* Every other case here pins nothing on kdrv_display_band()'s own y/h and
 * never looks at what actually reached the wire - deliberately, to keep
 * from re-testing the encode path six times over. That leaves exactly the
 * gap an inverted y/h wiring bug would live in undetected: this case pins
 * the band call's arguments AND decodes the one record the first frame
 * produces, so both the call in and the header out are checked against the
 * same, independently known values. */
TEST_CASE("the emitted record's header carries the band it was encoded from",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    static uint8_t         s_band[4];
    static const uint8_t  *s_rows      = s_band;
    static size_t           s_band_len = sizeof(s_band);
    static uint8_t          *s_jpg;
    static uint32_t          s_jlen;
    static int32_t           s_y1;
    static int32_t           s_y2;

    /* Ignored: the first frame after mirror_start always forces the whole
     * panel, so what dirty_take() answers here does not matter. */
    s_y1    = 0;
    s_y2    = 0;
    s_jpg   = (uint8_t *)JPEG_SMALL;
    s_jlen  = (uint32_t)sizeof(JPEG_SMALL);

    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&s_y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&s_y2);

    /* Pinned, not ExpectAnyArgs: at scale 1 the whole-panel override is
     * (y=0, h=V_RES) exactly, and this is the call an inverted y/h would
     * get wrong. rows/len stay free - CMock has no way to know their
     * addresses ahead of the call - but ReturnThruPtr still fills them in. */
    kdrv_display_band_ExpectAndReturn((uint32_t)0U,
                                      (uint32_t)KBSP_LCD_V_RES,
                                      NULL, NULL, KODE_OK);
    kdrv_display_band_IgnoreArg_rows();
    kdrv_display_band_IgnoreArg_len();
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);

    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);

    mirror_tick();                       /* encode + copy */
    mirror_tick();                       /* the one record this frame needs */
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    const char *line = fake_klog_line(0U);
    TEST_ASSERT_EQUAL_STRING_LEN(">|m=", line, 4U);

    uint8_t raw[KMIRROR_HDR_LEN + sizeof(JPEG_SMALL)];
    size_t  raw_len = mirror_b64_decode(line + 4, raw);
    TEST_ASSERT_EQUAL_UINT(sizeof(raw), raw_len);

    kmirror_hdr_t hdr;
    TEST_ASSERT_TRUE(kmirror_hdr_get(raw, &hdr));
    TEST_ASSERT_EQUAL_UINT16(0U, hdr.seq);
    TEST_ASSERT_EQUAL_UINT16(0U, hdr.x);
    TEST_ASSERT_EQUAL_UINT16(0U, hdr.y);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KBSP_LCD_H_RES, hdr.w);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KBSP_LCD_V_RES, hdr.h);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JPEG_SMALL), hdr.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(JPEG_SMALL, &raw[KMIRROR_HDR_LEN],
                                  sizeof(JPEG_SMALL));

    mirror_stop();
    mocks_done();
}

/* ---- The critical fix: an oversized frame must not loop forever ---------- */

/* This is the finding's own reproduction, cut down to what a host case can
 * pin: a frame too big for me->frame is dropped, but a bug left me->first
 * true afterward, which forced every following tick back through the same
 * doomed whole-panel attempt - at 100 Hz, on the highest-priority AO on the
 * board, until a human typed mirror_stop. The second half of this case is
 * what actually catches that: it pins a NARROW band (50, 10) on the retry,
 * which only kmirror_band() ever produces from a real dirty range - a
 * whole-panel override would ask kdrv_display_band() for (0, V_RES)
 * instead, and CMock would fail this case on the mismatched argument before
 * a single assertion ran. */
TEST_CASE("an oversized frame is dropped without getting stuck on the whole panel",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    expect_frame(0, 0, JPEG_HUGE, (uint32_t)sizeof(JPEG_HUGE));
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_klog_count(),
        "the drop must be logged - a mirror that silently drops every frame "
        "looks identical to one that is simply idle");

    /* Pinned band args, see the case comment: this is what me->first left
     * true would get wrong. */
    static uint8_t         s_band[4];
    static const uint8_t  *s_rows      = s_band;
    static size_t           s_band_len = sizeof(s_band);
    static uint8_t          *s_jpg  = (uint8_t *)JPEG_SMALL;
    static uint32_t          s_jlen = (uint32_t)sizeof(JPEG_SMALL);
    static int32_t           s_y1   = 50;
    static int32_t           s_y2   = 60;

    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&s_y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&s_y2);

    kdrv_display_band_ExpectAndReturn(50U, 10U, NULL, NULL, KODE_OK);
    kdrv_display_band_IgnoreArg_rows();
    kdrv_display_band_IgnoreArg_len();
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);

    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);

    mirror_tick();
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_klog_count(),
        "the mirror must recover and send an ordinary frame after a drop, "
        "not repeat the same whole-panel attempt forever");

    mirror_stop();
    mocks_done();
}

/* ---- A dangerous-but-legal quality request is clamped --------------------- */

/* q=100 is exactly the request that took the board down before this clamp
 * existed: legal on the wire (1..100), and several times KMIRROR_JPEG_CAP on
 * this hardware. Pinning kdrv_jpeg_encode()'s own quality argument is what
 * proves the clamp reaches the encoder rather than only living in a comment. */
TEST_CASE("a quality above the measured ceiling is clamped, not trusted verbatim",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 100U);              /* the legal, dangerous request */

    static uint8_t         s_band[4];
    static const uint8_t  *s_rows      = s_band;
    static size_t           s_band_len = sizeof(s_band);
    static uint8_t          *s_jpg  = (uint8_t *)JPEG_SMALL;
    static uint32_t          s_jlen = (uint32_t)sizeof(JPEG_SMALL);
    static int32_t           s_y1   = 0;
    static int32_t           s_y2   = 0;

    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&s_y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&s_y2);
    kdrv_display_band_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);

    /* 70, not 100 - display_ao.c's KMIRROR_QUALITY_MAX, the only quality
     * this hardware has ever been measured at (kdrv_jpeg.h). */
    kdrv_jpeg_encode_ExpectAndReturn(NULL, 0U, 0U, 70U, NULL, NULL, KODE_OK);
    kdrv_jpeg_encode_IgnoreArg_rgb565();
    kdrv_jpeg_encode_IgnoreArg_w();
    kdrv_jpeg_encode_IgnoreArg_h();
    kdrv_jpeg_encode_IgnoreArg_out();
    kdrv_jpeg_encode_IgnoreArg_out_len();
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);

    mirror_tick();
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    mirror_stop();
    mocks_done();
}

/* ---- Important 1: a host that stops acknowledging is paused, not dropped -
 * The old behaviour disarmed the session outright on a credit stall, which
 * is exactly what made a real viewer unusable: a burst of unrelated `btn`
 * traffic pushing one `mirror_ack` behind it should cost a pause, not the
 * whole session - see display_ao.c's own comment on KMIRROR_STALL_MS. This
 * case drives the full arc: the stall trips, the buffer is genuinely freed
 * (not merely a flag flipped - fake_psram_alloc/free_count() is what proves
 * that), nothing at all happens while paused, and a single ack revives the
 * session with a forced full frame - the host missed every frame while this
 * AO slept, so a delta would draw onto a stale canvas. */
TEST_CASE("with no ack for 5 seconds, the mirror pauses rather than ending",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_alloc_count(),
        "mirror_start must hold exactly one frame buffer");

    fill_credit_window();
    TEST_ASSERT_EQUAL_UINT(TEST_MIRROR_WINDOW, fake_klog_count());

    /* 500 further ticks at 10 ms each is the 5 s KMIRROR_STALL_TICKS names.
     * No expectation is queued for any driver call across this whole run:
     * a stalled mirror must ask the driver for nothing while it waits, and
     * an unexpected call would fail the case on its own - the same proof
     * "a paused mirror does no work at all" needs. The 500th is the one
     * that trips the timeout, and it produces exactly TWO lines on the one
     * capture sink KLOG_W and kode_log_reply() share: the warning that says
     * why, and the `>|mpause` record that tells the host it is owed a
     * restart. */
    for ( unsigned i = 0U; 500U > i; ++i )
    {
        mirror_tick();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW + 2U, fake_klog_count(),
        "the stall must be logged once and announced once, and nothing else "
        "must reach the wire");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(MIRROR_PAUSE_LINE,
        fake_klog_line(TEST_MIRROR_WINDOW + 1U),
        "the host cannot infer a pause: it has nothing outstanding left to "
        "ack, so the wire has to say so");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_free_count(),
        "a paused mirror must hold no memory - the frame buffer must be "
        "released, not merely left idle");

    /* The tick was not re-armed on the pass that tripped the pause, so a
     * further real clock tick has nothing to fire - proof the session is
     * genuinely doing nothing, not merely quiet for one tick. Still no
     * driver expectation queued, so an unexpected call fails this on its
     * own too. */
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW + 2U, fake_klog_count(),
        "a paused mirror's tick must stay disarmed until an ack revives it");

    /* Any ack revives it - the LAST sequence sent, seq 3, is used here only
     * to be an unremarkable, legitimately-outstanding one; kmirror_ack()'s
     * own cumulative refusal rules do not matter to the revival, since
     * mirror_pause()'s ack handler resets the whole credit window rather
     * than feeding it this one ack. */
    mirror_ack(TEST_MIRROR_WINDOW - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW + 2U, fake_klog_count(),
        "reviving must not, by itself, emit anything - only the next tick "
        "may encode");

    /* The revived frame must be the WHOLE panel, not a delta: pinning
     * kdrv_display_band()'s own y/h to (0, V_RES) is what an inverted
     * me->first would get wrong - the same proof "the emitted record's
     * header..." uses. */
    static uint8_t         s_band[4];
    static const uint8_t  *s_rows      = s_band;
    static size_t           s_band_len = sizeof(s_band);
    static uint8_t          *s_jpg  = (uint8_t *)JPEG_SMALL;
    static uint32_t          s_jlen = (uint32_t)sizeof(JPEG_SMALL);
    static int32_t           s_y1   = 90;   /* irrelevant: me->first wins */
    static int32_t           s_y2   = 100;

    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&s_y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&s_y2);
    kdrv_display_band_ExpectAndReturn((uint32_t)0U, (uint32_t)KBSP_LCD_V_RES,
                                      NULL, NULL, KODE_OK);
    kdrv_display_band_IgnoreArg_rows();
    kdrv_display_band_IgnoreArg_len();
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);

    mirror_tick();                        /* revived: encode + copy */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_psram_alloc_count(),
        "revival must allocate a fresh frame buffer, not reuse a freed one");
    mirror_tick();                        /* the one record this frame needs */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW + 3U, fake_klog_count(),
        "a revived mirror must actually resume sending");

    mirror_stop();
    mocks_done();
}

/**
 * @brief mirror_stop on a session this AO itself paused must still end it
 *        cleanly - the same terminal state an ordinary stop leaves, not a
 *        state that quietly waits for an ack that will never come.
 */
TEST_CASE("mirror_stop on a paused session ends it, not merely leaves it "
          "paused", "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);
    fill_credit_window();
    for ( unsigned i = 0U; 500U > i; ++i )
    {
        mirror_tick();
    }
    TEST_ASSERT_EQUAL_UINT(TEST_MIRROR_WINDOW + 2U, fake_klog_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_free_count(),
        "the pause must already have freed the buffer");

    mirror_stop();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW + 2U, fake_klog_count(),
        "a stop on an already-paused session has nothing mid-frame to abort");

    /* An ack after this stop must not revive anything - the same proof
     * "mirror_stop disarms the tick, and a later ack changes nothing" uses
     * for an ordinary stop. No expectation is queued for any driver call,
     * so an unexpected mirror_try_start() would fail this on its own. */
    mirror_ack(0U);
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(TEST_MIRROR_WINDOW + 2U, fake_klog_count(),
        "a stopped session must stay stopped, even after the ack that would "
        "have revived a merely-paused one");

    mocks_done();
}

/* ---- Important 2: a stop or restart mid-frame tells the host to give up --- */

TEST_CASE("mirror_stop mid-frame emits an explicit abort record",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    expect_frame(0, 0, JPEG_BIG, (uint32_t)sizeof(JPEG_BIG));
    mirror_tick();                       /* encode + copy */
    mirror_tick();                       /* first send-tick: 12 of 19 records */
    TEST_ASSERT_EQUAL_UINT(12U, fake_klog_count());

    mirror_stop();                       /* mid-frame: 7 records still owed */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(13U, fake_klog_count(),
        "a stop mid-frame must tell the host to discard what it has");
    TEST_ASSERT_EQUAL_STRING(MIRROR_ABORT_LINE, fake_klog_line(12U));

    mirror_tick();                       /* disarmed: nothing more to send */
    TEST_ASSERT_EQUAL_UINT(13U, fake_klog_count());

    mocks_done();
}

TEST_CASE("a restart landing mid-frame aborts the old one before the new starts",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    mirror_start(1U, 70U);

    expect_frame(0, 0, JPEG_BIG, (uint32_t)sizeof(JPEG_BIG));
    mirror_tick();
    mirror_tick();                       /* first send-tick: 12 of 19 records */
    TEST_ASSERT_EQUAL_UINT(12U, fake_klog_count());

    mirror_start(1U, 70U);               /* a host reattaching without stopping */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(13U, fake_klog_count(),
        "the old frame's own records already promised bytes that will now "
        "never come");
    TEST_ASSERT_EQUAL_STRING(MIRROR_ABORT_LINE, fake_klog_line(12U));

    expect_frame(0, 0, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
    mirror_tick();
    mirror_tick();                       /* the new session's own frame 1 */
    TEST_ASSERT_EQUAL_UINT(14U, fake_klog_count());

    mirror_stop();
    mocks_done();
}

/* ---- Important 3: a transport that stops accepting must not spin forever -
 * Both cases below go over the BLE binary characteristic - fakes/fake_kdrv_
 * ble_mirror.c is the seam, see its own comment and the CMakeLists.txt note
 * on why CONFIG_KODEOS_BLE_CONSOLE is scoped to display_ao.c alone. Frame
 * DATA never reaches fake_klog_count() on this transport - that is the whole
 * point of the binary characteristic - but the two CONTROL records do:
 * `>|mabort` and `>|mpause` ride the console line on both transports, so a
 * klog count here is this AO's own KLOG_W plus whichever of those two the
 * case provoked. See mirror_abort() for why an in-band marker on the binary
 * pipe could not be read.
 */

TEST_CASE("a BLE central that stops accepting mid-frame ends the session "
          "at once, not on a retry", "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();
    fake_ble_mirror_set_attached(true);

    mirror_start(1U, 70U);

    expect_frame(0, 0, JPEG_BIG, (uint32_t)sizeof(JPEG_BIG));
    mirror_tick();                       /* encode + copy; me->ble latched true */
    mirror_tick();                       /* first send-tick: 12 of 19 records */
    TEST_ASSERT_EQUAL_UINT(12U, fake_ble_mirror_write_calls());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "nothing is wrong yet; this AO must stay quiet");

    /* The central is gone. kdrv_ble.h's own contract marks this the ONE
     * answer that is not "try again": every write from here on is refused
     * for good, the same as a real disconnect leaves behind. */
    fake_ble_mirror_deny_writes();

    mirror_tick();                       /* must end the session THIS tick */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_klog_count(),
        "a denied transport must be logged AND the half-sent frame un-promised, "
        "not retried quietly");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(MIRROR_ABORT_LINE, fake_klog_line(1U),
        "twelve records of this frame are already on the air with a `len` "
        "promising the rest; the abort is what withdraws that promise, and it "
        "goes out on the console line because the binary pipe has no way to "
        "frame it");
    unsigned const calls_at_end = fake_ble_mirror_write_calls();

    /* No expectation is queued for any driver call across this next tick -
     * the frame buffer was freed and the codec/PPA claims are done with -
     * and an unexpected call would fail the case on its own, the same proof
     * "with no ack for 5 seconds" uses for a stall. */
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls_at_end, fake_ble_mirror_write_calls(),
        "the mirror must have ended outright, not merely gone quiet for one "
        "tick - AO_Display is the highest-priority AO on the board and has "
        "no business spinning on a link nobody is receiving");
    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());

    mocks_done();
}

TEST_CASE("a BLE ring that never drains pauses on the same 5 s ceiling as "
          "starved credit, and an ack still revives it", "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();
    fake_ble_mirror_set_attached(true);

    mirror_start(1U, 70U);

    expect_frame(0, 0, JPEG_BIG, (uint32_t)sizeof(JPEG_BIG));
    mirror_tick();                       /* encode + copy */
    mirror_tick();                       /* first send-tick: 12 of 19 records */
    TEST_ASSERT_EQUAL_UINT(12U, fake_ble_mirror_write_calls());

    /* KODE_NO_RESOURCES, unlike KODE_DENIED, is the retryable "ring is full
     * right now" - kdrv_ble.h's own contract says try the same slice again.
     * A ring that never drains looks identical to that, forever: this is
     * the shape a permanently congested cable has too, and neither has any
     * protection but the stall counter mirror_pass() now also feeds from a
     * sending tick that moves nothing. */
    fake_ble_mirror_congest_writes();

    /* 500 further ticks at 10 ms each is the 5 s KMIRROR_STALL_TICKS names -
     * the same figure "with no ack for 5 seconds" uses for the credit-
     * starved half of this same counter. */
    for ( unsigned i = 0U; 500U > i; ++i )
    {
        mirror_tick();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, fake_klog_count(),
        "a sending tick that makes no progress must feed the same stall "
        "counter starved credit does, and trip it once - one warning, one "
        "abort for the frame it was half way through, one pause record");
    TEST_ASSERT_EQUAL_STRING(MIRROR_ABORT_LINE, fake_klog_line(1U));
    TEST_ASSERT_EQUAL_STRING(MIRROR_PAUSE_LINE, fake_klog_line(2U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_free_count(),
        "the mid-frame stall must free the buffer too, exactly like the "
        "credit-starved one");
    unsigned const calls_at_end = fake_ble_mirror_write_calls();

    mirror_tick();                       /* the timer was not re-armed */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls_at_end, fake_ble_mirror_write_calls(),
        "the mirror must have paused itself, not merely gone quiet for "
        "one tick");
    TEST_ASSERT_EQUAL_UINT(3U, fake_klog_count());

    /* Still an ordinary ack that revives it - the mid-frame BLE stall is
     * not a different kind of pause. The ring is not congested any more
     * (fake_ble_mirror_congest_writes() only lasted while it was set), so a
     * revived frame reaches the transport that was subscribed the whole
     * time. */
    mirror_ack(0U);
    /* (0, 0) here means "irrelevant" the same way it does at every other
     * forced-full frame in this file: me->first overrides whatever dirty_
     * take() answers. */
    expect_frame(0, 0, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
    mirror_tick();                       /* revived: encode + copy */
    mirror_tick();                       /* the one record this frame needs */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls_at_end + 1U,
        fake_ble_mirror_write_calls(),
        "a revived session must resume sending over the same transport");

    mirror_stop();
    mocks_done();
}

/* ---- Important 4: a pipeline that never yields a frame must say so -------
 * The three refusals mirror_try_start() can meet - no shadow, no PPA, no
 * codec - are all "transient" in shape and two of them are permanent in
 * practice: a board whose PPA or shadow failed to allocate at boot answers
 * the same way on every single pass. The stall counter used to be fed only
 * by starved credit and by a frame in flight that moved nothing, so this
 * third shape went uncounted, and the session answered `>ok` to mirror_start
 * and then retried the identical refusal at 100 Hz, for the life of the
 * board, with nothing in any log to say why.
 *
 * Both cases below drive hundreds of consecutive passes, which is what the
 * 5 s ceiling means at a 10 ms tick, so both use CMock STUBS rather than
 * queued expectations: a thousand Expect() calls would be a test of CMock's
 * queue depth, and a Stub answers every call from one function while still
 * counting them. Whether a PAUSED mirror calls the driver at all is already
 * proven strictly, by expectation, in "with no ack for 5 seconds".
 */

static uint8_t        s_stub_band[4];
static uint8_t        s_stub_ppa[4];
static unsigned       s_stub_band_calls;
static bool           s_stub_dirty;   /**< false = an idle screen.           */

/** @brief The whole panel, or an empty band, on @c s_stub_dirty's word. */
static void stub_dirty_take(int32_t *y1, int32_t *y2, int calls)
{
    (void)calls;
    if ( s_stub_dirty )
    {
        *y1 = 0;
        *y2 = (int32_t)KBSP_LCD_V_RES;
    }
    else
    {
        /* y1 above y2 is kdrv_display_dirty_take()'s own "nothing changed". */
        *y1 = (int32_t)KBSP_LCD_V_RES;
        *y2 = 0;
    }
}

static kode_status_t stub_band(uint32_t y, uint32_t h, const uint8_t **rows,
                               size_t *len, int calls)
{
    (void)y; (void)h; (void)calls;
    ++s_stub_band_calls;
    *rows = s_stub_band;
    *len  = sizeof(s_stub_band);
    return KODE_OK;
}

static kode_status_t stub_ppa_denies(const uint8_t *src, uint32_t w,
                                     uint32_t h, uint32_t scale, uint8_t **dst,
                                     uint32_t *dst_w, uint32_t *dst_h,
                                     int calls)
{
    (void)src; (void)w; (void)h; (void)scale; (void)dst; (void)dst_w;
    (void)dst_h; (void)calls;
    return KODE_DENIED;
}

static kode_status_t stub_ppa_ok(const uint8_t *src, uint32_t w, uint32_t h,
                                 uint32_t scale, uint8_t **dst,
                                 uint32_t *dst_w, uint32_t *dst_h, int calls)
{
    (void)src; (void)w; (void)h; (void)scale; (void)calls;
    *dst   = s_stub_ppa;
    *dst_w = (uint32_t)KBSP_LCD_H_RES;
    *dst_h = (uint32_t)KBSP_LCD_V_RES;
    return KODE_OK;
}

static kode_status_t stub_jpeg(const uint8_t *rgb565, uint32_t w, uint32_t h,
                               uint32_t quality, uint8_t **out,
                               uint32_t *out_len, int calls)
{
    (void)rgb565; (void)w; (void)h; (void)quality; (void)calls;
    *out     = (uint8_t *)JPEG_SMALL;
    *out_len = (uint32_t)sizeof(JPEG_SMALL);
    return KODE_OK;
}

TEST_CASE("a pipeline that refuses every pass pauses on the same 5 s ceiling, "
          "instead of retrying forever in silence", "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    s_stub_dirty      = true;
    s_stub_band_calls = 0U;
    kdrv_display_dirty_take_Stub(stub_dirty_take);
    kdrv_display_band_Stub(stub_band);
    kdrv_ppa_prepare_Stub(stub_ppa_denies);

    mirror_start(2U, 70U);               /* halving: the PPA is in the path */
    TEST_ASSERT_EQUAL_UINT(1U, fake_psram_alloc_count());

    for ( unsigned i = 0U; 499U > i; ++i )
    {
        mirror_tick();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "a refusal is ordinary for as long as it might still clear - the "
        "codec really is sometimes busy with a selftest - so nothing may be "
        "said before the ceiling");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(499U, s_stub_band_calls,
        "every pass really did try, which is what makes the silence a bug "
        "rather than an idle screen");

    mirror_tick();                       /* the 500th: the ceiling */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_klog_count(),
        "the warning that says why, and the record that tells the host it is "
        "owed a restart");
    TEST_ASSERT_EQUAL_STRING(MIRROR_PAUSE_LINE, fake_klog_line(1U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_free_count(),
        "a paused mirror holds no frame buffer, whichever of the three "
        "stalls put it to sleep");

    unsigned const calls_at_pause = s_stub_band_calls;
    for ( unsigned i = 0U; 50U > i; ++i )
    {
        mirror_tick();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(calls_at_pause, s_stub_band_calls,
        "the tick must stay disarmed - a paused mirror asks the driver for "
        "nothing at all");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_klog_count(),
        "one warning per stall, not one every 10 ms for as long as the fault "
        "lasts");

    mirror_stop();
    mocks_done();
}

/* ---- An idle screen is not a stall --------------------------------------
 * The counter above has to tell "nothing was dirty" from "rows were owed and
 * refused", because a still launcher produces the first on every single tick.
 * Getting that wrong would pause the mirror every five seconds on exactly the
 * screen the whole delta design exists to make free.
 */

TEST_CASE("a screen that is simply idle never trips the stall ceiling",
          "[display_mirror]")
{
    mocks_init();
    display_mirror_up_once();

    s_stub_dirty      = true;
    s_stub_band_calls = 0U;
    kdrv_display_dirty_take_Stub(stub_dirty_take);
    kdrv_display_band_Stub(stub_band);
    kdrv_ppa_prepare_Stub(stub_ppa_ok);
    kdrv_jpeg_encode_Stub(stub_jpeg);

    mirror_start(1U, 70U);

    /* The forced first frame, so me->first is behind us and every later tick
     * is a genuine "nothing changed" rather than the full panel this AO
     * sends regardless of the driver's answer. */
    mirror_tick();                       /* encode + copy */
    mirror_tick();                       /* the one record it needs */
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    s_stub_dirty = false;                /* the launcher, sitting still */
    unsigned const bands_before = s_stub_band_calls;

    /* Nine seconds - well past the 5 s ceiling - on a screen nobody touches. */
    for ( unsigned i = 0U; 900U > i; ++i )
    {
        mirror_tick();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_klog_count(),
        "an idle screen is the feature working, not a stall: it must never "
        "pause itself");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(bands_before, s_stub_band_calls,
        "and it must cost nothing while it sits there - not one band read");

    /* And it is genuinely still running, not merely quiet. */
    s_stub_dirty = true;
    mirror_tick();
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_klog_count(),
        "a mirror that sat idle for nine seconds must still answer the next "
        "repaint - a pause here would have needed a restart nobody sent");

    mirror_stop();
    mocks_done();
}

/* ---- The shot: the whole glass as one JPEG, lent until released -----------
 * AO_Display encodes the front framebuffer top-down in one step and lends the
 * result to whoever asked. It is not the mirror: it takes no credit, reads no
 * dirty range and starts no session, and it must not disturb one that runs.
 * The cap is the header's (KSHOT_JPEG_CAP) and the quality a literal, pinned
 * here against the value the spec fixes rather than against its definition.
 */

/** @brief A frame one byte past KSHOT_JPEG_CAP: never copied, so zeros do. */
static uint8_t JPEG_PAST_CAP[KSHOT_JPEG_CAP + 1U];

/** @brief What a JPEG starts with; the shot copies it byte for byte. */
static const uint8_t JPEG_SOI[3] = { 0xFFU, 0xD8U, 0xFFU };

/** @brief The quality the spec fixes for a shot. */
#define TEST_SHOT_QUALITY 80U

/**
 * @brief Give back whatever a failed earlier case left lent.
 *
 * The lend is module state, so a case that failed half way would hand its
 * successor a "shot already lent" for no reason of its own.
 */
static void shot_release(void)
{
    static const QEvt end = QEVT_INITIALIZER(DISPLAY_SHOT_END_SIG);
    QASM_DISPATCH(AO_Display, &end, 0U);
}

/**
 * @brief Ask for a shot and take the answer the spy received.
 * @return The answer's status; its length lands in @p len.
 */
static uint8_t shot_ask(uint32_t *len)
{
    DisplayShotReqEvt req;
    uint8_t           st = (uint8_t)KODE_ERROR;

    (void)memset(&req, 0, sizeof(req));
    req.super   = (QEvt)QEVT_INITIALIZER(DISPLAY_SHOT_REQ_SIG);
    req.who     = ao_spy();
    req.quality = (uint8_t)TEST_SHOT_QUALITY;
    QASM_DISPATCH(AO_Display, &req.super, 0U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ao_spy_count(),
        "a shot request is always answered, even by a refusal");
    const DisplayShotRspEvt *rsp = (const DisplayShotRspEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_RSP_SIG, rsp->super.sig);
    st   = rsp->status;
    *len = rsp->len;
    ao_spy_drain();
    return st;
}

/**
 * @brief Arrange the whole-glass read and the encode a shot makes.
 *
 * Pins the arguments that are the spec: the band is the whole glass from
 * row 0, the encode is that band where it lies at full width and height and
 * the shot's quality. Statics for the reason expect_frame() gives.
 */
static void expect_shot(const uint8_t *jpeg, uint32_t jlen, kode_status_t rc)
{
    static uint8_t        s_band[4];
    static const uint8_t *s_rows;
    static size_t         s_band_len;
    static uint8_t       *s_jpg;
    static uint32_t       s_jlen;

    s_rows     = s_band;
    s_band_len = sizeof(s_band);
    s_jpg      = (uint8_t *)jpeg;
    s_jlen     = jlen;

    kdrv_display_band_ExpectAndReturn(0U, (uint32_t)KBSP_LCD_V_RES, NULL, NULL,
                                      KODE_OK);
    kdrv_display_band_IgnoreArg_rows();
    kdrv_display_band_IgnoreArg_len();
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);

    kdrv_jpeg_encode_ExpectAndReturn(s_band, (uint32_t)KBSP_LCD_H_RES,
                                     (uint32_t)KBSP_LCD_V_RES,
                                     TEST_SHOT_QUALITY, NULL, NULL, rc);
    kdrv_jpeg_encode_IgnoreArg_out();
    kdrv_jpeg_encode_IgnoreArg_out_len();
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);
}

/* This case is first on purpose: the shot buffer is taken once and kept, so
 * only the first shot of the binary can meet a PSRAM that refuses it. */
TEST_CASE("a shot with no PSRAM at first use answers NO_RESOURCES and retries",
          "[display_shot]")
{
    mocks_init();
    display_mirror_up_once();
    shot_release();

    uint32_t len = 0U;
    fake_psram_refuse(1U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, shot_ask(&len));
    TEST_ASSERT_NULL(display_shot_buf());

    /* Nothing was kept, so the next request asks PSRAM again and works. */
    expect_shot(JPEG_SOI, (uint32_t)sizeof(JPEG_SOI), KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, shot_ask(&len));
    TEST_ASSERT_EQUAL_UINT(1U, fake_psram_alloc_count());
    shot_release();
    mocks_done();
}

TEST_CASE("a shot encodes the whole glass and lends it until released",
          "[display_shot]")
{
    mocks_init();
    display_mirror_up_once();
    shot_release();

    uint32_t len = 0U;
    expect_shot(JPEG_SOI, (uint32_t)sizeof(JPEG_SOI), KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, shot_ask(&len));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JPEG_SOI), len);
    const uint8_t *first = display_shot_buf();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(JPEG_SOI, first, sizeof(JPEG_SOI));

    /* Lent: a second request is refused, and touches neither the panel nor
     * the codec (a mock call here would fail the case). */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, shot_ask(&len));
    TEST_ASSERT_EQUAL_PTR_MESSAGE(first, display_shot_buf(),
        "a refused request must not disturb the shot that is lent");

    shot_release();
    TEST_ASSERT_NULL_MESSAGE(display_shot_buf(),
        "a released shot lends nothing");

    /* The buffer is kept: the next shot is the same block, and nothing was
     * allocated or freed for it. */
    fake_psram_reset();
    expect_shot(JPEG_SOI, (uint32_t)sizeof(JPEG_SOI), KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, shot_ask(&len));
    TEST_ASSERT_EQUAL_PTR(first, display_shot_buf());
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_free_count());
    shot_release();

    /* Releasing with nothing lent is harmless. */
    shot_release();
    TEST_ASSERT_NULL(display_shot_buf());
    mocks_done();
}

TEST_CASE("a busy codec answers DENIED, lends nothing and costs no allocation",
          "[display_shot]")
{
    mocks_init();
    display_mirror_up_once();
    shot_release();

    uint32_t len = 0U;
    expect_shot(JPEG_SOI, (uint32_t)sizeof(JPEG_SOI), KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, shot_ask(&len));
    shot_release();                      /* the buffer exists from here on */

    fake_psram_reset();
    expect_shot(JPEG_SOI, (uint32_t)sizeof(JPEG_SOI), KODE_DENIED);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, shot_ask(&len));
    TEST_ASSERT_NULL(display_shot_buf());
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_free_count());

    /* Nothing was held, so the next request is a request, not a refusal. */
    expect_shot(JPEG_SOI, (uint32_t)sizeof(JPEG_SOI), KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, shot_ask(&len));
    shot_release();
    mocks_done();
}

TEST_CASE("a frame past the cap answers ERROR and lends nothing",
          "[display_shot]")
{
    mocks_init();
    display_mirror_up_once();
    shot_release();

    uint32_t len = 0U;
    expect_shot(JPEG_PAST_CAP, (uint32_t)sizeof(JPEG_PAST_CAP), KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_ERROR, shot_ask(&len));
    TEST_ASSERT_NULL(display_shot_buf());

    /* Exactly the cap is the largest that fits. */
    expect_shot(JPEG_PAST_CAP, (uint32_t)KSHOT_JPEG_CAP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, shot_ask(&len));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KSHOT_JPEG_CAP, len);
    shot_release();
    mocks_done();
}

TEST_CASE("a shot with no framebuffer answers NO_RESOURCES", "[display_shot]")
{
    mocks_init();
    display_mirror_up_once();
    shot_release();

    uint32_t len = 0U;
    kdrv_display_band_ExpectAndReturn(0U, (uint32_t)KBSP_LCD_V_RES, NULL, NULL,
                                      KODE_NO_RESOURCES);
    kdrv_display_band_IgnoreArg_rows();
    kdrv_display_band_IgnoreArg_len();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, shot_ask(&len));
    TEST_ASSERT_NULL(display_shot_buf());
    mocks_done();
}

TEST_CASE("a shot never touches the mirror's credit or dirty range",
          "[display_shot]")
{
    mocks_init();
    display_mirror_up_once();
    shot_release();

    mirror_start(1U, 70U);
    expect_frame(0, 0, JPEG_SMALL, (uint32_t)sizeof(JPEG_SMALL));
    mirror_tick();
    mirror_tick();                       /* the first, whole frame is out */
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    /* The mirror takes rows 50..60 and the codec turns it away: the band is
     * now owed, held in the AO as a pending range. */
    static int32_t s_y1 = 50;
    static int32_t s_y2 = 60;
    static uint8_t s_band[4];
    static const uint8_t *s_rows = s_band;
    static size_t s_band_len = sizeof(s_band);
    kdrv_display_dirty_take_ExpectAnyArgs();
    kdrv_display_dirty_take_ReturnThruPtr_y1(&s_y1);
    kdrv_display_dirty_take_ReturnThruPtr_y2(&s_y2);
    kdrv_display_band_ExpectAndReturn(50U, 10U, NULL, NULL, KODE_OK);
    kdrv_display_band_IgnoreArg_rows();
    kdrv_display_band_IgnoreArg_len();
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_DENIED);
    mirror_tick();

    /* The shot goes through in between. It calls neither the dirty tracker
     * (an unexpected call fails the case) nor the credit. */
    uint32_t len = 0U;
    expect_shot(JPEG_SOI, (uint32_t)sizeof(JPEG_SOI), KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, shot_ask(&len));
    shot_release();

    /* The mirror's next frame is still the band it owed, taken from its own
     * pending range and not from the driver. */
    static uint8_t *s_jpg  = (uint8_t *)JPEG_SMALL;
    static uint32_t s_jlen = (uint32_t)sizeof(JPEG_SMALL);
    kdrv_display_band_ExpectAndReturn(50U, 10U, NULL, NULL, KODE_OK);
    kdrv_display_band_IgnoreArg_rows();
    kdrv_display_band_IgnoreArg_len();
    kdrv_display_band_ReturnThruPtr_rows(&s_rows);
    kdrv_display_band_ReturnThruPtr_len(&s_band_len);
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);
    mirror_tick();
    mirror_tick();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fake_klog_count(),
        "the mirror goes on with the frame it owed, as if no shot had run");

    mirror_stop();
    mocks_done();
}

/* ---- A worker's encode, asked of the engine's owner ------------------------
 * AO_TestDot's worker never calls the encoder: it asks AO_Display, which
 * encodes in its own step and copies the JPEG into the asker's buffer before
 * it answers. The copy is what this case pins: the asker never holds a
 * pointer into the engine's buffer, which the next mirror frame overwrites.
 * A request its asker stopped waiting for is dropped unread and unanswered. */
TEST_CASE("a worker's encode is copied into its own buffer; a stale one is dropped",
          "[display_mirror]")
{
    static uint16_t src[4];
    static uint8_t  *s_jpg  = (uint8_t *)JPEG_SMALL;
    static uint32_t  s_jlen = (uint32_t)sizeof(JPEG_SMALL);
    uint8_t          dst[sizeof(JPEG_SMALL)];
    display_jpeg_t   job;
    uint32_t         len = 0U;
    uint32_t         rc  = 0U;
    uint32_t         cur = 1U;
    uint32_t         ack = 0U;
    khal_sem_t const sem = khal_sem_create();

    mocks_init();
    display_mirror_up_once();
    TEST_ASSERT_NOT_NULL(sem);

    (void)memset(dst, 0, sizeof(dst));
    (void)memset(&job, 0, sizeof(job));
    job.src     = (const uint8_t *)(const void *)src;
    job.w       = 2U;
    job.h       = 2U;
    job.quality = 70U;
    job.dst     = dst;
    job.cap     = (uint32_t)sizeof(dst);
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);
    display_jpeg_request(&job, &len, &rc, sem, 1U, &cur, &ack);
    TEST_ASSERT_EQUAL_UINT(1U, qf_support_pump(AO_Display, 1U));
    TEST_ASSERT_TRUE_MESSAGE(khal_sem_wait_timeout(sem, 0U), "the asker is woken");
    TEST_ASSERT_EQUAL_UINT32(1U, ack);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_OK, rc);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sizeof(JPEG_SMALL), len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY_MESSAGE(JPEG_SMALL, dst, sizeof(JPEG_SMALL), "copied, not pointed at");

    /* Bigger than the asker's buffer: refused, nothing copied. */
    (void)memset(dst, 0, sizeof(dst));
    job.cap = (uint32_t)sizeof(JPEG_SMALL) - 1U;
    cur     = 2U;
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_jpeg_encode_ReturnThruPtr_out(&s_jpg);
    kdrv_jpeg_encode_ReturnThruPtr_out_len(&s_jlen);
    display_jpeg_request(&job, &len, &rc, sem, 2U, &cur, &ack);
    (void)qf_support_pump(AO_Display, 1U);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(sem, 0U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_NO_RESOURCES, rc);
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_UINT8(0U, dst[0]);

    /* The engine mid-encode for the mirror: the answer says so. */
    job.cap = (uint32_t)sizeof(dst);
    cur     = 3U;
    kdrv_jpeg_encode_ExpectAnyArgsAndReturn(KODE_DENIED);
    display_jpeg_request(&job, &len, &rc, sem, 3U, &cur, &ack);
    (void)qf_support_pump(AO_Display, 1U);
    TEST_ASSERT_TRUE(khal_sem_wait_timeout(sem, 0U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_DENIED, rc);

    /* The asker gave up on tag 4 and moved on: no encode, no answer. */
    cur = 5U;
    display_jpeg_request(&job, &len, &rc, sem, 4U, &cur, &ack);
    (void)qf_support_pump(AO_Display, 1U);
    TEST_ASSERT_FALSE_MESSAGE(khal_sem_wait_timeout(sem, 0U), "nobody is woken for a stale tag");
    TEST_ASSERT_EQUAL_UINT32(3U, ack);

    khal_sem_delete(sem);
    mocks_done();
}
