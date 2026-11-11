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
