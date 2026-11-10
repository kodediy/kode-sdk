/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file runner.c
 * @brief Entry point of the kdrv_es8311 pins suite.
 */
#include <stdlib.h>

#include "unity.h"

#include "fakes.h"

void setUp(void)
{
}

/** @brief After every case: the witnesses saw nothing the driver must not do. */
void tearDown(void)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_viol_pad,
                                   "the clock pad was let go while a channel was enabled");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, g_viol_ctrl,
                                   "the chip was written with its clock not on the pad");
}

void app_main(void)
{
    UNITY_BEGIN();
    unity_run_all_tests();
    exit(UNITY_END());
}
