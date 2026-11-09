/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_wifi_scan.c
 * @brief The sweep's shape, its bounded copy, and the state it leaves behind.
 */
#include <string.h>

#include "unity.h"

#include "Mockesp_wifi.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi_default.h"
#include "Mockesp_event.h"

#include "fake_wifi_events.h"
#include "kdrv_wifi.h"

/** @brief Bytes for a network name copied out of the driver, NUL included. */
#define SSID_COPY_BYTES  33U

/* ---- The room ------------------------------------------------------------- */

/*
 * A sweep is one scan per channel, so a room here is not "sixty networks" but
 * "sixty networks on the first channel and nothing anywhere else". Everything
 * the stack answers is a stub rather than a queued expectation for exactly
 * that reason: a case must not have to know how many channels a sweep walks,
 * or it breaks the next time the channel list changes.
 */

/** @brief More steps than any sweep can take (three groups plus four). */
#define ROOM_STEPS  64U

/** @brief What a sweep over a whole lower band comes to. */
#define SWEEP_STEPS (KDRV_WIFI_SCAN_24_STEPS + KDRV_WIFI_SCAN_5G_STEPS)

static wifi_ap_record_t s_records[64];
static uint16_t         s_room[ROOM_STEPS];  /**< networks per step.         */
static wifi_scan_config_t s_seen[ROOM_STEPS];/**< what each step asked for.  */
static unsigned  s_step;      /**< The step the stack is answering.          */
static unsigned  s_starts;    /**< scan_start calls this sweep.              */
static unsigned  s_clears;    /**< clear_ap_list calls this sweep.           */
static unsigned  s_taken;     /**< Records handed out this sweep.            */
static unsigned  s_fail_at;   /**< 0 = never; else refuse from this record.  */
static esp_err_t s_num_rc;    /**< What get_ap_num answers.                  */
static uint8_t   s_nchan24;   /**< How many channels the country opens.      */
static unsigned  s_knocks;    /**< Progress hook calls this sweep.           */
static unsigned  s_knock_at[ROOM_STEPS]; /**< Listed so far at each knock.   */
static kdrv_wifi_ap_t s_listed[128];     /**< Everything handed over.        */
static unsigned  s_listed_n;

/** @brief An empty room, a fresh sweep, a stack that answers everything. */
static void room_reset(void)
{
    (void)memset(s_records, 0, sizeof(s_records));
    (void)memset(s_room, 0, sizeof(s_room));
    (void)memset(s_seen, 0, sizeof(s_seen));
    (void)memset(s_knock_at, 0, sizeof(s_knock_at));
    s_step    = 0U;
    s_starts  = 0U;
    s_clears  = 0U;
    s_taken   = 0U;
    s_fail_at = 0U;
    s_num_rc  = ESP_OK;
    s_nchan24 = 13U;
    s_knocks  = 0U;
    s_listed_n = 0U;
    (void)memset(s_listed, 0, sizeof(s_listed));
}

/** @brief Put @p n networks on sweep step @p step (0 is the first channel). */
static void room_put(unsigned step, unsigned n)
{
    if ( ROOM_STEPS > step )
    {
        s_room[step] = (uint16_t)n;
    }
}

/** @brief Fill the record table with @p n distinct networks, room empty. */
static void seed_records(unsigned n)
{
    room_reset();
    for ( unsigned i = 0U; (i < n) && (64U > i); ++i )
    {
        (void)snprintf((char *)s_records[i].ssid, sizeof(s_records[i].ssid),
                       "net%u", i);
        s_records[i].rssi    = (int8_t)(-40 - (int)i);
        s_records[i].primary = (uint8_t)(1U + (i % 11U));
    }
}

/**
 * @brief Stands in for esp_wifi_scan_start: the stack is asked not to block,
 *        and the SCAN_DONE it would raise later is delivered right away.
 *
 * The driver waits on that event for up to KDRV_WIFI_SCAN_STEP_TIMEOUT_MS
 * per step, so a stub that merely returned ESP_OK would hold every scan case
 * for over a minute.
 */
static esp_err_t scan_start_now(const wifi_scan_config_t *config, bool block,
                                int calls)
{
    (void)calls;
    TEST_ASSERT_FALSE_MESSAGE(block, "the scan must not block the RPC");
    TEST_ASSERT_NOT_NULL(config);
    s_step = s_starts;
    if ( ROOM_STEPS > s_starts )
    {
        s_seen[s_starts] = *config;
    }
    ++s_starts;
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_SCAN_DONE, NULL);
    return ESP_OK;
}

/** @brief A stack that will not start a scan at all. */
static esp_err_t scan_start_refuse(const wifi_scan_config_t *config, bool block,
                                   int calls)
{
    (void)config; (void)block; (void)calls;
    ++s_starts;
    return ESP_FAIL;
}

/** @brief How many networks the step being answered holds. */
static esp_err_t ap_num_now(uint16_t *number, int calls)
{
    (void)calls;
    if ( ESP_OK != s_num_rc )
    {
        return s_num_rc;
    }
    if ( NULL != number )
    {
        *number = s_room[s_step % ROOM_STEPS];
    }
    return ESP_OK;
}

/**
 * @brief Stands in for esp_wifi_scan_get_ap_record, popping in order.
 * @param out   Receives the record.
 * @param calls CMock's counter.
 * @return ESP_OK, or ESP_FAIL once the case's failure point is reached.
 */
static esp_err_t pop_record(wifi_ap_record_t *out, int calls)
{
    (void)calls;
    if ( (0U != s_fail_at) && (s_taken >= s_fail_at) )
    {
        return ESP_FAIL;
    }
    if ( NULL != out )
    {
        *out = s_records[s_taken % 64U];
    }
    ++s_taken;
    return ESP_OK;
}

/** @brief Count the buffer clears; every started channel owes one. */
static esp_err_t clear_list_now(int calls)
{
    (void)calls;
    ++s_clears;
    return ESP_OK;
}

/** @brief A country that opens s_nchan24 channels from channel 1. */
static esp_err_t country_now(wifi_country_t *country, int calls)
{
    (void)calls;
    if ( NULL != country )
    {
        (void)memset(country, 0, sizeof(*country));
        country->schan = 1U;
        country->nchan = s_nchan24;
    }
    return ESP_OK;
}

/**
 * @brief The progress hook, doing what its only real owner does: empty the
 *        ring, and remember how much had been handed over by then.
 */
static void on_more(void)
{
    kdrv_wifi_ap_t ap;

    while ( kdrv_wifi_scan_next(&ap) )
    {
        if ( 128U > s_listed_n )
        {
            s_listed[s_listed_n] = ap;
        }
        ++s_listed_n;
    }
    if ( ROOM_STEPS > s_knocks )
    {
        s_knock_at[s_knocks] = s_listed_n;
    }
    ++s_knocks;
}

/** @brief Point the whole stack at the stubs above. */
static void install_stubs(void)
{
    esp_wifi_get_country_Stub(&country_now);
    esp_wifi_scan_start_Stub(&scan_start_now);
    esp_wifi_scan_get_ap_num_Stub(&ap_num_now);
    esp_wifi_scan_get_ap_record_Stub(&pop_record);
    esp_wifi_clear_ap_list_Stub(&clear_list_now);
}

/** @brief Bring the stack up once per binary. */
static void wifi_up(void)
{
    static bool s_up;

    if ( s_up )
    {
        return;
    }
    esp_netif_init_ExpectAndReturn(ESP_OK);
    esp_event_handler_instance_register_Stub(&fake_wifi_events_register);
    esp_wifi_init_ExpectAnyArgsAndReturn(ESP_OK);
    esp_wifi_set_mode_ExpectAndReturn(WIFI_MODE_STA, ESP_OK);
    esp_wifi_start_ExpectAndReturn(ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_start());
    s_up = true;
}

/**
 * @brief Run one sweep over a room whose networks are all on the first
 *        channel, which is what every case that predates the per-channel
 *        sweep means by "a room with n networks".
 * @param found How many the coprocessor claims to see there.
 */
static void run_scan(uint16_t found)
{
    room_put(0U, found);
    install_stubs();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(s_starts, s_clears,
                                   "every channel started owes a buffer clear");
}

/* ---- The wait ----------------------------------------------------------------- */

/* The sweep is started without blocking the RPC and the driver waits for the
 * stack's own SCAN_DONE: esp-hosted gives up on a blocking scan_start after
 * 30 s, and a dual-band sweep at this dwell measured 37 s on the board. The
 * stub above asserts the `block` argument; this case is what makes the
 * assertion run and checks the handler that receives the event is the one
 * the driver registered. */
TEST_CASE("the scan asks the stack not to block and waits for its SCAN_DONE",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    TEST_ASSERT_TRUE_MESSAGE(fake_wifi_events_registered() >= 1U,
                             "the driver registered no event handler");
    seed_records(1U);
    run_scan(1U);
    TEST_ASSERT_EQUAL_UINT8(1U, kdrv_wifi_scan_count());

    /* No Verify here: whether wifi_up() had anything to do depends on which
     * file brought the stack up first, and the stub's own assertion on
     * `block` has already run inside run_scan(). */
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* ---- The bounded copy ------------------------------------------------------ */

TEST_CASE("a scan keeps what it found, in order", "[kdrv_wifi_scan]")
{
    kdrv_wifi_ap_t ap;

    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(3U);
    run_scan(3U);

    TEST_ASSERT_EQUAL_UINT8(3U, kdrv_wifi_scan_count());

    TEST_ASSERT_TRUE(kdrv_wifi_scan_get(0U, &ap));
    TEST_ASSERT_EQUAL_STRING("net0", ap.ssid);
    TEST_ASSERT_EQUAL_INT8(-40, ap.rssi);
    TEST_ASSERT_EQUAL_UINT8(1U, ap.channel);

    TEST_ASSERT_TRUE(kdrv_wifi_scan_get(2U, &ap));
    TEST_ASSERT_EQUAL_STRING("net2", ap.ssid);

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* What a phone app reads to tell one access point from another with the
 * same name, and a camera by its maker: the BSSID, as the stack heard it. */
TEST_CASE("a scan record's BSSID reaches the table and the listing",
          "[kdrv_wifi_scan]")
{
    static const uint8_t BSSID[6] = { 0x3cU, 0x84U, 0x6aU, 0x01U, 0x02U, 0x03U };
    kdrv_wifi_ap_t ap;

    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(1U);
    (void)memcpy(s_records[0].bssid, BSSID, sizeof(BSSID));
    run_scan(1U);

    TEST_ASSERT_TRUE(kdrv_wifi_scan_get(0U, &ap));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(BSSID, ap.bssid, sizeof(BSSID));
    TEST_ASSERT_TRUE(kdrv_wifi_scan_next(&ap));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(BSSID, ap.bssid, sizeof(BSSID));
    while ( kdrv_wifi_scan_next(&ap) )
    {
    }

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* A crowded room. Every record kept reaches AO_Radio and then the console as
 * its own event, so the cap is what stands between a busy office and a
 * subscriber's queue filling - which QP answers with a reset. */
TEST_CASE("a room with more networks than the cap keeps exactly the cap",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(64U);
    run_scan(60U);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_WIFI_SCAN_MAX, kdrv_wifi_scan_count());

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

TEST_CASE("exactly the cap is kept whole", "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(KDRV_WIFI_SCAN_MAX);
    run_scan((uint16_t)KDRV_WIFI_SCAN_MAX);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_WIFI_SCAN_MAX, kdrv_wifi_scan_count());

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* An empty room is an answer, not a failure - and the coprocessor's buffer is
 * still cleared, or the next scan would start on somebody else's leftovers. */
TEST_CASE("an empty room is a valid scan, and still clears the radio",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(0U);

    /* run_scan() asserts the clears match the starts; this is the case that
     * makes that assertion mean something, since here nothing else happens. */
    run_scan(0U);

    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_wifi_scan_count());
    TEST_ASSERT_TRUE_MESSAGE(1U < s_starts,
                             "an empty room is still a whole sweep");

    Mockesp_wifi_Verify();
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* A previous scan's results must not survive a new one that found less. The
 * count is zeroed before the radio is even asked, so a failed scan cannot
 * leave the last room's networks readable as if they were this one's. */
TEST_CASE("a new scan replaces the last one rather than adding to it",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(5U);
    run_scan(5U);
    TEST_ASSERT_EQUAL_UINT8(5U, kdrv_wifi_scan_count());

    seed_records(2U);
    run_scan(2U);
    TEST_ASSERT_EQUAL_UINT8(2U, kdrv_wifi_scan_count());

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* ---- The sweep, channel by channel ----------------------------------------- */

/* The whole point of walking the channels rather than asking for one sweep:
 * a network found on channel 1 is readable while channel 6 is still ahead. A
 * caller that had to wait for the last channel would wait up to forty seconds
 * to hear about a network the radio heard in the first second. */
TEST_CASE("a channel's networks are readable before the sweep ends",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(8U);
    room_put(0U, 2U);
    room_put(5U, 3U);
    install_stubs();
    kdrv_wifi_scan_progress(&on_more);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());

    /* Two knocks, not two-per-network and not one-per-channel: the hook is
     * for channels that found something. The first channel's two networks
     * were in the caller's hands with thirty-odd channels still ahead. */
    TEST_ASSERT_EQUAL_UINT(2U, s_knocks);
    TEST_ASSERT_EQUAL_UINT(2U, s_knock_at[0]);
    TEST_ASSERT_EQUAL_UINT(5U, s_knock_at[1]);
    TEST_ASSERT_EQUAL_UINT(5U, s_listed_n);
    TEST_ASSERT_EQUAL_UINT8(5U, kdrv_wifi_scan_count());
    TEST_ASSERT_EQUAL_UINT16(0U, kdrv_wifi_scan_dropped());

    kdrv_wifi_scan_progress(NULL);
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* A sweep with no hook is the ordinary case for the autojoin round and the
 * production row, and it must not be a special case in the driver. */
TEST_CASE("a sweep with no hook installed still fills the table",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(4U);
    room_put(2U, 4U);
    install_stubs();
    kdrv_wifi_scan_progress(NULL);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT8(4U, kdrv_wifi_scan_count());
    TEST_ASSERT_EQUAL_UINT(0U, s_knocks);
    TEST_ASSERT_EQUAL_UINT16(0U, kdrv_wifi_scan_dropped());

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* A crowded lower band must not be able to hide the upper one. The cap bounds
 * what is remembered, and stopping the sweep when it is reached would mean
 * that on a busy street - where the first six channels alone carry more than
 * the cap - kodeOS never saw a 5 GHz network at all. Measured on the bench
 * before this case existed: the table filled on channel 6 of 41. */
TEST_CASE("a full table does not stop the sweep", "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(64U);
    room_put(0U, (unsigned)KDRV_WIFI_SCAN_MAX);
    room_put(5U, 2U);   /* well into the upper band */
    install_stubs();
    kdrv_wifi_scan_progress(&on_more);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT(SWEEP_STEPS, s_starts);

    /* The table is capped and the listing is not: that is the whole split. */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_WIFI_SCAN_MAX, kdrv_wifi_scan_count());
    TEST_ASSERT_EQUAL_UINT((unsigned)KDRV_WIFI_SCAN_MAX + 2U, s_listed_n);

    kdrv_wifi_scan_progress(NULL);
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* Which twenty-four the table keeps is the question autojoin asks it: a
 * network it cannot hear is a network it cannot join, so the strongest win
 * rather than whichever channel happened to be swept first. */
TEST_CASE("a full table keeps the strongest, not the first",
          "[kdrv_wifi_scan]")
{
    kdrv_wifi_ap_t ap;
    bool           found_strong = false;

    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(30U);
    /* seed_records makes each network weaker than the last, so everything on
     * the first channel is stronger than everything after it. One network in
     * the upper band beats the lot. */
    s_records[29].rssi = -10;
    (void)snprintf((char *)s_records[29].ssid, sizeof(s_records[29].ssid),
                   "the-loud-one");
    room_put(0U, 29U);
    room_put(4U, 1U);
    install_stubs();
    kdrv_wifi_scan_progress(&on_more);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_WIFI_SCAN_MAX, kdrv_wifi_scan_count());

    for ( uint8_t i = 0U; kdrv_wifi_scan_count() > i; ++i )
    {
        TEST_ASSERT_TRUE(kdrv_wifi_scan_get(i, &ap));
        if ( strcmp(ap.ssid, "the-loud-one") == 0 )
        {
            found_strong = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(found_strong,
                             "the strongest network was swept last and lost");

    kdrv_wifi_scan_progress(NULL);
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* A ring nobody empties fills, and the networks that do not fit are said out
 * loud rather than quietly missing from the listing. */
TEST_CASE("networks nobody takes are counted, not swallowed",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(64U);
    room_put(0U, (unsigned)KDRV_WIFI_LIST_MAX + 10U);
    install_stubs();
    kdrv_wifi_scan_progress(NULL);   /* nobody is draining */

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT16(11U, kdrv_wifi_scan_dropped());

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* Seven steps, both bands through the bitmap, and the shape of each step is
 * the whole point: one step per channel ran forty DMA receives per sweep,
 * and on the P4 each of those invalidates both cores' instruction caches -
 * the tick ISR twice fetched a line of zeros from intact IRAM. Bit 0 of a
 * band's field is its bypass, bit N of the lower band is channel N, and the
 * bitmap only applies when `channel` is zero, which is the trap this guards. */
TEST_CASE("both bands are swept through the bitmap, in seven steps",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(0U);
    s_nchan24 = 13U;
    install_stubs();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT(SWEEP_STEPS, s_starts);

    /* The scan is active, with a dwell a beacon can land in: that is
     * per step now. */
    TEST_ASSERT_TRUE(s_seen[0].show_hidden);
    TEST_ASSERT_EQUAL_INT(WIFI_SCAN_TYPE_ACTIVE, s_seen[0].scan_type);
    TEST_ASSERT_EQUAL_UINT32(150U, s_seen[0].scan_time.active.max);

    /* 13 channels split three ways: 1-5, 6-10, 11-13. */
    for ( unsigned i = 0U; SWEEP_STEPS > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, s_seen[i].channel,
                                        "a bitmap with a channel set is ignored");
    }
    TEST_ASSERT_EQUAL_UINT32(0x003EU, s_seen[0].channel_bitmap.ghz_2_channels);
    TEST_ASSERT_EQUAL_UINT32(1U,      s_seen[0].channel_bitmap.ghz_5_channels);
    TEST_ASSERT_EQUAL_UINT32(0x07C0U, s_seen[1].channel_bitmap.ghz_2_channels);
    TEST_ASSERT_EQUAL_UINT32(0x3800U, s_seen[2].channel_bitmap.ghz_2_channels);

    /* 28 slots four ways: bits 1-7, 8-14, 15-21, 22-28. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, s_seen[3].channel_bitmap.ghz_2_channels,
                                     "the lower band must be bypassed whole");
    TEST_ASSERT_EQUAL_UINT32(0x000000FEU, s_seen[3].channel_bitmap.ghz_5_channels);
    TEST_ASSERT_EQUAL_UINT32(0x00007F00U, s_seen[4].channel_bitmap.ghz_5_channels);
    TEST_ASSERT_EQUAL_UINT32(0x003F8000U, s_seen[5].channel_bitmap.ghz_5_channels);
    TEST_ASSERT_EQUAL_UINT32(0x1FC00000U, s_seen[6].channel_bitmap.ghz_5_channels);

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* The lower band is whatever the slave's country opens, asked for rather than
 * assumed: probing a channel the board may not use is the kind of thing a
 * regulator has opinions about. Eleven channels still make three steps, split
 * 4-4-3. */
TEST_CASE("the lower band is the country's range, not a fixed one",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(0U);
    s_nchan24 = 11U;
    install_stubs();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT(SWEEP_STEPS, s_starts);
    TEST_ASSERT_EQUAL_UINT32(0x001EU, s_seen[0].channel_bitmap.ghz_2_channels);
    TEST_ASSERT_EQUAL_UINT32(0x01E0U, s_seen[1].channel_bitmap.ghz_2_channels);
    TEST_ASSERT_EQUAL_UINT32(0x0E00U, s_seen[2].channel_bitmap.ghz_2_channels);
    TEST_ASSERT_EQUAL_UINT32(0x00FEU, s_seen[3].channel_bitmap.ghz_5_channels);

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* ---- When the radio stops answering mid-scan ------------------------------- */

/* The loop breaks with whatever landed, and the buffer is still cleared. Half
 * a scan is a better answer than none, but a radio buffer left full would
 * poison the next one. */
TEST_CASE("a scan cut short keeps what it read and still clears the radio",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(6U);
    s_fail_at = 3U;   /* the fourth record refuses */

    run_scan(6U);
    TEST_ASSERT_EQUAL_UINT8(3U, kdrv_wifi_scan_count());

    Mockesp_wifi_Verify();
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* A scan the radio refused outright leaves no results readable, rather than
 * the previous room's. */
TEST_CASE("a refused scan is an error and leaves nothing behind",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(4U);
    run_scan(4U);
    TEST_ASSERT_EQUAL_UINT8(4U, kdrv_wifi_scan_count());

    /* Every channel refused, not one: a single closed slot in the upper band
     * is ordinary and the sweep walks past it. An error is what a radio that
     * takes nothing at all earns. */
    room_reset();
    install_stubs();
    esp_wifi_scan_start_Stub(&scan_start_refuse);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_wifi_scan_count());
    TEST_ASSERT_TRUE_MESSAGE(1U < s_starts, "the sweep gave up on one refusal");

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* A radio that will not say how many it found is not a radio to read records
 * from: the count would be a guess. */
TEST_CASE("a radio that will not report a count is an error",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    room_reset();
    s_num_rc = ESP_FAIL;
    install_stubs();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_scan());

    Mockesp_wifi_Verify();
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* ---- Reading the results --------------------------------------------------- */

TEST_CASE("reading past the end, or into nothing, is refused",
          "[kdrv_wifi_scan]")
{
    kdrv_wifi_ap_t ap;

    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(2U);
    run_scan(2U);

    TEST_ASSERT_FALSE(kdrv_wifi_scan_get(2U, &ap));
    TEST_ASSERT_FALSE(kdrv_wifi_scan_get(200U, &ap));
    TEST_ASSERT_FALSE(kdrv_wifi_scan_get(0U, NULL));

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* ---- forget: the teardown with nobody to talk to --------------------------- */

/* AO_Radio calls this after the link to the coprocessor is gone. It clears the
 * same state a stop would, and issues no RPC at all - because there is nothing
 * on the other end to receive one. A forget that talked would hang the worker
 * on a coprocessor that is not there. */
TEST_CASE("forgetting the stack talks to nobody", "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    seed_records(3U);
    run_scan(3U);
    TEST_ASSERT_EQUAL_UINT8(3U, kdrv_wifi_scan_count());

    /* No expectations queued: any RPC at all fails this case. */
    kdrv_wifi_forget();

    TEST_ASSERT_EQUAL_UINT8(0U, kdrv_wifi_scan_count());
    TEST_ASSERT_EQUAL_INT(KDRV_WIFI_IDLE, kdrv_wifi_state(NULL));
    {
        char name[SSID_COPY_BYTES];

        kdrv_wifi_ssid_copy(name, sizeof(name));
        TEST_ASSERT_EQUAL_STRING("", name);
    }
    TEST_ASSERT_EQUAL_INT8(0, kdrv_wifi_rssi_last());
    TEST_ASSERT_FALSE(kdrv_wifi_started());

    Mockesp_wifi_Verify();
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* ---- rssi: the local check that avoids a round trip ------------------------ */

/* Asking the coprocessor costs a round trip over a sequential RPC channel, and
 * `wifi_info` with nothing joined must not wait on one to say so. */
TEST_CASE("signal strength with nothing joined answers without asking",
          "[kdrv_wifi_scan]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_up();
    kdrv_wifi_forget();          /* state is IDLE */

    /* No expectation for esp_wifi_sta_get_ap_info: reaching it fails here. */
    TEST_ASSERT_EQUAL_INT8(0, kdrv_wifi_rssi());

    Mockesp_wifi_Verify();
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}
