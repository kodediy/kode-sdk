/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kapp_data.h
 * @brief One app.json, read into a fixed-size struct.
 */
#ifndef KAPP_DATA_H_
#define KAPP_DATA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kapp_cat.h"    /* KAPP_CATEGORY_MAX */
#include "kapp_desc.h"   /* KAPP_NAME_MAX */

#ifdef __cplusplus
extern "C" {
#endif

#define KAPP_DATA_JSON_MAX     1024U   /**< A longer app.json is refused with SIZE. */
#define KAPP_DATA_VERSION_MAX  16U     /**< version, terminator included (AppEntryEvt). */
#define KAPP_DATA_DESC_MAX     56U     /**< description: 52 chars + terminator, rounded. */
#define KAPP_DATA_ICON_MAX     41U     /**< icon path, relative, terminator included. */
#define KAPP_ICON_W            128U    /**< The icon the board holds, in pixels across. */
#define KAPP_ICON_H            128U    /**< The icon the board holds, in pixels down.   */
#define KAPP_ICON_BYTES      49152U    /**< 128*128*2 RGB565 + 128*128 A8: the block
                                        *   every reader and drawer of an icon uses.   */
#define KAPP_ICON_HALF_W        64U    /**< The smaller form a file may have, across.   */
#define KAPP_ICON_HALF_H        64U    /**< The smaller form a file may have, down.     */
#define KAPP_ICON_HALF_BYTES 12288U    /**< 64*64*2 RGB565 + 64*64 A8: the smaller form a
                                        *   file may have. It is brought up to
                                        *   KAPP_ICON_BYTES when it is read.            */
_Static_assert(KAPP_ICON_BYTES == ((KAPP_ICON_W * KAPP_ICON_H * 2U) + (KAPP_ICON_W * KAPP_ICON_H)),
               "KAPP_ICON_BYTES is a 128x128 RGB565 plane and an A8 plane");
_Static_assert(KAPP_ICON_HALF_BYTES == ((KAPP_ICON_HALF_W * KAPP_ICON_HALF_H * 2U)
                                        + (KAPP_ICON_HALF_W * KAPP_ICON_HALF_H)),
               "KAPP_ICON_HALF_BYTES is a 64x64 RGB565 plane and an A8 plane");
_Static_assert(KAPP_ICON_W == (2U * KAPP_ICON_HALF_W),
               "the smaller icon doubles to the one the board holds");
_Static_assert(KAPP_ICON_H == (2U * KAPP_ICON_HALF_H),
               "the smaller icon doubles to the one the board holds");
#define KAPP_DATA_JSON_NAME  "app.json"  /**< A data folder's first file.          */
#define KAPP_DATA_ICON_NAME  "icon.k64"  /**< Its icon, when a writer of the folder
                                          *   stores one.                          */

/** @brief What kapp_data_parse() found, or why it found nothing. */
typedef enum {
    KAPP_JSON_OK = 0,   /**< Parsed; the fields below are the file's.     */
    KAPP_JSON_NONE,     /**< No app.json in the folder.                   */
    KAPP_JSON_SIZE,     /**< Longer than KAPP_DATA_JSON_MAX.              */
    KAPP_JSON_SYNTAX,   /**< Outside the subset, or a field over its limit.*/
    KAPP_JSON_REVISION  /**< `kode` missing or not "1".                   */
} kapp_json_status_t;

/** @brief One app.json's fields, or all-zero when the status is not OK. */
typedef struct {
    char    name[KAPP_NAME_MAX];
    char    category[KAPP_CATEGORY_MAX];   /**< Normalised, lowercased.   */
    char    version[KAPP_DATA_VERSION_MAX];
    char    description[KAPP_DATA_DESC_MAX];
    char    icon[KAPP_DATA_ICON_MAX];      /**< Relative path, or "".     */
    uint8_t color[3];                      /**< RGB when color_valid.      */
    bool    color_valid;
} kapp_data_t;

/**
 * @brief Parse one app.json (section B of the spec) into @p out.
 * @param text  The file's bytes, not necessarily terminated.
 * @param len   How many; more than KAPP_DATA_JSON_MAX is refused unread.
 * @param out   Zeroed on every status but OK.
 * @return The status token the console prints.
 */
kapp_json_status_t kapp_data_parse(const char *text, size_t len, kapp_data_t *out);

/** @brief The one-word name of a status, for the console and the log. */
const char *kapp_json_status_name(kapp_json_status_t st);

/**
 * @brief Whether @p len is the length of an icon.k64 the board reads.
 * @param len A file's length.
 * @return true for KAPP_ICON_BYTES and KAPP_ICON_HALF_BYTES, false for any other.
 */
bool kapp_icon_len_ok(size_t len);

/**
 * @brief Bring an icon read into @p block up to the 128x128 form, in place.
 *
 * A file of KAPP_ICON_BYTES is left as it is. One of KAPP_ICON_HALF_BYTES is
 * doubled in both directions, each pixel of the RGB565 plane and of the A8
 * plane becoming a square of four, so the block reads as a 128x128 icon
 * whichever the file was. The caller is the one place that reads icon.k64;
 * nothing above it learns that the file had two sizes.
 *
 * @param block KAPP_ICON_BYTES of writable memory, the file's bytes at its head.
 * @param len   The file's length, as the read reported it.
 * @return true when @p block holds a 128x128 icon; false, with @p block
 *         untouched, when @p len is neither size.
 */
bool kapp_icon_widen(uint8_t *block, size_t len);

/** @brief What an app.json that arrived alone, ahead of its folder, asks for. */
typedef enum {
    KAPP_FETCH_NO_ICON = 0,  /**< A good file that names no icon: it is the folder. */
    KAPP_FETCH_ICON,         /**< A good file that names KAPP_DATA_ICON_NAME:
                              *   the icon is to be fetched beside it.          */
    KAPP_FETCH_REFUSED       /**< Not what the board reads, or an icon by a name
                              *   a writer of the folder does not store.        */
} kapp_fetch_t;

/**
 * @brief Judge an app.json fetched to make a data folder of.
 *
 * A folder's writers store the icon as KAPP_DATA_ICON_NAME and nothing else,
 * so a file that names another path would leave a folder whose reader looks
 * for a file that is not there.
 *
 * @param text The file's bytes, not necessarily terminated.
 * @param len  How many.
 * @param why  Receives kapp_data_parse()'s status; KAPP_JSON_OK for a file
 *             refused only for its icon's name. May be NULL.
 * @return What the file asks for.
 */
kapp_fetch_t kapp_data_fetched(const char *text, size_t len, kapp_json_status_t *why);

#ifdef __cplusplus
}
#endif

#endif /* KAPP_DATA_H_ */
