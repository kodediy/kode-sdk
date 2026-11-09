/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_grammar.c
 * @brief The console's grammar: how a line splits into tokens and how quoted strings are read.
 */
#include <string.h>

#include "unity.h"

#include "console_grammar.h"

/* ---- Splitting a line ------------------------------------------------------ */

/* The ordinary case is unchanged by quoting: bare words, split on spaces
 * and tabs, as many of either as the line happens to carry. */
TEST_CASE("bare tokens split on spaces and tabs", "[console_grammar]")
{
    char  line[] = "  led_rgb \t rgb=0,0,200\tms=10  ";
    char *cur    = line;
    char *tok    = NULL;

    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("led_rgb", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("rgb=0,0,200", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("ms=10", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_END, console_token(&cur, &tok));
}

/* The reason quoting exists: everything between the quotes is one value,
 * separators, equals signs and symbols included. */
TEST_CASE("a quoted value is one token, spaces and symbols included",
          "[console_grammar]")
{
    char  line[] = "wifi_join ssid=\"Casa 5G\" pass=\"p@ss w=rd!\t#\" x=1";
    char *cur    = line;
    char *tok    = NULL;

    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("wifi_join", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("ssid=\"Casa 5G\"", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("pass=\"p@ss w=rd!\t#\"", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("x=1", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_END, console_token(&cur, &tok));
}

/* An escaped quote does not close the value, and an escaped backslash does
 * not escape the quote after it. */
TEST_CASE("an escaped quote stays inside the value", "[console_grammar]")
{
    char  line[] = "k=\"say \\\"hi\\\" \\\\\" next=\"x\"";
    char *cur    = line;
    char *tok    = NULL;

    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("k=\"say \\\"hi\\\" \\\\\"", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("next=\"x\"", tok);
}

/* `k=""` is a value - the empty one - and not the start of something that
 * swallows the rest of the line. */
TEST_CASE("an empty quoted value is one token", "[console_grammar]")
{
    char  line[] = "write text=\"\" path=\"a\"";
    char *cur    = line;
    char *tok    = NULL;

    (void)console_token(&cur, &tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("text=\"\"", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("path=\"a\"", tok);
}

/* A quote that never closes is refused, naming the key and nothing else:
 * the value may be a password, and a refusal is not where it should end up. */
TEST_CASE("an unterminated quote is refused, naming only its key",
          "[console_grammar]")
{
    char  line[] = "nvs_set key=\"k\" value=\"hunter2 and more";
    char *cur    = line;
    char *tok    = NULL;

    (void)console_token(&cur, &tok);
    (void)console_token(&cur, &tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_BAD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("value", tok);
}

/* A backslash right before the end of the line escapes nothing - there is
 * nothing left - so the quote before it was never closed. */
TEST_CASE("a trailing escaped quote leaves the value open", "[console_grammar]")
{
    char  line[] = "k=\"abc\\\"";
    char *cur    = line;
    char *tok    = NULL;

    TEST_ASSERT_EQUAL(CONSOLE_TOK_BAD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("k", tok);
}

/* `name="a"b` is not a value followed by more of it: a closing quote ends
 * the value, and anything glued to it is a typo, refused rather than read. */
TEST_CASE("bytes glued to a closing quote are refused", "[console_grammar]")
{
    char  line[] = "app_run name=\"a\"b";
    char *cur    = line;
    char *tok    = NULL;

    (void)console_token(&cur, &tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_BAD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("name", tok);
}

/* A quote opens a value only right after the `=`. Anywhere else it is an
 * ordinary byte, and the line splits exactly as it would without it. */
TEST_CASE("a quote in the middle of a bare value opens nothing",
          "[console_grammar]")
{
    char  line[] = "k=ab\"c d\"";
    char *cur    = line;
    char *tok    = NULL;

    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("k=ab\"c", tok);
    TEST_ASSERT_EQUAL(CONSOLE_TOK_WORD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("d\"", tok);
}

/* ---- Reading a quoted value ------------------------------------------------ */

TEST_CASE("unquoting keeps the bytes between the quotes", "[console_grammar]")
{
    char val[] = "\"Casa 5G = home!\"";

    TEST_ASSERT_TRUE(console_unquote(val));
    TEST_ASSERT_EQUAL_STRING("Casa 5G = home!", val);
}

/* The three escapes, and only those three. */
TEST_CASE("unquoting reads the three escapes", "[console_grammar]")
{
    char val[] = "\"a\\\"b\\\\c\\nd\\\"\"";

    TEST_ASSERT_TRUE(console_unquote(val));
    TEST_ASSERT_EQUAL_STRING("a\"b\\c\nd\"", val);
}

/* Any other backslash pair is no escape: the value is malformed, the token is
 * refused naming its key, and unquoting leaves it as it came. */
TEST_CASE("an unknown escape makes the value malformed", "[console_grammar]")
{
    char  line[] = "k=\"a\\qb\" next=1";
    char *cur    = line;
    char *tok    = NULL;
    char  val[]  = "\"a\\tb\"";

    TEST_ASSERT_EQUAL_INT(CONSOLE_TOK_BAD, console_token(&cur, &tok));
    TEST_ASSERT_EQUAL_STRING("k", tok);

    TEST_ASSERT_FALSE(console_unquote(val));
    TEST_ASSERT_EQUAL_STRING("\"a\\tb\"", val);
}

TEST_CASE("an empty quoted value unquotes to the empty string",
          "[console_grammar]")
{
    char val[] = "\"\"";

    TEST_ASSERT_TRUE(console_unquote(val));
    TEST_ASSERT_EQUAL_STRING("", val);
}

/* A bare value is not a quoted one, and is handed back untouched: which form
 * arrived is what the type check is about. */
TEST_CASE("a bare value does not unquote", "[console_grammar]")
{
    char bare[] = "kaos.boot";
    char open[] = "\"abc";
    char glue[] = "\"a\"b";

    TEST_ASSERT_FALSE(console_unquote(bare));
    TEST_ASSERT_EQUAL_STRING("kaos.boot", bare);
    TEST_ASSERT_FALSE(console_unquote(open));
    TEST_ASSERT_EQUAL_STRING("\"abc", open);
    TEST_ASSERT_FALSE(console_unquote(glue));
    TEST_ASSERT_EQUAL_STRING("\"a\"b", glue);
}

/* ---- Writing a quoted value ------------------------------------------------ */

TEST_CASE("quoting wraps a value and escapes the three bytes",
          "[console_grammar]")
{
    char dst[CONSOLE_QUOTED_MAX(16U)];

    TEST_ASSERT_EQUAL_STRING("\"my notes.txt\"",
                             console_quote(dst, sizeof(dst), "my notes.txt"));
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\\nd\"",
                             console_quote(dst, sizeof(dst), "a\"b\\c\nd"));
    TEST_ASSERT_EQUAL_STRING("\"\"", console_quote(dst, sizeof(dst), ""));
    TEST_ASSERT_EQUAL_STRING("\"\"", console_quote(dst, sizeof(dst), NULL));
}

/* The promise the output side makes: whatever the console prints can be
 * typed straight back. Quote, then unquote, and the bytes are the same. */
TEST_CASE("a quoted value unquotes to exactly what was quoted",
          "[console_grammar]")
{
    static const char NASTY[] = "\"\\\\n\\\" = \t\n\\x \"";
    char              buf[CONSOLE_QUOTED_MAX(sizeof(NASTY))];

    (void)console_quote(buf, sizeof(buf), NASTY);
    TEST_ASSERT_TRUE(console_unquote(buf));
    TEST_ASSERT_EQUAL_STRING(NASTY, buf);
}

/* Too long for the buffer: the tail goes, the closing quote does not, and
 * an escape is never split into a lone backslash that would eat it. */
TEST_CASE("quoting into a short buffer cuts the tail, never the quote",
          "[console_grammar]")
{
    char dst[7];

    TEST_ASSERT_EQUAL_STRING("\"abcd\"", console_quote(dst, sizeof(dst),
                                                         "abcdefgh"));
    TEST_ASSERT_EQUAL_STRING("\"abc\"", console_quote(dst, sizeof(dst),
                                                        "abc\"def"));
    TEST_ASSERT_TRUE(console_unquote(dst));
}

/* What a caller checks before trusting a value went out whole: the size
 * console_quote() needs, escapes, quotes and terminator counted. */
TEST_CASE("the quoted size counts quotes, escapes and the terminator",
          "[console_grammar]")
{
    char dst[CONSOLE_QUOTED_MAX(8U)];

    TEST_ASSERT_EQUAL_UINT(3U, console_quoted_size(""));
    TEST_ASSERT_EQUAL_UINT(3U, console_quoted_size(NULL));
    TEST_ASSERT_EQUAL_UINT(8U, console_quoted_size("a b c"));
    TEST_ASSERT_EQUAL_UINT(12U, console_quoted_size("a\"b\\c\n"));
    TEST_ASSERT_EQUAL_UINT(console_quoted_size("a\"b\\c\n"),
        strlen(console_quote(dst, sizeof(dst), "a\"b\\c\n")) + 1U);
}
