/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_ble_events.c
 * @brief The app's Bluetooth events: ble_written, ble_connected and ble_disconnected.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "kaos.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "qf_support.h"

/** @brief The longest a case waits for a listener, in ms. */
#define BLE_EV_WAIT_MS 4000U

/** @brief Bytes of a text a record keeps. */
#define BLE_EV_TEXT 8U

/** @brief Records a case can keep. */
#define BLE_EV_REC_MAX 8U

/** @brief Longest name or source a record keeps. */
#define BLE_EV_NAME 24U

/** @brief What a listener saw. */
typedef struct {
    char    name[BLE_EV_NAME];    /**< The event's name.             */
    char    source[BLE_EV_NAME];  /**< Its source.                   */
    int32_t value;                /**< Its value.                    */
    uint8_t text[BLE_EV_TEXT];    /**< Its text's first bytes, raw.  */
} ble_rec_t;

static ble_rec_t         s_rec[BLE_EV_REC_MAX];  /**< What the listener saw, in order. */
static volatile uint32_t s_rec_n;                /**< How many records.                */
static SemaphoreHandle_t s_given;                /**< Given per event.                 */

/** @brief Record the event and tell the case. */
static void listener_rec(const kode_event_t *e)
{
    const uint32_t i = s_rec_n;

    if ( BLE_EV_REC_MAX > i )
    {
        (void)snprintf(s_rec[i].name, BLE_EV_NAME, "%s", e->name);
        (void)snprintf(s_rec[i].source, BLE_EV_NAME, "%s", e->source);
        s_rec[i].value = e->value;
        (void)memcpy(s_rec[i].text, e->text, BLE_EV_TEXT);
    }
    s_rec_n = i + 1U;
    (void)xSemaphoreGive(s_given);
}

/** @brief Start the layer with real locks and forget what earlier cases recorded. */
static void ble_events_up(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    if ( NULL == s_given )
    {
        s_given = xSemaphoreCreateCounting(BLE_EV_REC_MAX, 0U);
    }
    while ( xSemaphoreTake(s_given, 0U) == pdTRUE )
    {
    }
    (void)memset(s_rec, 0, sizeof(s_rec));
    s_rec_n = 0U;
}

/** @brief Wait for @p n more events; false if they do not all come. */
static bool ble_rec_wait(uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; (i < n) && ok; i++ )
    {
        ok = (xSemaphoreTake(s_given, pdMS_TO_TICKS(BLE_EV_WAIT_MS)) == pdTRUE);
    }
    return ok;
}

/** @brief Whether the layer's Active Object hears @p sig. */
static bool ble_subscribed(enum_t sig)
{
    extern QSubscrList *QActive_subscrList_;

    return QPSet_hasElement(&QActive_subscrList_[sig].set, (uint_fast8_t)AO_KodeStandard->prio);
}

TEST_CASE("a write of 00 41 00 to 'cmd' arrives as ble_written, source cmd, value 3, the bytes "
          "in text", "[kode_ble_events]")
{
    static const uint8_t bytes[3] = { 0x00U, 0x41U, 0x00U };

    ble_events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_BLE_WRITTEN, &listener_rec));
    TEST_ASSERT_TRUE(ble_subscribed(BLE_APP_WRITE_SIG));
    TEST_ASSERT_TRUE(ble_subscribed(BLE_APP_LINK_SIG));
    TEST_ASSERT_TRUE(ble_subscribed(BLE_APP_TAKEN_SIG));

    BleAppWriteEvt *const w = Q_NEW(BleAppWriteEvt, BLE_APP_WRITE_SIG);

    w->chr = 1U;
    (void)snprintf(w->name, sizeof(w->name), "%s", "cmd");
    w->len = (uint16_t)sizeof(bytes);
    (void)memcpy(w->data, bytes, sizeof(bytes));
    (void)memset(&w->data[sizeof(bytes)], 0x5A, sizeof(w->data) - sizeof(bytes));
    QACTIVE_PUBLISH(&w->super, (void *)0);

    TEST_ASSERT_TRUE(ble_rec_wait(1U));
    TEST_ASSERT_EQUAL_STRING("ble_written", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("cmd", s_rec[0].source);
    TEST_ASSERT_EQUAL_INT32(3, s_rec[0].value);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, s_rec[0].text, sizeof(bytes));
    TEST_ASSERT_EQUAL_HEX8_MESSAGE(0x00U, s_rec[0].text[3], "one NUL after the bytes");
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_BLE_WRITTEN, &listener_rec));
}

TEST_CASE("each edge of the link arrives as ble_connected or ble_disconnected",
          "[kode_ble_events]")
{
    ble_events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_BLE_CONNECTED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_BLE_DISCONNECTED, &listener_rec));

    BleAppLinkEvt *up = Q_NEW(BleAppLinkEvt, BLE_APP_LINK_SIG);

    up->connected = true;
    QACTIVE_PUBLISH(&up->super, (void *)0);
    BleAppLinkEvt *down = Q_NEW(BleAppLinkEvt, BLE_APP_LINK_SIG);

    down->connected = false;
    QACTIVE_PUBLISH(&down->super, (void *)0);

    TEST_ASSERT_TRUE(ble_rec_wait(2U));
    TEST_ASSERT_EQUAL_STRING("ble_connected", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("ble_disconnected", s_rec[1].name);
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_BLE_CONNECTED, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_BLE_DISCONNECTED, &listener_rec));
}

TEST_CASE("a value a central took from 'log' arrives as ble_taken, source log, value the "
          "values left", "[kode_ble_events]")
{
    ble_events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_BLE_TAKEN, &listener_rec));

    BleAppTakenEvt *const t = Q_NEW(BleAppTakenEvt, BLE_APP_TAKEN_SIG);

    t->chr  = 0U;
    t->left = 3U;
    (void)snprintf(t->name, sizeof(t->name), "%s", "log");
    QACTIVE_PUBLISH(&t->super, (void *)0);

    TEST_ASSERT_TRUE(ble_rec_wait(1U));
    TEST_ASSERT_EQUAL_STRING("ble_taken", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("log", s_rec[0].source);
    TEST_ASSERT_EQUAL_INT32(3, s_rec[0].value);
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_BLE_TAKEN, &listener_rec));
}

TEST_CASE("a pairing's code arrives as ble_pair_code, the code as value and as six digits, "
          "and its end as ble_pair_done", "[kode_ble_events]")
{
    ble_events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_BLE_PAIR_CODE, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_add_listener(KODE_EVENT_BLE_PAIR_DONE, &listener_rec));
    TEST_ASSERT_TRUE(ble_subscribed(BLE_PAIR_SIG));

    BlePairEvt *show = Q_NEW(BlePairEvt, BLE_PAIR_SIG);
    show->phase   = (uint8_t)KODE_SYS_BLE_PAIR_SHOW;
    show->passkey = 4217U;
    show->outcome = 0U;
    QACTIVE_PUBLISH(&show->super, NULL);
    BlePairEvt *done = Q_NEW(BlePairEvt, BLE_PAIR_SIG);
    done->phase   = (uint8_t)KODE_SYS_BLE_PAIR_DONE;
    done->passkey = 0U;
    done->outcome = (uint8_t)KODE_SYS_BLE_PAIR_OK;
    QACTIVE_PUBLISH(&done->super, NULL);
    BlePairEvt *lost = Q_NEW(BlePairEvt, BLE_PAIR_SIG);
    lost->phase   = (uint8_t)KODE_SYS_BLE_PAIR_DONE;
    lost->passkey = 0U;
    lost->outcome = (uint8_t)(KODE_SYS_BLE_PAIR_OK + 1U);
    QACTIVE_PUBLISH(&lost->super, NULL);

    TEST_ASSERT_TRUE(ble_rec_wait(3U));
    TEST_ASSERT_EQUAL_STRING("ble_pair_code", s_rec[0].name);
    TEST_ASSERT_EQUAL_INT32(4217, s_rec[0].value);
    TEST_ASSERT_EQUAL_STRING("004217", (const char *)s_rec[0].text);
    TEST_ASSERT_EQUAL_STRING("ble_pair_done", s_rec[1].name);
    TEST_ASSERT_EQUAL_INT32(1, s_rec[1].value);
    TEST_ASSERT_EQUAL_STRING("ble_pair_done", s_rec[2].name);
    TEST_ASSERT_EQUAL_INT32(0, s_rec[2].value);
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_BLE_PAIR_CODE, &listener_rec));
    TEST_ASSERT_TRUE(kode_event_remove_listener(KODE_EVENT_BLE_PAIR_DONE, &listener_rec));
}
