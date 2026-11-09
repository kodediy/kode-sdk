/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kwav.h
 * @brief Forty-four bytes of WAV header, and a refusal.
 */
#ifndef KWAV_H_
#define KWAV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What kwav_build() writes, and the least a file can be. */
#define KWAV_HEADER_BYTES     44U

/** @brief The only sample rate this board plays or records. */
#define KWAV_RATE_HZ       48000U
/** @brief The only sample width. */
#define KWAV_BITS             16U
/** @brief The only channel count: the ES8311 is mono. */
#define KWAV_CHANNELS          1U

/** @brief Why a file was refused, or that it was not. */
typedef enum {
    KWAV_OK = 0,       /**< 48 kHz, 16-bit, mono, with data behind it.      */
    KWAV_BAD_RIFF,     /**< Not a RIFF/WAVE file at all.                    */
    KWAV_TRUNCATED,    /**< A chunk runs off the end of the buffer; for a data chunk, the format is good. */
    KWAV_NO_FMT,       /**< No format chunk before the data.                */
    KWAV_NO_DATA,      /**< No data chunk.                                  */
    KWAV_BAD_FORMAT,   /**< Not uncompressed PCM.                           */
    KWAV_BAD_RATE,     /**< Some other sample rate.                         */
    KWAV_BAD_BITS,     /**< Some other sample width.                        */
    KWAV_BAD_CHANNELS, /**< Not mono.                                       */
} kwav_reason_t;

/** @brief Where the samples are, once a file has been accepted. */
typedef struct {
    uint32_t rate;       /**< As declared. KWAV_RATE_HZ when accepted.      */
    uint32_t data_off;   /**< Byte offset of the first sample.              */
    uint32_t frames;     /**< How many mono frames follow.                  */
    uint16_t bits;       /**< As declared.                                  */
    uint16_t channels;   /**< As declared.                                  */
} kwav_info_t;

/**
 * @brief Write the canonical header for @p frames mono 16-bit frames.
 *
 * @param frames How many frames will follow it.
 * @param out    Where to write. Exactly KWAV_HEADER_BYTES are written.
 * @param cap    How many bytes @p out holds.
 * @return KODE_OK, or KODE_DENIED for a NULL @p out or a @p cap under
 *         KWAV_HEADER_BYTES.
 */
kode_status_t kwav_build(uint32_t frames, uint8_t *out, size_t cap);

/**
 * @brief Read @p len bytes of @p buf as a WAV header.
 *
 * @p out is filled in whenever the file parsed as RIFF/WAVE with a format
 * chunk, even when the format is one this board refuses - so a caller can
 * report what the file actually is rather than only that it was rejected.
 *
 * @param buf The start of the file. At least the header must be present; the
 *            samples need not be.
 * @param len How many bytes @p buf holds.
 * @param out Where to write what was found. May be NULL.
 * @return KWAV_OK when the file is 48 kHz, 16-bit, mono PCM, or which
 *         property failed. KWAV_TRUNCATED with @p out->data_off set means the
 *         format is good and the data runs past @p len; @p out->frames is the
 *         declared count.
 */
kwav_reason_t kwav_parse(const uint8_t *buf, size_t len, kwav_info_t *out);

/**
 * @brief What to call a refusal in a log line or a console record.
 *
 * @param r The reason.
 * @return A short lower-case phrase. Never NULL.
 */
const char *kwav_reason_text(kwav_reason_t r);

#ifdef __cplusplus
}
#endif

#endif /* KWAV_H_ */
