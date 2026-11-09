/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_kody_anchor.c
 * @brief Anchors and crossings. See kui_kody_anchor.h.
 *
 * Integer arithmetic in 32 bits: a 16-bit frame plus an anchor can pass
 * 65535, and the answer must not wrap where the pass does not.
 */
#include "kui_kody_anchor.h"

uint16_t kui_kody_next_anchor(uint16_t frame, uint16_t anchor, uint16_t frames)
{
    uint16_t next = 0U;

    if ( 0U == frames )
    {
        next = 0U;
    }
    else if ( 0U == anchor )
    {
        next = (uint16_t)(frames - 1U);
    }
    else
    {
        uint32_t const after = (((uint32_t)frame / (uint32_t)anchor) + 1U) * (uint32_t)anchor;
        if ( (uint32_t)frames > after )
        {
            next = (uint16_t)after;
        }
        else
        {
            next = 0U;
        }
    }
    return next;
}

bool kui_kody_crossed(uint16_t prev, uint16_t now, uint16_t target, uint16_t frames)
{
    bool crossed = false;

    if ( (target >= frames) || (prev == now) )
    {
        crossed = false;
    }
    else if ( now > prev )
    {
        crossed = (target > prev) && (target <= now);
    }
    else
    {
        /* Through the end of the pass: everything after prev, then 0 .. now. */
        crossed = (target > prev) || (target <= now);
    }
    return crossed;
}
