/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_banner.c
 * @brief The boot banner's words: the reset table and the reset line.
 */
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kode_banner.h"
#include "kode_log.h"
#include "kdrv_slot.h"

#define DUMP_STUCK   "AO stuck: Net, 30 s in one event"  /**< A supervisor panic's reason. */
#define LINE_ROOM    96U                                  /**< Plenty for any line below.   */
#define SMALL_ROOM   8U                                   /**< Too small for any line.      */
#define UNLISTED     200                                  /**< A number ESP-IDF never uses. */

TEST_CASE("every reset reason has its words and an unknown number reads unknown",
          "[banner]")
{
    for ( int r = (int)KDRV_RESET_UNKNOWN; (int)KDRV_RESET_CPU_LOCKUP >= r; ++r )
    {
        char const *const w = kode_reset_words(r);
        TEST_ASSERT_NOT_NULL(w);
        TEST_ASSERT_TRUE_MESSAGE('\0' != w[0], "a reason with no words");
    }
    TEST_ASSERT_EQUAL_STRING("unknown", kode_reset_words(UNLISTED));
    TEST_ASSERT_EQUAL_STRING("unknown", kode_reset_words(-1));
    TEST_ASSERT_NOT_NULL(strstr(kode_reset_words((int)KDRV_RESET_POWERON), "BOT+LEFT"));
}

TEST_CASE("the reset line gives a task watchdog the dump's reason", "[banner]")
{
    char line[LINE_ROOM];
    size_t const n = kode_banner_reset_line(line, sizeof(line),
                                            (int)KDRV_RESET_TASK_WDT,
                                            DUMP_STUCK, NULL);
    TEST_ASSERT_EQUAL_STRING("reset " DUMP_STUCK, line);
    TEST_ASSERT_EQUAL_UINT(strlen(line), n);
}

TEST_CASE("the reset line uses the dump for a panic and the interrupt watchdog too",
          "[banner]")
{
    char line[LINE_ROOM];
    (void)kode_banner_reset_line(line, sizeof(line), (int)KDRV_RESET_PANIC, "boom", NULL);
    TEST_ASSERT_EQUAL_STRING("reset boom", line);
    (void)kode_banner_reset_line(line, sizeof(line), (int)KDRV_RESET_INT_WDT, "boom", NULL);
    TEST_ASSERT_EQUAL_STRING("reset boom", line);
}

TEST_CASE("the reset line without a dump says the words", "[banner]")
{
    char line[LINE_ROOM];
    (void)kode_banner_reset_line(line, sizeof(line), (int)KDRV_RESET_TASK_WDT, NULL, NULL);
    TEST_ASSERT_EQUAL_STRING("reset task watchdog", line);
    (void)kode_banner_reset_line(line, sizeof(line), (int)KDRV_RESET_TASK_WDT, "", NULL);
    TEST_ASSERT_EQUAL_STRING("reset task watchdog", line);
}

TEST_CASE("the reset line of a power-on ignores yesterday's dump", "[banner]")
{
    char line[LINE_ROOM];
    (void)kode_banner_reset_line(line, sizeof(line), (int)KDRV_RESET_POWERON,
                                 DUMP_STUCK, NULL);
    TEST_ASSERT_NULL(strstr(line, DUMP_STUCK));
    TEST_ASSERT_NOT_NULL(strstr(line, "BOT+LEFT"));
}

TEST_CASE("the reset line names the app the board came back from", "[banner]")
{
    char line[LINE_ROOM];
    (void)kode_banner_reset_line(line, sizeof(line), (int)KDRV_RESET_PANIC,
                                 NULL, "my_app");
    TEST_ASSERT_EQUAL_STRING("reset back from app my_app: panic", line);
}

TEST_CASE("the reset line of a return from an app ignores the dump", "[banner]")
{
    char line[LINE_ROOM];
    (void)kode_banner_reset_line(line, sizeof(line), (int)KDRV_RESET_PANIC,
                                 DUMP_STUCK, "my_app");
    TEST_ASSERT_EQUAL_STRING("reset back from app my_app: panic", line);
}

TEST_CASE("the reset line in a small buffer is cut and still terminated",
          "[banner]")
{
    char line[SMALL_ROOM];
    (void)memset(line, 'x', sizeof(line));
    size_t const n = kode_banner_reset_line(line, sizeof(line),
                                            (int)KDRV_RESET_TASK_WDT,
                                            DUMP_STUCK, NULL);
    TEST_ASSERT_EQUAL_UINT(sizeof(line) - 1U, n);
    TEST_ASSERT_EQUAL_CHAR('\0', line[sizeof(line) - 1U]);
    TEST_ASSERT_EQUAL_STRING("reset A", line);

    TEST_ASSERT_EQUAL_UINT(0U, kode_banner_reset_line(line, 0U, 0, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT(0U, kode_banner_reset_line(NULL, SMALL_ROOM, 0, NULL, NULL));
}

/* ---- kode_log_always(): past the filters, not past off ------------------- */

TEST_CASE("the banner line passes a filter that holds NOTICE back", "[banner]")
{
    kode_log_set_level(NULL, KODE_LOG_ERROR);
    kode_log_set_level("t.ban", KODE_LOG_ERROR);

    kode_log_always(KODE_LOG_NOTICE, "t.ban", "hello %d", 7);

    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!N 0 t.ban hello 7\n", fake_klog_last());
}

TEST_CASE("the banner line stays quiet when the log is off", "[banner]")
{
    kode_log_set_level(NULL, KODE_LOG_OFF);
    kode_log_always(KODE_LOG_NOTICE, "t.ban", "hello");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

TEST_CASE("an always line prints at the level it was given, past a filter at error",
          "[banner]")
{
    kode_log_set_level(NULL, KODE_LOG_ERROR);
    kode_log_set_level("t.ban", KODE_LOG_ERROR);

    kode_log_always(KODE_LOG_WARN, "t.ban", "slow %u", 1234U);

    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!W 0 t.ban slow 1234\n", fake_klog_last());
}

TEST_CASE("an always line at warn stays quiet when the log is off", "[banner]")
{
    kode_log_set_level(NULL, KODE_LOG_OFF);
    kode_log_always(KODE_LOG_WARN, "t.ban", "slow");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

TEST_CASE("an always line refuses a level that is not one", "[banner]")
{
    kode_log_set_level(NULL, KODE_LOG_ERROR);
    kode_log_always(KODE_LOG_OFF, "t.ban", "no");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}
