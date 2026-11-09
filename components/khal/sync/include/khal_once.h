/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_once.h
 * @brief A one-shot: run a body once, whoever calls and however many do.
 */
#ifndef KHAL_ONCE_H_
#define KHAL_ONCE_H_

#include <stdatomic.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The state of a one-shot nobody has run (the zeroed state). */
#define KHAL_ONCE_IDLE     0

/** @brief The state while the winner's body is running. */
#define KHAL_ONCE_RUNNING  1

/** @brief The state once a body has returned for good. */
#define KHAL_ONCE_DONE     2

/** @brief How long a caller that lost the election sleeps between looks, in ms. */
#define KHAL_ONCE_POLL_MS  1U

/* An atomic word, not a mutex: a mutex must be created, and two first callers would race to. */
/** @brief A one-shot; a static one starts in its zeroed state, ready to run. */
typedef struct {
    atomic_int state;   /**< KHAL_ONCE_IDLE, _RUNNING or _DONE. */
} khal_once_t;

/** @brief What khal_once_run() runs. */
typedef void (*khal_once_body_t)(void);

/** @brief What khal_once_try() runs: true when the work was done. */
typedef bool (*khal_once_try_body_t)(void);

/**
 * @brief Run @p body on the first call and on no other.
 *
 * Safe from any number of tasks at once. The first caller runs the body in
 * its own thread; every other caller, whether it arrives during the body or
 * after, returns only once the body has returned. The wait sleeps; it never
 * spins.
 *
 * @param once The one-shot, zero-initialised.
 * @param body What to run; called at most once per one-shot.
 */
void khal_once_run(khal_once_t *once, khal_once_body_t body);

/**
 * @brief Run @p body until one run reports success.
 *
 * Like khal_once_run(), except that a body which answers false leaves the
 * one-shot as it was found, so the next caller tries again. A caller that
 * waited on a winner whose body failed becomes the next winner itself.
 *
 * @param once The one-shot, zero-initialised.
 * @param body What to run; true when the work is done, false when it failed.
 * @return true when the work is done (by this call or an earlier one); false
 *         when this caller's own run failed.
 */
bool khal_once_try(khal_once_t *once, khal_once_try_body_t body);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_ONCE_H_ */
