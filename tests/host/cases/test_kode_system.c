/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_system.c
 * @brief The standard layer's system calls (time, device id, print, random) and the local clock.
 */
#include <setjmp.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"

#include "fakes.h"
#include "kaos.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode.h"
#include "kode_log.h"
#include "kode_sys_priv.h"
#include "qf_support.h"

/** @brief A moment on the wall clock: 2026-03-04T05:06:07Z, in seconds. */
#define TEST_WALL_S 1772600767ULL

/** @brief Microseconds in a second. */
#define TEST_US_PER_S 1000000ULL

/** @brief A moment past 2038-01-19 (a signed 32-bit time_t's end): 2100-01-01T00:00:00Z. */
#define TEST_WALL_2100_S 4102444800ULL

/** @brief How far a case moves the fake clock, in ms. */
#define TEST_ELAPSED_MS 1234U

/** @brief How long a case asks kode_delay() to sleep, in ms. */
#define TEST_SLEEP_MS 250U

/** @brief Make kode_init() a no-op, as it is once the system is up. */
static void system_is_up(void)
{
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
}

TEST_CASE("kode_uptime_ms follows the fake clock", "[kode_system]")
{
    system_is_up();
    fake_time_advance_ms(TEST_ELAPSED_MS);

    TEST_ASSERT_EQUAL_UINT32(TEST_ELAPSED_MS, kode_uptime_ms());

    fake_time_advance_ms(TEST_SLEEP_MS);
    TEST_ASSERT_EQUAL_UINT32(TEST_ELAPSED_MS + TEST_SLEEP_MS, kode_uptime_ms());
}

TEST_CASE("test_clock_minus_one_until_set", "[kode_system]")
{
    system_is_up();

    TEST_ASSERT_EQUAL_INT64(-1, kode_time_unix());
    TEST_ASSERT_EQUAL_STRING("the clock is not set", kode_error());
}

TEST_CASE("test_clock_is_unix_seconds", "[kode_system]")
{
    system_is_up();
    fake_time_wall_set(TEST_WALL_S * TEST_US_PER_S);

    TEST_ASSERT_EQUAL_INT64((int64_t)TEST_WALL_S, kode_time_unix());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("test_clock_beyond_2038", "[kode_system]")
{
    system_is_up();
    fake_time_wall_set(TEST_WALL_2100_S * TEST_US_PER_S);

    TEST_ASSERT_EQUAL_INT64((int64_t)TEST_WALL_2100_S, kode_time_unix());
}

TEST_CASE("a set clock clears the error the unset one left", "[kode_system]")
{
    system_is_up();

    (void)kode_time_unix();
    TEST_ASSERT_EQUAL_STRING("the clock is not set", kode_error());

    fake_time_wall_set(TEST_WALL_S * TEST_US_PER_S);
    TEST_ASSERT_EQUAL_INT64((int64_t)TEST_WALL_S, kode_time_unix());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("the clock is UTC whatever zone the process runs in", "[kode_system]")
{
    system_is_up();
    (void)setenv("TZ", "CET-1", 1);
    tzset();
    fake_time_wall_set(TEST_WALL_S * TEST_US_PER_S);

    const int64_t now = kode_time_unix();

    (void)setenv("TZ", "UTC0", 1);
    tzset();
    TEST_ASSERT_EQUAL_INT64((int64_t)TEST_WALL_S, now);
}

/* ---- Local time -------------------------------------------------------------
 * One moment, 2026-07-01T10:00:00Z, a Wednesday: 10:00 in UTC and 12:00 in
 * Madrid's summer time. The zone is applied the way AO_Boot applies it
 * (khal_time_zone_set()), and every case puts the process back in UTC. */

/** @brief 2026-07-01T10:00:00Z, in seconds. */
#define TEST_JULY_S          1782900000ULL
/** @brief Madrid and Paris, daylight saving included. */
#define TEST_ZONE_MADRID     "CET-1CEST,M3.5.0,M10.5.0/3"
/** @brief The board's own time. */
#define TEST_ZONE_UTC        "UTC0"
/** @brief The local hour of TEST_JULY_S in UTC. */
#define TEST_HOUR_UTC        10U
/** @brief The local hour of TEST_JULY_S in Madrid. */
#define TEST_HOUR_MADRID     12U
/** @brief 1 July 2026 is a Wednesday (0 = Sunday). */
#define TEST_JULY_WEEKDAY    3U
/** @brief The year, month and day of TEST_JULY_S, in both zones. */
#define TEST_JULY_YEAR       2026U
#define TEST_JULY_MONTH      7U
#define TEST_JULY_DAY        1U

TEST_CASE("kode_sys_clock_local gives 12:00 on a Wednesday in Madrid at 10:00 UTC",
          "[kode_system]")
{
    kode_sys_time_t t;

    fake_time_wall_set(TEST_JULY_S * TEST_US_PER_S);
    TEST_ASSERT_TRUE(khal_time_zone_set(TEST_ZONE_MADRID));

    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_clock_local(&t));
    TEST_ASSERT_EQUAL_UINT16(TEST_JULY_YEAR, t.year);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_MONTH, t.month);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_DAY, t.day);
    TEST_ASSERT_EQUAL_UINT8(TEST_HOUR_MADRID, t.hour);
    TEST_ASSERT_EQUAL_UINT8(0U, t.minute);
    TEST_ASSERT_EQUAL_UINT8(0U, t.second);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_WEEKDAY, t.weekday);

    TEST_ASSERT_TRUE(khal_time_zone_set(TEST_ZONE_UTC));
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_clock_local(&t));
    TEST_ASSERT_EQUAL_UINT8(TEST_HOUR_UTC, t.hour);
}

TEST_CASE("kode_sys_clock_local is an error while the clock is unknown, and "
          "refuses NULL", "[kode_system]")
{
    kode_sys_time_t t;

    (void)memset(&t, 0xA5, sizeof(t));
    TEST_ASSERT_EQUAL(KODE_ERROR, kode_sys_clock_local(&t));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xA5U, t.hour, "written while unknown");
    TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_clock_local(NULL));
    fake_time_wall_set(TEST_JULY_S * TEST_US_PER_S);
    TEST_ASSERT_EQUAL(KODE_DENIED, kode_sys_clock_local(NULL));
}

TEST_CASE("kode_time_local fills a kode_time_t, and says why when it cannot",
          "[kode_system]")
{
    kode_time_t t;

    system_is_up();
    TEST_ASSERT_FALSE(kode_time_local(&t));
    TEST_ASSERT_EQUAL_STRING("the clock is not set", kode_error());

    fake_time_wall_set(TEST_JULY_S * TEST_US_PER_S);
    TEST_ASSERT_FALSE(kode_time_local(NULL));
    TEST_ASSERT_EQUAL_STRING("time is NULL", kode_error());

    TEST_ASSERT_TRUE(khal_time_zone_set(TEST_ZONE_MADRID));
    TEST_ASSERT_TRUE(kode_time_local(&t));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT16(TEST_JULY_YEAR, t.year);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_MONTH, t.month);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_DAY, t.day);
    TEST_ASSERT_EQUAL_UINT8(TEST_HOUR_MADRID, t.hour);
    TEST_ASSERT_EQUAL_UINT8(0U, t.minute);
    TEST_ASSERT_EQUAL_UINT8(0U, t.second);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_WEEKDAY, t.weekday);

    TEST_ASSERT_TRUE(khal_time_zone_set(TEST_ZONE_UTC));
}

/* ---- A change of zone while another task reads -----------------------------
 * The zone is applied (setenv, then tzset) and read (localtime_r) under the C
 * library's environment lock, which the host's library does not have: the
 * fake's lock is a real recursive mutex, and its release hook stops the task
 * releasing it while it still holds it. So each half is caught holding the
 * lock, and the other half, started then, must wait for it: a reader that did
 * not lock would read in the middle of a change, a change that did not lock
 * would land in the middle of a reading, and either is a failure here. */

/** @brief Room for a task that formats a date. */
#define TEST_TASK_STACK  8192U
/** @brief Above the case's own task, so a started task runs at once. */
#define TEST_TASK_PRIO   2U
/** @brief How long a case watches a task wait for the lock, in ms. */
#define TEST_HELD_MS     50U
/** @brief How long a case waits for a task that should finish, in ms. */
#define TEST_DONE_MS     2000U

static SemaphoreHandle_t s_lock_held;  /**< The holder is inside its release. */
static SemaphoreHandle_t s_lock_go;    /**< The holder may release now.       */
static SemaphoreHandle_t s_zone_done;  /**< The change of zone returned.      */
static SemaphoreHandle_t s_read_done;  /**< The reading returned.             */
static kode_sys_time_t   s_read;       /**< What the reading gave.            */
static kode_status_t     s_read_st;    /**< How the reading went.             */

/** @brief The release hook: say the lock is held, and hold it until told. */
static void hold_in_release(void)
{
    (void)xSemaphoreGive(s_lock_held);
    (void)xSemaphoreTake(s_lock_go, portMAX_DELAY);
}

/** @brief A task that changes the zone. @param arg The zone. */
static void zone_task(void *arg)
{
    (void)khal_time_zone_set((const char *)arg);
    (void)xSemaphoreGive(s_zone_done);
}

/** @brief A task that reads the local time. @param arg Unused. */
static void read_task(void *arg)
{
    (void)arg;
    s_read_st = kode_sys_clock_local(&s_read);
    (void)xSemaphoreGive(s_read_done);
}

/**
 * @brief Catch @p first holding the lock, start @p second, and let go.
 *
 * Whatever happens, both tasks end before this returns: a failed case must
 * not leave a task holding the lock for the cases after it.
 *
 * @param first      The task caught inside its release.
 * @param first_arg  Its argument.
 * @param second     The task started while the lock is held.
 * @param second_arg Its argument.
 * @param second_done Given when @p second returns.
 * @param[out] held  @p first held the lock: it took it at all.
 * @param[out] early @p second returned while @p first held the lock.
 */
static void race(khal_task_fn_t first, void *first_arg,
                 khal_task_fn_t second, void *second_arg,
                 SemaphoreHandle_t second_done, bool *held, bool *early)
{
    fake_env_release_hook(&hold_in_release);
    TEST_ASSERT_EQUAL(KODE_OK, khal_task_start(first, "t_first", TEST_TASK_STACK,
                                               TEST_TASK_PRIO, first_arg));
    *held = (xSemaphoreTake(s_lock_held, pdMS_TO_TICKS(TEST_DONE_MS)) == pdTRUE);
    if ( !*held )
    {
        fake_env_release_hook(NULL);
    }
    TEST_ASSERT_EQUAL(KODE_OK, khal_task_start(second, "t_second", TEST_TASK_STACK,
                                               TEST_TASK_PRIO, second_arg));
    *early = (xSemaphoreTake(second_done, pdMS_TO_TICKS(TEST_HELD_MS)) == pdTRUE);
    (void)xSemaphoreGive(s_lock_go);
    if ( !*early )
    {
        (void)xSemaphoreTake(second_done, pdMS_TO_TICKS(TEST_DONE_MS));
    }
}

TEST_CASE("test_clock_local_under_zone_change", "[kode_system]")
{
    bool held  = false;
    bool early = false;

    s_lock_held = xSemaphoreCreateBinary();
    s_lock_go   = xSemaphoreCreateBinary();
    s_zone_done = xSemaphoreCreateBinary();
    s_read_done = xSemaphoreCreateBinary();
    fake_time_wall_set(TEST_JULY_S * TEST_US_PER_S);
    TEST_ASSERT_TRUE(khal_time_zone_set(TEST_ZONE_UTC));

    /* A change of zone caught holding the lock: the reading waits for it,
     * then reads the new zone whole. */
    race(&zone_task, (void *)TEST_ZONE_MADRID, &read_task, NULL, s_read_done,
         &held, &early);
    TEST_ASSERT_TRUE(xSemaphoreTake(s_zone_done, pdMS_TO_TICKS(TEST_DONE_MS)) == pdTRUE);
    TEST_ASSERT_TRUE_MESSAGE(held, "the zone was applied without the environment lock");
    TEST_ASSERT_FALSE_MESSAGE(early, "a local time was read in the middle of a change of zone");
    TEST_ASSERT_EQUAL(KODE_OK, s_read_st);
    TEST_ASSERT_EQUAL_UINT8(TEST_HOUR_MADRID, s_read.hour);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_DAY, s_read.day);
    TEST_ASSERT_EQUAL_UINT8(TEST_JULY_WEEKDAY, s_read.weekday);

    /* A reading caught holding the lock: the change waits for it, and the
     * reading is wholly the zone before the change. */
    race(&read_task, NULL, &zone_task, (void *)TEST_ZONE_UTC, s_zone_done,
         &held, &early);
    TEST_ASSERT_TRUE(xSemaphoreTake(s_read_done, pdMS_TO_TICKS(TEST_DONE_MS)) == pdTRUE);
    TEST_ASSERT_TRUE_MESSAGE(held, "a local time was read without the environment lock");
    TEST_ASSERT_FALSE_MESSAGE(early, "the zone changed in the middle of a reading");
    TEST_ASSERT_EQUAL(KODE_OK, s_read_st);
    TEST_ASSERT_EQUAL_UINT8(TEST_HOUR_MADRID, s_read.hour);

    /* And after both, the new zone. */
    TEST_ASSERT_EQUAL(KODE_OK, kode_sys_clock_local(&s_read));
    TEST_ASSERT_EQUAL_UINT8(TEST_HOUR_UTC, s_read.hour);

    vSemaphoreDelete(s_lock_held);
    vSemaphoreDelete(s_lock_go);
    vSemaphoreDelete(s_zone_done);
    vSemaphoreDelete(s_read_done);
}

TEST_CASE("kode_delay sleeps the milliseconds asked", "[kode_system]")
{
    system_is_up();

    const uint32_t before = kode_uptime_ms();

    kode_delay(TEST_SLEEP_MS);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(TEST_SLEEP_MS, kode_uptime_ms() - before);
}

TEST_CASE("kode_device_id is never NULL", "[kode_system]")
{
    system_is_up();

    TEST_ASSERT_NOT_NULL(kode_device_id());
}

/** @brief A static event that only takes a slot of AO_Boot's queue. */
static const QEvt s_boot_filler = QEVT_INITIALIZER(Q_USER_SIG);

/** @brief Where the exit case leaves kode_exit() from. */
static jmp_buf s_exit_jump;

/** @brief The signal found at the head of AO_Boot's queue on the second sleep. */
static int32_t s_exit_sig;

/** @brief Recycle everything waiting in AO_Boot's queue. */
static void boot_queue_drain(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The second sleep: read what kode_exit() posted, and leave it. */
static void exit_second_sleep(void)
{
    const QEvt *e = NULL;

    s_exit_sig = -1;
    if ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        s_exit_sig = (int32_t)e->sig;
        QF_gc(e);
    }
    longjmp(s_exit_jump, 1);
}

/** @brief The first sleep: AO_Boot drains its queue, as its task would. */
static void exit_first_sleep(void)
{
    boot_queue_drain();
    fake_time_delay_hook(&exit_second_sleep);
}

TEST_CASE("kode_exit asks again until AO_Boot takes the request", "[kode_system]")
{
    system_is_up();
    fake_time_reset();
    qf_support_init();
    qf_support_margin_queue(AO_Boot);
    while ( uxQueueSpacesAvailable(AO_Boot->eQueue) > KQP_BOOT_CMD_MARGIN )
    {
        TEST_ASSERT_TRUE(QACTIVE_POST_X(AO_Boot, &s_boot_filler, 0U, NULL));
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_NO_RESOURCES, kode_sys_exit_to_os(),
                                  "AO_Boot's queue is within the margin");

    fake_time_delay_hook(&exit_first_sleep);
    if ( setjmp(s_exit_jump) == 0 )
    {
        kode_exit();
        TEST_FAIL_MESSAGE("kode_exit returned");
    }
    fake_time_delay_hook(NULL);
    TEST_ASSERT_EQUAL_INT32((int32_t)BOOT_EXIT_REQ_SIG, s_exit_sig);
    boot_queue_drain();
}

/* ---- kode_print(): one line, past the filters, not past off --------------- */

/** @brief Bytes of a text longer than one kode_print() line holds. */
#define TEST_PRINT_LONG 300U

TEST_CASE("kode_print prints one app line past a filter at error", "[kode_system]")
{
    const kode_log_lvl_t was = kode_log_get_level();

    system_is_up();
    kode_log_set_level(NULL, KODE_LOG_ERROR);

    kode_print("colour %u, brightness %u %%", 2U, 60U);

    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!N 0 app colour 2, brightness 60 %\n", fake_klog_last());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    kode_log_set_level(NULL, was);
}

TEST_CASE("kode_print stays quiet when the log is off", "[kode_system]")
{
    const kode_log_lvl_t was = kode_log_get_level();

    system_is_up();
    kode_log_set_level(NULL, KODE_LOG_OFF);

    kode_print("hello");

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    kode_log_set_level(NULL, was);
}

TEST_CASE("kode_print cuts a long text at KODE_PRINT_MAX", "[kode_system]")
{
    char text[TEST_PRINT_LONG];
    char line[KODE_PRINT_MAX + 16U];

    system_is_up();
    (void)memset(text, 'x', sizeof(text) - 1U);
    text[sizeof(text) - 1U] = '\0';

    kode_print("%s", text);

    (void)snprintf(line, sizeof(line), "!N 0 app %.*s\n",
                   (int)(KODE_PRINT_MAX - 1U), text);
    TEST_ASSERT_EQUAL_STRING(line, fake_klog_last());
}

TEST_CASE("kode_print with a NULL format prints nothing and says why", "[kode_system]")
{
    system_is_up();

    kode_print(NULL);

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("format is NULL", kode_error());
}

/* ---- kode_random(): within the range, every number reached ---------------- */

/** @brief Draws a case takes from a small range. */
#define TEST_RANDOM_DRAWS 300U

/** @brief The small range's top: 0 to 2, three numbers. */
#define TEST_RANDOM_SMALL_MAX 2U

/** @brief A range that does not start at 0. */
#define TEST_RANDOM_MIN 10U

/** @brief Its top. */
#define TEST_RANDOM_MAX 20U

TEST_CASE("kode_random stays within min and max, both reached", "[kode_system]")
{
    uint32_t seen[TEST_RANDOM_SMALL_MAX + 1U] = { 0U };

    system_is_up();
    for ( uint32_t i = 0U; TEST_RANDOM_DRAWS > i; i++ )
    {
        const uint32_t n = kode_random(0U, TEST_RANDOM_SMALL_MAX);

        TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_RANDOM_SMALL_MAX, n);
        seen[n]++;
    }
    for ( uint32_t v = 0U; TEST_RANDOM_SMALL_MAX >= v; v++ )
    {
        TEST_ASSERT_GREATER_THAN_UINT32(0U, seen[v]);
    }

    for ( uint32_t i = 0U; TEST_RANDOM_DRAWS > i; i++ )
    {
        const uint32_t n = kode_random(TEST_RANDOM_MIN, TEST_RANDOM_MAX);

        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(TEST_RANDOM_MIN, n);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(TEST_RANDOM_MAX, n);
    }
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("kode_random answers min when it is max", "[kode_system]")
{
    system_is_up();

    TEST_ASSERT_EQUAL_UINT32(TEST_RANDOM_MAX, kode_random(TEST_RANDOM_MAX, TEST_RANDOM_MAX));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("kode_random over the whole range answers the draw itself", "[kode_system]")
{
    system_is_up();
    const uint32_t expected = khal_random();

    fake_system_reset();
    TEST_ASSERT_EQUAL_UINT32(expected, kode_random(0U, UINT32_MAX));
}

TEST_CASE("kode_random with max below min answers min and says why", "[kode_system]")
{
    system_is_up();

    TEST_ASSERT_EQUAL_UINT32(TEST_RANDOM_MAX, kode_random(TEST_RANDOM_MAX, TEST_RANDOM_MIN));
    TEST_ASSERT_EQUAL_STRING("max 10 is below min 20", kode_error());
}
