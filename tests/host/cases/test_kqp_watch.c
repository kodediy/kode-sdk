/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kqp_watch.c
 * @brief The watchdog's judge: stuck, starved, or fine.
 */
#include <string.h>

#include "unity.h"

#include "kqp_watch_judge.h"

/** @brief The timeout every case judges against, the one the port ships. */
#define TIMEOUT_MS 30000U

/** @brief One pass of the supervisor, in ms. */
#define PERIOD_MS 1000U

/** @brief Start tick of a case that does not care about it. */
#define T0_MS 1000U

/** @brief The signal a stuck handler is inside, to see it copied. */
#define SIG_MARK 0x1234U

/** @brief Samples of the longest-over case. */
#define TWO 2U

/** @brief A sample for an Active Object, valid, with the fields a case sets. */
static KqpWatchSample sample(uint32_t count, uint32_t start_ms,
                             uint32_t waiting)
{
    KqpWatchSample s;

    (void)memset(&s, 0, sizeof(s));
    (void)strcpy(s.name, "AO_Test");
    s.prio = 5U;
    s.count = count;
    s.start_ms = start_ms;
    s.sig = SIG_MARK;
    s.waiting = waiting;
    s.valid = true;
    return s;
}

TEST_CASE("an odd count under the deadline is fine", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];

    s[0] = sample(3U, T0_MS, 0U);
    KqpWatchVerdict v = kqp_watch_judge(slot, s, 1U, T0_MS + TIMEOUT_MS - 1U,
                                        TIMEOUT_MS);

    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
}

TEST_CASE("an odd count over the deadline is stuck", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];

    s[0] = sample(3U, T0_MS, 2U);
    KqpWatchVerdict v = kqp_watch_judge(slot, s, 1U, T0_MS + TIMEOUT_MS + 1U,
                                        TIMEOUT_MS);

    TEST_ASSERT_EQUAL_INT(KQP_WATCH_STUCK, v.kind);
    TEST_ASSERT_EQUAL_UINT8(0U, v.index);
    TEST_ASSERT_EQUAL_UINT32(TIMEOUT_MS + 1U, v.over_ms);
    TEST_ASSERT_EQUAL_UINT16(SIG_MARK, v.sig);
    TEST_ASSERT_EQUAL_UINT32(2U, v.waiting);
}

TEST_CASE("an even count with an empty queue is fine however long", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    KqpWatchVerdict v;
    uint32_t now;

    s[0] = sample(4U, T0_MS, 0U);
    v.kind = KQP_WATCH_OK;
    for ( now = T0_MS; (T0_MS + (40U * PERIOD_MS)) >= now; now += PERIOD_MS )
    {
        v = kqp_watch_judge(slot, s, 1U, now, TIMEOUT_MS);
        TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
    }
}

TEST_CASE("a queue that is served as it fills is fine", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    KqpWatchVerdict v;
    uint32_t now;
    uint32_t count = 0U;

    for ( now = T0_MS; (T0_MS + (60U * PERIOD_MS)) >= now; now += PERIOD_MS )
    {
        count += 2U;
        s[0] = sample(count, now, 2U);
        v = kqp_watch_judge(slot, s, 1U, now, TIMEOUT_MS);
        TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
    }
}

TEST_CASE("events waiting behind a frozen count are starved", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    KqpWatchVerdict v;
    uint32_t now;

    s[0] = sample(6U, T0_MS, 3U);
    for ( now = T0_MS; (T0_MS + TIMEOUT_MS) >= now; now += PERIOD_MS )
    {
        v = kqp_watch_judge(slot, s, 1U, now, TIMEOUT_MS);
        TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
    }
    v = kqp_watch_judge(slot, s, 1U, T0_MS + TIMEOUT_MS + 1U, TIMEOUT_MS);

    TEST_ASSERT_EQUAL_INT(KQP_WATCH_STARVED, v.kind);
    TEST_ASSERT_EQUAL_UINT8(0U, v.index);
    TEST_ASSERT_EQUAL_UINT32(TIMEOUT_MS + 1U, v.over_ms);
    TEST_ASSERT_EQUAL_UINT32(3U, v.waiting);
}

TEST_CASE("a queue that fills after an hour of idle is not starved", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    KqpWatchVerdict v;
    uint32_t now;
    uint32_t hour_ms = 3600U * 1000U;

    s[0] = sample(8U, T0_MS, 0U);
    for ( now = T0_MS; (T0_MS + hour_ms) >= now; now += PERIOD_MS )
    {
        v = kqp_watch_judge(slot, s, 1U, now, TIMEOUT_MS);
        TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
    }
    /* The streak starts here, not an hour ago. */
    s[0] = sample(8U, T0_MS, 1U);
    v = kqp_watch_judge(slot, s, 1U, T0_MS + hour_ms + PERIOD_MS, TIMEOUT_MS);

    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
}

TEST_CASE("the tick wrapping does not fake or hide a hang", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    KqpWatchVerdict v;

    s[0] = sample(5U, 0xFFFFF000U, 0U);

    /* 28672 ms after the event began, across the wrap. */
    v = kqp_watch_judge(slot, s, 1U, 0x00006000U, TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);

    /* 34097 ms after it began. */
    v = kqp_watch_judge(slot, s, 1U, 0x00007531U, TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(KQP_WATCH_STUCK, v.kind);
    TEST_ASSERT_EQUAL_UINT32(34097U, v.over_ms);
}

TEST_CASE("the Active Object over its deadline by the most is the one named",
          "[kqp_watch]")
{
    KqpWatchSlot slot[TWO] = {{0U, 0U, false}, {0U, 0U, false}};
    KqpWatchSample s[TWO];
    KqpWatchVerdict v;
    uint32_t now = 100000U;

    s[0] = sample(3U, now - 31000U, 0U);
    s[1] = sample(5U, now - 40000U, 0U);
    v = kqp_watch_judge(slot, s, TWO, now, TIMEOUT_MS);

    TEST_ASSERT_EQUAL_INT(KQP_WATCH_STUCK, v.kind);
    TEST_ASSERT_EQUAL_UINT8(1U, v.index);
    TEST_ASSERT_EQUAL_UINT32(40000U, v.over_ms);

    /* A tie goes to the lower index. */
    s[0] = sample(3U, now - 40000U, 0U);
    v = kqp_watch_judge(slot, s, TWO, now, TIMEOUT_MS);
    TEST_ASSERT_EQUAL_UINT8(0U, v.index);
}

TEST_CASE("an invalid sample is ignored", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];

    s[0] = sample(3U, T0_MS, 5U);
    s[0].valid = false;
    KqpWatchVerdict v = kqp_watch_judge(slot, s, 1U, T0_MS + (2U * TIMEOUT_MS),
                                        TIMEOUT_MS);

    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
}

TEST_CASE("an event that began a tick after the reading is just started",
          "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    uint32_t now = 100000U;

    s[0] = sample(3U, now + 1U, 0U);
    KqpWatchVerdict v = kqp_watch_judge(slot, s, 1U, now, TIMEOUT_MS);

    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
}

TEST_CASE("a streak that started after the reading is just started",
          "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    KqpWatchVerdict v;
    uint32_t now = 100000U;

    s[0] = sample(6U, now, 2U);
    v = kqp_watch_judge(slot, s, 1U, now, TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);

    /* The next pass reads a tick one behind the streak's start. */
    v = kqp_watch_judge(slot, s, 1U, now - 1U, TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
}

/* ---- the slow steps: the port times them, the judge decides the line ------ */

/** @brief The limit the rule gives a step, in ms. */
#define STEP_MAX_MS 1000U

/** @brief A step well over the limit, in ms. */
#define SLOW_MS 1500U

/** @brief A signal for the slow step, to see it copied. */
#define SLOW_SIG 0x2aU

/** @brief Three slow steps between two passes. */
#define THREE 3U

/** @brief A sample whose port counted @p seq slow steps, the latest of @p ms. */
static KqpWatchSample slow_sample(uint32_t seq, uint32_t ms)
{
    KqpWatchSample s = sample(2U, T0_MS, 0U);

    s.slow_seq = seq;
    s.slow_ms = ms;
    s.slow_sig = SLOW_SIG;
    return s;
}

TEST_CASE("one slow step of 1500 ms is logged with 1500", "[kqp_watch]")
{
    uint32_t seen = 0U;
    KqpWatchSample const s = slow_sample(1U, SLOW_MS);
    KqpWatchSlow const r = kqp_watch_slow(&seen, &s, STEP_MAX_MS);

    TEST_ASSERT_TRUE(r.log);
    TEST_ASSERT_EQUAL_UINT32(SLOW_MS, r.ms);
    TEST_ASSERT_EQUAL_UINT16(SLOW_SIG, r.sig);
    TEST_ASSERT_EQUAL_UINT32(1U, r.count);
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
}

TEST_CASE("a step of exactly the limit is not logged", "[kqp_watch]")
{
    uint32_t seen = 0U;
    KqpWatchSample const s = slow_sample(1U, STEP_MAX_MS);

    TEST_ASSERT_FALSE(kqp_watch_slow(&seen, &s, STEP_MAX_MS).log);
}

TEST_CASE("one tick over the limit is logged", "[kqp_watch]")
{
    uint32_t seen = 0U;
    KqpWatchSample const s = slow_sample(1U, STEP_MAX_MS + 1U);

    TEST_ASSERT_TRUE(kqp_watch_slow(&seen, &s, STEP_MAX_MS).log);
}

TEST_CASE("the same count is not logged twice", "[kqp_watch]")
{
    uint32_t seen = 0U;
    KqpWatchSample const s = slow_sample(1U, SLOW_MS);

    TEST_ASSERT_TRUE(kqp_watch_slow(&seen, &s, STEP_MAX_MS).log);
    TEST_ASSERT_FALSE(kqp_watch_slow(&seen, &s, STEP_MAX_MS).log);
}

TEST_CASE("three slow steps between passes make one line with the count 3",
          "[kqp_watch]")
{
    uint32_t seen = 0U;
    KqpWatchSample const s = slow_sample(THREE, SLOW_MS);
    KqpWatchSlow const r = kqp_watch_slow(&seen, &s, STEP_MAX_MS);

    TEST_ASSERT_TRUE(r.log);
    TEST_ASSERT_EQUAL_UINT32(THREE, r.count);
    TEST_ASSERT_EQUAL_UINT32(SLOW_MS, r.ms);
}

TEST_CASE("the count survives the counter wrapping", "[kqp_watch]")
{
    uint32_t seen = UINT32_MAX;
    KqpWatchSample const s = slow_sample(1U, SLOW_MS);
    KqpWatchSlow const r = kqp_watch_slow(&seen, &s, STEP_MAX_MS);

    TEST_ASSERT_TRUE(r.log);
    TEST_ASSERT_EQUAL_UINT32(TWO, r.count);
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
}

TEST_CASE("an unused slot says nothing about slow steps", "[kqp_watch]")
{
    uint32_t seen = 0U;
    KqpWatchSample s = slow_sample(THREE, SLOW_MS);

    s.valid = false;
    TEST_ASSERT_FALSE(kqp_watch_slow(&seen, &s, STEP_MAX_MS).log);
    TEST_ASSERT_EQUAL_UINT32(0U, seen);
}

/* ---- The tick resting ---------------------------------------------------- */

/** @brief A count of rests the first pass of a case has seen. */
#define SEEN_NONE 0U

TEST_CASE("a pass during a rest judges nothing and keeps the slots", "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{5U, T0_MS, true}};
    uint32_t seen = SEEN_NONE;

    TEST_ASSERT_TRUE(kqp_watch_rested(true, SEEN_NONE, &seen, slot, 1U));
    TEST_ASSERT_TRUE(slot[0].streaking);
    TEST_ASSERT_EQUAL_UINT32(T0_MS, slot[0].streak_start_ms);
}

TEST_CASE("the first pass after a rest judges nothing and starts the streaks over",
          "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{5U, T0_MS, true}};
    uint32_t seen = SEEN_NONE;

    TEST_ASSERT_TRUE(kqp_watch_rested(false, 1U, &seen, slot, 1U));
    TEST_ASSERT_EQUAL_UINT32(1U, seen);
    TEST_ASSERT_FALSE(slot[0].streaking);
    TEST_ASSERT_EQUAL_UINT32(0U, slot[0].streak_start_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, slot[0].last_count);

    /* The pass after that judges again. */
    TEST_ASSERT_FALSE(kqp_watch_rested(false, 1U, &seen, slot, 1U));
}

TEST_CASE("a starved queue is not blamed for the time the board slept",
          "[kqp_watch]")
{
    KqpWatchSlot slot[1] = {{0U, 0U, false}};
    KqpWatchSample s[1];
    KqpWatchVerdict v;
    uint32_t seen = SEEN_NONE;

    /* Events waiting behind an even count: a streak begins. */
    s[0] = sample(2U, T0_MS, 1U);
    v = kqp_watch_judge(slot, s, 1U, T0_MS, TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);

    /* The board sleeps for a minute and wakes: without the wipe the next
     * judging reads a minute of starvation. */
    TEST_ASSERT_TRUE(kqp_watch_rested(false, 1U, &seen, slot, 1U));
    v = kqp_watch_judge(slot, s, 1U, T0_MS + (2U * TIMEOUT_MS), TIMEOUT_MS);
    TEST_ASSERT_EQUAL_INT(KQP_WATCH_OK, v.kind);
}
