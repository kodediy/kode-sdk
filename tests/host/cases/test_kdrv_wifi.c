/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_wifi.c
 * @brief What the driver hands the remote stack when asked to join.
 */
#include <string.h>

#include "unity.h"

#include "esp_system.h"

#include "Mockesp_wifi.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi_default.h"
#include "Mockesp_event.h"

#include "fake_wifi_events.h"
#include "fakes.h"
#include "kdrv_wifi.h"

/** @brief Exactly 32 characters - the longest name 802.11 permits. */
#define SSID_32  "0123456789abcdef0123456789abcdef"
/** @brief Exactly 64 characters - a raw PMK, which is a legal password. */
#define PSK_64   "0123456789abcdef0123456789abcdef" \
                 "0123456789abcdef0123456789abcdef"

/** @brief Bytes for a network name copied out of the driver, NUL included. */
#define SSID_COPY_BYTES  33U
/** @brief A buffer too small for "home": two characters and the NUL. */
#define SSID_CUT_BYTES   3U
/** @brief What a buffer holds before a copy writes into it. */
#define SSID_FILL        'z'

/** @brief The driver's network name, copied out. */
static const char *ssid_now(void)
{
    static char name[SSID_COPY_BYTES];

    kdrv_wifi_ssid_copy(name, sizeof(name));
    return name;
}

/** @brief The config the driver handed to the stack, captured by the stub. */
static wifi_config_t s_seen;
static int           s_set_calls;

/**
 * @brief Stands in for esp_wifi_set_config and keeps what it was given.
 * @param iface Ignored.
 * @param conf  The configuration under inspection.
 * @param calls CMock's invocation counter.
 * @return ESP_OK, so the caller carries on to the connect.
 */
static esp_err_t capture_config(wifi_interface_t iface, wifi_config_t *conf,
                                int calls)
{
    (void)iface;
    (void)calls;
    if ( NULL != conf )
    {
        s_seen = *conf;
    }
    ++s_set_calls;
    return ESP_OK;
}

/** @brief The driver's stack is up: set by wifi_bring_up() and by the adopt
 *         cases, which leave it started. */
static bool          s_stack_up;

/**
 * @brief Bring the remote stack up once per binary, all of it mocked.
 *
 * kdrv_wifi keeps `started`, `netif` and its event group in file statics, so
 * this runs once and every later case inherits a stack that is up and idle.
 */
static void wifi_bring_up(void)
{
    if ( s_stack_up )
    {
        return;
    }

    esp_netif_init_ExpectAndReturn(ESP_OK);
    /* The stub keeps the handler, so a case can deliver the stack's events
     * by hand; see fakes/fake_wifi_events.c. */
    esp_event_handler_instance_register_Stub(&fake_wifi_events_register);
    esp_wifi_init_ExpectAnyArgsAndReturn(ESP_OK);
    esp_wifi_set_mode_ExpectAndReturn(WIFI_MODE_STA, ESP_OK);
    esp_wifi_start_ExpectAndReturn(ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_start());

    s_stack_up = true;
}

/**
 * @brief Drive one join attempt to the point where the credentials are set.
 *
 * The wait for a result is left to time out on a 1 ms deadline: what is under
 * test is what went onto the wire, not what came back.
 *
 * @param ssid Network name to offer.
 * @param pass Password, or NULL for an open network.
 */
static void attempt_join(const char *ssid, const char *pass)
{
    (void)memset(&s_seen, 0, sizeof(s_seen));
    s_set_calls = 0;

    /* First join of the binary also creates the netif; later ones do not. */
    esp_netif_create_default_wifi_sta_IgnoreAndReturn((esp_netif_t *)&s_seen);
    esp_wifi_stop_IgnoreAndReturn(ESP_OK);
    esp_wifi_start_IgnoreAndReturn(ESP_OK);

    esp_wifi_set_config_Stub(&capture_config);
    esp_wifi_connect_IgnoreAndReturn(ESP_OK);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);
    esp_wifi_sta_get_ap_info_IgnoreAndReturn(ESP_FAIL);
    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_OK);

    (void)kdrv_wifi_connect(ssid, pass, 1U);
}

/* ---- Adopting a station the C5 already holds ------------------------------
 *
 * kdrv_wifi keeps `started` and `netif` in file statics. Every case here
 * starts with kdrv_wifi_forget() (started goes), arms the netif create, and
 * judges the calls that actually fired, so it passes whether or not the netif
 * already exists and in any order.
 */

/** @brief The calls the adopt path makes, in the order they were made. */
typedef enum {
    CALL_NETIF_INIT = 1,
    CALL_WIFI_INIT,
    CALL_SET_MODE,
    CALL_NETIF_CREATE,
    CALL_WIFI_START,
    CALL_CONNECT
} adopt_call_t;

/** @brief Room for every call one adopt makes, with a margin. */
#define ADOPT_TRACE_MAX  16U

/** @brief The recorded order, filled by the stubs below. */
static adopt_call_t s_trace[ADOPT_TRACE_MAX];
static unsigned     s_trace_n;

/** @brief What the C5 reports as the associated network. */
#define ADOPT_SSID  "home"
/** @brief The address the lease carries: 192.168.1.137. */
#define ADOPT_IP4   0x8901A8C0U

/** @brief What the "C5" does when asked to connect. */
typedef enum {
    C5_SILENT,       /**< Nothing arrives.                          */
    C5_GIVES_IP,     /**< STA_CONNECTED replayed, DHCP answers.     */
    C5_DISCONNECTS   /**< The station is gone.                      */
} c5_mode_t;

static c5_mode_t s_c5_mode;

/** @brief Remember that @p call happened. */
static void trace(adopt_call_t call)
{
    if ( ADOPT_TRACE_MAX > s_trace_n )
    {
        s_trace[s_trace_n] = call;
        ++s_trace_n;
    }
}

static esp_err_t stub_netif_init(int calls)
{
    (void)calls;
    trace(CALL_NETIF_INIT);
    return ESP_OK;
}

static esp_err_t stub_wifi_init(const wifi_init_config_t *cfg, int calls)
{
    (void)cfg; (void)calls;
    trace(CALL_WIFI_INIT);
    return ESP_OK;
}

static esp_err_t stub_set_mode(wifi_mode_t mode, int calls)
{
    (void)calls;
    TEST_ASSERT_EQUAL_INT((int)WIFI_MODE_STA, (int)mode);
    trace(CALL_SET_MODE);
    return ESP_OK;
}

static esp_netif_t *stub_netif_create(int calls)
{
    (void)calls;
    trace(CALL_NETIF_CREATE);
    return (esp_netif_t *)&s_seen;
}

static esp_err_t stub_wifi_start(int calls)
{
    (void)calls;
    trace(CALL_WIFI_START);
    return ESP_OK;
}

/** @brief The C5 answers the connect the way s_c5_mode says. */
static esp_err_t stub_connect(int calls)
{
    (void)calls;
    trace(CALL_CONNECT);
    if ( C5_GIVES_IP == s_c5_mode )
    {
        ip_event_got_ip_t got;
        (void)memset(&got, 0, sizeof(got));
        got.ip_info.ip.addr = ADOPT_IP4;
        fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &got);
    }
    else if ( C5_DISCONNECTS == s_c5_mode )
    {
        fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    }
    else
    {
        /* Silent: the wait runs out. */
    }
    return ESP_OK;
}

/** @brief The association the C5 reports once it holds one. */
static esp_err_t stub_ap_info(wifi_ap_record_t *ap, int calls)
{
    (void)calls;
    (void)memset(ap, 0, sizeof(*ap));
    (void)memcpy(ap->ssid, ADOPT_SSID, sizeof(ADOPT_SSID));
    ap->rssi = -47;
    return ESP_OK;
}

/**
 * @brief Arm every call an adopt reaches with a stub that records it.
 *
 * esp_wifi_stop() and esp_wifi_disconnect() are NOT armed here: a case that
 * lets the adopt reach them fails on the unexpected call.
 *
 * @param mode What the C5 does when connect() is called.
 */
static void adopt_arm(c5_mode_t mode)
{
    s_trace_n  = 0U;
    s_c5_mode  = mode;
    esp_event_handler_instance_register_Stub(&fake_wifi_events_register);
    esp_netif_init_Stub(&stub_netif_init);
    esp_netif_create_default_wifi_sta_Stub(&stub_netif_create);
    esp_wifi_init_Stub(&stub_wifi_init);
    esp_wifi_set_mode_Stub(&stub_set_mode);
    esp_wifi_start_Stub(&stub_wifi_start);
    esp_wifi_connect_Stub(&stub_connect);
    esp_wifi_sta_get_ap_info_Stub(&stub_ap_info);
    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_OK);
}

/** @brief Fresh mocks, as the first cases of this file start. */
static void adopt_mocks_init(void)
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();
}

/** @brief Release the mocks and leave the driver started, as the cases after
 *         this block expect to find it. */
static void adopt_mocks_done(void)
{
    s_stack_up = true;
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

TEST_CASE("adopt creates the station netif before it starts the Wi-Fi",
          "[kdrv_wifi]")
{
    adopt_mocks_init();
    kdrv_wifi_forget();
    adopt_arm(C5_GIVES_IP);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_adopt(50U));

    /* The order is the point: a netif made after the start never hears
     * STA_START, and one made with the stack stopped and started around it
     * drops the association the C5 is holding. esp_wifi_stop() and
     * esp_wifi_disconnect() are unexpected calls here, so a path through
     * either fails the case. */
    unsigned creates = 0U;
    for ( unsigned i = 0U; i < s_trace_n; ++i )
    {
        if ( CALL_NETIF_CREATE == s_trace[i] )
        {
            ++creates;
        }
    }
    /* Once on a fresh driver, none when an earlier case already made the
     * netif; never twice. */
    TEST_ASSERT_TRUE(1U >= creates);

    static const adopt_call_t head[] = {
        CALL_NETIF_INIT, CALL_WIFI_INIT, CALL_SET_MODE
    };
    unsigned n = 0U;
    for ( unsigned i = 0U; (sizeof(head) / sizeof(head[0])) > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT((int)head[i], (int)s_trace[n]);
        ++n;
    }
    if ( 1U == creates )
    {
        TEST_ASSERT_EQUAL_INT((int)CALL_NETIF_CREATE, (int)s_trace[n]);
        ++n;
    }
    TEST_ASSERT_EQUAL_INT((int)CALL_WIFI_START, (int)s_trace[n]);
    ++n;
    TEST_ASSERT_EQUAL_INT((int)CALL_CONNECT, (int)s_trace[n]);
    ++n;
    TEST_ASSERT_EQUAL_UINT(n, s_trace_n);

    Mockesp_wifi_Verify();
    adopt_mocks_done();
}

TEST_CASE("adopt answers OK once the IP arrives", "[kdrv_wifi]")
{
    adopt_mocks_init();
    kdrv_wifi_forget();
    adopt_arm(C5_GIVES_IP);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_adopt(50U));

    uint32_t ip = 0U;
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_GOT_IP, (int)kdrv_wifi_state(&ip));
    TEST_ASSERT_EQUAL_HEX32(ADOPT_IP4, ip);
    /* wifi_info names the network, as after a join. */
    TEST_ASSERT_EQUAL_STRING(ADOPT_SSID, ssid_now());
    TEST_ASSERT_EQUAL_INT8(-47, kdrv_wifi_rssi_last());

    adopt_mocks_done();
}

TEST_CASE("adopt answers DENIED on a disconnect", "[kdrv_wifi]")
{
    adopt_mocks_init();
    kdrv_wifi_forget();
    adopt_arm(C5_DISCONNECTS);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_adopt(50U));
    TEST_ASSERT_EQUAL_STRING("", ssid_now());

    adopt_mocks_done();
}

TEST_CASE("adopt answers TIMEOUT when nothing arrives", "[kdrv_wifi]")
{
    adopt_mocks_init();
    kdrv_wifi_forget();
    adopt_arm(C5_SILENT);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);

    /* A deadline of 0 ms, as the vault cases use: what is under test is the
     * answer to an empty wait, not the host scheduler. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_wifi_adopt(0U));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_IDLE, (int)kdrv_wifi_state(NULL));

    adopt_mocks_done();
}

TEST_CASE("a join after a failed adopt still works", "[kdrv_wifi]")
{
    adopt_mocks_init();
    kdrv_wifi_forget();
    adopt_arm(C5_SILENT);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_wifi_adopt(0U));

    /* The failed adopt left the stack up with its netif. The fallback join
     * must reuse both: no second netif, no stop or start around one
     * (unarmed, so either would fail here), no second bring-up. */
    s_trace_n = 0U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    TEST_ASSERT_EQUAL_UINT(0U, s_trace_n);
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_IDLE, (int)kdrv_wifi_state(NULL));

    Mockesp_wifi_Verify();
    Mockesp_netif_Verify();
    adopt_mocks_done();
}

TEST_CASE("a 32-character SSID reaches the stack whole", "[kdrv_wifi]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();
    attempt_join(SSID_32, "secret");

    TEST_ASSERT_EQUAL_INT(1, s_set_calls);

    /* The field has no terminator at this length, so compare the bytes. A
     * copy that wrote a NUL would have lost the last character. */
    TEST_ASSERT_EQUAL_UINT(32U, strlen(SSID_32));
    TEST_ASSERT_EQUAL_MEMORY(SSID_32, s_seen.sta.ssid, 32U);

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

TEST_CASE("a 64-character PSK reaches the stack whole", "[kdrv_wifi]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();
    attempt_join("home", PSK_64);

    TEST_ASSERT_EQUAL_UINT(64U, strlen(PSK_64));
    TEST_ASSERT_EQUAL_MEMORY(PSK_64, s_seen.sta.password, 64U);

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* Anything shorter than the field must still be terminated, because that is
 * how the stack learns where the name ends. The config is zeroed before the
 * copy, so the terminator is the zero that was already there. */
TEST_CASE("a short SSID is still terminated inside the field", "[kdrv_wifi]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();
    attempt_join("kode", "pw");

    TEST_ASSERT_EQUAL_STRING("kode", (const char *)s_seen.sta.ssid);
    TEST_ASSERT_EQUAL_UINT8(0U, s_seen.sta.ssid[4]);

    /* And nothing of a previous, longer attempt survives into this one. */
    for ( unsigned i = 5U; 32U > i; ++i )
    {
        TEST_ASSERT_EQUAL_UINT8(0U, s_seen.sta.ssid[i]);
    }

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* An open network is a NULL password, and it must leave the field empty
 * rather than carrying whatever the last join used. */
TEST_CASE("an open network sends an empty password", "[kdrv_wifi]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();
    attempt_join("cafe", NULL);

    TEST_ASSERT_EQUAL_UINT8(0U, s_seen.sta.password[0]);

    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

TEST_CASE("a join with no network name is refused before the stack is asked",
          "[kdrv_wifi]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();

    /* No expectations queued: reaching the stack at all would fail here. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_connect(NULL, "pw", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_connect("", "pw", 1U));

    Mockesp_wifi_Verify();
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/* ---- A join the C5 makes from its vault: prepare here, await here --------- */

/** @brief The stubs prepare() and await() reach but for the band call, all
 *         permissive; a case that expects the band calls itself starts here. */
static void join_stubs_no_band(void)
{
    esp_netif_create_default_wifi_sta_IgnoreAndReturn((esp_netif_t *)&s_seen);
    esp_wifi_stop_IgnoreAndReturn(ESP_OK);
    esp_wifi_start_IgnoreAndReturn(ESP_OK);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);
    esp_wifi_sta_get_ap_info_IgnoreAndReturn(ESP_FAIL);
}

/** @brief The stubs prepare() and await() reach, all permissive. */
static void join_stubs(void)
{
    join_stubs_no_band();
    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_OK);
}

TEST_CASE("a vault join: prepare clears the way, await reports the address",
          "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    /* Readied, not connecting: the C5 has not been asked yet. */
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_IDLE, (int)kdrv_wifi_state(NULL));

    /* The C5 associated by itself; this netif's DHCP got the lease. */
    ip_event_got_ip_t got;
    (void)memset(&got, 0, sizeof(got));
    got.ip_info.ip.addr = 0x8901A8C0U;   /* 192.168.1.137 */
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &got);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await("home", 50U));
    uint32_t ip = 0U;
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_GOT_IP, (int)kdrv_wifi_state(&ip));
    TEST_ASSERT_EQUAL_HEX32(0x8901A8C0U, ip);
}

TEST_CASE("a vault join the network turns away is a refusal", "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
}

/** @brief A disconnect as the stack posts it, carrying @p reason. */
static void deliver_disconnect(uint16_t reason)
{
    wifi_event_sta_disconnected_t ev;

    (void)memset(&ev, 0, sizeof(ev));
    ev.reason = reason;
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, &ev);
}

TEST_CASE("a disconnect's reason is sorted into what a person can act on",
          "[kdrv_wifi]")
{
    static const struct {
        uint16_t         reason;
        kdrv_wifi_fail_t fail;
    } CASES[] = {
        { (uint16_t)WIFI_REASON_AUTH_FAIL,              KDRV_WIFI_FAIL_PASSWORD },
        { (uint16_t)WIFI_REASON_MIC_FAILURE,            KDRV_WIFI_FAIL_PASSWORD },
        { (uint16_t)WIFI_REASON_HANDSHAKE_TIMEOUT,      KDRV_WIFI_FAIL_HANDSHAKE },
        { (uint16_t)WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT, KDRV_WIFI_FAIL_HANDSHAKE },
        { (uint16_t)WIFI_REASON_NO_AP_FOUND,            KDRV_WIFI_FAIL_NOT_FOUND },
        { (uint16_t)WIFI_REASON_BEACON_TIMEOUT,         KDRV_WIFI_FAIL_NOT_FOUND },
        { (uint16_t)WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD, KDRV_WIFI_FAIL_NOT_FOUND },
        { (uint16_t)WIFI_REASON_ASSOC_TOOMANY,          KDRV_WIFI_FAIL_OTHER },
        { (uint16_t)WIFI_REASON_NO_AP_FOUND_W_COMPATIBLE_SECURITY, KDRV_WIFI_FAIL_OTHER },
        { (uint16_t)WIFI_REASON_UNSPECIFIED,            KDRV_WIFI_FAIL_OTHER },
    };

    for ( size_t i = 0U; i < (sizeof(CASES) / sizeof(CASES[0])); ++i )
    {
        TEST_ASSERT_EQUAL_INT((int)CASES[i].fail,
                              (int)kdrv_wifi_fail_of(CASES[i].reason));
    }
}

TEST_CASE("the refusal is what a join reports, not the leave that follows it",
          "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_FAIL_NONE, (int)kdrv_wifi_fail_last());

    deliver_disconnect((uint16_t)WIFI_REASON_AUTH_FAIL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
    /* The station's own step off the network, arriving behind the refusal. */
    deliver_disconnect((uint16_t)WIFI_REASON_ASSOC_LEAVE);
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_FAIL_PASSWORD, (int)kdrv_wifi_fail_last());

    /* The next attempt starts with nothing against it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_FAIL_NONE, (int)kdrv_wifi_fail_last());
    deliver_disconnect((uint16_t)WIFI_REASON_NO_AP_FOUND);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_FAIL_NOT_FOUND, (int)kdrv_wifi_fail_last());
}

/** @brief Whether a captured log line holds @p words. */
static bool wifi_log_says(const char *words)
{
    bool found = false;

    for ( unsigned i = 0U; (!found) && (fake_klog_count() > i); ++i )
    {
        found = (strstr(fake_klog_line(i), words) != NULL);
    }
    return found;
}

/* A handshake that times out is a wrong password or a signal too weak to
 * finish it, and the driver says which of the two it cannot tell apart: the
 * stack's own number is what tells an engineer which of its reasons it was. */
TEST_CASE("a refusal's log line carries the stack's own reason", "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_klog_reset();
    deliver_disconnect((uint16_t)WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_TRUE_MESSAGE(wifi_log_says("'home' refused us (handshake timed out, reason 15)"),
                             fake_klog_last());

    /* A refusal that is no timeout is the password, said outright. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_klog_reset();
    deliver_disconnect((uint16_t)WIFI_REASON_AUTH_FAIL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_TRUE_MESSAGE(wifi_log_says("'home' refused us (wrong password, reason 202)"),
                             fake_klog_last());

    /* The next attempt's line carries its own reason, not the last one's. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_klog_reset();
    deliver_disconnect((uint16_t)WIFI_REASON_ASSOC_TOOMANY);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_TRUE_MESSAGE(wifi_log_says("'home' refused us (other failure, reason 5)"),
                             fake_klog_last());

    /* A disconnect that carries nothing has no number to give. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_klog_reset();
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_TRUE_MESSAGE(wifi_log_says("'home' refused us (other failure, reason 0)"),
                             fake_klog_last());
}

TEST_CASE("a disconnect that carries nothing is the other failure, and an "
          "address clears it", "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_FAIL_OTHER, (int)kdrv_wifi_fail_last());

    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &(ip_event_got_ip_t){ 0 });
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_FAIL_NONE, (int)kdrv_wifi_fail_last());
    (void)kdrv_wifi_disconnect();
}

/** @brief How many times the driver said the network in use went away. */
static unsigned s_lost;

/** @brief kdrv_wifi_on_lost()'s hook, counting. */
static void on_lost(void)
{
    ++s_lost;
}

/** @brief On a network, with the hook installed and its count at zero. */
static void on_network(void)
{
    wifi_bring_up();
    join_stubs();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP,
                             &(ip_event_got_ip_t){ 0 });
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await("home", 50U));
    s_lost = 0U;
    kdrv_wifi_on_lost(&on_lost);
}

TEST_CASE("a network that goes away by itself is reported, once", "[kdrv_wifi]")
{
    on_network();

    deliver_disconnect((uint16_t)WIFI_REASON_BEACON_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT(1U, s_lost);
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_DISCONNECTED, (int)kdrv_wifi_state(NULL));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_FAIL_NOT_FOUND, (int)kdrv_wifi_fail_last());

    /* The station saying so again is not a second network lost. */
    deliver_disconnect((uint16_t)WIFI_REASON_ASSOC_LEAVE);
    TEST_ASSERT_EQUAL_UINT(1U, s_lost);
    kdrv_wifi_on_lost(NULL);
}

/* "The network is gone" tells nobody why. The stack's reason does: a beacon
 * timeout is range or the access point, a deauthentication is the network's
 * own doing. */
TEST_CASE("a network that goes away says the stack's reason", "[kdrv_wifi]")
{
    on_network();
    fake_klog_reset();

    deliver_disconnect((uint16_t)WIFI_REASON_BEACON_TIMEOUT);
    TEST_ASSERT_TRUE_MESSAGE(wifi_log_says("the network dropped (reason 200)"), fake_klog_last());

    /* A step off that was asked for is no loss and logs none. */
    on_network();
    fake_klog_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_disconnect());
    deliver_disconnect((uint16_t)WIFI_REASON_ASSOC_LEAVE);
    TEST_ASSERT_FALSE(wifi_log_says("the network dropped"));
    kdrv_wifi_on_lost(NULL);
}

TEST_CASE("a step off the network that was asked for is not a network lost",
          "[kdrv_wifi]")
{
    on_network();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_disconnect());
    deliver_disconnect((uint16_t)WIFI_REASON_ASSOC_LEAVE);
    TEST_ASSERT_EQUAL_UINT(0U, s_lost);

    /* Nor is the one a join to another network takes first. */
    on_network();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("other", KDRV_WIFI_CHANNEL_ANY));
    deliver_disconnect((uint16_t)WIFI_REASON_ASSOC_LEAVE);
    TEST_ASSERT_EQUAL_UINT(0U, s_lost);
    kdrv_wifi_on_lost(NULL);
}

TEST_CASE("a join the network refuses is not a network lost", "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();
    s_lost = 0U;
    kdrv_wifi_on_lost(&on_lost);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    deliver_disconnect((uint16_t)WIFI_REASON_AUTH_FAIL);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_EQUAL_UINT(0U, s_lost);
    kdrv_wifi_on_lost(NULL);
}

TEST_CASE("a vault join that never gets an address times out", "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    /* A deadline of 0 ms: the host's FreeRTOS port can deadlock when its
     * tick lands inside a blocked wait, and what is under test is the
     * answer to an empty wait, not the scheduler. */
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_wifi_await("home", 0U));
}

TEST_CASE("a vault join the C5 refuses leaves Wi-Fi idle, with no network named",
          "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    /* the C5 answers the JOIN with a refusal: no await follows */
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_IDLE, (int)kdrv_wifi_state(NULL));
    TEST_ASSERT_EQUAL_STRING("", ssid_now());
}

TEST_CASE("an await that times out does not stay connecting", "[kdrv_wifi]")
{
    wifi_bring_up();
    join_stubs();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_wifi_await("home", 0U));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_IDLE, (int)kdrv_wifi_state(NULL));
}

TEST_CASE("prepare with no network name is refused", "[kdrv_wifi]")
{
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_prepare(NULL, KDRV_WIFI_CHANNEL_ANY));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_prepare("", KDRV_WIFI_CHANNEL_ANY));
}

/* ---- The band a join is aimed at ------------------------------------------- */

/** @brief The channel of an access point on 5 GHz. */
#define BAND_CH_5G   48U
/** @brief The channel of an access point on 2.4 GHz. */
#define BAND_CH_24   6U

/** @brief How many captured log lines hold @p words. */
static unsigned wifi_log_count(const char *words)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( NULL != strstr(fake_klog_line(i), words) )
        {
            ++n;
        }
    }
    return n;
}

/** @brief Fresh mocks and the stack up, with every call a join reaches armed
 *         but the band call, which the case expects for itself. */
static void band_case_start(void)
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();
    join_stubs_no_band();
}

/** @brief Check that every expected band call was made, and release the mocks. */
static void band_case_end(void)
{
    Mockesp_wifi_Verify();
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

TEST_CASE("prepare asks for the 5 GHz band alone when the access point is on it",
          "[kdrv_wifi]")
{
    band_case_start();

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_5G_ONLY, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_5G));

    band_case_end();
}

TEST_CASE("prepare asks for the 2.4 GHz band alone when the access point is on it",
          "[kdrv_wifi]")
{
    band_case_start();

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_2G_ONLY, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_24));

    band_case_end();
}

TEST_CASE("prepare asks for both bands when the channel is not known",
          "[kdrv_wifi]")
{
    band_case_start();

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_AUTO, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));

    band_case_end();
}

TEST_CASE("the band changes where the lower band ends", "[kdrv_wifi]")
{
    static const struct {
        uint8_t          channel;
        wifi_band_mode_t band;
    } AIMS[] = {
        { 1U,                             WIFI_BAND_MODE_2G_ONLY },
        { KDRV_WIFI_LAST_24_CHANNEL,      WIFI_BAND_MODE_2G_ONLY },
        { KDRV_WIFI_LAST_24_CHANNEL + 1U, WIFI_BAND_MODE_5G_ONLY },
        { 36U,                            WIFI_BAND_MODE_5G_ONLY },
        { 165U,                           WIFI_BAND_MODE_5G_ONLY },
    };

    band_case_start();

    for ( size_t i = 0U; (sizeof(AIMS) / sizeof(AIMS[0])) > i; ++i )
    {
        esp_wifi_set_band_mode_ExpectAndReturn(AIMS[i].band, ESP_OK);
        TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", AIMS[i].channel));
    }

    band_case_end();
}

TEST_CASE("await gives both bands back after a join that got its address",
          "[kdrv_wifi]")
{
    band_case_start();

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_5G_ONLY, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_5G));
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP,
                             &(ip_event_got_ip_t){ 0 });

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_AUTO, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_GOT_IP, (int)kdrv_wifi_state(NULL));

    band_case_end();
}

TEST_CASE("await gives both bands back after a refusal", "[kdrv_wifi]")
{
    band_case_start();

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_5G_ONLY, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_5G));
    deliver_disconnect((uint16_t)WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT);

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_AUTO, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_wifi_await("home", 50U));

    band_case_end();
}

TEST_CASE("await gives both bands back after a timeout", "[kdrv_wifi]")
{
    band_case_start();

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_2G_ONLY, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_24));

    esp_wifi_set_band_mode_ExpectAndReturn(WIFI_BAND_MODE_AUTO, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_wifi_await("home", 0U));

    band_case_end();
}

/** @brief The band the stack was last asked for; 0 before any ask. */
static wifi_band_mode_t s_band_asked;
/** @brief The band that stood when the first scan step was started. */
static wifi_band_mode_t s_band_at_scan;

/** @brief Stands in for esp_wifi_set_band_mode and keeps what it was given. */
static esp_err_t band_note(wifi_band_mode_t mode, int calls)
{
    (void)calls;
    s_band_asked = mode;
    return ESP_OK;
}

/** @brief Stands in for esp_wifi_scan_start: notes the band, refuses the step. */
static esp_err_t scan_step_refused(const wifi_scan_config_t *config, bool block,
                                   int calls)
{
    (void)config;
    (void)block;
    (void)calls;
    s_band_at_scan = s_band_asked;
    return ESP_FAIL;
}

TEST_CASE("a scan never runs band-limited, even after a join aimed at one band",
          "[kdrv_wifi]")
{
    s_band_asked   = (wifi_band_mode_t)0;
    s_band_at_scan = (wifi_band_mode_t)0;
    band_case_start();
    esp_wifi_set_band_mode_Stub(&band_note);
    esp_wifi_get_country_IgnoreAndReturn(ESP_FAIL);
    esp_wifi_scan_start_Stub(&scan_step_refused);
    kdrv_wifi_scan_abort(false);

    /* A join aimed at 5 GHz that no await followed: the C5's vault may refuse
     * the join between the two. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_5G));
    TEST_ASSERT_EQUAL_INT((int)WIFI_BAND_MODE_5G_ONLY, (int)s_band_asked);

    /* Every step is refused, so the sweep ends empty; what is judged is the
     * band standing when it began. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_scan());
    TEST_ASSERT_EQUAL_INT((int)WIFI_BAND_MODE_AUTO, (int)s_band_at_scan);

    band_case_end();
}

TEST_CASE("a coprocessor that refuses the band call still joins, and the log "
          "says so once", "[kdrv_wifi]")
{
    band_case_start();
    esp_wifi_set_band_mode_IgnoreAndReturn(ESP_ERR_NOT_SUPPORTED);
    fake_klog_reset();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_5G));
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP,
                             &(ip_event_got_ip_t){ 0 });
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await("home", 50U));
    TEST_ASSERT_EQUAL_INT((int)KDRV_WIFI_GOT_IP, (int)kdrv_wifi_state(NULL));

    /* The next join is refused the same way and says nothing more. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", BAND_CH_24));
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kdrv_wifi_await("home", 0U));
    TEST_ASSERT_EQUAL_UINT(1U, wifi_log_count("band mode"));

    band_case_end();
}

/* ---- Power save ---------------------------------------------------------- */

TEST_CASE("the power save is modem save when on and none when off", "[kdrv_wifi]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();

    esp_wifi_set_ps_ExpectAndReturn(WIFI_PS_MIN_MODEM, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_power_save(true));
    esp_wifi_set_ps_ExpectAndReturn(WIFI_PS_NONE, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_power_save(false));
}

TEST_CASE("a stack that refuses the power save is an error, not a silent pass",
          "[kdrv_wifi]")
{
    Mockesp_wifi_Init();
    Mockesp_netif_Init();
    Mockesp_event_Init();

    wifi_bring_up();

    esp_wifi_set_ps_ExpectAndReturn(WIFI_PS_NONE, ESP_FAIL);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_wifi_power_save(false));
}

/* ---- A restart keeps the association ---------------------------------------- */

/** @brief IDF's STA netif registers this at creation; esp_restart() runs it. */
#define STOP_HANDLER  ((shutdown_handler_t)esp_wifi_stop)

TEST_CASE("keeping the station on a restart takes esp_wifi_stop off the "
          "shutdown handlers", "[kdrv_wifi]")
{
    (void)esp_unregister_shutdown_handler(STOP_HANDLER);
    TEST_ASSERT_EQUAL_INT(ESP_OK, esp_register_shutdown_handler(STOP_HANDLER));

    kdrv_wifi_keep_station_on_restart();

    TEST_ASSERT_EQUAL_INT_MESSAGE(ESP_ERR_INVALID_STATE,
        esp_unregister_shutdown_handler(STOP_HANDLER),
        "esp_restart() would still stop the Wi-Fi the C5 is keeping");
}

TEST_CASE("keeping the station is harmless when IDF registered nothing",
          "[kdrv_wifi]")
{
    (void)esp_unregister_shutdown_handler(STOP_HANDLER);

    kdrv_wifi_keep_station_on_restart();
    kdrv_wifi_keep_station_on_restart();

    TEST_ASSERT_EQUAL_INT(ESP_ERR_INVALID_STATE,
                          esp_unregister_shutdown_handler(STOP_HANDLER));
}

TEST_CASE("the ssid copy is whole, cut to the buffer and always terminated",
          "[kdrv_wifi]")
{
    char whole[SSID_COPY_BYTES];
    char cut[SSID_CUT_BYTES];

    wifi_bring_up();
    join_stubs();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("home", KDRV_WIFI_CHANNEL_ANY));
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &(ip_event_got_ip_t){ 0 });
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await("home", 50U));

    (void)memset(whole, SSID_FILL, sizeof(whole));
    kdrv_wifi_ssid_copy(whole, sizeof(whole));
    TEST_ASSERT_EQUAL_STRING("home", whole);

    (void)memset(cut, SSID_FILL, sizeof(cut));
    kdrv_wifi_ssid_copy(cut, sizeof(cut));
    TEST_ASSERT_EQUAL_STRING("ho", cut);

    (void)memset(cut, SSID_FILL, sizeof(cut));
    kdrv_wifi_ssid_copy(cut, 0U);
    TEST_ASSERT_EQUAL_INT(SSID_FILL, cut[0]);   /* nothing to write into: untouched */
}
