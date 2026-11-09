/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kes_cap.c
 * @brief The speaker's cap, from the field table's side.
 */
#include "unity.h"

#include "Mockkdrv_es8311.h"

#include "kdrv_es8311.h"
#include "kes_fields.h"

/** @brief The register law's bottom: register / 2 - 95.5 dB, in tenth-dB. */
#define LAW_X10_AT_ZERO  (-955)
/** @brief One register step, in tenth-dB. */
#define LAW_X10_STEP        5

TEST_CASE("the cap is under full scale, on a register step, and covers the "
          "fitted 82K", "[kes_cap]")
{
    int32_t cap = KDRV_ES8311_OUT_DB_X10_MAX;

    TEST_ASSERT_TRUE_MESSAGE(0 > cap,
        "a cap at or above full scale caps nothing");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, cap % LAW_X10_STEP,
        "the register moves in half-decibels; a cap between two steps is "
        "truncated by the codec layer to the one below, and reads back wrong");
    TEST_ASSERT_TRUE_MESSAGE(-106 >= cap,
        "the fitted 82K needs 10.6 dB at the worst feedback resistor");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0xA9U,
        (uint32_t)((cap - LAW_X10_AT_ZERO) / LAW_X10_STEP),
        "the bench's acceptance reading: volume 100 leaves 0x32 at 0xA9");
}

TEST_CASE("volume 100 reads back as the cap, through the chip's own law",
          "[kes_cap]")
{
    static uint8_t reg32 = 0xA9U;
    int32_t        db    = 0;

    Mockkdrv_es8311_Init();
    kdrv_es8311_present_IgnoreAndReturn(true);
    kdrv_es8311_reg_read_ExpectAndReturn(0x32U, NULL, KODE_OK);
    kdrv_es8311_reg_read_IgnoreArg_val();
    kdrv_es8311_reg_read_ReturnThruPtr_val(&reg32);

    TEST_ASSERT_EQUAL_UINT(KODE_OK, kdrv_es8311_get(KDRV_ES_DAC_VOL_DB, &db));
    TEST_ASSERT_EQUAL_INT(KDRV_ES8311_OUT_DB_X10_MAX, db);
    Mockkdrv_es8311_Verify();
}

TEST_CASE("the three writes that could lift the speaker past its cap are "
          "refused, and reach nothing", "[kes_cap]")
{
    Mockkdrv_es8311_Init();

    /* No write is expected: the strict mock fails the case if one reaches the chip. */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KODE_DENIED,
        kdrv_es8311_set(KDRV_ES_REG32, 0xBF), "full scale, raw");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KODE_DENIED,
        kdrv_es8311_set(KDRV_ES_REG34, 0x86), "the compressor, as its byte");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KODE_DENIED,
        kdrv_es8311_set(KDRV_ES_DRC_EN, 1), "the compressor, as its bit");

    /* And the console is told so before it asks: what `audio_help` prints. */
    TEST_ASSERT_FALSE(kdrv_es8311_field_info(KDRV_ES_REG32)->writable);
    TEST_ASSERT_FALSE(kdrv_es8311_field_info(KDRV_ES_REG34)->writable);
    TEST_ASSERT_FALSE(kdrv_es8311_field_info(KDRV_ES_DRC_EN)->writable);
    Mockkdrv_es8311_Verify();
}

TEST_CASE("the volume scale stays 0 to 100 for every caller", "[kes_cap]")
{
    Mockkdrv_es8311_Init();

    TEST_ASSERT_EQUAL_INT((int32_t)KDRV_ES8311_VOL_MAX,
                          kdrv_es8311_field_info(KDRV_ES_VOL)->max);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KODE_DENIED,
        kdrv_es8311_set(KDRV_ES_VOL, (int32_t)KDRV_ES8311_VOL_MAX + 1),
        "past the top is refused, not clamped into the cap");

    /* The top itself is taken: the cap lives in the curve, not in the scale. */
    kdrv_es8311_apply_settings_Expect();
    TEST_ASSERT_EQUAL_UINT(KODE_OK,
        kdrv_es8311_set(KDRV_ES_VOL, (int32_t)KDRV_ES8311_VOL_MAX));
    Mockkdrv_es8311_Verify();
}
