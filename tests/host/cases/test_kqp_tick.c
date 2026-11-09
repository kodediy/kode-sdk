/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kqp_tick.c
 * @brief kqp_tick_rest(): the QP tick stops and runs again, once each time.
 */
#include "unity.h"

#include "fakes.h"
#include "kqp.h"
#include "kqp_bsp.h"

/** @brief Start the tick the way kqp_init() does and forget earlier counts. */
static void fresh(void)
{
    kqp_tick_start();
    fake_time_periodic_forget();
}

TEST_CASE("resting the tick twice and waking it twice stops and starts it once",
          "[kqp_tick]")
{
    fresh();

    kqp_tick_rest(true);
    kqp_tick_rest(true);
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_periodic_stops());
    TEST_ASSERT_EQUAL_UINT(0U, fake_time_periodic_starts());
    TEST_ASSERT_FALSE(fake_time_periodic_running());

    kqp_tick_rest(false);
    kqp_tick_rest(false);
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_periodic_stops());
    TEST_ASSERT_EQUAL_UINT(1U, fake_time_periodic_starts());
    TEST_ASSERT_TRUE(fake_time_periodic_running());
}

TEST_CASE("waking a tick that never rested does nothing", "[kqp_tick]")
{
    fresh();

    kqp_tick_rest(false);
    TEST_ASSERT_EQUAL_UINT(0U, fake_time_periodic_stops());
    TEST_ASSERT_EQUAL_UINT(0U, fake_time_periodic_starts());
}

TEST_CASE("the supervisor's flag is up for the rest and the rest is counted once it ends",
          "[kqp_tick]")
{
    uint32_t before;

    fresh();
    before = kqp_tick_rests();
    TEST_ASSERT_FALSE(kqp_tick_resting());

    kqp_tick_rest(true);
    TEST_ASSERT_TRUE(kqp_tick_resting());
    TEST_ASSERT_EQUAL_UINT32(before, kqp_tick_rests());

    kqp_tick_rest(false);
    TEST_ASSERT_FALSE(kqp_tick_resting());
    TEST_ASSERT_EQUAL_UINT32(before + 1U, kqp_tick_rests());
}

/* A sleeping board woke once a second for a supervisor pass that had nothing
 * to judge. Its task now waits for the wake, told by the tick itself. */
TEST_CASE("the supervisor's task rests while the tick is stopped, and only then",
          "[kqp_tick]")
{
    unsigned before;

    fresh();
    kqp_tick_rest(false);
    before = fake_watch_rest_calls();
    TEST_ASSERT_FALSE(fake_watch_resting());

    kqp_tick_rest(true);
    kqp_tick_rest(true);
    TEST_ASSERT_TRUE(fake_watch_resting());
    TEST_ASSERT_EQUAL_UINT(before + 1U, fake_watch_rest_calls());

    kqp_tick_rest(false);
    kqp_tick_rest(false);
    TEST_ASSERT_FALSE(fake_watch_resting());
    TEST_ASSERT_EQUAL_UINT(before + 2U, fake_watch_rest_calls());
}
