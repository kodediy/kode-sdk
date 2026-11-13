/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_es8311_stream.c
 * @brief The stream's ring as its owner reads it: whether writes are taken,
 *        and how long since one put frames in.
 *
 * The driver is the real source. Nothing plays here: no job takes frames out
 * of the ring, so what a case writes stays in it, and the clock is the
 * fake's, set by the case.
 */
#include <stddef.h>
#include <stdint.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_es8311.h"

/** @brief Frames a case writes at a time. */
#define FEW_FRAMES  16U

/** @brief A stream opened at @p now_ms, with a ring to write into. */
static void stream_opened_at(uint32_t now_ms)
{
    g_psram   = true;
    g_time_ms = now_ms;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_es8311_stream_reset());
}

TEST_CASE("a stream nobody has written to is idle since it was opened",
          "[kdrv_es8311][stream]")
{
    stream_opened_at(1000U);

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, kdrv_es8311_stream_id());
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_es8311_stream_idle_ms());
    g_time_ms = 1500U;
    TEST_ASSERT_EQUAL_UINT32(500U, kdrv_es8311_stream_idle_ms());

    kdrv_es8311_stream_end();
}

TEST_CASE("a write that puts frames in starts the idle count again",
          "[kdrv_es8311][stream]")
{
    static const int16_t frames[FEW_FRAMES];
    size_t               taken = 0U;

    stream_opened_at(1000U);

    g_time_ms = 4000U;
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, FEW_FRAMES, 1U, 0U, &taken));
    TEST_ASSERT_EQUAL_UINT((unsigned)FEW_FRAMES, (unsigned)taken);
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_es8311_stream_idle_ms());
    g_time_ms = 4250U;
    TEST_ASSERT_EQUAL_UINT32(250U, kdrv_es8311_stream_idle_ms());

    kdrv_es8311_stream_end();
}

/* A writer that finds the ring full has put nothing in: the sound is not
 * moving, and that is what the count measures. */
TEST_CASE("a write the full ring takes nothing of does not start the count again",
          "[kdrv_es8311][stream]")
{
    static const int16_t frames[KDRV_ES8311_STREAM_FRAMES];
    size_t               taken = 0U;

    stream_opened_at(1000U);
    g_time_ms = 2000U;
    (void)kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, KDRV_ES8311_STREAM_FRAMES, 1U, 0U, &taken);
    TEST_ASSERT_TRUE_MESSAGE(0U < taken, "the empty ring took frames");
    (void)kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, KDRV_ES8311_STREAM_FRAMES, 1U, 0U, &taken);

    g_time_ms = 9000U;
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
        kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, FEW_FRAMES, 1U, 0U, &taken));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)taken);
    TEST_ASSERT_EQUAL_UINT32(7000U, kdrv_es8311_stream_idle_ms());

    kdrv_es8311_stream_end();
}

TEST_CASE("an ended stream has no number and takes no write", "[kdrv_es8311][stream]")
{
    static const int16_t frames[FEW_FRAMES];
    size_t               taken = FEW_FRAMES;

    stream_opened_at(1000U);
    kdrv_es8311_stream_end();

    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_es8311_stream_id());
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, FEW_FRAMES, 1U, 0U, &taken));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)taken);
}

/* The clock wraps after about 49 days; the count is a difference of two
 * readings and goes on across the wrap. */
TEST_CASE("the idle count goes on across the clock's wrap", "[kdrv_es8311][stream]")
{
    stream_opened_at(UINT32_MAX - 99U);

    g_time_ms = 400U;
    TEST_ASSERT_EQUAL_UINT32(500U, kdrv_es8311_stream_idle_ms());

    kdrv_es8311_stream_end();
}

/* A writer that left its stream to be closed must not take the next stream
 * for its own: each stream has its own number. */
TEST_CASE("each stream opened has a number of its own", "[kdrv_es8311][stream]")
{
    uint32_t first;
    uint32_t second;

    stream_opened_at(1000U);
    first = kdrv_es8311_stream_id();
    kdrv_es8311_stream_end();
    stream_opened_at(2000U);
    second = kdrv_es8311_stream_id();

    TEST_ASSERT_NOT_EQUAL_UINT32(0U, first);
    TEST_ASSERT_NOT_EQUAL_UINT32(0U, second);
    TEST_ASSERT_NOT_EQUAL_UINT32(first, second);

    kdrv_es8311_stream_end();
}

/** @brief While the writer waits, its stream ends and the next one opens. */
static void next_stream_opens(void)
{
    g_sem_wait_hook = NULL;
    kdrv_es8311_stream_end();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_es8311_stream_reset());
}

/* A writer waits for room in the ring of its own stream. If that stream ends
 * for silence and another task opens the next one meanwhile, the writer's
 * frames are not the next stream's to play. */
TEST_CASE("a writer that waits for room never writes into the next stream",
          "[kdrv_es8311][stream]")
{
    static const int16_t frames[KDRV_ES8311_STREAM_FRAMES];
    size_t               room  = 0U;
    size_t               taken = 0U;

    stream_opened_at(1000U);
    (void)kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, KDRV_ES8311_STREAM_FRAMES, 1U, 0U, &room);
    TEST_ASSERT_TRUE_MESSAGE(0U < room, "the empty ring took no frames");

    g_sem_wait_hook = next_stream_opens;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, FEW_FRAMES, 1U, 100U, &taken));
    g_sem_wait_hook = NULL;
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (unsigned)taken, "the old stream's frames went into the new one");

    /* The new stream's ring is whole: nothing of the old writer is in it. */
    (void)kdrv_es8311_stream_write(kdrv_es8311_stream_id(), frames, KDRV_ES8311_STREAM_FRAMES, 1U, 0U, &taken);
    TEST_ASSERT_EQUAL_UINT((unsigned)room, (unsigned)taken);

    kdrv_es8311_stream_end();
}
