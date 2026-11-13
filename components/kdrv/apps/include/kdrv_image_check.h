/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_image_check.h
 * @brief An ESP image judged as its bytes arrive: the header chain against the file's length, then the SHA-256 it carries.
 */
#ifndef KDRV_IMAGE_CHECK_H
#define KDRV_IMAGE_CHECK_H

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Open a check for an image in a file of @p file_len bytes.
 *
 * One check at a time. The bytes are fed in order, from the image's first;
 * nothing is read from flash and nothing is written. Whoever is about to give
 * up an installed app for an image that arrives from a file feeds it through
 * here first.
 *
 * @param file_len The file's length: no segment may declare bytes past it.
 * @return KODE_OK; KODE_DENIED when a check is already open.
 */
kode_status_t kdrv_image_check_begin(uint32_t file_len);

/**
 * @brief Feed the next bytes of the file.
 *
 * Bytes past the image's end (the SHA-256 included) are taken and ignored.
 *
 * @param data The bytes.
 * @param len  How many.
 * @return KODE_OK while the file may still be a whole image; KODE_ERROR once
 *         it cannot be: a bad magic or segment count, a segment that leaves
 *         the file, or a SHA-256 that does not match. The check is then spent
 *         and kdrv_image_check_end() answers KODE_ERROR; KODE_DENIED with no
 *         check open.
 */
kode_status_t kdrv_image_check_feed(const void *data, size_t len);

/**
 * @brief Close the check and give its verdict.
 *
 * @return KODE_OK when the chain closed inside the bytes fed and the SHA-256
 *         it carries, if it carries one, matched; KODE_ERROR otherwise,
 *         including a file that ends inside its image; KODE_DENIED with no
 *         check open.
 */
kode_status_t kdrv_image_check_end(void);

/** @brief Abandon the check. Always safe. */
void kdrv_image_check_abort(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_IMAGE_CHECK_H */
