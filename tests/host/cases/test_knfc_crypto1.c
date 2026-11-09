/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_crypto1.c
 * @brief Crypto1: the cipher, checked against frozen vectors.
 */
#include "unity.h"

#include "knfc_crypto1.h"

/** @brief One frozen exchange: a key, a feed, and what must come out. */
typedef struct {
    uint64_t key;
    uint32_t feed;    /**< The uid ^ nt an authentication starts with. */
    uint32_t word;    /**< The keystream word that feed produces.      */
    uint8_t  byte0;   /**< The two keystream bytes after it.           */
    uint8_t  byte1;
    uint8_t  peek;    /**< And the bit that would come next.           */
} vector_t;

static const vector_t VECTORS[] = {
    { 0xFFFFFFFFFFFFULL, 0xCD769724UL, 0xFFAF65F9UL, 0x58, 0xA1, 0 },
    { 0xA0A1A2A3A4A5ULL, 0x01020304UL, 0x5A151A5BUL, 0x94, 0x3C, 1 },
    { 0x000000000000ULL, 0x00000000UL, 0x00000000UL, 0x00, 0x00, 0 },
    { 0xD3F7D3F7D3F7ULL, 0xDEADBEEFUL, 0xA90AB72EUL, 0x5B, 0x1A, 1 },
};

TEST_CASE("the keystream matches its frozen vectors", "[knfc_crypto1]")
{
    for ( size_t i = 0U; (sizeof(VECTORS) / sizeof(VECTORS[0])) > i; ++i )
    {
        const vector_t *v = &VECTORS[i];
        knfc_crypto1_t      c;

        knfc_crypto1_init(&c, v->key);
        TEST_ASSERT_EQUAL_HEX32(v->word, knfc_crypto1_word(&c, v->feed, false));
        TEST_ASSERT_EQUAL_HEX8(v->byte0, knfc_crypto1_byte(&c, 0U, false));
        TEST_ASSERT_EQUAL_HEX8(v->byte1, knfc_crypto1_byte(&c, 0U, false));
        TEST_ASSERT_EQUAL_UINT8(v->peek, knfc_crypto1_peek(&c));
    }
}

TEST_CASE("peeking does not advance the register", "[knfc_crypto1]")
{
    knfc_crypto1_t c;

    knfc_crypto1_init(&c, 0xA0A1A2A3A4A5ULL);
    (void)knfc_crypto1_word(&c, 0x01020304UL, false);

    /* The parity bit of a Mifare exchange is encrypted with the bit that
     * belongs to the NEXT byte, so it is read before the register moves.
     * Reading it twice must give the same answer, and it must be the bit the
     * next step actually produces. */
    uint8_t a = knfc_crypto1_peek(&c);
    uint8_t b = knfc_crypto1_peek(&c);
    TEST_ASSERT_EQUAL_UINT8(a, b);
    TEST_ASSERT_EQUAL_UINT8(a, knfc_crypto1_bit(&c, 0U, false));
}

TEST_CASE("a byte is eight bits and a word is four bytes", "[knfc_crypto1]")
{
    knfc_crypto1_t whole;
    knfc_crypto1_t piece;

    /* Same key, same feed, taken in different sized steps: the keystream is
     * one sequence and how it is drawn must not change it. */
    knfc_crypto1_init(&whole, 0xFFFFFFFFFFFFULL);
    knfc_crypto1_init(&piece, 0xFFFFFFFFFFFFULL);

    uint32_t w = knfc_crypto1_word(&whole, 0xCD769724UL, false);

    uint32_t assembled = 0UL;
    for ( unsigned i = 0U; 4U > i; ++i )
    {
        uint8_t feed = (uint8_t)(0xCD769724UL >> (24U - (8U * i)));
        uint8_t ks   = knfc_crypto1_byte(&piece, feed, false);
        assembled |= (uint32_t)ks << (24U - (8U * i));
    }
    TEST_ASSERT_EQUAL_HEX32(w, assembled);

    /* And a byte is eight knfc_crypto1_bit() calls, least significant first. */
    knfc_crypto1_t by_byte;
    knfc_crypto1_t by_bit;
    knfc_crypto1_init(&by_byte, 0xA0A1A2A3A4A5ULL);
    knfc_crypto1_init(&by_bit, 0xA0A1A2A3A4A5ULL);

    uint8_t one = knfc_crypto1_byte(&by_byte, 0x5AU, false);
    uint8_t many = 0U;
    for ( unsigned i = 0U; 8U > i; ++i )
    {
        many = (uint8_t)(many | (uint8_t)(knfc_crypto1_bit(&by_bit, (uint8_t)(0x5AU >> i),
                                                       false) << i));
    }
    TEST_ASSERT_EQUAL_HEX8(one, many);
}

TEST_CASE("feeding ciphertext back recovers the same state as plaintext",
          "[knfc_crypto1]")
{
    /* This is the property the whole authentication rests on: the reader
     * feeds back what it sent in the clear, the card feeds back what it
     * received encrypted, and both registers end up in the same state. A
     * cipher that got is_encrypted wrong still produces a keystream, and the
     * card simply never answers. */
    knfc_crypto1_t reader;
    knfc_crypto1_t card;

    knfc_crypto1_init(&reader, 0xFFFFFFFFFFFFULL);
    knfc_crypto1_init(&card, 0xFFFFFFFFFFFFULL);
    (void)knfc_crypto1_word(&reader, 0xCD769724UL, false);
    (void)knfc_crypto1_word(&card, 0xCD769724UL, false);

    const uint32_t nr = 0x12345678UL;

    /* The reader encrypts its nonce, feeding back the plaintext. */
    uint32_t sent = 0UL;
    for ( unsigned i = 0U; 4U > i; ++i )
    {
        uint8_t plain = (uint8_t)(nr >> (24U - (8U * i)));
        uint8_t ks    = knfc_crypto1_byte(&reader, plain, false);
        sent |= (uint32_t)(uint8_t)(ks ^ plain) << (24U - (8U * i));
    }

    /* The card decrypts it, feeding back the ciphertext. */
    for ( unsigned i = 0U; 4U > i; ++i )
    {
        uint8_t cipher = (uint8_t)(sent >> (24U - (8U * i)));
        uint8_t ks     = knfc_crypto1_byte(&card, cipher, true);
        TEST_ASSERT_EQUAL_HEX8((uint8_t)(nr >> (24U - (8U * i))),
                               (uint8_t)(ks ^ cipher));
    }

    /* Same state now, so the next keystream word is the same on both. */
    TEST_ASSERT_EQUAL_HEX32(knfc_crypto1_word(&reader, 0U, false),
                            knfc_crypto1_word(&card, 0U, false));
}

TEST_CASE("the card's nonce generator matches its frozen vectors",
          "[knfc_crypto1]")
{
    TEST_ASSERT_EQUAL_HEX32(0x01020304UL,
                            knfc_crypto1_prng_successor(0x01020304UL, 0U));
    /* suc64 is what the reader answers with and suc96 what the card
     * answers back; getting either wrong is an authentication that fails
     * with no diagnostic at all. */
    TEST_ASSERT_EQUAL_HEX32(0x20F8ED56UL,
                            knfc_crypto1_prng_successor(0x01020304UL, 64U));
    TEST_ASSERT_EQUAL_HEX32(0x3C2BCDADUL,
                            knfc_crypto1_prng_successor(0x01020304UL, 96U));
}

TEST_CASE("odd parity is what ISO14443-A sends", "[knfc_crypto1]")
{
    /* Odd parity: the bit that makes the number of ones odd in total. */
    TEST_ASSERT_EQUAL_UINT8(1U, knfc_crypto1_odd_parity(0x00U));
    TEST_ASSERT_EQUAL_UINT8(0U, knfc_crypto1_odd_parity(0x01U));
    TEST_ASSERT_EQUAL_UINT8(1U, knfc_crypto1_odd_parity(0x03U));
    TEST_ASSERT_EQUAL_UINT8(0U, knfc_crypto1_odd_parity(0xFFU ^ 0x01U));
    TEST_ASSERT_EQUAL_UINT8(1U, knfc_crypto1_odd_parity(0xFFU));
}

TEST_CASE("every entry point survives a NULL", "[knfc_crypto1]")
{
    knfc_crypto1_init(NULL, 0ULL);
    TEST_ASSERT_EQUAL_UINT8(0U, knfc_crypto1_bit(NULL, 0U, false));
    TEST_ASSERT_EQUAL_UINT8(0U, knfc_crypto1_peek(NULL));
}
