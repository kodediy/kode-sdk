/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kqp_isr.c
 * @brief kqp_post_from_isr(): the one door an interrupt has into an AO.
 */
#include "unity.h"

#include "ao_spy.h"
#include "kqp.h"
#include "qf_support.h"

/* Any published signal will do; the spy takes direct posts of anything. */
static const QEvt EDGE = QEVT_INITIALIZER(IOEXP_INT_SIG);

TEST_CASE("an event posted from interrupt context reaches the AO's queue once",
          "[kqp]")
{
    qf_support_init();
    QActive *spy = ao_spy();
    ao_spy_drain();

    kqp_post_from_isr(spy, &EDGE);

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(IOEXP_INT_SIG, ao_spy_next_sig());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    ao_spy_drain();
}
