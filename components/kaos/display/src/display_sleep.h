/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_sleep.h
 * @brief The two words that say whether the glass is at rest and how the last
 *        request to sleep or wake it ended: AO_Display's side. Private to kaos.
 */
#ifndef DISPLAY_SLEEP_H_
#define DISPLAY_SLEEP_H_

#include <stdbool.h>

#include "kode_defs.h"
#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Note whether the glass is at rest; display_asleep() reads it.
 * @note AO_Display's thread alone.
 * @param asleep true once the link is down, false once the glass is back.
 */
void display_sleep_glass_set(bool asleep);

/**
 * @brief Note how a request ended and count it; display_sleep_last() reads it.
 * @note AO_Display's thread alone, just before it publishes DISPLAY_SLEEP_SIG.
 * @param rc  KODE_OK when the glass is as asked.
 * @param why Why not, when @p rc is not KODE_OK.
 */
void display_sleep_answer_set(kode_status_t rc, display_sleep_why_t why);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_SLEEP_H_ */
