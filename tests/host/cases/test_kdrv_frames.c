/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_frames.c
 * @brief The frames service's P4 end: answers read, batches decoded, the ring bounded.
 *
 * The real driver, with no mock. The first case runs before anything calls
 * kdrv_frames_init(): the driver keeps its state for the life of the binary.
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

TEST_CASE("a batch fed before the driver exists is refused", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX];
    size_t const n = batch(p, 6U, 1U, 0U);
    kdrv_frame_t f;
    kdrv_frames_tick_t t;

    TEST_ASSERT_FALSE(kdrv_frames_feed(p, n));
    TEST_ASSERT_FALSE(kdrv_frames_next(&f));
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_frames_dropped());
    kdrv_frames_forget();
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
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(100U + i), f.len);
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

TEST_CASE("a batch shorter than its records is ignored", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX];
    size_t const n = batch(p, 6U, 2U, 0U);
    kdrv_frames_tick_t t;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    TEST_ASSERT_FALSE(kdrv_frames_feed(p, n - 1U));
    TEST_ASSERT_FALSE(kdrv_frames_feed(p, sizeof(kode_sniff_batch_t) - 1U));
    TEST_ASSERT_FALSE(kdrv_frames_feed(NULL, n));
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
    TEST_ASSERT_EQUAL_UINT32(0U, drain());
}

TEST_CASE("a count over the maximum is ignored", "[kdrv_frames]")
{
    uint8_t p[PAYLOAD_MAX + sizeof(kode_sniff_rec_t)];
    kdrv_frames_tick_t t;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_frames_init());
    kdrv_frames_forget();
    (void)memset(p, 0, sizeof(p));
    kode_sniff_batch_t const h = { 6U, (uint8_t)(KODE_SNIFF_BATCH_MAX + 1U), -40, 0U, 1U, 1U, 0U };
    (void)memcpy(p, &h, sizeof(h));
    TEST_ASSERT_FALSE(kdrv_frames_feed(p, sizeof(p)));
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));
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
