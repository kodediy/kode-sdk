/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_appmgr_backup.c
 * @brief AO_AppMgr's table of which installed apps have a copy on the card.
 */
#include <string.h>

#include "unity.h"

#include "appmgr_backup.h"

static appmgr_backup_t s_table;

/* At boot nothing is known: the card is asked about every app. */
TEST_CASE("a table starts with every app unseen", "[appmgr_backup]")
{
    appmgr_backup_reset(&s_table);

    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 1U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 77U));
}

TEST_CASE("a state set for an app is read back, and no other app's changes",
          "[appmgr_backup]")
{
    appmgr_backup_reset(&s_table);
    appmgr_backup_set(&s_table, 5U, APPMGR_BACKUP_COPIED);
    appmgr_backup_set(&s_table, 9U, APPMGR_BACKUP_SKIPPED);

    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_COPIED, appmgr_backup_get(&s_table, 5U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_SKIPPED, appmgr_backup_get(&s_table, 9U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 6U));

    appmgr_backup_set(&s_table, 9U, APPMGR_BACKUP_COPIED);
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_COPIED, appmgr_backup_get(&s_table, 9U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_COPIED, appmgr_backup_get(&s_table, 5U));
}

/* A card mounted and a card gone are the same to the table as a boot. */
TEST_CASE("a reset makes every app unseen again", "[appmgr_backup]")
{
    appmgr_backup_reset(&s_table);
    appmgr_backup_set(&s_table, 5U, APPMGR_BACKUP_COPIED);
    appmgr_backup_set(&s_table, 9U, APPMGR_BACKUP_SKIPPED);

    appmgr_backup_reset(&s_table);

    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 5U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 9U));
}

/* An app erased or evicted leaves the table, and its place is free. */
TEST_CASE("an app forgotten is unseen and gives its place to another",
          "[appmgr_backup]")
{
    appmgr_backup_reset(&s_table);
    for ( uint32_t seq = 1U; KAPPS_MAX_ENTRIES >= seq; ++seq )
    {
        appmgr_backup_set(&s_table, seq, APPMGR_BACKUP_COPIED);
    }

    appmgr_backup_forget(&s_table, 3U);
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 3U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_COPIED, appmgr_backup_get(&s_table, KAPPS_MAX_ENTRIES));

    appmgr_backup_set(&s_table, 500U, APPMGR_BACKUP_SKIPPED);
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_SKIPPED, appmgr_backup_get(&s_table, 500U));
    appmgr_backup_forget(&s_table, 4242U);   /* never there: nothing to do */
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_COPIED, appmgr_backup_get(&s_table, 1U));
}

/* The table holds one place per directory entry. An app past them reads as
 * unseen, which only means the card is asked about it again. */
TEST_CASE("a full table leaves one app more unseen", "[appmgr_backup]")
{
    appmgr_backup_reset(&s_table);
    for ( uint32_t seq = 1U; KAPPS_MAX_ENTRIES >= seq; ++seq )
    {
        appmgr_backup_set(&s_table, seq, APPMGR_BACKUP_SKIPPED);
    }

    appmgr_backup_set(&s_table, 900U, APPMGR_BACKUP_COPIED);

    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 900U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_SKIPPED, appmgr_backup_get(&s_table, 1U));
}

/* Setting an app unseen is forgetting it: the table keeps only what it knows. */
TEST_CASE("an app set unseen takes no place", "[appmgr_backup]")
{
    appmgr_backup_reset(&s_table);
    for ( uint32_t seq = 1U; KAPPS_MAX_ENTRIES >= seq; ++seq )
    {
        appmgr_backup_set(&s_table, seq, APPMGR_BACKUP_COPIED);
    }

    appmgr_backup_set(&s_table, 2U, APPMGR_BACKUP_UNSEEN);
    appmgr_backup_set(&s_table, 900U, APPMGR_BACKUP_COPIED);

    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_UNSEEN, appmgr_backup_get(&s_table, 2U));
    TEST_ASSERT_EQUAL_INT(APPMGR_BACKUP_COPIED, appmgr_backup_get(&s_table, 900U));
}

/* A file of the app's name is its copy at the image's length and with its
 * build id: a new version of the same length is another app to the card. */
TEST_CASE("a file is an app's copy only at its length and with its build id",
          "[appmgr_backup]")
{
    uint8_t image[KAPP_BUILD_LEN];
    uint8_t file[KAPP_BUILD_LEN];

    (void)memset(image, 0x21, sizeof(image));
    (void)memset(file, 0x21, sizeof(file));
    TEST_ASSERT_TRUE(appmgr_backup_same(4096U, file, 4096U, image));
    TEST_ASSERT_FALSE_MESSAGE(appmgr_backup_same(4095U, file, 4096U, image),
                              "another length was taken for the copy");

    file[KAPP_BUILD_LEN - 1U] ^= 0x01U;
    TEST_ASSERT_FALSE_MESSAGE(appmgr_backup_same(4096U, file, 4096U, image),
                              "another build was taken for the copy");
}
