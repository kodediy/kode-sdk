/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio_note.c
 * @brief The handover note's word, and the rule that decides whether the C5
 *        is reset at the first link.
 */
#include "kdrv_radio_note.h"

#include <stddef.h>

#include "kdrv_radio.h"

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rule; kdrv_radio.c calls it */
uint32_t kradio_note_pack(uint32_t target)
{
    return (((uint32_t)KRADIO_NOTE_MAGIC) << KRADIO_NOTE_MAGIC_SHIFT) |
           (target & KRADIO_NOTE_TARGET_MASK);
}

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rule; kdrv_radio.c calls it */
kradio_note_t kradio_note_unpack(uint32_t word)
{
    kradio_note_t note;

    note.magic  = word >> KRADIO_NOTE_MAGIC_SHIFT;
    note.target = word & KRADIO_NOTE_TARGET_MASK;
    return note;
}

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rule; kdrv_radio.c calls it */
bool kradio_note_is_update(const kradio_note_t *note, bool reset_was_sw)
{
    if ( (NULL == note) || !reset_was_sw )
    {
        return false;
    }
    return (KRADIO_NOTE_MAGIC == note->magic)
           && ((uint32_t)KDRV_RADIO_NOTE_UPDATE == note->target);
}

/* cppcheck-suppress misra-c2012-8.7 ; a file of its own so the host suite can compile the rule; kdrv_radio.c calls it */
bool kradio_note_keeps_c5(const kradio_note_t *note, bool reset_was_sw,
                          bool role_is_os, bool wants_wifi)
{
    uint32_t mine = (uint32_t)KDRV_RADIO_NOTE_APP;

    if ( NULL == note )
    {
        return false;
    }
    if ( KRADIO_NOTE_MAGIC != note->magic )
    {
        return false;
    }
    /* The new image is confirmed by the first request that reaches it, and a
     * reset before that is what gives it up: no wish outranks that. */
    if ( kradio_note_is_update(note, reset_was_sw) )
    {
        return true;
    }
    if ( !reset_was_sw || !wants_wifi )
    {
        return false;
    }
    if ( role_is_os )
    {
        mine = (uint32_t)KDRV_RADIO_NOTE_OS;
    }
    /* A target that is neither role never equals either of them. */
    return note->target == mine;
}
