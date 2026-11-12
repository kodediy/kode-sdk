/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file qf_support.h
 * @brief The QF bring-up an Active Object case needs, done once per binary.
 */
#ifndef QF_SUPPORT_H_
#define QF_SUPPORT_H_

#include <stdint.h>

#include "qpc.h"

/**
 * @brief Bring QF up if it is not already. Safe to call from every case.
 *
 * Registering an AO is deliberately not part of this: that asserts on a
 * priority slot already taken, so it belongs to whichever case file owns the
 * AO in question.
 */
void qf_support_init(void);

/**
 * @brief Give @p ao the event queue QP posts into, without starting a task.
 *
 * An AO that posts to itself - a paced transfer, a deadline - needs somewhere
 * for those events to land. QACTIVE_START would provide one and a scheduler
 * with it, which is what this tier exists to do without. Idempotent.
 *
 * @param ao The Active Object.
 */
void qf_support_give_queue(QActive *ao);

/**
 * @brief Give @p ao the queue QP lays out in its osObject, which a post with a
 *        margin needs; the events waiting in its old queue are recycled.
 *
 * Another file's fake may have given the AO a heap queue, which a margin post
 * reads as always full. A queue made here is kept, not made again.
 *
 * @param ao The Active Object.
 */
void qf_support_margin_queue(QActive *ao);

/**
 * @brief Post @p filler to @p ao until its queue takes no more.
 * @param ao     The Active Object.
 * @param filler A static event the AO ignores.
 * @return true when the queue refused a post; false when it never filled.
 */
bool qf_support_queue_stuff(QActive *ao, QEvt const *filler);

/**
 * @brief How many steps of qf_support_pump() / qf_support_tick() hold @p ms_.
 *
 * A step is 10 ms whatever the QP tick is, which is the unit the cases were
 * written in. Pass a multiple of 10.
 */
#define QF_SUPPORT_STEPS(ms_)  ((ms_) / 10U)

/**
 * @brief Run one 10 ms step of QF's ticks, then dispatch whatever landed in
 *        @p ao's own queue.
 *
 * This is the piece a bench never has to think about: on the board the tick
 * runs at 1 kHz and a QTimeEvt disarms itself when it fires, so an AO can
 * re-arm on the next chunk. Under direct dispatch nothing ticks, so a paced
 * transfer arms once and then trips QP's "already armed" assertion the moment
 * it tries again. Turning the crank by hand is what keeps the machine honest.
 *
 * @param ao  The Active Object to pump.
 * @param max Most events to deliver, so a bug cannot loop forever.
 * @return How many events were dispatched.
 */
unsigned qf_support_pump(QActive *ao, unsigned max);


/**
 * @brief Declare that @p ao has an armed periodic timer and no dispatcher.
 *
 * The tick inside qf_support_pump() is global: it fires every armed QTimeEvt
 * in the binary, not just the one belonging to the AO being pumped. An AO
 * whose state machine arms a periodic timer on entry - AO_Power polls the
 * PMIC every two seconds - therefore keeps posting to itself for the rest of
 * the run, and its queue fills the first time some other file pumps a few
 * thousand ticks. QP answers a full queue with an assertion and a reset, so
 * the symptom is a completely unrelated case dying in qf_port.
 *
 * There is no way to disarm it from outside: QP's list of armed time events
 * is private, and the AO has exactly one state, so it never exits and never
 * disarms. Saying so here is the alternative - the pump empties these queues
 * on every tick, and the AO goes back to being background noise.
 *
 * Register from the same place the timer gets armed, so a file that runs
 * before that one is unaffected. Calling twice for the same AO is harmless.
 *
 * @param ao The Active Object whose self-posts should be discarded.
 */
void qf_support_background(QActive *ao);

/**
 * @brief Name an Active Object whose queue is dispatched beside every
 *        subject's, rather than left alone or thrown away.
 *
 * Some commands are two Active Objects talking: the console asks AO_Autotest
 * for a run, AO_Autotest publishes each row, the console prints it. With a
 * scheduler that exchange happens by itself. Here nothing carries an event
 * from one queue to the next, so a case that drives one of the two would see
 * half a conversation. A relay is the other half: qf_support_tick(),
 * qf_support_pump() and qf_support_settle() dispatch its queue and the
 * subject's in turn until both are empty.
 *
 * A relay that is itself the subject of a call is dispatched as the subject
 * and no more. Calling twice for the same AO is harmless.
 *
 * @param ao The Active Object to dispatch beside every subject.
 */
void qf_support_relay(QActive *ao);

/**
 * @brief Stop dispatching @p ao beside every subject, for a file whose
 *        subject reads that AO's queue as a mailbox instead.
 *
 * qf_support_relay() puts it back.
 *
 * @param ao The Active Object to leave alone.
 * @return Whether it was a relay.
 */
bool qf_support_relay_drop(QActive *ao);

/**
 * @brief Dispatch what waits for @p ao and for the relays, with no tick.
 *
 * For the moment right after a case dispatches an event by hand: whatever
 * that event made @p ao post is delivered, and simulated time does not move.
 *
 * @param ao The Active Object whose queue is the subject.
 * @return How many events @p ao dispatched.
 */
unsigned qf_support_settle(QActive *ao);

/**
 * @brief True the first time it is asked about @p ao, false ever after.
 *
 * QActive_register_() asserts on a priority slot that is already taken and
 * there is no unregister, so an Active Object is brought up ONCE per binary.
 * A file that owns its AO can hold that in a `static bool` of its own - until
 * a second file wants the same AO, which is what happened when the console's
 * cases outgrew one file: two once-flags, two registrations, and QP stopped
 * the run at qf_qact:110.
 *
 * The flag belongs to the AO rather than to whoever asks about it, so this
 * keeps it here.
 *
 * @param ao The Active Object about to be brought up.
 * @return true if the caller should construct and register it.
 */
bool qf_support_claim_ao(QActive *ao);

/**
 * @brief Register @p ao with QF, once per binary, whoever asks first.
 *
 * For an Active Object that several files bring up and that subscribes in its
 * initial transition: QActive_subscribe() insists the AO is registered, and
 * registering twice is the failure qf_support_claim_ao() describes. Every
 * file that runs such an AO's initial transition calls this first, with the
 * priority already set, and the first call is the only one that registers.
 *
 * @param ao The Active Object, with prio and pthre set.
 */
void qf_support_register(QActive *ao);

/**
 * @brief Advance the clock and let @p ao dispatch its OWN timer events.
 *
 * qf_support_pump() cannot do this for an AO registered as background: it
 * empties those queues on every tick, which is the whole point of registering
 * one. But an AO can be background for its idle timer and still be the
 * subject of a case about a DIFFERENT timer - AO_Console arms a watchdog for
 * every command that waits on an owner, and what that watchdog does when it
 * fires is not something to throw away.
 *
 * So this drains every background AO EXCEPT @p ao, and dispatches @p ao's own
 * queue. Without the drain the periodic timers belonging to AO_Power, AO_Touch
 * and AO_Buttons fill their queues over a few hundred ticks and QP answers a
 * full queue with an assertion - which is how this function came to exist.
 *
 * @param ao    The Active Object whose timers are the subject.
 * @param ticks How many 10 ms steps to advance, each run as the QP ticks it
 *              holds.
 * @return How many events @p ao dispatched.
 */
unsigned qf_support_tick(QActive *ao, unsigned ticks);

/**
 * @brief Balance the critical section a jump out of a tick left entered.
 *
 * khal_restart() is noreturn and the fake leaves by longjmp, so a case that
 * drives an AO to a restart never returns through the call it made. That is
 * harmless for a plain QASM_DISPATCH and is not harmless here: qf_support_
 * tick() and qf_support_pump() hold ONE critical section across their whole
 * loop (see the note in qf_support.c on why), and a jump out of the middle of
 * one leaves the host port's nesting count above zero for good - after which
 * the port never unblocks its signals again, the FreeRTOS tick stops, and some
 * later case that waits on a real timeout hangs the run.
 *
 * So a restart that fires from a TIMER - AO_Boot's home grace is the first -
 * has to say so on landing. Calling it without such a jump is not allowed:
 * it would exit a section nobody entered.
 */
void qf_support_tick_escaped(void);

/**
 * @brief Run one QP tick and dispatch nothing, so every time event that was
 *        disarmed since the last tick is unlinked.
 *
 * QTimeEvt_disarm() only zeroes a counter; the tick is what takes the node
 * out of QF's list. An Active Object that is constructed again while one of
 * its time events is still linked cuts the list at that node, and every timer
 * behind it stops counting for the rest of the binary. A case that rebuilds an
 * AO calls this first, when the case before it may have disarmed a timer and
 * ended without another tick.
 */
void qf_support_reap_timers(void);

/**
 * @brief Disarm every time event @p ao owns, so a case that ended with one
 *        armed cannot break the next.
 *
 * A case that fails stops where it is, and an Active Object it left with a
 * time event armed is constructed again by the next case: QF's list of armed
 * time events then runs in a cycle, and the next tick never returns. A case
 * that rebuilds an AO calls this before qf_support_reap_timers(), which then
 * takes the disarmed events out of the list.
 *
 * @param ao The Active Object whose time events are disarmed.
 */
void qf_support_disarm_timers(QActive *ao);

/**
 * @brief Let @p ticks of QP time pass and dispatch nothing.
 *
 * Every armed time event counts down and posts to its owner's queue, and no
 * Active Object runs: what a stalled owner finds when it runs again.
 *
 * @param ticks How many QP ticks.
 */
void qf_support_time_passes(uint32_t ticks);

/**
 * @brief Take everything out of @p ao's queue and say how many were @p sig.
 *
 * The rest is thrown away. Counts what waited, so a case can say that one
 * time event waits where a periodic one would have posted several.
 *
 * @param ao  The Active Object whose queue is emptied.
 * @param sig The signal to count.
 * @return How many of the events that waited carried it.
 */
uint32_t qf_support_waiting_of(QActive *ao, QSignal sig);

/** @brief Most blocks qf_support_pool_dry_second() takes out of a pool. */
#define QF_SUPPORT_DRY_MAX  128U

/** @brief Blocks taken out of the pools so a request fails at its second allocation. */
typedef struct {
    QEvt const *held[QF_SUPPORT_DRY_MAX]; /**< Taken; qf_support_pool_give_back() returns them. */
    uint32_t    n;                        /**< How many.                                     */
    uint8_t     rsp_pool;                 /**< The answer's pool, numbered from 1.            */
} qf_support_dry_t;

/**
 * @brief Leave a request's pool dry and its answer's with room: an ask then
 *        takes its answer and fails to take its request.
 *
 * When both come from one pool, that pool is left one block.
 *
 * @param dry      Out: what was taken.
 * @param req_size sizeof the request.
 * @param rsp_size sizeof the answer.
 * @return The answer's pool's free blocks, to compare after the ask; 0 when
 *         the request's pool would not run dry within QF_SUPPORT_DRY_MAX.
 */
uint16_t qf_support_pool_dry_second(qf_support_dry_t *dry, uint16_t req_size, uint16_t rsp_size);

/**
 * @brief The free blocks of the answer's pool.
 * @param dry What qf_support_pool_dry_second() took.
 * @return The count now.
 */
uint16_t qf_support_pool_free(qf_support_dry_t const *dry);

/**
 * @brief Give back every block qf_support_pool_dry_second() took.
 * @param dry What it took; emptied.
 */
void qf_support_pool_give_back(qf_support_dry_t *dry);

#endif /* QF_SUPPORT_H_ */
