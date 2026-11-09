/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_mfc.c
 * @brief Mifare Classic: the guards that protect a card from a wrong write.
 */
#include "unity.h"

#include "Mockkdrv_st25r3916.h"
#include "knfc_mfc.h"

void setUp(void);
void tearDown(void);

TEST_CASE("a sector trailer is every fourth block on a 1K", "[knfc_mfc]")
{
    /* Blocks 3, 7, 11 ... are trailers; the data blocks between them are
     * not. Writing a trailer changes the keys, so this is what stops it. */
    TEST_ASSERT_TRUE(knfc_mfc_is_trailer(3U));
    TEST_ASSERT_TRUE(knfc_mfc_is_trailer(7U));
    TEST_ASSERT_TRUE(knfc_mfc_is_trailer(63U));
    TEST_ASSERT_FALSE(knfc_mfc_is_trailer(1U));
    TEST_ASSERT_FALSE(knfc_mfc_is_trailer(4U));
    TEST_ASSERT_FALSE(knfc_mfc_is_trailer(62U));
}

TEST_CASE("a 4K's high sectors have sixteen-block trailers", "[knfc_mfc]")
{
    /* Above block 128 the sectors are four times as large, so the trailer
     * arithmetic changes with them. */
    TEST_ASSERT_TRUE(knfc_mfc_is_trailer(143U));
    TEST_ASSERT_TRUE(knfc_mfc_is_trailer(159U));
    TEST_ASSERT_FALSE(knfc_mfc_is_trailer(140U));
}

TEST_CASE("a write to a trailer or block 0 is refused without a frame",
          "[knfc_mfc]")
{
    uint8_t data[KNFC_MFC_BLOCK_LEN] = { 0 };

    Mockkdrv_st25r3916_Init();
    /* No transceive is expected. Reaching for one fails the case, which is
     * the whole assertion: the block never went out. */
    knfc_mfc_reset();          /* not authenticated, but the block check is
                                * what refuses first for a trailer */
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_mfc_write(3U, data));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_mfc_write(0U, data));
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("reading or writing without authenticating first is refused",
          "[knfc_mfc]")
{
    uint8_t data[KNFC_MFC_BLOCK_LEN] = { 0 };
    uint8_t out[KNFC_MFC_BLOCK_LEN];

    Mockkdrv_st25r3916_Init();
    knfc_mfc_reset();
    /* Block 4 is a data block, so it is the authentication that must refuse
     * it, and again with no frame sent. */
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_mfc_read(4U, out));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_mfc_write(4U, data));
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("every entry point refuses a NULL", "[knfc_mfc]")
{
    uint8_t uid[4] = { 0 };
    uint8_t key[KNFC_MFC_KEY_LEN] = { 0 };

    Mockkdrv_st25r3916_Init();
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_mfc_auth(4U, KNFC_MFC_KEY_A, NULL, uid));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_mfc_auth(4U, KNFC_MFC_KEY_A, key, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_mfc_auth_any(4U, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_mfc_read(4U, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_mfc_write(4U, NULL));
    Mockkdrv_st25r3916_Verify();
}
