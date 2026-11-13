/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_frames.c
 * @brief The frames service's P4 end: answers read, batches decoded, the ring bounded.
 *
 * The real driver, with no mock.
 */
#include <string.h>

#include "unity.h"
#include "fakes.h"
#include "kdrv_frames.h"
#include "kode_wire.h"

/** @brief Largest payload a case builds: a full batch. */
#define PAYLOAD_MAX  (sizeof(kode_sniff_batch_t) \
                      + ((size_t)KODE_SNIFF_BATCH_MAX * sizeof(kode_sniff_rec_t)))

/** @brief Batches that overfill the ring: nine of eight records against 64 places. */
#define OVERFILL_BATCHES  9U

/** @brief Build a batch payload; the records' bytes are seeded from seed. */
static size_t batch(uint8_t *out, uint8_t channel, uint8_t count, uint8_t seed)
{
    kode_sniff_batch_t h = { channel, count, -40, 1U, 10U, 3U, 7U };
    (void)memcpy(out, &h, sizeof(h));
    for ( uint8_t i = 0U; count > i; i++ )
    {
        kode_sniff_rec_t r;
        (void)memset(&r, 0, sizeof(r));
        r.rssi = (int8_t)(-50 - (int)i);
        r.fc[0] = seed;
        r.fc[1] = i;
        r.addr1[0] = (uint8_t)(0x10U + i);
        r.addr2[5] = (uint8_t)(0x20U + i);
        r.addr3[2] = (uint8_t)(0x30U + i);
        r.len = (uint16_t)(100U + i);
        r.head[0] = 0xA0U;
        r.head[KODE_SNIFF_HEAD_LEN - 1U] = i;
        (void)memcpy(&out[sizeof(h) + ((size_t)i * sizeof(r))], &r, sizeof(r));
    }
    return sizeof(h) + ((size_t)count * sizeof(kode_sniff_rec_t));
}

/** @brief Drain the frame ring and count what came out. */
static uint32_t drain(void)
{
    kdrv_frame_t f;
    uint32_t n = 0U;
    while ( kdrv_frames_next(&f) )
    {
        n++;
    }
    return n;
}

TEST_CASE("an OK answer reads as a state", "[kdrv_frames]")
{
    kode_sniff_state_t const s = { 1U, 6U, 0U, KODE_SNIFF_WHY_NONE };
    kdrv_frames_state_t st;

    kdrv_frames_answer(KODE_ST_OK, (const uint8_t *)&s, sizeof(s), &st);
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_TRUE(st.on);
    TEST_ASSERT_EQUAL_UINT8(6U, st.channel);
    TEST_ASSERT_FALSE(st.joined);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_WHY_NONE, st.why);
}

TEST_CASE("a refused answer keeps its why", "[kdrv_frames]")
{
    kode_sniff_state_t const s = { 0U, 2U, 1U, KODE_SNIFF_WHY_JOINED };
    kdrv_frames_state_t st;

    kdrv_frames_answer(KODE_ST_REFUSED, (const uint8_t *)&s, sizeof(s), &st);
    TEST_ASSERT_TRUE(st.present);
    TEST_ASSERT_FALSE(st.on);
    TEST_ASSERT_EQUAL_UINT8(2U, st.channel);
    TEST_ASSERT_TRUE(st.joined);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_WHY_JOINED, st.why);
}

TEST_CASE("a bad request means the C5 has no such service", "[kdrv_frames]")
{
    kdrv_frames_state_t st;

    kdrv_frames_answer(KODE_ST_BAD_REQUEST, NULL, 0U, &st);
    TEST_ASSERT_FALSE(st.present);
    TEST_ASSERT_FALSE(st.on);
}

TEST_CASE("a short answer means the C5 has no such service", "[kdrv_frames]")
{
    kode_sniff_state_t const s = { 1U, 6U, 0U, KODE_SNIFF_WHY_NONE };
    kdrv_frames_state_t st;

    kdrv_frames_answer(KODE_ST_OK, (const uint8_t *)&s, sizeof(s) - 1U, &st);
    TEST_ASSERT_FALSE(st.present);
}

TEST_CASE("a batch becomes a tick and records", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX];
    kdrv_frames_tick_t t;
    kdrv_frame_t f;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, batch(p, 6U, 2U, 0x55U)));

    TEST_ASSERT_TRUE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT8(6U, t.channel);
    TEST_ASSERT_TRUE(t.joined);
    TEST_ASSERT_EQUAL_INT8(-40, t.rssi_max);
    TEST_ASSERT_EQUAL_UINT32(10U, t.heard);
    TEST_ASSERT_EQUAL_UINT16(3U, t.mgmt);
    TEST_ASSERT_EQUAL_UINT16(7U, t.data);

    for ( uint8_t i = 0U; 2U > i; i++ )
    {
        TEST_ASSERT_TRUE(kdrv_frames_next(&f));
        TEST_ASSERT_EQUAL_UINT8(6U, f.channel);
        TEST_ASSERT_EQUAL_INT8((int8_t)(-50 - (int)i), f.rssi);
        TEST_ASSERT_EQUAL_UINT8(0x55U, f.fc[0]);
        TEST_ASSERT_EQUAL_UINT8(i, f.fc[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x10U + i), f.addr1[0]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x20U + i), f.addr2[5]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x30U + i), f.addr3[2]);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(100U + i), f.len);
        TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_HEAD_LEN, f.head_len);
        TEST_ASSERT_EQUAL_UINT8(0xA0U, f.head[0]);
        TEST_ASSERT_EQUAL_UINT8(i, f.head[KODE_SNIFF_HEAD_LEN - 1U]);
    }
    TEST_ASSERT_FALSE(kdrv_frames_next(&f));
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
}

TEST_CASE("a newer tick replaces an untaken one", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX];
    kdrv_frames_tick_t t;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, batch(p, 1U, 1U, 0U)));
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, batch(p, 11U, 1U, 0U)));

    TEST_ASSERT_TRUE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT8(11U, t.channel);
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(2U, drain());
}

/* A batch whose records are not whole is refused, and the records it claims
 * are counted lost: when the two chips disagree on a record's size, the drop
 * count is where it shows. Its header is still the tick, which is how the
 * count reaches a listener when every batch is refused. Shorter than a
 * header, it claims nothing. */
TEST_CASE("a batch shorter than its records is refused, counted lost, and still a tick", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX];
    size_t const n = batch(p, 6U, 2U, 0U);
    kdrv_frames_tick_t t;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, n - 1U));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U, kdrv_frames_dropped(), "the batch's two records were not counted");
    TEST_ASSERT_TRUE_MESSAGE(kdrv_frames_tick_take(&t), "a refused batch left no tick");
    TEST_ASSERT_EQUAL_UINT8(6U, t.channel);
    TEST_ASSERT_EQUAL_UINT32(10U, t.heard);
    TEST_ASSERT_FALSE(kdrv_frames_feed(p, sizeof(kode_sniff_batch_t) - 1U));
    TEST_ASSERT_FALSE(kdrv_frames_feed(NULL, n));
    TEST_ASSERT_EQUAL_UINT32(2U, kdrv_frames_dropped());
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(0U, drain());
}

TEST_CASE("a count over the maximum is refused and counted lost", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX + sizeof(kode_sniff_rec_t)];
    kdrv_frames_tick_t t;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    (void)memset(p, 0, sizeof(p));
    kode_sniff_batch_t const h = { 6U, (uint8_t)(KODE_SNIFF_BATCH_MAX + 1U), -40, 0U, 1U, 1U, 0U };
    (void)memcpy(p, &h, sizeof(h));
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, sizeof(p)));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KODE_SNIFF_BATCH_MAX + 1U, kdrv_frames_dropped());
    TEST_ASSERT_TRUE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(0U, drain());
}

TEST_CASE("a full ring counts what it refuses", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    for ( uint8_t i = 0U; OVERFILL_BATCHES > i; i++ )
    {
        TEST_ASSERT_TRUE(kdrv_frames_feed(p, batch(p, 6U, KODE_SNIFF_BATCH_MAX, i)));
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(OVERFILL_BATCHES * KODE_SNIFF_BATCH_MAX)
                             - KDRV_FRAMES_RING_LEN, kdrv_frames_dropped());
    TEST_ASSERT_EQUAL_UINT32(KDRV_FRAMES_RING_LEN, drain());
}

TEST_CASE("forget empties the ring, the tick and the count", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX];
    kdrv_frames_tick_t t;
    kdrv_frame_t f;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    for ( uint8_t i = 0U; OVERFILL_BATCHES > i; i++ )
    {
        TEST_ASSERT_TRUE(kdrv_frames_feed(p, batch(p, 6U, KODE_SNIFF_BATCH_MAX, i)));
    }
    kdrv_frames_forget();
    TEST_ASSERT_FALSE(kdrv_frames_next(&f));
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_frames_dropped());
}

/* ---- What a frame's head is for ----------------------------------------------
 * A beacon's body starts with twelve fixed bytes - the timestamp, the interval
 * and the capabilities - and its first element is the network's name, up to
 * 32 bytes behind a two-byte header. The head is long enough for all of it.
 */

#define BEACON_FIXED_LEN   12U    /**< Timestamp, interval and capabilities.        */
#define BEACON_CAP_AT      10U    /**< Where the capabilities start in the body.    */
#define BEACON_CAP_PRIVACY 0x10U  /**< Their privacy bit, in the first byte.        */
#define BEACON_SSID_MAX    32U    /**< The longest network name.                    */
#define ELEMENT_HEADER_LEN 2U     /**< An element's id and its length.              */

_Static_assert(KODE_SNIFF_HEAD_LEN >= (BEACON_FIXED_LEN + ELEMENT_HEADER_LEN + BEACON_SSID_MAX),
               "a head holds a beacon's fixed part and the longest name");

TEST_CASE("a beacon's privacy bit and its longest name reach the ring whole",
          "[kdrv_frames]")
{
    uint8_t            p[PAYLOAD_MAX];
    kode_sniff_batch_t h = { 11U, 1U, -40, 0U, 1U, 1U, 0U };
    kode_sniff_rec_t   r;
    kdrv_frame_t       f;

    (void)memset(&r, 0, sizeof(r));
    r.fc[0] = 0x80U;                                   /* a beacon */
    (void)memset(r.addr3, 0xB5, sizeof(r.addr3));      /* the BSSID */
    r.head[BEACON_CAP_AT]        = BEACON_CAP_PRIVACY;
    r.head[BEACON_FIXED_LEN]     = 0U;                 /* the SSID element */
    r.head[BEACON_FIXED_LEN + 1U] = (uint8_t)BEACON_SSID_MAX;
    (void)memset(&r.head[BEACON_FIXED_LEN + ELEMENT_HEADER_LEN], 'n', BEACON_SSID_MAX);
    (void)memcpy(p, &h, sizeof(h));
    (void)memcpy(&p[sizeof(h)], &r, sizeof(r));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, sizeof(h) + sizeof(r)));
    TEST_ASSERT_TRUE(kdrv_frames_next(&f));
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_HEAD_LEN, f.head_len);
    TEST_ASSERT_EQUAL_MEMORY(r.head, f.head, sizeof(r.head));
    TEST_ASSERT_EQUAL_MEMORY(r.addr3, f.addr3, sizeof(r.addr3));
}

/* ---- A C5 from before KODE_SNIFF_WIDE, with this P4 -----------------------------
 * That C5 ignores the flag this P4 asks with and sends the short record:
 * eight bytes of head and no third address. Its records are told apart by
 * their size, which the batch's length and its count give, and its frames
 * are taken for what they carry. The layout is written out here on its own,
 * so the case also checks kode_wire.h's.
 */

#define SHORT_HEAD_LEN  8U   /**< Bytes of head the record had.  */

/** @brief The record as it was: 25 bytes. */
typedef struct __attribute__((packed)) {
    int8_t   rssi;
    uint8_t  fc[2];
    uint8_t  addr1[6];
    uint8_t  addr2[6];
    uint16_t len;
    uint8_t  head[SHORT_HEAD_LEN];
} short_rec_t;
_Static_assert(sizeof(short_rec_t) == sizeof(kode_sniff_rec_short_t), "the wire's short record");

/** @brief Build a batch of @p count records of the size the record had. */
static size_t batch_short(uint8_t *out, uint8_t channel, uint8_t count)
{
    kode_sniff_batch_t h = { channel, count, -40, 1U, 10U, 3U, 7U };

    (void)memcpy(out, &h, sizeof(h));
    for ( uint8_t i = 0U; count > i; i++ )
    {
        short_rec_t r;

        (void)memset(&r, 0, sizeof(r));
        r.rssi     = (int8_t)(-60 - (int)i);
        r.fc[0]    = 0x80U;
        r.addr1[0] = (uint8_t)(0x10U + i);
        r.addr2[5] = (uint8_t)(0x20U + i);
        r.len      = (uint16_t)(200U + i);
        (void)memset(r.head, (int)(0xC0U + i), sizeof(r.head));
        (void)memcpy(&out[sizeof(h) + ((size_t)i * sizeof(r))], &r, sizeof(r));
    }
    return sizeof(h) + ((size_t)count * sizeof(short_rec_t));
}

TEST_CASE("a C5 that ignores the wide flag is heard: eight bytes of head and no third address",
          "[kdrv_frames]")
{
    uint8_t            p[PAYLOAD_MAX];
    uint8_t const      zeros[KODE_SNIFF_HEAD_LEN] = { 0U };
    kdrv_frames_tick_t t;
    kdrv_frame_t       f;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, batch_short(p, 6U, 2U)));
    TEST_ASSERT_TRUE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(10U, t.heard);

    for ( uint8_t i = 0U; 2U > i; i++ )
    {
        TEST_ASSERT_TRUE(kdrv_frames_next(&f));
        TEST_ASSERT_EQUAL_INT8((int8_t)(-60 - (int)i), f.rssi);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x10U + i), f.addr1[0]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x20U + i), f.addr2[5]);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(200U + i), f.len);
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(SHORT_HEAD_LEN, f.head_len,
                                        "the frame says how much of its head is real");
        TEST_ASSERT_EACH_EQUAL_UINT8((uint8_t)(0xC0U + i), f.head, SHORT_HEAD_LEN);
        TEST_ASSERT_EQUAL_MEMORY(zeros, &f.head[SHORT_HEAD_LEN],
                                 KODE_SNIFF_HEAD_LEN - SHORT_HEAD_LEN);
        TEST_ASSERT_EQUAL_MEMORY(zeros, f.addr3, sizeof(f.addr3));
    }
    TEST_ASSERT_FALSE(kdrv_frames_next(&f));
}

TEST_CASE("a batch whose records are of neither size is refused, counted lost, and still a tick",
          "[kdrv_frames]")
{
    uint8_t            p[PAYLOAD_MAX];
    kode_sniff_batch_t h = { 6U, 2U, -40, 0U, 1U, 1U, 0U };
    kdrv_frames_tick_t t;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    (void)memset(p, 0, sizeof(p));
    (void)memcpy(p, &h, sizeof(h));
    /* Two records of 40 bytes: neither what the record was nor what it is. */
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, sizeof(h) + (2U * 40U)));
    /* Bytes left over after two whole records. */
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, sizeof(h) + (2U * sizeof(kode_sniff_rec_t)) + 1U));
    TEST_ASSERT_EQUAL_UINT32(4U, kdrv_frames_dropped());
    TEST_ASSERT_TRUE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(0U, drain());
}

/* What follows a data frame's header is a third party's traffic: the P4
 * keeps none of it, whichever record a C5 sends. */
TEST_CASE("a data frame keeps no head, from a record of either size", "[kdrv_frames]")
{
    uint8_t            p[PAYLOAD_MAX];
    uint8_t const      zeros[KODE_SNIFF_HEAD_LEN] = { 0U };
    kdrv_frame_t       f;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    /* The record as it is: a QoS data frame (type 2, subtype 8). */
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, batch(p, 6U, 1U, 0x88U)));
    TEST_ASSERT_TRUE(kdrv_frames_next(&f));
    TEST_ASSERT_EQUAL_UINT8(0x88U, f.fc[0]);
    TEST_ASSERT_EQUAL_UINT8(0x30U, f.addr3[2]);
    TEST_ASSERT_EQUAL_UINT8(0U, f.head_len);
    TEST_ASSERT_EQUAL_MEMORY(zeros, f.head, sizeof(f.head));

    /* The record as it was, which a C5 of v0.1.1 fills with the LLC header. */
    size_t const n = batch_short(p, 6U, 1U);
    short_rec_t  r;

    (void)memcpy(&r, &p[sizeof(kode_sniff_batch_t)], sizeof(r));
    r.fc[0] = 0x08U;
    (void)memcpy(&p[sizeof(kode_sniff_batch_t)], &r, sizeof(r));
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, n));
    TEST_ASSERT_TRUE(kdrv_frames_next(&f));
    TEST_ASSERT_EQUAL_UINT8(0x08U, f.fc[0]);
    TEST_ASSERT_EQUAL_UINT8(0U, f.head_len);
    TEST_ASSERT_EQUAL_MEMORY(zeros, f.head, sizeof(f.head));
    TEST_ASSERT_FALSE(kdrv_frames_next(&f));
}

/* A period that heard nothing is a batch of its header alone. */
TEST_CASE("a batch with no record is a tick and no frame", "[kdrv_frames]")
{
    uint8_t            p[PAYLOAD_MAX];
    kode_sniff_batch_t h = { 6U, 0U, KODE_SNIFF_RSSI_NONE, 0U, 4U, 0U, 0U };
    kdrv_frames_tick_t t;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    (void)memcpy(p, &h, sizeof(h));
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, sizeof(h)));
    TEST_ASSERT_TRUE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(4U, t.heard);
    TEST_ASSERT_EQUAL_UINT32(0U, drain());
}
