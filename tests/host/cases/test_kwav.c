/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kwav.c
 * @brief The WAV header, and the refusals that are the point of reading one.
 */
#include <string.h>

#include "unity.h"

#include "kwav.h"

/** @brief Room for a header, a chunk in the way, and a little audio. */
static uint8_t s_f[256];

static void w32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static void w16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
}

/**
 * @brief Build a file with arbitrary fields, for the cases that need a bad one.
 *
 * @param rate    What to declare.
 * @param bits    What to declare.
 * @param ch      What to declare.
 * @param fmt     The format code; 1 is PCM.
 * @param dbytes  How many bytes of audio to actually put there.
 * @param junk    Whether to put a chunk in the way of the data.
 * @param claim   What the data chunk should claim; 0 means tell the truth.
 * @return How long the file is.
 */
static size_t mk(uint32_t rate, uint16_t bits, uint16_t ch, uint16_t fmt,
                 uint32_t dbytes, bool junk, uint32_t claim)
{
    size_t   p = 0U;
    uint32_t say = claim;

    if ( 0U == say )
    {
        say = dbytes;
    }
    (void)memcpy(&s_f[0], "RIFF", 4U);
    (void)memcpy(&s_f[8], "WAVE", 4U);
    p = 12U;
    (void)memcpy(&s_f[p], "fmt ", 4U);
    w32(&s_f[p + 4U], 16U);
    p += 8U;
    w16(&s_f[p], fmt);
    w16(&s_f[p + 2U], ch);
    w32(&s_f[p + 4U], rate);
    w32(&s_f[p + 8U], rate * ch * (bits / 8U));
    w16(&s_f[p + 12U], (uint16_t)(ch * (bits / 8U)));
    w16(&s_f[p + 14U], bits);
    p += 16U;
    if ( junk )
    {
        (void)memcpy(&s_f[p], "LIST", 4U);
        w32(&s_f[p + 4U], 7U);
        p += 8U;
        (void)memset(&s_f[p], 'x', 7U);
        p += 8U;   /* seven, padded to even */
    }
    (void)memcpy(&s_f[p], "data", 4U);
    w32(&s_f[p + 4U], say);
    p += 8U;
    (void)memset(&s_f[p], 0, dbytes);
    p += dbytes;
    w32(&s_f[4], (uint32_t)p - 8U);
    return p;
}

TEST_CASE("a header written here is a header read back", "[kwav]")
{
    uint8_t     full[KWAV_HEADER_BYTES + 200U];
    kwav_info_t i;

    (void)memset(full, 0, sizeof(full));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kwav_build(100U, full, sizeof(full)));
    TEST_ASSERT_EQUAL_INT(KWAV_OK, kwav_parse(full, sizeof(full), &i));
    TEST_ASSERT_EQUAL_UINT32(KWAV_RATE_HZ, i.rate);
    TEST_ASSERT_EQUAL_UINT(KWAV_BITS, i.bits);
    TEST_ASSERT_EQUAL_UINT(KWAV_CHANNELS, i.channels);
    TEST_ASSERT_EQUAL_UINT32(100U, i.frames);
    TEST_ASSERT_EQUAL_UINT32(KWAV_HEADER_BYTES, i.data_off);
}

TEST_CASE("a header will not be written where it does not fit", "[kwav]")
{
    uint8_t h[KWAV_HEADER_BYTES];
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kwav_build(0U, h, sizeof(h) - 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kwav_build(0U, NULL, 999U));
}

TEST_CASE("each wrong format is refused by its own name", "[kwav]")
{
    kwav_info_t i;
    size_t      n;

    n = mk(16000U, 16U, 1U, 1U, 64U, false, 0U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_RATE, kwav_parse(s_f, n, &i));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(16000U, i.rate,
        "a refusal still reports what the file actually is");

    n = mk(48000U, 16U, 2U, 1U, 64U, false, 0U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_CHANNELS, kwav_parse(s_f, n, &i));

    n = mk(48000U, 24U, 1U, 1U, 66U, false, 0U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_BITS, kwav_parse(s_f, n, &i));

    n = mk(48000U, 16U, 1U, 3U, 64U, false, 0U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_FORMAT, kwav_parse(s_f, n, &i));

    for ( int r = KWAV_OK; KWAV_BAD_CHANNELS >= r; ++r )
    {
        TEST_ASSERT_NOT_NULL(kwav_reason_text((kwav_reason_t)r));
    }
}

TEST_CASE("a file that is not a WAV at all is refused before anything else",
          "[kwav]")
{
    kwav_info_t  i;
    size_t const n = mk(48000U, 16U, 1U, 1U, 64U, false, 0U);

    (void)memcpy(&s_f[0], "RIFX", 4U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_RIFF, kwav_parse(s_f, n, &i));

    (void)mk(48000U, 16U, 1U, 1U, 64U, false, 0U);
    (void)memcpy(&s_f[8], "AVI ", 4U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_RIFF, kwav_parse(s_f, n, &i));
}

TEST_CASE("a data chunk claiming more than is there is a truncated file",
          "[kwav]")
{
    /* Believing it would mean reading past whatever buffer the samples were
     * loaded into, which is the whole reason this is checked. */
    kwav_info_t  i;
    size_t const n = mk(48000U, 16U, 1U, 1U, 64U, false, 100000U);

    TEST_ASSERT_EQUAL_INT(KWAV_TRUNCATED, kwav_parse(s_f, n, &i));
    TEST_ASSERT_EQUAL_INT(KWAV_TRUNCATED, kwav_parse(s_f, 20U, &i));
    TEST_ASSERT_EQUAL_INT(KWAV_TRUNCATED, kwav_parse(NULL, 999U, &i));
}

TEST_CASE("a data chunk past the buffer with a good format says so and keeps what it found",
          "[kwav]")
{
    kwav_info_t  i;
    size_t const n = mk(48000U, 16U, 1U, 1U, 64U, false, 100000U);

    (void)memset(&i, 0, sizeof(i));
    TEST_ASSERT_EQUAL_INT(KWAV_TRUNCATED, kwav_parse(s_f, n, &i));
    TEST_ASSERT_EQUAL_UINT32(KWAV_HEADER_BYTES, i.data_off);
    TEST_ASSERT_EQUAL_UINT32(50000U, i.frames);
    TEST_ASSERT_EQUAL_UINT32(KWAV_RATE_HZ, i.rate);
}

TEST_CASE("a data chunk past the buffer is judged on its format first", "[kwav]")
{
    kwav_info_t i;
    size_t      n = mk(44100U, 16U, 1U, 1U, 64U, false, 100000U);

    TEST_ASSERT_EQUAL_INT(KWAV_BAD_RATE, kwav_parse(s_f, n, &i));
    n = mk(48000U, 16U, 2U, 1U, 64U, false, 100000U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_CHANNELS, kwav_parse(s_f, n, &i));
    n = mk(48000U, 8U, 1U, 1U, 64U, false, 100000U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_BITS, kwav_parse(s_f, n, &i));
    n = mk(48000U, 16U, 1U, 3U, 64U, false, 100000U);
    TEST_ASSERT_EQUAL_INT(KWAV_BAD_FORMAT, kwav_parse(s_f, n, &i));
}

TEST_CASE("a chunk in the way is skipped rather than played", "[kwav]")
{
    /* Files that arrive from elsewhere carry LIST and fact chunks ahead of
     * the data, and a reader that trusted byte 44 would play metadata as
     * sound. */
    kwav_info_t  i;
    size_t const n = mk(48000U, 16U, 1U, 1U, 64U, true, 0U);

    TEST_ASSERT_EQUAL_INT(KWAV_OK, kwav_parse(s_f, n, &i));
    TEST_ASSERT_EQUAL_UINT32(60U, i.data_off);
    TEST_ASSERT_EQUAL_UINT32(32U, i.frames);
}

TEST_CASE("a file with no data chunk says so", "[kwav]")
{
    kwav_info_t i;

    (void)memcpy(&s_f[0], "RIFF", 4U);
    w32(&s_f[4], 36U);
    (void)memcpy(&s_f[8], "WAVE", 4U);
    (void)memcpy(&s_f[12], "fmt ", 4U);
    w32(&s_f[16], 16U);
    w16(&s_f[20], 1U);
    w16(&s_f[22], 1U);
    w32(&s_f[24], 48000U);
    w32(&s_f[28], 96000U);
    w16(&s_f[32], 2U);
    w16(&s_f[34], 16U);
    (void)memcpy(&s_f[36], "LIST", 4U);
    w32(&s_f[40], 0U);
    TEST_ASSERT_EQUAL_INT(KWAV_NO_DATA, kwav_parse(s_f, 44U, &i));
    TEST_ASSERT_EQUAL_INT(KWAV_NO_DATA, kwav_parse(s_f, 44U, NULL));
}
