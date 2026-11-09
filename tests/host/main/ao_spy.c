/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ao_spy.c
 * @brief See ao_spy.h.
 */
#include "ao_spy.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "kqp_config.h"
#include "probe_ao.h"
#include "qf_support.h"

typedef struct {
    QActive super;
} Spy;

static Spy         l_spy;
static const QEvt *s_held[AO_SPY_QUEUE_LEN];
static unsigned    s_held_n;

/** @brief The spy never runs, so its state machine only has to be valid. */
static QState Spy_idle(Spy * const me, QEvt const * const e)
{
    (void)me;
    (void)e;
    return Q_SUPER(&QHsm_top);
}

static QState Spy_initial(Spy * const me, void const * const par)
{
    (void)par;
    return Q_TRAN(&Spy_idle);
}

QActive *ao_spy(void)
{
    static bool s_up;

    if ( !s_up )
    {
        qf_support_init();

        QActive_ctor(&l_spy.super, Q_STATE_CAST(&Spy_initial));

        /* KODE_PRIO_USER_LOW is one of the three levels kqp deliberately
         * leaves free, so the spy cannot collide with an AO under test. */
        l_spy.super.prio  = KODE_PRIO_USER_LOW;
        l_spy.super.pthre = KODE_PRIO_USER_LOW;
        QActive_register_(&l_spy.super);

        /* The queue QP posts into. Created here rather than by QACTIVE_START,
         * because starting it would create a task and bring a scheduler into
         * a tier that exists precisely to do without one. */
        /* Static, as QACTIVE_START makes it, so a post with a margin reads
         * the same free count it reads on the board. */
        static uint8_t s_storage[AO_SPY_QUEUE_LEN * sizeof(QEvt *)];
        l_spy.super.eQueue = xQueueCreateStatic(AO_SPY_QUEUE_LEN, sizeof(QEvt *),
                                                s_storage,
                                                &l_spy.super.osObject.queue);
        s_up = true;
    }
    return &l_spy.super;
}

void ao_spy_subscribe(enum_t sig)
{
    QActive_subscribe(ao_spy(), sig);
}

unsigned ao_spy_count(void)
{
    if ( NULL == l_spy.super.eQueue )
    {
        return 0U;
    }
    return (unsigned)uxQueueMessagesWaiting(l_spy.super.eQueue);
}

const QEvt *ao_spy_take(void)
{
    const QEvt *e = NULL;

    if ( (NULL != l_spy.super.eQueue)
        && (xQueueReceive(l_spy.super.eQueue, &e, 0U) == pdTRUE) )
    {
        if ( AO_SPY_QUEUE_LEN > s_held_n )
        {
            s_held[s_held_n++] = e;   /* keep the reference until drain */
        }
        return e;
    }
    return NULL;
}

enum_t ao_spy_next_sig(void)
{
    const QEvt * const e = ao_spy_take();

    return (NULL != e) ? (enum_t)e->sig : 0;
}

void ao_spy_drain(void)
{
    const QEvt *e;

    while ( (e = ao_spy_take()) != NULL )
    {
        /* keep taking; ao_spy_take() records the reference */
    }
    for ( unsigned i = 0U; i < s_held_n; ++i )
    {
        QF_gc(s_held[i]);
    }
    s_held_n = 0U;
}

void ao_spy_release(void)
{
    for ( unsigned i = 0U; i < s_held_n; ++i )
    {
        QF_gc(s_held[i]);
    }
    s_held_n = 0U;
}

/* ---- A second asker -------------------------------------------------------
 * Not registered: a post needs only the queue, and the priority it carries
 * is what a per-asker door is keyed by. The probe's level (probe_ao.h),
 * because no AO under test asks at it: every owner level is taken in this
 * binary, and the probe asks no door.
 */

#define OTHER_QUEUE_LEN 8

static Spy l_other;

QActive *ao_spy_other(void)
{
    static bool    s_up;
    static uint8_t s_storage[OTHER_QUEUE_LEN * sizeof(QEvt *)];

    if ( !s_up )
    {
        qf_support_init();
        QActive_ctor(&l_other.super, Q_STATE_CAST(&Spy_initial));
        l_other.super.prio  = PROBE_AO_PRIO;
        l_other.super.pthre = PROBE_AO_PRIO;
        l_other.super.eQueue = xQueueCreateStatic(OTHER_QUEUE_LEN, sizeof(QEvt *),
                                                  s_storage,
                                                  &l_other.super.osObject.queue);
        s_up = true;
    }
    return &l_other.super;
}

const QEvt *ao_spy_other_take(void)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(ao_spy_other()->eQueue, &e, 0U) != pdTRUE )
    {
        e = NULL;
    }
    return e;
}

void ao_spy_other_drain(void)
{
    const QEvt *e;

    while ( (e = ao_spy_other_take()) != NULL )
    {
        QF_gc(e);
    }
}
