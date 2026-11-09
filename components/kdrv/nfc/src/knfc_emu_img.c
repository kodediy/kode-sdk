/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_emu_img.c
 * @brief Build the identity and Type-2 image an emulated NFC-A card presents.
 */
#include "knfc_emu_img.h"

#include <string.h>

#include "knfc_ndef.h"

/** @brief ISO 14443-3 cascade tag; never the first byte of a real UID. */
#define KNFC_EMU_CT       0x88U
/** @brief NXP manufacturer byte, the first of a synthesised UID. */
#define KNFC_EMU_NXP      0x04U
/** @brief SENS_RES size bits (bits 7:6): 4-byte UID. */
#define KNFC_EMU_SIZE_4   0x00U
/** @brief SENS_RES size bits: 7-byte (double) UID. */
#define KNFC_EMU_SIZE_7   0x40U
/** @brief SENS_RES size bits: 10-byte (triple) UID. */
#define KNFC_EMU_SIZE_10  0x80U

/**
 * @brief Whether @p card names an identity coherent enough to present verbatim.
 *
 * A clone is only presented as itself when the UID length is one the standard
 * allows and the SENS_RES carries the matching size bits: a mismatch is the
 * "present a link" path, which passes a 4-byte 'KODE' marker with the
 * double-size ATQA on purpose, and must take the synthesised branch instead.
 *
 * @param card The request.
 * @return true when the identity is coherent and should be cloned exactly.
 */
static bool card_is_exact(const knfc_emu_card_t *card)
{
    uint8_t want;

    if ( 4U == card->uid_len )
    {
        want = KNFC_EMU_SIZE_4;
    }
    else if ( 7U == card->uid_len )
    {
        want = KNFC_EMU_SIZE_7;
    }
    else if ( 10U == card->uid_len )
    {
        want = KNFC_EMU_SIZE_10;
    }
    else
    {
        return false;
    }
    if ( 0U == card->atqa )
    {
        return false;
    }
    return (uint8_t)(card->atqa & 0xC0U) == want;
}

/**
 * @brief The 7-byte UID the Type-2 image carries, whatever was cloned.
 *
 * The image is what a phone reads with a plain Type-2 READ, and a Type-2 tag
 * has a 7-byte UID, so a cloned 4-byte or 10-byte identity still gets a 7-byte
 * image UID that embeds it. A first byte of 0x88 is replaced: it is the
 * cascade tag and never a real UID byte.
 *
 * @param card The request.
 * @param uid7 Out: seven bytes.
 */
static void image_uid7(const knfc_emu_card_t *card, uint8_t uid7[7])
{
    if ( 7U == card->uid_len )
    {
        (void)memcpy(uid7, card->uid, 7U);
    }
    else
    {
        uid7[0] = KNFC_EMU_NXP;
        uid7[1] = card->uid[0];
        uid7[2] = card->uid[1];
        uid7[3] = card->uid[2];
        uid7[4] = card->uid[3];
        uid7[5] = (uint8_t)'K';
        uid7[6] = (uint8_t)'D';
    }
    if ( KNFC_EMU_CT == uid7[0] )
    {
        uid7[0] = KNFC_EMU_NXP;
    }
}

/**
 * @brief Lay the fixed Type-2 structure into @p t2t: UID pages, BCCs, the
 *        capability container.
 *
 * @param t2t  The image, at least KNFC_EMU_T2T_BYTES.
 * @param uid7 The seven-byte image UID.
 */
static void image_header(uint8_t *t2t, const uint8_t uid7[7])
{
    (void)memset(t2t, 0, KNFC_EMU_T2T_BYTES);

    /* Pages 0-2: UID and its two check bytes. */
    t2t[0] = uid7[0];
    t2t[1] = uid7[1];
    t2t[2] = uid7[2];
    t2t[3] = (uint8_t)(KNFC_EMU_CT ^ uid7[0] ^ uid7[1] ^ uid7[2]); /* BCC0 */
    t2t[4] = uid7[3];
    t2t[5] = uid7[4];
    t2t[6] = uid7[5];
    t2t[7] = uid7[6];
    t2t[8] = (uint8_t)(uid7[3] ^ uid7[4] ^ uid7[5] ^ uid7[6]);     /* BCC1 */
    t2t[9] = 0x48U;   /* internal byte, as an NTAG reports it */

    /* Page 3: capability container - NDEF-capable, 144-byte area, read/write. */
    t2t[12] = 0xE1U;
    t2t[13] = 0x10U;
    t2t[14] = 0x12U;
    t2t[15] = 0x00U;
}

kode_status_t knfc_emu_image_build(const knfc_emu_card_t *card,
                                   knfc_emu_image_t *out)
{
    if ( (NULL == card) || (NULL == out) )
    {
        return KODE_DENIED;
    }
    if ( (0U != card->uid_len) && (4U != card->uid_len)
        && (7U != card->uid_len) && (10U != card->uid_len) )
    {
        return KODE_DENIED;
    }
    if ( KNFC_EMU_RAW_MAX < card->raw_len )
    {
        return KODE_DENIED;
    }

    (void)memset(out, 0, sizeof(*out));

    uint8_t uid7[7];
    image_uid7(card, uid7);

    /* The presented identity: the cloned one when it is coherent, otherwise
     * the synthesised 7-byte Type-2 tag. */
    if ( card_is_exact(card) )
    {
        (void)memcpy(out->nfcid, card->uid, card->uid_len);
        if ( KNFC_EMU_CT == out->nfcid[0] )
        {
            out->nfcid[0] = KNFC_EMU_NXP;
        }
        out->nfcid_len   = card->uid_len;
        out->sens_res[0] = (uint8_t)(card->atqa & 0xFFU);
        out->sens_res[1] = (uint8_t)(card->atqa >> 8);
        out->sak         = card->sak;
    }
    else
    {
        (void)memcpy(out->nfcid, uid7, 7U);
        out->nfcid_len   = 7U;
        out->sens_res[0] = 0x44U;   /* SENS_RES 0x0044: double-size Type-2 */
        out->sens_res[1] = 0x00U;
        out->sak         = 0x00U;   /* no ISO-DEP, no Mifare auth */
    }

    /* The Type-2 image: fixed header, then the NDEF message. */
    image_header(out->t2t, uid7);

    uint8_t        rec[KNFC_EMU_NDEF_MAX + 8U];
    const uint8_t *msg     = rec;
    size_t         rec_len = 0U;
    kode_status_t  rc      = KODE_OK;
    if ( 0U != card->raw_len )
    {
        /* A message the caller built - a vCard, a Wi-Fi credential - goes
         * into the TLV as it is. */
        msg     = card->raw;
        rec_len = card->raw_len;
    }
    else if ( card->uri )
    {
        rc = knfc_ndef_make_uri(card->ndef, rec, sizeof(rec), &rec_len);
    }
    else
    {
        const char *text = card->ndef;
        if ( '\0' == text[0] )
        {
            text = "KODE";
        }
        rc = knfc_ndef_make_text(text, "en", rec, sizeof(rec), &rec_len);
    }
    if ( KODE_OK != rc )
    {
        return rc;
    }

    size_t used = 0U;
    rc = knfc_ndef_wrap_tlv(msg, rec_len, &out->t2t[KNFC_EMU_T2T_NDEF_OFF],
                            KNFC_EMU_T2T_BYTES - KNFC_EMU_T2T_NDEF_OFF, &used);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    out->t2t_len = (uint16_t)KNFC_EMU_T2T_BYTES;
    return KODE_OK;
}
