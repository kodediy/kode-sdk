/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_net_headers.c
 * @brief kdrv_net: the request headers of the caller's own - which are sent,
 *        which are refused, and that a token among them stays out of the log.
 */
#include <string.h>

#include "unity.h"

#include "Mockesp_http_client.h"

#include "kdrv_net.h"
#include "host_spies.h"

/** @brief Where the cases send their requests. */
#define HDR_URL         "https://example.invalid/me"
/** @brief The deadline the cases hand the client. */
#define HDR_TIMEOUT_MS  1000U
/** @brief The room of the buffer the cases lend. */
#define HDR_CAP         16U
/** @brief The status the server answers with. */
#define HDR_STATUS      200
/** @brief The client's read size, as kdrv_net asks for it. */
#define HDR_READ        256
/** @brief The request buffer kdrv_net asks for when headers of its caller go out. */
#define HDR_TX_BYTES    4096
/** @brief The token the cases send. */
#define HDR_BEARER      "Bearer abc123"

/** @brief What the server answers. */
static const char s_answer[] = "ok";

/** @brief What the server answers a range with: exactly the room asked for. */
static const char s_range[HDR_CAP] = "0123456789abcdef";

/** @brief What esp_http_client_init() was given, and what open() saw of the log. */
static struct {
    int            buffer_size_tx;
    unsigned       inits;
    bool           held;
    kode_log_lvl_t min;
} s_seen;

/** @brief The handle the client mock hands out. */
static int s_token;

/** @brief Stands in for esp_http_client_init(): records the request buffer's size. */
static esp_http_client_handle_t hdr_init(const esp_http_client_config_t *cfg, int num_calls)
{
    (void)num_calls;
    s_seen.inits++;
    s_seen.buffer_size_tx = cfg->buffer_size_tx;
    return (esp_http_client_handle_t)&s_token;
}

/** @brief Stands in for open(): the headers go out here, so the log's hold is read here. */
static esp_err_t hdr_open(esp_http_client_handle_t client, int write_len, int num_calls)
{
    (void)client; (void)write_len; (void)num_calls;
    s_seen.held = log_stub_held("HTTP_CLIENT", &s_seen.min);
    return ESP_OK;
}

/** @brief A fresh client mock that records what the request asks of it. */
static void hdr_begin(void)
{
    (void)memset(&s_seen, 0, sizeof(s_seen));
    log_stub_forget();
    Mockesp_http_client_Init();
    esp_http_client_init_Stub(hdr_init);
}

/** @brief The answer: headers, the body, its end, the close. */
static void hdr_expect_answer(esp_http_client_handle_t h)
{
    int const n = (int)sizeof(s_answer) - 1;

    esp_http_client_open_Stub(hdr_open);
    esp_http_client_fetch_headers_ExpectAndReturn(h, (int64_t)n);
    esp_http_client_get_status_code_ExpectAndReturn(h, HDR_STATUS);
    esp_http_client_read_ExpectAndReturn(h, NULL, HDR_READ, n);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_read_ReturnArrayThruPtr_buffer((char *)s_answer, n);
    esp_http_client_read_ExpectAndReturn(h, NULL, HDR_READ, 0);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_is_complete_data_received_ExpectAndReturn(h, true);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);
}

/** @brief Every expectation met, the mock put away. */
static void hdr_end(void)
{
    Mockesp_http_client_Verify();
    Mockesp_http_client_Destroy();
}

/* ---- What is refused ------------------------------------------------------- */

TEST_CASE("no list, and an empty one, are sent as none", "[kdrv_net][headers]")
{
    static const kdrv_net_header_t none[] = { { NULL, NULL } };
    uint32_t                       bad    = 7U;

    TEST_ASSERT_TRUE(kdrv_net_headers_ok(NULL, false, &bad));
    TEST_ASSERT_TRUE(kdrv_net_headers_ok(none, true, &bad));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7U, bad, "a list that is sent leaves the index alone");
}

TEST_CASE("eight headers are sent and a ninth refuses the list", "[kdrv_net][headers]")
{
    kdrv_net_header_t list[KDRV_NET_HEADERS_MAX + 2U];
    uint32_t          bad = 0U;

    for ( uint32_t i = 0U; KDRV_NET_HEADERS_MAX >= i; i++ )
    {
        list[i].name  = "X-Item";
        list[i].value = "1";
    }
    list[KDRV_NET_HEADERS_MAX].name      = NULL;
    list[KDRV_NET_HEADERS_MAX + 1U].name = NULL;
    TEST_ASSERT_TRUE(kdrv_net_headers_ok(list, false, &bad));

    list[KDRV_NET_HEADERS_MAX].name = "X-Ninth";
    TEST_ASSERT_FALSE(kdrv_net_headers_ok(list, false, &bad));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(KDRV_NET_HEADERS_MAX, bad,
                                     "a list too long names the first header past the limit");
}

TEST_CASE("a header that would not reach the server as written is refused, by its index",
          "[kdrv_net][headers]")
{
    static const char *const names[]  = { "", "X:Y", "X\rY", "X\nY", "X-Ok", "X-Ok", "X-Ok" };
    static const char *const values[] = { "v", "v", "v", "v", NULL, "a\r\nX-Evil: 1", "a\nb" };
    kdrv_net_header_t        list[3];
    uint32_t                 bad;

    for ( uint32_t i = 0U; (sizeof(names) / sizeof(names[0])) > i; i++ )
    {
        list[0].name  = "Authorization";
        list[0].value = HDR_BEARER;
        list[1].name  = names[i];
        list[1].value = values[i];
        list[2].name  = NULL;
        bad           = 0U;
        TEST_ASSERT_FALSE_MESSAGE(kdrv_net_headers_ok(list, false, &bad), names[i]);
        TEST_ASSERT_EQUAL_UINT32(1U, bad);
    }
}

TEST_CASE("a header of 4000 bytes is sent and one of 4001 refused", "[kdrv_net][headers]")
{
    static char       value[KDRV_NET_HEADER_MAX + 1U];
    kdrv_net_header_t list[2] = { { "X-A", value }, { NULL, NULL } };
    uint32_t          bad     = 9U;

    (void)memset(value, 'v', sizeof(value) - 1U);
    value[KDRV_NET_HEADER_MAX - 3U] = '\0';   /* "X-A" and the value: 4000 bytes */
    TEST_ASSERT_TRUE(kdrv_net_headers_ok(list, false, &bad));

    value[KDRV_NET_HEADER_MAX - 3U] = 'v';
    value[KDRV_NET_HEADER_MAX - 2U] = '\0';   /* one more */
    TEST_ASSERT_FALSE(kdrv_net_headers_ok(list, false, &bad));
    TEST_ASSERT_EQUAL_UINT32(0U, bad);
}

TEST_CASE("a Range of the caller's own is refused on a ranged GET alone", "[kdrv_net][headers]")
{
    static const kdrv_net_header_t list[] = { { "range", "bytes=0-1" }, { NULL, NULL } };

    TEST_ASSERT_TRUE(kdrv_net_headers_ok(list, false, NULL));
    TEST_ASSERT_FALSE(kdrv_net_headers_ok(list, true, NULL));
}

TEST_CASE("a request whose headers are refused never opens a client", "[kdrv_net][headers]")
{
    static const kdrv_net_header_t bad[]   = { { "X-A", "a\r\nb" }, { NULL, NULL } };
    static const kdrv_net_header_t range[] = { { "Range", "bytes=0-1" }, { NULL, NULL } };
    uint8_t                        buf[HDR_CAP];
    kdrv_net_http_result_t         res;

    hdr_begin();   /* nothing expected: any call into the client fails the case */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_get(HDR_URL, bad, buf, sizeof(buf), &res, HDR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_post(HDR_URL, "{}", 2U, NULL, bad, buf, sizeof(buf), &res, HDR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_get_range(HDR_URL, 0U, range, buf, sizeof(buf), &res, HDR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_UINT(0U, s_seen.inits);
    TEST_ASSERT_TRUE(log_stub_saw("a header cannot be sent as it is"));
    hdr_end();
}

/* ---- What is sent ---------------------------------------------------------- */

TEST_CASE("a GET sends the caller's headers in order, from a 4 KB request buffer, "
          "with the client's log held at INFO", "[kdrv_net][headers]")
{
    static const kdrv_net_header_t list[] = {
        { "Authorization", HDR_BEARER },
        { "X-Api-Key",     "k-42" },
        { NULL, NULL },
    };
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_token;
    uint8_t                        buf[HDR_CAP];
    kdrv_net_http_result_t         res;

    hdr_begin();
    esp_http_client_set_header_ExpectAndReturn(h, "Authorization", HDR_BEARER, ESP_OK);
    esp_http_client_set_header_ExpectAndReturn(h, "X-Api-Key", "k-42", ESP_OK);
    hdr_expect_answer(h);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_get(HDR_URL, list, buf, sizeof(buf), &res, HDR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_UINT16((uint16_t)HDR_STATUS, res.status);
    TEST_ASSERT_EQUAL_MEMORY(s_answer, buf, sizeof(s_answer) - 1U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(HDR_TX_BYTES, s_seen.buffer_size_tx,
        "the client's own 512 bytes fail a long header: every header is written whole");
    TEST_ASSERT_TRUE_MESSAGE(s_seen.held,
        "the request went out with the client free to log a bearer at DEBUG");
    TEST_ASSERT_TRUE(KODE_LOG_INFO <= s_seen.min);
    TEST_ASSERT_FALSE_MESSAGE(log_stub_held("HTTP_CLIENT", NULL),
                              "the hold outlived the request");
    hdr_end();
}

TEST_CASE("a GET with no headers of its own keeps the client's buffer and holds no log",
          "[kdrv_net][headers]")
{
    static const kdrv_net_header_t none[] = { { NULL, NULL } };
    esp_http_client_handle_t const h      = (esp_http_client_handle_t)&s_token;
    uint8_t                        buf[HDR_CAP];
    kdrv_net_http_result_t         res;

    hdr_begin();
    hdr_expect_answer(h);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_get(HDR_URL, none, buf, sizeof(buf), &res, HDR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_INT(0, s_seen.buffer_size_tx);
    TEST_ASSERT_FALSE(s_seen.held);
    hdr_end();
}

TEST_CASE("a POST's own Content-Type is set before the caller's, which replaces it",
          "[kdrv_net][headers]")
{
    static const kdrv_net_header_t list[] = {
        { "Content-Type", "application/x-www-form-urlencoded" },
        { NULL, NULL },
    };
    esp_http_client_handle_t const h = (esp_http_client_handle_t)&s_token;
    uint8_t                        buf[HDR_CAP];
    kdrv_net_http_result_t         res;

    hdr_begin();
    esp_http_client_set_header_ExpectAndReturn(h, "Content-Type", "application/json", ESP_OK);
    esp_http_client_set_header_ExpectAndReturn(h, "Content-Type",
                                               "application/x-www-form-urlencoded", ESP_OK);
    hdr_expect_answer(h);
    esp_http_client_write_ExpectAndReturn(h, "a=1", 3, 3);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_post(HDR_URL, "a=1", 3U, "application/json", list, buf, sizeof(buf),
                           &res, HDR_TIMEOUT_MS));
    hdr_end();
}

TEST_CASE("a ranged GET names its bytes before the caller's headers", "[kdrv_net][headers]")
{
    static const kdrv_net_header_t list[] = { { "X-Api-Key", "k-42" }, { NULL, NULL } };
    esp_http_client_handle_t const h      = (esp_http_client_handle_t)&s_token;
    uint8_t                        buf[HDR_CAP];
    kdrv_net_http_result_t         res;

    hdr_begin();
    esp_http_client_set_header_ExpectAndReturn(h, "Range", "bytes=10-25", ESP_OK);
    esp_http_client_set_header_ExpectAndReturn(h, "X-Api-Key", "k-42", ESP_OK);
    esp_http_client_open_Stub(hdr_open);
    esp_http_client_fetch_headers_ExpectAndReturn(h, (int64_t)HDR_CAP);
    esp_http_client_get_status_code_ExpectAndReturn(h, 206);
    esp_http_client_read_ExpectAndReturn(h, NULL, (int)HDR_CAP, (int)HDR_CAP);
    esp_http_client_read_IgnoreArg_buffer();
    esp_http_client_read_ReturnArrayThruPtr_buffer((char *)s_range, (int)HDR_CAP);
    esp_http_client_close_ExpectAndReturn(h, ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_net_http_get_range(HDR_URL, 10U, list, buf, sizeof(buf), &res, HDR_TIMEOUT_MS));
    TEST_ASSERT_EQUAL_UINT16(206U, res.status);
    TEST_ASSERT_EQUAL_MEMORY(s_range, buf, HDR_CAP);
    TEST_ASSERT_TRUE(s_seen.held);
    hdr_end();
}

TEST_CASE("a request whose log cannot be held quiet is not sent", "[kdrv_net][headers]")
{
    static const kdrv_net_header_t list[] = { { "Authorization", HDR_BEARER }, { NULL, NULL } };
    esp_http_client_handle_t const h      = (esp_http_client_handle_t)&s_token;
    uint8_t                        buf[HDR_CAP];
    kdrv_net_http_result_t         res;

    hdr_begin();
    esp_http_client_set_header_ExpectAndReturn(h, "Authorization", HDR_BEARER, ESP_OK);
    esp_http_client_get_and_clear_last_tls_error_IgnoreAndReturn(ESP_OK);
    esp_http_client_cleanup_ExpectAndReturn(h, ESP_OK);   /* and no open */
    log_stub_refuse_holds(true);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kdrv_net_http_get(HDR_URL, list, buf, sizeof(buf), &res, HDR_TIMEOUT_MS));
    log_stub_refuse_holds(false);
    TEST_ASSERT_TRUE(log_stub_saw("could not be held quiet"));
    hdr_end();
}
