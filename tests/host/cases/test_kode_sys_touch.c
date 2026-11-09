/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_touch.c
 * @brief kode_sys_touch_subscribe(): the subscription that links AO_Touch.
 */
#include <stdbool.h>

#include "unity.h"

#include "ao_spy.h"
#include "kaos.h"
#include "kode_sys.h"
#include "qf_support.h"

TEST_CASE("kode_sys_touch_subscribe() makes its AO hear AO_Touch's reports",
          "[kode_sys]")
{
    static TouchEvt touch;

    qf_support_init();
    QActive * const spy = ao_spy();
    ao_spy_drain();
    QActive_unsubscribe(spy, TOUCH_EVT_SIG);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_touch_subscribe(spy));
    touch.super   = (QEvt)QEVT_INITIALIZER(TOUCH_EVT_SIG);
    touch.pressed = true;
    QACTIVE_PUBLISH(&touch.super, NULL);

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(TOUCH_EVT_SIG, (int)ao_spy_next_sig());
    ao_spy_drain();
    QActive_unsubscribe(spy, TOUCH_EVT_SIG);
}

TEST_CASE("kode_sys_touch_subscribe() refuses no AO, and one not started",
          "[kode_sys]")
{
    static QActive idle;   /* never started: priority 0 */

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_touch_subscribe(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_touch_subscribe(&idle));
}
