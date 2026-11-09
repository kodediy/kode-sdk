/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file fake_neighbour_aos.c
 * @brief Stand-ins for the Active Objects an AO under test posts to.
 */
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "fakes.h"
#include "kaos.h"
#include "qf_support.h"

#define NEIGHBOUR_QUEUE_LEN 16

/* Nothing is a stand-in any more. AO_Log and AO_Display were both defined
 * here once; log_ao.c and display_ao.c are in the host build now, so the real
 * symbols are in the binary and a second definition would not link. What is
 * left is the useful half: their queues are still the record of what somebody
 * posted to them, which is what "did it tell the display?" is asked with. */

#if CONFIG_KODE_LOWPOWER
/** @brief How many answers the scan keeps while a case asks for the other kind. */
#define LP_STASH_READY  4U


static LowPowerReadyEvt  s_ready[LP_STASH_READY];   /**< Answers passed over, oldest first. */
static unsigned          s_ready_n;                 /**< How many of them.                  */
static LowPowerWakeAtEvt s_wake;                    /**< The newest wake note passed over.  */
static bool              s_wake_have;               /**< @c s_wake means something.         */
static unsigned          s_radio_done;              /**< Radio-done notes passed over.      */
static unsigned          s_rest;                    /**< Rest requests passed over.         */
static uint8_t           s_rest_src;                /**< The source of the newest.          */
#endif

/** @brief Empty @p ao's queue, releasing what it holds. */
static void drain(QActive *ao)
{
    const QEvt *e;

    if ( NULL == ao->eQueue )
    {
        ao->eQueue = xQueueCreate(NEIGHBOUR_QUEUE_LEN, sizeof(QEvt *));
        return;
    }
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Empty @p ao's queue, making it the static kind QP's margin reads.
 *
 * A post with a margin reads the free count from the queue QP lays out in the
 * AO's osObject, so a heap queue answers "no room" to every one. The AOs whose
 * doors post with a margin get the proper one. */
static void drain_static(QActive *ao)
{
    const QEvt *e;

    if ( (NULL != ao->eQueue) && ((void *)ao->eQueue != (void *)&ao->osObject.queue) )
    {
        while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
        {
            QF_gc(e);
        }
        ao->eQueue = NULL;   /* a heap queue left by someone else: replaced */
    }
    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

void fake_neighbours_reset(void)
{
    /* Unconditionally, because drain() CREATES the queue when there is none -
     * and that is the only thing standing between an AO under test posting to
     * a neighbour and "assert failed: xQueueGenericSend queue.c:936
     * (pxQueue)". Guarding these calls on the queue already existing means it
     * never does. */
    drain(AO_Display);
    /* AO_Log takes every post with a margin (a line, a byte, a level), and a
     * margin needs the static kind of queue. */
    drain_static(AO_Log);
    /* AO_IoExp is real in this binary too, and AO_Buttons posts to it from
     * cases that run long before its own file brings it up. Its queue is the
     * record of those asks, drained between cases like the others. */
    drain(AO_IoExp);
    /* AO_Touch likewise: the console's `tp` and `test component=tp` post
     * to it from test_console_tp.c, which sorts before the file that
     * brings it up. */
    drain(AO_Touch);
    /* AO_Radio is real in this binary as well, and AO_Net's initial
     * transition asks it for the WiFi state from test_net_ao.c, which sorts
     * before the file that brings the radio up. */
    drain_static(AO_Radio);
    /* AO_Ir for the same reason again: the console's `test component=ir` and
     * the walk under `test` post to it from test_console_power.c, which sorts
     * before test_ir_ao.c. */
    drain(AO_Ir);
    /* AO_Imu and AO_Haptic for the same reason once more: the console posts
     * to both of them from files that sort before either AO's own. */
    drain(AO_Imu);
    drain(AO_Haptic);
    /* AO_Power for the same reason: AO_Autotest's cases ask it for the
     * `pmic` and `gauge` rows from a file that sorts before any that brings
     * it up. Its ship door posts with a margin, so the queue is the static
     * kind. */
    drain_static(AO_Power);
#if CONFIG_KODE_LOWPOWER
    /* AO_LowPower hears every console line (lowpower_input_note()), and the
     * console's cases run long before, or without, the file that brings it
     * up. The queue is the one QACTIVE_START would make, so that file finds
     * it in place. */
    qf_support_give_queue(AO_LowPower);
    drain(AO_LowPower);
    s_ready_n   = 0U;
    s_wake_have = false;
    s_radio_done = 0U;
    s_rest       = 0U;
#endif
}

unsigned fake_neighbour_display_posts(void)
{
    return (NULL != AO_Display->eQueue)
         ? (unsigned)uxQueueMessagesWaiting(AO_Display->eQueue) : 0U;
}

unsigned fake_neighbour_log_posts(void)
{
    return (NULL != AO_Log->eQueue)
         ? (unsigned)uxQueueMessagesWaiting(AO_Log->eQueue) : 0U;
}

unsigned fake_neighbour_ioexp_posts(void)
{
    return (NULL != AO_IoExp->eQueue)
         ? (unsigned)uxQueueMessagesWaiting(AO_IoExp->eQueue) : 0U;
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Move everything AO_LowPower's queue holds into the stash.
 *
 * The queue holds answers and wake notes in the order they were posted, and a
 * case reads them in its own order, so neither reader may throw away the
 * other's. Anything else queued there is discarded.
 */
static void lp_scan(void)
{
    const QEvt *e = NULL;

    while ( (NULL != AO_LowPower->eQueue)
           && (xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE) )
    {
        if ( ((QSignal)LOWPOWER_READY_SIG == e->sig) && (LP_STASH_READY > s_ready_n) )
        {
            s_ready[s_ready_n] = *(const LowPowerReadyEvt *)e;
            s_ready_n++;
        }
        else if ( (QSignal)LOWPOWER_RADIO_DONE_SIG == e->sig )
        {
            s_radio_done++;
        }
        else if ( (QSignal)LOWPOWER_REST_SIG == e->sig )
        {
            s_rest++;
            s_rest_src = ((const LowPowerReqEvt *)e)->src;
        }
        else if ( (QSignal)LOWPOWER_WAKE_AT_SIG == e->sig )
        {
            s_wake      = *(const LowPowerWakeAtEvt *)e;
            s_wake_have = true;
        }
        else
        {
            /* Not a thing a case reads. */
        }
        QF_gc(e);
    }
}

bool fake_lowpower_ready_take(LowPowerReadyEvt *out)
{
    bool found;

    lp_scan();
    found = (0U < s_ready_n);
    if ( found )
    {
        *out = s_ready[0];
        s_ready_n--;
        for ( unsigned i = 0U; i < s_ready_n; i++ )
        {
            s_ready[i] = s_ready[i + 1U];
        }
    }
    return found;
}

unsigned fake_lowpower_radio_done_count(void)
{
    unsigned n;

    lp_scan();
    n = s_radio_done;
    s_radio_done = 0U;
    return n;
}

unsigned fake_lowpower_rest_count(uint8_t *src)
{
    unsigned n;

    lp_scan();
    n = s_rest;
    if ( (0U < n) && (NULL != src) )
    {
        *src = s_rest_src;
    }
    s_rest = 0U;
    return n;
}

bool fake_lowpower_wake_take(LowPowerWakeAtEvt *out)
{
    bool found;

    lp_scan();
    found       = s_wake_have;
    s_wake_have = false;
    if ( found )
    {
        *out = s_wake;
    }
    return found;
}
#endif
