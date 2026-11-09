/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kregistry.h
 * @brief The registry's console-facing half: the variables, the functions and the settings.
 */
#ifndef KREGISTRY_H_
#define KREGISTRY_H_

#include <stddef.h>

#include "kode_config.h"
#include "kode_functions.h"
#include "kode_variables.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether @p s is a name the console's registries take: [a-z0-9_], 1..@p max.
 * @param s   The name, or NULL.
 * @param max The most characters it may have.
 * @return true when it is one.
 */
bool kregistry_name_ok(const char *s, size_t max);

/**
 * @brief The buffer kvariables_read() fills: KODE_VALUE_STR_MAX characters,
 *        one more to catch a longer word, and the terminator.
 */
#define KREGISTRY_WORD_MAX  (KODE_VALUE_STR_MAX + 2U)

/* ---- Variables -------------------------------------------------------------- */

/**
 * @brief How many variables are registered, over every table.
 * @return The total, 0..KODE_VARIABLES_MAX.
 */
size_t kvariables_count(void);

/**
 * @brief Variable @p i in registration order: tables first, rows within them.
 * @param i 0..kvariables_count()-1.
 * @return The row, or NULL past the end.
 */
const kode_variable_t *kvariables_at(size_t i);

/**
 * @brief The variable called @p name. Exact, case-sensitive.
 * @param name The word after `name=`.
 * @return The row, or NULL - a function of that name included.
 */
const kode_variable_t *kvariables_find(const char *name);

/**
 * @brief Read @p row: its getter's word, made fit for the wire.
 *
 * Zeroes @p buf and calls `get`. Whitespace, control characters and quotes
 * become `_`, since the value goes out as one bare word; an empty word or
 * one longer than KODE_VALUE_STR_MAX is refused, never cut.
 *
 * @param row The row.
 * @param buf At least KREGISTRY_WORD_MAX bytes.
 * @param cap Its size.
 * @return KODE_OK; whatever `get` answered when that was not KODE_OK; or
 *         KODE_DENIED for an empty or overlong word, or a buffer under
 *         KREGISTRY_WORD_MAX.
 */
kode_status_t kvariables_read(const kode_variable_t *row, char *buf, size_t cap);

/**
 * @brief Forget every variable table. For the host suite.
 *
 * The console's watch slots hold row pointers, so a case stops every watch
 * before calling this.
 */
void kvariables_reset(void);

/* ---- Functions -------------------------------------------------------------- */

/**
 * @brief How many functions are registered, over every table.
 * @return The total, 0..KODE_FUNCTIONS_MAX.
 */
size_t kfunctions_count(void);

/**
 * @brief Function @p i in registration order: tables first, rows within them.
 * @param i 0..kfunctions_count()-1.
 * @return The row, or NULL past the end.
 */
const kode_function_t *kfunctions_at(size_t i);

/**
 * @brief The function called @p name. Exact, case-sensitive.
 * @param name The word after `name=`.
 * @return The row, or NULL - a variable of that name included.
 */
const kode_function_t *kfunctions_find(const char *name);

/**
 * @brief Parse @p word against @p row's argument type into @p out.
 *
 * INT: an optional `-` and digits, within int32_t and within [min, max] when
 * they differ. BOOL: `on` or `off`. ENUM: one of the row's words. STRING: any
 * text of at most KODE_ARG_STR_MAX characters. A row whose argument is
 * KODE_TYPE_NONE takes no word at all.
 *
 * @param row  The row.
 * @param word The value after `arg=`, its quotes and escapes already read.
 * @param out  Zeroed, then the one field the type names is set.
 * @return KODE_OK, or KODE_DENIED for anything the type does not accept.
 */
kode_status_t kfunctions_parse(const kode_function_t *row, const char *word,
                             kode_value_t *out);

/** @brief Forget every function table. For the host suite; see kvariables_reset(). */
void kfunctions_reset(void);

/* ---- Settings --------------------------------------------------------------- */

/**
 * @brief How many settings are registered, over every table.
 * @return The total, 0..KODE_CONFIG_MAX.
 */
size_t kconfig_count(void);

/**
 * @brief Setting @p i in registration order: tables first, rows within them.
 * @param i 0..kconfig_count()-1.
 * @return The row, or NULL past the end.
 */
const kode_config_t *kconfig_at(size_t i);

/**
 * @brief The setting called @p name. Exact, case-sensitive.
 * @param name The word after `key=`.
 * @return The row, or NULL.
 */
const kode_config_t *kconfig_find(const char *name);

/**
 * @brief The AO that answers for @p row, and the room its requests leave.
 * @param row    A row of a registered table.
 * @param margin Set to the margin the table was registered with, when the
 *               row is found. May be NULL.
 * @return The owner, or NULL for a row no table holds (NULL included).
 */
QActive *kconfig_owner(const kode_config_t *row, uint16_t *margin);

/**
 * @brief Parse @p word against @p row's type into @p out.
 * The rules of kfunctions_parse(), with a string held to KODE_VALUE_STR_MAX.
 * @param row  The row.
 * @param word The value after `value=`, its quotes and escapes already read.
 * @param out  Zeroed, then the one field the type names is set.
 * @return KODE_OK, or KODE_DENIED for anything the type does not accept.
 */
kode_status_t kconfig_parse(const kode_config_t *row, const char *word,
                            kode_value_t *out);

/** @brief Forget every setting table. For the host suite; see kvariables_reset(). */
void kconfig_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* KREGISTRY_H_ */
