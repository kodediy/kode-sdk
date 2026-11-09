/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knec.c
 * @brief NEC in symbols: a 9 ms leader, 32 bits, a stop mark.
 */
#include "knec.h"

#define LEADER_MARK_US   9000U
#define LEADER_SPACE_US  4500U
#define BIT_MARK_US       560U
#define ZERO_SPACE_US     560U
#define ONE_SPACE_US     1690U
#define TOL_DEN             4U   /* a quarter of nominal */
#define NEC_BITS           32U

/**
 * @brief Is @p got a mark of @p nominal, allowing for the AGC's excess?
 *
 * The window is a quarter of nominal either side, widened UPWARDS only by
 * KNEC_MARK_EXCESS_US: the TSOP holds its output low past the end of a burst,
 * so a received mark is always longer than the one that was sent, never
 * shorter.
 *
 * @param[in] got     The measured duration, in microseconds.
 * @param[in] nominal What the protocol says it should be, in microseconds.
 * @return true when @p got falls inside the window.
 */
static bool mark_ok(uint16_t got, uint32_t nominal)
{
    uint32_t const tol = nominal / TOL_DEN;
    uint32_t const hi  = nominal + tol + KNEC_MARK_EXCESS_US;
    uint32_t lo = 0U;

    if ( nominal > tol )
    {
        lo = nominal - tol;
    }
    if ( (uint32_t)got < lo )
    {
        return false;
    }
    if ( (uint32_t)got > hi )
    {
        return false;
    }
    return true;
}

/**
 * @brief Is @p got a space of @p nominal, allowing for the AGC's excess?
 *
 * The mirror image of mark_ok(): the same quarter of nominal either side,
 * widened DOWNWARDS only by KNEC_MARK_EXCESS_US, because every microsecond
 * the AGC adds to a mark is one it takes from the space that follows it.
 *
 * @param[in] got     The measured duration, in microseconds.
 * @param[in] nominal What the protocol says it should be, in microseconds.
 * @return true when @p got falls inside the window.
 */
static bool space_ok(uint16_t got, uint32_t nominal)
{
    uint32_t const tol  = nominal / TOL_DEN;
    uint32_t const drop = tol + KNEC_MARK_EXCESS_US;
    uint32_t const hi   = nominal + tol;
    uint32_t lo = 0U;

    if ( nominal > drop )
    {
        lo = nominal - drop;
    }
    if ( (uint32_t)got < lo )
    {
        return false;
    }
    if ( (uint32_t)got > hi )
    {
        return false;
    }
    return true;
}

kode_status_t knec_encode(uint16_t addr, uint16_t cmd,
                          kdrv_ir_symbol_t *out, size_t cap, size_t *n)
{
    uint32_t word;
    size_t   bit;

    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    if ( NULL == n )
    {
        return KODE_DENIED;
    }
    if ( KNEC_SYMBOLS > cap )
    {
        return KODE_DENIED;
    }

    out[0].mark_us  = LEADER_MARK_US;
    out[0].space_us = LEADER_SPACE_US;

    /* addr then cmd, sixteen bits each, LSB first - so bit 0 of addr is
     * symbol 1 and bit 15 of cmd is symbol 32. */
    word = ((uint32_t)cmd << 16) | (uint32_t)addr;
    for ( bit = 0U; NEC_BITS > bit; ++bit )
    {
        kdrv_ir_symbol_t *s = &out[1U + bit];

        s->mark_us = BIT_MARK_US;
        if ( ((word >> bit) & 1U) != 0U )
        {
            s->space_us = ONE_SPACE_US;
        }
        else
        {
            s->space_us = ZERO_SPACE_US;
        }
    }

    out[KNEC_SYMBOLS - 1U].mark_us  = BIT_MARK_US;
    out[KNEC_SYMBOLS - 1U].space_us = 0U;

    *n = KNEC_SYMBOLS;
    return KODE_OK;
}

bool knec_decode(const kdrv_ir_symbol_t *sym, size_t n,
                 uint16_t *addr, uint16_t *cmd)
{
    uint32_t word = 0U;
    size_t   bit;

    if ( NULL == sym )
    {
        return false;
    }
    if ( NULL == addr )
    {
        return false;
    }
    if ( NULL == cmd )
    {
        return false;
    }
    if ( KNEC_SYMBOLS > n )
    {
        return false;
    }

    if ( !mark_ok(sym[0].mark_us, LEADER_MARK_US) )
    {
        return false;
    }
    if ( !space_ok(sym[0].space_us, LEADER_SPACE_US) )
    {
        return false;
    }

    for ( bit = 0U; NEC_BITS > bit; ++bit )
    {
        kdrv_ir_symbol_t const *s = &sym[1U + bit];

        if ( !mark_ok(s->mark_us, BIT_MARK_US) )
        {
            return false;
        }
        if ( space_ok(s->space_us, ONE_SPACE_US) )
        {
            word |= ((uint32_t)1U << bit);
        }
        else if ( space_ok(s->space_us, ZERO_SPACE_US) )
        {
            /* bit stays 0 */
        }
        else
        {
            return false;
        }
    }

    if ( !mark_ok(sym[KNEC_SYMBOLS - 1U].mark_us, BIT_MARK_US) )
    {
        return false;
    }

    *addr = (uint16_t)(word & 0xFFFFU);
    *cmd  = (uint16_t)((word >> 16) & 0xFFFFU);
    return true;
}
