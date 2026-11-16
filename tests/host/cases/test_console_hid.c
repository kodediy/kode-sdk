/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_hid.c
 * @brief The HID verbs, from the wire: `ble_hid_info`, `ble_hid_mouse`, `ble_hid_wheel` and `ble_hid_key`.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kdrv_ble_hid.h"
#include "kdrv_ble_hid_table.h"
#include "kqp.h"
#include "owners_support.h"
#include "qf_support.h"
#include "radio_knocks.h"

/* The command watchdog, in QP ticks, repeated rather than imported for the
 * reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/**
 * @brief The console up and idle, AO_Radio's queue empty, and a table that serves @p kinds.
 * @param kinds  The kinds chosen; 0 for a board that asked for nothing.
 * @param hosted Whether a host is subscribed to every one of them.
 */
static void console_up(uint8_t kinds, bool hosted)
{
    const QEvt *e;

    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    owners_up();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);

    qf_support_give_queue(AO_Radio);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    radio_knocks_reset();   /* what AO_Radio's start does: a let-go with the ring full knocks */
    kdrv_ble_hid_select_forget();
    if ( 0U != kinds )
    {
        TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)kdrv_ble_hid_select(kinds));
        TEST_ASSERT_EQUAL_UINT8(kinds, kdrv_ble_hid_fix());
    }
    if ( hosted )
    {
        kdrv_ble_hid_subscribed(KDRV_BLE_HID_MOUSE, true);
        kdrv_ble_hid_subscribed(KDRV_BLE_HID_KEYBOARD, true);
    }
    fake_klog_reset();
}

/** @brief Leave nothing of a case behind. */
static void console_done(void)
{
    const QEvt *e;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    kdrv_ble_hid_select_forget();
}

/**
 * @brief Hand the console one line, as the port @p src delivers it.
 * @param src  KODE_CONSOLE_SRC_SERIAL, _NET or _SCHED.
 * @param text The line.
 */
static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Hand the console one line from the cable. */
static void type_line(const char *text)
{
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, text);
}

/** @brief The first console line holding @p needle, or NULL. */
static const char *line_with(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return l;
        }
    }
    return NULL;
}

/**
 * @brief Take the HID report the console posted to AO_Radio.
 * @param out Receives a copy of it.
 * @return false when the head of the queue is no HID report.
 */
static bool posted_report(BleHidReportEvt *out)
{
    const QEvt *e   = NULL;
    bool        got = false;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)BLE_HID_REPORT_REQ_SIG == e->sig )
        {
            *out = *(const BleHidReportEvt *)(const void *)e;
            got  = true;
        }
        QF_gc(e);
    }
    return got;
}

TEST_CASE("ble_hid_info says off and not ready on a board that asked for nothing",
          "[console_hid]")
{
    console_up(0U, false);
    type_line("ble_hid_info");
    TEST_ASSERT_NOT_NULL(line_with(">|ble_hid kinds=off ready=off sent=0 dropped=0"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    console_done();
}

TEST_CASE("ble_hid_info names the kinds served and whether a host takes them", "[console_hid]")
{
    console_up(KDRV_BLE_HID_KINDS, true);
    type_line("ble_hid_info");
    TEST_ASSERT_NOT_NULL(line_with(">|ble_hid kinds=both ready=on "));
    console_done();
}

TEST_CASE("ble_hid_mouse moves by the difference of its directions and holds no button",
          "[console_hid]")
{
    BleHidReportEvt r;

    console_up(KDRV_BLE_HID_MOUSE, true);
    type_line("ble_hid_mouse right=20 left=5 up=7");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_TRUE(posted_report(&r));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_MOUSE, r.kind);
    TEST_ASSERT_FALSE(r.tap);
    TEST_ASSERT_EQUAL_HEX8(0x00U, r.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x0FU, r.data[1]);
    TEST_ASSERT_EQUAL_HEX8(0xF9U, r.data[2]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, r.data[3]);
    console_done();
}

TEST_CASE("ble_hid_mouse clicks its buttons unless told to hold them", "[console_hid]")
{
    BleHidReportEvt r;

    console_up(KDRV_BLE_HID_MOUSE, true);
    type_line("ble_hid_mouse buttons=1");
    TEST_ASSERT_TRUE(posted_report(&r));
    TEST_ASSERT_EQUAL_HEX8(0x01U, r.data[0]);
    TEST_ASSERT_TRUE_MESSAGE(r.tap, "a click was left held");
    type_line("ble_hid_mouse buttons=2 hold=on");
    TEST_ASSERT_TRUE(posted_report(&r));
    TEST_ASSERT_EQUAL_HEX8(0x02U, r.data[0]);
    TEST_ASSERT_FALSE(r.tap);
    console_done();
}

TEST_CASE("ble_hid_wheel turns the wheel and moves nothing else", "[console_hid]")
{
    BleHidReportEvt r;

    console_up(KDRV_BLE_HID_MOUSE, true);
    type_line("ble_hid_wheel down=3");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_TRUE(posted_report(&r));
    TEST_ASSERT_EQUAL_HEX8(0x00U, r.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, r.data[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, r.data[2]);
    TEST_ASSERT_EQUAL_HEX8(0xFDU, r.data[3]);
    console_done();
}

TEST_CASE("ble_hid_key presses one key with its modifiers and lets it go", "[console_hid]")
{
    BleHidReportEvt r;

    console_up(KDRV_BLE_HID_KEYBOARD, true);
    type_line("ble_hid_key code=4 mods=2");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_TRUE(posted_report(&r));
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYBOARD, r.kind);
    TEST_ASSERT_TRUE(r.tap);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HID_KEYS_LEN, r.len);
    TEST_ASSERT_EQUAL_HEX8(0x02U, r.data[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04U, r.data[KDRV_BLE_HID_KEYS_AT]);
    console_done();
}

TEST_CASE("the HID verbs say off, kind, no_host or busy for a report that cannot go",
          "[console_hid]")
{
    console_up(0U, false);
    type_line("ble_hid_mouse right=1");
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=off"));

    console_up(KDRV_BLE_HID_MOUSE, true);
    type_line("ble_hid_key code=4");
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=kind"));

    console_up(KDRV_BLE_HID_MOUSE, false);
    type_line("ble_hid_mouse right=1");
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=no_host"));

    console_up(KDRV_BLE_HID_MOUSE, true);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK,
                          (int)kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, (uint8_t)KDRV_BLE_HID_RING_SLOTS));
    type_line("ble_hid_mouse right=1");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue));
    console_done();
}

TEST_CASE("a step past what one report carries, or a key of no code, is refused by the grammar",
          "[console_hid]")
{
    console_up(KDRV_BLE_HID_KINDS, true);
    type_line("ble_hid_mouse right=128");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=right"));
    type_line("ble_hid_key code=0");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=code"));
    /* Past the last key the report map declares, a host would take it for an error. */
    fake_klog_reset();
    type_line("ble_hid_key code=102");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=code"));
    fake_klog_reset();
    type_line("ble_hid_key code=101");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    type_line("ble_hid_key");
    TEST_ASSERT_NOT_NULL(line_with(">err code=2 msg=code"));
    TEST_ASSERT_EQUAL_UINT(1U, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue));
    console_done();
}

TEST_CASE("ble_hid_mouse with nothing to say lets the buttons go, with the ring full too",
          "[console_hid]")
{
    console_up(KDRV_BLE_HID_MOUSE, true);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK,
                          (int)kdrv_ble_hid_claim(KDRV_BLE_HID_MOUSE, (uint8_t)KDRV_BLE_HID_RING_SLOTS));
    type_line("ble_hid_mouse");
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">ok"), "a held button could not be let go");
    console_done();
}

TEST_CASE("a report is sent from the cable alone: the network and the scheduler are refused",
          "[console_hid]")
{
    console_up(KDRV_BLE_HID_KINDS, true);
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ble_hid_key code=4");
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=net"));
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ble_hid_mouse right=1");
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ble_hid_wheel up=1");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue),
                                   "a line off the cable reached the host's computer");
    /* What the board is doing is no secret to its owner's other ports. */
    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ble_hid_info");
    TEST_ASSERT_NOT_NULL(line_with(">|ble_hid kinds=both ready=on "));
    console_done();
}
