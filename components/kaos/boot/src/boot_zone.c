/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file boot_zone.c
 * @brief The grammar of a POSIX TZ string, read without the C library.
 */
#include "boot_zone.h"

#include <stdint.h>

#include "kset.h"

/**
 * @brief Whether @p c is an ASCII letter.
 *
 * @param c The character.
 * @return true for A-Z and a-z.
 */
static bool zone_is_alpha(char c)
{
    return (('A' <= c) && ('Z' >= c)) || (('a' <= c) && ('z' >= c));
}

/**
 * @brief Whether @p c is an ASCII digit.
 *
 * @param c The character.
 * @return true for 0-9.
 */
static bool zone_is_digit(char c)
{
    return ('0' <= c) && ('9' >= c);
}

/**
 * @brief Read a run of digits whose value is at most @p max.
 *
 * @param s   The zone, terminated.
 * @param at  The place to read from; moved past the digits on success.
 * @param min The least value that is allowed.
 * @param max The most value that is allowed.
 * @return true when there was at least one digit and the value is in range.
 */
static bool zone_scan_num(const char *s, size_t *at, uint32_t min, uint32_t max)
{
    size_t   i      = *at;
    uint32_t value  = 0U;
    bool     in_cap = true;

    while ( zone_is_digit(s[i]) && in_cap )
    {
        value = (value * KSET_ZONE_RADIX)
              + ((uint32_t)(uint8_t)s[i] - (uint32_t)(uint8_t)'0');
        in_cap = (value <= max);
        ++i;
    }
    if ( in_cap && (i > *at) && (value >= min) )
    {
        *at = i;
        return true;
    }
    return false;
}

/**
 * @brief Read `[+-]hh[:mm[:ss]]`, hours up to @p hour_max.
 *
 * @param s        The zone, terminated.
 * @param at       The place to read from; moved past the time on success.
 * @param hour_max The most hours that are allowed.
 * @return true when a time was read.
 */
static bool zone_scan_time(const char *s, size_t *at, uint32_t hour_max)
{
    size_t i  = *at;
    bool   ok;

    if ( ('+' == s[i]) || ('-' == s[i]) )
    {
        ++i;
    }
    ok = zone_scan_num(s, &i, 0U, hour_max);
    if ( ok && (':' == s[i]) )
    {
        ++i;
        ok = zone_scan_num(s, &i, 0U, KSET_ZONE_MINSEC_MAX);
        if ( ok && (':' == s[i]) )
        {
            ++i;
            ok = zone_scan_num(s, &i, 0U, KSET_ZONE_MINSEC_MAX);
        }
    }
    if ( ok )
    {
        *at = i;
    }
    return ok;
}

/**
 * @brief Read a zone's name: KSET_ZONE_NAME_MIN or more letters, or
 *        `<` characters `>` where each is a letter, a digit, `+` or `-`.
 *
 * @param s  The zone, terminated.
 * @param at The place to read from; moved past the name on success.
 * @return true when a name was read.
 */
static bool zone_scan_name(const char *s, size_t *at)
{
    size_t i = *at;
    bool   ok;

    if ( '<' == s[i] )
    {
        size_t const start = i + 1U;

        i = start;
        while ( zone_is_alpha(s[i]) || zone_is_digit(s[i])
               || ('+' == s[i]) || ('-' == s[i]) )
        {
            ++i;
        }
        ok = ('>' == s[i]) && ((i - start) >= KSET_ZONE_NAME_MIN);
        if ( ok )
        {
            ++i;
        }
    }
    else
    {
        while ( zone_is_alpha(s[i]) )
        {
            ++i;
        }
        ok = ((i - *at) >= KSET_ZONE_NAME_MIN);
    }
    if ( ok )
    {
        *at = i;
    }
    return ok;
}

/**
 * @brief Read one change-over date: `Jn`, `n` or `Mm.w.d`, then an optional
 *        `/time`.
 *
 * @param s  The zone, terminated.
 * @param at The place to read from; moved past the date on success.
 * @return true when a date was read.
 */
static bool zone_scan_rule(const char *s, size_t *at)
{
    size_t i = *at;
    bool   ok;

    if ( 'J' == s[i] )
    {
        ++i;
        ok = zone_scan_num(s, &i, 1U, KSET_ZONE_DAY_MAX);
    }
    else if ( 'M' == s[i] )
    {
        ++i;
        ok = zone_scan_num(s, &i, 1U, KSET_ZONE_MONTH_MAX)
          && ('.' == s[i]);
        if ( ok )
        {
            ++i;
            ok = zone_scan_num(s, &i, 1U, KSET_ZONE_WEEK_MAX)
              && ('.' == s[i]);
        }
        if ( ok )
        {
            ++i;
            ok = zone_scan_num(s, &i, 0U, KSET_ZONE_WEEKDAY_MAX);
        }
    }
    else
    {
        ok = zone_scan_num(s, &i, 0U, KSET_ZONE_DAY_MAX);
    }
    if ( ok && ('/' == s[i]) )
    {
        ++i;
        ok = zone_scan_time(s, &i, KSET_ZONE_RULE_HOUR_MAX);
    }
    if ( ok )
    {
        *at = i;
    }
    return ok;
}

/**
 * @brief Whether @p zone spells a POSIX TZ string:
 *        `std offset [dst [offset] [,start[/time],end[/time]]]`.
 *
 * @param zone The candidate, terminated.
 * @return true when the whole of it reads.
 */
static bool zone_grammar_ok(const char *zone)
{
    size_t at = 0U;
    bool   ok = zone_scan_name(zone, &at)
              && zone_scan_time(zone, &at, KSET_ZONE_HOUR_MAX);

    if ( ok && ('\0' != zone[at]) )
    {
        ok = zone_scan_name(zone, &at);
        if ( ok && ('\0' != zone[at]) && (',' != zone[at]) )
        {
            ok = zone_scan_time(zone, &at, KSET_ZONE_HOUR_MAX);
        }
        if ( ok && (',' == zone[at]) )
        {
            ++at;
            ok = zone_scan_rule(zone, &at) && (',' == zone[at]);
            if ( ok )
            {
                ++at;
                ok = zone_scan_rule(zone, &at);
            }
        }
    }
    return ok && ('\0' == zone[at]);
}

bool boot_zone_valid(const char *zone, size_t cap)
{
    size_t n = 0U;

    while ( (n < cap) && ('\0' != zone[n]) )
    {
        ++n;
    }
    return (0U < n) && (KSET_ZONE_MAX >= n) && (n < cap) && zone_grammar_ok(zone);
}
