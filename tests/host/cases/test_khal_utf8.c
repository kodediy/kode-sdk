/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_utf8.c
 * @brief khal_utf8_valid(): which texts are well-formed UTF-8.
 */
#include <stdbool.h>
#include <stddef.h>

#include "unity.h"

#include "khal_utf8.h"

/** @brief One text and whether it is well-formed. */
typedef struct {
    const char *text;   /**< The bytes, NUL-terminated. */
    bool        valid;  /**< What khal_utf8_valid() must answer. */
    const char *what;   /**< Why, for the failure message. */
} utf8_row_t;

/** @brief The table: each kind of character, and each way one is malformed. */
static const utf8_row_t s_rows[] = {
    { "",                         true,  "empty" },
    { "cafe.txt",                 true,  "ASCII" },
    { "\x7F",                     true,  "the last one-byte character" },
    { "caf\xC3\xA9.txt",          true,  "two bytes: U+00E9" },
    { "\xC2\x80",                 true,  "two bytes: U+0080, the first" },
    { "\xDF\xBF",                 true,  "two bytes: U+07FF, the last" },
    { "\xE0\xA0\x80",             true,  "three bytes: U+0800, the first" },
    { "\xE2\x82\xAC",             true,  "three bytes: U+20AC" },
    { "\xED\x9F\xBF",             true,  "three bytes: U+D7FF, below the surrogates" },
    { "\xEE\x80\x80",             true,  "three bytes: U+E000, above the surrogates" },
    { "\xEF\xBF\xBF",             true,  "three bytes: U+FFFF, the last" },
    { "\xF0\x90\x80\x80",         true,  "four bytes: U+10000, the first" },
    { "\xF0\x9F\x98\x80",         true,  "four bytes: U+1F600" },
    { "\xF4\x8F\xBF\xBF",         true,  "four bytes: U+10FFFF, the last" },
    { "caf\xE9.txt",              false, "Latin-1: a lead byte with no continuation" },
    { "\x80",                     false, "a stray continuation byte" },
    { "a\xBF" "b",                false, "a continuation byte after ASCII" },
    { "\xC3",                     false, "two bytes, cut after the lead" },
    { "\xE2\x82",                 false, "three bytes, cut after the second" },
    { "\xF0\x9F\x98",             false, "four bytes, cut after the third" },
    { "\xE2\x82" "a",             false, "three bytes, ASCII where the third goes" },
    { "\xC0\xAF",                 false, "overlong: '/' in two bytes" },
    { "\xC1\xBF",                 false, "overlong: U+007F in two bytes" },
    { "\xE0\x80\xAF",             false, "overlong: '/' in three bytes" },
    { "\xE0\x9F\xBF",             false, "overlong: U+07FF in three bytes" },
    { "\xF0\x80\x80\xAF",         false, "overlong: '/' in four bytes" },
    { "\xF0\x8F\xBF\xBF",         false, "overlong: U+FFFF in four bytes" },
    { "\xED\xA0\x80",             false, "a surrogate: U+D800" },
    { "\xED\xBF\xBF",             false, "a surrogate: U+DFFF" },
    { "\xF4\x90\x80\x80",         false, "past U+10FFFF: U+110000" },
    { "\xF5\x80\x80\x80",         false, "a lead byte past U+10FFFF: 0xF5" },
    { "\xF8\x88\x80\x80\x80",     false, "a five-byte lead: 0xF8" },
    { "\xFE",                     false, "0xFE" },
    { "\xFF",                     false, "0xFF" },
    { "ok\xC3\xA9then\xE9",       false, "well-formed, then Latin-1 at the end" },
};

TEST_CASE("khal_utf8_valid takes every well-formed character and refuses each malformed one",
          "[khal_utf8]")
{
    for ( size_t i = 0U; i < (sizeof(s_rows) / sizeof(s_rows[0])); i++ )
    {
        TEST_ASSERT_EQUAL_MESSAGE(s_rows[i].valid, khal_utf8_valid(s_rows[i].text), s_rows[i].what);
    }
    TEST_ASSERT_FALSE(khal_utf8_valid(NULL));
}
