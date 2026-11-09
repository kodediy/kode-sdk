/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_variables.h
 * @brief Variables: what the app lets a host read, declared once.
 */
#ifndef KODE_VARIABLES_H_
#define KODE_VARIABLES_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KODE_ROW_NAME_MAX          15U    /**< Chars in a name or an enum word.  */
#define KODE_ROW_DESC_MAX          48U    /**< Chars in a description.           */
#define KODE_VALUE_STR_MAX         32U    /**< Chars in a value's word.          */
#define KODE_VARIABLES_MAX         32U    /**< Variables per firmware, all tables.*/
#define KODE_VARIABLE_TABLES       4U     /**< kode_variables_add() calls accepted.*/
#define KODE_VARIABLE_WATCH_MAX    4U     /**< Variables followed at once.       */
#define KODE_VARIABLE_WATCH_MIN_MS 100U   /**< Shortest watch period.            */
#define KODE_VARIABLE_WATCH_MAX_MS 60000U /**< Longest watch period.             */

/* Runs on AO_Console's task inside one step: only read, return promptly, call nothing in kode.h. */
/**
 * @brief Write the variable's value into @p buf, as one word.
 * @param buf Zeroed before the call.
 * @param cap Its size, room for more than KODE_VALUE_STR_MAX characters:
 *            snprintf() into it and the console refuses what does not fit.
 * @return KODE_OK, or any other status to refuse - which also ends a watch.
 */
typedef kode_status_t (*kode_variable_get_fn)(char *buf, size_t cap);

/** @brief One row of the app's table. Every field but name and get has a
 *         default. */
typedef struct {
    const char           *name; /**< [a-z0-9_], 1..KODE_ROW_NAME_MAX, unique
                                 *   over every variable and function.     */
    const char           *desc; /**< Prose for variable_list; the unit and the
                                 *   words the value can be belong here.
                                 *   NULL prints `-`.                        */
    kode_variable_get_fn  get;  /**< Required.                               */
} kode_variable_t;

/**
 * @brief Register one table of variables. Call after kode_init().
 *
 * The table is kept by pointer, so it must be `static const` and outlive the
 * firmware. A table with one bad row is refused whole, with a log line
 * naming the row.
 *
 * @param table The rows.
 * @param n     How many.
 * @return KODE_OK; KODE_EXISTS for a name already taken; KODE_NO_RESOURCES
 *         when KODE_VARIABLES_MAX or KODE_VARIABLE_TABLES would be exceeded;
 *         KODE_DENIED for any other rule, and always in a build without
 *         CONFIG_KODEOS_CONSOLE, where there is nothing to reach the table.
 */
kode_status_t kode_variables_add(const kode_variable_t *table, size_t n);

/** @brief kode_variables_add() over a whole array. */
#define KODE_VARIABLES_ADD(t) kode_variables_add((t), sizeof(t) / sizeof((t)[0]))

#ifdef __cplusplus
}
#endif

#endif /* KODE_VARIABLES_H_ */
