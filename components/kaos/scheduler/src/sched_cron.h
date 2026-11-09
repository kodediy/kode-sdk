/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file sched_cron.h
 * @brief The scheduler's time expression: five cron fields and the next minute that matches.
 */
#ifndef SCHED_CRON_H
#define SCHED_CRON_H

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

/** @brief Days sched_cron_next() looks ahead: 4 years (1461 days, one of them leap) and a day. */
#define SCHED_CRON_HORIZON_DAYS  1462U

/** @brief Fields in an expression. */
#define SCHED_CRON_FIELDS        5U

/** @brief Seconds in a minute. */
#define SCHED_CRON_SEC_MIN       60

/** @brief Seconds in an hour. */
#define SCHED_CRON_SEC_HOUR      3600

/** @brief Seconds in a day. */
#define SCHED_CRON_SEC_DAY       86400

/** @brief A time expression, as sets of the values each field accepts. */
typedef struct {
    uint64_t minute;  /**< Bit n set: minute n matches (0..59).                 */
    uint32_t hour;    /**< Bit n set: hour n matches (0..23).                   */
    uint32_t dom;     /**< Bit n set: day of month n matches (1..31).           */
    uint16_t month;   /**< Bit n set: month n matches (1..12).                  */
    uint8_t  dow;     /**< Bit n set: weekday n matches (0..6, Sunday 0).       */
    bool     dom_any; /**< The day-of-month text began with a star.             */
    bool     dow_any; /**< The day-of-week text began with a star.              */
} sched_cron_t;

/**
 * @brief Parse five fields into @p out.
 *
 * Fields are separated by spaces or tabs, which may also lead and trail.
 * @p out is written only on KODE_OK.
 *
 * @param text The expression; NULL is a caller error, see @p why.
 * @param out  Where the sets go.
 * @param why  Receives a short static reason on KODE_DENIED: "fields" (not
 *             five), "args" (@p text or @p out is NULL), "range" (a value
 *             outside its field, or a range running backwards), "step"
 *             (zero, or wider than the field) or "syntax" (anything else).
 *             May be NULL.
 * @return KODE_OK, or KODE_DENIED.
 */
kode_status_t sched_cron_parse(const char *text, sched_cron_t *out,
                               const char **why);

/**
 * @brief The first matching minute strictly after @p after_s.
 *
 * Walks days with the calendar, then hours, then minutes, so the cost is
 * bounded by SCHED_CRON_HORIZON_DAYS whatever the expression.
 *
 * @param c       A parsed expression.
 * @param after_s Unix seconds, UTC; a negative value or one past the far
 *                bound (2^60) has no answer.
 * @return The start of the matching minute in Unix seconds, or -1 when none
 *         falls within SCHED_CRON_HORIZON_DAYS (as "0 0 31 2 *" never does).
 */
int64_t sched_cron_next(const sched_cron_t *c, int64_t after_s);

#endif /* SCHED_CRON_H */
