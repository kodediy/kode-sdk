/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_kfw_decode.c
 * @brief The decode session: five states, and the gate at the end of it.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "esp_app_format.h"

#include "fakes.h"
#include "kdrv_kfw.h"

/* ---- A sink that remembers what it was told -------------------------------- */

#define SINK_LOG_MAX 32
#define SINK_BUF_MAX 4096

typedef enum { SINK_BEGIN, SINK_WRITE, SINK_END, SINK_ABORT } sink_call_t;

static sink_call_t   s_log[SINK_LOG_MAX];
static unsigned      s_log_n;
static uint8_t       s_got[SINK_BUF_MAX];
static uint32_t      s_got_n;
static uint32_t      s_begin_len;
static kode_status_t s_begin_rc;
static kode_status_t s_write_rc;
static kode_status_t s_end_rc;

/** @brief Record one call, dropping the tail if a case goes mad. */
static void note(sink_call_t c)
{
    if ( SINK_LOG_MAX > s_log_n )
    {
        s_log[s_log_n] = c;
    }
    ++s_log_n;
}

static kode_status_t sink_begin(void *ctx, uint32_t img_len)
{
    (void)ctx;
    s_begin_len = img_len;
    note(SINK_BEGIN);
    return s_begin_rc;
}

static kode_status_t sink_write(void *ctx, const void *data, size_t len)
{
    (void)ctx;
    note(SINK_WRITE);
    if ( (KODE_OK == s_write_rc) && ((s_got_n + len) <= SINK_BUF_MAX) )
    {
        (void)memcpy(&s_got[s_got_n], data, len);
        s_got_n += (uint32_t)len;
    }
    return s_write_rc;
}

static kode_status_t sink_end(void *ctx)
{
    (void)ctx;
    note(SINK_END);
    return s_end_rc;
}

static void sink_abort(void *ctx)
{
    (void)ctx;
    note(SINK_ABORT);
}

static const kfw_sink_t RECORDER = {
    .begin = &sink_begin,
    .write = &sink_write,
    .end   = &sink_end,
    .abort = &sink_abort,
};

/** @brief Forget everything the sink was told, and accept everything next. */
static void sink_fresh(void)
{
    kfw_abort();                 /* whatever a previous case left open */
    s_log_n     = 0U;
    s_got_n     = 0U;
    s_begin_len = 0U;
    s_begin_rc  = KODE_OK;
    s_write_rc  = KODE_OK;
    s_end_rc    = KODE_OK;
}

/** @brief Was @p c ever called? */
static bool sink_saw(sink_call_t c)
{
    for ( unsigned i = 0U; (i < s_log_n) && (SINK_LOG_MAX > i); ++i )
    {
        if ( s_log[i] == c )
        {
            return true;
        }
    }
    return false;
}

/* ---- Building a stream ----------------------------------------------------- */

#define HDR_CRC_LEN  offsetof(kfw_hdr_t, crc)

/**
 * @brief Build a complete uncompressed container over @p body.
 * @param out   Receives header + body.
 * @param body  The image bytes.
 * @param len   How many.
 * @param wrong When true, seal a digest that does not match @p body.
 * @return Total stream length.
 */
static size_t build_stream(uint8_t *out, const uint8_t *body, uint32_t len,
                           bool wrong)
{
    kfw_hdr_t h;

    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = 1U;
    h.hdr_len     = KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_NONE;
    h.payload_len = len;
    h.image_len   = len;
    (void)snprintf(h.name, sizeof(h.name), "%s", "demo");

    kfw_sha256(body, len, h.sha256);
    if ( wrong )
    {
        h.sha256[0] ^= 0xFFU;
    }
    h.crc = kfw_crc32(&h, HDR_CRC_LEN);

    (void)memcpy(out, &h, sizeof(h));
    (void)memcpy(&out[KFW_HDR_LEN], body, len);
    return (size_t)KFW_HDR_LEN + len;
}

/* ---- Telling a container from a raw image ---------------------------------- */

/**
 * @brief Say how long a raw image's first segment is, in its segment header.
 * @param raw The image, already filled.
 * @param seg The length the segment header declares.
 */
static void raw_seg_len(uint8_t *raw, uint32_t seg)
{
    esp_image_segment_header_t h;

    (void)memset(&h, 0, sizeof(h));
    h.load_addr = 0x40000000U;
    h.data_len  = seg;
    (void)memcpy(&raw[sizeof(esp_image_header_t)], &h, sizeof(h));
}

/**
 * @brief Give a stand-in raw image a first segment that fits it.
 *
 * The decoder refuses a raw image whose first segment is longer than the
 * source, so a stand-in that is filler from end to end has to say a length
 * that fits: here, everything behind the two headers.
 *
 * @param raw The image, already filled.
 * @param len Its length, at least the two headers.
 */
static void raw_first_seg(uint8_t *raw, size_t len)
{
    raw_seg_len(raw, (uint32_t)(len - (sizeof(esp_image_header_t)
                                       + sizeof(esp_image_segment_header_t))));
}

/* The first byte settles it. An ESP image starts 0xE9 and has no header to
 * read, so the decoder must not sit waiting for 72 bytes that will never
 * describe anything. */
TEST_CASE("a raw ESP image is recognised from its first byte", "[kfw_decode]")
{
    static uint8_t raw[64];

    sink_fresh();
    (void)memset(raw, 0xAAU, sizeof(raw));
    raw[0] = 0xE9U;
    raw[12] = (uint8_t)ESP_CHIP_ID_ESP32P4;     /* chip_id, little-endian */
    raw[13] = 0U;
    raw_first_seg(raw, sizeof(raw));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, sizeof(raw), NULL, NULL));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(raw, sizeof(raw)));

    /* The sink was opened for the source's own length, since a raw image has
     * no header to state one. */
    TEST_ASSERT_TRUE(sink_saw(SINK_BEGIN));
    TEST_ASSERT_EQUAL_UINT32(sizeof(raw), s_begin_len);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());
    TEST_ASSERT_TRUE(sink_saw(SINK_END));

    /* Every byte reached the sink, the first one included. */
    TEST_ASSERT_EQUAL_UINT32(sizeof(raw), s_got_n);
    TEST_ASSERT_EQUAL_MEMORY(raw, s_got, sizeof(raw));
}

/* An image for another chip is refused from its header, before the sink is
 * opened: opening it is what places an app, and placing one may evict
 * others. The C5's slave image, picked from a card by mistake, is the case. */
TEST_CASE("a raw image built for another chip is refused before it is placed",
          "[kfw_decode]")
{
    static uint8_t raw[64];

    sink_fresh();
    (void)memset(raw, 0xAAU, sizeof(raw));
    raw[0] = 0xE9U;
    raw[12] = (uint8_t)ESP_CHIP_ID_ESP32C5;
    raw[13] = 0U;
    raw_first_seg(raw, sizeof(raw));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, sizeof(raw), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_feed(raw, sizeof(raw)));
    TEST_ASSERT_FALSE(sink_saw(SINK_BEGIN));
    kfw_abort();

    /* The same image is the C5 target's own, and goes through. */
    sink_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_C5, sizeof(raw), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(raw, sizeof(raw)));
    TEST_ASSERT_TRUE(sink_saw(SINK_BEGIN));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());
}

/* A file cut short is refused from its header too, when the cut falls inside
 * the first segment: the header says how long that segment is, and the source
 * says how long the file is. The verify at the end would say the same, after
 * the apps the image displaced were gone. */
/** @brief Bytes of the two headers a raw image starts with: its own and its first segment's. */
#define RAW_HEADS_LEN  ((uint32_t)(sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t)))

TEST_CASE("a raw image cut inside its first segment is refused before it is placed",
          "[kfw_decode]")
{
    static uint8_t raw[64];

    sink_fresh();
    (void)memset(raw, 0, sizeof(raw));
    raw[0]  = 0xE9U;
    raw[1]  = 1U;                               /* one segment */
    raw[12] = (uint8_t)ESP_CHIP_ID_ESP32P4;
    raw_seg_len(raw, (uint32_t)sizeof(raw) - RAW_HEADS_LEN + 1U);   /* one byte more than the file has */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, sizeof(raw), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_feed(raw, sizeof(raw)));
    TEST_ASSERT_FALSE(sink_saw(SINK_BEGIN));
    kfw_abort();

    /* One byte less and the segment fits: the image goes through. */
    sink_fresh();
    raw_seg_len(raw, (uint32_t)sizeof(raw) - RAW_HEADS_LEN);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, sizeof(raw), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(raw, sizeof(raw)));
    TEST_ASSERT_TRUE(sink_saw(SINK_BEGIN));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    /* A first chunk that ends before the segment's header does says nothing
     * of its length, and is the end's to judge. */
    sink_fresh();
    raw_seg_len(raw, (uint32_t)sizeof(raw) - RAW_HEADS_LEN + 1U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, sizeof(raw), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(raw, RAW_HEADS_LEN - 1U));
    TEST_ASSERT_TRUE(sink_saw(SINK_BEGIN));
    kfw_abort();

    /* A source of no stated length cannot be cut short of it. */
    sink_fresh();
    raw_seg_len(raw, 0x00100000U);                                  /* 1 MB */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(raw, sizeof(raw)));
    TEST_ASSERT_TRUE(sink_saw(SINK_BEGIN));
    kfw_abort();
}

/* A raw image carries no digest, so there is nothing to check at the end -
 * and the decoder must not invent a failure out of that. */
TEST_CASE("a raw image is not held to a digest it does not carry",
          "[kfw_decode]")
{
    static uint8_t raw[16];

    sink_fresh();
    (void)memset(raw, 0x5AU, sizeof(raw));
    raw[0] = 0xE9U;

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, sizeof(raw), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(raw, sizeof(raw)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_FALSE(sink_saw(SINK_ABORT));
}

TEST_CASE("a container is decoded and delivered whole", "[kfw_decode]")
{
    static uint8_t body[128];
    static uint8_t stream[KFW_HDR_LEN + 128];
    size_t         n;

    sink_fresh();
    for ( unsigned i = 0U; sizeof(body) > i; ++i )
    {
        body[i] = (uint8_t)(i * 7U);
    }
    n = build_stream(stream, body, sizeof(body), false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, n));

    /* The count is read here rather than after the close: it belongs to the
     * open session, and kfw_end() clears it along with everything else. What
     * it counts is the image, not the stream that carried it - so the 72-byte
     * header is not in this number, which is what makes it the right thing to
     * report progress from. */
    TEST_ASSERT_EQUAL_UINT32(sizeof(body), kfw_produced());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());
    TEST_ASSERT_EQUAL_UINT32(0U, kfw_produced());   /* the session is over */

    /* The sink was opened for the image's length, not the stream's. */
    TEST_ASSERT_EQUAL_UINT32(sizeof(body), s_begin_len);
    TEST_ASSERT_EQUAL_UINT32(sizeof(body), s_got_n);
    TEST_ASSERT_EQUAL_MEMORY(body, s_got, sizeof(body));
    TEST_ASSERT_TRUE(sink_saw(SINK_END));
    TEST_ASSERT_FALSE(sink_saw(SINK_ABORT));
}

/* ---- The header arriving in pieces ----------------------------------------- */

/* Nothing controls how a stream is chopped: a card reads 32 KB at a time, the
 * console arrives 64 bytes at a time, HTTP arrives however the network felt.
 * The header has to reassemble across any of those, and nothing may reach the
 * sink until it is complete and accepted. */
TEST_CASE("a header split one byte at a time still reassembles",
          "[kfw_decode]")
{
    static uint8_t body[32];
    static uint8_t stream[KFW_HDR_LEN + 32];
    size_t         n;

    sink_fresh();
    (void)memset(body, 0x11U, sizeof(body));
    n = build_stream(stream, body, sizeof(body), false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));

    /* All but the last byte of the header. Nothing may reach the sink yet:
     * opening it is what erases the destination, and 71 bytes do not describe
     * anything worth erasing for. */
    for ( size_t i = 0U; (KFW_HDR_LEN - 1U) > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(&stream[i], 1U));
        TEST_ASSERT_FALSE_MESSAGE(sink_saw(SINK_BEGIN),
            "the sink was opened before the header was complete");
    }

    /* The seventy-second byte is what settles it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(&stream[KFW_HDR_LEN - 1U],
                                            n - (KFW_HDR_LEN - 1U)));
    TEST_ASSERT_TRUE(sink_saw(SINK_BEGIN));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());
    TEST_ASSERT_EQUAL_UINT32(sizeof(body), s_got_n);
}

/* Every split point, not just the interesting-looking ones: the take/room
 * arithmetic that copies into the header buffer is where an off-by-one would
 * live, and it would only show at one particular boundary. */
TEST_CASE("a header split at every boundary reassembles the same",
          "[kfw_decode]")
{
    static uint8_t body[16];
    static uint8_t stream[KFW_HDR_LEN + 16];
    size_t         n;

    (void)memset(body, 0x22U, sizeof(body));

    for ( size_t cut = 1U; KFW_HDR_LEN > cut; ++cut )
    {
        sink_fresh();
        n = build_stream(stream, body, sizeof(body), false);

        TEST_ASSERT_EQUAL_INT(KODE_OK,
            kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
        TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, cut));
        TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(&stream[cut], n - cut));
        TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

        TEST_ASSERT_EQUAL_UINT32(sizeof(body), s_got_n);
        TEST_ASSERT_EQUAL_MEMORY(body, s_got, sizeof(body));
    }
}

/* ---- The gate at the end --------------------------------------------------- */

/* The case this file exists for. The image is written as it arrives, so when
 * the digest does not match there is a wrong image already in the
 * destination - and the only correct move is to tell the sink to abandon it.
 * Calling end() would commit a corrupted image to a slot the board will boot. */
TEST_CASE("an image that fails its digest is abandoned, never committed",
          "[kfw_decode]")
{
    static uint8_t body[64];
    static uint8_t stream[KFW_HDR_LEN + 64];
    size_t         n;

    sink_fresh();
    (void)memset(body, 0x33U, sizeof(body));
    n = build_stream(stream, body, sizeof(body), true);   /* wrong digest */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, n));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_end());

    TEST_ASSERT_TRUE_MESSAGE(sink_saw(SINK_ABORT),
        "the destination was left holding a corrupted image");
    TEST_ASSERT_FALSE_MESSAGE(sink_saw(SINK_END),
        "a corrupted image was committed");
}

/* One flipped byte in the payload is the ordinary corruption - a bad sector, a
 * truncated download that still had the right length - and it has to be caught
 * by the digest rather than by luck. */
TEST_CASE("a single flipped payload byte is caught by the digest",
          "[kfw_decode]")
{
    static uint8_t body[64];
    static uint8_t stream[KFW_HDR_LEN + 64];
    size_t         n;

    sink_fresh();
    (void)memset(body, 0x44U, sizeof(body));
    n = build_stream(stream, body, sizeof(body), false);

    stream[KFW_HDR_LEN + 10U] ^= 0x01U;   /* one bit, in the payload */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_end());

    TEST_ASSERT_TRUE(sink_saw(SINK_ABORT));
    TEST_ASSERT_FALSE(sink_saw(SINK_END));
}

/* A stream that stopped early has the right digest for nothing: the byte
 * count catches it first, and it is caught rather than committed. */
TEST_CASE("a stream that ends early is abandoned", "[kfw_decode]")
{
    static uint8_t body[128];
    static uint8_t stream[KFW_HDR_LEN + 128];
    size_t         n;

    sink_fresh();
    (void)memset(body, 0x55U, sizeof(body));
    n = build_stream(stream, body, sizeof(body), false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));

    /* Everything but the last sixteen bytes. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, n - 16U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_end());

    TEST_ASSERT_TRUE(sink_saw(SINK_ABORT));
    TEST_ASSERT_FALSE(sink_saw(SINK_END));
}

/* The stream ended inside the header: there is not even a description, let
 * alone an image. */
TEST_CASE("a stream that ends inside the header is an error", "[kfw_decode]")
{
    static uint8_t body[16];
    static uint8_t stream[KFW_HDR_LEN + 16];

    sink_fresh();
    (void)memset(body, 0x66U, sizeof(body));
    (void)build_stream(stream, body, sizeof(body), false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, 40U));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_end());
    TEST_ASSERT_FALSE(sink_saw(SINK_BEGIN));
}

/* ---- Refusing before a byte is written ------------------------------------- */

/* The command line decides where an image goes; the file only agrees or does
 * not. A container declaring the OS slot when the operator asked for an app
 * must be refused the instant its header completes - before the sink is
 * opened, because opening it is what erases the destination. */
TEST_CASE("a container for another destination is refused before the erase",
          "[kfw_decode]")
{
    static uint8_t body[32];
    static uint8_t stream[KFW_HDR_LEN + 32];
    size_t         n;

    sink_fresh();
    (void)memset(body, 0x77U, sizeof(body));
    n = build_stream(stream, body, sizeof(body), false);   /* declares APP */

    /* The operator asked for the OS slot. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_OS, 0U, NULL, NULL));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_feed(stream, n));

    TEST_ASSERT_FALSE_MESSAGE(sink_saw(SINK_BEGIN),
        "the destination was opened for an image that was never going there");
}

/* A sink that refuses to open kills the session rather than letting the
 * stream run on into nothing. */
TEST_CASE("a destination that will not open ends the session", "[kfw_decode]")
{
    static uint8_t body[32];
    static uint8_t stream[KFW_HDR_LEN + 32];
    size_t         n;

    sink_fresh();
    s_begin_rc = KODE_ERROR;
    (void)memset(body, 0x88U, sizeof(body));
    n = build_stream(stream, body, sizeof(body), false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_feed(stream, n));

    /* And it stays dead: a later chunk is refused rather than acted on. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_feed(stream, 8U));
    TEST_ASSERT_FALSE(sink_saw(SINK_WRITE));
}

/* ---- The session's own rules ----------------------------------------------- */

TEST_CASE("a second session is refused while one is open", "[kfw_decode]")
{
    sink_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_TRUE(kfw_busy());

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_OS, 0U, NULL, NULL));
}

TEST_CASE("feeding with no session open is refused", "[kfw_decode]")
{
    static const uint8_t byte = 0xE9U;

    sink_fresh();
    TEST_ASSERT_FALSE(kfw_busy());

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfw_feed(&byte, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kfw_end());
}

/* Abandoning is safe wherever it happens, including before the sink was ever
 * opened - which is the state a transfer refused at the header dies in. */
TEST_CASE("abandoning is safe at any point, and frees the session",
          "[kfw_decode]")
{
    sink_fresh();

    kfw_abort();   /* nothing open at all */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    kfw_abort();
    TEST_ASSERT_FALSE(kfw_busy());

    /* And the next session starts clean. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    kfw_abort();
}

/* AO_AppMgr and AO_Radio share the one decoder, and either one's cleanup may
 * run while the other holds it: an abort on someone else's behalf must leave
 * the open session and its sink alone. */
TEST_CASE("an abort by another owner leaves the session alone",
          "[kfw_decode]")
{
    static int mine;
    static int theirs;

    sink_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, &mine, KFW_TARGET_APP, 0U, NULL, NULL));

    kfw_abort_owned(&theirs);
    TEST_ASSERT_TRUE_MESSAGE(kfw_busy(), "another owner ended this session");
    TEST_ASSERT_FALSE(sink_saw(SINK_ABORT));

    kfw_abort_owned(&mine);
    TEST_ASSERT_FALSE(kfw_busy());
    TEST_ASSERT_TRUE(sink_saw(SINK_ABORT));

    kfw_abort_owned(&mine);   /* nothing open: a no-op */
    TEST_ASSERT_FALSE(kfw_busy());
}

TEST_CASE("a session with no sink is refused", "[kfw_decode]")
{
    sink_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kfw_begin(NULL, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_FALSE(kfw_busy());
}

/* Feeding nothing is not an error - a source that returns zero bytes is
 * waiting, not finished. */
TEST_CASE("a chunk of no bytes is accepted and changes nothing",
          "[kfw_decode]")
{
    sink_fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, s_log_n);

    kfw_abort();
}

/* ---- The app-data block ----------------------------------------------------
 * A container for an app may carry app.json and an icon between its header
 * and its payload. The decoder checks the block, keeps it in PSRAM and hands
 * it to the session's owner after kfw_end(); it never acts on it. A block that
 * fails a check is dropped and the image installs as if it had none.
 */

#define AD_BODY_LEN   128U
#define AD_STREAM_MAX (KFW_HDR_LEN + KFW_AD_HDR_LEN + KFW_AD_JSON_MAX + KFW_AD_ICON_LEN + AD_BODY_LEN)
#define AD_ICON_FILL  0x5AU
#define AD_JSON       "{\"kode\":\"1\",\"name\":\"Demo\"}"

static uint8_t s_ad_stream[AD_STREAM_MAX];
static uint8_t s_ad_body[AD_BODY_LEN];
static uint8_t s_ad_icon[KFW_AD_ICON_LEN];

/**
 * @brief Build a container for an app with an app-data block before the payload.
 * @param out       Receives header + block + body.
 * @param body      The image bytes.
 * @param len       How many.
 * @param json      The block's app.json bytes.
 * @param json_len  How many.
 * @param icon      The block's icon bytes, or NULL.
 * @param icon_len  How many.
 * @param bad_crc   When true, seal a block CRC that does not match.
 * @return Total stream length.
 */
static size_t build_stream_ad(uint8_t *out, const uint8_t *body, uint32_t len,
                              const char *json, uint16_t json_len,
                              const uint8_t *icon, uint16_t icon_len,
                              bool bad_crc)
{
    kfw_hdr_t    h;
    kfw_ad_hdr_t ad;
    size_t       at = KFW_HDR_LEN;

    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = 1U;
    h.hdr_len     = KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_NONE;
    h.flags       = KFW_FLAG_APPDATA;
    h.payload_len = len;
    h.image_len   = len;
    (void)snprintf(h.name, sizeof(h.name), "%s", "demo");
    kfw_sha256(body, len, h.sha256);
    h.crc = kfw_crc32(&h, HDR_CRC_LEN);
    (void)memcpy(out, &h, sizeof(h));

    (void)memset(&ad, 0, sizeof(ad));
    ad.magic    = KFW_AD_MAGIC;
    ad.json_len = json_len;
    ad.icon_len = icon_len;

    /* The CRC runs over json then icon, so lay them down first. */
    (void)memcpy(&out[at + KFW_AD_HDR_LEN], json, json_len);
    if ( 0U < icon_len )
    {
        (void)memcpy(&out[at + KFW_AD_HDR_LEN + json_len], icon, icon_len);
    }
    ad.crc = kfw_crc32(&out[at + KFW_AD_HDR_LEN], (size_t)json_len + icon_len);
    if ( bad_crc )
    {
        ad.crc ^= 1U;
    }
    (void)memcpy(&out[at], &ad, sizeof(ad));
    at += KFW_AD_HDR_LEN + json_len + icon_len;

    (void)memcpy(&out[at], body, len);
    return at + len;
}

/** @brief Fresh sink, fresh PSRAM counters, a body and an icon to carry. */
static void ad_fresh(void)
{
    sink_fresh();
    fake_psram_reset();
    for ( unsigned i = 0U; sizeof(s_ad_body) > i; ++i )
    {
        s_ad_body[i] = (uint8_t)(i * 5U);
    }
    (void)memset(s_ad_icon, AD_ICON_FILL, sizeof(s_ad_icon));
}

/** @brief Assert @p ad carries exactly the demo block, then release it. */
static void ad_expect_demo(kfw_appdata_t *ad, uint16_t icon_len)
{
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BLOCK, (int)ad->kind);
    TEST_ASSERT_NOT_NULL(ad->buf);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)strlen(AD_JSON), ad->json_len);
    TEST_ASSERT_EQUAL_UINT16(icon_len, ad->icon_len);
    TEST_ASSERT_EQUAL_MEMORY(AD_JSON, ad->buf, ad->json_len);
    if ( 0U < icon_len )
    {
        TEST_ASSERT_EQUAL_MEMORY(s_ad_icon, &ad->buf[ad->json_len], icon_len);
    }
    kfw_appdata_release(ad);
    TEST_ASSERT_NULL(ad->buf);
    TEST_ASSERT_EQUAL_UINT(fake_psram_alloc_count(), fake_psram_free_count());
}

/** @brief Assert the session handed over nothing, and nothing is held. */
static void ad_expect_none(void)
{
    kfw_appdata_t ad;

    kfw_appdata_take(&ad);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_NONE, (int)ad.kind);
    TEST_ASSERT_NULL(ad.buf);
    TEST_ASSERT_EQUAL_UINT(fake_psram_alloc_count(), fake_psram_free_count());
}

/** @brief Assert the session dropped its block: nothing to take, and the folder is not its to touch. */
static void ad_expect_dropped(void)
{
    kfw_appdata_t ad;

    kfw_appdata_take(&ad);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KFW_AD_DROPPED, (int)ad.kind,
        "a dropped block was reported as no block, and the folder would go");
    TEST_ASSERT_NULL(ad.buf);
    TEST_ASSERT_EQUAL_UINT(fake_psram_alloc_count(), fake_psram_free_count());
}

TEST_CASE("a container with a block installs and hands the block over",
          "[kfw_decode]")
{
    kfw_appdata_t ad;
    size_t        n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    TEST_ASSERT_EQUAL_MEMORY(s_ad_body, s_got, sizeof(s_ad_body));
    TEST_ASSERT_TRUE(sink_saw(SINK_END));

    kfw_appdata_take(&ad);
    ad_expect_demo(&ad, (uint16_t)KFW_AD_ICON_LEN);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BLOCK, (int)ad.kind);   /* kind stays */
}

TEST_CASE("a container with a 64 px icon in its block hands it over as it is",
          "[kfw_decode]")
{
    kfw_appdata_t ad;
    size_t        n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN_HALF, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    kfw_appdata_take(&ad);
    ad_expect_demo(&ad, (uint16_t)KFW_AD_ICON_LEN_HALF);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BLOCK, (int)ad.kind);
}

TEST_CASE("the block arrives one byte at a time", "[kfw_decode]")
{
    kfw_appdata_t ad;
    size_t        n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    for ( size_t i = 0U; i < n; ++i )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(&s_ad_stream[i], 1U));
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    TEST_ASSERT_EQUAL_MEMORY(s_ad_body, s_got, sizeof(s_ad_body));
    kfw_appdata_take(&ad);
    ad_expect_demo(&ad, (uint16_t)KFW_AD_ICON_LEN);
}

TEST_CASE("a block without an icon", "[kfw_decode]")
{
    kfw_appdata_t ad;
    size_t        n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), NULL, 0U, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    kfw_appdata_take(&ad);
    TEST_ASSERT_EQUAL_UINT16(0U, ad.icon_len);
    ad_expect_demo(&ad, 0U);
}

TEST_CASE("a block with a bad crc is dropped and the image still installs",
          "[kfw_decode]")
{
    size_t n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    TEST_ASSERT_EQUAL_MEMORY(s_ad_body, s_got, sizeof(s_ad_body));
    ad_expect_dropped();
}

TEST_CASE("a block whose json is not kode 1 is dropped", "[kfw_decode]")
{
    static const char json[] = "{\"kode\":\"2\"}";
    size_t            n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), json,
                        (uint16_t)strlen(json), NULL, 0U, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    ad_expect_dropped();
}

/* The size is read before anything is allocated, and a block that cannot be
 * believed is stepped over by the length it states, not read. */
TEST_CASE("a block with icon_len 100 is dropped without allocating",
          "[kfw_decode]")
{
    static const uint8_t icon[100] = { 0U };
    size_t               n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), icon, (uint16_t)sizeof(icon),
                        false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    TEST_ASSERT_EQUAL_MEMORY(s_ad_body, s_got, sizeof(s_ad_body));
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
    ad_expect_dropped();
}

TEST_CASE("a block with an icon between the two sizes is dropped without allocating",
          "[kfw_decode]")
{
    static const uint8_t icon[KFW_AD_ICON_LEN_HALF + 1U] = { 0U };
    size_t               n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), icon, (uint16_t)sizeof(icon),
                        false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
    ad_expect_dropped();
}

TEST_CASE("a block that cannot get PSRAM is dropped and the image installs",
          "[kfw_decode]")
{
    size_t n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN, false);

    fake_psram_refuse(1U);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    TEST_ASSERT_EQUAL_UINT32(sizeof(s_ad_body), s_got_n);
    TEST_ASSERT_EQUAL_MEMORY(s_ad_body, s_got, sizeof(s_ad_body));
    ad_expect_dropped();
}

TEST_CASE("a block that does not start with its magic ends the transfer",
          "[kfw_decode]")
{
    size_t n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), NULL, 0U, false);
    s_ad_stream[KFW_HDR_LEN] ^= 0xFFU;      /* the block's first byte */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_NOT_EQUAL(KODE_OK, kfw_end());
    ad_expect_none();
}

TEST_CASE("a container without the flag reports none", "[kfw_decode]")
{
    static uint8_t stream[KFW_HDR_LEN + AD_BODY_LEN];
    size_t         n;

    ad_fresh();
    n = build_stream(stream, s_ad_body, sizeof(s_ad_body), false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());
    ad_expect_none();
}

TEST_CASE("a raw image reports raw", "[kfw_decode]")
{
    static uint8_t raw[64];
    kfw_appdata_t  ad;

    ad_fresh();
    (void)memset(raw, 0x5AU, sizeof(raw));
    raw[0]  = 0xE9U;
    raw[12] = (uint8_t)ESP_CHIP_ID_ESP32P4;     /* chip_id, little-endian */
    raw[13] = 0U;
    raw_first_seg(raw, sizeof(raw));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, sizeof(raw), NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(raw, sizeof(raw)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());

    kfw_appdata_take(&ad);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_RAW, (int)ad.kind);
    TEST_ASSERT_NULL(ad.buf);
}

TEST_CASE("a failed digest leaves nothing to take", "[kfw_decode]")
{
    size_t n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN, false);
    s_ad_stream[n - 1U] ^= 0xFFU;           /* the image no longer matches */

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_NOT_EQUAL(KODE_OK, kfw_end());
    ad_expect_none();
}

TEST_CASE("an abandoned session leaves nothing to take", "[kfw_decode]")
{
    size_t n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n / 2U));
    kfw_abort();
    ad_expect_none();
}

TEST_CASE("a stream that ends inside the block is refused", "[kfw_decode]")
{
    size_t n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), NULL, 0U, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, KFW_HDR_LEN + 20U));
    TEST_ASSERT_NOT_EQUAL(KODE_OK, kfw_end());
    (void)n;
    ad_expect_none();
}

TEST_CASE("kfw_begin frees a block nobody took", "[kfw_decode]")
{
    static uint8_t stream[KFW_HDR_LEN + AD_BODY_LEN];
    size_t         n;

    ad_fresh();
    n = build_stream_ad(s_ad_stream, s_ad_body, sizeof(s_ad_body), AD_JSON,
                        (uint16_t)strlen(AD_JSON), s_ad_icon,
                        (uint16_t)KFW_AD_ICON_LEN, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(s_ad_stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());
    TEST_ASSERT_EQUAL_UINT(1U, fake_psram_alloc_count());
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_free_count());   /* still held */

    /* A second session, with no block: the first one's is freed at begin. */
    n = build_stream(stream, s_ad_body, sizeof(s_ad_body), false);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kfw_begin(&RECORDER, NULL, KFW_TARGET_APP, 0U, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(1U, fake_psram_free_count());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_feed(stream, n));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_end());
    ad_expect_none();
}

/* ---- The block's own check, with no session ---------------------------------- */

/** @brief Lay a block's header and body for kfw_ad_check(). */
static void ad_block(kfw_ad_hdr_t *h, uint8_t *body, const char *json,
                     uint16_t json_len, uint16_t icon_len)
{
    (void)memset(body, AD_ICON_FILL, (size_t)json_len + icon_len);
    (void)memcpy(body, json, json_len);
    h->magic    = KFW_AD_MAGIC;
    h->json_len = json_len;
    h->icon_len = icon_len;
    h->crc      = kfw_crc32(body, (size_t)json_len + icon_len);
}

TEST_CASE("kfw_ad_check judges a block with no session", "[kfw_decode]")
{
    static uint8_t body[KFW_AD_JSON_MAX + KFW_AD_ICON_LEN];
    uint16_t const jl = (uint16_t)strlen(AD_JSON);
    kfw_ad_hdr_t   h;

    ad_block(&h, body, AD_JSON, jl, (uint16_t)KFW_AD_ICON_LEN);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_OK, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, (uint16_t)KFW_AD_ICON_LEN_HALF);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_OK, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_OK, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    h.magic ^= 1U;
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_MAGIC, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    h.json_len = 0U;
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_SIZE, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    h.json_len = (uint16_t)(KFW_AD_JSON_MAX + 1U);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_SIZE, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    h.icon_len = 1U;
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_SIZE, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    h.icon_len = (uint16_t)(KFW_AD_ICON_LEN_HALF + 1U);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_SIZE, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    h.icon_len = (uint16_t)(KFW_AD_ICON_LEN - 1U);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_SIZE, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, AD_JSON, jl, 0U);
    h.crc ^= 1U;
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_CRC, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, "{\"kode\":", 8U, 0U);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_SYNTAX, (int)kfw_ad_check(&h, body));

    ad_block(&h, body, "{\"kode\":\"2\"}", 12U, 0U);
    TEST_ASSERT_EQUAL_INT((int)KFW_AD_BAD_REVISION, (int)kfw_ad_check(&h, body));
}

TEST_CASE("every block status has a name", "[kfw_decode]")
{
    TEST_ASSERT_EQUAL_STRING("ok",       kfw_ad_status_name(KFW_AD_OK));
    TEST_ASSERT_EQUAL_STRING("magic",    kfw_ad_status_name(KFW_AD_BAD_MAGIC));
    TEST_ASSERT_EQUAL_STRING("size",     kfw_ad_status_name(KFW_AD_BAD_SIZE));
    TEST_ASSERT_EQUAL_STRING("crc",      kfw_ad_status_name(KFW_AD_BAD_CRC));
    TEST_ASSERT_EQUAL_STRING("syntax",   kfw_ad_status_name(KFW_AD_BAD_SYNTAX));
    TEST_ASSERT_EQUAL_STRING("revision", kfw_ad_status_name(KFW_AD_BAD_REVISION));
}
