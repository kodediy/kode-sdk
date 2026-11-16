/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_knock.c
 * @brief A knock's steps: the claim, the post, the mark of a refusal, the look and the payment.
 */
#include "radio_knock.h"

#include <stddef.h>

/** @brief A bit of a knock's word: a knock is pending, queued or owed. */
#define KNOCK_PENDING    0x01U
/** @brief A bit of a knock's word: the pending knock has no event in the queue and nobody about to post one. */
#define KNOCK_OWED       0x02U
/** @brief The bit of a knock's word where what the first post carried begins. */
#define KNOCK_ARG_SHIFT  8U

void radio_knock_init(radio_knock_t *k, const radio_knock_door_t *door)
{
    atomic_store(&k->word, 0U);
    k->door = door;
}

void radio_knock_post_begin(radio_knock_run_t *run, uint8_t arg)
{
    run->at      = (uint8_t)RADIO_KNOCK_AT_CLAIM;
    run->payer   = false;
    run->refused = false;
    run->held    = KNOCK_PENDING | ((unsigned int)arg << KNOCK_ARG_SHIFT);
}

void radio_knock_pay_begin(radio_knock_run_t *run)
{
    run->at      = (uint8_t)RADIO_KNOCK_AT_RECLAIM;
    run->payer   = true;
    run->refused = false;
    run->held    = 0U;
}

bool radio_knock_step(radio_knock_t *k, radio_knock_run_t *run)
{
    switch ( (radio_knock_at_t)run->at )
    {
        case RADIO_KNOCK_AT_CLAIM: {
            /* A word that holds anything is a knock that waits or is owed:
             * the Active Object has yet to take it, and reads the state
             * after it does, so this post is told by it. */
            unsigned int idle = 0U;

            if ( atomic_compare_exchange_strong(&k->word, &idle, run->held) )
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_SEND;
            }
            else
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_DONE;
            }
            break;
        }
        case RADIO_KNOCK_AT_SEND: {
            if ( k->door->post(k->door->ctx) )
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_DONE;
            }
            else
            {
                run->refused = true;
                run->at      = (uint8_t)RADIO_KNOCK_AT_OWE;
            }
            break;
        }
        case RADIO_KNOCK_AT_OWE: {
            /* The mark is made only on the word this run holds. A knock that
             * was taken since is not this run's to owe: marking it would
             * raise again what was heard already. The word holds nothing
             * else, so one step makes the whole mark and no later step of
             * this run writes a word it does not hold. */
            unsigned int held = run->held;

            if ( !atomic_compare_exchange_strong(&k->word, &held, run->held | KNOCK_OWED) )
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_DONE;
            }
            else if ( run->payer )
            {
                /* The Active Object is inside a dispatch and its queue is
                 * not empty: the next event it handles pays again. */
                run->at = (uint8_t)RADIO_KNOCK_AT_DONE;
            }
            else
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_LOOK;
            }
            break;
        }
        case RADIO_KNOCK_AT_LOOK: {
            /* Looked at after the mark, not before. A queue that still has
             * no room holds an event the Active Object has yet to take, and
             * the payment before that event finds the mark. A queue with
             * room may belong to an Active Object that emptied it before the
             * mark and waits: nobody else would post, so this run does. */
            if ( k->door->room(k->door->ctx) )
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_RECLAIM;
            }
            else
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_DONE;
            }
            break;
        }
        case RADIO_KNOCK_AT_RECLAIM: {
            /* One of the runs that find the knock owed takes it back, and
             * that one posts: the others find it queued and are done. */
            /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
            unsigned int const before = atomic_fetch_and(&k->word, ~KNOCK_OWED);

            if ( 0U != (before & KNOCK_OWED) )
            {
                run->held = before & ~KNOCK_OWED;
                run->at   = (uint8_t)RADIO_KNOCK_AT_SEND;
            }
            else
            {
                run->at = (uint8_t)RADIO_KNOCK_AT_DONE;
            }
            break;
        }
        default: {
            run->at = (uint8_t)RADIO_KNOCK_AT_DONE;
            break;
        }
    }
    return (uint8_t)RADIO_KNOCK_AT_DONE != run->at;
}

bool radio_knock_post(radio_knock_t *k, uint8_t arg)
{
    radio_knock_run_t run;

    radio_knock_post_begin(&run, arg);
    while ( radio_knock_step(k, &run) )
    {
        /* each step is one read or write of the word or the queue */
    }
    return !run.refused;
}

void radio_knock_pay(radio_knock_t *k)
{
    radio_knock_run_t run;

    radio_knock_pay_begin(&run);
    while ( radio_knock_step(k, &run) )
    {
        /* each step is one read or write of the word or the queue */
    }
}

bool radio_knock_take(radio_knock_t *k, uint8_t *arg)
{
    unsigned int const word    = atomic_exchange(&k->word, 0U);
    bool const         pending = 0U != (word & KNOCK_PENDING);

    if ( pending && (NULL != arg) )
    {
        *arg = (uint8_t)(word >> KNOCK_ARG_SHIFT);
    }
    return pending;
}

bool radio_knock_pending(radio_knock_t *k)
{
    return 0U != (atomic_load(&k->word) & KNOCK_PENDING);
}
