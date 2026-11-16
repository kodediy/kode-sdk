/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_compose.h
 * @brief Package-private: AO_Display's side of display_compose_run(), and its self-test row.
 */
#ifndef DISPLAY_COMPOSE_H_
#define DISPLAY_COMPOSE_H_

#include <stdbool.h>

#include "sdkconfig.h"

#include "kqp.h"

#if defined(CONFIG_KODE_COMPOSE)
/**
 * @brief Run a request's pass and answer it. AO_Display's step.
 *
 * A request whose asker stopped waiting is dropped unanswered. With the
 * glass asleep the pass is refused, KODE_DENIED, and not run.
 *
 * @param q The request.
 */
void display_compose_answer(DisplayComposeReqEvt const *q);
#endif

#if defined(CONFIG_KODEOS_AUTOTEST) && defined(CONFIG_KODE_COMPOSE)
/**
 * @brief The `compose` row: three passes judged by their pixels, then frames timed.
 *
 * Runs on AO_Autotest's thread and asks AO_Display as an app's renderer
 * does. Answers one `>|compose` record.
 *
 * @return true when every pass answered and every pixel read back right.
 */
bool display_compose_row(void);
#endif

#endif /* DISPLAY_COMPOSE_H_ */
