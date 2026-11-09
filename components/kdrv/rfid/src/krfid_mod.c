/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file krfid_mod.c
 * @brief The Manchester modulator of the tag emulation (see krfid_mod.h).
 */
#include "krfid_mod.h"

#include <stddef.h>

#include "krfid_em4100.h"

/** @brief Manchester: the level of a 1's first half - a 1 loads the coil
 *         first and releases it at mid-bit. */
#define MANCHESTER_ONE_FIRST   1U

/** @brief Bits per byte. */
#define MOD_BYTE_BITS          8U

/** @brief The index of a byte's most significant bit. */
#define MOD_MSB_SHIFT          7U

/** @brief Bytes of an EM4100 frame. */
#define MOD_EM4100_BYTES       8U

/** @brief Bits of an EM4100 frame. */
#define MOD_EM4100_BITS        64U

_Static_assert(KRFID_MOD_EM4100_LEVELS == (MOD_EM4100_BITS * KRFID_MOD_EM4100_RF),
               "an EM4100 frame is 64 bits of 64 periods");

/**
 * @brief Read one bit of a frame, the first bit most significant.
 *
 * @param frame The frame.
 * @param i     The bit index.
 * @return 1U or 0U.
 */
static uint8_t frame_bit(const uint8_t *frame, uint16_t i)
{
    uint8_t const byte  = frame[i / MOD_BYTE_BITS];
    uint8_t const shift = (uint8_t)(MOD_MSB_SHIFT - (uint8_t)(i % MOD_BYTE_BITS));

    return (uint8_t)((uint8_t)(byte >> shift) & 1U);
}

/**
 * @brief Write one Manchester bit.
 *
 * @param rf  Carrier periods per bit.
 * @param bit The bit.
 * @param lv  Its rf levels.
 */
static void manchester_bit(uint8_t rf, uint8_t bit, uint8_t *lv)
{
    uint8_t const half  = (uint8_t)(rf / 2U);
    uint8_t       first = MANCHESTER_ONE_FIRST;
    uint8_t       k;

    if ( 0U == bit )
    {
        first = (uint8_t)(MANCHESTER_ONE_FIRST ^ 1U);
    }
    for ( k = 0U; k < rf; ++k )
    {
        if ( k < half )
        {
            lv[k] = first;
        }
        else
        {
            lv[k] = (uint8_t)(first ^ 1U);
        }
    }
}

uint32_t krfid_mod_manchester(uint8_t rf, const uint8_t *frame,
                              uint16_t nbits, uint8_t *lv, uint32_t cap)
{
    uint32_t at = 0U;
    uint16_t i;

    if ( (NULL == frame) || (NULL == lv) || (0U == nbits) )
    {
        return 0U;
    }
    if ( (0U == rf) || ((rf % 2U) != 0U) )
    {
        return 0U;
    }
    if ( ((uint32_t)nbits * (uint32_t)rf) > cap )
    {
        return 0U;
    }
    for ( i = 0U; i < nbits; ++i )
    {
        manchester_bit(rf, frame_bit(frame, i), &lv[at]);
        at += rf;
    }
    return at;
}

uint32_t krfid_mod_em4100(const uint8_t id[5], uint8_t *lv, uint32_t cap)
{
    uint8_t  frame[MOD_EM4100_BYTES];
    uint64_t f;
    uint8_t  i;

    if ( (NULL == id) || (NULL == lv) )
    {
        return 0U;
    }
    f = krfid_em4100_frame(id);
    for ( i = 0U; MOD_EM4100_BYTES > i; ++i )
    {
        uint8_t const shift = (uint8_t)(MOD_BYTE_BITS
                                        * (uint8_t)(MOD_EM4100_BYTES - 1U - i));

        frame[i] = (uint8_t)(f >> shift);
    }
    return krfid_mod_manchester((uint8_t)KRFID_MOD_EM4100_RF, frame,
                                (uint16_t)MOD_EM4100_BITS, lv, cap);
}
