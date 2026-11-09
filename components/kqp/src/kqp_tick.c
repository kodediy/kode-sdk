/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_tick.c
 * @brief The QP tick: started once at boot, and stopped while the board sleeps.
 */
#include <stdatomic.h>

#include "kqp.h"
#include "kqp_bsp.h"

#include "qpc.h"
#include "kqp_config.h"   /* KQP_TICKS_PER_SEC */
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"

Q_DEFINE_THIS_MODULE("kqp_tick")

/** @brief Microseconds in a second. */
#define TICK_US_PER_S 1000000ULL

static atomic_bool s_resting;       /**< The tick is stopped, or about to be. */
static atomic_uint s_rests;         /**< Rests that ended; wraps. */

/* khal_timer_periodic() calls back in task context, so the task-level tick. */
static void kqp_tick_cb(void *arg)
{
    (void)arg;
    QTIMEEVT_TICK_X(0U, (void *)0); /* sender used for QS tracing only */
}

void kqp_tick_start(void)
{
    kode_status_t const st = khal_timer_periodic(
        &kqp_tick_cb, (void *)0, TICK_US_PER_S / KQP_TICKS_PER_SEC);

    /* A framework with no clock would sit there looking healthy while every
     * QTimeEvt in the system never fired, so this is worth dying over. */
    Q_ASSERT_ID(100, KODE_OK == st);

    KLOG_I("kqp", "QF up: tick=%uHz, assert hook installed",
           (unsigned)KQP_TICKS_PER_SEC);
}

void kqp_tick_rest(bool rest)
{
    if ( atomic_load(&s_resting) == rest )
    {
        return;
    }

    if ( rest )
    {
        /* Before the tick stops, so the supervisor never sees a stopped tick
         * with its check still on. */
        atomic_store(&s_resting, true);
        if ( khal_timer_periodic_run(&kqp_tick_cb, false) != KODE_OK )
        {
            /* Still ticking: the check goes back on rather than stay blind. */
            atomic_store(&s_resting, false);
            KLOG_W("kqp", "the QP tick would not stop");
        }
        else
        {
            khal_watch_rest(true);
        }
    }
    else
    {
        if ( khal_timer_periodic_run(&kqp_tick_cb, true) != KODE_OK )
        {
            /* Every QTimeEvt in the system is dead without it. */
            Q_ASSERT_ID(110, false);
        }
        /* After the tick runs again, and the count first: the supervisor
         * checks the flag before the count. */
        /* cppcheck-suppress misra-c2012-17.3 ; C11 stdatomic's own function, which cppcheck's library does not declare */
        atomic_fetch_add(&s_rests, 1U);
        atomic_store(&s_resting, false);
        khal_watch_rest(false);
    }
}

bool kqp_tick_resting(void)
{
    return atomic_load(&s_resting);
}

uint32_t kqp_tick_rests(void)
{
    return (uint32_t)atomic_load(&s_rests);
}
