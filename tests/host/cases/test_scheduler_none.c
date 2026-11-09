/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_scheduler_none.c
 * @brief scheduler_none.c, the doors of a build without CONFIG_KODE_SCHEDULER.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kode_log.h"

#define sched_request_set    none_sched_request_set
#define sched_request_set_ao none_sched_request_set_ao
#define sched_request_remove none_sched_request_remove
#define sched_request_list   none_sched_request_list
#define sched_line_done      none_sched_line_done
#define sched_line_busy      none_sched_line_busy
#define sched_check_done     none_sched_check_done
#include "scheduler_none.c"
#undef sched_request_set
#undef sched_request_set_ao
#undef sched_request_remove
#undef sched_request_list
#undef sched_line_done
#undef sched_line_busy
#undef sched_check_done

bool none_sched_request_set(QActive *who, const char *name, const char *when,
                            const char *cmd);
bool none_sched_request_set_ao(QActive *who, const char *name, const char *when,
                               QActive *target, enum_t sig);
bool none_sched_request_remove(QActive *who, const char *name);
bool none_sched_request_list(QActive *who, const char *name);
void none_sched_line_done(void);
void none_sched_line_busy(void);
void none_sched_check_done(int64_t passed_s);

/** @brief The signal an entry of code would be told with. */
#define NONE_FIRE_SIG ((enum_t)MAX_SIG)

/** @brief Lines in the log that contain @p needle. */
static unsigned none_logged(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);

        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            ++n;
        }
    }
    return n;
}

TEST_CASE("without the scheduler every door refuses, says so and posts nothing",
          "[scheduler_none]")
{
    (void)ao_spy();
    ao_spy_drain();
    fake_klog_reset();

    TEST_ASSERT_FALSE(none_sched_request_set(ao_spy(), "tick", "* * * * *", "ping"));
    TEST_ASSERT_FALSE(none_sched_request_set_ao(ao_spy(), "code", "* * * * *", ao_spy(),
                                                NONE_FIRE_SIG));
    TEST_ASSERT_FALSE(none_sched_request_remove(ao_spy(), "code"));
    TEST_ASSERT_FALSE(none_sched_request_list(ao_spy(), NULL));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(4U, none_logged("the scheduler is not built"),
                                   "each refusal is said");

    {
        unsigned const lines = fake_klog_count();

        none_sched_line_done();
        none_sched_line_busy();
        none_sched_check_done(INT64_C(1793385480));
        TEST_ASSERT_EQUAL_UINT_MESSAGE(lines, fake_klog_count(), "the three notes say nothing");
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "and nobody is answered or told");
}
