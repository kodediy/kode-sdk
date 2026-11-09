/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ask_support.c
 * @brief See ask_support.h.
 */
#include "ask_support.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/** @brief How long the parent sleeps between looks at the child, in nanoseconds. */
#define ASK_SUPPORT_POLL_NS  1000000L

/** @brief Milliseconds in a second. */
#define ASK_SUPPORT_MS_PER_S  1000U

/** @brief Nanoseconds in a millisecond. */
#define ASK_SUPPORT_NS_PER_MS  1000000U

/**
 * @brief The monotonic clock in milliseconds.
 * @return Milliseconds since an arbitrary start.
 */
static uint64_t now_ms(void)
{
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * ASK_SUPPORT_MS_PER_S) + ((uint64_t)ts.tv_nsec / ASK_SUPPORT_NS_PER_MS);
}

/**
 * @brief Read how a reaped child ended.
 * @param status What waitpid() wrote: an int, because its signature says so.
 * @return The outcome.
 */
static ask_support_child_t classify(int status)
{
    if ( WIFSIGNALED(status) && (WTERMSIG(status) == SIGABRT) )
    {
        return ASK_CHILD_ABORTED;
    }
    if ( WIFEXITED(status) && (WEXITSTATUS(status) == 0) )
    {
        return ASK_CHILD_RETURNED;
    }
    return ASK_CHILD_OTHER;
}

ask_support_child_t ask_support_fork_run(ask_support_body_t body, void *arg)
{
    struct rlimit   no_core = { 0, 0 };
    struct timespec nap     = { 0, ASK_SUPPORT_POLL_NS };
    uint64_t        start;
    pid_t           pid;
    pid_t           got;
    int             status = 0;   /* waitpid()'s out parameter is an int * */

    (void)fflush(stdout);
    pid = fork();
    if ( 0 > pid )
    {
        return ASK_CHILD_FORK_FAILED;
    }
    if ( 0 == pid )
    {
        /* The runner catches an abort to report it; the child must die of it, and leave no core. */
        (void)setrlimit(RLIMIT_CORE, &no_core);
        (void)signal(SIGABRT, SIG_DFL);
        body(arg);
        _exit(0);
    }

    start = now_ms();
    for ( ;; )
    {
        got = waitpid(pid, &status, WNOHANG);
        if ( got == pid )
        {
            return classify(status);
        }
        if ( (0 > got) && (EINTR != errno) )
        {
            return ASK_CHILD_OTHER;
        }
        if ( (now_ms() - start) > ASK_SUPPORT_CHILD_TIMEOUT_MS )
        {
            (void)kill(pid, SIGKILL);
            do
            {
                got = waitpid(pid, &status, 0);
            } while ( (0 > got) && (EINTR == errno) );
            return ASK_CHILD_HUNG;
        }
        (void)nanosleep(&nap, NULL);
    }
}
