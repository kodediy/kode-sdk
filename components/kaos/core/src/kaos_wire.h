/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kaos_wire.h
 * @brief The base64 encoder and decoder, and the slice sizing every kaos wire producer uses.
 */
#ifndef KAOS_WIRE_H_
#define KAOS_WIRE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Records emitted per AO step, for every producer: they share one sink
 *        (kode_log_reply() -> AO_Log), and an AO step must stay short. */
#define KAOS_WIRE_SLICE        12U

/* The payload record: 216 base64 characters is a whole number of 4-character
 * groups (so a whole number of source bytes, 162) and small enough that the
 * record plus its wire prefix stays inside kode_log_reply()'s line buffer
 * with room to spare. */
#define KAOS_WIRE_PAY_B64      216U
#define KAOS_WIRE_PAY_BYTES    ((KAOS_WIRE_PAY_B64 / 4U) * 3U)   /* 162 */

/** @brief Base64's alphabet, in the only order it has. */
static const char KAOS_WIRE_B64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/**
 * @brief Encode @p len bytes of @p in into @p out, NUL-terminated, padded.
 *
 * The tail is padded, so every record decodes by the same rule and a host
 * never has to know which one is last.
 *
 * @param in  Bytes to encode.
 * @param len How many.
 * @param out Must hold at least (((len + 2) / 3) * 4) + 1 characters.
 * @return Characters written, padding included.
 */
static inline size_t kaos_wire_b64_encode(const uint8_t *in, size_t len,
                                          char *out)
{
    size_t o = 0U;
    size_t i = 0U;

    while ( (i + 3U) <= len )
    {
        uint32_t const v = ((uint32_t)in[i] << 16)
                         | ((uint32_t)in[i + 1U] << 8)
                         | (uint32_t)in[i + 2U];
        out[o]      = KAOS_WIRE_B64[(v >> 18) & 0x3FU];
        out[o + 1U] = KAOS_WIRE_B64[(v >> 12) & 0x3FU];
        out[o + 2U] = KAOS_WIRE_B64[(v >> 6) & 0x3FU];
        out[o + 3U] = KAOS_WIRE_B64[v & 0x3FU];
        o += 4U;
        i += 3U;
    }
    if ( i < len )
    {
        bool const     two = ((i + 1U) < len);
        uint32_t       v   = (uint32_t)in[i] << 16;
        if ( two )
        {
            v |= (uint32_t)in[i + 1U] << 8;
        }
        out[o]      = KAOS_WIRE_B64[(v >> 18) & 0x3FU];
        out[o + 1U] = KAOS_WIRE_B64[(v >> 12) & 0x3FU];
        out[o + 2U] = two ? KAOS_WIRE_B64[(v >> 6) & 0x3FU] : '=';
        out[o + 3U] = '=';
        o += 4U;
    }
    out[o] = '\0';
    return o;
}

/**
 * @brief The value of one base64 character.
 * @return 0..63, or 64 for a character outside the alphabet.
 */
static inline uint8_t kaos_wire_b64_value(char c)
{
    uint8_t const u = (uint8_t)c;
    uint8_t       v = 64U;

    if ( ((uint8_t)'A' <= u) && ((uint8_t)'Z' >= u) )
    {
        v = (uint8_t)(u - (uint8_t)'A');
    }
    else if ( ((uint8_t)'a' <= u) && ((uint8_t)'z' >= u) )
    {
        v = (uint8_t)((uint8_t)(u - (uint8_t)'a') + 26U);
    }
    else if ( ((uint8_t)'0' <= u) && ((uint8_t)'9' >= u) )
    {
        v = (uint8_t)((uint8_t)(u - (uint8_t)'0') + 52U);
    }
    else if ( (uint8_t)'+' == u )
    {
        v = 62U;
    }
    else if ( (uint8_t)'/' == u )
    {
        v = 63U;
    }
    else
    {
        v = 64U;
    }
    return v;
}

/**
 * @brief Decode padded base64, as kaos_wire_b64_encode() writes it.
 *
 * Strict: whole groups of four, padding only in the last group, nothing
 * outside the alphabet.
 *
 * @param in      NUL-terminated base64.
 * @param out     Where the bytes go.
 * @param cap     Bytes available there.
 * @param out_len Out: bytes written.
 * @return false for malformed input or one that does not fit @p cap.
 */
static inline bool kaos_wire_b64_decode(const char *in, uint8_t *out,
                                        size_t cap, size_t *out_len)
{
    size_t const len = strlen(in);
    size_t       o   = 0U;
    bool         ok  = ((len % 4U) == 0U);

    for ( size_t i = 0U; ok && (i < len); i += 4U )
    {
        bool const last = ((i + 4U) == len);
        uint8_t    v[4];
        uint8_t    pad = 0U;
        for ( size_t k = 0U; ok && (4U > k); k++ )
        {
            char const c = in[i + k];
            if ( last && ('=' == c) && (2U <= k) )
            {
                v[k] = 0U;
                pad++;
            }
            else
            {
                v[k] = kaos_wire_b64_value(c);
                ok = (64U > v[k]) && (0U == pad);
            }
        }
        size_t const n = 3U - (size_t)pad;
        ok = ok && ((o + n) <= cap);
        if ( ok )
        {
            uint32_t const w = ((uint32_t)v[0] << 18) | ((uint32_t)v[1] << 12)
                             | ((uint32_t)v[2] << 6) | (uint32_t)v[3];
            out[o] = (uint8_t)(w >> 16);
            if ( 1U < n )
            {
                out[o + 1U] = (uint8_t)(w >> 8);
            }
            if ( 2U < n )
            {
                out[o + 2U] = (uint8_t)w;
            }
            o += n;
        }
    }
    *out_len = o;
    return ok;
}

#ifdef __cplusplus
}
#endif

#endif /* KAOS_WIRE_H_ */
