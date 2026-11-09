/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kapp_cat.h
 * @brief A category list, and the device's own overrides for it.
 */
#ifndef KAPP_CAT_H_
#define KAPP_CAT_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One category, terminator included. Revision 1's whole field. */
#define KAPP_CATEGORY_ITEM_MAX  16U
/** @brief How many categories one app may carry. */
#define KAPP_CATEGORIES_MAX      4U
/* Twinned in kqp_config.h as KODE_APP_CATEGORY_MAX, because kqp sits below
 * kdrv and its events carry this string. Change one, change the other. */
/**
 * @brief The whole list, terminator included.
 *
 * Not a round number: four items of maximum length, three commas and a
 * terminator is 4*15 + 3 + 1 = 64 exactly.
 */
#define KAPP_CATEGORY_MAX       64U

/**
 * @brief Copy the item @p cursor points at and return the cursor past it.
 *
 * Leading separators are skipped, so an empty item is never produced. An
 * item longer than @p len is truncated into @p item; callers that must
 * reject rather than truncate pass a buffer big enough to tell.
 *
 * @param cursor Where to read from; the list itself on the first call.
 * @param item   Receives the item, always terminated.
 * @param len    Size of @p item.
 * @return The cursor for the next call, or NULL when the list is exhausted.
 */
const char *kapp_cat_next(const char *cursor, char *item, size_t len);

/**
 * @brief Does @p list contain @p cat, as a whole item?
 *
 * "game" does not match "games". A prefix match here would put apps in
 * categories nobody assigned them to.
 *
 * @param list A category list; may be empty. Must already be normalised —
 *             an item over KAPP_CATEGORY_ITEM_MAX truncates into the
 *             internal scratch buffer and could then falsely match.
 * @param cat  The category to look for; empty never matches.
 * @return true when the list carries exactly that item.
 */
bool kapp_cat_has(const char *list, const char *cat);

/**
 * @brief Clean a list up: lowercase, trimmed, deduplicated, capped.
 *
 * These bytes arrive from a file on a card or from a console line, so the
 * reader does not trust them. Items that are empty, over-long or beyond
 * KAPP_CATEGORIES_MAX are dropped and logged - never truncated in silence.
 *
 * @param in  The list to clean; NULL is treated as empty.
 * @param out Receives the cleaned list, always terminated.
 * @param len Size of @p out; KAPP_CATEGORY_MAX is always enough.
 */
void kapp_cat_normalise(const char *in, char *out, size_t len);

/** @brief "KCAT": the override table's blob. */
#define KAPP_CAT_BLOB_MAGIC   0x5441434BU
/** @brief Of the blob layout, not of the categories in it. */
#define KAPP_CAT_BLOB_VERSION 1U
/**
 * @brief How many apps may carry an override.
 *
 * As many as the launcher can show (KODE_APP_LIST_MAX) and the apps region
 * can hold. The table lives in PSRAM, taken once by kapp_cat_init(), and in
 * one NVS blob of about 6 KB at its largest. A row past this is refused
 * (kapp_cat_set()); the apps without one keep the category their own
 * manifest gives them. A table stored with fewer rows loads as it is.
 */
#define KAPP_CAT_ROWS_MAX     64U

/** @brief One assignment: an app's identity, and what it is filed under. */
typedef struct {
    char project[32];                  /**< esp_app_desc_t.project_name.   */
    char category[KAPP_CATEGORY_MAX];  /**< The packed list.               */
} kapp_cat_row_t;                      /* 96 bytes */

/** @brief The blob at its largest: header plus a full table. */
#define KAPP_CAT_BLOB_LEN     (12U + (KAPP_CAT_ROWS_MAX * sizeof(kapp_cat_row_t)))

/**
 * @brief Write @p count rows into @p buf as the stored blob.
 *
 * @param rows  The table.
 * @param count How many rows; must not exceed KAPP_CAT_ROWS_MAX.
 * @param buf   Receives the blob.
 * @param len   Size of @p buf.
 * @return Bytes written, or 0 when @p buf is too small or @p count too big.
 */
size_t kapp_cat_serialise(const kapp_cat_row_t *rows, uint8_t count,
                          uint8_t *buf, size_t len);

/**
 * @brief Read a stored blob back into a table.
 *
 * All-or-nothing: a blob whose magic, version, count or length disagree
 * yields KODE_ERROR and touches neither output. A half-read table looks
 * like it worked, which is the failure worth refusing. The rows themselves
 * are validated too, not just the header: every row's project[] and
 * category[] must carry a NUL within their array, checked before either
 * output is touched, or the whole blob is rejected the same way. Every
 * row's category is then normalised on the way in, so a list that reaches
 * kapp_cat_has() is always normalised regardless of what NVS actually held.
 *
 * @param buf   The blob.
 * @param len   Bytes in @p buf.
 * @param rows  Receives up to KAPP_CAT_ROWS_MAX rows.
 * @param count Receives how many were read.
 * @return KODE_OK, or KODE_ERROR.
 */
kode_status_t kapp_cat_deserialise(const uint8_t *buf, size_t len,
                                   kapp_cat_row_t *rows, uint8_t *count);

/**
 * @brief Bring the override table up: NVS, the lock, and the stored rows.
 *
 * Calls khal_nvs_init() itself rather than relying on whoever else may have.
 * A missing key is an empty table, not a failure; a blob that does not add
 * up is an empty table and a loud log, never a partial load.
 *
 * @return KODE_OK when the table is usable, even if it is empty.
 */
kode_status_t kapp_cat_init(void);

/**
 * @brief What the device says this app is, if it says anything.
 *
 * Safe to call from any task: the table is guarded.
 *
 * @param project esp_app_desc_t.project_name - the identity, not a file.
 * @param cats    Receives the stored list; untouched when there is none.
 * @param len     Size of @p cats.
 * @return true when an override exists and was written to @p cats.
 */
bool kapp_cat_resolve(const char *project, char *cats, size_t len);

/**
 * @brief Assign categories to @p project, replacing whatever was stored.
 *
 * Normalises first: a list that cleans up to nothing is refused rather than
 * stored empty, because clearing is what kapp_cat_clear() is for.
 *
 * Writes flash. Call it from AO_AppMgr, the table's only writer by design;
 * the lock inside kapp_cat.c is what protects the ones that arrive anyway.
 *
 * @param project The app's project name.
 * @param cats    A comma-separated list.
 * @return KODE_OK, KODE_ERROR on a bad list, KODE_DENIED when the table is full.
 */
kode_status_t kapp_cat_set(const char *project, const char *cats);

/**
 * @brief Drop @p project's row; the app returns to its manifest.
 * @param project The app's project name.
 * @return KODE_OK, including when there was no row to drop.
 */
kode_status_t kapp_cat_clear(const char *project);

/** @brief How many rows the table holds. @return 0..KAPP_CAT_ROWS_MAX. */
uint8_t kapp_cat_count(void);

/**
 * @brief Read row @p idx out of the table, for listing it.
 *
 * @param idx     0..kapp_cat_count()-1.
 * @param project Receives the project name.
 * @param plen    Size of @p project.
 * @param cats    Receives the list.
 * @param clen    Size of @p cats.
 * @return false when @p idx is past the end.
 */
bool kapp_cat_row(uint8_t idx, char *project, size_t plen,
                  char *cats, size_t clen);

#ifdef __cplusplus
}
#endif

#endif /* KAPP_CAT_H_ */
