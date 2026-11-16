/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_knock.h
 * @brief Package-private: a knock, a post from another task that a full queue delays and never loses.
 *
 * A knock is one static event that tells an Active Object to read a state
 * somebody else keeps. One waits at a time: a post that finds one waiting is
 * folded into it. A post the queue refuses is owed, and posted again by the
 * poster when the queue has room or by the Active Object before the next
 * event it handles.
 *
 * A post and a payment are runs of steps, and each step reads or writes the
 * knock's word or the queue once. The board runs a run from its first step to
 * its last; the host suite takes the steps of several runs one at a time, in
 * every order.
 */
#ifndef RADIO_KNOCK_H_
#define RADIO_KNOCK_H_

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

/** @brief The queue a knock's event goes to, as the knock sees it. */
typedef struct {
    /**
     * @brief Post the knock's event, leaving the knock's margin free.
     * @param ctx The door's own.
     * @return false when the queue refused it.
     */
    bool (*post)(const void *ctx);
    /**
     * @brief Whether the queue would take the knock's event now.
     * @param ctx The door's own.
     * @return true when a post would be taken.
     */
    bool (*room)(const void *ctx);
    const void *ctx;   /**< What both calls are given. */
} radio_knock_door_t;

/** @brief One knock: its word, and the queue its event goes to. */
typedef struct {
    atomic_uint               word;   /**< Whether a knock is pending, whether it is owed, and what the first post carried. */
    const radio_knock_door_t *door;   /**< The queue. */
} radio_knock_t;

/** @brief Where a run stands. */
typedef enum {
    RADIO_KNOCK_AT_CLAIM = 0,   /**< Next: make the knock pending, if none is. */
    RADIO_KNOCK_AT_SEND,        /**< Next: post the event. */
    RADIO_KNOCK_AT_OWE,         /**< Next: mark the refused event owed. */
    RADIO_KNOCK_AT_LOOK,        /**< Next: see whether the queue has room now. */
    RADIO_KNOCK_AT_RECLAIM,     /**< Next: take an owed event back to post it. */
    RADIO_KNOCK_AT_DONE         /**< Nothing is left to do. */
} radio_knock_at_t;

/** @brief One run: a post from another task, or a payment by the Active Object. */
typedef struct {
    uint8_t      at;        /**< A radio_knock_at_t. */
    bool         payer;     /**< The run is the Active Object's own. */
    bool         refused;   /**< The queue refused a post of this run. */
    unsigned int held;      /**< The word this run made or took back, pending and not owed. */
} radio_knock_run_t;

/**
 * @brief Make a knock that nobody has posted: nothing is pending and nothing is owed.
 *
 * For a start of the Active Object, before anything can post.
 *
 * @param k    The knock.
 * @param door The queue its event goes to; it outlives the knock.
 */
void radio_knock_init(radio_knock_t *k, const radio_knock_door_t *door);

/**
 * @brief Begin a post from another task.
 * @param run The run to begin.
 * @param arg What the post carries; the first of the posts that fold stands.
 */
void radio_knock_post_begin(radio_knock_run_t *run, uint8_t arg);

/**
 * @brief Begin a payment: the Active Object posts again what is owed.
 * @param run The run to begin.
 */
void radio_knock_pay_begin(radio_knock_run_t *run);

/**
 * @brief Take one step of a run.
 * @param k   The knock.
 * @param run The run.
 * @return true while a step is left.
 */
bool radio_knock_step(radio_knock_t *k, radio_knock_run_t *run);

/**
 * @brief Post a knock from another task.
 *
 * The event is posted once for the posts that fold. A queue with no room owes
 * it: it is posted again from here when the queue has room after the mark,
 * and by radio_knock_pay() otherwise. The call goes round again only when
 * the queue had room at the look and none at the post that followed, which
 * takes an event another task posted in between: it never spins by itself,
 * and it cannot stop short of a look that finds no room without leaving the
 * knock to an Active Object that may already wait on an empty queue.
 *
 * @param k   The knock.
 * @param arg What the post carries; the first of the posts that fold stands.
 * @return false when the queue refused the event at least once.
 */
bool radio_knock_post(radio_knock_t *k, uint8_t arg);

/**
 * @brief Post again an owed knock, on the Active Object's own thread.
 *
 * Called before each event the Active Object handles. A refusal leaves the
 * knock owed for the next call.
 *
 * @param k The knock.
 */
void radio_knock_pay(radio_knock_t *k);

/**
 * @brief Take a knock: the Active Object has its event in hand.
 *
 * Called before the state the knock stands for is read, so a post that comes
 * after it makes an event of its own. A knock is taken once: an event that
 * finds nothing pending was heard already.
 *
 * @param k   The knock.
 * @param arg Receives what the first post carried, when a knock was pending; NULL to ignore it.
 * @return true when a knock was pending.
 */
bool radio_knock_take(radio_knock_t *k, uint8_t *arg);

/**
 * @brief Whether a knock waits to be taken.
 * @param k The knock.
 * @return true when it is pending, queued or owed: its event reaches the Active Object later, for certain.
 */
bool radio_knock_pending(radio_knock_t *k);

#endif /* RADIO_KNOCK_H_ */
