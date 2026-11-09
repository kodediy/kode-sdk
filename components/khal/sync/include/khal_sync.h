/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_sync.h
 * @brief A mutex and a binary semaphore, for the few places no Active Object owns.
 */
#ifndef KHAL_SYNC_H_
#define KHAL_SYNC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Mutex ---- */

/** @brief A non-recursive mutex. */
typedef void *khal_mutex_t;

/** @brief Create a mutex. @return NULL if there is no memory for one. */
khal_mutex_t khal_mutex_create(void);
/**
 * @brief Take the lock, blocking for as long as it takes.
 * @param m The mutex, from khal_mutex_create().
 */
void khal_mutex_lock(khal_mutex_t m);

/**
 * @brief Release the lock.
 * @param m The mutex the caller holds.
 */
void khal_mutex_unlock(khal_mutex_t m);

/* ---- Binary semaphore ---- */

/** @brief A binary semaphore. */
typedef void *khal_sem_t;

/**
 * @brief Create a binary semaphore, not signalled.
 * @return NULL if there is no memory for one.
 */
khal_sem_t khal_sem_create(void);

/**
 * @brief Wait until the semaphore is signalled, or until @p ms elapses.
 * @param s  The semaphore.
 * @param ms How long to wait; 0 takes a pending signal without waiting.
 * @return true when it was signalled, false when the time ran out.
 */
bool khal_sem_wait_timeout(khal_sem_t s, uint32_t ms);

/**
 * @brief Free a semaphore.
 * @param s The semaphore, from khal_sem_create(); NULL does nothing. No task
 *          may wait on it, and none may use it afterwards.
 */
void khal_sem_delete(khal_sem_t s);

/**
 * @brief Signal the semaphore, from a task or from an ISR.
 * @param s The semaphore.
 * @note In IRAM: the DSI ISR calls it with the flash cache off.
 */
void khal_sem_signal(khal_sem_t s);

/* ---- The C library's environment lock ---- */

/**
 * @brief Take the C library's environment lock, blocking for as long as it takes.
 *
 * The lock setenv(), getenv(), tzset() and localtime_r() take inside the
 * library, so a caller that holds it makes several of those calls one step
 * that no other task's call can come between. Recursive: the library's own
 * calls take it again inside. Held for microseconds; khal_time_zone_set() and
 * khal_time_local() are its callers.
 */
void khal_env_lock(void);

/** @brief Release the lock khal_env_lock() took, once per take. */
void khal_env_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_SYNC_H_ */
