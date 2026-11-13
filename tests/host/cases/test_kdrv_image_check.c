/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kdrv_image_check.c
 * @brief An image judged as its bytes arrive: the chain against the file, then the SHA-256 it carries.
 */
#include <string.h>

#include "esp_image_format.h"
#include "unity.h"

#include "fakes.h"
#include "kdrv_image_check.h"
#include "kdrv_kfw.h"

/** @brief The most bytes a case's image takes: header, sixteen segments, pad, hash. */
#define IMG_MAX       512U

/** @brief Bytes of the first segment's data. */
#define SEG0_LEN      20U

/** @brief Bytes of the second segment's data. */
#define SEG1_LEN      7U

/**
 * @brief Build an image of two segments in @p img.
 *
 * Header, a segment of SEG0_LEN bytes, a segment of SEG1_LEN bytes, the
 * checksum byte padding the image to 16 bytes, and with @p hash its SHA-256.
 *
 * @param img  At least IMG_MAX bytes.
 * @param hash Whether the image carries its hash.
 * @return The image's length.
 */
static uint32_t image_make(uint8_t *img, bool hash)
{
    esp_image_header_t         header;
    esp_image_segment_header_t seg;
    uint32_t                   off = 0U;

    (void)memset(img, 0xA5, IMG_MAX);
    (void)memset(&header, 0, sizeof(header));
    header.magic         = ESP_IMAGE_HEADER_MAGIC;
    header.segment_count = 2U;
    header.hash_appended = hash ? 1U : 0U;
    (void)memcpy(&img[off], &header, sizeof(header));
    off += (uint32_t)sizeof(header);
    (void)memset(&seg, 0, sizeof(seg));
    seg.data_len = SEG0_LEN;
    (void)memcpy(&img[off], &seg, sizeof(seg));
    off += (uint32_t)sizeof(seg) + SEG0_LEN;
    seg.data_len = SEG1_LEN;
    (void)memcpy(&img[off], &seg, sizeof(seg));
    off += (uint32_t)sizeof(seg) + SEG1_LEN;
    off  = (off | 15U) + 1U;                    /* the checksum byte pads it out */
    if ( hash )
    {
        kfw_sha256(img, off, &img[off]);
        off += 32U;
    }
    return off;
}

/** @brief Feed @p len bytes in pieces of @p step and say whether every piece was taken. */
static bool feed_in(const uint8_t *img, uint32_t len, uint32_t step)
{
    for ( uint32_t at = 0U; at < len; at += step )
    {
        uint32_t n = len - at;
        if ( step < n )
        {
            n = step;
        }
        if ( kdrv_image_check_feed(&img[at], n) != KODE_OK )
        {
            return false;
        }
    }
    return true;
}

TEST_CASE("an image whose chain closes and whose hash matches passes",
          "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len, len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());
}

TEST_CASE("the same image fed a byte at a time passes", "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());
}

TEST_CASE("an image with no hash passes on its chain alone", "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len, 5U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());
}

TEST_CASE("a flipped byte in a segment fails on the hash", "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    img[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + 3U] ^= 0x01U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_FALSE_MESSAGE(feed_in(img, len, len),
        "the hash that does not match was taken");
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_image_check_end());
}

TEST_CASE("a file cut inside a segment fails at the end", "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, 40U, 40U));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR, kdrv_image_check_end(),
        "a file that ends inside its image was called whole");
}

TEST_CASE("a file cut before its hash fails at the end", "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len - 1U, len - 1U));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_image_check_end());
}

TEST_CASE("a segment that leaves the file is refused as soon as its header is read",
          "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    /* The file is shorter than the first segment says it is. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len - 70U));
    TEST_ASSERT_FALSE_MESSAGE(feed_in(img, len, len),
        "a segment that runs past the file's length was waited for");
    kdrv_image_check_abort();
}

TEST_CASE("a bad magic is refused at the first byte", "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    img[0] = 0x00U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_FALSE(feed_in(img, len, len));
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_image_check_end());
}

TEST_CASE("bytes after the image are taken and ignored", "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len + 16U));
    TEST_ASSERT_TRUE(feed_in(img, len + 16U, 16U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());
}

TEST_CASE("one check at a time, and a spent one can be followed by another",
          "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_make(img, false);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_image_check_begin(len));
    kdrv_image_check_abort();
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kdrv_image_check_feed(img, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len, len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());
}

/**
 * @brief Build an image of @p segs segments of @p data bytes each in @p img.
 *
 * @param img  At least IMG_MAX bytes.
 * @param segs How many segments the header says there are.
 * @param data Bytes of each segment's data.
 * @param hash Whether the image carries its hash.
 * @return The image's length.
 */
static uint32_t image_segs(uint8_t *img, uint8_t segs, uint32_t data, bool hash)
{
    esp_image_header_t         header;
    esp_image_segment_header_t seg;
    uint32_t                   off = 0U;

    (void)memset(img, 0xA5, IMG_MAX);
    (void)memset(&header, 0, sizeof(header));
    header.magic         = ESP_IMAGE_HEADER_MAGIC;
    header.segment_count = segs;
    header.hash_appended = hash ? 1U : 0U;
    (void)memcpy(&img[off], &header, sizeof(header));
    off += (uint32_t)sizeof(header);
    for ( uint8_t i = 0U; segs > i; ++i )
    {
        (void)memset(&seg, 0, sizeof(seg));
        seg.data_len = data;
        (void)memcpy(&img[off], &seg, sizeof(seg));
        off += (uint32_t)sizeof(seg) + data;
    }
    off = (off | 15U) + 1U;
    if ( hash )
    {
        kfw_sha256(img, off, &img[off]);
        off += 32U;
    }
    return off;
}

/* The checksum byte pads the image out to 16 bytes and is itself part of the
 * padding: an image whose segments end on a boundary takes a whole block more.
 * The walk of the region reads it so, and so does this. */
TEST_CASE("an image that ends its segments on a 16-byte boundary takes a whole block of padding",
          "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_segs(img, 1U, 16U, true);   /* 24 + 8 + 16 = 48, then 16 more */

    TEST_ASSERT_EQUAL_UINT32(48U + 16U + 32U, len);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len, 7U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());

    uint32_t const bare = image_segs(img, 1U, 16U, false);   /* the same, no hash: 64 bytes */

    TEST_ASSERT_EQUAL_UINT32(64U, bare);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(bare));
    TEST_ASSERT_TRUE(feed_in(img, bare - 1U, bare - 1U));       /* the block's last byte is missing */
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_ERROR, kdrv_image_check_end(),
        "an image without its whole checksum block was called whole");
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(bare));
    TEST_ASSERT_TRUE(feed_in(img, 48U, 48U));                   /* and with none of it */
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_image_check_end());
}

TEST_CASE("sixteen segments pass and a seventeenth is refused at the header",
          "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_segs(img, 16U, 4U, true);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len, 13U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());

    (void)image_segs(img, 17U, 4U, false);
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(IMG_MAX));
    TEST_ASSERT_FALSE_MESSAGE(feed_in(img, sizeof(esp_image_header_t), sizeof(esp_image_header_t)),
        "a header of seventeen segments was taken");
    kdrv_image_check_abort();
}

/* What is and is not caught: an image with no hash is judged by its chain
 * alone, so a flipped byte in a segment's data goes through. The checksum
 * byte is not read either; the verify that follows the write is what holds
 * those. */
TEST_CASE("an image with no hash passes with a flipped byte in its data: only the chain is checked",
          "[kdrv_image_check]")
{
    uint8_t        img[IMG_MAX];
    uint32_t const len = image_segs(img, 2U, SEG0_LEN, false);

    img[sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + 3U] ^= 0x01U;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(len));
    TEST_ASSERT_TRUE(feed_in(img, len, len));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());
}

/* A length that would wrap the running offset is judged by the room left, not
 * by a sum: the segment is refused when its header is read. */
TEST_CASE("a segment of 0xFFFFFFFF bytes is refused at its header", "[kdrv_image_check]")
{
    uint8_t                    img[IMG_MAX];
    esp_image_segment_header_t seg;

    (void)image_segs(img, 2U, SEG0_LEN, false);
    (void)memset(&seg, 0, sizeof(seg));
    seg.data_len = UINT32_MAX;
    (void)memcpy(&img[sizeof(esp_image_header_t)], &seg, sizeof(seg));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(UINT32_MAX));
    TEST_ASSERT_FALSE_MESSAGE(
        feed_in(img, sizeof(esp_image_header_t) + sizeof(seg), 4U),
        "a segment longer than any file was waited for");
    TEST_ASSERT_EQUAL_INT(KODE_ERROR, kdrv_image_check_end());
}

TEST_CASE("a segment of no bytes between two others passes", "[kdrv_image_check]")
{
    uint8_t                    img[IMG_MAX];
    esp_image_header_t         header;
    esp_image_segment_header_t seg;
    uint32_t                   off = 0U;

    (void)memset(img, 0xA5, sizeof(img));
    (void)memset(&header, 0, sizeof(header));
    header.magic         = ESP_IMAGE_HEADER_MAGIC;
    header.segment_count = 3U;
    header.hash_appended = 1U;
    (void)memcpy(&img[off], &header, sizeof(header));
    off += (uint32_t)sizeof(header);
    for ( uint8_t i = 0U; 3U > i; ++i )
    {
        (void)memset(&seg, 0, sizeof(seg));
        if ( 1U != i )
        {
            seg.data_len = SEG1_LEN;
        }
        (void)memcpy(&img[off], &seg, sizeof(seg));
        off += (uint32_t)sizeof(seg) + seg.data_len;
    }
    off = (off | 15U) + 1U;
    kfw_sha256(img, off, &img[off]);
    off += 32U;

    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_begin(off));
    TEST_ASSERT_TRUE(feed_in(img, off, 3U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kdrv_image_check_end());
}
