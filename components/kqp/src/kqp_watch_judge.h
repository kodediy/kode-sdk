/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_watch_judge.h
 * @brief Decides whether an Active Object is stuck or starved. Private to kqp.
 */
#ifndef KQP_WATCH_JUDGE_H
#define KQP_WATCH_JUDGE_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Bytes of an Active Object's name in a sample, with its terminator. */
#define KQP_WATCH_NAME_LEN 16U

/** @brief Bit of the dispatch counter that is set while an event is handled. */
#define KQP_WATCH_BUSY_BIT 1U

/** @brief What the supervisor read from one Active Object. */
typedef struct {
    char     name[KQP_WATCH_NAME_LEN]; /**< The AO's name, NUL-terminated.      */
    uint8_t  prio;                     /**< Its QP priority.                    */
    uint32_t count;                    /**< Dispatch counter: odd inside an event. */
    uint32_t start_ms;                 /**< Tick, in ms, when the current event began. */
    uint16_t sig;                      /**< Signal of the event being handled.  */
    uint32_t waiting;                  /**< Events held in the AO's queue.      */
    uint32_t slow_seq;                 /**< Steps the port timed over the limit; wraps. */
    uint32_t slow_ms;                  /**< Length of the latest of them, in ms. */
    uint16_t slow_sig;                 /**< Signal of that step.                */
    bool     valid;                    /**< False for an unused or unreadable slot. */
} KqpWatchSample;

/** @brief What the judge remembers about one Active Object between passes. */
typedef struct {
    uint32_t last_count;       /**< The counter at the previous pass.           */
    uint32_t streak_start_ms;  /**< When the frozen-queue streak began.         */
    bool     streaking;        /**< True while a frozen-queue streak is running. */
} KqpWatchSlot;

/** @brief The judge's answer. */
typedef enum {
    KQP_WATCH_OK,      /**< Every Active Object is making progress.            */
    KQP_WATCH_STUCK,   /**< A handler has run past the timeout.                */
    KQP_WATCH_STARVED  /**< A queue has held events past the timeout, unserved. */
} KqpWatchKind;

/** @brief The Active Object over the deadline, if any. */
typedef struct {
    KqpWatchKind kind;     /**< OK, STUCK or STARVED.                          */
    uint8_t      index;    /**< Index of the offender in the sample array.     */
    uint32_t     over_ms;  /**< How long it has been in that state, in ms.     */
    uint16_t     sig;      /**< Signal of the event it is inside or waits on.  */
    uint32_t     waiting;  /**< Events in its queue.                           */
} KqpWatchVerdict;

/**
 * @brief Milliseconds from an earlier tick to now, never negative.
 *
 * The supervisor reads the tick before it walks the Active Objects, so an AO
 * on the other core can begin an event a tick after that reading. The
 * unsigned difference would then be about 2^32 ms; read as signed 32-bit it
 * is negative, and that counts as just started. The timeout is far below
 * 2^31 ms, so a real age is never mistaken for a negative one, and a wrap of
 * the tick is still harmless.
 *
 * @param now_ms  The tick of this pass.
 * @param then_ms An earlier (or, by a tick, later) tick.
 * @return The age in ms, 0 when then_ms is ahead of now_ms.
 */
uint32_t kqp_watch_age_ms(uint32_t now_ms, uint32_t then_ms);

/**
 * @brief Judge one pass over the Active Objects.
 *
 * Updates every slot's streak, then picks the offender that has been in its
 * state the longest (the lower index wins a tie). A starved streak starts at
 * the first pass that sees events queued behind an even counter, and ends
 * when the counter moves or the queue empties. Times are 32-bit millisecond
 * ticks and are subtracted, so a wrap of the tick is harmless.
 *
 * @param slots      One slot per sample, owned by the caller, zeroed at start.
 * @param samples    What the supervisor read this pass.
 * @param n          Number of entries in both arrays.
 * @param now_ms     The tick, in ms, of this pass.
 * @param timeout_ms An event or a frozen queue lasting longer than this is a hang.
 * @return The verdict; kind is KQP_WATCH_OK when nobody is over the timeout.
 */
KqpWatchVerdict kqp_watch_judge(KqpWatchSlot slots[],
                                KqpWatchSample const samples[],
                                uint8_t n,
                                uint32_t now_ms,
                                uint32_t timeout_ms);

/**
 * @brief Whether this pass must leave the Active Objects unjudged because the
 *        tick rested.
 *
 * With the tick stopped nothing dispatches on purpose, so a pass during the
 * rest judges nothing. The first pass after it also judges nothing and wipes
 * every slot: a frozen queue's streak that began before the sleep would
 * otherwise count the sleep as time the Active Object was starved.
 *
 * @param resting The tick is stopped now.
 * @param rests   The count of rests that have ended, as the tick keeps it.
 * @param seen    The count the last pass saw; updated to this one's.
 * @param slots   The judge's slots; zeroed when a rest ended since the last pass.
 * @param n       Number of slots.
 * @return true when the pass must stop here.
 */
bool kqp_watch_rested(bool resting, uint32_t rests, uint32_t *seen,
                      KqpWatchSlot slots[], uint8_t n);

/** @brief What the supervisor logs for one Active Object's slow steps. */
typedef struct {
    bool     log;    /**< True when a warning is due.                         */
    uint32_t ms;     /**< Exact length of the latest slow step, in ms.        */
    uint16_t sig;    /**< Signal of that step.                                */
    uint32_t count;  /**< Slow steps since the last pass; 1 for a lone one.   */
} KqpWatchSlow;

/**
 * @brief Decide whether an Active Object ran a step over the limit since the
 *        last pass, and say how long and how many.
 *
 * The port times each step where it ends and counts those over the limit in
 * the sample's slow_seq, so the length is exact and no step falls between
 * passes. A warning is due when the count moved and the latest step is
 * strictly over @p limit_ms (a step of exactly the limit is within the rule).
 * The count is the difference of 32-bit counters, so a wrap is harmless.
 *
 * @param seen     The slow_seq the last pass saw; updated to this one's.
 * @param s        What the supervisor read this pass; an invalid sample says nothing.
 * @param limit_ms The longest step the rule allows.
 * @return What to log; log is false when nothing is due.
 */
KqpWatchSlow kqp_watch_slow(uint32_t *seen, KqpWatchSample const *s,
                            uint32_t limit_ms);

/**
 * @brief Start the supervisor: a task that samples every Active Object once a
 *        period and panics naming one that is stuck or starved.
 *
 * Called last by kqp_init(), under CONFIG_KQP_AO_WATCH. A refusal by the
 * task watchdog is logged and leaves the system running without a supervisor.
 */
void kqp_watch_start(void);

#endif /* KQP_WATCH_JUDGE_H */
