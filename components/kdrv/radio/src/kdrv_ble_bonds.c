/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_bonds.c
 * @brief The count of bonded peers and the number of the change that left it, in one word.
 */
#include "kdrv_ble_bonds.h"

#include <stdatomic.h>

/** @brief The bits of the word that hold the count. */
#define BONDS_COUNT_MASK   0xFFU
/** @brief The bit of the word where the number of the last change begins. */
#define BONDS_CHANGE_SHIFT 8U

/** @brief The count kept, and above it the number of the change of the store that left it. */
static atomic_uint s_word;

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, which is a file this check does not join to this one */
bool kdrv_ble_bonds_changed(uint8_t n)
{
    /* Changes of the store so far; written inside the store's lock alone. */
    static uint32_t changes = 0U;

    /* The number goes up with every change, so the word differs from every
     * ticket taken before this call even when the count is the same. */
    changes++;
    unsigned int const word = ((unsigned int)changes << BONDS_CHANGE_SHIFT) | (unsigned int)n;
    unsigned int const old  = atomic_exchange(&s_word, word);

    return (old & BONDS_COUNT_MASK) != (unsigned int)n;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, which is a file this check does not join to this one */
uint32_t kdrv_ble_bonds_begin(void)
{
    return (uint32_t)atomic_load(&s_word);
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, which is a file this check does not join to this one */
bool kdrv_ble_bonds_settle(uint32_t ticket, uint8_t n)
{
    unsigned int       seen = (unsigned int)ticket;
    unsigned int const word = (seen & ~BONDS_COUNT_MASK) | (unsigned int)n;
    bool               moved = false;

    /* The word still is the ticket only when no change of the store came
     * between the two: the count was made of a store that stood still. */
    if ( atomic_compare_exchange_strong(&s_word, &seen, word) )
    {
        moved = ((unsigned int)ticket & BONDS_COUNT_MASK) != (unsigned int)n;
    }
    return moved;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, which is a file this check does not join to this one */
uint8_t kdrv_ble_bonds_kept(void)
{
    return (uint8_t)(atomic_load(&s_word) & BONDS_COUNT_MASK);
}
