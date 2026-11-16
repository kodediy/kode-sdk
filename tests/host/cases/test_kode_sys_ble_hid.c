/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_ble_hid.c
 * @brief The board as a mouse and a keyboard in kode_sys: the choice, the reports and the pairing doors.
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
#include "kdrv_ble_hid.h"
#include "kdrv_ble_hid_table.h"
#include "kode_sys.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"
#include "radio_knocks.h"

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/**
 * @brief A fresh case: AO_Radio's queue empty and a table that serves @p kinds.
 * @param kinds  The kinds chosen; 0 for a board that asked for nothing.
 * @param hosted Whether a host is subscribed to every one of them.
 */
static void hid_fresh(uint8_t kinds, bool hosted)
{
    qf_support_init();
    qf_support_give_queue(AO_Radio);
    radio_flush();
    radio_knocks_reset();   /* what AO_Radio's start does: a let-go with the ring full knocks */
    kdrv_ble_hid_select_forget();
    if ( 0U != kinds )
    {
        TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_hid_select(kinds));
        TEST_ASSERT_EQUAL_UINT8(kinds, kdrv_ble_hid_fix());
    }
    if ( hosted )
    {
        kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, true);
        kdrv_ble_hid_subscribed(KDRV_BLE_HID_KEYBOARD, true);
    }
}

/** @brief Leave nothing of a case behind. */
static void hid_done(void)
{
    radio_flush();
    kdrv_ble_hid_select_forget();
}

/** @brief The request at the head of AO_Radio's queue, which must be an HID report. */
static const BleHidReportEvt *report_head(void)
{
    const QEvt *e = NULL;

    TEST_ASSERT_EQUAL_INT_MESSAGE(pdTRUE, xQueueReceive(AO_Radio->eQueue, &e, 0U),
                                  "nothing was posted to AO_Radio");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)BLE_HID_REPORT_REQ_SIG, (uint32_t)e->sig);
    return (const BleHidReportEvt *)(const void *)e;
}

TEST_CASE("a mouse report is posted to AO_Radio, its steps as bytes", "[kode_sys_ble][hid]")
{
    static const uint8_t WANT[KDRV_BLE_HID_MOUSE_LEN] = { 0x05U, 0x0AU, 0xF6U, 0xFFU };

    hid_fresh(KODE_SYS_BLE_HID_MOUSE, true);
    TEST_ASSERT_TRUE(kode_sys_ble_hid_ready());
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_hid_mouse(0x05U, 10, -10, -1));

    const BleHidReportEvt *const r = report_head();

    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_MOUSE, r->kind);
    TEST_ASSERT_FALSE(r->tap);
    TEST_ASSERT_EQUAL_UINT8(sizeof(WANT), r->len);
    TEST_ASSERT_EQUAL_MEMORY(WANT, r->data, sizeof(WANT));
    QF_gc(&r->super);
    hid_done();
}

TEST_CASE("a keyboard report is posted to AO_Radio, modifiers first", "[kode_sys_ble][hid]")
{
    static const uint8_t KEYS[KODE_SYS_BLE_HID_KEYS_MAX] = { 0x0BU, 0x0CU, 0U, 0U, 0U, 0U };
    static const uint8_t WANT[KDRV_BLE_HID_KEYS_LEN]     = { 0x20U, 0U, 0x0BU, 0x0CU, 0U, 0U, 0U, 0U };

    hid_fresh(KODE_SYS_BLE_HID_KEYBOARD, true);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_hid_keys(0x20U, KEYS));

    const BleHidReportEvt *const r = report_head();

    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, r->kind);
    TEST_ASSERT_EQUAL_UINT8(sizeof(WANT), r->len);
    TEST_ASSERT_EQUAL_MEMORY(WANT, r->data, sizeof(WANT));
    QF_gc(&r->super);
    hid_done();
}

TEST_CASE("a board that asked for no HID refuses every report and posts nothing",
          "[kode_sys_ble][hid]")
{
    hid_fresh(0U, true);
    TEST_ASSERT_FALSE(kode_sys_ble_hid_ready());
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_mouse(0U, 1, 1, 0));
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_keys(0U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue));
    hid_done();
}

TEST_CASE("a report with no host subscribed, or of a kind not chosen, is refused",
          "[kode_sys_ble][hid]")
{
    hid_fresh(KODE_SYS_BLE_HID_MOUSE, false);
    TEST_ASSERT_FALSE(kode_sys_ble_hid_ready());
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_mouse(0U, 1, 1, 0));
    kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, true);
    TEST_ASSERT_TRUE(kode_sys_ble_hid_ready());
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_keys(0U, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue));
    hid_done();
}

TEST_CASE("a report AO_Radio's queue has no room for is refused and its slot is free again",
          "[kode_sys_ble][hid]")
{
    uint32_t posted = 0U;

    hid_fresh(KODE_SYS_BLE_HID_MOUSE, true);
    while ( (KODE_SYS_BLE_HID_RING_SLOTS > posted)
            && (kode_sys_ble_hid_mouse(0U, 1, 0, 0) == KODE_OK) )
    {
        ++posted;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(KODE_SYS_BLE_HID_RING_SLOTS, posted,
                                         "the queue's margin never refused a report");
    TEST_ASSERT_EQUAL_INT((int)KODE_NO_RESOURCES, (int)kode_sys_ble_hid_mouse(0U, 1, 0, 0));
    /* The posted ones hold their slots; the refused ones hold none. */
    TEST_ASSERT_EQUAL_INT((int)KODE_OK,
                          (int)kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE,
                                                  (uint8_t)(KODE_SYS_BLE_HID_RING_SLOTS - posted)));
    TEST_ASSERT_EQUAL_INT((int)KODE_NO_RESOURCES, (int)kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, 1U));
    hid_done();
}

TEST_CASE("a choice of kinds is the driver's: kept before the table, refused after",
          "[kode_sys_ble][hid]")
{
    hid_fresh(0U, false);
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_select(0x04U));
    TEST_ASSERT_EQUAL_INT((int)KODE_OK,
                          (int)kode_sys_ble_hid_select(KODE_SYS_BLE_HID_MOUSE | KODE_SYS_BLE_HID_KEYBOARD));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KINDS, kdrv_ble_hid_fix());
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_select(0U));
    hid_done();
}

TEST_CASE("the time to pair is asked of AO_Radio", "[kode_sys_ble][hid]")
{
    const QEvt *e = NULL;

    hid_fresh(0U, false);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_pair_open());
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Radio->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)BLE_PAIR_OPEN_REQ_SIG, (uint32_t)e->sig);
    hid_done();
}

TEST_CASE("the time to pair is refused, not asserted on, when AO_Radio's queue has no room",
          "[kode_sys_ble][hid]")
{
    static const QEvt filler = QEVT_INITIALIZER(BLE_STATE_REQ_SIG);

    hid_fresh(KODE_SYS_BLE_HID_MOUSE, false);
    /* Fill to the door's margin: the slots left are the knocks' and the AO's own. */
    while ( (UBaseType_t)KQP_RADIO_POST_MARGIN < uxQueueSpacesAvailable(AO_Radio->eQueue) )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    UBaseType_t const waiting = uxQueueMessagesWaiting(AO_Radio->eQueue);

    TEST_ASSERT_EQUAL_INT((int)KODE_NO_RESOURCES, (int)kode_sys_ble_pair_open());
    TEST_ASSERT_EQUAL_UINT_MESSAGE((unsigned)waiting, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue),
                                   "the refused request took a slot");
    hid_done();
}

TEST_CASE("an AO subscribes to the pairings, and one that is not started is refused",
          "[kode_sys_ble][hid]")
{
    extern QSubscrList *QActive_subscrList_;

    hid_fresh(0U, false);
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_pair_subscribe(NULL));
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_pair_subscribe(ao_spy()));
    TEST_ASSERT_TRUE(QPSet_hasElement(&QActive_subscrList_[BLE_PAIR_SIG].set,
                                      (uint_fast8_t)ao_spy()->prio));
    QActive_unsubscribe(ao_spy(), BLE_PAIR_SIG);
    hid_done();
}

TEST_CASE("a tap is one event that carries the key and says its release follows",
          "[kode_sys_ble][hid]")
{
    hid_fresh(KODE_SYS_BLE_HID_KEYBOARD, true);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_hid_tap(0x02U, 0x04U));

    const BleHidReportEvt *const r = report_head();

    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, r->kind);
    TEST_ASSERT_TRUE_MESSAGE(r->tap, "the key would stay down");
    TEST_ASSERT_EQUAL_HEX8(0x02U, r->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04U, r->data[KDRV_BLE_HID_KEYS_AT]);
    QF_gc(&r->super);
    /* Two slots went with it: fourteen are left. */
    TEST_ASSERT_EQUAL_INT((int)KODE_OK,
                          (int)kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD,
                                                  (uint8_t)(KODE_SYS_BLE_HID_RING_SLOTS - 2U)));
    TEST_ASSERT_EQUAL_INT((int)KODE_NO_RESOURCES, (int)kode_sys_ble_hid_tap(0U, 0x05U));
    hid_done();
}

TEST_CASE("a tap of no key, or of one the map does not declare, is refused and posts nothing",
          "[kode_sys_ble][hid]")
{
    hid_fresh(KODE_SYS_BLE_HID_KEYBOARD, true);
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_tap(0U, 0U));
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED,
                          (int)kode_sys_ble_hid_tap(0U, (uint8_t)(KODE_SYS_BLE_HID_KEY_MAX + 1U)));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue));
    TEST_ASSERT_EQUAL_INT((int)KODE_OK,
                          (int)kode_sys_ble_hid_tap(0U, (uint8_t)KODE_SYS_BLE_HID_KEY_MAX));
    hid_done();
}

TEST_CASE("a click is one event that carries the buttons and says their release follows",
          "[kode_sys_ble][hid]")
{
    hid_fresh(KODE_SYS_BLE_HID_MOUSE, true);
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_click(0U));
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)kode_sys_ble_hid_click(0x08U));
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_hid_click(0x01U));

    const BleHidReportEvt *const r = report_head();

    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_MOUSE, r->kind);
    TEST_ASSERT_TRUE(r->tap);
    TEST_ASSERT_EQUAL_HEX8(0x01U, r->data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, r->data[1]);
    QF_gc(&r->super);
    hid_done();
}

TEST_CASE("letting every key go is never refused for want of room; a press is",
          "[kode_sys_ble][hid]")
{
    static const uint8_t KEYS[KODE_SYS_BLE_HID_KEYS_MAX] = { 0x04U, 0U, 0U, 0U, 0U, 0U };
    uint8_t  kind = 0U;
    uint8_t  got[KDRV_BLE_HID_REPORT_MAX];
    uint16_t len  = 0U;

    hid_fresh(KODE_SYS_BLE_HID_MOUSE | KODE_SYS_BLE_HID_KEYBOARD, true);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK,
                          (int)kdrv_ble_hid_claim(KDRV_BLE_HID_KEYBOARD,
                                                  (uint8_t)KODE_SYS_BLE_HID_RING_SLOTS));
    TEST_ASSERT_EQUAL_INT((int)KODE_NO_RESOURCES, (int)kode_sys_ble_hid_keys(0U, KEYS));
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_hid_keys(0U, NULL));
    /* The same for a mouse report of no button and no movement. */
    TEST_ASSERT_EQUAL_INT((int)KODE_NO_RESOURCES, (int)kode_sys_ble_hid_mouse(0U, 1, 0, 0));
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kode_sys_ble_hid_mouse(0U, 0, 0, 0));
    /* The mouse has nothing on its way: its zeroes go now. The keyboard's wait. */
    TEST_ASSERT_TRUE(kdrv_ble_hid_peek(&kind, got, &len));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_MOUSE, kind);
    kdrv_ble_hid_pop(true);
    TEST_ASSERT_FALSE(kdrv_ble_hid_queued());
    hid_done();
}
