/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_ble_adv.c
 * @brief The BLE advertisement request in kode_sys: what an AO's call posts to AO_Radio.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kode_sys.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"

/** @brief Posts that surely fill AO_Radio's queue. */
#define ADV_FILL_MAX  64U

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief A fresh case: AO_Radio's queue empty. */
static void heard_fresh(void)
{
    qf_support_init();
    qf_support_give_queue(AO_Radio);
    radio_flush();
}

/** @brief How many requests wait for AO_Radio. */
static uint32_t radio_pending(void)
{
    return (uint32_t)uxQueueMessagesWaiting(AO_Radio->eQueue);
}

TEST_CASE("an advertisement request from no AO is refused and posts nothing", "[kode_sys_ble_heard]")
{
    heard_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_heard(NULL, true));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

TEST_CASE("an advertisement request posts one BLE_HEARD_REQ_SIG with the asker and the wish",
          "[kode_sys_ble_heard]")
{
    QActive *const me = ao_spy();
    const QEvt    *e  = NULL;

    heard_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_heard(me, true));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int)BLE_HEARD_REQ_SIG, (int)e->sig);
    {
        const BleHeardReqEvt *const r = (const BleHeardReqEvt *)(const void *)e;

        TEST_ASSERT_TRUE(me == r->who);
        TEST_ASSERT_TRUE(r->on);
    }
    QF_gc(e);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_heard(me, false));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int)BLE_HEARD_REQ_SIG, (int)e->sig);
    {
        const BleHeardReqEvt *const r = (const BleHeardReqEvt *)(const void *)e;

        TEST_ASSERT_TRUE(me == r->who);
        TEST_ASSERT_FALSE(r->on);
    }
    QF_gc(e);
}

TEST_CASE("an advertisement request AO_Radio has no room for is refused", "[kode_sys_ble_heard]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    heard_fresh();
    for ( uint32_t guard = 0U;
         (ADV_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ble_heard(ao_spy(), true));
    radio_flush();
}
