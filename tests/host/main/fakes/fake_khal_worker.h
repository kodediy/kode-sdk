/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_khal_worker.h
 * @brief The control surface of the faked worker task (fake_khal_worker.c).
 */
#ifndef FAKE_KHAL_WORKER_H_
#define FAKE_KHAL_WORKER_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Drop every queued job and forget which worker was created last.
 *
 * The workers themselves stay: an Active Object built once per binary keeps
 * the worker it created, and a case that builds its AO afresh creates the
 * same one again by name.
 */
void fake_khal_worker_reset(void);

/**
 * @brief Run the oldest queued job of the most recently created worker, on
 *        the caller's thread.
 * @return true if a job ran, false if the queue was empty.
 */
bool fake_khal_worker_run_next(void);

/**
 * @brief Run the oldest queued job of the worker called @p name.
 * @param name The name the worker was created with.
 * @return true if a job ran, false if the queue was empty or there is none.
 */
bool fake_khal_worker_run_next_for(char const *name);

/** @brief How many jobs the most recently created worker has queued. */
unsigned fake_khal_worker_pending(void);

/**
 * @brief How many jobs the worker called @p name has queued.
 * @param name The name the worker was created with.
 * @return The count; 0 for a name no worker was created with.
 */
unsigned fake_khal_worker_pending_for(char const *name);

/**
 * @brief Make the worker called @p name answer every submit with no room,
 *        as a queue that is full does; the reset lifts it.
 * @param name   The name the worker was created with.
 * @param refuse true to refuse, false to take jobs again.
 */
void fake_khal_worker_refuse_for(char const *name, bool refuse);

/** @brief The @c psram_stack the most recent khal_worker_create() was given. */
bool fake_khal_worker_last_psram(void);

/**
 * @brief The @c psram_stack the worker called @p name was created with.
 * @param name The name the worker was created with.
 * @return The flag; false for a name no worker was created with.
 */
bool fake_khal_worker_last_psram_for(char const *name);

/**
 * @brief Set what khal_worker_stack_free() answers for the worker called @p name.
 *
 * A worker reads its whole stack as unused until a case says otherwise.
 *
 * @param name  The name the worker was created with.
 * @param bytes The unused stack to report.
 */
void fake_khal_worker_set_stack_free_for(char const *name, uint32_t bytes);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_KHAL_WORKER_H_ */
