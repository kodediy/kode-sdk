/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_tag.c
 * @brief Type-2 and Type-4 tags: where the message lives on each.
 */
#include "knfc_tag.h"

#include <string.h>

#include "kdrv_st25r3916.h"
#include "knfc_mfc.h"
#include "knfc_ndef.h"
#include "kode_log.h"

#define TAG "kdrv.tag"

/* ---- what the SAK says ------------------------------------------------- */

#define SAK_ISO_DEP     0x20U   /**< Bit 5: the tag speaks ISO-DEP.      */
#define SAK_NFC_DEP     0x40U   /**< Bit 6: peer-to-peer, not carried.   */
#define SAK_MIFARE      0x08U   /**< Bit 3, with bit 5 clear: Mifare
                                 *   Classic. 0x08 is a 1K and 0x18 a 4K. */

/* ---- Type-2 ------------------------------------------------------------ */

#define T2T_CC_BLOCK      3U      /**< The capability container.          */
#define T2T_DATA_BLOCK    4U      /**< Where the TLV area starts.         */
#define T2T_BLOCK_LEN     4U
#define T2T_READ_LEN     16U      /**< A READ answers with four blocks.   */
#define T2T_CC_MAGIC   0xE1U      /**< CC[0] on a formatted tag.          */
#define T2T_CC_SIZE_UNIT  8U      /**< CC[2] counts eight-byte units.     */
#define T2T_RW_MASK    0x0FU      /**< CC[3] low nibble: write access.    */
#define T2T_RW_OPEN    0x00U      /**< ...and zero is "anyone may".       */

/** @brief The largest message this reads or writes, either tag type. */
#define NDEF_MAX        480U

/* ---- Type-4 ------------------------------------------------------------ */

#define APDU_MAX        256U
#define SW_LEN            2U      /**< Every response ends in a status word. */
#define SW1_OK         0x90U
#define SW2_OK         0x00U

/** @brief The NDEF application, selected by name. */
static const uint8_t NDEF_AID[] = { 0xD2U, 0x76U, 0x00U, 0x00U,
                                    0x85U, 0x01U, 0x01U };
/** @brief The capability container file, selected by identifier. */
static const uint8_t CC_FILE_ID[] = { 0xE1U, 0x03U };

/** @brief How much of the CC file is worth reading: the file control TLV. */
#define CC_READ_LEN      15U
/** @brief Where the NDEF File Control TLV starts inside the CC file. */
#define CC_TLV_OFF        7U
#define CC_TLV_TYPE    0x04U

/* ==========================================================================
 * Identity
 * ========================================================================== */

const char *knfc_tag_model(uint16_t atqa, uint8_t sak)
{
    /* The common NFC-A families, keyed on the bytes anticollision returns.
     * SAK is the stronger signal; ATQA separates a 1K from a 4K where SAK
     * alone is ambiguous. Anything not matched is honestly "NFC-A". */
    if ( (sak & SAK_ISO_DEP) != 0U )
    {
        return "Type-4 (ISO-DEP / DESFire)";
    }
    if ( 0x00U == sak )
    {
        return "Ultralight / NTAG (Type-2)";
    }
    if ( 0x09U == sak )
    {
        return "Mifare Mini";
    }
    if ( (0x08U == sak) || (0x88U == sak) )
    {
        return "Mifare Classic 1K";
    }
    if ( (0x18U == sak) || (0x98U == sak) )
    {
        return "Mifare Classic 4K";
    }
    if ( 0x01U == sak )
    {
        return "Mifare Classic 1K (TNP3xxx)";
    }
    if ( (sak & SAK_MIFARE) != 0U )
    {
        return "Mifare Classic";
    }
    return "NFC-A";
}

kode_status_t knfc_tag_identify(knfc_tag_t *out)
{
    if ( NULL == out )
    {
        return KODE_DENIED;
    }

    uint8_t  uid[KDRV_ST25R3916_UID_MAX];
    uint8_t  uid_len = 0U;
    uint16_t atqa    = 0U;
    uint8_t  sak     = 0U;

    kode_status_t rc = kdrv_st25r3916_active_tag(uid, &uid_len, &atqa, &sak);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    (void)memset(out, 0, sizeof(*out));
    if ( KNFC_UID_MAX < uid_len )
    {
        uid_len = KNFC_UID_MAX;
    }
    (void)memcpy(out->uid, uid, uid_len);
    out->uid_len = uid_len;
    out->atqa    = atqa;
    out->sak     = sak;

    out->type = KNFC_TAG_T2T;
    if ( (sak & SAK_MIFARE) != 0U )
    {
        /* Before the ISO-DEP test below, which overrides it: a card that is
         * both - a Mifare Plus in SL3, say - is reachable as a Type-4 and
         * that is the half kodeOS can use. */
        out->type = KNFC_TAG_MIFARE;
    }
    if ( (sak & SAK_NFC_DEP) != 0U )
    {
        out->type = KNFC_TAG_UNKNOWN;
    }
    if ( (sak & SAK_ISO_DEP) != 0U )
    {
        out->type = KNFC_TAG_T4T;
    }
    return KODE_OK;
}

/* ==========================================================================
 * Type-2
 * ========================================================================== */

/**
 * @brief Read the capability container and say what it allows.
 *
 * @param size     Out: how many bytes of data area the tag declares.
 * @param writable Out: whether write access is open.
 * @return KODE_OK, KODE_ERROR when the tag is not NDEF-formatted, or
 *         whatever the read returned.
 */
static kode_status_t t2t_cc(size_t *size, bool *writable)
{
    uint8_t  buf[T2T_READ_LEN];
    uint16_t got = 0U;

    /* One READ from block 0 answers with blocks 0..3, so the CC arrives in
     * the same transaction as the UID rather than costing a second one. */
    kode_status_t rc = kdrv_st25r3916_t2t_read(0U, buf, sizeof(buf), &got);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( T2T_READ_LEN > got )
    {
        return KNFC_NDEF_NO_MESSAGE;
    }

    const uint8_t *cc = &buf[T2T_CC_BLOCK * T2T_BLOCK_LEN];
    if ( T2T_CC_MAGIC != cc[0] )
    {
        KLOG_W(TAG, "not an NDEF tag: CC starts 0x%02X", (unsigned)cc[0]);
        return KNFC_NDEF_NO_MESSAGE;
    }

    *size     = (size_t)cc[2] * T2T_CC_SIZE_UNIT;
    *writable = (cc[3] & T2T_RW_MASK) == T2T_RW_OPEN;
    return KODE_OK;
}

/**
 * @brief Read @p want bytes of the data area into @p buf.
 *
 * @param buf  Out: at least @p want bytes.
 * @param want How many to read; rounded up to whole reads.
 * @param got  Out: how many arrived.
 * @return KODE_OK, or whatever the transport returned.
 */
static kode_status_t t2t_read_area(uint8_t *buf, size_t want, size_t *got)
{
    size_t done = 0U;
    while ( done < want )
    {
        uint8_t  chunk[T2T_READ_LEN];
        uint16_t n     = 0U;
        uint8_t  block = (uint8_t)(T2T_DATA_BLOCK
                                   + (done / T2T_BLOCK_LEN));

        kode_status_t rc = kdrv_st25r3916_t2t_read(block, chunk, sizeof(chunk),
                                                   &n);
        if ( KODE_OK != rc )
        {
            if ( 0U < done )
            {
                break;   /* Keep what arrived; the walk may already have it. */
            }
            return rc;
        }
        if ( 0U == n )
        {
            break;
        }

        size_t take = (size_t)n;
        if ( (want - done) < take )
        {
            take = want - done;
        }
        (void)memcpy(&buf[done], chunk, take);
        done += take;
    }
    *got = done;
    return KODE_OK;
}

/**
 * @brief Write @p len bytes to the data area, four at a time.
 *
 * @param data The bytes, already TLV-wrapped.
 * @param len  How many.
 * @return KODE_OK, or whatever the transport returned.
 */
static kode_status_t t2t_write_area(const uint8_t *data, size_t len)
{
    size_t done = 0U;
    while ( done < len )
    {
        uint8_t page[T2T_BLOCK_LEN];
        size_t  take = len - done;
        if ( T2T_BLOCK_LEN < take )
        {
            take = T2T_BLOCK_LEN;
        }
        /* The tail is padded with zeros: a NULL TLV, which any reader skips. */
        (void)memset(page, 0, sizeof(page));
        (void)memcpy(page, &data[done], take);

        uint8_t block = (uint8_t)(T2T_DATA_BLOCK + (done / T2T_BLOCK_LEN));
        kode_status_t rc = kdrv_st25r3916_t2t_write(block, page);
        if ( KODE_OK != rc )
        {
            KLOG_E(TAG, "block %u would not take the write", (unsigned)block);
            return rc;
        }
        done += take;
    }
    return KODE_OK;
}

/* ==========================================================================
 * Type-4
 * ========================================================================== */

/**
 * @brief Send one APDU and check its status word.
 *
 * @param tx     The command APDU.
 * @param tx_len How long it is.
 * @param rx     Out: the response, status word stripped. May be NULL.
 * @param rx_cap How many bytes @p rx holds.
 * @param rx_len Out: how many were written to @p rx. May be NULL.
 * @return KODE_OK when the card answered 0x9000, KODE_ERROR when it answered
 *         anything else, or whatever the transport returned.
 */
static kode_status_t t4t_apdu(const uint8_t *tx, uint16_t tx_len, uint8_t *rx,
                              size_t rx_cap, size_t *rx_len)
{
    uint8_t  buf[APDU_MAX + SW_LEN];
    uint16_t got = 0U;

    kode_status_t rc = kdrv_st25r3916_apdu(tx, tx_len, buf, sizeof(buf), &got);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( SW_LEN > got )
    {
        return KNFC_NDEF_NO_MESSAGE;
    }
    if ( (SW1_OK != buf[got - 2U]) || (SW2_OK != buf[got - 1U]) )
    {
        KLOG_W(TAG, "card answered %02X%02X", (unsigned)buf[got - 2U],
               (unsigned)buf[got - 1U]);
        return KNFC_NDEF_NO_MESSAGE;
    }

    size_t body = (size_t)got - SW_LEN;
    if ( NULL != rx )
    {
        if ( body > rx_cap )
        {
            return KNFC_NDEF_TOO_LONG;
        }
        if ( 0U < body )
        {
            (void)memcpy(rx, buf, body);
        }
    }
    if ( NULL != rx_len )
    {
        *rx_len = body;
    }
    return KODE_OK;
}

/**
 * @brief SELECT by name: the NDEF application.
 *
 * @return What t4t_apdu() returned.
 */
static kode_status_t t4t_select_app(void)
{
    uint8_t apdu[6U + sizeof(NDEF_AID)];

    apdu[0] = 0x00U;   /* CLA */
    apdu[1] = 0xA4U;   /* INS: SELECT */
    apdu[2] = 0x04U;   /* P1: by name */
    apdu[3] = 0x00U;   /* P2: first or only occurrence */
    apdu[4] = (uint8_t)sizeof(NDEF_AID);
    (void)memcpy(&apdu[5], NDEF_AID, sizeof(NDEF_AID));
    apdu[5U + sizeof(NDEF_AID)] = 0x00U;   /* Le */

    return t4t_apdu(apdu, (uint16_t)sizeof(apdu), NULL, 0U, NULL);
}

/**
 * @brief SELECT by identifier: one file inside the application.
 *
 * @param id The two-byte file identifier.
 * @return What t4t_apdu() returned.
 */
static kode_status_t t4t_select_file(const uint8_t *id)
{
    uint8_t apdu[7];

    apdu[0] = 0x00U;
    apdu[1] = 0xA4U;
    apdu[2] = 0x00U;   /* P1: by identifier */
    apdu[3] = 0x0CU;   /* P2: no response data */
    apdu[4] = 0x02U;
    apdu[5] = id[0];
    apdu[6] = id[1];

    return t4t_apdu(apdu, (uint16_t)sizeof(apdu), NULL, 0U, NULL);
}

/**
 * @brief READ BINARY from the selected file.
 *
 * @param off    Where to start.
 * @param want   How many bytes; at most 255.
 * @param rx     Out: the bytes.
 * @param rx_cap How many @p rx holds.
 * @param rx_len Out: how many arrived.
 * @return What t4t_apdu() returned.
 */
static kode_status_t t4t_read(uint16_t off, uint8_t want, uint8_t *rx,
                              size_t rx_cap, size_t *rx_len)
{
    uint8_t apdu[5];

    apdu[0] = 0x00U;
    apdu[1] = 0xB0U;   /* READ BINARY */
    apdu[2] = (uint8_t)(off >> 8);
    apdu[3] = (uint8_t)(off & 0xFFU);
    apdu[4] = want;

    return t4t_apdu(apdu, (uint16_t)sizeof(apdu), rx, rx_cap, rx_len);
}

/**
 * @brief UPDATE BINARY into the selected file.
 *
 * @param off  Where to start.
 * @param data The bytes.
 * @param len  How many; at most 250, which leaves room for the header.
 * @return What t4t_apdu() returned.
 */
static kode_status_t t4t_update(uint16_t off, const uint8_t *data, uint8_t len)
{
    uint8_t apdu[5U + 250U];

    if ( 250U < len )
    {
        return KODE_DENIED;
    }
    apdu[0] = 0x00U;
    apdu[1] = 0xD6U;   /* UPDATE BINARY */
    apdu[2] = (uint8_t)(off >> 8);
    apdu[3] = (uint8_t)(off & 0xFFU);
    apdu[4] = len;
    (void)memcpy(&apdu[5], data, len);

    return t4t_apdu(apdu, (uint16_t)(5U + len), NULL, 0U, NULL);
}

/**
 * @brief Select the application and its NDEF file, and learn its limits.
 *
 * @param max_len Out: the largest message the file can hold.
 * @return KODE_OK, KODE_ERROR when the card's capability file does not
 *         describe an NDEF file, or whatever an APDU returned.
 */
static kode_status_t t4t_open_ndef(size_t *max_len)
{
    kode_status_t rc = t4t_select_app();
    if ( KODE_OK != rc )
    {
        return rc;
    }
    rc = t4t_select_file(CC_FILE_ID);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    uint8_t cc[CC_READ_LEN];
    size_t  got = 0U;
    rc = t4t_read(0U, CC_READ_LEN, cc, sizeof(cc), &got);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( CC_READ_LEN > got )
    {
        return KNFC_NDEF_NO_MESSAGE;
    }

    /* The NDEF File Control TLV: 04 06 <file id 2> <max size 2> <read 1>
     * <write 1>. */
    if ( CC_TLV_TYPE != cc[CC_TLV_OFF] )
    {
        return KNFC_NDEF_NO_MESSAGE;
    }

    uint8_t file_id[2];
    file_id[0] = cc[CC_TLV_OFF + 2U];
    file_id[1] = cc[CC_TLV_OFF + 3U];

    size_t declared = ((size_t)cc[CC_TLV_OFF + 4U] << 8)
                    | (size_t)cc[CC_TLV_OFF + 5U];
    if ( SW_LEN >= declared )
    {
        return KNFC_NDEF_NO_MESSAGE;
    }
    /* The first two bytes of the file are its length field, not message. */
    *max_len = declared - SW_LEN;

    return t4t_select_file(file_id);
}

/* ==========================================================================
 * Mifare Classic
 * ========================================================================== */

/*
 * The NFC Forum's mapping onto a card that was never meant to carry NDEF.
 *
 * Sector 0 holds the Mifare Application Directory: two bytes per sector
 * saying what lives there, and 0x03E1 means NDEF. The message itself is an
 * ordinary TLV in the data blocks of whichever sectors the MAD points at.
 *
 * kodeOS reads that map and writes where it says. It does NOT format a card
 * that has no map: writing a MAD means changing sector trailers, a trailer
 * holds the keys, and a wrong write there loses the sector for good. Turning
 * somebody's card into an NDEF tag is a decision with no undo, and not one
 * to take on their behalf because they held it near a reader.
 */

#define MAD_SECTOR        0U
#define MAD_BLOCK_1       1U     /**< Sectors 1..7.                        */
#define MAD_BLOCK_2       2U     /**< Sectors 8..15.                       */
#define MAD_AID_NDEF  0x03E1U
#define MFC_DATA_BLOCKS   3U     /**< Blocks per sector that are not trailer. */
#define MFC_SECTOR_1_BLK  4U     /**< Where sector 1's data starts.        */

/**
 * @brief Which sector the MAD says holds NDEF, if any.
 *
 * @param uid The card's four-byte identifier.
 * @param out Out: the sector number.
 * @return KODE_OK when one was found, KODE_ERROR when the card has no MAD
 *         or its MAD claims no NDEF, or whatever the card returned.
 */
static kode_status_t mfc_ndef_sector(const uint8_t *uid, uint8_t *out)
{
    kode_status_t rc = knfc_mfc_auth_any(MAD_BLOCK_1, uid, NULL, NULL);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    uint8_t blk[KNFC_MFC_BLOCK_LEN];
    rc = knfc_mfc_read(MAD_BLOCK_1, blk);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    /* Byte 0 is the MAD's own checksum; the entries start after it, two
     * bytes each, least significant first, sector 1 first. */
    for ( uint8_t sector = 1U; 8U > sector; ++sector )
    {
        uint8_t  off = (uint8_t)(sector * 2U);
        uint16_t aid = (uint16_t)blk[off]
                     | (uint16_t)((uint16_t)blk[off + 1U] << 8);
        if ( MAD_AID_NDEF == aid )
        {
            *out = sector;
            return KODE_OK;
        }
    }
    return KODE_ERROR;
}

/**
 * @brief Read the TLV area of a Mifare Classic into @p buf.
 *
 * @param uid    The card's four-byte identifier.
 * @param sector Which sector the message starts in.
 * @param buf    Out: the bytes.
 * @param cap    How many @p buf holds.
 * @param got    Out: how many were read.
 * @return KODE_OK, or whatever the card returned.
 */
static kode_status_t mfc_read_area(const uint8_t *uid, uint8_t sector,
                                   uint8_t *buf, size_t cap, size_t *got)
{
    size_t done = 0U;

    /* Two sectors is 96 bytes of message, which is more than a URI record
     * and a Text record together ever need here. */
    for ( uint8_t s = sector; ((sector + 2U) > s) && (done < cap); ++s )
    {
        kode_status_t rc = knfc_mfc_auth_any((uint8_t)(s * 4U), uid, NULL,
                                             NULL);
        if ( KODE_OK != rc )
        {
            if ( 0U < done )
            {
                break;
            }
            return rc;
        }
        for ( uint8_t b = 0U; (MFC_DATA_BLOCKS > b) && (done < cap); ++b )
        {
            uint8_t blk[KNFC_MFC_BLOCK_LEN];
            rc = knfc_mfc_read((uint8_t)((s * 4U) + b), blk);
            if ( KODE_OK != rc )
            {
                if ( 0U < done )
                {
                    break;
                }
                return rc;
            }
            size_t take = KNFC_MFC_BLOCK_LEN;
            if ( (cap - done) < take )
            {
                take = cap - done;
            }
            (void)memcpy(&buf[done], blk, take);
            done += take;
        }
    }
    *got = done;
    return KODE_OK;
}

/**
 * @brief Write @p len bytes into the data blocks from @p sector on.
 *
 * @param uid    The card's four-byte identifier.
 * @param sector Which sector to start in.
 * @param data   The bytes, already TLV-wrapped.
 * @param len    How many.
 * @return KODE_OK, KODE_NO_RESOURCES when it does not fit two sectors, or
 *         whatever the card returned.
 */
static kode_status_t mfc_write_area(const uint8_t *uid, uint8_t sector,
                                    const uint8_t *data, size_t len)
{
    size_t const room = (size_t)MFC_DATA_BLOCKS * KNFC_MFC_BLOCK_LEN * 2U;

    if ( len > room )
    {
        return KNFC_NDEF_TOO_LONG;
    }

    size_t done = 0U;
    for ( uint8_t s = sector; ((sector + 2U) > s) && (done < len); ++s )
    {
        kode_status_t rc = knfc_mfc_auth_any((uint8_t)(s * 4U), uid, NULL,
                                             NULL);
        if ( KODE_OK != rc )
        {
            return rc;
        }
        for ( uint8_t b = 0U; (MFC_DATA_BLOCKS > b) && (done < len); ++b )
        {
            uint8_t blk[KNFC_MFC_BLOCK_LEN];
            size_t  take = len - done;
            if ( KNFC_MFC_BLOCK_LEN < take )
            {
                take = KNFC_MFC_BLOCK_LEN;
            }
            /* The tail is zeroed: a NULL TLV, which any reader steps over. */
            (void)memset(blk, 0, sizeof(blk));
            (void)memcpy(blk, &data[done], take);

            rc = knfc_mfc_write((uint8_t)((s * 4U) + b), blk);
            if ( KODE_OK != rc )
            {
                return rc;
            }
            done += take;
        }
    }
    return KODE_OK;
}

/**
 * @brief The four bytes a Mifare Classic authenticates with.
 *
 * The cipher is seeded from a four-byte identifier. A card with a longer
 * UID - a 4K with a 7-byte one - uses the LAST four, not the first.
 *
 * @param tag The activated tag.
 * @return A pointer into @p tag's own UID.
 */
static const uint8_t *cuid_of(const knfc_tag_t *tag)
{
    if ( 4U < tag->uid_len )
    {
        return &tag->uid[tag->uid_len - 4U];
    }
    return tag->uid;
}

/**
 * @brief Where a Mifare Classic's message lives, and whether a phone
 *        will find it there.
 *
 * @param uid    The card's four-byte identifier.
 * @param out    Out: the sector to use.
 * @param mapped Out: whether the MAD actually points at it.
 * @return KODE_OK always; @p mapped says how much to trust @p out.
 */
static kode_status_t mfc_where(const uint8_t *uid, uint8_t *out, bool *mapped)
{
    *mapped = false;
    *out    = 1U;

    if ( mfc_ndef_sector(uid, out) == KODE_OK )
    {
        *mapped = true;
    }
    return KODE_OK;
}

/* ==========================================================================
 * What the caller sees
 * ========================================================================== */

kode_status_t knfc_tag_ndef_read(const knfc_tag_t *tag, char *out, size_t cap,
                                 size_t *n, knfc_ndef_kind_t *kind)
{
    if ( (NULL == tag) || (NULL == out) || (0U == cap) )
    {
        return KODE_DENIED;
    }

    uint8_t       raw[NDEF_MAX];
    size_t        len = 0U;
    kode_status_t rc  = KODE_DENIED;

    if ( KNFC_TAG_T2T == tag->type )
    {
        size_t size     = 0U;
        bool   writable = false;
        rc = t2t_cc(&size, &writable);
        if ( KODE_OK != rc )
        {
            return rc;
        }
        if ( sizeof(raw) < size )
        {
            size = sizeof(raw);
        }
        rc = t2t_read_area(raw, size, &len);
        if ( KODE_OK != rc )
        {
            return rc;
        }
    }
    else if ( KNFC_TAG_MIFARE == tag->type )
    {
        const uint8_t *cuid   = cuid_of(tag);
        uint8_t        sector = 1U;
        bool           mapped = false;

        (void)mfc_where(cuid, &sector, &mapped);
        knfc_mfc_reset();
        rc = mfc_read_area(cuid, sector, raw, sizeof(raw), &len);
        if ( KODE_OK != rc )
        {
            return rc;
        }
    }
    else if ( KNFC_TAG_T4T == tag->type )
    {
        size_t max_len = 0U;
        rc = t4t_open_ndef(&max_len);
        if ( KODE_OK != rc )
        {
            return rc;
        }

        /* The file's first two bytes are the message length. */
        uint8_t nlen[2];
        size_t  got = 0U;
        rc = t4t_read(0U, SW_LEN, nlen, sizeof(nlen), &got);
        if ( KODE_OK != rc )
        {
            return rc;
        }
        if ( SW_LEN > got )
        {
            return KNFC_NDEF_NO_MESSAGE;
        }

        size_t msg = ((size_t)nlen[0] << 8) | (size_t)nlen[1];
        if ( 0U == msg )
        {
            return KNFC_NDEF_NO_MESSAGE;
        }
        if ( sizeof(raw) < msg )
        {
            msg = sizeof(raw);
        }

        /* A Type-4 file carries the bare message, with no TLV around it, so
         * one is put back on for the shared parser. Two bytes of header and
         * one of terminator have to fit. */
        if ( (msg + 3U) > sizeof(raw) )
        {
            msg = sizeof(raw) - 3U;
        }
        raw[0] = (uint8_t)KNFC_TLV_NDEF;
        raw[1] = (uint8_t)msg;

        while ( len < msg )
        {
            size_t  want = msg - len;
            if ( 250U < want )
            {
                want = 250U;
            }
            size_t chunk = 0U;
            rc = t4t_read((uint16_t)(SW_LEN + len), (uint8_t)want,
                          &raw[2U + len], sizeof(raw) - 2U - len, &chunk);
            if ( KODE_OK != rc )
            {
                return rc;
            }
            if ( 0U == chunk )
            {
                break;
            }
            len += chunk;
        }
        raw[2U + len] = (uint8_t)KNFC_TLV_TERMINATOR;
        len += 3U;
    }
    else
    {
        return KNFC_NDEF_LOCKED;
    }

    rc = knfc_ndef_parse(raw, len, out, cap, kind);
    if ( (KODE_OK == rc) && (NULL != n) )
    {
        *n = strlen(out);
    }
    return rc;
}

kode_status_t knfc_tag_ndef_write(const knfc_tag_t *tag, const char *text,
                                  bool uri)
{
    if ( (NULL == tag) || (NULL == text) )
    {
        return KODE_DENIED;
    }

    uint8_t       rec[NDEF_MAX];
    size_t        rec_len = 0U;
    kode_status_t rc      = KODE_DENIED;

    if ( uri )
    {
        rc = knfc_ndef_make_uri(text, rec, sizeof(rec), &rec_len);
    }
    else
    {
        rc = knfc_ndef_make_text(text, NULL, rec, sizeof(rec), &rec_len);
    }
    if ( KODE_OK != rc )
    {
        return rc;
    }

    if ( KNFC_TAG_T2T == tag->type )
    {
        size_t size     = 0U;
        bool   writable = false;
        rc = t2t_cc(&size, &writable);
        if ( KODE_OK != rc )
        {
            return rc;
        }
        if ( !writable )
        {
            KLOG_W(TAG, "the tag says it is read-only");
            return KNFC_NDEF_LOCKED;
        }

        uint8_t tlv[NDEF_MAX];
        size_t  tlv_len = 0U;
        rc = knfc_ndef_wrap_tlv(rec, rec_len, tlv, sizeof(tlv), &tlv_len);
        if ( KODE_OK != rc )
        {
            return rc;
        }
        /* Checked before the first block goes out: a half-written tag reads
         * as corrupt to everything, and the caller cannot tell from the
         * error which of the two it got. */
        if ( tlv_len > size )
        {
            KLOG_W(TAG, "message is %u bytes, the tag holds %u",
                   (unsigned)tlv_len, (unsigned)size);
            return KNFC_NDEF_TOO_LONG;
        }
        return t2t_write_area(tlv, tlv_len);
    }

    if ( KNFC_TAG_MIFARE == tag->type )
    {
        const uint8_t *cuid   = cuid_of(tag);
        uint8_t        sector = 1U;
        bool           mapped = false;

        (void)mfc_where(cuid, &sector, &mapped);
        if ( !mapped )
        {
            /* Written where an NDEF-formatted card would keep it, so the
             * bytes are there and read back - but no MAD points at them, so
             * a phone will not find them. Said out loud rather than
             * silently formatting the card, which would mean rewriting its
             * sector keys with no way back. */
            KLOG_W(TAG, "this card has no NDEF directory; the message is "
                        "written to sector 1 and a phone will not see it");
        }

        uint8_t tlv[NDEF_MAX];
        size_t  tlv_len = 0U;
        rc = knfc_ndef_wrap_tlv(rec, rec_len, tlv, sizeof(tlv), &tlv_len);
        if ( KODE_OK != rc )
        {
            return rc;
        }
        knfc_mfc_reset();
        return mfc_write_area(cuid, sector, tlv, tlv_len);
    }

    if ( KNFC_TAG_T4T == tag->type )
    {
        size_t max_len = 0U;
        rc = t4t_open_ndef(&max_len);
        if ( KODE_OK != rc )
        {
            return rc;
        }
        if ( rec_len > max_len )
        {
            KLOG_W(TAG, "message is %u bytes, the file holds %u",
                   (unsigned)rec_len, (unsigned)max_len);
            return KNFC_NDEF_TOO_LONG;
        }

        /* The length field goes to zero first and is written last. A reader
         * that arrives in the middle sees an empty file rather than a
         * message that is half the old one and half the new. */
        uint8_t nlen[2] = { 0x00U, 0x00U };
        rc = t4t_update(0U, nlen, sizeof(nlen));
        if ( KODE_OK != rc )
        {
            return rc;
        }

        size_t done = 0U;
        while ( done < rec_len )
        {
            size_t take = rec_len - done;
            if ( 250U < take )
            {
                take = 250U;
            }
            rc = t4t_update((uint16_t)(SW_LEN + done), &rec[done],
                            (uint8_t)take);
            if ( KODE_OK != rc )
            {
                return rc;
            }
            done += take;
        }

        nlen[0] = (uint8_t)(rec_len >> 8);
        nlen[1] = (uint8_t)(rec_len & 0xFFU);
        return t4t_update(0U, nlen, sizeof(nlen));
    }

    return KNFC_NDEF_LOCKED;
}
