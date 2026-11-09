/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_init_once.c
 * @brief kode_init() brings the system up once, from any number of calls and tasks.
 */
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fakes.h"
#include "kbsp_owners.h"
#include "khal_system.h"
#include "kode.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"

/* QP keeps its registry of started Active Objects private; reading it, and
 * only reading it, is how a case sees that a call started none. */
extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

/** @brief Stack of a test task, in bytes. */
#define TEST_STACK_BYTES 8192U

/** @brief Priority of a test task. */
#define TEST_PRIO 1U

/** @brief The longest a case waits for a task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief The longest the body waits to be released, in ms; a guard that lets
 *         the loser through early fails the case instead of hanging it. */
#define TEST_BODY_BOUND_MS 2000U

/** @brief Polls the loser makes before the body is released. */
#define TEST_POLLS_BEFORE_RELEASE 2U

/** @brief How long a poll of the loser really sleeps, in ticks. A tick is the
 *         shortest sleep there is: a delay of zero ticks is only a yield, and
 *         the loser would spin. */
#define TEST_LOSER_SLEEP_TICKS 1U

/** @brief Number of tasks that call together. */
#define TEST_CALLERS 2U

/** @brief What the callers of one case share. */
typedef struct {
    khal_once_t       once;                /**< The guard under test.            */
    volatile uint32_t body_runs;           /**< Times the body started.          */
    volatile bool     body_done;           /**< The body has finished.           */
    volatile bool     done_at_return[TEST_CALLERS]; /**< body_done when each call returned. */
    SemaphoreHandle_t finished[TEST_CALLERS];       /**< Given by each caller on return.    */
    SemaphoreHandle_t release;             /**< Given once the loser has polled enough. */
} race_t;

/** @brief Which caller a task is, and the case it belongs to. */
typedef struct {
    race_t  *race;   /**< The shared state.   */
    uint32_t index;  /**< This caller's slot. */
} caller_t;

static race_t *s_race;   /**< The case the body under test reports to. */

/**
 * @brief A body that holds the system "coming up" until the loser has polled.
 *
 * It waits for the release the loser's poll hook gives, so the order is
 * fixed by the case and not by the scheduler: the body cannot finish before
 * the loser has slept twice. The wait is bounded; a guard that lets the
 * loser through early never gives the release and fails the case after it.
 */
static void slow_body(void)
{
    ++s_race->body_runs;
    (void)xSemaphoreTake(s_race->release, pdMS_TO_TICKS(TEST_BODY_BOUND_MS));
    s_race->body_done = true;
}

/** @brief How many times the loser slept through the fake clock. */
static volatile uint32_t s_loser_polls;

/**
 * @brief What the fake clock runs each time a caller sleeps: really yield.
 *
 * The fake clears its hook before calling it, so the hook arms itself again
 * and every poll of the loser sleeps for real. The poll that makes the
 * second releases the body.
 */
static void loser_sleeps(void)
{
    ++s_loser_polls;
    fake_time_delay_hook(&loser_sleeps);
    if ( TEST_POLLS_BEFORE_RELEASE == s_loser_polls )
    {
        (void)xSemaphoreGive(s_race->release);
    }
    vTaskDelay(TEST_LOSER_SLEEP_TICKS);
}

/** @brief One task calling the guard. */
static void caller_task(void *arg)
{
    caller_t *const c = (caller_t *)arg;

    khal_once_run(&c->race->once, &slow_body);
    c->race->done_at_return[c->index] = c->race->body_done;
    (void)xSemaphoreGive(c->race->finished[c->index]);
}

/** @brief Times the plain counting body has run. */
static uint32_t s_counted;

/** @brief A body that only counts itself. */
static void counting_body(void)
{
    ++s_counted;
}

/** @brief A body that stands for a bring-up already done. */
static void marker_body(void)
{
}

/** @brief The registry as it stands, copied. */
static void registry_copy(QActive *out[QF_MAX_ACTIVE + 1U])
{
    (void)memcpy(out, QActive_registry_, sizeof(QActive *) * (QF_MAX_ACTIVE + 1U));
}

/** @brief Longest owner name the snapshot keeps. */
#define OWNER_NAME_MAX 48U

/** @brief Copy who holds each resource, "-" for a free one. */
static void owners_copy(char out[KBSP_RES_MAX][OWNER_NAME_MAX])
{
    for ( int i = 0; (int)KBSP_RES_MAX > i; ++i )
    {
        const char *const name = kbsp_owner_name((kbsp_res_t)i);

        (void)strncpy(out[i], "-", OWNER_NAME_MAX);
        if ( NULL != name )
        {
            (void)strncpy(out[i], name, OWNER_NAME_MAX - 1U);
            out[i][OWNER_NAME_MAX - 1U] = '\0';
        }
    }
}

/* The real bring-up cannot run here: earlier files already hold the QP slots it would take. */
TEST_CASE("a second kode_init starts nothing", "[kode_init]")
{
    QActive *before[QF_MAX_ACTIVE + 1U];
    char     owners_before[KBSP_RES_MAX][OWNER_NAME_MAX];
    char     owners_after[KBSP_RES_MAX][OWNER_NAME_MAX];

    /* Make the first call "have happened": the guard is armed with a body that
     * does nothing, so the next kode_init() meets a system that is up. */
    khal_once_run(&kode_sys_init_once, &marker_body);

    registry_copy(before);
    owners_copy(owners_before);

    kode_init();
    kode_init();

    owners_copy(owners_after);
    TEST_ASSERT_EQUAL_MEMORY(before, QActive_registry_,
                             sizeof(QActive *) * (QF_MAX_ACTIVE + 1U));
    TEST_ASSERT_EQUAL_MEMORY(owners_before, owners_after, sizeof(owners_before));
}

TEST_CASE("the guard runs its body once and only once", "[kode_init]")
{
    static khal_once_t once;

    s_counted = 0U;
    khal_once_run(&once, &counting_body);
    khal_once_run(&once, &counting_body);
    khal_once_run(&once, &counting_body);
    TEST_ASSERT_EQUAL_UINT32(1U, s_counted);
}

TEST_CASE("two tasks calling together run the body once", "[kode_init]")
{
    static race_t race;
    caller_t callers[TEST_CALLERS];

    (void)memset(&race, 0, sizeof(race));
    s_race = &race;
    /* The fake clock only counts; every sleep is made a real one so the
     * loser never spins against the winner's body. */
    s_loser_polls = 0U;
    race.release = xSemaphoreCreateBinary();
    fake_time_delay_hook(&loser_sleeps);
    for ( uint32_t i = 0U; TEST_CALLERS > i; ++i )
    {
        race.finished[i] = xSemaphoreCreateBinary();
        callers[i].race  = &race;
        callers[i].index = i;
    }
    kode_status_t started = KODE_OK;
    BaseType_t    took    = pdTRUE;

    for ( uint32_t i = 0U; (TEST_CALLERS > i) && (KODE_OK == started); ++i )
    {
        started = khal_task_start(&caller_task, "t_once", TEST_STACK_BYTES,
                                  TEST_PRIO, &callers[i]);
    }
    for ( uint32_t i = 0U; (TEST_CALLERS > i) && (KODE_OK == started) && (took == pdTRUE); ++i )
    {
        took = xSemaphoreTake(race.finished[i], pdMS_TO_TICKS(TEST_WAIT_MS));
    }
    /* Out before any assertion can leave the case: the hook must not reach
     * the next one. (setUp clears it as well, for a case that fails earlier.) */
    fake_time_delay_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_OK, started);
    TEST_ASSERT_EQUAL_INT(pdTRUE, took);

    TEST_ASSERT_EQUAL_UINT32(1U, race.body_runs);
    TEST_ASSERT_TRUE(race.body_done);
    /* The body was held until the second poll, so there were at least two. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2U, s_loser_polls);
    /* Neither call came back before the system was up. */
    for ( uint32_t i = 0U; TEST_CALLERS > i; ++i )
    {
        TEST_ASSERT_TRUE(race.done_at_return[i]);
        vSemaphoreDelete(race.finished[i]);
    }
    vSemaphoreDelete(race.release);
}

/* ---- khal_once_try(): a start that fails is tried again --------------------- */

/** @brief Runs of the try body in one case. */
static uint32_t s_try_runs;

/** @brief What the try body answers on its next run. */
static bool s_try_answer;

/** @brief A try body that counts itself and answers what the case says. */
static bool counting_try_body(void)
{
    ++s_try_runs;
    return s_try_answer;
}

TEST_CASE("a failed try leaves the one-shot open and a success closes it", "[kode_init]")
{
    static khal_once_t once;

    s_try_runs   = 0U;
    s_try_answer = false;
    TEST_ASSERT_FALSE(khal_once_try(&once, &counting_try_body));
    TEST_ASSERT_EQUAL_INT(KHAL_ONCE_IDLE, atomic_load(&once.state));
    TEST_ASSERT_FALSE(khal_once_try(&once, &counting_try_body));
    TEST_ASSERT_EQUAL_UINT32(2U, s_try_runs);

    s_try_answer = true;
    TEST_ASSERT_TRUE(khal_once_try(&once, &counting_try_body));
    TEST_ASSERT_EQUAL_INT(KHAL_ONCE_DONE, atomic_load(&once.state));

    /* Done is done: the body is not run again, and the answer is true. */
    s_try_answer = false;
    TEST_ASSERT_TRUE(khal_once_try(&once, &counting_try_body));
    TEST_ASSERT_EQUAL_UINT32(3U, s_try_runs);
}

/** @brief Given once the loser has polled enough, to release the failing winner. */
static SemaphoreHandle_t s_try_release;

/** @brief Polls the loser made. */
static volatile uint32_t s_try_polls;

/** @brief Runs of the racing body. */
static volatile uint32_t s_racing_runs;

/** @brief The first run is held until the loser has polled, then fails; the next succeeds. */
static bool failing_then_ok_body(void)
{
    const uint32_t run = ++s_racing_runs;

    if ( 1U == run )
    {
        (void)xSemaphoreTake(s_try_release, pdMS_TO_TICKS(TEST_BODY_BOUND_MS));
        return false;
    }
    return true;
}

/** @brief The loser's sleep, really yielding; the second poll releases the winner. */
static void try_loser_sleeps(void)
{
    ++s_try_polls;
    fake_time_delay_hook(&try_loser_sleeps);
    if ( TEST_POLLS_BEFORE_RELEASE == s_try_polls )
    {
        (void)xSemaphoreGive(s_try_release);
    }
    vTaskDelay(TEST_LOSER_SLEEP_TICKS);
}

/** @brief The shared one-shot of the racing case. */
static khal_once_t s_try_once;

/** @brief What each racing caller was told. */
static volatile bool s_try_told[TEST_CALLERS];

/** @brief Given by each racing caller on return. */
static SemaphoreHandle_t s_try_finished[TEST_CALLERS];

/** @brief One caller of khal_once_try(). */
static void try_caller_task(void *arg)
{
    const uint32_t i = (uint32_t)(uintptr_t)arg;

    s_try_told[i] = khal_once_try(&s_try_once, &failing_then_ok_body);
    (void)xSemaphoreGive(s_try_finished[i]);
}

TEST_CASE("a loser takes over from a winner whose try failed", "[kode_init]")
{
    (void)memset(&s_try_once, 0, sizeof(s_try_once));
    s_try_polls   = 0U;
    s_racing_runs = 0U;
    s_try_release = xSemaphoreCreateBinary();
    fake_time_delay_hook(&try_loser_sleeps);

    kode_status_t started = KODE_OK;
    BaseType_t    took    = pdTRUE;

    for ( uint32_t i = 0U; (TEST_CALLERS > i) && (KODE_OK == started); ++i )
    {
        s_try_finished[i] = xSemaphoreCreateBinary();
        started = khal_task_start(&try_caller_task, "t_try", TEST_STACK_BYTES,
                                  TEST_PRIO, (void *)(uintptr_t)i);
    }
    for ( uint32_t i = 0U; (TEST_CALLERS > i) && (KODE_OK == started) && (took == pdTRUE); ++i )
    {
        took = xSemaphoreTake(s_try_finished[i], pdMS_TO_TICKS(TEST_WAIT_MS));
    }
    fake_time_delay_hook(NULL);
    TEST_ASSERT_EQUAL_INT(KODE_OK, started);
    TEST_ASSERT_EQUAL_INT(pdTRUE, took);

    /* The winner's run failed while the loser slept; the loser then ran it
     * itself and it held. One caller was told no, the other yes. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(TEST_POLLS_BEFORE_RELEASE, s_try_polls);
    TEST_ASSERT_EQUAL_UINT32(2U, s_racing_runs);
    TEST_ASSERT_TRUE(s_try_told[0] != s_try_told[1]);
    TEST_ASSERT_EQUAL_INT(KHAL_ONCE_DONE, atomic_load(&s_try_once.state));
    for ( uint32_t i = 0U; TEST_CALLERS > i; ++i )
    {
        vSemaphoreDelete(s_try_finished[i]);
    }
    vSemaphoreDelete(s_try_release);
}
