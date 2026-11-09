/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_harness.c
 * @brief The suite proving it can run at all.
 */
#include "unity.h"

TEST_CASE("the harness runs a case and reports it", "[harness]")
{
    TEST_ASSERT_EQUAL_INT(4, 2 + 2);
}
