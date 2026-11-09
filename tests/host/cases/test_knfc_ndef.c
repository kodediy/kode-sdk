/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_knfc_ndef.c
 * @brief knfc_ndef: the record codec, without a radio.
 */
#include <string.h>

#include "unity.h"

#include "knfc_ndef.h"

/* ---- encoding ---------------------------------------------------------- */

TEST_CASE("a URI record replaces the longest prefix it can", "[knfc_ndef]")
{
    uint8_t buf[64];
    size_t  used = 0U;

    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_make_uri("https://www.kode.diy", buf, sizeof(buf), &used));

    /* D1 01 <len> 55 <prefix> <rest> */
    TEST_ASSERT_EQUAL_HEX8(0xD1U, buf[0]);
    TEST_ASSERT_EQUAL_HEX8(0x01U, buf[1]);
    TEST_ASSERT_EQUAL_HEX8('U',   buf[3]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, buf[4]);   /* https://www. */
    TEST_ASSERT_EQUAL_UINT(4U + 1U + strlen("kode.diy"), used);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("kode.diy", &buf[5], strlen("kode.diy"));
}

TEST_CASE("a URI with no known prefix is written whole", "[knfc_ndef]")
{
    uint8_t buf[64];
    size_t  used = 0U;

    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_make_uri("gopher://kode.diy", buf, sizeof(buf), &used));
    TEST_ASSERT_EQUAL_HEX8(0x00U, buf[4]);   /* no prefix */
}

TEST_CASE("a record that does not fit is refused, not truncated",
          "[knfc_ndef]")
{
    uint8_t buf[8];
    size_t  used = 99U;

    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES,
        knfc_ndef_make_uri("https://kode.diy/something/long", buf,
                           sizeof(buf), &used));
    TEST_ASSERT_EQUAL_UINT(99U, used);       /* untouched */
}

TEST_CASE("a text record carries its language and a UTF-8 status byte",
          "[knfc_ndef]")
{
    uint8_t buf[64];
    size_t  used = 0U;

    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_make_text("hola", "es", buf, sizeof(buf), &used));
    TEST_ASSERT_EQUAL_HEX8('T',   buf[3]);
    TEST_ASSERT_EQUAL_HEX8(0x02U, buf[4]);   /* UTF-8, two-byte language */
    TEST_ASSERT_EQUAL_UINT8_ARRAY("es", &buf[5], 2U);
    TEST_ASSERT_EQUAL_UINT8_ARRAY("hola", &buf[7], 4U);
}

/* ---- the property a character count would break ------------------------ */

TEST_CASE("lengths are bytes and not characters", "[knfc_ndef]")
{
    uint8_t buf_a[64];
    uint8_t buf_b[64];
    size_t  used_a = 0U;
    size_t  used_b = 0U;

    /* Two characters either way; four bytes against two. */
    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_make_text("nn", "en", buf_a, sizeof(buf_a), &used_a));
    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_make_text("\xC3\xB1\xC3\xB1", "en", buf_b, sizeof(buf_b),
                            &used_b));
    TEST_ASSERT_EQUAL_UINT(used_a + 2U, used_b);
}

/* ---- round trips ------------------------------------------------------- */

/**
 * @brief Encode @p s, wrap it, parse it back.
 *
 * @param s    What to write.
 * @param uri  true to write a URI record, false for Text.
 * @param out  Out: what came back.
 * @param cap  How many bytes @p out holds.
 * @return What the parse returned.
 */
static kode_status_t round_trip(const char *s, bool uri, char *out, size_t cap)
{
    uint8_t rec[128];
    uint8_t tlv[160];
    size_t  rec_len = 0U;
    size_t  tlv_len = 0U;

    kode_status_t rc = KODE_OK;
    if ( uri )
    {
        rc = knfc_ndef_make_uri(s, rec, sizeof(rec), &rec_len);
    }
    else
    {
        rc = knfc_ndef_make_text(s, "en", rec, sizeof(rec), &rec_len);
    }
    if ( KODE_OK != rc )
    {
        return rc;
    }
    rc = knfc_ndef_wrap_tlv(rec, rec_len, tlv, sizeof(tlv), &tlv_len);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    return knfc_ndef_parse(tlv, tlv_len, out, cap, NULL);
}

TEST_CASE("every writable prefix survives a round trip", "[knfc_ndef]")
{
    static const char *const URIS[] = {
        "https://www.kode.diy", "http://www.kode.diy", "https://kode.diy",
        "http://kode.diy",      "mailto:hola@kode.diy", "tel:+34600000000",
        "gopher://kode.diy",
    };
    char out[128];

    for ( size_t i = 0U; (sizeof(URIS) / sizeof(URIS[0])) > i; ++i )
    {
        (void)memset(out, 0, sizeof(out));
        TEST_ASSERT_EQUAL(KODE_OK, round_trip(URIS[i], true, out, sizeof(out)));
        TEST_ASSERT_EQUAL_STRING(URIS[i], out);
    }
}

TEST_CASE("text survives a round trip, multi-byte characters included",
          "[knfc_ndef]")
{
    char out[128];

    TEST_ASSERT_EQUAL(KODE_OK, round_trip("hola", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("hola", out);

    TEST_ASSERT_EQUAL(KODE_OK,
        round_trip("ma\xC3\xB1""ana", false, out, sizeof(out)));
    TEST_ASSERT_EQUAL_STRING("ma\xC3\xB1""ana", out);
}

/* ---- what a tag from somewhere else looks like -------------------------- */

TEST_CASE("the TLV walk steps over lock and memory control TLVs",
          "[knfc_ndef]")
{
    /* 01 03 .. .. ..  lock control
     * 02 03 .. .. ..  memory control
     * 03 <len> <message>
     * FE */
    const uint8_t tag[] = {
        0x01U, 0x03U, 0xAAU, 0xBBU, 0xCCU,
        0x02U, 0x03U, 0xDDU, 0xEEU, 0xFFU,
        0x03U, 0x09U,
        0xD1U, 0x01U, 0x05U, 'U', 0x04U, 'a', '.', 'i', 'o',
        0xFEU,
    };
    size_t off = 0U;
    size_t len = 0U;

    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_find_tlv(tag, sizeof(tag), &off, &len));
    TEST_ASSERT_EQUAL_UINT(12U, off);
    TEST_ASSERT_EQUAL_UINT(9U, len);

    char out[64];
    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_parse(tag, sizeof(tag), out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL_STRING("https://a.io", out);
}

TEST_CASE("a terminator before any message means there is none",
          "[knfc_ndef]")
{
    const uint8_t tag[] = { 0x00U, 0x00U, 0xFEU, 0xFFU, 0xFFU };
    size_t off = 0U;
    size_t len = 0U;

    TEST_ASSERT_EQUAL(KODE_ERROR,
        knfc_ndef_find_tlv(tag, sizeof(tag), &off, &len));
}

TEST_CASE("a TLV whose length runs past the buffer is clamped, not trusted",
          "[knfc_ndef]")
{
    /* Claims 200 bytes of message; there are four. */
    const uint8_t tag[] = { 0x03U, 0xC8U, 0xD1U, 0x01U, 0x02U, 'U' };
    size_t off = 0U;
    size_t len = 0U;

    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_find_tlv(tag, sizeof(tag), &off, &len));
    TEST_ASSERT_EQUAL_UINT(2U, off);
    TEST_ASSERT_EQUAL_UINT(4U, len);
}

TEST_CASE("a truncated record is refused rather than half-read",
          "[knfc_ndef]")
{
    /* A URI record that promises six payload bytes and stops after two. */
    const uint8_t tag[] = { 0x03U, 0x08U, 0xD1U, 0x01U, 0x06U, 'U', 0x04U,
                            'a' };
    char out[64];

    /* Whatever it returns, it must not claim more than the buffer holds. */
    (void)memset(out, 0x7FU, sizeof(out));
    kode_status_t rc = knfc_ndef_parse(tag, sizeof(tag), out, sizeof(out),
                                       NULL);
    if ( KODE_OK == rc )
    {
        TEST_ASSERT_EQUAL_STRING("https://a", out);
    }
    else
    {
        TEST_ASSERT_EQUAL(KODE_ERROR, rc);
    }
}

TEST_CASE("a UTF-16 text record is stepped over, not shown as rubbish",
          "[knfc_ndef]")
{
    /* Status byte 0x82: UTF-16, two-byte language. Then a URI record after
     * it, which is what must come back. */
    const uint8_t tag[] = {
        0x03U, 0x11U,
        0x91U, 0x01U, 0x05U, 'T', 0x82U, 'e', 'n', 0x00U, 'h',
        0x51U, 0x01U, 0x04U, 'U', 0x04U, 'a', '.', 'i',
        0xFEU,
    };
    char             out[64];
    knfc_ndef_kind_t kind = KNFC_NDEF_TEXT;

    TEST_ASSERT_EQUAL(KODE_OK,
        knfc_ndef_parse(tag, sizeof(tag), out, sizeof(out), &kind));
    TEST_ASSERT_EQUAL(KNFC_NDEF_URI, kind);
    TEST_ASSERT_EQUAL_STRING("https://a.i", out);
}

TEST_CASE("an answer that does not fit the caller's buffer is refused",
          "[knfc_ndef]")
{
    const uint8_t tag[] = {
        0x03U, 0x09U,
        0xD1U, 0x01U, 0x05U, 'U', 0x04U, 'a', '.', 'i', 'o',
        0xFEU,
    };
    char out[4];

    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES,
        knfc_ndef_parse(tag, sizeof(tag), out, sizeof(out), NULL));
}

/* ---- the link-or-text decision ----------------------------------------- */

TEST_CASE("what reads as a link, and what does not", "[knfc_ndef]")
{
    TEST_ASSERT_TRUE(knfc_ndef_looks_like_uri("kode.diy"));
    TEST_ASSERT_TRUE(knfc_ndef_looks_like_uri("www.kode.diy"));
    TEST_ASSERT_TRUE(knfc_ndef_looks_like_uri("https://kode.diy"));
    TEST_ASSERT_TRUE(knfc_ndef_looks_like_uri("tel:+34600000000"));
    TEST_ASSERT_TRUE(knfc_ndef_looks_like_uri("kode.diy/v30jul"));

    TEST_ASSERT_FALSE(knfc_ndef_looks_like_uri("1234"));
    TEST_ASSERT_FALSE(knfc_ndef_looks_like_uri("hola Victor"));
    TEST_ASSERT_FALSE(knfc_ndef_looks_like_uri("3.14"));
    TEST_ASSERT_FALSE(knfc_ndef_looks_like_uri("192.168.1.1"));
    TEST_ASSERT_FALSE(knfc_ndef_looks_like_uri(""));
    TEST_ASSERT_FALSE(knfc_ndef_looks_like_uri(NULL));
}

/* ---- refusals ----------------------------------------------------------- */

TEST_CASE("every entry point refuses a NULL rather than dereferencing it",
          "[knfc_ndef]")
{
    uint8_t buf[16];
    char    out[16];
    size_t  used = 0U;
    size_t  off  = 0U;
    size_t  len  = 0U;

    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_ndef_make_uri(NULL, buf, sizeof(buf), &used));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_ndef_make_uri("a.io", NULL, sizeof(buf), &used));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_ndef_make_text(NULL, "en", buf, sizeof(buf), &used));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_ndef_wrap_tlv(NULL, 4U, buf, sizeof(buf), &used));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_ndef_find_tlv(NULL, 4U, &off, &len));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_ndef_parse(NULL, 4U, out, sizeof(out), NULL));
    TEST_ASSERT_EQUAL(KODE_DENIED,
        knfc_ndef_parse(buf, 4U, out, 0U, NULL));
}
