/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_kfw.c
 * @brief The firmware container: its header, and the decoder around it.
 */
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_kfw.h"
#include "khal_crc32.h"

/* ---- Building a header a case can corrupt ---------------------------------- */

/** @brief Where the CRC stops: the 52 bytes before it. */
#define HDR_CRC_LEN  offsetof(kfw_hdr_t, crc)

/**
 * @brief Lay a well-formed container header into @p buf.
 * @param buf  At least KFW_HDR_LEN bytes.
 * @param hdr  Receives the struct, for cases that then corrupt one field.
 */
static void make_hdr(uint8_t *buf, kfw_hdr_t *hdr)
{
    kfw_hdr_t h;

    (void)memset(&h, 0, sizeof(h));
    h.magic       = KFW_MAGIC;
    h.hdr_version = 1U;
    h.hdr_len     = KFW_HDR_LEN;
    h.target      = (uint8_t)KFW_TARGET_APP;
    h.comp        = (uint8_t)KFW_COMP_NONE;
    h.flags       = 0U;
    h.payload_len = 256U;
    h.image_len   = 256U;
    (void)snprintf(h.name, sizeof(h.name), "%s", "demo");

    h.crc = kfw_crc32(&h, HDR_CRC_LEN);

    (void)memcpy(buf, &h, sizeof(h));
    if ( NULL != hdr )
    {
        *hdr = h;
    }
}

/** @brief Re-seal @p h after a case changed a field, and lay it in @p buf. */
static void seal(uint8_t *buf, kfw_hdr_t *h)
{
    h->crc = kfw_crc32(h, HDR_CRC_LEN);
    (void)memcpy(buf, h, sizeof(*h));
}

/* ---- The header's own shape ------------------------------------------------ */

/* The layout is a contract with mkkfw.py on the host side. If these move, an
 * image built by the tool stops being readable by the board that it was built
 * for, and the symptom is every update failing at once. */
TEST_CASE("the header is 72 bytes with its checksum at 52", "[kdrv_kfw]")
{
    TEST_ASSERT_EQUAL_UINT(72U, sizeof(kfw_hdr_t));
    TEST_ASSERT_EQUAL_UINT(72U, (unsigned)KFW_HDR_LEN);
    TEST_ASSERT_EQUAL_UINT(12U, offsetof(kfw_hdr_t, payload_len));
    TEST_ASSERT_EQUAL_UINT(16U, offsetof(kfw_hdr_t, image_len));
    TEST_ASSERT_EQUAL_UINT(20U, offsetof(kfw_hdr_t, sha256));
    TEST_ASSERT_EQUAL_UINT(52U, offsetof(kfw_hdr_t, crc));
    TEST_ASSERT_EQUAL_UINT(56U, offsetof(kfw_hdr_t, name));
}

TEST_CASE("a well-formed header parses, and carries its fields", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t got;

    make_hdr(buf, NULL);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_hdr_parse(buf, sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT32(256U, got.image_len);
    TEST_ASSERT_EQUAL_UINT32(256U, got.payload_len);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_TARGET_APP, got.target);
    TEST_ASSERT_EQUAL_STRING("demo", got.name);
}

/* ---- Every way a header can lie -------------------------------------------- */

/* Not a container at all. This one must be *quiet*: the mailbox reader asks
 * every arriving image whether it is a container, and a raw ESP image
 * answering "no" is the ordinary case, not a fault worth a log line. */
TEST_CASE("a stream that is not a container is refused without complaint",
          "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.magic = 0U;
    seal(buf, &h);

    unsigned const before = fake_klog_count();
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before, fake_klog_count(),
        "'not a container' is an answer, not an error worth logging");
}

TEST_CASE("a header shorter than the header is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t got;

    make_hdr(buf, NULL);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR,
        kfw_hdr_parse(buf, KFW_HDR_LEN - 1U, &got));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, 0U, &got));

    /* And exactly the header is enough. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_hdr_parse(buf, KFW_HDR_LEN, &got));
}

/* A version from the future is refused rather than guessed at. Guessing means
 * writing an image laid out to rules this firmware does not know. */
TEST_CASE("a header version this firmware does not know is refused",
          "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.hdr_version = 2U;
    seal(buf, &h);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}

TEST_CASE("a header claiming the wrong length is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.hdr_len = KFW_HDR_LEN + 8U;
    seal(buf, &h);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}

/* A destination or a compression this firmware cannot honour. Accepting
 * either would mean deciding at write time what to do about it, which is
 * later than the operator can be told. */
TEST_CASE("a target or a compression outside the enum is refused",
          "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.target = (uint8_t)KFW_TARGET_COUNT;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));

    make_hdr(buf, &h);
    h.comp = (uint8_t)KFW_COMP_COUNT;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}

/* Reserved bits are reserved. A future flag that this firmware ignored would
 * be a container it half-understood. Bit 0 is the app-data flag, so the walk
 * starts above it; its own cases follow. */
TEST_CASE("any reserved flag set is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    for ( unsigned bit = 1U; 16U > bit; ++bit )
    {
        make_hdr(buf, &h);
        h.flags = (uint16_t)(1U << bit);
        seal(buf, &h);
        TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
    }
}

TEST_CASE("an image or payload of no bytes is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.image_len = 0U;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));

    make_hdr(buf, &h);
    h.payload_len = 0U;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}

/* An uncompressed container whose two lengths disagree is describing
 * something impossible, and the disagreement is the only warning available
 * before the bytes start arriving. */
TEST_CASE("an uncompressed image whose lengths disagree is refused",
          "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.comp        = (uint8_t)KFW_COMP_NONE;
    h.payload_len = 128U;
    h.image_len   = 256U;
    seal(buf, &h);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));

    /* Compressed, the same pair is ordinary. */
    make_hdr(buf, &h);
    h.comp        = (uint8_t)KFW_COMP_DEFLATE;
    h.payload_len = 128U;
    h.image_len   = 256U;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_hdr_parse(buf, sizeof(buf), &got));
}

/* The name is printed and logged, so it has to end. A field full of
 * characters would run whatever printed it off the end of the struct. */
TEST_CASE("a name with no terminator is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    (void)memset(h.name, 'x', sizeof(h.name));
    seal(buf, &h);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}

/* ---- What the checksum covers, and what it does not ------------------------ */

/* The CRC seals the 52 bytes before itself. That boundary is the whole point:
 * a bit flipped anywhere in the description must be caught, and the name that
 * follows is outside it by design. Both halves are asserted, because a reader
 * who assumes the CRC covers the whole header gets the second one wrong. */
TEST_CASE("the checksum covers everything before it, and nothing after",
          "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t got;

    /* Every byte inside the sealed range, one at a time. */
    for ( unsigned i = 0U; HDR_CRC_LEN > i; ++i )
    {
        make_hdr(buf, NULL);
        buf[i] ^= 0x01U;
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR,
            kfw_hdr_parse(buf, sizeof(buf), &got),
            "a flipped bit inside the sealed range was accepted");
    }

    /* And a byte of the name, which is past the checksum: still valid. */
    make_hdr(buf, NULL);
    buf[offsetof(kfw_hdr_t, name)] = 'Z';
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_hdr_parse(buf, sizeof(buf), &got));
    TEST_ASSERT_EQUAL_CHAR('Z', got.name[0]);
}

TEST_CASE("a corrupted checksum is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.crc ^= 0xFFFFFFFFU;
    (void)memcpy(buf, &h, sizeof(h));   /* not re-sealed, on purpose */

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}

TEST_CASE("a null buffer or destination is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t got;

    make_hdr(buf, NULL);

    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(NULL, KFW_HDR_LEN, &got));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, KFW_HDR_LEN, NULL));
}

/* ---- The digest and the checksum, as functions ----------------------------- */

/* The SHA-256 of the empty input, which is the one vector everybody knows.
 * It is here because the digest is what stands between a corrupted download
 * and a board that boots it. */
TEST_CASE("the digest matches the published vector for no bytes", "[kdrv_kfw]")
{
    static const uint8_t EMPTY_SHA256[KFW_SHA_LEN] = {
        0xE3U, 0xB0U, 0xC4U, 0x42U, 0x98U, 0xFCU, 0x1CU, 0x14U,
        0x9AU, 0xFBU, 0xF4U, 0xC8U, 0x99U, 0x6FU, 0xB9U, 0x24U,
        0x27U, 0xAEU, 0x41U, 0xE4U, 0x64U, 0x9BU, 0x93U, 0x4CU,
        0xA4U, 0x95U, 0x99U, 0x1BU, 0x78U, 0x52U, 0xB8U, 0x55U,
    };
    uint8_t got[KFW_SHA_LEN];

    kfw_sha256("", 0U, got);
    TEST_ASSERT_EQUAL_MEMORY(EMPTY_SHA256, got, KFW_SHA_LEN);
}

TEST_CASE("the digest of a known string matches", "[kdrv_kfw]")
{
    /* sha256("abc"), the other vector every implementation is checked on. */
    static const uint8_t ABC_SHA256[KFW_SHA_LEN] = {
        0xBAU, 0x78U, 0x16U, 0xBFU, 0x8FU, 0x01U, 0xCFU, 0xEAU,
        0x41U, 0x41U, 0x40U, 0xDEU, 0x5DU, 0xAEU, 0x22U, 0x23U,
        0xB0U, 0x03U, 0x61U, 0xA3U, 0x96U, 0x17U, 0x7AU, 0x9CU,
        0xB4U, 0x10U, 0xFFU, 0x61U, 0xF2U, 0x00U, 0x15U, 0xADU,
    };
    uint8_t got[KFW_SHA_LEN];

    kfw_sha256("abc", 3U, got);
    TEST_ASSERT_EQUAL_MEMORY(ABC_SHA256, got, KFW_SHA_LEN);
}

/* The container's checksum is **not** zlib's default, and that is deliberate:
 * the ROM routine starts its running CRC at 0xFFFFFFFF where zlib starts at
 * ~0. mkkfw.py reproduces it by passing 0xFFFFFFFF as zlib's running value,
 * and its own comment says that getting this wrong produces a file the board
 * refuses with no explanation anybody can act on.
 *
 * Both halves are asserted here. The first pins what the board computes; the
 * second is what makes the first mean something, because "it agrees with
 * zlib" is the assumption a reader arrives with and it is wrong. */
TEST_CASE("the container's checksum matches what the host tool builds",
          "[kdrv_kfw]")
{
    /* python: zlib.crc32(b"123456789", 0xFFFFFFFF) */
    TEST_ASSERT_EQUAL_HEX32(0xD202D277U, kfw_crc32("123456789", 9U));

    /* And it is not zlib's default, which is the seed a naive tool would use. */
    TEST_ASSERT_NOT_EQUAL_HEX32(0xCBF43926U, kfw_crc32("123456789", 9U));
}

/* The two CRCs in this system are different functions, and the difference is
 * the seed. khal_crc32 is the one the console's transfers use and it does
 * agree with zlib; the container's does not. Anyone reaching for "the CRC"
 * has to know there are two. */
TEST_CASE("the container's checksum is not the one transfers use",
          "[kdrv_kfw]")
{
    TEST_ASSERT_EQUAL_HEX32(0xCBF43926U,
        khal_crc32(KHAL_CRC32_INIT, "123456789", 9U));
    TEST_ASSERT_TRUE(kfw_crc32("123456789", 9U)
                     != khal_crc32(KHAL_CRC32_INIT, "123456789", 9U));
}

/* ---- The app-data flag ------------------------------------------------------ */

TEST_CASE("the app-data flag is accepted for an app", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.flags  = KFW_FLAG_APPDATA;
    h.target = (uint8_t)KFW_TARGET_APP;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kfw_hdr_parse(buf, sizeof(buf), &got));
    TEST_ASSERT_EQUAL_UINT16(KFW_FLAG_APPDATA, got.flags);
}

/* The block describes an app; an image for the OS slot or the coprocessor has
 * nowhere to put it, and a firmware that ignored it would half-understand. */
TEST_CASE("the app-data flag is refused for os and c5", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.flags  = KFW_FLAG_APPDATA;
    h.target = (uint8_t)KFW_TARGET_OS;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));

    make_hdr(buf, &h);
    h.flags  = KFW_FLAG_APPDATA;
    h.target = (uint8_t)KFW_TARGET_C5;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}

TEST_CASE("an unknown flag bit is refused", "[kdrv_kfw]")
{
    uint8_t   buf[KFW_HDR_LEN];
    kfw_hdr_t h;
    kfw_hdr_t got;

    make_hdr(buf, &h);
    h.flags  = 0x0002U;
    h.target = (uint8_t)KFW_TARGET_APP;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));

    /* The known bit does not make an unknown one acceptable beside it. */
    make_hdr(buf, &h);
    h.flags  = (uint16_t)(KFW_FLAG_APPDATA | 0x0002U);
    h.target = (uint8_t)KFW_TARGET_APP;
    seal(buf, &h);
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kfw_hdr_parse(buf, sizeof(buf), &got));
}
