/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_last.h
 * @brief The summary of the last rest: how AO_LowPower builds and keeps it.
 */
#ifndef LOWPOWER_LAST_H_
#define LOWPOWER_LAST_H_

#include <stdbool.h>
#include <stdint.h>

#include "kaos.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Start a summary for a rest that begins now.
 *
 * @param l       The summary, any contents.
 * @param start_s The wall clock in Unix seconds; 0 when it is not set.
 */
void lowpower_last_begin(lowpower_last_t *l, uint32_t start_s);

/**
 * @brief Add one gauge sample, dropping the oldest once the ring is full.
 *
 * @param l   The summary.
 * @param t_s Seconds from the start of the rest.
 * @param ma  The cell's current, into it positive.
 * @param mv  The cell's voltage.
 */
void lowpower_last_add(lowpower_last_t *l, uint32_t t_s, int16_t ma, uint16_t mv);

/**
 * @brief Write the summary to the store as it stands.
 *
 * @param l The summary.
 * @return true when the store took it.
 */
bool lowpower_last_save(const lowpower_last_t *l);

#ifdef __cplusplus
}
#endif

#endif /* LOWPOWER_LAST_H_ */
