/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kbsp_exp.c
 * @brief The expansion header's table: fourteen positions, and what each may be.
 */
#include "unity.h"

#include "kbsp_board.h"
#include "kbsp_exp.h"
#include "kbsp_owners.h"
#include "kbsp_pins.h"

TEST_CASE("the header has fourteen positions carrying GPIO8..21 once each",
          "[kbsp_exp]")
{
    bool seen[64] = { false };
    static const uint8_t order[KBSP_EXP_COUNT] = {
        KBSP_PIN_EXP_1,  KBSP_PIN_EXP_2,  KBSP_PIN_EXP_3,  KBSP_PIN_EXP_4,
        KBSP_PIN_EXP_5,  KBSP_PIN_EXP_6,  KBSP_PIN_EXP_7,  KBSP_PIN_EXP_8,
        KBSP_PIN_EXP_9,  KBSP_PIN_EXP_10, KBSP_PIN_EXP_11, KBSP_PIN_EXP_12,
        KBSP_PIN_EXP_13, KBSP_PIN_EXP_14,
    };

    for ( unsigned i = 0U; KBSP_EXP_COUNT > i; ++i )
    {
        uint8_t const g = KBSP_EXP[i].gpio;
        TEST_ASSERT_EQUAL_UINT8(order[i], g);
        TEST_ASSERT_TRUE(8U <= g);
        TEST_ASSERT_TRUE(21U >= g);
        TEST_ASSERT_FALSE_MESSAGE(seen[g], "a GPIO appears twice");
        seen[g] = true;
    }
}

TEST_CASE("ADC1 is on GPIO16..21 and nowhere else, with the P4's channels",
          "[kbsp_exp]")
{
    for ( unsigned i = 0U; KBSP_EXP_COUNT > i; ++i )
    {
        uint8_t const g = KBSP_EXP[i].gpio;
        if ( (16U <= g) && (21U >= g) )
        {
            TEST_ASSERT_EQUAL_UINT8((uint8_t)(g - 16U), KBSP_EXP[i].adc_chan);
        }
        else
        {
            TEST_ASSERT_EQUAL_UINT8(KBSP_EXP_NO_ADC, KBSP_EXP[i].adc_chan);
        }
    }
}

TEST_CASE("the LP positions are GPIO8..15 and nowhere else", "[kbsp_exp]")
{
    for ( unsigned i = 0U; KBSP_EXP_COUNT > i; ++i )
    {
        uint8_t const g = KBSP_EXP[i].gpio;
        TEST_ASSERT_EQUAL(15U >= g, KBSP_EXP[i].lp);
    }
}

TEST_CASE("the header is a resource with a name", "[kbsp_exp]")
{
    TEST_ASSERT_EQUAL_STRING("expansion header (J3)", kbsp_res_name(KBSP_RES_EXP));
}

/* ---- The controllers ------------------------------------------------------- */

TEST_CASE("the app's controllers are I2C1, SPI3 and the five UARTs",
          "[kbsp_exp]")
{
    static const kbsp_exp_ctrl_t want[] = {
        { KBSP_EXP_I2C,  1U },
        { KBSP_EXP_SPI,  2U },   /* SPI3_HOST */
#ifndef CONFIG_ESP_CONSOLE_UART
        { KBSP_EXP_UART, 0U },
#endif
        { KBSP_EXP_UART, 1U }, { KBSP_EXP_UART, 2U },
        { KBSP_EXP_UART, 3U }, { KBSP_EXP_UART, 4U },
    };

    /* UART0 is the app's only while the console is not a UART: the board's
     * console is the USB, the host build's is a UART, so this case sees both
     * shapes of the table across the two builds. */
    TEST_ASSERT_EQUAL_UINT(sizeof(want) / sizeof(want[0]), KBSP_EXP_CTRL_COUNT);
    for ( size_t i = 0U; KBSP_EXP_CTRL_COUNT > i; ++i )
    {
        TEST_ASSERT_EQUAL_INT(want[i].bus, KBSP_EXP_CTRL[i].bus);
        TEST_ASSERT_EQUAL_UINT8(want[i].id, KBSP_EXP_CTRL[i].id);
    }
}

TEST_CASE("no controller an app may take is one kodeOS owns", "[kbsp_exp]")
{
    for ( size_t i = 0U; KBSP_EXP_CTRL_COUNT > i; ++i )
    {
        if ( KBSP_EXP_I2C == KBSP_EXP_CTRL[i].bus )
        {
            TEST_ASSERT_NOT_EQUAL(KBSP_I2C_BOARD, KBSP_EXP_CTRL[i].id);
        }
        if ( KBSP_EXP_SPI == KBSP_EXP_CTRL[i].bus )
        {
            TEST_ASSERT_NOT_EQUAL(KBSP_SPI_NFC, KBSP_EXP_CTRL[i].id);
            /* SPI1_HOST, IDF's 0, is the flash. */
            TEST_ASSERT_NOT_EQUAL(0U, KBSP_EXP_CTRL[i].id);
        }
    }
    /* J3's analog positions and the RFID line are on different units. */
    TEST_ASSERT_NOT_EQUAL(KBSP_ADC_EXP, KBSP_ADC_RFID);
}
