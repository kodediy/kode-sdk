/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_utf8.c
 * @brief khal_utf8_fit(): where a UTF-8 text may be cut.
 */
#include "khal_utf8.h"

#include <stdint.h>
#include <string.h>

/** @brief Mask that isolates the marker bits of a UTF-8 byte. */
#define UTF8_CONT_MASK 0xC0U

/** @brief What those bits read on a continuation byte (10xxxxxx). */
#define UTF8_CONT_BITS 0x80U

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
