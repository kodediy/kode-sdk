/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_note.h
 * @brief The handover note a parked C5 leaves across a restart, and the one rule that reads it.
 */
#ifndef KDRV_RADIO_NOTE_H_
#define KDRV_RADIO_NOTE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** "KP": a note left by a park. */
#define KRADIO_NOTE_MAGIC       0x4B50U

/** Where the magic sits in the word. */
#define KRADIO_NOTE_MAGIC_SHIFT 16U

/** The bits of the word that hold the target. */
#define KRADIO_NOTE_TARGET_MASK 0xFFFFUL

/** The note, decoded from its word. */
typedef struct {
    uint32_t magic;     /**< KRADIO_NOTE_MAGIC when a note is there.            */
    uint32_t target;    /**< A kdrv_radio_note_t: the role the jump goes to.    */
} kradio_note_t;

/**
 * @brief The word that stores a note for @p target.
 *
 * @param target A kdrv_radio_note_t.
 * @return The magic in the high half and the target in the low half.
 */
uint32_t kradio_note_pack(uint32_t target);

/**
 * @brief Decode a stored word. Any word decodes; whether it is a note is the
 *        magic's to say.
 *
 * @param word What the register held.
 * @return The two halves of @p word.
 */
kradio_note_t kradio_note_unpack(uint32_t word);

/**
 * @brief Whether the note says the C5 runs an update it has not confirmed.
 *
 * True when the note's magic is there, its target is KDRV_RADIO_NOTE_UPDATE
 * and the reset was a software one; after any other reset the register's word
 * says nothing.
 *
 * @param note         The note as read at boot.
 * @param reset_was_sw The reset reason is a software reset.
 * @return true for an update's note.
 */
bool kradio_note_is_update(const kradio_note_t *note, bool reset_was_sw);

/**
 * @brief Whether the C5 stays running at this boot's first link.
 *
 * True for an update's note (kradio_note_is_update()), whatever the role and
 * the wish. Otherwise true only when every condition holds: the note's magic
 * is there, its target is a known role and this firmware's own, the reset was
 * a software one, and this boot wants Wi-Fi. Any other case resets the C5.
 *
 * @param note         The note as read at boot (before it was erased).
 * @param reset_was_sw The reset reason is a software reset.
 * @param role_is_os   This firmware's role is the OS (false: an app).
 * @param wants_wifi   This boot wants Wi-Fi.
 * @return true to keep the C5 as it is, false to pulse its reset line.
 */
bool kradio_note_keeps_c5(const kradio_note_t *note, bool reset_was_sw,
                          bool role_is_os, bool wants_wifi);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_RADIO_NOTE_H_ */
