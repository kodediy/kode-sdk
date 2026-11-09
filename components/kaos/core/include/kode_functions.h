/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_functions.h
 * @brief Functions: the app's own commands, declared once.
 */
#ifndef KODE_FUNCTIONS_H_
#define KODE_FUNCTIONS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kode_variables.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KODE_FUNCTIONS_MAX    64U /**< Functions per firmware, all tables.    */
#define KODE_FUNCTION_TABLES  4U  /**< kode_functions_add() calls accepted.  */
#define KODE_VALUE_ENUMS_MAX  6U  /**< Words an enum argument may list.      */
#define KODE_ARG_STR_MAX      256U /**< Chars in a string argument.          */
#define KODE_FUNCTION_ANSWER_MS_MAX 600000U /**< Longest `answer_ms`: ten minutes. */

/** @brief What an argument is, and how it is spelled on the wire. */
typedef enum {
    KODE_TYPE_NONE = 0, /**< No argument: the function takes none.        */
    KODE_TYPE_INT,      /**< int32_t. A signed decimal on the wire: -12, 235. */
    KODE_TYPE_BOOL,     /**< `on` or `off`.                                   */
    KODE_TYPE_ENUM,     /**< One word of the row's list; the index travels.  */
    KODE_TYPE_STRING    /**< Quoted text, at most KODE_ARG_STR_MAX chars.    */
} kode_type_t;

/**
 * @brief One argument, in the field its row's type names.
 *
 * A struct rather than a union, the shape the console's own karg_val_t has:
 * a case reads exactly one field and the others are zero.
 */
typedef struct {
    int32_t     i; /**< KODE_TYPE_INT.                                       */
    bool        b; /**< KODE_TYPE_BOOL.                                      */
    uint8_t     e; /**< KODE_TYPE_ENUM: index into the row's enums.          */
    const char *s; /**< KODE_TYPE_STRING. Valid for the duration of the call:
                    *   copy what you want to keep.                          */
} kode_value_t;

/* Runs on AO_Console's task inside one step: return promptly and never block (no kode.h call). */
/**
 * @brief Run the function.
 * @param arg The argument when the line carried `arg=`, NULL otherwise.
 *            Always NULL for a row whose arg is KODE_TYPE_NONE.
 * @return KODE_OK once accepted; KODE_DENIED to refuse the argument (or its
 *         absence); anything else is the row's failure.
 */
typedef kode_status_t (*kode_function_run_fn)(const kode_value_t *arg);

/** @brief One row of the app's table. Every field but name and run has a
 *         default: no argument, no description. */
typedef struct {
    const char           *name;    /**< [a-z0-9_], 1..KODE_ROW_NAME_MAX,
                                    *   unique over every variable and
                                    *   function.                          */
    const char           *desc;    /**< Prose for function_list. NULL: `-`. */
    kode_type_t           arg;     /**< The argument's type; NONE takes none.*/
    int32_t               min;     /**< KODE_TYPE_INT only. min == max:
                                    *   unbounded.                         */
    int32_t               max;     /**< KODE_TYPE_INT only.                 */
    const char *const    *enums;   /**< KODE_TYPE_ENUM: the words, each a
                                    *   valid name.                        */
    uint8_t               n_enums; /**< 1..KODE_VALUE_ENUMS_MAX.            */
    kode_function_run_fn  run;     /**< Required.                           */
    uint32_t              answer_ms; /**< 0: `>ok` on accept. Otherwise the line
                                    *   stays open this long for records and
                                    *   function_end(); at most
                                    *   KODE_FUNCTION_ANSWER_MS_MAX.        */
} kode_function_t;

/**
 * @brief Register one table of functions. Call after kode_init().
 *
 * The table is kept by pointer, so it must be `static const` and outlive the
 * firmware. A table with one bad row is refused whole, with a log line
 * naming the row.
 *
 * @param table The rows.
 * @param n     How many.
 * @return KODE_OK; KODE_EXISTS for a name already taken; KODE_NO_RESOURCES
 *         when KODE_FUNCTIONS_MAX or KODE_FUNCTION_TABLES would be exceeded;
 *         KODE_DENIED for any other rule, and always in a build without
 *         CONFIG_KODEOS_CONSOLE, where there is nothing to reach the table.
 */
kode_status_t kode_functions_add(const kode_function_t *table, size_t n);

/** @brief kode_functions_add() over a whole array. */
#define KODE_FUNCTIONS_ADD(t) kode_functions_add((t), sizeof(t) / sizeof((t)[0]))

/* ---- A function that answers --------------------------------------------
 * A row with answer_ms set keeps the line open while the AO the run posted
 * to answers: records through function_record(), the end through
 * function_end().
 */

/* cppcheck-suppress misra-c2012-2.5 ; a size for the apps that quote records, never used inside the SDK */
/** @brief Bytes @p n characters of text take once function_quote() has
 *         quoted them, terminator included. */
#define FUNCTION_QUOTED_MAX(n)  ((2U * (size_t)(n)) + 3U)

/**
 * @brief The ticket of the function being run: what its owner must echo.
 *
 * Call it from the `run` callback of a row with `answer_ms` and hand the
 * number to the owner with the request. Each run gets a new ticket, so a
 * late answer is told from the one in flight.
 *
 * @return The ticket; never 0.
 */
uint32_t function_ticket(void);

/**
 * @brief Send one record of the answer of the function with @p ticket.
 *
 * Dropped when that function is not the one waiting. Posted with a margin:
 * a full queue is a false, and the console's watchdog closes the line.
 *
 * @param ticket  What function_ticket() gave the run.
 * @param record  The record without its `>|`: `assets source=sd ...`. Text
 *                values go through function_quote().
 * @return true when the record is in AO_Console's queue; false for a NULL
 *         record, one of KODE_FUNCTION_REC_MAX characters or more (it is
 *         refused, never cut), or a full pool or queue.
 */
bool function_record(uint32_t ticket, const char *record);

/**
 * @brief End the function in flight: `>ok`, or the `>err` its status names.
 *
 * KODE_OK closes with `>ok`; KODE_DENIED with `>err code=3 msg=arg`; any
 * other status with `>err code=5 msg=<msg>`.
 *
 * @param ticket What function_ticket() gave the run.
 * @param status How it went.
 * @param msg    The word after `msg=` of a code 5, one word of at most 15
 *               characters; NULL for the function's own name.
 * @return true when the end is in AO_Console's queue. A false leaves the
 *         line open until `answer_ms`: the caller posts it again.
 */
bool function_end(uint32_t ticket, kode_status_t status, const char *msg);

/**
 * @brief Write @p src as one quoted value of a record, the way every verb of
 *        the console writes text.
 *
 * `"`, `\` and a newline are escaped. Size @p dst with FUNCTION_QUOTED_MAX().
 *
 * @param dst Where to write.
 * @param cap Its size, at least 3.
 * @param src The text; NULL is an empty value.
 * @return @p dst.
 */
const char *function_quote(char *dst, size_t cap, const char *src);

#ifdef __cplusplus
}
#endif

#endif /* KODE_FUNCTIONS_H_ */
