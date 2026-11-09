/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_sched_cron.c
 * @brief The scheduler's time expression: what parses, what is refused, and the next match.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "sched_cron.h"

/** @brief Friday 2026-10-30 18:37:12 UTC. */
#define T0  INT64_C(1793385432)

/** @brief Parse @p text, which must be good, and return the next match after @p after. */
static int64_t next_of(const char *text, int64_t after)
{
    sched_cron_t c;
    const char *why = NULL;

    TEST_ASSERT_EQUAL_INT(KODE_OK, sched_cron_parse(text, &c, &why));
    return sched_cron_next(&c, after);
}

/** @brief Assert that @p text is refused with @p reason. */
static void refused(const char *text, const char *reason)
{
    sched_cron_t c;
    const char *why = NULL;

    (void)memset(&c, 0, sizeof(c));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, sched_cron_parse(text, &c, &why));
    TEST_ASSERT_NOT_NULL(why);
    TEST_ASSERT_EQUAL_STRING(reason, why);
}

TEST_CASE("every minute: the next one", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1793385480, next_of("* * * * *", T0));
}

TEST_CASE("a step lands on the next multiple", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1793385900, next_of("*/15 * * * *", T0));
}

TEST_CASE("a match is strictly after the instant", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1793386800, next_of("*/15 * * * *", 1793385900));
}

TEST_CASE("an instant inside a matching minute waits for the next", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1793386800, next_of("*/15 * * * *", 1793385959));
}

TEST_CASE("daily at seven rolls to tomorrow", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1793430000, next_of("0 7 * * *", T0));
}

TEST_CASE("a weekday: Monday 2 November", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1793610000, next_of("0 9 * * 1", T0));
}

TEST_CASE("day of month and day of week combine with OR when both are set",
          "[sched_cron]")
{
    /* Sunday 1 November by the day of month, a day before the Monday. */
    TEST_ASSERT_EQUAL_INT64(1793523600, next_of("0 9 1 * 1", T0));
}

TEST_CASE("a star with a step in one day field makes the pair an AND",
          "[sched_cron]")
{
    sched_cron_t c;
    const char *why = NULL;

    TEST_ASSERT_EQUAL_INT(KODE_OK, sched_cron_parse("0 9 */2 * 1", &c, &why));
    TEST_ASSERT_TRUE(c.dom_any);
    TEST_ASSERT_FALSE(c.dow_any);
    /* Monday 2 Nov is even, 9 Nov odd: the first Monday on an odd day. */
    TEST_ASSERT_EQUAL_INT64(1794214800, sched_cron_next(&c, T0));
}

TEST_CASE("a range with a step", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1793439000, next_of("30 9-17/2 * * *", T0));
}

TEST_CASE("a list in one field", "[sched_cron]")
{
    /* From 18:37, the next of 1 and 15 past is 19:01. */
    TEST_ASSERT_EQUAL_INT64(1793386860, next_of("1,15 * * * *", T0));
    TEST_ASSERT_EQUAL_INT64(1793386860, next_of(" 1,15 *\t* * *  ", T0));
}

TEST_CASE("the year turns", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1798761600, next_of("0 0 1 1 *", 1798761570));
}

TEST_CASE("29 February is found four years on", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(1835395200, next_of("0 0 29 2 *", 1803729600));
}

TEST_CASE("29 February is found when it is the very day after", "[sched_cron]")
{
    /* Just after 29 Feb 2028 00:00: the next is 29 Feb 2032, 1461 days on. */
    TEST_ASSERT_EQUAL_INT64(1961625600, next_of("0 0 29 2 *", 1835395200));
}

TEST_CASE("past 2038 the time stays 64-bit", "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(2147483700, next_of("* * * * *", 2147483647));
}

TEST_CASE("Sunday may be written 7", "[sched_cron]")
{
    sched_cron_t a;
    sched_cron_t b;
    const char *why = NULL;

    TEST_ASSERT_EQUAL_INT(KODE_OK, sched_cron_parse("0 0 * * 7", &a, &why));
    TEST_ASSERT_EQUAL_INT(KODE_OK, sched_cron_parse("0 0 * * 0", &b, &why));
    TEST_ASSERT_EQUAL_UINT8(b.dow, a.dow);
    TEST_ASSERT_EQUAL_UINT8(0x01U, a.dow);
    TEST_ASSERT_EQUAL_UINT64(b.minute, a.minute);
    TEST_ASSERT_EQUAL_UINT32(b.hour, a.hour);
    TEST_ASSERT_EQUAL_UINT32(b.dom, a.dom);
    TEST_ASSERT_EQUAL_UINT16(b.month, a.month);
    /* Sunday 1 November at midnight. */
    TEST_ASSERT_EQUAL_INT64(1793491200, sched_cron_next(&a, T0));
}

TEST_CASE("a weekday range may end on 7", "[sched_cron]")
{
    sched_cron_t c;
    const char *why = NULL;

    TEST_ASSERT_EQUAL_INT(KODE_OK, sched_cron_parse("0 0 * * 5-7", &c, &why));
    TEST_ASSERT_EQUAL_UINT8(0x61U, c.dow); /* Friday, Saturday, Sunday. */
}

TEST_CASE("the sets hold exactly the listed values", "[sched_cron]")
{
    sched_cron_t c;
    const char *why = NULL;

    TEST_ASSERT_EQUAL_INT(KODE_OK, sched_cron_parse("0-2,59 23 31 12 *", &c, &why));
    TEST_ASSERT_EQUAL_UINT64((1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 59),
                             c.minute);
    TEST_ASSERT_EQUAL_UINT32(1UL << 23, c.hour);
    TEST_ASSERT_EQUAL_UINT32(1UL << 31, c.dom);
    TEST_ASSERT_EQUAL_UINT16(1U << 12, c.month);
    TEST_ASSERT_EQUAL_UINT8(0x7FU, c.dow);
    TEST_ASSERT_FALSE(c.dom_any);
    TEST_ASSERT_TRUE(c.dow_any);
}

TEST_CASE("an expression that never matches parses and ends the search",
          "[sched_cron]")
{
    TEST_ASSERT_EQUAL_INT64(-1, next_of("0 0 31 2 *", T0));
    TEST_ASSERT_EQUAL_INT64(-1, next_of("0 0 30 2 *", T0));
}

TEST_CASE("an instant with no answer gives -1", "[sched_cron]")
{
    sched_cron_t c;
    const char *why = NULL;

    TEST_ASSERT_EQUAL_INT(KODE_OK, sched_cron_parse("* * * * *", &c, &why));
    TEST_ASSERT_EQUAL_INT64(-1, sched_cron_next(&c, -1));
    TEST_ASSERT_EQUAL_INT64(-1, sched_cron_next(&c, INT64_MAX));
    TEST_ASSERT_EQUAL_INT64(-1, sched_cron_next(NULL, T0));
}

TEST_CASE("refusals name their reason", "[sched_cron]")
{
    refused("60 * * * *", "range");
    refused("* 24 * * *", "range");
    refused("* * 0 * *", "range");
    refused("* * 32 * *", "range");
    refused("* * * 13 *", "range");
    refused("* * * 0 *", "range");
    refused("* * * * 8", "range");
    refused("30-10 * * * *", "range");
    refused("99999999999 * * * *", "range");
    refused("*/0 * * * *", "step");
    refused("0-30/0 * * * *", "step");
    refused("*/61 * * * *", "step");
    refused("* * * *", "fields");
    refused("* * * * * *", "fields");
    refused("", "fields");
    refused("   ", "fields");
    refused("a * * * *", "syntax");
    refused("1,,2 * * * *", "syntax");
    refused("1, * * * *", "syntax");
    refused("*/ * * * *", "syntax");
    refused("5/2 * * * *", "syntax");
    refused("1- * * * *", "syntax");
    refused("* * * JAN *", "syntax");
    refused("@hourly * * * *", "syntax");
    refused("1-2-3 * * * *", "syntax");
    refused("*5 * * * *", "syntax");
}

TEST_CASE("a refused text leaves the result alone and a NULL argument is a caller error",
          "[sched_cron]")
{
    sched_cron_t c;
    const char *why = NULL;

    (void)memset(&c, 0xA5, sizeof(c));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, sched_cron_parse(NULL, &c, &why));
    TEST_ASSERT_EQUAL_STRING("args", why);
    TEST_ASSERT_EQUAL_UINT64(0xA5A5A5A5A5A5A5A5ULL, c.minute);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, sched_cron_parse("60 * * * *", &c, NULL));
    why = NULL;
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, sched_cron_parse("* * * * *", NULL, &why));
    TEST_ASSERT_EQUAL_STRING("args", why);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, sched_cron_parse(NULL, NULL, NULL));
}
