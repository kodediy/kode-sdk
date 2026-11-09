/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_init.h
 * @brief kode_init(), declared once for kode.h and kode_sys.h.
 */
#ifndef KODE_INIT_H_
#define KODE_INIT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bring kodeOS up: logging, QF, the pools, the core and every Active
 *        Object the app links; an owner it does not link has its part parked.
 * @note Returns once they run. Safe to call again or from several tasks: the
 *       first does the work, the others wait for it. Never call it from its own
 *       bring-up (a hook or an AO it starts): it would wait for itself.
 */
void kode_init(void);

/**
 * @brief Whether every owner kode_init() starts has its queue.
 *
 * An interface that starts with the first rows (kode_app_early()) runs while
 * kode_init() walks the rest, and a post to an owner with no queue yet is an
 * assert. What it asks those owners waits for this.
 *
 * @return true once kode_init() has walked all its rows.
 */
bool kode_up(void);

#ifdef __cplusplus
}
#endif

#endif /* KODE_INIT_H_ */
