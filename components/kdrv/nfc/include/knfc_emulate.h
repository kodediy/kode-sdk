/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_emulate.h
 * @brief What the Dot presents when it emulates an NFC-A card, and what a window came to.
 */
#ifndef KNFC_EMULATE_H_
#define KNFC_EMULATE_H_

#include <stdbool.h>
#include <stdint.h>

#include "knfc_tag.h"   /* KNFC_UID_MAX */

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The longest NDEF message an emulated card carries, terminator
 *        excluded.
 *
 * Mirrors KQP_NFC_EMU_NDEF_MAX (kqp_signals.h): the card travels to AO_Nfc in
 * an event, so the two must agree. It is kept here as well because a driver
 * cannot reach into kqp.
 */
#define KNFC_EMU_NDEF_MAX  96U

/**
 * @brief The longest ready-made NDEF message an emulated card carries.
 *
 * The Type-2 image's NDEF area is 144 bytes, and the message's TLV takes a
 * tag, a one-byte length and a terminator, which leaves 141. 140 keeps one
 * byte of margin and stays below the 255 where the TLV length grows to three
 * bytes. knfc_emu_img.h holds it to the area with a _Static_assert.
 */
#define KNFC_EMU_RAW_MAX   140U

/**
 * @brief The identity, and optional NDEF content, to present as an NFC-A card.
 *
 * Two ways to fill it, and the driver reads @c uid_len to tell them apart:
 *
 * - **Clone an identity.** @c uid_len is 4, 7 or 10 and @c atqa is coherent
 *   with it (the two size bits of SENS_RES match the UID length): the card is
 *   presented verbatim - same UID, same ATQA, same SAK - which is what a UID
 *   reader (an access control, a Flipper) expects. A SAK of 0x08 makes a
 *   phone attempt a Crypto1 authentication that nothing answers, exactly as
 *   the original card does; a UID reader does not care.
 * - **Present a link or text.** @c uid_len is 0 (or @c atqa does not match a
 *   real card): a 7-byte NFC Forum Type-2 tag is synthesised - SAK 0x00, so a
 *   phone does a plain Type-2 READ and settles - carrying @c ndef.
 *
 * @c ndef is NUL-terminated. Empty means a default Text record. @c uri true
 * makes it a URI record (a phone offers to open it); false a Text record.
 *
 * Or carry a ready-made NDEF message in @c raw - a MIME vCard a phone offers
 * to add as a contact, a Wi-Fi WSC credential it offers to join. With
 * @c raw_len not 0 the message is wrapped in the Type-2 TLV as it is, and
 * @c uri and @c ndef are ignored; the identity rules above still apply. A
 * zeroed card has @c raw_len 0 and behaves as it always did.
 */
typedef struct {
    uint8_t  uid[KNFC_UID_MAX];   /**< NFCID1 to clone; ignored when @c
                                   *   uid_len is 0.                        */
    uint8_t  uid_len;             /**< 4, 7 or 10 to clone; 0 to synthesise. */
    uint16_t atqa;                /**< SENS_RES to clone; 0 to derive.       */
    uint8_t  sak;                 /**< SEL_RES to clone.                     */
    bool     uri;                 /**< @c ndef is a URI rather than text.    */
    char     ndef[KNFC_EMU_NDEF_MAX + 1U]; /**< The message; "" for default. */
    uint8_t  raw_len;                  /**< Bytes of @c raw; 0 = not used.    */
    uint8_t  raw[KNFC_EMU_RAW_MAX];    /**< A ready-made NDEF message, without
                                         *   its TLV.                           */
} knfc_emu_card_t;

/**
 * @brief What an emulation window has seen since it opened.
 *
 * @c field and @c selected are levels - true while a reader's field is on the
 * antenna and while it holds us selected. @c sessions and @c reads only ever
 * grow: @c sessions counts activations (a SELECT completed), @c reads counts
 * Type-2 READs actually served. A window with @c sessions high and @c reads
 * zero is a link that dies before the first data exchange - what metal behind
 * the antenna does - and one that serves READs is a link that works.
 */
typedef struct {
    bool     active;    /**< An emulation window is open.                    */
    bool     field;     /**< A reader's field is on the antenna now.         */
    bool     selected;  /**< A reader has us selected now.                   */
    uint32_t sessions;  /**< Activations since the window opened.            */
    uint32_t reads;     /**< Type-2 READs served since the window opened.    */
} knfc_emu_stat_t;

#ifdef __cplusplus
}
#endif

#endif /* KNFC_EMULATE_H_ */
