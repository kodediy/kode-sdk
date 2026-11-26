/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_utf8.c
 * @brief khal_utf8_fit() and khal_utf8_valid(): where a UTF-8 text may be cut, and
 *        whether it is well-formed.
 */
#include "khal_utf8.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/** @brief Mask that isolates the marker bits of a UTF-8 byte. */
#define UTF8_CONT_MASK 0xC0U

/** @brief What those bits read on a continuation byte (10xxxxxx). */
#define UTF8_CONT_BITS 0x80U

/** @brief The bits a continuation byte carries. */
#define UTF8_CONT_PAYLOAD 0x3FU

/** @brief How many bits a continuation byte carries. */
#define UTF8_CONT_SHIFT 6U

/** @brief The highest byte that is a character by itself. */
#define UTF8_ASCII_MAX 0x7FU

/** @brief The first surrogate, which UTF-8 does not carry. */
#define UTF8_SURROGATE_FIRST 0xD800U

/** @brief The last surrogate. */
#define UTF8_SURROGATE_LAST 0xDFFFU

/** @brief The highest code point. */
#define UTF8_CODE_POINT_MAX 0x10FFFFU

/** @brief One length of character: its lead byte's marker and the least code point it may carry. */
typedef struct {
    uint8_t  mask;   /**< The lead byte's marker bits.                         */
    uint8_t  bits;   /**< What they read.                                      */
    uint32_t least;  /**< The least code point this length carries; below is overlong. */
} utf8_lead_t;

/** @brief The lengths a character of more than one byte has: two, three and four. */
#define UTF8_LEADS 3U

size_t khal_utf8_fit(const char *text, size_t room)
{
    size_t at = strlen(text);

    if ( at > room )
    {
        /* The byte at `at` is the first one left out: a continuation byte
         * there means the character it belongs to began earlier. */
        at = room;
        while ( (0U < at) && (((uint8_t)text[at] & UTF8_CONT_MASK) == UTF8_CONT_BITS) )
        {
            --at;
        }
    }
    return at;
}

/**
 * @brief Bytes of the character that starts at @p s, when it is well-formed.
 * @param s A character's first byte, not the terminator.
 * @return 1 to 4; 0 for a malformed character. Reads no byte past a terminator.
 */
static size_t char_bytes(const char *s)
{
    /* Characters of two, three and four bytes, in that order. */
    static const utf8_lead_t s_leads[UTF8_LEADS] = {
        { 0xE0U, 0xC0U, 0x80U },
        { 0xF0U, 0xE0U, 0x800U },
        { 0xF8U, 0xF0U, 0x10000U },
    };
    const uint8_t lead = (uint8_t)s[0];
    size_t        n    = 0U;

    if ( UTF8_ASCII_MAX >= lead )
    {
        n = 1U;
    }
    for ( size_t i = 0U; i < UTF8_LEADS; i++ )
    {
        if ( (lead & s_leads[i].mask) == s_leads[i].bits )
        {
            uint32_t cp = (uint32_t)lead & (uint32_t)(uint8_t)~s_leads[i].mask;
            bool     ok = true;

            /* A terminator is no continuation byte: the walk stops on it. */
            for ( size_t k = 1U; ok && (k <= (i + 1U)); k++ )
            {
                const uint8_t b = (uint8_t)s[k];

                ok = ((b & UTF8_CONT_MASK) == UTF8_CONT_BITS);
                cp = (cp << UTF8_CONT_SHIFT) | ((uint32_t)b & UTF8_CONT_PAYLOAD);
            }
            if ( ok && (cp >= s_leads[i].least) && (cp <= UTF8_CODE_POINT_MAX)
                 && ((cp < UTF8_SURROGATE_FIRST) || (cp > UTF8_SURROGATE_LAST)) )
            {
                n = i + 2U;
            }
        }
    }
    return n;
}

bool khal_utf8_valid(const char *text)
{
    bool   ok = (NULL != text);
    size_t at = 0U;

    while ( ok && ('\0' != text[at]) )
    {
        const size_t n = char_bytes(&text[at]);

        ok = (0U != n);
        at += n;
    }
    return ok;
}
