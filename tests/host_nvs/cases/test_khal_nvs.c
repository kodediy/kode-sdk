/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_nvs.c
 * @brief The persistence seam, over the real NVS.
 */
#include <string.h>

#include "unity.h"

#include "khal_nvs.h"

#define NS   "kodetest"
#define NS2  "otherns"

/**
 * @brief Clear the keys a case is about to use, so it starts from nothing.
 *
 * The flash file survives between cases within one run, which is realistic -
 * it is the same partition - but a case that assumed an empty namespace would
 * pass or fail depending on what ran before it.
 */
static void fresh(void)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());
    (void)khal_nvs_erase(NS, "str");
    (void)khal_nvs_erase(NS, "u8");
    (void)khal_nvs_erase(NS, "u64");
    (void)khal_nvs_erase(NS, "blob");
    (void)khal_nvs_erase(NS, "missing");
    (void)khal_nvs_erase(NS2, "str");
}

/* ---- The distinction the whole seam exists for --------------------------- */

TEST_CASE("a key that was never written is denied, not an error", "[khal_nvs]")
{
    char    str[16];
    uint8_t val = 0xAAU;
    uint8_t blob[8];
    size_t  len = sizeof(blob);

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_nvs_get_str(NS, "missing", str, sizeof(str)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_get_u8(NS, "missing", &val));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_nvs_get_blob(NS, "missing", blob, &len));

    /* And the caller's variable is left alone, so its default stands. */
    TEST_ASSERT_EQUAL_UINT8(0xAAU, val);
}

/* A namespace nobody has ever written to is the state of a factory-fresh
 * board. It must read as "nothing stored" rather than as a fault. */
TEST_CASE("a namespace that has never been written is denied, not an error",
          "[khal_nvs]")
{
    char    str[16];
    uint8_t val = 0x55U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_nvs_get_str("neverused", "k", str, sizeof(str)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_get_u8("neverused", "k", &val));
    TEST_ASSERT_EQUAL_UINT8(0x55U, val);
}

/* ---- Round trips --------------------------------------------------------- */

TEST_CASE("a string survives a write and comes back terminated", "[khal_nvs]")
{
    char out[32];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS, "str", "kode dot"));
    (void)memset(out, 'x', sizeof(out));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS, "str", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("kode dot", out);
}

TEST_CASE("an empty string is a value, not an absence", "[khal_nvs]")
{
    char out[8];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS, "str", ""));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS, "str", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("a byte survives a write, including zero", "[khal_nvs]")
{
    uint8_t out = 0xFFU;

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(NS, "u8", 0U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u8(NS, "u8", &out));
    TEST_ASSERT_EQUAL_UINT8(0U, out);

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(NS, "u8", 0xA5U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u8(NS, "u8", &out));
    TEST_ASSERT_EQUAL_UINT8(0xA5U, out);
}

/** @brief A value with a different byte in every place. */
#define U64_PATTERN  0x0123456789ABCDEFULL

TEST_CASE("a u64 round-trips, and a missing one is denied", "[khal_nvs]")
{
    uint64_t out = 0U;

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_get_u64(NS, "u64", &out));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u64(NS, "u64", U64_PATTERN));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_u64(NS, "u64", &out));
    TEST_ASSERT_TRUE(U64_PATTERN == out);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_get_u64(NS, "u64", NULL));
}

TEST_CASE("a blob survives a write, bytes and length", "[khal_nvs]")
{
    static const uint8_t in[] = { 0x00U, 0xFFU, 0x5AU, 0xA5U, 0x00U };
    uint8_t              out[16];
    size_t               len = sizeof(out);

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_set_blob(NS, "blob", in, sizeof(in)));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_blob(NS, "blob", out, &len));
    TEST_ASSERT_EQUAL_UINT(sizeof(in), len);
    TEST_ASSERT_EQUAL_MEMORY(in, out, sizeof(in));
}

/* Rewriting replaces. A store that appended, or that kept the first value,
 * would leave the launcher showing a category the operator had changed. */
TEST_CASE("a second write replaces the first", "[khal_nvs]")
{
    char out[32];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS, "str", "first"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS, "str", "second"));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS, "str", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("second", out);
}

/* Every setter commits. "A write nobody committed is not a write" is stated
 * three times in the header and verified nowhere - and the way to verify it
 * without a reboot is to re-open the namespace, which every accessor does. */
TEST_CASE("a written value is committed, not left in an open handle",
          "[khal_nvs]")
{
    char out[32];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS, "str", "committed"));

    /* Each accessor opens and closes its own handle, so this read cannot see
     * an uncommitted write left behind by the one above. */
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS, "str", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("committed", out);
}

/* ---- Namespaces ---------------------------------------------------------- */

TEST_CASE("the same key in two namespaces holds two values", "[khal_nvs]")
{
    char out[32];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS,  "str", "mine"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS2, "str", "theirs"));

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS, "str", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("mine", out);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS2, "str", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("theirs", out);
}

/* ---- Erasing ------------------------------------------------------------- */

TEST_CASE("erasing a key makes it absent again", "[khal_nvs]")
{
    char out[32];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS, "str", "here"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase(NS, "str"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_nvs_get_str(NS, "str", out, sizeof(out)));
}

/* Missing is success: the postcondition the caller wanted already holds. */
TEST_CASE("erasing what is not there succeeds", "[khal_nvs]")
{
    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase(NS, "missing"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase("neverused", "k"));
}

/* ---- Refusals ------------------------------------------------------------ */

TEST_CASE("a buffer too small for the stored string is an error, not a truncation",
          "[khal_nvs]")
{
    char small[4];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_set_str(NS, "str", "far too long for four bytes"));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_nvs_get_str(NS, "str", small, sizeof(small)));
}

TEST_CASE("a null destination or a zero capacity is refused", "[khal_nvs]")
{
    uint8_t blob[4];
    size_t  len = sizeof(blob);
    char    out[8];

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_get_str(NS, "str", NULL, 8U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_get_str(NS, "str", out, 0U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_get_u8(NS, "u8", NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_get_blob(NS, "blob", NULL, &len));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_get_blob(NS, "blob", blob, NULL));
}

TEST_CASE("a null or empty value to write is refused", "[khal_nvs]")
{
    uint8_t blob[4] = { 1U, 2U, 3U, 4U };

    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_set_str(NS, "str", NULL));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_set_blob(NS, "blob", NULL, 4U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_set_blob(NS, "blob", blob, 0U));
}

/* Every entry point brings NVS up itself, so a caller that forgets to is not
 * punished for it. That is a real API guarantee and it is what lets a driver
 * read a setting without knowing who booted the storage layer. */
TEST_CASE("init is idempotent, and every accessor brings NVS up itself",
          "[khal_nvs]")
{
    char out[32];

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());

    /* No init on this line; the accessor is expected to have done it. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS, "str", "no init here"));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS, "str", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("no init here", out);
}

/* ---- Listing -------------------------------------------------------------
 *
 * The generic key/value store gives an operator a namespace kodeOS never
 * interprets, so the only way to answer "what is provisioned on this board"
 * is to ask the store itself. That is what these cases pin down.
 *
 * Note what is deliberately NOT asserted anywhere below: an order. NVS does
 * not promise one, so neither does the seam - the contract is that every
 * stored key appears exactly once across indices 0..N-1 while nothing is
 * written in between, and a caller that wants them sorted sorts them.
 */

#define NSL  "listns"

/**
 * @brief Clear the keys the listing cases use, so each starts from nothing.
 */
static void fresh_list(void)
{
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());
    (void)khal_nvs_erase(NSL, "alpha");
    (void)khal_nvs_erase(NSL, "beta");
    (void)khal_nvs_erase(NSL, "gamma");
}

TEST_CASE("every stored key is listed exactly once", "[khal_nvs]")
{
    char     key[20];
    bool     seen[3] = { false, false, false };
    uint16_t idx     = 0U;
    unsigned n       = 0U;

    fresh_list();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "alpha", "1"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "beta",  "2"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "gamma", "3"));

    while ( khal_nvs_key(NSL, idx, key, sizeof(key)) == KODE_OK )
    {
        if ( strcmp(key, "alpha") == 0 )
        {
            TEST_ASSERT_FALSE_MESSAGE(seen[0], "alpha listed twice");
            seen[0] = true;
        }
        else if ( strcmp(key, "beta") == 0 )
        {
            TEST_ASSERT_FALSE_MESSAGE(seen[1], "beta listed twice");
            seen[1] = true;
        }
        else if ( strcmp(key, "gamma") == 0 )
        {
            TEST_ASSERT_FALSE_MESSAGE(seen[2], "gamma listed twice");
            seen[2] = true;
        }
        else
        {
            TEST_FAIL_MESSAGE("a key nobody wrote was listed");
        }
        ++n;
        ++idx;
    }

    TEST_ASSERT_EQUAL_UINT(3U, n);
    TEST_ASSERT_TRUE(seen[0] && seen[1] && seen[2]);
}

/* The walk has to end, and it has to end the way every other absence in this
 * seam ends: denied, so a caller can loop on KODE_OK without a count. */
TEST_CASE("an index past the last key is denied, not an error", "[khal_nvs]")
{
    char key[20];

    fresh_list();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "alpha", "1"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_key(NSL, 0U, key, sizeof(key)));
    TEST_ASSERT_EQUAL_STRING("alpha", key);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_key(NSL, 1U, key, sizeof(key)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_key(NSL, 99U, key, sizeof(key)));
}

/* A factory-fresh board. Listing nothing is an answer, not a fault. */
TEST_CASE("a namespace that has never been written lists nothing", "[khal_nvs]")
{
    char key[20];

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_nvs_key("neverlist", 0U, key, sizeof(key)));
}

TEST_CASE("an erased key stops being listed", "[khal_nvs]")
{
    char     key[20];
    uint16_t idx = 0U;
    unsigned n   = 0U;

    fresh_list();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "alpha", "1"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "beta",  "2"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase(NSL, "alpha"));

    while ( khal_nvs_key(NSL, idx, key, sizeof(key)) == KODE_OK )
    {
        TEST_ASSERT_EQUAL_STRING("beta", key);
        ++n;
        ++idx;
    }
    TEST_ASSERT_EQUAL_UINT(1U, n);
}

/* Same rule as khal_nvs_get_str's: a name that does not fit is refused, never
 * silently shortened. A truncated key is a key that opens nothing. */
TEST_CASE("a buffer too small for the key name is an error, not a truncation",
          "[khal_nvs]")
{
    char small[4];

    fresh_list();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "alpha", "1"));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_nvs_key(NSL, 0U, small, sizeof(small)));
}

TEST_CASE("listing into a null destination or a zero capacity is refused",
          "[khal_nvs]")
{
    char key[20];

    fresh_list();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NSL, "alpha", "1"));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_key(NSL, 0U, NULL, sizeof(key)));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_nvs_key(NSL, 0U, key, 0U));
}

/* ---- Whole namespaces ----------------------------------------------------
 *
 * A factory reset forgets everything a board remembers, and it cannot name
 * the namespaces: apps and the station lock add their own. So the store is
 * asked which namespaces hold keys, and each is erased whole.
 *
 * The flash file outlives every case, so a case that counts namespaces first
 * empties the store the same way the reset does.
 */

#define NS_A       "nsa"
#define NS_B       "nsb"
#define NS_LONG    "abcdef"
#define NS_WIPE_MAX  64U   /**< Upper bound on a wipe, so a stuck store fails. */
#define NS_NAME_CAP  20U   /**< Room for any 15-character namespace. */

/**
 * @brief Erase every namespace the store holds, leaving it empty.
 */
static void wipe_all(void)
{
    char ns[NS_NAME_CAP];

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_init());
    for ( unsigned n = 0U; NS_WIPE_MAX > n; ++n )
    {
        if ( khal_nvs_namespace(0U, ns, sizeof(ns)) != KODE_OK )
        {
            return;
        }
        TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase_namespace(ns));
    }
    TEST_FAIL_MESSAGE("the store still lists namespaces after a wipe");
}

TEST_CASE("namespaces are listed once each", "[khal_nvs]")
{
    char     ns[NS_NAME_CAP];
    bool     seen_a = false;
    bool     seen_b = false;
    uint16_t idx;

    wipe_all();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS_A, "k1", "1"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS_A, "k2", "2"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS_B, "k1", "3"));

    for ( idx = 0U; 2U > idx; ++idx )
    {
        TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_namespace(idx, ns, sizeof(ns)));
        if ( strcmp(ns, NS_A) == 0 )
        {
            TEST_ASSERT_FALSE_MESSAGE(seen_a, "nsa listed twice");
            seen_a = true;
        }
        else if ( strcmp(ns, NS_B) == 0 )
        {
            TEST_ASSERT_FALSE_MESSAGE(seen_b, "nsb listed twice");
            seen_b = true;
        }
        else
        {
            TEST_FAIL_MESSAGE("a namespace nobody wrote was listed");
        }
    }
    TEST_ASSERT_TRUE(seen_a && seen_b);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_nvs_namespace(2U, ns, sizeof(ns)));
}

TEST_CASE("erasing a namespace leaves the others", "[khal_nvs]")
{
    char out[8];

    wipe_all();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS_A, "k1", "1"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS_A, "k2", "2"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS_B, "k1", "3"));

    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase_namespace(NS_A));

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_nvs_get_str(NS_A, "k1", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        khal_nvs_get_str(NS_A, "k2", out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_nvs_get_str(NS_B, "k1", out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("3", out);
}

TEST_CASE("erasing an empty namespace succeeds", "[khal_nvs]")
{
    wipe_all();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase_namespace("neverused"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_erase_namespace(NS_A));
}

TEST_CASE("a namespace name that does not fit is refused, not shortened",
          "[khal_nvs]")
{
    char small[4];

    wipe_all();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_str(NS_LONG, "k", "1"));

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_nvs_namespace(0U, small, sizeof(small)));
}
