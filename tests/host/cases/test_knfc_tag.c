/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_tag.c
 * @brief knfc_tag: where a message lives on each kind of tag.
 */
#include <string.h>

#include "unity.h"

#include "Mockkdrv_st25r3916.h"
#include "knfc_tag.h"

/* ---- what the mock hands back ------------------------------------------ */

/** @brief A Type-2 tag: 4-byte UID, SAK 0x00. */
static uint8_t  s_uid[KDRV_ST25R3916_UID_MAX] = { 0x04U, 0x4BU, 0x4FU, 0x44U };
static uint8_t  s_uid_len = 4U;
static uint16_t s_atqa    = 0x0044U;
static uint8_t  s_sak_t2t = 0x00U;
static uint8_t  s_sak_t4t = 0x20U;   /* bit 5: ISO-DEP        */
static uint8_t  s_sak_p2p = 0x40U;   /* bit 6: NFC-DEP        */
static uint8_t  s_sak_mfc = 0x08U;   /* bit 3: Mifare Classic 1K */
static uint8_t  s_sak_plus = 0x28U;  /* both: reachable as Type-4 */

/* Blocks 0..3 of a formatted NTAG213: the CC in block 3 says E1 10 12 00 -
 * 0x12 * 8 = 144 usable bytes, and write access open. */
static uint8_t s_head[16] = {
    0x04U, 0x4BU, 0x4FU, 0x44U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0xE1U, 0x10U, 0x12U, 0x00U,
};

/* A read-only CC: the low nibble of byte 3 is non-zero. */
static uint8_t s_head_ro[16] = {
    0x04U, 0x4BU, 0x4FU, 0x44U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0xE1U, 0x10U, 0x12U, 0x0FU,
};

/* A tag that was never NDEF-formatted: no 0xE1 magic. */
static uint8_t s_head_raw[16] = {
    0x04U, 0x4BU, 0x4FU, 0x44U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
};

/* A CC declaring eight usable bytes: too small for anything real. */
static uint8_t s_head_tiny[16] = {
    0x04U, 0x4BU, 0x4FU, 0x44U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0x00U, 0x00U, 0x00U, 0x00U,
    0xE1U, 0x10U, 0x01U, 0x00U,
};

/* The data area: a URI record for "https://a.io", TLV-wrapped. */
static uint8_t s_area[16] = {
    0x03U, 0x09U,
    0xD1U, 0x01U, 0x05U, 'U', 0x04U, 'a', '.', 'i', 'o',
    0xFEU, 0x00U, 0x00U, 0x00U, 0x00U,
};

static uint16_t s_read16 = 16U;

/* Every block a write was offered, in order. */
#define WRITES_MAX 64
static uint8_t s_written[WRITES_MAX][4];
static uint8_t s_write_block[WRITES_MAX];
static int     s_writes;

/* Every APDU that went out, in order. */
#define APDUS_MAX 32
static uint8_t  s_sent[APDUS_MAX][64];
static uint16_t s_sent_len[APDUS_MAX];
static int      s_apdus;

/** @brief The card's answer to each APDU, in the order they are asked. */
static const uint8_t *s_answer[APDUS_MAX];
static uint16_t       s_answer_len[APDUS_MAX];

/* 0x9000 alone: what a SELECT or an UPDATE answers. */
static const uint8_t OK_SW[]    = { 0x90U, 0x00U };
/* A capability file: len 15, mapping 2.0, MLe 256, MLc 255, then the NDEF
 * File Control TLV - type 04, len 06, file E104, max 256, read 0, write 0. */
static const uint8_t CC_FILE[]  = { 0x00U, 0x0FU, 0x20U, 0x01U, 0x00U,
                                    0x00U, 0xFFU,
                                    0x04U, 0x06U, 0xE1U, 0x04U, 0x01U,
                                    0x00U, 0x00U, 0x00U,
                                    0x90U, 0x00U };
/* NLEN = 11, then the bare message. */
static const uint8_t NLEN_11[]  = { 0x00U, 0x0BU, 0x90U, 0x00U };
static const uint8_t MSG_11[]   = { 0xD1U, 0x01U, 0x05U, 'U', 0x04U,
                                    'a', '.', 'i', 'o', 0x00U, 0x00U,
                                    0x90U, 0x00U };
/* A capability file whose NDEF file holds ten bytes and no more. */
static const uint8_t CC_TINY[]  = { 0x00U, 0x0FU, 0x20U, 0x01U, 0x00U,
                                    0x00U, 0xFFU,
                                    0x04U, 0x06U, 0xE1U, 0x04U, 0x00U,
                                    0x0AU, 0x00U, 0x00U,
                                    0x90U, 0x00U };

/**
 * @brief Stands in for the reader on every Type-2 read.
 *
 * @param block The block asked for.
 * @param buf   Out: what the tag would have answered.
 * @param cap   How many bytes @p buf holds.
 * @param got   Out: how many were written.
 * @param calls Which call this is; unused.
 * @return KODE_OK.
 */
static kode_status_t t2t_read_cb(uint8_t block, uint8_t *buf, uint16_t cap,
                                 uint16_t *got, int calls)
{
    (void)calls;
    (void)cap;
    const uint8_t *src = s_area;
    if ( 4U > block )
    {
        src = s_head;
    }
    (void)memcpy(buf, src, 16U);
    if ( NULL != got )
    {
        *got = 16U;
    }
    return KODE_OK;
}

/**
 * @brief Records every block a write was offered.
 *
 * @param block The block.
 * @param data  The four bytes.
 * @param calls Which call this is; unused.
 * @return KODE_OK.
 */
static kode_status_t t2t_write_cb(uint8_t block, const uint8_t *data,
                                  int calls)
{
    (void)calls;
    if ( WRITES_MAX > s_writes )
    {
        s_write_block[s_writes] = block;
        (void)memcpy(s_written[s_writes], data, 4U);
    }
    ++s_writes;
    return KODE_OK;
}

/**
 * @brief Records every APDU and answers from the script.
 *
 * @param tx     The command.
 * @param tx_len How long.
 * @param rx     Out: the answer.
 * @param rx_cap How many bytes @p rx holds.
 * @param rx_len Out: how many were written.
 * @param calls  Which call this is; unused.
 * @return KODE_OK.
 */
static kode_status_t apdu_cb(const uint8_t *tx, uint16_t tx_len, uint8_t *rx,
                             uint16_t rx_cap, uint16_t *rx_len, int calls)
{
    (void)calls;
    (void)rx_cap;
    int i = s_apdus;
    if ( APDUS_MAX > i )
    {
        uint16_t n = tx_len;
        if ( (uint16_t)sizeof(s_sent[0]) < n )
        {
            n = (uint16_t)sizeof(s_sent[0]);
        }
        (void)memcpy(s_sent[i], tx, n);
        s_sent_len[i] = tx_len;
    }
    ++s_apdus;

    const uint8_t *ans = OK_SW;
    uint16_t       len = (uint16_t)sizeof(OK_SW);
    if ( (APDUS_MAX > i) && (NULL != s_answer[i]) )
    {
        ans = s_answer[i];
        len = s_answer_len[i];
    }
    (void)memcpy(rx, ans, len);
    if ( NULL != rx_len )
    {
        *rx_len = len;
    }
    return KODE_OK;
}

/** @brief Point call @p i at answer @p a. */
static void answer_with(int i, const uint8_t *a, uint16_t len)
{
    s_answer[i]     = a;
    s_answer_len[i] = len;
}

void setUp(void);
void tearDown(void);

/** @brief Fresh mock, fresh recordings, before each case. */
static void fresh(void)
{
    Mockkdrv_st25r3916_Init();
    s_writes = 0;
    s_apdus  = 0;
    (void)memset(s_answer, 0, sizeof(s_answer));
    (void)memset(s_answer_len, 0, sizeof(s_answer_len));
}

/**
 * @brief Arrange for knfc_tag_identify() to report a tag with @p sak.
 *
 * @param sak What the anticollision reported.
 */
static void tag_is(uint8_t sak)
{
    /* CMock keeps the pointer, not the byte, and reads it when the mock is
     * called - after this helper has returned - so the byte must outlive it. */
    static uint8_t s_reply_sak;

    s_reply_sak = sak;
    kdrv_st25r3916_active_tag_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_active_tag_ReturnArrayThruPtr_uid(s_uid,
                                                     KDRV_ST25R3916_UID_MAX);
    kdrv_st25r3916_active_tag_ReturnThruPtr_uid_len(&s_uid_len);
    kdrv_st25r3916_active_tag_ReturnThruPtr_atqa(&s_atqa);
    kdrv_st25r3916_active_tag_ReturnThruPtr_sak(&s_reply_sak);
}

/* ---- identity ---------------------------------------------------------- */

TEST_CASE("the tag type is read off the SAK, not asked of the caller",
          "[knfc_tag]")
{
    knfc_tag_t tag;

    fresh();
    tag_is(s_sak_t2t);
    TEST_ASSERT_EQUAL(KODE_OK, knfc_tag_identify(&tag));
    TEST_ASSERT_EQUAL(KNFC_TAG_T2T, tag.type);
    TEST_ASSERT_EQUAL_UINT8(4U, tag.uid_len);
    TEST_ASSERT_EQUAL_HEX16(0x0044U, tag.atqa);
    Mockkdrv_st25r3916_Verify();

    fresh();
    tag_is(s_sak_t4t);
    TEST_ASSERT_EQUAL(KODE_OK, knfc_tag_identify(&tag));
    TEST_ASSERT_EQUAL(KNFC_TAG_T4T, tag.type);
    Mockkdrv_st25r3916_Verify();

    fresh();
    tag_is(s_sak_p2p);
    TEST_ASSERT_EQUAL(KODE_OK, knfc_tag_identify(&tag));
    TEST_ASSERT_EQUAL(KNFC_TAG_UNKNOWN, tag.type);
    Mockkdrv_st25r3916_Verify();
}

/*
 * The case the bench found. A Mifare Classic 1K answers ATQA 0x0004 and SAK
 * 0x08 - neither ISO-DEP nor NFC-DEP - so a rule of "anything else is a
 * Type-2" calls it one, and every read then fails with nothing to say why.
 * It is named instead, and a card that is both Mifare and ISO-DEP is taken
 * as the Type-4 it also is.
 */
TEST_CASE("a Mifare Classic is named, not mistaken for a Type-2",
          "[knfc_tag]")
{
    knfc_tag_t tag;

    fresh();
    tag_is(s_sak_mfc);
    TEST_ASSERT_EQUAL(KODE_OK, knfc_tag_identify(&tag));
    TEST_ASSERT_EQUAL(KNFC_TAG_MIFARE, tag.type);
    Mockkdrv_st25r3916_Verify();

    fresh();
    tag_is(s_sak_plus);
    TEST_ASSERT_EQUAL(KODE_OK, knfc_tag_identify(&tag));
    TEST_ASSERT_EQUAL(KNFC_TAG_T4T, tag.type);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a Mifare Classic that answers nothing fails without hanging",
          "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_MIFARE, .uid_len = 4U };
    char       out[32];

    /* A card that will not authenticate goes silent, and every key in the
     * dictionary is then tried against that silence. What must not happen is
     * an unbounded walk: the dictionary is finite, each attempt ends in a
     * timeout, and the answer comes back. */
    fresh();
    kdrv_st25r3916_transceive_bits_IgnoreAndReturn(KODE_TIMEOUT);
    kdrv_st25r3916_reselect_IgnoreAndReturn(KODE_OK);
    TEST_ASSERT_NOT_EQUAL(KODE_OK,
        knfc_tag_ndef_read(&tag, out, sizeof(out), NULL, NULL));
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("identify refuses a NULL rather than dereferencing it",
          "[knfc_tag]")
{
    fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_tag_identify(NULL));
    Mockkdrv_st25r3916_Verify();
}

/* ---- Type-2 reading ---------------------------------------------------- */

TEST_CASE("a Type-2 read walks the TLVs and returns the URI", "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T2T };
    char       out[64];
    size_t     n = 0U;

    fresh();
    kdrv_st25r3916_t2t_read_StubWithCallback(&t2t_read_cb);
    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_tag_ndef_read(&tag, out, sizeof(out), &n, NULL));
    TEST_ASSERT_EQUAL_STRING("https://a.io", out);
    TEST_ASSERT_EQUAL_UINT(strlen("https://a.io"), n);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a Type-2 read says the record was a URI", "[knfc_tag]")
{
    knfc_tag_t       tag  = { .type = KNFC_TAG_T2T };
    char             out[64];
    knfc_ndef_kind_t kind = KNFC_NDEF_TEXT;

    fresh();
    kdrv_st25r3916_t2t_read_StubWithCallback(&t2t_read_cb);
    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_tag_ndef_read(&tag, out, sizeof(out), NULL, &kind));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KNFC_NDEF_URI, (uint32_t)kind);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a Type-2 read says the record was Text", "[knfc_tag]")
{
    static const uint8_t text_area[16] = {
        0x03U, 0x09U,
        0xD1U, 0x01U, 0x05U, 'T', 0x02U, 'e', 'n', 'h', 'i',
        0xFEU, 0x00U, 0x00U, 0x00U, 0x00U,
    };
    uint8_t          saved[sizeof(s_area)];
    knfc_tag_t       tag  = { .type = KNFC_TAG_T2T };
    char             out[64];
    knfc_ndef_kind_t kind = KNFC_NDEF_URI;

    (void)memcpy(saved, s_area, sizeof(saved));
    (void)memcpy(s_area, text_area, sizeof(s_area));
    fresh();
    kdrv_st25r3916_t2t_read_StubWithCallback(&t2t_read_cb);
    kode_status_t rc = knfc_tag_ndef_read(&tag, out, sizeof(out), NULL, &kind);
    (void)memcpy(s_area, saved, sizeof(s_area));

    TEST_ASSERT_EQUAL(KODE_OK, rc);
    TEST_ASSERT_EQUAL_STRING("hi", out);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KNFC_NDEF_TEXT, (uint32_t)kind);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a read that fails leaves the kind alone", "[knfc_tag]")
{
    knfc_tag_t       tag  = { .type = KNFC_TAG_T2T };
    char             out[64];
    knfc_ndef_kind_t kind = KNFC_NDEF_TEXT;

    fresh();
    kdrv_st25r3916_t2t_read_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_t2t_read_ReturnArrayThruPtr_buf(s_head_raw, 16);
    kdrv_st25r3916_t2t_read_ReturnThruPtr_got(&s_read16);
    TEST_ASSERT_EQUAL(KODE_ERROR,
        knfc_tag_ndef_read(&tag, out, sizeof(out), NULL, &kind));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)KNFC_NDEF_TEXT, (uint32_t)kind);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a tag that was never formatted is not read as an empty one",
          "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T2T };
    char       out[64];

    fresh();
    kdrv_st25r3916_t2t_read_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_t2t_read_ReturnArrayThruPtr_buf(s_head_raw, 16);
    kdrv_st25r3916_t2t_read_ReturnThruPtr_got(&s_read16);
    TEST_ASSERT_EQUAL(KODE_ERROR,
        knfc_tag_ndef_read(&tag, out, sizeof(out), NULL, NULL));
    Mockkdrv_st25r3916_Verify();
}

/* ---- Type-2 writing: the case that cannot be seen on a bench ----------- */

TEST_CASE("a message larger than the tag is refused before the first write",
          "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T2T };

    fresh();
    kdrv_st25r3916_t2t_read_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_t2t_read_ReturnArrayThruPtr_buf(s_head_tiny, 16);
    kdrv_st25r3916_t2t_read_ReturnThruPtr_got(&s_read16);
    /* No write is expected. An unexpected one fails the case. */
    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES,
        knfc_tag_ndef_write(&tag, "https://kode.diy/a/rather/long/one",
                            true));
    TEST_ASSERT_EQUAL_INT(0, s_writes);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a tag that says it is read-only is not written to", "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T2T };

    fresh();
    kdrv_st25r3916_t2t_read_ExpectAnyArgsAndReturn(KODE_OK);
    kdrv_st25r3916_t2t_read_ReturnArrayThruPtr_buf(s_head_ro, 16);
    kdrv_st25r3916_t2t_read_ReturnThruPtr_got(&s_read16);
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_tag_ndef_write(&tag, "a.io", true));
    TEST_ASSERT_EQUAL_INT(0, s_writes);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a Type-2 write starts at block 4 and pads its last page",
          "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T2T };

    fresh();
    kdrv_st25r3916_t2t_read_StubWithCallback(&t2t_read_cb);
    kdrv_st25r3916_t2t_write_StubWithCallback(&t2t_write_cb);

    TEST_ASSERT_EQUAL(KODE_OK, knfc_tag_ndef_write(&tag, "a.io", true));

    /* 03 09 D1 01 05 55 04 61 2E 69 6F FE = 12 bytes = three whole pages. */
    TEST_ASSERT_EQUAL_INT(3, s_writes);
    TEST_ASSERT_EQUAL_UINT8(4U, s_write_block[0]);
    TEST_ASSERT_EQUAL_UINT8(5U, s_write_block[1]);
    TEST_ASSERT_EQUAL_UINT8(6U, s_write_block[2]);
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_written[0][0]);
    TEST_ASSERT_EQUAL_HEX8(0x09U, s_written[0][1]);
    TEST_ASSERT_EQUAL_HEX8(0xFEU, s_written[2][3]);
    Mockkdrv_st25r3916_Verify();
}

/* ---- Type-4 ------------------------------------------------------------ */

TEST_CASE("a Type-4 read selects the application, the CC file, then the file",
          "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T4T };
    char       out[64];

    fresh();
    answer_with(2, CC_FILE, (uint16_t)sizeof(CC_FILE));
    answer_with(4, NLEN_11, (uint16_t)sizeof(NLEN_11));
    answer_with(5, MSG_11, (uint16_t)sizeof(MSG_11));
    kdrv_st25r3916_apdu_StubWithCallback(&apdu_cb);

    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_tag_ndef_read(&tag, out, sizeof(out), NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("https://a.io", out);

    /* SELECT by name, SELECT CC by id, READ CC, SELECT NDEF by id,
     * READ length, READ message. */
    TEST_ASSERT_EQUAL_INT(6, s_apdus);
    TEST_ASSERT_EQUAL_HEX8(0xA4U, s_sent[0][1]);   /* SELECT       */
    TEST_ASSERT_EQUAL_HEX8(0x04U, s_sent[0][2]);   /* by name      */
    TEST_ASSERT_EQUAL_HEX8(0xA4U, s_sent[1][1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[1][2]);   /* by identifier */
    TEST_ASSERT_EQUAL_HEX8(0xE1U, s_sent[1][5]);
    TEST_ASSERT_EQUAL_HEX8(0x03U, s_sent[1][6]);   /* the CC file  */
    TEST_ASSERT_EQUAL_HEX8(0xB0U, s_sent[2][1]);   /* READ BINARY  */
    TEST_ASSERT_EQUAL_HEX8(0xE1U, s_sent[3][5]);
    TEST_ASSERT_EQUAL_HEX8(0x04U, s_sent[3][6]);   /* the NDEF file */
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a Type-4 update writes its length field last", "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T4T };

    fresh();
    answer_with(2, CC_FILE, (uint16_t)sizeof(CC_FILE));
    kdrv_st25r3916_apdu_StubWithCallback(&apdu_cb);

    TEST_ASSERT_EQUAL(KODE_OK, knfc_tag_ndef_write(&tag, "a.io", true));

    /* SELECT, SELECT CC, READ CC, SELECT NDEF, then: zero the length,
     * write the message, write the length. */
    TEST_ASSERT_EQUAL_INT(7, s_apdus);

    /* The first update zeroes the length field at offset 0. */
    TEST_ASSERT_EQUAL_HEX8(0xD6U, s_sent[4][1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[4][2]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[4][3]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[4][5]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[4][6]);

    /* The message goes at offset 2. */
    TEST_ASSERT_EQUAL_HEX8(0xD6U, s_sent[5][1]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, s_sent[5][3]);
    TEST_ASSERT_EQUAL_HEX8(0xD1U, s_sent[5][5]);

    /* And only then does the length become non-zero. */
    TEST_ASSERT_EQUAL_HEX8(0xD6U, s_sent[6][1]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[6][3]);
    TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[6][5]);
    TEST_ASSERT_EQUAL_HEX8(0x09U, s_sent[6][6]);
    Mockkdrv_st25r3916_Verify();
}

TEST_CASE("a Type-4 file too small for the message is refused before any "
          "update", "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_T4T };

    fresh();
    answer_with(2, CC_TINY, (uint16_t)sizeof(CC_TINY));
    kdrv_st25r3916_apdu_StubWithCallback(&apdu_cb);

    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES,
        knfc_tag_ndef_write(&tag, "https://kode.diy/long/enough", true));

    /* Four selects and reads, and not one UPDATE BINARY. */
    TEST_ASSERT_EQUAL_INT(4, s_apdus);
    Mockkdrv_st25r3916_Verify();
}

/* ---- refusals ----------------------------------------------------------- */

TEST_CASE("a tag type this cannot read is refused, not guessed at",
          "[knfc_tag]")
{
    knfc_tag_t tag = { .type = KNFC_TAG_UNKNOWN };
    char       out[16];

    fresh();
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_tag_ndef_read(&tag, out, sizeof(out), NULL, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_tag_ndef_write(&tag, "a", false));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_tag_ndef_read(NULL, out, sizeof(out), NULL, NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED, knfc_tag_ndef_write(&tag, NULL, false));
    Mockkdrv_st25r3916_Verify();
}
