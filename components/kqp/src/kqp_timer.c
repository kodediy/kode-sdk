/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_timer.c
 * @brief The guard-timer helper every Active Object with a worker shares.
 */
#include "kqp.h"

bool kqp_timer_stale(QTimeEvt * const te, bool live)
{
    bool stale = false;

    if ( !QTimeEvt_disarm(te) )
    {
        /* Not counting any more. Either it never was, or it expired and
         * posted - and @p live is the only thing that tells those apart. */
        stale = live;
    }
    return stale;
}
