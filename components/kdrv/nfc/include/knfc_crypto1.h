/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_crypto1.h
 * @brief Crypto1, the stream cipher a Mifare Classic authenticates with.
 *
 * Written from the published description (Garcia et al., "Dismantling MIFARE
 * Classic", ESORICS 2008), not from Crapto1 or any code derived from it.
 */
#ifndef KNFC_CRYPTO1_H_
#define KNFC_CRYPTO1_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The cipher's whole state: a 48-bit register kept as two halves.
 *
 * Odd and even, twenty-four bits each, swapping on every step. That is the
 * shape the cipher is described in - the filter reads only the odd half -
 * and it keeps the feedback taps as two flat masks instead of interleaved
 * ones.
 */
typedef struct {
    uint32_t odd;    /**< Only the low 24 bits are used. */
    uint32_t even;   /**< Only the low 24 bits are used. */
} knfc_crypto1_t;

/**
 * @brief Load @p key into the register and start.
 *
 * @param c   The cipher.
 * @param key The 48-bit sector key, most significant byte first as it is
 *            stored in the tag's trailer.
 */
void knfc_crypto1_init(knfc_crypto1_t *c, uint64_t key);

/**
 * @brief One keystream bit, feeding @p in back into the register.
 *
 * @param c            The cipher.
 * @param in           The bit mixed into the feedback: plaintext while the
 *                     reader's nonce is being sent, zero afterwards.
 * @param is_encrypted true when @p in is ciphertext, in which case the
 *                     keystream bit is XORed into the feedback as well -
 *                     which is how the card recovers what the reader sent.
 * @return The keystream bit, 0 or 1.
 */
uint8_t knfc_crypto1_bit(knfc_crypto1_t *c, uint8_t in, bool is_encrypted);

/**
 * @brief Eight keystream bits, least significant first.
 *
 * @param c            The cipher.
 * @param in           The byte mixed into the feedback, bit by bit.
 * @param is_encrypted As knfc_crypto1_bit().
 * @return The keystream byte.
 */
uint8_t knfc_crypto1_byte(knfc_crypto1_t *c, uint8_t in, bool is_encrypted);

/**
 * @brief Thirty-two keystream bits.
 *
 * @param c            The cipher.
 * @param in           The word mixed into the feedback, most significant
 *                     byte first - the order the wire uses.
 * @param is_encrypted As knfc_crypto1_bit().
 * @return The keystream word, in the same order.
 */
uint32_t knfc_crypto1_word(knfc_crypto1_t *c, uint32_t in, bool is_encrypted);

/**
 * @brief The next keystream bit without advancing the register.
 *
 * The parity bit of a Mifare Classic exchange is encrypted with the keystream
 * bit that belongs to the NEXT byte, so it has to be read before the register
 * moves on. This is the only reason the filter is exposed at all.
 *
 * @param c The cipher.
 * @return The bit the next knfc_crypto1_bit() would produce.
 */
uint8_t knfc_crypto1_peek(const knfc_crypto1_t *c);

/**
 * @brief The card's nonce generator, advanced @p n steps.
 *
 * A 16-bit LFSR, which is what makes the card's nonces predictable and the
 * reader's answer computable: the reader proves it holds the key by sending
 * suc64(nt), and the card answers with suc96(nt).
 *
 * @param x The starting nonce.
 * @param n How many steps.
 * @return The nonce @p n steps on.
 */
uint32_t knfc_crypto1_prng_successor(uint32_t x, uint32_t n);

/**
 * @brief Odd parity of @p x, the bit ISO14443-A sends after every byte.
 *
 * @param x The byte.
 * @return 1 when @p x has an even number of set bits, 0 otherwise.
 */
uint8_t knfc_crypto1_odd_parity(uint8_t x);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_CRYPTO1_H_ */
