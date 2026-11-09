/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_klog_level.c
 * @brief klog's compile-time ceiling, seen from a file that lowered its own.
 */
/* Before any include: kode_log.h's per-file ceiling. The rest of the suite keeps the default. */
#define KLOG_MAXIMUM_LEVEL  KODE_LOG_WARN

#include "unity.h"

#include "fakes.h"
#include "kode_log.h"

#define TAG "t.klvl"

TEST_CASE("a line below the compiled ceiling is not there to turn on",
          "[klog]")
{
    kode_log_set_level(NULL, KODE_LOG_DEBUG);

    KLOG_D(TAG, "d");
    KLOG_I(TAG, "i");
    KLOG_W(TAG, "w");
    KLOG_E(TAG, "e");

    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!W 0 t.klvl w\n", fake_klog_line(0U));
    TEST_ASSERT_EQUAL_STRING("!E 0 t.klvl e\n", fake_klog_line(1U));
}

/* Compiled out means the line, not the work: an argument with a side effect
 * still runs, because call sites do real work inside one (the panel is
 * switched on inside a KLOG_D). And it is still an expression, so it goes
 * where a call went. */
TEST_CASE("a line compiled out still evaluates its arguments and is still an expression",
          "[klog]")
{
    int calls = 0;

    kode_log_set_level(NULL, KODE_LOG_DEBUG);

    KLOG_I(TAG, "%d", ++calls);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, calls,
        "a compiled-out line must still run its arguments' side effects");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    (void)(1 == calls ? KLOG_E(TAG, "kept") : KLOG_I(TAG, "gone"));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    TEST_ASSERT_EQUAL_STRING("!E 0 t.klvl kept\n", fake_klog_last());
}
