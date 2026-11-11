/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_ao.c
 * @brief AO_Radio: the C5's owner, and the production row it runs on the chip.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "esp_system.h"

#include "qpc.h"
#include "Mockesp_wifi.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi_default.h"
#include "Mockesp_event.h"

#include "esp_app_desc.h"   /* to lay IDF's descriptor in a test image */

#include "ao_spy.h"
#include "config_support.h"
#include "fake_wifi_events.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kdrv_ble.h"
#include "kdrv_ble_app.h"
#include "kdrv_c5.h"
#include "kdrv_c5svc.h"
#include "khal_identity.h"
#include "kode_wire.h"
#include "kdrv_kfw.h"
#include "kdrv_radio.h"
#include "kdrv_storage.h"
#include "kdrv_wifi.h"
#include "kode_sys.h"
#include "khal_crc32.h"
#include "khal_nvs.h"
#include "khal_sync.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "kset.h"
#include "lowpower_cases.h"
#include "qf_support.h"

#include "radio_harness.h"

/* ---- The C5's vault, from this side --------------------------------------- */

/** @brief The C5 answers the KODE request in flight. */
static void kode_answer(uint8_t st, const void *pay, uint16_t n)
{
    static uint8_t m[sizeof(kode_hdr_t) + KODE_MAX_PAYLOAD];
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_last_sent(NULL);
    kode_hdr_t const h = { KODE_WIRE_VERSION, req->service, req->op, req->seq,
                           st, 0U, n };
    (void)memcpy(m, &h, sizeof(h));
    if ( 0U < n )
    {
        (void)memcpy(&m[sizeof(h)], pay, n);
    }
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(h) + (size_t)n);
    pump();
}

/** @brief The vault's LIST, asked at link up, answers with one name. */
static void vault_has(const char *ssid)
{
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_last_sent(NULL);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_VAULT, req->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_LIST, req->op);
    kode_vault_list_t l;
    (void)memset(&l, 0, sizeof(l));
    l.count = 1U;
    (void)snprintf(l.ssid[0], sizeof(l.ssid[0]), "%s", ssid);
    kode_answer(KODE_ST_OK, &l, (uint16_t)sizeof(l));
}

/** @brief A join through the vault, from the readying job to the wait. */
static void vault_joins(const char *ssid)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PREPARE, running_job());
    finish(KDRV_RADIO_JOB_WIFI_PREPARE, KODE_OK);
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_last_sent(NULL);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_VAULT, req->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_JOIN, req->op);
    TEST_ASSERT_EQUAL_STRING(ssid, (const char *)&req[1]);
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_AWAIT, running_job());
}

/** @brief Ask for the row. */
static void ask_row(void)
{
    RadioTestReqEvt e = { QEVT_INITIALIZER(RADIO_TEST_REQ_SIG), false };
    QASM_DISPATCH(AO_Radio, &e.super, 0U);
    pump();
}

/** @brief End the info job the way the worker does: refresh, then report. */
static void finish_info(void)
{
    kode_status_t const rc = kdrv_c5_refresh();
    finish(KDRV_RADIO_JOB_C5_INFO, rc);
}

/* ---- The mocked scan ----------------------------------------------------------- */

static wifi_ap_record_t s_recs[8];
static unsigned         s_next;
static uint16_t         s_found;

static esp_err_t pop_record(wifi_ap_record_t *out, int calls)
{
    (void)calls;
    if ( NULL != out )
    {
        *out = s_recs[s_next % 8U];
    }
    ++s_next;
    return ESP_OK;
}

/**
 * @brief Put @p n networks in the air, the strongest at @p top dBm and the
 *        rest 5 dB apart below it; the second one on a 5 GHz channel.
 */
static void seed_air(unsigned n, int8_t top)
{
    (void)memset(s_recs, 0, sizeof(s_recs));
    for ( unsigned i = 0U; (i < n) && (8U > i); ++i )
    {
        (void)snprintf((char *)s_recs[i].ssid, sizeof(s_recs[i].ssid),
                       "line-ap-%u", i);
        s_recs[i].rssi    = (int8_t)(top - (int)(5U * i));
        s_recs[i].primary = (uint8_t)(1U + i);
        if ( 1U == i )
        {
            s_recs[i].primary = 36U;   /* the one on 5 GHz */
        }
    }
    s_next  = 0U;
    s_found = (uint16_t)n;
}

/** @brief How many channels this sweep has started. */
static unsigned s_starts;

/** @brief What the channel being answered holds. */
static uint16_t s_step_found;

/** @brief esp_wifi_scan_start, answered at once with the SCAN_DONE the
 *         driver waits on; see test_kdrv_wifi_scan.c for why.
 *
 * A sweep is one scan per channel, and everything a case seeded is on the
 * first: what the row judges is the table, not where in the air it came
 * from, and the record's own `primary` is what carries the band. */
static esp_err_t scan_start_now(const wifi_scan_config_t *config, bool block,
                                int calls)
{
    (void)config; (void)calls;
    TEST_ASSERT_FALSE(block);
    s_step_found = 0U;
    if ( 0U == s_starts )
    {
        s_step_found = s_found;
    }
    ++s_starts;
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    return ESP_OK;
}

/** @brief How many networks the channel being answered holds. */
static esp_err_t ap_num_now(uint16_t *number, int calls)
{
    (void)calls;
    if ( NULL != number )
    {
        *number = s_step_found;
    }
    return ESP_OK;
}

/** @brief The coprocessor's buffer, cleared once per channel. */
static esp_err_t clear_list_now(int calls)
{
    (void)calls;
    return ESP_OK;
}

/** @brief A country that opens the whole lower band. */
static esp_err_t country_now(wifi_country_t *country, int calls)
{
    (void)calls;
    if ( NULL != country )
    {
        (void)memset(country, 0, sizeof(*country));
        country->schan = 1U;
        country->nchan = 13U;
    }
    return ESP_OK;
}

/**
 * @brief Run the real scan under the mock, filling kdrv_wifi's table.
 * @param want What the sweep must answer.
 */
static void run_scan_rc(kode_status_t want)
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    if ( !kdrv_wifi_started() )
    {
        esp_netif_init_ExpectAndReturn(ESP_OK);
        esp_event_handler_instance_register_Stub(&fake_wifi_events_register);
        esp_wifi_init_ExpectAnyArgsAndReturn(ESP_OK);
        esp_wifi_set_mode_ExpectAndReturn(WIFI_MODE_STA, ESP_OK);
        esp_wifi_start_ExpectAndReturn(ESP_OK);
    }
    s_starts = 0U;
    esp_wifi_get_country_Stub(&country_now);
    esp_wifi_scan_start_Stub(&scan_start_now);
    esp_wifi_scan_get_ap_num_Stub(&ap_num_now);
    esp_wifi_scan_get_ap_record_Stub(&pop_record);
    esp_wifi_clear_ap_list_Stub(&clear_list_now);

    TEST_ASSERT_EQUAL_INT(want, kdrv_wifi_scan());
}

/** @brief Run the real scan under the mock, to its end. */
static void run_scan(void)
{
    run_scan_rc(KODE_OK);
}

/** @brief The row's scan job: fill the air, run it, end the job. */
static void finish_scan(unsigned n, int8_t top)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    seed_air(n, top);
    run_scan();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
}

/** @brief The row's BLE start job, the way the worker runs it: the driver
 *         (fake) starts, then the outcome is reported. */
static void finish_ble_start(void)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_START, running_job());
    finish(KDRV_RADIO_JOB_BLE_START, kdrv_ble_start());
}

/** @brief The row's BLE listen: @p n advertisers in the air, the strongest
 *         at @p top dBm, then the outcome. */
static void finish_ble_scan(uint8_t n, int8_t top)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    fake_ble_seed_air(n, top);
    finish(KDRV_RADIO_JOB_BLE_SCAN,
           kdrv_ble_scan(held_req()->len));
}

/** @brief Everything after the info on a healthy unit: a strong WiFi scan,
 *         the BLE start, a BLE listen that hears two advertisers. */
static void finish_rest(void)
{
    finish_scan(3U, -52);
    finish_ble_start();
    finish_ble_scan(2U, -60);
}

/** @brief The row's verdict, taken from the spy. Fails the case if none. */
static bool take_verdict(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the row never reached a verdict");
        if ( (QSignal)TEST_RESULT_SIG == e->sig )
        {
            TEST_ASSERT_EQUAL_STRING("c5", ((const TestResultEvt *)e)->name);
            return ((const TestResultEvt *)e)->ok;
        }
    }
}

/** @brief Whether a verdict is waiting in the spy, without taking anything else. */
static bool verdict_waiting(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            return false;
        }
        if ( (QSignal)TEST_RESULT_SIG == e->sig )
        {
            return true;
        }
    }
}

/** @brief A healthy row from up to its verdict: info, WiFi scan, BLE. */
static bool run_healthy_row(void)
{
    ask_row();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    finish_info();
    finish_rest();
    return take_verdict();
}

/* ---- Bring-up ---------------------------------------------------------------- */

/* A fresh board has no `wifi` and no `ble` key, so the radio boots to off
 * and asks the worker for nothing: the C5 runs its own firmware unbothered
 * until somebody wants a conversation. */
TEST_CASE("AO_Radio claims the C5 and boots to off with both flags clear",
          "[radio_ao]")
{
    radio_up_once();

    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    TEST_ASSERT_EQUAL_UINT(0U, job_submits());
    TEST_ASSERT_FALSE(job_held());
}

/* ---- WiFi join ---------------------------------------------------------------- */

/* wifi_join_request() is the request helper under test here, not the state
 * machine: it builds WifiJoinEvt and posts it straight to AO_Radio's queue,
 * so the event is read back off that queue before anything dispatches it -
 * the same way next_storage_op() reads AO_Storage's without running it. */
TEST_CASE("wifi_join_request carries the password and the remember flag as given",
          "[radio_ao]")
{
    const QEvt *e = NULL;

    radio_fresh();
    /* No store on this side any more: an empty password stays empty, and a
     * saved network is joined by the C5's vault instead. */
    wifi_join_request("join-test-net", "", true);
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_INT((int)WIFI_JOIN_REQ_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING("", ((const WifiJoinEvt *)e)->pass);
    TEST_ASSERT_TRUE(((const WifiJoinEvt *)e)->remember);
    QF_gc(e);

    wifi_join_request("factory-ap", "12345678", false);
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_STRING("12345678", ((const WifiJoinEvt *)e)->pass);
    TEST_ASSERT_FALSE(((const WifiJoinEvt *)e)->remember);
    QF_gc(e);
}

/* ---- The Wi-Fi doors and their margin ------------------------------------------- */

/** @brief Most fillers a case posts to reach a depth, so a bug cannot loop. */
#define DOOR_FILL_GUARD  64U

/** @brief The pool the join event comes from (the first the harness lays out). */
#define DOOR_POOL        1U

/** @brief Take every event out of AO_Radio's queue and recycle it. */
static void radio_queue_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Post fillers until exactly @p spaces slots of AO_Radio's queue are free. */
static void radio_queue_fill_to(UBaseType_t spaces)
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    for ( uint32_t guard = 0U;
         (DOOR_FILL_GUARD > guard) && (uxQueueSpacesAvailable(AO_Radio->eQueue) > spaces);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(spaces, uxQueueSpacesAvailable(AO_Radio->eQueue));
}

/** @brief Take the one event waiting and check its signal. */
static const QEvt *radio_queue_take(enum_t sig)
{
    const QEvt *e = NULL;

    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sig, (uint32_t)e->sig);
    return e;
}

TEST_CASE("the Wi-Fi doors post while the queue has room", "[radio_ao][doors]")
{
    radio_fresh();
    radio_queue_flush();

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw", true));
    QF_gc(radio_queue_take(WIFI_JOIN_REQ_SIG));
    TEST_ASSERT_TRUE(wifi_auto_request());
    QF_gc(radio_queue_take(WIFI_AUTO_REQ_SIG));
    TEST_ASSERT_TRUE(wifi_leave_request());
    QF_gc(radio_queue_take(WIFI_LEAVE_REQ_SIG));
}

TEST_CASE("a Wi-Fi door refuses once the queue is within its margin and keeps the pool whole",
          "[radio_ao][doors]")
{
    radio_fresh();
    radio_queue_flush();
    radio_queue_fill_to(KQP_RADIO_POST_MARGIN);
    uint16_t const pool = QF_getPoolFree(DOOR_POOL);

    TEST_ASSERT_FALSE(wifi_join_request("net", "pw", true));
    TEST_ASSERT_FALSE(wifi_auto_request());
    TEST_ASSERT_FALSE(wifi_leave_request());

    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(pool, QF_getPoolFree(DOOR_POOL),
                                     "the refused join event was not recycled");
    radio_queue_flush();
}

TEST_CASE("a Wi-Fi door takes the last slot above its margin and no more",
          "[radio_ao][doors]")
{
    radio_fresh();
    radio_queue_flush();
    radio_queue_fill_to(KQP_RADIO_POST_MARGIN + 1U);
    uint16_t const pool = QF_getPoolFree(DOOR_POOL);

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw", true));
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    TEST_ASSERT_EQUAL_UINT16(pool - 1U, QF_getPoolFree(DOOR_POOL));
    TEST_ASSERT_FALSE(wifi_join_request("net", "pw", true));
    TEST_ASSERT_EQUAL_UINT16(pool - 1U, QF_getPoolFree(DOOR_POOL));

    radio_queue_flush();
    TEST_ASSERT_EQUAL_UINT16(pool, QF_getPoolFree(DOOR_POOL));
}

/* ---- A Wi-Fi request that arrives while the radio cannot serve it ------------------ */

/** @brief Whether the job inside the worker is a join to @p ssid. */
static bool joining(const char *ssid)
{
    return (running_job() == (int)KDRV_RADIO_JOB_WIFI_JOIN)
           && (strcmp(held_req()->ssid, ssid) == 0);
}

/** @brief Take Wi-Fi off and end whatever runs, so the next case starts clean. */
static void wifi_owed_cleanup(void)
{
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

TEST_CASE("a join that arrives while the link is starting is made once it is up",
          "[radio_ao][owed]")
{
    radio_fresh();
    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_TRUE(joining("net"));
    TEST_ASSERT_EQUAL_STRING("pw12345678", held_req()->pass);
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    TEST_ASSERT_FALSE(job_held());
    wifi_owed_cleanup();
}

TEST_CASE("a join that arrives while a job is in flight is made when the job ends",
          "[radio_ao][owed]")
{
    radio_fresh();
    go_up();
    wifi_scan_request();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_TRUE(joining("net"));
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    TEST_ASSERT_FALSE(job_held());
    wifi_owed_cleanup();
}

/* The Wi-Fi page asks for a sweep every few seconds it is open, and a sweep
 * is about 38 s: a join waiting behind a whole one is what made joining from
 * the page slow. The join cuts it; here the cut is in before the fake worker
 * starts the sweep, so not one step runs. The sweep's end withdraws the cut:
 * the next sweep runs whole. */
TEST_CASE("a join that arrives during a listed scan cuts the sweep short",
          "[radio_ao][owed]")
{
    radio_fresh();
    go_up();
    wifi_scan_request();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();

    seed_air(2U, -50);
    run_scan_rc(KODE_DENIED);
    TEST_ASSERT_EQUAL_UINT(0U, s_starts);
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_TRUE(joining("net"));
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    TEST_ASSERT_FALSE(job_held());
    wifi_owed_cleanup();

    run_scan();
    TEST_ASSERT_TRUE(1U < s_starts);
}

TEST_CASE("of two joins that arrive while a job is in flight only the second is made",
          "[radio_ao][owed]")
{
    radio_fresh();
    go_up();
    wifi_scan_request();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    TEST_ASSERT_TRUE(wifi_join_request("first", "pw-first-1", false));
    pump();
    TEST_ASSERT_TRUE(wifi_join_request("second", "pw-second", false));
    pump();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_TRUE(joining("second"));
    TEST_ASSERT_EQUAL_STRING("pw-second", held_req()->pass);
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    TEST_ASSERT_FALSE(job_held());
    wifi_owed_cleanup();
}

TEST_CASE("a join that arrives with the link off brings the link up and is made",
          "[radio_ao][owed]")
{
    radio_fresh();
    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_TRUE(joining("net"));
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    wifi_owed_cleanup();
}

TEST_CASE("a leave that arrives while a join is owed clears the join",
          "[radio_ao][owed]")
{
    radio_fresh();
    go_up();
    wifi_scan_request();
    pump();
    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_TRUE(wifi_leave_request());
    pump();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_FALSE(job_held());
    wifi_owed_cleanup();
}

/**
 * @brief The last Wi-Fi state in the spy, kept: every other event is read past.
 * @param out Where it goes.
 * @return true when one was published since the last look.
 */
static bool wifi_state_last(WifiStateEvt *out)
{
    bool found = false;

    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (QSignal)WIFI_STATE_SIG == e->sig )
        {
            *out  = *(const WifiStateEvt *)e;
            found = true;
        }
    }
    return found;
}

/** @brief Ask where Wi-Fi stands and read the answer. */
static WifiStateEvt wifi_state_now(void)
{
    WifiStateEvt w;

    ao_spy_drain();
    dispatch(WIFI_STATE_REQ_SIG);
    TEST_ASSERT_TRUE(wifi_state_last(&w));
    return w;
}

TEST_CASE("a join's state says it is still out until it has ended, and why it "
          "failed", "[radio_ao][owed]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_FALSE(wifi_state_now().joining);

    /* Behind a sweep it waits: not an answer. */
    wifi_scan_request();
    pump();
    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_TRUE(wifi_state_now().joining);
    run_scan_rc(KODE_DENIED);   /* the join cuts the sweep it is behind */
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);

    /* With the worker: not an answer either. */
    TEST_ASSERT_TRUE(joining("net"));
    TEST_ASSERT_TRUE(wifi_state_now().joining);

    /* The fake worker runs no join, so the stack's two events stand for it:
     * the address clears what an earlier case left, the refusal says why. */
    wifi_event_sta_disconnected_t ev;

    (void)memset(&ev, 0, sizeof(ev));
    ev.reason = (uint16_t)WIFI_REASON_AUTH_FAIL;
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP,
                             &(ip_event_got_ip_t){ 0 });
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev);
    ao_spy_drain();
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_DENIED);

    WifiStateEvt w;

    TEST_ASSERT_TRUE(wifi_state_last(&w));
    TEST_ASSERT_FALSE(w.joining);
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_WIFI_DISCONNECTED, (unsigned)w.state);
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_WIFI_FAIL_PASSWORD, (unsigned)w.fail);
    wifi_owed_cleanup();
}

TEST_CASE("an autojoin that arrives with the link off brings the link up and runs",
          "[radio_ao][owed]")
{
    radio_fresh();
    TEST_ASSERT_TRUE(wifi_auto_request());
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    wifi_owed_cleanup();
}

TEST_CASE("a join that arrives during the autojoin round is made after the round",
          "[radio_ao][owed]")
{
    radio_fresh();
    go_up();
    vault_has("line-ap-0");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_TRUE(joining("net"));
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    wifi_owed_cleanup();
}

/* ---- The row and the link -------------------------------------------------------- */

/* The row is a test, not a switch: it brings the link up because it has to,
 * and writes nothing into the settings store on the way. */
TEST_CASE("the c5 row on a link that is off brings the link up without touching the wifi or ble flags",
          "[radio_ao]")
{
    radio_fresh();
    unsigned const nvs_before = fake_nvs_count();

    ask_row();

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_STARTING, take_link());
    TEST_ASSERT_EQUAL_UINT(nvs_before, fake_nvs_count());
}

TEST_CASE("the c5 row walks the card around the link: unmount before the connect, mount after",
          "[radio_ao]")
{
    radio_fresh();

    ask_row();
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    TEST_ASSERT_EQUAL_INT(-1, next_storage_op());

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_MOUNT, next_storage_op());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
}

/* The unmount is a call on AO_Storage's worker, so it has not run when the
 * request is posted: the C5 is reset only after AO_Storage has said it has. */
TEST_CASE("the connect waits for the card's unmount to be answered",
          "[radio_ao]")
{
    unsigned before;

    radio_fresh();
    s_gate_auto = false;
    before = job_submits();

    dispatch(RADIO_UP_REQ_SIG);

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
                                  "the C5 was reset before the card let go");
    TEST_ASSERT_FALSE(job_queued());
    TEST_ASSERT_EQUAL_UINT(before, job_submits());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_STARTING, link_now());

    kdrv_storage_unmount(KDRV_MEDIUM_SD);
    dispatch(STORAGE_CMD_RSP_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

/* A refused unmount is asked again: the radio's own window, as it names it. */
#define UNMOUNT_RETRY_MS  500U     /**< Between one ask and the next.            */
#define UNMOUNT_WAIT_MS   10000U   /**< Asks go on this long, then give up.      */

/** @brief AO_Storage's answer to an unmount it refused: the card stays mounted. */
static void unmount_refused(void)
{
    static const QEvt rsp = QEVT_INITIALIZER(STORAGE_CMD_RSP_SIG);

    QASM_DISPATCH(AO_Radio, &rsp, 0U);
    (void)radio_settle();
}

/**
 * @brief Let the radio ask again after a refusal, @p asks times, and refuse each.
 *
 * Each round is one retry period: the expiry makes the radio ask the card to
 * let go again (read off the storage queue), and the card refuses once more.
 * The first ask is not consumed here.
 *
 * @param asks How many times the radio is asked again and refuses.
 */
static void refuse_again(unsigned asks)
{
    for ( unsigned i = 0U; i < asks; ++i )
    {
        tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS));
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)STORAGE_OP_UNMOUNT, next_storage_op(),
            "the refused unmount was not asked again");
        unmount_refused();
    }
}

/* The answer comes back when the unmount was refused (a transfer in flight),
 * when the worker is not up, or when the call overran: the card is then still
 * mounted, and the reset would glitch its supply under a live volume. It is
 * asked again every half second for ten before the bring-up gives up. */
TEST_CASE("a card that is still mounted at the answer stops the bring-up "
          "with the C5 untouched", "[radio_ao]")
{
    unsigned before;

    radio_fresh();
    s_gate_auto = false;
    before = job_submits();
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_SD));

    ask_row();
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();

    /* Refused once is not the end: the radio waits and asks again. */
    TEST_ASSERT_EQUAL_INT((int)KRADIO_STARTING, link_now());
    TEST_ASSERT_FALSE(logged("stayed busy"));
    refuse_again((UNMOUNT_WAIT_MS / UNMOUNT_RETRY_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_STARTING, link_now(),
                                  "the bring-up gave up before the window");
    TEST_ASSERT_FALSE(logged("stayed busy"));
    tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();

    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, job_submits(),
                                   "the connect went out over a mounted card");
    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("the SD card stayed busy for 10 s; link not started"));
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the card is still mounted"));
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    /* and the card goes back onto the plain path */
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_MOUNT, next_storage_op());
}

TEST_CASE("a refused unmount is asked again, and the link comes up once the "
          "card lets go", "[radio_ao]")
{
    radio_fresh();
    s_gate_auto = false;
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_SD));

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();
    refuse_again(2U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_STARTING, link_now());

    /* The transfer is over: the next ask is honoured. */
    tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    kdrv_storage_unmount(KDRV_MEDIUM_SD);
    dispatch(STORAGE_CMD_RSP_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_FALSE(logged("stayed busy"));
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());

    /* No expiry is left over to ask a settled card again. */
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_MOUNT, next_storage_op());
    tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS) * 2U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, next_storage_op(),
                                  "the card was asked again after it let go");
}

TEST_CASE("the c5 row on a link that is up submits the info job and never a connect",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    unsigned const before = job_submits();

    ask_row();

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    TEST_ASSERT_EQUAL_UINT(before + 1U, job_submits());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("the c5 row asked while the link is coming up rides that bring-up",
          "[radio_ao]")
{
    radio_fresh();
    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    ask_row();
    TEST_ASSERT_FALSE_MESSAGE(job_queued(), "a second connect was asked for");

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
}

TEST_CASE("the c5 row ends with a verdict when the worker will not take the connect",
          "[radio_ao]")
{
    radio_fresh();
    fake_khal_worker_refuse_for(RADIO_WORKER, true);

    ask_row();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the worker refused the connect job"));
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    /* and the card went back where it was */
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_MOUNT, next_storage_op());
}

TEST_CASE("the c5 row fails when the C5 never answers CP_INIT", "[radio_ao]")
{
    radio_fresh();
    ask_row();

    finish(KDRV_RADIO_JOB_CONNECT, KODE_TIMEOUT);

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - no CP_INIT"));
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
}

/* `radio_down` is not one-way: the link is built again for whoever asks. The
 * row is refused only while the teardown is still running, and says so. */
TEST_CASE("the c5 row is refused while radio_down runs, and builds the link "
          "again once it is off", "[radio_ao]")
{
    radio_fresh();
    go_up();

    dispatch(RADIO_DOWN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    ask_row();
    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the link is on its way down"));

    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    ao_spy_drain();
    fake_klog_reset();

    ask_row();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_CONNECT, running_job(),
                                  "the link was not asked for a second time "
                                  "in one boot");
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("a link lost mid-row fails the row and tears down as usual", "[radio_ao]")
{
    radio_fresh();
    go_up();
    ask_row();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());

    fake_radio_drop_link();
    pump();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the link dropped"));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());

    /* The row's info job is still inside the worker, using its storage: the
     * teardown is owed, the channel is held for it, and it goes the moment
     * that job is back - whose answer is about a link that is gone. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    TEST_ASSERT_FALSE_MESSAGE(job_queued(), "the disconnect was queued over a job");
    finish(KDRV_RADIO_JOB_C5_INFO, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_DISCONNECT, running_job(),
                                  "no disconnect was ordered");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
}

/* ---- What the row judges ----------------------------------------------------------- */

TEST_CASE("the c5 row scans after the info and passes on a strong network",
          "[radio_ao]")
{
    radio_fresh();
    go_up();

    TEST_ASSERT_TRUE(run_healthy_row());
    TEST_ASSERT_TRUE(logged("test c5: PASS"));
}

TEST_CASE("the c5 row logs every reading before a PASS", "[radio_ao]")
{
    radio_fresh();
    go_up();
    kdrv_c5_note_reset(1);
    kdrv_c5_note_link_ms(870U);

    TEST_ASSERT_TRUE(run_healthy_row());

    TEST_ASSERT_TRUE(logged("test c5: chip=esp32c5 id=23"));
    TEST_ASSERT_TRUE(logged("test c5: fw slave=2.12.9 host=2.12.9"));
    TEST_ASSERT_TRUE(logged("test c5: app project=kode_c5 version=2.12.9 idf=v5.5.1 "
                            "date=Sep  5 2026 time=10:00:00 sha=a0a1a2a3"));
    TEST_ASSERT_TRUE(logged("test c5: mac=38:44:be:1c:1d:7e"));
    TEST_ASSERT_TRUE(logged("test c5: reset=1 (power-on) link_ms=870"));
    TEST_ASSERT_TRUE(logged("test c5: heap free=180KB largest=96KB"));
    TEST_ASSERT_TRUE(logged("test c5: sdio width=4 khz=40000"));
    TEST_ASSERT_TRUE(logged("test c5: sd_cd=1"));
    TEST_ASSERT_TRUE(logged("test c5: scan n=3 best='line-ap-0' rssi=-52 ch=1 5ghz=1"));
    TEST_ASSERT_TRUE(logged("test c5: ble addr=38:44:be:1c:1d:06 state=idle"));
    TEST_ASSERT_TRUE(logged("test c5: ble scan n=2 best=c0:00:00:00:00:00 rssi=-60 "
                            "name='beacon-0'"));
    TEST_ASSERT_TRUE(log_index("test c5: mac=") < log_index("test c5: PASS"));
    TEST_ASSERT_TRUE(log_index("test c5: ble scan") < log_index("test c5: PASS"));
}

TEST_CASE("the c5 row fails naming a stock slave when project_name is not kode_c5",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_set_project("network_adapter");

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - project_name 'network_adapter' is not "
                            "kode_c5: stock esp-hosted slave"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a failed row must not scan");
}

TEST_CASE("the c5 row logs the same readings before a FAIL at the project-name check",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_set_project("network_adapter");

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: chip=esp32c5 id=23"));
    TEST_ASSERT_TRUE(logged("test c5: mac=38:44:be:1c:1d:7e"));
    TEST_ASSERT_TRUE(logged("test c5: fw slave=2.12.9 host=2.12.9"));
    TEST_ASSERT_TRUE(log_index("test c5: mac=") < log_index("test c5: FAIL"));
}

TEST_CASE("the c5 row fails naming a stock or old slave when the app-descriptor RPC is unsupported",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_app_desc_unsupported(true);

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - stock or old slave: the app-descriptor "
                            "RPC went unanswered"));
}

TEST_CASE("the c5 row prints a dash for the descriptor fields an old slave does not send",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_app_desc_unsupported(true);

    ask_row();
    finish_info();

    (void)take_verdict();
    TEST_ASSERT_TRUE(logged("test c5: app project=- version=- idf=- date=- time=- sha=-"));
}

TEST_CASE("the c5 row fails when the slave's esp-hosted version is not the host's",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_set_fw(2U, 11U, 0U);

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - slave esp-hosted 2.11.0 is not the host's 2.12.9"));
}

TEST_CASE("the c5 row fails when the chip is not an ESP32-C5", "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_set_chip(13U, "esp32c6");

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - chip id 13 (esp32c6) is not an ESP32-C5"));
}

TEST_CASE("the c5 row fails when the SDIO link is not four lines at 40 MHz",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_set_sdio(40000U, 1U);

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - SDIO came up 1-bit @40000kHz, not 4-bit "
                            "@40000kHz: a data line is not reaching the C5"));
}

TEST_CASE("the c5 row fails when the RPC GPIO service does not answer", "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_gpio_dead(true);

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the RPC GPIO service did not answer"));
}

TEST_CASE("the c5 row fails when the coprocessor will not describe itself",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_hosted_link_down(true);

    ask_row();
    finish_info();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the coprocessor would not describe itself"));
}

TEST_CASE("the c5 row fails when the scan sees nothing", "[radio_ao]")
{
    radio_fresh();
    go_up();
    ask_row();
    finish_info();

    finish_scan(0U, 0);

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: scan n=0"));
    TEST_ASSERT_TRUE(logged("test c5: FAIL - no network in range"));
}

TEST_CASE("the c5 row fails when the strongest network is under the floor",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    ask_row();
    finish_info();

    finish_scan(2U, -78);

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the strongest network is -78 dBm, under "
                            "the -70 dBm floor"));
}

TEST_CASE("the c5 row fails when the scan itself fails", "[radio_ao]")
{
    radio_fresh();
    go_up();
    ask_row();
    finish_info();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the scan failed"));
}

/* The listing is a `wifi_scan` answer, and nobody typed one. */
TEST_CASE("the c5 row does not publish the scan it ran for itself", "[radio_ao]")
{
    radio_fresh();
    go_up();

    TEST_ASSERT_TRUE(run_healthy_row());

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            break;
        }
        TEST_ASSERT_NOT_EQUAL_MESSAGE((int)WIFI_SCAN_END_SIG, (int)e->sig,
                                      "the row's scan went out as a listing");
        /* The sweep knocks on every channel it finds something on, whoever
         * asked for it. What decides whether that becomes a listing is the
         * AO, and here nobody typed `wifi_scan`. */
        TEST_ASSERT_NOT_EQUAL_MESSAGE((int)WIFI_SCAN_ENTRY_SIG, (int)e->sig,
                                      "the row's scan published an entry");
        TEST_ASSERT_NOT_EQUAL_MESSAGE((int)BLE_SCAN_END_SIG, (int)e->sig,
                                      "the row's BLE listen went out as a listing");
    }
}

/* ---- The listing, as the sweep produces it ----------------------------------------- */

/* The reason the sweep walks channels at all: what it finds on the first one
 * is on the bus while the rest of the air is still ahead of it. The console
 * prints an entry the moment it arrives, so this is the whole feature. */
TEST_CASE("a typed scan lists its networks before the sweep is over",
          "[radio_ao]")
{
    unsigned entries = 0U;

    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    seed_air(3U, -50);
    run_scan();   /* the sweep knocks when its first channel is in */
    pump();       /* and the knock reaches the AO */

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            break;
        }
        TEST_ASSERT_NOT_EQUAL_MESSAGE((int)WIFI_SCAN_END_SIG, (int)e->sig,
                                      "the listing closed before the sweep did");
        if ( (QSignal)WIFI_SCAN_ENTRY_SIG == e->sig )
        {
            ++entries;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, entries,
                                   "nothing reached the bus until the job ended");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);

    /* The end marker counts what actually went out, and the entries already
     * published are not published again: `scan_sent` is the only bookkeeping
     * and a repeat would show up here as a fourth. */
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the listing was never closed");
        if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            TEST_ASSERT_TRUE(((const WifiScanEndEvt *)e)->ok);
            TEST_ASSERT_EQUAL_UINT8(3U, ((const WifiScanEndEvt *)e)->count);
            break;
        }
        TEST_ASSERT_NOT_EQUAL_MESSAGE((int)WIFI_SCAN_ENTRY_SIG, (int)e->sig,
                                      "a network was listed twice");
    }
}

/* A listing in flight when the link drops has a console waiting on its end
 * marker with the watchdog disarmed: without one it waits for good. */
TEST_CASE("a scan the link outlives is closed, not abandoned", "[radio_ao]")
{
    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    seed_air(2U, -50);
    run_scan();
    pump();

    finish_lost(KODE_ERROR);

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the listing was left open");
        if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            /* Not ok - the sweep never finished - but the two networks it
             * did find were real, and the count says so. */
            TEST_ASSERT_FALSE(((const WifiScanEndEvt *)e)->ok);
            TEST_ASSERT_EQUAL_UINT8(2U, ((const WifiScanEndEvt *)e)->count);
            break;
        }
    }
    TEST_ASSERT_TRUE(logged("the link dropped mid-scan"));
}

/* ---- A scan asked for by an AO or a blocked task ----------------------------------
 * wifi_scan_ask() and wifi_scan_request_wait(): the sweep the console's `wifi_scan`
 * runs, its entries posted to the asker instead of published, and an end that
 * says how many networks the sweep handed over and whether some did not fit.
 * The spy is subscribed to the console's listing, so a published entry or end
 * would land in it as well and show up here.
 */

/** @brief A static event the spy never reads, to fill its queue with. */
static const QEvt s_scan_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief Events put in the spy's queue ahead of a scan, to leave it small. */
#define SCAN_QUEUE_TAKEN  20U

/** @brief Entries that fit the spy's queue past SCAN_QUEUE_TAKEN, the free slots kept. */
#define SCAN_SMALL_PAGE  (AO_SPY_QUEUE_LEN - SCAN_QUEUE_TAKEN - KODE_STORAGE_LIST_FREE_SLOTS)

/** @brief Networks in the air for the small queue: more than SCAN_SMALL_PAGE. */
#define SCAN_MANY  8U

/** @brief What one asker received: entries, the end, and anything else. */
typedef struct {
    unsigned              entries;  /**< WIFI_SCAN_ENTRY_SIG.             */
    unsigned              ends;     /**< WIFI_SCAN_RSP_SIG.               */
    unsigned              bus_ends; /**< WIFI_SCAN_END_SIG: the console's. */
    unsigned              other;    /**< Anything else.                   */
    const ScanRspEvt      *end;     /**< The last end.                    */
} scan_seen_t;

/** @brief Read everything the spy holds. */
static scan_seen_t scan_take_spy(void)
{
    scan_seen_t s = { 0U, 0U, 0U, 0U, NULL };
    const QEvt *e;

    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)WIFI_SCAN_ENTRY_SIG == e->sig )
        {
            ++s.entries;
        }
        else if ( (QSignal)WIFI_SCAN_RSP_SIG == e->sig )
        {
            ++s.ends;
            s.end = (const ScanRspEvt *)(const void *)e;
        }
        else if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            ++s.bus_ends;
        }
        else
        {
            ++s.other;
        }
    }
    return s;
}

/** @brief The sweep the held scan job runs: @p n networks, the strongest at @p top. */
static void scan_sweep(unsigned n, int8_t top, kode_status_t rc)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    seed_air(n, top);
    run_scan();
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, rc);
}

TEST_CASE("an asked scan posts its networks to the asker, publishes none, and its door opens at the end",
          "[radio_ao][ask]")
{
    scan_seen_t s;

    radio_fresh();
    go_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_wifi_scan(ao_spy()),
                                  "a second scan of the asker's waits for the first's end");
    pump();
    scan_sweep(3U, -50, KODE_OK);

    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT(3U, s.entries);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.ends, "the asker hears one end");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.bus_ends, "the console's end was published as well");
    TEST_ASSERT_EQUAL_UINT(0U, s.other);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, s.end->rc);
    TEST_ASSERT_EQUAL_UINT8(3U, s.end->seen);
    TEST_ASSERT_FALSE(s.end->more);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, wifi_scan_ask(ao_spy()), "the end opens the door again");
    pump();
    scan_sweep(1U, -60, KODE_TIMEOUT);
    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_ERROR, s.end->rc, "a sweep cut short is a failure");
    TEST_ASSERT_EQUAL_UINT8(1U, s.end->seen);
    ao_spy_drain();
}

TEST_CASE("an asked scan leaves the free slots in the asker's queue and says more",
          "[radio_ao][ask]")
{
    scan_seen_t s;

    radio_fresh();
    go_up();
    for ( unsigned i = 0U; SCAN_QUEUE_TAKEN > i; ++i )
    {
        QACTIVE_POST(ao_spy(), &s_scan_filler, NULL);
    }

    TEST_ASSERT_EQUAL_INT(KODE_OK, wifi_scan_ask(ao_spy()));
    pump();
    scan_sweep(SCAN_MANY, -40, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KODE_STORAGE_LIST_FREE_SLOTS - 1U,
                                     (uint32_t)uxQueueSpacesAvailable(ao_spy()->eQueue),
                                     "the entries took a slot they promised to leave free");

    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT(SCAN_QUEUE_TAKEN, s.other);
    TEST_ASSERT_EQUAL_UINT(SCAN_SMALL_PAGE, s.entries);
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, s.end->rc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(SCAN_MANY, s.end->seen, "the end counts what the sweep saw");
    TEST_ASSERT_TRUE_MESSAGE(s.end->more, "entries that did not fit say more");
    ao_spy_drain();
}

TEST_CASE("the console's scan still publishes, and is turned away while an asked one runs",
          "[radio_ao][ask]")
{
    scan_seen_t s;

    radio_fresh();
    go_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, wifi_scan_ask(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    dispatch(WIFI_SCAN_REQ_SIG);
    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.bus_ends, "the console was not answered");
    TEST_ASSERT_EQUAL_UINT(0U, s.ends);
    ao_spy_drain();
    scan_sweep(2U, -50, KODE_OK);
    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT(2U, s.entries);
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT(0U, s.bus_ends);
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    scan_sweep(2U, -50, KODE_OK);
    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s.entries, "the console's listing went missing");
    TEST_ASSERT_EQUAL_UINT(1U, s.bus_ends);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.ends, "the console's scan answered an asker");
    ao_spy_drain();
}

TEST_CASE("with no link an asked scan brings the link up and scans once it is up",
          "[radio_ao][ask]")
{
    scan_seen_t s;
    unsigned    guard = 0U;

    radio_fresh();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());

    TEST_ASSERT_EQUAL_INT(KODE_OK, wifi_scan_ask(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    /* Whatever the bring-up reconciles goes first; the scan waits its turn. */
    while ( (running_job() != (int)KDRV_RADIO_JOB_WIFI_SCAN) && (8U > guard) )
    {
        TEST_ASSERT_TRUE_MESSAGE(job_held(), "the owed scan never started");
        finish_whatever_runs();
        ++guard;
    }
    ao_spy_drain();
    scan_sweep(2U, -55, KODE_OK);
    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT(2U, s.entries);
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, s.end->rc);
    TEST_ASSERT_EQUAL_UINT8(2U, s.end->seen);
    ao_spy_drain();
}

TEST_CASE("an asked scan the link drops under is ended, not abandoned",
          "[radio_ao][ask]")
{
    scan_seen_t s;

    radio_fresh();
    go_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, wifi_scan_ask(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    seed_air(2U, -50);
    run_scan();
    pump();
    finish_lost(KODE_ERROR);

    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.ends, "the asker was left waiting");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_ERROR, s.end->rc, "a lost link is a failure");
    TEST_ASSERT_EQUAL_UINT8(2U, s.end->seen);
    TEST_ASSERT_EQUAL_UINT(0U, s.bus_ends);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, wifi_scan_ask(ao_spy()), "the door stayed shut");
    pump();
    ao_spy_drain();
}

TEST_CASE("a second asker while a scan is held is refused in its end, at once",
          "[radio_ao][ask]")
{
    const QEvt *o;
    scan_seen_t s;

    radio_fresh();
    go_up();
    ao_spy_other_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, wifi_scan_ask(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, wifi_scan_ask(ao_spy_other()),
                                  "the door is the asker's, not the kind's");
    pump();
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker was not answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)WIFI_SCAN_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, ((const ScanRspEvt *)(const void *)o)->rc);
    TEST_ASSERT_EQUAL_UINT8(0U, ((const ScanRspEvt *)(const void *)o)->seen);
    QF_gc(o);
    TEST_ASSERT_NULL(ao_spy_other_take());

    scan_sweep(1U, -50, KODE_OK);
    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, s.end->rc);
    ao_spy_drain();
}

/** @brief The one answer an asked scan got at once, or NULL. */
static const ScanRspEvt *scan_answered_at_once(void)
{
    scan_seen_t const s = scan_take_spy();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.entries, "a refused scan listed networks");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.ends, "the asker was not answered at once");
    return s.end;
}

TEST_CASE("an asked scan while the console's sweep runs is refused at once, busy",
          "[radio_ao][ask]")
{
    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, scan_answered_at_once()->rc);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_wifi_scan(ao_spy()),
                                  "the refusal left the door shut");
    pump();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, scan_answered_at_once()->rc);
    ao_spy_drain();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    ao_spy_drain();
}

TEST_CASE("an asked scan while the autojoin round runs is refused at once, busy",
          "[radio_ao][ask]")
{
    radio_fresh();
    go_up();
    vault_has("line-ap-0");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, scan_answered_at_once()->rc);
    ao_spy_drain();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
}

TEST_CASE("an asked scan owed behind the bring-up is refused when the boot's autojoin round starts",
          "[radio_ao][ask]")
{
    unsigned guard = 0U;

    radio_fresh();
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, scan_take_spy().ends, "an owed scan was answered before the link was up");

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    pump();
    while ( (running_job() != (int)KDRV_RADIO_JOB_WIFI_SCAN) && (8U > guard) )
    {
        TEST_ASSERT_TRUE_MESSAGE(job_held(), "the round never started");
        finish_whatever_runs();
        ++guard;
    }
    {
        const QEvt *e;
        const ScanRspEvt *end = NULL;

        while ( (e = ao_spy_take()) != NULL )
        {
            if ( (QSignal)WIFI_SCAN_RSP_SIG == e->sig )
            {
                end = (const ScanRspEvt *)(const void *)e;
            }
            TEST_ASSERT_NOT_EQUAL_MESSAGE((int)WIFI_SCAN_ENTRY_SIG, (int)e->sig,
                                          "the round's networks went to the asker");
        }
        TEST_ASSERT_NOT_NULL_MESSAGE(end, "the owed scan was left waiting behind the round");
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, end->rc);
    }
    ao_spy_drain();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
}

TEST_CASE("an asked scan owed with no job in flight is refused when a C5 update starts",
          "[radio_ao][ask]")
{
    FlashReqEvt       f;
    StorageOpenRspEvt r;
    scan_seen_t       s;

    radio_fresh();
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    /* The link comes up by hand, nothing it posts to itself delivered yet:
     * the chain's autojoin and the owed scan's kick wait in AO_Radio's queue,
     * no job holds the channel, and the scan is still owed. */
    fake_radio_effects(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    deliver_held(KODE_OK);
    dispatch(STORAGE_CMD_RSP_SIG);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a job holds the channel");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, scan_take_spy().ends, "the owed scan was answered already");
    storage_drain();

    (void)memset(&f, 0, sizeof(f));
    f.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    f.src_kind = (uint8_t)KFW_SRC_FS;
    f.dst_kind = (uint8_t)KFW_TARGET_C5;
    (void)snprintf(f.src, sizeof(f.src), "%s", "c5_slave.bin");
    dispatch_evt(&f.super);
    TEST_ASSERT_EQUAL_INT(-2, next_storage_op());   /* the open: the update has begun */

    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.ends, "the owed scan waits behind an update");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, s.end->rc);
    TEST_ASSERT_EQUAL_UINT(0U, s.entries);
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, scan_take_spy().ends, "the scan was answered twice");
    ao_spy_drain();

    /* Out of the update the short way: the file was not there. */
    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(STORAGE_OPEN_RSP_SIG);
    r.status = (uint8_t)KODE_ERROR;
    dispatch_evt(&r.super);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
}

TEST_CASE("a task's scan while a sweep runs is answered at once, busy, in its waiter",
          "[radio_ao][ask]")
{
    kdrv_wifi_ap_t rows[KDRV_WIFI_SCAN_MAX];
    ScanWaiter     w;

    radio_fresh();
    go_up();
    ao_spy_drain();
    (void)memset(&w, 0, sizeof(w));
    w.done = khal_sem_create();
    w.rows = rows;
    w.cap  = (uint8_t)KDRV_WIFI_SCAN_MAX;

    dispatch(WIFI_SCAN_REQ_SIG);
    wifi_scan_request_wait(&w, 11U);
    pump();
    TEST_ASSERT_EQUAL_UINT32(11U, w.ack);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_NO_RESOURCES, w.rc);
    TEST_ASSERT_EQUAL_UINT8(0U, w.got);

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    ao_spy_drain();
}

TEST_CASE("a task's scan is written into its waiter, strongest kept, and tagged with its generation",
          "[radio_ao][ask]")
{
    kdrv_wifi_ap_t  rows[KDRV_WIFI_SCAN_MAX];
    ScanWaiter      w;
    scan_seen_t     s;
    unsigned const  signals = fake_sem_signals();

    radio_fresh();
    go_up();
    (void)memset(rows, 0, sizeof(rows));
    (void)memset(&w, 0, sizeof(w));
    w.done = khal_sem_create();
    w.rows = rows;
    w.cap  = (uint8_t)KDRV_WIFI_SCAN_MAX;
    w.rc   = (uint8_t)KODE_TIMEOUT;

    wifi_scan_request_wait(&w, 7U);
    pump();
    scan_sweep(3U, -45, KODE_OK);

    TEST_ASSERT_EQUAL_UINT32(7U, w.ack);
    TEST_ASSERT_EQUAL_UINT(signals + 1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, w.rc);
    TEST_ASSERT_EQUAL_UINT8(3U, w.got);
    TEST_ASSERT_EQUAL_STRING("line-ap-0", rows[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-45, rows[0].rssi);
    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.entries + s.ends + s.bus_ends, "a task's scan reached an AO");
    ao_spy_drain();

    /* A failed sweep is answered with nothing written. */
    (void)memset(rows, 0, sizeof(rows));
    wifi_scan_request_wait(&w, 8U);
    pump();
    scan_sweep(2U, -45, KODE_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT32(8U, w.ack);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_ERROR, w.rc);
    TEST_ASSERT_EQUAL_UINT8(0U, w.got);
    TEST_ASSERT_EQUAL_STRING("", rows[0].ssid);
    ao_spy_drain();
}

/* ---- The card comes back before anything is asked of the C5 ------------------------ */

TEST_CASE("the link is not up, and no service starts, until the card's remount has run",
          "[radio_ao]")
{
    radio_fresh();
    dispatch(WIFI_START_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    storage_drain();

    finish_only(KDRV_RADIO_JOB_CONNECT, KODE_OK);

    /* The remount is asked for, with an answer owed to this AO - and until
     * it comes the link is still on its way and the C5 is asked nothing. */
    const QEvt *e = NULL;
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Storage->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_CMD_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_MOUNT,
                          (int)((const StorageCmdEvt *)e)->op);
    TEST_ASSERT_EQUAL_PTR(AO_Radio, ((const StorageCmdEvt *)e)->who);
    QF_gc(e);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_STARTING, link_now());
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    dispatch(STORAGE_CMD_RSP_SIG);
    pump();

    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_TRUE(logged("looking for a network we know"));

    finish_whatever_runs();
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
}

TEST_CASE("an answer from the card that nobody is waiting for changes nothing",
          "[radio_ao]")
{
    radio_fresh();
    dispatch(STORAGE_CMD_RSP_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());

    go_up();
    dispatch(STORAGE_CMD_RSP_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_EQUAL_INT(-1, running_job());
}

/* ---- A C5 that stops answering ----------------------------------------------------- */

TEST_CASE("a C5 that goes mute is reset by building the link again, once",
          "[radio_ao]")
{
    radio_fresh();
    go_up();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish_lost(KODE_TIMEOUT);

    /* Torn down first, then built again without anybody asking. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_TRUE(logged("the C5 went mute; resetting it"));

    /* Mute again before it has answered a single job: not a third time. */
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish_lost(KODE_TIMEOUT);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    TEST_ASSERT_TRUE(logged("the C5 went mute again after a reset"));
}

TEST_CASE("a job the C5 answers earns the reset back", "[radio_ao]")
{
    radio_fresh();
    go_up();

    dispatch(WIFI_SCAN_REQ_SIG);
    finish_lost(KODE_TIMEOUT);
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());

    /* The reset C5 answers a job. */
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    seed_air(1U, -50);
    run_scan();
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);

    /* So a silence now is a new one. */
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish_lost(KODE_TIMEOUT);
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

/* ---- A lost link, for somebody ------------------------------------------ */

/** @brief Whether the link's state says nothing more is coming by itself. */
static bool link_gave_up(void)
{
    ao_spy_drain();
    dispatch(RADIO_STATE_REQ_SIG);
    for ( const QEvt *e = ao_spy_take(); NULL != e; e = ao_spy_take() )
    {
        if ( (QSignal)RADIO_STATE_SIG == e->sig )
        {
            return ((const RadioStateEvt *)e)->gave_up;
        }
    }
    TEST_FAIL_MESSAGE("no link state");
    return false;
}

/** @brief Bluetooth wanted, the link up, its bring-up's jobs done with. */
static void relink_up(void)
{
    radio_fresh();
    go_up();
    dispatch(BLE_START_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

/** @brief Take the service off and walk the link down, for the next case. */
static void relink_end(void)
{
    storage_drain();
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

/** @brief The transport drops, and its teardown comes back. */
static void relink_drop(void)
{
    storage_drain();
    finish_lost(KODE_ERROR);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
}

TEST_CASE("a transport that drops under a wanted service is built again at once",
          "[radio_ao][relink]")
{
    relink_up();
    relink_drop();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_TRUE(logged("the link to the C5 dropped; building it again"));
    TEST_ASSERT_FALSE(link_gave_up());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    relink_end();
}

TEST_CASE("a bring-up that keeps failing is tried a bounded number of times, then "
          "the state says so", "[radio_ao][relink]")
{
    relink_up();
    relink_drop();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_ERROR);
    storage_drain();   /* AO_Storage does not run here: its asks pile up */
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    TEST_ASSERT_FALSE(link_gave_up());

    /* Waits, not a loop. */
    tick(QF_SUPPORT_STEPS(29000U));
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "tried before its wait");
    tick(QF_SUPPORT_STEPS(2000U));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_ERROR);
    storage_drain();
    tick(QF_SUPPORT_STEPS(121000U));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_ERROR);
    storage_drain();

    TEST_ASSERT_TRUE_MESSAGE(link_gave_up(), "out of tries and not saying so");
    unsigned const submits = job_submits();

    tick(QF_SUPPORT_STEPS(900000U));
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(submits, job_submits(), "tried after giving up");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());

    /* Somebody asking is what starts it over. */
    storage_drain();
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    dispatch(BLE_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_FALSE(link_gave_up());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    relink_end();
}

TEST_CASE("a transport that keeps dropping stops being built again", "[radio_ao][relink]")
{
    relink_up();
    for ( unsigned i = 0U; 3U > i; ++i )
    {
        relink_drop();
        TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
        finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
        while ( job_held() )
        {
            finish_whatever_runs();
        }
    }
    relink_drop();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_TRUE(link_gave_up());
    relink_end();
}

TEST_CASE("a C5 that goes mute twice with a service wanted says the state gave up",
          "[radio_ao][relink]")
{
    relink_up();
    dispatch(WIFI_SCAN_REQ_SIG);
    finish_lost(KODE_TIMEOUT);
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    /* Nothing answered since: its own bring-up's jobs are not run. */
    fake_klog_reset();
    finish_lost(KODE_TIMEOUT);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_TRUE(logged("the C5 went mute again"));
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    TEST_ASSERT_TRUE(link_gave_up());
    relink_end();
}

TEST_CASE("a link lost with the board going to sleep waits for it to wake",
          "[radio_ao][relink]")
{
    relink_up();
    lp_round(AO_Radio, KODE_LP_OFF);
    drain();
    relink_drop();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "the C5 was reset under a rest");
    TEST_ASSERT_FALSE(link_gave_up());
    tick(QF_SUPPORT_STEPS(200000U));
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    lp_awake(AO_Radio);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    relink_end();
}

TEST_CASE("a transport that drops by itself is not built again unasked",
          "[radio_ao]")
{
    radio_fresh();
    go_up();

    finish_lost(KODE_ERROR);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);

    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
}

/* ---- The BLE step ------------------------------------------------------------------ */

TEST_CASE("the c5 row starts the BLE stack after the WiFi scan and passes on an advertiser in range",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    unsigned const nvs_before = fake_nvs_count();

    ask_row();
    finish_info();
    finish_scan(3U, -52);

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_START, running_job());
    finish_ble_start();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT32(KODE_BLE_SCAN_MS, held_req()->len);
    finish_ble_scan(1U, -70);

    TEST_ASSERT_TRUE(take_verdict());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(nvs_before, fake_nvs_count(),
                                   "the row wrote a service flag");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_BLE_IDLE, (int)kdrv_ble_state(),
                                  "the stack stays up and idle: it cannot go down this boot");
}

TEST_CASE("the c5 row skips the BLE start when the stack is already up",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_start());
    unsigned const starts = fake_ble_starts();

    ask_row();
    finish_info();
    finish_scan(3U, -52);

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT(starts, fake_ble_starts());
    TEST_ASSERT_TRUE(logged("test c5: ble addr=38:44:be:1c:1d:06 state=idle"));
    finish_ble_scan(2U, -60);
    TEST_ASSERT_TRUE(take_verdict());
}

TEST_CASE("the c5 row fails when the C5's BT controller will not start",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_fail_start(true);

    ask_row();
    finish_info();
    finish_scan(3U, -52);
    finish_ble_start();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the C5's BT controller would not start"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a failed row must not listen");
}

TEST_CASE("the c5 row fails when NimBLE has no own address", "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_set_sync(false);

    ask_row();
    finish_info();
    finish_scan(3U, -52);
    finish_ble_start();

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: ble addr=- state=idle"));
    TEST_ASSERT_TRUE(logged("test c5: FAIL - NimBLE did not synchronise with the "
                            "C5's controller: no own address"));
}

TEST_CASE("the c5 row fails when no BLE advertiser is in range", "[radio_ao]")
{
    radio_fresh();
    go_up();

    ask_row();
    finish_info();
    finish_scan(3U, -52);
    finish_ble_start();
    finish_ble_scan(0U, 0);

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: ble scan n=0 best=- rssi=- name=-"));
    TEST_ASSERT_TRUE(logged("test c5: FAIL - no BLE advertiser in range"));
}

TEST_CASE("the c5 row fails when the BLE listen itself fails", "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_fail_scan(true);

    ask_row();
    finish_info();
    finish_scan(3U, -52);
    finish_ble_start();
    finish_ble_scan(2U, -60);

    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - the BLE scan failed"));
}

/* ---- The channel ------------------------------------------------------------------- */

TEST_CASE("the c5 row asked while a scan holds the channel waits and runs when it is free",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    ask_row();
    TEST_ASSERT_TRUE(logged("test c5: waiting for the channel"));
    TEST_ASSERT_FALSE(job_queued());

    seed_air(2U, -60);
    run_scan();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);

    /* The typed scan's listing went out, and the row got the channel next. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    finish_info();
    finish_rest();
    TEST_ASSERT_TRUE(take_verdict());
}

TEST_CASE("the c5 row asked during an autojoin round runs after the round",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    vault_has("line-ap-0");

    /* WiFi on with the link up is "get on a network": a round starts. */
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    ask_row();
    TEST_ASSERT_TRUE(logged("test c5: waiting for the channel"));

    seed_air(2U, -60);
    run_scan();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    vault_joins("line-ap-0");
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_OK);

    /* The round is over; the row is the next thing on the channel. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    finish_info();
    finish_rest();
    TEST_ASSERT_TRUE(take_verdict());

    /* Leave the flag as this file found it. */
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

/* A central that leaves while the round holds the channel finds this AO busy,
 * so its BLE_LINK_SIG cannot re-arm the advert and leaves it to the next job
 * to end. The round's jobs end in Radio_autojoin, so its end is that job. */
TEST_CASE("a central that leaves during the autojoin round is advertised to "
          "again when the round ends", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    go_up();
    vault_has("line-ap-0");

    /* Bluetooth on: the stack starts, and the board goes on the air. */
    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));

    /* A central arrives, and a round starts while it is there. */
    fake_ble_central(true);
    pump();
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    /* It leaves mid-round. */
    fake_ble_central(false);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    seed_air(2U, -60);
    run_scan();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    vault_joins("line-ap-0");
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_OK);

    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_BLE_ADV, running_job(),
        "the round ended and the board stayed off the air");
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_ADVERTISING, (int)kdrv_ble_state());

    /* Leave the flags as this file found them. */
    dispatch(WIFI_STOP_REQ_SIG);
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

TEST_CASE("the c5 row asked during a C5 update is refused with a verdict",
          "[radio_ao]")
{
    FlashReqEvt       f;
    StorageOpenRspEvt r;

    radio_fresh();
    go_up();

    (void)memset(&f, 0, sizeof(f));
    f.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    f.src_kind = (uint8_t)KFW_SRC_FS;
    f.dst_kind = (uint8_t)KFW_TARGET_C5;
    (void)snprintf(f.src, sizeof(f.src), "%s", "c5_slave.bin");
    dispatch_evt(&f.super);
    TEST_ASSERT_EQUAL_INT(-2, next_storage_op());   /* the open */

    ask_row();
    TEST_ASSERT_FALSE(take_verdict());
    TEST_ASSERT_TRUE(logged("test c5: FAIL - a C5 firmware update is in progress"));

    /* Out of the update the short way: the file was not there. */
    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(STORAGE_OPEN_RSP_SIG);
    r.status = (uint8_t)KODE_ERROR;
    dispatch_evt(&r.super);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("a second c5 row while one runs is ignored with a line", "[radio_ao]")
{
    radio_fresh();
    go_up();
    ask_row();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());

    ask_row();
    TEST_ASSERT_TRUE(logged("test c5: already running"));
    TEST_ASSERT_FALSE(job_queued());

    finish_info();
    finish_rest();
    TEST_ASSERT_TRUE(take_verdict());
    TEST_ASSERT_FALSE_MESSAGE(verdict_waiting(), "two verdicts for one row");
}

/* ---- The field verb ---------------------------------------------------------------- */

/** @brief Ask for one field, from the spy. */
static void ask_field(QActive *who, uint8_t op, uint16_t field, int32_t value)
{
    RadioFieldReqEvt q;

    (void)memset(&q, 0, sizeof(q));
    q.super = (QEvt)QEVT_INITIALIZER(RADIO_FIELD_REQ_SIG);
    q.who   = who;
    q.op    = op;
    q.field = field;
    q.value = value;
    dispatch_evt(&q.super);
}

/** @brief The next field answer in the spy. Fails the case if none. */
static const RadioFieldEvt *take_field(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the field request went unanswered");
        if ( (QSignal)RADIO_FIELD_RSP_SIG == e->sig )
        {
            return (const RadioFieldEvt *)e;
        }
    }
}

TEST_CASE("a c5 field is read and dumped for whoever asks, link up or down",
          "[radio_ao]")
{
    const RadioFieldEvt *a;

    radio_fresh();

    ask_field(ao_spy(), FIELD_OP_GET, (uint16_t)KDRV_C5_VALID, 0);
    a = take_field();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(0, a->value);

    go_up();
    ask_row();
    finish_info();
    finish_rest();
    (void)take_verdict();

    ask_field(ao_spy(), FIELD_OP_GET, (uint16_t)KDRV_C5_CHIP_ID, 0);
    a = take_field();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_INT32(KDRV_C5_CHIP_ESP32C5, a->value);

    fake_klog_reset();
    ask_field(ao_spy(), FIELD_OP_DUMP, 0U, 0);
    a = take_field();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->rc);
    TEST_ASSERT_TRUE(logged("chip_id=23"));
}

TEST_CASE("a c5 field not in the snapshot yet is refused, and still answered",
          "[radio_ao]")
{
    radio_fresh();

    ask_field(ao_spy(), FIELD_OP_GET, (uint16_t)KDRV_C5_CHIP_ID, 0);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, take_field()->rc);
}

TEST_CASE("a write to a c5 field is refused: the table has no knobs", "[radio_ao]")
{
    radio_fresh();

    ask_field(ao_spy(), FIELD_OP_SET, (uint16_t)KDRV_C5_SD_CD, 1);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, take_field()->rc);
}

TEST_CASE("a c5 field request with nobody to answer is served and dropped",
          "[radio_ao]")
{
    radio_fresh();

    ask_field(NULL, FIELD_OP_GET, (uint16_t)KDRV_C5_VALID, 0);

    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* ---- Pairing: the code that goes on the glass ------------------------------- */

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/** @brief The next published pairing record in the spy, or NULL. */
static const BlePairEvt *take_pair(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            return NULL;
        }
        if ( (QSignal)BLE_PAIR_SIG == e->sig )
        {
            return (const BlePairEvt *)e;
        }
    }
}

TEST_CASE("a code offered by the driver reaches the bus with its digits",
          "[radio_ao]")
{
    radio_fresh();
    go_up();

    fake_ble_pair_show(123456U);
    pump();

    const BlePairEvt * const p = take_pair();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_SHOW, p->phase);
    TEST_ASSERT_EQUAL_UINT32(123456U, p->passkey);
}

TEST_CASE("a code of six zeros is published as a code, not as nothing",
          "[radio_ao]")
{
    radio_fresh();
    go_up();

    /* 000000 is a legal passkey and the one a range check written as a
     * truthiness test would silently swallow. */
    fake_ble_pair_show(0U);
    pump();

    const BlePairEvt * const p = take_pair();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_SHOW, p->phase);
    TEST_ASSERT_EQUAL_UINT32(0U, p->passkey);
}

TEST_CASE("a pairing that bonds ends on the bus as done and OK", "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_pair_show(654321U);
    pump();
    ao_spy_drain();

    fake_ble_pair_end(KDRV_BLE_PAIR_OK);
    pump();

    const BlePairEvt * const p = take_pair();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_DONE, p->phase);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_OK, p->outcome);
}

TEST_CASE("a peer that walks away mid-pairing is reported as gone, not as a "
          "failure", "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_pair_show(111111U);
    pump();
    ao_spy_drain();

    fake_ble_pair_end(KDRV_BLE_PAIR_GONE);
    pump();

    const BlePairEvt * const p = take_pair();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_DONE, p->phase);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_GONE, p->outcome);
}

TEST_CASE("a pairing that fails its encryption is reported as failed",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_pair_show(222222U);
    pump();
    ao_spy_drain();

    fake_ble_pair_end(KDRV_BLE_PAIR_FAILED);
    pump();

    const BlePairEvt * const p = take_pair();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_FAILED, p->outcome);
}

TEST_CASE("a rejection reaches the driver and comes back as rejected",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_pair_show(333333U);
    pump();
    ao_spy_drain();

    dispatch(BLE_PAIR_REJECT_REQ_SIG);
    pump();

    TEST_ASSERT_EQUAL_UINT(1U, fake_ble_pair_rejects());
    const BlePairEvt * const p = take_pair();
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_DONE, p->phase);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_PAIR_REJECTED, p->outcome);
}

TEST_CASE("a rejection with no pairing in flight is refused and publishes "
          "nothing", "[radio_ao]")
{
    radio_fresh();
    go_up();

    dispatch(BLE_PAIR_REJECT_REQ_SIG);
    pump();

    TEST_ASSERT_EQUAL_UINT(0U, fake_ble_pair_rejects());
    TEST_ASSERT_NULL(take_pair());
}

TEST_CASE("a second take of one record publishes nothing", "[radio_ao]")
{
    radio_fresh();
    go_up();
    fake_ble_pair_show(444444U);
    pump();
    ao_spy_drain();

    /* One nudge per phase and one take per nudge, so a take that arrives
     * with the record already collected has to answer with silence rather
     * than with a stale code. */
    dispatch(BLE_PAIR_TAKE_SIG);
    pump();

    TEST_ASSERT_NULL(take_pair());
}

TEST_CASE("a pairing record left over from before the link went down is "
          "collected and thrown away", "[radio_ao]")
{
    radio_fresh();

    /* The link is off here, which is where an expired record would otherwise
     * sit waiting to be handed to somebody a boot later. */
    fake_ble_pair_show(555555U);
    pump();
    ao_spy_drain();

    dispatch(BLE_PAIR_TAKE_SIG);
    pump();

    TEST_ASSERT_NULL(take_pair());
}
#endif /* CONFIG_KODEOS_BLE_PAIRING_PASSKEY */

/* ---- install src="terminal" dst=c5: the image over the console wire --------
 * The terminal install's conversation with AO_Radio at the far end: the bytes
 * are CRC'd and decoded into the same PSRAM staging buffer `install dst=c5`
 * fills from a medium, verified whole, and only then pushed over the RPC. The
 * link is brought up for the image if it is down, and the `>ok` is the C5
 * activating it. There is no read-back: esp-hosted builds its transport once
 * per boot, so the update ends with the link off, as `radio_down` does.
 */

/** @brief A synthetic image: header, chip id, and IDF's descriptor at 0x20. */
static uint8_t s_img[0x140];

/** @brief Lay an image for @p chip that says it is @p project @p version. */
static void make_image(uint16_t chip, const char *project, const char *version)
{
    esp_app_desc_t d;

    for ( unsigned i = 0U; sizeof(s_img) > i; ++i )
    {
        s_img[i] = (uint8_t)((i * 3U) + 7U);   /* anything but zeros */
    }
    s_img[0]  = 0xE9U;                       /* ESP_IMAGE_HEADER_MAGIC */
    s_img[12] = (uint8_t)(chip & 0xFFU);
    s_img[13] = (uint8_t)(chip >> 8);
    (void)memset(&d, 0, sizeof(d));
    d.magic_word = ESP_APP_DESC_MAGIC_WORD;
    (void)snprintf(d.project_name, sizeof(d.project_name), "%s", project);
    (void)snprintf(d.version, sizeof(d.version), "%s", version);
    (void)memcpy(&s_img[0x20], &d, sizeof(d));
}

/** @brief The CRC the host would promise for the whole of s_img. */
static uint32_t image_crc(size_t len)
{
    return khal_crc32(KHAL_CRC32_INIT, s_img, len);
}

/** @brief Watch the receive protocol's five answers, and clear the records. */
static void recv_spy(void)
{
    ao_spy_subscribe(RECV_READY_SIG);
    ao_spy_subscribe(RECV_ACK_SIG);
    ao_spy_subscribe(RECV_DONE_SIG);
    ao_spy_subscribe(RECV_FAIL_SIG);
    ao_spy_subscribe(RECV_STALL_SIG);
    fake_neighbours_reset();   /* AO_Log's queue is where the arming lands */
    ao_spy_drain();
}

/** @brief Ask AO_Radio to receive an image, exactly as a terminal install
 *         to `dst=c5` does. */
static void inst_request(uint32_t size, uint32_t crc)
{
    AppRecvReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(RADIO_RECV_REQ_SIG);
    e.dst   = (uint8_t)KFW_TARGET_C5;
    e.size  = size;
    e.crc32 = crc;
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    dispatch_evt(&e.super);
    pump();
}

/** @brief Answer the console-arm request the way AO_Log would. */
static void inst_armed(bool ok)
{
    ConsoleRawRspEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(CONSOLE_RAW_RSP_SIG);
    e.status = (uint8_t)(ok ? KODE_OK : KODE_ERROR);
    dispatch_evt(&e.super);
    pump();
}

/** @brief Feed one chunk, exactly as the console reader task posts it. */
static void inst_feed(const uint8_t *data, uint16_t len)
{
    static RecvDataEvt e;

    e.super = (QEvt)QEVT_INITIALIZER(RECV_DATA_SIG);
    e.len   = len;
    (void)memcpy(e.data, data, len);
    dispatch_evt(&e.super);
    pump();
}

/** @brief Count the receive protocol's answers the spy has collected. */
static void inst_answers(unsigned *ready, unsigned *ack, unsigned *done,
                         unsigned *fail, unsigned *stall)
{
    *ready = 0U; *ack = 0U; *done = 0U; *fail = 0U; *stall = 0U;
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            break;
        }
        switch ( (int)e->sig )
        {
            case RECV_READY_SIG: ++*ready; break;
            case RECV_ACK_SIG:   ++*ack;   break;
            case RECV_DONE_SIG:  ++*done;  break;
            case RECV_FAIL_SIG:  ++*fail;  break;
            case RECV_STALL_SIG: ++*stall; break;
            default: break;
        }
    }
}

/** @brief Whether AO_Log was posted @p sig, draining its queue. */
static bool log_got(enum_t sig)
{
    const QEvt *e;
    bool        found = false;

    while ( xQueueReceive(AO_Log->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (enum_t)e->sig == sig )
        {
            found = true;
        }
        QF_gc(e);
    }
    return found;
}

/** @brief The whole conversation up to the verified image, on a link that is up. */
static void inst_deliver(void)
{
    make_image(0x0017U, "kode_c5", "9.9.9");
    inst_request((uint32_t)sizeof(s_img), image_crc(sizeof(s_img)));
    TEST_ASSERT_TRUE_MESSAGE(log_got(CONSOLE_RAW_REQ_SIG),
        "the console must be armed for the payload");
    inst_armed(true);
    inst_feed(s_img, (uint16_t)sizeof(s_img));
}

/** @brief Walk the push from BEGIN to the end of the update. */
static void inst_push_to_end(void)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_BEGIN, running_job());
    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_WRITE, running_job());
    TEST_ASSERT_EQUAL_UINT32(sizeof(s_img), held_req()->len);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(s_img, held_req()->data, 32U,
        "the bytes pushed are the bytes received");
    finish(KDRV_RADIO_JOB_C5_WRITE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_FINISH, running_job());
    finish(KDRV_RADIO_JOB_C5_FINISH, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "no second bring-up: the transport cannot be built twice in one boot");
    /* The link is not read here: link_now() drains the spy queue, and the
     * install's verdict is in it. Callers count the answers first. */
}

/** @brief Walk a started push to its `>ok`: the C5 activating the image. */
static void inst_complete(void)
{
    unsigned ready, ack, done, fail, stall;

    inst_push_to_end();
    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(1U, done);
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_OFF, link_now(),
        "an update ends with the link off, as radio_down does - not failed");
}

TEST_CASE("install dst=c5 on a link that is up arms the console, says ready "
          "once, and answers once the C5 has activated the verified image",
          "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();

    inst_deliver();
    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ready, "exactly one `>|ready`");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ack, "a single partial chunk earns no ack");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, done, "no result before the activate");
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)STORAGE_OP_UNMOUNT, next_storage_op(),
        "the card is let go of before the C5 is asked to reboot");

    inst_push_to_end();

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, done, "the activate is the `>ok`");
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    TEST_ASSERT_TRUE(logged("is written and activated"));
    TEST_ASSERT_TRUE_MESSAGE(logged("`radio_up` brings the link back"),
        "the log says how to see what the C5 now runs");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)STORAGE_OP_MOUNT, next_storage_op(),
        "the card goes back on the plain path, as after radio_down");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_OFF, link_now(),
        "an update ends with the link off, as radio_down does - not failed");
}

/* AO_AppMgr's done names the app it installed. A C5 image installs none, so
 * AO_Radio's done still arrives, and names nothing. The case asks the spy for
 * the done alone, and takes it back before its own assertions: a case that
 * stopped half-way would leave every later one a subscription it never asked
 * for. */
TEST_CASE("a C5 update's done arrives naming no app", "[radio_ao]")
{
    unsigned dones = 0U;
    bool     named = false;

    radio_fresh();
    go_up();
    ao_spy_subscribe(APP_FLASH_DONE_SIG);
    ao_spy_drain();

    inst_deliver();
    inst_push_to_end();
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            break;
        }
        if ( (int)e->sig == APP_FLASH_DONE_SIG )
        {
            AppFlashDoneEvt const * const d = (AppFlashDoneEvt const *)e;
            ++dones;
            if ( '\0' != d->name[0] )
            {
                named = true;
            }
        }
    }
    QActive_unsubscribe(ao_spy(), APP_FLASH_DONE_SIG);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, dones,
        "activated: the card closes and the LED lets go");
    TEST_ASSERT_FALSE_MESSAGE(named,
        "a C5 image installs no app, so its done names none");
}

TEST_CASE("a C5 that refuses to activate the image fails the install, and "
          "the link stays up", "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();

    inst_deliver();
    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_OK);
    finish(KDRV_RADIO_JOB_C5_WRITE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_FINISH, running_job());
    finish(KDRV_RADIO_JOB_C5_FINISH, KODE_ERROR);

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(0U, done);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail,
        "the activate is the verdict, so a refused one is the failure");
    TEST_ASSERT_TRUE(logged("refused to activate"));
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_UP, link_now(),
        "nothing rebooted, so the link it had is still there");
}

TEST_CASE("install dst=c5 on a link that is off brings the link up once the "
          "image is verified, and the push goes before any reconcile",
          "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    recv_spy();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    ao_spy_drain();

    inst_deliver();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_CONNECT, running_job(),
        "the link comes up for the image, not before it");
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_C5_BEGIN, running_job(),
        "the push is the first job on the fresh link");
    TEST_ASSERT_FALSE_MESSAGE(job_queued(),
        "no service reconcile is queued behind it");

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(1U, ready);
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    inst_complete();
}

TEST_CASE("an image built for another chip is refused before the C5 is touched",
          "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;
    unsigned submits;

    radio_fresh();
    go_up();
    recv_spy();
    submits = job_submits();   /* the connect above is not the point */

    make_image(0x0012U, "kodeos", "00.00.18");   /* ESP_CHIP_ID_ESP32P4 */
    inst_request((uint32_t)sizeof(s_img), image_crc(sizeof(s_img)));
    inst_armed(true);
    inst_feed(s_img, (uint16_t)sizeof(s_img));

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(1U, ready);
    TEST_ASSERT_EQUAL_UINT(0U, done);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail, "refused, and said so");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(submits, job_submits(),
        "no job reached the worker");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_TRUE(logged("the C5 was not touched"));
}

TEST_CASE("a wire that betrays its CRC fails the install and touches no job",
          "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;
    unsigned submits;

    radio_fresh();
    go_up();
    recv_spy();
    submits = job_submits();

    make_image(0x0017U, "kode_c5", "9.9.9");
    inst_request((uint32_t)sizeof(s_img), image_crc(sizeof(s_img)));
    inst_armed(true);
    s_img[40] ^= 0xFFU;   /* the promise was made before this byte turned */
    inst_feed(s_img, (uint16_t)sizeof(s_img));

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(0U, done);
    TEST_ASSERT_EQUAL_UINT(1U, fail);
    TEST_ASSERT_EQUAL_UINT(submits, job_submits());
    TEST_ASSERT_TRUE(logged("CRC mismatch"));
}

TEST_CASE("install dst=c5 while a job holds the channel waits, and pushes the "
          "moment it is free", "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    inst_deliver();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job(),
        "the scan keeps the channel; the push waits");
    TEST_ASSERT_FALSE(job_queued());
    TEST_ASSERT_TRUE(logged("waiting for the channel"));

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_C5_BEGIN, running_job(),
        "the push is owed, and paid the moment the channel is free");

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(1U, ready);
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    inst_complete();
}

TEST_CASE("a host that goes quiet mid-install is a stall, and raw mode comes "
          "down with it", "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();

    make_image(0x0017U, "kode_c5", "9.9.9");
    inst_request((uint32_t)sizeof(s_img), image_crc(sizeof(s_img)));
    (void)log_got(CONSOLE_RAW_REQ_SIG);
    inst_armed(true);
    inst_feed(s_img, 64U);   /* and then nothing */

    for ( unsigned t = 0U; ((KODE_RECV_IDLE_MS / 10U) + 5U) > t; ++t )
    {
        pump();
    }

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(1U, ready);
    TEST_ASSERT_EQUAL_UINT(0U, done);
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, stall, "the host is presumed gone");
    TEST_ASSERT_TRUE_MESSAGE(log_got(CONSOLE_RAW_END_SIG),
        "the line editor must come back");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("install dst=c5 off a medium while one is receiving is refused",
          "[radio_ao]")
{
    FlashReqEvt f;

    radio_fresh();
    go_up();
    recv_spy();

    make_image(0x0017U, "kode_c5", "9.9.9");
    inst_request((uint32_t)sizeof(s_img), image_crc(sizeof(s_img)));
    inst_armed(true);

    (void)memset(&f, 0, sizeof(f));
    f.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    f.src_kind = (uint8_t)KFW_SRC_FS;
    f.dst_kind = (uint8_t)KFW_TARGET_C5;
    (void)snprintf(f.src, sizeof(f.src), "%s", "c5_slave.bin");
    dispatch_evt(&f.super);
    pump();

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, next_storage_op(),
        "nothing was opened: the staging buffer is the install's");
    TEST_ASSERT_TRUE(logged("install dst=c5 is in progress"));

    /* Let the install finish, so the next case starts clean. */
    inst_feed(s_img, (uint16_t)sizeof(s_img));
    inst_complete();
}

TEST_CASE("install dst=c5 while the link is being torn down is refused with an "
          "answer", "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();

    dispatch(RADIO_DOWN_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_STOPPING, link_now());
    ao_spy_drain();

    make_image(0x0017U, "kode_c5", "9.9.9");
    inst_request((uint32_t)sizeof(s_img), image_crc(sizeof(s_img)));

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(0U, ready);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail, "refused, and the console told");
    TEST_ASSERT_FALSE_MESSAGE(log_got(CONSOLE_RAW_REQ_SIG),
        "the wire was never armed");
}

TEST_CASE("the acks count one per whole chunk and none for the remainder",
          "[radio_ao]")
{
    static uint8_t big[KODE_RECV_CHUNK + 64U];
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();

    make_image(0x0017U, "kode_c5", "9.9.9");
    (void)memcpy(big, s_img, sizeof(s_img));
    for ( unsigned i = sizeof(s_img); sizeof(big) > i; ++i )
    {
        big[i] = (uint8_t)i;
    }
    inst_request((uint32_t)sizeof(big),
                 khal_crc32(KHAL_CRC32_INIT, big, sizeof(big)));
    inst_armed(true);
    inst_feed(big, (uint16_t)KODE_RECV_CHUNK);
    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, ack, "a whole chunk earns its ack");

    inst_feed(&big[KODE_RECV_CHUNK], 64U);
    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ack, "the remainder earns none");
    TEST_ASSERT_EQUAL_UINT(0U, fail);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_BEGIN, running_job());

    /* Walk it out by hand: the image is bigger than s_img, so the push's
     * length is this case's own to check. */
    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(sizeof(big), held_req()->len);
    finish(KDRV_RADIO_JOB_C5_WRITE, KODE_OK);
    finish(KDRV_RADIO_JOB_C5_FINISH, KODE_OK);
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(1U, done);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
}

/* ---- What the glass and the LED see of a C5 update ---------------------------
 * The Opening sheet and the LED follow APP_FLASH_PROGRESS_SIG and its two
 * endings alone, so a C5 image has to report on that family as an app does:
 * 0 % with the ready, one bar of two halves - staging into PSRAM to 50 %, the
 * push to the C5 to 100 % - each report saying it is the radio, and then one
 * done or one failure. The receive protocol's own answers stay the console's.
 */

/** @brief What the write's family carried, counted off the spy. */
typedef struct {
    unsigned reports;
    unsigned pct[8];
    unsigned done;
    unsigned fail;
    bool     all_c5;
    bool     no_name;
} write_seen_t;

/** @brief Watch the write's family, and clear the records. */
static void write_spy(void)
{
    ao_spy_subscribe(APP_FLASH_PROGRESS_SIG);
    ao_spy_subscribe(APP_FLASH_DONE_SIG);
    ao_spy_subscribe(APP_FLASH_FAIL_SIG);
    ao_spy_drain();
}

/** @brief Stop watching it, so later cases are not posted what they never asked for. */
static void write_unspy(void)
{
    QActive_unsubscribe(ao_spy(), APP_FLASH_PROGRESS_SIG);
    QActive_unsubscribe(ao_spy(), APP_FLASH_DONE_SIG);
    QActive_unsubscribe(ao_spy(), APP_FLASH_FAIL_SIG);
    ao_spy_drain();
}

/** @brief Count what the write's family carried, draining the spy. */
static write_seen_t write_seen(void)
{
    write_seen_t w;

    (void)memset(&w, 0, sizeof(w));
    w.all_c5  = true;
    w.no_name = true;
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            break;
        }
        if ( (int)e->sig == APP_FLASH_PROGRESS_SIG )
        {
            AppProgressEvt const * const p = (AppProgressEvt const *)e;
            if ( Q_DIM(w.pct) > w.reports )
            {
                w.pct[w.reports] = p->percent;
            }
            ++w.reports;
            if ( (uint8_t)KWRITE_C5 != p->what )
            {
                w.all_c5 = false;
            }
            if ( '\0' != p->file[0] )
            {
                w.no_name = false;
            }
        }
        else if ( (int)e->sig == APP_FLASH_DONE_SIG )
        {
            ++w.done;
        }
        else if ( (int)e->sig == APP_FLASH_FAIL_SIG )
        {
            ++w.fail;
            TEST_ASSERT_EQUAL_UINT8((uint8_t)KFAIL_WRITE,
                                    ((AppFlashFailEvt const *)e)->why);
        }
        else
        {
            /* the receive protocol's own answers: not this family */
        }
    }
    return w;
}

TEST_CASE("install dst=c5 reports 0 % with its ready, half the bar staged, the "
          "rest pushed, then one done", "[radio_ao]")
{
    write_seen_t w;

    radio_fresh();
    go_up();
    write_spy();

    inst_deliver();          /* the whole image in one chunk: staged */
    w = write_seen();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, w.reports, "0 % and the staged half");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, w.pct[0], "0 % goes out with the ready");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(50U, w.pct[1],
        "staging into PSRAM is the first half of the bar");
    TEST_ASSERT_TRUE_MESSAGE(w.all_c5, "every report says it is the radio");
    TEST_ASSERT_TRUE_MESSAGE(w.no_name, "a stream has no file name");
    TEST_ASSERT_EQUAL_UINT(0U, w.done);

    inst_push_to_end();      /* one chunk pushed, activated, link down */
    w = write_seen();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, w.reports, "the push is the other half");
    TEST_ASSERT_EQUAL_UINT(100U, w.pct[0]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, w.done,
        "activated: the card closes and the LED lets go");
    TEST_ASSERT_EQUAL_UINT(0U, w.fail);

    write_unspy();
}

TEST_CASE("a C5 that refuses to activate ends the card as a failure, once",
          "[radio_ao]")
{
    write_seen_t w;

    radio_fresh();
    go_up();
    write_spy();

    inst_deliver();
    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_OK);
    finish(KDRV_RADIO_JOB_C5_WRITE, KODE_OK);
    finish(KDRV_RADIO_JOB_C5_FINISH, KODE_ERROR);

    w = write_seen();
    TEST_ASSERT_EQUAL_UINT(0U, w.done);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, w.fail,
        "one failure, though ota_fail and the install's own end both pass "
        "through it");

    write_unspy();
}

TEST_CASE("an install dst=c5 refused before its ready leaves the glass alone",
          "[radio_ao]")
{
    write_seen_t w;

    radio_fresh();
    go_up();
    write_spy();

    inst_request(0U, 0U);    /* not an image: refused at the request */
    w = write_seen();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, w.reports, "nothing was announced");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, w.fail,
        "a refusal the glass never saw start is the console's to tell");

    write_unspy();
}

/* ---- The C5's own power ------------------------------------------------------
 * Built with KODEOS_C5_HOLD, AO_Radio holds the C5 in reset from the start
 * of a boot that wants neither WiFi nor BLE, and lets it go for the link or
 * for whoever publishes C5_RUN_REQ_SIG - AO_Audio, whose amplifier the C5's
 * firmware enables.
 */

/**
 * @brief Boot AO_Radio again: a power cycle, then its initial transition.
 *
 * The one thing the rest of this file never does, because the AO is brought
 * up once per binary. It is safe here for test_imu_ao.c's reason: the AO is
 * walked to `off` first and ticked, so no one-shot is armed when the
 * constructor runs over it.
 */
static void radio_reboot(void)
{
    tick(QF_SUPPORT_STEPS(KODE_C5_BOOT_MS) + 8U);
    fake_radio_power_cycle();
    Radio_ctor();
    qf_support_give_queue(AO_Radio);
    AO_Radio->prio  = KODE_PRIO_RADIO;
    AO_Radio->pthre = KODE_PRIO_RADIO;
    fake_klog_reset();
    QASM_INIT(AO_Radio, (void *)0, 0U);
    (void)pull_jobs();
    storage_drain();
}

/**
 * @brief Leave nothing of a case behind: the link off, both mailboxes empty.
 *
 * These cases are the last in the file, and a pool block still queued here
 * is one no later file can allocate.
 */
static void c5_case_end(void)
{
    settle_to_off();
    storage_drain();
    net_drain();
    (void)boot_parked_taken();
    ao_spy_drain();
}

/** @brief How many C5_RUNNING_SIG the spy holds; drains it. */
static unsigned running_said(void)
{
    unsigned n = 0U;

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();

        if ( NULL == e )
        {
            break;
        }
        if ( C5_RUNNING_SIG == e->sig )
        {
            n += 1U;
        }
    }
    return n;
}

TEST_CASE("a boot that wants no radio holds the C5 in reset; one that wants "
          "it does not", "[radio_ao]")
{
    uint8_t const on  = 1U;
    uint8_t const off = 0U;

    radio_fresh();
    radio_reboot();
    TEST_ASSERT_TRUE_MESSAGE(kdrv_radio_c5_held(),
                             "the C5 runs on a board that asked for nothing");
    TEST_ASSERT_TRUE(logged("held in reset"));
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());

    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_WIFI, &on, sizeof(on));
    radio_reboot();
    TEST_ASSERT_FALSE_MESSAGE(kdrv_radio_c5_held(),
                              "the C5 is held under a link that is wanted");
    TEST_ASSERT_FALSE(logged("held in reset"));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    /* And a last boot that wants nothing again, because this AO outlives the
     * case: one left wanting WiFi is what every later file would inherit. */
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_WIFI, &off, sizeof(off));
    radio_reboot();
    c5_case_end();
}

TEST_CASE("a held C5 is let go for whoever needs it, and the answer waits "
          "for its boot", "[radio_ao]")
{
    radio_fresh();
    radio_reboot();
    ao_spy_subscribe(C5_RUNNING_SIG);
    ao_spy_drain();
    TEST_ASSERT_TRUE(kdrv_radio_c5_held());

    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_FALSE_MESSAGE(kdrv_radio_c5_held(), "the C5 was not let go");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, running_said(),
                                   "said to be running before it had booted");

    /* A second request inside the wait starts nothing of its own. */
    dispatch(C5_RUN_REQ_SIG);
    tick(QF_SUPPORT_STEPS(KODE_C5_BOOT_MS) - 1U);
    TEST_ASSERT_EQUAL_UINT(0U, running_said());

    tick(2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, running_said(),
                                   "one answer, for both of them");

    /* And from then on it is simply true. */
    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, running_said(),
                                   "a C5 that is running is answered for in "
                                   "the step");
    c5_case_end();
}

TEST_CASE("the link's own bring-up is what lets a held C5 go", "[radio_ao]")
{
    radio_fresh();
    radio_reboot();
    ao_spy_subscribe(C5_RUNNING_SIG);
    TEST_ASSERT_TRUE(kdrv_radio_c5_held());

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_FALSE_MESSAGE(kdrv_radio_c5_held(),
                              "the C5 is held under a link being built");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kdrv_radio_c5_hold(true),
                                  "the reset line is esp-hosted's from here");
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    ao_spy_drain();

    /* Up, the C5 runs: nothing to wait for. */
    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT(1U, running_said());
    c5_case_end();
}

/* ---- The link comes down when nothing uses it --------------------------------
 * A service switched off is not a link torn down by itself: the link ends
 * when the LAST thing using it has gone - no service wanted, nobody who asked
 * for the link itself, no sound that asked for the C5 - and the C5 is held
 * in reset behind it. Switching a service back on builds it again.
 */

/** @brief End whatever runs, happily, until @p want is the job running.
 *  @return true when it got there. */
static bool drive_until(kdrv_radio_job_t want)
{
    for ( unsigned guard = 0U; 16U > guard; ++guard )
    {
        pump();
        if ( running_job() == (int)want )
        {
            return true;
        }
        switch ( (kdrv_radio_job_t)running_job() )
        {
            case KDRV_RADIO_JOB_CONNECT:
                finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
                break;
            case KDRV_RADIO_JOB_BLE_START:
                finish(KDRV_RADIO_JOB_BLE_START, KODE_OK);
                break;
            default:
                if ( !job_held() )
                {
                    return false;
                }
                finish_whatever_runs();
                break;
        }
    }
    return false;
}

TEST_CASE("the last service going off takes the link down and holds the C5",
          "[radio_ao]")
{
    radio_fresh();
    radio_reboot();
    TEST_ASSERT_TRUE(kdrv_radio_c5_held());

    dispatch(BLE_START_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());

    dispatch(BLE_STOP_REQ_SIG);
    TEST_ASSERT_TRUE_MESSAGE(drive_until(KDRV_RADIO_JOB_DISCONNECT),
                             "the link stayed up with nothing using it");
    TEST_ASSERT_TRUE(logged("nothing uses the C5 link any more"));
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    TEST_ASSERT_TRUE_MESSAGE(kdrv_radio_c5_held(),
                             "the C5 runs with no link and nothing to do");

    /* And it is not one-way: the service switched back on builds the link
     * again, in the same boot. */
    dispatch(BLE_START_REQ_SIG);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_CONNECT, running_job(),
                                  "Bluetooth could not be switched back on");
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    dispatch(BLE_STOP_REQ_SIG);
    c5_case_end();
}

TEST_CASE("a link somebody asked for by itself outlives the services",
          "[radio_ao]")
{
    radio_fresh();
    radio_reboot();
    go_up();                         /* `radio_up`: the link, for itself */

    dispatch(BLE_START_REQ_SIG);
    (void)drive_until(KDRV_RADIO_JOB_DISCONNECT);   /* runs the start out */
    dispatch(BLE_STOP_REQ_SIG);
    TEST_ASSERT_FALSE_MESSAGE(drive_until(KDRV_RADIO_JOB_DISCONNECT),
                              "a service going off took down a link that "
                              "was asked for by name");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());

    /* `radio_down` gives it back, and then the C5 is held. */
    dispatch(RADIO_DOWN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_TRUE(kdrv_radio_c5_held());
    c5_case_end();
}

TEST_CASE("a boot in which a sound asked for the C5 keeps its link and its C5",
          "[radio_ao]")
{
    radio_fresh();
    radio_reboot();
    dispatch(C5_RUN_REQ_SIG);        /* AO_Audio, before its first sound */
    tick(QF_SUPPORT_STEPS(KODE_C5_BOOT_MS) + 2U);

    dispatch(BLE_START_REQ_SIG);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    dispatch(BLE_STOP_REQ_SIG);
    TEST_ASSERT_FALSE_MESSAGE(drive_until(KDRV_RADIO_JOB_DISCONNECT),
                              "the C5 was taken from under an amplifier "
                              "AO_Audio believes is there");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());

    /* The next boot forgets it: this AO outlives the case. */
    dispatch(RADIO_DOWN_REQ_SIG);
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    radio_reboot();
    c5_case_end();
}

/* ---- The KODE services: INFO on every link ---------------------------------- */

/** @brief Whether any captured log line contains @p needle. */
static bool log_has(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char *const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("a link that comes up asks the C5 for its KODE INFO", "[radio][kode]")
{
    radio_fresh();
    fake_peer_reset();
    go_up();
    TEST_ASSERT_EQUAL_HEX32(KODE_MSG_REQ, fake_peer_last_msg_id());
    /* INFO first, then the vault's LIST behind it. */
    const kode_hdr_t *const h = (const kode_hdr_t *)fake_peer_sent_back(1U);
    TEST_ASSERT_NOT_NULL(h);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_SELF, h->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_SELF_INFO, h->op);
}

TEST_CASE("an INFO answer is logged and kept for info", "[radio][kode]")
{
    radio_fresh();
    fake_peer_reset();
    go_up();
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_sent_back(1U);
    TEST_ASSERT_EQUAL_UINT8(KODE_SELF_INFO, req->op);
    uint8_t m[sizeof(kode_hdr_t) + sizeof(kode_info_t)];
    kode_hdr_t const h = { KODE_WIRE_VERSION, KODE_SVC_SELF, KODE_SELF_INFO,
                           req->seq, KODE_ST_OK, 0U, (uint16_t)sizeof(kode_info_t) };
    kode_info_t info;
    (void)memset(&info, 0, sizeof(info));
    info.wire_version = KODE_WIRE_VERSION;
    info.heap_low     = 44036U;
    (void)memcpy(m, &h, sizeof(h));
    (void)memcpy(&m[sizeof(h)], &info, sizeof(info));
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(m));
    pump();
    char want[32];
    (void)snprintf(want, sizeof(want), "KODE services v%u", (unsigned)KODE_WIRE_VERSION);
    TEST_ASSERT_TRUE(log_has(want));
    kode_info_t got;
    TEST_ASSERT_TRUE(kdrv_c5svc_info(&got));
    TEST_ASSERT_EQUAL_UINT32(44036U, got.heap_low);
}

TEST_CASE("a C5 that never answers INFO is said so and nothing waits on it", "[radio][kode]")
{
    radio_fresh();
    go_up();
    tick(QF_SUPPORT_STEPS(3000U) + 2U);
    pump();
    TEST_ASSERT_TRUE(log_has("no KODE services"));
    kode_info_t got;
    TEST_ASSERT_FALSE(kdrv_c5svc_info(&got));
    /* the AO is not stuck: it still answers a state request */
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("a link that comes up names the board after the C5's MAC", "[radio][kode]")
{
    static const uint8_t c5[6] = { 0x38U, 0x44U, 0xBEU, 0xA1U, 0xB2U, 0xC3U };
    char expect[KHAL_ID_LEN];

    radio_fresh();
    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_identity_init());   /* nameless */
    fake_hosted_set_mac(c5);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5_refresh());   /* the worker's snapshot */
    go_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(c5, expect, sizeof(expect)));
    TEST_ASSERT_EQUAL_STRING(expect, khal_identity_serial());
}


/* ---- The Wi-Fi vault on the C5 ------------------------------------------------ */

/** @brief The last KODE request the AO sent. */
static const kode_hdr_t *kode_sent(void)
{
    return (const kode_hdr_t *)fake_peer_last_sent(NULL);
}

TEST_CASE("a join that worked is saved in the vault, password and all, once", "[radio][vault]")
{
    radio_fresh();
    go_up();
    vault_has("other");
    wifi_join_request("home", "secret99", true);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_JOIN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_VAULT, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_REMEMBER, kode_sent()->op);
    const kode_vault_cred_t *const c = (const kode_vault_cred_t *)&kode_sent()[1];
    TEST_ASSERT_EQUAL_STRING("home", c->ssid);
    TEST_ASSERT_EQUAL_STRING("secret99", c->pass);
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_TRUE(logged("'home' saved in the C5's vault"));
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

TEST_CASE("a join asked not to be remembered leaves the vault alone", "[radio][vault]")
{
    radio_fresh();
    go_up();
    vault_has("other");
    wifi_join_request("factory-ap", "12345678", false);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_OK);
    TEST_ASSERT_FALSE((KODE_SVC_VAULT == kode_sent()->service)
                      && (KODE_VAULT_REMEMBER == kode_sent()->op));
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

TEST_CASE("a saved network asked by name is joined by the C5, not with a password", "[radio][vault]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    wifi_join_request("home", "", true);
    pump();
    vault_joins("home");
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_OK);
    TEST_ASSERT_TRUE(logged("joined 'home'"));
    /* nothing to save: the vault had it */
    TEST_ASSERT_FALSE(KODE_VAULT_REMEMBER == kode_sent()->op);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

TEST_CASE("an autojoin whose vault never answers the join moves on, and ends", "[radio][vault]")
{
    radio_fresh();
    go_up();
    vault_has("line-ap-0");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    seed_air(2U, -60);
    run_scan();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PREPARE, running_job());
    finish(KDRV_RADIO_JOB_WIFI_PREPARE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_JOIN, kode_sent()->op);
    /* the C5 says nothing for three seconds */
    tick(QF_SUPPORT_STEPS(3000U) + 2U);
    pump();
    TEST_ASSERT_TRUE(logged("KODE service 1 did not answer"));
    TEST_ASSERT_EQUAL_INT(-1, running_job());       /* the round is over */
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

/** @brief Boot with one channel hint in NVS, or none with NULL. */
static void boot_with_hint(const char *ssid, uint8_t channel)
{
    struct {
        char    ssid[33];
        uint8_t channel;
    } hints[4];

    (void)memset(hints, 0, sizeof(hints));
    if ( NULL != ssid )
    {
        (void)snprintf(hints[0].ssid, sizeof(hints[0].ssid), "%s", ssid);
        hints[0].channel = channel;
    }
    fake_nvs_seed(KSET_NAMESPACE, "wifi_hints", hints, sizeof(hints));
    radio_reboot();
}

/** @brief What the probe "heard" of its network. */
static void hint_heard(bool seen)
{
    kdrv_radio_probe_result_t res;

    (void)memset(&res, 0, sizeof(res));
    res.net[0].seen    = seen;
    res.net[0].rssi    = -50;
    res.net[0].channel = 6U;
    fake_radio_set_probe_result(&res);
}

/** @brief Leave nothing of a hint case behind. */
static void hint_case_end(void)
{
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    boot_with_hint(NULL, 0U);
    c5_case_end();
}

TEST_CASE("a round goes straight to the channel of the network last joined", "[radio][vault]")
{
    radio_fresh();
    boot_with_hint("home", 6U);
    go_up();
    vault_has("home");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());
    hint_heard(true);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PREPARE, running_job());
    TEST_ASSERT_EQUAL_STRING("home", held_req()->ssid);
    finish(KDRV_RADIO_JOB_WIFI_PREPARE, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_JOIN, kode_sent()->op);
    tick(QF_SUPPORT_STEPS(3000U) + 2U);
    pump();
    hint_case_end();
}

TEST_CASE("a network not heard on its channel falls back to the sweep", "[radio][vault]")
{
    radio_fresh();
    boot_with_hint("home", 6U);
    go_up();
    vault_has("home");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());
    hint_heard(false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    hint_case_end();
}

TEST_CASE("a hint the vault no longer holds is not looked for", "[radio][vault]")
{
    radio_fresh();
    boot_with_hint("gone", 6U);
    go_up();
    vault_has("home");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    hint_case_end();
}

TEST_CASE("wifi_forget asks the vault to forget", "[radio][vault]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    wifi_forget_request("home");
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_FORGET, kode_sent()->op);
    TEST_ASSERT_EQUAL_STRING("home", (const char *)&kode_sent()[1]);
}

TEST_CASE("wifi_list lists what the vault keeps, names only", "[radio][vault]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_subscribe(WIFI_SAVED_ENTRY_SIG);
    ao_spy_subscribe(WIFI_SAVED_END_SIG);
    ao_spy_drain();
    wifi_list_request();
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_LIST, kode_sent()->op);
    kode_vault_list_t l;
    (void)memset(&l, 0, sizeof(l));
    l.count = 2U;
    (void)snprintf(l.ssid[0], sizeof(l.ssid[0]), "%s", "home");
    (void)snprintf(l.ssid[1], sizeof(l.ssid[1]), "%s", "work");
    kode_answer(KODE_ST_OK, &l, (uint16_t)sizeof(l));
    unsigned entries = 0U;
    bool end = false;
    while ( ao_spy_count() > 0U )
    {
        const QEvt *const e = ao_spy_take();
        if ( (enum_t)WIFI_SAVED_ENTRY_SIG == e->sig )
        {
            entries++;
        }
        else if ( (enum_t)WIFI_SAVED_END_SIG == e->sig )
        {
            end = true;
            TEST_ASSERT_EQUAL_UINT8(2U, ((const WifiScanEndEvt *)e)->count);
        }
        else
        {
            /* other traffic */
        }
        ao_spy_release();
    }
    TEST_ASSERT_EQUAL_UINT(2U, entries);
    TEST_ASSERT_TRUE(end);
}

/* ---- The unit's identity, held by the C5 ------------------------------------ */

/** @brief The next event the spy holds, which must be @p sig; copied out. */
static void spy_expect(enum_t sig, void *out, size_t size)
{
    TEST_ASSERT_TRUE_MESSAGE(ao_spy_count() > 0U, "nothing came back");
    const QEvt *const e = ao_spy_take();
    TEST_ASSERT_EQUAL_INT((int)sig, (int)e->sig);
    (void)memcpy(out, e, size);
    ao_spy_release();
}

/** @brief A DER element of @p total bytes: SEQUENCE, long form, filler. */
static void der_element(uint8_t *out, uint16_t total, uint8_t fill)
{
    (void)memset(out, fill, total);
    out[0] = 0x30U;
    out[1] = 0x82U;
    out[2] = (uint8_t)((total - 4U) >> 8);
    out[3] = (uint8_t)((total - 4U) & 0xFFU);
}

TEST_CASE("id_sign_request asks the C5 to sign the nonce and hands back the DER",
          "[radio][id]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_ID, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_SIGN_AUTH, kode_sent()->op);
    const kode_id_nonce_t *const q = (const kode_id_nonce_t *)&kode_sent()[1];
    TEST_ASSERT_EQUAL_UINT8(3U, q->len);
    TEST_ASSERT_EQUAL_MEMORY("abc", q->nonce, 3U);

    uint8_t sig[70];
    (void)memset(sig, 0xA5, sizeof(sig));
    kode_answer(KODE_ST_OK, sig, (uint16_t)sizeof(sig));
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, r.status);
    TEST_ASSERT_EQUAL_UINT8(70U, r.der_len);
    TEST_ASSERT_EQUAL_MEMORY(sig, r.der, 70U);
}

TEST_CASE("a signature the C5 never answers is refused after three seconds",
          "[radio][id]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_SIGN_AUTH, kode_sent()->op);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    tick(QF_SUPPORT_STEPS(3000U) + 2U);
    pump();
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);
    TEST_ASSERT_EQUAL_UINT8(0U, r.der_len);
}

TEST_CASE("a nonce outside the alphabet is refused before the C5 is asked",
          "[radio][id]")
{
    radio_fresh();
    go_up();
    const kode_hdr_t *const before = kode_sent();
    uint8_t const before_op = before->op;
    uint8_t const before_svc = before->service;
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "a/b"));
    pump();
    TEST_ASSERT_EQUAL_UINT8(before_svc, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(before_op, kode_sent()->op);
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_BAD_REQUEST, r.status);
}

TEST_CASE("a second identity request while one is in flight is told busy",
          "[radio][id]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "one"));
    pump();
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U));
    pump();
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, end.status);
    /* the first one is still answered */
    uint8_t sig[8] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
    kode_answer(KODE_ST_OK, sig, (uint16_t)sizeof(sig));
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, r.status);
}

TEST_CASE("an identity request with the link down is refused at once",
          "[radio][id]")
{
    radio_fresh();
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    pump();
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U));
    pump();
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, end.status);
}

TEST_CASE("the certificate pair comes back in parts, then an end that splits it",
          "[radio][id]")
{
    static kode_id_cert_t c;
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_ID, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_CERT_GET, kode_sent()->op);

    c.unit_len  = 1000U;
    c.chain_len = 536U;
    der_element(c.der, 1000U, 0x11U);
    der_element(&c.der[1000], 536U, 0x22U);
    kode_answer(KODE_ST_OK, &c, (uint16_t)(4U + 1536U));

    static uint8_t got[1536];
    size_t fill = 0U;
    for ( uint8_t i = 1U; 11U >= i; i++ )
    {
        IdPartEvt p;
        spy_expect(ID_PART_SIG, &p, sizeof(p));
        TEST_ASSERT_EQUAL_UINT8(i, p.n);
        TEST_ASSERT_EQUAL_UINT8(11U, p.of);
        TEST_ASSERT_TRUE(KODE_ID_PART_BYTES >= p.len);
        (void)memcpy(&got[fill], p.der, p.len);
        fill += p.len;
    }
    TEST_ASSERT_EQUAL_UINT(1536U, fill);
    TEST_ASSERT_EQUAL_MEMORY(c.der, got, 1536U);
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, end.status);
    TEST_ASSERT_EQUAL_UINT16(1536U, end.len);
    TEST_ASSERT_EQUAL_UINT16(1000U, end.unit_len);
}

TEST_CASE("an asker with no room for the whole pair gets what fits and a refused end",
          "[radio][id]")
{
    static kode_id_cert_t c;
    static const QEvt filler = QEVT_INITIALIZER(MAX_SIG);
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U));
    pump();
    /* the asker's queue: 4 free of 32 */
    for ( unsigned i = 0U; 28U > i; i++ )
    {
        QACTIVE_POST(ao_spy(), &filler, NULL);
    }
    c.unit_len  = 1000U;
    c.chain_len = 536U;
    der_element(c.der, 1000U, 0x11U);
    der_element(&c.der[1000], 536U, 0x22U);
    kode_answer(KODE_ST_OK, &c, (uint16_t)(4U + 1536U));

    unsigned parts = 0U;
    IdEndEvt end;
    bool got_end = false;
    while ( ao_spy_count() > 0U )
    {
        const QEvt *const e = ao_spy_take();
        if ( (enum_t)ID_PART_SIG == e->sig )
        {
            parts++;
        }
        else if ( (enum_t)ID_END_SIG == e->sig )
        {
            (void)memcpy(&end, e, sizeof(end));
            got_end = true;
        }
        else
        {
            /* filler */
        }
        ao_spy_release();
    }
    TEST_ASSERT_EQUAL_UINT(3U, parts);       /* one slot kept for the end */
    TEST_ASSERT_TRUE(got_end);
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, end.status);
}

TEST_CASE("a unit with no certificate ends the read with nothing found",
          "[radio][id]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U));
    pump();
    kode_answer(KODE_ST_NOT_FOUND, NULL, 0U);
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_NOT_FOUND, end.status);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

TEST_CASE("the CSR comes back in parts", "[radio][id]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CSR, 0U, 0U, NULL, 0U));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_CSR, kode_sent()->op);
    uint8_t csr[300];
    der_element(csr, 300U, 0x33U);
    kode_answer(KODE_ST_OK, csr, (uint16_t)sizeof(csr));
    IdPartEvt p;
    spy_expect(ID_PART_SIG, &p, sizeof(p));
    TEST_ASSERT_EQUAL_UINT8(3U, p.of);
    spy_expect(ID_PART_SIG, &p, sizeof(p));
    spy_expect(ID_PART_SIG, &p, sizeof(p));
    TEST_ASSERT_EQUAL_UINT8(3U, p.n);
    TEST_ASSERT_EQUAL_UINT8(300U - (2U * KODE_ID_PART_BYTES), p.len);
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, end.status);
    TEST_ASSERT_EQUAL_UINT16(300U, end.len);
}

TEST_CASE("a certificate pair put in parts is stored whole, unit and chain apart",
          "[radio][id]")
{
    static uint8_t pair[400];
    radio_fresh();
    go_up();
    der_element(pair, 250U, 0x44U);
    der_element(&pair[250], 150U, 0x55U);
    uint8_t const of = 3U;
    for ( uint8_t n = 1U; n <= of; n++ )
    {
        size_t const at  = (size_t)(n - 1U) * KODE_ID_PART_BYTES;
        size_t       len = sizeof(pair) - at;
        if ( KODE_ID_PART_BYTES < len )
        {
            len = KODE_ID_PART_BYTES;
        }
        TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CERT_PUT, n, of,
                                    &pair[at], len));
        pump();
        if ( n < of )
        {
            IdEndEvt end;
            spy_expect(ID_END_SIG, &end, sizeof(end));
            TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, end.status);
        }
    }
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_ID, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_CERT_SET, kode_sent()->op);
    size_t sent = 0U;
    (void)fake_peer_last_sent(&sent);
    TEST_ASSERT_EQUAL_UINT(sizeof(kode_hdr_t) + 4U + 400U, sent);
    const kode_id_cert_t *const c = (const kode_id_cert_t *)&kode_sent()[1];
    TEST_ASSERT_EQUAL_UINT16(250U, c->unit_len);
    TEST_ASSERT_EQUAL_UINT16(150U, c->chain_len);
    TEST_ASSERT_EQUAL_MEMORY(pair, c->der, 400U);
    kode_answer(KODE_ST_OK, NULL, 0U);
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, end.status);
}

TEST_CASE("a certificate part out of order is refused and nothing is stored",
          "[radio][id]")
{
    uint8_t part[KODE_ID_PART_BYTES];
    radio_fresh();
    go_up();
    uint8_t const before_op = kode_sent()->op;
    (void)memset(part, 0x66, sizeof(part));
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_CERT_PUT, 2U, 3U,
                                part, sizeof(part)));
    pump();
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_BAD_REQUEST, end.status);
    TEST_ASSERT_EQUAL_UINT8(before_op, kode_sent()->op);
}

TEST_CASE("ident_lock asks the C5 to lock and passes its answer on", "[radio][id]")
{
    radio_fresh();
    go_up();
    TEST_ASSERT_TRUE(id_request(ao_spy(), (uint8_t)ID_OP_LOCK, 0U, 0U, NULL, 0U));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_LOCK, kode_sent()->op);
    kode_answer(KODE_ST_LOCKED, NULL, 0U);
    IdEndEvt end;
    spy_expect(ID_END_SIG, &end, sizeof(end));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_LOCKED, end.status);
}

/* ---- c5_wipe: the user's secrets on the C5, and the bonds here --------------- */

TEST_CASE("c5_wipe empties the vault, then the BLE keys, then the bond store",
          "[radio][wipe]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    TEST_ASSERT_TRUE(c5_wipe_request(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_VAULT, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_WIPE, kode_sent()->op);
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_BTKEYS, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_BTKEYS_WIPE, kode_sent()->op);
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_UNPAIR, running_job());
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    finish(KDRV_RADIO_JOB_BLE_UNPAIR, KODE_OK);

    C5WipeRspEvt r;
    spy_expect(C5_WIPE_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, r.status);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)C5_WIPE_DONE, r.step);
    /* the identity was never asked about */
    for ( unsigned back = 0U; 3U > back; back++ )
    {
        TEST_ASSERT_NOT_EQUAL(KODE_SVC_ID,
                              ((const kode_hdr_t *)fake_peer_sent_back(back))->service);
    }
}

TEST_CASE("a vault that will not wipe stops c5_wipe there and says so",
          "[radio][wipe]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    TEST_ASSERT_TRUE(c5_wipe_request(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_WIPE, kode_sent()->op);
    kode_answer(KODE_ST_STORAGE, NULL, 0U);
    C5WipeRspEvt r;
    spy_expect(C5_WIPE_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_STORAGE, r.status);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)C5_WIPE_VAULT, r.step);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_VAULT, kode_sent()->service);   /* no BTKEYS */
    TEST_ASSERT_EQUAL_INT(-1, running_job());
}

TEST_CASE("BLE keys that do not answer stop c5_wipe at that step",
          "[radio][wipe]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    TEST_ASSERT_TRUE(c5_wipe_request(ao_spy()));
    pump();
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT8(KODE_BTKEYS_WIPE, kode_sent()->op);
    tick(QF_SUPPORT_STEPS(3000U) + 2U);
    pump();
    C5WipeRspEvt r;
    spy_expect(C5_WIPE_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)C5_WIPE_BTKEYS, r.step);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
}

TEST_CASE("c5_wipe with the link down is refused at once", "[radio][wipe]")
{
    radio_fresh();
    TEST_ASSERT_TRUE(c5_wipe_request(ao_spy()));
    pump();
    C5WipeRspEvt r;
    spy_expect(C5_WIPE_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)C5_WIPE_VAULT, r.step);
}

/* ---- The station's fast mode, asked for by the Kode channel ------------------ */

/**
 * @brief The remote stack is up and the station has an address, all mocked.
 *
 * kdrv_wifi is the real one, so what AO_Radio reads (started, GOT_IP) is what
 * the driver's own event handler recorded.
 */
static void wifi_station_up(void)
{
    ip_event_got_ip_t got;

    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();
    if ( !kdrv_wifi_started() )
    {
        esp_netif_init_ExpectAndReturn(ESP_OK);
        esp_event_handler_instance_register_Stub(&fake_wifi_events_register);
        esp_wifi_init_ExpectAnyArgsAndReturn(ESP_OK);
        esp_wifi_set_mode_ExpectAndReturn(WIFI_MODE_STA, ESP_OK);
        esp_wifi_start_ExpectAndReturn(ESP_OK);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_start());
    (void)memset(&got, 0, sizeof(got));
    got.ip_info.ip.addr = 0x0100A8C0U;
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &got);
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_GOT_IP, (int)kdrv_wifi_state(NULL));
}

/** @brief The network is gone; the stack stays started, as it does in a drop. */
static void wifi_station_gone(void)
{
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
}

/** @brief What is running now: a power-save job (1), with its mode, or not (0). */
static bool ps_running(bool *on)
{
    if ( running_job() != (int)KDRV_RADIO_JOB_WIFI_PS )
    {
        return false;
    }
    *on = held_req()->on;
    return true;
}

TEST_CASE("the fast mode takes the station's power save off while it is joined, "
          "once, and gives the default back", "[radio_ao][fast]")
{
    bool first_job;
    bool first_on = true;
    bool again_job;
    bool back_job;
    bool back_on = false;

    radio_fresh();
    go_up();
    wifi_station_up();

    wifi_fast_request(true);
    pump();
    first_job = ps_running(&first_on);
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);

    wifi_fast_request(true);          /* what already is: no job */
    pump();
    again_job = (running_job() != -1);

    wifi_fast_request(false);
    pump();
    back_job = ps_running(&back_on);
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);
    wifi_station_gone();

    TEST_ASSERT_TRUE(first_job);
    TEST_ASSERT_FALSE(first_on);      /* on = modem save; fast is "off" there */
    TEST_ASSERT_FALSE(again_job);
    TEST_ASSERT_TRUE(back_job);
    TEST_ASSERT_TRUE(back_on);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
}

TEST_CASE("a wish for the fast mode with no network joined is not applied",
          "[radio_ao][fast]")
{
    int job;

    radio_fresh();
    go_up();
    wifi_station_up();
    wifi_station_gone();

    wifi_fast_request(true);
    pump();
    job = running_job();

    TEST_ASSERT_EQUAL_INT(-1, job);
}

TEST_CASE("a wish made while the worker is busy is applied when it is free",
          "[radio_ao][fast]")
{
    bool during = true;
    bool after_job;
    bool after_on = true;

    radio_fresh();
    go_up();
    wifi_station_up();

    wifi_rssi_request();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_RSSI, running_job());
    wifi_fast_request(true);
    pump();
    during = ps_running(&after_on);                /* the RSSI still runs */
    finish(KDRV_RADIO_JOB_WIFI_RSSI, KODE_OK);
    after_job = ps_running(&after_on);
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);
    wifi_station_gone();

    TEST_ASSERT_FALSE(during);
    TEST_ASSERT_TRUE(after_job);
    TEST_ASSERT_FALSE(after_on);
}

TEST_CASE("a join starts again on the default power save", "[radio_ao][fast]")
{
    bool join_job;
    bool back_job;
    bool back_on = false;

    radio_fresh();
    go_up();
    wifi_station_up();

    wifi_fast_request(true);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);

    wifi_join_request("net", "12345678", false);
    pump();
    join_job = (running_job() == (int)KDRV_RADIO_JOB_WIFI_JOIN);
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_OK);
    back_job = ps_running(&back_on);
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);
    wifi_station_gone();
    /* An accepted join records "wifi wanted"; the next case starts without it. */
    wifi_stop_request();
    pump();
    finish_whatever_runs();

    TEST_ASSERT_TRUE(join_job);
    TEST_ASSERT_TRUE(back_job);
    TEST_ASSERT_TRUE(back_on);
}

TEST_CASE("the worker refusing the power-save job costs a line, not a loop",
          "[radio_ao][fast]")
{
    unsigned before;
    unsigned after;

    radio_fresh();
    go_up();
    wifi_station_up();

    wifi_fast_request(true);
    pump();
    before = job_submits();
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_ERROR);
    pump();
    after = job_submits();
    wifi_station_gone();

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PS, (int)last_req()->type);
    TEST_ASSERT_EQUAL_UINT(before, after);
    TEST_ASSERT_TRUE(logged("would not change the power save"));
}


TEST_CASE("a refused return to the default is tried again by the next other "
          "job, never straight away", "[radio_ao][fast]")
{
    unsigned refused;
    unsigned held;
    bool     again;
    bool     again_on = false;

    radio_fresh();
    go_up();
    wifi_station_up();

    wifi_fast_request(true);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);
    wifi_fast_request(false);
    pump();
    refused = job_submits();
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_ERROR);
    pump();
    held = job_submits();
    wifi_rssi_request();
    pump();
    finish(KDRV_RADIO_JOB_WIFI_RSSI, KODE_OK);
    again = ps_running(&again_on);
    finish_whatever_runs();
    wifi_station_gone();

    TEST_ASSERT_EQUAL_UINT(refused, held);
    TEST_ASSERT_TRUE(again);
    TEST_ASSERT_TRUE(again_on);
}

/* ---- The store before the vault ----------------------------------------------- */

TEST_CASE("the old Wi-Fi store on the P4 is erased at boot, once and only if there",
          "[radio_ao]")
{
    static const uint8_t old[8] = { 'K', 'W', 'I', 'F', 'p', 'a', 's', 's' };
    uint8_t              back[8];
    size_t               n = sizeof(back);

    radio_fresh();
    fake_nvs_seed("kwifi", "nets", old, sizeof(old));
    radio_reboot();
    TEST_ASSERT_TRUE(logged("the old Wi-Fi store on this chip is erased"));
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED,
                          (int)khal_nvs_get_blob("kwifi", "nets", back, &n));

    radio_reboot();
    TEST_ASSERT_FALSE_MESSAGE(logged("the old Wi-Fi store"),
                              "a boot with nothing to erase said it erased something");
    c5_case_end();
}

/* ---- The expansion header's supply -------------------------------------------
 *
 * J3's +3.3V_EXT is switched by the C5's IO5, pulled up on the board. The
 * wish starts off; a link-up leaves the pin as the wish was when the connect
 * went out; with the link up a wish is a job of its own; with no link "on"
 * is a reset of the C5 and "off" waits for the next link-up.
 */

/** radio_ao.c's RADIO_EXP_PULSE_MS: how long the C5 is held for "on". */
#define EXP_PULSE_MS  10U

/**
 * @brief Ask for J3's supply, as kode_sys_exp_power() and `exp_power` do.
 *
 * Dispatched and not pumped: a pump is a 10 ms step, as long as the pulse
 * itself, and would end it before the case could look.
 */
static void exp_ask(bool on)
{
    ExpPowerEvt e = { QEVT_INITIALIZER(EXP_POWER_REQ_SIG), on };
    dispatch_evt(&e.super);
}

/** @brief Leave the wish off and the link off, for the cases after these. */
static void exp_case_end(void)
{
    exp_ask(false);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    settle_to_off();
    storage_drain();
    ao_spy_drain();
}

TEST_CASE("a link-up switches J3's supply off when nobody asked for it",
          "[radio_ao]")
{
    radio_fresh();
    exp_ask(false);

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on,
                              "the connect would leave the supply on");
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_NOT_EQUAL_MESSAGE((int)KDRV_RADIO_JOB_EXP_POWER, running_job(),
                                  "the link-up needed a job of its own");
    exp_case_end();
}

TEST_CASE("with the link up, J3's supply is a job each way and only on a change",
          "[radio_ao]")
{
    radio_fresh();
    exp_ask(false);
    go_up();

    exp_ask(true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    TEST_ASSERT_TRUE(held_req()->on);
    TEST_ASSERT_TRUE(logged("asked on"));
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    TEST_ASSERT_FALSE(job_held());

    exp_ask(true);
    TEST_ASSERT_FALSE_MESSAGE(job_held(),
                              "a wish already carried out was sent again");

    exp_ask(false);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_case_end();
}

TEST_CASE("with no link, J3's supply comes on by a short reset of the C5",
          "[radio_ao]")
{
    /* The case that needs it: a C5 that runs with no link and drives IO5
     * low. A boot that wants nothing holds the C5, and a held C5 already
     * has the supply on, so the amplifier is asked for first - the C5 is
     * then never held again in this boot - and a link-up leaves IO5 low. */
    radio_fresh();
    radio_reboot();
    dispatch(C5_RUN_REQ_SIG);
    tick(QF_SUPPORT_STEPS(KODE_C5_BOOT_MS) + 1U);
    exp_ask(false);
    go_up();
    settle_to_off();
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    fake_klog_reset();

    exp_ask(true);
    TEST_ASSERT_TRUE_MESSAGE(kdrv_radio_c5_held(),
                             "the C5 was not reset to let go of IO5");
    TEST_ASSERT_FALSE_MESSAGE(job_held(),
                              "a job was sent with no link to carry it");
    TEST_ASSERT_TRUE(logged("the C5 is reset"));

    tick(QF_SUPPORT_STEPS(EXP_PULSE_MS) + 1U);
    TEST_ASSERT_FALSE_MESSAGE(kdrv_radio_c5_held(),
                              "the C5 was never let go");

    /* On is on: asked again, the C5 is left alone. */
    fake_klog_reset();
    exp_ask(true);
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    TEST_ASSERT_FALSE(logged("the C5 is reset"));
    exp_case_end();
    /* The amplifier's request outlives the case otherwise. */
    radio_reboot();
    c5_case_end();
}

TEST_CASE("with no link, a C5 held in reset already has J3's supply on",
          "[radio_ao]")
{
    radio_fresh();
    fake_radio_set_c5_held(true);

    exp_ask(true);
    TEST_ASSERT_TRUE(kdrv_radio_c5_held());
    TEST_ASSERT_FALSE(logged("the C5 is reset"));

    tick(QF_SUPPORT_STEPS(EXP_PULSE_MS) + 1U);
    TEST_ASSERT_TRUE_MESSAGE(kdrv_radio_c5_held(),
                             "a hold that was not the pulse's was let go");
    fake_radio_set_c5_held(false);
    exp_case_end();
}

TEST_CASE("an off with no link brings the link up, and the link-up carries it",
          "[radio_ao]")
{
    radio_fresh();
    exp_ask(true);
    tick(QF_SUPPORT_STEPS(EXP_PULSE_MS) + 1U);

    exp_ask(false);
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_CONNECT, running_job(),
                                  "the off did not bring the link up");
    TEST_ASSERT_FALSE(held_req()->on);
    exp_case_end();
}

TEST_CASE("a wish changed while the link comes up is applied once it is up",
          "[radio_ao]")
{
    radio_fresh();
    exp_ask(false);

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_FALSE(held_req()->on);
    exp_ask(true);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_CONNECT, running_job(),
                                  "the wish did not wait for the connect");

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    TEST_ASSERT_TRUE(held_req()->on);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_case_end();
}

TEST_CASE("a switch the C5 refuses drops the wish instead of asking again",
          "[radio_ao]")
{
    radio_fresh();
    exp_ask(false);
    go_up();

    exp_ask(true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_ERROR);
    TEST_ASSERT_FALSE_MESSAGE(job_held(),
                              "a refused switch was sent again");
    TEST_ASSERT_TRUE(logged("would not switch J3's supply"));

    /* A new request asks again. */
    exp_ask(true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_case_end();
}

/* ---- The end of a switch of J3's supply ----------------------------------------
 *
 * EXP_POWER_SIG says a switch is over: carried out, refused, or with no link
 * an "off" that has to wait for one. Published, so only a subscriber hears
 * it: the spy subscribes for these cases alone.
 */

/** @brief What the spy heard of EXP_POWER_SIG since the last look. */
typedef struct {
    unsigned      n;    /**< How many.                          */
    bool          on;   /**< The last one's switch.             */
    kode_status_t rc;   /**< The last one's code (kode_status_t). */
} exp_heard_t;

/** @brief Take every event the spy holds and count EXP_POWER_SIG among them. */
static exp_heard_t exp_heard(void)
{
    exp_heard_t h = { 0U, false, KODE_TIMEOUT };

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();

        if ( NULL == e )
        {
            break;
        }
        if ( EXP_POWER_SIG == e->sig )
        {
            const ExpPowerDoneEvt * const d = (const ExpPowerDoneEvt *)e;

            h.n  += 1U;
            h.on  = d->on;
            h.rc  = (kode_status_t)d->rc;
        }
    }
    ao_spy_release();
    return h;
}

/** @brief The spy hears the end of a switch from here on. */
static void exp_listen(void)
{
    ao_spy_subscribe(EXP_POWER_SIG);
    ao_spy_drain();
}

/** @brief The spy stops hearing it, and the wish and the link are left off. */
static void exp_listen_end(void)
{
    QActive_unsubscribe(ao_spy(), EXP_POWER_SIG);
    exp_case_end();
}

/** @brief Exactly one EXP_POWER_SIG was heard, and it said @p on and @p rc. */
static void exp_heard_one(bool on, kode_status_t rc)
{
    const exp_heard_t h = exp_heard();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.n, "one end of the switch is said");
    TEST_ASSERT_EQUAL_MESSAGE(on, h.on, "the end names the switch");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)rc, (int)h.rc, "the end says how it went");
}

TEST_CASE("a switch of J3's supply the C5 carries out is said once it is over, on and ok",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    go_up();
    exp_listen();

    exp_ask(true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said before the C5 drove the pin");
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_heard_one(true, KODE_OK);

    /* Asked again, the rail is as asked: said at once, with no job. */
    exp_ask(true);
    TEST_ASSERT_FALSE(job_held());
    exp_heard_one(true, KODE_OK);

    exp_ask(false);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_heard_one(false, KODE_OK);
    exp_listen_end();
}

TEST_CASE("a switch of J3's supply the C5 refuses is said refused, KODE_IO_ERROR",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    go_up();
    exp_listen();

    exp_ask(true);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_ERROR);
    exp_heard_one(true, KODE_IO_ERROR);
    exp_listen_end();
}

TEST_CASE("an off asked while a refused on is with the C5 is said too, once the rail is off",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    go_up();
    exp_listen();

    exp_ask(true);
    exp_ask(false);
    TEST_ASSERT_EQUAL_UINT(0U, exp_heard().n);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_ERROR);
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "the refusal left the rail off: nothing to send");

    /* The on is said refused, then the off as carried out. */
    const exp_heard_t h = exp_heard();

    TEST_ASSERT_EQUAL_UINT(2U, h.n);
    TEST_ASSERT_FALSE(h.on);
    TEST_ASSERT_EQUAL_INT((int)KODE_OK, (int)h.rc);
    exp_listen_end();
}

TEST_CASE("a request inside a switch on its way is said by that switch's end, once",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    go_up();
    exp_listen();

    exp_ask(true);
    exp_ask(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said while the C5 still had the job");
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_heard_one(true, KODE_OK);
    exp_listen_end();
}

TEST_CASE("with no link, the reset that puts J3's supply on is said, on and ok",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    radio_reboot();
    dispatch(C5_RUN_REQ_SIG);
    tick(QF_SUPPORT_STEPS(KODE_C5_BOOT_MS) + 1U);
    exp_ask(false);
    go_up();
    settle_to_off();
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    exp_listen();

    exp_ask(true);
    TEST_ASSERT_TRUE(kdrv_radio_c5_held());
    exp_heard_one(true, KODE_OK);
    tick(QF_SUPPORT_STEPS(EXP_PULSE_MS) + 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "the let-go is not a second end");
    exp_listen_end();
    radio_reboot();
    c5_case_end();
}

TEST_CASE("with no link, a C5 held in reset says J3's supply is on at once",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    fake_radio_set_c5_held(true);
    exp_listen();

    exp_ask(true);
    exp_heard_one(true, KODE_OK);
    fake_radio_set_c5_held(false);
    exp_listen_end();
}

TEST_CASE("an off with no link, Wi-Fi and Bluetooth off, brings the link up and is said ok "
          "once it has switched; the link stays up",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(true);
    tick(QF_SUPPORT_STEPS(EXP_PULSE_MS) + 1U);
    exp_listen();

    exp_ask(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said before the link could carry it");
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the connect would leave the supply on");
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    exp_heard_one(false, KODE_OK);

    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_UP, link_now(),
                                  "the link the switch brought up came down");
    exp_listen_end();
}

TEST_CASE("an off with no link whose link-up fails is said refused, KODE_DENIED",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(true);
    tick(QF_SUPPORT_STEPS(EXP_PULSE_MS) + 1U);
    exp_listen();

    exp_ask(false);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_EQUAL_UINT(0U, exp_heard().n);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_ERROR);
    exp_heard_one(false, KODE_DENIED);
    exp_listen_end();
    c5_case_end();
}

TEST_CASE("an asked scan and a switch of J3's supply owed behind one job: the switch goes first",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    go_up();

    exp_ask(true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    exp_ask(false);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());

    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_EXP_POWER, running_job(),
                                  "the asked scan went ahead of J3's switch");
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);

    scan_sweep(1U, -50, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, scan_take_spy().ends, "the scan was not run after the switch");
    ao_spy_drain();
    exp_case_end();
}

TEST_CASE("a switch of J3's supply asked during a sweep goes before the autojoin round owed behind it",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    go_up();
    vault_has("line-ap-0");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    dispatch(WIFI_START_REQ_SIG);   /* the round is owed behind the sweep */
    pump();
    exp_ask(true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    scan_sweep(1U, -50, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_EXP_POWER, running_job(),
                                  "the owed autojoin round went ahead of J3's switch");
    TEST_ASSERT_TRUE(held_req()->on);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job(),
                                  "the owed round did not run after the switch");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
    exp_case_end();
}

TEST_CASE("an on asked while the link comes up is said once the link-up's job has switched it",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    exp_listen();

    dispatch(RADIO_UP_REQ_SIG);
    (void)exp_heard();
    exp_ask(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said before the link could carry it");

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    TEST_ASSERT_EQUAL_UINT(0U, exp_heard().n);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_heard_one(true, KODE_OK);
    exp_listen_end();
}

TEST_CASE("a switch of J3's supply inside the worker when the link goes is said owed to the next link, once",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    go_up();
    exp_listen();

    exp_ask(true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    fake_radio_drop_link();
    pump();
    exp_heard_one(true, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());

    /* It comes back about a link that is gone: nobody is told twice. */
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(0U, exp_heard().n);
    exp_listen_end();
    c5_case_end();
}

TEST_CASE("an off asked while the link comes up is said owed at once when the bring-up fails",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    exp_listen();

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    exp_ask(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said before the bring-up was over");

    finish(KDRV_RADIO_JOB_CONNECT, KODE_ERROR);
    exp_heard_one(false, KODE_DENIED);
    exp_listen_end();
    c5_case_end();
}

TEST_CASE("an on asked while the link comes up resets the C5 when the bring-up fails, "
          "and is said on",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    exp_listen();

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    exp_ask(true);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said before the bring-up was over");

    /* The failed bring-up may never have reset the C5: the rail is not
     * known, so the "on" is carried out by a reset of its own (counted: the
     * finish's pump outlasts the pulse). */
    const unsigned holds = fake_radio_c5_holds();

    finish(KDRV_RADIO_JOB_CONNECT, KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(holds + 1U, fake_radio_c5_holds(), "the on was said with no reset");
    exp_heard_one(true, KODE_OK);
    tick(QF_SUPPORT_STEPS(EXP_PULSE_MS) + 1U);
    exp_listen_end();
    c5_case_end();
}

/**
 * @brief radio_reboot() with the boot's hold of the C5 skipped, as a build
 *        without KODEOS_C5_HOLD boots: nothing has driven or let go of IO5,
 *        so the rail is as the last firmware left it, and not known.
 */
static void radio_reboot_unheld(void)
{
    tick(QF_SUPPORT_STEPS(KODE_C5_BOOT_MS) + 8U);
    fake_radio_power_cycle();
    /* Held by nobody the AO knows of: Radio_off's entry holds only a C5
     * that is not held already. */
    fake_radio_set_c5_held(true);
    Radio_ctor();
    qf_support_give_queue(AO_Radio);
    AO_Radio->prio  = KODE_PRIO_RADIO;
    AO_Radio->pthre = KODE_PRIO_RADIO;
    fake_klog_reset();
    QASM_INIT(AO_Radio, (void *)0, 0U);
    (void)pull_jobs();
    storage_drain();
    fake_radio_set_c5_held(false);
}

TEST_CASE("a first off of a boot is not said done because the rail was believed off: "
          "with no link it brings the link up, and is said once the pin is driven",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    radio_reboot_unheld();
    exp_listen();

    exp_ask(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said done on what was believed");

    /* The link-up drives the pin off, and from then on the rail is known. */
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    exp_heard_one(false, KODE_OK);
    exp_ask(false);
    TEST_ASSERT_FALSE(job_held());
    exp_heard_one(false, KODE_OK);
    exp_listen_end();
    c5_case_end();
}

TEST_CASE("a link lost with the rail known off leaves it unknown: an off is then said owed, not done",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    go_up();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    exp_listen();
    exp_ask(false);
    exp_heard_one(false, KODE_OK);

    /* Often a C5 that restarted by itself, and let go of IO5. */
    fake_radio_drop_link();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    exp_ask(false);
    exp_heard_one(false, KODE_DENIED);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    exp_listen_end();
    c5_case_end();
}

TEST_CASE("a C5 update leaves J3's rail unknown: an off then brings the link up, and is "
          "said once the pin is driven",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    go_up();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    /* Something asked for the C5 this boot, so Radio_off holds it in reset
     * no more: the rail after the update is what the new image left. */
    dispatch(C5_RUN_REQ_SIG);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
    recv_spy();
    inst_deliver();
    inst_push_to_end();
    ao_spy_drain();
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    exp_listen();

    exp_ask(false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said done on a rail nobody knows");
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    exp_heard_one(false, KODE_OK);
    exp_listen_end();
    radio_reboot();
    c5_case_end();
}

TEST_CASE("a link-up whose switch of J3's supply failed leaves the rail unknown: "
          "a request waiting on it gets a job",
          "[radio_ao][exp_power]")
{
    radio_fresh();
    exp_ask(false);
    exp_listen();
    fake_radio_fail_exp_switch(true);

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    exp_ask(false);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, exp_heard().n, "said off when the link-up did not drive the pin");
    pump();
    while ( running_job() != (int)KDRV_RADIO_JOB_EXP_POWER )
    {
        TEST_ASSERT_TRUE_MESSAGE(job_held(), "no job of its own for the rail the link-up left unknown");
        finish_whatever_runs();
    }
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    exp_heard_one(false, KODE_OK);
    fake_radio_fail_exp_switch(false);
    exp_listen_end();
}

/* ---- The station's two settings ------------------------------------------------ */

/* `wifi_ps=off` is a standing wish for no power save: applied at once with a
 * network joined, by the fast mode's own job, and `on` gives the default back. */
TEST_CASE("wifi_ps off keeps the station out of power save; on gives it back",
          "[radio_ao][fast]")
{
    const KodeConfigAnsEvt *a;
    bool off_job;
    bool off_on = true;
    bool on_job;
    bool on_on  = false;

    radio_fresh();
    go_up();
    wifi_station_up();

    a = config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_WIFI_PS, .set = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_TRUE(a->stored);
    pump();
    off_job = ps_running(&off_on);
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);

    a = config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_WIFI_PS, .set = true,
                                              .b = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("on", a->value);
    pump();
    on_job = ps_running(&on_on);
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);
    wifi_station_gone();
    fake_nvs_reset();
    ao_spy_drain();

    TEST_ASSERT_TRUE(off_job);
    TEST_ASSERT_FALSE(off_on);
    TEST_ASSERT_TRUE(on_job);
    TEST_ASSERT_TRUE(on_on);
}

/* `wifi_join=off`: a link brought up for WiFi says it is enabled and waits to
 * be told which network, instead of starting the automatic round. */
TEST_CASE("wifi_join off brings WiFi up idle, with no automatic round",
          "[radio_ao]")
{
    const KodeConfigAnsEvt *a;
    int job;

    radio_fresh();
    a = config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_WJOIN, .set = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_TRUE(a->stored);

    wifi_start_request();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    pump();
    job = running_job();

    wifi_stop_request();
    pump();
    (void)config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_WJOIN, .set = true,
                                                .b = true });
    fake_nvs_reset();
    c5_case_end();

    TEST_ASSERT_NOT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, job,
                                      "the link up started the automatic round");
}

/* ---- config key="wifi" and key="ble" ----------------------------------------
 * The value is the service's stored wish. A set does not answer on the spot:
 * it posts the same request `wifi_up`/`wifi_down` (`ble_up`/`ble_down`) post,
 * then a read behind it, so the answer reports what that request made of the
 * wish. Both top states handle the key - the link off and the link up - and
 * both are walked.
 */

/** @brief The answer an owner published, taken off the spy; NULL when none. */
static const KodeConfigAnsEvt *config_answer_taken(void)
{
    const QEvt *e = ao_spy_take();

    while ( (NULL != e) && ((QSignal)CONFIG_ANS_SIG != e->sig) )
    {
        e = ao_spy_take();
    }
    return (const KodeConfigAnsEvt *)e;
}

/**
 * @brief Set @p key to @p on, let the AO handle what the set posted, and take
 *        the answer.
 *
 * @param key The setting, KSET_KEY_WIFI or KSET_KEY_BLE.
 * @param on  The wish.
 * @return The answer; NULL when the AO published none.
 */
static const KodeConfigAnsEvt *radio_service_set(const char *key, bool on)
{
    /* Answered behind the request it posts, so this call has nothing to take. */
    (void)config_ask(AO_Radio, &(config_ask_t){ .key = key, .set = true, .b = on });
    pump();
    return config_answer_taken();
}

/** @brief What service_stored() returns for a key with nothing under it. */
#define SERVICE_NOT_STORED  0xFFU

/** @brief The byte stored under @p key, or SERVICE_NOT_STORED. */
static uint8_t service_stored(const char *key)
{
    uint8_t v = SERVICE_NOT_STORED;

    if ( khal_nvs_get_u8(KSET_NAMESPACE, key, &v) != KODE_OK )
    {
        v = SERVICE_NOT_STORED;
    }
    return v;
}

/**
 * @brief One round of the wish's life: read, switch on, switch off.
 *
 * @param key      KSET_KEY_WIFI or KSET_KEY_BLE.
 * @param link_up  Walk the top state with the link up rather than off.
 */
static void service_round(const char *key, bool link_up)
{
    const KodeConfigAnsEvt *a;

    radio_fresh();
    fake_nvs_reset();
    if ( link_up )
    {
        go_up();
    }

    a = config_ask(AO_Radio, &(config_ask_t){ .key = key });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_FALSE(a->stored);
    ao_spy_drain();

    a = radio_service_set(key, true);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("on", a->value);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_UINT8(1U, service_stored(key));
    ao_spy_drain();

    a = config_ask(AO_Radio, &(config_ask_t){ .key = key });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("on", a->value);
    ao_spy_drain();

    a = radio_service_set(key, false);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_UINT8(0U, service_stored(key));

    fake_nvs_reset();
    c5_case_end();
}

TEST_CASE("config wifi with the link off stores the wish and answers it",
          "[radio_ao]")
{
    service_round(KSET_KEY_WIFI, false);
}

TEST_CASE("config wifi with the link up stores the wish and answers it",
          "[radio_ao]")
{
    service_round(KSET_KEY_WIFI, true);
}

TEST_CASE("config ble with the link off stores the wish and answers it",
          "[radio_ao]")
{
    service_round(KSET_KEY_BLE, false);
}

TEST_CASE("config ble with the link up stores the wish and answers it",
          "[radio_ao]")
{
    service_round(KSET_KEY_BLE, true);
}

/* ---- The asked BLE listen and the scan response ---------------------------- */

/** @brief The listen a case asks for, in ms. */
#define BLE_ASK_MS  2000U

/** @brief What one asker received from a listen: entries, the end, and anything else. */
typedef struct {
    unsigned          entries;   /**< BLE_SCAN_ENTRY_SIG.              */
    unsigned          ends;      /**< BLE_SCAN_RSP_SIG.                */
    unsigned          bus_ends;  /**< BLE_SCAN_END_SIG: the console's. */
    unsigned          other;     /**< Anything else.                   */
    const ScanRspEvt *end;       /**< The last end.                    */
} ble_seen_t;

/** @brief Read everything the spy holds. */
static ble_seen_t ble_take_spy(void)
{
    ble_seen_t  s = { 0U, 0U, 0U, 0U, NULL };
    const QEvt *e;

    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)BLE_SCAN_ENTRY_SIG == e->sig )
        {
            ++s.entries;
        }
        else if ( (QSignal)BLE_SCAN_RSP_SIG == e->sig )
        {
            ++s.ends;
            s.end = (const ScanRspEvt *)(const void *)e;
        }
        else if ( (QSignal)BLE_SCAN_END_SIG == e->sig )
        {
            ++s.bus_ends;
        }
        else
        {
            ++s.other;
        }
    }
    return s;
}

/** @brief Bluetooth on in Settings, the link up and the board on the air. */
static void ble_on_up(void)
{
    radio_fresh();
    fake_ble_reset();
    fake_nvs_reset();
    go_up();
    (void)radio_service_set(KSET_KEY_BLE, true);
    finish_ble_start();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_TRUE_MESSAGE(ble_enabled(), "the switch was not mirrored");
    ao_spy_drain();
}

/** @brief Bluetooth back off in Settings, and the AO walked back to off. */
static void ble_case_end(void)
{
    (void)radio_service_set(KSET_KEY_BLE, false);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_FALSE(ble_enabled());
    fake_nvs_reset();
    c5_case_end();
    fake_ble_reset();
}

/** @brief Post the scan response request kode_sys_ble_set_*() posts, and let the AO take it. */
static void ble_rsp_ask(const char *name, const uint8_t *data, size_t len)
{
    QEvt const *const e = ble_rsp_evt(name, data, len);

    TEST_ASSERT_NOT_NULL(e);
    QACTIVE_POST(AO_Radio, e, NULL);
    pump();
}

TEST_CASE("an asked BLE listen posts its devices to the asker, publishes none, and its door opens at the end",
          "[radio_ao][ble_ask]")
{
    ble_seen_t s;

    ble_on_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_scan(ao_spy(), BLE_ASK_MS));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_ble_scan(ao_spy(), BLE_ASK_MS),
                                  "a second listen of the asker's waits for the first's end");
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(BLE_ASK_MS, held_req()->len, "the listen is not the length asked");
    finish_ble_scan(3U, -50);

    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT(3U, s.entries);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.ends, "the asker hears one end");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.bus_ends, "the console's end was published as well");
    TEST_ASSERT_EQUAL_UINT(0U, s.other);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, s.end->rc);
    TEST_ASSERT_EQUAL_UINT8(3U, s.end->seen);
    TEST_ASSERT_FALSE(s.end->more);
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ble_scan_ask(ao_spy(), KODE_BLE_SCAN_MIN_MS),
                                  "the end opens the door again");
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_ERROR);
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_ERROR, s.end->rc, "a failed listen is a failure");
    TEST_ASSERT_EQUAL_UINT(0U, s.entries);
    ao_spy_drain();
    ble_case_end();
}

TEST_CASE("a BLE listen of 499 or 30001 ms is refused and posts nothing", "[radio_ao][ble_ask]")
{
    ScanWaiter w;

    ble_on_up();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_scan(ao_spy(), KODE_BLE_SCAN_MIN_MS - 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_ble_scan(ao_spy(), KODE_BLE_SCAN_MAX_MS + 1U));
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_NULL_MESSAGE(ao_spy_take(), "a refused listen was answered as well");

    (void)memset(&w, 0, sizeof(w));
    w.done = khal_sem_create();
    ble_scan_request_wait(&w, 5U, KODE_BLE_SCAN_MAX_MS + 1U);
    TEST_ASSERT_EQUAL_UINT32(5U, w.ack);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_DENIED, w.rc);
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    ble_case_end();
}

TEST_CASE("with Bluetooth off in Settings an asked BLE listen is refused, denied, and nothing runs",
          "[radio_ao][ble_ask]")
{
    ble_seen_t s;

    radio_fresh();
    fake_nvs_reset();
    go_up();
    (void)radio_service_set(KSET_KEY_BLE, false);
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_FALSE(ble_enabled());
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, ble_scan_ask(ao_spy(), BLE_ASK_MS));
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a listen ran with Bluetooth off");
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, s.end->rc);
    ao_spy_drain();
    fake_nvs_reset();
    c5_case_end();
}

TEST_CASE("with Bluetooth off an asked BLE listen is denied during the autojoin round, "
          "not busy", "[radio_ao][ble_ask]")
{
    ble_seen_t s;

    radio_fresh();
    fake_nvs_reset();
    go_up();
    TEST_ASSERT_FALSE(ble_enabled());
    vault_has("line-ap-0");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    ao_spy_drain();

    /* The round refuses every scan as busy, but asking again would never
     * help a listen with Bluetooth off: the switch is read first. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, ble_scan_ask(ao_spy(), BLE_ASK_MS));
    pump();
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_DENIED, s.end->rc,
                                    "a listen with Bluetooth off was told to ask again");
    ao_spy_drain();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_NO_RESOURCES, scan_answered_at_once()->rc,
                                    "a Wi-Fi scan in the round is still busy");
    ao_spy_drain();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
    fake_nvs_reset();
    c5_case_end();
}

TEST_CASE("a BLE listen and a Wi-Fi scan share one slot: the second is refused at once, busy",
          "[radio_ao][ble_ask]")
{
    ble_seen_t s;

    ble_on_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, wifi_scan_ask(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, ble_scan_ask(ao_spy(), BLE_ASK_MS),
                                  "the door is the kind's, not the radio's");
    pump();
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, s.end->rc);
    ao_spy_drain();
    scan_sweep(1U, -50, KODE_OK);
    ao_spy_drain();
    ble_case_end();
}

TEST_CASE("an asked BLE listen while the console's listen runs is refused at once, busy",
          "[radio_ao][ble_ask]")
{
    ble_seen_t s;

    ble_on_up();
    TEST_ASSERT_TRUE(ble_scan_request(BLE_ASK_MS));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    TEST_ASSERT_EQUAL_INT(KODE_OK, ble_scan_ask(ao_spy(), BLE_ASK_MS));
    pump();
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.ends, "the asker was owed behind the console's listen");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, s.end->rc);
    ao_spy_drain();

    finish_ble_scan(2U, -60);
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.bus_ends, "the console's listing lost its end");
    TEST_ASSERT_EQUAL_UINT(0U, s.ends);

    /* The console's listen is over: an asked one runs again. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, ble_scan_ask(ao_spy(), BLE_ASK_MS));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    finish_ble_scan(1U, -60);
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, s.end->rc);
    ao_spy_drain();
    ble_case_end();
}

TEST_CASE("a task's BLE listen is written into its waiter and tagged with its generation",
          "[radio_ao][ble_ask]")
{
    kdrv_ble_dev_t rows[KDRV_BLE_SCAN_MAX];
    ScanWaiter     w;
    ble_seen_t     s;

    ble_on_up();
    (void)memset(rows, 0, sizeof(rows));
    (void)memset(&w, 0, sizeof(w));
    w.done = khal_sem_create();
    w.rows = rows;
    w.cap  = (uint8_t)KDRV_BLE_SCAN_MAX;
    w.rc   = (uint8_t)KODE_TIMEOUT;

    ble_scan_request_wait(&w, 9U, BLE_ASK_MS);
    pump();
    TEST_ASSERT_EQUAL_UINT32(BLE_ASK_MS, held_req()->len);
    finish_ble_scan(2U, -48);
    TEST_ASSERT_EQUAL_UINT32(9U, w.ack);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, w.rc);
    TEST_ASSERT_EQUAL_UINT8(2U, w.got);
    TEST_ASSERT_EQUAL_STRING("beacon-0", rows[0].name);
    TEST_ASSERT_EQUAL_INT8(-48, rows[0].rssi);
    s = ble_take_spy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.entries + s.ends + s.bus_ends, "a task's listen reached an AO");
    ao_spy_drain();
    ble_case_end();
}

TEST_CASE("the scan response an app asks for goes to the radio as one job, the advert untouched",
          "[radio_ao][ble_ask]")
{
    static const uint8_t DATA[4] = { 0xFFU, 0xFFU, 0x01U, 0x02U };

    ble_on_up();
    ble_rsp_ask("Dot-7", DATA, sizeof(DATA));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_RSP, running_job());
    TEST_ASSERT_EQUAL_STRING("Dot-7", held_req()->ssid);
    TEST_ASSERT_EQUAL_UINT32(sizeof(DATA), held_req()->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(DATA, held_req()->data, sizeof(DATA));
    finish(KDRV_RADIO_JOB_BLE_RSP,
           kdrv_ble_set_scan_rsp(held_req()->ssid, held_req()->data, held_req()->len));
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "the advert was started again");

    /* Back to the serial: the name goes empty, the data stays as asked. */
    ble_rsp_ask(NULL, DATA, sizeof(DATA));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_RSP, running_job());
    TEST_ASSERT_EQUAL_STRING("", held_req()->ssid);
    finish(KDRV_RADIO_JOB_BLE_RSP, KODE_OK);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    ao_spy_drain();
    ble_case_end();
}

/** @brief The name the next BLE state in the spy says; fails the case if none. */
static void take_ble_name(char out[KQP_BLE_NAME_MAX])
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "no BLE state was published");
        if ( (QSignal)BLE_STATE_SIG == e->sig )
        {
            (void)snprintf(out, KQP_BLE_NAME_MAX, "%s", ((const BleStateEvt *)e)->name);
            return;
        }
    }
}

TEST_CASE("the BLE state says the name the board goes by: the serial, then the one that was set",
          "[radio_ao][ble_ask]")
{
    char          name[KQP_BLE_NAME_MAX];
    kode_status_t rc;

    ble_on_up();
    ao_spy_subscribe(BLE_STATE_SIG);
    ao_spy_drain();

    dispatch(BLE_STATE_REQ_SIG);
    take_ble_name(name);
    TEST_ASSERT_EQUAL_STRING(kdrv_ble_serial_name(), name);
    ao_spy_drain();

    ble_rsp_ask("KODE zz", NULL, 0U);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_RSP, running_job());
    /* The worker is writing the name: a state asked now says the one it had. */
    rc = kdrv_ble_set_scan_rsp(held_req()->ssid, held_req()->data, held_req()->len);
    dispatch(BLE_STATE_REQ_SIG);
    take_ble_name(name);
    TEST_ASSERT_EQUAL_STRING_MESSAGE(kdrv_ble_serial_name(), name,
                                     "the name was read under the worker");
    ao_spy_drain();

    finish(KDRV_RADIO_JOB_BLE_RSP, rc);
    dispatch(BLE_STATE_REQ_SIG);
    take_ble_name(name);
    TEST_ASSERT_EQUAL_STRING("KODE zz", name);
    ao_spy_drain();

    /* Back to the serial. */
    ble_rsp_ask(NULL, NULL, 0U);
    finish(KDRV_RADIO_JOB_BLE_RSP, kdrv_ble_set_scan_rsp(NULL, NULL, 0U));
    dispatch(BLE_STATE_REQ_SIG);
    take_ble_name(name);
    TEST_ASSERT_EQUAL_STRING(kdrv_ble_serial_name(), name);
    ao_spy_drain();
    ble_case_end();
}

TEST_CASE("a scan response asked while the radio is busy waits, and goes once the job ends",
          "[radio_ao][ble_ask]")
{
    static const uint8_t DATA[2] = { 0x34U, 0x12U };

    ble_on_up();
    TEST_ASSERT_TRUE(ble_scan_request(BLE_ASK_MS));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    ble_rsp_ask("first", DATA, sizeof(DATA));
    ble_rsp_ask("kept", NULL, 0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_BLE_SCAN, running_job(),
                                  "the response pushed into a busy channel");
    finish_ble_scan(1U, -60);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_RSP, running_job());
    TEST_ASSERT_EQUAL_STRING_MESSAGE("kept", held_req()->ssid, "the last asked is the one sent");
    TEST_ASSERT_EQUAL_UINT32(0U, held_req()->len);
    finish(KDRV_RADIO_JOB_BLE_RSP, KODE_OK);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    ao_spy_drain();
    ble_case_end();
}

TEST_CASE("a scan response asked with Bluetooth off is kept, and goes once it is on",
          "[radio_ao][ble_ask]")
{
    radio_fresh();
    fake_ble_reset();
    fake_nvs_reset();
    go_up();
    TEST_ASSERT_FALSE(ble_enabled());
    ble_rsp_ask("later", NULL, 0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a response went out with Bluetooth off");

    (void)radio_service_set(KSET_KEY_BLE, true);
    finish_ble_start();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_RSP, running_job());
    TEST_ASSERT_EQUAL_STRING("later", held_req()->ssid);
    finish(KDRV_RADIO_JOB_BLE_RSP, KODE_OK);
    ao_spy_drain();
    ble_case_end();
}

/* The interface sends the stored name as soon as it can post, which on a boot
 * is before the link exists, and AO_Audio asks for the amplifier at the same
 * time. The amplifier's job makes the bring-up's autojoin wait its turn, and
 * with no network in the vault that autojoin then ends without a job: nothing
 * comes back to pay what waited behind it. The name is one of those debts. */
TEST_CASE("a scan response asked before a boot's link is up goes out when the bring-up ends "
          "with no network to join", "[radio_ao][ble_ask]")
{
    uint8_t const     on   = 1U;
    uint8_t const     off  = 0U;
    bool              sent = false;
    kode_vault_list_t none;

    radio_fresh();
    fake_ble_reset();
    fake_nvs_reset();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_WIFI, &on, sizeof(on));
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_BLE, &on, sizeof(on));
    radio_reboot();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    dispatch(C5_AMP_OFF_SIG);
    ble_rsp_ask("KODE zz", NULL, 0U);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_CONNECT, running_job(),
                                  "something went out with the link down");

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    (void)memset(&none, 0, sizeof(none));
    kode_answer(KODE_ST_OK, &none, (uint16_t)sizeof(none));

    /* Every job of the bring-up, in whatever order the AO runs them. */
    for ( unsigned guard = 0U; job_held() && (12U > guard); ++guard )
    {
        int const job = running_job();

        if ( (int)KDRV_RADIO_JOB_BLE_START == job )
        {
            finish_ble_start();
        }
        else if ( (int)KDRV_RADIO_JOB_BLE_ADV == job )
        {
            finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
        }
        else if ( (int)KDRV_RADIO_JOB_BLE_RSP == job )
        {
            TEST_ASSERT_EQUAL_STRING("KODE zz", held_req()->ssid);
            sent = true;
            finish(KDRV_RADIO_JOB_BLE_RSP, KODE_OK);
        }
        else
        {
            finish((kdrv_radio_job_t)job, KODE_OK);
        }
        pump();
    }
    TEST_ASSERT_FALSE(job_held());
    TEST_ASSERT_TRUE_MESSAGE(logged("no networks remembered"),
                             "the bring-up did not end at the empty vault");
    TEST_ASSERT_TRUE_MESSAGE(sent, "the bring-up is over and the name was never sent");

    /* Both wishes and the amplifier back as the cases after this one expect
     * them, the link walked down, and a last boot that wants nothing: this AO
     * outlives the case. */
    ao_spy_drain();
    (void)radio_service_set(KSET_KEY_BLE, false);
    (void)radio_service_set(KSET_KEY_WIFI, false);
    radio_amp_request(true);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    c5_case_end();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_WIFI, &off, sizeof(off));
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_BLE, &off, sizeof(off));
    radio_reboot();
    c5_case_end();
    fake_nvs_reset();
    fake_ble_reset();
}

/* Switching wifi on from a dark link is `wifi_up`: the C5 is asked for the
 * link, and that is what makes the wish more than a stored byte. */
TEST_CASE("config wifi on with the link off asks the C5 for the link",
          "[radio_ao]")
{
    radio_fresh();
    fake_nvs_reset();

    (void)radio_service_set(KSET_KEY_WIFI, true);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    fake_nvs_reset();
    ao_spy_drain();
    c5_case_end();
}

/* The two services are independent wishes: setting one must not move or store
 * the other. */
TEST_CASE("config wifi and config ble keep separate wishes", "[radio_ao]")
{
    const KodeConfigAnsEvt *a;

    radio_fresh();
    fake_nvs_reset();

    (void)radio_service_set(KSET_KEY_WIFI, true);
    ao_spy_drain();
    a = config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_BLE });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_FALSE(a->stored);
    TEST_ASSERT_EQUAL_UINT8(SERVICE_NOT_STORED, service_stored(KSET_KEY_BLE));
    ao_spy_drain();

    (void)radio_service_set(KSET_KEY_WIFI, false);
    fake_nvs_reset();
    ao_spy_drain();
    c5_case_end();
}

/* One key, one owner: a setting that is not the radio's gets no answer, so the
 * console never reads two for one question. */
TEST_CASE("another owner's key is not AO_Radio's to answer", "[radio_ao]")
{
    radio_fresh();

    TEST_ASSERT_NULL(config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_VOL }));

    ao_spy_drain();
    c5_case_end();
}

/* ---- The workers ---------------------------------------------------------------
 * AO_Radio hands the C5's waits to two KqpWorkers: one for the link's jobs
 * (internal stack: the bring-up initialises NVS and the BLE jobs use NimBLE's
 * bond store) and one for the KODE sends (PSRAM: a send is one RPC and
 * touches no flash). The cases below are what the move to them promises: the
 * AO keeps serving while a job is inside, a job is never submitted over one
 * that is, a send is not held up behind a job, and what comes back late
 * answers nobody.
 */

/** @brief Take the service worker's done event out of the queue, unread. */
static const QEvt *hold_svc_done(void)
{
    const QEvt *const done = take_queued(RADIO_SVC_JOB_SIG);

    TEST_ASSERT_NOT_NULL_MESSAGE(done, "the service worker's done event was not queued");
    return done;
}

/** @brief Hand a held done event to the AO and recycle it. */
static void release_done(const QEvt *done)
{
    dispatch_evt(done);
    QF_gc(done);
    drain();
}

/** @brief Whether the spy holds a scan end marker; takes the events it passes. */
static bool s_end_busy;   /**< The busy flag of the end marker scan_end_seen() last took. */

static bool scan_end_seen(bool *ok, uint8_t *count)
{
    for ( ;; )
    {
        const QEvt *const e = ao_spy_take();

        if ( NULL == e )
        {
            return false;
        }
        if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            *ok    = ((const WifiScanEndEvt *)e)->ok;
            *count = ((const WifiScanEndEvt *)e)->count;
            s_end_busy = ((const WifiScanEndEvt *)e)->busy;
            ao_spy_release();
            return true;
        }
        ao_spy_release();
    }
}

TEST_CASE("the radio worker keeps an internal stack, the KODE sends a worker of their own in PSRAM",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();

    TEST_ASSERT_FALSE_MESSAGE(fake_khal_worker_last_psram_for(RADIO_WORKER),
        "the bring-up initialises NVS and the BLE jobs use its bond store: "
        "an internal stack");
    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_last_psram_for(RADIO_SVC_WORKER),
        "a send touches no flash: its worker's stack is PSRAM");
    c5_case_end();
}

/* The brief's name: a send is a job, and not a call in the AO's step. */
TEST_CASE("test_radio_svc_send_runs_as_job: a KODE request is sent by the service worker, "
          "and its answer's deadline starts when the send has returned",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();
    fake_peer_reset();

    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    (void)radio_settle();   /* the AO's step, and nothing else */

    size_t sent = 99U;
    (void)fake_peer_last_sent(&sent);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sent, "the AO sent the request from its own step");
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(RADIO_SVC_WORKER));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the link's worker was used for a send");

    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_ID, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_SIGN_AUTH, kode_sent()->op);

    /* The send is back and the AO has not read that: what runs is the send's
     * own guard, a minute long, and not the answer's three seconds. */
    const QEvt *const done = hold_svc_done();
    (void)qf_support_tick(AO_Radio, QF_SUPPORT_STEPS(2000U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "refused while the send had not even been read as returned");
    release_done(done);

    /* Now the answer's three seconds start, and not before. */
    tick(QF_SUPPORT_STEPS(2500U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the deadline counted the time the send took");
    tick(QF_SUPPORT_STEPS(700U));
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);
    c5_case_end();
}

TEST_CASE("a KODE answer is not held up by a scan inside the radio worker",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    fake_peer_reset();
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    pump();
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(KODE_ID_SIGN_AUTH, kode_sent()->op,
        "the request waited behind the scan");
    uint8_t sig[8];
    (void)memset(sig, 0x5A, sizeof(sig));
    kode_answer(KODE_ST_OK, sig, (uint16_t)sizeof(sig));
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, r.status);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job(),
                                  "the scan was taken out of the worker");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    c5_case_end();
}

TEST_CASE("a send the link refuses is answered as refused when the worker reports it",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();
    fake_klog_reset();

    fake_radio_set_link(false);   /* down under the AO, whose drop it has not heard */
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    pump();
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);
    TEST_ASSERT_TRUE(logged("the request could not be sent"));

    /* And the service is free again: nothing is left waiting. */
    fake_radio_set_link(true);
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    pump();
    TEST_ASSERT_EQUAL_UINT8(KODE_ID_SIGN_AUTH, kode_sent()->op);
    c5_case_end();
}

TEST_CASE("an answer that beats the send's done event is not lost, and the next request of that "
          "service goes at once", "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();
    fake_klog_reset();

    wifi_forget_request("home");
    (void)radio_settle();
    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_FORGET, kode_sent()->op);
    const QEvt *const first = hold_svc_done();

    /* The C5 is quick: the vault's answer is read before the send's return.
     * Its handler asks for the names again - the same service. */
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(KODE_VAULT_LIST, kode_sent()->op,
        "the follow-up was refused because the last send had not been read yet");

    /* The follow-up is answered too; only then does the first one's return
     * come in, late. It changes nothing: nothing waits on it any more, and
     * no deadline is started for an answer that has come. */
    vault_has("home");
    release_done(first);
    tick(QF_SUPPORT_STEPS(4000U));
    TEST_ASSERT_FALSE_MESSAGE(logged("did not answer"),
                              "a deadline was started for an answer that had come");
    c5_case_end();
}

TEST_CASE("a send that never comes back is answered after its guard, and its late return answers nobody",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();

    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    (void)radio_settle();   /* queued, and the worker never gets to it */
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(RADIO_SVC_WORKER));

    (void)qf_support_tick(AO_Radio, QF_SUPPORT_STEPS(KODE_RADIO_SVC_SEND_GUARD_MS) + 4U);
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);

    /* The worker wakes up and does what it was asked: the result is nobody's. */
    drain();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "a second answer went to the asker");
    c5_case_end();
}

TEST_CASE("a send queued when the link goes is not answered twice", "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();

    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    (void)radio_settle();   /* queued */
    fake_radio_drop_link();
    (void)radio_settle();   /* the link is lost: the asker is told once */
    IdSignRspEvt r;
    spy_expect(ID_SIGN_RSP_SIG, &r, sizeof(r));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_REFUSED, r.status);

    /* The worker gets to it only now. A request for a link that is gone is
     * not sent - not even if the transport happens to read up again - and
     * its return answers nobody. */
    fake_radio_set_link(true);
    fake_peer_reset();
    drain();
    size_t sent = 99U;
    (void)fake_peer_last_sent(&sent);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sent, "a request for a lost link was sent");
    unsigned answers = 0U;
    while ( ao_spy_count() > 0U )
    {
        if ( (QSignal)ID_SIGN_RSP_SIG == ao_spy_take()->sig )
        {
            ++answers;
        }
        ao_spy_release();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, answers, "the late return was answered");
    c5_case_end();
}

TEST_CASE("test_radio_request_while_job_runs_is_deferred: a request that needs the channel is "
          "owed, answered or turned away while a job runs, and the AO goes on serving",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    unsigned const before = job_submits();
    ao_spy_drain();
    fake_klog_reset();

    dispatch(WIFI_STOP_REQ_SIG);          /* owed: paid when the scan is back */
    TEST_ASSERT_TRUE(logged("is owed"));
    dispatch(WIFI_SCAN_REQ_SIG);          /* answered on the spot: busy, ask again */
    TEST_ASSERT_TRUE(logged("was answered busy; ask again"));
    bool ok = true;
    uint8_t count = 9U;
    TEST_ASSERT_TRUE_MESSAGE(scan_end_seen(&ok, &count), "the second scan was not answered");
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_TRUE_MESSAGE(s_end_busy, "and the end says the C5 was busy, not that the scan failed");
    TEST_ASSERT_EQUAL_UINT8(0U, count);

    /* And the AO answers what it can: its state, with the job still inside. */
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());

    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, job_submits(), "a job went over the one inside");
    TEST_ASSERT_FALSE(job_queued());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_STOP, running_job(),
                                  "the owed stop was not paid when the scan came back");
    finish(KDRV_RADIO_JOB_WIFI_STOP, KODE_OK);
    c5_case_end();
}

TEST_CASE("test_radio_result_after_link_lost_is_dropped: the job that was inside when the link "
          "went answers nobody, and the teardown follows it", "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    seed_air(2U, -50);
    run_scan();
    pump();

    fake_radio_drop_link();
    pump();
    bool ok = true;
    uint8_t count = 0U;
    TEST_ASSERT_TRUE(scan_end_seen(&ok, &count));
    TEST_ASSERT_FALSE(ok);
    TEST_ASSERT_EQUAL_UINT8(2U, count);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());

    /* The scan is still inside the worker, using its storage: the teardown is
     * owed and holds the channel, and nothing is submitted over the scan. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_FALSE(job_queued());
    ao_spy_drain();

    /* It comes back, and says it worked: about a link that is gone. */
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    TEST_ASSERT_FALSE_MESSAGE(scan_end_seen(&ok, &count),
                              "a listing was closed twice");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_DISCONNECT, running_job(),
                                  "the owed teardown did not follow the job");
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    c5_case_end();
}

TEST_CASE("a done event that is not the job's is dropped and the job stays inside",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    ao_spy_drain();

    KqpWorkerDoneEvt stray;
    (void)memset(&stray, 0, sizeof(stray));
    stray.super  = (QEvt)QEVT_INITIALIZER(RADIO_JOB_SIG);
    stray.seq    = (uint16_t)(((KqpWorkerDoneEvt const *)(void const *)s_held)->seq + 7U);
    stray.status = (uint8_t)KODE_OK;
    dispatch_evt(&stray.super);
    pump();

    bool ok = true;
    uint8_t count = 0U;
    TEST_ASSERT_FALSE_MESSAGE(scan_end_seen(&ok, &count),
                              "a stranger's answer ended the scan");
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    c5_case_end();
}

TEST_CASE("a job that never comes back is answered as a timeout, and the worker stays busy until it does",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    fake_klog_reset();

    unsigned const submits = job_submits();

    tick(QF_SUPPORT_STEPS(KODE_RADIO_GUARD_MS) + 4U);
    TEST_ASSERT_TRUE(logged("did not answer job"));
    bool ok = true;
    uint8_t count = 9U;
    TEST_ASSERT_TRUE_MESSAGE(scan_end_seen(&ok, &count), "the listing was left open");
    TEST_ASSERT_FALSE(ok);

    /* The worker is still inside the scan: a new one is refused as busy. */
    fake_klog_reset();
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_TRUE(logged("still inside a job"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(submits, job_submits(), "a job went over the stuck one");

    /* The late return frees it, answering nobody, and the next scan goes. */
    ao_spy_drain();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    TEST_ASSERT_FALSE_MESSAGE(scan_end_seen(&ok, &count), "the late return was answered");
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    c5_case_end();
}

/* esp_hosted_deinit() destroys the semaphore a send inside esp-hosted's RPC
 * wait is sleeping on, and the send destroys it again when it wakes. The old
 * code could not overlap them (the AO sent from its own thread); two workers
 * can, so the teardown waits for the sends. */
TEST_CASE("a teardown after a lost link waits for the KODE send that is inside the C5 link",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();

    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    (void)radio_settle();   /* the send is with the service worker */
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(RADIO_SVC_WORKER));

    fake_radio_drop_link();
    (void)radio_settle();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown went while a send was inside the link");
    TEST_ASSERT_TRUE(logged("the teardown waits for a KODE request"));

    /* The send comes back (refused: its link is gone); only now does it go. */
    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown went before the send's done event was read");
    (void)radio_settle();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown was not released by the last send");
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    c5_case_end();
}

TEST_CASE("radio_down waits for the KODE send that is inside the C5 link, then tears down",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();

    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    (void)radio_settle();
    QEvt down = QEVT_INITIALIZER(RADIO_DOWN_REQ_SIG);
    QASM_DISPATCH(AO_Radio, &down, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown went while a send was inside the link");

    TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
    (void)radio_settle();
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    c5_case_end();
}

/**
 * @brief The link is lost with a scan inside the radio worker AND a KODE send
 *        inside the service worker: the two holds on the teardown cross.
 *
 * The teardown is owed twice over, and must go once, only when both are back,
 * whichever returns last.
 *
 * @param job_first true: the scan's done event is read first, then the
 *                  send's; false: the other way round.
 */
static void teardown_after_both(bool job_first)
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    (void)radio_settle();
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(RADIO_SVC_WORKER));
    unsigned const before = job_submits();

    fake_radio_drop_link();
    (void)radio_settle();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown went with a scan and a send inside");

    if ( job_first )
    {
        deliver_held(KODE_OK);                  /* the scan comes back first */
        (void)radio_settle();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
            "the teardown went with the send still inside");
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
            "the teardown went before the send's done event was read");
        (void)radio_settle();
    }
    else
    {
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
        (void)radio_settle();      /* the send is back first */
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
            "the teardown went with the scan still inside");
        deliver_held(KODE_OK);
        (void)radio_settle();
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown was not released by the last one back");
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, job_submits(),
                                   "more than one job followed the lost link");
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, job_submits(),
                                   "a second teardown was submitted");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    c5_case_end();
}

TEST_CASE("a lost link with a scan and a KODE send inside tears down once, after both, the scan back first",
          "[radio_ao][worker]")
{
    teardown_after_both(true);
}

TEST_CASE("a lost link with a scan and a KODE send inside tears down once, after both, the send back first",
          "[radio_ao][worker]")
{
    teardown_after_both(false);
}

/* ---- The teardown waits for the card's unmount ---------------------------- */

/* The unmount is a call on AO_Storage's worker and may sit behind another for
 * seconds. esp-hosted's teardown gives up the SDMMC host a mounted card is on,
 * and the C5 held in reset switches the card's supply off: neither may come
 * before AO_Storage has said the card let go. These cases withhold the answer
 * and then give it by hand. */

/** @brief AO_Storage's answer to the radio's unmount, given by hand. */
static void unmount_answered(void)
{
    static const QEvt rsp = QEVT_INITIALIZER(STORAGE_CMD_RSP_SIG);

    kdrv_storage_unmount(KDRV_MEDIUM_SD);
    QASM_DISPATCH(AO_Radio, &rsp, 0U);
    (void)radio_settle();
}

TEST_CASE("a lost link does not tear down before the card's unmount is answered",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    s_gate_auto = false;
    unsigned const before = job_submits();

    fake_radio_drop_link();
    (void)radio_settle();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "esp-hosted was torn down with the card's unmount unanswered");
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_TRUE(logged("the teardown waits for the card"));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());

    unmount_answered();
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    TEST_ASSERT_EQUAL_UINT(before + 1U, job_submits());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, job_submits(),
                                   "a second teardown was submitted");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    c5_case_end();
}

TEST_CASE("radio_down holds the C5 only after the card's unmount is answered",
          "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    s_gate_auto = false;

    QEvt down = QEVT_INITIALIZER(RADIO_DOWN_REQ_SIG);
    QASM_DISPATCH(AO_Radio, &down, 0U);
    (void)radio_settle();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown went with the card's unmount unanswered");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_STOPPING, link_now(),
        "the C5 is held in reset only when the teardown has run");

    unmount_answered();
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    c5_case_end();
}

TEST_CASE("a card that stays mounted does not stop the link coming down, "
          "after a window of asking", "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    s_gate_auto = false;
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_SD));
    storage_drain();

    QEvt down = QEVT_INITIALIZER(RADIO_DOWN_REQ_SIG);
    QASM_DISPATCH(AO_Radio, &down, 0U);
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();                   /* refused: the card is mounted still */
    (void)pull_jobs();

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "the link came down while the card was still being asked");
    refuse_again((UNMOUNT_WAIT_MS / UNMOUNT_RETRY_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "the link came down before the window was spent");
    tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();
    (void)pull_jobs();

    TEST_ASSERT_TRUE(logged("the SD card stayed busy for 10 s; the link comes "
                            "down anyway"));
    TEST_ASSERT_TRUE(logged("the card is still mounted; the link comes down"));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    c5_case_end();
}

TEST_CASE("a refused unmount before a teardown is asked again, and the link "
          "comes down on the first that is honoured", "[radio_ao][worker]")
{
    radio_fresh();
    go_up();
    s_gate_auto = false;
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_SD));
    storage_drain();

    QEvt down = QEVT_INITIALIZER(RADIO_DOWN_REQ_SIG);
    QASM_DISPATCH(AO_Radio, &down, 0U);
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();
    refuse_again(1U);

    tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_answered();
    (void)pull_jobs();

    TEST_ASSERT_FALSE(logged("stayed busy"));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());
    c5_case_end();
}

/**
 * @brief The link is lost with a KODE send inside the service worker AND the
 *        card's unmount unanswered: the teardown is owed twice, goes once, and
 *        only when both are back.
 *
 * @param answer_first true: the card answers, then the send returns; false:
 *                     the other way round.
 */
static void teardown_after_card_and_send(bool answer_first)
{
    radio_fresh();
    go_up();
    vault_has("home");
    ao_spy_drain();
    s_gate_auto = false;

    TEST_ASSERT_TRUE(id_sign_request(ao_spy(), "abc"));
    (void)radio_settle();
    TEST_ASSERT_EQUAL_UINT(1U, fake_khal_worker_pending_for(RADIO_SVC_WORKER));
    unsigned const before = job_submits();

    fake_radio_drop_link();
    (void)radio_settle();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown went with a send and the card's unmount outstanding");

    if ( answer_first )
    {
        unmount_answered();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
            "the teardown went with the send still inside");
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
        (void)radio_settle();
    }
    else
    {
        TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(RADIO_SVC_WORKER));
        (void)radio_settle();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
            "the teardown went with the card's unmount unanswered");
        unmount_answered();
    }

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_khal_worker_pending_for(RADIO_WORKER),
        "the teardown was not released by the last one back");
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, job_submits(),
                                   "more than one job followed the lost link");
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, job_submits(),
                                   "a second teardown was submitted");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    c5_case_end();
}

TEST_CASE("a lost link tears down once, after the card's answer and the send, the card first",
          "[radio_ao][worker]")
{
    teardown_after_card_and_send(true);
}

TEST_CASE("a lost link tears down once, after the card's answer and the send, the send first",
          "[radio_ao][worker]")
{
    teardown_after_card_and_send(false);
}

TEST_CASE("a C5 update opens its session only after the card's unmount is answered",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    recv_spy();
    s_gate_auto = false;

    inst_deliver();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "the session opened with the card's unmount unanswered");
    TEST_ASSERT_FALSE(job_queued());

    unmount_answered();
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_BEGIN, running_job());
    s_gate_auto = true;
    inst_push_to_end();
}

TEST_CASE("a C5 update is refused while the card is still mounted, after a "
          "window of asking", "[radio_ao]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();
    s_gate_auto = false;
    storage_drain();

    inst_deliver();
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_SD));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();
    refuse_again((UNMOUNT_WAIT_MS / UNMOUNT_RETRY_MS) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "the C5 was opened over a mounted card");
    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fail, "the update gave up early");

    tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();
    (void)pull_jobs();

    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "the C5 was opened over a mounted card");
    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT(1U, fail);
    TEST_ASSERT_TRUE(logged("the SD card stayed busy for 10 s; C5 update not "
                            "started"));
    TEST_ASSERT_TRUE(logged("the card is still mounted; the C5 was not touched"));
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("a C5 update waits out a refused unmount and opens its session when "
          "the card lets go", "[radio_ao]")
{
    radio_fresh();
    go_up();
    recv_spy();
    s_gate_auto = false;
    storage_drain();

    inst_deliver();
    TEST_ASSERT_NOT_NULL(kdrv_storage_mount(KDRV_MEDIUM_SD));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_refused();
    refuse_again(3U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    tick(QF_SUPPORT_STEPS(UNMOUNT_RETRY_MS));
    TEST_ASSERT_EQUAL_INT((int)STORAGE_OP_UNMOUNT, next_storage_op());
    unmount_answered();
    (void)pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_BEGIN, running_job());
    TEST_ASSERT_FALSE(logged("stayed busy"));
    s_gate_auto = true;
    inst_push_to_end();
}

/* The C5's update has no rollback, so a link that drops under it is the case
 * to keep honest: the install fails once, the teardown is owed and goes once,
 * after the job that was inside has come back, and nothing else is sent to a
 * C5 that is gone. */
TEST_CASE("a link lost under a C5 update fails the install once and tears down "
          "once, after the job inside has come back", "[radio_ao][worker]")
{
    unsigned ready, ack, done, fail, stall;

    radio_fresh();
    go_up();
    recv_spy();

    inst_deliver();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_BEGIN, running_job());
    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_WRITE, running_job());
    unsigned const before = job_submits();

    fake_radio_lose_link(KODE_ERROR);
    (void)radio_settle();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_khal_worker_pending_for(RADIO_WORKER),
        "a teardown went to the worker while a job was still inside it");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_C5_WRITE, running_job(),
        "the job inside is still the worker's");
    TEST_ASSERT_TRUE(logged("the link dropped mid-transfer"));

    deliver_held(KODE_ERROR);          /* the write comes back, failed */
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, job_submits(),
        "more than the one teardown followed the lost link");
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);

    inst_answers(&ready, &ack, &done, &fail, &stall);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fail, "the install failed exactly once");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, done, "a lost update must not say ok");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, job_submits(),
                                   "a second teardown was submitted");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_FAILED, link_now());
    c5_case_end();
}

/* ---- Across a jump: the park before it, the adoption after it ---------------
 * AO_Boot asks with RADIO_PARK_REQ_SIG and is answered with exactly one
 * BOOT_RADIO_PARKED_SIG, whatever the state. The note is left only when the
 * C5 itself said OK.
 */

/** @brief AO_Boot asks the radio to park for a jump to @p target. */
static void park_ask(kdrv_radio_note_t target)
{
    RadioParkEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super.sig = (QSignal)RADIO_PARK_REQ_SIG;
    e.target    = (uint8_t)target;
    /* Dispatched and not pumped: a pump's tick would take AO_Boot's mail
     * away, as it does for any background AO. */
    dispatch_evt(&e.super);
}

/** @brief The C5 answers the INFO the link-up asked, so the service is free. */
static void info_answered(void)
{
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_sent_back(1U);
    uint8_t m[sizeof(kode_hdr_t)];

    TEST_ASSERT_EQUAL_UINT8(KODE_SELF_INFO, req->op);
    kode_hdr_t const h = { KODE_WIRE_VERSION, KODE_SVC_SELF, KODE_SELF_INFO,
                           req->seq, KODE_ST_REFUSED, 0U, 0U };
    (void)memcpy(m, &h, sizeof(h));
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(m));
    pump();
}

/** @brief The link up, INFO answered, nothing asked of the C5 any more. */
static void park_ready(void)
{
    radio_fresh();
    fake_peer_reset();
    go_up();
    info_answered();
}

/** @brief The request the C5 now holds is the park. */
static void assert_park_sent(void)
{
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_SELF, kode_sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_SELF_PARK, kode_sent()->op);
}

/** @brief The park's amplifier step: IO4 is driven low, then the C5 is asked. */
static void park_muted(void)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the park raised the amplifier");
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
}

/**
 * @brief End a case whose park took the amplifier low.
 *
 * The AO is not built again between cases, and a wish for the pin low would
 * cost the next case's link-up a job of its own.
 */
static void park_case_end(void)
{
    dispatch(C5_RUN_REQ_SIG);
    c5_case_end();
}

/** @brief Put the settings and the AO back as a boot with nothing wanted. */
static void boot_wanting_nothing(void)
{
    uint8_t const off = 0U;

    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_WIFI, &off, sizeof(off));
    radio_reboot();
    c5_case_end();
}

/** @brief The restart after a park that had BLE up: a boot with BLE off too,
 *         so no advert is chased across the reboot. */
static void boot_wanting_no_ble(void)
{
    uint8_t const off = 0U;

    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_BLE, &off, sizeof(off));
    boot_wanting_nothing();
    fake_nvs_reset();
}

/** @brief IDF's STA netif registers this at creation; esp_restart() runs it. */
#define STOP_HANDLER  ((shutdown_handler_t)esp_wifi_stop)

/** @brief Whether esp_restart() would still run esp_wifi_stop (and now would
 *         not: the check takes the handler off). */
static bool stop_handler_still_there(void)
{
    return esp_unregister_shutdown_handler(STOP_HANDLER) == ESP_OK;
}

TEST_CASE("a park with the link up hangs up BLE, takes the amplifier low, then "
          "asks the C5 to park", "[radio_ao][park]")
{
    park_ready();
    (void)kdrv_ble_advertise(true);   /* BLE up and on the air */

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_QUIET, running_job());
    TEST_ASSERT_EQUAL_UINT(0U, boot_parked_taken());

    finish(KDRV_RADIO_JOB_BLE_QUIET, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(0U, boot_parked_taken());
    park_muted();
    assert_park_sent();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
                                  "the advert came back between the two steps");

    kode_answer(KODE_ST_OK, NULL, 0U);
    boot_wanting_nothing();
}

TEST_CASE("a park the C5 accepts leaves the note and answers AO_Boot",
          "[radio_ao][park]")
{
    park_ready();

    park_ask(KDRV_RADIO_NOTE_APP);
    park_muted();
    assert_park_sent();
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(0U, boot_parked_taken());

    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KDRV_RADIO_NOTE_APP, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    TEST_ASSERT_TRUE(logged("C5 parked"));
    boot_wanting_nothing();
}

TEST_CASE("the park's target travels: a jump home names kodeOS",
          "[radio_ao][park]")
{
    park_ready();

    park_ask(KDRV_RADIO_NOTE_OS);
    park_muted();
    assert_park_sent();
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KDRV_RADIO_NOTE_OS, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    boot_wanting_nothing();
}

TEST_CASE("a second park request after a park is answered and asks nothing",
          "[radio_ao][park]")
{
    park_ready();

    park_ask(KDRV_RADIO_NOTE_APP);
    park_muted();
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());

    park_ask(KDRV_RADIO_NOTE_OS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, boot_parked_taken(),
        "AO_Boot waits for an answer and must get one");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)KDRV_RADIO_NOTE_APP,
        fake_radio_note_last(), "the first note stands");
    boot_wanting_nothing();
}

TEST_CASE("a park the C5 refuses leaves no note and still answers",
          "[radio_ao][park]")
{
    park_ready();

    /* Refused: no station to keep. */
    park_ask(KDRV_RADIO_NOTE_APP);
    park_muted();
    assert_park_sent();
    kode_answer(KODE_ST_REFUSED, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());

    /* An image older than the op answers BAD_REQUEST. The pin is low
     * already, so this park asks the C5 at once. */
    park_ask(KDRV_RADIO_NOTE_OS);
    assert_park_sent();
    kode_answer(KODE_ST_BAD_REQUEST, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    TEST_ASSERT_TRUE(logged("C5 was not parked"));
    park_case_end();
}

TEST_CASE("a park whose send fails in the service worker answers AO_Boot at once "
          "with no note", "[radio_ao][park]")
{
    park_ready();
    fake_klog_reset();

    fake_radio_set_link(false);   /* down under the AO, whose drop it has not heard */
    park_ask(KDRV_RADIO_NOTE_APP);
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, boot_parked_taken(),
        "AO_Boot was left waiting for a park that could not be sent");
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    TEST_ASSERT_TRUE(logged("C5 was not parked"));
    park_case_end();
}

TEST_CASE("a park the C5 accepts keeps the station through esp_restart()",
          "[radio_ao][park]")
{
    park_ready();
    (void)esp_unregister_shutdown_handler(STOP_HANDLER);
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_register_shutdown_handler(STOP_HANDLER));

    park_ask(KDRV_RADIO_NOTE_OS);
    park_muted();
    assert_park_sent();
    TEST_ASSERT_TRUE_MESSAGE(stop_handler_still_there(),
        "the handler went before the C5 answered");
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_register_shutdown_handler(STOP_HANDLER));

    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_FALSE_MESSAGE(stop_handler_still_there(),
        "esp_restart() would stop the Wi-Fi the C5 now keeps");
    boot_wanting_nothing();
}

TEST_CASE("a park the C5 refuses leaves esp_restart() as it was",
          "[radio_ao][park]")
{
    park_ready();
    (void)esp_unregister_shutdown_handler(STOP_HANDLER);
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_register_shutdown_handler(STOP_HANDLER));

    park_ask(KDRV_RADIO_NOTE_OS);
    park_muted();
    kode_answer(KODE_ST_REFUSED, NULL, 0U);
    park_ask(KDRV_RADIO_NOTE_OS);
    kode_answer(KODE_ST_BAD_REQUEST, NULL, 0U);

    TEST_ASSERT_TRUE_MESSAGE(stop_handler_still_there(),
        "a C5 that was not parked keeps nothing, so the restart stops it");
    park_case_end();
}

TEST_CASE("after a park the C5 accepted nothing new is asked of the worker",
          "[radio_ao][park]")
{
    park_ready();
    wifi_station_up();
    dispatch(BLE_START_REQ_SIG);
    (void)kdrv_ble_start();
    finish(KDRV_RADIO_JOB_BLE_START, KODE_OK);
    (void)kdrv_ble_advertise(true);
    finish(KDRV_RADIO_JOB_BLE_ADV, KODE_OK);

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_QUIET, running_job());
    (void)kdrv_ble_quiet();
    finish(KDRV_RADIO_JOB_BLE_QUIET, KODE_OK);
    park_muted();
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());

    /* The central leaves: the invariant that keeps the board on the air
     * would put the advert back, and the C5 is parked. */
    dispatch(BLE_LINK_SIG);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "the advert was re-armed on a parked C5");

    /* A power-save wish that arrives now is a debt nobody can pay. */
    wifi_fast_request(true);
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
        "a job was submitted to a parked C5");

    /* The parked C5 resets its SDIO slave and the link drops: that is not a
     * park that failed, and AO_Boot has its answer already. */
    fake_klog_reset();
    fake_radio_drop_link();
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, boot_parked_taken(),
        "AO_Boot was answered a second time");
    TEST_ASSERT_FALSE_MESSAGE(logged("C5 was not parked"),
        "a parked C5 whose link dropped was reported as not parked");

    wifi_station_gone();
    fake_nvs_reset();
    boot_wanting_no_ble();
}

TEST_CASE("after a park the C5 refused the worker is still served",
          "[radio_ao][park]")
{
    bool on = false;

    park_ready();
    wifi_station_up();

    park_ask(KDRV_RADIO_NOTE_APP);
    park_muted();
    kode_answer(KODE_ST_REFUSED, NULL, 0U);

    wifi_fast_request(true);
    pump();
    TEST_ASSERT_TRUE_MESSAGE(ps_running(&on),
        "a refused park left the worker shut");
    TEST_ASSERT_FALSE(on);
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);
    wifi_station_gone();
    fake_nvs_reset();
    park_case_end();
}

TEST_CASE("the phone that was hung up for a park is not advertised to again",
          "[radio_ao][park]")
{
    park_ready();
    dispatch(BLE_START_REQ_SIG);               /* BLE wanted, and started */
    (void)kdrv_ble_start();
    finish(KDRV_RADIO_JOB_BLE_START, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    (void)kdrv_ble_advertise(true);
    finish(KDRV_RADIO_JOB_BLE_ADV, KODE_OK);

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_QUIET, running_job());
    (void)kdrv_ble_quiet();                    /* the worker's effect */
    finish(KDRV_RADIO_JOB_BLE_QUIET, KODE_OK);
    park_muted();
    assert_park_sent();

    /* The central leaves; the invariant that keeps the board on the air
     * would put the advert back, and the jump is already on its way. */
    dispatch(BLE_LINK_SIG);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
                                  "the advert came back during the park");

    kode_answer(KODE_ST_OK, NULL, 0U);
    boot_wanting_no_ble();
}

TEST_CASE("a park with the worker busy skips BLE and the amplifier and still "
          "asks the C5",
          "[radio_ao][park]")
{
    park_ready();
    (void)kdrv_ble_advertise(true);
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    unsigned const submits = job_submits();

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(submits, job_submits(),
                                   "a job was queued behind the scan");
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    assert_park_sent();

    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KDRV_RADIO_NOTE_APP, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    boot_wanting_nothing();
}

TEST_CASE("a park while INFO is unanswered answers at once with no note",
          "[radio_ao][park]")
{
    radio_fresh();
    fake_peer_reset();
    go_up();   /* INFO is in flight on the SELF service */

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    /* Delivered and not pumped: the refused send answers AO_Boot in this
     * step, and a pump's tick would take that mail away. */
    fake_radio_effects(KDRV_RADIO_JOB_AMP, KODE_OK);
    deliver_held(KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    park_case_end();
}

TEST_CASE("a park with the link down answers at once with no note",
          "[radio_ao][park]")
{
    radio_fresh();
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    park_case_end();
}

TEST_CASE("a park whose C5 never answers is not waited for",
          "[radio_ao][park]")
{
    unsigned answers = 0U;

    park_ready();
    park_ask(KDRV_RADIO_NOTE_APP);
    park_muted();
    assert_park_sent();

    /* Up to the service deadline, then a step at a time: the answer is
     * counted the step it is sent, before the next tick takes it away. */
    tick(QF_SUPPORT_STEPS(3000U) - 2U);
    answers += boot_parked_taken();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, answers, "answered before the deadline");
    for ( unsigned i = 0U; 6U > i; ++i )
    {
        tick(1U);
        answers += boot_parked_taken();
    }
    TEST_ASSERT_EQUAL_UINT(1U, answers);
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());

    /* A late answer finds nobody waiting. */
    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, boot_parked_taken());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    park_case_end();
}

TEST_CASE("a link lost while parking still answers once", "[radio_ao][park]")
{
    park_ready();
    (void)kdrv_ble_advertise(true);

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_QUIET, running_job());
    fake_radio_drop_link();
    pump();
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    park_case_end();
}

TEST_CASE("a park whose amplifier step fails still asks the C5",
          "[radio_ao][park]")
{
    park_ready();
    fake_klog_reset();

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    finish(KDRV_RADIO_JOB_AMP, KODE_ERROR);
    TEST_ASSERT_TRUE(logged("would not drive the amplifier"));
    assert_park_sent();

    kode_answer(KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KDRV_RADIO_NOTE_APP, fake_radio_note_last());
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    boot_wanting_nothing();
}

TEST_CASE("a link lost while the amplifier goes low still answers once",
          "[radio_ao][park]")
{
    park_ready();

    park_ask(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    fake_radio_drop_link();
    pump();
    TEST_ASSERT_EQUAL_UINT(1U, boot_parked_taken());
    TEST_ASSERT_EQUAL_UINT32(0U, fake_radio_note_last());
    park_case_end();
}

/** @brief Boot with Wi-Fi wanted, the link coming up. */
static void boot_wanting_wifi(void)
{
    uint8_t const on = 1U;

    radio_fresh();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_WIFI, &on, sizeof(on));
    radio_reboot();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
}

TEST_CASE("CONNECT carries the boot's wish for Wi-Fi", "[radio_ao][park]")
{
    boot_wanting_wifi();
    TEST_ASSERT_TRUE(held_req()->wants_wifi);
    c5_case_end();   /* the worker is inside one job at a time */

    uint8_t const off = 0U;
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_WIFI, &off, sizeof(off));
    radio_reboot();
    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    TEST_ASSERT_FALSE(held_req()->wants_wifi);
    boot_wanting_nothing();
}

TEST_CASE("a boot that kept the C5 adopts instead of scanning",
          "[radio_ao][park]")
{
    boot_wanting_wifi();
    fake_radio_set_adopted(true);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);

    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_ADOPT, running_job(),
                                  "the kept station was not taken over first");

    /* It worked: BLE is next if it is wanted, and no scan ever is. */
    finish(KDRV_RADIO_JOB_WIFI_ADOPT, KODE_OK);
    TEST_ASSERT_TRUE(logged("Wi-Fi adopted from the parked C5"));
    for ( unsigned guard = 0U; (8U > guard) && job_held(); ++guard )
    {
        TEST_ASSERT_NOT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
        TEST_ASSERT_NOT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_ADOPT, running_job());
        finish_whatever_runs();
    }
    boot_wanting_nothing();
}

TEST_CASE("a boot that did not keep the C5 does not adopt", "[radio_ao][park]")
{
    boot_wanting_wifi();
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);

    TEST_ASSERT_TRUE_MESSAGE(drive_until(KDRV_RADIO_JOB_WIFI_SCAN),
                             "the ordinary boot no longer scans");
    TEST_ASSERT_FALSE(logged("adopt"));
    boot_wanting_nothing();
}

TEST_CASE("a failed adoption falls back to the autojoin round",
          "[radio_ao][park]")
{
    boot_wanting_wifi();
    fake_radio_set_adopted(true);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_ADOPT, running_job());

    finish(KDRV_RADIO_JOB_WIFI_ADOPT, KODE_TIMEOUT);
    TEST_ASSERT_TRUE(logged("adoption failed"));
    /* BLE keeps its place in the chain; the round follows it. */
    TEST_ASSERT_TRUE_MESSAGE(drive_until(KDRV_RADIO_JOB_WIFI_SCAN),
                             "the failed adoption left Wi-Fi unjoined");
    boot_wanting_nothing();
}

TEST_CASE("an adopted station gets the power save this side believes in",
          "[radio_ao][park]")
{
    bool on = false;

    boot_wanting_wifi();
    wifi_station_up();               /* the stack the adoption brought up */
    fake_radio_set_adopted(true);
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_ADOPT, running_job());
    finish(KDRV_RADIO_JOB_WIFI_ADOPT, KODE_OK);

    /* The C5 may hold the mode the last firmware left it in: the default
     * this side believes in is applied the way a join applies it. */
    TEST_ASSERT_TRUE_MESSAGE(drive_until(KDRV_RADIO_JOB_WIFI_PS),
        "no power-save job followed the adoption");
    TEST_ASSERT_TRUE(ps_running(&on));
    TEST_ASSERT_TRUE_MESSAGE(on, "the default is modem save");
    finish(KDRV_RADIO_JOB_WIFI_PS, KODE_OK);

    wifi_station_gone();
    fake_nvs_reset();
    boot_wanting_nothing();
}

/* ---- What a scan entry carries past the name ------------------------------ */

/** @brief Ask for a BLE scan the way an app does, and let the AO take it. */
static void ask_ble_scan(void)
{
    ble_scan_request(0U);
    pump();
}

/** @brief The next published BLE_SCAN_ENTRY_SIG, or a failed case. */
static const BleScanEvt *next_ble_entry(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "fewer BLE entries than seeded");
        if ( (QSignal)BLE_SCAN_ENTRY_SIG == e->sig )
        {
            return (const BleScanEvt *)e;
        }
    }
}

TEST_CASE("a BLE scan entry carries the advert's fields", "[radio_ao]")
{
    const BleScanEvt *d;

    radio_fresh();
    go_up();
    ao_spy_subscribe(BLE_SCAN_ENTRY_SIG);
    ao_spy_drain();

    ask_ble_scan();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    fake_ble_seed_air(2U, -50);
    finish(KDRV_RADIO_JOB_BLE_SCAN, kdrv_ble_scan(held_req()->len));

    d = next_ble_entry();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_ADDR_RANDOM, d->addr_type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)FAKE_BLE_MFG_LEN, d->mfg_len);
    TEST_ASSERT_EQUAL_HEX8(FAKE_BLE_MFG_ID_LO, d->mfg[0]);
    TEST_ASSERT_EQUAL_HEX8(FAKE_BLE_MFG_ID_HI, d->mfg[1]);
    TEST_ASSERT_EQUAL_HEX8(FAKE_BLE_MFG_TYPE, d->mfg[2]);
    TEST_ASSERT_EQUAL_UINT8(1U, d->svc_n);
    TEST_ASSERT_EQUAL_HEX16(FAKE_BLE_SVC16, d->svc16[0]);

    d = next_ble_entry();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_BLE_ADDR_PUBLIC, d->addr_type);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, d->mfg_len, "a bare advert says nothing more");
    TEST_ASSERT_EQUAL_UINT8(0U, d->svc_n);

    QActive_unsubscribe(ao_spy(), BLE_SCAN_ENTRY_SIG);
    ao_spy_drain();
}

TEST_CASE("a Wi-Fi scan entry carries its BSSID", "[radio_ao]")
{
    static const uint8_t BSSID[6] = { 0x3cU, 0x84U, 0x6aU, 0x0aU, 0x0bU, 0x0cU };
    const QEvt *e;

    radio_fresh();
    go_up();
    ao_spy_drain();

    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    seed_air(1U, -50);
    (void)memcpy(s_recs[0].bssid, BSSID, sizeof(BSSID));
    run_scan();
    pump();

    for ( ;; )
    {
        e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "no entry was published");
        if ( (QSignal)WIFI_SCAN_ENTRY_SIG == e->sig )
        {
            break;
        }
    }
    TEST_ASSERT_EQUAL_HEX8_ARRAY(BSSID, ((const WifiScanEntryEvt *)e)->bssid,
                                 sizeof(BSSID));
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    ao_spy_drain();
}

/* ---- How long a BLE scan listens ------------------------------------------ */

/** @brief Whether the next BLE_SCAN_END_SIG on the bus says ok, and that
 *         there is exactly one. */
static bool ble_scan_end_ok(void)
{
    bool     ok    = false;
    unsigned found = 0U;

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            break;
        }
        if ( (QSignal)BLE_SCAN_END_SIG == e->sig )
        {
            ok = ((const WifiScanEndEvt *)e)->ok;
            ++found;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, found, "one end marker, no more");
    return ok;
}

TEST_CASE("a BLE scan listens as long as it is asked", "[radio_ao]")
{
    radio_fresh();
    go_up();

    ble_scan_request(1500U);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT32(1500U, held_req()->len);
    finish(KDRV_RADIO_JOB_BLE_SCAN, kdrv_ble_scan(held_req()->len));
    TEST_ASSERT_TRUE(ble_scan_end_ok());

    ble_scan_request(0U);
    pump();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KODE_BLE_SCAN_MS, held_req()->len,
                                     "0 is the default");
    finish(KDRV_RADIO_JOB_BLE_SCAN, kdrv_ble_scan(held_req()->len));
    ao_spy_drain();
}

TEST_CASE("a BLE scan length out of range is refused", "[radio_ao]")
{
    static const uint32_t BAD[2] = { KODE_BLE_SCAN_MIN_MS - 1U,
                                     KODE_BLE_SCAN_MAX_MS + 1U };

    radio_fresh();
    go_up();

    for ( unsigned i = 0U; 2U > i; ++i )
    {
        unsigned const scans = fake_ble_scans();
        ble_scan_request(BAD[i]);
        pump();
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "no job for it");
        TEST_ASSERT_EQUAL_UINT(scans, fake_ble_scans());
        TEST_ASSERT_FALSE(ble_scan_end_ok());
    }

    /* The ends of the range are in it. */
    ble_scan_request(KODE_BLE_SCAN_MIN_MS);
    pump();
    TEST_ASSERT_EQUAL_UINT32(KODE_BLE_SCAN_MIN_MS, held_req()->len);
    finish(KDRV_RADIO_JOB_BLE_SCAN, kdrv_ble_scan(held_req()->len));
    ble_scan_request(KODE_BLE_SCAN_MAX_MS);
    pump();
    TEST_ASSERT_EQUAL_UINT32(KODE_BLE_SCAN_MAX_MS, held_req()->len);
    finish(KDRV_RADIO_JOB_BLE_SCAN, kdrv_ble_scan(held_req()->len));
    ao_spy_drain();
}

/* ---- The amplifier's enable (radio_amp_request()) ---------------------------
 * The NS4150B's enable is the C5's IO4, raised by the C5's own firmware. Off
 * is asked by the audio park of an app with no sound; on, by AO_Audio. Each is
 * a job over the link, and only when the pin is not already where it is
 * wanted. */

/** @brief Leave the amplifier wanted and the link off, for the cases after these. */
static void amp_case_end(void)
{
    radio_amp_request(true);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    settle_to_off();
    storage_drain();
    ao_spy_drain();
}

TEST_CASE("with the link up, the amplifier asked off is a job, and on again "
          "another", "[radio_ao]")
{
    radio_fresh();
    go_up();
    while ( job_held() )
    {
        finish_whatever_runs();
    }

    dispatch(C5_AMP_OFF_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);

    dispatch(C5_AMP_OFF_SIG);
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "a pin already low was driven again");

    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    TEST_ASSERT_TRUE(held_req()->on);
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
    amp_case_end();
}

TEST_CASE("with no link, the amplifier asked off waits for the link-up",
          "[radio_ao]")
{
    radio_fresh();
    dispatch(C5_AMP_OFF_SIG);
    TEST_ASSERT_NOT_EQUAL_MESSAGE((int)KDRV_RADIO_JOB_AMP, running_job(),
                                  "a pin was driven with no link to drive it over");

    go_up();
    for ( unsigned guard = 0U; (8U > guard) && job_held()
                              && (running_job() != (int)KDRV_RADIO_JOB_AMP); ++guard )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
    amp_case_end();
}

TEST_CASE("a C5 the bring-up reset has the amplifier on: asking for it costs no job",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_FALSE(job_held());
    amp_case_end();
}

/* AO_Audio opens the codec on C5_RUNNING_SIG. A C5 the bring-up reset has the
 * pin high and is answered at once; one the last firmware parked may have been
 * left with it low, and the answer waits for the job that raises it. */
TEST_CASE("the amplifier asked on with a C5 the bring-up reset is answered at once",
          "[radio_ao]")
{
    radio_fresh();
    go_up();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
    ao_spy_subscribe(C5_RUNNING_SIG);

    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_FALSE(job_held());
    TEST_ASSERT_EQUAL_UINT(1U, running_said());
    amp_case_end();
}

TEST_CASE("an amplifier asked on while its pin is low is answered once the job "
          "has raised it", "[radio_ao]")
{
    radio_fresh();
    go_up();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    /* Left low, as an app with no sound leaves it for the next firmware. */
    dispatch(C5_AMP_OFF_SIG);
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
    ao_spy_drain();
    ao_spy_subscribe(C5_RUNNING_SIG);

    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    TEST_ASSERT_TRUE(held_req()->on);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, running_said(), "the open went ahead of the pin");
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, running_said());
    amp_case_end();
}

/* Every way out of the hope of a link pays an owed "on" at once, so AO_Audio
 * never waits past it, and the next "on" with no link coming is answered in
 * the step. */

/** @brief Up, with the pin left low and known so: an "on" is then owed a job. */
static void amp_up_low(void)
{
    radio_fresh();
    go_up();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    dispatch(C5_AMP_OFF_SIG);
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
    ao_spy_drain();
}

TEST_CASE("an amplifier owed during a C5 update is answered when the update ends "
          "the link, and the next one at once", "[radio_ao]")
{
    amp_up_low();
    recv_spy();
    inst_deliver();
    ao_spy_subscribe(C5_RUNNING_SIG);
    dispatch(C5_RUN_REQ_SIG);                 /* the install holds the channel */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, running_said(), "answered before the pin could be driven");

    inst_push_to_end();                       /* activated: the link ends, Radio_off */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, running_said(), "the update ended and the sound still waits");

    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, running_said(), "a sound with no link coming waited");
    TEST_ASSERT_FALSE(job_held());
    amp_case_end();
}

TEST_CASE("an amplifier owed while the link comes up is answered when the bring-up "
          "fails", "[radio_ao]")
{
    amp_up_low();
    dispatch(RADIO_DOWN_REQ_SIG);
    settle_to_off();
    ao_spy_drain();
    ao_spy_subscribe(C5_RUNNING_SIG);

    dispatch(RADIO_UP_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());
    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, running_said(), "answered before the link that could drive the pin");
    finish(KDRV_RADIO_JOB_CONNECT, KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT(1U, running_said());
    amp_case_end();
}

TEST_CASE("an amplifier owed while radio_down ends the link is answered when the "
          "link is off", "[radio_ao]")
{
    amp_up_low();
    recv_spy();
    /* An install that is receiving holds the channel and runs no job, so
     * radio_down is taken while the "on" still waits for its pin. */
    make_image(0x0017U, "kode_c5", "9.9.9");
    inst_request((uint32_t)sizeof(s_img), image_crc(sizeof(s_img)));
    TEST_ASSERT_TRUE(log_got(CONSOLE_RAW_REQ_SIG));
    inst_armed(true);
    ao_spy_drain();
    ao_spy_subscribe(C5_RUNNING_SIG);
    dispatch(C5_RUN_REQ_SIG);
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "the pin was driven under the install");
    TEST_ASSERT_EQUAL_UINT(0U, running_said());

    dispatch(RADIO_DOWN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, running_said(), "radio_down ended the link and the sound still waits");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());

    /* The receive outlives the link; the host goes quiet, and it stalls. */
    for ( unsigned t = 0U; ((KODE_RECV_IDLE_MS / 10U) + 5U) > t; ++t )
    {
        pump();
    }
    (void)log_got(CONSOLE_RAW_END_SIG);
    ao_spy_drain();
    amp_case_end();
}

TEST_CASE("an amplifier owed during a C5 update that fails is answered by the "
          "job the settle submits once the link is free", "[radio_ao]")
{
    amp_up_low();
    recv_spy();
    inst_deliver();
    ao_spy_subscribe(C5_RUNNING_SIG);
    dispatch(C5_RUN_REQ_SIG);                 /* the update holds the channel */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_BEGIN, running_job());
    finish(KDRV_RADIO_JOB_C5_BEGIN, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_WRITE, running_job());
    finish(KDRV_RADIO_JOB_C5_WRITE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_FINISH, running_job());
    finish(KDRV_RADIO_JOB_C5_FINISH, KODE_ERROR);   /* refused: the link stays up */
    for ( unsigned guard = 0U; (8U > guard) && job_held()
                              && (running_job() != (int)KDRV_RADIO_JOB_AMP); ++guard )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    TEST_ASSERT_TRUE(held_req()->on);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, running_said(), "answered before the pin was raised");
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, running_said());
    amp_case_end();
}

TEST_CASE("an amplifier owed during the c5 row is answered by the job the settle "
          "submits once the row has ended", "[radio_ao]")
{
    amp_up_low();
    ao_spy_subscribe(C5_RUNNING_SIG);
    ask_row();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    dispatch(C5_RUN_REQ_SIG);                 /* the row holds the channel */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    finish_info();
    finish_rest();
    for ( unsigned guard = 0U; (8U > guard) && job_held()
                              && (running_job() != (int)KDRV_RADIO_JOB_AMP); ++guard )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_AMP, running_job());
    TEST_ASSERT_TRUE(held_req()->on);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, running_said(), "answered before the pin was raised");
    finish(KDRV_RADIO_JOB_AMP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, running_said());
    amp_case_end();
}

/* ---- The app's own service ------------------------------------------------ */

/** @brief The spy's next event of @p sig, skipping the others; NULL when none waits. */
static const QEvt *spy_take_sig(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    while ( (NULL != e) && (e->sig != sig) )
    {
        e = ao_spy_take();
    }
    return e;
}

/** @brief The app's service the cases declare: one notified, one written. */
static const kdrv_ble_app_char_t s_app_chars[] = {
    { "temp", "6e400002-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_READ | KDRV_BLE_APP_NOTIFY },
    { "cmd",  "6e400003-b5a3-f393-e0a9-e50e24dcca9e", KDRV_BLE_APP_WRITE },
};

/** @brief The service of the two. */
static const kdrv_ble_app_service_t s_app_svc = {
    "6e400001-b5a3-f393-e0a9-e50e24dcca9e", s_app_chars, 2U };

TEST_CASE("a central's write to the app's service is published as a copy, and the link's "
          "edges once each", "[radio_ao][ble_app]")
{
    static const uint8_t bytes[3] = { 0x00U, 0x41U, 0x00U };

    radio_fresh();
    fake_ble_reset();
    go_up();
    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));

    fake_ble_app_service_set(&s_app_svc);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_app_set_value("cmd", NULL, 0U));
    ble_app_subscribe(ao_spy());
    ao_spy_drain();

    /* A central arrives: one edge. */
    fake_ble_central(true);
    pump();
    const BleAppLinkEvt *l = (const BleAppLinkEvt *)spy_take_sig(BLE_APP_LINK_SIG);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_TRUE(l->connected);
    TEST_ASSERT_NULL(spy_take_sig(BLE_APP_LINK_SIG));
    ao_spy_drain();

    /* It writes three bytes, NULs included: the host task's copy, published. */
    TEST_ASSERT_TRUE(kdrv_ble_app_written(1U, bytes, (uint16_t)sizeof(bytes)));
    pump();
    const BleAppWriteEvt *w = (const BleAppWriteEvt *)spy_take_sig(BLE_APP_WRITE_SIG);
    TEST_ASSERT_NOT_NULL(w);
    TEST_ASSERT_EQUAL_UINT8(1U, w->chr);
    TEST_ASSERT_EQUAL_STRING("cmd", w->name);
    TEST_ASSERT_EQUAL_UINT16(3U, w->len);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(bytes, w->data, sizeof(bytes));
    ao_spy_drain();

    /* It leaves: one edge. */
    fake_ble_central(false);
    pump();
    l = (const BleAppLinkEvt *)spy_take_sig(BLE_APP_LINK_SIG);
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_FALSE(l->connected);
    TEST_ASSERT_NULL(spy_take_sig(BLE_APP_LINK_SIG));
    ao_spy_drain();

    /* Leave the flags and the subscriptions as this file found them. */
    QActive_unsubscribe(ao_spy(), BLE_APP_WRITE_SIG);
    QActive_unsubscribe(ao_spy(), BLE_APP_LINK_SIG);
    fake_ble_app_service_set(NULL);
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

TEST_CASE("a reconnect folded into one read of the state is told as an end and a beginning",
          "[radio_ao][ble_app]")
{
    radio_fresh();
    fake_ble_reset();
    go_up();
    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    ble_app_subscribe(ao_spy());

    fake_ble_central(true);
    pump();
    ao_spy_drain();

    /* Gone and back before AO_Radio reads the driver: the state reads
     * CONNECTED both times, and only the count of links moved. */
    fake_ble_central(false);
    fake_ble_central(true);
    radio_queue_flush();
    dispatch(BLE_LINK_SIG);
    pump();

    const BleAppLinkEvt *a = (const BleAppLinkEvt *)spy_take_sig(BLE_APP_LINK_SIG);
    const BleAppLinkEvt *b = (const BleAppLinkEvt *)spy_take_sig(BLE_APP_LINK_SIG);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_FALSE(a->connected);
    TEST_ASSERT_TRUE(b->connected);
    ao_spy_drain();

    QActive_unsubscribe(ao_spy(), BLE_APP_WRITE_SIG);
    QActive_unsubscribe(ao_spy(), BLE_APP_LINK_SIG);
    fake_ble_central(false);
    pump();
    ao_spy_drain();
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}

/** @brief The generation the value cases stamp their waiter with. */
#define VALUE_TEST_GEN 7U

/** @brief Post an app's value or notification to AO_Radio, as kode_sys_ble.c builds it. */
static void post_value(const char *chr, bool notify, const char *text, BleValueWaiter *wait)
{
    QEvt const *const e = ble_value_evt(chr, notify, text, strlen(text), wait, VALUE_TEST_GEN);

    TEST_ASSERT_NOT_NULL(e);
    QACTIVE_POST(AO_Radio, e, NULL);
    pump();
}

TEST_CASE("AO_Radio sets the app's values and queues its notifications, answers a full ring "
          "into the waiter and wakes the host for each one queued", "[radio_ao][ble_app]")
{
    BleValueWaiter w;
    uint8_t        got[KDRV_BLE_APP_VALUE_MAX];

    radio_fresh();
    fake_ble_app_service_set(&s_app_svc);
    kdrv_ble_app_link_reset();
    kdrv_ble_app_subscribed(0U, true);
    unsigned const kicks = fake_ble_app_kicks();

    /* A command sets the value and answers nobody. */
    post_value("temp", false, "a", NULL);
    TEST_ASSERT_EQUAL_UINT16(1U, kdrv_ble_app_read(0U, got, (uint16_t)sizeof(got)));
    TEST_ASSERT_EQUAL_HEX8('a', got[0]);
    TEST_ASSERT_FALSE(kdrv_ble_app_queued());
    TEST_ASSERT_EQUAL_UINT(kicks, fake_ble_app_kicks());

    /* Notifications fill the ring, and each wakes the host: no job is in flight. */
    for ( uint32_t i = 0U; KDRV_BLE_APP_RING_SLOTS > i; ++i )
    {
        post_value("temp", true, "b", NULL);
    }
    TEST_ASSERT_TRUE(kdrv_ble_app_queued());
    TEST_ASSERT_EQUAL_UINT(kicks + KDRV_BLE_APP_RING_SLOTS, fake_ble_app_kicks());

    /* One more, from a task: the ring is full, the value is set, the waiter told. */
    fake_sem_reset();
    w.done = khal_sem_create();
    w.ack  = 0U;
    w.rc   = (uint8_t)KODE_OK;
    post_value("temp", true, "c", &w);
    TEST_ASSERT_EQUAL_INT((int)KODE_NO_RESOURCES, (int)w.rc);
    TEST_ASSERT_EQUAL_UINT32(VALUE_TEST_GEN, __atomic_load_n(&w.ack, __ATOMIC_ACQUIRE));
    TEST_ASSERT_EQUAL_UINT(1U, fake_sem_signals());
    TEST_ASSERT_EQUAL_UINT(kicks + KDRV_BLE_APP_RING_SLOTS, fake_ble_app_kicks());
    TEST_ASSERT_EQUAL_UINT16(1U, kdrv_ble_app_read(0U, got, (uint16_t)sizeof(got)));
    TEST_ASSERT_EQUAL_HEX8('c', got[0]);

    /* A characteristic the table refuses is answered DENIED, nothing queued. */
    w.ack = 0U;
    post_value("cmd", true, "d", &w);
    TEST_ASSERT_EQUAL_INT((int)KODE_DENIED, (int)w.rc);
    TEST_ASSERT_EQUAL_UINT32(VALUE_TEST_GEN, __atomic_load_n(&w.ack, __ATOMIC_ACQUIRE));
    khal_sem_delete(w.done);

    kdrv_ble_app_link_reset();
    fake_ble_app_service_set(NULL);
}

TEST_CASE("a notification queued while a job runs wakes the host when the job ends",
          "[radio_ao][ble_app]")
{
    radio_fresh();
    fake_ble_reset();
    go_up();
    fake_ble_app_service_set(&s_app_svc);
    kdrv_ble_app_link_reset();
    kdrv_ble_app_subscribed(0U, true);

    dispatch(BLE_START_REQ_SIG);
    pump();
    TEST_ASSERT_TRUE(job_held());
    unsigned const kicks = fake_ble_app_kicks();

    post_value("temp", true, "b", NULL);
    TEST_ASSERT_TRUE(kdrv_ble_app_queued());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(kicks, fake_ble_app_kicks(),
                                   "the host was woken while a worker job held NimBLE");
    finish_ble_start();
    TEST_ASSERT_GREATER_THAN_UINT(kicks, fake_ble_app_kicks());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));

    kdrv_ble_app_link_reset();
    fake_ble_app_service_set(NULL);
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
}


/* ---- The board's rest ------------------------------------------------------- */

/** @brief Open a round on AO_Radio and run what it handed the worker. */
static void lp_radio_round(kode_lp_state_t target)
{
    lp_round(AO_Radio, target);
    drain();
}

/** @brief AO_Net answers the round just opened: it has left. */
static void lp_net_left(void)
{
    radio_net_left(s_lp_round_no);
    drain();
}

/* ---- Getting back on a network ------------------------------------------ */

/** @brief Wi-Fi on with a network remembered, its first round over and nothing
 *         joined: the first wait after a round is running. */
static void retry_ready(void)
{
    radio_fresh();
    go_up();
    vault_has("line-ap-0");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
}

/** @brief On a network, then the network goes away by itself. */
static void retry_network_drops(void)
{
    wifi_station_up();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_GOT_IP, (int)kdrv_wifi_state(NULL));
    wifi_event_sta_disconnected_t ev;

    (void)memset(&ev, 0, sizeof(ev));
    ev.reason = (uint16_t)WIFI_REASON_BEACON_TIMEOUT;
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev);
}

/** @brief The time passes by @p ms, as the QP ticks it. */
static void retry_wait(uint32_t ms)
{
    tick(QF_SUPPORT_STEPS(ms));
    pump();
}

/** @brief Tell the AO the board is ASLEEP. */
static void retry_asleep(void)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_ASLEEP;
    e.cause = (uint8_t)KODE_LP_CAUSE_IDLE;
    e.prev  = (uint8_t)KODE_LP_AWAKE;
    QASM_DISPATCH(AO_Radio, &e.super, 0U);
}

TEST_CASE("a network that goes away is said at once, and tried again with "
          "growing waits", "[radio_ao][retry]")
{
    WifiStateEvt w;

    retry_ready();
    retry_network_drops();
    ao_spy_drain();
    pump();
    TEST_ASSERT_TRUE_MESSAGE(wifi_state_last(&w), "nobody was told");
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_WIFI_DISCONNECTED, (unsigned)w.state);
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_WIFI_FAIL_NOT_FOUND, (unsigned)w.fail);
    TEST_ASSERT_TRUE(w.enabled);
    TEST_ASSERT_TRUE(w.reconnecting);
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    static const uint32_t WAITS_MS[] = { 5000U, 30000U, 120000U, 600000U, 600000U };

    for ( size_t i = 0U; i < (sizeof(WAITS_MS) / sizeof(WAITS_MS[0])); ++i )
    {
        retry_wait(WAITS_MS[i] - 1000U);
        TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "tried before its wait");
        retry_wait(2000U);
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job(),
                                      "the wait ended and nothing was tried");
        ao_spy_drain();
        finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
        TEST_ASSERT_TRUE(wifi_state_last(&w));
        TEST_ASSERT_TRUE(w.reconnecting);
    }
    wifi_owed_cleanup();
}

TEST_CASE("getting an address back ends the waits, and the next drop starts "
          "from the first", "[radio_ao][retry]")
{
    retry_ready();
    retry_network_drops();
    pump();
    retry_wait(6000U);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    wifi_station_up();
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_FALSE(wifi_state_now().reconnecting);
    retry_wait(700000U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "on a network, and still trying");

    retry_network_drops();
    pump();
    retry_wait(6000U);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    wifi_owed_cleanup();
}

TEST_CASE("a step off the network the person asked for is not tried again",
          "[radio_ao][retry]")
{
    retry_ready();
    wifi_station_up();
    pump();
    dispatch(WIFI_LEAVE_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_LEAVE, running_job());
    finish(KDRV_RADIO_JOB_WIFI_LEAVE, KODE_OK);
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    pump();
    TEST_ASSERT_FALSE(wifi_state_now().reconnecting);
    retry_wait(700000U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    /* Asking for it again is what ends that. */
    dispatch(WIFI_AUTO_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    TEST_ASSERT_TRUE(wifi_state_now().reconnecting);
    wifi_owed_cleanup();
}

TEST_CASE("Wi-Fi off, or nothing remembered, or wifi_join off: nothing is tried",
          "[radio_ao][retry]")
{
    retry_ready();
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    finish_whatever_runs();
    TEST_ASSERT_FALSE(wifi_state_now().reconnecting);
    retry_wait(700000U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());

    retry_ready();
    (void)config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_WJOIN, .set = true,
                                                .b = false });
    retry_network_drops();
    pump();
    TEST_ASSERT_FALSE(wifi_state_now().reconnecting);
    retry_wait(700000U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    (void)config_ask(AO_Radio, &(config_ask_t){ .key = KSET_KEY_WJOIN, .set = true,
                                                .b = true });
    wifi_owed_cleanup();

    radio_fresh();
    go_up();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    kode_vault_list_t none;

    (void)memset(&none, 0, sizeof(none));
    kode_answer(KODE_ST_OK, &none, (uint16_t)sizeof(none));
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_FALSE(wifi_state_now().reconnecting);
    retry_wait(700000U);
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    wifi_owed_cleanup();
}

TEST_CASE("asleep nothing is tried, and a wake tries at once from the first wait",
          "[radio_ao][retry]")
{
    retry_ready();
    retry_network_drops();
    pump();
    retry_asleep();

    /* The tick stops in a real sleep. A radio wake's does run: not for this. */
    lp_radio(AO_Radio);
    retry_wait(700000U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a sweep inside a radio wake");
    TEST_ASSERT_TRUE(wifi_state_now().reconnecting);

    retry_asleep();
    lp_awake(AO_Radio);
    pump();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job(),
                                  "awake and not trying");
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    wifi_owed_cleanup();
}

TEST_CASE("a round for the rest stops a try from starting; abandoned, the wait "
          "goes on as it was", "[radio_ao][retry]")
{
    retry_ready();
    retry_network_drops();
    pump();
    lp_radio_round(KODE_LP_OFF);
    retry_wait(6000U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a round started under a rest");

    /* It was due: the abandoned round lets it go, once and no sooner. */
    lp_awake(AO_Radio);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    lp_awake(AO_Radio);
    pump();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    wifi_owed_cleanup();
}

TEST_CASE("a try that falls due behind a job goes when the job ends, and a join "
          "somebody asked for goes first", "[radio_ao][retry]")
{
    retry_ready();
    retry_network_drops();
    pump();
    wifi_scan_request();
    pump();
    retry_wait(6000U);
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_RADIO_JOB_WIFI_SCAN,
                           (unsigned)held_req()->type);
    unsigned const submits = job_submits();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(submits + 1U, job_submits(), "the try did not go");
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);

    wifi_scan_request();
    pump();
    retry_wait(35000U);
    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    run_scan_rc(KODE_DENIED);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_TRUE_MESSAGE(joining("net"), "the try took the join's place");
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    wifi_owed_cleanup();
}

TEST_CASE("a join asked for during a round cuts its sweep short and goes next",
          "[radio_ao][retry]")
{
    retry_ready();
    retry_wait(31000U);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_TRUE(wifi_state_now().joining);
    run_scan_rc(KODE_DENIED);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_TRUE_MESSAGE(joining("net"), "the round did not give way");
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    wifi_owed_cleanup();
}

TEST_CASE("a park under way stops a try from starting", "[radio_ao][retry]")
{
    park_ready();
    vault_has("line-ap-0");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    retry_network_drops();
    pump();
    retry_wait(4000U);          /* the C5 has 3 s to answer the park, the try 5 */
    park_ask(KDRV_RADIO_NOTE_APP);
    park_muted();
    assert_park_sent();

    retry_wait(2000U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a round under a park");
    kode_answer(KODE_ST_REFUSED, NULL, 0U);
    (void)boot_parked_taken();
    dispatch(C5_RUN_REQ_SIG);
    wifi_owed_cleanup();
}

/** @brief The board is awake again; a stop of the sentinel is run and ended. */
static void lp_radio_awake(void)
{
    lp_awake(AO_Radio);
    drain();
    while ( job_held() && (KDRV_RADIO_JOB_SLEEP == held_req()->type) )
    {
        finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    }
}

/** @brief Take AO_LowPower's next request, if the radio posted one. */
static bool lp_request_take(LowPowerReqEvt *out)
{
    const QEvt *e = NULL;
    bool found    = false;

    while ( !found && (xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE) )
    {
        if ( (QSignal)LOWPOWER_REQ_SIG == e->sig )
        {
            *out  = *(const LowPowerReqEvt *)e;
            found = true;
        }
        QF_gc(e);
    }
    return found;
}

/** @brief From up to asleep: the sentinel is started and the round answered. */
static void lp_to_asleep(void)
{
    lp_radio_round(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
}

TEST_CASE("an ASLEEP round starts the sleep sentinel and answers ready once it runs",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();

    lp_radio_round(KODE_LP_ASLEEP);

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_TRUE_MESSAGE(held_req()->on, "the start, not the stop");
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_starts());
    lp_expect_silence();                      /* the outcome answers */

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_UP, link_now(),
                                  "the link is kept through the sleep");

    /* A second round of the same kind finds it at rest already. */
    lp_radio_round(KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_starts());

    lp_radio_awake();
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());
}

TEST_CASE("the sleep sentinel's start asks for the rest connection and its stop for the awake one",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();

    /* The BLE fake keeps counting across cases: read the difference. */
    unsigned const rest0  = fake_ble_conn_calls(true);
    unsigned const awake0 = fake_ble_conn_calls(false);

    lp_radio_round(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_ble_conn_calls(true) - rest0,
                                   "asked while the bus still runs, before the sentinel");
    TEST_ASSERT_EQUAL_UINT(0U, fake_ble_conn_calls(false) - awake0);
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);

    lp_radio_awake();
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_ble_conn_calls(false) - awake0,
                                   "the awake interval back once the bus is up");
    TEST_ASSERT_EQUAL_UINT(1U, fake_ble_conn_calls(true) - rest0);
}

TEST_CASE("a sleep sentinel that would not start gives the central its awake interval back",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();

    unsigned const rest0  = fake_ble_conn_calls(true);
    unsigned const awake0 = fake_ble_conn_calls(false);

    fake_radio_sleep_fail(KODE_IO_ERROR);
    lp_radio_round(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, fake_ble_conn_calls(true) - rest0);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_ble_conn_calls(false) - awake0,
                                   "the board stays awake, and so does the link");
    fake_radio_sleep_fail(KODE_OK);
}

TEST_CASE("a refused sleep start answers the round busy with the reason",
          "[radio_ao][lowpower]")
{
    static const struct {
        kode_status_t rc;
        const char   *why;
    } REFUSALS[] = {
        { KODE_IO_ERROR,     "c5 refused sleep"   },
        { KODE_DENIED,       "sdio not idle"      },
        { KODE_ERROR,        "wake line"          },
        { KODE_NO_RESOURCES, "radio sleep failed" },
    };

    radio_fresh();
    go_up();
    for ( unsigned i = 0U; Q_DIM(REFUSALS) > i; ++i )
    {
        lp_radio_round(KODE_LP_ASLEEP);
        TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
        finish(KDRV_RADIO_JOB_SLEEP, REFUSALS[i].rc);
        lp_expect_answer(true, REFUSALS[i].why);
        lp_expect_no_more();
        lp_radio_awake();     /* nothing was armed: nothing to stop */
        TEST_ASSERT_FALSE(job_held());
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_radio_sleep_stops(),
                                   "a sentinel that never started is not stopped");
}

TEST_CASE("a round meets a busy link, a starting link and no link",
          "[radio_ao][lowpower]")
{
    radio_fresh();

    /* No link: nothing to rest. */
    lp_radio_round(KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_EQUAL_UINT(0U, fake_radio_sleep_starts());

    dispatch(RADIO_UP_REQ_SIG);          /* the bring-up is in flight */
    lp_radio_round(KODE_LP_ASLEEP);
    lp_expect_answer(true, "link starting");
    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);

    wifi_scan_request();                 /* a scan holds the channel */
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    lp_radio_round(KODE_LP_ASLEEP);
    lp_expect_answer(true, "radio busy");
    TEST_ASSERT_EQUAL_UINT(0U, fake_radio_sleep_starts());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
}

TEST_CASE("AWAKE while the start is still with the worker stops the sentinel as it returns",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_radio_round(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(held_req()->on);

    lp_awake(AO_Radio);                  /* the round was abandoned */
    drain();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_radio_sleep_stops(),
                                   "a stop cannot overtake the start");

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_silence();                 /* the round is over: nobody is told */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the stop follows the start");
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());

    lp_radio_awake();                    /* AWAKE repeats: nothing more to do */
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());
}

TEST_CASE("AWAKE stops the sentinel and a second AWAKE does nothing",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(logged("sleep sentinel stopped"),
                             "what the sentinel did is said when it stops");

    lp_awake(AO_Radio);
    drain();
    TEST_ASSERT_FALSE(job_held());
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());

    /* The link may come down again, now that nothing keeps it. */
    dispatch(RADIO_DOWN_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());
}

TEST_CASE("an OFF round that meets the stop of an awake board is answered once the stop returns",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE(held_req()->on);

    lp_round(AO_Radio, KODE_LP_OFF);          /* the stop is still with the worker */
    drain();
    lp_net_left();
    lp_expect_silence();                      /* owed, not refused */

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());
}

TEST_CASE("an ASLEEP round that meets the stop of an awake board starts the sentinel after it",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    lp_expect_silence();

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);    /* the stop */
    lp_expect_silence();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_TRUE_MESSAGE(held_req()->on, "the round is decided as for a free AO");
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);    /* the start */
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    lp_radio_awake();
}

TEST_CASE("a round that meets a stop of the awake board that fails gets the deaf reason",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    lp_round(AO_Radio, KODE_LP_OFF);
    drain();
    lp_expect_silence();

    finish(KDRV_RADIO_JOB_SLEEP, KODE_TIMEOUT);
    lp_expect_answer(true, "c5 would not wake");
    lp_expect_no_more();
}

TEST_CASE("a round that was abandoned while the stop was out is not answered after it",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    lp_round(AO_Radio, KODE_LP_OFF);
    drain();
    lp_awake(AO_Radio);                       /* the round timed out: AWAKE again */
    drain();

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_silence();

    /* The silence came from the cleared round, not from a deaf AO: the next
     * round is answered as usual. */
    lp_radio_round(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(held_req()->on);
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    lp_radio_awake();
}

TEST_CASE("a retry round during the same stop is answered once, for the latest round",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    lp_round(AO_Radio, KODE_LP_OFF);          /* the first try */
    drain();
    lp_awake(AO_Radio);                       /* it timed out */
    drain();
    lp_round(AO_Radio, KODE_LP_OFF);          /* the retry, a new number */
    drain();
    lp_net_left();
    lp_expect_silence();

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);            /* s_lp_round_no is the retry's */
    lp_expect_no_more();
}

TEST_CASE("a round during the stop for an OFF round is still answered busy at once",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_radio_round(KODE_LP_OFF);
    lp_net_left();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the stop of the sentinel is out");
    lp_expect_silence();

    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    lp_expect_answer(true, "radio busy");
    lp_expect_no_more();

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_silence();                      /* the round was answered already */
}

/* The link cannot be anything but up when the stop returns: a request to take
 * it down is refused while a job is out, and a link that is lost drops the
 * job's outcome. So the round is refused when the link goes, whatever its
 * target. */
TEST_CASE("a link lost while the stop of an awake board holds an OFF round refuses it",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    lp_round(AO_Radio, KODE_LP_OFF);
    drain();
    lp_net_left();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    lp_expect_silence();

    finish_lost(KODE_ERROR);

    lp_expect_answer(true, "link lost");      /* never ready over a worker still inside */
    lp_expect_no_more();
}

TEST_CASE("a link lost while the stop of an awake board holds an ASLEEP round refuses it",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_awake(AO_Radio);
    drain();
    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    lp_expect_silence();

    finish_lost(KODE_ERROR);

    lp_expect_answer(true, "link lost");
    lp_expect_no_more();
}

TEST_CASE("a round that meets the start of the sentinel is still answered busy at once",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_radio_round(KODE_LP_ASLEEP);           /* the start is with the worker */
    TEST_ASSERT_TRUE(held_req()->on);

    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    lp_expect_answer(true, "radio busy");
    lp_expect_no_more();

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_radio_awake();
}

TEST_CASE("a sentinel that stands down while ASLEEP asks for AWAKE",
          "[radio_ao][lowpower]")
{
    LowPowerReqEvt rq;
    RadioSleepNoteEvt n;

    radio_fresh();
    go_up();
    fake_radio_sleep_note(KDRV_RADIO_WOKE_STOOD_DOWN, 0U);
    drain();
    TEST_ASSERT_FALSE_MESSAGE(lp_request_take(&rq),
                              "awake, the bus was never down: nothing to correct");

    lp_to_asleep();
    fake_radio_sleep_note(KDRV_RADIO_WOKE_C5, 2U);
    drain();
    TEST_ASSERT_FALSE_MESSAGE(lp_request_take(&rq), "an ordinary wake is not a request");
    TEST_ASSERT_TRUE(radio_sleep_last_note(&n));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_RADIO_WOKE_C5, n.kind);
    TEST_ASSERT_EQUAL_UINT16(2U, n.knocks);
    TEST_ASSERT_EQUAL_UINT32(3U, n.tx_sta);

    fake_radio_sleep_note(KDRV_RADIO_WOKE_STOOD_DOWN, 0U);
    drain();
    TEST_ASSERT_TRUE_MESSAGE(lp_request_take(&rq), "the system state must follow the hardware");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, rq.target);
    TEST_ASSERT_TRUE(radio_sleep_last_note(&n));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_RADIO_WOKE_STOOD_DOWN, n.kind);

    lp_radio_awake();
}

TEST_CASE("an OFF round waits for AO_Net, then answers with the link left up",
          "[radio_ao][lowpower]")
{
    unsigned subs;

    radio_fresh();
    go_up();
    storage_drain();
    subs = job_submits();

    lp_radio_round(KODE_LP_OFF);
    lp_expect_silence();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
                                  "the round waits while AO_Net still has sessions");

    lp_net_left();
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(subs, job_submits(),
                                   "no teardown job: NimBLE and esp-hosted keep running");
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, next_storage_op(),
                                  "the card was not asked to let go");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_radio_c5_held(),
                              "the board's power-off entry holds C5_EN, not this AO");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("AO_Net leaving before the round reaches the radio is remembered",
          "[radio_ao][lowpower]")
{
    unsigned subs;

    radio_fresh();
    go_up();
    subs = job_submits();

    radio_net_left(s_lp_round_no + 1U);       /* the round about to open */
    drain();
    lp_radio_round(KODE_LP_OFF);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_EQUAL_UINT(subs, job_submits());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
}

TEST_CASE("an OFF round from ASLEEP stops the sentinel, then answers without a teardown",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_radio_round(KODE_LP_OFF);
    lp_net_left();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the sentinel is stopped first");
    lp_expect_silence();

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "nothing is torn down after the stop");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
}

TEST_CASE("a C5 that will not wake for the stop refuses the OFF round",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_radio_round(KODE_LP_OFF);
    lp_net_left();
    finish(KDRV_RADIO_JOB_SLEEP, KODE_IO_ERROR);
    lp_expect_answer(true, "c5 would not wake");
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(),
                                  "esp-hosted is not touched over a bus that is down");
    lp_radio_awake();
}

TEST_CASE("an OFF round with no link is answered once AO_Net has left",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    fake_radio_set_c5_held(false);

    lp_radio_round(KODE_LP_OFF);
    lp_expect_silence();
    lp_net_left();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    TEST_ASSERT_EQUAL_INT(-1, running_job());
}

TEST_CASE("a link lost while an OFF round waits for AO_Net does not refuse it",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();

    lp_radio_round(KODE_LP_OFF);
    lp_expect_silence();
    finish_lost(KODE_ERROR);
    lp_expect_silence();                      /* AO_Net has not left yet */
    finish_only(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    lp_net_left();
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
}

/* The outcome of a job that was inside when the link went is dropped, so a
 * round answered ready over it would deep-sleep the P4 with the worker still
 * inside the driver, and nothing would answer later. */
TEST_CASE("a link lost while the sentinel's stop for an OFF round is out refuses the round",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    lp_radio_round(KODE_LP_OFF);
    lp_net_left();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the stop of the sentinel is out");
    lp_expect_silence();

    finish_lost(KODE_ERROR);

    lp_expect_answer(true, "link lost");
    lp_expect_no_more();
}

TEST_CASE("AO_Net leaving while the link is going down refuses the OFF round",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();

    lp_radio_round(KODE_LP_OFF);
    lp_expect_silence();                      /* AO_Net has not left yet */
    dispatch(RADIO_DOWN_REQ_SIG);             /* somebody takes the link down */
    pump();
    lp_expect_silence();
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KRADIO_STOPPING, link_now(),
                                  "the teardown is in flight");

    lp_net_left();

    lp_expect_answer(true, "link stopping");
    lp_expect_no_more();
}

TEST_CASE("an OFF abandoned after the answer finds the link as it was",
          "[radio_ao][lowpower]")
{
    unsigned subs;

    radio_fresh();
    go_up();
    lp_radio_round(KODE_LP_OFF);
    lp_net_left();
    lp_expect_answer(false, NULL);
    storage_drain();
    subs = job_submits();

    lp_radio_awake();
    lp_radio_awake();                         /* it repeats */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(subs, job_submits(), "nothing to bring back");
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, next_storage_op(), "no remount, no unmount");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
}

TEST_CASE("an OFF abandoned before AO_Net left costs the link nothing",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_radio_round(KODE_LP_OFF);              /* AO_Net never leaves: another owner was busy */

    lp_radio_awake();
    lp_net_left();                            /* too late: the round is over */
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    lp_expect_silence();
}

TEST_CASE("a link lost under an ASLEEP round refuses it", "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_radio_round(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());

    finish_lost(KODE_ERROR);
    lp_expect_answer(true, "link lost");
    lp_expect_no_more();
}

TEST_CASE("an OFF round does not wait for an AO_Net that is not taking part",
          "[radio_ao][lowpower]")
{
    unsigned subs;

    radio_fresh();
    radio_lp_expect_net(false);
    go_up();
    subs = job_submits();

    lp_radio_round(KODE_LP_OFF);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(subs, job_submits(),
                                   "nobody to wait for, and nothing to take down");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());

    /* And with no link at all. */
    radio_fresh();
    radio_lp_expect_net(false);
    lp_radio_round(KODE_LP_OFF);
    lp_expect_answer(false, NULL);
    radio_lp_expect_net(true);
}

TEST_CASE("a stop that AWAKE owed survives a round that is refused meanwhile",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_radio_round(KODE_LP_ASLEEP);           /* the start is with the worker */
    lp_awake(AO_Radio);                       /* the round was abandoned: a stop is owed */
    drain();

    lp_radio_round(KODE_LP_ASLEEP);           /* asked again at once */
    lp_expect_answer(true, "radio busy");

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_SLEEP, running_job(),
                                  "the owed stop must not be lost to the second round");
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());
}

TEST_CASE("a stop the worker refuses is owed and paid when a job returns",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();

    fake_khal_worker_refuse_for(RADIO_WORKER, true);
    lp_awake(AO_Radio);
    drain();
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_EQUAL_UINT(0U, fake_radio_sleep_stops());

    fake_khal_worker_refuse_for(RADIO_WORKER, false);
    exp_power_request(true);                  /* any job will do: its return pays the debt */
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_EXP_POWER, running_job());
    finish(KDRV_RADIO_JOB_EXP_POWER, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE(held_req()->on);
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
}

TEST_CASE("a full queue drops a wake note instead of asserting, and a stand-down still gets in",
          "[radio_ao][lowpower]")
{
    static const QEvt filler = QEVT_INITIALIZER(LOWPOWER_MINUTE_SIG);   /* nobody here answers it */
    RadioSleepNoteEvt n;
    LowPowerReqEvt    rq;

    radio_fresh();
    go_up();
    lp_to_asleep();
    fake_radio_sleep_note(KDRV_RADIO_WOKE_TX, 1U);    /* a note on record */
    drain();
    TEST_ASSERT_TRUE(radio_sleep_last_note(&n));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_RADIO_WOKE_TX, n.kind);

    /* Leave the margin's worth of room, no more. */
    while ( uxQueueSpacesAvailable(AO_Radio->eQueue) > 4U )
    {
        const QEvt *f = &filler;

        TEST_ASSERT_TRUE(xQueueSend(AO_Radio->eQueue, &f, 0U) == pdTRUE);
    }
    fake_klog_reset();
    fake_radio_sleep_note(KDRV_RADIO_WOKE_C5, 2U);
    TEST_ASSERT_TRUE_MESSAGE(logged("wake note was dropped"), "the drop is said");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4U, uxQueueSpacesAvailable(AO_Radio->eQueue),
                                   "and the queue was left as it was");

    fake_radio_sleep_note(KDRV_RADIO_WOKE_STOOD_DOWN, 0U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, uxQueueSpacesAvailable(AO_Radio->eQueue),
                                   "a stand-down yields last and gets in");
    for ( unsigned i = 0U; 4U > i; ++i )
    {
        drain();
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, (unsigned)uxQueueMessagesWaiting(AO_Radio->eQueue),
                                   "the queue is served to the end");
    TEST_ASSERT_TRUE(lp_request_take(&rq));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_AWAKE, rq.target);
    lp_radio_awake();
}

/* ---- A radio wake: the bus awake, the glass asleep ------------------------------ */

TEST_CASE("a radio wake stops the sentinel and says done when the stop has returned; "
          "the round that ends it starts the sentinel again",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Radio);
    drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the bus is woken for the network");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(),
                                   "not done while the stop is with the worker");

    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(), "done, once");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_UP, link_now());
    lp_expect_no_more();

    lp_radio_round(KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    TEST_ASSERT_TRUE_MESSAGE(held_req()->on, "the sentinel is started again");
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_radio_done_count());

    lp_radio_awake();
}

TEST_CASE("a radio wake with no sentinel armed is done at once", "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Radio);
    drain();

    TEST_ASSERT_FALSE(job_held());
    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_radio_done_count());
}

TEST_CASE("a radio wake with no link is done at once", "[radio_ao][lowpower]")
{
    radio_fresh();
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Radio);
    drain();

    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_radio_done_count());
}

TEST_CASE("a whole AWAKE after a radio wake does not say done, and stops nothing twice",
          "[radio_ao][lowpower]")
{
    radio_fresh();
    go_up();
    lp_to_asleep();
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Radio);
    drain();
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_radio_done_count());

    lp_radio_awake();
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_radio_done_count());
    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_sleep_stops());
}

/* The advert: visible while the board knows nobody, hidden from all but its
 * bonded devices once it has one, and Pair new device shows it again for five
 * minutes. Never off by itself. */
static void ble_adv_case_end(void)
{
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    fake_ble_set_bonds(0U);
    fake_ble_secure(false);
}

/** @brief The advert the worker holds: its interval, its time, hidden or not. */
static void expect_adv(uint16_t itvl, uint32_t len, bool hidden)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    TEST_ASSERT_TRUE_MESSAGE(held_req()->on, "the advert was taken off the air");
    TEST_ASSERT_EQUAL_UINT((unsigned)itvl, held_req()->itvl);
    TEST_ASSERT_EQUAL_UINT(len, held_req()->len);
    TEST_ASSERT_EQUAL_MESSAGE(hidden, held_req()->hidden, "the advert showed the board the wrong way");
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
}

TEST_CASE("with no bond the advert is visible, fast when asked for, then slow "
          "with no end", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    go_up();

    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, false);
    unsigned const nvs_before = fake_nvs_count();

    fake_time_advance_ms(30000U);
    fake_ble_adv_end();
    pump();
    expect_adv((uint16_t)KDRV_BLE_ITVL_SLOW, 0U, false);

    fake_time_advance_ms(600000U);
    pump();
    TEST_ASSERT_TRUE_MESSAGE(ble_enabled(), "Bluetooth went off by itself");
    TEST_ASSERT_FALSE(job_held());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(nvs_before, fake_nvs_count(), "the store heard about the advert");
    ble_adv_case_end();
}

TEST_CASE("with a bond the advert hides the board, and Pair new device shows it "
          "for five minutes", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    fake_ble_set_bonds(1U);
    go_up();

    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, true);
    fake_time_advance_ms(30000U);
    fake_ble_adv_end();
    pump();
    expect_adv((uint16_t)KDRV_BLE_ITVL_SLOW, 0U, true);

    /* The hidden advert comes off, and the visible one follows, fast. */
    dispatch(BLE_PAIR_OPEN_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the hidden advert stayed on the air");
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(false));
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, false);

    fake_time_advance_ms(30000U);
    fake_ble_adv_end();
    pump();
    expect_adv((uint16_t)KDRV_BLE_ITVL_SLOW, 270000U, false);

    /* The time runs out: hidden again, with no end. */
    fake_time_advance_ms(270000U);
    fake_ble_adv_end();
    pump();
    expect_adv((uint16_t)KDRV_BLE_ITVL_SLOW, 0U, true);
    TEST_ASSERT_TRUE(ble_enabled());
    ble_adv_case_end();
}

TEST_CASE("a device that pairs ends the time to pair: the board hides once it "
          "leaves", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    fake_ble_pair_reset();
    fake_ble_set_bonds(1U);
    go_up();

    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, true);

    dispatch(BLE_PAIR_OPEN_REQ_SIG);
    pump();
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(false));
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, false);

    fake_time_advance_ms(10000U);
    fake_ble_central(true);
    pump();
    fake_ble_pair_show(123456U);
    pump();
    fake_ble_set_bonds(2U);
    fake_ble_secure(true);
    fake_ble_pair_end(KDRV_BLE_PAIR_OK);
    pump();
    fake_ble_central(false);
    pump();
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, true);
    ble_adv_case_end();
}

TEST_CASE("Forget all shows a hidden board again", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    fake_ble_set_bonds(1U);
    go_up();

    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, true);

    dispatch(BLE_UNPAIR_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_UNPAIR, running_job());
    finish(KDRV_RADIO_JOB_BLE_UNPAIR, kdrv_ble_unpair());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    TEST_ASSERT_FALSE_MESSAGE(held_req()->on, "the hidden advert stayed on the air");
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(false));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    TEST_ASSERT_FALSE(held_req()->hidden);
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    ble_adv_case_end();
}

TEST_CASE("a phone that encrypted and left gets a fast advert, and a stranger "
          "the advert as it was", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    fake_ble_set_bonds(1U);
    go_up();

    dispatch(BLE_START_REQ_SIG);
    pump();
    finish_ble_start();
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, true);

    /* A stranger, two minutes in: the slow advert, still hidden. */
    fake_time_advance_ms(120000U);
    fake_ble_secure(false);
    fake_ble_central(true);
    pump();
    fake_ble_central(false);
    pump();
    expect_adv((uint16_t)KDRV_BLE_ITVL_SLOW, 0U, true);

    /* A phone with its bond, later on: fast for 30 s. */
    fake_time_advance_ms(60000U);
    fake_ble_central(true);
    pump();
    fake_ble_secure(true);
    fake_ble_central(false);
    pump();
    expect_adv((uint16_t)KDRV_BLE_ITVL_FAST, 30000U, true);
    ble_adv_case_end();
}

TEST_CASE("Pair new device with Bluetooth off puts nothing on the air", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    go_up();

    dispatch(BLE_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    dispatch(BLE_PAIR_OPEN_REQ_SIG);
    pump();
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "a board switched off went on the air");
    TEST_ASSERT_FALSE(ble_enabled());
    ble_adv_case_end();
}

/* ---- The bond count and the store a teardown frees ---------------------------------- */

/** @brief The bond count of the next BLE state in the spy; fails the case if none. */
static uint8_t take_bonds(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "no BLE state was published");
        if ( (QSignal)BLE_STATE_SIG == e->sig )
        {
            return ((const BleStateEvt *)e)->bonds;
        }
    }
}

TEST_CASE("a BLE state published while the worker takes the link down keeps the count and "
          "does not read NimBLE's store", "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    fake_ble_set_bonds(2U);
    go_up();
    ao_spy_subscribe(BLE_STATE_SIG);

    dispatch(BLE_STATE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT8(2U, take_bonds());
    ao_spy_drain();

    dispatch(RADIO_DOWN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());

    /* The store is gone under the worker: a read would see none. */
    fake_ble_set_bonds(0U);
    dispatch(BLE_STATE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(2U, take_bonds(), "the store was read during the teardown");
    ao_spy_drain();

    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    dispatch(BLE_STATE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, take_bonds(), "the count is read again once the worker is back");
    c5_case_end();
}

TEST_CASE("the same when the link is lost and the teardown is the AO's own, not radio_down's",
          "[radio_ao]")
{
    radio_fresh();
    fake_ble_reset();
    fake_ble_set_bonds(3U);
    go_up();
    ao_spy_subscribe(BLE_STATE_SIG);

    dispatch(BLE_STATE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT8(3U, take_bonds());
    ao_spy_drain();

    fake_radio_drop_link();
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_DISCONNECT, running_job());

    fake_ble_set_bonds(0U);
    dispatch(BLE_STATE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(3U, take_bonds(), "the store was read during the teardown");
    ao_spy_drain();

    finish(KDRV_RADIO_JOB_DISCONNECT, KODE_OK);
    dispatch(BLE_STATE_REQ_SIG);
    TEST_ASSERT_EQUAL_UINT8(0U, take_bonds());
    c5_case_end();
}

TEST_CASE("an autojoin that finds the vault empty says so once, however often it is asked",
          "[radio_ao]")
{
    kode_vault_list_t none;

    radio_fresh();
    go_up();
    (void)memset(&none, 0, sizeof(none));
    kode_answer(KODE_ST_OK, &none, (uint16_t)sizeof(none));
    fake_klog_reset();

    for ( unsigned i = 0U; 3U > i; i++ )
    {
        TEST_ASSERT_TRUE(wifi_auto_request());
        pump();
    }
    TEST_ASSERT_EQUAL_UINT(1U, logged("no networks remembered"));
    TEST_ASSERT_FALSE(job_held());
    c5_case_end();
}

/* ---- wifi_scan_stop -------------------------------------------------------------- */

/**
 * @brief The listing's end, taken from the spy: every other event is read past.
 * @return The end; the case fails when the listing was never closed.
 */
static const WifiScanEndEvt *listing_end(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the listing was never closed");
        if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            return (const WifiScanEndEvt *)(const void *)e;
        }
    }
}

/** @brief Ask for the stop the way `wifi_scan_stop` does, and let AO_Radio read it. */
static void ask_stop(void)
{
    TEST_ASSERT_TRUE(wifi_scan_stop_request());
    pump();
}

/* The stop is in before the fake worker starts the sweep, so not one step
 * runs: the driver reads the cut between steps, the first included. The
 * sweep's end withdraws it, and the next sweep runs whole. */
TEST_CASE("a stop asked during a listed scan cuts the sweep short",
          "[radio_ao][scan_stop]")
{
    const WifiScanEndEvt *end;

    radio_fresh();
    go_up();
    ao_spy_drain();
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    ask_stop();
    TEST_ASSERT_TRUE(logged("wifi_scan_stop: the scan ends after the step on the air"));

    seed_air(2U, -50);
    run_scan_rc(KODE_DENIED);
    TEST_ASSERT_EQUAL_UINT(0U, s_starts);
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);

    end = listing_end();
    TEST_ASSERT_FALSE(end->ok);
    TEST_ASSERT_FALSE(end->busy);
    TEST_ASSERT_TRUE_MESSAGE(end->cut, "the end does not say the sweep was cut");
    TEST_ASSERT_EQUAL_UINT8(0U, end->count);
    TEST_ASSERT_FALSE(job_held());

    run_scan();
    TEST_ASSERT_TRUE_MESSAGE(1U < s_starts, "the cut outlived the sweep it was for");
}

/* The steps that ran before the stop are on the bus already; the end counts
 * them and says the list is not the whole air. */
TEST_CASE("a stopped scan publishes what it heard and an end that says cut",
          "[radio_ao][scan_stop]")
{
    const WifiScanEndEvt *end;
    unsigned              entries = 0U;

    radio_fresh();
    go_up();
    ao_spy_drain();
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    seed_air(3U, -50);
    run_scan();
    pump();
    ask_stop();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();
        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the listing was never closed");
        if ( (QSignal)WIFI_SCAN_ENTRY_SIG == e->sig )
        {
            ++entries;
        }
        if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            end = (const WifiScanEndEvt *)(const void *)e;
            break;
        }
    }
    TEST_ASSERT_EQUAL_UINT(3U, entries);
    TEST_ASSERT_FALSE(end->ok);
    TEST_ASSERT_TRUE(end->cut);
    TEST_ASSERT_EQUAL_UINT8(3U, end->count);
}

/* A sweep that failed, or that the link dropped under, was not cut by anybody. */
TEST_CASE("a scan that fails is not said to be cut", "[radio_ao][scan_stop]")
{
    const WifiScanEndEvt *end;

    radio_fresh();
    go_up();
    ao_spy_drain();
    dispatch(WIFI_SCAN_REQ_SIG);
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);

    end = listing_end();
    TEST_ASSERT_FALSE(end->ok);
    TEST_ASSERT_FALSE(end->cut);
}

TEST_CASE("a stop with no scan running sets nothing, and the next sweep runs whole",
          "[radio_ao][scan_stop]")
{
    radio_fresh();
    go_up();
    unsigned const before = job_submits();

    ask_stop();
    TEST_ASSERT_TRUE(logged("wifi_scan_stop: no scan to stop"));
    TEST_ASSERT_EQUAL_UINT(before, job_submits());
    TEST_ASSERT_FALSE(job_held());

    seed_air(2U, -50);
    run_scan();
    TEST_ASSERT_TRUE_MESSAGE(1U < s_starts, "a stop with nothing to stop cut the next sweep");
}

TEST_CASE("a stop with no link sets nothing", "[radio_ao][scan_stop]")
{
    radio_fresh();
    unsigned const before = job_submits();

    ask_stop();
    TEST_ASSERT_TRUE(logged("wifi_scan_stop: no scan to stop"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, job_submits(), "a stop brought the link up");
    TEST_ASSERT_EQUAL_INT((int)KRADIO_OFF, link_now());

    seed_air(2U, -50);
    run_scan();
    TEST_ASSERT_TRUE(1U < s_starts);
}

/* The round's sweep is not a listing: cut, the round would run it again. */
TEST_CASE("a stop leaves the automatic round's sweep alone", "[radio_ao][scan_stop]")
{
    radio_fresh();
    go_up();
    vault_has("line-ap-0");
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    ask_stop();
    TEST_ASSERT_TRUE(logged("wifi_scan_stop: no scan to stop"));
    seed_air(2U, -50);
    run_scan();
    TEST_ASSERT_TRUE_MESSAGE(1U < s_starts, "the round's sweep was cut");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    ao_spy_drain();
}

/* A row judged on half the air would fail a good unit. */
TEST_CASE("a stop leaves the c5 row's sweep alone", "[radio_ao][scan_stop]")
{
    radio_fresh();
    go_up();
    ask_row();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    finish_info();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    ask_stop();
    TEST_ASSERT_TRUE(logged("wifi_scan_stop: no scan to stop"));
    finish_rest();   /* its sweep runs to its end, or run_scan() fails the case */
    TEST_ASSERT_TRUE(take_verdict());
}

/* An asked scan is its asker's: its end would say the scan failed. */
TEST_CASE("a stop leaves an asked scan alone", "[radio_ao][scan_stop]")
{
    scan_seen_t s;

    radio_fresh();
    go_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    ask_stop();
    TEST_ASSERT_TRUE(logged("wifi_scan_stop: no scan to stop"));
    scan_sweep(3U, -50, KODE_OK);

    s = scan_take_spy();
    TEST_ASSERT_EQUAL_UINT(3U, s.entries);
    TEST_ASSERT_EQUAL_UINT(1U, s.ends);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, s.end->rc);
    ao_spy_drain();
}

/* The sweep was past its last step when the stop came: the list is whole and
 * says so, and the cut does not wait for the next sweep. */
TEST_CASE("a stop after the last step leaves the scan whole and the cut withdrawn",
          "[radio_ao][scan_stop]")
{
    const WifiScanEndEvt *end;

    radio_fresh();
    go_up();
    ao_spy_drain();
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    seed_air(2U, -50);
    run_scan();
    pump();

    ask_stop();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);

    end = listing_end();
    TEST_ASSERT_TRUE(end->ok);
    TEST_ASSERT_FALSE(end->cut);
    TEST_ASSERT_EQUAL_UINT8(2U, end->count);

    run_scan();
    TEST_ASSERT_TRUE_MESSAGE(1U < s_starts, "the cut waited for the next sweep");
}

/* The link drops with the stop asked and the sweep still on the worker: the
 * listing is closed by the lost link, and the sweep that follows the next
 * bring-up must not find the cut standing. */
TEST_CASE("a stopped scan the link drops under leaves no cut standing",
          "[radio_ao][scan_stop]")
{
    radio_fresh();
    go_up();
    ao_spy_drain();
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    ask_stop();
    finish_lost(KODE_ERROR);
    TEST_ASSERT_FALSE(listing_end()->ok);
    while ( job_held() )
    {
        finish_whatever_runs();
    }

    seed_air(2U, -50);
    run_scan();
    TEST_ASSERT_TRUE_MESSAGE(1U < s_starts, "the cut outlived the link");
    c5_case_end();
}

/* The door keeps AO_Radio's half of its queue: within the margin nothing is
 * posted, and the caller is told. */
TEST_CASE("a stop asked on a full radio queue is refused and posts nothing",
          "[radio_ao][scan_stop]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);
    const QEvt       *e      = NULL;

    radio_fresh();
    for ( uint32_t guard = 0U;
         (64U > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_POST_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_FALSE(wifi_scan_stop_request());
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        TEST_ASSERT_NOT_EQUAL((int)WIFI_SCAN_STOP_REQ_SIG, (int)e->sig);
        QF_gc(e);
    }
}

/* The next file to run the AO cannot finish a job this one holds. */
TEST_CASE("AO_Radio is left off for the next file", "[radio_ao]")
{
    c5_case_end();
}
