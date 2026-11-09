/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kqp_timer.c
 * @brief kqp_timer_stale(): the two things a failed disarm can mean.
 */
#include "unity.h"

#include "ao_spy.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief Signal of the timer the cases build; the spy takes a post of anything. */
#define TIMER_SIG IOEXP_INT_SIG

/** @brief A long interval, in QP ticks, that no case reaches. */
#define TIMER_FAR_TICKS 1000U

TEST_CASE("a timer that was never armed is stale only if the caller thinks it was",
          "[kqp_timer]")
{
    QTimeEvt te;

    qf_support_init();
    QTimeEvt_ctorX(&te, ao_spy(), TIMER_SIG, 0U);

    TEST_ASSERT_FALSE(kqp_timer_stale(&te, false));
    TEST_ASSERT_TRUE(kqp_timer_stale(&te, true));
}

TEST_CASE("a timer still running is disarmed and is not stale", "[kqp_timer]")
{
    QTimeEvt te;

    qf_support_init();
    QTimeEvt_ctorX(&te, ao_spy(), TIMER_SIG, 0U);
    QTimeEvt_armX(&te, TIMER_FAR_TICKS, 0U);

    TEST_ASSERT_FALSE(kqp_timer_stale(&te, true));
    /* Disarmed: asking again finds it not armed. */
    TEST_ASSERT_TRUE(kqp_timer_stale(&te, true));
}
