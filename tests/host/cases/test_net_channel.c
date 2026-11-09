/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_net_channel.c
 * @brief The Kode channel's pure logic: the answer scanner, the bodies, error verdicts, back-off.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "kaos.h"
#include "kode_wire.h"
#include "kqp_config.h"
#include "net_channel.h"

/** The bench capture: 726 bytes, token masked. */
static const char CONNECT_ANSWER[] =
    "{\"serial\":\"KODE0001F00MR1C\",\"brokerUrl\":\"mqtts://kodes.kode.diy:8883\",\"wssUrl\":\"wss://kodes.kode.diy:8084/mqtt\",\"username\":\"KODE0001F00MR1C\",\"clientId\":\"kode-KODE0001F00MR1C\",\"token\":\""
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx"
    "\",\"expiresAt\":\"2026-10-17T10:52:04.438Z\",\"topics\":{\"in\":\"kode/KODE0001F00MR1C/cmd/in\",\"out\":\"kode/KODE0001F00MR1C/cmd/out\",\"status\":\"kode/KODE0001F00MR1C/status\"}}";

/** Fill @p out with a connect answer carrying @p token_len x as its token. */
static size_t answer_with_token(char *out, size_t cap, size_t token_len)
{
    size_t n = 0U;
    n += (size_t)snprintf(out + n, cap - n,
                          "{\"serial\":\"S1\",\"brokerUrl\":\"mqtts://h:1\","
                          "\"username\":\"S1\",\"clientId\":\"kode-S1\","
                          "\"token\":\"");
    for ( size_t i = 0U; i < token_len; i++ )
    {
        out[n++] = 'x';
    }
    n += (size_t)snprintf(out + n, cap - n,
                          "\",\"topics\":{\"in\":\"a/in\",\"out\":\"a/out\","
                          "\"status\":\"a/st\"}}");
    return n;
}

/** @brief The bench capture with a `shots` member added before its closing brace. */
static size_t answer_with_shots(char *out, size_t cap, const char *shots)
{
    size_t base = strlen(CONNECT_ANSWER) - 1U;   /* up to the closing brace */

    (void)memcpy(out, CONNECT_ANSWER, base);
    return base + (size_t)snprintf(out + base, cap - base,
                                   ",\"shots\":\"%s\"}", shots);
}

/* ---- The scanner ------------------------------------------------------------ */

TEST_CASE("the bench capture parses into a whole session", "[net_channel]")
{
    kchan_session_t s;

    TEST_ASSERT_EQUAL_UINT(726U, strlen(CONNECT_ANSWER));
    TEST_ASSERT_TRUE(kchan_parse_connect(CONNECT_ANSWER, strlen(CONNECT_ANSWER), NULL, &s));
    TEST_ASSERT_EQUAL_STRING("KODE0001F00MR1C", s.serial);
    TEST_ASSERT_EQUAL_STRING("mqtts://kodes.kode.diy:8883", s.broker);
    TEST_ASSERT_EQUAL_STRING("KODE0001F00MR1C", s.user);
    TEST_ASSERT_EQUAL_STRING("kode-KODE0001F00MR1C", s.client_id);
    TEST_ASSERT_EQUAL_UINT(379U, strlen(s.token));
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/cmd/in", s.t_in);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/cmd/out", s.t_out);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/status", s.t_status);
}

/* ---- The shots prefix ------------------------------------------------------- */

/** @brief The prefix the api names for this unit's uploads. */
#define SHOTS_GOOD "https://api.kode.diy/kodes/KODE0001F00MR1C/shots/up/"

TEST_CASE("the connect answer's shots prefix is kept", "[net_channel]")
{
    char            buf[1024];
    kchan_session_t s;
    size_t          n = answer_with_shots(buf, sizeof(buf), SHOTS_GOOD);

    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_STRING(SHOTS_GOOD, s.shots);
}

TEST_CASE("a connect answer without shots is still a session, with no uploads",
          "[net_channel]")
{
    kchan_session_t s;

    memset(&s, 0x5A, sizeof(s));
    TEST_ASSERT_TRUE(kchan_parse_connect(CONNECT_ANSWER, strlen(CONNECT_ANSWER), NULL, &s));
    TEST_ASSERT_EQUAL_UINT(0U, s.shots[0]);
}

TEST_CASE("a shots prefix that is not https or has no trailing slash is dropped",
          "[net_channel]")
{
    char            buf[1024];
    kchan_session_t s;
    size_t          n;

    n = answer_with_shots(buf, sizeof(buf), "http://api.kode.diy/kodes/S/shots/up/");
    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_UINT(0U, s.shots[0]);
    TEST_ASSERT_EQUAL_STRING("mqtts://kodes.kode.diy:8883", s.broker);

    n = answer_with_shots(buf, sizeof(buf), "https://api.kode.diy/kodes/S/shots/up");
    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_UINT(0U, s.shots[0]);
}

TEST_CASE("a shots prefix past 128 bytes is dropped", "[net_channel]")
{
    char            buf[1024];
    char            prefix[200];
    kchan_session_t s;
    size_t          n;
    size_t          at = (size_t)snprintf(prefix, sizeof(prefix), "https://");

    /* 8 + 119 + 1 = 128 bytes: the longest that is kept. */
    memset(prefix + at, 'a', 119U);
    prefix[at + 119U] = '/';
    prefix[at + 120U] = '\0';
    TEST_ASSERT_EQUAL_UINT(128U, strlen(prefix));
    n = answer_with_shots(buf, sizeof(buf), prefix);
    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_STRING(prefix, s.shots);

    /* One byte more: 129, dropped, the session still good. */
    memset(prefix + at, 'a', 120U);
    prefix[at + 120U] = '/';
    prefix[at + 121U] = '\0';
    TEST_ASSERT_EQUAL_UINT(129U, strlen(prefix));
    n = answer_with_shots(buf, sizeof(buf), prefix);
    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_UINT(0U, s.shots[0]);
}

TEST_CASE("kchan_shots_valid takes an https prefix ending in a slash only",
          "[net_channel]")
{
    TEST_ASSERT_FALSE(kchan_shots_valid(NULL));
    TEST_ASSERT_FALSE(kchan_shots_valid(""));
    TEST_ASSERT_TRUE(kchan_shots_valid("https://a/"));
    TEST_ASSERT_FALSE(kchan_shots_valid("https://a b/"));
    TEST_ASSERT_FALSE(kchan_shots_valid("https://a\tb/"));
    TEST_ASSERT_FALSE(kchan_shots_valid("http://a/"));
    TEST_ASSERT_FALSE(kchan_shots_valid("https://a"));
    TEST_ASSERT_FALSE(kchan_shots_valid("https:/"));
}

/* A prefix with no host: esp_http_client would refuse to parse it, and its
 * error line prints the whole URL - the token included - at ERROR, which is
 * always on. */
TEST_CASE("kchan_shots_valid refuses a prefix with no host after the scheme",
          "[net_channel]")
{
    TEST_ASSERT_FALSE(kchan_shots_valid("https:///"));
    TEST_ASSERT_FALSE(kchan_shots_valid("https:///kodes/x/shots/up/"));
    TEST_ASSERT_FALSE(kchan_shots_valid("https://"));
    TEST_ASSERT_TRUE(kchan_shots_valid("https://h/"));
}


TEST_CASE("a missing field refuses the whole answer and zeroes the session",
          "[net_channel]")
{
    char            buf[1024];
    kchan_session_t s;
    /* "token" renamed: every other field is still there. */
    size_t n = answer_with_token(buf, sizeof(buf), 10U);
    char  *t = strstr(buf, "\"token\"");

    TEST_ASSERT_NOT_NULL(t);
    t[1] = 'T';
    memset(&s, 0x5A, sizeof(s));
    TEST_ASSERT_FALSE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_UINT(0U, s.serial[0]);
    TEST_ASSERT_EQUAL_UINT(0U, s.token[0]);
}

TEST_CASE("the token is accepted up to 511 characters and refused past it",
          "[net_channel]")
{
    char            buf[1024];
    kchan_session_t s;
    size_t          n = answer_with_token(buf, sizeof(buf), 511U);

    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_UINT(511U, strlen(s.token));
    n = answer_with_token(buf, sizeof(buf), 512U);
    TEST_ASSERT_FALSE(kchan_parse_connect(buf, n, NULL, &s));
    n = answer_with_token(buf, sizeof(buf), 600U);
    TEST_ASSERT_FALSE(kchan_parse_connect(buf, n, NULL, &s));
    TEST_ASSERT_EQUAL_UINT(0U, s.token[0]);
}

/** @brief A complete answer whose broker, serial and in-topic are given. */
static size_t answer_for(char *out, size_t cap, const char *broker,
                         const char *serial, const char *t_in, const char *client)
{
    return (size_t)snprintf(out, cap,
        "{\"serial\":\"%s\",\"brokerUrl\":\"%s\",\"username\":\"%s\","
        "\"clientId\":\"%s\",\"token\":\"t\",\"in\":\"%s\",\"out\":\"o\","
        "\"status\":\"s\"}", serial, broker, serial, client, t_in);
}

TEST_CASE("an answer for a plain broker, another unit or a wildcard is refused",
          "[net_channel]")
{
    char            buf[512];
    kchan_session_t s;
    size_t          n;

    n = answer_for(buf, sizeof(buf), "mqtts://h:1", "S1", "kode/S1/cmd/in", "c");
    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, "S1", &s));
    TEST_ASSERT_TRUE(kchan_parse_connect(buf, n, "", &s));

    n = answer_for(buf, sizeof(buf), "mqtt://h:1883", "S1", "kode/S1/cmd/in", "c");
    TEST_ASSERT_FALSE_MESSAGE(kchan_parse_connect(buf, n, "S1", &s),
        "a broker without TLS would carry the token in clear");
    TEST_ASSERT_EQUAL_UINT(0U, s.token[0]);

    n = answer_for(buf, sizeof(buf), "mqtts://h:1", "S2", "kode/S2/cmd/in", "c");
    TEST_ASSERT_FALSE_MESSAGE(kchan_parse_connect(buf, n, "S1", &s),
        "another unit's session was taken");

    n = answer_for(buf, sizeof(buf), "mqtts://h:1", "S1", "#", "c");
    TEST_ASSERT_FALSE_MESSAGE(kchan_parse_connect(buf, n, "S1", &s),
        "a wildcard topic was taken");
    n = answer_for(buf, sizeof(buf), "mqtts://h:1", "S1", "kode/+/cmd/in", "c");
    TEST_ASSERT_FALSE(kchan_parse_connect(buf, n, "S1", &s));

    n = answer_for(buf, sizeof(buf), "mqtts://h:1", "S1", "kode/S1/cmd/in", "");
    TEST_ASSERT_FALSE_MESSAGE(kchan_parse_connect(buf, n, "S1", &s),
        "an empty client id was taken");
}

TEST_CASE("the client id is used exactly as the api gave it", "[net_channel]")
{
    kchan_session_t s;
    static const char plain[] =
        "{\"serial\":\"S1\",\"brokerUrl\":\"mqtts://h:1\",\"username\":\"S1\","
        "\"clientId\":\"aA\",\"token\":\"t\",\"in\":\"i\",\"out\":\"o\","
        "\"status\":\"s\"}";
    /* The backslash is an escape the scanner does not read. */
    static const char escaped[] =
        "{\"serial\":\"S1\",\"brokerUrl\":\"mqtts://h:1\",\"username\":\"S1\","
        "\"clientId\":\"a\\A\",\"token\":\"t\",\"in\":\"i\",\"out\":\"o\","
        "\"status\":\"s\"}";

    TEST_ASSERT_TRUE(kchan_parse_connect(plain, strlen(plain), NULL, &s));
    TEST_ASSERT_EQUAL_STRING("aA", s.client_id);
    TEST_ASSERT_FALSE(kchan_parse_connect(escaped, strlen(escaped), NULL, &s));
}

TEST_CASE("a key repeated in an answer reads as its first occurrence",
          "[net_channel]")
{
    static const char twice[] = "{\"nonce\":\"first\",\"x\":{\"nonce\":\"second\"}}";
    char              out[16];

    TEST_ASSERT_TRUE(kchan_json_str(twice, strlen(twice), "nonce", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("first", out);
}

TEST_CASE("an escaped slash is read, any other escape refused",
          "[net_channel]")
{
    static const char slash[]  = "{\"brokerUrl\":\"mqtts:\\/\\/kodes.kode.diy:8883\"}";
    static const char quote[]  = "{\"k\":\"a\\\"b\\\\c\"}";
    static const char nl[]     = "{\"k\":\"a\\nb\"}";
    static const char uni[]    = "{\"k\":\"a\\u0041b\"}";
    char              out[64];

    TEST_ASSERT_TRUE(kchan_json_str(slash, strlen(slash), "brokerUrl", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mqtts://kodes.kode.diy:8883", out);
    TEST_ASSERT_TRUE(kchan_json_str(quote, strlen(quote), "k", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c", out);
    TEST_ASSERT_FALSE(kchan_json_str(nl, strlen(nl), "k", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0U, out[0]);
    TEST_ASSERT_FALSE(kchan_json_str(uni, strlen(uni), "k", out, sizeof(out)));
    TEST_ASSERT_EQUAL_UINT(0U, out[0]);
}

TEST_CASE("a body that is not JSON, or is cut, is refused", "[net_channel]")
{
    kchan_session_t   s;
    static const char html[]    = "<html>503</html>";
    static const char cut[]     = "{\"serial\":\"KODE0001F00M";
    static const char number[]  = "{\"serial\":12}";
    char              out[16];

    TEST_ASSERT_FALSE(kchan_parse_connect(html, strlen(html), NULL, &s));
    TEST_ASSERT_FALSE(kchan_parse_connect(cut, strlen(cut), NULL, &s));
    TEST_ASSERT_FALSE(kchan_parse_connect("", 0U, NULL, &s));
    TEST_ASSERT_FALSE(kchan_json_str(number, strlen(number), "serial", out, sizeof(out)));
    TEST_ASSERT_FALSE(kchan_json_str(NULL, 0U, "serial", out, sizeof(out)));
}

TEST_CASE("a value spelling a key is not the key, and the value must fit",
          "[net_channel]")
{
    static const char tricky[] = "{\"a\":\"nonce\",\"nonce\":\"v1.x\"}";
    char              out[8];

    TEST_ASSERT_TRUE(kchan_json_str(tricky, strlen(tricky), "nonce", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("v1.x", out);
    /* "v1.x" is 4 characters: it needs 5 bytes. */
    TEST_ASSERT_TRUE(kchan_json_str(tricky, strlen(tricky), "nonce", out, 5U));
    TEST_ASSERT_FALSE(kchan_json_str(tricky, strlen(tricky), "nonce", out, 4U));
    TEST_ASSERT_EQUAL_UINT(0U, out[0]);
}

/* ---- The challenge ---------------------------------------------------------- */

TEST_CASE("a nonce inside the alphabet is read, one outside is refused",
          "[net_channel]")
{
    char               nonce[KODE_NONCE_MAX + 1U];
    static const char good[]  = "{\"nonce\":\"v1.connect.1792148038\"}";
    static const char space[] = "{\"nonce\":\"v1 connect\"}";
    static const char none[]  = "{\"challenge\":\"v1\"}";
    char               big[256];

    TEST_ASSERT_TRUE(kchan_parse_nonce(good, strlen(good), nonce, sizeof(nonce)));
    TEST_ASSERT_EQUAL_STRING("v1.connect.1792148038", nonce);
    TEST_ASSERT_FALSE(kchan_parse_nonce(space, strlen(space), nonce, sizeof(nonce)));
    TEST_ASSERT_EQUAL_UINT(0U, nonce[0]);
    TEST_ASSERT_FALSE(kchan_parse_nonce(none, strlen(none), nonce, sizeof(nonce)));

    /* 200 characters pass, 201 do not, whatever room the caller gave. */
    memset(big, 0, sizeof(big));
    (void)snprintf(big, sizeof(big), "{\"nonce\":\"");
    memset(big + strlen(big), 'n', 200U);
    strcat(big, "\"}");
    TEST_ASSERT_TRUE(kchan_parse_nonce(big, strlen(big), nonce, sizeof(nonce)));
    (void)snprintf(big, sizeof(big), "{\"nonce\":\"");
    memset(big + strlen(big), 'n', 201U);
    strcat(big, "\"}");
    TEST_ASSERT_FALSE(kchan_parse_nonce(big, strlen(big), nonce, sizeof(nonce)));
    {
        char wide[256];
        TEST_ASSERT_FALSE(kchan_parse_nonce(big, strlen(big), wide, sizeof(wide)));
    }
}

/* ---- The bodies ------------------------------------------------------------- */

TEST_CASE("the connect body round-trips through the scanner", "[net_channel]")
{
    char   body[128];
    char   out[64];
    size_t n = kchan_connect_body(body, sizeof(body), "Q0VSVA+/==",
                                  "v1.connect.9", "U0lH/g==");

    TEST_ASSERT_EQUAL_STRING("{\"cert\":\"Q0VSVA+/==\",\"nonce\":\"v1.connect.9\","
                             "\"signature\":\"U0lH/g==\"}", body);
    TEST_ASSERT_EQUAL_UINT(strlen(body), n);
    TEST_ASSERT_TRUE(kchan_json_str(body, n, "cert", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("Q0VSVA+/==", out);
    TEST_ASSERT_TRUE(kchan_json_str(body, n, "nonce", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("v1.connect.9", out);
    TEST_ASSERT_TRUE(kchan_json_str(body, n, "signature", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("U0lH/g==", out);
}

TEST_CASE("the connect body fits to the byte and not one byte less",
          "[net_channel]")
{
    char   body[128];
    size_t n = kchan_connect_body(body, sizeof(body), "C", "N", "S");

    TEST_ASSERT_GREATER_THAN_UINT(0U, n);
    TEST_ASSERT_EQUAL_UINT(n, kchan_connect_body(body, n + 1U, "C", "N", "S"));
    TEST_ASSERT_EQUAL_UINT(0U, kchan_connect_body(body, n, "C", "N", "S"));
    TEST_ASSERT_EQUAL_UINT(0U, body[0]);
    TEST_ASSERT_EQUAL_UINT(0U, kchan_connect_body(body, 0U, "C", "N", "S"));
    TEST_ASSERT_EQUAL_UINT(0U, kchan_connect_body(NULL, 16U, "C", "N", "S"));
}

TEST_CASE("a body never carries a character JSON would have to escape",
          "[net_channel]")
{
    char body[128];

    TEST_ASSERT_EQUAL_UINT(0U, kchan_connect_body(body, sizeof(body), "a\"b", "N", "S"));
    TEST_ASSERT_EQUAL_UINT(0U, kchan_connect_body(body, sizeof(body), "C", "a\\b", "S"));
    TEST_ASSERT_EQUAL_UINT(0U, kchan_connect_body(body, sizeof(body), "C", "N", "a\nb"));
    TEST_ASSERT_EQUAL_UINT(0U, kchan_connect_body(body, sizeof(body), NULL, "N", "S"));
    TEST_ASSERT_EQUAL_UINT(0U, kchan_status_body(body, sizeof(body), "kodeos", "1\"2"));
    TEST_ASSERT_EQUAL_UINT(0U, kchan_status_body(body, sizeof(body), "sn\"ake", "1.2.0"));
}

TEST_CASE("the status body and the last will are the spec's", "[net_channel]")
{
    char   body[96];
    size_t n = kchan_status_body(body, sizeof(body), "kodeos", "v00.00.99");

    TEST_ASSERT_EQUAL_STRING("{\"online\":true,\"app\":\"kodeos\",\"version\":\"v00.00.99\"}",
                             body);
    TEST_ASSERT_EQUAL_UINT(strlen(body), n);
    TEST_ASSERT_EQUAL_UINT(0U, kchan_status_body(body, n, "kodeos", "v00.00.99"));
    TEST_ASSERT_EQUAL_STRING("{\"online\":false}", KCHAN_WILL);
    TEST_ASSERT_TRUE(strlen(KCHAN_WILL) < KDRV_MQTT_WILL_MAX);
}

TEST_CASE("the status body names the app that holds the channel", "[net_channel]")
{
    char   body[96];
    size_t n = kchan_status_body(body, sizeof(body), "snake", "1.2.0");

    TEST_ASSERT_EQUAL_STRING("{\"online\":true,\"app\":\"snake\",\"version\":\"1.2.0\"}",
                             body);
    TEST_ASSERT_EQUAL_UINT(strlen(body), n);
}

/* ---- The verdict and the back-off ------------------------------------------- */

TEST_CASE("only a refused identity is terminal", "[net_channel]")
{
    TEST_ASSERT_EQUAL(KCHAN_TERMINAL, kchan_classify(401U, "bad_cert"));
    TEST_ASSERT_EQUAL(KCHAN_TERMINAL, kchan_classify(401U, "untrusted_cert"));
    TEST_ASSERT_EQUAL(KCHAN_TERMINAL, kchan_classify(401U, "cert_expired"));
    TEST_ASSERT_EQUAL(KCHAN_TERMINAL, kchan_classify(401U, "not_client_cert"));
    TEST_ASSERT_EQUAL(KCHAN_RETRY, kchan_classify(401U, "bad_signature"));
    TEST_ASSERT_EQUAL(KCHAN_RETRY, kchan_classify(401U, ""));
    TEST_ASSERT_EQUAL(KCHAN_RETRY, kchan_classify(401U, NULL));
    TEST_ASSERT_EQUAL(KCHAN_RETRY, kchan_classify(503U, "channel_unavailable"));
    TEST_ASSERT_EQUAL(KCHAN_RETRY, kchan_classify(429U, ""));
    TEST_ASSERT_EQUAL(KCHAN_RETRY, kchan_classify(0U, NULL));
    /* The word alone is not enough: it has to come with the 401. */
    TEST_ASSERT_EQUAL(KCHAN_RETRY, kchan_classify(503U, "bad_cert"));
}

TEST_CASE("the back-off doubles to the cap with 25 percent of jitter",
          "[net_channel]")
{
    TEST_ASSERT_EQUAL_UINT32(3750U, kchan_backoff_ms(0U, 0U));
    TEST_ASSERT_EQUAL_UINT32(5000U, kchan_backoff_ms(0U, 25U));
    TEST_ASSERT_EQUAL_UINT32(6250U, kchan_backoff_ms(0U, 50U));
    TEST_ASSERT_EQUAL_UINT32(3750U, kchan_backoff_ms(0U, 51U));
    TEST_ASSERT_EQUAL_UINT32(10000U, kchan_backoff_ms(1U, 25U));
    TEST_ASSERT_EQUAL_UINT32(320000U, kchan_backoff_ms(6U, 25U));
    TEST_ASSERT_EQUAL_UINT32(600000U, kchan_backoff_ms(7U, 25U));
    TEST_ASSERT_EQUAL_UINT32(600000U, kchan_backoff_ms(20U, 25U));
    TEST_ASSERT_EQUAL_UINT32(600000U, kchan_backoff_ms(255U, 25U));
    TEST_ASSERT_EQUAL_UINT32(750000U, kchan_backoff_ms(255U, 50U));
}

TEST_CASE("the back-off never shrinks as attempts pile up", "[net_channel]")
{
    for ( uint32_t rnd = 0U; 51U > rnd; rnd++ )
    {
        uint32_t prev = 0U;
        for ( uint32_t a = 0U; 256U > a; a++ )
        {
            uint32_t const w = kchan_backoff_ms((uint8_t)a, rnd);
            TEST_ASSERT_TRUE(w >= prev);
            TEST_ASSERT_TRUE(3750U <= w);
            TEST_ASSERT_TRUE(750000U >= w);
            prev = w;
        }
    }
}

/* ---- The reply ring ------------------------------------------------------- */

static unsigned s_wakes;
static bool     s_wake_answer;

static bool count_wake(void)
{
    s_wakes++;
    return s_wake_answer;
}

/** @brief The ring, empty, with a counting wake and nothing carried over. */
static void ring_fresh(void)
{
    char scratch[KODE_LOG_LINE_MAX + 1U];

    TEST_ASSERT_TRUE(kchan_out_init());
    kchan_out_set_wake(NULL);
    while ( kchan_out_pop(scratch, sizeof(scratch)) != 0U )
    {
        /* emptied */
    }
    s_wakes       = 0U;
    s_wake_answer = true;
    kchan_out_set_wake(&count_wake);
}

TEST_CASE("the ring hands lines back in the order they came", "[net_channel]")
{
    char got[KODE_LOG_LINE_MAX + 1U];

    ring_fresh();

    TEST_ASSERT_TRUE(kchan_out_push(">|a=1", 5U));
    TEST_ASSERT_TRUE(kchan_out_push(">|b=22", 6U));
    TEST_ASSERT_TRUE(kchan_out_push(">ok", 3U));
    TEST_ASSERT_TRUE(kchan_out_pending());

    TEST_ASSERT_EQUAL_UINT(5U, kchan_out_pop(got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING(">|a=1", got);
    TEST_ASSERT_EQUAL_UINT(6U, kchan_out_pop(got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING(">|b=22", got);
    TEST_ASSERT_EQUAL_UINT(3U, kchan_out_pop(got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING(">ok", got);
    TEST_ASSERT_EQUAL_UINT(0U, kchan_out_pop(got, sizeof(got)));
    TEST_ASSERT_FALSE(kchan_out_pending());
}

TEST_CASE("a line of 256 bytes goes through whole", "[net_channel]")
{
    char line[KODE_LOG_LINE_MAX];
    char got[KODE_LOG_LINE_MAX + 1U];

    ring_fresh();

    for ( size_t i = 0U; sizeof(line) > i; i++ )
    {
        line[i] = (char)('a' + (i % 26U));
    }
    TEST_ASSERT_TRUE(kchan_out_push(line, sizeof(line)));
    TEST_ASSERT_EQUAL_UINT(sizeof(line), kchan_out_pop(got, sizeof(got)));
    TEST_ASSERT_EQUAL_MEMORY(line, got, sizeof(line));
    TEST_ASSERT_EQUAL_CHAR('\0', got[sizeof(line)]);
}

TEST_CASE("a full ring refuses the next line and counts it lost", "[net_channel]")
{
    char     line[KODE_LOG_LINE_MAX];
    char     got[KODE_LOG_LINE_MAX + 1U];
    unsigned stored = 0U;
    uint32_t lost0;

    ring_fresh();
    (void)memset(line, 'x', sizeof(line));
    lost0 = kchan_out_lost();

    for ( ;; )
    {
        line[0] = (char)('A' + (stored % 26U));
        if ( !kchan_out_push(line, sizeof(line)) )
        {
            break;
        }
        stored++;
        TEST_ASSERT_TRUE_MESSAGE(200U > stored, "the ring never filled");
    }
    TEST_ASSERT_EQUAL_UINT32(lost0 + 1U, kchan_out_lost());
    TEST_ASSERT_TRUE(120U <= stored);   /* 32 KB of 258-byte records */

    /* What was stored is intact and in order, and the ring takes lines again. */
    for ( unsigned i = 0U; i < stored; i++ )
    {
        TEST_ASSERT_EQUAL_UINT(sizeof(line), kchan_out_pop(got, sizeof(got)));
        TEST_ASSERT_EQUAL_CHAR((char)('A' + (i % 26U)), got[0]);
    }
    TEST_ASSERT_TRUE(kchan_out_push(">ok", 3U));
}

TEST_CASE("a line longer than the log's, or none at all, is not stored",
          "[net_channel]")
{
    char     big[KODE_LOG_LINE_MAX + 1U];
    uint32_t lost0;

    ring_fresh();
    (void)memset(big, 'x', sizeof(big));
    lost0 = kchan_out_lost();

    TEST_ASSERT_FALSE(kchan_out_push(big, sizeof(big)));
    TEST_ASSERT_FALSE(kchan_out_push(big, 0U));
    TEST_ASSERT_EQUAL_UINT32(lost0 + 2U, kchan_out_lost());
    TEST_ASSERT_FALSE(kchan_out_pending());
}

TEST_CASE("records wrap around the end of the ring without a byte changing",
          "[net_channel]")
{
    char got[KODE_LOG_LINE_MAX + 1U];
    char want[KODE_LOG_LINE_MAX + 1U];

    ring_fresh();

    /* ~20 times the ring's size through it, in lines of every length, with a
     * few always waiting, so the head and the tail both cross the end often. */
    unsigned next_in  = 0U;
    unsigned next_out = 0U;

    for ( unsigned round = 0U; 2000U > round; round++ )
    {
        size_t const len = 1U + ((round * 37U) % KODE_LOG_LINE_MAX);

        (void)memset(want, (int)('a' + (round % 26U)), len);
        TEST_ASSERT_TRUE(kchan_out_push(want, len));
        next_in++;
        if ( (round % 3U) != 0U )
        {
            size_t const n = kchan_out_pop(got, sizeof(got));
            size_t const w = 1U + ((next_out * 37U) % KODE_LOG_LINE_MAX);
            TEST_ASSERT_EQUAL_UINT(w, n);
            TEST_ASSERT_EQUAL_CHAR((char)('a' + (next_out % 26U)), got[0]);
            TEST_ASSERT_EQUAL_CHAR((char)('a' + (next_out % 26U)), got[n - 1U]);
            next_out++;
        }
        if ( (next_in - next_out) > 40U )
        {
            while ( next_out < next_in )
            {
                size_t const n = kchan_out_pop(got, sizeof(got));
                TEST_ASSERT_EQUAL_UINT(1U + ((next_out * 37U) % KODE_LOG_LINE_MAX), n);
                next_out++;
            }
        }
    }
}

TEST_CASE("the wake fires when the ring stops being empty, and again if it "
          "could not be delivered", "[net_channel]")
{
    char got[KODE_LOG_LINE_MAX + 1U];

    ring_fresh();

    (void)kchan_out_push("a", 1U);
    (void)kchan_out_push("b", 1U);
    TEST_ASSERT_EQUAL_UINT(1U, s_wakes);          /* the edge only */
    (void)kchan_out_pop(got, sizeof(got));
    (void)kchan_out_pop(got, sizeof(got));
    (void)kchan_out_push("c", 1U);
    TEST_ASSERT_EQUAL_UINT(2U, s_wakes);          /* empty again, so again */

    (void)kchan_out_pop(got, sizeof(got));
    s_wake_answer = false;                         /* the consumer cannot be told */
    (void)kchan_out_push("d", 1U);
    TEST_ASSERT_EQUAL_UINT(3U, s_wakes);
    (void)kchan_out_push("e", 1U);
    TEST_ASSERT_EQUAL_UINT(4U, s_wakes);          /* tried again: it was owed */
    s_wake_answer = true;
    (void)kchan_out_push("f", 1U);
    TEST_ASSERT_EQUAL_UINT(5U, s_wakes);          /* this one got through */
    (void)kchan_out_push("g", 1U);
    TEST_ASSERT_EQUAL_UINT(5U, s_wakes);          /* and now it is not owed */
}

TEST_CASE("clearing the ring counts every waiting line as lost", "[net_channel]")
{
    char     got[KODE_LOG_LINE_MAX + 1U];
    uint32_t lost0;

    ring_fresh();
    (void)kchan_out_push("a", 1U);
    (void)kchan_out_push("b", 1U);
    (void)kchan_out_push("c", 1U);
    lost0 = kchan_out_lost();

    kchan_out_clear();

    TEST_ASSERT_EQUAL_UINT32(lost0 + 3U, kchan_out_lost());
    TEST_ASSERT_FALSE(kchan_out_pending());
    TEST_ASSERT_EQUAL_UINT(0U, kchan_out_pop(got, sizeof(got)));
    kchan_out_lose(2U);
    TEST_ASSERT_EQUAL_UINT32(lost0 + 5U, kchan_out_lost());
}

TEST_CASE("a small buffer cuts the line it is given and the ring moves on",
          "[net_channel]")
{
    char got[5];

    ring_fresh();
    (void)kchan_out_push(">|name=long", 11U);
    (void)kchan_out_push(">ok", 3U);

    TEST_ASSERT_EQUAL_UINT(4U, kchan_out_pop(got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING(">|na", got);
    TEST_ASSERT_EQUAL_UINT(3U, kchan_out_pop(got, sizeof(got)));
    TEST_ASSERT_EQUAL_STRING(">ok", got);
}

/* ---- A number in an answer --------------------------------------------------- */

TEST_CASE("a whole number is read, anything else under the key is refused",
          "[net_channel]")
{
    static const char shot[]  = "{\"ok\":true,\"id\":\"sh_1\",\"seq\":42}";
    static const char spaced[] = "{\"seq\" : 7 , \"x\":1}";
    static const char big[]   = "{\"seq\":4294967295}";
    static const char over[]  = "{\"seq\":4294967296}";
    static const char neg[]   = "{\"seq\":-1}";
    static const char frac[]  = "{\"seq\":1.5}";
    static const char str[]   = "{\"seq\":\"3\"}";
    static const char none[]  = "{\"seq\":}";
    static const char cut[]   = "{\"seq\":12";
    uint32_t          n = 99U;

    TEST_ASSERT_TRUE(kchan_json_u32(shot, strlen(shot), "seq", &n));
    TEST_ASSERT_EQUAL_UINT32(42U, n);
    TEST_ASSERT_TRUE(kchan_json_u32(spaced, strlen(spaced), "seq", &n));
    TEST_ASSERT_EQUAL_UINT32(7U, n);
    TEST_ASSERT_TRUE(kchan_json_u32(big, strlen(big), "seq", &n));
    TEST_ASSERT_EQUAL_UINT32(4294967295U, n);
    /* A number that ends where the text does may have been cut. */
    TEST_ASSERT_FALSE(kchan_json_u32(cut, strlen(cut), "seq", &n));

    TEST_ASSERT_FALSE(kchan_json_u32(over, strlen(over), "seq", &n));
    TEST_ASSERT_EQUAL_UINT32(0U, n);
    TEST_ASSERT_FALSE(kchan_json_u32(neg, strlen(neg), "seq", &n));
    TEST_ASSERT_FALSE(kchan_json_u32(frac, strlen(frac), "seq", &n));
    TEST_ASSERT_FALSE(kchan_json_u32(str, strlen(str), "seq", &n));
    TEST_ASSERT_FALSE(kchan_json_u32(none, strlen(none), "seq", &n));
    TEST_ASSERT_FALSE(kchan_json_u32(shot, strlen(shot), "missing", &n));
    TEST_ASSERT_FALSE(kchan_json_u32(NULL, 0U, "seq", &n));
    TEST_ASSERT_FALSE(kchan_json_u32(shot, strlen(shot), "seq", NULL));
    /* The string reader still reads the same answer's string. */
    {
        char id[8];
        TEST_ASSERT_TRUE(kchan_json_str(shot, strlen(shot), "id", id, sizeof(id)));
        TEST_ASSERT_EQUAL_STRING("sh_1", id);
    }
}

/* ---- An upload token ------------------------------------------------------- */

TEST_CASE("an upload token is 1 to 128 letters, digits, underscores and dashes",
          "[net_channel]")
{
    char tok[KSHOT_TOKEN_MAX + 2U];

    TEST_ASSERT_TRUE(kshot_token_valid("Ab3_x-Z9"));
    TEST_ASSERT_TRUE(kshot_token_valid("a"));
    (void)memset(tok, 'k', KSHOT_TOKEN_MAX);
    tok[KSHOT_TOKEN_MAX] = '\0';
    TEST_ASSERT_TRUE(kshot_token_valid(tok));
    tok[KSHOT_TOKEN_MAX] = 'k';
    tok[KSHOT_TOKEN_MAX + 1U] = '\0';
    TEST_ASSERT_FALSE(kshot_token_valid(tok));

    TEST_ASSERT_FALSE(kshot_token_valid(""));
    TEST_ASSERT_FALSE(kshot_token_valid(NULL));
    /* Nothing that could leave the path or start a query. */
    TEST_ASSERT_FALSE(kshot_token_valid("a/b"));
    TEST_ASSERT_FALSE(kshot_token_valid("a%2F"));
    TEST_ASSERT_FALSE(kshot_token_valid(".."));
    TEST_ASSERT_FALSE(kshot_token_valid("a?b"));
    TEST_ASSERT_FALSE(kshot_token_valid("a#b"));
    TEST_ASSERT_FALSE(kshot_token_valid("a@b"));
    TEST_ASSERT_FALSE(kshot_token_valid("a b"));
    TEST_ASSERT_FALSE(kshot_token_valid("a\"b"));
    TEST_ASSERT_FALSE(kshot_token_valid("\xC3\xA9"));
}
