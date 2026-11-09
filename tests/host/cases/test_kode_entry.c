/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_entry.c
 * @brief The standard layer's prologue: kode_enter(), kode_err_set(), kode_error(), kode_retry().
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "probe_ao.h"
#include "qf_support.h"

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of a test task. */
#define TEST_PRIO 1U

/** @brief The longest a case waits for a task, in ms. */
#define TEST_WAIT_MS 4000U


/** @brief A bound that no attempt count here comes near, in ms. */
#define TEST_BOUND_MS 100U

/** @brief Attempts a case lets a retry make before it succeeds. */
#define TEST_SUCCEED_ON 3U

/** @brief Attempts a retry without a bound makes before it succeeds. */
#define TEST_FOREVER_ATTEMPTS 25U

/** @brief What a retried attempt counts and answers. */
typedef struct {
    uint32_t calls;       /**< Times the attempt ran.                    */
    uint32_t succeed_on;  /**< The call that answers true; 0 = never.    */
} attempts_t;

/** @brief The attempt kode_retry() is given: true on the configured call. */
static bool attempt_counted(void *ctx)
{
    attempts_t *const a = (attempts_t *)ctx;

    ++a->calls;
    return (0U != a->succeed_on) && (a->calls >= a->succeed_on);
}

/* ---- A function run in a task of its own ---------------------------------- */

/** @brief What a job run in a fresh task leaves behind. */
typedef struct {
    SemaphoreHandle_t done;                 /**< Given when the job returns. */
    char              before[KODE_ERROR_MAX]; /**< kode_error() on entry.     */
    char              after[KODE_ERROR_MAX];  /**< kode_error() on exit.      */
    const char       *set;                  /**< Error the job sets, or NULL. */
} job_t;

/** @brief Copy @p s into @p out, always terminated. */
static void text_copy(char *out, const char *s)
{
    (void)strncpy(out, s, KODE_ERROR_MAX - 1U);
    out[KODE_ERROR_MAX - 1U] = '\0';
}

/** @brief The job: read the error, optionally set one, read it again. */
static void job_task(void *arg)
{
    job_t *const j = (job_t *)arg;

    text_copy(j->before, kode_error());
    if ( NULL != j->set )
    {
        kode_err_set("%s", j->set);
    }
    text_copy(j->after, kode_error());
    (void)xSemaphoreGive(j->done);
}

/**
 * @brief Run a job in a fresh task and wait for it.
 * @return true when the task ran to its end in time.
 */
static bool job_run(job_t *j, const char *set)
{
    j->done = xSemaphoreCreateBinary();
    j->set  = set;
    j->before[0] = '\0';
    j->after[0]  = '\0';

    bool ok = (khal_task_start(&job_task, "t_entry", TEST_STACK_BYTES,
                               TEST_PRIO, j) == KODE_OK);
    if ( ok )
    {
        ok = (xSemaphoreTake(j->done, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    vSemaphoreDelete(j->done);
    return ok;
}

/** @brief Make kode_init() a no-op, as it is once the system is up. */
static void system_is_up(void)
{
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

/* ---- kode_retry ----------------------------------------------------------- */

TEST_CASE("a retry that succeeds on the third try stops there", "[kode_entry]")
{
    attempts_t a = { 0U, TEST_SUCCEED_ON };

    TEST_ASSERT_TRUE(kode_retry(&attempt_counted, &a, TEST_BOUND_MS));
    TEST_ASSERT_EQUAL_UINT32(TEST_SUCCEED_ON, a.calls);
}

TEST_CASE("a retry gives up after its bound", "[kode_entry]")
{
    attempts_t     a = { 0U, 0U };
    const uint32_t t0 = kode_sys_time_ms();

    TEST_ASSERT_FALSE(kode_retry(&attempt_counted, &a, TEST_BOUND_MS));

    const uint32_t spent = kode_sys_time_ms() - t0;

    /* It tried again and again, never more than once per step of the bound
     * plus the first, and it did not give up before the bound had passed.
     * The clock is shared with every task in the binary, so nothing here
     * asserts how close to the bound it stopped. */
    TEST_ASSERT_GREATER_THAN_UINT32(1U, a.calls);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32((TEST_BOUND_MS / KODE_RETRY_STEP_MS) + 1U, a.calls);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(TEST_BOUND_MS, spent);
}

TEST_CASE("a retry with a bound of zero tries once", "[kode_entry]")
{
    attempts_t a = { 0U, 0U };

    TEST_ASSERT_FALSE(kode_retry(&attempt_counted, &a, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, a.calls);
}

TEST_CASE("a retry with no bound goes on until it succeeds", "[kode_entry]")
{
    attempts_t a = { 0U, TEST_FOREVER_ATTEMPTS };

    TEST_ASSERT_TRUE(kode_retry(&attempt_counted, &a, KODE_FOREVER));
    TEST_ASSERT_EQUAL_UINT32(TEST_FOREVER_ATTEMPTS, a.calls);
}

TEST_CASE("a retry without an attempt gives up at once", "[kode_entry]")
{
    TEST_ASSERT_FALSE(kode_retry(NULL, NULL, TEST_BOUND_MS));
}

/* ---- kode_error ----------------------------------------------------------- */

TEST_CASE("the error is empty in a task that never had one", "[kode_entry]")
{
    job_t j;

    TEST_ASSERT_TRUE(job_run(&j, NULL));
    TEST_ASSERT_EQUAL_STRING("", j.before);
    TEST_ASSERT_EQUAL_STRING("", j.after);
}

TEST_CASE("the error is each task's own", "[kode_entry]")
{
    job_t j;

    kode_err_set("main task %d", 1);
    TEST_ASSERT_EQUAL_STRING("main task 1", kode_error());

    /* A new task starts clean, sets its own, and leaves ours alone. */
    TEST_ASSERT_TRUE(job_run(&j, "child task"));
    TEST_ASSERT_EQUAL_STRING("", j.before);
    TEST_ASSERT_EQUAL_STRING("child task", j.after);
    TEST_ASSERT_EQUAL_STRING("main task 1", kode_error());
}

TEST_CASE("a later error replaces the earlier one", "[kode_entry]")
{
    kode_err_set("first");
    kode_err_set("second %s", "one");
    TEST_ASSERT_EQUAL_STRING("second one", kode_error());
}

TEST_CASE("a successful call clears the error", "[kode_entry]")
{
    system_is_up();
    kode_err_set("boom %d", 7);
    TEST_ASSERT_EQUAL_STRING("boom 7", kode_error());

    TEST_ASSERT_TRUE(kode_enter("kode_probe"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("a long error is cut to the maximum and ends in an ellipsis", "[kode_entry]")
{
    char long_text[KODE_ERROR_MAX * 2U];

    (void)memset(long_text, 'x', sizeof(long_text) - 1U);
    long_text[sizeof(long_text) - 1U] = '\0';

    kode_err_set("%s", long_text);

    const char  *got = kode_error();
    const size_t len = strlen(got);

    /* Cut to what fits, the ellipsis's three bytes at the end. */
    TEST_ASSERT_EQUAL_UINT32(KODE_ERROR_MAX - 1U, len);
    TEST_ASSERT_EQUAL_STRING("\xE2\x80\xA6", &got[len - 3U]);
    TEST_ASSERT_EQUAL_CHAR('x', got[len - 4U]);

    /* The text that fits whole is kept whole. */
    char fits[KODE_ERROR_MAX];

    (void)memset(fits, 'y', sizeof(fits) - 1U);
    fits[sizeof(fits) - 1U] = '\0';
    kode_err_set("%s", fits);
    TEST_ASSERT_EQUAL_STRING(fits, kode_error());
}

TEST_CASE("an error that cannot be stored leaves kode_error empty", "[kode_entry]")
{
    job_t j;

    /* A fresh task has no slot yet; its first error needs PSRAM, which is
     * refused. Nothing may crash, and the reader still gets a string. */
    fake_psram_refuse(1U);
    TEST_ASSERT_TRUE(job_run(&j, "no room"));
    fake_psram_refuse(0U);
    TEST_ASSERT_EQUAL_STRING("", j.after);
}

/* ---- kode_enter ----------------------------------------------------------- */

/** @brief What the hook did while kode_enter() waited on kode_init(). */
static volatile uint32_t s_init_polls;

/** @brief The body of a first kode_init() ends: what a sleeping caller wakes to. */
static void init_body_ends(void)
{
    ++s_init_polls;
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

TEST_CASE("the first kode.h call brings the system up by itself", "[kode_entry]")
{
    /* Another task is mid-way through the bring-up; no kode_init() of ours
     * was called. kode_enter() must go through kode_init() and so wait for
     * that body, which the fake clock ends on the caller's first sleep. */
    s_init_polls = 0U;
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_RUNNING);
    fake_time_delay_hook(&init_body_ends);

    const bool entered = kode_enter("kode_probe");

    fake_time_delay_hook(NULL);
    TEST_ASSERT_TRUE(entered);
    TEST_ASSERT_EQUAL_UINT32(1U, s_init_polls);
    TEST_ASSERT_EQUAL_INT(KHAL_ONCE_DONE, atomic_load(&kode_sys_init_once.state));
}

/** @brief What kode_enter() answered inside the probe Active Object (probe_ao.h). */
typedef struct {
    bool entered;                /**< kode_enter()'s answer.    */
    char error[KODE_ERROR_MAX];  /**< kode_error() right after. */
} Entered;

/** @brief The probe's job: call kode_enter() and keep what it said. */
static void enter_job(void *ctx)
{
    Entered *r = (Entered *)ctx;

    r->entered = kode_enter("kode_led_set_rgb");
    text_copy(r->error, kode_error());
}

TEST_CASE("kode_enter refuses inside an Active Object and names it", "[kode_entry]")
{
    Entered r = { true, "" };

    system_is_up();
    TEST_ASSERT_TRUE(probe_ao_run(&enter_job, &r, TEST_WAIT_MS));
    TEST_ASSERT_FALSE(r.entered);
    TEST_ASSERT_EQUAL_STRING("kode_led_set_rgb was called from an Active Object "
                             "(" PROBE_AO_NAME "); kode.h calls block - call "
                             "kode_sys_* instead",
                             r.error);
}

TEST_CASE("a refusal is logged once, as an error", "[kode_entry]")
{
    Entered r = { true, "" };

    system_is_up();
    (void)probe_ao_run(NULL, NULL, TEST_WAIT_MS);   /* the probe up first: its start logs nothing counted here */
    fake_klog_reset();

    TEST_ASSERT_TRUE(probe_ao_run(&enter_job, &r, TEST_WAIT_MS));
    TEST_ASSERT_EQUAL_UINT32(1U, fake_klog_count());
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_line(0U), "kode_led_set_rgb"));
}

/** @brief Times the hook of the next case ran. */
static volatile uint32_t s_wait_hook_runs;

/** @brief Counts a sleep and ends the bring-up it would be waiting for, so a
 *         caller that waits wrongly is told apart from one that does not and
 *         the case reports instead of spinning. */
static void wait_hook_counts(void)
{
    ++s_wait_hook_runs;
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

TEST_CASE("an Active Object is refused before the bring-up is waited on", "[kode_entry]")
{
    Entered r = { true, "" };

    (void)probe_ao_run(NULL, NULL, TEST_WAIT_MS);   /* the probe up before the bring-up is mid-way */
    s_wait_hook_runs = 0U;

    /* Another task is mid-way through the bring-up. An AO that waited on it
     * would sleep, and the hook would run. */
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_RUNNING);
    fake_time_delay_hook(&wait_hook_counts);

    const bool took = probe_ao_run(&enter_job, &r, TEST_WAIT_MS);

    fake_time_delay_hook(NULL);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    TEST_ASSERT_TRUE(took);
    TEST_ASSERT_FALSE(r.entered);
    TEST_ASSERT_EQUAL_UINT32(0U, s_wait_hook_runs);
}

TEST_CASE("kode_has_prefix matches the start of a text in any case", "[kode_entry]")
{
    TEST_ASSERT_TRUE(kode_has_prefix("mqtt://broker", "mqtt://"));
    TEST_ASSERT_TRUE(kode_has_prefix("MQTT://Broker", "mqtt://"));
    TEST_ASSERT_TRUE(kode_has_prefix("mqtt://", "mqtt://"));
    TEST_ASSERT_TRUE(kode_has_prefix("anything", ""));
    TEST_ASSERT_TRUE(kode_has_prefix("", ""));
    TEST_ASSERT_FALSE(kode_has_prefix("mqtt:/", "mqtt://"));
    TEST_ASSERT_FALSE(kode_has_prefix("", "mqtt://"));
    TEST_ASSERT_FALSE(kode_has_prefix("mqtts://broker", "mqtt://"));
    TEST_ASSERT_FALSE(kode_has_prefix("xmqtt://broker", "mqtt://"));
}
