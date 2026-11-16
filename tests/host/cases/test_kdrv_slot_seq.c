/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_slot_seq.c
 * @brief Which kodeOS slot otadata selected last, from its two entries.
 */
#include <stdbool.h>
#include <stdint.h>

#include "unity.h"

#include "kdrv_slot_seq.h"

#define OTA_0  0U   /**< ota_0's index. */
#define OTA_1  1U   /**< ota_1's index. */
#define SLOTS  2U   /**< The table's OTA slots. */

TEST_CASE("a switch to the other slot is the newer entry", "[slot_seq]")
{
    uint32_t const seq[KSLOT_SEQ_ENTRIES]   = { 3U, 4U };   /* 3 names ota_0, 4 ota_1 */
    bool const     valid[KSLOT_SEQ_ENTRIES] = { true, true };

    TEST_ASSERT_TRUE(kslot_seq_newer(seq, valid, OTA_1, OTA_0, SLOTS));
    TEST_ASSERT_FALSE(kslot_seq_newer(seq, valid, OTA_0, OTA_1, SLOTS));
}

TEST_CASE("an older entry of the other slot is an earlier update's", "[slot_seq]")
{
    uint32_t const seq[KSLOT_SEQ_ENTRIES]   = { 5U, 4U };   /* ota_0 selected after ota_1 */
    bool const     valid[KSLOT_SEQ_ENTRIES] = { true, true };

    TEST_ASSERT_FALSE(kslot_seq_newer(seq, valid, OTA_1, OTA_0, SLOTS));
}

TEST_CASE("an entry that is not one names nobody", "[slot_seq]")
{
    uint32_t const seq[KSLOT_SEQ_ENTRIES]   = { 1U, 2U };
    bool const     bad[KSLOT_SEQ_ENTRIES]   = { true, false };
    bool const     blank[KSLOT_SEQ_ENTRIES] = { false, false };
    uint32_t const zero[KSLOT_SEQ_ENTRIES]  = { 0U, 0U };
    bool const     both[KSLOT_SEQ_ENTRIES]  = { true, true };

    TEST_ASSERT_FALSE_MESSAGE(kslot_seq_newer(seq, bad, OTA_1, OTA_0, SLOTS),
        "an entry whose CRC fails was believed");
    TEST_ASSERT_FALSE(kslot_seq_newer(seq, blank, OTA_1, OTA_0, SLOTS));
    TEST_ASSERT_FALSE_MESSAGE(kslot_seq_newer(zero, both, OTA_1, OTA_0, SLOTS),
        "a sequence of 0 named a slot");
    TEST_ASSERT_FALSE_MESSAGE(kslot_seq_newer(seq, both, OTA_1, OTA_0, 0U),
        "a table with no slots named one");
}

TEST_CASE("the other slot's only entry outranks a running slot with none", "[slot_seq]")
{
    uint32_t const seq[KSLOT_SEQ_ENTRIES]   = { 2U, 0U };   /* 2 names ota_1 */
    bool const     valid[KSLOT_SEQ_ENTRIES] = { true, false };

    TEST_ASSERT_TRUE(kslot_seq_newer(seq, valid, OTA_1, OTA_0, SLOTS));
}
