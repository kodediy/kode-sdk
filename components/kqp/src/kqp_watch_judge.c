/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_watch_judge.c
 * @brief The watchdog's judge: arithmetic over samples, no port, no task.
 */
#include "kqp_watch_judge.h"

uint32_t kqp_watch_age_ms(uint32_t now_ms, uint32_t then_ms)
{
    uint32_t const diff = now_ms - then_ms;
    uint32_t age = diff;

    if ( (int32_t)diff < 0 )
    {
        age = 0U;
    }
    return age;
}

/**
 * @brief Judge one Active Object and update its slot.
 *
 * @param slot       The AO's slot, updated in place.
 * @param s          What the supervisor read from it.
 * @param now_ms     The tick of this pass.
 * @param timeout_ms The hang threshold.
 * @param kind       Receives OK, STUCK or STARVED.
 * @return How long the AO has been in that state, in ms; 0 for OK.
 */
static uint32_t judge_one(KqpWatchSlot *slot, KqpWatchSample const *s,
                          uint32_t now_ms, uint32_t timeout_ms,
                          KqpWatchKind *kind)
{
    uint32_t elapsed = 0U;

    *kind = KQP_WATCH_OK;

    if ( !s->valid )
    {
        slot->streaking = false;
        slot->last_count = 0U;
    }
    else
    {
        if ( (s->count & KQP_WATCH_BUSY_BIT) != 0U )
        {
            /* Inside an event: only its age counts, and no streak runs. */
            slot->streaking = false;
            elapsed = kqp_watch_age_ms(now_ms, s->start_ms);
            if ( elapsed > timeout_ms )
            {
                *kind = KQP_WATCH_STUCK;
            }
        }
        else if ( 0U == s->waiting )
        {
            slot->streaking = false;
        }
        else
        {
            /* Between events with work queued: a streak that survives while
             * the counter stands still. */
            if ( (!slot->streaking) || (slot->last_count != s->count) )
            {
                slot->streaking = true;
                slot->streak_start_ms = now_ms;
            }
            elapsed = kqp_watch_age_ms(now_ms, slot->streak_start_ms);
            if ( elapsed > timeout_ms )
            {
                *kind = KQP_WATCH_STARVED;
            }
        }
        slot->last_count = s->count;
    }

    if ( KQP_WATCH_OK == *kind )
    {
        elapsed = 0U;
    }
    return elapsed;
}

KqpWatchVerdict kqp_watch_judge(KqpWatchSlot slots[],
                                KqpWatchSample const samples[],
                                uint8_t n,
                                uint32_t now_ms,
                                uint32_t timeout_ms)
{
    KqpWatchVerdict v;
    KqpWatchKind kind;
    uint8_t i;

    v.kind = KQP_WATCH_OK;
    v.index = 0U;
    v.over_ms = 0U;
    v.sig = 0U;
    v.waiting = 0U;

    for ( i = 0U; i < n; ++i )
    {
        uint32_t const over = judge_one(&slots[i], &samples[i], now_ms,
                                        timeout_ms, &kind);
        /* Strictly greater: a tie stays with the lower index. */
        if ( (KQP_WATCH_OK != kind) && (over > v.over_ms) )
        {
            v.kind = kind;
            v.index = i;
            v.over_ms = over;
            v.sig = samples[i].sig;
            v.waiting = samples[i].waiting;
        }
    }
    return v;
}

bool kqp_watch_rested(bool resting, uint32_t rests, uint32_t *seen,
                      KqpWatchSlot slots[], uint8_t n)
{
    bool stop = resting;

    if ( !stop && (rests != *seen) )
    {
        uint8_t i;

        for ( i = 0U; i < n; ++i )
        {
            slots[i].last_count = 0U;
            slots[i].streak_start_ms = 0U;
            slots[i].streaking = false;
        }
        *seen = rests;
        stop = true;
    }
    return stop;
}

KqpWatchSlow kqp_watch_slow(uint32_t *seen, KqpWatchSample const *s,
                            uint32_t limit_ms)
{
    KqpWatchSlow r;
    uint32_t delta = 0U;

    r.log = false;
    r.ms = 0U;
    r.sig = 0U;
    r.count = 0U;

    if ( s->valid )
    {
        delta = s->slow_seq - *seen;
        *seen = s->slow_seq;
    }
    if ( (0U != delta) && (s->slow_ms > limit_ms) )
    {
        r.log = true;
        r.ms = s->slow_ms;
        r.sig = s->slow_sig;
        r.count = delta;
    }
    return r;
}
