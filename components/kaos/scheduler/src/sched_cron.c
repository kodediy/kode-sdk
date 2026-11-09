/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sched_cron.c
 * @brief The scheduler's cron fields: parsing all five and finding the next minute that matches.
 */
#include "sched_cron.h"

#include <stddef.h>

/** @brief Largest value the number reader keeps; anything above saturates here and fails the range check. */
#define NUM_SATURATE        1000U

/** @brief Radix of the numbers in the text. */
#define NUM_RADIX           10U

/** @brief Largest weekday the text may carry: 7 is Sunday again and folds onto 0. */
#define DOW_RAW_MAX         7U

/** @brief The bit that stands for Sunday written as 7. */
#define DOW_SEVEN_BIT       (1ULL << DOW_RAW_MAX)

/** @brief The bit that stands for Sunday written as 0. */
#define DOW_ZERO_BIT        1ULL

/** @brief Last minute of an hour. */
#define MINUTE_LAST         59U

/** @brief Last hour of a day. */
#define HOUR_LAST           23U

/** @brief Last day of a month any month can have. */
#define DOM_LAST            31U

/** @brief Months in a year. */
#define MONTH_LAST          12U

/** @brief Days in a week. */
#define WEEK_DAYS           7

/** @brief Weekday of day 0 (1970-01-01, a Thursday), Sunday being 0. */
#define EPOCH_WEEKDAY       4

/** @brief Days from 0000-03-01 to 1970-01-01, which moves the origin to the start of an era. */
#define CIVIL_EPOCH_SHIFT   719468

/** @brief Days in one 400-year era. */
#define CIVIL_DAYS_ERA      146097

/** @brief Days in 4 years less one: the divisor that takes out each leap day within an era. */
#define CIVIL_DAYS_4Y       1460

/** @brief Days in 100 years less one: the divisor that puts back the century year that is not leap. */
#define CIVIL_DAYS_100Y     36524

/** @brief Days in an ordinary year. */
#define CIVIL_DAYS_YEAR     365

/** @brief Days in an era less one: the divisor that takes out the era's own leap day. */
#define CIVIL_DAYS_ERA_LAST 146096

/** @brief Years between two leap days: the divisor that counts them in a year-of-era. */
#define CIVIL_YEARS_LEAP    4

/** @brief Years in a century: the divisor that takes back the leap days of the century years. */
#define CIVIL_YEARS_CENTURY 100

/** @brief The first day of a month, which is what a day of month counts from. */
#define CIVIL_FIRST_DOM     1

/** @brief Month index counted from March (0..11) is (5 * doy + 2) / 153; this is the 5. */
#define CIVIL_MONTH_MUL     5

/** @brief The 2 in (5 * doy + 2) / 153 and in (153 * month + 2) / 5. */
#define CIVIL_MONTH_BIAS    2

/** @brief The 153 in (5 * doy + 2) / 153: days in five months from March. */
#define CIVIL_MONTH_SPAN    153

/** @brief Months counted from March (0..9) that are March to December. */
#define CIVIL_MARCH_MONTHS  10

/** @brief Added to a month counted from March (0..9) to give the month from January. */
#define CIVIL_MONTH_UP      3

/** @brief Subtracted from a month counted from March (10, 11) to give the month from January. */
#define CIVIL_MONTH_DOWN    9

/** @brief The farthest second sched_cron_next() accepts: 2^60, far beyond any wall clock and short of overflow. */
#define AFTER_MAX_S         INT64_C(1152921504606846976)

/** @brief Index of the minute field, in the order the fields are written. */
#define FIELD_MINUTE  0U

/** @brief Index of the hour field. */
#define FIELD_HOUR    1U

/** @brief Index of the day-of-month field. */
#define FIELD_DOM     2U

/** @brief Index of the month field. */
#define FIELD_MONTH   3U

/** @brief Index of the day-of-week field. */
#define FIELD_DOW     4U

/** @brief The shape of one field: the values it accepts. */
typedef struct {
    uint32_t lo; /**< Smallest value. */
    uint32_t hi; /**< Largest value the text may carry. */
} field_spec_t;

/** @brief Reason: the text is not five fields. */
#define WHY_FIELDS  "fields"

/** @brief Reason: text or result is NULL, a caller error and not a fault of the expression. */
#define WHY_ARGS    "args"

/** @brief Reason: a value is outside its field. */
#define WHY_RANGE   "range"

/** @brief Reason: a step is zero or wider than its field. */
#define WHY_STEP    "step"

/** @brief Reason: anything else that is not the grammar. */
#define WHY_SYNTAX  "syntax"

/** @brief Bits in the widest set, which no shift may reach. */
#define SET_BITS    64U

/**
 * @brief Is value @p n in @p set.
 *
 * @param set A set of up to 64 values, bit n for value n.
 * @param n   The value.
 * @return True when it is in the set; false for a value past the set's width.
 */
static bool set_has(uint64_t set, uint32_t n)
{
    if ( SET_BITS <= n )
    {
        return false;
    }
    return ((set >> n) & 1U) != 0U;
}

/**
 * @brief Put value @p n in @p set.
 *
 * @param set A set of up to 64 values.
 * @param n   The value; one past the set's width is ignored.
 * @return The set with the value in it.
 */
static uint64_t set_add(uint64_t set, uint32_t n)
{
    if ( SET_BITS <= n )
    {
        return set;
    }
    return set | (1ULL << n);
}

/** @brief Is @p ch a field separator. */
static bool is_blank(char ch)
{
    return (' ' == ch) || ('\t' == ch);
}

/** @brief Is @p ch the end of a field. */
static bool is_field_end(char ch)
{
    return ('\0' == ch) || is_blank(ch);
}

/**
 * @brief Move @p pos past separators.
 *
 * @param text The expression.
 * @param pos  The position to advance.
 */
static void skip_blanks(const char *text, size_t *pos)
{
    while ( is_blank(text[*pos]) )
    {
        *pos += 1U;
    }
}

/**
 * @brief Read an unsigned decimal number at @p pos.
 *
 * @param text The expression.
 * @param pos  Moved past the digits on success.
 * @param val  Receives the number, saturated at NUM_SATURATE.
 * @return True when there was at least one digit.
 */
static bool read_number(const char *text, size_t *pos, uint32_t *val)
{
    size_t i = *pos;
    uint32_t v = 0U;

    while ( ('0' <= text[i]) && ('9' >= text[i]) )
    {
        const uint32_t digit = (uint32_t)text[i];

        v = (v * NUM_RADIX) + (digit - (uint32_t)'0');
        if ( NUM_SATURATE < v )
        {
            v = NUM_SATURATE;
        }
        i++;
    }
    if ( i == *pos )
    {
        return false;
    }
    *pos = i;
    *val = v;
    return true;
}

/**
 * @brief Parse one comma-separated item and add its values to @p bits.
 *
 * @param text The expression.
 * @param pos  Moved past the item on success.
 * @param spec The field it belongs to.
 * @param bits The set it adds to.
 * @param why  Receives the reason on failure.
 * @return True on success.
 */
static bool parse_item(const char *text, size_t *pos, const field_spec_t *spec,
                       uint64_t *bits, const char **why)
{
    uint32_t lo = 0U;
    uint32_t hi = 0U;
    uint32_t step = 1U;
    bool stepable = false;
    size_t i = *pos;

    if ( '*' == text[i] )
    {
        lo = spec->lo;
        hi = spec->hi;
        stepable = true;
        i++;
    }
    else
    {
        if ( !read_number(text, &i, &lo) )
        {
            *why = WHY_SYNTAX;
            return false;
        }
        hi = lo;
        if ( '-' == text[i] )
        {
            i++;
            stepable = true;
            if ( !read_number(text, &i, &hi) )
            {
                *why = WHY_SYNTAX;
                return false;
            }
        }
    }

    if ( '/' == text[i] )
    {
        i++;
        if ( !stepable || !read_number(text, &i, &step) )
        {
            *why = WHY_SYNTAX;
            return false;
        }
    }

    if ( (',' != text[i]) && !is_field_end(text[i]) )
    {
        *why = WHY_SYNTAX;
        return false;
    }
    if ( (lo < spec->lo) || (hi > spec->hi) || (lo > hi) )
    {
        *why = WHY_RANGE;
        return false;
    }
    if ( (0U == step) || (((spec->hi - spec->lo) + 1U) < step) )
    {
        *why = WHY_STEP;
        return false;
    }

    for ( uint32_t v = lo; v <= hi; v += step )
    {
        *bits = set_add(*bits, v);
    }
    *pos = i;
    return true;
}

/**
 * @brief Parse one field, which ends at a separator or the end of the text.
 *
 * @param text The expression.
 * @param pos  Moved past the field on success.
 * @param spec The field's shape.
 * @param bits Receives the set.
 * @param star Receives whether the field began with a star.
 * @param why  Receives the reason on failure.
 * @return True on success.
 */
static bool parse_field(const char *text, size_t *pos, const field_spec_t *spec,
                        uint64_t *bits, bool *star, const char **why)
{
    *bits = 0U;
    *star = ('*' == text[*pos]);
    for ( ;; )
    {
        if ( !parse_item(text, pos, spec, bits, why) )
        {
            return false;
        }
        if ( ',' != text[*pos] )
        {
            break;
        }
        *pos += 1U;
    }
    return true;
}

kode_status_t sched_cron_parse(const char *text, sched_cron_t *out,
                               const char **why)
{
    /** The five fields in the order they are written. */
    static const field_spec_t spec[SCHED_CRON_FIELDS] = {
        [FIELD_MINUTE] = { 0U, MINUTE_LAST },
        [FIELD_HOUR]   = { 0U, HOUR_LAST },
        [FIELD_DOM]    = { 1U, DOM_LAST },
        [FIELD_MONTH]  = { 1U, MONTH_LAST },
        [FIELD_DOW]    = { 0U, DOW_RAW_MAX }
    };
    uint64_t sets[SCHED_CRON_FIELDS] = { 0U, 0U, 0U, 0U, 0U };
    bool star[SCHED_CRON_FIELDS] = { false, false, false, false, false };
    const char *reason = WHY_FIELDS;
    size_t pos = 0U;
    bool ok = true;

    if ( (NULL == text) || (NULL == out) )
    {
        if ( NULL != why )
        {
            *why = WHY_ARGS;
        }
        return KODE_DENIED;
    }
    skip_blanks(text, &pos);
    for ( uint32_t i = 0U; ok && (SCHED_CRON_FIELDS > i); i++ )
    {
        if ( '\0' == text[pos] )
        {
            reason = WHY_FIELDS;
            ok = false;
        }
        else
        {
            ok = parse_field(text, &pos, &spec[i], &sets[i], &star[i], &reason);
        }
        if ( ok )
        {
            skip_blanks(text, &pos);
        }
    }
    if ( ok && ('\0' != text[pos]) )
    {
        reason = WHY_FIELDS;
        ok = false;
    }
    if ( !ok )
    {
        if ( NULL != why )
        {
            *why = reason;
        }
        return KODE_DENIED;
    }

    if ( set_has(sets[FIELD_DOW], DOW_RAW_MAX) )
    {
        sets[FIELD_DOW] = (sets[FIELD_DOW] & ~DOW_SEVEN_BIT) | DOW_ZERO_BIT;
    }
    out->minute  = sets[FIELD_MINUTE];
    out->hour    = (uint32_t)sets[FIELD_HOUR];
    out->dom     = (uint32_t)sets[FIELD_DOM];
    out->month   = (uint16_t)sets[FIELD_MONTH];
    out->dow     = (uint8_t)sets[FIELD_DOW];
    out->dom_any = star[FIELD_DOM];
    out->dow_any = star[FIELD_DOW];
    return KODE_OK;
}

/**
 * @brief Month and day of month of a day count.
 *
 * @param days  Days since 1970-01-01, not negative.
 * @param month Receives 1..12.
 * @param dom   Receives 1..31.
 */
static void civil_from_days(int64_t days, uint32_t *month, uint32_t *dom)
{
    const int64_t z = days + CIVIL_EPOCH_SHIFT;
    const int64_t era = z / CIVIL_DAYS_ERA;
    const int64_t doe = z - (era * CIVIL_DAYS_ERA);
    const int64_t yoe = (doe - (doe / CIVIL_DAYS_4Y) + (doe / CIVIL_DAYS_100Y)
                         - (doe / CIVIL_DAYS_ERA_LAST)) / CIVIL_DAYS_YEAR;
    const int64_t doy = doe - ((CIVIL_DAYS_YEAR * yoe) + (yoe / CIVIL_YEARS_LEAP) - (yoe / CIVIL_YEARS_CENTURY));
    const int64_t mp = ((CIVIL_MONTH_MUL * doy) + CIVIL_MONTH_BIAS) / CIVIL_MONTH_SPAN;
    int64_t d = doy - (((CIVIL_MONTH_SPAN * mp) + CIVIL_MONTH_BIAS) / CIVIL_MONTH_MUL) + CIVIL_FIRST_DOM;
    int64_t m = mp - CIVIL_MONTH_DOWN;

    if ( CIVIL_MARCH_MONTHS > mp )
    {
        m = mp + CIVIL_MONTH_UP;
    }
    *dom = (uint32_t)d;
    *month = (uint32_t)m;
}

/**
 * @brief Does the day pass the month, day-of-month and weekday sets.
 *
 * @param c    The expression.
 * @param days Days since 1970-01-01, not negative.
 * @return True when the day matches.
 */
static bool day_matches(const sched_cron_t *c, int64_t days)
{
    uint32_t month = 0U;
    uint32_t dom = 0U;
    const int64_t weekday = (days + EPOCH_WEEKDAY) % WEEK_DAYS;
    bool dom_hit;
    bool dow_hit;

    civil_from_days(days, &month, &dom);
    if ( !set_has(c->month, month) )
    {
        return false;
    }
    dom_hit = set_has(c->dom, dom);
    dow_hit = set_has(c->dow, (uint32_t)weekday);
    if ( c->dom_any || c->dow_any )
    {
        return dom_hit && dow_hit;
    }
    return dom_hit || dow_hit;
}

/**
 * @brief The first hour and minute of a day at or after a time of day.
 *
 * @param c      The expression.
 * @param from_h Hour to start from.
 * @param from_m Minute of that hour to start from.
 * @param hour   Receives the hour.
 * @param minute Receives the minute.
 * @return True when the day has one.
 */
static bool time_of_day_find(const sched_cron_t *c, uint32_t from_h,
                             uint32_t from_m, uint32_t *hour, uint32_t *minute)
{
    uint32_t m_start = from_m;

    for ( uint32_t h = from_h; HOUR_LAST >= h; h++ )
    {
        if ( set_has(c->hour, h) )
        {
            for ( uint32_t m = m_start; MINUTE_LAST >= m; m++ )
            {
                if ( set_has(c->minute, m) )
                {
                    *hour = h;
                    *minute = m;
                    return true;
                }
            }
        }
        m_start = 0U;
    }
    return false;
}

int64_t sched_cron_next(const sched_cron_t *c, int64_t after_s)
{
    int64_t found = -1;
    int64_t start;
    int64_t day0;
    int64_t sod;
    int64_t hour0;
    int64_t minute0;
    uint32_t from_h;
    uint32_t from_m;
    uint32_t h = 0U;
    uint32_t m = 0U;

    if ( (NULL == c) || (0 > after_s) || (AFTER_MAX_S < after_s) )
    {
        return -1;
    }
    start = ((after_s / SCHED_CRON_SEC_MIN) + 1) * SCHED_CRON_SEC_MIN;
    day0 = start / SCHED_CRON_SEC_DAY;
    sod = start % SCHED_CRON_SEC_DAY;
    hour0 = sod / SCHED_CRON_SEC_HOUR;
    minute0 = (sod % SCHED_CRON_SEC_HOUR) / SCHED_CRON_SEC_MIN;
    from_h = (uint32_t)hour0;
    from_m = (uint32_t)minute0;

    for ( uint32_t i = 0U; (0 > found) && (SCHED_CRON_HORIZON_DAYS >= i); i++ )
    {
        const int64_t day = day0 + (int64_t)i;

        if ( day_matches(c, day) && time_of_day_find(c, from_h, from_m, &h, &m) )
        {
            found = (day * SCHED_CRON_SEC_DAY) + ((int64_t)h * SCHED_CRON_SEC_HOUR)
                  + ((int64_t)m * SCHED_CRON_SEC_MIN);
        }
        from_h = 0U;
        from_m = 0U;
    }
    return found;
}
