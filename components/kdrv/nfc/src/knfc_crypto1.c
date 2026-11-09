/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_crypto1.c
 * @brief Crypto1: a 48-bit LFSR and a filter over five of its nibbles.
 */
#include "knfc_crypto1.h"

#include <stddef.h>

/*
 * The register is kept the way the cipher is described: two 24-bit halves,
 * odd and even, that swap on every step. A single 48-bit word would need the
 * taps interleaved and buys nothing.
 *
 * The feedback polynomial, split over the two halves:
 *
 *   x^48 + x^43 + x^39 + x^38 + x^36 + x^34 + x^33 + x^31 + x^29 + x^24
 *        + x^23 + x^21 + x^19 + x^13 + x^9 + x^7 + x^6 + x^5 + 1
 */
#define POLY_ODD   0x29CE5CUL
#define POLY_EVEN  0x870804UL
#define HALF_MASK  0xFFFFFFUL

/*
 * The non-linear filter. Five nibbles of the odd half go through a 4-to-1
 * function each and the five results index a 32-entry one.
 *
 * There are only two distinct 4-to-1 functions, applied in the pattern
 * a-b-a-a-b, which is what the published description calls fa and fb; the
 * implementations that circulate hide that behind five pre-shifted
 * constants.
 */
#define FILTER_FA  0xF22CUL
#define FILTER_FB  0xD938UL
#define FILTER_FC  0xEC57E80AUL

/**
 * @brief One of the two 4-to-1 truth tables, read at @p x.
 *
 * @param table FILTER_FA or FILTER_FB.
 * @param x     The four-bit index.
 * @return The bit.
 */
static uint8_t table4(uint32_t table, uint32_t x)
{
    return (uint8_t)((table >> (x & 0xFUL)) & 1UL);
}

/**
 * @brief The filter's output for the odd half as it stands.
 *
 * @param odd The odd half of the register.
 * @return The bit the cipher emits next.
 */
static uint8_t filter(uint32_t odd)
{
    uint32_t i = 0UL;

    i |= (uint32_t)table4(FILTER_FA, odd)        << 4;
    i |= (uint32_t)table4(FILTER_FB, odd >>  4)  << 3;
    i |= (uint32_t)table4(FILTER_FA, odd >>  8)  << 2;
    i |= (uint32_t)table4(FILTER_FA, odd >> 12)  << 1;
    i |= (uint32_t)table4(FILTER_FB, odd >> 16);
    return (uint8_t)((FILTER_FC >> i) & 1UL);
}

/**
 * @brief Parity of @p x: 1 when an odd number of bits is set.
 *
 * @param x The value.
 * @return 0 or 1.
 */
static uint8_t parity32(uint32_t x)
{
    x ^= x >> 16;
    x ^= x >> 8;
    x ^= x >> 4;
    x ^= x >> 2;
    x ^= x >> 1;
    return (uint8_t)(x & 1UL);
}

void knfc_crypto1_init(knfc_crypto1_t *c, uint64_t key)
{
    if ( NULL == c )
    {
        return;
    }
    c->odd  = 0UL;
    c->even = 0UL;

    /* The key's bits are taken most significant pair first, and reversed
     * within each byte - the order a Mifare Classic keeps them in, which is
     * why the `^ 7`. */
    for ( int i = 47; 0 < i; i -= 2 )
    {
        c->odd  = (c->odd  << 1) | (uint32_t)((key >> ((i - 1) ^ 7)) & 1ULL);
        c->even = (c->even << 1) | (uint32_t)((key >> (i ^ 7)) & 1ULL);
    }
    c->odd  &= HALF_MASK;
    c->even &= HALF_MASK;
}

uint8_t knfc_crypto1_peek(const knfc_crypto1_t *c)
{
    if ( NULL == c )
    {
        return 0U;
    }
    return filter(c->odd);
}

uint8_t knfc_crypto1_bit(knfc_crypto1_t *c, uint8_t in, bool is_encrypted)
{
    if ( NULL == c )
    {
        return 0U;
    }

    uint8_t out = filter(c->odd);
    uint8_t fb  = 0U;

    /* Ciphertext going in is decrypted first: both sides feed back the
     * PLAINTEXT, which is how the card recovers the reader's nonce. */
    if ( is_encrypted )
    {
        fb = out;
    }
    if ( (in & 1U) != 0U )
    {
        fb ^= 1U;
    }
    fb ^= parity32(POLY_ODD & c->odd);
    fb ^= parity32(POLY_EVEN & c->even);

    /* The new bit enters the even half, then the halves swap - which is one
     * step of the 48-bit register the two of them make up. */
    uint32_t shifted = ((c->even << 1) | (uint32_t)fb) & HALF_MASK;
    c->even = c->odd;
    c->odd  = shifted;

    return out;
}

uint8_t knfc_crypto1_byte(knfc_crypto1_t *c, uint8_t in, bool is_encrypted)
{
    uint8_t out = 0U;

    for ( unsigned i = 0U; 8U > i; ++i )
    {
        uint8_t bit = knfc_crypto1_bit(c, (uint8_t)(in >> i), is_encrypted);
        out = (uint8_t)(out | (uint8_t)(bit << i));
    }
    return out;
}

uint32_t knfc_crypto1_word(knfc_crypto1_t *c, uint32_t in, bool is_encrypted)
{
    uint32_t out = 0U;

    /* Byte order on the wire: the most significant byte goes first, and
     * within it the least significant bit. */
    for ( unsigned i = 0U; 4U > i; ++i )
    {
        uint8_t feed = (uint8_t)(in >> (24U - (8U * i)));
        uint8_t ks   = knfc_crypto1_byte(c, feed, is_encrypted);
        out |= (uint32_t)ks << (24U - (8U * i));
    }
    return out;
}

uint32_t knfc_crypto1_prng_successor(uint32_t x, uint32_t n)
{
    /*
     * The card's nonce generator is a 16-bit LFSR, x^16 + x^14 + x^13 + x^11
     * + 1, clocked over the nonce held most significant byte first. Sixteen
     * steps of it move the nonce on by one whole halfword, which is why the
     * authentication's answers are suc64 and suc96.
     */
    uint32_t s = ((x & 0xFF000000UL) >> 24) | ((x & 0x00FF0000UL) >> 8)
               | ((x & 0x0000FF00UL) << 8)  | ((x & 0x000000FFUL) << 24);

    for ( uint32_t i = 0U; i < n; ++i )
    {
        uint32_t fb = ((s >> 16) ^ (s >> 18) ^ (s >> 19) ^ (s >> 21)) & 1UL;
        s = (s >> 1) | (fb << 31);
    }

    return ((s & 0xFF000000UL) >> 24) | ((s & 0x00FF0000UL) >> 8)
         | ((s & 0x0000FF00UL) << 8)  | ((s & 0x000000FFUL) << 24);
}

uint8_t knfc_crypto1_odd_parity(uint8_t x)
{
    /* ISO14443-A sends odd parity: the bit that makes the count of ones in
     * the byte plus the bit itself odd. */
    return (uint8_t)(parity32((uint32_t)x) ^ 1U);
}
