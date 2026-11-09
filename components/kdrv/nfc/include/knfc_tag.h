/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_tag.h
 * @brief NFC Forum tags: what sits between a record and a radio.
 */
#ifndef KNFC_TAG_H_
#define KNFC_TAG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "knfc_ndef.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest NFCID1 there is. */
#define KNFC_UID_MAX  10U

/*
 * What knfc_tag_ndef_read() and knfc_tag_ndef_write() answer when the tag is
 * there but the exchange did not succeed, by name. knfc_tag.c returns the
 * first three by these names; the lost-tag status is the transport's
 * KODE_TIMEOUT passed up, and the transport sits below this header.
 */
#define KNFC_NDEF_NO_MESSAGE  KODE_ERROR         /**< A read: no readable message on the tag. A write: any other failure. */
#define KNFC_NDEF_LOCKED      KODE_DENIED        /**< A tag type this cannot read or write (a Mifare Classic whose key is not known), or a read-only tag. */
#define KNFC_NDEF_TOO_LONG    KODE_NO_RESOURCES  /**< A read: the message does not fit. A write: it does not fit the tag, nothing written. */

/** @brief What kind of tag activated. */
typedef enum {
    KNFC_TAG_UNKNOWN = 0,  /**< NFC-A, and nothing this can read.      */
    KNFC_TAG_T2T,          /**< Type-2: NTAG and its relatives.        */
    KNFC_TAG_T4T,          /**< Type-4: ISO-DEP, a file system.        */
    KNFC_TAG_MIFARE,       /**< Mifare Classic: recognised, not read.  */
} knfc_tag_type_t;

/** @brief One activated tag, as it identified itself. */
typedef struct {
    uint8_t         uid[KNFC_UID_MAX];  /**< NFCID1.                    */
    uint8_t         uid_len;            /**< 4, 7 or 10.                */
    uint16_t        atqa;               /**< SENS_RES.                  */
    uint8_t         sak;                /**< SEL_RES.                   */
    knfc_tag_type_t type;               /**< Read off @c sak.           */
} knfc_tag_t;

/**
 * @brief A human name for the card model, from its ATQA and SAK.
 *
 * The anticollision bytes narrow a card to a family - Mifare Classic 1K/4K,
 * Ultralight/NTAG, a Type-4 card - which is what a reader shows before it
 * knows anything else. It is a best guess from two bytes, not a chip
 * interrogation, so it names families rather than exact part numbers.
 *
 * @param atqa The SENS_RES.
 * @param sak  The SEL_RES.
 * @return A static string, never NULL.
 */
const char *knfc_tag_model(uint16_t atqa, uint8_t sak);

/**
 * @brief Fill @p out from whatever the reader has activated.
 *
 * The type is derived here, and the order matters:
 *
 * - SAK bit 5 set means the tag speaks ISO-DEP: a Type-4.
 * - SAK bit 6 set means NFC-DEP, peer-to-peer, which this slice does not
 *   carry.
 * - SAK bit 3 set with bit 5 clear is the Mifare Classic family - SAK 0x08
 *   is a 1K, 0x18 a 4K. It answers anticollision like any NFC-A tag and
 *   nothing else without Crypto1 authentication, so it is named rather than
 *   guessed at: reporting it as a Type-2 makes every read fail with no
 *   explanation, which is what this enumerant exists to prevent.
 * - SAK 0x00 is a Type-2: NTAG, Ultralight and their relatives.
 *
 * That is the whole of the protocol knowledge the driver deliberately does
 * not have.
 *
 * @param out Out: the tag. Written only on KODE_OK.
 * @return KODE_OK, KODE_DENIED for a NULL @p out or an absent reader,
 *         KODE_ERROR when nothing is activated.
 */
kode_status_t knfc_tag_identify(knfc_tag_t *out);

/**
 * @brief Read the activated tag's NDEF message as text.
 *
 * A URI record comes back as its full URI, prefix expanded; a Text record as
 * its text. A tag carrying neither is an error rather than an empty string,
 * because those two mean different things to a caller.
 *
 * @param tag  What knfc_tag_identify() returned.
 * @param out  Out: the message, NUL-terminated.
 * @param cap  How many bytes @p out holds, terminator included.
 * @param n    Out: how many bytes were written, terminator excluded. May be
 *             NULL.
 * @param kind Out: which record it was. Written only on KODE_OK. May be NULL.
 * @return KODE_OK; KODE_DENIED for a NULL argument or a zero @p cap;
 *         KNFC_NDEF_LOCKED for a tag type this cannot read; KODE_TIMEOUT
 *         when the tag stopped answering; KNFC_NDEF_TOO_LONG when the message
 *         does not fit @p cap; KNFC_NDEF_NO_MESSAGE when there is no readable
 *         message on it.
 */
kode_status_t knfc_tag_ndef_read(const knfc_tag_t *tag, char *out, size_t cap,
                                 size_t *n, knfc_ndef_kind_t *kind);

/**
 * @brief Write one NDEF record to the activated tag.
 *
 * The size is checked against what the tag says it can hold BEFORE the first
 * byte is written. A half-written tag is worse than an untouched one: it
 * reads as corrupt to everything, and the caller cannot tell from the error
 * which it got.
 *
 * @param tag  What knfc_tag_identify() returned.
 * @param text The URI or the text.
 * @param uri  true to write a URI record, false for a Text record.
 * @return KODE_OK; KODE_DENIED for a NULL argument; KNFC_NDEF_LOCKED for a
 *         tag type this cannot write, or a tag that says it is read-only;
 *         KNFC_NDEF_TOO_LONG when the message does not fit the tag, in which
 *         case nothing was written; KODE_TIMEOUT when it stopped answering,
 *         in which case something may have been; KNFC_NDEF_NO_MESSAGE
 *         otherwise.
 */
kode_status_t knfc_tag_ndef_write(const knfc_tag_t *tag, const char *text,
                                  bool uri);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_TAG_H_ */
