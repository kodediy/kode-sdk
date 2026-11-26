/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_utf8.h
 * @brief Whether a text is well-formed UTF-8, and where it may be cut.
 */
#ifndef KHAL_UTF8_H_
#define KHAL_UTF8_H_

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Bytes of @p text that fit in @p room without splitting a character.
 *
 * A text of @p room bytes or fewer fits whole. A longer one is cut at the
 * last character that ends within @p room bytes.
 *
 * @param text A NUL-terminated UTF-8 text.
 * @param room The most bytes the cut may keep, its NUL not counted.
 * @return The bytes to keep: strlen(@p text) when it fits whole, else at
 *         most @p room, ending where a character ends.
 */
size_t khal_utf8_fit(const char *text, size_t room);

/**
 * @brief Whether @p text is well-formed UTF-8 (RFC 3629).
 *
 * Every character is one byte below 0x80, or a lead byte and as many
 * continuation bytes as it announces, decoding to the shortest form of a code
 * point up to U+10FFFF that is not a surrogate. A stray continuation byte, a
 * lead byte of 0xF8 or above, a character the text ends inside, an overlong
 * form, a surrogate or a code point past U+10FFFF makes it ill-formed.
 *
 * @param text A NUL-terminated text; NULL is ill-formed.
 * @return true when every character is well-formed; "" is.
 */
bool khal_utf8_valid(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_UTF8_H_ */
