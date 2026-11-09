/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_ndef.h
 * @brief NDEF records: the format a phone understands, encoded and parsed.
 */
#ifndef KNFC_NDEF_H_
#define KNFC_NDEF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The NDEF message TLV, the one this reads and writes. */
#define KNFC_TLV_NDEF        0x03U
/** @brief The terminator TLV. */
#define KNFC_TLV_TERMINATOR  0xFEU
/** @brief What knfc_ndef_wrap_tlv() adds to a message: the tag, a one-byte
 *         length and the terminator. */
#define KNFC_TLV_WRAP_BYTES  3U

/** @brief Which record a message turned out to carry. */
typedef enum {
    KNFC_NDEF_URI = 0,   /**< A 'U' record: a link.        */
    KNFC_NDEF_TEXT,      /**< A 'T' record: plain text.    */
} knfc_ndef_kind_t;

/**
 * @brief Encode @p uri as one NDEF URI record.
 *
 * The longest matching prefix is replaced by its one-byte code, longest
 * first: matching "https://" before "https://www." would spend four bytes to
 * say nothing.
 *
 * @param uri  The link, NUL-terminated.
 * @param out  Out: the record.
 * @param cap  How many bytes @p out holds.
 * @param used Out: how many were written.
 * @return KODE_OK, KODE_DENIED for a NULL argument, KODE_NO_RESOURCES when
 *         the record does not fit @p cap or the payload passes 255 bytes,
 *         which is what a short record can express.
 */
kode_status_t knfc_ndef_make_uri(const char *uri, uint8_t *out, size_t cap,
                                 size_t *used);

/**
 * @brief Encode @p text as one NDEF Text record.
 *
 * @param text NUL-terminated, UTF-8.
 * @param lang IANA language code; NULL means "en".
 * @param out  Out: the record.
 * @param cap  How many bytes @p out holds.
 * @param used Out: how many were written.
 * @return KODE_OK, KODE_DENIED for a NULL argument or a language code past
 *         63 bytes - the status byte has six bits for it - and
 *         KODE_NO_RESOURCES when it does not fit.
 */
kode_status_t knfc_ndef_make_text(const char *text, const char *lang,
                                  uint8_t *out, size_t cap, size_t *used);

/**
 * @brief Wrap a message in an NDEF TLV with its terminator.
 *
 * @param msg     The encoded record or records.
 * @param msg_len How many bytes of it.
 * @param out     Out: TLV header, message, terminator.
 * @param cap     How many bytes @p out holds.
 * @param used    Out: how many were written.
 * @return KODE_OK, KODE_DENIED for a NULL argument, KODE_NO_RESOURCES when
 *         it does not fit or @p msg_len passes 254, which is where the
 *         three-byte length format begins and nothing here writes one.
 */
kode_status_t knfc_ndef_wrap_tlv(const uint8_t *msg, size_t msg_len,
                                 uint8_t *out, size_t cap, size_t *used);

/**
 * @brief Find the NDEF message TLV in @p data.
 *
 * Walks the TLVs from the start, stepping over lock-control and
 * memory-control TLVs, which is what a Type-2 tag puts before the message.
 *
 * @param data    The bytes read from the tag.
 * @param len     How many.
 * @param off     Out: where the message starts inside @p data.
 * @param msg_len Out: how long it claims to be, clamped to what @p data
 *                actually holds.
 * @return KODE_OK, KODE_DENIED for a NULL argument, KODE_ERROR when there is
 *         no NDEF TLV - a terminator before one, or a length running past
 *         the buffer.
 */
kode_status_t knfc_ndef_find_tlv(const uint8_t *data, size_t len, size_t *off,
                                 size_t *msg_len);

/**
 * @brief Read the first URI or Text record out of a message.
 *
 * A record whose type is neither is stepped over rather than refused: a tag
 * may carry several, and the one that can be shown is the answer. A UTF-16
 * Text record is stepped over too - dumping its bytes would produce a string
 * that looks like a result and is not.
 *
 * @param data The bytes read from the tag, TLVs included.
 * @param len  How many.
 * @param out  Out: the URI or the text, NUL-terminated.
 * @param cap  How many bytes @p out holds, terminator included.
 * @param kind Out: which it was. May be NULL.
 * @return KODE_OK, KODE_DENIED for a NULL argument or a zero @p cap,
 *         KODE_NO_RESOURCES when the answer does not fit @p cap, KODE_ERROR
 *         when there is no record this can show.
 */
kode_status_t knfc_ndef_parse(const uint8_t *data, size_t len, char *out,
                              size_t cap, knfc_ndef_kind_t *kind);

/**
 * @brief Does @p s read as a link rather than as text?
 *
 * Biased towards text on purpose: writing text where a link was meant is
 * harmless, and writing a broken link makes a phone open rubbish. A known
 * prefix, or "www.", settles it; otherwise it must have no spaces and at
 * least two letters after its last dot, which is what keeps "3.14" and
 * "192.168.1.1" out.
 *
 * @param s NUL-terminated, or NULL.
 * @return true when it should be written as a URI record.
 */
bool knfc_ndef_looks_like_uri(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_NDEF_H_ */
