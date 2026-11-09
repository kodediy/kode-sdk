/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_time.c
 * @brief The clock seam, checked by what it asks esp_timer for.
 */
#include <string.h>

#include "unity.h"

#include "Mockesp_timer.h"

#include "khal_time.h"

/** @brief Any non-NULL handle; every use of it is mocked. */
static esp_timer_handle_t s_timer = (esp_timer_handle_t)(void *)&s_timer;

/** @brief What the driver asked esp_timer to create, captured by the stub. */
static esp_timer_create_args_t s_args;
static int                     s_creates;

/**
 * @brief Stands in for esp_timer_create and keeps its arguments.
 * @param args  The creation arguments under inspection.
 * @param out   Receives the handle.
 * @param calls CMock's invocation counter.
 * @return ESP_OK.
 */
static esp_err_t capture_create(const esp_timer_create_args_t *args,
                                esp_timer_handle_t *out, int calls)
{
    (void)calls;
    if ( NULL != args )
    {
        s_args = *args;
    }
    if ( NULL != out )
    {
        *out = s_timer;
    }
    ++s_creates;
    return ESP_OK;
}

/** @brief Forget what the last case captured. */
static void fresh(void)
{
    (void)memset(&s_args, 0, sizeof(s_args));
    s_creates = 0;
}

/**
 * @brief A callback these cases never let fire.
 *
 * Only its address matters: khal_timer_periodic() is being asked to fail
 * before the timer ever runs. It exists as a properly typed function rather
 * than a cast of whatever was to hand - GCC rejects a cast between
 * incompatible function types under -Werror=cast-function-type, clang does
 * not, and the cast said something untrue about what was being passed.
 */
static void never_fired(void *arg)
{
    (void)arg;
}

/* ---- Reading the clock --------------------------------------------------- */

TEST_CASE("microseconds pass straight through", "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_get_time_ExpectAndReturn(1234567);
    TEST_ASSERT_EQUAL_UINT64(1234567U, khal_time_us());

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* Milliseconds truncate rather than round. Worth pinning at the boundary,
 * because a caller measuring an elapsed time across it sees the step. */
TEST_CASE("milliseconds truncate, they do not round", "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_get_time_ExpectAndReturn(999);
    TEST_ASSERT_EQUAL_UINT32(0U, khal_time_ms());

    esp_timer_get_time_ExpectAndReturn(1000);
    TEST_ASSERT_EQUAL_UINT32(1U, khal_time_ms());

    esp_timer_get_time_ExpectAndReturn(1999);
    TEST_ASSERT_EQUAL_UINT32(1U, khal_time_ms());

    esp_timer_get_time_ExpectAndReturn(2000);
    TEST_ASSERT_EQUAL_UINT32(2U, khal_time_ms());

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* klog stamps every line from this, and the field is a uint32 of
 * milliseconds - so it wraps after about 49 days of uptime. The wrap is
 * documented; this is what makes it a fact. */
TEST_CASE("milliseconds wrap at 32 bits, as the log format assumes",
          "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    /* 2^32 milliseconds in microseconds: the first value that wraps to 0. */
    esp_timer_get_time_ExpectAndReturn(4294967296LL * 1000LL);
    TEST_ASSERT_EQUAL_UINT32(0U, khal_time_ms());

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* ---- Arming a periodic timer --------------------------------------------- */

TEST_CASE("a timer is created with the caller's callback and argument",
          "[khal_time]")
{
    static int    marker;

    Mockesp_timer_Init();
    fresh();

    esp_timer_create_Stub(&capture_create);
    esp_timer_start_periodic_ExpectAndReturn(s_timer, 10000U, ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_timer_periodic(&never_fired, &marker, 10000U));

    TEST_ASSERT_EQUAL_INT(1, s_creates);
    TEST_ASSERT_EQUAL_PTR(&never_fired, s_args.callback);
    TEST_ASSERT_EQUAL_PTR(&marker, s_args.arg);
    TEST_ASSERT_EQUAL_STRING("khal_tick", s_args.name);

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* The period reaches esp_timer in microseconds, unconverted. kqp computes
 * 1000000/KQP_TICKS_PER_SEC and hands the result straight down, so the whole
 * system's QP tick is this number arriving intact. */
TEST_CASE("the period is passed in microseconds, unconverted", "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_create_Stub(&capture_create);
    esp_timer_start_periodic_ExpectAndReturn(s_timer, 10000U, ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_timer_periodic(&never_fired, NULL, 10000U));

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* ---- The paths a board cannot produce ------------------------------------ */

/* Out of timers. */
TEST_CASE("a timer that cannot be created is refused", "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_create_ExpectAnyArgsAndReturn(ESP_ERR_NO_MEM);

    /* No start expected: refusing here means never reaching it. */
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES,
        khal_timer_periodic(&never_fired, NULL, 10000U));

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* The branch this whole file is worth writing for. The timer was created and
 * then would not start, so it must be deleted on the way out - otherwise the
 * failure costs an esp_timer for the rest of the boot, on exactly the boot
 * where something is already wrong. Reproducing it on hardware means making
 * esp_timer_start_periodic fail on demand, which nobody can do. */
TEST_CASE("a timer that cannot be started is deleted, not leaked",
          "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_create_Stub(&capture_create);
    esp_timer_start_periodic_ExpectAndReturn(s_timer, 10000U, ESP_FAIL);

    /* The delete is the assertion: CMock fails the case if it never happens,
     * and fails it too if it happens with the wrong handle. */
    esp_timer_delete_ExpectAndReturn(s_timer, ESP_OK);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_timer_periodic(&never_fired, NULL, 10000U));

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* Even when the cleanup itself fails there is nothing more to be done, and
 * the caller still hears about the original failure rather than this one. */
TEST_CASE("a failed cleanup does not change what the caller is told",
          "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_create_Stub(&capture_create);
    esp_timer_start_periodic_ExpectAndReturn(s_timer, 10000U, ESP_FAIL);
    esp_timer_delete_ExpectAndReturn(s_timer, ESP_FAIL);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        khal_timer_periodic(&never_fired, NULL, 10000U));

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

/* ---- Stopping and restarting a periodic timer ---------------------------- */

/** @brief The callback the stop-and-restart cases name their timer by. */
static void stoppable(void *arg)
{
    (void)arg;
}

/** @brief A callback that no case ever starts. */
static void unknown(void *arg)
{
    (void)arg;
}

/** @brief The period the stop-and-restart cases start their timer with. */
#define STOP_PERIOD_US 1000U

TEST_CASE("a periodic timer stops, restarts with its own period, and repeats are free",
          "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_create_Stub(&capture_create);
    esp_timer_start_periodic_ExpectAndReturn(s_timer, STOP_PERIOD_US, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_timer_periodic(&stoppable, NULL, STOP_PERIOD_US));

    /* Running already: running it again touches nothing (no expectation). */
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_timer_periodic_run(&stoppable, true));

    /* One stop for two requests. */
    esp_timer_stop_ExpectAndReturn(s_timer, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_timer_periodic_run(&stoppable, false));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_timer_periodic_run(&stoppable, false));

    /* One start, at the period it was created with. */
    esp_timer_start_periodic_ExpectAndReturn(s_timer, STOP_PERIOD_US, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_timer_periodic_run(&stoppable, true));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_timer_periodic_run(&stoppable, true));

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

TEST_CASE("a callback that started no periodic timer cannot be stopped",
          "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_timer_periodic_run(&unknown, false));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, khal_timer_periodic_run(NULL, true));

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}

TEST_CASE("a stop that esp_timer refuses is reported and the timer counts as running",
          "[khal_time]")
{
    Mockesp_timer_Init();
    fresh();

    esp_timer_create_Stub(&capture_create);
    esp_timer_start_periodic_ExpectAndReturn(s_timer, STOP_PERIOD_US, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        khal_timer_periodic(&stoppable, NULL, STOP_PERIOD_US));

    esp_timer_stop_ExpectAndReturn(s_timer, ESP_ERR_INVALID_STATE);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, khal_timer_periodic_run(&stoppable, false));

    /* Still believed running, so asking again asks esp_timer again. */
    esp_timer_stop_ExpectAndReturn(s_timer, ESP_OK);
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_timer_periodic_run(&stoppable, false));

    Mockesp_timer_Verify();
    Mockesp_timer_Destroy();
}
