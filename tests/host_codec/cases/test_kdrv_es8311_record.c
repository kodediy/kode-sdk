/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_es8311_record.c
 * @brief What a recording reads off the microphone after an open, and what it keeps.
 *
 * The driver is the real source. The fake's receive half numbers its blocks:
 * every sample of a block reads how many blocks were read before it, so the
 * first frame a recording keeps says how many it threw away. These cases run
 * after the pins cases, which bring the driver up.
 */
#include <stdint.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_es8311.h"
#include "ktone.h"

/** @brief The settle blocks a fresh open is given: ES_SETTLE_BLOCKS in kdrv_es8311.c. */
#define SETTLE_BLOCKS   6U
/** @brief The microphone's start-up step, in blocks: ES_FRESH_RX_BLOCKS in kdrv_es8311.c. */
#define FRESH_BLOCKS   36U
/** @brief Blocks the first recording after an open throws away. */
#define SKIP_BLOCKS    (SETTLE_BLOCKS + FRESH_BLOCKS)
/** @brief One analysis window, in blocks. */
#define WINDOW_BLOCKS  (KTONE_WIN_N / KTONE_BLOCK_FRAMES)
/** @brief Frames a case records: three whole blocks and part of a fourth. */
#define REC_FRAMES     ((3U * KTONE_BLOCK_FRAMES) + 100U)
/** @brief Blocks a recording of REC_FRAMES reads. */
#define REC_BLOCKS     4U

/** @brief Where a recording goes. */
static int16_t s_rec[REC_FRAMES];

/** @brief A closed codec and an empty log, whatever the case before left. */
static void codec_closed(void)
{
    kdrv_es8311_req_t req = { .type = KDRV_ES8311_JOB_CLOSE };

    (void)kdrv_es8311_run(&req);
    fake_reset();
}

/**
 * @brief Run one job with no buffer on the driver, as AO_Audio's worker would.
 * @param type The job.
 * @return What the driver answered.
 */
static kode_status_t job(kdrv_es8311_job_t type)
{
    kdrv_es8311_req_t req = { .type = type };

    return kdrv_es8311_run(&req);
}

/**
 * @brief Record REC_FRAMES into s_rec, as AO_Audio does: the early end cleared first.
 * @return The frames the driver says it recorded.
 */
static uint32_t record(void)
{
    kdrv_es8311_req_t    req = { .type = KDRV_ES8311_JOB_RECORD, .buf = s_rec, .frames = REC_FRAMES };
    kdrv_es8311_result_t res;

    kdrv_es8311_record_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_es8311_run(&req));
    kdrv_es8311_result(&res);
    return res.frames;
}

TEST_CASE("the first recording after an open throws the start-up step away and keeps every frame asked for",
          "[kdrv_es8311][record]")
{
    codec_closed();
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_OPEN));

    TEST_ASSERT_EQUAL_UINT32(REC_FRAMES, record());
    TEST_ASSERT_EQUAL_UINT32(SKIP_BLOCKS + REC_BLOCKS, g_reads);
    /* The first frame kept is the first block after the ones thrown away,
     * and the last is the fourth block after them. */
    TEST_ASSERT_EQUAL_INT16((int16_t)SKIP_BLOCKS, s_rec[0]);
    TEST_ASSERT_EQUAL_INT16((int16_t)(SKIP_BLOCKS + REC_BLOCKS - 1U), s_rec[REC_FRAMES - 1U]);
}

TEST_CASE("a second recording on the same open throws nothing away",
          "[kdrv_es8311][record]")
{
    uint32_t before;

    codec_closed();
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    (void)record();
    before = g_reads;

    TEST_ASSERT_EQUAL_UINT32(REC_FRAMES, record());
    TEST_ASSERT_EQUAL_UINT32(REC_BLOCKS, g_reads - before);
    TEST_ASSERT_EQUAL_INT16((int16_t)before, s_rec[0]);
}

TEST_CASE("a recording after a close and a new open throws the start-up step away again",
          "[kdrv_es8311][record]")
{
    uint32_t before;

    codec_closed();
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    (void)record();
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_CLOSE));
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    before = g_reads;

    TEST_ASSERT_EQUAL_UINT32(REC_FRAMES, record());
    TEST_ASSERT_EQUAL_UINT32(SKIP_BLOCKS + REC_BLOCKS, g_reads - before);
    TEST_ASSERT_EQUAL_INT16((int16_t)(before + SKIP_BLOCKS), s_rec[0]);
}

/* The production row's window waits out the step on its own; it reads what it
 * always read, and a recording after it on the same open finds the step past. */
TEST_CASE("a window after an open reads what it always did and leaves a recording nothing to throw away",
          "[kdrv_es8311][record]")
{
    uint32_t before;

    codec_closed();
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_OPEN));

    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_FLOOR));
    TEST_ASSERT_EQUAL_UINT32(SKIP_BLOCKS + WINDOW_BLOCKS, g_reads);
    before = g_reads;

    TEST_ASSERT_EQUAL_UINT32(REC_FRAMES, record());
    TEST_ASSERT_EQUAL_UINT32(REC_BLOCKS, g_reads - before);
    TEST_ASSERT_EQUAL_INT16((int16_t)before, s_rec[0]);
}

TEST_CASE("a window after a recording on the same open reads what it always did",
          "[kdrv_es8311][record]")
{
    uint32_t before;

    codec_closed();
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    (void)record();
    before = g_reads;

    /* Nothing has been emitted since the open, so the window still waits out
     * the step itself, as it did before recordings threw it away. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_FLOOR));
    TEST_ASSERT_EQUAL_UINT32(SKIP_BLOCKS + WINDOW_BLOCKS, g_reads - before);
}

/* A recording ended before it starts keeps nothing and reads nothing; the step
 * is still owed, and the next recording on the same open throws it away. */
TEST_CASE("a recording ended while the start-up step is owed leaves it to the next one",
          "[kdrv_es8311][record]")
{
    kdrv_es8311_req_t    req = { .type = KDRV_ES8311_JOB_RECORD, .buf = s_rec, .frames = REC_FRAMES };
    kdrv_es8311_result_t res;

    codec_closed();
    TEST_ASSERT_EQUAL_INT(KODE_OK, job(KDRV_ES8311_JOB_OPEN));
    kdrv_es8311_record_end();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_es8311_run(&req));
    kdrv_es8311_result(&res);
    TEST_ASSERT_EQUAL_UINT32(0U, res.frames);
    TEST_ASSERT_EQUAL_UINT32(0U, g_reads);

    TEST_ASSERT_EQUAL_UINT32(REC_FRAMES, record());
    TEST_ASSERT_EQUAL_UINT32(SKIP_BLOCKS + REC_BLOCKS, g_reads);
    TEST_ASSERT_EQUAL_INT16((int16_t)SKIP_BLOCKS, s_rec[0]);
}
