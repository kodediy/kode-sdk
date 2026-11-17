/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kapp_data.c
 * @brief The app.json subset reader: what it accepts, what it refuses, and
 *        that a refusal leaves nothing behind.
 */
#include "unity.h"
#include "kapp_data.h"
#include <string.h>

static const char FULL[] =
    "{ \"kode\": \"1\", \"name\": \"Scope\", \"category\": \"Tools, Lab\","
    " \"version\": \"0.4\", \"description\": \"Two-channel oscilloscope.\","
    " \"icon\": \"images/scope.k64\", \"color\": \"#8E8E93\" }";

TEST_CASE("a full app.json fills every field", "[kapp_data]")
{
    kapp_data_t d;
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_OK, kapp_data_parse(FULL, strlen(FULL), &d));
    TEST_ASSERT_EQUAL_STRING("Scope", d.name);
    TEST_ASSERT_EQUAL_STRING("tools,lab", d.category);   /* normalised */
    TEST_ASSERT_EQUAL_STRING("0.4", d.version);
    TEST_ASSERT_EQUAL_STRING("Two-channel oscilloscope.", d.description);
    TEST_ASSERT_EQUAL_STRING("images/scope.k64", d.icon);
    TEST_ASSERT_TRUE(d.color_valid);
    TEST_ASSERT_EQUAL_HEX8(0x8E, d.color[0]);
    TEST_ASSERT_EQUAL_HEX8(0x93, d.color[2]);
}

TEST_CASE("kode is the only required key", "[kapp_data]")
{
    kapp_data_t d;
    const char only[] = "{\"kode\":\"1\"}";
    const char none[] = "{\"name\":\"Scope\"}";
    const char two[]  = "{\"kode\":\"2\"}";
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_OK, kapp_data_parse(only, strlen(only), &d));
    TEST_ASSERT_EQUAL_STRING("", d.name);
    TEST_ASSERT_FALSE(d.color_valid);
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_REVISION, kapp_data_parse(none, strlen(none), &d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_REVISION, kapp_data_parse(two, strlen(two), &d));
}

TEST_CASE("outside the subset is a syntax refusal that zeroes the output", "[kapp_data]")
{
    kapp_data_t d;
    const char nested[] = "{\"kode\":\"1\",\"x\":{\"y\":\"z\"}}";
    const char number[] = "{\"kode\":\"1\",\"version\":4}";
    const char array[]  = "{\"kode\":\"1\",\"category\":[\"a\"]}";
    const char open[]   = "{\"kode\":\"1\",\"name\":\"Sco";
    (void)memset(&d, 0x55, sizeof(d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(nested, strlen(nested), &d));
    TEST_ASSERT_EQUAL_STRING("", d.name);
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(number, strlen(number), &d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(array, strlen(array), &d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(open, strlen(open), &d));
}

TEST_CASE("a field over its limit is refused, not cut", "[kapp_data]")
{
    kapp_data_t d;
    char big[128];
    (void)snprintf(big, sizeof(big), "{\"kode\":\"1\",\"name\":\"%s\"}",
                   "abcdefghijklmnopqrstuvwxyzabcdef");   /* 32 chars: one over */
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(big, strlen(big), &d));
}

TEST_CASE("the icon path is relative and never climbs", "[kapp_data]")
{
    kapp_data_t d;
    const char up[]  = "{\"kode\":\"1\",\"icon\":\"../x.k64\"}";
    const char abs[] = "{\"kode\":\"1\",\"icon\":\"/storage/x.k64\"}";
    const char bs[]  = "{\"kode\":\"1\",\"icon\":\"a\\\\b.k64\"}";
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(up, strlen(up), &d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(abs, strlen(abs), &d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(bs, strlen(bs), &d));
}

TEST_CASE("color is exactly #RRGGBB", "[kapp_data]")
{
    kapp_data_t d;
    const char shrt[] = "{\"kode\":\"1\",\"color\":\"#FFF\"}";
    const char bad[]  = "{\"kode\":\"1\",\"color\":\"#GG0000\"}";
    const char ok[]   = "{\"kode\":\"1\",\"color\":\"#ffac00\"}";
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(shrt, strlen(shrt), &d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, kapp_data_parse(bad, strlen(bad), &d));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_OK, kapp_data_parse(ok, strlen(ok), &d));
    TEST_ASSERT_EQUAL_HEX8(0xAC, d.color[1]);
}

TEST_CASE("unknown keys and the two escapes pass", "[kapp_data]")
{
    kapp_data_t d;
    const char t[] = "{\"kode\":\"1\",\"author\":\"x\",\"name\":\"Say \\\"hi\\\" \\\\ ok\"}";
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_OK, kapp_data_parse(t, strlen(t), &d));
    TEST_ASSERT_EQUAL_STRING("Say \"hi\" \\ ok", d.name);
}

TEST_CASE("a file over 1 KB is refused before parsing", "[kapp_data]")
{
    kapp_data_t d;
    static char big[KAPP_DATA_JSON_MAX + 1U];
    (void)memset(big, ' ', sizeof(big));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SIZE, kapp_data_parse(big, sizeof(big), &d));
}

/* ---- An app.json fetched ahead of its folder -------------------------------
 * An app's data may reach the board before the app, read from a URL. The file
 * says whether an icon is to be fetched beside it, and it can only name the
 * one file a folder's writers store. */

TEST_CASE("a fetched app.json with no icon key is the whole folder", "[kapp_data]")
{
    const char         t[] = "{\"kode\":\"1\",\"name\":\"Snake\",\"color\":\"#30D158\"}";
    kapp_json_status_t why = KAPP_JSON_NONE;

    TEST_ASSERT_EQUAL_INT(KAPP_FETCH_NO_ICON, kapp_data_fetched(t, strlen(t), &why));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_OK, why);
}

TEST_CASE("a fetched app.json that names icon.k64 asks for the icon", "[kapp_data]")
{
    const char         t[] = "{\"kode\":\"1\",\"icon\":\"icon.k64\"}";
    kapp_json_status_t why = KAPP_JSON_NONE;

    TEST_ASSERT_EQUAL_INT(KAPP_FETCH_ICON, kapp_data_fetched(t, strlen(t), &why));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_OK, why);
}

TEST_CASE("a fetched app.json that names another icon is refused", "[kapp_data]")
{
    const char         png[]  = "{\"kode\":\"1\",\"icon\":\"icon.png\"}";
    const char         deep[] = "{\"kode\":\"1\",\"icon\":\"art/icon.k64\"}";
    kapp_json_status_t why    = KAPP_JSON_NONE;

    TEST_ASSERT_EQUAL_INT(KAPP_FETCH_REFUSED, kapp_data_fetched(png, strlen(png), &why));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KAPP_JSON_OK, why,
        "the file is read; only its icon's name is not one a folder stores");
    TEST_ASSERT_EQUAL_INT(KAPP_FETCH_REFUSED, kapp_data_fetched(deep, strlen(deep), NULL));
}

TEST_CASE("a fetched app.json the reader would refuse is refused, with its status",
          "[kapp_data]")
{
    const char         rev[] = "{\"kode\":\"2\",\"icon\":\"icon.k64\"}";
    const char         syn[] = "{\"kode\":\"1\",\"icon\":\"icon.k64\"";
    kapp_json_status_t why   = KAPP_JSON_OK;

    TEST_ASSERT_EQUAL_INT(KAPP_FETCH_REFUSED, kapp_data_fetched(rev, strlen(rev), &why));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_REVISION, why);
    TEST_ASSERT_EQUAL_INT(KAPP_FETCH_REFUSED, kapp_data_fetched(syn, strlen(syn), &why));
    TEST_ASSERT_EQUAL_INT(KAPP_JSON_SYNTAX, why);
}

/* ---- The icon: two sizes on the media, one in memory -------------------------- */

/** @brief The 128x128 block an icon is read into. */
static uint8_t s_icon[KAPP_ICON_BYTES];

/** @brief The colour a 64 px icon holds at (@p x, @p y): its two bytes are told apart. */
static uint16_t half_colour(uint32_t x, uint32_t y)
{
    return (uint16_t)(0x1234U + (y * 251U) + (x * 7U));
}

/** @brief The alpha a 64 px icon holds at (@p x, @p y). */
static uint8_t half_alpha(uint32_t x, uint32_t y)
{
    return (uint8_t)(0x5AU ^ ((y * 13U) + x));
}

/** @brief Lay a 64 px icon at the head of s_icon, as a file read leaves it. */
static void put_half_icon(void)
{
    uint32_t const plane = (uint32_t)KAPP_ICON_HALF_W * KAPP_ICON_HALF_H * 2U;

    (void)memset(s_icon, 0xEE, sizeof(s_icon));   /* what the read did not reach */
    for ( uint32_t y = 0U; KAPP_ICON_HALF_H > y; ++y )
    {
        for ( uint32_t x = 0U; KAPP_ICON_HALF_W > x; ++x )
        {
            uint32_t const at = (y * KAPP_ICON_HALF_W) + x;
            uint16_t const c  = half_colour(x, y);

            s_icon[at * 2U]        = (uint8_t)(c & 0xFFU);
            s_icon[(at * 2U) + 1U] = (uint8_t)(c >> 8);
            s_icon[plane + at]     = half_alpha(x, y);
        }
    }
}

TEST_CASE("the two icon sizes are named and tie to each other", "[kapp_data][icon]")
{
    TEST_ASSERT_EQUAL_UINT32(49152U, KAPP_ICON_BYTES);
    TEST_ASSERT_EQUAL_UINT32(12288U, KAPP_ICON_HALF_BYTES);
    TEST_ASSERT_EQUAL_UINT32(128U, KAPP_ICON_W);
    TEST_ASSERT_EQUAL_UINT32(64U, KAPP_ICON_HALF_W);
    TEST_ASSERT_TRUE(kapp_icon_len_ok(KAPP_ICON_BYTES));
    TEST_ASSERT_TRUE(kapp_icon_len_ok(KAPP_ICON_HALF_BYTES));
    TEST_ASSERT_FALSE(kapp_icon_len_ok(0U));
    TEST_ASSERT_FALSE(kapp_icon_len_ok(KAPP_ICON_HALF_BYTES - 1U));
    TEST_ASSERT_FALSE(kapp_icon_len_ok(KAPP_ICON_HALF_BYTES + 1U));
    TEST_ASSERT_FALSE(kapp_icon_len_ok(KAPP_ICON_BYTES - 1U));
    TEST_ASSERT_FALSE(kapp_icon_len_ok(KAPP_ICON_BYTES + 1U));
}

TEST_CASE("a 128 px icon comes through byte for byte", "[kapp_data][icon]")
{
    static uint8_t want[KAPP_ICON_BYTES];

    for ( size_t i = 0U; sizeof(s_icon) > i; ++i )
    {
        s_icon[i] = (uint8_t)((i * 31U) + (i >> 8));
    }
    (void)memcpy(want, s_icon, sizeof(want));

    TEST_ASSERT_TRUE(kapp_icon_widen(s_icon, KAPP_ICON_BYTES));
    TEST_ASSERT_EQUAL_MEMORY(want, s_icon, sizeof(want));
}

TEST_CASE("a 64 px icon is brought up to 128 px by doubling each pixel",
          "[kapp_data][icon]")
{
    uint32_t const plane = (uint32_t)KAPP_ICON_W * KAPP_ICON_H * 2U;

    put_half_icon();

    TEST_ASSERT_TRUE(kapp_icon_widen(s_icon, KAPP_ICON_HALF_BYTES));

    for ( uint32_t y = 0U; KAPP_ICON_H > y; ++y )
    {
        for ( uint32_t x = 0U; KAPP_ICON_W > x; ++x )
        {
            uint32_t const at = (y * KAPP_ICON_W) + x;
            uint16_t const c  = half_colour(x / 2U, y / 2U);

            TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(c & 0xFFU), s_icon[at * 2U],
                                           "colour, low byte");
            TEST_ASSERT_EQUAL_HEX8_MESSAGE((uint8_t)(c >> 8), s_icon[(at * 2U) + 1U],
                                           "colour, high byte");
            TEST_ASSERT_EQUAL_HEX8_MESSAGE(half_alpha(x / 2U, y / 2U),
                                           s_icon[plane + at], "alpha");
        }
    }
}

TEST_CASE("any other icon length is refused and the block left as it was",
          "[kapp_data][icon]")
{
    static uint8_t want[KAPP_ICON_BYTES];
    static const size_t BAD[] = { 0U, 1U, KAPP_ICON_HALF_BYTES - 1U,
                                  KAPP_ICON_HALF_BYTES + 1U, KAPP_ICON_BYTES - 1U,
                                  KAPP_ICON_BYTES + 1U };

    put_half_icon();
    (void)memcpy(want, s_icon, sizeof(want));
    for ( size_t i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        TEST_ASSERT_FALSE(kapp_icon_widen(s_icon, BAD[i]));
        TEST_ASSERT_EQUAL_MEMORY(want, s_icon, sizeof(want));
    }
}
