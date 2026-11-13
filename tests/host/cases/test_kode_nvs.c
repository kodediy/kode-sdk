/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_nvs.c
 * @brief The generic store, from the surface an app writes against.
 */
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "fakes.h"
#include "kaos.h"
#include "khal_nvs.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp.h"
#include "qf_support.h"

/* ---- Round trips --------------------------------------------------------- */

TEST_CASE("a value written through the facade comes back", "[kode_nvs]")
{
    char out[64];

    fake_nvs_reset();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_nvs_set("pat", "ghp_notarealtoken"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nvs_get("pat", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ghp_notarealtoken", out);
}

TEST_CASE("a second write replaces the first", "[kode_nvs]")
{
    char out[64];

    fake_nvs_reset();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_nvs_set("netid", "first"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_nvs_set("netid", "second"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nvs_get("netid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("second", out);
}

/* ---- Where it lands ------------------------------------------------------
 *
 * The namespace is the contract: it is what the console verbs write into and
 * what an app reads out of, across two different firmwares that never run at
 * the same time. Pinned here rather than left to whatever the facade happens
 * to pass down.
 */

TEST_CASE("a written value lands in the store's own namespace", "[kode_nvs]")
{
    char out[64];

    fake_nvs_reset();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_nvs_set("netid", "dot-07"));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(KODE_NVS_NAMESPACE, "netid", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("dot-07", out);
}

/* kodeOS keeps its own settings in `kset` and its network list in `kwifi`,
 * each key owned by exactly one AO. An operator typing a key that collides
 * with one of those must reach the store and nothing else. */
TEST_CASE("the store cannot reach the settings namespace", "[kode_nvs]")
{
    char out[64];

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str("kset", "bright", "100"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_nvs_set("bright", "hijacked"));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str("kset", "bright", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("100", out);
}

/* ---- Absence -------------------------------------------------------------
 *
 * The same distinction the seam underneath makes: denied means "nothing was
 * provisioned", which is what a first-boot app must be able to tell apart
 * from a store that is broken.
 */

TEST_CASE("a key that was never written is denied, not an error", "[kode_nvs]")
{
    char out[8];

    fake_nvs_reset();
    (void)memcpy(out, "keepme", 7U);

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_get("absent", out, sizeof(out)));
    /* And the caller's buffer is left alone, so its own default stands. */
    TEST_ASSERT_EQUAL_STRING("keepme", out);
}

/* ---- Refusals ------------------------------------------------------------ */

/* NVS stores at most 15 characters of key. Refusing here rather than letting
 * the backend decide is what makes the ceiling the same on every backend, and
 * what stops `personal_access_token` from becoming a silent second key. */
TEST_CASE("a key longer than the store allows is refused, not truncated",
          "[kode_nvs]")
{
    char longest[KODE_SYS_NVS_KEY_MAX];
    char toolong[KODE_SYS_NVS_KEY_MAX + 1U];

    fake_nvs_reset();

    (void)memset(longest, 'k', sizeof(longest) - 1U);
    longest[sizeof(longest) - 1U] = '\0';
    (void)memset(toolong, 'k', sizeof(toolong) - 1U);
    toolong[sizeof(toolong) - 1U] = '\0';

    TEST_ASSERT_EQUAL_INT(KODE_OK,    kode_block_nvs_set(longest, "fits"));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_nvs_set(toolong, "does not"));
}

TEST_CASE("a value longer than the store allows is refused, not truncated",
          "[kode_nvs]")
{
    char longest[KODE_SYS_NVS_VALUE_MAX];
    char toolong[KODE_SYS_NVS_VALUE_MAX + 1U];

    fake_nvs_reset();

    (void)memset(longest, 'v', sizeof(longest) - 1U);
    longest[sizeof(longest) - 1U] = '\0';
    (void)memset(toolong, 'v', sizeof(toolong) - 1U);
    toolong[sizeof(toolong) - 1U] = '\0';

    TEST_ASSERT_EQUAL_INT(KODE_OK,    kode_block_nvs_set("fits", longest));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_nvs_set("nope", toolong));
}

/* An empty key is not a key. NVS would take it and nothing could ever name it
 * again from a console line, where the token would simply be missing. */
TEST_CASE("an empty key is refused", "[kode_nvs]")
{
    char out[64];

    fake_nvs_reset();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_nvs_set("", "orphan"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_get("", out, sizeof(out)));
}

/* An empty value, unlike an empty key, is a value: clearing a token to the
 * empty string is a thing an operator may mean, and it is not the same as
 * deleting the key. */
TEST_CASE("an empty value is stored, not refused", "[kode_nvs]")
{
    char out[64];

    fake_nvs_reset();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_block_nvs_set("pat", ""));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nvs_get("pat", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("a null key, value or destination is refused", "[kode_nvs]")
{
    char out[64];

    fake_nvs_reset();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_nvs_set(NULL, "v"));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kode_block_nvs_set("k", NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_get(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_get("k", NULL, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_get("k", out, 0U));
}

/* ---- The set an Active Object asks for -----------------------------------
 *
 * kode_sys_nvs_set() posts the pair to AO_Boot, so what it refuses it must
 * refuse before anything is posted: a pair the store would not take never
 * costs AO_Boot an event.
 */

/** @brief How many events wait in AO_Boot's queue. */
static unsigned boot_waiting(void)
{
    return (unsigned)uxQueueMessagesWaiting(AO_Boot->eQueue);
}

TEST_CASE("an unusable key or value is refused before anything is posted",
          "[kode_nvs]")
{
    char key_long[KODE_SYS_NVS_KEY_MAX + 1U];
    char value_long[KODE_SYS_NVS_VALUE_MAX + 1U];
    const QEvt *e = NULL;

    qf_support_init();
    qf_support_margin_queue(AO_Boot);
    (void)memset(key_long, 'k', sizeof(key_long) - 1U);
    key_long[sizeof(key_long) - 1U] = '\0';
    (void)memset(value_long, 'v', sizeof(value_long) - 1U);
    value_long[sizeof(value_long) - 1U] = '\0';

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_set(NULL, "v"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_set("", "v"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_set(key_long, "v"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_set("k", NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_nvs_set("k", value_long));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, boot_waiting(), "nothing was posted");

    /* The longest pair the store takes is posted whole. */
    key_long[sizeof(key_long) - 2U]     = '\0';
    value_long[sizeof(value_long) - 2U] = '\0';
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_nvs_set(key_long, value_long));
    TEST_ASSERT_EQUAL_UINT(1U, boot_waiting());
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Boot->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT((int)BOOT_NVS_SET_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_STRING(key_long, Q_EVT_CAST(BootNvsSetEvt)->key);
    TEST_ASSERT_EQUAL_STRING(value_long, Q_EVT_CAST(BootNvsSetEvt)->value);
    QF_gc(e);
}
