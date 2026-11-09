/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ask_support.h
 * @brief A body run in a forked child, for what QP answers with an abort.
 */
#ifndef ASK_SUPPORT_H_
#define ASK_SUPPORT_H_

/** @brief Milliseconds a child may run before it counts as hung and is killed. */
#define ASK_SUPPORT_CHILD_TIMEOUT_MS  10000U

/** @brief How a forked body ended. */
typedef enum {
    ASK_CHILD_ABORTED = 0,  /**< Died of SIGABRT: QP's assert.                     */
    ASK_CHILD_RETURNED,     /**< Ran to its end.                                   */
    ASK_CHILD_OTHER,        /**< Died of another signal, or exited with a status.  */
    ASK_CHILD_HUNG,         /**< Still running at the timeout; killed.             */
    ASK_CHILD_FORK_FAILED   /**< The fork itself failed.                           */
} ask_support_child_t;

/** @brief A body to run in the child. */
typedef void (*ask_support_body_t)(void *arg);

/**
 * @brief Run @p body in a forked child and report how it ended.
 *
 * The child has no core file and the default SIGABRT action, which the runner
 * otherwise catches. The parent polls with a timeout of its own: an alarm in
 * the child would be eaten by the port's SIGALRM tick handler.
 *
 * @param body The body.
 * @param arg  Its argument.
 * @return How the child ended.
 */
ask_support_child_t ask_support_fork_run(ask_support_body_t body, void *arg);

#endif /* ASK_SUPPORT_H_ */
