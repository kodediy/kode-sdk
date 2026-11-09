/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file ao_spy.h
 * @brief A stand-in recipient for the events an Active Object posts back.
 */
#ifndef AO_SPY_H_
#define AO_SPY_H_

#include "qpc.h"

/** @brief The slots of the spy's queue. */
#define AO_SPY_QUEUE_LEN 32U

/** @brief The spy, ready to be posted to. Brought up on first use. */
QActive *ao_spy(void);

/** @brief How many events are waiting, unread. */
unsigned ao_spy_count(void);

/**
 * @brief Take the next event, oldest first.
 *
 * The event stays valid until the next ao_spy_drain(); the spy holds the
 * reference QP handed it, exactly as a real recipient would until it garbage
 * collected.
 *
 * @return The event, or NULL when nothing is waiting.
 */
const QEvt *ao_spy_take(void);

/** @brief Take the next event and return its signal, or 0 if there is none. */
enum_t ao_spy_next_sig(void);

/**
 * @brief Subscribe the spy to a published signal.
 *
 * Not everything an AO answers is posted to whoever asked: some answers go on
 * the bus, because more than one subscriber wants them. Those reach the spy
 * only if it has asked for them, exactly as a real AO would.
 *
 * @param sig The signal, which must be below MAX_PUB_SIG.
 */
void ao_spy_subscribe(enum_t sig);

/**
 * @brief Release the events already taken, and leave what is waiting.
 *
 * A long exchange takes more events than the pool holds, and every one the
 * spy took stays allocated until it is released. Call this once a case has
 * finished reading what it took: an event taken before the call is no longer
 * valid after it.
 */
void ao_spy_release(void);

/** @brief Drop everything waiting and release the references. */
void ao_spy_drain(void);

/**
 * @brief A second asking AO: a queue of 8 and a priority of its own, no task,
 *        not registered.
 */
QActive *ao_spy_other(void);

/**
 * @brief Take the next event the second asker holds; the caller gc's it.
 * @return The event, or NULL when nothing is waiting.
 */
const QEvt *ao_spy_other_take(void);

/** @brief Drop everything the second asker holds. */
void ao_spy_other_drain(void);

#endif /* AO_SPY_H_ */
