/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_sys_ble.c
 * @brief Bluetooth in kode_sys: the scan response, an AO's listen and the blocking listen.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_ble.h"
#include "khal_identity.h"
#include "khal_sync.h"
#include "khal_time.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"

/** @brief The listen the cases ask for, in ms. */
#define BLE_TEST_MS       1000U

/** @brief The bound the cases hand the blocking piece, in ms. */
#define BLE_TEST_WAIT_MS  2000U

/** @brief Posts that surely fill AO_Radio's queue. */
#define BLE_FILL_MAX      64U

/** @brief Timeouts in a row the allocation case runs. */
#define BLE_VALUE_TIMEOUTS  5U

/** @brief What the stand-in could not do, if anything; read once the call returned. */
static const char *s_hook_fault;

/** @brief The devices the callback was offered, in order. */
static kode_sys_ble_device_t s_kept[KODE_SYS_BLE_SCAN_MAX];

/** @brief How many devices the callback was offered. */
static unsigned s_kept_n;

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief How many requests wait for AO_Radio. */
static unsigned radio_pending(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue);
}

/** @brief A fresh case: AO_Radio's queue empty, the response back to the serial and no data. */
static void ble_fresh(void)
{
    qf_support_init();
    qf_support_give_queue(AO_Radio);
    radio_flush();
    fake_sem_reset();
    fake_time_reset();
    s_hook_fault = NULL;
    s_kept_n     = 0U;
    (void)memset(s_kept, 0, sizeof(s_kept));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_data(NULL, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_name(NULL));
    radio_flush();
}

/** @brief The request at the head of AO_Radio's queue, which must be a scan response. */
static const BleRspReqEvt *rsp_head(void)
{
    const QEvt *e = NULL;

    TEST_ASSERT_EQUAL_INT_MESSAGE(pdTRUE, xQueueReceive(AO_Radio->eQueue, &e, 0U),
                                  "nothing was posted to AO_Radio");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)BLE_RSP_REQ_SIG, (uint32_t)e->sig);
    return (const BleRspReqEvt *)(const void *)e;
}

/** @brief The callback: keep every device offered. */
static bool keep_dev(const kode_sys_ble_device_t *dev, void *ctx)
{
    (void)ctx;
    if ( KODE_SYS_BLE_SCAN_MAX > s_kept_n )
    {
        s_kept[s_kept_n] = *dev;
    }
    ++s_kept_n;
    return true;
}

/**
 * @brief Answer the listen at the head of AO_Radio's queue, as AO_Radio would.
 * @param n      Devices heard, written into the waiter's table.
 * @param prefix What their names start with.
 * @param rc     The answer's status.
 */
static void answer_head(unsigned n, const char *prefix, kode_status_t rc)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) != pdTRUE )
    {
        s_hook_fault = "nothing was posted to AO_Radio";
        return;
    }
    if ( (QSignal)RADIO_SCAN_ASK_SIG != e->sig )
    {
        s_hook_fault = "AO_Radio was posted something other than a scan";
        QF_gc(e);
        return;
    }

    const ScanAskEvt *const a = (const ScanAskEvt *)(const void *)e;
    ScanWaiter *const       w = a->wait;

    if ( (NULL == w) || ((uint8_t)KQP_SCAN_BLE != a->kind) || (BLE_TEST_MS != a->ms) )
    {
        s_hook_fault = "a task's listen came without its waiter, its kind or its length";
        QF_gc(e);
        return;
    }
    kdrv_ble_dev_t *const rows = (kdrv_ble_dev_t *)w->rows;
    unsigned              got  = 0U;

    for ( unsigned i = 0U; (i < n) && ((unsigned)w->cap > i); ++i )
    {
        (void)memset(&rows[i], 0, sizeof(rows[i]));
        (void)snprintf(rows[i].name, sizeof(rows[i].name), "%s-%u", prefix, i);
        rows[i].addr[0] = 0xC0U;
        rows[i].addr[5] = (uint8_t)(0xA0U + i);
        rows[i].rssi    = (int8_t)(-40 - (int)i);
        ++got;
    }
    w->rc  = (uint8_t)rc;
    w->got = (uint8_t)got;
    __atomic_store_n(&w->ack, a->gen, __ATOMIC_RELEASE);
    khal_sem_signal(w->done);
    QF_gc(e);
}

/** @brief AO_Radio, inside the wait: two devices. */
static void radio_answers_two(void)
{
    answer_head(2U, "dev", KODE_OK);
}

/** @brief AO_Radio, inside the wait: Bluetooth is off in Settings. */
static void radio_answers_off(void)
{
    answer_head(0U, "dev", KODE_DENIED);
}

/** @brief Answers given by radio_answers_late(), in order. */
static unsigned s_late_step;

/** @brief AO_Radio, late: the abandoned listen first, then the one asked now. */
static void radio_answers_late(void)
{
    if ( 0U == s_late_step )
    {
        answer_head(3U, "old", KODE_OK);
        s_late_step = 1U;
        fake_sem_wait_hook(&radio_answers_late);
    }
    else
    {
        answer_head(1U, "new", KODE_OK);
        s_late_step = 2U;
    }
}

/* ---- The scan response ------------------------------------------------------- */

TEST_CASE("a name and data that fit are posted whole, the other part kept", "[kode_sys_ble]")
{
    static const uint8_t DATA[4] = { 0xFFU, 0xFFU, 0x01U, 0x02U };
    const BleRspReqEvt  *r;

    ble_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_name("Dot-7"));
    r = rsp_head();
    TEST_ASSERT_EQUAL_STRING("Dot-7", r->name);
    TEST_ASSERT_EQUAL_UINT8(0U, r->len);
    QF_gc(&r->super);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_data(DATA, sizeof(DATA)));
    r = rsp_head();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Dot-7", r->name, "the data lost the name");
    TEST_ASSERT_EQUAL_UINT8(sizeof(DATA), r->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DATA, r->data, sizeof(DATA));
    QF_gc(&r->super);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_name(NULL));
    r = rsp_head();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", r->name, "NULL is the serial, sent as empty");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(sizeof(DATA), r->len, "the name lost the data");
    QF_gc(&r->super);
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

TEST_CASE("a name and data that do not fit together are refused and change nothing",
          "[kode_sys_ble]")
{
    static const uint8_t DATA[KQP_BLE_DATA_MAX] = { 0x4CU, 0x00U };
    const BleRspReqEvt  *r;

    ble_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_name("0123456789"));
    QF_gc(&rsp_head()->super);

    /* 12 bytes of name; data of 18 would make 32. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_set_data(DATA, 18U));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_ble_set_data(DATA, 17U), "exactly 31 bytes fit");
    r = rsp_head();
    TEST_ASSERT_EQUAL_STRING("0123456789", r->name);
    TEST_ASSERT_EQUAL_UINT8(17U, r->len);
    QF_gc(&r->super);

    /* The refused name leaves the kept one: the data still goes beside it. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_set_name("0123456789A"));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_data(DATA, 2U));
    r = rsp_head();
    TEST_ASSERT_EQUAL_STRING("0123456789", r->name);
    QF_gc(&r->super);
}

TEST_CASE("a name too long, a data of one byte or data with no bytes are refused",
          "[kode_sys_ble]")
{
    static const uint8_t ONE[1] = { 0x4CU };

    ble_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_set_name("0123456789012345678901234567890"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_set_data(ONE, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_set_data(NULL, 2U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_set_data(ONE, KQP_BLE_DATA_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

TEST_CASE("a name AO_Radio has no room for is refused, nothing kept", "[kode_sys_ble]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);
    const BleRspReqEvt *r;

    ble_fresh();
    for ( uint32_t guard = 0U;
         (BLE_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ble_set_name("lost"));
    radio_flush();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_data(NULL, 0U));
    r = rsp_head();
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", r->name, "a name nobody took was kept");
    QF_gc(&r->super);
}

/* ---- The listen: kode_sys_ble_scan() and kode_block_ble_scan() ---------------- */

TEST_CASE("a listen asked by no AO, or for 499 or 30001 ms, is refused and posts nothing",
          "[kode_sys_ble]")
{
    ble_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_scan(NULL, BLE_TEST_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_scan(AO_Radio, 499U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_scan(AO_Radio, 30001U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_ble_scan(&keep_dev, NULL, 499U, BLE_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_ble_scan(&keep_dev, NULL, 30001U, BLE_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_ble_scan(NULL, NULL, BLE_TEST_MS, BLE_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

TEST_CASE("a blocking listen offers each device AO_Radio wrote, its address as text",
          "[kode_sys_ble]")
{
    ble_fresh();
    fake_sem_wait_hook(&radio_answers_two);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_ble_scan(&keep_dev, NULL, BLE_TEST_MS, BLE_TEST_WAIT_MS));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(2U, s_kept_n);
    TEST_ASSERT_EQUAL_STRING("dev-0", s_kept[0].name);
    TEST_ASSERT_EQUAL_STRING("c0:00:00:00:00:a0", s_kept[0].address);
    TEST_ASSERT_EQUAL_INT8(-40, s_kept[0].rssi);
    TEST_ASSERT_EQUAL_STRING("c0:00:00:00:00:a1", s_kept[1].address);
    TEST_ASSERT_EQUAL_UINT32(BLE_TEST_WAIT_MS, fake_sem_last_wait_ms());
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());

    fake_sem_wait_hook(&radio_answers_off);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_ble_scan(&keep_dev, NULL, BLE_TEST_MS, BLE_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT(2U, s_kept_n);
}

TEST_CASE("test_ble_scan_late_answer_is_dropped", "[kode_sys_ble]")
{
    ble_fresh();
    s_late_step = 0U;

    /* Nobody answers in time: the wait runs out, and the request stays owed. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ble_scan(&keep_dev, NULL, BLE_TEST_MS, BLE_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());

    /* The owed answer never lands: the storage is not lent again, nothing is posted. */
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
                          kode_block_ble_scan(&keep_dev, NULL, BLE_TEST_MS, BLE_TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());
    TEST_ASSERT_EQUAL_UINT(0U, s_kept_n);

    /* It lands at last, tagged with the old generation, inside the next call's wait:
     * that call waits it out, asks again, and offers its own devices alone. */
    fake_sem_wait_hook(&radio_answers_late);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_ble_scan(&keep_dev, NULL, BLE_TEST_MS, BLE_TEST_WAIT_MS));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(2U, s_late_step);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_kept_n, "the abandoned answer's devices were offered");
    TEST_ASSERT_EQUAL_STRING("new-0", s_kept[0].name);
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
}

/* ---- The app's own service -------------------------------------------------- */

/** @brief Two characteristics, as an app declares them with KODE_SYS_BLE_SERVICE(). */
static const kode_sys_ble_char_t s_app_chars[] = {
    { "temp", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ | KODE_SYS_BLE_NOTIFY },
    { "cmd",  "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_WRITE },
};

/** @brief The service, built the way the macro builds it. */
static const kode_sys_ble_service_t s_app_svc = {
    "6e400001-b5a3-f393-e0a9-e50e24dcca9e", s_app_chars,
    sizeof(s_app_chars) / sizeof(s_app_chars[0]) };

/** @brief Five characteristics: one more than a service may have. */
static const kode_sys_ble_char_t s_app_five[] = {
    { "a", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ },
    { "b", "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ },
    { "c", "6e400004-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ },
    { "d", "6e400005-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ },
    { "e", "6e400006-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ },
};

/** @brief The service of five. */
static const kode_sys_ble_service_t s_app_svc_five = {
    "6e400001-b5a3-f393-e0a9-e50e24dcca9e", s_app_five, 5U };

/**
 * @brief The request at the head of AO_Radio's queue, which must be a value.
 * @return It; the case gives it back with QF_gc().
 */
static const BleAppValueReqEvt *value_head(void)
{
    const QEvt *e = NULL;

    TEST_ASSERT_EQUAL_INT_MESSAGE(pdTRUE, xQueueReceive(AO_Radio->eQueue, &e, 0U),
                                  "nothing was posted to AO_Radio");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)BLE_APP_VALUE_REQ_SIG, (uint32_t)e->sig);
    return (const BleAppValueReqEvt *)(const void *)e;
}

/** @brief What the stand-in answers a blocking value with. */
static kode_status_t s_value_rc;

/** @brief AO_Radio, inside the wait: answer the value at the head with s_value_rc. */
static void radio_answers_value(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Radio->eQueue, &e, 0U) != pdTRUE )
    {
        s_hook_fault = "nothing was posted to AO_Radio";
        return;
    }
    const BleAppValueReqEvt *const r = (const BleAppValueReqEvt *)(const void *)e;

    if ( ((QSignal)BLE_APP_VALUE_REQ_SIG != e->sig) || (NULL == r->wait) )
    {
        s_hook_fault = "a task's value came without its waiter";
        QF_gc(e);
        return;
    }
    r->wait->rc = (uint8_t)s_value_rc;
    __atomic_store_n(&r->wait->ack, r->gen, __ATOMIC_RELEASE);
    khal_sem_signal(r->wait->done);
    QF_gc(e);
}

TEST_CASE("an app's value and notification are checked at once, and those that pass are "
          "posted to AO_Radio as copies", "[kode_sys_ble]")
{
    char                            why[KODE_SYS_BLE_FAULT_MAX];
    const BleAppValueReqEvt        *v;

    ble_fresh();
    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_app_svc);
    TEST_ASSERT_FALSE(kode_sys_ble_service_fault(why, sizeof(why)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_set_value("temp", "1", 1U));
    v = value_head();
    TEST_ASSERT_NULL(v->wait);
    TEST_ASSERT_EQUAL_UINT8(KQP_BLE_APP_OP_SET, v->op);
    TEST_ASSERT_EQUAL_STRING("temp", v->name);
    TEST_ASSERT_EQUAL_UINT16(1U, v->len);
    TEST_ASSERT_EQUAL_HEX8('1', v->data[0]);
    QF_gc(&v->super);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_notify("temp", "2", 1U));
    v = value_head();
    TEST_ASSERT_EQUAL_UINT8(KQP_BLE_APP_OP_NOTIFY, v->op);
    TEST_ASSERT_EQUAL_HEX8('2', v->data[0]);
    QF_gc(&v->super);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_notify("cmd", "2", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_set_value("nope", "2", 1U));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());

    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_app_svc_five);
    TEST_ASSERT_TRUE(kode_sys_ble_service_fault(why, sizeof(why)));
    TEST_ASSERT_EQUAL_STRING("the service has 5 characteristics; at most 4", why);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_notify("a", "2", 1U));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
    fake_ble_app_service_set(NULL);
    TEST_ASSERT_FALSE(kode_sys_ble_service_fault(why, sizeof(why)));
}

/** @brief A queued characteristic beside a plain one. */
static const kode_sys_ble_char_t s_queue_chars[] = {
    { "log",  "6e400005-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ | KODE_SYS_BLE_QUEUED },
    { "temp", "6e400006-b5a3-f393-e0a9-e50e24dcca9e", KODE_SYS_BLE_READ },
};

/** @brief Their service. */
static const kode_sys_ble_service_t s_queue_svc = {
    "6e400004-b5a3-f393-e0a9-e50e24dcca9e", s_queue_chars,
    sizeof(s_queue_chars) / sizeof(s_queue_chars[0]) };

TEST_CASE("a push and a clear are posted with their operation, the room is read at once, "
          "and a characteristic with no queue is refused all three", "[kode_sys_ble]")
{
    const BleAppValueReqEvt *v;
    size_t                   room = 0U;

    ble_fresh();
    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_queue_svc);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_queue_push("log", "ab", 2U));
    v = value_head();
    TEST_ASSERT_NULL(v->wait);
    TEST_ASSERT_EQUAL_UINT8(KQP_BLE_APP_OP_PUSH, v->op);
    TEST_ASSERT_EQUAL_STRING("log", v->name);
    TEST_ASSERT_EQUAL_UINT16(2U, v->len);
    QF_gc(&v->super);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_queue_clear("log"));
    v = value_head();
    TEST_ASSERT_EQUAL_UINT8(KQP_BLE_APP_OP_CLEAR, v->op);
    TEST_ASSERT_EQUAL_UINT16(0U, v->len);
    QF_gc(&v->super);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_queue_room("log", &room));
    TEST_ASSERT_EQUAL_UINT(KODE_SYS_BLE_QUEUE_SLOTS, room);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_queue_push("log", "ab", 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_queue_push("temp", "ab", 2U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_queue_clear("temp"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_queue_room("temp", &room));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_queue_room("log", NULL));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
    fake_ble_app_service_set(NULL);
}

TEST_CASE("a value command AO_Radio's queue has no room for is NO_RESOURCES and posts nothing",
          "[kode_sys_ble]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    ble_fresh();
    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_app_svc);
    for ( uint32_t guard = 0U;
         (BLE_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    unsigned const before = radio_pending();

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ble_notify("temp", "2", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_ble_set_value("temp", "2", 1U));
    TEST_ASSERT_EQUAL_UINT(before, radio_pending());
    radio_flush();
    fake_ble_app_service_set(NULL);
}

TEST_CASE("a blocking value returns AO_Radio's answer, the full ring's included", "[kode_sys_ble]")
{
    ble_fresh();
    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_app_svc);

    s_value_rc = KODE_OK;
    fake_sem_wait_hook(&radio_answers_value);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_ble_value("temp", KQP_BLE_APP_OP_NOTIFY, "2", 1U));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);

    s_value_rc = KODE_NO_RESOURCES;
    fake_sem_wait_hook(&radio_answers_value);
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_block_ble_value("temp", KQP_BLE_APP_OP_NOTIFY, "2", 1U));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);

    /* Refused at once, nothing posted: the table says no. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_block_ble_value("cmd", KQP_BLE_APP_OP_NOTIFY, "2", 1U));
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());

    /* Nobody answers: a timeout, and the request stays with AO_Radio. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ble_value("temp", KQP_BLE_APP_OP_SET, "2", 1U));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());
    radio_flush();
    fake_ble_app_service_set(NULL);
}

/** @brief The answers radio_answers_in_turn() gives, oldest request first. */
static kode_status_t s_value_turns[2];

/** @brief How many of s_value_turns it has given. */
static unsigned s_value_turn;

/**
 * @brief AO_Radio, inside the wait: answer the oldest value with the next of
 *        s_value_turns, and come back for the next one in the next wait.
 */
static void radio_answers_in_turn(void)
{
    if ( (sizeof(s_value_turns) / sizeof(s_value_turns[0])) <= s_value_turn )
    {
        s_hook_fault = "more requests than answers";
        return;
    }
    s_value_rc = s_value_turns[s_value_turn];
    s_value_turn++;
    radio_answers_value();
    if ( radio_pending() > 0U )
    {
        fake_sem_wait_hook(&radio_answers_in_turn);
    }
}

TEST_CASE("the late answer to a call that timed out is dropped, and the next call gets its own",
          "[kode_sys_ble]")
{
    ble_fresh();
    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_app_svc);

    /* Nobody answers: the request stays with AO_Radio. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ble_value("temp", KQP_BLE_APP_OP_NOTIFY, "1", 1U));
    TEST_ASSERT_EQUAL_UINT(1U, radio_pending());

    /* AO_Radio answers it late, ring full, and then the next call's, OK. */
    s_value_turns[0] = KODE_NO_RESOURCES;
    s_value_turns[1] = KODE_OK;
    s_value_turn     = 0U;
    fake_sem_wait_hook(&radio_answers_in_turn);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_ble_value("temp", KQP_BLE_APP_OP_NOTIFY, "2", 1U));
    TEST_ASSERT_NULL_MESSAGE(s_hook_fault, s_hook_fault);
    TEST_ASSERT_EQUAL_UINT(2U, s_value_turn);
    TEST_ASSERT_EQUAL_UINT(0U, radio_pending());
    fake_ble_app_service_set(NULL);
}

TEST_CASE("blocking values that time out again and again allocate nothing", "[kode_sys_ble]")
{
    ble_fresh();
    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_app_svc);

    /* The first call makes the door's semaphore and lock, if no case did. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ble_value("temp", KQP_BLE_APP_OP_NOTIFY, "1", 1U));
    radio_flush();
    unsigned const psram = fake_psram_alloc_count();
    unsigned const sems  = fake_sem_creates();

    for ( uint32_t i = 0U; BLE_VALUE_TIMEOUTS > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ble_value("temp", KQP_BLE_APP_OP_NOTIFY, "1", 1U));
        radio_flush();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(psram, fake_psram_alloc_count(), "a timeout took PSRAM");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(sems, fake_sem_creates(), "a timeout made a semaphore");
    fake_ble_app_service_set(NULL);
}

TEST_CASE("a blocking value at a full queue tries again for a second, then times out",
          "[kode_sys_ble]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    ble_fresh();
    /* cppcheck-suppress misra-c2012-11.3 ; the two types are asserted equal in kode_sys_ble.c */
    fake_ble_app_service_set((const kdrv_ble_app_service_t *)&s_app_svc);
    for ( uint32_t guard = 0U;
         (BLE_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    unsigned const before = radio_pending();
    uint32_t const t0     = khal_time_ms();

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kode_block_ble_value("temp", KQP_BLE_APP_OP_NOTIFY, "2", 1U));
    TEST_ASSERT_EQUAL_UINT(before, radio_pending());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_SYS_BLE_VALUE_WAIT_MS, khal_time_ms() - t0);
    radio_flush();
    fake_ble_app_service_set(NULL);
}

TEST_CASE("kode_sys_ble_subscribe() makes its AO hear the writes and the link, and refuses no AO",
          "[kode_sys_ble]")
{
    static QActive       idle;   /* never started: priority 0 */
    static BleAppLinkEvt link;

    qf_support_init();
    QActive * const spy = ao_spy();
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_subscribe(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_subscribe(&idle));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_subscribe(spy));
    link.super     = (QEvt)QEVT_INITIALIZER(BLE_APP_LINK_SIG);
    link.connected = true;
    QACTIVE_PUBLISH(&link.super, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(BLE_APP_LINK_SIG, (int)ao_spy_next_sig());
    ao_spy_drain();
    QActive_unsubscribe(spy, BLE_APP_LINK_SIG);
    QActive_unsubscribe(spy, BLE_APP_WRITE_SIG);
    QActive_unsubscribe(spy, BLE_APP_TAKEN_SIG);
}
