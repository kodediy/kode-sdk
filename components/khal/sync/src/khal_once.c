/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_once.c
 * @brief khal_once_run() and khal_once_try(): the one-shot election.
 */
#include "khal_once.h"

#include "khal_time.h"

/**
 * @brief Take part in the election.
 *
 * @param once The one-shot.
 * @return true when the caller won and must run the body, then leave the
 *         word as the body's outcome says; false when the work is done.
 */
static bool once_enter(khal_once_t *once)
{
    bool won  = false;
    bool done = false;

    while (!won && !done) {
        int state = atomic_load(&once->state);

        if (state == KHAL_ONCE_DONE) {
            done = true;
        }
        else if ((state == KHAL_ONCE_IDLE)
                 && atomic_compare_exchange_strong(&once->state, &state, KHAL_ONCE_RUNNING)) {
            won = true;
        }
        else {
            khal_delay_ms(KHAL_ONCE_POLL_MS);
        }
    }
    return won;
}

void khal_once_run(khal_once_t *once, khal_once_body_t body)
{
    if (once_enter(once)) {
        body();
        atomic_store(&once->state, KHAL_ONCE_DONE);
    }
}

bool khal_once_try(khal_once_t *once, khal_once_try_body_t body)
{
    bool ok = true;

    if (once_enter(once)) {
        ok = body();
        if (ok) {
            atomic_store(&once->state, KHAL_ONCE_DONE);
        }
        else {
            atomic_store(&once->state, KHAL_ONCE_IDLE);
        }
    }
    return ok;
}
