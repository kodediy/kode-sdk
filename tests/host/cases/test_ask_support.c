/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_ask_support.c
 * @brief ask_support_fork_run(): how the end of a body run in a child is read.
 */
#include <stdio.h>
#include <stdlib.h>

#include "unity.h"

#include "ask_support.h"

/** @brief A body that runs to its end. */
static void body_returns(void *arg)
{
    (void)arg;
}

/** @brief A body that dies the way QP's assert does. */
static void body_aborts(void *arg)
{
    (void)arg;
    abort();
}

/** @brief A body whose assertion fails: the failure belongs to the child,
 *         whose output is dropped so the run's log shows no FAIL line. */
static void body_fails(void *arg)
{
    /* Kept, not cast away: glibc marks freopen() warn_unused_result, and GCC
     * warns about a (void) cast of it all the same. */
    FILE *dropped = freopen("/dev/null", "w", stdout);

    (void)arg;
    (void)dropped;
    TEST_FAIL_MESSAGE("a failure inside the forked body, on purpose");
}

TEST_CASE("a body that runs to its end is read as returned", "[ask_support]")
{
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ASK_CHILD_RETURNED,
                             (uint32_t)ask_support_fork_run(&body_returns, NULL));
}

TEST_CASE("a body that aborts is read as aborted", "[ask_support]")
{
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ASK_CHILD_ABORTED,
                             (uint32_t)ask_support_fork_run(&body_aborts, NULL));
}

TEST_CASE("an assertion that fails in the body ends the child there", "[ask_support]")
{
    /* Before, the failure left by longjmp to the runner's frame in the
     * child's copy of the stack: the child ran the rest of the suite as a
     * second runner, past the parent's timeout, and was read as hung. */
    TEST_ASSERT_EQUAL_UINT32((uint32_t)ASK_CHILD_FAILED,
                             (uint32_t)ask_support_fork_run(&body_fails, NULL));
}
