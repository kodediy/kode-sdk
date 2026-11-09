/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_ident.c
 * @brief The reader's identity judgement: the park's, and RFAL's.
 */
#include "unity.h"

#include "kdrv_st25r3916.h"
#include "knfc_ident.h"

/** @brief The plain ST25R3916's type code, which this build does not drive. */
#define ST25R3916_PLAIN 0x28U

TEST_CASE("the board's ST25R3916B is a reader the park knows", "[knfc_ident]")
{
    TEST_ASSERT_TRUE(knfc_ident_supported(KDRV_ST25R3916_IDENTITY));
    TEST_ASSERT_TRUE(knfc_ident_supported(0x37U));
}

TEST_CASE("a revision 0 B, a plain ST25R3916 and an empty bus are not",
          "[knfc_ident]")
{
    TEST_ASSERT_FALSE(knfc_ident_supported(KDRV_ST25R3916_IC_TYPE_B));
    TEST_ASSERT_FALSE(knfc_ident_supported(ST25R3916_PLAIN | 1U));
    TEST_ASSERT_FALSE(knfc_ident_supported(0x00U));
    TEST_ASSERT_FALSE(knfc_ident_supported(0xFFU));
}
