/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_config.c
 * @brief kode_config_post() and kode_config_answer(): a request to its owner and the answer back.
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kqp.h"
#include "kregistry.h"
#include "qf_support.h"

/** @brief The sequence number the request in these cases carries. */
#define ANSWER_TEST_SEQ  41U

/** @brief Room for a value one character past the longest the event holds. */
#define ANSWER_TEST_BUF  (KODE_CONFIG_VALUE_MAX + 2U)

/**
 * @brief Bring the spy up, subscribed to the answers, with nothing waiting.
 *
 * @return The spy, standing in for the owner that answers.
 */
static QActive *answer_spy(void)
{
    qf_support_init();
    ao_spy_drain();
    ao_spy_subscribe(CONFIG_ANS_SIG);
    return ao_spy();
}

/** @brief A request as the console would have published it. */
static KodeConfigReqEvt request(void)
{
    KodeConfigReqEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(CONFIG_REQ_SIG);
    r.seq   = ANSWER_TEST_SEQ;
    (void)snprintf(r.key, sizeof(r.key), "%s", "mode");
    return r;
}

/* The answer is matched to its question by the sequence number, and the
 * console reads status, stored and value off it: all four must arrive as the
 * owner gave them. */
TEST_CASE("an answer carries the request's sequence, the status, the stored "
          "flag and the value", "[kode_config]")
{
    KodeConfigReqEvt  r = request();
    QActive * const   owner = answer_spy();
    const KodeConfigAnsEvt *a;

    TEST_ASSERT_TRUE(kode_config_answer(owner, &r, KODE_OK, true, "slow"));

    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    a = (const KodeConfigAnsEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)CONFIG_ANS_SIG, (uint32_t)a->super.sig);
    TEST_ASSERT_EQUAL_UINT32(ANSWER_TEST_SEQ, (uint32_t)a->seq);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, a->status);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_STRING("slow", a->value);
    TEST_ASSERT_EQUAL_STRING("mode", a->key);

    ao_spy_drain();
}

/* A refusal travels in the same event: the status is the owner's, and the
 * value is whatever stands, so the console can say both. */
TEST_CASE("a refusal is published with its status and the value that stands",
          "[kode_config]")
{
    KodeConfigReqEvt  r = request();
    QActive * const   owner = answer_spy();
    const KodeConfigAnsEvt *a;

    TEST_ASSERT_TRUE(kode_config_answer(owner, &r, KODE_DENIED, false, "24"));

    a = (const KodeConfigAnsEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, a->status);
    TEST_ASSERT_FALSE(a->stored);
    TEST_ASSERT_EQUAL_STRING("24", a->value);

    ao_spy_drain();
}

/* A value with nothing to say is an empty word, not a crash and not a stale
 * buffer. */
TEST_CASE("a NULL value is published as an empty word", "[kode_config]")
{
    KodeConfigReqEvt  r = request();
    QActive * const   owner = answer_spy();
    const KodeConfigAnsEvt *a;

    TEST_ASSERT_TRUE(kode_config_answer(owner, &r, KODE_OK, false, NULL));

    a = (const KodeConfigAnsEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("", a->value);

    ao_spy_drain();
}

/* The longest value the event holds is the boundary: it fits, whole and
 * terminated. */
TEST_CASE("a value of exactly the longest length is published whole",
          "[kode_config]")
{
    KodeConfigReqEvt  r = request();
    QActive * const   owner = answer_spy();
    char              value[ANSWER_TEST_BUF];
    const KodeConfigAnsEvt *a;

    (void)memset(value, 'v', sizeof(value));
    value[KODE_CONFIG_VALUE_MAX] = '\0';

    TEST_ASSERT_TRUE(kode_config_answer(owner, &r, KODE_OK, true, value));

    a = (const KodeConfigAnsEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT(KODE_CONFIG_VALUE_MAX, strlen(a->value));
    TEST_ASSERT_EQUAL_STRING(value, a->value);

    ao_spy_drain();
}

/* One character more would be cut short, and a cut value read back is a
 * different value: the door refuses it and publishes nothing, which is the
 * console's timeout rather than a lie. */
TEST_CASE("a value longer than the event holds is refused and nothing is "
          "published", "[kode_config]")
{
    KodeConfigReqEvt  r = request();
    QActive * const   owner = answer_spy();
    char              value[ANSWER_TEST_BUF];

    (void)memset(value, 'v', sizeof(value));
    value[KODE_CONFIG_VALUE_MAX + 1U] = '\0';

    TEST_ASSERT_FALSE(kode_config_answer(owner, &r, KODE_OK, true, value));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    ao_spy_drain();
}

/* An owner handed no request has nobody to answer: refuse, do not read it. */
TEST_CASE("a NULL request is refused and nothing is published", "[kode_config]")
{
    QActive * const owner = answer_spy();

    TEST_ASSERT_FALSE(kode_config_answer(owner, NULL, KODE_OK, true, "x"));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    ao_spy_drain();
}

/* ---- kode_config_post() ------------------------------------------------- */

/** @brief The table the posting cases register: one key, answered by the spy. */
static const kode_config_t POST_CONFIG[] = {
    { .name = "mode", .type = KODE_TYPE_BOOL },
};

/**
 * @brief A request for @p key, from the pool as the console makes one.
 * @param key The setting.
 * @return The event; the case fails when the pool is empty.
 */
static KodeConfigReqEvt *post_request(const char *key)
{
    KodeConfigReqEvt * const e = Q_NEW_X(KodeConfigReqEvt, 1U, CONFIG_REQ_SIG);

    TEST_ASSERT_NOT_NULL(e);
    (void)memset(&e->seq, 0, sizeof(*e) - offsetof(KodeConfigReqEvt, seq));
    e->seq = ANSWER_TEST_SEQ;
    (void)snprintf(e->key, sizeof(e->key), "%s", key);
    return e;
}

/* The owner is the one the table was registered with, and the request lands
 * in its queue and in no other. */
TEST_CASE("a request is posted to its key's owner", "[kode_config]")
{
    const QEvt *e;

    (void)answer_spy();
    kconfig_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_CONFIG_ADD(ao_spy(), KODE_CONFIG_MARGIN, POST_CONFIG));

    TEST_ASSERT_TRUE(kode_config_post(post_request("mode"), NULL));
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());
    e = ao_spy_take();
    TEST_ASSERT_EQUAL_UINT32((uint32_t)CONFIG_REQ_SIG, (uint32_t)e->sig);
    TEST_ASSERT_EQUAL_STRING("mode", ((const KodeConfigReqEvt *)e)->key);

    ao_spy_drain();
    kconfig_reset();
}

/* A key no table holds has nobody to post to; a NULL request has nothing to
 * post. Neither reaches a queue. */
TEST_CASE("a key no table holds, or no request, is posted nowhere", "[kode_config]")
{
    (void)answer_spy();
    kconfig_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_CONFIG_ADD(ao_spy(), KODE_CONFIG_MARGIN, POST_CONFIG));

    TEST_ASSERT_FALSE(kode_config_post(post_request("nosuch"), NULL));
    TEST_ASSERT_FALSE(kode_config_post(NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    ao_spy_drain();
    kconfig_reset();
}

/* The margin is the owner's: with all but one slot of its queue kept, a
 * request goes only into an empty queue, and the next is refused rather
 * than asserted on. */
TEST_CASE("a request the owner's margin has no room for is refused", "[kode_config]")
{
    (void)answer_spy();
    kconfig_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_CONFIG_ADD(ao_spy(), AO_SPY_QUEUE_LEN - 1U, POST_CONFIG));

    TEST_ASSERT_TRUE(kode_config_post(post_request("mode"), NULL));
    TEST_ASSERT_FALSE(kode_config_post(post_request("mode"), NULL));
    TEST_ASSERT_EQUAL_UINT(1U, ao_spy_count());

    ao_spy_drain();
    kconfig_reset();
}
