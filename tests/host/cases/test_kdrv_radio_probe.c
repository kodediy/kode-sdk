/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_radio_probe.c
 * @brief The radio check's two worker calls: one Wi-Fi channel probed, one
 *        BLE name found, and the flag that ends a sweep early.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "Mockesp_wifi.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi_default.h"
#include "Mockesp_event.h"

#include "fake_wifi_events.h"
#include "kdrv_ble_find.h"
#include "kdrv_wifi.h"

/** @brief Records the fake room can hold. */
#define PROBE_ROOM_MAX    8U

/** @brief A 2.4 GHz channel, where transmitting is free. */
#define PROBE_CH_ACTIVE   6U

/** @brief A radar channel (U-NII-2C). */
#define PROBE_CH_RADAR    100U

/** @brief A dwell shorter than one beacon interval. */
#define PROBE_DWELL_SHORT_MS  80U

/** @brief A dwell on an active channel. */
#define PROBE_DWELL_ACTIVE_MS 120U

/** @brief The first and last radar channel of each block, and their neighbours. */
#define PROBE_RADAR_LO    52U
#define PROBE_RADAR_LO_M  51U
#define PROBE_RADAR_MID_HI 64U
#define PROBE_RADAR_MID_HI_P 65U
#define PROBE_RADAR_HI_M  99U
#define PROBE_RADAR_HI    144U
#define PROBE_RADAR_HI_P  145U

/** @brief The weaker and the stronger reading of one network. */
#define PROBE_RSSI_WEAK   (-71)
#define PROBE_RSSI_STRONG (-64)

static wifi_ap_record_t   s_room[PROBE_ROOM_MAX];
static uint16_t           s_room_n;
static uint16_t           s_room_taken;
static wifi_scan_config_t s_cfg;
static unsigned           s_starts;
static esp_err_t          s_start_rc;
static bool               s_abort_at_start;
static unsigned           s_steps_seen;
static wifi_band_mode_t   s_band_asked;     /**< The band the stack was last asked for.  */
static wifi_band_mode_t   s_band_at_start;  /**< The band standing at the first start.   */

/** @brief An empty room, a stack that starts everything. */
static void room_reset(void)
{
    (void)memset(s_room, 0, sizeof(s_room));
    (void)memset(&s_cfg, 0, sizeof(s_cfg));
    s_room_n         = 0U;
    s_room_taken     = 0U;
    s_starts         = 0U;
    s_start_rc       = ESP_OK;
    s_abort_at_start = false;
    s_steps_seen     = 0U;
    s_band_asked     = (wifi_band_mode_t)0;
    s_band_at_start  = (wifi_band_mode_t)0;
}

/** @brief Put one network in the room. */
static void room_put(const char *ssid, int8_t rssi, uint8_t primary)
{
    wifi_ap_record_t * const r = &s_room[s_room_n];

    (void)snprintf((char *)r->ssid, sizeof(r->ssid), "%s", ssid);
    r->rssi    = rssi;
    r->primary = primary;
    ++s_room_n;
}

/** @brief Stands in for esp_wifi_scan_start: SCAN_DONE arrives at once. */
static esp_err_t scan_start_now(const wifi_scan_config_t *config, bool block,
                                int calls)
{
    (void)calls;
    TEST_ASSERT_FALSE(block);
    ++s_starts;
    ++s_steps_seen;
    if ( 1U == s_starts )
    {
        s_band_at_start = s_band_asked;
    }
    s_cfg = *config;
    s_room_taken = 0U;
    if ( ESP_OK != s_start_rc )
    {
        return s_start_rc;
    }
    if ( s_abort_at_start )
    {
        kdrv_wifi_scan_abort(true);
    }
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    return ESP_OK;
}

static esp_err_t ap_num_now(uint16_t *number, int calls)
{
    (void)calls;
    *number = s_room_n;
    return ESP_OK;
}

static esp_err_t pop_record(wifi_ap_record_t *out, int calls)
{
    (void)calls;
    *out = s_room[s_room_taken];
    ++s_room_taken;
    return ESP_OK;
}

static esp_err_t clear_list_now(int calls)
{
    (void)calls;
    return ESP_OK;
}

static esp_err_t country_now(wifi_country_t *country, int calls)
{
    (void)calls;
    (void)memset(country, 0, sizeof(*country));
    country->schan = 1U;
    country->nchan = 13U;
    return ESP_OK;
}

/** @brief Stands in for esp_wifi_set_band_mode and keeps what it was given. */
static esp_err_t band_note(wifi_band_mode_t mode, int calls)
{
    (void)calls;
    s_band_asked = mode;
    return ESP_OK;
}

/** @brief Point the whole stack at the stubs above and bring it up once. */
static void probe_setup(void)
{
    static bool s_up;

    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();
    room_reset();
    kdrv_wifi_scan_abort(false);

    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_OK);
    esp_wifi_get_country_Stub(&country_now);
    esp_wifi_scan_start_Stub(&scan_start_now);
    esp_wifi_scan_get_ap_num_Stub(&ap_num_now);
    esp_wifi_scan_get_ap_record_Stub(&pop_record);
    esp_wifi_clear_ap_list_Stub(&clear_list_now);
    if ( !s_up )
    {
        esp_netif_init_ExpectAndReturn(ESP_OK);
        esp_event_handler_instance_register_Stub(&fake_wifi_events_register);
        esp_wifi_init_ExpectAnyArgsAndReturn(ESP_OK);
        esp_wifi_set_mode_ExpectAndReturn(WIFI_MODE_STA, ESP_OK);
        esp_wifi_start_ExpectAndReturn(ESP_OK);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_start());
        s_up = true;
    }
}

/** @brief Release the mocks. */
static void probe_teardown(void)
{
    kdrv_wifi_scan_abort(false);
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

TEST_CASE("test_probe_merges_the_strongest_reading", "[kdrv_radio_probe]")
{
    kdrv_wifi_probe_t want[2];
    bool passive = true;

    probe_setup();
    room_put("kode-test", (int8_t)PROBE_RSSI_WEAK, PROBE_CH_ACTIVE);
    room_put("someone-else", -30, PROBE_CH_ACTIVE);
    room_put("kode-test", (int8_t)PROBE_RSSI_STRONG, PROBE_CH_ACTIVE);
    (void)memset(want, 0, sizeof(want));
    want[0].ssid = "kode-test";
    want[1].ssid = "kode-5g";

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_probe(PROBE_CH_ACTIVE, PROBE_DWELL_ACTIVE_MS,
                                                   want, 2U, &passive));
    TEST_ASSERT_TRUE(want[0].seen);
    TEST_ASSERT_EQUAL_INT8((int8_t)PROBE_RSSI_STRONG, want[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(PROBE_CH_ACTIVE, want[0].channel);
    TEST_ASSERT_FALSE(want[1].seen);
    TEST_ASSERT_FALSE(passive);
    TEST_ASSERT_EQUAL_UINT8(PROBE_CH_ACTIVE, s_cfg.channel);
    TEST_ASSERT_EQUAL_INT(WIFI_SCAN_TYPE_ACTIVE, s_cfg.scan_type);
    TEST_ASSERT_EQUAL_UINT32(PROBE_DWELL_ACTIVE_MS, s_cfg.scan_time.active.max);
    probe_teardown();
}

TEST_CASE("a later channel does not weaken an earlier reading", "[kdrv_radio_probe]")
{
    kdrv_wifi_probe_t want[1];

    probe_setup();
    (void)memset(want, 0, sizeof(want));
    want[0].ssid = "kode-test";
    room_put("kode-test", (int8_t)PROBE_RSSI_STRONG, PROBE_CH_ACTIVE);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_probe(PROBE_CH_ACTIVE, PROBE_DWELL_ACTIVE_MS,
                                                   want, 1U, NULL));
    room_reset();
    room_put("kode-test", (int8_t)PROBE_RSSI_WEAK, 11U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_probe(11U, PROBE_DWELL_ACTIVE_MS, want, 1U, NULL));
    TEST_ASSERT_EQUAL_INT8((int8_t)PROBE_RSSI_STRONG, want[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(PROBE_CH_ACTIVE, want[0].channel);
    probe_teardown();
}

TEST_CASE("a probe listens to both bands, whatever a join aimed at before it",
          "[kdrv_radio_probe]")
{
    kdrv_wifi_probe_t want[1];

    probe_setup();
    esp_wifi_set_band_mode_Stub(&band_note);
    s_band_asked = WIFI_BAND_MODE_5G_ONLY;   /* what a join left standing */
    (void)memset(want, 0, sizeof(want));
    want[0].ssid = "kode-test";

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_probe(PROBE_CH_ACTIVE, PROBE_DWELL_ACTIVE_MS,
                                                   want, 1U, NULL));
    TEST_ASSERT_EQUAL_INT((int)WIFI_BAND_MODE_AUTO, (int)s_band_at_start);
    probe_teardown();
}

TEST_CASE("test_probe_on_a_radar_channel_is_passive_and_150_ms", "[kdrv_radio_probe]")
{
    kdrv_wifi_probe_t want[1];
    bool passive = false;

    probe_setup();
    (void)memset(want, 0, sizeof(want));
    want[0].ssid = "kode-5g";
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_probe(PROBE_CH_RADAR, PROBE_DWELL_SHORT_MS,
                                                   want, 1U, &passive));
    TEST_ASSERT_TRUE(passive);
    TEST_ASSERT_EQUAL_UINT8(PROBE_CH_RADAR, s_cfg.channel);
    TEST_ASSERT_EQUAL_INT(WIFI_SCAN_TYPE_PASSIVE, s_cfg.scan_type);
    TEST_ASSERT_EQUAL_UINT32(150U, s_cfg.scan_time.passive);
    TEST_ASSERT_EQUAL_UINT32(KDRV_WIFI_RADAR_DWELL_MS, s_cfg.scan_time.passive);

    /* A longer dwell is kept as asked. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_probe(PROBE_CH_RADAR, 250U, want, 1U, &passive));
    TEST_ASSERT_EQUAL_UINT32(250U, s_cfg.scan_time.passive);
    probe_teardown();
}

TEST_CASE("the radar blocks start and end where the rules put them", "[kdrv_radio_probe]")
{
    static const struct {
        uint8_t channel;
        bool    radar;
    } rows[] = {
        { 36U, false }, { PROBE_RADAR_LO_M, false }, { PROBE_RADAR_LO, true },
        { PROBE_RADAR_MID_HI, true }, { PROBE_RADAR_MID_HI_P, false },
        { PROBE_RADAR_HI_M, false }, { PROBE_CH_RADAR, true },
        { PROBE_RADAR_HI, true }, { PROBE_RADAR_HI_P, false }, { 149U, false },
        { 1U, false }, { 13U, false },
    };

    probe_setup();
    for ( size_t i = 0U; (sizeof(rows) / sizeof(rows[0])) > i; ++i )
    {
        bool passive = !rows[i].radar;
        kdrv_wifi_probe_t want = { .ssid = "x" };

        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_probe(rows[i].channel, PROBE_DWELL_ACTIVE_MS,
                                                       &want, 1U, &passive));
        TEST_ASSERT_EQUAL_INT_MESSAGE((int)rows[i].radar, (int)passive, "channel class");
    }
    probe_teardown();
}

TEST_CASE("a channel the stack refuses is denied and leaves the wanted untouched",
          "[kdrv_radio_probe]")
{
    kdrv_wifi_probe_t want[1];

    probe_setup();
    s_start_rc = ESP_FAIL;
    (void)memset(want, 0, sizeof(want));
    want[0].ssid = "kode-test";
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_probe(PROBE_CH_ACTIVE, PROBE_DWELL_ACTIVE_MS,
                                                       want, 1U, NULL));
    TEST_ASSERT_FALSE(want[0].seen);
    probe_teardown();
}

TEST_CASE("test_abort_ends_a_sweep_before_its_next_step", "[kdrv_radio_probe]")
{
    unsigned full;

    probe_setup();

    /* A whole sweep, for the number of steps to compare with. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    full = s_starts;
    TEST_ASSERT_TRUE(1U < full);

    /* Asked to stop while the first step is on the air: that step ends, the
     * next never starts, and the sweep says it was cut. */
    s_starts         = 0U;
    s_abort_at_start = true;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT(1U, s_starts);

    /* Asked before it begins: no step at all, and still cut, not failed. */
    s_starts         = 0U;
    s_abort_at_start = false;
    kdrv_wifi_scan_abort(true);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT(0U, s_starts);

    /* Taken back, the next sweep runs whole. */
    kdrv_wifi_scan_abort(false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT(full, s_starts);
    probe_teardown();
}

/* ---- The BLE find ------------------------------------------------------------ */

/** @brief The name the station advertises in the tests. */
#define FIND_NAME   "KODE-BEACON"

/** @brief One address, little-endian as NimBLE hands it over. */
static const uint8_t FIND_ADDR_LE[6] = { 0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U };

/** @brief Another one. */
static const uint8_t FIND_OTHER_LE[6] = { 0x11U, 0x12U, 0x13U, 0x14U, 0x15U, 0x16U };

TEST_CASE("test_find_stops_at_the_first_match", "[kdrv_radio_probe]")
{
    kdrv_ble_find_t f;
    static const char other[] = "SOME-PHONE";
    static const char full[]  = FIND_NAME "-7F";

    TEST_ASSERT_TRUE(kdrv_ble_find_begin(&f, FIND_NAME));

    /* Nothing that is not the name, and an advert with no name, end it. */
    TEST_ASSERT_FALSE(kdrv_ble_find_offer(&f, FIND_OTHER_LE, 0U, -40,
                                          (const uint8_t *)other, strlen(other)));
    TEST_ASSERT_FALSE(kdrv_ble_find_offer(&f, FIND_OTHER_LE, 0U, -40, NULL, 0U));
    TEST_ASSERT_FALSE(kdrv_ble_find_offer(&f, FIND_OTHER_LE, 0U, -40,
                                          (const uint8_t *)"KODE", 4U));
    TEST_ASSERT_FALSE(f.hit);

    /* A name that starts with the wanted one is the one: the listen ends. */
    TEST_ASSERT_TRUE(kdrv_ble_find_offer(&f, FIND_ADDR_LE, KDRV_BLE_ADDR_RANDOM, -52,
                                         (const uint8_t *)full, strlen(full)));
    TEST_ASSERT_TRUE(f.hit);
    TEST_ASSERT_EQUAL_INT8(-52, f.dev.rssi);
    TEST_ASSERT_EQUAL_STRING(full, f.dev.name);
    TEST_ASSERT_EQUAL_UINT8(KDRV_BLE_ADDR_RANDOM, f.dev.addr_type);
    /* Printed order: the reverse of NimBLE's. */
    TEST_ASSERT_EQUAL_HEX8(0x06U, f.dev.addr[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01U, f.dev.addr[5]);

    /* A later match changes nothing: the first one stands. */
    TEST_ASSERT_FALSE(kdrv_ble_find_offer(&f, FIND_OTHER_LE, 0U, -10,
                                          (const uint8_t *)FIND_NAME, strlen(FIND_NAME)));
    TEST_ASSERT_EQUAL_INT8(-52, f.dev.rssi);
    TEST_ASSERT_EQUAL_HEX8(0x06U, f.dev.addr[0]);

    /* Ended, it hears nothing. */
    kdrv_ble_find_end(&f);
    TEST_ASSERT_FALSE(kdrv_ble_find_offer(&f, FIND_OTHER_LE, 0U, -10,
                                          (const uint8_t *)FIND_NAME, strlen(FIND_NAME)));
}

TEST_CASE("a find refuses an empty name and one that cannot fit an advert", "[kdrv_radio_probe]")
{
    kdrv_ble_find_t f;
    char big[KDRV_BLE_FIND_NAME_MAX + 2U];

    (void)memset(big, 'a', sizeof(big));
    big[sizeof(big) - 1U] = '\0';
    TEST_ASSERT_FALSE(kdrv_ble_find_begin(&f, ""));
    TEST_ASSERT_FALSE(kdrv_ble_find_begin(&f, NULL));
    TEST_ASSERT_FALSE(kdrv_ble_find_begin(&f, big));
    big[KDRV_BLE_FIND_NAME_MAX] = '\0';
    TEST_ASSERT_TRUE(kdrv_ble_find_begin(&f, big));
}
