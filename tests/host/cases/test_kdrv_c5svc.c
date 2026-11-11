/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_c5svc.c
 * @brief The KODE transport: a request goes out framed, its answer reaches
 *        the service that asked, and nothing else does.
 */
#include <string.h>

#include "unity.h"
#include "fakes.h"
#include "kdrv_c5svc.h"
#include "kode_wire.h"

static uint8_t s_st;
static uint8_t s_op;
static size_t  s_len;
static uint8_t s_buf[64];
static int     s_calls;

static void on_rsp(uint8_t svc, uint8_t op, uint8_t st, const uint8_t *d,
                   size_t n, void *ctx)
{
    (void)svc;
    (void)ctx;
    s_op = op;
    s_st = st;
    s_len = n;
    s_calls++;
    if ( sizeof(s_buf) >= n )
    {
        (void)memcpy(s_buf, d, n);
    }
}

/** @brief A fresh transport with the link up and nobody listening. */
static void fresh(void)
{
    s_st = 0xFFU;
    s_op = 0U;
    s_len = 0U;
    s_calls = 0;
    fake_radio_set_link(true);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_c5svc_init());
    kdrv_c5svc_forget();
    for ( uint8_t s = 0U; KODE_SVC_COUNT > s; s++ )
    {
        (void)kdrv_c5svc_listen(s, NULL, NULL);
    }
}

/** @brief The C5's answer to the request in flight. */
static void answer(uint8_t svc, uint8_t op, uint8_t seq, uint8_t st,
                   const void *p, uint16_t n)
{
    uint8_t m[sizeof(kode_hdr_t) + 64U];
    kode_hdr_t h = { KODE_WIRE_VERSION, svc, op, seq, st, 0U, n };
    (void)memcpy(m, &h, sizeof(h));
    if ( 0U < n )
    {
        (void)memcpy(&m[sizeof(h)], p, n);
    }
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(h) + n);
}

static const kode_hdr_t *sent(void)
{
    return (const kode_hdr_t *)fake_peer_last_sent(NULL);
}

TEST_CASE("a request goes out with header, service, op and payload", "[c5svc]")
{
    fresh();
    kode_vault_ssid_t s;
    (void)memset(&s, 0, sizeof(s));
    (void)strcpy(s.ssid, "home");
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_FORGET, &s, sizeof(s)));
    size_t n = 0U;
    const uint8_t *m = fake_peer_last_sent(&n);
    TEST_ASSERT_EQUAL_HEX32(KODE_MSG_REQ, fake_peer_last_msg_id());
    TEST_ASSERT_EQUAL_size_t(sizeof(kode_hdr_t) + sizeof(s), n);
    TEST_ASSERT_EQUAL_UINT8(KODE_WIRE_VERSION, sent()->version);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_VAULT, sent()->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_FORGET, sent()->op);
    TEST_ASSERT_EQUAL_UINT16(sizeof(s), sent()->len);
    TEST_ASSERT_EQUAL_STRING("home", (const char *)&m[sizeof(kode_hdr_t)]);
}

TEST_CASE("the answer reaches the service's listener", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_VAULT, on_rsp, NULL);
    (void)kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_WIPE, NULL, 0U);
    static const uint8_t pay[3] = { 1U, 2U, 3U };
    answer(KODE_SVC_VAULT, KODE_VAULT_WIPE, sent()->seq, KODE_ST_OK, pay, 3U);
    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, s_st);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_WIPE, s_op);
    TEST_ASSERT_EQUAL_size_t(3U, s_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(pay, s_buf, 3U);
}

TEST_CASE("an answer for another service reaches only that one", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_ID, on_rsp, NULL);
    (void)kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_WIPE, NULL, 0U);
    answer(KODE_SVC_VAULT, KODE_VAULT_WIPE, sent()->seq, KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

TEST_CASE("an answer with a stale seq is dropped", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_VAULT, on_rsp, NULL);
    (void)kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_WIPE, NULL, 0U);
    answer(KODE_SVC_VAULT, KODE_VAULT_WIPE, (uint8_t)(sent()->seq + 1U),
           KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

TEST_CASE("an answer from another wire version is dropped", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_VAULT, on_rsp, NULL);
    (void)kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_WIPE, NULL, 0U);
    kode_hdr_t bad = { (uint8_t)(KODE_WIRE_VERSION + 1U), KODE_SVC_VAULT,
                       KODE_VAULT_WIPE, sent()->seq, 0U, 0U, 0U };
    fake_peer_deliver(KODE_MSG_RSP, (const uint8_t *)&bad, sizeof(bad));
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

TEST_CASE("an answer whose length disagrees with its header is dropped", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_VAULT, on_rsp, NULL);
    (void)kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_WIPE, NULL, 0U);
    uint8_t m[sizeof(kode_hdr_t) + 2U];
    kode_hdr_t h = { KODE_WIRE_VERSION, KODE_SVC_VAULT, KODE_VAULT_WIPE,
                     sent()->seq, 0U, 0U, 5U };   /* says 5, carries 2 */
    (void)memcpy(m, &h, sizeof(h));
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(m));
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}

TEST_CASE("a second request for a busy service is refused", "[c5svc]")
{
    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_ID, KODE_ID_STATE, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_c5svc_send(KODE_SVC_ID, KODE_ID_STATE, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_LIST, NULL, 0U));
    kdrv_c5svc_abandon(KODE_SVC_ID);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_ID, KODE_ID_STATE, NULL, 0U));
}

TEST_CASE("link down refuses at once and leaves nothing in flight", "[c5svc]")
{
    fresh();
    fake_radio_set_link(false);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_LIST, NULL, 0U));
    fake_radio_set_link(true);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_VAULT, KODE_VAULT_LIST, NULL, 0U));
}

TEST_CASE("bad arguments are refused", "[c5svc]")
{
    fresh();
    static uint8_t big[KODE_MAX_PAYLOAD + 1U];
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_c5svc_send(KODE_SVC_COUNT, 1U, NULL, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_c5svc_send(KODE_SVC_VAULT, 1U, big, sizeof(big)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_c5svc_send(KODE_SVC_VAULT, 1U, NULL, 4U));
}

TEST_CASE("a blocking call with no answer times out and frees the service", "[c5svc]")
{
    uint8_t st = 0xFFU;
    size_t n = 0U;
    fresh();
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT,
        kdrv_c5svc_call(KODE_SVC_BTKEYS, KODE_BTKEYS_WIPE, NULL, 0U,
                        &st, NULL, 0U, &n, 50U));
    TEST_ASSERT_EQUAL_UINT32(50U, fake_sem_last_wait_ms());
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_BTKEYS, KODE_BTKEYS_WIPE, NULL, 0U));
}

/* The C5 answering while the caller sleeps in khal_sem_wait_timeout(). */
static void c5_answers_during_wait(void)
{
    static const uint8_t pay[2] = { 0xABU, 0xCDU };
    answer(KODE_SVC_BTKEYS, KODE_BTKEYS_PUT, sent()->seq, KODE_ST_OK, pay, 2U);
}

TEST_CASE("a blocking call returns the answer copied out", "[c5svc]")
{
    uint8_t st = 0xFFU;
    uint8_t out[4] = { 0U };
    size_t n = 0U;
    fresh();
    fake_sem_wait_hook(c5_answers_during_wait);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_call(KODE_SVC_BTKEYS, KODE_BTKEYS_PUT, NULL, 0U,
                        &st, out, sizeof(out), &n, 3000U));
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, st);
    TEST_ASSERT_EQUAL_size_t(2U, n);
    TEST_ASSERT_EQUAL_HEX8(0xABU, out[0]);
    TEST_ASSERT_EQUAL_HEX8(0xCDU, out[1]);
}

TEST_CASE("the last INFO is kept for whoever reads it, until the link is forgotten", "[c5svc]")
{
    kode_info_t got;
    fresh();
    TEST_ASSERT_FALSE(kdrv_c5svc_info(&got));
    (void)kdrv_c5svc_send(KODE_SVC_SELF, KODE_SELF_INFO, NULL, 0U);
    kode_info_t info;
    (void)memset(&info, 0, sizeof(info));
    info.wire_version = KODE_WIRE_VERSION;
    info.vault_count  = 3U;
    info.heap_low     = 45000U;
    answer(KODE_SVC_SELF, KODE_SELF_INFO, sent()->seq, KODE_ST_OK,
           &info, sizeof(info));
    TEST_ASSERT_TRUE(kdrv_c5svc_info(&got));
    TEST_ASSERT_EQUAL_UINT8(3U, got.vault_count);
    TEST_ASSERT_EQUAL_UINT32(45000U, got.heap_low);
    kdrv_c5svc_forget();
    TEST_ASSERT_FALSE(kdrv_c5svc_info(&got));
}

/** @brief One KODE_SNIFF_EVT_BATCH frame: an empty batch header, no records. */
static void event_batch(uint8_t version)
{
    uint8_t m[sizeof(kode_hdr_t) + sizeof(kode_sniff_batch_t)];
    kode_hdr_t h = { version, KODE_SVC_SNIFF, KODE_SNIFF_EVT_BATCH, 0U,
                     KODE_ST_OK, 0U, (uint16_t)sizeof(kode_sniff_batch_t) };
    (void)memset(m, 0, sizeof(m));
    (void)memcpy(m, &h, sizeof(h));
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(m));
}

/** @brief Whether any captured log line contains @p needle. */
static bool logged_text(const char *needle)
{
    for ( uint32_t i = 0U; fake_klog_count() > i; i++ )
    {
        const char *const l = fake_klog_line(i);
        if ( (NULL != l) && (NULL != strstr(l, needle)) )
        {
            return true;
        }
    }
    return false;
}

TEST_CASE("an event reaches the listener with nothing in flight", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_SNIFF, on_rsp, NULL);
    event_batch(KODE_WIRE_VERSION);
    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_EVT_BATCH, s_op);
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_OK, s_st);
    TEST_ASSERT_EQUAL_size_t(sizeof(kode_sniff_batch_t), s_len);
}

TEST_CASE("an event with no listener is dropped quietly", "[c5svc]")
{
    fresh();
    fake_klog_reset();
    event_batch(KODE_WIRE_VERSION);
    TEST_ASSERT_EQUAL_INT(0, s_calls);
    TEST_ASSERT_FALSE(logged_text("late answer"));
}

TEST_CASE("an event does not complete a request in flight", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_SNIFF, on_rsp, NULL);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_SNIFF, KODE_SNIFF_STATE, NULL, 0U));
    event_batch(KODE_WIRE_VERSION);
    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_EVT_BATCH, s_op);
    /* The request is still out: a second one is refused. */
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        kdrv_c5svc_send(KODE_SVC_SNIFF, KODE_SNIFF_STATE, NULL, 0U));
    answer(KODE_SVC_SNIFF, KODE_SNIFF_STATE, sent()->seq, KODE_ST_OK, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(2, s_calls);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STATE, s_op);
    /* The answer completed it: the service takes a request again. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        kdrv_c5svc_send(KODE_SVC_SNIFF, KODE_SNIFF_STATE, NULL, 0U));
}

TEST_CASE("a bad-request answer is delivered", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_SNIFF, on_rsp, NULL);
    (void)kdrv_c5svc_send(KODE_SVC_SNIFF, KODE_SNIFF_STATE, NULL, 0U);
    answer(KODE_SVC_SNIFF, KODE_SNIFF_STATE, sent()->seq,
           KODE_ST_BAD_REQUEST, NULL, 0U);
    TEST_ASSERT_EQUAL_INT(1, s_calls);
    TEST_ASSERT_EQUAL_UINT8(KODE_ST_BAD_REQUEST, s_st);
    TEST_ASSERT_EQUAL_size_t(0U, s_len);
}

TEST_CASE("an event of another wire version is dropped", "[c5svc]")
{
    fresh();
    (void)kdrv_c5svc_listen(KODE_SVC_SNIFF, on_rsp, NULL);
    event_batch(2U);
    TEST_ASSERT_EQUAL_INT(0, s_calls);
}
