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
#define KAPP_ICON_W             64U
#define KAPP_ICON_H             64U
#define KAPP_ICON_BYTES      12288U    /**< 64*64*2 RGB565 + 64*64 A8. */

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

#ifdef __cplusplus
}
#endif

#endif /* KAPP_DATA_H_ */
