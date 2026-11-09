/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kapp_cat.c
 * @brief The category list: how it splits, matches, cleans up and stores.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kapp_cat.h"

/* ---- Splitting, matching, normalising ------------------------------------ */

TEST_CASE("case, spaces and duplicates all collapse", "[kapp_cat]")
{
    char out[KAPP_CATEGORY_MAX];

    kapp_cat_normalise("Game, game ,  Demo", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("game,demo", out);
}

TEST_CASE("empty items disappear rather than becoming categories", "[kapp_cat]")
{
    char out[KAPP_CATEGORY_MAX];

    kapp_cat_normalise("game,,,demo,", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("game,demo", out);
}

TEST_CASE("the cap drops the fifth item, not the first", "[kapp_cat]")
{
    char out[KAPP_CATEGORY_MAX];

    kapp_cat_normalise("a,b,c,d,e", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("a,b,c,d", out);
}

TEST_CASE("an over-long item goes and the rest of the list stands", "[kapp_cat]")
{
    char out[KAPP_CATEGORY_MAX];

    kapp_cat_normalise("game,0123456789abcdef,demo", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("game,demo", out);
}

TEST_CASE("a dropped item is reported, never dropped in silence", "[kapp_cat]")
{
    char out[KAPP_CATEGORY_MAX];

    kapp_cat_normalise("game,0123456789abcdef,demo", out, sizeof(out));
    TEST_ASSERT_GREATER_THAN_UINT(0U, fake_klog_count());
}

TEST_CASE("nothing at all is a valid answer, not a crash", "[kapp_cat]")
{
    char out[KAPP_CATEGORY_MAX];

    kapp_cat_normalise("", out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);

    kapp_cat_normalise(NULL, out, sizeof(out));
    TEST_ASSERT_EQUAL_STRING("", out);
}

TEST_CASE("matching is whole-item, so a prefix must not match", "[kapp_cat]")
{
    TEST_ASSERT_TRUE(kapp_cat_has("games,demo", "demo"));
    TEST_ASSERT_TRUE(kapp_cat_has("game", "game"));
    TEST_ASSERT_FALSE(kapp_cat_has("games,demo", "game"));
    TEST_ASSERT_FALSE(kapp_cat_has("", "game"));
    TEST_ASSERT_FALSE(kapp_cat_has("game,demo", ""));
}

TEST_CASE("the cursor walks a list and then stops", "[kapp_cat]")
{
    char        item[KAPP_CATEGORY_ITEM_MAX];
    const char *c = "game,demo";

    c = kapp_cat_next(c, item, sizeof(item));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("game", item);

    c = kapp_cat_next(c, item, sizeof(item));
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_STRING("demo", item);

    TEST_ASSERT_NULL(kapp_cat_next(c, item, sizeof(item)));
}

/* ---- The stored blob ----------------------------------------------------- */

/**
 * @brief Fill @p rows with two known assignments.
 * @param rows Receives the rows; must hold at least two.
 */
static void two_rows(kapp_cat_row_t *rows)
{
    (void)memset(rows, 0, 2U * sizeof(*rows));
    (void)snprintf(rows[0].project, sizeof(rows[0].project), "%s", "hello");
    (void)snprintf(rows[0].category, sizeof(rows[0].category), "%s", "game,demo");
    (void)snprintf(rows[1].project, sizeof(rows[1].project), "%s", "ir_nec");
    (void)snprintf(rows[1].category, sizeof(rows[1].category), "%s", "tool");
}

TEST_CASE("a stored table round-trips, and nothing else comes back", "[kapp_cat]")
{
    static uint8_t        blob[KAPP_CAT_BLOB_LEN];
    static kapp_cat_row_t rows[3];
    kapp_cat_row_t        in[2];
    uint8_t               count = 0U;
    size_t                n;

    two_rows(in);

    n = kapp_cat_serialise(in, 2U, blob, sizeof(blob));
    TEST_ASSERT_EQUAL_UINT(12U + (2U * sizeof(kapp_cat_row_t)), n);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_deserialise(blob, n, rows, &count));
    TEST_ASSERT_EQUAL_UINT8(2U, count);
    TEST_ASSERT_EQUAL_STRING("hello", rows[0].project);
    TEST_ASSERT_EQUAL_STRING("game,demo", rows[0].category);
    TEST_ASSERT_EQUAL_STRING("ir_nec", rows[1].project);
}

TEST_CASE("a wrong magic is not a table", "[kapp_cat]")
{
    static uint8_t        blob[KAPP_CAT_BLOB_LEN];
    static kapp_cat_row_t rows[3];
    kapp_cat_row_t        in[2];
    uint8_t               count = 0U;
    size_t                n;

    two_rows(in);
    n = kapp_cat_serialise(in, 2U, blob, sizeof(blob));
    blob[0] ^= 0xFFU;
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kapp_cat_deserialise(blob, n, rows, &count));
}

TEST_CASE("a count the bytes cannot back refuses, never half-loads", "[kapp_cat]")
{
    static uint8_t        blob[KAPP_CAT_BLOB_LEN];
    static kapp_cat_row_t rows[3];
    kapp_cat_row_t        in[2];
    uint8_t               count = 0U;
    size_t                n;

    two_rows(in);

    n = kapp_cat_serialise(in, 2U, blob, sizeof(blob));
    blob[8] = 9U;
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kapp_cat_deserialise(blob, n, rows, &count));

    n = kapp_cat_serialise(in, 2U, blob, sizeof(blob));
    blob[8] = (uint8_t)(KAPP_CAT_ROWS_MAX + 1U);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kapp_cat_deserialise(blob, n, rows, &count));
}

TEST_CASE("an empty table is a table, not an error", "[kapp_cat]")
{
    static uint8_t        blob[KAPP_CAT_BLOB_LEN];
    static kapp_cat_row_t rows[3];
    kapp_cat_row_t        in[2];
    uint8_t               count = 9U;
    size_t                n;

    two_rows(in);
    n = kapp_cat_serialise(in, 0U, blob, sizeof(blob));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_deserialise(blob, n, rows, &count));
    TEST_ASSERT_EQUAL_UINT8(0U, count);
}

TEST_CASE("a buffer too small writes no part of a table", "[kapp_cat]")
{
    static uint8_t blob[KAPP_CAT_BLOB_LEN];
    kapp_cat_row_t in[2];

    two_rows(in);
    TEST_ASSERT_EQUAL_UINT(0U, kapp_cat_serialise(in, 2U, blob, 16U));
}

/* ---- The live override table --------------------------------------------- */
/* Against the faked NVS, so what is under test here is the table's logic. The
 * on-target selftest covers the other half - that a row survives a real flash
 * write - which is a different assertion, not this one repeated. */

TEST_CASE("a set row resolves back, normalised", "[kapp_cat]")
{
    char cats[KAPP_CATEGORY_MAX];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_set("hello", "Game, demo"));
    TEST_ASSERT_TRUE(kapp_cat_resolve("hello", cats, sizeof(cats)));
    TEST_ASSERT_EQUAL_STRING("game,demo", cats);
}

TEST_CASE("setting again replaces rather than appending", "[kapp_cat]")
{
    char cats[KAPP_CATEGORY_MAX];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_set("hello", "game"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_set("hello", "tool"));
    TEST_ASSERT_TRUE(kapp_cat_resolve("hello", cats, sizeof(cats)));
    TEST_ASSERT_EQUAL_STRING("tool", cats);
    TEST_ASSERT_EQUAL_UINT8(1U, kapp_cat_count());
}

TEST_CASE("an app with no row resolves to nothing and says so", "[kapp_cat]")
{
    char cats[KAPP_CATEGORY_MAX];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    TEST_ASSERT_FALSE(kapp_cat_resolve("__no_such_app", cats, sizeof(cats)));
}

TEST_CASE("clearing removes the row, and clearing nothing succeeds", "[kapp_cat]")
{
    char cats[KAPP_CATEGORY_MAX];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_set("hello", "game"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_clear("hello"));
    TEST_ASSERT_FALSE(kapp_cat_resolve("hello", cats, sizeof(cats)));

    /* Missing is success: the postcondition holds either way. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_clear("hello"));
}

TEST_CASE("a list that cleans up to nothing is refused, not stored empty",
          "[kapp_cat]")
{
    char cats[KAPP_CATEGORY_MAX];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kapp_cat_set("hello", " , , "));
    TEST_ASSERT_FALSE(kapp_cat_resolve("hello", cats, sizeof(cats)));
}

/* ---- The table's size ------------------------------------------------------ */

/** @brief Rows the cases below fill the table with: the launcher's ceiling. */
#define CAT_FULL  64U

TEST_CASE("an override past the 24th is stored, up to the ceiling",
          "[kapp_cat]")
{
    char project[32];
    char cats[KAPP_CATEGORY_MAX];

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    for ( unsigned i = 0U; CAT_FULL > i; ++i )
    {
        (void)snprintf(project, sizeof(project), "app%02u", i);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kapp_cat_set(project, "tool"),
                                      project);
    }
    TEST_ASSERT_EQUAL_UINT8(CAT_FULL, kapp_cat_count());
    TEST_ASSERT_TRUE(kapp_cat_resolve("app24", cats, sizeof(cats)));
    TEST_ASSERT_EQUAL_STRING("tool", cats);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kapp_cat_set("one_too_many", "tool"));

    /* Read back from NVS, so the whole table survived the write. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    TEST_ASSERT_EQUAL_UINT8(CAT_FULL, kapp_cat_count());
}

TEST_CASE("a table stored with 24 rows still loads", "[kapp_cat]")
{
    static uint8_t        blob[KAPP_CAT_BLOB_LEN];
    static kapp_cat_row_t rows[24];
    char                  cats[KAPP_CATEGORY_MAX];

    (void)memset(rows, 0, sizeof(rows));
    for ( unsigned i = 0U; 24U > i; ++i )
    {
        (void)snprintf(rows[i].project, sizeof(rows[i].project), "old%02u", i);
        (void)snprintf(rows[i].category, sizeof(rows[i].category), "%s", "game");
    }
    size_t const n = kapp_cat_serialise(rows, 24U, blob, sizeof(blob));
    TEST_ASSERT_EQUAL_UINT(12U + (24U * sizeof(kapp_cat_row_t)), n);
    fake_nvs_seed("kode", "app_cats", blob, n);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kapp_cat_init());
    TEST_ASSERT_EQUAL_UINT8(24U, kapp_cat_count());
    TEST_ASSERT_TRUE(kapp_cat_resolve("old23", cats, sizeof(cats)));
    TEST_ASSERT_EQUAL_STRING("game", cats);
}
