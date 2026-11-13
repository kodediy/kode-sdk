/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_wifi.c
 * @brief kode_wifi_*() over the real facade and driver, a scripted copy of the module, and the Wi-Fi events.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Mockesp_event.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi.h"
#include "Mockesp_wifi_default.h"

#include "fake_wifi_events.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_wifi.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"
#include "standard_ao_support.h"

#define WIFI_IP4        0x0100A8C0U  /**< The address the fake lease carries.            */
#define WIFI_AWAIT_MS   50U          /**< The driver's own wait, already satisfied.      */
#define WIFI_FILL_MAX   64U          /**< Posts that surely fill AO_Radio's queue.       */
#define DOOR_BOUND_MS   2500U        /**< More than the module waits for a full queue.   */
#define ERROR_COPY      KODE_ERROR_MAX /**< Bytes of a copied error text.                */

#define NAME_32  "0123456789abcdef0123456789abcdef"
#define NAME_33  NAME_32 "x"
#define PASS_64  NAME_32 NAME_32
#define PASS_65  PASS_64 "x"

/* ---- The facade's seam: what the scripted copy of the module calls ------------ */

/** @brief States a script can hold. */
#define SEAM_MAX 8U

/** @brief Sleeps the first scripted join takes before it is refused. */
#define SEAM_SLEEPS 3U

/** @brief One read of the scripted status: a state and the network it names. */
typedef struct {
    kdrv_wifi_state_t state;  /**< The state.                                        */
    const char       *ssid;   /**< The name reported with CONNECTING or GOT_IP.      */
} seam_step_t;

static seam_step_t       s_seam_step[SEAM_MAX];   /**< The reads status serves, in order.     */
static uint32_t          s_seam_len;              /**< How many are scripted.                 */
static uint32_t          s_seam_reads;            /**< How many reads status has served.      */
static kode_status_t     s_seam_status_rc;        /**< What status answers.                   */
static kode_status_t     s_seam_connect_rc;       /**< What connect answers.                  */
static uint32_t          s_seam_joins;            /**< Requests connect has taken.            */

static kode_status_t seam_status(kode_sys_wifi_status_t *out);
static kode_status_t seam_connect(const char *ssid, const char *pass);
static kode_status_t seam_disconnect(void);
bool seam_wifi_connect(const char *ssid, const char *password);
bool seam_wifi_is_connected(void);
bool seam_wifi_disconnect(void);
const char *seam_event_wifi_connected(void);
const char *seam_event_wifi_lost(void);

/** @brief Rows the scripted scan offers at most. */
#define SEAM_SCAN_MAX 16U

static kode_status_t seam_block_scan(kode_sys_wifi_ap_cb cb, void *ctx, uint32_t ms);
int seam_wifi_scan(kode_wifi_network_t *out, size_t cap);

#define kode_block_wifi_scan     seam_block_scan
#define kode_wifi_scan           seam_wifi_scan
#define kode_sys_wifi_status     seam_status
#define kode_sys_wifi_connect    seam_connect
#define kode_sys_wifi_disconnect seam_disconnect
#define kode_wifi_connect        seam_wifi_connect
#define kode_wifi_is_connected   seam_wifi_is_connected
#define kode_wifi_disconnect     seam_wifi_disconnect
#define kode_event_wifi_connected seam_event_wifi_connected
#define kode_event_wifi_lost      seam_event_wifi_lost
#include "kode_wifi.c"
#undef kode_event_wifi_connected
#undef kode_event_wifi_lost
#undef kode_sys_wifi_status
#undef kode_sys_wifi_connect
#undef kode_sys_wifi_disconnect
#undef kode_wifi_connect
#undef kode_wifi_is_connected
#undef kode_wifi_disconnect
#undef kode_block_wifi_scan
#undef kode_wifi_scan

static kode_sys_wifi_ap_t s_seam_rows[SEAM_SCAN_MAX];  /**< What the scripted scan offers.   */
static uint32_t           s_seam_rows_n;               /**< How many rows it offers.         */
static kode_status_t      s_seam_scan_rc;              /**< What it answers.                 */
static uint32_t           s_seam_scans;                /**< Scans it was asked for.          */
static uint32_t           s_seam_scan_ms;              /**< The bound it was handed.         */

static kode_status_t seam_block_scan(kode_sys_wifi_ap_cb cb, void *ctx, uint32_t ms)
{
    s_seam_scans++;
    s_seam_scan_ms = ms;
    if ( KODE_OK == s_seam_scan_rc )
    {
        for ( uint32_t i = 0U; i < s_seam_rows_n; i++ )
        {
            if ( !cb(&s_seam_rows[i], ctx) )
            {
                break;
            }
        }
    }
    return s_seam_scan_rc;
}

/**
 * @brief Script the scan: @p n rows whose strengths are @p rssi, named "ap-<i>", and its answer.
 * @param rssi The strengths, in the order the sweep reports them.
 * @param n    How many.
 * @param rc   The answer.
 */
static void seam_scan_script(const int8_t *rssi, uint32_t n, kode_status_t rc)
{
    (void)memset(s_seam_rows, 0, sizeof(s_seam_rows));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        (void)snprintf(s_seam_rows[i].ssid, sizeof(s_seam_rows[i].ssid), "ap-%" PRIu32, i);
        s_seam_rows[i].rssi    = rssi[i];
        s_seam_rows[i].channel = (uint8_t)(1U + i);
        s_seam_rows[i].open    = (i % 2U) == 0U;
    }
    s_seam_rows_n  = n;
    s_seam_scan_rc = rc;
    s_seam_scans   = 0U;
    s_seam_scan_ms = 0U;
}

static kode_status_t seam_status(kode_sys_wifi_status_t *out)
{
    if ( KODE_OK == s_seam_status_rc )
    {
        uint32_t at = s_seam_reads;

        if ( at >= s_seam_len )
        {
            at = s_seam_len - 1U;
        }
        (void)memset(out, 0, sizeof(*out));
        out->state = (uint8_t)s_seam_step[at].state;
        if ( ((uint8_t)KDRV_WIFI_CONNECTING == out->state) || ((uint8_t)KDRV_WIFI_GOT_IP == out->state) )
        {
            (void)snprintf(out->ssid, sizeof(out->ssid), "%s", s_seam_step[at].ssid);
        }
        s_seam_reads++;
    }
    return s_seam_status_rc;
}

static kode_status_t seam_connect(const char *ssid, const char *pass)
{
    (void)ssid;
    (void)pass;
    s_seam_joins++;
    return s_seam_connect_rc;
}

static kode_status_t seam_disconnect(void)
{
    return KODE_OK;
}

/** @brief Script the states status will report and the answers of the doors. */
static void seam_script(const seam_step_t *steps, uint32_t n, kode_status_t status_rc,
                        kode_status_t connect_rc)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        s_seam_step[i] = steps[i];
    }
    s_seam_len        = n;
    s_seam_reads      = 0U;
    s_seam_status_rc  = status_rc;
    s_seam_connect_rc = connect_rc;
    s_seam_joins      = 0U;
}

/* ---- The world: AO_Radio's queue and the real driver --------------------------- */

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The one event waiting for AO_Radio, which must be of @p sig. */
static const QEvt *radio_only(enum_t sig)
{
    const QEvt *e = NULL;

    TEST_ASSERT_EQUAL_UINT(1U, uxQueueMessagesWaiting(AO_Radio->eQueue));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)sig, (uint32_t)e->sig);
    return e;
}

/** @brief Fill AO_Radio's queue until only its margin is free. */
static void radio_fill_to_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    for ( uint32_t guard = 0U;
         (WIFI_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_CMD_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
}

/** @brief Whether the driver was up before the case; the case puts it back. */
static bool s_was_up;

/** @brief The netif the mock hands out. */
static uint8_t s_netif;

/** @brief The remote stack mocked up and started, if the driver is not already. */
static bool wifi_stack_up(void)
{
    bool const was_up = kdrv_wifi_started();

    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();
    if ( !was_up )
    {
        esp_netif_init_ExpectAndReturn(ESP_OK);
        esp_event_handler_instance_register_Stub(&fake_wifi_events_register);
        esp_wifi_init_ExpectAnyArgsAndReturn(ESP_OK);
        esp_wifi_set_mode_ExpectAndReturn(WIFI_MODE_STA, ESP_OK);
        esp_wifi_start_ExpectAndReturn(ESP_OK);
    }
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_start());
    return was_up;
}

/** @brief Release the mocks; a stack this case started goes back to unstarted. */
static void wifi_stack_done(bool was_up)
{
    if ( !was_up )
    {
        kdrv_wifi_forget();
    }
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/** @brief Locks, clock and AO_Radio's queue ready; the system marked up. */
static void fresh(void)
{
    /* The module's lock is made on its first call in the binary: a real one. */
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    qf_support_init();
    qf_support_give_queue(AO_Radio);
    radio_flush();
    fake_sem_reset();
    fake_time_reset();
}

/** @brief The case's world: the driver started, readied and idle, the clock at zero. */
static void world_up(void)
{
    fresh();
    s_was_up = wifi_stack_up();
    esp_netif_create_default_wifi_sta_IgnoreAndReturn((esp_netif_t *)&s_netif);
    esp_wifi_stop_IgnoreAndReturn(ESP_OK);
    esp_wifi_start_IgnoreAndReturn(ESP_OK);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);
    esp_wifi_sta_get_ap_info_IgnoreAndReturn(ESP_FAIL);
    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("net", KDRV_WIFI_CHANNEL_ANY));
    fake_time_reset();
}

/** @brief Give the world back and leave AO_Radio's queue empty. */
static void world_down(void)
{
    radio_flush();
    wifi_stack_done(s_was_up);
}

/** @brief The radio gets an address on @p ssid. */
static void reach_ip(const char *ssid)
{
    ip_event_got_ip_t got;

    (void)memset(&got, 0, sizeof(got));
    got.ip_info.ip.addr = WIFI_IP4;
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &got);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await(ssid, WIFI_AWAIT_MS));
}

/** @brief The hook of most cases: the first sleep finds the board on "net". */
static void reach_net(void)
{
    reach_ip("net");
}

/** @brief The first sleep finds the board on the longest name there is. */
static void reach_long(void)
{
    reach_ip(NAME_32);
}

/** @brief The queue has room by the first sleep, and the board is then on "net". */
static void room_then_net(void)
{
    radio_flush();
    reach_ip("net");
}

/** @brief The network drops. */
static void net_drops(void)
{
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
}

/** @brief Expect one WIFI_JOIN_REQ_SIG for @p ssid and @p pass, and recycle it. */
static void expect_join(const char *ssid, const char *pass)
{
    const WifiJoinEvt *const e = (const WifiJoinEvt *)radio_only(WIFI_JOIN_REQ_SIG);

    TEST_ASSERT_EQUAL_STRING(ssid, e->ssid);
    TEST_ASSERT_EQUAL_STRING(pass, e->pass);
    TEST_ASSERT_TRUE(e->remember);
    QF_gc(&e->super);
}

/* ---- connect ------------------------------------------------------------------- */

TEST_CASE("connect posts a join and returns true once the board has an address there", "[kode_wifi]")
{
    world_up();
    fake_time_delay_hook(&reach_net);

    TEST_ASSERT_TRUE(kode_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    expect_join("net", "pw");
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_WIFI_CONNECT_MS, kode_sys_time_ms());
    world_down();
}

TEST_CASE("connect to the network the board is on answers true and posts nothing", "[kode_wifi]")
{
    world_up();
    reach_ip("net");

    TEST_ASSERT_TRUE(kode_wifi_connect("net", "pw"));
    TEST_ASSERT_TRUE(kode_wifi_connect(NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Radio->eQueue));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    world_down();
}

TEST_CASE("connect with no name posts the auto request and waits for an address", "[kode_wifi]")
{
    world_up();
    fake_time_delay_hook(&reach_net);

    TEST_ASSERT_TRUE(kode_wifi_connect(NULL, NULL));
    QF_gc(radio_only(WIFI_AUTO_REQ_SIG));
    world_down();
}

TEST_CASE("connect to a name that differs in case is another network", "[kode_wifi]")
{
    world_up();
    reach_ip("net");

    TEST_ASSERT_FALSE(kode_wifi_connect("NET", "pw"));
    TEST_ASSERT_EQUAL_STRING("no address from 'NET' within 30 s", kode_error());
    expect_join("NET", "pw");
    world_down();
}

TEST_CASE("connect that never gets an address fails after the bound and says so", "[kode_wifi]")
{
    world_up();

    TEST_ASSERT_FALSE(kode_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("no address from 'net' within 30 s", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_WIFI_CONNECT_MS, kode_sys_time_ms());
    expect_join("net", "pw");

    TEST_ASSERT_FALSE(kode_wifi_connect(NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("no remembered network answered within 30 s", kode_error());
    QF_gc(radio_only(WIFI_AUTO_REQ_SIG));
    world_down();
}

TEST_CASE("connect waits for a full queue to take the join, then joins", "[kode_wifi]")
{
    world_up();
    radio_fill_to_margin();
    fake_time_delay_hook(&room_then_net);

    TEST_ASSERT_TRUE(kode_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    expect_join("net", "pw");
    world_down();
}

TEST_CASE("connect with a queue that stays full is busy and posts nothing more", "[kode_wifi]")
{
    world_up();
    radio_fill_to_margin();

    TEST_ASSERT_FALSE(kode_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("Wi-Fi is busy", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(DOOR_BOUND_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_CMD_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    world_down();
}

TEST_CASE("connect refuses a bad name or password before it posts anything", "[kode_wifi]")
{
    world_up();

    TEST_ASSERT_FALSE(kode_wifi_connect("", "pw"));
    TEST_ASSERT_EQUAL_STRING("bad network name", kode_error());
    TEST_ASSERT_FALSE(kode_wifi_connect(NAME_33, "pw"));
    TEST_ASSERT_EQUAL_STRING("bad network name", kode_error());
    TEST_ASSERT_FALSE(kode_wifi_connect("net", PASS_65));
    TEST_ASSERT_EQUAL_STRING("password too long", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Radio->eQueue));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());

    /* The longest of each is fine. */
    fake_time_delay_hook(&reach_long);
    TEST_ASSERT_TRUE(kode_wifi_connect(NAME_32, PASS_64));
    expect_join(NAME_32, PASS_64);
    world_down();
}

/* ---- is_connected ---------------------------------------------------------------- */

TEST_CASE("is_connected reads the driver and never waits", "[kode_wifi]")
{
    world_up();

    TEST_ASSERT_FALSE(kode_wifi_is_connected());
    reach_ip("net");
    TEST_ASSERT_TRUE(kode_wifi_is_connected());
    net_drops();
    TEST_ASSERT_FALSE(kode_wifi_is_connected());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Radio->eQueue));
    world_down();
}

/* ---- disconnect ------------------------------------------------------------------ */

TEST_CASE("disconnect posts a leave and returns true once the address is gone", "[kode_wifi]")
{
    world_up();
    reach_ip("net");
    fake_time_delay_hook(&net_drops);

    TEST_ASSERT_TRUE(kode_wifi_disconnect());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    QF_gc(radio_only(WIFI_LEAVE_REQ_SIG));
    TEST_ASSERT_LESS_THAN_UINT32(KODE_WIFI_LEAVE_MS, kode_sys_time_ms());
    world_down();
}

TEST_CASE("disconnect when not connected posts nothing", "[kode_wifi]")
{
    world_up();

    TEST_ASSERT_TRUE(kode_wifi_disconnect());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Radio->eQueue));
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    world_down();
}

TEST_CASE("disconnect that the radio does not carry out fails after the bound", "[kode_wifi]")
{
    world_up();
    reach_ip("net");

    TEST_ASSERT_FALSE(kode_wifi_disconnect());
    TEST_ASSERT_EQUAL_STRING("could not leave 'net' within 5 s", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_WIFI_LEAVE_MS, kode_sys_time_ms());
    QF_gc(radio_only(WIFI_LEAVE_REQ_SIG));
    world_down();
}

/* ---- Scripted: the join seen trying, and a firmware without Wi-Fi ----------------- */

TEST_CASE("a join seen connecting and then refused fails early and names the network", "[kode_wifi]")
{
    static const seam_step_t script[] = {
        { KDRV_WIFI_IDLE, "" }, { KDRV_WIFI_IDLE, "" }, { KDRV_WIFI_CONNECTING, "net" },
        { KDRV_WIFI_CONNECTING, "net" }, { KDRV_WIFI_DISCONNECTED, "" },
    };

    fresh();
    seam_script(script, 5U, KODE_OK, KODE_OK);

    TEST_ASSERT_FALSE(seam_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("could not join 'net': wrong password or out of range", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_joins);
    /* Three sleeps of the thirty-odd hundred the bound would have taken. */
    TEST_ASSERT_EQUAL_UINT32(SEAM_SLEEPS * KODE_RETRY_STEP_MS, kode_sys_time_ms());
}

TEST_CASE("a join seen connecting and then idle again is refused the same way", "[kode_wifi]")
{
    static const seam_step_t script[] = {
        { KDRV_WIFI_IDLE, "" }, { KDRV_WIFI_CONNECTING, "net" }, { KDRV_WIFI_IDLE, "" },
    };

    fresh();
    seam_script(script, 3U, KODE_OK, KODE_OK);

    TEST_ASSERT_FALSE(seam_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("could not join 'net': wrong password or out of range", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_WIFI_CONNECT_MS, kode_sys_time_ms());
}

TEST_CASE("a disconnect that was there before the join does not fail it", "[kode_wifi]")
{
    static const seam_step_t script[] = {
        { KDRV_WIFI_DISCONNECTED, "" }, { KDRV_WIFI_DISCONNECTED, "" },
        { KDRV_WIFI_CONNECTING, "net" }, { KDRV_WIFI_GOT_IP, "net" },
    };

    fresh();
    seam_script(script, 4U, KODE_OK, KODE_OK);

    TEST_ASSERT_TRUE(seam_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("an attempt on another network that was running at the post is not this join's", "[kode_wifi]")
{
    static const seam_step_t script[] = {
        { KDRV_WIFI_CONNECTING, "old" }, { KDRV_WIFI_CONNECTING, "old" },
        { KDRV_WIFI_DISCONNECTED, "" }, { KDRV_WIFI_IDLE, "" },
        { KDRV_WIFI_CONNECTING, "net" }, { KDRV_WIFI_GOT_IP, "net" },
    };

    fresh();
    seam_script(script, 6U, KODE_OK, KODE_OK);

    TEST_ASSERT_TRUE(seam_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("an attempt on the same network that was running at the post is not this join's", "[kode_wifi]")
{
    static const seam_step_t script[] = {
        { KDRV_WIFI_CONNECTING, "net" }, { KDRV_WIFI_CONNECTING, "net" },
        { KDRV_WIFI_DISCONNECTED, "" }, { KDRV_WIFI_CONNECTING, "net" },
        { KDRV_WIFI_GOT_IP, "net" },
    };

    fresh();
    seam_script(script, 5U, KODE_OK, KODE_OK);

    TEST_ASSERT_TRUE(seam_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a remembered network that refuses is followed by the next one", "[kode_wifi]")
{
    static const seam_step_t script[] = {
        { KDRV_WIFI_IDLE, "" }, { KDRV_WIFI_CONNECTING, "first" },
        { KDRV_WIFI_DISCONNECTED, "" }, { KDRV_WIFI_CONNECTING, "second" },
        { KDRV_WIFI_GOT_IP, "second" },
    };

    fresh();
    seam_script(script, 5U, KODE_OK, KODE_OK);

    TEST_ASSERT_TRUE(seam_wifi_connect(NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("remembered networks that all refuse are judged by the bound alone", "[kode_wifi]")
{
    static const seam_step_t script[] = {
        { KDRV_WIFI_IDLE, "" }, { KDRV_WIFI_CONNECTING, "first" }, { KDRV_WIFI_DISCONNECTED, "" },
    };

    fresh();
    seam_script(script, 3U, KODE_OK, KODE_OK);

    TEST_ASSERT_FALSE(seam_wifi_connect(NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("no remembered network answered within 30 s", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_WIFI_CONNECT_MS, kode_sys_time_ms());
}

TEST_CASE("a firmware without Wi-Fi says so from every call", "[kode_wifi]")
{
    static const seam_step_t script[] = { { KDRV_WIFI_IDLE, "" } };

    fresh();
    seam_script(script, 1U, KODE_DENIED, KODE_DENIED);
    TEST_ASSERT_FALSE(seam_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("this firmware has no Wi-Fi", kode_error());
    TEST_ASSERT_FALSE(seam_wifi_is_connected());
    TEST_ASSERT_EQUAL_STRING("this firmware has no Wi-Fi", kode_error());
    TEST_ASSERT_FALSE(seam_wifi_disconnect());
    TEST_ASSERT_EQUAL_STRING("this firmware has no Wi-Fi", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_joins);

    /* A status that answers and a door that refuses is the same firmware. */
    seam_script(script, 1U, KODE_OK, KODE_DENIED);
    TEST_ASSERT_FALSE(seam_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_STRING("this firmware has no Wi-Fi", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_joins);
}

/* ---- scan ------------------------------------------------------------------------- */

TEST_CASE("scan keeps the strongest networks that fit, strongest first", "[kode_wifi]")
{
    static const int8_t rssi[12] = { -70, -40, -90, -55, -40, -81, -62, -35, -77, -50, -88, -66 };
    kode_wifi_network_t out[5];

    fresh();
    seam_scan_script(rssi, 12U, KODE_OK);
    (void)memset(out, 0xA5, sizeof(out));

    TEST_ASSERT_EQUAL_INT(5, seam_wifi_scan(out, 5U));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_scans);
    TEST_ASSERT_EQUAL_UINT32(KODE_WIFI_SCAN_MS, s_seam_scan_ms);
    TEST_ASSERT_EQUAL_STRING("ap-7", out[0].ssid);
    TEST_ASSERT_EQUAL_INT8(-35, out[0].rssi);
    TEST_ASSERT_EQUAL_UINT8(8U, out[0].channel);
    TEST_ASSERT_FALSE(out[0].open);
    /* Two at -40: the one the sweep reported first stays first. */
    TEST_ASSERT_EQUAL_STRING("ap-1", out[1].ssid);
    TEST_ASSERT_EQUAL_STRING("ap-4", out[2].ssid);
    TEST_ASSERT_TRUE(out[2].open);
    TEST_ASSERT_EQUAL_STRING("ap-9", out[3].ssid);
    TEST_ASSERT_EQUAL_STRING("ap-3", out[4].ssid);
    TEST_ASSERT_EQUAL_INT8(-55, out[4].rssi);
}

TEST_CASE("scan with room for more than were found returns them all, in order", "[kode_wifi]")
{
    static const int8_t rssi[3] = { -80, -45, -60 };
    kode_wifi_network_t out[8];

    fresh();
    seam_scan_script(rssi, 3U, KODE_OK);
    TEST_ASSERT_EQUAL_INT(3, seam_wifi_scan(out, 8U));
    TEST_ASSERT_EQUAL_STRING("ap-1", out[0].ssid);
    TEST_ASSERT_EQUAL_STRING("ap-2", out[1].ssid);
    TEST_ASSERT_EQUAL_STRING("ap-0", out[2].ssid);

    seam_scan_script(rssi, 0U, KODE_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, seam_wifi_scan(out, 8U), "an empty air is no failure");
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("scan refuses no room before it asks for anything", "[kode_wifi]")
{
    static const int8_t rssi[1] = { -50 };
    kode_wifi_network_t out[1];

    fresh();
    seam_scan_script(rssi, 1U, KODE_OK);
    TEST_ASSERT_EQUAL_INT(-1, seam_wifi_scan(out, 0U));
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(kode_error()));
    TEST_ASSERT_EQUAL_INT(-1, seam_wifi_scan(NULL, 4U));
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(kode_error()));
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_scans);
}

TEST_CASE("scan that fails says why and returns -1", "[kode_wifi]")
{
    static const int8_t rssi[2] = { -50, -60 };
    kode_wifi_network_t out[4];

    fresh();
    seam_scan_script(rssi, 2U, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT(-1, seam_wifi_scan(out, 4U));
    TEST_ASSERT_EQUAL_STRING("this firmware has no Wi-Fi", kode_error());

    seam_scan_script(rssi, 2U, KODE_NO_RESOURCES);
    TEST_ASSERT_EQUAL_INT(-1, seam_wifi_scan(out, 4U));
    TEST_ASSERT_EQUAL_STRING("the radio is busy with a scan, a self-test or an install; try again",
                             kode_error());

    seam_scan_script(rssi, 2U, KODE_TIMEOUT);
    TEST_ASSERT_EQUAL_INT(-1, seam_wifi_scan(out, 4U));
    TEST_ASSERT_EQUAL_STRING("no answer to the scan within 110 s", kode_error());

    seam_scan_script(rssi, 2U, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, seam_wifi_scan(out, 4U));
    TEST_ASSERT_EQUAL_STRING("the Wi-Fi scan failed", kode_error());

    /* A good scan after a failed one clears the error. */
    seam_scan_script(rssi, 2U, KODE_OK);
    TEST_ASSERT_EQUAL_INT(2, seam_wifi_scan(out, 4U));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

/* ---- Two tasks, one Wi-Fi ---------------------------------------------------------- */

/** @brief Stack of the second task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of the second task. */
#define TEST_PRIO 1U

/** @brief The longest the case waits for the second task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief Ticks the first task sleeps between two looks at the second. */
#define TEST_POLL_TICKS 1U

static SemaphoreHandle_t s_two_go;       /**< Given to let the second task begin.   */
static SemaphoreHandle_t s_two_at_door;  /**< Given by it just before its call.     */
static SemaphoreHandle_t s_two_done;     /**< Given by it when its call returned.   */
static TaskHandle_t      s_two_task;     /**< The second task.                      */
static atomic_bool       s_two_returned; /**< Its call returned.                    */
static bool              s_two_ok;       /**< What its call answered.               */
static char              s_two_error[ERROR_COPY]; /**< Its error after the call.    */

/** @brief The second task: one join of the same network, at the moment it is let go. */
static void two_second(void *arg)
{
    (void)arg;
    (void)xSemaphoreTake(s_two_go, pdMS_TO_TICKS(TEST_WAIT_MS));
    s_two_task = xTaskGetCurrentTaskHandle();
    (void)xSemaphoreGive(s_two_at_door);
    s_two_ok = kode_wifi_connect("net", "pw");
    (void)strncpy(s_two_error, kode_error(), sizeof(s_two_error) - 1U);
    s_two_error[sizeof(s_two_error) - 1U] = '\0';
    atomic_store(&s_two_returned, true);
    (void)xSemaphoreGive(s_two_done);
}

/** @brief From inside the first task's wait: let the second call in, then give the address. */
static void two_let_in(void)
{
    bool settled = (xSemaphoreGive(s_two_go) == pdTRUE)
                   && (xSemaphoreTake(s_two_at_door, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);

    for ( uint32_t i = 0U; settled && (pdMS_TO_TICKS(TEST_WAIT_MS) > i) && !atomic_load(&s_two_returned)
                          && (eTaskGetState(s_two_task) != eBlocked); i++ )
    {
        vTaskDelay(TEST_POLL_TICKS);
    }
    reach_net();
}

TEST_CASE("two tasks joining at once are served one after the other", "[kode_wifi]")
{
    world_up();
    s_two_go      = xSemaphoreCreateBinary();
    s_two_at_door = xSemaphoreCreateBinary();
    s_two_done    = xSemaphoreCreateBinary();
    s_two_task    = NULL;
    s_two_ok      = false;
    s_two_error[0] = '\0';
    atomic_store(&s_two_returned, false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&two_second, "t_wifi", TEST_STACK_BYTES,
                                                    TEST_PRIO, NULL));

    fake_time_delay_hook(&two_let_in);
    TEST_ASSERT_TRUE(kode_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_two_done, pdMS_TO_TICKS(TEST_WAIT_MS)));
    vSemaphoreDelete(s_two_go);
    vSemaphoreDelete(s_two_at_door);
    vSemaphoreDelete(s_two_done);

    /* The second came in after the first had its address, so it found it and posted nothing. */
    TEST_ASSERT_TRUE(s_two_ok);
    TEST_ASSERT_EQUAL_STRING("", s_two_error);
    expect_join("net", "pw");
    world_down();
}

/* ---- The events -------------------------------------------------------------------- */

/** @brief Records a listener can keep. */
#define REC_MAX  8U

/** @brief Longest text a record keeps. */
#define REC_TEXT 64U

/** @brief What the listener saw. */
typedef struct {
    char name[REC_TEXT];    /**< The event's name.   */
    char source[REC_TEXT];  /**< Its source.         */
} rec_t;

static rec_t             s_rec[REC_MAX];  /**< What the listener saw, in order. */
static volatile uint32_t s_rec_n;         /**< How many records.                */
static SemaphoreHandle_t s_given;         /**< Counts the calls.                */

/** @brief Record one call and tell the case. */
static void on_wifi_event(const kode_event_t *e)
{
    const uint32_t i = s_rec_n;

    if ( REC_MAX > i )
    {
        (void)snprintf(s_rec[i].name, REC_TEXT, "%s", e->name);
        (void)snprintf(s_rec[i].source, REC_TEXT, "%s", e->source);
    }
    s_rec_n = i + 1U;
    (void)xSemaphoreGive(s_given);
}

/** @brief Wait for @p n more calls; false if they do not all come. */
static bool rec_wait(uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; (i < n) && ok; i++ )
    {
        ok = (xSemaphoreTake(s_given, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    return ok;
}

/** @brief Publish WIFI_STATE_SIG the way AO_Radio does. */
static void publish_wifi(kdrv_wifi_state_t state, const char *ssid)
{
    WifiStateEvt *const e = Q_NEW(WifiStateEvt, WIFI_STATE_SIG);

    e->state = (uint8_t)state;
    e->ip4   = 0U;
    (void)snprintf(e->ssid, sizeof(e->ssid), "%s", ssid);
    e->rssi    = 0;
    e->enabled = true;
    QACTIVE_PUBLISH(&e->super, (void *)0);
}

/** @brief The layer up and hearing the radio, its memory of the link cleared, a listener on both events. */
static void events_up(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    QActive_subscribe(AO_KodeStandard, WIFI_STATE_SIG);

    /* Whatever an earlier case left, the Active Object now believes the link is down. */
    publish_wifi(KDRV_WIFI_IDLE, "");
    standard_ao_support_drained();

    if ( NULL == s_given )
    {
        s_given = xSemaphoreCreateCounting(REC_MAX, 0U);
    }
    while ( xSemaphoreTake(s_given, 0U) == pdTRUE )
    {
    }
    s_rec_n = 0U;
    TEST_ASSERT_TRUE(kode_event_add_listener("wifi_connected", &on_wifi_event));
    TEST_ASSERT_TRUE(kode_event_add_listener("wifi_lost", &on_wifi_event));
}

/** @brief Off the bus again, so the cases of other files never reach the layer through it. */
static void events_down(void)
{
    TEST_ASSERT_TRUE(kode_event_remove_listener("wifi_connected", &on_wifi_event));
    TEST_ASSERT_TRUE(kode_event_remove_listener("wifi_lost", &on_wifi_event));
    QActive_unsubscribe(AO_KodeStandard, WIFI_STATE_SIG);
}

TEST_CASE("an address becomes wifi_connected and its loss wifi_lost, once each", "[kode_wifi]")
{
    events_up();

    publish_wifi(KDRV_WIFI_GOT_IP, "net");
    publish_wifi(KDRV_WIFI_GOT_IP, "net");   /* the same link again: no second event */
    publish_wifi(KDRV_WIFI_DISCONNECTED, "");
    publish_wifi(KDRV_WIFI_DISCONNECTED, "");
    publish_wifi(KDRV_WIFI_GOT_IP, "other");
    publish_wifi(KDRV_WIFI_IDLE, "");
    TEST_ASSERT_TRUE(rec_wait(4U));
    standard_ao_support_drained();

    TEST_ASSERT_EQUAL_UINT32(4U, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("wifi_connected", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("net", s_rec[0].source);
    TEST_ASSERT_EQUAL_STRING("wifi_lost", s_rec[1].name);
    TEST_ASSERT_EQUAL_STRING("net", s_rec[1].source);
    TEST_ASSERT_EQUAL_STRING("wifi_connected", s_rec[2].name);
    TEST_ASSERT_EQUAL_STRING("other", s_rec[2].source);
    TEST_ASSERT_EQUAL_STRING("wifi_lost", s_rec[3].name);
    TEST_ASSERT_EQUAL_STRING("other", s_rec[3].source);
    events_down();
}

TEST_CASE("a connecting link is not an address", "[kode_wifi]")
{
    events_up();

    publish_wifi(KDRV_WIFI_CONNECTING, "net");
    publish_wifi(KDRV_WIFI_GOT_IP, "net");
    TEST_ASSERT_TRUE(rec_wait(1U));
    publish_wifi(KDRV_WIFI_DISCONNECTED, "");
    TEST_ASSERT_TRUE(rec_wait(1U));
    standard_ao_support_drained();

    TEST_ASSERT_EQUAL_UINT32(2U, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("wifi_connected", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("wifi_lost", s_rec[1].name);
    events_down();
}

TEST_CASE("the Wi-Fi events are known names in any case", "[kode_wifi]")
{
    events_up();

    TEST_ASSERT_TRUE(kode_event_add_listener("WIFI_CONNECTED", &on_wifi_event));
    TEST_ASSERT_TRUE(kode_event_add_listener("Wifi_Lost", &on_wifi_event));
    TEST_ASSERT_FALSE(kode_event_add_listener("wifi_gone", &on_wifi_event));
    TEST_ASSERT_EQUAL_STRING("unknown event 'wifi_gone'", kode_error());
    events_down();
}
