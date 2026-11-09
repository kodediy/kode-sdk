/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_http.c
 * @brief kode_http_get(), kode_http_post(), kode_http_status(), kode_net_ping(),
 *        kode_net_resolve() and kode_udp_send() over the real facade and a mocked kdrv_net.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "Mockesp_event.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi.h"
#include "Mockesp_wifi_default.h"
#include "Mockkdrv_net.h"

#include "fake_wifi_events.h"
#include "fakes.h"
#include "kdrv_net.h"
#include "kdrv_wifi.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kode_task.h"
#include "kqp_config.h"

#define HTTP_IP4       0x0100A8C0U  /**< The address the fake lease carries.       */
#define HTTP_AWAIT_MS  50U          /**< The driver's own wait, already satisfied. */
#define HTTP_STACK     8192U        /**< Stack of a test task, in bytes.           */
#define HTTP_PRIO      1U           /**< Priority of a test task.                  */
#define HTTP_WAIT_MS   4000U        /**< The longest a case waits for a task, ms.  */
#define HTTP_URL       "https://example.test/page"
#define HTTP_URL_BAD   "ftp://example.test/page"
#define HTTP_LEN_NONE  ((int32_t)-1)
#define HTTP_NO_BODY   0U
#define HTTP_OK        200U
#define HTTP_CREATED   201U
#define HTTP_MISSING   404U
#define HTTP_URL_FILL  249U   /**< "http://" plus this many makes 256 bytes.   */
#define HTTP_URL_BYTES 258U   /**< Room for a 256-byte URL and its NUL.        */
#define HTTP_URL_LONG  256U   /**< Bytes of the URL that is one too long.      */
#define HTTP_URL_EDGE  255U   /**< Bytes of the longest URL accepted.          */
#define HTTP_OVER_MSG  HTTP_URL ": the answer is over 64 KiB"

#define NO_NETWORK "no network: call kode_wifi_connect() first"

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

/** @brief The case's world: locks real, the system up, the driver readied and idle, the net mock fresh. */
static void world_up(void)
{
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sem_reset();
    fake_time_reset();
    s_was_up = wifi_stack_up();
    esp_netif_create_default_wifi_sta_IgnoreAndReturn((esp_netif_t *)&s_netif);
    esp_wifi_stop_IgnoreAndReturn(ESP_OK);
    esp_wifi_start_IgnoreAndReturn(ESP_OK);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);
    esp_wifi_sta_get_ap_info_IgnoreAndReturn(ESP_FAIL);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("net"));
    Mockkdrv_net_Init();
}

/** @brief The board has an address. */
static void reach_ip(void)
{
    ip_event_got_ip_t got;

    (void)memset(&got, 0, sizeof(got));
    got.ip_info.ip.addr = HTTP_IP4;
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &got);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await("net", HTTP_AWAIT_MS));
}

/** @brief Give the world back; every expectation of the net mock was met. */
static void world_down(void)
{
    Mockkdrv_net_Verify();
    Mockkdrv_net_Destroy();
    if ( !s_was_up )
    {
        kdrv_wifi_forget();
    }
    Mockesp_event_Destroy();
    Mockesp_netif_Destroy();
    Mockesp_wifi_Destroy();
}

/** @brief The online world of most cases. */
static void online_up(void)
{
    world_up();
    reach_ip();
}

/** @brief What the driver answers an exchange with. */
static kdrv_net_http_result_t drv_result(uint16_t status, uint32_t body_len, uint32_t kept, bool truncated)
{
    kdrv_net_http_result_t r;

    (void)memset(&r, 0, sizeof(r));
    r.status         = status;
    r.content_length = HTTP_LEN_NONE;
    r.body_len       = body_len;
    r.kept           = kept;
    r.truncated      = truncated;
    return r;
}

/** @brief @p text as the bytes CMock copies into the driver's buffer; it only reads them. */
static uint8_t *bytes_of(const char *text)
{
    return (uint8_t *)(uintptr_t)text;
}

/**
 * @brief Expect one GET and script the driver's answer.
 * @param url  The address the driver must be handed.
 * @param rc   What the driver answers.
 * @param res  The result it writes.
 * @param text The body it writes; NULL for none.
 */
static void expect_get(const char *url, kode_status_t rc, kdrv_net_http_result_t *res, const char *text)
{
    kdrv_net_http_get_ExpectAndReturn(url, NULL, KODE_HTTP_BODY_MAX, NULL, KODE_HTTP_TIMEOUT_MS, rc);
    kdrv_net_http_get_IgnoreArg_buf();
    kdrv_net_http_get_IgnoreArg_out();
    kdrv_net_http_get_ReturnThruPtr_out(res);
    if ( NULL != text )
    {
        kdrv_net_http_get_ReturnMemThruPtr_buf(bytes_of(text), (uint32_t)strlen(text));
    }
}

/** @brief Expect one POST of @p body with @p ctype and script the driver's answer. */
static void expect_post(const char *body, const char *ctype, kode_status_t rc,
                        kdrv_net_http_result_t *res, const char *text)
{
    kdrv_net_http_post_ExpectAndReturn(HTTP_URL, body, (uint32_t)strlen(body), ctype, NULL,
                                       KODE_HTTP_BODY_MAX, NULL, KODE_HTTP_TIMEOUT_MS, rc);
    kdrv_net_http_post_IgnoreArg_buf();
    kdrv_net_http_post_IgnoreArg_out();
    kdrv_net_http_post_ReturnThruPtr_out(res);
    if ( NULL != text )
    {
        kdrv_net_http_post_ReturnMemThruPtr_buf(bytes_of(text), (uint32_t)strlen(text));
    }
}

TEST_CASE("a get returns the body as text and the status 200", "[kode_http]")
{
    const char                  body[] = "<html>hi</html>";
    kdrv_net_http_result_t res    = drv_result(HTTP_OK, (uint32_t)strlen(body), (uint32_t)strlen(body), false);

    online_up();
    expect_get(HTTP_URL, KODE_OK, &res, body);
    TEST_ASSERT_EQUAL_STRING(body, kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(HTTP_OK, kode_http_status());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    world_down();
}

TEST_CASE("a 404 returns its body, the status 404 and no error", "[kode_http]")
{
    const char                  body[] = "not here";
    kdrv_net_http_result_t res    = drv_result(HTTP_MISSING, (uint32_t)strlen(body), (uint32_t)strlen(body), false);

    online_up();
    expect_get(HTTP_URL, KODE_OK, &res, body);
    TEST_ASSERT_EQUAL_STRING(body, kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(HTTP_MISSING, kode_http_status());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    world_down();
}

TEST_CASE("an empty answer is the empty text with its status", "[kode_http]")
{
    kdrv_net_http_result_t res = drv_result(HTTP_CREATED, HTTP_NO_BODY, HTTP_NO_BODY, false);

    online_up();
    expect_get(HTTP_URL, KODE_OK, &res, NULL);
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(HTTP_CREATED, kode_http_status());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    world_down();
}

TEST_CASE("a post of a JSON object is sent as application/json", "[kode_http]")
{
    const char                  body[] = "{\"a\":1}";
    kdrv_net_http_result_t res    = drv_result(HTTP_CREATED, 2U, 2U, false);

    online_up();
    expect_post(body, "application/json", KODE_OK, &res, "ok");
    TEST_ASSERT_EQUAL_STRING("ok", kode_http_post(HTTP_URL, body));
    TEST_ASSERT_EQUAL_UINT16(HTTP_CREATED, kode_http_status());
    world_down();
}

TEST_CASE("a post of plain text is sent as text/plain; charset=utf-8", "[kode_http]")
{
    kdrv_net_http_result_t res = drv_result(HTTP_OK, 2U, 2U, false);

    online_up();
    expect_post("hello", "text/plain; charset=utf-8", KODE_OK, &res, "ok");
    TEST_ASSERT_EQUAL_STRING("ok", kode_http_post(HTTP_URL, "hello"));
    expect_post("", "text/plain; charset=utf-8", KODE_OK, &res, "ok");
    TEST_ASSERT_EQUAL_STRING("ok", kode_http_post(HTTP_URL, ""));
    world_down();
}

TEST_CASE("a post whose body starts with white space then a bracket is JSON", "[kode_http]")
{
    kdrv_net_http_result_t res = drv_result(HTTP_OK, 2U, 2U, false);

    online_up();
    expect_post("  [1]", "application/json", KODE_OK, &res, "ok");
    TEST_ASSERT_EQUAL_STRING("ok", kode_http_post(HTTP_URL, "  [1]"));
    expect_post("\r\n\t{}", "application/json", KODE_OK, &res, "ok");
    TEST_ASSERT_EQUAL_STRING("ok", kode_http_post(HTTP_URL, "\r\n\t{}"));
    expect_post("  x[1]", "text/plain; charset=utf-8", KODE_OK, &res, "ok");
    TEST_ASSERT_EQUAL_STRING("ok", kode_http_post(HTTP_URL, "  x[1]"));
    world_down();
}

TEST_CASE("the second request replaces the first's text in the one block", "[kode_http]")
{
    kdrv_net_http_result_t first  = drv_result(HTTP_OK, 5U, 5U, false);
    kdrv_net_http_result_t second = drv_result(HTTP_MISSING, 2U, 2U, false);
    const char                  *a;
    const char                  *b;

    online_up();
    expect_get(HTTP_URL, KODE_OK, &first, "first");
    a = kode_http_get(HTTP_URL);
    TEST_ASSERT_EQUAL_STRING("first", a);
    expect_get(HTTP_URL, KODE_OK, &second, "no");
    b = kode_http_get(HTTP_URL);
    TEST_ASSERT_EQUAL_STRING("no", b);
    TEST_ASSERT_EQUAL_PTR(a, b);
    TEST_ASSERT_EQUAL_UINT16(HTTP_MISSING, kode_http_status());
    world_down();
}

TEST_CASE("an answer of exactly 64 KiB is returned whole", "[kode_http]")
{
    kdrv_net_http_result_t res = drv_result(HTTP_OK, KODE_HTTP_BODY_MAX, KODE_HTTP_BODY_MAX, false);
    char *const                  big = (char *)khal_alloc_psram(sizeof(void *), KODE_HTTP_BODY_MAX + 1U);
    const char                  *got;

    TEST_ASSERT_NOT_NULL(big);
    (void)memset(big, 'a', KODE_HTTP_BODY_MAX);
    big[KODE_HTTP_BODY_MAX] = '\0';
    online_up();
    expect_get(HTTP_URL, KODE_OK, &res, big);
    got = kode_http_get(HTTP_URL);
    TEST_ASSERT_EQUAL_UINT32(KODE_HTTP_BODY_MAX, (uint32_t)strlen(got));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT16(HTTP_OK, kode_http_status());
    world_down();
    khal_free(big);
}

TEST_CASE("an answer over 64 KiB is empty text with its status and the reason", "[kode_http]")
{
    kdrv_net_http_result_t res = drv_result(HTTP_OK, KODE_HTTP_BODY_MAX + 1U, KODE_HTTP_BODY_MAX, true);

    online_up();
    expect_get(HTTP_URL, KODE_OK, &res, "partial");
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(HTTP_OK, kode_http_status());
    TEST_ASSERT_EQUAL_STRING(HTTP_OVER_MSG, kode_error());
    world_down();
}

TEST_CASE("a timeout is empty text, status 0 and says no answer in time", "[kode_http]")
{
    kdrv_net_http_result_t ok  = drv_result(HTTP_OK, 2U, 2U, false);
    kdrv_net_http_result_t cut = drv_result(0U, 0U, 0U, false);

    online_up();
    expect_get(HTTP_URL, KODE_OK, &ok, "ok");
    TEST_ASSERT_EQUAL_STRING("ok", kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(HTTP_OK, kode_http_status());
    expect_get(HTTP_URL, KODE_TIMEOUT, &cut, NULL);
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(0U, kode_http_status());
    TEST_ASSERT_EQUAL_STRING(HTTP_URL ": no answer in time", kode_error());
    world_down();
}

TEST_CASE("a refused connection says it could not connect", "[kode_http]")
{
    kdrv_net_http_result_t cut = drv_result(0U, 0U, 0U, false);

    online_up();
    expect_get(HTTP_URL, KODE_ERROR, &cut, NULL);
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(0U, kode_http_status());
    TEST_ASSERT_EQUAL_STRING(HTTP_URL ": could not connect", kode_error());
    expect_post("x", "text/plain; charset=utf-8", KODE_ERROR, &cut, NULL);
    TEST_ASSERT_EQUAL_STRING("", kode_http_post(HTTP_URL, "x"));
    TEST_ASSERT_EQUAL_STRING(HTTP_URL ": could not connect", kode_error());
    world_down();
}

TEST_CASE("a driver without a network says the firmware has none", "[kode_http]")
{
    kdrv_net_http_result_t cut = drv_result(0U, 0U, 0U, false);

    online_up();
    expect_get(HTTP_URL, KODE_DENIED, &cut, NULL);
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_UINT16(0U, kode_http_status());
    TEST_ASSERT_EQUAL_STRING("this firmware has no network", kode_error());
    world_down();
}

TEST_CASE("a board with no address calls nothing and says to connect", "[kode_http]")
{
    world_up();
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(HTTP_URL));
    TEST_ASSERT_EQUAL_STRING(NO_NETWORK, kode_error());
    TEST_ASSERT_EQUAL_UINT16(0U, kode_http_status());
    TEST_ASSERT_EQUAL_STRING("", kode_http_post(HTTP_URL, "x"));
    TEST_ASSERT_EQUAL_STRING(NO_NETWORK, kode_error());
    world_down();
}

TEST_CASE("an address that is not http or https is refused before anything is asked", "[kode_http]")
{
    char far[HTTP_URL_BYTES];

    online_up();
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(HTTP_URL_BAD));
    TEST_ASSERT_EQUAL_STRING("bad URL '" HTTP_URL_BAD "'", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(""));
    TEST_ASSERT_EQUAL_STRING("bad URL ''", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(NULL));
    TEST_ASSERT_EQUAL_STRING("bad URL '(null)'", kode_error());
    TEST_ASSERT_EQUAL_STRING("", kode_http_get("http:/example.test"));
    TEST_ASSERT_EQUAL_STRING("", kode_http_get("http"));
    TEST_ASSERT_EQUAL_STRING("", kode_http_post(HTTP_URL_BAD, "x"));
    TEST_ASSERT_EQUAL_STRING("", kode_http_post(NULL, "x"));

    (void)memset(far, 'a', sizeof(far));
    (void)memcpy(far, "http://", 7U);
    far[HTTP_URL_LONG] = '\0';
    TEST_ASSERT_EQUAL_STRING("", kode_http_get(far));
    TEST_ASSERT_EQUAL_STRING("", kode_http_post(far, "x"));
    TEST_ASSERT_EQUAL_UINT16(0U, kode_http_status());
    world_down();
}

TEST_CASE("the scheme takes any case and a 255-byte address is accepted", "[kode_http]")
{
    kdrv_net_http_result_t res = drv_result(HTTP_OK, 1U, 1U, false);
    char                         edge[HTTP_URL_BYTES];

    online_up();
    expect_get("HTTP://example.test", KODE_OK, &res, "a");
    TEST_ASSERT_EQUAL_STRING("a", kode_http_get("HTTP://example.test"));
    expect_get("HtTpS://example.test", KODE_OK, &res, "b");
    TEST_ASSERT_EQUAL_STRING("b", kode_http_get("HtTpS://example.test"));

    (void)memset(edge, 'a', sizeof(edge));
    (void)memcpy(edge, "http://", 7U);
    edge[HTTP_URL_EDGE] = '\0';
    expect_get(edge, KODE_OK, &res, "c");
    TEST_ASSERT_EQUAL_STRING("c", kode_http_get(edge));
    world_down();
}

TEST_CASE("a post with no body is refused", "[kode_http]")
{
    online_up();
    TEST_ASSERT_EQUAL_STRING("", kode_http_post(HTTP_URL, NULL));
    TEST_ASSERT_EQUAL_STRING("no body to post", kode_error());
    TEST_ASSERT_EQUAL_UINT16(0U, kode_http_status());
    world_down();
}

/* ---- Ping, lookup and UDP ------------------------------------------------- */

#define NET_HOST      "example.test"  /**< The host the cases ask about.        */
#define NET_IP4       0x22D8B85DU     /**< The address the driver resolves to. */
#define NET_PORT      5000U           /**< A destination port.                 */
#define NET_COUNT     3U              /**< Echoes a ping asks for.             */
#define NET_HOST_LONG 64U             /**< One byte past the longest host.     */

TEST_CASE("a ping returns its counts, and a silent host is answered with none received", "[kode_net_standard]")
{
    kdrv_net_ping_stats_t st = { NET_IP4, 3U, 2U, 11U, 12U, 14U };
    kdrv_net_ping_stats_t none = { NET_IP4, 1U, 0U, 0U, 0U, 0U };
    kode_net_ping_t           p;

    online_up();
    kdrv_net_ping_ExpectAndReturn(NET_HOST, NET_COUNT, NULL, 0U, NULL, NULL, KODE_OK);
    kdrv_net_ping_IgnoreArg_stats();
    kdrv_net_ping_ReturnThruPtr_stats(&st);
    p = kode_net_ping(NET_HOST, NET_COUNT);
    TEST_ASSERT_TRUE(p.ok);
    TEST_ASSERT_EQUAL_UINT32(3U, p.sent);
    TEST_ASSERT_EQUAL_UINT32(2U, p.received);
    TEST_ASSERT_EQUAL_UINT32(11U, p.min_ms);
    TEST_ASSERT_EQUAL_UINT32(12U, p.avg_ms);
    TEST_ASSERT_EQUAL_UINT32(14U, p.max_ms);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    kdrv_net_ping_ExpectAndReturn(NET_HOST, 1U, NULL, 0U, NULL, NULL, KODE_OK);
    kdrv_net_ping_IgnoreArg_stats();
    kdrv_net_ping_ReturnThruPtr_stats(&none);
    p = kode_net_ping(NET_HOST, 1U);
    TEST_ASSERT_TRUE_MESSAGE(p.ok, "a host that never answers is an answer");
    TEST_ASSERT_EQUAL_UINT32(0U, p.received);

    kdrv_net_ping_ExpectAndReturn(NET_HOST, 1U, NULL, 0U, NULL, NULL, KODE_ERROR);
    kdrv_net_ping_IgnoreArg_stats();
    p = kode_net_ping(NET_HOST, 1U);
    TEST_ASSERT_FALSE(p.ok);
    TEST_ASSERT_EQUAL_STRING(NET_HOST ": could not reach it", kode_error());
    world_down();
}

TEST_CASE("a lookup returns the address, and a name that does not resolve says so", "[kode_net_standard]")
{
    uint32_t ip4  = 0U;
    uint32_t want = NET_IP4;

    online_up();
    kdrv_net_lookup_ExpectAndReturn(NET_HOST, NULL, KODE_OK);
    kdrv_net_lookup_IgnoreArg_ip4();
    kdrv_net_lookup_ReturnThruPtr_ip4(&want);
    TEST_ASSERT_TRUE(kode_net_resolve(NET_HOST, &ip4));
    TEST_ASSERT_EQUAL_HEX32(NET_IP4, ip4);
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    ip4 = 0U;
    kdrv_net_lookup_ExpectAndReturn(NET_HOST, NULL, KODE_ERROR);
    kdrv_net_lookup_IgnoreArg_ip4();
    TEST_ASSERT_FALSE(kode_net_resolve(NET_HOST, &ip4));
    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0U, ip4, "untouched on failure");
    TEST_ASSERT_EQUAL_STRING(NET_HOST " does not resolve", kode_error());
    world_down();
}

TEST_CASE("a UDP send hands the datagram to the driver", "[kode_net_standard]")
{
    static const char data[] = "hello";

    online_up();
    kdrv_net_udp_send_ExpectAndReturn(NET_HOST, NET_PORT, data, (uint16_t)(sizeof(data) - 1U), KODE_OK);
    TEST_ASSERT_TRUE(kode_udp_send(NET_HOST, NET_PORT, data, (uint32_t)(sizeof(data) - 1U)));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    kdrv_net_udp_send_ExpectAndReturn(NET_HOST, NET_PORT, NULL, 0U, KODE_OK);
    TEST_ASSERT_TRUE_MESSAGE(kode_udp_send(NET_HOST, NET_PORT, NULL, 0U), "an empty datagram goes too");

    kdrv_net_udp_send_ExpectAndReturn(NET_HOST, NET_PORT, data, 1U, KODE_ERROR);
    TEST_ASSERT_FALSE(kode_udp_send(NET_HOST, NET_PORT, data, 1U));
    TEST_ASSERT_EQUAL_STRING(NET_HOST ": could not reach it", kode_error());
    world_down();
}

TEST_CASE("ping, lookup and UDP refuse a bad argument or a board with no address before asking", "[kode_net_standard]")
{
    char     far[NET_HOST_LONG + 1U];
    uint32_t ip4 = 0U;

    (void)memset(far, 'a', sizeof(far));
    far[NET_HOST_LONG] = '\0';

    online_up();
    TEST_ASSERT_FALSE(kode_net_ping(NULL, 1U).ok);
    TEST_ASSERT_EQUAL_STRING("bad host '(null)'", kode_error());
    TEST_ASSERT_FALSE(kode_net_ping("", 1U).ok);
    TEST_ASSERT_FALSE(kode_net_ping(far, 1U).ok);
    TEST_ASSERT_FALSE(kode_net_ping(NET_HOST, 0U).ok);
    TEST_ASSERT_FALSE(kode_net_ping(NET_HOST, KODE_PING_COUNT_MAX + 1U).ok);
    TEST_ASSERT_FALSE(kode_net_resolve(NET_HOST, NULL));
    TEST_ASSERT_FALSE(kode_net_resolve(far, &ip4));
    TEST_ASSERT_FALSE(kode_udp_send(NET_HOST, 0U, "x", 1U));
    TEST_ASSERT_FALSE(kode_udp_send(NET_HOST, NET_PORT, NULL, 1U));
    TEST_ASSERT_FALSE(kode_udp_send(NET_HOST, NET_PORT, "x", KODE_UDP_DATA_MAX + 1U));
    world_down();

    world_up();
    TEST_ASSERT_FALSE(kode_net_ping(NET_HOST, 1U).ok);
    TEST_ASSERT_EQUAL_STRING(NO_NETWORK, kode_error());
    TEST_ASSERT_FALSE(kode_net_resolve(NET_HOST, &ip4));
    TEST_ASSERT_EQUAL_STRING(NO_NETWORK, kode_error());
    TEST_ASSERT_FALSE(kode_udp_send(NET_HOST, NET_PORT, "x", 1U));
    TEST_ASSERT_EQUAL_STRING(NO_NETWORK, kode_error());
    world_down();
}

/* ---- The task's state goes with the task ----------------------------------- */

/** @brief What the task of a task-end case leaves for the case. */
typedef struct {
    SemaphoreHandle_t done;       /**< Given when the task has noted what it holds.  */
    void             *slot;       /**< The task's khal slot (its kode_task_t).       */
    bool              refuse;     /**< PSRAM refuses the HTTP state after the task's own struct. */
    bool              fresh_zero; /**< The status read before any request was 0.     */
    uint16_t          status;     /**< The status after the request.                 */
    char              text[8];    /**< The answer's text, copied.                    */
    uint32_t          allocs;     /**< PSRAM blocks the task took.                   */
} ender_t;

/** @brief A task that asks once and ends. */
static void http_task_ends(void *arg)
{
    ender_t *const e      = (ender_t *)arg;
    const uint32_t before = (uint32_t)fake_psram_alloc_count();
    const char    *text   = NULL;

    e->fresh_zero = (kode_http_status() == 0U);
    if ( e->refuse )
    {
        fake_psram_refuse_after(1U, 1U);
    }
    text = kode_http_get(HTTP_URL);
    fake_psram_refuse(0U);
    (void)snprintf(e->text, sizeof(e->text), "%s", text);
    e->status = kode_http_status();
    e->allocs = (uint32_t)fake_psram_alloc_count() - before;
    e->slot   = khal_task_slot_get();
    fake_task_slot_watch(e->slot);
    (void)xSemaphoreGive(e->done);
}

/** @brief Run the asking task to its end and wait for the slot's clean-up. */
static void run_http_task(ender_t *e)
{
    e->done = xSemaphoreCreateBinary();
    fake_task_slot_watch(NULL);

    const uint32_t allocs_before = (uint32_t)fake_psram_alloc_count();
    const uint32_t frees_before  = (uint32_t)fake_psram_free_count();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_task_start(&http_task_ends, "t_http", HTTP_STACK, HTTP_PRIO, e));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(e->done, pdMS_TO_TICKS(HTTP_WAIT_MS)));
    TEST_ASSERT_NOT_NULL(e->slot);
    TEST_ASSERT_TRUE(fake_task_slot_wait_ended(HTTP_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_task_slot_watch_frees());

    /* Everything the task and its start took was given back. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)fake_psram_alloc_count() - allocs_before,
                             (uint32_t)fake_psram_free_count() - frees_before);
    fake_task_slot_watch(NULL);
    vSemaphoreDelete(e->done);
}

TEST_CASE("a task's HTTP state and answer block are freed when the task ends", "[kode_http]")
{
    kdrv_net_http_result_t res = drv_result(HTTP_OK, 3U, 3U, false);
    ender_t                      e   = { NULL, NULL, false, false, 0U, "", 0U };

    online_up();
    expect_get(HTTP_URL, KODE_OK, &res, "abc");
    run_http_task(&e);
    TEST_ASSERT_TRUE(e.fresh_zero);
    TEST_ASSERT_EQUAL_STRING("abc", e.text);
    TEST_ASSERT_EQUAL_UINT16(HTTP_OK, e.status);
    /* The task's struct, the HTTP state and the answer block. */
    TEST_ASSERT_EQUAL_UINT32(3U, e.allocs);
    world_down();
}

TEST_CASE("a task whose HTTP state PSRAM refuses gets empty text and leaks nothing", "[kode_http]")
{
    ender_t e = { NULL, NULL, true, false, 0U, "", 0U };

    online_up();
    run_http_task(&e);
    TEST_ASSERT_TRUE(e.fresh_zero);
    TEST_ASSERT_EQUAL_STRING("", e.text);
    TEST_ASSERT_EQUAL_UINT16(0U, e.status);
    TEST_ASSERT_EQUAL_UINT32(1U, e.allocs);
    world_down();
}
