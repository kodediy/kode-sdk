/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_net.c
 * @brief kdrv_net: the job bodies, the guards, and the tables an operator reads.
 */
#include <string.h>

#include "unity.h"

#include "Mockesp_http_client.h"
#include "Mockping_sock.h"

#include "kdrv_net.h"
#include "host_spies.h"
#include "time_stub.h"

/* ---- Names and addresses --------------------------------------------------- */

/* A lookup with nothing to look up is an error rather than a socket call.
 * Every one of these is a caller bug, and answering "no such host" for an
 * empty string is what turns it into a confusing log line instead. */
TEST_CASE("a lookup with no host, or nowhere to put the answer, is refused",
          "[kdrv_net]")
{
    uint32_t ip = 0xDEADBEEFU;

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_lookup(NULL, &ip));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_lookup("", &ip));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_lookup("localhost", NULL));

    TEST_ASSERT_EQUAL_HEX32_MESSAGE(0xDEADBEEFU, ip,
        "a refused lookup must not have written an address");
}

/* A dotted quad, which resolves without asking anybody. Deliberately not a
 * name: a case that needs DNS is a case that fails on a train. */
TEST_CASE("a literal address resolves to itself", "[kdrv_net]")
{
    uint32_t ip = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_net_lookup("127.0.0.1", &ip));

    /* Network byte order, which is what the ping target and the UDP socket
     * both want - a host-order answer here would ping 1.0.0.127. */
    TEST_ASSERT_EQUAL_HEX32(0x0100007FU, ip);
}

/* ---- Ping: the guards ------------------------------------------------------ */

TEST_CASE("a ping of nothing, or with nowhere to report, is refused",
          "[kdrv_net]")
{
    kdrv_net_ping_stats_t stats;
    kdrv_net_ping_reply_t rows[4];
    uint8_t               n = 9U;

    Mockping_sock_Init();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_ping("127.0.0.1", 0U, rows, 4U, &n, &stats));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_ping("127.0.0.1", 1U, rows, 4U, &n, NULL));

    Mockping_sock_Verify();
    Mockping_sock_Destroy();
}

/* A host that does not resolve never reaches the ICMP session, and the row
 * count is cleared on the way out - a stale count from an earlier ping would
 * make `net_ping` print somebody else's replies. */
TEST_CASE("a ping to a name that does not resolve reports no rows",
          "[kdrv_net]")
{
    kdrv_net_ping_stats_t stats;
    kdrv_net_ping_reply_t rows[4];
    uint8_t               n = 9U;

    Mockping_sock_Init();
    /* No esp_ping_new_session is expected: the lookup fails first, and the
     * mock fails this case if the session is opened anyway. */

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_ping("", 2U, rows, 4U, &n, &stats));

    TEST_ASSERT_EQUAL_UINT(0U, n);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.tx);
    TEST_ASSERT_EQUAL_UINT32(0U, stats.rx);

    Mockping_sock_Destroy();
}

/* A session that cannot be opened is an error, and the handle is never
 * started. */
TEST_CASE("a ping session that cannot be opened is reported", "[kdrv_net]")
{
    kdrv_net_ping_stats_t stats;
    uint8_t               n = 0U;

    Mockping_sock_Init();
    esp_ping_new_session_ExpectAnyArgsAndReturn(ESP_ERR_NO_MEM);
    /* And esp_ping_delete_session is NOT expected: there is no session to
     * delete, and calling it with the null handle a failed open leaves behind
     * is how a driver turns one failure into two. */

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_ping("127.0.0.1", 1U, NULL, 0U, &n, &stats));

    Mockping_sock_Verify();
    Mockping_sock_Destroy();
}

/* Opened but not started: still an error, and the session is deleted rather
 * than leaked. */
TEST_CASE("a ping session that cannot be started is deleted, not leaked",
          "[kdrv_net]")
{
    kdrv_net_ping_stats_t stats;
    uint8_t               n = 0U;

    /* The handle has to come back through the out-parameter, not just as a
     * return code: the driver only deletes a session it was actually given
     * one for, so an expectation without ReturnThruPtr would leave `hdl` NULL
     * and quietly test nothing. */
    static int               token;
    static esp_ping_handle_t handle;

    handle = (esp_ping_handle_t)&token;

    Mockping_sock_Init();
    esp_ping_new_session_ExpectAnyArgsAndReturn(ESP_OK);
    esp_ping_new_session_ReturnThruPtr_hdl_out(&handle);
    esp_ping_start_ExpectAnyArgsAndReturn(ESP_FAIL);
    esp_ping_delete_session_ExpectAnyArgsAndReturn(ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_ping("127.0.0.1", 1U, NULL, 0U, &n, &stats));

    Mockping_sock_Verify();
    Mockping_sock_Destroy();
}

/* ---- The result tables ----------------------------------------------------- */

/* Everything below is read straight into a console reply or a GUI row, so an
 * out-of-range index must be a refusal rather than whatever was in the array.
 * The count is whatever the last ping left, which after the cases above is
 * zero - and zero is the interesting boundary anyway. */
TEST_CASE("a ping row that was never filled cannot be read", "[kdrv_net]")
{
    kdrv_net_ping_reply_t row;

    TEST_ASSERT_FALSE(kdrv_net_ping_get(kdrv_net_ping_count(), &row));
    TEST_ASSERT_FALSE(kdrv_net_ping_get(0xFFU, &row));
    TEST_ASSERT_FALSE(kdrv_net_ping_get(0U, NULL));
}

/* Asking for the summary with nowhere to put it is what a caller that only
 * wanted the side effect does. It must not write through NULL. */
TEST_CASE("asking for the tables with nowhere to answer is safe", "[kdrv_net]")
{
    kdrv_net_ping_stats(NULL);
    kdrv_net_http_result(NULL);
}

/* The body accessor is paged: the console reads it in pieces, and both ends of
 * the range have to hold. An offset past what was kept is zero bytes rather
 * than a read off the end of the buffer. */
TEST_CASE("the body is readable only where there is body", "[kdrv_net]")
{
    kdrv_net_http_result_t res;
    uint8_t                buf[16];

    kdrv_net_http_result(&res);

    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_net_http_body(0U, NULL, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0U, kdrv_net_http_body(res.kept, buf, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0U,
        kdrv_net_http_body(res.kept + 1000U, buf, sizeof(buf)));
}

/* ---- MQTT, in a build without it ------------------------------------------- */

/* Four entry points that must refuse rather than quietly succeed. A firmware
 * built without the MQTT half still links every one of them - the console
 * still has the verbs - and a `mqtt_pub` that returned KODE_OK on a board with
 * no MQTT client would be a message the operator believes was sent. */
TEST_CASE("without the MQTT half, every MQTT verb refuses", "[kdrv_net]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_net_mqtt_publish("t", "x", 1U, false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          kdrv_net_mqtt_publish("t", "x", 1U, true));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_mqtt_subscribe("t"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_mqtt_unsubscribe("t"));
    TEST_ASSERT_EQUAL_INT(KDRV_MQTT_DOWN, kdrv_net_mqtt_state());
    {
        char uri[KDRV_MQTT_URI_MAX];

        (void)memset(uri, 'x', sizeof(uri));
        kdrv_net_mqtt_uri_copy(uri, sizeof(uri));
        TEST_ASSERT_EQUAL_STRING("", uri);
    }

    /* And installing a callback is accepted rather than crashing, because the
     * AO installs one at startup whatever this firmware was built with. */
    kdrv_net_mqtt_set_cb(NULL);
}

/* ---- The jobs -------------------------------------------------------------- */

/* A job with nothing to run. The caller is an Active Object's worker, and a
 * NULL here would be its own bug, but the answer is a refusal and not a crash,
 * and not a status that reads as a job that ran. */
TEST_CASE("running nothing is refused", "[kdrv_net]")
{
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_run(NULL));
}

/* The driver used to refuse an unknown type at submit, because its worker's
 * `default` arm answered nobody. A body that is called directly has no such
 * door, so the refusal is its return value - the worker posts that, and the
 * AO is told. */
TEST_CASE("a job type nobody recognises is refused", "[kdrv_net]")
{
    kdrv_net_req_t req;

    (void)memset(&req, 0, sizeof(req));
    req.type = (kdrv_net_job_t)(KDRV_NET_JOB_MQTT_STOP + 1);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_run(&req));

    req.type = (kdrv_net_job_t)0x7F;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_run(&req));
}

/* A build without the MQTT half still answers a START it should never have
 * been sent, and a STOP: "an unanswered job is an AO waiting forever". */
TEST_CASE("the MQTT jobs of a build without MQTT answer", "[kdrv_net]")
{
    kdrv_net_req_t req;

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_NET_JOB_MQTT_START;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_run(&req));

    req.type = KDRV_NET_JOB_MQTT_STOP;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_net_run(&req));
}

/* ---- HTTPS POST ------------------------------------------------------------ */

/* The body is not in the job: it travels through the setter. A POST that
 * reaches the driver with no body set is a caller bug, and it is answered like
 * any job - refused, not sent empty, not left unanswered - with the result
 * table cleared, so the GET answer that was there before is not read as this
 * one's. These cases come last on purpose: the first body ever set stays set.*/
TEST_CASE("a POST nobody gave a body to is answered as refused", "[kdrv_net]")
{
    kdrv_net_req_t         req;
    kdrv_net_http_result_t res;

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_NET_JOB_HTTP_POST;
    (void)strcpy(req.url, "https://example.invalid/kodes/challenge");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_run(&req));
    kdrv_net_http_result(&res);
    TEST_ASSERT_EQUAL_UINT16(0U, res.status);
    TEST_ASSERT_EQUAL_UINT32(0U, res.kept);
}

TEST_CASE("the POST body setter refuses what it cannot carry", "[kdrv_net]")
{
    static char big[KDRV_NET_POST_MAX + 1U];

    (void)memset(big, 'x', sizeof(big));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_http_post_body(NULL, 2U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_http_post_body("{}", 0U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_post_body(big, KDRV_NET_POST_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_post_body(big, KDRV_NET_POST_MAX));
}

/**
 * @brief Stands in for the socket write: checks exactly what would go out.
 *
 * The buffer is the driver's, one kilobyte long and not terminated after the
 * body, so a string comparison would read past it. The length is the contract.
 */
static int post_write_checked(esp_http_client_handle_t client, const char *buffer,
                              int len, int num_calls)
{
    (void)client; (void)num_calls;
    TEST_ASSERT_EQUAL_INT(2, len);
    TEST_ASSERT_EQUAL_MEMORY("{}", buffer, 2U);
    return len;
}

/* A server that takes the connection and hangs up before a status line: the
 * client reports no headers and a status of -1. That is no answer, and a GET
 * must say so instead of reporting a status nobody sent. */
TEST_CASE("a GET hung up on before its headers is an error with no status", "[kdrv_net]")
{
    static int                     token;
    kdrv_net_req_t                 req;
    kdrv_net_http_result_t         res;
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&token;

    Mockesp_http_client_Init();
    esp_http_client_init_ExpectAnyArgsAndReturn(h);
    esp_http_client_open_ExpectAndReturn(h, 0, ESP_OK);
    esp_http_client_fetch_headers_ExpectAndReturn(h, (int64_t)-1);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_NET_JOB_HTTP_GET;
    (void)strcpy(req.url, "http://example.invalid/");

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_run(&req));
    kdrv_net_http_result(&res);
    TEST_ASSERT_EQUAL_UINT16(0U, res.status);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

/* The whole exchange, against the mocked client, in the order the server sees
 * it: the header, the length, the body, then the answer read to its end. A
 * POST that sent its headers without the length, or wrote the body before
 * telling the server how long it was, would fail here and not on a bench. */
TEST_CASE("a POST sends its body as JSON and reads the answer", "[kdrv_net]")
{
    static int                     token;
    static const char              answer[] = "{\"a\":1}";
    kdrv_net_req_t                 req;
    kdrv_net_http_result_t         res;
    uint8_t                        got[16];
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&token;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_net_http_post_body("{}", 2U));

    Mockesp_http_client_Init();
    esp_http_client_init_ExpectAnyArgsAndReturn(h);
    esp_http_client_set_header_ExpectAndReturn(h, "Content-Type",
                                               "application/json", ESP_OK);
    esp_http_client_open_ExpectAndReturn(h, 2, ESP_OK);
    esp_http_client_write_Stub(post_write_checked);
    esp_http_client_fetch_headers_ExpectAndReturn(h, (int64_t)sizeof(answer) - 1);
    esp_http_client_get_status_code_ExpectAndReturn(h, 200);
    esp_http_client_read_ExpectAndReturn(h, NULL, 256, (int)sizeof(answer) - 1);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_read_ReturnArrayThruPtr_buffer((char *)answer,
                                                   (int)sizeof(answer) - 1);
    esp_http_client_read_ExpectAndReturn(h, NULL, 256, 0);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_is_complete_data_received_ExpectAndReturn(h, true);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_NET_JOB_HTTP_POST;
    (void)strcpy(req.url, "https://example.invalid/kodes/challenge");

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_net_run(&req));
    kdrv_net_http_result(&res);
    TEST_ASSERT_EQUAL_UINT16(200U, res.status);
    TEST_ASSERT_EQUAL_UINT32(sizeof(answer) - 1U, res.kept);
    TEST_ASSERT_EQUAL_UINT32(sizeof(answer) - 1U,
                             kdrv_net_http_body(0U, got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(answer, got, sizeof(answer) - 1U);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

/** @brief Milliseconds the door cases hand the client as its deadline. */
#define POST_DOOR_TIMEOUT_MS 1000U

TEST_CASE("the POST door refuses what it cannot send and never opens a client",
          "[kdrv_net]")
{
    uint8_t                buf[8];
    kdrv_net_http_result_t res;

    Mockesp_http_client_Init();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_http_post(NULL, "{}", 2U, NULL, buf,
                                          sizeof(buf), &res, POST_DOOR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_http_post("", "{}", 2U, NULL, buf,
                                          sizeof(buf), &res, POST_DOOR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_http_post("https://example.invalid/x",
                                          "{}", 2U, NULL, buf, sizeof(buf), NULL,
                                          POST_DOOR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_http_post("https://example.invalid/x",
                                          NULL, 2U, NULL, buf, sizeof(buf), &res,
                                          POST_DOOR_TIMEOUT_MS));
    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a POST of length 0 sends its headers and no body", "[kdrv_net]")
{
    static int                     token;
    static const char              answer[] = "ok";
    uint8_t                        buf[8];
    kdrv_net_http_result_t         res;
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&token;

    Mockesp_http_client_Init();
    esp_http_client_init_ExpectAnyArgsAndReturn(h);
    esp_http_client_open_ExpectAndReturn(h, 0, ESP_OK);
    esp_http_client_fetch_headers_ExpectAndReturn(h, (int64_t)sizeof(answer) - 1);
    esp_http_client_get_status_code_ExpectAndReturn(h, 200);
    esp_http_client_read_ExpectAndReturn(h, NULL, 256, (int)sizeof(answer) - 1);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_read_ReturnArrayThruPtr_buffer((char *)answer,
                                                   (int)sizeof(answer) - 1);
    esp_http_client_read_ExpectAndReturn(h, NULL, 256, 0);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_is_complete_data_received_ExpectAndReturn(h, true);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);

    /* A body that is there but 0 bytes long is not written; no write is expected. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_net_http_post("https://example.invalid/x",
                                          "left out", 0U, NULL, buf, sizeof(buf),
                                          &res, POST_DOOR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_UINT16(200U, res.status);
    TEST_ASSERT_EQUAL_UINT32(sizeof(answer) - 1U, res.kept);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

/* ---- HTTPS PUT ------------------------------------------------------------- */

/* The PUT takes its URL, body, content type and bearer through one setter, for
 * the reason the POST takes its body through another: none of them fits the
 * job. Every case below sets them again, because the bearer is wiped once the
 * job is done. */
#define PUT_URL        "https://api.example.invalid/kodes/X/shots/up/tok"
#define PUT_OTHER_URL  "https://api.example.invalid/kodes/Y/shots/up/tok"
#define PUT_TYPE       "image/jpeg"
#define PUT_BEARER     "abc"
#define PUT_BODY_LEN   300U

static kode_status_t           s_rc;          /**< What the last job answered. */
static int                     s_put_token;
static uint8_t                 s_put_body[PUT_BODY_LEN];
static const char              s_put_answer[] = "{\"ok\":true,\"id\":\"x\",\"seq\":1}";
static esp_http_client_handle_t s_put_h2;

/** What the init stub saw. */
static struct {
    unsigned                   calls;
    int                        method;
    bool                       no_redirect;
    char                       url[KDRV_NET_PUT_URL_MAX];
    esp_http_client_handle_t   hand_out;
} s_init_seen;

/** @brief Stands in for esp_http_client_init: records the config. */
static esp_http_client_handle_t put_init_stub(const esp_http_client_config_t *cfg,
                                              int num_calls)
{
    (void)num_calls;
    ++s_init_seen.calls;
    s_init_seen.method      = (int)cfg->method;
    s_init_seen.no_redirect = cfg->disable_auto_redirect;
    (void)strncpy(s_init_seen.url, cfg->url, sizeof(s_init_seen.url) - 1U);
    return s_init_seen.hand_out;
}

/** @brief Stands in for the socket write: the body must arrive whole. */
static int put_write_checked(esp_http_client_handle_t client, const char *buffer,
                             int len, int num_calls)
{
    (void)client; (void)num_calls;
    TEST_ASSERT_EQUAL_INT((int)PUT_BODY_LEN, len);
    TEST_ASSERT_EQUAL_MEMORY(s_put_body, buffer, PUT_BODY_LEN);
    return len;
}

/** @brief The answer the server gives: headers, the body, then the end. */
static void put_expect_answer(esp_http_client_handle_t h, int status)
{
    int const n = (int)sizeof(s_put_answer) - 1;

    esp_http_client_fetch_headers_ExpectAndReturn(h, (int64_t)n);
    esp_http_client_get_status_code_ExpectAndReturn(h, status);
    esp_http_client_read_ExpectAndReturn(h, NULL, 256, n);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_read_ReturnArrayThruPtr_buffer((char *)s_put_answer, n);
    esp_http_client_read_ExpectAndReturn(h, NULL, 256, 0);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_is_complete_data_received_ExpectAndReturn(h, true);
}

/** @brief The two headers a PUT sets, every time. */
static void put_expect_headers_for(esp_http_client_handle_t h,
                                   const char *authorization)
{
    esp_http_client_set_header_ExpectAndReturn(h, "Content-Type", PUT_TYPE, ESP_OK);
    esp_http_client_set_header_ExpectAndReturn(h, "Authorization",
                                               authorization, ESP_OK);
}

static void put_expect_headers(esp_http_client_handle_t h)
{
    put_expect_headers_for(h, "Bearer " PUT_BEARER);
}

/** @brief Prepare with @p bearer, run the job. */
static void put_run_with(const char *url, bool keep, const char *bearer)
{
    kdrv_net_req_t req;

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_put_prepare(url, s_put_body, PUT_BODY_LEN, PUT_TYPE,
                                  bearer));
    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_NET_JOB_HTTP_PUT;
    req.keep = keep;

    s_rc = kdrv_net_run(&req);
}

/** @brief Prepare with the usual bearer, run the job. */
static void put_run(const char *url, bool keep)
{
    put_run_with(url, keep, PUT_BEARER);
}

static void put_close_run(void)
{
    kdrv_net_req_t req;

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_NET_JOB_HTTP_CLOSE;
    s_rc = kdrv_net_run(&req);
}

TEST_CASE("the host of a URL is cut before its path", "[kdrv_net]")
{
    char out[40];
    char tiny[8];

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_url_host("https://api.example.invalid/kodes/X/shots/up/tok",
                          out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("api.example.invalid", out);

    /* No path at all, and a query with no path: the host still ends there. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_url_host("https://api.example.invalid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("api.example.invalid", out);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_url_host("https://h.invalid?k=secret", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("h.invalid", out);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_url_host("api.example.invalid/x", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_url_host("https:///x", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_url_host(NULL, out, sizeof(out)));

    (void)strcpy(tiny, "junk");
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_url_host("https://api.example.invalid/x", tiny, sizeof(tiny)));
    TEST_ASSERT_EQUAL_STRING("", tiny);
}

TEST_CASE("put_prepare refuses what it cannot carry", "[kdrv_net]")
{
    char     longurl[KDRV_NET_PUT_URL_MAX + 2U];
    uint8_t  b = 0x55U;

    TEST_ASSERT_EQUAL_UINT(260U, KDRV_NET_PUT_URL_MAX);

    (void)memset(longurl, 'a', sizeof(longurl));
    (void)memcpy(longurl, "https://", 8U);
    longurl[KDRV_NET_PUT_URL_MAX - 1U] = '\0';        /* 259 characters: fits */

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(NULL, &b, 1U, PUT_TYPE, PUT_BEARER));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare("http://api.example.invalid/x", &b, 1U,
                                  PUT_TYPE, PUT_BEARER));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(PUT_URL, NULL, 1U, PUT_TYPE, PUT_BEARER));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(PUT_URL, &b, 0U, PUT_TYPE, PUT_BEARER));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(PUT_URL, &b, KDRV_NET_PUT_MAX + 1U, PUT_TYPE,
                                  PUT_BEARER));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(PUT_URL, &b, 1U, NULL, PUT_BEARER));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(PUT_URL, &b, 1U, PUT_TYPE, NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(PUT_URL, &b, 1U, PUT_TYPE, ""));

    /* The URL is 259 characters at most: one more is refused, 259 is not. */
    longurl[KDRV_NET_PUT_URL_MAX - 1U] = 'a';
    longurl[KDRV_NET_PUT_URL_MAX]      = '\0';        /* 260 characters */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_put_prepare(longurl, &b, 1U, PUT_TYPE, PUT_BEARER));
    longurl[KDRV_NET_PUT_URL_MAX - 1U] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_put_prepare(longurl, &b, 1U, PUT_TYPE, PUT_BEARER));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_put_prepare(PUT_URL, &b, 1U, PUT_TYPE, PUT_BEARER));
}

TEST_CASE("the bearer is wiped by the job, so a PUT with none is refused",
          "[kdrv_net]")
{
    kdrv_net_req_t         req;
    kdrv_net_http_result_t res;
    uint8_t                b = 1U;

    /* Prepared once and wiped by the job below, so the second job has no
     * bearer left: it is refused, never sent without one. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_put_prepare(PUT_URL, &b, 1U, PUT_TYPE, PUT_BEARER));

    (void)memset(&req, 0, sizeof(req));
    req.type = KDRV_NET_JOB_HTTP_PUT;

    Mockesp_http_client_Init();
    esp_http_client_init_ExpectAnyArgsAndReturn(NULL);   /* the first job */

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_run(&req));  /* init refused */

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_net_run(&req));
    kdrv_net_http_result(&res);
    TEST_ASSERT_EQUAL_UINT16(0U, res.status);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a PUT sends the body with its type and the bearer, and follows no "
          "redirect", "[kdrv_net]")
{
    kdrv_net_http_result_t res;
    uint8_t                got[40];
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_put_token;

    (void)memset(s_put_body, 0xA5, sizeof(s_put_body));
    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    esp_http_client_write_Stub(put_write_checked);
    put_expect_answer(h, 201);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);

    put_run(PUT_URL, false);

    TEST_ASSERT_EQUAL_UINT(1U, s_init_seen.calls);
    TEST_ASSERT_EQUAL_INT((int)HTTP_METHOD_PUT, s_init_seen.method);
    TEST_ASSERT_TRUE_MESSAGE(s_init_seen.no_redirect,
        "max_redirection_count 0 means ten in esp_http_client: the PUT must "
        "turn redirects off, not count them");
    TEST_ASSERT_EQUAL_STRING(PUT_URL, s_init_seen.url);

    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    kdrv_net_http_result(&res);
    TEST_ASSERT_EQUAL_UINT16(201U, res.status);
    TEST_ASSERT_EQUAL_UINT32(sizeof(s_put_answer) - 1U, res.kept);
    TEST_ASSERT_EQUAL_UINT32(sizeof(s_put_answer) - 1U,
                             kdrv_net_http_body(0U, got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(s_put_answer, got, sizeof(s_put_answer) - 1U);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a kept PUT leaves the handle open and the next PUT to the same URL "
          "reuses it", "[kdrv_net]")
{
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_put_token;

    (void)memset(s_put_body, 0x5A, sizeof(s_put_body));
    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();

    /* First: opened, answered, and NOT closed. */
    esp_http_client_init_Stub(put_init_stub);
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    esp_http_client_write_Stub(put_write_checked);
    put_expect_answer(h, 201);
    esp_http_client_is_persistent_connection_ExpectAndReturn(h, true);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT(1U, s_init_seen.calls);

    /* Second, same URL: the same handle, no init, no set_url - the headers
     * are set again, because the bearer may have been renewed in between. */
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h, 201);
    esp_http_client_is_persistent_connection_ExpectAndReturn(h, true);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_init_seen.calls,
        "a second init means the connection was not reused");

    /* The close job lets go of it, and answers like any HTTP job. */
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_close_run();
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    {
        kdrv_net_http_result_t res;
        kdrv_net_http_result(&res);
        TEST_ASSERT_EQUAL_UINT16(0U, res.status);
    }

    /* And a close with nothing kept touches nothing. */
    put_close_run();
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a PUT to another URL closes the kept handle first", "[kdrv_net]")
{
    esp_http_client_handle_t const h  = (esp_http_client_handle_t)&s_put_token;
    esp_http_client_handle_t const h2 = (esp_http_client_handle_t)&s_put_h2;

    (void)memset(s_put_body, 0x3C, sizeof(s_put_body));
    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    esp_http_client_write_Stub(put_write_checked);
    put_expect_answer(h, 201);
    esp_http_client_is_persistent_connection_ExpectAndReturn(h, true);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);

    /* The old one goes before the new one is made. */
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    s_init_seen.hand_out = h2;
    put_expect_headers(h2);
    esp_http_client_open_ExpectAndReturn(h2, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h2, 201);
    esp_http_client_close_ExpectAndReturn(h2, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h2, ESP_OK);
    put_run(PUT_OTHER_URL, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT(2U, s_init_seen.calls);
    TEST_ASSERT_EQUAL_STRING(PUT_OTHER_URL, s_init_seen.url);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a kept PUT that fails does not keep its handle", "[kdrv_net]")
{
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_put_token;

    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_FAIL);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, s_rc);

    /* Nothing is kept, so the close job has nothing to close. */
    put_close_run();
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a kept PUT whose connection died is an error, not a kept success",
          "[kdrv_net]")
{
    kdrv_net_http_result_t res;
    esp_http_client_handle_t const h  = (esp_http_client_handle_t)&s_put_token;
    esp_http_client_handle_t const h2 = (esp_http_client_handle_t)&s_put_h2;

    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    esp_http_client_write_Stub(put_write_checked);

    /* Kept. */
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h, 201);
    esp_http_client_is_persistent_connection_ExpectAndReturn(h, true);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);

    /* The server closed it meanwhile: open() takes the request, the answer
     * never comes. No status is read, no body, and nothing is kept. */
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    esp_http_client_fetch_headers_ExpectAndReturn(h, (int64_t)-1);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, s_rc);
    kdrv_net_http_result(&res);
    TEST_ASSERT_EQUAL_UINT16(0U, res.status);

    /* So the next PUT starts a connection of its own. */
    s_init_seen.hand_out = h2;
    put_expect_headers(h2);
    esp_http_client_open_ExpectAndReturn(h2, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h2, 201);
    esp_http_client_close_ExpectAndReturn(h2, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h2, ESP_OK);
    put_run(PUT_URL, false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_init_seen.calls,
        "the dead handle was reused instead of replaced");

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a kept PUT is closed when the server does not keep the connection",
          "[kdrv_net]")
{
    esp_http_client_handle_t const h  = (esp_http_client_handle_t)&s_put_token;
    esp_http_client_handle_t const h2 = (esp_http_client_handle_t)&s_put_h2;

    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    esp_http_client_write_Stub(put_write_checked);

    /* keep = true, but the server answered Connection: close. */
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h, 401);
    esp_http_client_is_persistent_connection_ExpectAndReturn(h, false);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);

    /* Nothing is kept: the close job has nothing to close, and the next PUT
     * inits. */
    put_close_run();
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    s_init_seen.hand_out = h2;
    put_expect_headers(h2);
    esp_http_client_open_ExpectAndReturn(h2, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h2, 201);
    esp_http_client_close_ExpectAndReturn(h2, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h2, ESP_OK);
    put_run(PUT_URL, false);
    TEST_ASSERT_EQUAL_UINT(2U, s_init_seen.calls);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a renewed bearer reaches the request on a kept connection",
          "[kdrv_net]")
{
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_put_token;

    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    esp_http_client_write_Stub(put_write_checked);

    put_expect_headers_for(h, "Bearer first");
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h, 201);
    esp_http_client_is_persistent_connection_ExpectAndReturn(h, true);
    put_run_with(PUT_URL, true, "first");
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);

    /* The mock fails the case if the second request carries the old one. */
    put_expect_headers_for(h, "Bearer second");
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h, 201);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_run_with(PUT_URL, false, "second");
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    TEST_ASSERT_EQUAL_UINT(1U, s_init_seen.calls);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

/* ---- What a PUT leaves behind, and what it lets ESP-IDF log --------------- */

/** What the HTTP client's log hold was when the request went out. */
static struct {
    bool           held;
    kode_log_lvl_t min;
} s_open_saw;

/** @brief Stands in for open(): the request goes out here, so this is the
 *  moment the HTTP client's log must be held. Recorded, and judged by the
 *  case: an assertion here would fail on the worker's thread. */
static esp_err_t put_open_held(esp_http_client_handle_t client, int write_len,
                               int num_calls)
{
    (void)client; (void)write_len; (void)num_calls;
    s_open_saw.held = log_stub_held("HTTP_CLIENT", &s_open_saw.min);
    return ESP_OK;
}

/** @brief Whether @p text is anywhere in the PSRAM block kdrv_net's PUT took. */
static bool put_block_holds(const char *text)
{
    size_t                     size = 0U;
    const unsigned char *const blk  = psram_stub_block(KDRV_NET_PUT_MAX, &size);
    size_t const               n    = strlen(text);

    TEST_ASSERT_NOT_NULL(blk);
    for ( size_t i = 0U; (i + n) <= size; ++i )
    {
        if ( memcmp(&blk[i], text, n) == 0 )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("a PUT holds the HTTP client's log at INFO while it runs, and lets "
          "go after", "[kdrv_net]")
{
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_put_token;

    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    put_expect_headers(h);
    esp_http_client_open_Stub(put_open_held);
    esp_http_client_write_Stub(put_write_checked);
    put_expect_answer(h, 201);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    (void)memset(&s_open_saw, 0, sizeof(s_open_saw));
    put_run(PUT_URL, false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);

    TEST_ASSERT_TRUE_MESSAGE(s_open_saw.held,
        "the request went out with ESP-IDF's HTTP client free to log its "
        "headers, the token and the bearer among them, at DEBUG");
    TEST_ASSERT_TRUE(KODE_LOG_INFO <= s_open_saw.min);
    TEST_ASSERT_FALSE_MESSAGE(log_stub_held("HTTP_CLIENT", NULL),
        "the hold outlived the PUT: a GET's log would be held too");

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a PUT whose log cannot be held quiet is not sent", "[kdrv_net]")
{
    kdrv_net_http_result_t res;

    log_stub_refuse_holds(true);

    /* No expectation at all: any call into the HTTP client fails the case. */
    Mockesp_http_client_Init();
    put_run(PUT_URL, false);
    log_stub_refuse_holds(false);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, s_rc);
    kdrv_net_http_result(&res);
    TEST_ASSERT_EQUAL_UINT16(0U, res.status);
    TEST_ASSERT_FALSE(put_block_holds("Bearer " PUT_BEARER));
    TEST_ASSERT_FALSE(put_block_holds(PUT_URL));

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

/* The URL carries the token in its path. Once a job is over nothing needs it:
 * the next PUT is prepared again (its bearer is gone), and a kept handle is
 * matched on its own copy, which goes with the handle. */
TEST_CASE("no copy of the link stays in PSRAM after a PUT that keeps nothing",
          "[kdrv_net]")
{
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_put_token;

    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    esp_http_client_write_Stub(put_write_checked);

    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h, 201);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_run(PUT_URL, false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    TEST_ASSERT_FALSE_MESSAGE(put_block_holds(PUT_URL),
        "the link, token and all, outlived the PUT in PSRAM");
    TEST_ASSERT_FALSE(put_block_holds("Bearer " PUT_BEARER));

    /* A PUT that failed keeps nothing either. */
    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_FAIL);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, s_rc);
    TEST_ASSERT_FALSE(put_block_holds(PUT_URL));

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

TEST_CASE("a kept PUT keeps the link only with its handle, until the close",
          "[kdrv_net]")
{
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_put_token;

    (void)memset(&s_init_seen, 0, sizeof(s_init_seen));
    s_init_seen.hand_out = h;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(put_init_stub);
    esp_http_client_write_Stub(put_write_checked);

    put_expect_headers(h);
    esp_http_client_open_ExpectAndReturn(h, (int)PUT_BODY_LEN, ESP_OK);
    put_expect_answer(h, 201);
    esp_http_client_is_persistent_connection_ExpectAndReturn(h, true);
    put_run(PUT_URL, true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, s_rc);
    /* The handle's own copy: what the next frame is matched against. */
    TEST_ASSERT_TRUE(put_block_holds(PUT_URL));

    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
    put_close_run();
    TEST_ASSERT_FALSE_MESSAGE(put_block_holds(PUT_URL),
        "the close let go of the handle but not of the link");

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

/* ---- The MQTT session, in a build without MQTT ----------------------------- */

TEST_CASE("without the MQTT half, a session is refused and clearing is too",
          "[kdrv_net]")
{
    kdrv_mqtt_session_t sess;

    (void)memset(&sess, 0, sizeof(sess));
    (void)strcpy(sess.client_id, "kode-test");

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_mqtt_session(&sess));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_net_mqtt_session(NULL));
}

/* ---- The stream's open: a budget of its own ------------------------------- */

static int                             s_cfg_timeout;
static uint32_t                        s_open_takes_ms;
static int                             s_client_token;

/** @brief The client every open in this section is given. */
#define STREAM_H  ((esp_http_client_handle_t)&s_client_token)

/** @brief Stands in for esp_http_client_init(): remembers the timeout it was given. */
static esp_http_client_handle_t stream_init(const esp_http_client_config_t *config,
                                            int num_calls)
{
    (void)num_calls;
    s_cfg_timeout = config->timeout_ms;
    return STREAM_H;
}

/** @brief Stands in for the connect: it takes @c s_open_takes_ms of the budget. */
static esp_err_t stream_open_takes(esp_http_client_handle_t client, int write_len,
                                   int num_calls)
{
    (void)client; (void)write_len; (void)num_calls;
    time_stub_advance_ms(s_open_takes_ms);
    return ESP_OK;
}

/** @brief Close the client an open that succeeded left with the driver. */
static void release_stream(void)
{
    Mockesp_http_client_Init();
    esp_http_client_close_IgnoreAndReturn(ESP_OK);
    esp_http_client_cleanup_IgnoreAndReturn(ESP_OK);
    kdrv_net_stream_close();
    Mockesp_http_client_Destroy();
}

/* The open as a whole spends at most KDRV_NET_STREAM_OPEN_MAX_MS, and each I/O
 * is given the time that is left, the way the read gives each of its rounds.
 * A quick connect leaves the normal per-I/O timeout for the headers, and the
 * body is read with the normal one. */
TEST_CASE("a stream open gives each I/O the time left and reads on the normal timeout",
          "[kdrv_net]")
{
    uint32_t total  = 99U;
    uint16_t status = 0U;

    time_stub_set_ms(1000U);
    s_open_takes_ms = 2000U;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(stream_init);
    esp_http_client_open_Stub(stream_open_takes);
    esp_http_client_set_timeout_ms_ExpectAndReturn(STREAM_H, 15000, ESP_OK);
    esp_http_client_fetch_headers_ExpectAndReturn(STREAM_H, 5000);
    esp_http_client_get_status_code_ExpectAndReturn(STREAM_H, 200);
    esp_http_client_set_timeout_ms_ExpectAndReturn(STREAM_H, 15000, ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_stream_open("http://example.invalid/app.bin", &total, &status));
    TEST_ASSERT_EQUAL_INT_MESSAGE(15000, s_cfg_timeout,
        "the connect gets a full per-I/O timeout while the budget is whole");
    TEST_ASSERT_EQUAL_UINT32(5000U, total);
    TEST_ASSERT_EQUAL_UINT16(200U, status);

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
    release_stream();
}

/* A token rides in the query, and the log is read on a cable, over BLE and in
 * bench captures: a GET line names the URL up to its query. */
TEST_CASE("a stream open logs the URL without its query", "[kdrv_net]")
{
    uint32_t total  = 0U;
    uint16_t status = 0U;

    time_stub_set_ms(1000U);
    s_open_takes_ms = 0U;
    log_stub_forget();

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(stream_init);
    esp_http_client_open_Stub(stream_open_takes);
    esp_http_client_set_timeout_ms_IgnoreAndReturn(ESP_OK);
    esp_http_client_fetch_headers_IgnoreAndReturn(5000);
    esp_http_client_get_status_code_IgnoreAndReturn(200);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_stream_open("http://example.invalid/app.bin?token=s3cret#x",
                             &total, &status));
    TEST_ASSERT_TRUE_MESSAGE(log_stub_saw("GET http://example.invalid/app.bin: 5000 bytes"),
        "the open did not name the URL up to its query");
    TEST_ASSERT_FALSE_MESSAGE(log_stub_saw("s3cret"), "the log printed the token");

    Mockesp_http_client_Destroy();
    release_stream();
}

TEST_CASE("a slow connect leaves the headers only what is left of the budget",
          "[kdrv_net]")
{
    uint32_t total  = 0U;
    uint16_t status = 0U;

    time_stub_set_ms(1000U);
    s_open_takes_ms = 14000U;       /* 6 s of the 20 s are left */

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(stream_init);
    esp_http_client_open_Stub(stream_open_takes);
    esp_http_client_set_timeout_ms_ExpectAndReturn(STREAM_H, 6000, ESP_OK);
    esp_http_client_fetch_headers_ExpectAndReturn(STREAM_H, 0);
    esp_http_client_get_status_code_ExpectAndReturn(STREAM_H, 200);
    esp_http_client_set_timeout_ms_ExpectAndReturn(STREAM_H, 15000, ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_stream_open("http://example.invalid/app.bin", &total, &status));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, total,
        "no Content-Length is a 0, not a negative number");

    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
    release_stream();
}

/* The cap is a ceiling, not a hint: a connect that spent all of it is a
 * timeout, and the headers are not waited for. The client is closed and
 * cleaned, and the driver holds nothing. */
TEST_CASE("an open that spent its whole budget is a timeout and holds nothing",
          "[kdrv_net]")
{
    uint32_t total  = 7U;
    uint16_t status = 0U;

    time_stub_set_ms(1000U);
    s_open_takes_ms = KDRV_NET_STREAM_OPEN_MAX_MS;

    Mockesp_http_client_Init();
    esp_http_client_init_Stub(stream_init);
    esp_http_client_open_Stub(stream_open_takes);
    esp_http_client_close_ExpectAndReturn(STREAM_H, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(STREAM_H, ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
        kdrv_net_stream_open("http://example.invalid/app.bin", &total, &status));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7U, total, "no length is reported for a timeout");

    Mockesp_http_client_Verify();

    /* Nothing is held: a second open is not refused as "already open". It
     * gets as far as asking the server, which says no. */
    s_open_takes_ms = 0U;
    esp_http_client_set_timeout_ms_IgnoreAndReturn(ESP_OK);
    esp_http_client_fetch_headers_IgnoreAndReturn(0);
    esp_http_client_get_status_code_IgnoreAndReturn(404);
    esp_http_client_close_IgnoreAndReturn(ESP_OK);
    esp_http_client_cleanup_IgnoreAndReturn(ESP_OK);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
        kdrv_net_stream_open("http://example.invalid/app.bin", &total, &status),
        "the first open left the driver holding a client");
    TEST_ASSERT_EQUAL_UINT16(404U, status);

    Mockesp_http_client_Destroy();
}
