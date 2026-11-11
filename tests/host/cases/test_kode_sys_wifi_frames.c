/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_wifi_frames.c
 * @brief The Wi-Fi frames request in kode_sys: what an AO's call posts to AO_Radio.
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

/** @brief The channel the cases ask for. */
#define FRAMES_TEST_CH   6U

/** @brief Posts that surely fill AO_Radio's queue. */
#define FRAMES_FILL_MAX  64U

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
static void frames_fresh(void)
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

TEST_CASE("a frames request from no AO is refused and posts nothing", "[kode_sys_wifi_frames]")
{
    frames_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_wifi_frames(NULL, true, FRAMES_TEST_CH));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

TEST_CASE("a frames request posts one WIFI_FRAMES_REQ_SIG with the asker, the wish and the channel",
          "[kode_sys_wifi_frames]")
{
    QActive *const me = ao_spy();
    const QEvt    *e  = NULL;

    frames_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_frames(me, true, FRAMES_TEST_CH));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int)WIFI_FRAMES_REQ_SIG, (int)e->sig);
    {
        const WifiFramesReqEvt *const r = (const WifiFramesReqEvt *)(const void *)e;

        TEST_ASSERT_TRUE(me == r->who);
        TEST_ASSERT_TRUE(r->on);
        TEST_ASSERT_EQUAL_UINT8(FRAMES_TEST_CH, r->channel);
    }
    QF_gc(e);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_frames(me, false, 0U));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int)WIFI_FRAMES_REQ_SIG, (int)e->sig);
    {
        const WifiFramesReqEvt *const r = (const WifiFramesReqEvt *)(const void *)e;

        TEST_ASSERT_TRUE(me == r->who);
        TEST_ASSERT_FALSE(r->on);
    }
    QF_gc(e);
}

TEST_CASE("a frames request AO_Radio has no room for is refused", "[kode_sys_wifi_frames]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    frames_fresh();
    for ( uint32_t guard = 0U;
         (FRAMES_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_wifi_frames(ao_spy(), true, FRAMES_TEST_CH));
    radio_flush();
}
