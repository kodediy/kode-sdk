/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_probe.c
 * @brief AO_Radio's radio check: the worker jobs behind radio_probe_request() and its answer.
 */
#include <stdio.h>
#include <string.h>

#include "radio_harness.h"

/** @brief The names of the networks and the beacon the cases look for. */
#define NET24   "line-24"
#define NET5    "line-5"
#define BEACON  "KODE-BEACON"

/** @brief The channels they are on. */
#define CH24    6U
#define CH5     36U

/** @brief The readings the fake driver reports. */
#define RSSI_24     (-60)
#define RSSI_5      (-70)
#define RSSI_BLE    (-55)

/** @brief The esp-hosted version the fake C5 reports. */
#define FW_MAJOR_WANTED  1U

/** @brief More time than the check's budget, ms. */
#define PAST_BUDGET_MS   (KODE_RADIO_PROBE_MS + 1000U)

/** @brief Leave the AO off and its neighbours' mailboxes empty. */
static void probe_case_end(void)
{
    settle_to_off();
    storage_drain();
    net_drain();
    (void)boot_parked_taken();
    ao_spy_drain();
    kdrv_wifi_scan_abort(false);
}

/** @brief A link that is up, nothing wanted, the spy listening. */
static void probe_up(void)
{
    radio_fresh();
    fake_ble_reset();
    fake_time_reset();
    ao_spy_subscribe(WIFI_SCAN_END_SIG);
    go_up();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a job runs before the check is asked");
}

/** @brief Ask for a check and let the AO take it. */
static bool ask(const radio_probe_t *req)
{
    bool const posted = radio_probe_request(ao_spy(), req);

    pump();
    return posted;
}

/** @brief The check's answer in the spy, or a failure. */
static const RadioProbeEvt *answer(void)
{
    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();

        TEST_ASSERT_NOT_NULL_MESSAGE(e, "the check was never answered");
        if ( (QSignal)RADIO_PROBE_SIG == e->sig )
        {
            return (const RadioProbeEvt *)e;
        }
    }
}

/** @brief How many answers the spy holds; drains it. */
static unsigned answers(void)
{
    unsigned n = 0U;

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();

        if ( NULL == e )
        {
            break;
        }
        if ( (QSignal)RADIO_PROBE_SIG == e->sig )
        {
            n += 1U;
        }
    }
    return n;
}

/** @brief A request for both networks, the beacon and the card. */
static radio_probe_t full_request(void)
{
    radio_probe_t r;

    (void)memset(&r, 0, sizeof(r));
    (void)snprintf(r.ssid24, sizeof(r.ssid24), "%s", NET24);
    r.ch24 = CH24;
    (void)snprintf(r.ssid5, sizeof(r.ssid5), "%s", NET5);
    r.ch5 = CH5;
    (void)snprintf(r.ble, sizeof(r.ble), "%s", BEACON);
    r.card = true;
    return r;
}

/** @brief The driver's table after a probe pass: what it heard of the first network. */
static void leave_heard(bool seen, int8_t rssi, uint8_t ch, bool passive)
{
    kdrv_radio_probe_result_t res;

    (void)memset(&res, 0, sizeof(res));
    res.net[0].seen    = seen;
    res.net[0].rssi    = rssi;
    res.net[0].channel = ch;
    res.passive        = passive;
    fake_radio_set_probe_result(&res);
}

/** @brief The driver's table after a BLE find. */
static void leave_found(bool found, int8_t rssi)
{
    kdrv_radio_probe_result_t res;

    (void)memset(&res, 0, sizeof(res));
    res.found    = found;
    res.ble_rssi = rssi;
    fake_radio_set_probe_result(&res);
}

/** @brief End the card's job the way the worker does. */
static void finish_card(void)
{
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    finish(KDRV_RADIO_JOB_C5_INFO, kdrv_c5_refresh());
}

/* ---- The sweep the mocked stack runs --------------------------------------- */

static unsigned      s_sweep_starts;   /**< Steps the real sweep has started. */
static kode_status_t s_sweep_rc;       /**< What the real sweep answered.     */

static esp_err_t sweep_start(const wifi_scan_config_t *config, bool block, int calls)
{
    (void)config; (void)block; (void)calls;
    ++s_sweep_starts;
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    return ESP_OK;
}

static esp_err_t sweep_num(uint16_t *number, int calls)
{
    (void)calls;
    *number = 0U;
    return ESP_OK;
}

static esp_err_t sweep_clear(int calls)
{
    (void)calls;
    return ESP_OK;
}

static esp_err_t sweep_country(wifi_country_t *country, int calls)
{
    (void)calls;
    (void)memset(country, 0, sizeof(*country));
    country->schan = 1U;
    country->nchan = 13U;
    return ESP_OK;
}

/** @brief Run the driver's real sweep as the worker would; the steps it started.
 *         What it answered is in s_sweep_rc. */
static unsigned run_real_sweep(void)
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
    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_OK);
    esp_wifi_get_country_Stub(&sweep_country);
    esp_wifi_scan_start_Stub(&sweep_start);
    esp_wifi_scan_get_ap_num_Stub(&sweep_num);
    esp_wifi_clear_ap_list_Stub(&sweep_clear);
    s_sweep_starts = 0U;
    s_sweep_rc     = kdrv_wifi_scan();
    return s_sweep_starts;
}

/* ---- The cases --------------------------------------------------------------- */

TEST_CASE("test_probe_runs_card_networks_and_find_in_order", "[radio_probe]")
{
    probe_up();
    radio_probe_t const req = full_request();

    TEST_ASSERT_TRUE(ask(&req));

    finish_card();

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());
    TEST_ASSERT_EQUAL_STRING(NET24, held_req()->ssid);
    TEST_ASSERT_EQUAL_UINT8(CH24, held_req()->channel);
    TEST_ASSERT_EQUAL_UINT16(KODE_RADIO_PROBE_DWELL_MS, held_req()->dwell_ms);
    leave_heard(true, (int8_t)RSSI_24, CH24, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());
    TEST_ASSERT_EQUAL_STRING(NET5, held_req()->ssid);
    TEST_ASSERT_EQUAL_UINT8(CH5, held_req()->channel);
    TEST_ASSERT_EQUAL_UINT16(KODE_RADIO_PROBE_PDWELL_MS, held_req()->dwell_ms);
    leave_heard(true, (int8_t)RSSI_5, CH5, true);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_FIND, running_job());
    TEST_ASSERT_EQUAL_STRING(BEACON, held_req()->ssid);
    TEST_ASSERT_EQUAL_UINT32(KODE_RADIO_FIND_MS, held_req()->len);
    leave_found(true, (int8_t)RSSI_BLE);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);

    TEST_ASSERT_EQUAL_INT(-1, running_job());
    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_TRUE(a->card);
    TEST_ASSERT_EQUAL_UINT16(23U, a->chip);
    TEST_ASSERT_EQUAL_UINT8(0x38U, a->mac[0]);
    TEST_ASSERT_EQUAL_UINT8(0x7EU, a->mac[5]);
    TEST_ASSERT_TRUE(a->seen24);
    TEST_ASSERT_EQUAL_INT8((int8_t)RSSI_24, a->rssi24);
    TEST_ASSERT_EQUAL_UINT8(CH24, a->ch24);
    TEST_ASSERT_TRUE(a->seen5);
    TEST_ASSERT_EQUAL_INT8((int8_t)RSSI_5, a->rssi5);
    TEST_ASSERT_TRUE(a->passive5);
    TEST_ASSERT_TRUE(a->ble_rx);
    TEST_ASSERT_EQUAL_INT8((int8_t)RSSI_BLE, a->rssi_rx);
    probe_case_end();
}

TEST_CASE("a network not heard is tried again, and then given up on", "[radio_probe]")
{
    radio_probe_t req;

    probe_up();
    (void)memset(&req, 0, sizeof(req));
    (void)snprintf(req.ssid24, sizeof(req.ssid24), "%s", NET24);
    req.ch24 = CH24;
    TEST_ASSERT_TRUE(ask(&req));

    for ( unsigned pass = 0U; KODE_RADIO_PROBE_TRIES > pass; ++pass )
    {
        TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());
        leave_heard(false, 0, 0U, false);
        finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    }
    TEST_ASSERT_EQUAL_INT(-1, running_job());
    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_FALSE(a->seen24);
    TEST_ASSERT_EQUAL_UINT8(CH24, a->ch24);
    probe_case_end();
}

TEST_CASE("a network heard on the second pass is not probed a third time", "[radio_probe]")
{
    radio_probe_t req;

    probe_up();
    (void)memset(&req, 0, sizeof(req));
    (void)snprintf(req.ssid24, sizeof(req.ssid24), "%s", NET24);
    req.ch24 = CH24;
    TEST_ASSERT_TRUE(ask(&req));

    leave_heard(false, 0, 0U, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());
    leave_heard(true, (int8_t)RSSI_24, CH24, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);

    TEST_ASSERT_EQUAL_INT(-1, running_job());
    TEST_ASSERT_TRUE(answer()->seen24);
    probe_case_end();
}

TEST_CASE("a job that fails is said in the status and the rest still runs", "[radio_probe]")
{
    probe_up();
    radio_probe_t req = full_request();

    req.card = false;
    TEST_ASSERT_TRUE(ask(&req));

    leave_heard(false, 0, 0U, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_DENIED);       /* a channel the country closes */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());
    TEST_ASSERT_EQUAL_STRING(NET5, held_req()->ssid);     /* no second pass after a refusal */
    leave_heard(true, (int8_t)RSSI_5, CH5, true);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    leave_found(false, 0);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);

    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->status);
    TEST_ASSERT_TRUE(a->seen5);
    TEST_ASSERT_FALSE(a->ble_rx);
    TEST_ASSERT_FALSE(a->card);
    probe_case_end();
}

TEST_CASE("test_probe_with_the_link_down_answers_not_ready", "[radio_probe]")
{
    radio_fresh();                       /* off: the check never brings the link up */
    radio_probe_t const req = full_request();
    unsigned const      before = job_submits();

    TEST_ASSERT_TRUE(ask(&req));

    TEST_ASSERT_EQUAL_INT(-1, running_job());
    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->status);
    TEST_ASSERT_FALSE(a->seen24);
    TEST_ASSERT_EQUAL_UINT(before, job_submits());
    probe_case_end();
}

TEST_CASE("the link lost mid-check ends it with what it has", "[radio_probe]")
{
    probe_up();
    radio_probe_t const req = full_request();

    TEST_ASSERT_TRUE(ask(&req));
    finish_card();
    leave_heard(true, (int8_t)RSSI_24, CH24, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_PROBE, running_job());

    finish_lost(KODE_ERROR);

    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->status);
    TEST_ASSERT_TRUE(a->card);
    TEST_ASSERT_TRUE(a->seen24);
    TEST_ASSERT_EQUAL_UINT(0U, answers());
    probe_case_end();
}

TEST_CASE("test_second_probe_while_one_runs_is_busy", "[radio_probe]")
{
    probe_up();
    radio_probe_t const req = full_request();

    TEST_ASSERT_TRUE(ask(&req));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());

    TEST_ASSERT_TRUE(ask(&req));
    const RadioProbeEvt * const refused = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_NO_RESOURCES, refused->status);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_C5_INFO, running_job(),
                                  "the second request disturbed the first");
    ao_spy_release();

    /* The first goes on to its end, answered once. */
    finish_card();
    leave_heard(true, (int8_t)RSSI_24, CH24, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    leave_heard(true, (int8_t)RSSI_5, CH5, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    leave_found(true, (int8_t)RSSI_BLE);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, answer()->status);
    probe_case_end();
}

TEST_CASE("test_probe_cuts_a_running_sweep", "[radio_probe]")
{
    probe_up();
    fake_ble_reset();

    /* A console sweep holds the channel. */
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    radio_probe_t const req = full_request();
    TEST_ASSERT_TRUE(ask(&req));
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job(),
                                  "the check waits for the sweep, it does not replace it");
    TEST_ASSERT_EQUAL_UINT(0U, answers());

    /* The sweep, run by the worker, ends before its first step, and says
     * it was cut. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, run_real_sweep(),
                                   "the sweep was not asked to end");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, s_sweep_rc);
    finish(KDRV_RADIO_JOB_WIFI_SCAN, s_sweep_rc);

    /* Its owner is told it was cut, not that it ended, and the check begins. */
    bool ended = false;
    bool ok    = true;

    for ( ;; )
    {
        const QEvt * const e = ao_spy_take();

        if ( NULL == e )
        {
            break;
        }
        if ( (QSignal)WIFI_SCAN_END_SIG == e->sig )
        {
            ended = true;
            ok    = ((const WifiScanEndEvt *)e)->ok;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(ended, "the sweep's owner was not told it ended");
    TEST_ASSERT_FALSE_MESSAGE(ok, "a cut sweep reached its owner as complete");
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());

    /* The next sweep runs whole: the request to stop was only for that one. */
    TEST_ASSERT_TRUE_MESSAGE(run_real_sweep() > 1U, "the stop outlived the sweep it was for");
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_sweep_rc);

    finish_card();
    leave_heard(true, (int8_t)RSSI_24, CH24, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    leave_heard(true, (int8_t)RSSI_5, CH5, false);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);
    leave_found(false, 0);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, answer()->status);
    probe_case_end();
}

TEST_CASE("test_probe_keeps_advertising_on", "[radio_probe]")
{
    probe_up();
    fake_ble_reset();

    /* Bluetooth is wanted and the board is on the air. */
    dispatch(BLE_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_START, running_job());
    finish(KDRV_RADIO_JOB_BLE_START, kdrv_ble_start());
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_ADVERTISING, (int)kdrv_ble_state());
    ao_spy_drain();

    radio_probe_t req;
    (void)memset(&req, 0, sizeof(req));
    (void)snprintf(req.ble, sizeof(req.ble), "%s", BEACON);
    TEST_ASSERT_TRUE(ask(&req));

    /* The listen takes the advert off the air, as a scan does. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_FIND, running_job());
    (void)kdrv_ble_advertise(false);
    leave_found(true, (int8_t)RSSI_BLE);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);

    /* It goes back before the answer, and the answer says so. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_ADV, running_job());
    TEST_ASSERT_EQUAL_UINT(0U, answers());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_TRUE(a->ble_adv);
    TEST_ASSERT_TRUE(a->ble_rx);

    dispatch(BLE_STOP_REQ_SIG);
    pump();
    probe_case_end();
}

TEST_CASE("a check that runs out of time answers with what it has", "[radio_probe]")
{
    probe_up();
    radio_probe_t const req = full_request();

    TEST_ASSERT_TRUE(ask(&req));
    finish_card();
    leave_heard(true, (int8_t)RSSI_24, CH24, false);
    fake_time_advance_ms(PAST_BUDGET_MS);
    finish(KDRV_RADIO_JOB_WIFI_PROBE, KODE_OK);

    TEST_ASSERT_EQUAL_INT(-1, running_job());
    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_TIMEOUT, a->status);
    TEST_ASSERT_TRUE(a->card);
    TEST_ASSERT_TRUE(a->seen24);
    TEST_ASSERT_FALSE(a->seen5);
    probe_case_end();
}

#if CONFIG_KODEOS_WIFI
TEST_CASE("discover sweeps once and reports where the two networks were heard", "[radio_probe]")
{
    radio_probe_t req;

    probe_up();
    (void)memset(&req, 0, sizeof(req));
    (void)snprintf(req.ssid24, sizeof(req.ssid24), "%s", NET24);
    (void)snprintf(req.ssid5, sizeof(req.ssid5), "%s", NET5);
    req.discover = true;
    TEST_ASSERT_TRUE(ask(&req));

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    TEST_ASSERT_TRUE(run_real_sweep() > 1U);    /* the table is the sweep's: an empty room */
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_sweep_rc);
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);

    TEST_ASSERT_EQUAL_INT(-1, running_job());
    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_UINT8(0U, a->ch24);       /* an empty room: heard nowhere */
    TEST_ASSERT_FALSE(a->seen24);
    TEST_ASSERT_FALSE(a->seen5);
    probe_case_end();
}
#endif

/* ---- The sweep and the budget ------------------------------------------------ */

/** @brief The C5 answers the KODE request in flight with @p st and no payload. */
static void kode_ok_answer(const void *pay, uint16_t n)
{
    static uint8_t m[sizeof(kode_hdr_t) + KODE_MAX_PAYLOAD];
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_last_sent(NULL);
    kode_hdr_t const h = { KODE_WIRE_VERSION, req->service, req->op, req->seq,
                           KODE_ST_OK, 0U, n };

    (void)memcpy(m, &h, sizeof(h));
    if ( 0U < n )
    {
        (void)memcpy(&m[sizeof(h)], pay, n);
    }
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(h) + (size_t)n);
    pump();
}

/** @brief The vault's LIST, asked at link up, answers with one name. */
static void vault_lists(const char *ssid)
{
    kode_vault_list_t l;

    (void)memset(&l, 0, sizeof(l));
    l.count = 1U;
    (void)snprintf(l.ssid[0], sizeof(l.ssid[0]), "%s", ssid);
    kode_ok_answer(&l, (uint16_t)sizeof(l));
}

TEST_CASE("an automatic round whose sweep was cut is owed again, not ended", "[radio_probe]")
{
    radio_probe_t req;

    radio_fresh();
    fake_ble_reset();
    fake_time_reset();
    go_up();
    vault_lists(NET24);

    /* Wi-Fi on: the round's sweep holds the channel. */
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    (void)memset(&req, 0, sizeof(req));
    req.card = true;
    TEST_ASSERT_TRUE(ask(&req));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, run_real_sweep(), "the round's sweep was not cut");
    fake_klog_reset();
    finish(KDRV_RADIO_JOB_WIFI_SCAN, s_sweep_rc);

    /* No network was judged absent, and the check goes first. */
    TEST_ASSERT_FALSE(logged("nothing to join"));
    TEST_ASSERT_FALSE(logged("would let us on"));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_C5_INFO, running_job());
    finish_card();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, answer()->status);

    /* Then the round is run again, from its sweep. */
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());

    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    probe_case_end();
}

TEST_CASE("a check asked behind a join is owed and cuts nothing", "[radio_probe]")
{
    radio_probe_t req;

    probe_up();
    TEST_ASSERT_TRUE(wifi_join_request("some-net", "pw", false));
    pump();
    int const joining = running_job();
    TEST_ASSERT_TRUE_MESSAGE(0 <= joining, "the join was not started");
    TEST_ASSERT_NOT_EQUAL((int)KDRV_RADIO_JOB_WIFI_SCAN, joining);

    (void)memset(&req, 0, sizeof(req));
    req.card = true;
    TEST_ASSERT_TRUE(ask(&req));
    TEST_ASSERT_EQUAL_INT(joining, running_job());
    TEST_ASSERT_EQUAL_UINT(0U, answers());

    /* A sweep run now would have been cut had the AO asked for it. */
    TEST_ASSERT_TRUE_MESSAGE(run_real_sweep() > 1U, "a join was mistaken for a sweep");
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_sweep_rc);

    /* The join wanted Wi-Fi on; leave the flag as this file found it. */
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    probe_case_end();
    (void)answers();
}

TEST_CASE("a check owed when the link is lost answers denied and withdraws the cut",
          "[radio_probe]")
{
    radio_probe_t req;

    probe_up();
    dispatch(WIFI_SCAN_REQ_SIG);
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    (void)memset(&req, 0, sizeof(req));
    req.card = true;
    TEST_ASSERT_TRUE(ask(&req));
    TEST_ASSERT_EQUAL_UINT(0U, answers());

    finish_lost(KODE_ERROR);

    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, answer()->status);
    TEST_ASSERT_TRUE_MESSAGE(run_real_sweep() > 1U, "the cut outlived the check");
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_sweep_rc);
    probe_case_end();
}

TEST_CASE("discover spends no budget on its sweep: the find after it still runs",
          "[radio_probe]")
{
    radio_probe_t req;

    probe_up();
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "a job is running before the check");
    (void)memset(&req, 0, sizeof(req));
    (void)snprintf(req.ssid24, sizeof(req.ssid24), "%s", NET24);
    (void)snprintf(req.ble, sizeof(req.ble), "%s", BEACON);
    req.discover = true;
    TEST_ASSERT_TRUE(ask(&req));

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    unsigned const dbg = run_real_sweep();
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_sweep_rc);
    TEST_ASSERT_EQUAL_UINT(7U, dbg);
    fake_time_advance_ms(PAST_BUDGET_MS);          /* the sweep took its ~36 s */
    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_OK);

    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_BLE_FIND, running_job(),
                                  "the sweep used up the check's budget");
    leave_found(true, (int8_t)RSSI_BLE);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);

    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_TRUE(a->ble_rx);
    probe_case_end();
}

TEST_CASE("the advert is put back even when the budget is spent, and the answer says so",
          "[radio_probe]")
{
    radio_probe_t req;

    probe_up();
    fake_ble_reset();
    dispatch(BLE_START_REQ_SIG);
    pump();
    finish(KDRV_RADIO_JOB_BLE_START, kdrv_ble_start());
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    (void)kdrv_ble_advertise(false);               /* the advert is off at the start */
    TEST_ASSERT_EQUAL_INT((int)KDRV_BLE_IDLE, (int)kdrv_ble_state());
    ao_spy_drain();

    (void)memset(&req, 0, sizeof(req));
    (void)snprintf(req.ble, sizeof(req.ble), "%s", BEACON);
    TEST_ASSERT_TRUE(ask(&req));
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_BLE_FIND, running_job());
    leave_found(true, (int8_t)RSSI_BLE);
    fake_time_advance_ms(PAST_BUDGET_MS);
    finish(KDRV_RADIO_JOB_BLE_FIND, KODE_OK);

    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_RADIO_JOB_BLE_ADV, running_job(),
                                  "the budget kept the advert off the air");
    finish(KDRV_RADIO_JOB_BLE_ADV, kdrv_ble_advertise(true));
    const RadioProbeEvt * const a = answer();
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_TRUE(a->ble_adv);

    dispatch(BLE_STOP_REQ_SIG);
    pump();
    probe_case_end();
}

/* The cases of this file are the last to run the AO; the next file's walk to
 * `off` cannot finish a job this file holds. */
TEST_CASE("AO_Radio is left off after the radio check", "[radio_probe]")
{
    radio_fresh();
    probe_case_end();
}
