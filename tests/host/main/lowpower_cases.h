/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_cases.h
 * @brief What a case needs to play AO_LowPower to one owner and judge its answer.
 */
#ifndef LOWPOWER_CASES_H_
#define LOWPOWER_CASES_H_

#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kaos.h"
#include "kqp.h"

#if !CONFIG_KODE_LOWPOWER
#error "lowpower_cases.h needs CONFIG_KODE_LOWPOWER: main/CMakeLists.txt spells it out"
#endif

/** @brief The number of the last round a case opened in this file. */
static uint32_t s_lp_round_no;

/* Dispatched to the owner, never published: a publish reaches every owner left subscribed. */
/**
 * @brief Open a round for @p target on @p ao, as AO_LowPower would.
 *
 * @param ao     The owner under test.
 * @param target KODE_LP_ASLEEP or KODE_LP_OFF.
 */
static inline void lp_round(QActive * const ao, kode_lp_state_t target)
{
    LowPowerRoundEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(LOWPOWER_ROUND_SIG);
    e.target = (uint8_t)target;
    e.round  = ++s_lp_round_no;
    QASM_DISPATCH(ao, &e.super, 0U);
}

/**
 * @brief Tell @p ao the board is AWAKE (again).
 *
 * @param ao The owner under test.
 */
static inline void lp_awake(QActive * const ao)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_AWAKE;
    e.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    e.prev  = (uint8_t)KODE_LP_ASLEEP;
    QASM_DISPATCH(ao, &e.super, 0U);
}

/**
 * @brief Tell @p ao the radio alone is awake: a radio wake, with the glass asleep.
 *
 * @param ao The owner under test.
 */
static inline void lp_radio(QActive * const ao)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_RADIO;
    e.cause = (uint8_t)KODE_LP_CAUSE_TIMER;
    e.prev  = (uint8_t)KODE_LP_ASLEEP;
    QASM_DISPATCH(ao, &e.super, 0U);
}

/**
 * @brief The next answer must be for the open round, with this verdict.
 *
 * @param busy Whether the owner refused.
 * @param why  The reason it must give, or NULL to leave it unjudged.
 */
static inline void lp_expect_answer(bool busy, const char *why)
{
    LowPowerReadyEvt a;

    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_ready_take(&a), "the round was not answered");
    TEST_ASSERT_EQUAL_UINT32(s_lp_round_no, a.round);
    TEST_ASSERT_EQUAL_INT(busy, a.busy);
    if ( NULL != why )
    {
        TEST_ASSERT_EQUAL_STRING(why, a.why);
    }
}

/** @brief The round has been answered once already, and not twice. */
static inline void lp_expect_no_more(void)
{
    LowPowerReadyEvt a;

    TEST_ASSERT_FALSE_MESSAGE(fake_lowpower_ready_take(&a), "a second answer for one round");
}

/** @brief The owner has not answered the round (yet). */
static inline void lp_expect_silence(void)
{
    lp_expect_no_more();
}

#endif /* LOWPOWER_CASES_H_ */
