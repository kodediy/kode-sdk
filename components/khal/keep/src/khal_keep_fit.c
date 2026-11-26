/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_keep_fit.c
 * @brief khal_keep_fit.h: the kept block's place and the heap's cut, in plain arithmetic.
 */
#include "khal_keep_fit.h"

#define KEEP_HOLD_CLASS (32U * 1024U) /**< The heap's size class between 1 and 2 MB. */

/* A piece on a class boundary is taken whole: nothing of the top is left free. */
_Static_assert(0U == (KHAL_KEEP_HOLD_BYTES % KEEP_HOLD_CLASS),
               "the held piece is a whole number of the heap's size classes");

/**
 * @brief The size class's width for a request: 1 << (its highest set bit - KHAL_KEEP_SL_LOG2).
 * @param bytes A size of KHAL_KEEP_SMALL or more.
 * @return The width, a power of two.
 */
static size_t keep_class(size_t bytes)
{
    size_t   rest = bytes;
    uint32_t msb  = 0U;

    while ( 1U < rest )
    {
        rest >>= 1U;
        ++msb;
    }
    return (size_t)1U << (msb - KHAL_KEEP_SL_LOG2);
}

uintptr_t khal_keep_target(uintptr_t top)
{
    return (top - KHAL_KEEP_BYTES - KHAL_KEEP_END_GAP) & ~(uintptr_t)KHAL_KEEP_LINE;
}

size_t khal_keep_taken(size_t request_bytes)
{
    size_t round = KHAL_KEEP_ALIGN;

    if ( KHAL_KEEP_SMALL <= request_bytes )
    {
        round = keep_class(request_bytes);
    }
    return (request_bytes + round - 1U) & ~(round - 1U);
}

size_t khal_keep_fill(size_t block_bytes)
{
    if ( (KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MAX) >= block_bytes )
    {
        return 0U;
    }
    /* What the fill and its header may take, rounded down to its own class: the heap takes it whole. */
    size_t const room = block_bytes - KHAL_KEEP_HOLD_BYTES - KHAL_KEEP_SLACK_MIN - KHAL_KEEP_HEADER;
    size_t const up   = khal_keep_taken(room);

    if ( up == room )
    {
        return room;
    }
    return up - keep_class(room);
}

bool khal_keep_ready(size_t block_bytes)
{
    return ((KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MIN) <= block_bytes)
        && ((KHAL_KEEP_HOLD_BYTES + KHAL_KEEP_SLACK_MAX) >= block_bytes);
}

bool khal_keep_covers(uintptr_t top, uintptr_t from, size_t held_bytes)
{
    uintptr_t const target = khal_keep_target(top);

    return (from <= target) && ((from + held_bytes) >= (target + KHAL_KEEP_BYTES));
}
