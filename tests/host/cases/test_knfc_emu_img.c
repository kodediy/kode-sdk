/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_emu_img.c
 * @brief knfc_emu_img: the identity and Type-2 image an emulated card presents.
 */
#include <string.h>

#include "unity.h"

#include "knfc_emu_img.h"
#include "knfc_ndef.h"

/* ---- cloning an identity verbatim -------------------------------------- */

TEST_CASE("a coherent 4-byte card is presented exactly", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    card.uid[0] = 0xDCU;
    card.uid[1] = 0x99U;
    card.uid[2] = 0x70U;
    card.uid[3] = 0x44U;
    card.uid_len = 4U;
    card.atqa    = 0x0004U;   /* size bits 00: 4-byte UID */
    card.sak     = 0x08U;     /* Mifare Classic 1K */

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_UINT8(4U, img.nfcid_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(card.uid, img.nfcid, 4U);
    TEST_ASSERT_EQUAL_HEX8(0x04U, img.sens_res[0]);  /* LSB first */
    TEST_ASSERT_EQUAL_HEX8(0x00U, img.sens_res[1]);
    TEST_ASSERT_EQUAL_HEX8(0x08U, img.sak);
}

TEST_CASE("a coherent 7-byte card keeps its UID and SAK", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;
    const uint8_t uid[7] = { 0x04U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };

    (void)memset(&card, 0, sizeof(card));
    (void)memcpy(card.uid, uid, 7U);
    card.uid_len = 7U;
    card.atqa    = 0x0044U;   /* size bits 01: 7-byte UID */
    card.sak     = 0x00U;

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_UINT8(7U, img.nfcid_len);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(uid, img.nfcid, 7U);
    TEST_ASSERT_EQUAL_HEX8(0x44U, img.sens_res[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, img.sens_res[1]);
}

TEST_CASE("a UID beginning with the cascade tag is corrected", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    card.uid[0] = 0x88U;     /* the cascade tag; never a real first UID byte */
    card.uid[1] = 0x01U;
    card.uid[2] = 0x02U;
    card.uid[3] = 0x03U;
    card.uid_len = 4U;
    card.atqa    = 0x0004U;
    card.sak     = 0x08U;

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_HEX8(0x04U, img.nfcid[0]);
}

/* ---- synthesising a Type-2 tag for a link ------------------------------- */

TEST_CASE("an incoherent ATQA is not cloned but synthesised", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    /* A 4-byte UID carried with the double-size (7-byte) ATQA: the "link"
     * marker, which must present as a 7-byte Type-2 tag, not a broken clone. */
    card.uid[0] = 0x4BU;   /* 'K' */
    card.uid[1] = 0x4FU;   /* 'O' */
    card.uid[2] = 0x44U;   /* 'D' */
    card.uid[3] = 0x45U;   /* 'E' */
    card.uid_len = 4U;
    card.atqa    = 0x0044U;

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_UINT8(7U, img.nfcid_len);
    TEST_ASSERT_EQUAL_HEX8(0x04U, img.nfcid[0]);   /* NXP manufacturer byte */
    TEST_ASSERT_EQUAL_HEX8(0x44U, img.sens_res[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, img.sens_res[1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, img.sak);
}

TEST_CASE("uid_len 0 synthesises a 7-byte tag", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    card.uid_len = 0U;       /* present a link: no identity to clone */
    card.uri     = true;
    (void)snprintf(card.ndef, sizeof(card.ndef), "%s", "https://kode.diy");

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_UINT8(7U, img.nfcid_len);
    TEST_ASSERT_EQUAL_HEX8(0x04U, img.nfcid[0]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, img.sak);
}

/* ---- the Type-2 image bytes -------------------------------------------- */

TEST_CASE("the image carries the UID pages and their check bytes",
          "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;
    const uint8_t uid[7] = { 0x04U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U };

    (void)memset(&card, 0, sizeof(card));
    (void)memcpy(card.uid, uid, 7U);
    card.uid_len = 7U;
    card.atqa    = 0x0044U;

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_UINT16(KNFC_EMU_T2T_BYTES, img.t2t_len);
    /* Pages 0-2: UID with BCC0 = CT^u0^u1^u2 and BCC1 = u3^u4^u5^u6. */
    TEST_ASSERT_EQUAL_HEX8(uid[0], img.t2t[0]);
    TEST_ASSERT_EQUAL_HEX8(uid[2], img.t2t[2]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(0x88U ^ uid[0] ^ uid[1] ^ uid[2]),
                           img.t2t[3]);
    TEST_ASSERT_EQUAL_HEX8(uid[3], img.t2t[4]);
    TEST_ASSERT_EQUAL_HEX8((uint8_t)(uid[3] ^ uid[4] ^ uid[5] ^ uid[6]),
                           img.t2t[8]);
    /* Page 3: the capability container. */
    TEST_ASSERT_EQUAL_HEX8(0xE1U, img.t2t[12]);
    TEST_ASSERT_EQUAL_HEX8(0x10U, img.t2t[13]);
}

TEST_CASE("the image NDEF area holds the encoded URI record", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    card.uid_len = 0U;
    card.uri     = true;
    (void)snprintf(card.ndef, sizeof(card.ndef), "%s", "https://kode.diy");

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));

    /* The area starts with the NDEF-message TLV (0x03), and the record parses
     * back to the URI that went in. */
    TEST_ASSERT_EQUAL_HEX8(0x03U, img.t2t[KNFC_EMU_T2T_NDEF_OFF]);

    char  back[64];
    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_parse(&img.t2t[KNFC_EMU_T2T_NDEF_OFF],
                        (size_t)(KNFC_EMU_T2T_BYTES - KNFC_EMU_T2T_NDEF_OFF),
                        back, sizeof(back), NULL));
    TEST_ASSERT_EQUAL_STRING("https://kode.diy", back);
}

TEST_CASE("an empty message becomes a default Text record", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    card.uid_len = 0U;
    card.ndef[0] = '\0';

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));

    char  back[64];
    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_parse(&img.t2t[KNFC_EMU_T2T_NDEF_OFF],
                        (size_t)(KNFC_EMU_T2T_BYTES - KNFC_EMU_T2T_NDEF_OFF),
                        back, sizeof(back), NULL));
    TEST_ASSERT_EQUAL_STRING("KODE", back);
}

/* ---- refusals ---------------------------------------------------------- */

TEST_CASE("a bad UID length is refused", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    card.uid_len = 5U;   /* not 0, 4, 7 or 10 */

    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_emu_image_build(&card, &img));
}

TEST_CASE("a NULL argument is refused", "[knfc_emu_img]")
{
    knfc_emu_image_t img;
    knfc_emu_card_t  card;

    (void)memset(&card, 0, sizeof(card));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_emu_image_build(NULL, &img));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_emu_image_build(&card, NULL));
}

/* ---- a ready-made NDEF message ------------------------------------------ */
/* The three cases below come from Luismi's kode-dot-os (0574a8f). */

TEST_CASE("a raw NDEF message is wrapped as it is, ahead of uri and ndef", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;
    /* A short-record MIME message: MB|ME|SR|TNF=2, type "a/b", payload "xyz". */
    const uint8_t msg[] = { 0xD2U, 0x03U, 0x03U, 'a', '/', 'b', 'x', 'y', 'z' };

    (void)memset(&card, 0, sizeof(card));
    card.uri = true;
    (void)strcpy(card.ndef, "https://ignored.example");
    (void)memcpy(card.raw, msg, sizeof(msg));
    card.raw_len = (uint8_t)sizeof(msg);

    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_HEX8(0x03U, img.t2t[KNFC_EMU_T2T_NDEF_OFF]);                 /* NDEF TLV */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)sizeof(msg), img.t2t[KNFC_EMU_T2T_NDEF_OFF + 1U]);
    TEST_ASSERT_EQUAL_UINT8_ARRAY(msg, &img.t2t[KNFC_EMU_T2T_NDEF_OFF + 2U], sizeof(msg));
    TEST_ASSERT_EQUAL_HEX8(0xFEU, img.t2t[KNFC_EMU_T2T_NDEF_OFF + 2U + sizeof(msg)]); /* terminator */
}

TEST_CASE("the longest raw message fits the NDEF area", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    (void)memset(card.raw, 0x55, KNFC_EMU_RAW_MAX);
    card.raw_len = (uint8_t)KNFC_EMU_RAW_MAX;
    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_HEX8(0xFEU, img.t2t[KNFC_EMU_T2T_NDEF_OFF + 2U + KNFC_EMU_RAW_MAX]);
}

TEST_CASE("a zero raw_len keeps the old text behaviour", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    TEST_ASSERT_EQUAL(KODE_OK, knfc_emu_image_build(&card, &img));
    TEST_ASSERT_EQUAL_HEX8(0x03U, img.t2t[KNFC_EMU_T2T_NDEF_OFF]);
}

TEST_CASE("a raw message over KNFC_EMU_RAW_MAX is refused", "[knfc_emu_img]")
{
    knfc_emu_card_t card;
    knfc_emu_image_t img;

    (void)memset(&card, 0, sizeof(card));
    card.raw_len = (uint8_t)(KNFC_EMU_RAW_MAX + 1U);
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_emu_image_build(&card, &img));
}
