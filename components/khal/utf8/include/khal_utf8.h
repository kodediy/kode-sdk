/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_utf8.h
 * @brief Cutting a UTF-8 text at a whole character.
 */
#ifndef KHAL_UTF8_H_
#define KHAL_UTF8_H_

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

#ifdef __cplusplus
}
#endif

#endif /* KHAL_UTF8_H_ */
