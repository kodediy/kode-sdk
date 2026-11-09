/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_slot_crash.c
 * @brief The word that counts a slot's crashes: its layout and whose it is.
 */
#include <stddef.h>

#include "unity.h"

#include "kdrv_slot_crash.h"

#define OTA_0  0x10U
#define OTA_1  0x11U

TEST_CASE("a word is read back for the slot that wrote it", "[slot_crash]")
{
    uint32_t count = 99U;

    TEST_ASSERT_TRUE(kslot_crash_read(kslot_crash_pack(OTA_1, 2U), OTA_1, &count));
    TEST_ASSERT_EQUAL_UINT32(2U, count);
}

TEST_CASE("a word of none is this slot's and holds none", "[slot_crash]")
{
    uint32_t count = 99U;

    TEST_ASSERT_TRUE(kslot_crash_read(kslot_crash_pack(OTA_0, 0U), OTA_0, &count));
    TEST_ASSERT_EQUAL_UINT32(0U, count);
}

TEST_CASE("another slot's word is not this slot's", "[slot_crash]")
{
    uint32_t count = 99U;

    TEST_ASSERT_FALSE(kslot_crash_read(kslot_crash_pack(OTA_0, 2U), OTA_1, &count));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(99U, count, "nothing is read from it");
}

TEST_CASE("a word with no magic is nobody's", "[slot_crash]")
{
    /* Erased, all ones, the panel's and the radio's magics over a good slot. */
    static const uint32_t WORDS[] = {
        0x00000000UL, 0xFFFFFFFFUL, 0x4B521002UL, 0x4B501002UL, 0x00001002UL,
    };
    uint32_t count = 99U;

    for ( size_t i = 0U; (sizeof(WORDS) / sizeof(WORDS[0])) > i; ++i )
    {
        TEST_ASSERT_FALSE(kslot_crash_read(WORDS[i], OTA_0, &count));
    }
}

TEST_CASE("the count does not reach the slot's field", "[slot_crash]")
{
    uint32_t count = 0U;

    TEST_ASSERT_TRUE(kslot_crash_read(kslot_crash_pack(OTA_0, 0x1FFU), OTA_0, &count));
    TEST_ASSERT_EQUAL_UINT32(0xFFU, count);
}
