/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_cert.h
 * @brief The FCC and CE bench's requests: maximum-drive carriers and field listening.
 */
#ifndef KODE_CERT_H_
#define KODE_CERT_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"
#include "kqp.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The longest listen window of kode_cert_nfc_field_read(), in ms. */
#define KODE_CERT_NFC_FIELD_MS_MAX   200U

/** @brief The longest window of kode_cert_rfid_field_read(), in ms. */
#define KODE_CERT_RFID_FIELD_MS_MAX  150U

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/** @brief Answers the carrier calls (NFC_CERT_RSP_SIG, RFID_CERT_RSP_SIG): @c rc. */
typedef StatusRspEvt kode_cert_carrier_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers kode_cert_nfc_field_read() (NFC_FIELD_RSP_SIG): @c st and the window's
 *        @c min, @c mean, @c peak, @c mod, @c ext and @c n, all zero unless @c st is KODE_OK.
 */
typedef NfcFieldRspEvt kode_cert_nfc_field_answer_t;

/* cppcheck-suppress misra-c2012-2.3 ; an answer's name for the apps that read it, never used inside the SDK */
/**
 * @brief Answers kode_cert_rfid_field_read() (RFID_FIELD_RSP_SIG): @c st, the window's
 *        @c min, @c mean, @c peak, @c ac and @c n, and the @c points written to the wave.
 */
typedef RfidFieldRspEvt kode_cert_rfid_field_answer_t;

/* ---- NFC (13.56 MHz) ---- */

/** @brief What kode_cert_nfc_carrier() does with the 13.56 MHz field. */
typedef enum {
    KODE_CERT_NFC_CARRIER_OFF = 0, /**< Drop the field.                            */
    KODE_CERT_NFC_CARRIER_ON  = 1, /**< A bare, unmodulated maximum-drive carrier. */
    KODE_CERT_NFC_CARRIER_MOD = 2, /**< A continuously ASK-modulated one.          */
} kode_cert_nfc_carrier_mode_t;

/**
 * @brief Hold or drop the NFC reader's field at maximum drive, for the FCC bench (15.225).
 *
 * Does not wait for a tag; the field stays up until KODE_CERT_NFC_CARRIER_OFF,
 * from any asker: the bench's all-off drops a carrier whoever raised it.
 * @param me   The asking AO; NFC_CERT_RSP_SIG answers it: KODE_OK when the field is
 *             up (mode 0: down). KODE_ERROR when no reader is fitted or the
 *             carrier would not come up. KODE_DENIED when the reader is busy.
 * @param mode A kode_cert_nfc_carrier_mode_t.
 * @return As kode_sys.h's requests; a @p mode outside 0..2 is KODE_DENIED.
 *         So is every call in a build without CONFIG_KODEOS_NFC_CERT.
 */
kode_status_t kode_cert_nfc_carrier(QActive *me, int32_t mode);

/**
 * @brief Listen on the 13.56 MHz antenna for @p ms with the reader's field off, and describe what was there.
 * @param me The asking AO; NFC_FIELD_RSP_SIG answers it: KODE_OK with the window.
 *           KODE_IO_ERROR when no reader is fitted (looked for at boot only).
 *           KODE_DENIED while another request holds the reader; that passes.
 *           KODE_ERROR for a window that failed.
 * @param ms How long to listen, 1..KODE_CERT_NFC_FIELD_MS_MAX.
 * @return As kode_sys.h's requests; an @p ms of 0 or over the maximum is KODE_DENIED.
 */
kode_status_t kode_cert_nfc_field_read(QActive *me, uint32_t ms);

/* ---- RFID (125 kHz LF) ---- */

/** @brief What kode_cert_rfid_carrier() does with the 125 kHz field. */
typedef enum {
    KODE_CERT_RFID_CARRIER_OFF = 0, /**< Drop the field.                            */
    KODE_CERT_RFID_CARRIER_ON  = 1, /**< A bare, unmodulated maximum-drive carrier. */
    KODE_CERT_RFID_CARRIER_MOD = 2, /**< A continuously OOK-modulated one.          */
} kode_cert_rfid_carrier_mode_t;

/**
 * @brief Hold or drop the LF front end's 125 kHz field at maximum drive, boosted, for the FCC bench (15.209).
 *
 * KODE_OK in the answer means the carrier was commanded, not that RF radiated.
 * It stays up until KODE_CERT_RFID_CARRIER_OFF, from any asker: the bench's
 * all-off drops a carrier whoever raised it.
 * @param me   The asking AO; RFID_CERT_RSP_SIG answers it: KODE_ERROR when the
 *             driver refused the bring-up. KODE_DENIED when the front end is busy.
 * @param mode A kode_cert_rfid_carrier_mode_t.
 * @return As kode_sys.h's requests; a @p mode outside 0..2 is KODE_DENIED.
 *         So is every call in a build without CONFIG_KODEOS_NFC_CERT.
 */
kode_status_t kode_cert_rfid_carrier(QActive *me, int32_t mode);

/**
 * @brief Read the 125 kHz demod line for @p ms, carrier off, and describe it.
 * @note LENT: the worker writes @p wave in place while the window runs, so keep it
 *       valid and unchanged until RFID_FIELD_RSP_SIG arrives, and never write it
 *       before. A window that outlives its deadline is answered KODE_TIMEOUT once
 *       the worker has let go of it. A refused window never writes it.
 * @param me   The asking AO; RFID_FIELD_RSP_SIG answers it: KODE_OK with the window and
 *             the @c points written to @p wave. KODE_DENIED while the front end is busy
 *             (a read, a clone, another capture, the production row, a carrier, or a
 *             window whose worker has not reported); that passes. KODE_ERROR for a
 *             window the front end could not read. No front end is not detected.
 * @param wave Where the waveform goes, squeezed to @p cap points by bucket peak, or NULL for statistics only.
 * @param cap  Points @p wave holds; ignored when @p wave is NULL.
 * @param ms   How long to read, 1..KODE_CERT_RFID_FIELD_MS_MAX.
 * @return As kode_sys.h's requests; an @p ms of 0 or over the maximum, or a @p wave with a zero @p cap, is KODE_DENIED.
 */
kode_status_t kode_cert_rfid_field_read(QActive *me, uint16_t *wave, size_t cap, uint32_t ms);

#ifdef __cplusplus
}
#endif

#endif /* KODE_CERT_H_ */
