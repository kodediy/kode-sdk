/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_newest.c
 * @brief AO_Radio: a newer radio request ends the scan in flight and runs.
 */
#include <stdio.h>
#include <string.h>

#include "radio_harness.h"

/** @brief A listen's length the cases ask for, ms, and a second one to tell it from. */
#define LISTEN_MS        3000U
#define LISTEN_NEWER_MS  1000U

/** @brief Networks the mocked air holds, at most. */
#define AIR_MAX  4U

/* ---- The mocked sweep ------------------------------------------------------------
 * The real kdrv_wifi_scan() under ESP-IDF's mock: every step answers at once,
 * and the first one holds the networks a case put in the air.
 */

static wifi_ap_record_t s_air[AIR_MAX];
static uint16_t         s_air_n;
static unsigned         s_air_next;
static unsigned         s_steps;
static uint16_t         s_step_found;

static esp_err_t step_now(const wifi_scan_config_t *config, bool block, int calls)
{
    (void)config; (void)calls;
    TEST_ASSERT_FALSE(block);
    s_step_found = 0U;
    if ( 0U == s_steps )
    {
        s_step_found = s_air_n;
    }
    ++s_steps;
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    return ESP_OK;
}

static esp_err_t found_now(uint16_t *number, int calls)
{
    (void)calls;
    if ( NULL != number )
    {
        *number = s_step_found;
    }
    return ESP_OK;
}

static esp_err_t record_now(wifi_ap_record_t *out, int calls)
{
    (void)calls;
    if ( NULL != out )
    {
        *out = s_air[s_air_next % AIR_MAX];
    }
    ++s_air_next;
    return ESP_OK;
}

static esp_err_t country_open(wifi_country_t *country, int calls)
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
 * @brief Run the driver's sweep with @p n networks in the air.
 * @param n    Networks, AIR_MAX at most.
 * @param want What the sweep must answer.
 * @return The steps it started: 0 for a sweep cut before its first.
 */
static unsigned sweep(unsigned n, kode_status_t want)
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
    (void)memset(s_air, 0, sizeof(s_air));
    for ( unsigned i = 0U; (i < n) && (AIR_MAX > i); ++i )
    {
        (void)snprintf((char *)s_air[i].ssid, sizeof(s_air[i].ssid), "net-%u", i);
        s_air[i].rssi    = (int8_t)(-50 - (int)(5U * i));
        s_air[i].primary = (uint8_t)(1U + i);
    }
    s_air_n    = (uint16_t)n;
    s_air_next = 0U;
    s_steps    = 0U;
    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_OK);
    esp_wifi_get_country_Stub(&country_open);
    esp_wifi_scan_start_Stub(&step_now);
    esp_wifi_scan_get_ap_num_Stub(&found_now);
    esp_wifi_scan_get_ap_record_Stub(&record_now);
    esp_wifi_clear_ap_list_IgnoreAndReturn(ESP_OK);

    TEST_ASSERT_EQUAL_INT(want, kdrv_wifi_scan());
    return s_steps;
}

/* ---- What the bus and the askers heard -------------------------------------------- */

/** @brief Everything a case reads back, counted, the last of each kept. */
typedef struct {
    unsigned       entries;     /**< WIFI_SCAN_ENTRY_SIG.                  */
    unsigned       ends;        /**< WIFI_SCAN_END_SIG.                    */
    unsigned       agains;      /**< WIFI_SCAN_AGAIN_SIG.                  */
    unsigned       ble_ends;    /**< BLE_SCAN_END_SIG.                     */
    unsigned       rsps;        /**< WIFI_SCAN_RSP_SIG.                    */
    unsigned       ble_rsps;    /**< BLE_SCAN_RSP_SIG.                     */
    WifiScanEndEvt end;         /**< The last Wi-Fi end.                   */
    WifiScanEndEvt again;       /**< The last restart.                     */
    WifiScanEndEvt ble_end;     /**< The last BLE end.                     */
    ScanRspEvt     rsp;         /**< The last asked Wi-Fi scan's answer.   */
    ScanRspEvt     ble_rsp;     /**< The last asked listen's answer.       */
} heard_t;

/** @brief Count one event into @p h. */
static void heard_note(heard_t *h, const QEvt *e)
{
    if ( (QSignal)WIFI_SCAN_ENTRY_SIG == e->sig )
    {
        ++h->entries;
    }
    else if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
    {
        ++h->ends;
        h->end = *(const WifiScanEndEvt *)(const void *)e;
    }
    else if ( (QSignal)WIFI_SCAN_AGAIN_SIG == e->sig )
    {
        ++h->agains;
        h->again = *(const WifiScanEndEvt *)(const void *)e;
    }
    else if ( (QSignal)BLE_SCAN_END_SIG == e->sig )
    {
        ++h->ble_ends;
        h->ble_end = *(const WifiScanEndEvt *)(const void *)e;
    }
    else if ( (QSignal)WIFI_SCAN_RSP_SIG == e->sig )
    {
        ++h->rsps;
        h->rsp = *(const ScanRspEvt *)(const void *)e;
    }
    else if ( (QSignal)BLE_SCAN_RSP_SIG == e->sig )
    {
        ++h->ble_rsps;
        h->ble_rsp = *(const ScanRspEvt *)(const void *)e;
    }
    else
    {
        /* a state, an entry of a listen: not what these cases judge */
    }
}

/** @brief Read everything the spy holds. */
static heard_t heard(void)
{
    heard_t     h;
    const QEvt *e;

    (void)memset(&h, 0, sizeof(h));
    while ( (e = ao_spy_take()) != NULL )
    {
        heard_note(&h, e);
    }
    return h;
}

/** @brief Read everything the second asker holds. */
static heard_t heard_other(void)
{
    heard_t     h;
    const QEvt *e;

    (void)memset(&h, 0, sizeof(h));
    while ( (e = ao_spy_other_take()) != NULL )
    {
        heard_note(&h, e);
        QF_gc(e);   /* the second asker hands its events over, to be recycled */
    }
    return h;
}

/* ---- Fixture ---------------------------------------------------------------------- */

/** @brief A link that is up, no service wanted, nothing in flight, the spy listening. */
static void newest_up(void)
{
    fake_nvs_reset();
    radio_fresh();
    fake_ble_reset();
    kdrv_wifi_scan_abort(false);
    ao_spy_subscribe(WIFI_SCAN_AGAIN_SIG);
    go_up();
    ao_spy_other_drain();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a job runs before the case asks");
}

/** @brief Leave nothing in flight and no cut standing. */
static void newest_end(void)
{
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    kdrv_wifi_scan_abort(false);
    fake_ble_reset();
    fake_nvs_reset();
    ao_spy_drain();
    ao_spy_other_drain();
}

/** @brief Ask for the console's Wi-Fi listing and let the AO read it. */
static void ask_listing(void)
{
    (void)wifi_scan_request(NULL);
    pump();
}

/** @brief Ask for the console's BLE listing of @p ms and let the AO read it. */
static void ask_listen(uint32_t ms)
{
    TEST_ASSERT_TRUE(ble_scan_request(ms, NULL));
    pump();
}

/** @brief End the listen the worker holds as the driver would, cut or whole. */
static void finish_listen(void)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    finish(KDRV_RADIO_JOB_BLE_SCAN, kdrv_ble_scan(held_req()->len));
}

/** @brief A blocked task's waiter over @p rows. */
static void waiter_make(ScanWaiter *w, kdrv_wifi_ap_t *rows)
{
    (void)memset(w, 0, sizeof(*w));
    w->done = khal_sem_create();
    w->rows = rows;
    w->cap  = (uint8_t)KDRV_WIFI_SCAN_MAX;
    w->rc   = (uint8_t)KODE_TIMEOUT;
}

/* ---- The Wi-Fi listing ------------------------------------------------------------- */

TEST_CASE("a second wifi_scan ends the sweep in flight and the listing starts again",
          "[radio_ao][newest]")
{
    heard_t  h;
    unsigned before;

    newest_up();
    before = job_submits();
    ask_listing();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_TRUE(1U < sweep(2U, KODE_OK));
    pump();

    ask_listing();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(2U, h.entries);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.agains, "the listeners were not told the listing starts again");
    TEST_ASSERT_TRUE(h.again.cut);
    TEST_ASSERT_FALSE(h.again.ok);
    TEST_ASSERT_FALSE(h.again.busy);
    TEST_ASSERT_EQUAL_UINT8(2U, h.again.count);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "the cut sweep closed the listing the new one continues");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sweep(2U, KODE_DENIED), "the sweep in flight was not asked to end");

    /* The cut sweep comes back: nothing of it is published, and the new one starts. */
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(0U, h.entries);
    TEST_ASSERT_EQUAL_UINT(0U, h.ends);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT(before + 2U, job_submits());

    TEST_ASSERT_TRUE_MESSAGE(1U < sweep(3U, KODE_OK), "the cut outlived the sweep it was for");
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(3U, h.entries);
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    TEST_ASSERT_FALSE(h.end.cut);
    TEST_ASSERT_EQUAL_UINT8(3U, h.end.count);
    TEST_ASSERT_FALSE(job_held());
    newest_end();
}

TEST_CASE("three wifi_scans in a row run one new sweep, not two",
          "[radio_ao][newest]")
{
    heard_t  h;
    unsigned before;

    newest_up();
    before = job_submits();
    ask_listing();
    ask_listing();
    ask_listing();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(0U, h.ends);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.agains, "a listing that waits has nothing to start again");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    TEST_ASSERT_FALSE(job_held());
    TEST_ASSERT_EQUAL_UINT(before + 2U, job_submits());
    newest_end();
}

TEST_CASE("a join during a listed scan closes the listing at once and is made next",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    (void)sweep(2U, KODE_OK);
    pump();

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(2U, h.entries);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ends, "the listing waits for the worker to be told it was cut");
    TEST_ASSERT_TRUE(h.end.cut);
    TEST_ASSERT_FALSE(h.end.ok);
    TEST_ASSERT_EQUAL_UINT8(2U, h.end.count);
    TEST_ASSERT_EQUAL_UINT(0U, h.agains);

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "the cut sweep closed the listing a second time");
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_JOIN, running_job());
    TEST_ASSERT_EQUAL_STRING("net", held_req()->ssid);
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    newest_end();
}

TEST_CASE("wifi_down during a listed scan closes the listing at once and Wi-Fi stops next",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    (void)sweep(1U, KODE_OK);
    pump();

    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.cut);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sweep(1U, KODE_DENIED), "the sweep in flight was not asked to end");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_STOP, running_job());
    finish(KDRV_RADIO_JOB_WIFI_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(0U, heard().ends);
    newest_end();
}

TEST_CASE("a join after a second wifi_scan ends the listing that waits and is made alone",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    ask_listing();                       /* the first is cut, the second waits */
    TEST_ASSERT_EQUAL_UINT(0U, heard().ends);

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ends, "the listing that waited was left open");
    TEST_ASSERT_TRUE(h.end.cut);

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_JOIN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "the listing the join ended ran all the same");
    TEST_ASSERT_EQUAL_UINT(0U, heard().ends);
    newest_end();
}

TEST_CASE("a listing that waits is closed when the link is lost",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    ask_listing();
    TEST_ASSERT_EQUAL_UINT(0U, heard().ends);

    finish_lost(KODE_ERROR);
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ends, "the listing that waited was left open");
    TEST_ASSERT_FALSE(h.end.ok);
    TEST_ASSERT_FALSE(h.end.busy);
    TEST_ASSERT_TRUE_MESSAGE(1U < sweep(1U, KODE_OK), "the cut outlived the link");
    newest_end();
}

TEST_CASE("a wifi_scan during a join waits for it, as an asked scan does, and runs next",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_JOIN, running_job());

    ask_listing();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "the scan was turned away behind a join");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_JOIN, running_job(), "the join was cut");

    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    newest_end();
}

TEST_CASE("the board's own round does not cut a listed scan: it waits for its end",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    TEST_ASSERT_TRUE(wifi_auto_request());
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(0U, h.ends);
    TEST_ASSERT_EQUAL_UINT(0U, h.agains);
    TEST_ASSERT_TRUE_MESSAGE(1U < sweep(1U, KODE_OK), "the round cut the scan somebody asked for");
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    newest_end();
}

/* ---- The BLE listing --------------------------------------------------------------- */

TEST_CASE("a second ble_scan ends the listen in flight and a new one runs its own length",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listen(LISTEN_MS);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT32(LISTEN_MS, held_req()->len);

    ask_listen(LISTEN_NEWER_MS);
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ble_ends, "the newer listen was answered busy, or the cut one closed the listing");

    /* The driver ends the listen in flight cut; nothing of it is published. */
    fake_ble_seed_air(2U, -60);
    finish_listen();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(0U, h.ble_ends);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());
    TEST_ASSERT_EQUAL_UINT32(LISTEN_NEWER_MS, held_req()->len);

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kdrv_ble_scan(LISTEN_NEWER_MS), "the cut outlived the listen it was for");
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ble_ends);
    TEST_ASSERT_TRUE(h.ble_end.ok);
    TEST_ASSERT_EQUAL_UINT8(2U, h.ble_end.count);
    TEST_ASSERT_FALSE(job_held());
    newest_end();
}

TEST_CASE("a second ble_scan asks the driver to end the listen in flight",
          "[radio_ao][newest]")
{
    newest_up();
    ask_listen(LISTEN_MS);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_ble_scan(LISTEN_MS));

    ask_listen(LISTEN_NEWER_MS);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kdrv_ble_scan(LISTEN_MS),
                                  "the listen in flight was not asked to end");
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_DENIED);
    finish(KDRV_RADIO_JOB_BLE_SCAN, KODE_OK);
    newest_end();
}

TEST_CASE("a wifi_scan during a ble_scan is answered busy: it ends no Bluetooth listen",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listen(LISTEN_MS);
    ask_listing();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ble_ends, "a Wi-Fi request ended the Bluetooth listen");
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.busy);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kdrv_ble_scan(LISTEN_MS), "the listen was asked to end");

    fake_ble_seed_air(1U, -60);
    finish_listen();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ble_ends);
    TEST_ASSERT_TRUE(h.ble_end.ok);
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "the sweep answered busy ran all the same");
    newest_end();
}

TEST_CASE("a ble_scan during a wifi_scan is answered busy: it ends no Wi-Fi sweep",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    ask_listen(LISTEN_MS);
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "a Bluetooth request ended the Wi-Fi sweep");
    TEST_ASSERT_EQUAL_UINT(1U, h.ble_ends);
    TEST_ASSERT_TRUE(h.ble_end.busy);
    TEST_ASSERT_TRUE_MESSAGE(1U < sweep(1U, KODE_OK), "the sweep was asked to end");
    pump();

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "the listen answered busy ran all the same");
    newest_end();
}

TEST_CASE("ble_down during a ble_scan closes the listen at once and Bluetooth goes quiet next",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listen(LISTEN_MS);
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ble_ends);
    TEST_ASSERT_TRUE(h.ble_end.cut);

    finish_listen();
    TEST_ASSERT_EQUAL_UINT(0U, heard().ble_ends);
    while ( job_held() )
    {
        TEST_ASSERT_NOT_EQUAL_MESSAGE((int)KDRV_RADIO_JOB_BLE_SCAN, running_job(),
                                      "a listen ran after Bluetooth was switched off");
        finish_whatever_runs();
    }
    newest_end();
}

/* ---- The asked scan ---------------------------------------------------------------- */

TEST_CASE("a second asker's scan ends the first asker's at once, cancelled, and runs",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    (void)sweep(2U, KODE_OK);
    pump();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy_other()));
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(2U, h.entries);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.rsps, "the first asker was left waiting");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CANCELLED, h.rsp.rc);
    TEST_ASSERT_EQUAL_UINT8(2U, h.rsp.seen);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard_other().rsps, "the newer asker was refused");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, sweep(1U, KODE_DENIED), "the sweep in flight was not asked to end");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard().rsps, "the first asker was answered twice");
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    (void)sweep(3U, KODE_OK);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    /* The second asker's queue is shorter than the slots an entry leaves
     * free: its sweep reaches it as the answer, which counts the networks. */
    h = heard_other();
    TEST_ASSERT_EQUAL_UINT(1U, h.rsps);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, h.rsp.rc);
    TEST_ASSERT_EQUAL_UINT8(3U, h.rsp.seen);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard().entries, "the newer asker's networks went to the first");
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_wifi_scan(ao_spy()), "the first asker's door stayed shut");
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    newest_end();
}

TEST_CASE("a wifi_scan ends a task's scan at once, cancelled, with no rows",
          "[radio_ao][newest]")
{
    kdrv_wifi_ap_t rows[KDRV_WIFI_SCAN_MAX];
    ScanWaiter     w;
    heard_t        h;
    unsigned       signals;

    newest_up();
    waiter_make(&w, rows);
    wifi_scan_request_wait(&w, 5U);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    (void)sweep(2U, KODE_OK);
    pump();
    signals = fake_sem_signals();

    ask_listing();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(signals + 1U, fake_sem_signals(), "the task was left blocked");
    TEST_ASSERT_EQUAL_UINT32(5U, w.ack);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CANCELLED, w.rc);
    TEST_ASSERT_EQUAL_UINT8(0U, w.got);
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "the listing was answered busy, or closed by the task's sweep");
    TEST_ASSERT_EQUAL_UINT(0U, h.entries);

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(signals + 1U, fake_sem_signals(), "the task was answered twice");
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    (void)sweep(1U, KODE_OK);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.entries);
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    newest_end();
}

TEST_CASE("an asked scan closes the listed scan at once and runs next",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ends, "the listing was not told it ended early");
    TEST_ASSERT_TRUE(h.end.cut);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.rsps, "the asked scan was refused");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    (void)sweep(2U, KODE_OK);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(2U, h.entries);
    TEST_ASSERT_EQUAL_UINT(1U, h.rsps);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, h.rsp.rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "the asked scan's end was published");
    newest_end();
}

TEST_CASE("a join ends no Bluetooth listen: it waits for the asked one and is made next",
          "[radio_ao][newest]")
{
    heard_t  h;
    unsigned guard = 0U;

    newest_up();
    /* Bluetooth on in Settings: an asked listen is refused without it. */
    dispatch(BLE_START_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_START, running_job());
    finish(KDRV_RADIO_JOB_BLE_START, kdrv_ble_start());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    ao_spy_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_scan(ao_spy(), LISTEN_MS));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_SCAN, running_job());

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard().ble_rsps, "a Wi-Fi request ended the Bluetooth listen");

    fake_ble_seed_air(1U, -60);
    finish_listen();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ble_rsps);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, h.ble_rsp.rc);
    /* The advert the listen held back may go first; the join follows. */
    while ( (running_job() != (int)KDRV_RADIO_JOB_WIFI_JOIN) && (4U > guard) )
    {
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_BLE_ADV, running_job(),
                                      "something other than the advert went before the join");
        finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
        ++guard;
    }
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_JOIN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    if ( running_job() == (int)KDRV_RADIO_JOB_BLE_QUIET )
    {
        finish(KDRV_RADIO_JOB_BLE_QUIET, kdrv_ble_quiet());
    }
    newest_end();
}

TEST_CASE("a newer asked scan ends one that still waits for the link, and runs when it is up",
          "[radio_ao][newest]")
{
    kdrv_wifi_ap_t rows[KDRV_WIFI_SCAN_MAX];
    ScanWaiter     w;
    heard_t        h;
    unsigned       guard = 0U;

    fake_nvs_reset();
    radio_fresh();
    fake_ble_reset();
    kdrv_wifi_scan_abort(false);
    ao_spy_other_drain();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_CONNECT, running_job());

    waiter_make(&w, rows);
    wifi_scan_request_wait(&w, 9U);
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.rsps, "the asker that waited was left waiting");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CANCELLED, h.rsp.rc);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_TIMEOUT, w.rc, "the newer scan was answered before it ran");

    finish(KDRV_RADIO_JOB_CONNECT, KODE_OK);
    pump();
    /* Whatever the bring-up reconciles goes first; the scan waits its turn. */
    while ( (running_job() != (int)KDRV_RADIO_JOB_WIFI_SCAN) && (8U > guard) )
    {
        TEST_ASSERT_TRUE_MESSAGE(job_held(), "the newer scan never started");
        finish_whatever_runs();
        ++guard;
    }
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    (void)sweep(1U, KODE_OK);
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    TEST_ASSERT_EQUAL_UINT32(9U, w.ack);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, w.rc);
    TEST_ASSERT_EQUAL_UINT8(1U, w.got);
    newest_end();
}

/* ---- One radio's request ends that radio's scan and no other ----------------------- */

TEST_CASE("an asked listen while a Wi-Fi listing runs is refused: it ends no sweep",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    dispatch(BLE_START_REQ_SIG);
    finish(KDRV_RADIO_JOB_BLE_START, kdrv_ble_start());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    ao_spy_drain();
    ask_listing();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_ble_scan(ao_spy(), LISTEN_MS));
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ble_rsps, "the listen was owed behind the sweep");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, h.ble_rsp.rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "a Bluetooth request ended the Wi-Fi sweep");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    if ( running_job() == (int)KDRV_RADIO_JOB_BLE_QUIET )
    {
        finish(KDRV_RADIO_JOB_BLE_QUIET, kdrv_ble_quiet());
    }
    newest_end();
}

TEST_CASE("wifi_up from off ends a listed scan; asked again with Wi-Fi on it ends none",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    dispatch(WIFI_START_REQ_SIG);
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ends, "Wi-Fi switched on did not end the scan");
    TEST_ASSERT_TRUE(h.end.cut);
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    for ( unsigned guard = 0U; job_held() && (8U > guard); ++guard )
    {
        finish_whatever_runs();
    }
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "the round Wi-Fi's switch asked for never ended");
    ao_spy_drain();

    /* Wi-Fi is on now: the same request changes nothing a scan gives way to. */
    ask_listing();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard().ends, "a second wifi_up ended the scan");
    TEST_ASSERT_TRUE_MESSAGE(1U < sweep(1U, KODE_OK), "the sweep was asked to end");
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    newest_end();
}

TEST_CASE("ble_up from off ends a ble_scan",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listen(LISTEN_MS);
    dispatch(BLE_START_REQ_SIG);
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ble_ends, "Bluetooth switched on did not end the listen");
    TEST_ASSERT_TRUE(h.ble_end.cut);
    finish_listen();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, heard().ble_ends, "the cut listen closed the listing twice");
    dispatch(BLE_STOP_REQ_SIG);
    pump();
    for ( unsigned guard = 0U; job_held() && (8U > guard); ++guard )
    {
        if ( running_job() == (int)KDRV_RADIO_JOB_BLE_ADV )
        {
            finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
        }
        else
        {
            finish_whatever_runs();
        }
    }
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "Bluetooth never came to rest");
    newest_end();
}

/* ---- What waits, and what a waiting scan is told ------------------------------------ */

TEST_CASE("wifi_scan_stop closes a listing that waits for the worker",
          "[radio_ao][newest]")
{
    heard_t h;

    newest_up();
    ask_listing();
    ask_listing();                       /* the first is cut, the second waits */
    (void)heard();

    TEST_ASSERT_TRUE(wifi_scan_stop_request());
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ends, "the listing that waited was left open");
    TEST_ASSERT_TRUE(h.end.cut);

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    TEST_ASSERT_FALSE_MESSAGE(job_held(), "the stopped listing ran all the same");
    TEST_ASSERT_EQUAL_UINT(0U, heard().ends);
    newest_end();
}

TEST_CASE("with the board's own round owed a scan ends nothing: it is told to ask again",
          "[radio_ao][newest]")
{
    uint8_t first  = 0U;
    uint8_t second = 0U;
    heard_t h;

    newest_up();
    TEST_ASSERT_TRUE(wifi_scan_request(&first));
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_TRUE(wifi_auto_request());   /* owed behind the sweep; it cuts nothing */
    pump();
    (void)heard();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_scan(ao_spy()));
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.rsps, "the asked scan was owed behind a round that refuses it");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_NO_RESOURCES, h.rsp.rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, h.ends, "the scan that was refused ended the one in flight");

    /* The refusal names the request it refuses: the listing in flight is
     * somebody else's, and its listener must not take the refusal for its end. */
    TEST_ASSERT_TRUE(wifi_scan_request(&second));
    pump();
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE_MESSAGE(h.end.busy, "a listing behind an owed round was not answered busy");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(second, h.end.gen, "the refusal does not name its request");
    TEST_ASSERT_EQUAL_UINT(0U, h.agains);
    TEST_ASSERT_TRUE_MESSAGE(1U < sweep(1U, KODE_OK), "the sweep in flight was asked to end");
    pump();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_TRUE(h.end.ok);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(first, h.end.gen, "the listing lost its own generation to a refusal");
    newest_end();
}

/* ---- The doors ---------------------------------------------------------------------- */

TEST_CASE("a burst of scan requests nobody reads is refused past the margin, never asserted",
          "[radio_ao][newest]")
{
    unsigned taken = 0U;
    unsigned room;
    heard_t  h;

    newest_up();
    /* One request more than the queue has room for: the doors stop at their
     * margin, whatever depth the harness gave the queue. */
    room = (unsigned)uxQueueSpacesAvailable(AO_Radio->eQueue);
    TEST_ASSERT_TRUE(room > KQP_RADIO_POST_MARGIN);
    for ( unsigned i = 0U; room >= i; ++i )
    {
        if ( wifi_scan_request(NULL) )
        {
            ++taken;
        }
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(room - KQP_RADIO_POST_MARGIN, taken,
                                   "the door did not stop at its margin");
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    for ( unsigned i = 0U; room >= i; ++i )
    {
        TEST_ASSERT_FALSE_MESSAGE(ble_scan_request(LISTEN_MS, NULL),
                                  "a listen took a slot kept for the worker and the link");
    }
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));

    /* Read, the burst is one sweep in flight and at most one that waits. */
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    if ( running_job() == (int)KDRV_RADIO_JOB_WIFI_SCAN )
    {
        finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    }
    h = heard();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, h.ends, "the burst's listing was not closed once");
    TEST_ASSERT_FALSE(job_held());
    newest_end();
}

TEST_CASE("a listing's entries and end carry the generation of the newest request",
          "[radio_ao][newest]")
{
    uint8_t first  = 0U;
    uint8_t second = 0U;
    heard_t h;

    newest_up();
    TEST_ASSERT_TRUE(wifi_scan_request(&first));
    pump();
    TEST_ASSERT_NOT_EQUAL((uint8_t)KQP_SCAN_GEN_NONE, first);
    TEST_ASSERT_TRUE(wifi_scan_request(&second));
    pump();
    TEST_ASSERT_NOT_EQUAL(first, second);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.agains);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(second, h.again.gen, "the restart does not name the request it is for");

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_DENIED);
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);
    h = heard();
    TEST_ASSERT_EQUAL_UINT(1U, h.ends);
    TEST_ASSERT_EQUAL_UINT8(second, h.end.gen);
    newest_end();
}

