/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_heard.c
 * @brief AO_Radio's BLE advertisement door: the request, the hear job behind it and what reaches the asker.
 *
 * The real AO_Radio and the real kdrv_ble_heard ring; the stack is the fake one.
 * A hear job is held by the worker until a case ends it, and the host
 * task's part (a record in the ring, the hook's knock) is played by the case.
 * The case that needs a ring that cannot be made, test_no_ring_is_memory, is
 * in test_kdrv_ble_heard.c and runs the door alone: the ring exists for good once
 * any case has made it, and the cases of this file sort after that one.
 */
#include <stdio.h>
#include <string.h>

#include "esp_app_format.h"   /* the image and segment headers of a test image */

#include "radio_harness.h"

#include "kdrv_ble_heard.h"

/** @brief The length of a passive scan a case asks the scan doors for, ms. */
#define AD_SCAN_MS       2000U

/** @brief Records a case feeds. */
#define AD_TWO           2U
#define AD_THREE         3U

/** @brief Payload bytes of a case's records. */
#define AD_PAYLOAD_LEN   3U

/** @brief The first address byte of a record: which one it is. */
#define AD_MARK_BASE     0x10U

/** @brief The level a case's records report, dBm. */
#define AD_RSSI          (-61)

/** @brief The name the radio check looks for. */
#define AD_BEACON        "KODE-BEACON"

/** @brief Slots of a queue with no room left. */
#define AD_FULL          0U

/** @brief Fillers a case may need to leave a queue full. */
#define AD_FILL_MAX      64U

/** @brief The guard of the loops that bring Bluetooth up and down. */
#define AD_SETTLE_MAX    16U

/* ---- The fixture ------------------------------------------------------------ */

/** @brief Events the second asker's queue gave up, held until the case ends: the caller of ao_spy_other_take() recycles them. */
#define AD_OTHER_HELD_MAX  16U
static const QEvt *s_other_held[AD_OTHER_HELD_MAX];
static uint32_t    s_other_held_n;

/** @brief Recycle what the second asker's reads kept. */
static void other_release(void)
{
    for ( uint32_t i = 0U; s_other_held_n > i; ++i )
    {
        QF_gc(s_other_held[i]);
    }
    s_other_held_n = 0U;
}

/** @brief Empty both askers' queues. */
static void askers_drain(void)
{
    ao_spy_drain();
    ao_spy_other_drain();
    other_release();
}

/** @brief End the jobs of the link's Bluetooth steps the way the driver does, until the worker is free. */
static void ble_settle(void)
{
    for ( uint32_t guard = 0U; (AD_SETTLE_MAX > guard) && job_held(); ++guard )
    {
        switch ( (kdrv_radio_job_t)running_job() )
        {
            case KDRV_RADIO_JOB_BLE_START:
                finish(KDRV_RADIO_JOB_BLE_START, kdrv_ble_start());
                break;
            case KDRV_RADIO_JOB_BLE_ADV:
                finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
                break;
            default:
                finish_whatever_runs();
                break;
        }
    }
}

/** @brief The link up, Bluetooth on in Settings and the board on the air; both askers' queues empty. */
static void heard_up(void)
{
    radio_fresh();
    fake_ble_reset();
    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_heard_init());
    kdrv_ble_heard_forget();
    askers_drain();
    go_up();
    dispatch(BLE_START_REQ_SIG);
    ble_settle();
    TEST_ASSERT_TRUE_MESSAGE(ble_enabled(), "Bluetooth was not switched on");
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    askers_drain();
    fake_klog_reset();
}

/** @brief Leave Bluetooth off, the AO off and every mailbox empty. */
static void heard_end_case(void)
{
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    fake_nvs_reset();
    settle_to_off();
    storage_drain();
    net_drain();
    (void)boot_parked_taken();
    askers_drain();
    kdrv_ble_heard_forget();
    fake_ble_reset();
}

/** @brief Ask AO_Radio, and let it take the request. */
static void ask(QActive *who, bool on)
{
    TEST_ASSERT_TRUE(ble_heard_request(who, on));
    pump();
}

/** @brief The next event @p who holds, whatever it is, or NULL. */
static const QEvt *spy_next(QActive *who)
{
    const QEvt *e;

    if ( ao_spy() == who )
    {
        return ao_spy_take();
    }
    e = ao_spy_other_take();
    if ( (NULL != e) && (AD_OTHER_HELD_MAX > s_other_held_n) )
    {
        s_other_held[s_other_held_n++] = e;
    }
    return e;
}

/** @brief Check the next event of @p who is a state answer, and its fields. */
static void expect_state(QActive *who, bool on, kble_heard_why_t why, uint32_t heard, uint32_t dropped)
{
    const QEvt *const e = spy_next(who);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no answer reached the asker");
    TEST_ASSERT_EQUAL_INT((int)BLE_HEARD_STATE_SIG, (int)e->sig);

    BleHeardStateEvt const *const s = (BleHeardStateEvt const *)(const void *)e;

    TEST_ASSERT_EQUAL_INT(on, s->on);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)why, s->why);
    TEST_ASSERT_EQUAL_UINT32(heard, s->heard);
    TEST_ASSERT_EQUAL_UINT32(dropped, s->dropped);
}

/** @brief Check @p who holds nothing. */
static void expect_nothing(QActive *who)
{
    TEST_ASSERT_NULL_MESSAGE(spy_next(who), "the asker was told something");
}

/** @brief Check the next event of @p who is an advertisement, and return it. */
static const BleHeardEvt *take_heard(QActive *who)
{
    const QEvt *const e = spy_next(who);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no advertisement reached the asker");
    TEST_ASSERT_EQUAL_INT((int)BLE_HEARD_SIG, (int)e->sig);
    return (const BleHeardEvt *)(const void *)e;
}

/** @brief The hear job on the worker, and whether it starts or stops. */
static void expect_hear_job(bool on)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_HEAR, running_job());
    TEST_ASSERT_EQUAL_INT(on, held_req()->on);
}

/** @brief Start from @p who and let the stack say it listens. */
static void start_stream(QActive *who)
{
    ask(who, true);
    expect_hear_job(true);
    expect_nothing(who);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(who, true, KBLE_HEARD_OK, 0U, 0U);
}

/** @brief A record whose first address byte says which one it is. */
static kdrv_ble_heard_t record_of(uint8_t n)
{
    static const uint8_t le[KDRV_BLE_HEARD_ADDR_LEN] = { 1U, 2U, 3U, 4U, 5U, 6U };
    static const uint8_t payload[AD_PAYLOAD_LEN] = { 0x02U, 0x01U, 0x06U };
    kdrv_ble_heard_t           r;

    kdrv_ble_heard_make(le, 0U, 0U, (int8_t)AD_RSSI, payload, AD_PAYLOAD_LEN, &r);
    r.addr[0] = n;
    return r;
}

/** @brief The host task's part: @p n records into the ring, each with the hook told. */
static void air(uint8_t n)
{
    for ( uint8_t i = 0U; n > i; i++ )
    {
        kdrv_ble_heard_t const r = record_of((uint8_t)(AD_MARK_BASE + i));

        (void)kdrv_ble_heard_feed(&r);
        fake_ble_hear_note(KDRV_BLE_HEAR_RECORD);
    }
}

/** @brief How many knocks wait in AO_Radio's queue; the queue is left as it was. */
static uint32_t knocks_queued(void)
{
    uint32_t    knocks = 0U;
    UBaseType_t n      = uxQueueMessagesWaiting(AO_Radio->eQueue);

    for ( UBaseType_t i = 0U; i < n; ++i )
    {
        const QEvt *e = NULL;

        TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
        if ( (QSignal)RADIO_HEARD_KNOCK_SIG == e->sig )
        {
            knocks += 1U;
        }
        TEST_ASSERT_TRUE(xQueueSend(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    }
    return knocks;
}

/** @brief The board is awake again, and the sentinel a round started is stopped. */
static void heard_awake(void)
{
    lp_awake(AO_Radio);
    drain();
    while ( job_held() && (KDRV_RADIO_JOB_SLEEP == held_req()->type) )
    {
        finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    }
}

/* ---- Who may start, and when -------------------------------------------------- */

TEST_CASE("test_no_link_refuses", "[radio_heard]")
{
    radio_fresh();
    askers_drain();

    ask(ao_spy(), true);
    expect_state(ao_spy(), false, KBLE_HEARD_NO_LINK, 0U, 0U);
    expect_nothing(ao_spy());
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* A stop is answered too: every request is. */
    ask(ao_spy(), false);
    expect_state(ao_spy(), false, KBLE_HEARD_NO_LINK, 0U, 0U);
    heard_end_case();
}

TEST_CASE("test_ble_off_refuses_and_starts_nothing", "[radio_heard]")
{
    radio_fresh();
    fake_ble_reset();
    fake_nvs_reset();
    askers_drain();
    go_up();
    askers_drain();   /* the link's own states are the spy's too */
    TEST_ASSERT_FALSE_MESSAGE(ble_enabled(), "Bluetooth is on in Settings");
    unsigned const submits = job_submits();

    ask(ao_spy(), true);
    expect_state(ao_spy(), false, KBLE_HEARD_BLE_OFF, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT(submits, job_submits());
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* Bluetooth wanted, and the stack never came up. */
    fake_ble_fail_start(true);
    dispatch(BLE_START_REQ_SIG);
    ble_settle();
    TEST_ASSERT_TRUE(ble_enabled());
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_OFF, (int)kdrv_ble_state());
    askers_drain();
    ask(ao_spy(), true);
    expect_state(ao_spy(), false, KBLE_HEARD_BLE_OFF, 0U, 0U);
    TEST_ASSERT_FALSE(job_held());
    TEST_ASSERT_NOT_EQUAL((int)KDRV_RADIO_JOB_BLE_HEAR, (int)last_req()->type);
    heard_end_case();
}

/** @brief What the spy holds of scans: the listing's ends and an asked scan's answers; the rest is dropped. */
typedef struct {
    uint32_t       ble_ends;   /**< BLE_SCAN_END_SIG seen.   */
    WifiScanEndEvt ble_end;    /**< The last of them.        */
    uint32_t       wifi_ends;  /**< WIFI_SCAN_END_SIG seen.  */
    uint32_t       ble_rsps;   /**< BLE_SCAN_RSP_SIG seen.   */
    ScanRspEvt     ble_rsp;    /**< The last of them.        */
} scans_t;

/** @brief Read everything the spy holds. */
static scans_t scans_heard(void)
{
    scans_t     h;
    const QEvt *e;

    (void)memset(&h, 0, sizeof(h));
    for ( e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (QSignal)BLE_SCAN_END_SIG == e->sig )
        {
            ++h.ble_ends;
            h.ble_end = *(const WifiScanEndEvt *)(const void *)e;
        }
        else if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            ++h.wifi_ends;
        }
        else if ( (QSignal)BLE_SCAN_RSP_SIG == e->sig )
        {
            ++h.ble_rsps;
            h.ble_rsp = *(const ScanRspEvt *)(const void *)e;
        }
        else
        {
            /* a state, an entry: not what these cases judge */
        }
    }
    return h;
}

/** @brief The console's listen of AD_SCAN_MS, with the worker. */
static void listen_runs(void)
{
    TEST_ASSERT_TRUE(ble_scan_request(AD_SCAN_MS, NULL));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    ao_spy_drain();
}

TEST_CASE("a stream asked during a ble_scan ends the listen at once and starts when the worker is back",
          "[radio_heard]")
{
    scans_t h;

    heard_up();
    listen_runs();

    ask(ao_spy_other(), true);
    h = scans_heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ble_ends, "the listen in flight was not closed at once");
    TEST_ASSERT_TRUE(h.ble_end.cut);
    TEST_ASSERT_FALSE(h.ble_end.ok);
    TEST_ASSERT_FALSE(h.ble_end.busy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kdrv_ble_scan(AD_SCAN_MS),
                                  "the listen in flight was not asked to end");

    /* The stream is not busy and not on yet: its start waits for the worker. */
    expect_nothing(ao_spy_other());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_DENIED);
    expect_hear_job(true);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy_other(), true, KBLE_HEARD_OK, 0U, 0U);
    h = scans_heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ble_ends, "the listen that was cut was listed all the same");
    heard_end_case();
}

TEST_CASE("a stream asked during an asked listen ends it cancelled and starts next", "[radio_heard]")
{
    scans_t h;

    heard_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, ble_scan_ask(ao_spy(), AD_SCAN_MS));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    ao_spy_drain();

    ask(ao_spy_other(), true);
    h = scans_heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ble_rsps, "the asked listen was not answered at once");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CANCELLED, h.ble_rsp.rc);
    expect_nothing(ao_spy_other());

    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_DENIED);
    expect_hear_job(true);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy_other(), true, KBLE_HEARD_OK, 0U, 0U);
    ao_spy_drain();
    heard_end_case();
}

TEST_CASE("a listen asked while the stream's start is with the worker ends the stream at once and runs next",
          "[radio_heard]")
{
    scans_t h;

    heard_up();
    ask(ao_spy_other(), true);
    expect_hear_job(true);
    ao_spy_drain();

    TEST_ASSERT_TRUE(ble_scan_request(AD_SCAN_MS, NULL));
    pump();
    expect_state(ao_spy_other(), false, KBLE_HEARD_CANCELLED, 0U, 0U);
    h = scans_heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ble_ends, "the newer listen was answered busy");

    /* The scan the start made is stopped, and the listen runs then. */
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_hear_job(false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    expect_nothing(ao_spy_other());
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_OK);
    ao_spy_drain();
    heard_end_case();
}

TEST_CASE("a stream that waits for the worker is ended by a newer listen, cancelled, and never starts",
          "[radio_heard]")
{
    heard_up();
    listen_runs();
    ask(ao_spy_other(), true);
    expect_nothing(ao_spy_other());
    ao_spy_drain();

    TEST_ASSERT_TRUE(ble_scan_request(AD_SCAN_MS, NULL));
    pump();
    expect_state(ao_spy_other(), false, KBLE_HEARD_CANCELLED, 0U, 0U);

    /* The listen that was cut comes back, and the newest one is what runs. */
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "the stream that was cancelled started");
    expect_nothing(ao_spy_other());
    ao_spy_drain();
    heard_end_case();
}

TEST_CASE("a stop while the stream waits for the worker is answered off and nothing starts", "[radio_heard]")
{
    heard_up();
    listen_runs();
    ask(ao_spy_other(), true);
    expect_nothing(ao_spy_other());

    ask(ao_spy_other(), false);
    expect_state(ao_spy_other(), false, KBLE_HEARD_OK, 0U, 0U);
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a stream nobody waits for started");
    expect_nothing(ao_spy_other());
    ao_spy_drain();
    heard_end_case();
}

TEST_CASE("a Wi-Fi scan ends no stream: the sweep runs and the advertisements keep coming", "[radio_heard]")
{
    scans_t h;

    heard_up();
    start_stream(ao_spy_other());

    (void)wifi_scan_request(NULL);
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job(),
                                  "the sweep was refused for the stream");
    expect_nothing(ao_spy_other());

    /* The stream holds no job: what it hears is handed over while the sweep runs. */
    air(1U);
    pump();
    (void)take_heard(ao_spy_other());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    h = scans_heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.wifi_ends);
    expect_nothing(ao_spy_other());
    heard_end_case();
}

TEST_CASE("a stream asked during a Wi-Fi sweep is busy and ends no sweep", "[radio_heard]")
{
    scans_t h;

    heard_up();
    (void)wifi_scan_request(NULL);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    ao_spy_drain();

    ask(ao_spy_other(), true);
    expect_state(ao_spy_other(), false, KBLE_HEARD_BUSY, 0U, 0U);
    h = scans_heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.wifi_ends, "a Bluetooth request ended the Wi-Fi sweep");
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "the stream answered busy started all the same");
    ao_spy_drain();
    heard_end_case();
}

TEST_CASE("a stream asked during the radio check is busy: the check is not cut", "[radio_heard]")
{
    radio_probe_t probe;

    heard_up();

    (void)memset(&probe, 0, sizeof(probe));
    probe.card = true;
    TEST_ASSERT_TRUE(radio_probe_request(ao_spy(), &probe));
    pump();
    ask(ao_spy_other(), true);
    expect_state(ao_spy_other(), false, KBLE_HEARD_BUSY, 0U, 0U);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();

    /* Nothing took the door meanwhile: the same asker starts now. */
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("a second asker takes the stream, and the first is told a newer order has it", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());
    air(AD_TWO);
    pump();
    (void)take_heard(ao_spy());
    (void)take_heard(ao_spy());

    unsigned const submits = job_submits();

    /* The radio is not stopped: its scan goes on, for the new asker. */
    ask(ao_spy_other(), true);
    TEST_ASSERT_EQUAL_UINT(submits, job_submits());
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    expect_state(ao_spy(), false, KBLE_HEARD_CANCELLED, AD_TWO, 0U);
    expect_state(ao_spy_other(), true, KBLE_HEARD_OK, 0U, 0U);

    /* What is heard from now on is the new asker's, counted from zero. */
    air(1U);
    pump();
    (void)take_heard(ao_spy_other());
    expect_nothing(ao_spy());

    /* The first one's late stop is answered and changes nothing. */
    ask(ao_spy(), false);
    expect_state(ao_spy(), false, KBLE_HEARD_OK, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    air(1U);
    pump();
    (void)take_heard(ao_spy_other());
    expect_nothing(ao_spy_other());

    /* The stop of the one that holds it counts its own two. */
    ask(ao_spy_other(), false);
    expect_hear_job(false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy_other(), false, KBLE_HEARD_OK, AD_TWO, 0U);
    heard_end_case();
}

TEST_CASE("a second asker during the first one's start gets the start's answer", "[radio_heard]")
{
    heard_up();

    ask(ao_spy(), true);
    expect_hear_job(true);

    /* The first owed a stop too: it goes with its stream. */
    ask(ao_spy(), false);
    ask(ao_spy_other(), true);
    expect_state(ao_spy(), false, KBLE_HEARD_CANCELLED, 0U, 0U);
    expect_nothing(ao_spy_other());
    TEST_ASSERT_FALSE(job_queued());

    /* What is heard before the start answers is the new asker's, after its answer. */
    air(1U);
    pump();
    expect_nothing(ao_spy_other());

    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy_other(), true, KBLE_HEARD_OK, 1U, 0U);
    (void)take_heard(ao_spy_other());
    expect_nothing(ao_spy_other());
    expect_nothing(ao_spy());
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "the first asker's owed stop went out");
    heard_end_case();
}

TEST_CASE("a second asker while the first one's stop is with the worker is busy", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    ask(ao_spy(), false);
    expect_hear_job(false);
    ask(ao_spy_other(), true);
    expect_state(ao_spy_other(), false, KBLE_HEARD_BUSY, 0U, 0U);

    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy(), false, KBLE_HEARD_OK, 0U, 0U);

    /* Free now: asked again, it starts. */
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_rest_refuses_a_start", "[radio_heard]")
{
    heard_up();
    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);

    ask(ao_spy_other(), true);
    expect_state(ao_spy_other(), false, KBLE_HEARD_RESTING, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* Awake again: the door takes a start. */
    heard_awake();
    ask(ao_spy_other(), true);
    expect_hear_job(true);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy_other(), true, KBLE_HEARD_OK, 0U, 0U);
    heard_end_case();
}

/* ---- The start and the stop ---------------------------------------------------- */

TEST_CASE("an advertisement heard before the start is answered comes after the answer", "[radio_heard]")
{
    heard_up();

    ask(ao_spy_other(), true);
    expect_hear_job(true);

    /* The scan is on in the stack before the worker's answer is read: its
     * first records wait in the ring. */
    air(AD_TWO);
    pump();
    expect_nothing(ao_spy_other());

    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy_other(), true, KBLE_HEARD_OK, AD_TWO, 0U);
    TEST_ASSERT_EQUAL_UINT8(AD_MARK_BASE, take_heard(ao_spy_other())->addr[0]);
    TEST_ASSERT_EQUAL_UINT8(AD_MARK_BASE + 1U, take_heard(ao_spy_other())->addr[0]);
    expect_nothing(ao_spy_other());
    heard_end_case();
}

TEST_CASE("a start that is refused hands over nothing of what waited", "[radio_heard]")
{
    kdrv_ble_heard_t gone;

    heard_up();

    ask(ao_spy_other(), true);
    expect_hear_job(true);
    air(1U);
    pump();
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_DENIED);
    expect_state(ao_spy_other(), false, KBLE_HEARD_REFUSED, 0U, 0U);
    expect_nothing(ao_spy_other());
    TEST_ASSERT_FALSE(kdrv_ble_heard_next(&gone));
    heard_end_case();
}

TEST_CASE("the stream never fills more than half of its asker's queue", "[radio_heard]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);
    UBaseType_t const half   = (UBaseType_t)AO_SPY_QUEUE_LEN / 2U;

    heard_up();
    start_stream(ao_spy());

    /* The asker's own traffic takes half of its queue. */
    for ( uint32_t guard = 0U;
         (half < uxQueueSpacesAvailable(ao_spy()->eQueue)) && (AD_FILL_MAX > guard);
         ++guard )
    {
        QACTIVE_POST(ao_spy(), &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(half, uxQueueSpacesAvailable(ao_spy()->eQueue));

    /* A record would go past the half: it is dropped and counted. */
    air(AD_TWO);
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(half, uxQueueSpacesAvailable(ao_spy()->eQueue),
                                   "a record went past half of the asker's queue");

    /* One slot more than half free: one record fits, and says what was lost. */
    TEST_ASSERT_EQUAL_INT((int)WIFI_STATE_REQ_SIG, (int)ao_spy_take()->sig);
    air(AD_TWO);
    pump();
    TEST_ASSERT_EQUAL_UINT(half, uxQueueSpacesAvailable(ao_spy()->eQueue));
    for ( uint32_t i = 1U; half > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT((int)WIFI_STATE_REQ_SIG, (int)ao_spy_take()->sig);
    }
    TEST_ASSERT_EQUAL_UINT32(AD_TWO, take_heard(ao_spy())->dropped);
    expect_nothing(ao_spy());
    heard_end_case();
}

TEST_CASE("test_start_runs_a_job_then_answers", "[radio_heard]")
{
    heard_up();

    /* What an earlier stream left in the ring is not this one's. */
    for ( uint8_t i = 0U; AD_TWO > i; i++ )
    {
        kdrv_ble_heard_t const stale = record_of((uint8_t)(AD_MARK_BASE + i));

        TEST_ASSERT_TRUE(kdrv_ble_heard_feed(&stale));
    }
    TEST_ASSERT_EQUAL_UINT32(AD_TWO, kdrv_ble_heard_total());

    ask(ao_spy(), true);
    expect_hear_job(true);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, kdrv_ble_heard_total(), "the ring was not forgotten");
    expect_nothing(ao_spy());

    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy(), true, KBLE_HEARD_OK, 0U, 0U);
    expect_nothing(ao_spy());
    heard_end_case();
}

TEST_CASE("test_refused_job_frees_the_door", "[radio_heard]")
{
    heard_up();

    ask(ao_spy(), true);
    expect_hear_job(true);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_DENIED);
    expect_state(ao_spy(), false, KBLE_HEARD_REFUSED, 0U, 0U);
    expect_nothing(ao_spy());

    /* Free: another asker starts at once. */
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_stop_answers_with_totals", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    air(AD_THREE);
    pump();
    for ( uint8_t i = 0U; AD_THREE > i; i++ )
    {
        (void)take_heard(ao_spy());
    }

    /* One more lands while the stop is on the worker: it is handed over
     * before the answer, which counts it. */
    ask(ao_spy(), false);
    expect_hear_job(false);
    expect_nothing(ao_spy());
    air(1U);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    (void)take_heard(ao_spy());
    expect_state(ao_spy(), false, KBLE_HEARD_OK, AD_THREE + 1U, 0U);
    expect_nothing(ao_spy());

    /* The door is free. */
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_stop_during_start_is_owed", "[radio_heard]")
{
    heard_up();

    ask(ao_spy(), true);
    expect_hear_job(true);
    ask(ao_spy(), false);
    expect_hear_job(true);   /* nothing new went to the worker */
    TEST_ASSERT_FALSE(job_queued());
    expect_nothing(ao_spy());

    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy(), true, KBLE_HEARD_OK, 0U, 0U);
    expect_hear_job(false);

    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy(), false, KBLE_HEARD_OK, 0U, 0U);
    expect_nothing(ao_spy());
    heard_end_case();
}

TEST_CASE("test_stop_with_nobody_listening_is_answered_off", "[radio_heard]")
{
    heard_up();

    ask(ao_spy(), false);
    expect_state(ao_spy(), false, KBLE_HEARD_OK, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    heard_end_case();
}

TEST_CASE("test_stop_the_worker_cannot_take_leaves_the_stream_on", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    /* The worker is inside another job. */
    dispatch(BLE_UNPAIR_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_UNPAIR, running_job());
    ao_spy_drain();   /* what the unpair publishes is the spy's too */

    ask(ao_spy(), false);
    expect_state(ao_spy(), true, KBLE_HEARD_BUSY, 0U, 0U);

    /* Asked again with the worker free, it goes. */
    finish(KDRV_RADIO_JOB_BLE_UNPAIR, KODE_OK);
    ao_spy_drain();
    ask(ao_spy(), false);
    expect_hear_job(false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy(), false, KBLE_HEARD_OK, 0U, 0U);
    heard_end_case();
}

/* ---- The newest Bluetooth order has the radio -------------------------------------- */

/** @brief A stream for the second asker that has handed over one advertisement. */
static void stream_with_one_heard(void)
{
    heard_up();
    start_stream(ao_spy_other());
    air(1U);
    pump();
    (void)take_heard(ao_spy_other());
}

/** @brief The stream was ended for a newer order with one heard, and no stop job follows @p job. */
static void expect_cut_by(kdrv_radio_job_t job)
{
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)job, running_job(), "the newer order did not run");
    expect_state(ao_spy_other(), false, KBLE_HEARD_CANCELLED, 1U, 0U);
    expect_nothing(ao_spy_other());
}

TEST_CASE("a ble_scan ends the stream, tells its asker and runs", "[radio_heard]")
{
    stream_with_one_heard();

    TEST_ASSERT_TRUE(ble_scan_request(AD_SCAN_MS, NULL));
    pump();
    expect_cut_by(KDRV_RADIO_JOB_BLE_SCAN);
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_OK);
    ao_spy_drain();

    /* The scan took the host's one scan from the stream: nothing is left to stop. */
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* The door is free: the asker starts again when it wants. */
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("a scan an AO asks for ends the stream, tells its asker and runs", "[radio_heard]")
{
    stream_with_one_heard();

    TEST_ASSERT_EQUAL_INT(KODE_OK, ble_scan_ask(ao_spy(), AD_SCAN_MS));
    pump();
    expect_cut_by(KDRV_RADIO_JOB_BLE_SCAN);
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_OK);
    {
        const QEvt *e = ao_spy_take();

        while ( (NULL != e) && ((QSignal)BLE_SCAN_RSP_SIG != e->sig) )
        {
            e = ao_spy_take();
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the asked scan was not answered");
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, ((const ScanRspEvt *)(const void *)e)->rc);
    }
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    heard_end_case();
}

TEST_CASE("the radio check's name find ends the stream, tells its asker and runs", "[radio_heard]")
{
    radio_probe_t probe;

    stream_with_one_heard();

    (void)memset(&probe, 0, sizeof(probe));
    (void)snprintf(probe.ble, sizeof(probe.ble), "%s", AD_BEACON);
    TEST_ASSERT_TRUE(radio_probe_request(ao_spy(), &probe));
    pump();
    expect_cut_by(KDRV_RADIO_JOB_BLE_FIND);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    heard_end_case();
}

TEST_CASE("a record that still comes after a newer order took the scan stops what is left", "[radio_heard]")
{
    stream_with_one_heard();

    TEST_ASSERT_TRUE(ble_scan_request(AD_SCAN_MS, NULL));
    pump();
    expect_cut_by(KDRV_RADIO_JOB_BLE_SCAN);

    /* A record from before the scan took over lands while the scan runs: nobody is told. */
    air(1U);
    pump();
    expect_nothing(ao_spy_other());
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_OK);
    ao_spy_drain();

    /* One that comes with the worker free proves the stream's scan is still on: it is stopped, once. */
    air(1U);
    pump();
    expect_hear_job(false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_nothing(ao_spy_other());
    heard_end_case();
}

/* ---- What the asker is handed -------------------------------------------------------- */

TEST_CASE("test_knock_posts_in_order", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    /* Three records land before AO_Radio runs: one knock, and all of them handed over. */
    air(AD_THREE);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, knocks_queued(), "one knock per record");
    pump();
    for ( uint8_t i = 0U; AD_THREE > i; i++ )
    {
        BleHeardEvt const *const a = take_heard(ao_spy());

        TEST_ASSERT_EQUAL_UINT8((uint8_t)(AD_MARK_BASE + i), a->addr[0]);
        TEST_ASSERT_EQUAL_UINT8(1U, a->addr[KBLE_HEARD_ADDR_LEN - 1U]);   /* as printed: the stack's order reversed */
        TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_ADDR_PUBLIC, a->addr_type);
        TEST_ASSERT_EQUAL_INT8((int8_t)AD_RSSI, a->rssi);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_HEARD_PDU_CONN, a->pdu);
        TEST_ASSERT_EQUAL_UINT8(AD_PAYLOAD_LEN, a->len);
        TEST_ASSERT_EQUAL_UINT8(0x06U, a->data[AD_PAYLOAD_LEN - 1U]);
        TEST_ASSERT_EQUAL_UINT32(0U, a->dropped);
    }
    expect_nothing(ao_spy());
    ao_spy_release();   /* the spy holds what it took: give the pool back, as a dispatch would */

    /* Taken: the next record knocks again. */
    air(1U);
    TEST_ASSERT_EQUAL_UINT(1U, knocks_queued());
    pump();
    (void)take_heard(ao_spy());
    heard_end_case();
}

TEST_CASE("test_full_asker_queue_drops_and_counts", "[radio_heard]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    heard_up();
    start_stream(ao_spy_other());

    for ( uint32_t guard = 0U;
         (AD_FULL < uxQueueSpacesAvailable(ao_spy_other()->eQueue)) && (AD_FILL_MAX > guard);
         ++guard )
    {
        QACTIVE_POST(ao_spy_other(), &filler, NULL);
    }
    air(AD_TWO);
    pump();

    /* The queue is as it was left: full of the filler, nothing of the records. */
    uint32_t fillers = 0U;
    for ( const QEvt *e = ao_spy_other_take(); NULL != e; e = ao_spy_other_take() )
    {
        TEST_ASSERT_EQUAL_INT((int)WIFI_STATE_REQ_SIG, (int)e->sig);   /* static: nothing to recycle */
        ++fillers;
    }
    TEST_ASSERT_TRUE(0U < fillers);

    /* Room again: the next record says what was lost, and nothing is re-queued. */
    air(1U);
    pump();
    BleHeardEvt const *const a = take_heard(ao_spy_other());
    TEST_ASSERT_EQUAL_UINT32(AD_TWO, a->dropped);
    TEST_ASSERT_EQUAL_UINT8(AD_MARK_BASE, a->addr[0]);
    expect_nothing(ao_spy_other());

    ask(ao_spy_other(), false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_state(ao_spy_other(), false, KBLE_HEARD_OK, AD_TWO + 1U, AD_TWO);
    heard_end_case();
}

TEST_CASE("test_dry_pool_drops_and_counts", "[radio_heard]")
{
    static QEvt *held[KQP_MEDIUM_POOL_COUNT];
    uint32_t     n = 0U;

    heard_up();
    start_stream(ao_spy());

    /* The medium tier down to the margin an advertisement leaves for the rest. */
    while ( KQP_MEDIUM_POOL_COUNT > n )
    {
        BleHeardEvt *const e = Q_NEW_X(BleHeardEvt, KQP_HEARD_POOL_MARGIN, Q_USER_SIG);

        if ( NULL == e )
        {
            break;
        }
        held[n++] = &e->super;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(KQP_MEDIUM_POOL_COUNT, n, "the pool never ran to its margin");

    air(AD_TWO);
    pump();
    expect_nothing(ao_spy());
    for ( uint32_t i = 0U; n > i; i++ )
    {
        QF_gc(held[i]);
    }

    air(1U);
    pump();
    BleHeardEvt const *const a = take_heard(ao_spy());
    TEST_ASSERT_EQUAL_UINT32(AD_TWO, a->dropped);
    heard_end_case();
}

TEST_CASE("test_knock_with_no_asker_stops_the_listen", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    /* A rest round while the worker is inside a job: the asker is told, the
     * stop cannot go yet, and the passive scan is still on with nobody to hand it to. */
    dispatch(BLE_UNPAIR_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_UNPAIR, running_job());
    ao_spy_drain();   /* what the unpair publishes is the spy's too */
    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    lp_expect_answer(true, "heard");
    lp_expect_no_more();
    expect_state(ao_spy(), false, KBLE_HEARD_RESTING, 0U, 0U);
    finish(KDRV_RADIO_JOB_BLE_UNPAIR, KODE_OK);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* A record comes: the ring is emptied and nobody is told; one stop goes out. */
    air(AD_TWO);
    pump();
    expect_hear_job(false);
    expect_nothing(ao_spy());
    kdrv_ble_heard_t gone;
    TEST_ASSERT_FALSE(kdrv_ble_heard_next(&gone));

    /* A second record before the stop is answered sends no second stop. */
    unsigned const submits = job_submits();
    air(1U);
    pump();
    TEST_ASSERT_EQUAL_UINT(submits, job_submits());
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_nothing(ao_spy());

    heard_awake();
    heard_end_case();
}

/* ---- The stack ends it, or the radio does -------------------------------------------- */

TEST_CASE("test_stack_end_tells_the_asker", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    air(AD_TWO);
    fake_ble_hear_note(KDRV_BLE_HEAR_ENDED);
    pump();

    /* What was heard first, then the end with its totals. */
    (void)take_heard(ao_spy());
    (void)take_heard(ao_spy());
    expect_state(ao_spy(), false, KBLE_HEARD_REFUSED, AD_TWO, 0U);
    expect_nothing(ao_spy());
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* The door is free. */
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_ble_down_ends_the_stream_first", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy_other());

    dispatch(BLE_STOP_REQ_SIG);
    pump();

    expect_state(ao_spy_other(), false, KBLE_HEARD_BLE_OFF, 0U, 0U);
    expect_hear_job(false);   /* before the QUIET job */
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_QUIET, running_job());
    finish(KDRV_RADIO_JOB_BLE_QUIET, KODE_OK);
    expect_nothing(ao_spy_other());

    /* Bluetooth is off: the door says so. */
    ask(ao_spy_other(), true);
    expect_state(ao_spy_other(), false, KBLE_HEARD_BLE_OFF, 0U, 0U);
    heard_end_case();
}

TEST_CASE("test_ble_down_with_the_worker_busy_still_clears_the_listen", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    /* The stop cannot go: the worker is inside another job. QUIET follows and ends the passive scan unseen. */
    dispatch(BLE_UNPAIR_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_UNPAIR, running_job());
    ao_spy_drain();   /* what the unpair publishes is the spy's too */
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    expect_state(ao_spy(), false, KBLE_HEARD_BLE_OFF, 0U, 0U);
    finish(KDRV_RADIO_JOB_BLE_UNPAIR, KODE_OK);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_QUIET, running_job());
    finish(KDRV_RADIO_JOB_BLE_QUIET, KODE_OK);
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* Nothing is left to stop: a knock with no record behind it sends no job. */
    fake_ble_hear_note(KDRV_BLE_HEAR_RECORD);
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* Bluetooth on again: the door takes a start. */
    dispatch(BLE_START_REQ_SIG);
    ble_settle();
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_link_gone_tells_the_asker", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy());

    /* A record waits in the ring, its knock not yet taken, when the link goes. */
    kdrv_ble_heard_t const waiting = record_of(AD_MARK_BASE);

    TEST_ASSERT_TRUE(kdrv_ble_heard_feed(&waiting));

    finish_lost(KODE_ERROR);

    expect_state(ao_spy(), false, KBLE_HEARD_NO_LINK, 1U, 0U);
    kdrv_ble_heard_t gone;
    TEST_ASSERT_FALSE(kdrv_ble_heard_next(&gone));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_ble_heard_total());
    ao_spy_drain();   /* the link's own states are the spy's too */

    /* The door is free: after the link is back, another asker starts. */
    settle_to_off();
    go_up();
    pump();
    ble_settle();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_TRUE(ble_enabled());
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_rest_round_ends_the_stream", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy_other());

    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();

    lp_expect_answer(true, "heard");
    lp_expect_no_more();
    expect_hear_job(false);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_BLE_HEAR, running_job(),
                                  "the sentinel started under a listener");
    expect_state(ao_spy_other(), false, KBLE_HEARD_RESTING, 0U, 0U);

    /* The stop is not the asker's to hear. */
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_nothing(ao_spy_other());

    /* The round's retry finds the door gone. */
    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    heard_awake();
    heard_end_case();
}

TEST_CASE("an OFF round ends the stream and tells its asker", "[radio_heard]")
{
    stream_with_one_heard();

    lp_round(AO_Radio, KODE_LP_OFF);
    drain();

    lp_expect_answer(true, "heard");
    lp_expect_no_more();
    expect_hear_job(false);
    expect_state(ao_spy_other(), false, KBLE_HEARD_RESTING, 1U, 0U);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_nothing(ao_spy_other());

    heard_awake();
    heard_end_case();
}

/** @brief Ask for a C5 update from the card: Radio_updating, waiting for the file. */
static void c5_update_begins(void)
{
    FlashReqEvt f;

    (void)memset(&f, 0, sizeof(f));
    f.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    f.src_kind = (uint8_t)KFW_SRC_FS;
    f.dst_kind = (uint8_t)KFW_TARGET_C5;
    (void)snprintf(f.src, sizeof(f.src), "%s", "c5_slave.bin");
    dispatch_evt(&f.super);
    TEST_ASSERT_EQUAL_INT(-2, next_storage_op());   /* the open: the update has begun */
}

/** @brief The update's file was not there: back to Radio_up. */
static void c5_update_ends(void)
{
    StorageOpenRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(STORAGE_OPEN_RSP_SIG);
    r.status = (uint8_t)KODE_ERROR;
    dispatch_evt(&r.super);
    pump();
}

TEST_CASE("test_update_ends_the_stream", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy_other());

    c5_update_begins();
    pump();
    expect_state(ao_spy_other(), false, KBLE_HEARD_BUSY, 0U, 0U);
    expect_hear_job(false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_nothing(ao_spy_other());

    /* A start during the update is busy. */
    ask(ao_spy_other(), true);
    expect_state(ao_spy_other(), false, KBLE_HEARD_BUSY, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* A record the update's neighbours still hear is emptied, and starts nothing. */
    air(1U);
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    c5_update_ends();
    start_stream(ao_spy_other());
    heard_end_case();
}

/* ---- A passive scan the door lost track of ------------------------------------------------ */

/** @brief Leave AO_Radio's queue with no more free slots than a knock leaves. */
static void radio_queue_to_knock_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(RADIO_STATE_REQ_SIG);

    for ( uint32_t guard = 0U;
         (AD_FILL_MAX > guard) && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_KNOCK_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
}

/** @brief Empty AO_Radio's queue without dispatching it. */
static void radio_queue_empty(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

TEST_CASE("an end of the scan the queue had no room for is told once there is room", "[radio_heard]")
{
    heard_up();
    start_stream(ao_spy_other());

    /* The stack ends the scan and AO_Radio has no room for the knock: it is
     * owed, and no advertisement will ever come to knock again. */
    radio_queue_to_knock_margin();
    fake_ble_hear_note(KDRV_BLE_HEAR_ENDED);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, knocks_queued(), "a knock went past its margin");
    expect_nothing(ao_spy_other());

    /* Room again, and AO_Radio handles its next event: the knock is posted
     * before it, and the asker is told. */
    radio_queue_empty();
    dispatch(RADIO_STATE_REQ_SIG);
    pump();
    expect_state(ao_spy_other(), false, KBLE_HEARD_REFUSED, 0U, 0U);
    expect_nothing(ao_spy_other());
    ao_spy_drain();

    /* The door is free, and the old end is not held against the next stream. */
    start_stream(ao_spy_other());
    air(1U);
    pump();
    (void)take_heard(ao_spy_other());
    expect_nothing(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_a_timed_out_start_is_stopped_by_the_next_record", "[radio_heard]")
{
    heard_up();

    /* The start's answer is the guard's: the worker may have started the passive scan. */
    ask(ao_spy(), true);
    expect_hear_job(true);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_TIMEOUT);
    expect_state(ao_spy(), false, KBLE_HEARD_REFUSED, 0U, 0U);
    expect_nothing(ao_spy());

    /* Nobody asks and a record comes: the passive scan is on, and is stopped. */
    air(1U);
    pump();
    expect_hear_job(false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_nothing(ao_spy());

    /* The door is free again. */
    start_stream(ao_spy_other());
    heard_end_case();
}

TEST_CASE("test_a_record_with_no_asker_proves_a_listen_and_stops_it", "[radio_heard]")
{
    heard_up();

    /* The door believes nothing listens, and a record arrives. */
    air(1U);
    pump();
    expect_hear_job(false);
    finish(KDRV_RADIO_JOB_BLE_HEAR, KODE_OK);
    expect_nothing(ao_spy());

    /* A knock with no record behind it starts no job. */
    fake_ble_hear_note(KDRV_BLE_HEAR_RECORD);
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    heard_end_case();
}

/* ---- A cable update ------------------------ ------------------------------------------- */

/** @brief A synthetic C5 image: header, chip id, and IDF's descriptor at 0x20. */
static uint8_t s_img[0x140];

/** @brief Lay an image for the C5 that says it is kode_c5. */
static void make_image(void)
{
    esp_app_desc_t d;

    for ( unsigned i = 0U; sizeof(s_img) > i; ++i )
    {
        s_img[i] = (uint8_t)((i * 3U) + 7U);
    }
    s_img[0]  = 0xE9U;                       /* ESP_IMAGE_HEADER_MAGIC */
    s_img[12] = 0x17U;                       /* the C5's chip id */
    s_img[13] = 0x00U;
    /* Its first segment is the descriptor and no more: a length that fits
     * the file, which is what the decoder holds a raw image to. */
    esp_image_segment_header_t seg;

    (void)memset(&seg, 0, sizeof(seg));
    seg.load_addr = 0x40000000U;
    seg.data_len  = (uint32_t)sizeof(d);
    (void)memcpy(&s_img[sizeof(esp_image_header_t)], &seg, sizeof(seg));
    (void)memset(&d, 0, sizeof(d));
    d.magic_word = ESP_APP_DESC_MAGIC_WORD;
    (void)snprintf(d.project_name, sizeof(d.project_name), "%s", "kode_c5");
    (void)snprintf(d.version, sizeof(d.version), "%s", "9.9.9");
    (void)memcpy(&s_img[0x20], &d, sizeof(d));
}

/** @brief Empty AO_Log's queue, where the console's arming lands. */
static void log_drain(void)
{
    const QEvt *e;

    while ( xQueueReceive(AO_Log->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The cable install up to the verified image: AO_Radio asks for the channel and starts the update. */
static void cable_image_arrives(void)
{
    AppRecvReqEvt    req;
    ConsoleRawRspEvt armed;
    static RecvDataEvt chunk;

    make_image();
    (void)memset(&req, 0, sizeof(req));
    req.super = (QEvt)QEVT_INITIALIZER(RADIO_RECV_REQ_SIG);
    req.dst   = (uint8_t)KFW_TARGET_C5;
    req.size  = (uint32_t)sizeof(s_img);
    req.crc32 = khal_crc32(KHAL_CRC32_INIT, s_img, sizeof(s_img));
    req.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    dispatch_evt(&req.super);
    pump();
    (void)memset(&armed, 0, sizeof(armed));
    armed.super  = (QEvt)QEVT_INITIALIZER(CONSOLE_RAW_RSP_SIG);
    armed.status = (uint8_t)KODE_OK;
    dispatch_evt(&armed.super);
    pump();
    chunk.super = (QEvt)QEVT_INITIALIZER(RECV_DATA_SIG);
    chunk.len   = (uint16_t)sizeof(s_img);
    (void)memcpy(chunk.data, s_img, sizeof(s_img));
    dispatch_evt(&chunk.super);
    pump();
}

TEST_CASE("test_a_cable_update_with_the_worker_held_tells_busy_and_breaks_nothing", "[radio_heard]")
{
    heard_up();
    fake_neighbours_reset();
    start_stream(ao_spy_other());

    /* The staged image asks for the channel: AO_Radio holds the worker for the
     * card's unmount before it enters the update, so the stop is not submitted. */
    cable_image_arrives();
    expect_state(ao_spy_other(), false, KBLE_HEARD_BUSY, 0U, 0U);
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());

    /* The passive scan stays on until the C5 reboots; records are dropped, one knock at a time. */
    air(1U);
    pump();
    air(1U);
    pump();
    expect_nothing(ao_spy_other());
    ask(ao_spy_other(), true);
    expect_state(ao_spy_other(), false, KBLE_HEARD_BUSY, 0U, 0U);

    /* The update runs its own jobs, in its own order, to the end. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_BEGIN, running_job());
    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_WRITE, running_job());
    finish(KDRV_RADIO_JOB_C5_WRITE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_FINISH, running_job());
    finish(KDRV_RADIO_JOB_C5_FINISH, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    log_drain();
    heard_end_case();
}
