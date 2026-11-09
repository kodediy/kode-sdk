/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_klog.c
 * @brief The log front end: the line it produces, and who is allowed to see it.
 */
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kode_log.h"
#include "kqp_config.h"

#define TAG "t.klog"

/* ---- The line ------------------------------------------------------------ */

TEST_CASE("a line is level, tick, tag and message, with the mark first",
          "[klog]")
{
    KLOG_I(TAG, "hello");

    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!I 0 t.klog hello\n", fake_klog_last());
}

TEST_CASE("each level has its own letter", "[klog]")
{
    KLOG_D(TAG, "d");
    KLOG_I(TAG, "i");
    KLOG_W(TAG, "w");
    KLOG_E(TAG, "e");

    TEST_ASSERT_EQUAL_UINT(4U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!D 0 t.klog d\n", fake_klog_line(0U));
    TEST_ASSERT_EQUAL_STRING("!I 0 t.klog i\n", fake_klog_line(1U));
    TEST_ASSERT_EQUAL_STRING("!W 0 t.klog w\n", fake_klog_line(2U));
    TEST_ASSERT_EQUAL_STRING("!E 0 t.klog e\n", fake_klog_line(3U));
}

/* The timestamp is milliseconds of uptime, and the clock here is the one the
 * case advances by hand - which is the whole reason esp_timer is faked rather
 * than mocked. */
TEST_CASE("the timestamp is uptime in milliseconds", "[klog]")
{
    fake_time_advance_ms(1234U);
    KLOG_I(TAG, "later");
    TEST_ASSERT_EQUAL_STRING("!I 1234 t.klog later\n", fake_klog_last());
}

TEST_CASE("a formatted message keeps its arguments", "[klog]")
{
    KLOG_W(TAG, "%s at 0x%02X, %d times", "chip", 0x30, 3);
    TEST_ASSERT_EQUAL_STRING("!W 0 t.klog chip at 0x30, 3 times\n",
                             fake_klog_last());
}

/* ---- Levels -------------------------------------------------------------- */

TEST_CASE("the global level filters everything below it", "[klog]")
{
    kode_log_set_level(NULL, KODE_LOG_WARN);

    KLOG_D(TAG, "gone");
    KLOG_I(TAG, "gone");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    /* The comparison is `<`, so a line AT the minimum is emitted. */
    KLOG_W(TAG, "kept");
    KLOG_E(TAG, "kept");
    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());

    kode_log_set_level(NULL, KODE_LOG_DEBUG);
}

/* NOTICE is a filter step between INFO and WARN, so `level=n` keeps the
 * outcomes and the trouble and drops the chatter - and it prints as N. */
TEST_CASE("NOTICE sits between INFO and WARN, and prints as N", "[klog]")
{
    kode_log_set_level(NULL, KODE_LOG_NOTICE);

    KLOG_D(TAG, "gone");
    KLOG_I(TAG, "gone");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    KLOG_N(TAG, "kept");
    TEST_ASSERT_EQUAL_STRING("!N 0 t.klog kept\n", fake_klog_last());
    KLOG_W(TAG, "kept");
    KLOG_E(TAG, "kept");
    TEST_ASSERT_EQUAL_UINT(3U, fake_klog_count());

    kode_log_set_level(NULL, KODE_LOG_DEBUG);
}

TEST_CASE("OFF suppresses every normal log but not a console reply", "[klog]")
{
    kode_log_set_level(NULL, KODE_LOG_OFF);

    KLOG_D(TAG, "gone");
    KLOG_I(TAG, "gone");
    KLOG_W(TAG, "gone");
    KLOG_E(TAG, "gone");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    kode_log_reply(">ok");
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());

    kode_log_set_level("off.test", KODE_LOG_DEBUG);
    KLOG_D("off.test", "enabled");
    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!D 0 off.test enabled\n", fake_klog_last());

    kode_log_set_level(NULL, KODE_LOG_DEBUG);
}

TEST_CASE("a per-tag level beats the global one, in both directions", "[klog]")
{
    kode_log_set_level(NULL, KODE_LOG_ERROR);
    kode_log_set_level("chatty", KODE_LOG_DEBUG);

    kode_log(KODE_LOG_DEBUG, "chatty", "let me through");
    kode_log(KODE_LOG_WARN,  "quiet",  "but not me");
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!D 0 chatty let me through\n", fake_klog_last());

    /* And the other way: quieter than the global default. */
    kode_log_set_level(NULL, KODE_LOG_DEBUG);
    kode_log_set_level("chatty", KODE_LOG_ERROR);
    kode_log(KODE_LOG_WARN, "chatty", "suppressed");
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    kode_log_set_level("chatty", KODE_LOG_DEBUG);
    kode_log_set_level(NULL, KODE_LOG_DEBUG);
}

/* A tag the table cannot hold whole used to be accepted and then never match:
 * the copy kept 15 characters and a terminator while the lookup compared 16,
 * so the override silently did nothing and burned one of sixteen slots. There
 * is no eviction, and `log_level tag=` is a console verb taking an operator's
 * input - so seventeen long tags disabled per-tag levels for the whole boot. */
TEST_CASE("a tag too long for the table is refused, and says so", "[klog]")
{
    static const char *const too_long = "0123456789abcdef";   /* 16 chars */

    TEST_ASSERT_EQUAL_UINT(16U, strlen(too_long));

    kode_log_set_level(too_long, KODE_LOG_ERROR);

    /* It complained rather than accepting silently. */
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    /* And the override did not take, which is the honest outcome: the table
     * cannot hold that tag, so it says so instead of pretending. */
    kode_log(KODE_LOG_DEBUG, too_long, "still gets through");
    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());
}

TEST_CASE("a tag of exactly the longest usable length still works", "[klog]")
{
    static const char *const longest = "0123456789abcde";   /* 15 chars */

    TEST_ASSERT_EQUAL_UINT(15U, strlen(longest));
    kode_log_set_level(longest, KODE_LOG_ERROR);

    kode_log(KODE_LOG_WARN, longest, "suppressed");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    kode_log_set_level(longest, KODE_LOG_DEBUG);
}

/* kode_log() is public and inherited by apps, and it used to index two
 * four-element tables with whatever level the caller passed. The filter only
 * bounded the level from below. */
TEST_CASE("a level past the end of the scale is dropped, not indexed", "[klog]")
{
    kode_log((kode_log_lvl_t)99, TAG, "out of range");
    kode_log((kode_log_lvl_t)(KODE_LOG_OFF + 1), TAG, "past OFF");

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

TEST_CASE("a null tag or format is dropped rather than dereferenced", "[klog]")
{
    kode_log(KODE_LOG_INFO, NULL, "no tag");
    kode_log(KODE_LOG_INFO, TAG, NULL);

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

/* ---- Replies ------------------------------------------------------------- */

/* A reply is the console protocol's other half. It carries no mark, no level
 * and no timestamp, because the framing is the caller's contract with the
 * host - and it rides the same sink so replies and log lines reach the wire
 * in the order they were produced. */
TEST_CASE("a reply goes out verbatim, with only a newline added", "[klog]")
{
    kode_log_reply(">ok");
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());
}

TEST_CASE("a reply ignores the level filter entirely", "[klog]")
{
    kode_log_set_level(NULL, KODE_LOG_ERROR);

    KLOG_I(TAG, "suppressed");
    kode_log_reply(">ok");

    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());

    kode_log_set_level(NULL, KODE_LOG_DEBUG);
}

TEST_CASE("replies and log lines keep their production order", "[klog]")
{
    KLOG_I(TAG, "before");
    kode_log_reply(">ok");
    KLOG_I(TAG, "after");

    TEST_ASSERT_EQUAL_UINT(3U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!I 0 t.klog before\n", fake_klog_line(0U));
    TEST_ASSERT_EQUAL_STRING(">ok\n",                fake_klog_line(1U));
    TEST_ASSERT_EQUAL_STRING("!I 0 t.klog after\n",  fake_klog_line(2U));
}

/* ---- The 256-byte boundary ----------------------------------------------- */

/* A truncated line still ends in exactly one newline. That is the framing
 * rule the whole console protocol rests on: a host reads lines, and a line
 * that lost its terminator swallows the next one. */
TEST_CASE("an over-long line is truncated and still ends in one newline",
          "[klog]")
{
    static char big[600];

    (void)memset(big, 'x', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';

    KLOG_I(TAG, "%s", big);

    const char  *line = fake_klog_last();
    size_t const len  = strlen(line);

    TEST_ASSERT_EQUAL_UINT(255U, len);          /* 256 with the terminator */
    TEST_ASSERT_EQUAL_CHAR('\n', line[len - 1U]);
    TEST_ASSERT_EQUAL_CHAR('!',  line[0]);

    /* Exactly one newline, at the very end. */
    TEST_ASSERT_NULL(memchr(line, '\n', len - 1U));
}

TEST_CASE("an over-long reply truncates the same way", "[klog]")
{
    static char big[600];

    (void)memset(big, '>', sizeof(big) - 1U);
    big[sizeof(big) - 1U] = '\0';

    kode_log_reply(big);

    const char  *line = fake_klog_last();
    size_t const len  = strlen(line);

    TEST_ASSERT_EQUAL_UINT(255U, len);
    TEST_ASSERT_EQUAL_CHAR('\n', line[len - 1U]);
}

/* ---- Colour -------------------------------------------------------------- */

/* Off by default, because the primary console client is a host program and it
 * wants the mark in column 0, not an escape sequence. */
TEST_CASE("colour is off by default and wraps the whole line when on",
          "[klog]")
{
    KLOG_I(TAG, "plain");
    TEST_ASSERT_EQUAL_CHAR('!', fake_klog_last()[0]);

    kode_log_set_color(true);
    KLOG_I(TAG, "bright");

    const char *line = fake_klog_last();
    TEST_ASSERT_EQUAL_CHAR('\033', line[0]);

    /* One prefix and one suffix, so a host can strip both without parsing:
     * the reset sits before the newline, inside the line. */
    TEST_ASSERT_NOT_NULL(strstr(line, "\033[0m\n"));
    TEST_ASSERT_NOT_NULL(strstr(line, "bright"));

    kode_log_set_color(false);
    KLOG_I(TAG, "plain again");
    TEST_ASSERT_EQUAL_STRING("!I 0 t.klog plain again\n", fake_klog_last());
}

/* ---- Routing ------------------------------------------------------------- */

/* Three conditions send a line down the rescue path instead of to the sink:
 * no sink registered, an ISR, and a scheduler that is not running. The first
 * two are reachable here; the sink simply must not receive them. */
TEST_CASE("with no sink registered, nothing reaches the sink", "[klog]")
{
    fake_klog_detach();
    KLOG_I(TAG, "into the rescue path");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

TEST_CASE("from ISR context, the sink is bypassed", "[klog]")
{
    fake_port_set_can_yield(false);
    KLOG_E(TAG, "from an interrupt");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    fake_port_set_can_yield(true);
    KLOG_E(TAG, "and back on a task");
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
}

TEST_CASE("a line carries a date once the board knows one", "[klog]")
{
    /* 2026-09-03T21:09:44.585Z */
    fake_time_wall_set(1788469784585000ULL);
    kode_log(KODE_LOG_INFO, "t.klog", "dated");
    TEST_ASSERT_EQUAL_STRING("!I 2026-09-03T21:09:44.585Z t.klog dated\n",
                             fake_klog_last());
}

TEST_CASE("until then it carries milliseconds since boot", "[klog]")
{
    /* Which is the honest stamp for a board that has not been told the time,
     * and is still what a bench script diffs. */
    fake_time_wall_set(0U);
    fake_time_advance_ms(4585U);
    kode_log(KODE_LOG_INFO, "t.klog", "undated");
    TEST_ASSERT_EQUAL_STRING("!I 4585 t.klog undated\n", fake_klog_last());
}

/* ---- The tag a reply carries ------------------------------------------------ */

TEST_CASE("a reply carries the tag it was sent with, a log line none", "[klog]")
{
    kode_log_reply_src(">|pong", (uint8_t)KODE_CONSOLE_SRC_NET);
    kode_log_reply(">ok");
    KLOG_N(TAG, "hello");

    TEST_ASSERT_EQUAL_UINT(3U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING(">|pong\n", fake_klog_line(0U));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_NET, fake_klog_src(0U));
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_line(1U));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOG_SRC_NONE, fake_klog_src(1U));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOG_SRC_NONE, fake_klog_src(2U));
}
