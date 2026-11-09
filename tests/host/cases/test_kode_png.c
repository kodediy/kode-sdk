/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_png.c
 * @brief The PNG reader a draw call runs, and the blends that put pictures and glyphs on a canvas.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kode.h"
#include "miniz.h"
#include "kode_screen_priv.h"

/** @brief Bytes of the PNG a case builds. */
#define PNG_BYTES 16384U

/** @brief Bytes of the scanlines a case builds. */
#define RAW_BYTES 4096U

/** @brief The side of the picture the colour cases decode. */
#define SIDE 5U

/** @brief Bytes of a failure message. */
#define MSG_BYTES 96U

/** @brief Bytes of a PNG signature. */
#define SIG_BYTES 8U

/** @brief Bytes of a chunk around its data: length, type, checksum. */
#define CHUNK_FRAME 12U

/** @brief Bytes of the header chunk's data. */
#define IHDR_BYTES 13U

/** @brief The filters a PNG row can carry. */
#define F_NONE  0U
#define F_SUB   1U
#define F_UP    2U
#define F_AVG   3U
#define F_PAETH 4U

/** @brief How many filters there are. */
#define FILTERS 5U

/** @brief The PNG signature. */
static const uint8_t SIGNATURE[SIG_BYTES] = { 0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU };

/** @brief A real 2 x 2 RGBA PNG (zlib's own bytes), for the header and the walk. */
static const uint8_t REAL_PNG[] = {
    0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72, 0xB6, 0x0D,
    0x24, 0x00, 0x00, 0x00, 0x13, 0x49, 0x44, 0x41, 0x54, 0x78, 0xDA, 0x63, 0xF8, 0xCF, 0xC0, 0xF0,
    0x1F, 0x0C, 0x81, 0x34, 0x08, 0x34, 0x00, 0x00, 0x49, 0x49, 0x09, 0x78, 0x9C, 0x51, 0x17, 0x92,
    0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE, 0x42, 0x60, 0x82,
};

/** @brief The same picture as it inflates: filter byte 0 and the pixels. */
static const uint8_t REAL_RAW[] = {
    0x00, 0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF,
    0xFF, 0x80,
};

/** @brief Its pixels. */
static const uint8_t REAL_RGBA[] = {
    0xFF, 0x00, 0x00, 0xFF, 0x00, 0xFF, 0x00, 0xFF,
    0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x80,
};

/** @brief The PNG and the scanlines a case builds. */
static uint8_t s_png[PNG_BYTES];
static uint8_t s_raw[RAW_BYTES];

/** @brief Bytes of s_png in use. */
static uint32_t s_png_n;

/** @brief Put a 32-bit number, most significant byte first. */
static void be32(uint8_t *to, uint32_t v)
{
    to[0] = (uint8_t)(v >> 24U);
    to[1] = (uint8_t)(v >> 16U);
    to[2] = (uint8_t)(v >> 8U);
    to[3] = (uint8_t)v;
}

/** @brief Append a chunk to s_png; its checksum is zeros, which the reader does not read. */
static void chunk_put(const char *type, const uint8_t *data, uint32_t len)
{
    be32(&s_png[s_png_n], len);
    (void)memcpy(&s_png[s_png_n + 4U], type, 4U);
    if ( 0U < len )
    {
        (void)memcpy(&s_png[s_png_n + 8U], data, len);
    }
    (void)memset(&s_png[s_png_n + 8U + len], 0, 4U);
    s_png_n += CHUNK_FRAME + len;
}

/** @brief Start s_png with the signature and a header. */
static void png_start(uint32_t w, uint32_t h, uint8_t depth, uint8_t ctype, uint8_t interlace)
{
    uint8_t ihdr[IHDR_BYTES];

    (void)memcpy(s_png, SIGNATURE, SIG_BYTES);
    s_png_n = SIG_BYTES;
    be32(&ihdr[0], w);
    be32(&ihdr[4], h);
    ihdr[8]  = depth;
    ihdr[9]  = ctype;
    ihdr[10] = 0U;
    ihdr[11] = 0U;
    ihdr[12] = interlace;
    chunk_put("IHDR", ihdr, IHDR_BYTES);
}

/** @brief Finish s_png with two data chunks (what they hold is not read) and the end. */
static void png_end(void)
{
    static const uint8_t DATA[] = { 'a', 'b', 'c', 'd', 'e', 'f' };

    chunk_put("IDAT", DATA, 4U);
    chunk_put("IDAT", &DATA[4], 2U);
    chunk_put("IEND", NULL, 0U);
}

/** @brief The bytes a pixel takes, at least one: the filters work in these. */
static uint32_t filter_bpp(uint32_t spp, uint32_t depth)
{
    uint32_t bpp = (spp * depth) / 8U;

    if ( 0U == bpp )
    {
        bpp = 1U;
    }
    return bpp;
}

/** @brief How far apart two numbers are. */
static int32_t dist(int32_t a, int32_t b)
{
    int32_t d = a - b;

    if ( 0 > d )
    {
        d = -d;
    }
    return d;
}

/** @brief The Paeth predictor of the PNG specification. */
static uint32_t paeth(int32_t a, int32_t b, int32_t c)
{
    const int32_t p  = (a + b) - c;
    const int32_t pa = dist(p, a);
    const int32_t pb = dist(p, b);
    const int32_t pc = dist(p, c);
    int32_t       pick = c;

    if ( (pa <= pb) && (pa <= pc) )
    {
        pick = a;
    }
    else if ( pb <= pc )
    {
        pick = b;
    }
    else
    {
        pick = c;
    }
    return (uint32_t)pick;
}

/**
 * @brief Filter row @p row of the plain scanlines @p plain into @p out with filter @p f.
 * @param out   Receives the filter byte and the row.
 * @param plain All the rows, without filter bytes, @p stride apart.
 * @param row   Which row.
 * @param stride Bytes of a row.
 * @param bpp   Bytes of a pixel for the filters.
 * @param f     F_*.
 */
static void filter_row(uint8_t *out, const uint8_t *plain, uint32_t row, uint32_t stride,
                       uint32_t bpp, uint32_t f)
{
    const uint8_t *cur  = &plain[row * stride];
    const uint8_t *prev = NULL;

    if ( 0U < row )
    {
        prev = &plain[(row - 1U) * stride];
    }
    out[0] = (uint8_t)f;
    for ( uint32_t i = 0U; i < stride; i++ )
    {
        uint32_t a = 0U;
        uint32_t b = 0U;
        uint32_t c = 0U;
        uint32_t pred = 0U;

        if ( i >= bpp )
        {
            a = cur[i - bpp];
        }
        if ( NULL != prev )
        {
            b = prev[i];
        }
        if ( (NULL != prev) && (i >= bpp) )
        {
            c = prev[i - bpp];
        }

        if ( F_SUB == f )
        {
            pred = a;
        }
        else if ( F_UP == f )
        {
            pred = b;
        }
        else if ( F_AVG == f )
        {
            pred = (a + b) / 2U;
        }
        else if ( F_PAETH == f )
        {
            pred = paeth((int32_t)a, (int32_t)b, (int32_t)c);
        }
        else
        {
            pred = 0U;
        }
        out[1U + i] = (uint8_t)(cur[i] - pred);
    }
}

/* ---- The colour cases ------------------------------------------------------ */

/** @brief A colour type and a depth that go together. */
typedef struct {
    uint8_t ctype;  /**< Colour type.        */
    uint8_t depth;  /**< Bits per sample.    */
} combo_t;

static const combo_t COMBOS[] = {
    { 0U, 1U }, { 0U, 2U }, { 0U, 4U }, { 0U, 8U }, { 0U, 16U },
    { 2U, 8U }, { 2U, 16U },
    { 3U, 1U }, { 3U, 2U }, { 3U, 4U }, { 3U, 8U },
    { 4U, 8U }, { 4U, 16U },
    { 6U, 8U }, { 6U, 16U },
};

/** @brief Samples a pixel of a colour type holds. */
static uint32_t spp_of(uint8_t ctype)
{
    uint32_t n = 1U;

    if ( 2U == ctype )
    {
        n = 3U;
    }
    else if ( 4U == ctype )
    {
        n = 2U;
    }
    else if ( 6U == ctype )
    {
        n = 4U;
    }
    else
    {
        n = 1U;
    }
    return n;
}

/** @brief Entries of the palette the cases use. */
#define PAL_N 16U

/** @brief The palette's colour i. */
static void pal_rgb(uint32_t i, uint8_t *rgb)
{
    rgb[0] = (uint8_t)((i * 16U) + 1U);
    rgb[1] = (uint8_t)((i * 7U) + 3U);
    rgb[2] = (uint8_t)(255U - (i * 9U));
}

/** @brief The transparency of its first three entries. */
static const uint8_t TRNS[3] = { 0U, 64U, 128U };

/** @brief A sample's value for the picture of the cases: the largest it can be, and below. */
static uint32_t sample_of(uint32_t row, uint32_t col, uint32_t k, uint32_t depth)
{
    const uint32_t seed = (row * 37U) + (col * 11U) + (k * 53U) + 7U;

    if ( 16U == depth )
    {
        return ((seed * 257U) ^ 0x0F0FU) & 0xFFFFU;
    }
    return seed & ((1U << depth) - 1U);
}

/** @brief Build the scanlines (filtered) and the pixels they decode to, for a combo. */
static void colour_picture(const combo_t *cb, uint8_t *expect)
{
    const uint32_t spp    = spp_of(cb->ctype);
    const uint32_t depth  = cb->depth;
    const uint32_t stride = ((SIDE * spp * depth) + 7U) / 8U;
    uint8_t        plain[RAW_BYTES];
    uint32_t       top = 255U;

    if ( 8U > depth )
    {
        top = (1U << depth) - 1U;
    }

    (void)memset(plain, 0, sizeof(plain));
    for ( uint32_t row = 0U; SIDE > row; row++ )
    {
        for ( uint32_t col = 0U; SIDE > col; col++ )
        {
            uint32_t hi[4] = { 0U, 0U, 0U, 255U };

            for ( uint32_t k = 0U; k < spp; k++ )
            {
                uint32_t v = sample_of(row, col, k, depth);

                if ( 3U == cb->ctype )
                {
                    v %= PAL_N;
                }
                const uint32_t idx = (col * spp) + k;

                if ( 16U == depth )
                {
                    plain[(row * stride) + (idx * 2U)]      = (uint8_t)(v >> 8U);
                    plain[(row * stride) + (idx * 2U) + 1U] = (uint8_t)v;
                    hi[k] = v >> 8U;
                }
                else if ( 8U == depth )
                {
                    plain[(row * stride) + idx] = (uint8_t)v;
                    hi[k] = v;
                }
                else
                {
                    const uint32_t bit = idx * depth;

                    plain[(row * stride) + (bit / 8U)] |= (uint8_t)(v << (8U - depth - (bit % 8U)));
                    hi[k] = v;
                }
            }
            uint8_t *px = &expect[((row * SIDE) + col) * 4U];

            if ( 0U == cb->ctype )
            {
                px[0] = (uint8_t)((hi[0] * 255U) / top);
                px[1] = px[0];
                px[2] = px[0];
                px[3] = 255U;
            }
            else if ( 4U == cb->ctype )
            {
                px[0] = (uint8_t)hi[0];
                px[1] = px[0];
                px[2] = px[0];
                px[3] = (uint8_t)hi[1];
            }
            else if ( 3U == cb->ctype )
            {
                pal_rgb(hi[0], px);
                px[3] = 255U;
                if ( 3U > hi[0] )
                {
                    px[3] = TRNS[hi[0]];
                }
            }
            else
            {
                px[0] = (uint8_t)hi[0];
                px[1] = (uint8_t)hi[1];
                px[2] = (uint8_t)hi[2];
                px[3] = 255U;
                if ( 6U == cb->ctype )
                {
                    px[3] = (uint8_t)hi[3];
                }
            }
        }
    }
    for ( uint32_t row = 0U; SIDE > row; row++ )
    {
        filter_row(&s_raw[row * (stride + 1U)], plain, row, stride, filter_bpp(spp, depth),
                   row % FILTERS);
    }
}

/** @brief Build the PNG of a combo (palette and transparency in when it has them). */
static uint32_t colour_png(const combo_t *cb)
{
    png_start(SIDE, SIDE, cb->depth, cb->ctype, 0U);
    if ( 3U == cb->ctype )
    {
        uint8_t pal[PAL_N * 3U];

        for ( uint32_t i = 0U; PAL_N > i; i++ )
        {
            pal_rgb(i, &pal[i * 3U]);
        }
        chunk_put("PLTE", pal, PAL_N * 3U);
        chunk_put("tRNS", TRNS, sizeof(TRNS));
    }
    png_end();
    return ((SIDE * spp_of(cb->ctype) * cb->depth + 7U) / 8U + 1U) * SIDE;
}

/** @brief Pixels kept or freed: what a case must leave as it found. */
static int32_t held_now(void)
{
    return (int32_t)fake_psram_alloc_count() - (int32_t)fake_psram_free_count();
}

TEST_CASE("every colour type and depth decodes to its exact pixels through every filter", "[kode_png]")
{
    for ( uint32_t i = 0U; (uint32_t)(sizeof(COMBOS) / sizeof(COMBOS[0])) > i; i++ )
    {
        uint8_t     expect[SIDE * SIDE * 4U];
        kode_png_t  png = { 0U, 0U, NULL };
        char        msg[MSG_BYTES];
        const int32_t before = held_now();

        (void)snprintf(msg, sizeof(msg), "colour type %" PRIu32 " depth %" PRIu32,
                       (uint32_t)COMBOS[i].ctype, (uint32_t)COMBOS[i].depth);
        fake_tinfl_reset();
        colour_picture(&COMBOS[i], expect);
        const uint32_t raw_n = colour_png(&COMBOS[i]);

        fake_tinfl_set_payload(s_raw, raw_n);
        TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_PNG_OK, kode_png_decode(s_png, s_png_n, &png), msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(SIDE, png.w, msg);
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(SIDE, png.h, msg);
        TEST_ASSERT_NOT_NULL_MESSAGE(png.rgba, msg);
        TEST_ASSERT_EQUAL_HEX8_ARRAY_MESSAGE(expect, png.rgba, sizeof(expect), msg);
        kode_png_free(&png);
        TEST_ASSERT_EQUAL_INT_MESSAGE(before, held_now(), msg);
    }
    fake_tinfl_reset();
}

TEST_CASE("a real PNG's header is read and its data chunks walked", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };
    uint32_t   w   = 7U;
    uint32_t   h   = 7U;

    fake_tinfl_reset();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_size(REAL_PNG, sizeof(REAL_PNG), &w, &h));
    TEST_ASSERT_EQUAL_UINT32(2U, w);
    TEST_ASSERT_EQUAL_UINT32(2U, h);

    fake_tinfl_set_payload(REAL_RAW, sizeof(REAL_RAW));
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_decode(REAL_PNG, sizeof(REAL_PNG), &png));
    TEST_ASSERT_EQUAL_HEX8_ARRAY(REAL_RGBA, png.rgba, sizeof(REAL_RGBA));
    /* The reader hands the inflater the one data chunk's bytes, zlib header first. */
    TEST_ASSERT_EQUAL_UINT32(0x13U, (uint32_t)fake_tinfl_in_len());
    TEST_ASSERT_EQUAL_HEX8(0x78U, fake_tinfl_in()[0]);
    TEST_ASSERT_EQUAL_HEX8(0xDAU, fake_tinfl_in()[1]);
    kode_png_free(&png);
    fake_tinfl_reset();
}

TEST_CASE("several data chunks reach the inflater as one stream", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    fake_tinfl_reset();
    png_start(1U, 1U, 8U, 0U, 0U);
    png_end();
    s_raw[0] = 0U;
    s_raw[1] = 0x40U;
    fake_tinfl_set_payload(s_raw, 2U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_decode(s_png, s_png_n, &png));
    TEST_ASSERT_EQUAL_UINT32(6U, (uint32_t)fake_tinfl_in_len());
    TEST_ASSERT_EQUAL_MEMORY("abcdef", fake_tinfl_in(), 6U);
    TEST_ASSERT_EQUAL_HEX8(0x40U, png.rgba[0]);
    TEST_ASSERT_EQUAL_HEX8(0xFFU, png.rgba[3]);
    kode_png_free(&png);
    fake_tinfl_reset();
}

/* ---- What is not a PNG, and what is too big -------------------------------- */

TEST_CASE("bytes that are not a PNG are refused by name", "[kode_png]")
{
    static const uint8_t JPEG[] = { 0xFFU, 0xD8U, 0xFFU, 0xE0U, 0x00U, 0x10U, 0x4AU, 0x46U, 0x49U, 0x46U };
    static const uint8_t WORDS[] = "this is a text file, not a picture, at all.";
    kode_png_t           png = { 0U, 0U, NULL };

    TEST_ASSERT_EQUAL_INT(KODE_PNG_NOT_PNG, kode_png_decode(NULL, 0U, &png));
    TEST_ASSERT_EQUAL_INT(KODE_PNG_NOT_PNG, kode_png_decode(JPEG, sizeof(JPEG), &png));
    TEST_ASSERT_EQUAL_INT(KODE_PNG_NOT_PNG, kode_png_decode(WORDS, sizeof(WORDS), &png));
    TEST_ASSERT_EQUAL_INT(KODE_PNG_NOT_PNG, kode_png_decode(SIGNATURE, SIG_BYTES - 1U, &png));
    /* One byte off the signature. */
    (void)memcpy(s_png, REAL_PNG, sizeof(REAL_PNG));
    s_png[1] = (uint8_t)'Q';
    TEST_ASSERT_EQUAL_INT(KODE_PNG_NOT_PNG, kode_png_decode(s_png, sizeof(REAL_PNG), &png));
    TEST_ASSERT_NULL(png.rgba);
}

TEST_CASE("a picture wider or taller than the glass is refused before anything is allocated", "[kode_png]")
{
    static const uint32_t SIDES[][2] = {
        { 2000U, 40U }, { 411U, 1U }, { 1U, 503U }, { 0xFFFFFFFFU, 1U }, { 1U, 0xFFFFFFFFU },
        { 0x80000000U, 0x80000000U },
    };

    for ( uint32_t i = 0U; (uint32_t)(sizeof(SIDES) / sizeof(SIDES[0])) > i; i++ )
    {
        kode_png_t    png = { 0U, 0U, NULL };
        uint32_t      w   = 0U;
        uint32_t      h   = 0U;
        const uint32_t allocs = fake_psram_alloc_count();

        png_start(SIDES[i][0], SIDES[i][1], 8U, 6U, 0U);
        png_end();
        TEST_ASSERT_EQUAL_INT(KODE_PNG_TOO_BIG, kode_png_size(s_png, s_png_n, &w, &h));
        TEST_ASSERT_EQUAL_UINT32(SIDES[i][0], w);
        TEST_ASSERT_EQUAL_UINT32(SIDES[i][1], h);
        TEST_ASSERT_EQUAL_INT(KODE_PNG_TOO_BIG, kode_png_decode(s_png, s_png_n, &png));
        TEST_ASSERT_EQUAL_UINT32(SIDES[i][0], png.w);
        TEST_ASSERT_EQUAL_UINT32(SIDES[i][1], png.h);
        TEST_ASSERT_NULL(png.rgba);
        TEST_ASSERT_EQUAL_UINT32(allocs, fake_psram_alloc_count());
    }
    /* Only the header: the size is known before the rest is looked at. */
    png_start(2000U, 40U, 8U, 6U, 0U);
    kode_png_t png = { 0U, 0U, NULL };

    TEST_ASSERT_EQUAL_INT(KODE_PNG_TOO_BIG, kode_png_decode(s_png, s_png_n, &png));
}

TEST_CASE("the glass itself is not too big", "[kode_png]")
{
    uint32_t w = 0U;
    uint32_t h = 0U;

    png_start(KODE_CANVAS_WIDTH_MAX, KODE_CANVAS_HEIGHT_MAX, 8U, 6U, 0U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_size(s_png, s_png_n, &w, &h));
    TEST_ASSERT_EQUAL_UINT32(KODE_CANVAS_WIDTH_MAX, w);
    TEST_ASSERT_EQUAL_UINT32(KODE_CANVAS_HEIGHT_MAX, h);
}

/* ---- What is damaged -------------------------------------------------------- */

/** @brief A decode of the first @p n bytes of s_png in a block of exactly that size. */
static kode_png_result_t decode_prefix(uint32_t n, kode_png_t *png)
{
    kode_png_result_t r;

    if ( 0U == n )
    {
        r = kode_png_decode(NULL, 0U, png);
    }
    else
    {
        /* Exactly n bytes: a read past them is what a checker flags. */
        uint8_t *exact = (uint8_t *)malloc(n);

        TEST_ASSERT_NOT_NULL(exact);
        (void)memcpy(exact, s_png, n);
        r = kode_png_decode(exact, n, png);
        free(exact);
    }
    return r;
}

TEST_CASE("a PNG cut anywhere never decodes and never reads past its end", "[kode_png]")
{
    const int32_t before = held_now();

    fake_tinfl_reset();
    (void)memcpy(s_png, REAL_PNG, sizeof(REAL_PNG));
    fake_tinfl_set_payload(REAL_RAW, sizeof(REAL_RAW));
    for ( uint32_t n = 0U; (uint32_t)sizeof(REAL_PNG) > n; n++ )
    {
        kode_png_t        png = { 0U, 0U, NULL };
        const kode_png_result_t r = decode_prefix(n, &png);
        char              msg[MSG_BYTES];

        (void)snprintf(msg, sizeof(msg), "cut at %" PRIu32, n);
        TEST_ASSERT_TRUE_MESSAGE((KODE_PNG_NOT_PNG == r) || (KODE_PNG_DAMAGED == r), msg);
        TEST_ASSERT_NULL_MESSAGE(png.rgba, msg);
    }
    TEST_ASSERT_EQUAL_INT(before, held_now());
    fake_tinfl_reset();
}

TEST_CASE("a chunk that claims more than the file holds is damage", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    fake_tinfl_reset();
    png_start(2U, 2U, 8U, 6U, 0U);
    chunk_put("IDAT", (const uint8_t *)"abcd", 4U);
    /* The data chunk's length field says four gigabytes. */
    be32(&s_png[SIG_BYTES + CHUNK_FRAME + IHDR_BYTES], 0xFFFFFFFFU);
    chunk_put("IEND", NULL, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    be32(&s_png[SIG_BYTES + CHUNK_FRAME + IHDR_BYTES], 0xFFFFFFF8U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    TEST_ASSERT_NULL(png.rgba);
}

TEST_CASE("a PNG with no data, no end or a bad header is damage", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    fake_tinfl_reset();
    /* No data chunk. */
    png_start(2U, 2U, 8U, 6U, 0U);
    chunk_put("IEND", NULL, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    /* No end. */
    png_start(2U, 2U, 8U, 6U, 0U);
    chunk_put("IDAT", (const uint8_t *)"abcd", 4U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    /* A side of zero. */
    png_start(0U, 2U, 8U, 6U, 0U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    png_start(2U, 0U, 8U, 6U, 0U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    /* A depth the colour type does not allow, and one nobody does. */
    png_start(2U, 2U, 4U, 6U, 0U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    png_start(2U, 2U, 3U, 0U, 0U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    png_start(2U, 2U, 16U, 3U, 0U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    /* A colour type that does not exist. */
    png_start(2U, 2U, 8U, 5U, 0U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    /* A header chunk of the wrong length. */
    png_start(2U, 2U, 8U, 6U, 0U);
    be32(&s_png[SIG_BYTES], IHDR_BYTES - 1U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    TEST_ASSERT_NULL(png.rgba);
}

TEST_CASE("a palette picture without its palette, or with an index past it, is damage", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    fake_tinfl_reset();
    png_start(2U, 1U, 8U, 3U, 0U);
    png_end();
    s_raw[0] = 0U;
    s_raw[1] = 0U;
    s_raw[2] = 0U;
    fake_tinfl_set_payload(s_raw, 3U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));

    /* Two colours in the palette, and a pixel that names the third. */
    png_start(2U, 1U, 8U, 3U, 0U);
    chunk_put("PLTE", (const uint8_t *)"\x01\x02\x03\x04\x05\x06", 6U);
    png_end();
    s_raw[1] = 1U;
    s_raw[2] = 2U;
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    s_raw[2] = 1U;
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_decode(s_png, s_png_n, &png));
    TEST_ASSERT_EQUAL_HEX8(0x04U, png.rgba[0]);
    TEST_ASSERT_EQUAL_HEX8(0x04U, png.rgba[4]);
    kode_png_free(&png);

    /* A palette whose length is no multiple of three. */
    png_start(2U, 1U, 8U, 3U, 0U);
    chunk_put("PLTE", (const uint8_t *)"\x01\x02\x03\x04", 4U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    fake_tinfl_reset();
}

/** @brief The cap's terms: a flush allowance per row, slack, and the inflated size counted twice. */
#define CAP_ROW_ALLOWANCE 16U
#define CAP_SLACK         64U
#define CAP_RAW_FACTOR    2U

/** @brief The cap of a picture of @p raw inflated bytes in @p rows rows. */
#define CAP_OF(raw, rows) ((CAP_RAW_FACTOR * (raw)) + ((rows) * CAP_ROW_ALLOWANCE) + CAP_SLACK)

/** @brief A 2 x 2 RGBA picture inflates to 18 bytes in 2 rows. */
#define CAP_2X2_RAW  18U
#define CAP_2X2_ROWS 2U
#define CAP_2X2      CAP_OF(CAP_2X2_RAW, CAP_2X2_ROWS)

/** @brief A 1 x 502 gray picture that an encoder flushed after each row: a filter byte and a pixel per row. */
#define FLUSH_ROWS      502U
#define FLUSH_ROW_BYTES 2U
#define FLUSH_RAW       (FLUSH_ROWS * FLUSH_ROW_BYTES)
#define FLUSH_CAP       CAP_OF(FLUSH_RAW, FLUSH_ROWS)

/** @brief What libpng's png_set_flush(1) makes of those rows: 4024 bytes of stream. */
#define FLUSH_STREAM 4024U

/** @brief A stream's worth of data chunk, content unread by the stand-in inflater. */
static uint8_t s_zeros[FLUSH_CAP + 1U];

TEST_CASE("data longer than a picture can need is refused before a copy is made", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    fake_tinfl_reset();
    (void)memset(s_raw, 0, sizeof(s_raw));
    fake_tinfl_set_payload(s_raw, CAP_2X2_RAW);
    png_start(2U, 2U, 8U, 6U, 0U);
    chunk_put("IDAT", s_zeros, CAP_2X2 + 1U);
    chunk_put("IEND", NULL, 0U);
    const uint32_t allocs = fake_psram_alloc_count();

    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    TEST_ASSERT_EQUAL_UINT32(allocs, fake_psram_alloc_count());
    png_start(2U, 2U, 8U, 6U, 0U);
    chunk_put("IDAT", s_zeros, CAP_2X2);
    chunk_put("IEND", NULL, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_decode(s_png, s_png_n, &png));
    kode_png_free(&png);
    fake_tinfl_reset();
}

TEST_CASE("a stream flushed after every row is within the cap", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    fake_tinfl_reset();
    (void)memset(s_raw, 0, sizeof(s_raw));
    fake_tinfl_set_payload(s_raw, FLUSH_RAW);
    png_start(1U, FLUSH_ROWS, 8U, 0U, 0U);
    chunk_put("IDAT", s_zeros, FLUSH_STREAM);
    chunk_put("IEND", NULL, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_decode(s_png, s_png_n, &png));
    kode_png_free(&png);
    png_start(1U, FLUSH_ROWS, 8U, 0U, 0U);
    chunk_put("IDAT", s_zeros, FLUSH_CAP + 1U);
    chunk_put("IEND", NULL, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    fake_tinfl_reset();
}

TEST_CASE("a decode starts from an empty picture whatever it was handed", "[kode_png]")
{
    kode_png_t png = { 9U, 9U, NULL };

    TEST_ASSERT_EQUAL_INT(KODE_PNG_NOT_PNG, kode_png_decode(SIGNATURE, SIG_BYTES - 1U, &png));
    TEST_ASSERT_EQUAL_UINT32(0U, png.w);
    TEST_ASSERT_EQUAL_UINT32(0U, png.h);
    TEST_ASSERT_NULL(png.rgba);
}

TEST_CASE("data that inflates to the wrong size, or not at all, is damage", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };
    const int32_t before = held_now();

    png_start(2U, 2U, 8U, 6U, 0U);
    png_end();
    /* The header implies 2 rows of 1 + 8 bytes. */
    fake_tinfl_reset();
    (void)memset(s_raw, 0, sizeof(s_raw));
    fake_tinfl_set_payload(s_raw, 17U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    fake_tinfl_set_payload(s_raw, 19U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    fake_tinfl_set_payload(s_raw, 18U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_OK, kode_png_decode(s_png, s_png_n, &png));
    kode_png_free(&png);

    /* The decoder says it is broken, or that the stream ends early. */
    fake_tinfl_reset();
    fake_tinfl_set(TINFL_STATUS_FAILED, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    fake_tinfl_set(TINFL_STATUS_NEEDS_MORE_INPUT, 0U, 0U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    TEST_ASSERT_NULL(png.rgba);
    TEST_ASSERT_EQUAL_INT(before, held_now());
    fake_tinfl_reset();
}

TEST_CASE("a row with a filter that does not exist is damage", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    png_start(1U, 2U, 8U, 0U, 0U);
    png_end();
    fake_tinfl_reset();
    s_raw[0] = 0U;
    s_raw[1] = 9U;
    s_raw[2] = F_PAETH + 1U;
    s_raw[3] = 9U;
    fake_tinfl_set_payload(s_raw, 4U);
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    s_raw[2] = 0xFFU;
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    fake_tinfl_reset();
}

TEST_CASE("an interlaced PNG is valid and not supported", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    png_start(2U, 2U, 8U, 6U, 1U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_UNSUPPORTED, kode_png_decode(s_png, s_png_n, &png));
    png_start(2U, 2U, 8U, 6U, 2U);
    png_end();
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
}

TEST_CASE("no PSRAM at any step is a refusal that leaks nothing", "[kode_png]")
{
    const int32_t before = held_now();

    png_start(2U, 2U, 8U, 6U, 0U);
    png_end();
    fake_tinfl_reset();
    (void)memset(s_raw, 0, sizeof(s_raw));
    fake_tinfl_set_payload(s_raw, 18U);
    /* Four blocks: the joined data, the scanlines, the inflater's state, the pixels. */
    for ( uint32_t ok = 0U; 4U > ok; ok++ )
    {
        kode_png_t png = { 0U, 0U, NULL };

        fake_psram_refuse_after(ok, 1U);
        TEST_ASSERT_EQUAL_INT(KODE_PNG_NO_MEMORY, kode_png_decode(s_png, s_png_n, &png));
        TEST_ASSERT_NULL(png.rgba);
        TEST_ASSERT_EQUAL_INT(before, held_now());
        fake_psram_refuse(0U);
    }
    fake_tinfl_reset();
}

TEST_CASE("a zip bomb is stopped at the size the header gives", "[kode_png]")
{
    kode_png_t png = { 0U, 0U, NULL };

    png_start(2U, 2U, 8U, 6U, 0U);
    png_end();
    fake_tinfl_reset();
    (void)memset(s_raw, 0, sizeof(s_raw));
    /* More than the buffer holds: the inflater is stopped by its room. */
    fake_tinfl_set_payload(s_raw, sizeof(s_raw));
    TEST_ASSERT_EQUAL_INT(KODE_PNG_DAMAGED, kode_png_decode(s_png, s_png_n, &png));
    TEST_ASSERT_NULL(png.rgba);
    fake_tinfl_reset();
}

/* ---- What the task is told ----------------------------------------------------- */

TEST_CASE("each outcome has its words, with the path in front", "[kode_png]")
{
    kode_png_say("sd/a.png", KODE_PNG_NOT_PNG, 0U, 0U);
    TEST_ASSERT_EQUAL_STRING("sd/a.png: not a PNG", kode_error());
    kode_png_say("sd/a.png", KODE_PNG_TOO_BIG, 2000U, 40U);
    TEST_ASSERT_EQUAL_STRING("sd/a.png: too big for the glass (2000x40)", kode_error());
    kode_png_say("sd/a.png", KODE_PNG_DAMAGED, 0U, 0U);
    TEST_ASSERT_EQUAL_STRING("sd/a.png: damaged PNG", kode_error());
    kode_png_say("sd/a.png", KODE_PNG_UNSUPPORTED, 0U, 0U);
    TEST_ASSERT_EQUAL_STRING("sd/a.png: interlaced PNGs are not supported", kode_error());
    kode_png_say("sd/a.png", KODE_PNG_NO_MEMORY, 0U, 0U);
    TEST_ASSERT_EQUAL_STRING("sd/a.png: out of memory", kode_error());
}

TEST_CASE("red and blue swap in place for LVGL's byte order", "[kode_png]")
{
    uint8_t    px[8] = { 1U, 2U, 3U, 4U, 5U, 6U, 7U, 8U };
    kode_png_t png   = { 2U, 1U, px };

    kode_png_swap_rb(&png);
    TEST_ASSERT_EQUAL_HEX8(3U, px[0]);
    TEST_ASSERT_EQUAL_HEX8(2U, px[1]);
    TEST_ASSERT_EQUAL_HEX8(1U, px[2]);
    TEST_ASSERT_EQUAL_HEX8(4U, px[3]);
    TEST_ASSERT_EQUAL_HEX8(7U, px[4]);
    TEST_ASSERT_EQUAL_HEX8(6U, px[5]);
    TEST_ASSERT_EQUAL_HEX8(5U, px[6]);
    TEST_ASSERT_EQUAL_HEX8(8U, px[7]);
}

/* ---- Blending a picture ------------------------------------------------------------ */

/** @brief Canary words on each side of a grid. */
#define GUARD 16U

/** @brief What a guard word reads while untouched. */
#define CANARY 0xA5A5U

/** @brief Pixels of the grids below at most. */
#define GRID_MAX 64U

static uint16_t s_store[GUARD + GRID_MAX + GUARD];
static kode_raster_t s_grid;

/** @brief A blank grid of @p w by @p h between guards. */
static void grid_make(uint32_t w, uint32_t h)
{
    for ( uint32_t i = 0U; (GUARD + GRID_MAX + GUARD) > i; i++ )
    {
        s_store[i] = CANARY;
    }
    s_grid.px = &s_store[GUARD];
    s_grid.w  = w;
    s_grid.h  = h;
    for ( uint32_t i = 0U; (w * h) > i; i++ )
    {
        s_grid.px[i] = 0U;
    }
}

/** @brief Fail unless nothing outside the grid changed. */
static void guards_hold(void)
{
    for ( uint32_t i = 0U; GUARD > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(CANARY, s_store[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, s_store[GUARD + GRID_MAX + i]);
    }
    for ( uint32_t i = s_grid.w * s_grid.h; GRID_MAX > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(CANARY, s_grid.px[i]);
    }
}

/** @brief A 2 x 2 picture: opaque red, clear, half white, opaque blue. */
static const uint8_t PIC[] = {
    0xFF, 0x00, 0x00, 0xFF,   0x12, 0x34, 0x56, 0x00,
    0xFF, 0xFF, 0xFF, 0x80,   0x00, 0x00, 0xFF, 0xFF,
};

TEST_CASE("a picture blends at its offset: opaque writes, clear skips, half mixes", "[kode_png]")
{
    grid_make(5U, 4U);
    kode_raster_blend_rgba(&s_grid, 1, 1, PIC, 2U, 2U);
    TEST_ASSERT_EQUAL_HEX16(0xF800U, s_grid.px[(1U * 5U) + 1U]);
    TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[(1U * 5U) + 2U]);
    /* White at 128 of 255 over black: every channel (255 * 128 + 127) / 255 = 128. */
    TEST_ASSERT_EQUAL_HEX16(0x8410U, s_grid.px[(2U * 5U) + 1U]);
    TEST_ASSERT_EQUAL_HEX16(0x001FU, s_grid.px[(2U * 5U) + 2U]);
    TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[(3U * 5U) + 4U]);
    guards_hold();
}

TEST_CASE("a half transparent pixel mixes with what is under it", "[kode_png]")
{
    /* Over pure red (r5 = 31 -> 255, g 0, b 0): white at 128 -> (255*128 + 255*127 + 127)/255 = 255 red,
     * (255*128 + 0 + 127)/255 = 128 green and blue. */
    grid_make(2U, 2U);
    kode_raster_clear(&s_grid, 0xF800U);
    kode_raster_blend_rgba(&s_grid, 0, 0, &PIC[8], 1U, 1U);
    TEST_ASSERT_EQUAL_HEX16(0xFC10U, s_grid.px[0]);
    TEST_ASSERT_EQUAL_HEX16(0xF800U, s_grid.px[1]);
    guards_hold();
}

TEST_CASE("a picture is clipped at every edge and writes nothing outside", "[kode_png]")
{
    /* Over the left and top edges: only the picture's last pixel lands, at (0, 0). */
    grid_make(3U, 3U);
    kode_raster_blend_rgba(&s_grid, -1, -1, PIC, 2U, 2U);
    TEST_ASSERT_EQUAL_HEX16(0x001FU, s_grid.px[0]);
    for ( uint32_t i = 1U; 9U > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[i]);
    }
    guards_hold();

    /* Over the right and bottom edges: only its first pixel lands, at (2, 2). */
    grid_make(3U, 3U);
    kode_raster_blend_rgba(&s_grid, 2, 2, PIC, 2U, 2U);
    TEST_ASSERT_EQUAL_HEX16(0xF800U, s_grid.px[8]);
    for ( uint32_t i = 0U; 8U > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[i]);
    }
    guards_hold();

    /* Wholly outside, on every side, and far outside. */
    grid_make(3U, 3U);
    kode_raster_blend_rgba(&s_grid, -2, 0, PIC, 2U, 2U);
    kode_raster_blend_rgba(&s_grid, 3, 0, PIC, 2U, 2U);
    kode_raster_blend_rgba(&s_grid, 0, -2, PIC, 2U, 2U);
    kode_raster_blend_rgba(&s_grid, 0, 3, PIC, 2U, 2U);
    kode_raster_blend_rgba(&s_grid, KODE_DRAW_RANGE, KODE_DRAW_RANGE, PIC, 2U, 2U);
    kode_raster_blend_rgba(&s_grid, -KODE_DRAW_RANGE, -KODE_DRAW_RANGE, PIC, 2U, 2U);
    for ( uint32_t i = 0U; 9U > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[i]);
    }
    guards_hold();
}

TEST_CASE("a mask blends one colour by its coverage, row by row", "[kode_png]")
{
    /* A mask of 3 x 2 inside a stride of 4: the byte after each row is not read. */
    static const uint8_t MASK[] = { 0xFF, 0x80, 0x00, 0xEE,   0x00, 0xFF, 0xFF, 0xEE };

    grid_make(4U, 3U);
    kode_raster_blend_mask(&s_grid, 1, 1, MASK, 4U, 3U, 2U, KODE_WHITE);
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, s_grid.px[(1U * 4U) + 1U]);
    TEST_ASSERT_EQUAL_HEX16(0x8410U, s_grid.px[(1U * 4U) + 2U]);
    TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[(1U * 4U) + 3U]);
    TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[(2U * 4U) + 1U]);
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, s_grid.px[(2U * 4U) + 2U]);
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, s_grid.px[(2U * 4U) + 3U]);
    TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[0]);
    guards_hold();

    /* Clipped: the mask hangs off the left and the bottom. */
    grid_make(4U, 3U);
    kode_raster_blend_mask(&s_grid, -1, 2, MASK, 4U, 3U, 2U, KODE_WHITE);
    TEST_ASSERT_EQUAL_HEX16(0x8410U, s_grid.px[(2U * 4U) + 0U]);
    TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[(2U * 4U) + 1U]);
    for ( uint32_t i = 0U; 8U > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(0x0000U, s_grid.px[i]);
    }
    guards_hold();
}
