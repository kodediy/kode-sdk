/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_emu_img.h
 * @brief The bytes an emulated NFC-A card presents, built from a card request.
 */
#ifndef KNFC_EMU_IMG_H_
#define KNFC_EMU_IMG_H_

#include <stdint.h>

#include "knfc_emulate.h"
#include "knfc_ndef.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Pages in the emulated Type-2 image; 40 x 4 = 160 bytes. */
#define KNFC_EMU_T2T_PAGES  40U
/** @brief Bytes in the emulated Type-2 image. */
#define KNFC_EMU_T2T_BYTES  (KNFC_EMU_T2T_PAGES * 4U)
/** @brief Where the NDEF area starts: page 4, after the UID pages and the CC. */
#define KNFC_EMU_T2T_NDEF_OFF  16U
_Static_assert((KNFC_EMU_RAW_MAX + KNFC_TLV_WRAP_BYTES)
               <= (KNFC_EMU_T2T_BYTES - KNFC_EMU_T2T_NDEF_OFF),
               "the longest raw message must fit the image's NDEF area");

/**
 * @brief The identity to present and the Type-2 image to serve.
 *
 * @c nfcid / @c nfcid_len / @c sens_res / @c sak are what rfalListenStart()
 * loads into passive-target memory; @c sens_res is on-the-wire order, least
 * significant byte first. @c t2t is what a Type-2 READ returns, @c t2t_len
 * bytes of it.
 */
typedef struct {
    uint8_t  nfcid[KNFC_UID_MAX];        /**< The presented NFCID1.          */
    uint8_t  nfcid_len;                  /**< 4, 7 or 10.                    */
    uint8_t  sens_res[2];                /**< SENS_RES, LSB first.           */
    uint8_t  sak;                        /**< SEL_RES.                       */
    uint8_t  t2t[KNFC_EMU_T2T_BYTES];    /**< The Type-2 image.              */
    uint16_t t2t_len;                    /**< Bytes of @c t2t that are real. */
} knfc_emu_image_t;

/**
 * @brief Build the presented identity and Type-2 image from @p card.
 *
 * A coherent identity (@c uid_len 4/7/10 with an @c atqa whose size bits
 * match) is presented verbatim. Otherwise a 7-byte Type-2 tag is synthesised:
 * an NXP-style UID (0x04 first, never the 0x88 cascade tag), SENS_RES 0x0044,
 * SEL_RES 0x00, and the caller's message - or a default Text record when it is
 * empty - written into the NDEF area. The UID pages and their BCC bytes are
 * always patched to the presented UID so a phone reads a consistent tag.
 *
 * @param card What to present.
 * @param out  Out: the identity and image. Written only on KODE_OK.
 * @return KODE_OK; KODE_DENIED for a NULL argument or a @c uid_len that is not
 *         0, 4, 7 or 10; KODE_NO_RESOURCES when the message does not fit the
 *         NDEF area.
 */
kode_status_t knfc_emu_image_build(const knfc_emu_card_t *card,
                                   knfc_emu_image_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_EMU_IMG_H_ */
