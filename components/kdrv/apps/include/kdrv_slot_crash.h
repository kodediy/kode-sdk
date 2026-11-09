/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_slot_crash.h
 * @brief The word that counts a kodeOS slot's crashes in a row across resets.
 */
#ifndef KDRV_SLOT_CRASH_H_
#define KDRV_SLOT_CRASH_H_

#include <stdbool.h>
#include <stdint.h>

/* The subtype is kept because the slot that reads the word may not be the one that crashed:
 * an update that panics before it confirms comes back in the slot before it. */
#define KSLOT_CRASH_MAGIC       0x4B43UL  /**< "KC": a count left by kodeOS.    */
#define KSLOT_CRASH_MAGIC_SHIFT 16U       /**< Where the magic sits in the word. */
#define KSLOT_CRASH_SLOT_SHIFT  8U        /**< Where the subtype sits in it.     */
#define KSLOT_CRASH_FIELD       0xFFUL    /**< The subtype's and the count's bits. */

/**
 * @brief The word that stores @p count for the slot @p subtype.
 *
 * @param subtype The running partition's subtype.
 * @param count   The crashes; only its low eight bits are kept.
 * @return The word.
 */
static inline uint32_t kslot_crash_pack(uint32_t subtype, uint32_t count)
{
    return (KSLOT_CRASH_MAGIC << KSLOT_CRASH_MAGIC_SHIFT)
           | ((subtype & KSLOT_CRASH_FIELD) << KSLOT_CRASH_SLOT_SHIFT)
           | (count & KSLOT_CRASH_FIELD);
}

/**
 * @brief Read a stored word for the slot @p subtype.
 *
 * @param word    The word. After a power-on it is noise, and the magic is what
 *                makes that harmless, with the reset reason read beside it.
 * @param subtype The running partition's subtype.
 * @param count   Receives the crashes when the word is this slot's.
 * @return true when the word is this slot's.
 */
static inline bool kslot_crash_read(uint32_t word, uint32_t subtype, uint32_t *count)
{
    bool const mine = (kslot_crash_pack(subtype, 0U) == (word & ~KSLOT_CRASH_FIELD));

    if ( mine )
    {
        *count = word & KSLOT_CRASH_FIELD;
    }
    return mine;
}

#endif /* KDRV_SLOT_CRASH_H_ */
