/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_mfc.c
 * @brief Mifare Classic over the raw NFC-A exchange.
 */
#include "knfc_mfc.h"

#include <string.h>

#include "knfc_crypto1.h"
#include "kdrv_st25r3916.h"
#include "knfc_mfc_dict.h"
#include "khal_system.h"
#include "kode_log.h"

#define TAG "kdrv.mfc"

/* ---- The wire ----------------------------------------------------------- */

#define CMD_AUTH_A   0x60U
#define CMD_AUTH_B   0x61U
#define CMD_READ     0x30U
#define CMD_WRITE    0xA0U
#define ACK          0x0AU   /**< Four bits, and the only good answer.     */

#define FWT_MS         12U   /**< A real frame is ~1 ms; a wrong key answers\n                              *   nothing, so this bounds a failure, not a\n                              *   success. */
#define NT_BITS        36U   /**< Four bytes and their four parity bits.   */
#define BLOCK_BITS    162U   /**< Sixteen data, two CRC, eighteen parity.  */
#define ACK_BITS        4U

/** @brief The longest frame this file sends: eighteen bytes of nine bits. */
#define TX_MAX         32U
#define RX_MAX         32U

/* ---- What kodeOS knows how to open -------------------------------------- */

/*
 * How many of the dictionary's keys the quick path tries. The common keys
 * lead the table - factory default, blank, the NFC Forum's MAD and data keys
 * - so a blank or NDEF-formatted tag opens in the first few, and nfc_read /
 * nfc_write stay responsive instead of sweeping two thousand keys inside an
 * Active Object's step. The full sweep is a deliberate, paced operation
 * (nfc_keys), not something a casual read pays for.
 */
#define FAST_KEYS  20U


/* ---- State -------------------------------------------------------------- */

static knfc_crypto1_t s_cipher;
static bool       s_authed;

/* ---- Bit packing --------------------------------------------------------- */

/**
 * @brief Write one bit into a packed buffer.
 *
 * The wire order: within a byte the least significant bit goes first, which
 * is what the parity bit after it interrupts.
 *
 * @param buf The buffer.
 * @param pos The bit position.
 * @param bit The value, 0 or 1.
 */
static void bit_put(uint8_t *buf, uint16_t pos, uint8_t bit)
{
    uint16_t byte = (uint16_t)(pos / 8U);
    uint8_t  mask = (uint8_t)(1U << (pos % 8U));

    if ( 0U != bit )
    {
        buf[byte] = (uint8_t)(buf[byte] | mask);
    }
    else
    {
        buf[byte] = (uint8_t)(buf[byte] & (uint8_t)~mask);
    }
}

/**
 * @brief Read one bit out of a packed buffer.
 *
 * @param buf The buffer.
 * @param pos The bit position.
 * @return 0 or 1.
 */
static uint8_t bit_get(const uint8_t *buf, uint16_t pos)
{
    return (uint8_t)((buf[pos / 8U] >> (pos % 8U)) & 1U);
}

/**
 * @brief Pack @p n bytes with their parity bits into @p out.
 *
 * @param out  Out: nine bits per byte.
 * @param data The bytes, already encrypted if they are to be.
 * @param par  Their parity bits, already encrypted if they are to be.
 * @param n    How many bytes.
 * @return How many bits were written.
 */
static uint16_t pack(uint8_t *out, const uint8_t *data, const uint8_t *par,
                     uint16_t n)
{
    uint16_t pos = 0U;

    (void)memset(out, 0, TX_MAX);
    for ( uint16_t i = 0U; i < n; ++i )
    {
        for ( uint8_t b = 0U; 8U > b; ++b )
        {
            bit_put(out, pos, (uint8_t)((data[i] >> b) & 1U));
            ++pos;
        }
        bit_put(out, pos, par[i]);
        ++pos;
    }
    return pos;
}

/**
 * @brief Unpack @p n nine-bit groups back into bytes, decrypting as it goes.
 *
 * @param in  The received bits.
 * @param out Out: @p n bytes.
 * @param n   How many bytes to take.
 */
static void unpack_decrypt(const uint8_t *in, uint8_t *out, uint16_t n)
{
    uint16_t pos = 0U;

    for ( uint16_t i = 0U; i < n; ++i )
    {
        uint8_t cipher = 0U;
        for ( uint8_t b = 0U; 8U > b; ++b )
        {
            cipher = (uint8_t)(cipher | (uint8_t)(bit_get(in, pos) << b));
            ++pos;
        }
        ++pos;   /* The parity bit; encrypted, and nothing here checks it. */
        /* The reader decrypts by clocking the cipher with ZERO and XORing
         * the keystream in - NOT by feeding the ciphertext back. Feeding it
         * back is the CARD's operation, the one that recovers the reader's
         * nonce during authentication; a reader that does it here desyncs
         * the cipher after the first byte, so `at` verifies wrong and every
         * block reads as rubbish. The card generated this keystream clocking
         * with zero, and the reader must match it. */
        uint8_t ks = knfc_crypto1_byte(&s_cipher, 0U, false);
        out[i] = (uint8_t)(ks ^ cipher);
    }
}

/* ---- CRC_A --------------------------------------------------------------- */

/**
 * @brief The ISO14443-A checksum over @p n bytes.
 *
 * Nothing below computes it any more: this file asked the RF layer to leave
 * the CRC alone so that it could be encrypted with everything else.
 *
 * @param data The bytes.
 * @param n    How many.
 * @param out  Out: two bytes, low first, as they go on the wire.
 */
static void crc_a(const uint8_t *data, uint16_t n, uint8_t *out)
{
    uint16_t crc = 0x6363U;

    for ( uint16_t i = 0U; i < n; ++i )
    {
        uint8_t b = (uint8_t)(data[i] ^ (uint8_t)crc);
        b   = (uint8_t)(b ^ (uint8_t)(b << 4));
        crc = (uint16_t)((crc >> 8) ^ ((uint16_t)b << 8) ^ ((uint16_t)b << 3)
                         ^ ((uint16_t)b >> 4));
    }
    out[0] = (uint8_t)(crc & 0xFFU);
    out[1] = (uint8_t)((crc >> 8) & 0xFFU);
}

/* ---- Framing ------------------------------------------------------------- */

/**
 * @brief Send @p n bytes in the clear, with real parity and a real CRC.
 *
 * Used once, for the command that opens an authentication: at that point
 * nothing is encrypted yet.
 *
 * @param data    The command bytes, CRC excluded.
 * @param n       How many.
 * @param rx      Out: the raw answer, still packed.
 * @param rx_cap  How many bytes @p rx holds.
 * @param rx_bits Out: how many bits arrived.
 * @return What the transceive returned.
 */
static kode_status_t send_plain(const uint8_t *data, uint16_t n, uint8_t *rx,
                                uint16_t rx_cap, uint16_t *rx_bits)
{
    uint8_t body[8];
    uint8_t par[8];
    uint8_t tx[TX_MAX];

    if ( (n + 2U) > sizeof(body) )
    {
        return KODE_DENIED;
    }
    (void)memcpy(body, data, n);
    crc_a(data, n, &body[n]);

    uint16_t total = (uint16_t)(n + 2U);
    for ( uint16_t i = 0U; i < total; ++i )
    {
        par[i] = knfc_crypto1_odd_parity(body[i]);
    }

    uint16_t bits = pack(tx, body, par, total);
    return kdrv_st25r3916_transceive_bits(tx, bits, rx, rx_cap, rx_bits,
                                          FWT_MS);
}

/**
 * @brief Send @p n bytes encrypted, with the parity encrypted too.
 *
 * @param data    The command bytes, CRC excluded; it is appended here.
 * @param n       How many.
 * @param rx      Out: the raw answer, still packed.
 * @param rx_cap  How many bytes @p rx holds.
 * @param rx_bits Out: how many bits arrived.
 * @return What the transceive returned.
 */
static kode_status_t send_encrypted(const uint8_t *data, uint16_t n,
                                    uint8_t *rx, uint16_t rx_cap,
                                    uint16_t *rx_bits)
{
    uint8_t body[20];
    uint8_t enc[20];
    uint8_t par[20];
    uint8_t tx[TX_MAX];

    if ( (n + 2U) > sizeof(body) )
    {
        return KODE_DENIED;
    }
    (void)memcpy(body, data, n);
    crc_a(data, n, &body[n]);

    uint16_t total = (uint16_t)(n + 2U);
    for ( uint16_t i = 0U; i < total; ++i )
    {
        uint8_t ks = knfc_crypto1_byte(&s_cipher, 0U, false);
        enc[i] = (uint8_t)(ks ^ body[i]);
        /* The parity bit is encrypted with the keystream bit that belongs to
         * the NEXT byte, which is why it has to be peeked before the
         * register moves on. */
        par[i] = (uint8_t)(knfc_crypto1_peek(&s_cipher)
                           ^ knfc_crypto1_odd_parity(body[i]));
    }

    uint16_t bits = pack(tx, enc, par, total);
    return kdrv_st25r3916_transceive_bits(tx, bits, rx, rx_cap, rx_bits,
                                          FWT_MS);
}

/* ---- Authentication ------------------------------------------------------ */

void knfc_mfc_reset(void)
{
    s_authed = false;
}

uint8_t knfc_mfc_sectors(uint8_t sak)
{
    /* SAK narrows the size: 0x09 Mini (5), 0x18/0x98 4K (40), everything
     * else that is Classic is a 1K (16). */
    if ( 0x09U == sak )
    {
        return 5U;
    }
    if ( (0x18U == sak) || (0x98U == sak) )
    {
        return 40U;
    }
    return 16U;
}

uint8_t knfc_mfc_sector_first(uint8_t sector)
{
    if ( 32U > sector )
    {
        return (uint8_t)(sector * 4U);
    }
    return (uint8_t)(128U + ((sector - 32U) * 16U));
}

uint8_t knfc_mfc_sector_blocks(uint8_t sector)
{
    if ( 32U > sector )
    {
        return 4U;
    }
    return 16U;
}

bool knfc_mfc_is_trailer(uint8_t block)
{
    /* Four-block sectors up to block 127, sixteen-block sectors above it. */
    if ( 128U > block )
    {
        return ((block + 1U) % 4U) == 0U;
    }
    return ((block + 1U) % 16U) == 0U;
}

kode_status_t knfc_mfc_auth(uint8_t block, knfc_mfc_key_t type,
                            const uint8_t *key, const uint8_t *uid)
{
    if ( (NULL == key) || (NULL == uid) )
    {
        return KODE_DENIED;
    }

    s_authed = false;

    /* A plaintext AUTH is only valid on a freshly selected card. After any
     * previous authentication - a different sector, or a wrong key that
     * halted the card - the card must be re-selected first, so every auth
     * starts from the same clean state. */
    if ( kdrv_st25r3916_reselect() != KODE_OK )
    {
        return KODE_TIMEOUT;
    }

    uint8_t cmd[2];
    cmd[0] = CMD_AUTH_A;
    if ( KNFC_MFC_KEY_B == type )
    {
        cmd[0] = CMD_AUTH_B;
    }
    cmd[1] = block;

    uint8_t  rx[RX_MAX];
    uint16_t rx_bits = 0U;
    kode_status_t rc = send_plain(cmd, sizeof(cmd), rx, sizeof(rx), &rx_bits);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( NT_BITS > rx_bits )
    {
        return KODE_TIMEOUT;
    }

    /* nt comes back in the clear, nine bits per byte like everything else. */
    uint8_t nt[4];
    uint16_t pos = 0U;
    for ( uint8_t i = 0U; 4U > i; ++i )
    {
        uint8_t v = 0U;
        for ( uint8_t b = 0U; 8U > b; ++b )
        {
            v = (uint8_t)(v | (uint8_t)(bit_get(rx, pos) << b));
            ++pos;
        }
        ++pos;
        nt[i] = v;
    }

    uint32_t nt32 = ((uint32_t)nt[0] << 24) | ((uint32_t)nt[1] << 16)
                  | ((uint32_t)nt[2] << 8) | (uint32_t)nt[3];
    uint32_t uid32 = ((uint32_t)uid[0] << 24) | ((uint32_t)uid[1] << 16)
                   | ((uint32_t)uid[2] << 8) | (uint32_t)uid[3];
    uint64_t key48 = 0ULL;
    for ( uint8_t i = 0U; KNFC_MFC_KEY_LEN > i; ++i )
    {
        key48 = (key48 << 8) | (uint64_t)key[i];
    }

    knfc_crypto1_init(&s_cipher, key48);
    (void)knfc_crypto1_word(&s_cipher, uid32 ^ nt32, false);

    /* The reader's own nonce. It only has to be unpredictable to the card,
     * and the card cannot check it at all - what it checks is ar. */
    uint32_t nr32 = khal_random();
    uint32_t ar32 = knfc_crypto1_prng_successor(nt32, 64U);

    uint8_t plain[8];
    for ( uint8_t i = 0U; 4U > i; ++i )
    {
        plain[i]      = (uint8_t)(nr32 >> (24U - (8U * i)));
        plain[4U + i] = (uint8_t)(ar32 >> (24U - (8U * i)));
    }

    uint8_t enc[8];
    uint8_t par[8];

    /* The reader's nonce is fed back as plaintext: the card recovers it and
     * puts it into its own register, which is what keeps the two in step. */
    for ( uint8_t i = 0U; 4U > i; ++i )
    {
        uint8_t ks = knfc_crypto1_byte(&s_cipher, plain[i], false);
        enc[i] = (uint8_t)(ks ^ plain[i]);
        par[i] = (uint8_t)(knfc_crypto1_peek(&s_cipher)
                           ^ knfc_crypto1_odd_parity(plain[i]));
    }
    /* The answer is not: it is derived from nt, which both sides already
     * have, so nothing new goes into the register. */
    for ( uint8_t i = 4U; 8U > i; ++i )
    {
        uint8_t ks = knfc_crypto1_byte(&s_cipher, 0U, false);
        enc[i] = (uint8_t)(ks ^ plain[i]);
        par[i] = (uint8_t)(knfc_crypto1_peek(&s_cipher)
                           ^ knfc_crypto1_odd_parity(plain[i]));
    }

    uint8_t tx[TX_MAX];
    uint16_t bits = pack(tx, enc, par, 8U);

    rx_bits = 0U;
    rc = kdrv_st25r3916_transceive_bits(tx, bits, rx, sizeof(rx), &rx_bits,
                                        FWT_MS);
    if ( KODE_OK != rc )
    {
        /* Silence is how a Mifare Classic says no. The next auth reselects
         * before it starts, so nothing is needed here. */
        return KODE_TIMEOUT;
    }
    if ( NT_BITS > rx_bits )
    {
        return KODE_TIMEOUT;
    }

    uint8_t at[4];
    unpack_decrypt(rx, at, 4U);

    uint32_t at32 = ((uint32_t)at[0] << 24) | ((uint32_t)at[1] << 16)
                  | ((uint32_t)at[2] << 8) | (uint32_t)at[3];
    if ( knfc_crypto1_prng_successor(nt32, 96U) != at32 )
    {
        return KODE_ERROR;
    }

    s_authed = true;
    return KODE_OK;
}

kode_status_t knfc_mfc_auth_any(uint8_t block, const uint8_t *uid,
                                knfc_mfc_key_t *type, uint8_t *key)
{
    if ( NULL == uid )
    {
        return KODE_DENIED;
    }

    size_t n = FAST_KEYS;
    if ( KNFC_MFC_DICT_COUNT < n )
    {
        n = KNFC_MFC_DICT_COUNT;
    }
    for ( size_t k = 0U; k < n; ++k )
    {
        for ( uint8_t t = 0U; 2U > t; ++t )
        {
            knfc_mfc_key_t kt = KNFC_MFC_KEY_A;
            if ( 1U == t )
            {
                kt = KNFC_MFC_KEY_B;
            }
            if ( knfc_mfc_auth(block, kt, KNFC_MFC_DICT[k], uid) == KODE_OK )
            {
                if ( NULL != type )
                {
                    *type = kt;
                }
                if ( NULL != key )
                {
                    (void)memcpy(key, KNFC_MFC_DICT[k], KNFC_MFC_KEY_LEN);
                }
                return KODE_OK;
            }
        }
    }
    return KODE_DENIED;
}

/* ---- Reading and writing -------------------------------------------------- */

kode_status_t knfc_mfc_sweep(uint8_t block, const uint8_t *uid, size_t from,
                             size_t batch, size_t *found_idx,
                             knfc_mfc_key_t *found_type, size_t *tried)
{
    if ( NULL == uid )
    {
        return KODE_DENIED;
    }

    size_t done = 0U;
    size_t k    = from;
    for ( ; (KNFC_MFC_DICT_COUNT > k) && (done < batch); ++k )
    {
        for ( uint8_t t = 0U; 2U > t; ++t )
        {
            knfc_mfc_key_t kt = KNFC_MFC_KEY_A;
            if ( 1U == t )
            {
                kt = KNFC_MFC_KEY_B;
            }
            if ( knfc_mfc_auth(block, kt, KNFC_MFC_DICT[k], uid) == KODE_OK )
            {
                if ( NULL != found_idx )
                {
                    *found_idx = k;
                }
                if ( NULL != found_type )
                {
                    *found_type = kt;
                }
                if ( NULL != tried )
                {
                    *tried = (k - from) + 1U;
                }
                return KODE_OK;
            }
        }
        ++done;
    }
    if ( NULL != tried )
    {
        *tried = k - from;
    }
    return KODE_DENIED;
}

size_t knfc_mfc_dict_size(void)
{
    return KNFC_MFC_DICT_COUNT;
}

kode_status_t knfc_mfc_read(uint8_t block, uint8_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }
    if ( !s_authed )
    {
        return KODE_DENIED;
    }

    uint8_t cmd[2];
    cmd[0] = CMD_READ;
    cmd[1] = block;

    uint8_t  rx[RX_MAX];
    uint16_t rx_bits = 0U;
    kode_status_t rc = send_encrypted(cmd, sizeof(cmd), rx, sizeof(rx),
                                      &rx_bits);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( BLOCK_BITS > rx_bits )
    {
        return KODE_TIMEOUT;
    }

    uint8_t body[18];
    unpack_decrypt(rx, body, 18U);

    uint8_t crc[2];
    crc_a(body, KNFC_MFC_BLOCK_LEN, crc);
    if ( (crc[0] != body[16]) || (crc[1] != body[17]) )
    {
        KLOG_W(TAG, "block %u came back with a bad checksum", (unsigned)block);
        return KODE_ERROR;
    }

    (void)memcpy(out, body, KNFC_MFC_BLOCK_LEN);
    return KODE_OK;
}

/**
 * @brief Was the four-bit answer an ACK?
 *
 * @param rx      The raw answer.
 * @param rx_bits How many bits arrived.
 * @return true when the card acknowledged.
 */
static bool took_it(const uint8_t *rx, uint16_t rx_bits)
{
    if ( ACK_BITS > rx_bits )
    {
        return false;
    }

    /* Four bits, encrypted with four keystream bits. */
    uint8_t v = 0U;
    for ( uint8_t b = 0U; 4U > b; ++b )
    {
        uint8_t ks = knfc_crypto1_bit(&s_cipher, 0U, false);
        v = (uint8_t)(v | (uint8_t)((bit_get(rx, b) ^ ks) << b));
    }
    return ACK == v;
}

kode_status_t knfc_mfc_write(uint8_t block, const uint8_t *data)
{
    if ( NULL == data )
    {
        return KODE_DENIED;
    }
    if ( !s_authed )
    {
        return KODE_DENIED;
    }
    if ( knfc_mfc_is_trailer(block) || (0U == block) )
    {
        /* The trailer holds the keys and block 0 is the manufacturer's. A
         * wrong write to either is not recoverable, so neither is offered. */
        KLOG_W(TAG, "block %u is not writable by kodeOS", (unsigned)block);
        return KODE_DENIED;
    }

    uint8_t cmd[2];
    cmd[0] = CMD_WRITE;
    cmd[1] = block;

    uint8_t  rx[RX_MAX];
    uint16_t rx_bits = 0U;
    kode_status_t rc = send_encrypted(cmd, sizeof(cmd), rx, sizeof(rx),
                                      &rx_bits);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( !took_it(rx, rx_bits) )
    {
        KLOG_W(TAG, "the card refused a write to block %u", (unsigned)block);
        return KODE_ERROR;
    }

    rx_bits = 0U;
    rc = send_encrypted(data, KNFC_MFC_BLOCK_LEN, rx, sizeof(rx), &rx_bits);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( !took_it(rx, rx_bits) )
    {
        KLOG_W(TAG, "the card would not take the data for block %u",
               (unsigned)block);
        return KODE_ERROR;
    }
    return KODE_OK;
}
