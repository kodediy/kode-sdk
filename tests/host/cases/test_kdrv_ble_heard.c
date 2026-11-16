/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ble_heard.c
 * @brief The advertisement record built from a report's values, and its bounded hand-over ring.
 *
 * The real driver, with no mock. The ring is process state that no reset
 * clears, so the cases that need it before kdrv_ble_heard_init() has worked come
 * first in the file and nothing else in the suite initialises it earlier.
 * test_radio_heard.c, the cases of AO_Radio's door, sorts after this file for
 * that reason.
 */
#include <string.h>

#include "unity.h"
#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_ble_heard.h"
#include "kdrv_ble.h"
#include "qf_support.h"
#include "radio_heard.h"

/** @brief A payload longer than a legacy advertisement's. */
#define LONG_PAYLOAD_LEN  40U

/** @brief Records fed past a full ring. */
#define OVERFILL_EXTRA    5U

/** @brief Records the order case feeds. */
#define ORDER_COUNT       3U

/** @brief HCI event type with no name. */
#define HCI_EVT_UNNAMED   9U

/** @brief Bytes in the short payload. */
#define SHORT_PAYLOAD_LEN 3U

/** @brief The case's setUp: the ring exists and holds nothing. */
static void fresh(void)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_heard_init());
    kdrv_ble_heard_forget();
}

/** @brief A record whose first address byte and length say which one it is. */
static kdrv_ble_heard_t marked(uint8_t n)
{
    static const uint8_t addr[KDRV_BLE_HEARD_ADDR_LEN] = { 1U, 2U, 3U, 4U, 5U, 6U };
    kdrv_ble_heard_t r;

    kdrv_ble_heard_make(addr, 0U, 0U, -60, NULL, 0U, &r);
    r.addr[0] = n;
    return r;
}

TEST_CASE("a record fed before init is refused", "[kdrv_ble_heard]")
{
    kdrv_ble_heard_t r = marked(1U);
    kdrv_ble_heard_t o;

    TEST_ASSERT_FALSE(kdrv_ble_heard_feed(&r));
    TEST_ASSERT_FALSE(kdrv_ble_heard_next(&o));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_ble_heard_total());
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_ble_heard_dropped());
}

/** @brief Set by the hear job the door below hands out: none may go. */
static bool s_job_sent;

/** @brief The worker that must not be asked. */
static kode_status_t job_must_not_go(void *ctx, bool on)
{
    (void)ctx;
    (void)on;
    s_job_sent = true;
    return KODE_OK;
}

/*
 * The door's answer when the ring cannot be had. The door alone, with no
 * AO_Radio behind it: bringing the AO up here would leave it holding what the
 * cases that run before the radio's own expect free, and the ring, once made
 * by the cases below, is made for good.
 */
TEST_CASE("test_no_ring_is_memory", "[kdrv_ble_heard][radio_heard]")
{
    RadioHeard           door;
    BleHeardReqEvt const req = { QEVT_INITIALIZER(BLE_HEARD_REQ_SIG), NULL, true };
    BleHeardReqEvt       ask = req;
    kdrv_ble_heard_t     rec = marked(1U);

    qf_support_init();
    ao_spy_drain();
    ask.who    = ao_spy();
    s_job_sent = false;
    radio_heard_init(&door, &job_must_not_go, NULL);

    /* The first start to be accepted makes the ring: PSRAM refuses it. */
    fake_psram_refuse(1U);
    radio_heard_request(&door, &ask);

    const QEvt *const e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no answer reached the asker");
    TEST_ASSERT_EQUAL_INT((int)BLE_HEARD_STATE_SIG, (int)e->sig);
    TEST_ASSERT_FALSE(((BleHeardStateEvt const *)(const void *)e)->on);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KBLE_HEARD_MEMORY, ((BleHeardStateEvt const *)(const void *)e)->why);
    TEST_ASSERT_NULL(ao_spy_take());
    TEST_ASSERT_FALSE_MESSAGE(s_job_sent, "a hear job went out with no ring");
    TEST_ASSERT_NULL_MESSAGE(door.who, "the door was taken with no ring");
    TEST_ASSERT_FALSE_MESSAGE(door.scanning || door.job, "the door believes a scan is on");

    /* The ring was not made: a record has nowhere to go. */
    TEST_ASSERT_FALSE(kdrv_ble_heard_feed(&rec));
    ao_spy_drain();
}

TEST_CASE("make reverses the address", "[kdrv_ble_heard]")
{
    static const uint8_t le[KDRV_BLE_HEARD_ADDR_LEN] = { 1U, 2U, 3U, 4U, 5U, 6U };
    static const uint8_t be[KDRV_BLE_HEARD_ADDR_LEN] = { 6U, 5U, 4U, 3U, 2U, 1U };
    kdrv_ble_heard_t r;

    kdrv_ble_heard_make(le, 0U, 0U, -50, NULL, 0U, &r);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(be, r.addr, KDRV_BLE_HEARD_ADDR_LEN);
    TEST_ASSERT_EQUAL_INT8(-50, r.rssi);
}

TEST_CASE("make maps address types", "[kdrv_ble_heard]")
{
    static const uint8_t le[KDRV_BLE_HEARD_ADDR_LEN] = { 0U };
    kdrv_ble_heard_t r;

    kdrv_ble_heard_make(le, 0U, 0U, 0, NULL, 0U, &r);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_ADDR_PUBLIC, r.addr_type);
    kdrv_ble_heard_make(le, 1U, 0U, 0, NULL, 0U, &r);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_ADDR_RANDOM, r.addr_type);
    kdrv_ble_heard_make(le, 2U, 0U, 0, NULL, 0U, &r);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_ADDR_PUBLIC, r.addr_type);
    kdrv_ble_heard_make(le, 3U, 0U, 0, NULL, 0U, &r);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_ADDR_RANDOM, r.addr_type);
}

TEST_CASE("make maps pdus", "[kdrv_ble_heard]")
{
    static const uint8_t le[KDRV_BLE_HEARD_ADDR_LEN] = { 0U };
    static const uint8_t want[] = {
        (uint8_t)KDRV_BLE_HEARD_PDU_CONN, (uint8_t)KDRV_BLE_HEARD_PDU_DIRECT, (uint8_t)KDRV_BLE_HEARD_PDU_SCAN,
        (uint8_t)KDRV_BLE_HEARD_PDU_NONCONN, (uint8_t)KDRV_BLE_HEARD_PDU_SCAN_RSP
    };
    kdrv_ble_heard_t r;

    for ( uint8_t i = 0U; sizeof(want) > i; i++ )
    {
        kdrv_ble_heard_make(le, 0U, i, 0, NULL, 0U, &r);
        TEST_ASSERT_EQUAL_UINT8(want[i], r.pdu);
    }
    kdrv_ble_heard_make(le, 0U, HCI_EVT_UNNAMED, 0, NULL, 0U, &r);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_HEARD_PDU_OTHER, r.pdu);
}

TEST_CASE("make cuts a long payload", "[kdrv_ble_heard]")
{
    static const uint8_t le[KDRV_BLE_HEARD_ADDR_LEN] = { 0U };
    uint8_t p[LONG_PAYLOAD_LEN];
    kdrv_ble_heard_t r;

    for ( uint8_t i = 0U; LONG_PAYLOAD_LEN > i; i++ )
    {
        p[i] = (uint8_t)(0x80U + i);
    }
    kdrv_ble_heard_make(le, 0U, 0U, 0, p, LONG_PAYLOAD_LEN, &r);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_HEARD_DATA_MAX, r.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(p, r.data, KDRV_BLE_HEARD_DATA_MAX);
}

TEST_CASE("make zeroes past len", "[kdrv_ble_heard]")
{
    static const uint8_t le[KDRV_BLE_HEARD_ADDR_LEN] = { 0U };
    static const uint8_t p[SHORT_PAYLOAD_LEN] = { 0xAAU, 0xBBU, 0xCCU };
    kdrv_ble_heard_t r;

    (void)memset(&r, 0xFF, sizeof(r));
    kdrv_ble_heard_make(le, 0U, 0U, 0, p, SHORT_PAYLOAD_LEN, &r);
    TEST_ASSERT_EQUAL_UINT8(SHORT_PAYLOAD_LEN, r.len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(p, r.data, SHORT_PAYLOAD_LEN);
    for ( uint8_t i = SHORT_PAYLOAD_LEN; KDRV_BLE_HEARD_DATA_MAX > i; i++ )
    {
        TEST_ASSERT_EQUAL_UINT8(0U, r.data[i]);
    }
}

TEST_CASE("feed then next gives the records in order", "[kdrv_ble_heard]")
{
    kdrv_ble_heard_t o;

    fresh();
    for ( uint8_t i = 0U; ORDER_COUNT > i; i++ )
    {
        kdrv_ble_heard_t const r = marked((uint8_t)(10U + i));
        TEST_ASSERT_TRUE(kdrv_ble_heard_feed(&r));
    }
    for ( uint8_t i = 0U; ORDER_COUNT > i; i++ )
    {
        TEST_ASSERT_TRUE(kdrv_ble_heard_next(&o));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(10U + i), o.addr[0]);
    }
    TEST_ASSERT_FALSE(kdrv_ble_heard_next(&o));
}

TEST_CASE("a full ring refuses and counts", "[kdrv_ble_heard]")
{
    kdrv_ble_heard_t o;
    uint32_t taken = 0U;

    fresh();
    for ( uint32_t i = 0U; (KDRV_BLE_HEARD_RING_LEN + OVERFILL_EXTRA) > i; i++ )
    {
        kdrv_ble_heard_t const r = marked((uint8_t)i);
        if ( kdrv_ble_heard_feed(&r) )
        {
            taken++;
        }
    }
    TEST_ASSERT_EQUAL_UINT32(KDRV_BLE_HEARD_RING_LEN, taken);
    TEST_ASSERT_EQUAL_UINT32(KDRV_BLE_HEARD_RING_LEN + OVERFILL_EXTRA, kdrv_ble_heard_total());
    TEST_ASSERT_EQUAL_UINT32(OVERFILL_EXTRA, kdrv_ble_heard_dropped());
    TEST_ASSERT_TRUE(kdrv_ble_heard_next(&o));
    TEST_ASSERT_EQUAL_UINT8(0U, o.addr[0]);
}

TEST_CASE("forget empties the ring and zeroes the counts", "[kdrv_ble_heard]")
{
    kdrv_ble_heard_t o;

    fresh();
    for ( uint32_t i = 0U; (KDRV_BLE_HEARD_RING_LEN + OVERFILL_EXTRA) > i; i++ )
    {
        kdrv_ble_heard_t const r = marked((uint8_t)i);
        (void)kdrv_ble_heard_feed(&r);
    }
    kdrv_ble_heard_forget();
    TEST_ASSERT_FALSE(kdrv_ble_heard_next(&o));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_ble_heard_total());
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_ble_heard_dropped());
}
