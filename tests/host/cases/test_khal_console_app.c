/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_console_app.c
 * @brief The console's writes to a cable port the app provides.
 */
#include "unity.h"

#include "khal_console.h"
#include "khal_console_app.h"

static size_t   s_answer;      /* What the app's port takes at its next write. */
static uint32_t s_last_wait;   /* The wait the last write was given, in ms. */
static uint32_t s_calls;       /* Writes that reached the app's port. */

/* The app's side of the seam: it takes what the case scripted. */
size_t khal_console_app_write(const char *buf, size_t len, uint32_t wait_ms)
{
    (void)buf;
    (void)len;
    s_last_wait = wait_ms;
    ++s_calls;
    return s_answer;
}

/** @brief What the app's port takes at its next write. */
static void port_answers(size_t taken)
{
    s_answer = taken;
}

/** @brief The wait the last write was given, in ms. */
static uint32_t port_last_wait(void)
{
    return s_last_wait;
}

/** @brief Writes that reached the app's port so far. */
static uint32_t port_calls(void)
{
    return s_calls;
}

/** @brief Start a case on a port that took its last write whole. */
static void port_fresh(void)
{
    port_answers(1U);
    khal_console_app_put("x", 1U);
}

TEST_CASE("an app's port takes a whole write and drops nothing", "[khal_console_app]")
{
    port_fresh();
    uint32_t const before = khal_console_app_dropped();

    port_answers(5U);
    khal_console_app_put("hello", 5U);
    TEST_ASSERT_EQUAL_UINT32(KHAL_CONSOLE_TX_WAIT_MS, port_last_wait());
    TEST_ASSERT_EQUAL_UINT32(before, khal_console_app_dropped());
}

TEST_CASE("an app's port that takes a part counts the rest and is not waited for again", "[khal_console_app]")
{
    port_fresh();
    uint32_t const before = khal_console_app_dropped();

    port_answers(2U);
    khal_console_app_put("hello", 5U);
    TEST_ASSERT_EQUAL_UINT32(before + 3U, khal_console_app_dropped());
    port_answers(0U);
    khal_console_app_put("hello", 5U);
    TEST_ASSERT_EQUAL_UINT32(0U, port_last_wait());
    TEST_ASSERT_EQUAL_UINT32(before + 8U, khal_console_app_dropped());
}

TEST_CASE("an app's port is waited for again once a write fits", "[khal_console_app]")
{
    port_fresh();
    port_answers(0U);
    khal_console_app_put("hello", 5U);
    port_answers(5U);
    khal_console_app_put("hello", 5U);
    port_answers(5U);
    khal_console_app_put("hello", 5U);
    TEST_ASSERT_EQUAL_UINT32(KHAL_CONSOLE_TX_WAIT_MS, port_last_wait());
}

TEST_CASE("an empty write never reaches an app's port", "[khal_console_app]")
{
    port_fresh();
    uint32_t const calls = port_calls();

    khal_console_app_put("", 0U);
    TEST_ASSERT_EQUAL_UINT32(calls, port_calls());
}

TEST_CASE("an app's port that answers more than it was given took it all", "[khal_console_app]")
{
    port_fresh();
    uint32_t const before = khal_console_app_dropped();

    port_answers(9U);
    khal_console_app_put("hello", 5U);
    TEST_ASSERT_EQUAL_UINT32(before, khal_console_app_dropped());
    port_answers(5U);
    khal_console_app_put("hello", 5U);
    TEST_ASSERT_EQUAL_UINT32(KHAL_CONSOLE_TX_WAIT_MS, port_last_wait());
}
