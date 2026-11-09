/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_identity.c
 * @brief The board's name, derived from its MAC.
 */
#include <string.h>

#include "unity.h"

#include "kbsp_identity.h"
#include "khal_identity.h"
#include "fakes.h"

/** @brief The bench board: e8:f6:0a:e7:6c:1d. */
static const uint8_t BENCH_MAC[6] = { 0xE8U, 0xF6U, 0x0AU, 0xE7U, 0x6CU, 0x1DU };

/* ---- The shape of every serial -------------------------------------------- */

TEST_CASE("a serial is the prefix, ten symbols and a check character",
          "[khal_identity]")
{
    char out[KHAL_ID_LEN];

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_identity_encode(BENCH_MAC, out, sizeof(out)));

    TEST_ASSERT_EQUAL_UINT(KHAL_ID_LEN - 1U, strlen(out));
    TEST_ASSERT_EQUAL_MEMORY(KBSP_ID_PREFIX, out, strlen(KBSP_ID_PREFIX));
}

/* The check symbol is Luhn mod 32 over the ten body symbols. These serials
 * are fixed: a change here renames boards in the field. */
TEST_CASE("fixed MACs encode to their known serials", "[khal_identity]")
{
    static const struct {
        uint8_t     mac[6];
        const char *want;
    } VECS[] = {
        { { 0xE8U, 0xF6U, 0x0AU, 0xE7U, 0x6CU, 0x1DU }, "KODE78YR5EEV0XS" },
        { { 0xE8U, 0xF6U, 0x0AU, 0xE7U, 0x6CU, 0x1EU }, "KODE78YR5EEV0YQ" },
        { { 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x01U }, "KODE0000000001Y" },
        { { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU }, "KODE7ZZZZZZZZZ2" },
    };
    char out[KHAL_ID_LEN];

    for ( size_t i = 0U; (sizeof(VECS) / sizeof(VECS[0])) > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(VECS[i].mac, out, sizeof(out)));
        TEST_ASSERT_EQUAL_STRING(VECS[i].want, out);
    }
}

/* A serial travels in URLs, topics and BLE names, so every symbol, the check
 * included, is a letter or a digit of the Crockford alphabet. */
TEST_CASE("every symbol of a serial is a Crockford letter or digit", "[khal_identity]")
{
    static const char ALPHABET[] = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    uint8_t mac[6] = { 0xE8U, 0xF6U, 0x0AU, 0xE7U, 0x00U, 0x00U };
    char    out[KHAL_ID_LEN];

    for ( uint32_t n = 0U; 4096U > n; ++n )
    {
        mac[4] = (uint8_t)(n >> 8U);
        mac[5] = (uint8_t)(n * 37U);
        TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(mac, out, sizeof(out)));
        for ( size_t c = strlen(KBSP_ID_PREFIX); (KHAL_ID_LEN - 1U) > c; ++c )
        {
            TEST_ASSERT_NOT_NULL(strchr(ALPHABET, out[c]));
        }
    }
}

/* Determinism is the whole product promise: the same board answers with the
 * same name for its whole life. */
TEST_CASE("the same MAC always encodes to the same serial", "[khal_identity]")
{
    char a[KHAL_ID_LEN];
    char b[KHAL_ID_LEN];

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(BENCH_MAC, a, sizeof(a)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(BENCH_MAC, b, sizeof(b)));
    TEST_ASSERT_EQUAL_STRING(a, b);
}

/* Distinctness is the other half: two boards must not share a name. */
TEST_CASE("different MACs encode to different serials", "[khal_identity]")
{
    char    seen[64][KHAL_ID_LEN];
    uint8_t mac[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

    for ( unsigned i = 0U; 64U > i; ++i )
    {
        /* Spread the changes across bytes so this is not just the low byte. */
        mac[0] = (uint8_t)(i * 3U);
        mac[3] = (uint8_t)(i * 7U);
        mac[5] = (uint8_t)i;
        TEST_ASSERT_EQUAL_INT(KODE_OK,
            khal_identity_encode(mac, seen[i], KHAL_ID_LEN));

        for ( unsigned j = 0U; j < i; ++j )
        {
            TEST_ASSERT_TRUE(strcmp(seen[i], seen[j]) != 0);
        }
    }
}

/* ---- The properties a human relies on ------------------------------------- */

/* Forty-eight bits into ten five-bit symbols leaves the top symbol carrying
 * three bits, so it can only ever be 0..7. The file states it; this makes it
 * a fact over the whole input range rather than over one example. */
TEST_CASE("the first symbol of the body is always 0 to 7", "[khal_identity]")
{
    char    out[KHAL_ID_LEN];
    uint8_t mac[6];

    for ( unsigned i = 0U; 256U > i; ++i )
    {
        mac[0] = (uint8_t)i;
        mac[1] = (uint8_t)(i * 5U);
        mac[2] = (uint8_t)(i * 11U);
        mac[3] = (uint8_t)(i * 17U);
        mac[4] = (uint8_t)(i * 23U);
        mac[5] = (uint8_t)(i * 29U);

        TEST_ASSERT_EQUAL_INT(KODE_OK,
            khal_identity_encode(mac, out, sizeof(out)));

        char const first = out[strlen(KBSP_ID_PREFIX)];
        TEST_ASSERT_TRUE(('0' <= first) && ('7' >= first));
    }
}

/* No I, L, O or U in the body: that is the entire reason for choosing
 * Crockford, and a serial that grew one would start being misread aloud. */
TEST_CASE("no body symbol can be mistaken for a digit", "[khal_identity]")
{
    static const char *const ALPHABET = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    char    out[KHAL_ID_LEN];
    uint8_t mac[6];

    for ( unsigned i = 0U; 256U > i; ++i )
    {
        mac[0] = (uint8_t)(i * 31U);
        mac[1] = (uint8_t)(i * 13U);
        mac[2] = (uint8_t)(i * 7U);
        mac[3] = (uint8_t)(i * 3U);
        mac[4] = (uint8_t)(i * 2U);
        mac[5] = (uint8_t)i;

        TEST_ASSERT_EQUAL_INT(KODE_OK,
            khal_identity_encode(mac, out, sizeof(out)));

        size_t const body = strlen(KBSP_ID_PREFIX);
        for ( size_t c = body; (body + 10U) > c; ++c )
        {
            TEST_ASSERT_NOT_NULL(strchr(ALPHABET, out[c]));
            TEST_ASSERT_TRUE('I' != out[c]);
            TEST_ASSERT_TRUE('L' != out[c]);
            TEST_ASSERT_TRUE('O' != out[c]);
            TEST_ASSERT_TRUE('U' != out[c]);
        }
    }
}

/* ---- The two ends of the range -------------------------------------------- */

TEST_CASE("an all-zero MAC encodes without special-casing", "[khal_identity]")
{
    static const uint8_t zero[6] = { 0U, 0U, 0U, 0U, 0U, 0U };
    char out[KHAL_ID_LEN];

    /* The encoder is total; judging a MAC is khal_identity_init()'s job, not
     * this function's, and the split is worth pinning because an unprovisioned
     * board reads as all zeros. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(zero, out, sizeof(out)));

    size_t const body = strlen(KBSP_ID_PREFIX);
    for ( size_t c = body; (body + 10U) > c; ++c )
    {
        TEST_ASSERT_EQUAL_CHAR('0', out[c]);
    }
    TEST_ASSERT_EQUAL_CHAR('0', out[body + 10U]);   /* Luhn mod 32 of zeros */
}

TEST_CASE("the largest MAC encodes without overflowing", "[khal_identity]")
{
    static const uint8_t max[6] = { 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU };
    char out[KHAL_ID_LEN];

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(max, out, sizeof(out)));

    /* 2^48-1 >> 45 is 7, the top of the three-bit first symbol. */
    TEST_ASSERT_EQUAL_CHAR('7', out[strlen(KBSP_ID_PREFIX)]);
    TEST_ASSERT_EQUAL_UINT(KHAL_ID_LEN - 1U, strlen(out));
}

/* The bytes are consumed mac[0] first, which is the opposite of NimBLE's
 * order. The file warns that the two must not be confused; this is what makes
 * the warning checkable. */
TEST_CASE("the byte order is load-bearing, not incidental", "[khal_identity]")
{
    static const uint8_t fwd[6] = { 0xE8U, 0xF6U, 0x0AU, 0xE7U, 0x6CU, 0x1DU };
    static const uint8_t rev[6] = { 0x1DU, 0x6CU, 0xE7U, 0x0AU, 0xF6U, 0xE8U };
    char a[KHAL_ID_LEN];
    char b[KHAL_ID_LEN];

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(fwd, a, sizeof(a)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(rev, b, sizeof(b)));
    TEST_ASSERT_TRUE(strcmp(a, b) != 0);
}

/* ---- Refusals ------------------------------------------------------------- */

/* The capacity boundary from both sides. One byte short must be refused, not
 * truncated: a serial missing its check symbol still looks like a serial. */
TEST_CASE("a buffer one byte too small is refused, not truncated",
          "[khal_identity]")
{
    char out[KHAL_ID_LEN];

    (void)memset(out, '?', sizeof(out));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_identity_encode(BENCH_MAC, out, KHAL_ID_LEN - 1U));

    /* Nothing was written on the way to refusing. */
    TEST_ASSERT_EQUAL_CHAR('?', out[0]);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_identity_encode(BENCH_MAC, out, KHAL_ID_LEN));
}

TEST_CASE("a null MAC or destination is refused", "[khal_identity]")
{
    char out[KHAL_ID_LEN];

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_identity_encode(NULL, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_identity_encode(BENCH_MAC, NULL, KHAL_ID_LEN));
}

/* ---- The header's own arithmetic ------------------------------------------ */

/* The pretty form groups the eleven symbols 4-4-3 behind the prefix, and that
 * grouping is the one thing in the file written as literals rather than
 * derived from ID_SYMBOLS. It therefore has to be checked from outside: strip
 * the separators and the canonical form must come back exactly. */
TEST_CASE("the printed form is the serial with three separators inserted",
          "[khal_identity]")
{
    TEST_ASSERT_EQUAL_UINT(KHAL_ID_LEN + 3U, KHAL_ID_PRETTY_LEN);

    /* 4 + 4 + 3 is the eleven symbols behind the prefix: ten body plus the
     * check character. A grouping that summed to anything else would either
     * overrun KHAL_ID_PRETTY_LEN or leave it short. */
    TEST_ASSERT_EQUAL_UINT(11U, 4U + 4U + 3U);
    TEST_ASSERT_EQUAL_UINT(11U, (KHAL_ID_LEN - 1U) - strlen(KBSP_ID_PREFIX));
}

/* ---- The name comes from the C5's MAC (Versa, 2026-09-29) ------------------ */

/** @brief 6c:0a's C5: the MAC factory-link holds as mac_c5. */
static const uint8_t C5_MAC[6] = { 0x38U, 0x44U, 0xBEU, 0x07U, 0xE6U, 0x54U };

TEST_CASE("a board that has never seen its C5 has no name", "[khal_identity]")
{
    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_identity_init());
    TEST_ASSERT_FALSE(khal_identity_valid());
    TEST_ASSERT_EQUAL_STRING("", khal_identity_serial());
    TEST_ASSERT_EQUAL_STRING("", khal_identity_pretty());
}

TEST_CASE("the C5's MAC names the board and survives a reboot through the cache",
          "[khal_identity]")
{
    char expect[KHAL_ID_LEN];
    uint8_t mac[6];

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(C5_MAC, expect, sizeof(expect)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_set_mac(C5_MAC));
    TEST_ASSERT_TRUE(khal_identity_valid());
    TEST_ASSERT_EQUAL_STRING(expect, khal_identity_serial());
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_mac(mac));
    TEST_ASSERT_EQUAL_UINT8_ARRAY(C5_MAC, mac, 6U);

    /* The next boot: no C5 yet, the cache alone. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_init());
    TEST_ASSERT_EQUAL_STRING(expect, khal_identity_serial());
}

TEST_CASE("an all-zero or missing MAC is refused and names nothing", "[khal_identity]")
{
    static const uint8_t zero[6] = { 0U, 0U, 0U, 0U, 0U, 0U };

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_identity_set_mac(zero));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_identity_set_mac(NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_identity_init());
    TEST_ASSERT_FALSE(khal_identity_valid());
}

TEST_CASE("a C5 replaced in repair renames the board", "[khal_identity]")
{
    static const uint8_t other[6] = { 0x38U, 0x44U, 0xBEU, 0x1CU, 0x1DU, 0x58U };
    char first[KHAL_ID_LEN];
    char second[KHAL_ID_LEN];

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_set_mac(C5_MAC));
    (void)strcpy(first, khal_identity_serial());
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_set_mac(other));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(other, second, sizeof(second)));
    TEST_ASSERT_EQUAL_STRING(second, khal_identity_serial());
    TEST_ASSERT_TRUE(strcmp(first, khal_identity_serial()) != 0);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_init());   /* the cache moved too */
    TEST_ASSERT_EQUAL_STRING(second, khal_identity_serial());
}

TEST_CASE("a cache that cannot be written still names the board this boot",
          "[khal_identity]")
{
    char expect[KHAL_ID_LEN];

    fake_nvs_reset();
    fake_nvs_fail_next_set();
    TEST_ASSERT_EQUAL_INT(KODE_IO_ERROR, khal_identity_set_mac(C5_MAC));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_identity_encode(C5_MAC, expect, sizeof(expect)));
    TEST_ASSERT_EQUAL_STRING(expect, khal_identity_serial());
}
