/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_display_compose_none.c
 * @brief The compose call of an image built without CONFIG_KODE_COMPOSE: it refuses, and says why.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kaos.h"

/* This binary is built with the option on, so display_compose_none.c is
 * compiled beside the real call under another name (main/CMakeLists.txt). */
kode_status_t display_compose_run_none(display_compose_ask_t *ask, display_compose_t const *job,
                                       uint32_t timeout_ms, uint32_t *us);

/** @brief Whether a log line holds @p what. */
static bool logged(const char *what)
{
    bool seen = false;

    for ( unsigned i = 0U; (fake_klog_count() > i) && !seen; ++i )
    {
        const char * const l = fake_klog_line(i);

        seen = (NULL != l) && (strstr(l, what) != NULL);
    }
    return seen;
}

TEST_CASE("without the option a compose pass is refused at once, and the log says it is not built",
          "[display_compose_none]")
{
    static display_compose_ask_t ask;
    display_compose_t            job;
    uint32_t                     us      = 7U;
    unsigned const               created = fake_sem_creates();

    (void)memset(&job, 0, sizeof(job));
    fake_klog_reset();

    TEST_ASSERT_EQUAL(KODE_DENIED, display_compose_run_none(&ask, &job, 500U, &us));
    TEST_ASSERT_TRUE(logged("compose not built (CONFIG_KODE_COMPOSE)"));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(7U, us, "nothing is answered");
    TEST_ASSERT_NULL_MESSAGE(ask.done, "nothing is taken to wait on");
    TEST_ASSERT_EQUAL_UINT(created, fake_sem_creates());
}
