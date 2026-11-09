/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_net.c
 * @brief The facade's network doors: the blocking HTTP calls and Wi-Fi, over a mocked kdrv_net.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "esp_system.h"

#include "Mockesp_event.h"
#include "Mockesp_netif.h"
#include "Mockesp_wifi.h"
#include "Mockesp_wifi_default.h"
#include "Mockkdrv_net.h"

#include "fake_wifi_events.h"
#include "kaos.h"
#include "kdrv_net.h"
#include "kdrv_wifi.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp_config.h"
#include "qf_support.h"

#define POST_URL      "http://example.test/api"
#define POST_BODY     "{\"a\":1}"
#define POST_CTYPE    "application/json"
#define POST_CAP      64U
#define POST_STATUS   201U
#define POST_BODY_LEN 90U
#define POST_KEPT     64U
#define GET_STATUS    404U
#define GET_BODY_LEN  12U
#define LEN_UNKNOWN   ((int32_t)-1)
#define POST_BODY_SIZE ((uint32_t)(sizeof(POST_BODY) - 1U))
#define POISON        0xA5

/** @brief Open one case: a fresh mock, so nothing is left expected. */
static void net_fresh(void)
{
    Mockkdrv_net_Init();
}

/** @brief Close one case: every expectation was met. */
static void net_done(void)
{
    Mockkdrv_net_Verify();
    Mockkdrv_net_Destroy();
}

TEST_CASE("post_buffer hands every argument to the driver and copies its answer",
          "[kode_net]")
{
    static uint8_t                buf[POST_CAP];
    kdrv_net_http_result_t        drv;
    kode_sys_http_result_t        out;

    net_fresh();
    (void)memset(&drv, 0, sizeof(drv));
    drv.status         = (uint16_t)POST_STATUS;
    drv.content_length = LEN_UNKNOWN;
    drv.body_len       = POST_BODY_LEN;
    drv.kept           = POST_KEPT;
    drv.truncated      = true;

    kdrv_net_http_post_ExpectAndReturn(POST_URL, POST_BODY,
                                       POST_BODY_SIZE,
                                       POST_CTYPE, buf, POST_CAP, NULL,
                                       KODE_HTTP_TIMEOUT_MS, KODE_OK);
    kdrv_net_http_post_IgnoreArg_out();
    kdrv_net_http_post_ReturnThruPtr_out(&drv);

    (void)memset(&out, POISON, sizeof(out));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_http_post_buffer(POST_URL, POST_BODY,
                                                      POST_BODY_SIZE,
                                                      POST_CTYPE, buf, POST_CAP,
                                                      &out));
    TEST_ASSERT_EQUAL_UINT16(POST_STATUS, out.status);
    TEST_ASSERT_EQUAL_INT32(LEN_UNKNOWN, out.content_length);
    TEST_ASSERT_EQUAL_UINT32(POST_BODY_LEN, out.body_len);
    TEST_ASSERT_EQUAL_UINT32(POST_KEPT, out.kept);
    TEST_ASSERT_TRUE(out.truncated);
    net_done();
}

TEST_CASE("post_buffer returns a transport timeout with the result cleared",
          "[kode_net]")
{
    static uint8_t         buf[POST_CAP];
    kode_sys_http_result_t out;

    net_fresh();
    kdrv_net_http_post_ExpectAndReturn(POST_URL, POST_BODY,
                                       POST_BODY_SIZE,
                                       POST_CTYPE, buf, POST_CAP, NULL,
                                       KODE_HTTP_TIMEOUT_MS, KODE_TIMEOUT);
    kdrv_net_http_post_IgnoreArg_out();

    (void)memset(&out, POISON, sizeof(out));
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
                          kode_block_http_post_buffer(POST_URL, POST_BODY,
                                                      POST_BODY_SIZE,
                                                      POST_CTYPE, buf, POST_CAP,
                                                      &out));
    TEST_ASSERT_EQUAL_UINT16(0U, out.status);
    TEST_ASSERT_EQUAL_INT32(LEN_UNKNOWN, out.content_length);
    TEST_ASSERT_EQUAL_UINT32(0U, out.body_len);
    TEST_ASSERT_EQUAL_UINT32(0U, out.kept);
    TEST_ASSERT_FALSE(out.truncated);
    net_done();
}

TEST_CASE("post_buffer with no URL is an error and never reaches the driver",
          "[kode_net]")
{
    static uint8_t         buf[POST_CAP];
    kode_sys_http_result_t out;

    net_fresh();
    (void)memset(&out, POISON, sizeof(out));
    /* No kdrv_net_http_post expectation: reaching the driver fails this case. */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
                          kode_block_http_post_buffer(NULL, POST_BODY,
                                                      POST_BODY_SIZE,
                                                      POST_CTYPE, buf, POST_CAP,
                                                      &out));
    TEST_ASSERT_EQUAL_UINT16(0U, out.status);
    TEST_ASSERT_EQUAL_INT32(LEN_UNKNOWN, out.content_length);
    TEST_ASSERT_EQUAL_UINT32(0U, out.body_len);
    TEST_ASSERT_EQUAL_UINT32(0U, out.kept);
    TEST_ASSERT_FALSE(out.truncated);
    net_done();
}

TEST_CASE("get_buffer still hands its arguments through and copies the answer",
          "[kode_net]")
{
    static uint8_t         buf[POST_CAP];
    kdrv_net_http_result_t drv;
    kode_sys_http_result_t out;

    net_fresh();
    (void)memset(&drv, 0, sizeof(drv));
    drv.status         = (uint16_t)GET_STATUS;
    drv.content_length = (int32_t)GET_BODY_LEN;
    drv.body_len       = GET_BODY_LEN;
    drv.kept           = GET_BODY_LEN;

    kdrv_net_http_get_ExpectAndReturn(POST_URL, buf, POST_CAP, NULL,
                                      KODE_HTTP_TIMEOUT_MS, KODE_OK);
    kdrv_net_http_get_IgnoreArg_out();
    kdrv_net_http_get_ReturnThruPtr_out(&drv);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          kode_block_http_get_buffer(POST_URL, buf, POST_CAP,
                                                     &out));
    TEST_ASSERT_EQUAL_UINT16(GET_STATUS, out.status);
    TEST_ASSERT_EQUAL_INT32((int32_t)GET_BODY_LEN, out.content_length);
    TEST_ASSERT_EQUAL_UINT32(GET_BODY_LEN, out.body_len);
    TEST_ASSERT_EQUAL_UINT32(GET_BODY_LEN, out.kept);
    TEST_ASSERT_FALSE(out.truncated);
    net_done();
}

/* ---- Wi-Fi ----------------------------------------------------------------- */

#define WIFI_NAME_32   "0123456789abcdef0123456789abcdef"
#define WIFI_NAME_33   WIFI_NAME_32 "x"
#define WIFI_PASS_64   WIFI_NAME_32 WIFI_NAME_32
#define WIFI_PASS_65   WIFI_PASS_64 "x"
#define WIFI_IP4       0x0100A8C0U
#define WIFI_POOL      1U
#define WIFI_FILL_MAX  64U
#define WIFI_AWAIT_MS  50U

/** @brief Empty AO_Radio's queue, recycling what it holds. */
static void radio_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief An empty AO_Radio queue the margin can read. */
static void radio_open(void)
{
    qf_support_init();
    qf_support_give_queue(AO_Radio);
    radio_flush();
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

TEST_CASE("wifi_connect with no name asks for a remembered network", "[kode_net][wifi]")
{
    radio_open();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_connect(NULL, NULL));
    QF_gc(radio_only(WIFI_AUTO_REQ_SIG));
}

TEST_CASE("wifi_connect posts the join with the name, the password and remember set",
          "[kode_net][wifi]")
{
    const WifiJoinEvt *e;

    radio_open();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_connect("net", "pw"));
    e = (const WifiJoinEvt *)radio_only(WIFI_JOIN_REQ_SIG);
    TEST_ASSERT_EQUAL_STRING("net", e->ssid);
    TEST_ASSERT_EQUAL_STRING("pw", e->pass);
    TEST_ASSERT_TRUE(e->remember);
    QF_gc(&e->super);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_connect("open", NULL));
    e = (const WifiJoinEvt *)radio_only(WIFI_JOIN_REQ_SIG);
    TEST_ASSERT_EQUAL_STRING("open", e->ssid);
    TEST_ASSERT_EQUAL_STRING("", e->pass);
    QF_gc(&e->super);
}

TEST_CASE("wifi_connect carries the longest name and password whole", "[kode_net][wifi]")
{
    const WifiJoinEvt *e;

    radio_open();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_connect(WIFI_NAME_32, WIFI_PASS_64));
    e = (const WifiJoinEvt *)radio_only(WIFI_JOIN_REQ_SIG);
    TEST_ASSERT_EQUAL_STRING(WIFI_NAME_32, e->ssid);
    TEST_ASSERT_EQUAL_STRING(WIFI_PASS_64, e->pass);
    QF_gc(&e->super);
}

TEST_CASE("wifi_connect refuses an empty or too long name or password without posting",
          "[kode_net][wifi]")
{
    radio_open();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_wifi_connect("", "pw"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_wifi_connect(WIFI_NAME_33, "pw"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_wifi_connect("net", WIFI_PASS_65));
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Radio->eQueue));
}

TEST_CASE("wifi_disconnect posts the leave request", "[kode_net][wifi]")
{
    radio_open();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_disconnect());
    QF_gc(radio_only(WIFI_LEAVE_REQ_SIG));
}

TEST_CASE("the Wi-Fi calls answer no resources once the queue is within its margin",
          "[kode_net][wifi]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    radio_open();
    for ( uint32_t guard = 0U;
         (WIFI_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    uint16_t const pool = QF_getPoolFree(WIFI_POOL);

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_wifi_connect("net", "pw"));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_wifi_connect(NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_wifi_disconnect());
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_CMD_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    TEST_ASSERT_EQUAL_UINT16(pool, QF_getPoolFree(WIFI_POOL));
    radio_flush();
}

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

TEST_CASE("wifi_status refuses a missing destination", "[kode_net][wifi]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_wifi_status(NULL));
}

TEST_CASE("wifi_status follows the driver into an address and back out",
          "[kode_net][wifi]")
{
    kode_sys_wifi_status_t st;
    ip_event_got_ip_t      got;
    bool const             was_up = wifi_stack_up();

    esp_netif_create_default_wifi_sta_IgnoreAndReturn((esp_netif_t *)&st);
    esp_wifi_stop_IgnoreAndReturn(ESP_OK);
    esp_wifi_start_IgnoreAndReturn(ESP_OK);
    esp_wifi_disconnect_IgnoreAndReturn(ESP_OK);
    esp_wifi_sta_get_ap_info_IgnoreAndReturn(ESP_FAIL);

    /* Readied and not joined: nothing to report. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_prepare("net"));
    (void)memset(&st, POISON, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_status(&st));
    TEST_ASSERT_EQUAL_UINT8(KODE_SYS_WIFI_STATE_IDLE, st.state);
    TEST_ASSERT_EQUAL_UINT32(0U, st.ip4);
    TEST_ASSERT_EQUAL_STRING("", st.ssid);

    /* Joined: the state, the address and the name. */
    (void)memset(&got, 0, sizeof(got));
    got.ip_info.ip.addr = WIFI_IP4;
    fake_wifi_events_deliver(IP_EVENT, IP_EVENT_STA_GOT_IP, &got);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_wifi_await("net", WIFI_AWAIT_MS));
    (void)memset(&st, POISON, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_status(&st));
    TEST_ASSERT_EQUAL_UINT8(KODE_SYS_WIFI_STATE_GOT_IP, st.state);
    TEST_ASSERT_EQUAL_UINT32(WIFI_IP4, st.ip4);
    TEST_ASSERT_EQUAL_STRING("net", st.ssid);

    /* The network drops: no address, and no name to claim. */
    fake_wifi_events_deliver(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, NULL);
    (void)memset(&st, POISON, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_wifi_status(&st));
    TEST_ASSERT_EQUAL_UINT8(KODE_SYS_WIFI_STATE_DISCONNECTED, st.state);
    TEST_ASSERT_EQUAL_UINT32(0U, st.ip4);
    TEST_ASSERT_EQUAL_STRING("", st.ssid);

    wifi_stack_done(was_up);
}

/* ---- MQTT: the doors and the status --------------------------------------------- */

#define MQTT_URI        "mqtt://broker.example:1883"
#define MQTT_TOPIC      "some/topic"
#define MQTT_FILL_MAX   64U
#define MQTT_POOLS      2U

/** @brief Empty AO_Net's queue, recycling what it holds. */
static void net_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief An empty AO_Net queue the margin can read. */
static void net_open(void)
{
    qf_support_init();
    qf_support_give_queue(AO_Net);
    net_flush();
}

/** @brief Fill AO_Net's queue until only its margin is free. */
static void net_fill_to_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(MQTT_STATE_REQ_SIG);

    for ( uint32_t guard = 0U;
         (MQTT_FILL_MAX > guard)
         && (uxQueueSpacesAvailable(AO_Net->eQueue) > KQP_NET_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Net, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
}

/** @brief Blocks free in every pool. */
static uint32_t net_pools_free(void)
{
    uint32_t total = 0U;

    for ( uint8_t id = 1U; MQTT_POOLS >= id; ++id )
    {
        total += (uint32_t)QF_getPoolFree(id);
    }
    return total;
}

TEST_CASE("the MQTT calls post to AO_Net while it has room", "[kode_net][mqtt]")
{
    static const QSignal WANT[5] = {
        MQTT_UP_REQ_SIG, MQTT_DOWN_REQ_SIG, MQTT_PUB_REQ_SIG,
        MQTT_SUB_REQ_SIG, MQTT_UNSUB_REQ_SIG,
    };
    const QEvt *e = NULL;

    net_open();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_connect(MQTT_URI, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_disconnect());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_publish(MQTT_TOPIC, "x", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_subscribe(MQTT_TOPIC));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_unsubscribe(MQTT_TOPIC));
    for ( uint32_t i = 0U; 5U > i; ++i )
    {
        TEST_ASSERT_TRUE(xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)WANT[i], (uint32_t)e->sig);
        QF_gc(e);
    }
}

TEST_CASE("the MQTT calls refuse a missing or a too long argument and post nothing", "[kode_net][mqtt]")
{
    char far[KODE_SYS_MQTT_URI_MAX + 1U];

    (void)memset(far, 'a', sizeof(far));
    far[sizeof(far) - 1U] = '\0';

    net_open();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_connect(NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_connect(far, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_connect(MQTT_URI, far, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_connect(MQTT_URI, NULL, far));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_publish(NULL, "x", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_publish(far, "x", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_publish(MQTT_TOPIC, NULL, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_subscribe(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_subscribe(far));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_unsubscribe(""));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Net->eQueue));

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_mqtt_connect(MQTT_URI, "", ""),
                                  "empty credentials are an anonymous broker");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_publish(MQTT_TOPIC, NULL, 0U));
    net_flush();
}

TEST_CASE("the MQTT calls answer no resources once the queue is within its margin",
          "[kode_net][mqtt]")
{
    net_open();
    net_fill_to_margin();
    uint32_t const pools = net_pools_free();

    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_connect(MQTT_URI, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_disconnect());
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_publish(MQTT_TOPIC, "x", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_subscribe(MQTT_TOPIC));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_mqtt_unsubscribe(MQTT_TOPIC));
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
    TEST_ASSERT_EQUAL_UINT32(pools, net_pools_free());
    net_flush();
}

/** @brief The driver's copy of the broker, as a CMock callback. */
static void stub_uri_copy(char *out, size_t cap, int n)
{
    (void)n;
    (void)snprintf(out, cap, "%s", MQTT_URI);
}

TEST_CASE("mqtt_status refuses a missing destination", "[kode_net][mqtt]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_mqtt_status(NULL));
}

TEST_CASE("mqtt_status reads the driver's state and broker", "[kode_net][mqtt]")
{
    kode_sys_mqtt_status_t st;

    net_fresh();
    kdrv_net_mqtt_state_ExpectAndReturn(KDRV_MQTT_UP);
    kdrv_net_mqtt_uri_copy_Stub(&stub_uri_copy);

    (void)memset(&st, POISON, sizeof(st));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_status(&st));
    TEST_ASSERT_EQUAL_UINT8(KODE_SYS_MQTT_STATE_UP, st.state);
    TEST_ASSERT_EQUAL_STRING(MQTT_URI, st.uri);
    net_done();
}
