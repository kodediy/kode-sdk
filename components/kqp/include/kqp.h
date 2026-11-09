/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp.h
 * @brief kqp: the QP/C integration, the one component that touches QP directly.
 */
#ifndef KQP_H_
#define KQP_H_

#include "qpc.h"
#include "kqp_signals.h"
#include "kqp_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Initialize QF, the pub/sub bus and the event pools. Call once, before any AO starts. */
void kqp_init(void);

/**
 * @brief Post @p e to @p ao from interrupt context.
 *
 * The one door an ISR has into an Active Object. It wraps the FreeRTOS
 * "FromISR" flavour of the post and the context switch request that has to
 * follow it, so that a driver's interrupt callback and the AO code it calls
 * never spell a FreeRTOS type - kaos may not include freertos/, and kqp is
 * where QP's port already does.
 *
 * @param ao The recipient, already started.
 * @param e  A static, immutable event (QEVT_INITIALIZER): nothing here
 *           allocates, so nothing here can run the pool dry from an ISR.
 */
void kqp_post_from_isr(QActive *ao, QEvt const *e);

/**
 * @brief Disarm a one-shot and say whether its expiry is still queued.
 *
 * QTimeEvt_disarm() returns false for two opposite situations: a timer that
 * was never armed, and one that expired and posted its event into the AO's
 * queue. Reading that false as "nothing to worry about" is the defect: the
 * expired event is still coming, it is dispatched after whatever re-armed the
 * timer, and it then ends a window - or answers a request - that belongs to
 * the arming that replaced it. Nothing QP offers separates the two cases;
 * @p live is the missing half: whether the caller armed this timer and has
 * not yet seen its expiry. A true answer means the next event for @p te is a
 * generation behind and the handler must drop it.
 *
 * @param te   The one-shot.
 * @param live Whether the caller considers a previous arming outstanding.
 * @return true when an expired event for @p te is still queued.
 */
bool kqp_timer_stale(QTimeEvt * const te, bool live);

/**
 * @brief Say whether the calling task is the thread of a started Active Object.
 *
 * A call that blocks, made from an AO's step, stops that AO and everything
 * queued behind the step; the standard layer asks this before it blocks. The
 * answer is by task, so it holds in any function an AO calls, however deep.
 * An AO that was constructed but not started has no task and is never the
 * answer.
 *
 * @param name Receives the AO's task name when the answer is true, and is left
 *             alone otherwise. The string belongs to the task and lives as long
 *             as it does. May be NULL.
 * @return true when the caller is an Active Object's thread.
 */
bool kqp_current_is_ao(const char **name);

/**
 * @brief Say whether an Active Object already holds the QP priority @p prio.
 *
 * QP stops the run on a second start at a level taken, so a caller that is
 * about to start an Active Object at a level someone else may hold asks first
 * and refuses by name instead.
 *
 * @param prio A QP priority.
 * @param name Receives the holder's task name when the answer is true, "?" if
 *             it has no task yet, and is left alone otherwise. The string
 *             belongs to the task. May be NULL.
 * @return true when the priority is held.
 */
bool kqp_prio_taken(uint8_t prio, const char **name);

/**
 * @brief Say whether every started Active Object has at least half of its
 *        queue free.
 *
 * A request that fans out (its owner answers on the bus and announces the
 * change) reaches subscribers below the asker, which run after it. A door
 * that sends such requests one after another asks this first and answers
 * busy instead, so a burst waits at the door rather than filling the queue
 * of the lowest subscriber, where a post with no margin stops the run.
 * Half matches the doors' margins: every owner keeps half of its queue for
 * its own traffic.
 *
 * @return true when no queue is more than half full.
 */
bool kqp_queues_half_free(void);

/**
 * @brief The length of @p ao's event queue.
 *
 * Read by a door that keeps a margin in that queue, to refuse a margin the
 * queue can never leave.
 *
 * @param ao The Active Object.
 * @return The slots it was started with; 0 for NULL or an AO not started.
 */
uint16_t kqp_queue_len(QActive const *ao);

/**
 * @brief Stop or restart the QP tick.
 *
 * A tick every millisecond keeps the chip out of light sleep, so the
 * low-power Active Object stops it before the board sleeps and restarts it
 * on waking. While it is stopped no QTimeEvt fires, and the Active Object
 * supervisor does not judge: a stopped tick is not a hung Active Object, and
 * the supervisor forgets what it knew once the tick runs again. Idempotent,
 * and meant for one caller (the low-power Active Object).
 *
 * @param rest true stops the tick, false restarts it.
 */
void kqp_tick_rest(bool rest);

#ifdef __cplusplus
}
#endif

#endif // KQP_H_
