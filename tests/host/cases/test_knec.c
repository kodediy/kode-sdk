/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knec.c
 * @brief The NEC codec: what it builds, what it accepts, and what the
 *        receiver's AGC does to both.
 */
#include <string.h>

#include "unity.h"

#include "knec.h"

/** @brief Encode into @p out and assert it succeeded with a full frame. */
static void encode_ok(uint16_t addr, uint16_t cmd, kdrv_ir_symbol_t *out)
{
    size_t n = 0U;

    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          knec_encode(addr, cmd, out, KNEC_SYMBOLS, &n));
    TEST_ASSERT_EQUAL_UINT(KNEC_SYMBOLS, n);
}

TEST_CASE("a frame round-trips for any pair of values", "[knec]")
{
    static const uint16_t PAIRS[][2] = {
        { 0x0000U, 0x0000U },
        { 0xFFFFU, 0xFFFFU },
        { 0x0004U, 0x0008U },   /* the LG power code */
        { 0x1234U, 0x5678U },
        { 0x00FFU, 0xFF00U },
    };
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];

    for ( size_t i = 0U; (sizeof(PAIRS) / sizeof(PAIRS[0])) > i; ++i )
    {
        uint16_t addr = 0U;
        uint16_t cmd  = 0U;

        encode_ok(PAIRS[i][0], PAIRS[i][1], sym);
        TEST_ASSERT_TRUE(knec_decode(sym, KNEC_SYMBOLS, &addr, &cmd));
        TEST_ASSERT_EQUAL_HEX16(PAIRS[i][0], addr);
        TEST_ASSERT_EQUAL_HEX16(PAIRS[i][1], cmd);
    }
}

TEST_CASE("encoding refuses a buffer that cannot hold a frame", "[knec]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    size_t n = 99U;

    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          knec_encode(0x04U, 0x08U, sym, KNEC_SYMBOLS - 1U, &n));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
                          knec_encode(0x04U, 0x08U, NULL, KNEC_SYMBOLS, &n));
}

TEST_CASE("the receiver's AGC distortion still decodes", "[knec]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    uint16_t addr = 0U;
    uint16_t cmd  = 0U;

    encode_ok(0x1234U, 0x5678U, sym);

    /* Every mark longer and every space shorter by the excess: exactly what
     * comes back off the TSOP. A symmetric window would reject this. */
    for ( size_t i = 0U; KNEC_SYMBOLS > i; ++i )
    {
        sym[i].mark_us = (uint16_t)(sym[i].mark_us + KNEC_MARK_EXCESS_US);
        if ( KNEC_MARK_EXCESS_US < sym[i].space_us )
        {
            sym[i].space_us = (uint16_t)(sym[i].space_us - KNEC_MARK_EXCESS_US);
        }
    }

    TEST_ASSERT_TRUE(knec_decode(sym, KNEC_SYMBOLS, &addr, &cmd));
    TEST_ASSERT_EQUAL_HEX16(0x1234U, addr);
    TEST_ASSERT_EQUAL_HEX16(0x5678U, cmd);
}

TEST_CASE("distortion past the AGC's is rejected", "[knec]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    uint16_t addr = 0U;
    uint16_t cmd  = 0U;

    encode_ok(0x1234U, 0x5678U, sym);
    for ( size_t i = 0U; KNEC_SYMBOLS > i; ++i )
    {
        sym[i].mark_us = (uint16_t)(sym[i].mark_us
                                    + (3U * KNEC_MARK_EXCESS_US));
    }

    TEST_ASSERT_FALSE(knec_decode(sym, KNEC_SYMBOLS, &addr, &cmd));
}

TEST_CASE("a truncated capture is rejected, not guessed at", "[knec]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    uint16_t addr = 0U;
    uint16_t cmd  = 0U;

    encode_ok(0x1234U, 0x5678U, sym);
    TEST_ASSERT_FALSE(knec_decode(sym, KNEC_SYMBOLS - 4U, &addr, &cmd));
    TEST_ASSERT_FALSE(knec_decode(sym, 0U, &addr, &cmd));
    TEST_ASSERT_FALSE(knec_decode(NULL, KNEC_SYMBOLS, &addr, &cmd));
}

TEST_CASE("a frame with no leader is not a NEC frame", "[knec]")
{
    kdrv_ir_symbol_t sym[KNEC_SYMBOLS];
    uint16_t addr = 0U;
    uint16_t cmd  = 0U;

    encode_ok(0x1234U, 0x5678U, sym);
    sym[0].mark_us = 560U;   /* a bit mark where the 9 ms leader should be */

    TEST_ASSERT_FALSE(knec_decode(sym, KNEC_SYMBOLS, &addr, &cmd));
}
