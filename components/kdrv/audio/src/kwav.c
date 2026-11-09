/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kwav.c
 * @brief The WAV header, built and read.
 */
#include "kwav.h"

#include <string.h>

/** @brief Uncompressed PCM, the only format code this reader accepts. */
#define KWAV_FMT_PCM        1U

/** @brief A chunk's identifier and length, ahead of its payload. */
#define KWAV_CHUNK_HDR      8U

/**
 * @brief Read four little-endian bytes.
 *
 * @param p Where to read from.
 * @return The value.
 */
static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0]
         | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16)
         | ((uint32_t)p[3] << 24);
}

/**
 * @brief Read two little-endian bytes.
 *
 * @param p Where to read from.
 * @return The value.
 */
static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/**
 * @brief Write four little-endian bytes.
 *
 * @param p Where to write.
 * @param v The value.
 */
static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

/**
 * @brief Write two little-endian bytes.
 *
 * @param p Where to write.
 * @param v The value.
 */
static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

kode_status_t kwav_build(uint32_t frames, uint8_t *out, size_t cap)
{
    uint32_t data_bytes;
    uint32_t byte_rate;

    if ( (NULL == out) || ((size_t)KWAV_HEADER_BYTES > cap) )
    {
        return KODE_DENIED;
    }
    data_bytes = frames * (uint32_t)KWAV_CHANNELS * ((uint32_t)KWAV_BITS / 8U);
    byte_rate  = (uint32_t)KWAV_RATE_HZ * (uint32_t)KWAV_CHANNELS
                 * ((uint32_t)KWAV_BITS / 8U);

    (void)memcpy(&out[0], "RIFF", 4U);
    /* Everything after this field: the eight bytes of "WAVE" plus the two
     * chunk headers, plus the samples. */
    wr32(&out[4], (uint32_t)(KWAV_HEADER_BYTES - 8U) + data_bytes);
    (void)memcpy(&out[8], "WAVE", 4U);

    (void)memcpy(&out[12], "fmt ", 4U);
    wr32(&out[16], 16U);                       /* a PCM format chunk        */
    wr16(&out[20], (uint16_t)KWAV_FMT_PCM);
    wr16(&out[22], (uint16_t)KWAV_CHANNELS);
    wr32(&out[24], (uint32_t)KWAV_RATE_HZ);
    wr32(&out[28], byte_rate);
    wr16(&out[32], (uint16_t)((uint32_t)KWAV_CHANNELS
                              * ((uint32_t)KWAV_BITS / 8U)));  /* block align */
    wr16(&out[34], (uint16_t)KWAV_BITS);

    (void)memcpy(&out[36], "data", 4U);
    wr32(&out[40], data_bytes);
    return KODE_OK;
}

kwav_reason_t kwav_parse(const uint8_t *buf, size_t len, kwav_info_t *out)
{
    kwav_info_t found;
    size_t      pos = 12U;
    bool        have_fmt = false;
    uint16_t    format = 0U;

    (void)memset(&found, 0, sizeof(found));

    if ( (NULL == buf) || ((size_t)KWAV_HEADER_BYTES > len) )
    {
        return KWAV_TRUNCATED;
    }
    if ( (memcmp(&buf[0], "RIFF", 4U) != 0) || (memcmp(&buf[8], "WAVE", 4U) != 0) )
    {
        return KWAV_BAD_RIFF;
    }

    /* Walk the chunks rather than assuming the canonical layout: a file that
     * came from somewhere else carries LIST or fact chunks ahead of the data,
     * and a reader that trusted byte 44 would play metadata as sound. */
    while ( (pos + (size_t)KWAV_CHUNK_HDR) <= len )
    {
        uint32_t size = rd32(&buf[pos + 4U]);
        size_t   body = pos + (size_t)KWAV_CHUNK_HDR;

        if ( memcmp(&buf[pos], "fmt ", 4U) == 0 )
        {
            if ( (16U > size) || ((body + 16U) > len) )
            {
                return KWAV_TRUNCATED;
            }
            format         = rd16(&buf[body]);
            found.channels = rd16(&buf[body + 2U]);
            found.rate     = rd32(&buf[body + 4U]);
            found.bits     = rd16(&buf[body + 14U]);
            have_fmt       = true;
        }
        else if ( memcmp(&buf[pos], "data", 4U) == 0 )
        {
            if ( !have_fmt )
            {
                return KWAV_NO_FMT;
            }
            found.data_off = (uint32_t)body;
            if ( ((found.bits / 8U) == 0U) || (0U == found.channels) )
            {
                return KWAV_BAD_FORMAT;
            }
            found.frames = size
                           / ((uint32_t)found.channels
                              * ((uint32_t)found.bits / 8U));
            if ( NULL != out )
            {
                *out = found;
            }
            if ( (uint16_t)KWAV_FMT_PCM != format )
            {
                return KWAV_BAD_FORMAT;
            }
            if ( (uint32_t)KWAV_RATE_HZ != found.rate )
            {
                return KWAV_BAD_RATE;
            }
            if ( (uint16_t)KWAV_BITS != found.bits )
            {
                return KWAV_BAD_BITS;
            }
            if ( (uint16_t)KWAV_CHANNELS != found.channels )
            {
                return KWAV_BAD_CHANNELS;
            }
            /* A data chunk that claims more than the buffer holds is the
             * last thing judged: the format is good, the samples run on. */
            if ( ((uint64_t)body + (uint64_t)size) > (uint64_t)len )
            {
                return KWAV_TRUNCATED;
            }
            return KWAV_OK;
        }
        else
        {
            /* Something else. Skipped, not refused. */
        }

        /* Chunks are padded to an even length, and the pad byte is not
         * counted in the size field. */
        pos = body + size + (size & 1U);
    }

    if ( NULL != out )
    {
        *out = found;
    }
    if ( !have_fmt )
    {
        return KWAV_NO_FMT;
    }
    return KWAV_NO_DATA;
}

const char *kwav_reason_text(kwav_reason_t r)
{
    switch ( r )
    {
        case KWAV_OK:
            return "ok";
        case KWAV_BAD_RIFF:
            return "not a WAV file";
        case KWAV_TRUNCATED:
            return "truncated";
        case KWAV_NO_FMT:
            return "no format chunk";
        case KWAV_NO_DATA:
            return "no data chunk";
        case KWAV_BAD_FORMAT:
            return "not uncompressed PCM";
        case KWAV_BAD_RATE:
            return "wrong sample rate; this board plays 48000 Hz only";
        case KWAV_BAD_BITS:
            return "wrong sample width; this board plays 16-bit only";
        case KWAV_BAD_CHANNELS:
            return "not mono; the codec has one channel";
        default:
            return "?";
    }
}
