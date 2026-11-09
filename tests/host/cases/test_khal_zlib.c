/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_khal_zlib.c
 * @brief The one-call inflate: what it asks of the decoder, and what it makes of the answer.
 */
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "khal_zlib.h"
#include "miniz.h"

/** Any bytes do: the stand-in decoder reads none of them. */
static const uint8_t STREAM[24] = { 0x78U, 0xDAU };

/**
 * What khal_zlib_decompress() asks the decoder for: a zlib stream into a flat buffer,
 * and to stop where the input ends.
 */
#define ONE_CALL_FLAGS \
    ((uint32_t)TINFL_FLAG_PARSE_ZLIB_HEADER | (uint32_t)TINFL_FLAG_HAS_MORE_INPUT \
     | (uint32_t)TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF)

TEST_CASE("a whole stream comes back as its bytes and their count", "[khal_zlib]")
{
    uint8_t dst[64];
    size_t n = 999U;

    (void)memset(dst, 0xEE, sizeof(dst));
    fake_tinfl_set((int)TINFL_STATUS_DONE, sizeof(STREAM), 40U);
    fake_tinfl_set_fill((uint8_t)'K');

    TEST_ASSERT_EQUAL(KODE_OK, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(40U, n);
    TEST_ASSERT_EACH_EQUAL_UINT8((uint8_t)'K', dst, 40U);
    /* Nothing past what the decoder wrote is touched. */
    TEST_ASSERT_EACH_EQUAL_UINT8(0xEEU, &dst[40], sizeof(dst) - 40U);
}

TEST_CASE("one call to the decoder: a zlib stream, a flat buffer, a stop at the input's end", "[khal_zlib]")
{
    /* 100 is not a power of two: the size the ROM refuses when the buffer is
     * not declared flat. */
    uint8_t dst[100];
    size_t n = 0U;

    fake_tinfl_set((int)TINFL_STATUS_DONE, sizeof(STREAM), sizeof(dst));

    TEST_ASSERT_EQUAL(KODE_OK, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(1U, fake_tinfl_calls());
    TEST_ASSERT_EQUAL_HEX32(ONE_CALL_FLAGS, fake_tinfl_last_flags());
    /* The whole room was offered: the stand-in writes no more than it is given. */
    TEST_ASSERT_EQUAL_UINT(sizeof(dst), n);
}

TEST_CASE("the decoder's state is borrowed from PSRAM and given back on every way out", "[khal_zlib]")
{
    /* Each status, and the input the decoder says it used: the last row is a
     * stream that ends one byte short of its input. */
    static const struct {
        tinfl_status status;
        size_t       used;
    } ANSWERS[] = {
        { TINFL_STATUS_DONE,             sizeof(STREAM) },
        { TINFL_STATUS_FAILED,           sizeof(STREAM) },
        { TINFL_STATUS_ADLER32_MISMATCH, sizeof(STREAM) },
        { TINFL_STATUS_BAD_PARAM,        sizeof(STREAM) },
        { TINFL_STATUS_NEEDS_MORE_INPUT, sizeof(STREAM) },
        { TINFL_STATUS_HAS_MORE_OUTPUT,  sizeof(STREAM) },
        { TINFL_STATUS_DONE,             sizeof(STREAM) - 1U },
    };
    uint8_t dst[32];
    size_t n = 0U;

    for ( size_t i = 0U; (sizeof(ANSWERS) / sizeof(ANSWERS[0])) > i; ++i )
    {
        fake_psram_reset();
        fake_tinfl_set((int)ANSWERS[i].status, ANSWERS[i].used, 8U);
        (void)khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_alloc_count(), "one block is borrowed");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_psram_free_count(), "and it is given back");
    }
}

TEST_CASE("a decoder that fails is an error, and no bytes are reported", "[khal_zlib]")
{
    static const tinfl_status VERDICTS[] = {
        TINFL_STATUS_FAILED,
        TINFL_STATUS_ADLER32_MISMATCH,
        TINFL_STATUS_BAD_PARAM,
    };
    uint8_t dst[32];

    for ( size_t i = 0U; (sizeof(VERDICTS) / sizeof(VERDICTS[0])) > i; ++i )
    {
        size_t n = 999U;

        /* It had written ten bytes before it gave up: they are not a payload. */
        fake_tinfl_set((int)VERDICTS[i], sizeof(STREAM), 10U);
        TEST_ASSERT_EQUAL(KODE_ERROR, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
        TEST_ASSERT_EQUAL_UINT(0U, n);
    }
}

TEST_CASE("a stream that ends before it is complete is an error", "[khal_zlib]")
{
    uint8_t dst[32];
    size_t n = 999U;

    fake_tinfl_set((int)TINFL_STATUS_NEEDS_MORE_INPUT, sizeof(STREAM), 10U);

    TEST_ASSERT_EQUAL(KODE_ERROR, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
}

TEST_CASE("a stream with other bytes after it is an error, and no bytes are reported", "[khal_zlib]")
{
    /* How many of the input's bytes follow the stream: one, and all but one. */
    static const size_t AFTER[] = { 1U, sizeof(STREAM) - 1U };
    uint8_t dst[32];

    for ( size_t i = 0U; (sizeof(AFTER) / sizeof(AFTER[0])) > i; ++i )
    {
        size_t n = 999U;

        /* The decoder is done, with its payload written, short of the input's end. */
        fake_tinfl_set((int)TINFL_STATUS_DONE, sizeof(STREAM) - AFTER[i], 10U);
        TEST_ASSERT_EQUAL(KODE_ERROR, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
        TEST_ASSERT_EQUAL_UINT(0U, n);
    }
}

TEST_CASE("a stream that holds more than the buffer is refused", "[khal_zlib]")
{
    uint8_t dst[32];
    size_t n = 999U;

    /* The buffer is full and the decoder has more to give. */
    fake_tinfl_set((int)TINFL_STATUS_HAS_MORE_OUTPUT, 12U, sizeof(dst));

    TEST_ASSERT_EQUAL(KODE_DENIED, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
}

TEST_CASE("with no PSRAM for the state the call says so and the decoder is never asked", "[khal_zlib]")
{
    uint8_t dst[32];
    size_t n = 999U;

    fake_psram_reset();
    fake_psram_refuse(1U);
    fake_tinfl_set((int)TINFL_STATUS_DONE, sizeof(STREAM), 8U);

    TEST_ASSERT_EQUAL(KODE_NO_RESOURCES, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
    TEST_ASSERT_EQUAL_UINT(0U, fake_tinfl_calls());
    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_free_count());

    /* The refusal was for one allocation: the next call has its state. */
    TEST_ASSERT_EQUAL(KODE_OK, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(8U, n);
}

TEST_CASE("a missing pointer or an empty length is refused before anything is borrowed", "[khal_zlib]")
{
    uint8_t dst[32];
    size_t n = 999U;

    fake_psram_reset();
    fake_tinfl_set((int)TINFL_STATUS_DONE, sizeof(STREAM), 8U);

    TEST_ASSERT_EQUAL(KODE_DENIED, khal_zlib_decompress(NULL, sizeof(STREAM), dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
    n = 999U;
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_zlib_decompress(STREAM, 0U, dst, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
    n = 999U;
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_zlib_decompress(STREAM, sizeof(STREAM), NULL, sizeof(dst), &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
    n = 999U;
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, 0U, &n));
    TEST_ASSERT_EQUAL_UINT(0U, n);
    TEST_ASSERT_EQUAL(KODE_DENIED, khal_zlib_decompress(STREAM, sizeof(STREAM), dst, sizeof(dst), NULL));

    TEST_ASSERT_EQUAL_UINT(0U, fake_psram_alloc_count());
    TEST_ASSERT_EQUAL_UINT(0U, fake_tinfl_calls());
}
