/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file qf_support.c
 * @brief See qf_support.h.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "qf_support.h"

#include "unity.h"

#include "qpc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "kqp_config.h"
#include "kqp_signals.h"

/* Two pools rather than kqp's four. The tiers exist on the board to stop a
 * 4 KB firmware chunk from consuming a block that a log line needed; here
 * nothing competes, so one block size that fits the ordinary events is
 * simpler and cannot run out for the wrong reason.
 *
 * The jumbo tier is the exception, and it is not about size: how many of its
 * blocks are free is a fact AO_Storage ACTS on - an icon travels as three
 * events or not at all - so a case has to be able to take some and see the
 * refusal. It therefore has the board's own count, not a comfortable one. */
#define POOL_BLOCK  352U
#define POOL_COUNT   32U

/* Wide enough for the widest ordinary event on a host, where a pointer is
 * eight bytes: the scheduler's request that carries a name, an expression and
 * a console line. An event wider than the block would be served from the
 * jumbo pool and four of them would exhaust it. */
_Static_assert(sizeof(SchedSetReqEvt) <= POOL_BLOCK,
               "the scheduler's set request must fit the ordinary block");
_Static_assert(sizeof(SchedEntryEvt) <= POOL_BLOCK,
               "the scheduler's entry must fit the ordinary block");

/* What QMPool_init() will round the block up to, worked out here so the
 * storage below holds KQP_JUMBO_POOL_COUNT blocks and not one more: the count
 * is the point of this pool, so it may not be an accident of padding. */
#define JUMBO_BLOCK (((sizeof(AppDataIconEvt) + sizeof(void *) - 1U) \
                      / sizeof(void *)) * sizeof(void *))

_Static_assert(sizeof(MqttMsgEvt) <= POOL_BLOCK, "an MQTT message fits the ordinary block");
_Static_assert(sizeof(MqttPubEvt) <= POOL_BLOCK, "an MQTT publish fits the ordinary block");

static QSubscrList s_subscr[MAX_PUB_SIG];
static uint64_t    s_pool[(POOL_BLOCK * POOL_COUNT) / sizeof(uint64_t)];
static uint64_t    s_jumbo[(JUMBO_BLOCK * KQP_JUMBO_POOL_COUNT)
                           / sizeof(uint64_t)];

_Static_assert(((JUMBO_BLOCK * KQP_JUMBO_POOL_COUNT) % sizeof(uint64_t)) == 0U,
               "the jumbo storage must be exactly the blocks it stands for");

void qf_support_init(void)
{
    static bool s_up;

    if ( s_up )
    {
        return;
    }
    s_up = true;

    QF_init();
    QActive_psInit(s_subscr, Q_DIM(s_subscr));
    /* In order of strictly increasing block size, as QF requires. */
    QF_poolInit(s_pool, sizeof(s_pool), POOL_BLOCK);
    QF_poolInit(s_jumbo, sizeof(s_jumbo), JUMBO_BLOCK);
}

#define AO_QUEUE_LEN 32

void qf_support_give_queue(QActive *ao)
{
    if ( (NULL != ao) && (NULL == ao->eQueue) )
    {
        /* Static, as QACTIVE_START makes it: a post with a margin reads the
         * free count from the queue QP lays out in osObject, so a queue made
         * any other way answers "no room" to every one. The storage is taken
         * once per AO and kept for the life of the binary. */
        uint8_t *const store = malloc(AO_QUEUE_LEN * sizeof(QEvt *));

        if ( NULL != store )
        {
            ao->eQueue = xQueueCreateStatic(AO_QUEUE_LEN, sizeof(QEvt *),
                                            store, &ao->osObject.queue);
        }
    }
}

/** @brief Posts qf_support_queue_stuff() makes at most: more than a queue given here holds. */
#define STUFF_LIMIT (2U * (uint32_t)AO_QUEUE_LEN)

void qf_support_margin_queue(QActive *ao)
{
    const QEvt *held = NULL;

    while ( (NULL != ao->eQueue) && (xQueueReceive(ao->eQueue, &held, 0U) == pdTRUE) )
    {
        QF_gc(held);
    }
    if ( (void *)ao->eQueue != (void *)&ao->osObject.queue )
    {
        ao->eQueue = NULL;
    }
    qf_support_give_queue(ao);
}

bool qf_support_queue_stuff(QActive *ao, QEvt const *filler)
{
    uint32_t n = 0U;

    while ( (STUFF_LIMIT > n) && QACTIVE_POST_X(ao, filler, 0U, NULL) )
    {
        n++;
    }
    return STUFF_LIMIT > n;
}

#define BACKGROUND_MAX 4

static QActive *s_background[BACKGROUND_MAX];
static unsigned s_background_n;

void qf_support_background(QActive *ao)
{
    for ( unsigned i = 0U; i < s_background_n; ++i )
    {
        if ( s_background[i] == ao )
        {
            return;
        }
    }
    if ( (NULL != ao) && (BACKGROUND_MAX > s_background_n) )
    {
        s_background[s_background_n++] = ao;
    }
}

/**
 * @brief Throw away whatever the background AOs' timers posted.
 * @param except An AO to leave alone, or NULL. See qf_support_tick().
 */
static void background_drain(QActive *except)
{
    for ( unsigned i = 0U; i < s_background_n; ++i )
    {
        if ( s_background[i] == except )
        {
            continue;
        }
        const QEvt *e;
        while ( (NULL != s_background[i]->eQueue)
               && (xQueueReceive(s_background[i]->eQueue, &e, 0U) == pdTRUE) )
        {
            QF_gc(e);
        }
    }
}

#define RELAY_MAX 2

static QActive *s_relay[RELAY_MAX];
static unsigned s_relay_n;

void qf_support_relay(QActive *ao)
{
    for ( unsigned i = 0U; i < s_relay_n; ++i )
    {
        if ( s_relay[i] == ao )
        {
            return;
        }
    }
    if ( (NULL != ao) && (RELAY_MAX > s_relay_n) )
    {
        s_relay[s_relay_n++] = ao;
    }
}

bool qf_support_relay_drop(QActive *ao)
{
    bool was = false;

    for ( unsigned i = 0U; i < s_relay_n; ++i )
    {
        if ( s_relay[i] == ao )
        {
            was = true;
        }
        if ( was && ((i + 1U) < s_relay_n) )
        {
            s_relay[i] = s_relay[i + 1U];
        }
    }
    if ( was )
    {
        s_relay_n--;
    }
    return was;
}

/**
 * @brief Dispatch what waits in @p ao's queue, at most @p max events.
 * @return How many were dispatched.
 */
static unsigned dispatch_queue(QActive *ao, unsigned max)
{
    unsigned n = 0U;
    const QEvt *e = NULL;

    while ( (n < max) && (NULL != ao->eQueue)
           && (xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE) )
    {
        QASM_DISPATCH(ao, e, 0U);
        QF_gc(e);
        ++n;
    }
    return n;
}

/**
 * @brief Dispatch the relays and @p ao in turn until none has anything left.
 *
 * A relay answers the subject and the subject answers the relay, so one pass
 * over each is not enough: the exchange is over when a whole round moved
 * nothing.
 *
 * @param ao  The subject; a relay that is the subject is left to the caller.
 * @param max Most events to deliver to @p ao.
 * @return How many events @p ao dispatched.
 */
static unsigned relay_round(QActive *ao, unsigned max)
{
    unsigned n = 0U;
    unsigned moved;

    do
    {
        moved = 0U;
        for ( unsigned i = 0U; i < s_relay_n; ++i )
        {
            if ( s_relay[i] != ao )
            {
                moved += dispatch_queue(s_relay[i], 64U);
            }
        }
        unsigned const mine = dispatch_queue(ao, max - n);
        n += mine;
        moved += mine;
    } while ( 0U != moved );
    return n;
}

/* One slot per Active Object a case file brings up. A claim that does not fit
 * would be forgotten, and the next case would ask again and register a second
 * time: QActive_register_() then dies at qf_qact:110, in whichever file ran
 * next. So an overflow fails the case that caused it. */
#define CLAIMED_MAX 16

static QActive *s_claimed[CLAIMED_MAX];
static unsigned s_claimed_n;

bool qf_support_claim_ao(QActive *ao)
{
    for ( unsigned i = 0U; i < s_claimed_n; ++i )
    {
        if ( s_claimed[i] == ao )
        {
            return false;
        }
    }
    if ( CLAIMED_MAX <= s_claimed_n )
    {
        TEST_FAIL_MESSAGE("qf_support's claim table is full: raise CLAIMED_MAX");
    }
    s_claimed[s_claimed_n++] = ao;
    return true;
}

static QActive *s_registered[CLAIMED_MAX];
static unsigned s_registered_n;

void qf_support_register(QActive *ao)
{
    for ( unsigned i = 0U; i < s_registered_n; ++i )
    {
        if ( s_registered[i] == ao )
        {
            return;
        }
    }
    if ( CLAIMED_MAX <= s_registered_n )
    {
        TEST_FAIL_MESSAGE("qf_support's register table is full: raise CLAIMED_MAX");
    }
    QActive_register_(ao);
    s_registered[s_registered_n++] = ao;
}

/* Why the two functions below drive their ticks inside one critical section.
 *
 * The host port's uxCriticalNesting is documented as per-task and implemented
 * as one global (FreeRTOS-Kernel/portable/linux/port.c). What reconciles the
 * two is prvSwitchThread(), which saves it on the outgoing thread's stack -
 * but that function resumes the incoming thread BEFORE suspending the
 * outgoing one, so for a short window two task threads really do run at once,
 * both writing that global. Land a SIGALRM tick in the middle of
 * vPortExitCritical(), between the test that the count is above zero and the
 * decrement itself, and the count reaches -1: "Assertion failed:
 * (uxCriticalNesting >= 0), port.c:270", and the binary aborts mid-case.
 *
 * Two FreeRTOS tasks exist here - the one running the cases and the timer
 * service task IDF starts - which is all the race needs. It is rare per tick
 * and these functions drive a great many: the guided buttons row alone
 * advances fifteen thousand in one call, which was enough to abort roughly
 * one run in a hundred and twenty.
 *
 * Holding one critical section across the whole loop closes it, because the
 * nesting count never returns to zero inside: the port only unblocks signals
 * on the way through zero, so the tick that would preempt us is never
 * delivered and the timer task is never scheduled against us. Nothing in the
 * loop can yield - the harness gives its Active Objects queues but no tasks,
 * so no thread is ever waiting on one - and simulated time is this harness's
 * to advance anyway. A masked signal would not do: vPortEnableInterrupts()
 * unblocks every signal outright rather than restoring a saved mask, so the
 * first exit through zero would undo it.
 */
static portMUX_TYPE s_tick_mux = portMUX_INITIALIZER_UNLOCKED;

/* One step of these functions is 10 ms, whatever KQP_TICKS_PER_SEC is. The
 * cases were written against a 10 ms tick and count in it - bursts per tick,
 * feeds per tick, 51 ticks for a 500 ms blink - and that is the unit every
 * pacing gap in the firmware still uses, so a step stays the natural one to
 * count in. A step runs all of its real ticks and only then dispatches, which
 * is what one 10 ms tick did: the cases about two events landing in the same
 * tick (a request queued ahead of a window's expiry) depend on it. */
#define QF_SUPPORT_STEP_TICKS  KQP_TICKS_PER_MS(10U)   /* QF_SUPPORT_STEPS(10) */

unsigned qf_support_tick(QActive *ao, unsigned ticks)
{
    unsigned n = 0U;

    if ( (NULL == ao) || (NULL == ao->eQueue) )
    {
        return 0U;
    }

    portENTER_CRITICAL(&s_tick_mux);

    for ( unsigned t = 0U; t < ticks; ++t )
    {
        for ( unsigned r = 0U; QF_SUPPORT_STEP_TICKS > r; ++r )
        {
            background_drain(ao);   /* room for what the tick will post */
            QTIMEEVT_TICK_X(0U, (void *)0);
            background_drain(ao);   /* everyone else's, straight back out */
        }

        n += relay_round(ao, ~0U);
    }

    portEXIT_CRITICAL(&s_tick_mux);

    return n;
}

unsigned qf_support_pump(QActive *ao, unsigned max)
{
    unsigned n = 0U;

    if ( (NULL == ao) || (NULL == ao->eQueue) )
    {
        return 0U;
    }

    portENTER_CRITICAL(&s_tick_mux);   /* see the note above qf_support_tick */

    /* The ticks first: they are what moves an armed QTimeEvt from "counting"
     * to "posted", and a one-shot disarms itself on the way. Without that the
     * AO's next arm would find the timer still armed. The whole step's ticks,
     * whatever @p max is - a ceiling of zero is how a case asks for the ticks
     * alone - and only then the dispatch, so what @p ao posts to a background
     * AO stays queued for the case to read, as it did with one tick. */
    for ( unsigned t = 0U; QF_SUPPORT_STEP_TICKS > t; ++t )
    {
        background_drain(NULL);   /* room for what the tick is about to post */
        QTIMEEVT_TICK_X(0U, (void *)0);
        background_drain(NULL);   /* and take it straight back out again */
    }

    n = relay_round(ao, max);

    portEXIT_CRITICAL(&s_tick_mux);

    return n;
}

unsigned qf_support_settle(QActive *ao)
{
    unsigned n;

    if ( (NULL == ao) || (NULL == ao->eQueue) )
    {
        return 0U;
    }
    portENTER_CRITICAL(&s_tick_mux);   /* see the note above qf_support_tick */
    n = relay_round(ao, ~0U);
    portEXIT_CRITICAL(&s_tick_mux);
    return n;
}

void qf_support_tick_escaped(void)
{
    portEXIT_CRITICAL(&s_tick_mux);
}

void qf_support_reap_timers(void)
{
    portENTER_CRITICAL(&s_tick_mux);   /* see the note above qf_support_tick */
    QTIMEEVT_TICK_X(0U, (void *)0);
    portEXIT_CRITICAL(&s_tick_mux);
}

uint16_t qf_support_pool_dry_second(qf_support_dry_t *dry, uint16_t req_size, uint16_t rsp_size)
{
    QEvt *probe;

    dry->n = 0U;
    while ( QF_SUPPORT_DRY_MAX > dry->n )
    {
        QEvt * const e = QF_newX_((uint_fast16_t)req_size, 0U, (enum_t)Q_USER_SIG);

        if ( NULL == e )
        {
            break;
        }
        dry->held[dry->n] = e;
        dry->n++;
    }
    if ( (0U == dry->n) || (QF_SUPPORT_DRY_MAX == dry->n) )
    {
        dry->rsp_pool = 0U;
        return 0U;   /* the request's pool never ran dry, or had nothing to begin with */
    }
    probe = QF_newX_((uint_fast16_t)rsp_size, 0U, (enum_t)Q_USER_SIG);
    if ( NULL == probe )
    {
        /* One pool for both: one block back, for the answer alone. */
        dry->n--;
        dry->rsp_pool = (uint8_t)dry->held[dry->n]->poolNum_;
        QF_gc(dry->held[dry->n]);
    }
    else
    {
        dry->rsp_pool = (uint8_t)probe->poolNum_;
        QF_gc(probe);
    }
    return qf_support_pool_free(dry);
}

uint16_t qf_support_pool_free(qf_support_dry_t const *dry)
{
    if ( 0U == dry->rsp_pool )
    {
        return 0U;
    }
    return QF_getPoolFree((uint_fast8_t)dry->rsp_pool);
}

void qf_support_pool_give_back(qf_support_dry_t *dry)
{
    while ( 0U < dry->n )
    {
        dry->n--;
        QF_gc(dry->held[dry->n]);
    }
}
