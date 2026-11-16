/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_slot_seq.h
 * @brief Which kodeOS slot otadata selected last, from its two entries.
 *
 * Each entry names a slot by its sequence number, `(seq - 1) % slots`, and
 * the higher number is the later selection. A slot whose latest entry is
 * older than the running slot's was not the last one asked for: whatever
 * state that entry keeps belongs to an earlier update.
 */
#ifndef KDRV_SLOT_SEQ_H_
#define KDRV_SLOT_SEQ_H_

#include <stdbool.h>
#include <stdint.h>

#define KSLOT_SEQ_ENTRIES 2U   /**< otadata's entries, one per flash sector. */

/**
 * @brief Whether the slot @p other was selected after the slot @p running.
 * @param seq     Each entry's sequence number.
 * @param valid   Whether each entry is one: written, and its CRC holds.
 * @param other   The other slot's index, 0 for ota_0.
 * @param running The running slot's index.
 * @param slots   How many OTA slots the table has.
 * @return true when the latest entry naming @p other is newer than every
 *         entry naming @p running; false when none names it.
 */
static inline bool kslot_seq_newer(const uint32_t seq[KSLOT_SEQ_ENTRIES],
                                   const bool valid[KSLOT_SEQ_ENTRIES],
                                   uint32_t other, uint32_t running, uint32_t slots)
{
    uint32_t last_other   = 0U;
    uint32_t last_running = 0U;

    for ( uint32_t i = 0U; (KSLOT_SEQ_ENTRIES > i) && (0U != slots); ++i )
    {
        if ( valid[i] && (0U != seq[i]) )
        {
            uint32_t const slot = (seq[i] - 1U) % slots;

            if ( (slot == other) && (seq[i] > last_other) )
            {
                last_other = seq[i];
            }
            if ( (slot == running) && (seq[i] > last_running) )
            {
                last_running = seq[i];
            }
        }
    }
    return last_other > last_running;
}

#endif /* KDRV_SLOT_SEQ_H_ */
