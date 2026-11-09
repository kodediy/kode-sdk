/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_power.c
 * @brief The NFC reader's rest and wake transitions: which one a step needs, if any.
 */
#include "unity.h"

#include "knfc_power.h"

TEST_CASE("a use from rest wakes the reader", "[knfc_power]")
{
    TEST_ASSERT_EQUAL(KNFC_POWER_WAKE,
                      knfc_power_action(KNFC_POWER_RESTING, KNFC_POWER_AWAKE));
}

TEST_CASE("a second use inside one field-up wakes nothing", "[knfc_power]")
{
    TEST_ASSERT_EQUAL(KNFC_POWER_HOLD,
                      knfc_power_action(KNFC_POWER_AWAKE, KNFC_POWER_AWAKE));
}

TEST_CASE("the end of a use rests the reader", "[knfc_power]")
{
    TEST_ASSERT_EQUAL(KNFC_POWER_REST,
                      knfc_power_action(KNFC_POWER_AWAKE, KNFC_POWER_RESTING));
}

TEST_CASE("a rest while already resting rests nothing", "[knfc_power]")
{
    TEST_ASSERT_EQUAL(KNFC_POWER_HOLD,
                      knfc_power_action(KNFC_POWER_RESTING, KNFC_POWER_RESTING));
}

TEST_CASE("boot, a request, and its end walk rest to rest", "[knfc_power]")
{
    /* rfalNfcInitialize leaves the oscillator on, so the driver's state
     * starts awake; commit each step's new state only when its action fires,
     * the way the driver does after the chip agrees. */
    knfc_power_state_t s = KNFC_POWER_AWAKE;

    /* Bring-up drops it to rest. */
    TEST_ASSERT_EQUAL(KNFC_POWER_REST, knfc_power_action(s, KNFC_POWER_RESTING));
    s = KNFC_POWER_RESTING;

    /* A request wakes it. */
    TEST_ASSERT_EQUAL(KNFC_POWER_WAKE, knfc_power_action(s, KNFC_POWER_AWAKE));
    s = KNFC_POWER_AWAKE;

    /* A read within the same request touches nothing. */
    TEST_ASSERT_EQUAL(KNFC_POWER_HOLD, knfc_power_action(s, KNFC_POWER_AWAKE));

    /* The request ends and it rests again. */
    TEST_ASSERT_EQUAL(KNFC_POWER_REST, knfc_power_action(s, KNFC_POWER_RESTING));
    s = KNFC_POWER_RESTING;

    /* A deadline firing after the rest touches nothing. */
    TEST_ASSERT_EQUAL(KNFC_POWER_HOLD, knfc_power_action(s, KNFC_POWER_RESTING));
}
