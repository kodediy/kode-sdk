/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_entry.h
 * @brief The standard layer's prologue, per-task error text and retry loop. Private to kode.
 */
#ifndef KODE_ENTRY_H_
#define KODE_ENTRY_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_internal.h"   /* kode_enter(), kode_err_set(), kode_retry(), kode_name_is() */

/** @brief Bytes of a task's error text, the terminating NUL included. */
#define KODE_ERROR_MAX 128U

/** @brief How long kode_retry() sleeps between two tries, in ms. */
#define KODE_RETRY_STEP_MS 10U

/** @brief Milliseconds in a second. */
#define KODE_MS_PER_S 1000U

/**
 * @brief Whether @p text starts with @p prefix, ignoring ASCII case.
 * @param text   What the caller typed.
 * @param prefix The start to match, already in lower case.
 * @return true when @p text begins with @p prefix; an empty prefix always does.
 */
bool kode_has_prefix(const char *text, const char *prefix);

#endif /* KODE_ENTRY_H_ */
