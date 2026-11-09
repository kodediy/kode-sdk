/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_ble_tokens.c
 * @brief The P4's bond store tokens and when a BLE key moves to the C5, without NimBLE.
 */
#include <string.h>

#include "unity.h"

#include "kdrv_ble_tokens.h"

static const uint8_t ADDR[6] = { 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };
static const uint8_t LTK[16] = { 0xA0U, 0xA1U, 0xA2U, 0xA3U, 0xA4U, 0xA5U, 0xA6U, 0xA7U,
                                 0xA8U, 0xA9U, 0xAAU, 0xABU, 0xACU, 0xADU, 0xAEU, 0xAFU };
static const uint8_t IRK[16] = { 0xB0U, 0xB1U, 0xB2U, 0xB3U, 0xB4U, 0xB5U, 0xB6U, 0xB7U,
                                 0xB8U, 0xB9U, 0xBAU, 0xBBU, 0xBCU, 0xBDU, 0xBEU, 0xBFU };

static unsigned      s_puts;
static uint8_t       s_tok[2][16];
static uint8_t       s_key[2][16];
static unsigned      s_fail_on;     /* 1-based put that fails; 0 = none */

static kode_status_t put(const uint8_t tok[16], const uint8_t key[16])
{
    if ( 2U > s_puts )
    {
        (void)memcpy(s_tok[s_puts], tok, 16U);
        (void)memcpy(s_key[s_puts], key, 16U);
    }
    s_puts++;
    if ( s_puts == s_fail_on )
    {
        return KODE_TIMEOUT;
    }
    return KODE_OK;
}

static void put_reset(unsigned fail_on)
{
    s_puts    = 0U;
    s_fail_on = fail_on;
    (void)memset(s_tok, 0, sizeof(s_tok));
    (void)memset(s_key, 0, sizeof(s_key));
}

TEST_CASE("a peer's real LTK and IRK go to the C5 and tokens stay", "[ble_tokens]")
{
    uint8_t ltk[16];
    uint8_t irk[16];
    uint8_t want[16];

    (void)memcpy(ltk, LTK, 16U);
    (void)memcpy(irk, IRK, 16U);
    put_reset(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kbt_tokenize(ltk, irk, KBT_ROLE_PEER, ADDR, 0U, &put));
    TEST_ASSERT_EQUAL_UINT(2U, s_puts);

    kbt_token(KODE_TOKEN_LTK, KBT_ROLE_PEER, ADDR, 0U, want);
    TEST_ASSERT_EQUAL_MEMORY(want, s_tok[0], 16U);
    TEST_ASSERT_EQUAL_MEMORY(LTK, s_key[0], 16U);
    TEST_ASSERT_EQUAL_MEMORY(want, ltk, 16U);
    kbt_token(KODE_TOKEN_IRK, KBT_ROLE_PEER, ADDR, 0U, want);
    TEST_ASSERT_EQUAL_MEMORY(want, s_tok[1], 16U);
    TEST_ASSERT_EQUAL_MEMORY(IRK, s_key[1], 16U);
    TEST_ASSERT_EQUAL_MEMORY(want, irk, 16U);
}

TEST_CASE("our LTK and the peer's LTK get different tokens", "[ble_tokens]")
{
    uint8_t ours[16];
    uint8_t theirs[16];

    kbt_token(KODE_TOKEN_LTK, KBT_ROLE_OUR, ADDR, 1U, ours);
    kbt_token(KODE_TOKEN_LTK, KBT_ROLE_PEER, ADDR, 1U, theirs);
    TEST_ASSERT_TRUE(memcmp(ours, theirs, 16U) != 0);
    TEST_ASSERT_EQUAL_MEMORY("KBTL", ours, 4U);
    TEST_ASSERT_EQUAL_MEMORY(ADDR, &ours[4], 6U);
    TEST_ASSERT_EQUAL_UINT8(1U, ours[10]);
}

TEST_CASE("a key that is already a token is not sent again", "[ble_tokens]")
{
    uint8_t ltk[16];

    kbt_token(KODE_TOKEN_LTK, KBT_ROLE_OUR, ADDR, 0U, ltk);
    put_reset(0U);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kbt_tokenize(ltk, NULL, KBT_ROLE_OUR, ADDR, 0U, &put));
    TEST_ASSERT_EQUAL_UINT(0U, s_puts);
}

TEST_CASE("a refused IRK leaves the LTK untouched too", "[ble_tokens]")
{
    uint8_t ltk[16];
    uint8_t irk[16];

    (void)memcpy(ltk, LTK, 16U);
    (void)memcpy(irk, IRK, 16U);
    put_reset(2U);
    TEST_ASSERT_EQUAL_INT(KODE_TIMEOUT, kbt_tokenize(ltk, irk, KBT_ROLE_PEER, ADDR, 0U, &put));
    TEST_ASSERT_EQUAL_MEMORY(LTK, ltk, 16U);
    TEST_ASSERT_EQUAL_MEMORY(IRK, irk, 16U);
}

TEST_CASE("kbt_is_token knows both magics and no real key", "[ble_tokens]")
{
    uint8_t t[16];

    kbt_token(KODE_TOKEN_LTK, KBT_ROLE_PEER, ADDR, 0U, t);
    TEST_ASSERT_TRUE(kbt_is_token(t));
    kbt_token(KODE_TOKEN_IRK, KBT_ROLE_PEER, ADDR, 0U, t);
    TEST_ASSERT_TRUE(kbt_is_token(t));
    TEST_ASSERT_FALSE(kbt_is_token(LTK));
    TEST_ASSERT_FALSE(kbt_is_token(NULL));
}

TEST_CASE("a bond is forgotten only when the key itself failed", "[ble_tokens]")
{
    /* 0x206 PIN or key missing, 0x23D MIC failure: ble_hs's HCI codes */
    TEST_ASSERT_TRUE(kbt_forget_on_enc_fail(0x206, true));
    TEST_ASSERT_TRUE(kbt_forget_on_enc_fail(0x23D, true));
    TEST_ASSERT_FALSE(kbt_forget_on_enc_fail(0x23D, false));   /* nothing to forget */
    TEST_ASSERT_FALSE(kbt_forget_on_enc_fail(13, true));       /* a timeout keeps it */
    TEST_ASSERT_FALSE(kbt_forget_on_enc_fail(0, true));
}

TEST_CASE("a bond goes only when the C5 says its key is not there", "[ble_tokens]")
{
    TEST_ASSERT_TRUE(kbt_c5_lost_key(KODE_OK, (uint8_t)KODE_ST_NOT_FOUND));
    /* The key is there: the peer, or someone spoofing it, failed. */
    TEST_ASSERT_FALSE(kbt_c5_lost_key(KODE_OK, (uint8_t)KODE_ST_OK));
    /* No clear answer keeps the bond. */
    TEST_ASSERT_FALSE(kbt_c5_lost_key(KODE_TIMEOUT, (uint8_t)KODE_ST_NOT_FOUND));
    TEST_ASSERT_FALSE(kbt_c5_lost_key(KODE_OK, (uint8_t)KODE_ST_BAD_REQUEST));
    TEST_ASSERT_FALSE(kbt_c5_lost_key(KODE_NO_RESOURCES, (uint8_t)KODE_ST_REFUSED));
}
