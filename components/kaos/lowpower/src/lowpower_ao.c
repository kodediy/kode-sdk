/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_ao.c
 * @brief AO_LowPower: the one place that decides the board's state.
 */
#include "kaos.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "kdrv_radio.h"
#include "khal_console.h"
#include "khal_nvs.h"
#include "khal_power.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"
#include "kset.h"
#include "lowpower_last.h"
#include "kaos_claim.h"
#include "kaos_start.h"
#include "kverbs.h"

#define TAG "kaos.lowpower"

/* The glass's owner, named weakly (KAOS_WEAK, kaos.h): AO_LowPower lights the
 * minute on a glass the app linked, and must not link AO_Display itself. */
KAOS_WEAK void kaos_display_start(void);
KAOS_WEAK void display_minute_request(void);

/** @brief Bits in the mask of participants still to answer. */
#define WAITING_BITS    32U

_Static_assert(KODE_LOWPOWER_PARTICIPANTS <= WAITING_BITS,
               "the participants of a round must fit the mask that tracks them");

/** @brief The expander's BOT bit, in an IoexpInputEvt. Active low. */
#define BOT_MASK        (1U << KBSP_IOEXP_BIT_BTN_BOT)

/** @brief Causes there are (kode_lp_cause_t is dense and BOOT is its last). */
#define LP_CAUSES       ((uint8_t)KODE_LP_CAUSE_BOOT + 1U)

/** @brief A round number never used: no round has begun. */
#define ROUND_NONE      0U

/** @brief What the facade's refusals say for a request the table cannot take. */
#define WHO_REQUEST     "request"

/** @brief Whose refusal it is when a cable is in. */
#define WHO_CABLE       "cable"

/** @brief Why a cable refuses or abandons a sleep. */
#define WHY_CABLE       "external power"

/** @brief Whose abandon it is when the user used the board during an idle round. */
#define WHO_INPUT       "input"

/** @brief Why an idle round is abandoned by an input. With WHO_INPUT it is
 *         KODE_LOWPOWER_WHY_USED, the refusal that means a person has the board. */
#define WHY_INPUT       "board used"

/** @brief Why a rest yields to another's request that waits to begin again.
 *         With WHO_REQUEST it is KODE_LOWPOWER_WHY_PENDING. */
#define WHY_PENDING     "request pending"

/** @brief Why a request or a rest is refused while a round runs. With
 *         WHO_REQUEST it is KODE_LOWPOWER_WHY_RUNNING. */
#define WHY_RUNNING     "a round is running"

/** @brief Milliseconds in a second, for the idle time. */
#define MS_PER_S        1000U

/** @brief Microseconds in a second. */
#define US_PER_S        1000000ULL

/** @brief Microseconds in a millisecond. */
#define US_PER_MS       1000ULL

/** @brief How long a request is begun again, in microseconds. */
#define RETRY_WINDOW_US ((uint64_t)KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS * US_PER_MS)

/** @brief Microseconds in a minute, the period of the glass's time. */
#define US_PER_MINUTE   ((uint64_t)KODE_LOWPOWER_MINUTE_S * US_PER_S)

/** @brief The value in the store: seconds, four bytes. */
typedef uint32_t SettingSecs;

/** @brief The two settings, an index into LowPower::secs and SETTINGS. */
typedef enum {
    SETTING_SLEEP_AFTER = 0,   /**< `sleep_after`: idle time before ASLEEP.  */
    SETTING_OFF_AFTER,         /**< `off_after`: time asleep before OFF.     */
    SETTING_COUNT              /**< How many there are.                      */
} setting_id_t;

/** @brief One setting: its key, its words, the seconds of each and its default. */
typedef struct {
    const char        *key;     /**< The `config` key, also the store's.       */
    const char *const *words;   /**< What the console shows.                   */
    const SettingSecs *secs;    /**< Seconds of each word, in the same order.  */
    uint8_t            n;       /**< How many words.                           */
    SettingSecs        def_s;   /**< The seconds when nothing is stored.       */
} Setting;

static const char *const SLEEP_AFTER_WORDS[] = KSET_SLEEP_AFTER_WORDS;
static const SettingSecs SLEEP_AFTER_SECS[]  = KSET_SLEEP_AFTER_SECS;
static const char *const OFF_AFTER_WORDS[]   = KSET_OFF_AFTER_WORDS;
static const SettingSecs OFF_AFTER_SECS[]    = KSET_OFF_AFTER_SECS;

_Static_assert(Q_DIM(SLEEP_AFTER_WORDS) == Q_DIM(SLEEP_AFTER_SECS),
               "every idle time has a word and its seconds");
_Static_assert(Q_DIM(OFF_AFTER_WORDS) == Q_DIM(OFF_AFTER_SECS),
               "every time asleep has a word and its seconds");

/** @brief The settings, in setting_id_t order. */
static const Setting SETTINGS[SETTING_COUNT] = {
    { KSET_KEY_SLEEP_AFTER, SLEEP_AFTER_WORDS, SLEEP_AFTER_SECS,
      (uint8_t)Q_DIM(SLEEP_AFTER_WORDS), KODE_LOWPOWER_SLEEP_AFTER_DEF_S },
    { KSET_KEY_OFF_AFTER, OFF_AFTER_WORDS, OFF_AFTER_SECS,
      (uint8_t)Q_DIM(OFF_AFTER_WORDS), KODE_LOWPOWER_OFF_AFTER_DEF_S },
};

/** @brief One QTimeEvt one-shot and the bookkeeping that tells its expiries apart. */
typedef struct {
    QTimeEvt evt;       /**< The time event.                                  */
    uint32_t stale;     /**< Expired events of @c evt still queued.           */
    bool     live;      /**< @c evt is armed and unexpired.                   */
} Tmo;

/** @brief One owner that takes part in the rounds. */
typedef struct {
    QActive    *ao;     /**< The participant's Active Object.            */
    const char *name;   /**< Its short name, which must outlive the AO.  */
} Participant;

typedef struct {
    QActive     super;
    Tmo         round_tmo;        /**< LOWPOWER_ROUND_TMO_SIG: the round's budget, or OFF's grace. */
    Tmo         idle_tmo;         /**< LOWPOWER_IDLE_TMO_SIG: the idle time, AWAKE only. */
    Tmo         radio_tmo;        /**< LOWPOWER_RADIO_TMO_SIG: the bound of a radio wake. */
    Tmo         retry_tmo;        /**< LOWPOWER_RETRY_TMO_SIG: the wait before a requested round begins again. */
    Participant parts[KODE_LOWPOWER_PARTICIPANTS]; /**< The table lowpower_join() fills. */
    uint8_t     joined;           /**< Entries of parts in use.              */
    bool        sealed;           /**< A round has begun: joins are refused. */
    uint32_t    round;            /**< The number of the last round begun.   */
    uint32_t    waiting;          /**< Bit n: participant n has not answered.*/
    uint32_t    busy;             /**< Bit n: participant n answered busy to an order for OFF and is asked
                                   *   again when retry_tmo runs out (round_order()). */
    uint8_t     state;            /**< The board's state, a kode_lp_state_t. */
    uint8_t     target;           /**< The round's target state.             */
    uint8_t     cause;            /**< The round's cause, a kode_lp_cause_t. */
    uint8_t     round_src;        /**< Who asked for the round (the request's
                                   *   source), KODE_LOG_SRC_NONE when no one
                                   *   did: the timers and the inputs.        */
    bool        retry_on;         /**< A requested round was abandoned for a busy or silent owner
                                   *   and begins again when retry_tmo runs out; target and
                                   *   round_src still name the request.      */
    uint64_t    request_us;       /**< khal_time_us() when the request in hand came in. */
    bool        power_ok;         /**< khal_power_init() succeeded.          */
    bool        vbus;             /**< The last PowerEvt saw a cable.        */
    SettingSecs secs[SETTING_COUNT]; /**< The settings, in seconds; 0 is never. */
    uint64_t    asleep_since_us;  /**< khal_time_us() when ASLEEP began.     */
    uint64_t    off_since_us;     /**< khal_time_us() off_after counts from: the ASLEEP that followed
                                   *   the last wake somebody used (an owner's wake nobody used keeps it). */
    uint64_t    since_us;         /**< khal_time_us() at the last change of state. */
    uint8_t     last_cause;       /**< The kode_lp_cause_t of that change.   */
    uint32_t    wakes[LP_CAUSES]; /**< Wakes from ASLEEP, by the cause that ended it. */
    uint64_t    minute_due_us;    /**< When the one-shot comes due.          */
    uint64_t    boundary_us;      /**< When the minute's work is due.        */
    uint64_t    wake_due_us;      /**< When a radio owner's work is due; 0 for none. */
    uint8_t     wake_who;         /**< The radio owner whose time that is.   */
    uint64_t    app_due_us;       /**< When any other owner's work is due; 0 for none. A
                                   *   radio wake does not touch it.          */
    uint8_t     app_who;          /**< The owner whose time that is.         */
    uint32_t    radio_mask;       /**< Bit n: participant n takes part in radio wakes. */
    uint32_t    radio_pending;    /**< Bit n: radio owner n has not said it is done. */
    bool        radio;            /**< A radio wake is on: the tick runs and the lock is held inside ASLEEP. */
    bool        radio_resting;    /**< The radio wake's round for ASLEEP is open. */
    uint8_t     minute_out;       /**< One-shots armed and not yet heard.    */
    lowpower_last_t *last;        /**< This rest's summary, in PSRAM while ASLEEP; NULL otherwise. */
    bool        sample_due;       /**< The minute asked AO_Power for a reading and it has not come. */
    bool        save_due;         /**< A sample is in the working copy and not yet in the store. */
    khal_power_stats_t       stats0; /**< khal_power_stats() when this rest began.   */
    kdrv_radio_sleep_stats_t radio0; /**< kdrv_radio_sleep_stats() when it began.    */
    QTimeEvt    state_retry;      /**< LOWPOWER_STATE_RETRY_SIG: the state that no event carried. */
    bool        state_owed;       /**< A STATE change was not published and is still to be. */
    uint8_t     owed_state;       /**< The kode_lp_state_t that change reached. */
    uint8_t     owed_cause;       /**< The kode_lp_cause_t of that change.   */
    uint8_t     owed_prev;        /**< The kode_lp_state_t it left.          */
    bool        radio_closing;    /**< The radio wake's STATE(ASLEEP) is owed: the tick and the lock stay until it goes out. */
    bool        used;             /**< Something other than an owner's time used the board since it last woke:
                                   *   every wake but a timer's, and each real input after. */
    bool        rest_on;          /**< The request in hand, round or retry, is a rest (lowpower_request_rest()). */
} LowPower;

static LowPower l_lowpower;
QActive * const AO_LowPower = &l_lowpower.super;

/** @brief The last refusal or busy reason; written by this AO, read by the console. */
static char s_refusal[KODE_LOWPOWER_REFUSAL_MAX];

static QState LowPower_initial(LowPower * const me, void const * const par);
static QState LowPower_on(LowPower * const me, QEvt const * const e);
static QState LowPower_awake(LowPower * const me, QEvt const * const e);
static QState LowPower_resting(LowPower * const me, QEvt const * const e);
static QState LowPower_asleep(LowPower * const me, QEvt const * const e);
static QState LowPower_radio(LowPower * const me, QEvt const * const e);
static QState LowPower_off(LowPower * const me, QEvt const * const e);

/* ---- Names ---------------------------------------------------------------- */

/** @brief A state's name for the log. */
static const char *state_name(uint8_t state)
{
    static const char * const names[] = { "AWAKE", "ASLEEP", "OFF", "RADIO" };
    const char *name = "?";

    if ( Q_DIM(names) > state )
    {
        name = names[state];
    }
    return name;
}

/** @brief A cause's name for the log. */
static const char *cause_name(uint8_t cause)
{
    static const char * const names[] = {
        "request", "idle", "off-after", "BOT", "BLE", "MQTT", "cable", "timer",
        "TOP", "boot"
    };
    _Static_assert(Q_DIM(names) == ((size_t)KODE_LP_CAUSE_BOOT + 1U),
                   "a name for every cause, BOOT being the last");
    const char *name = "?";

    if ( Q_DIM(names) > cause )
    {
        name = names[cause];
    }
    return name;
}

/**
 * @brief Copy a text into a buffer, truncated, always terminated.
 *
 * @param dst  The buffer.
 * @param size Its size.
 * @param src  The text; NULL reads as "".
 */
static void why_copy(char *dst, size_t size, const char *src)
{
    size_t i = 0U;

    if ( NULL != src )
    {
        while ( ((size - 1U) > i) && ('\0' != src[i]) )
        {
            dst[i] = src[i];
            i++;
        }
    }
    dst[i] = '\0';
}

/* ---- The refusal ----------------------------------------------------------- */

/**
 * @brief Remember why the last request or round did not go through.
 *
 * @param who Whose reason: a participant's name, or the request's.
 * @param why The reason.
 */
static void refusal_set(const char *who, const char *why)
{
    (void)snprintf(s_refusal, sizeof(s_refusal), "%s: %s", who, why);
}

/**
 * @brief Tell whoever asked that the last request or round did not go through.
 *
 * Published, so the console can close the verb that asked. The event names its
 * source, so one port's refusal never closes another port's verb. Call it after
 * refusal_set().
 *
 * @param src The refused request's source, or the abandoned round's.
 */
static void refusal_publish(uint8_t src)
{
    LowPowerRefusedEvt *ev = Q_NEW_X(LowPowerRefusedEvt, 0U, LOWPOWER_REFUSED_SIG);

    if ( NULL == ev )
    {
        /* Nobody learns why; the console's watchdog answers for the verb. */
        KLOG_W(TAG, "out of events; the refusal was not published");
        return;
    }
    ev->src = src;
    (void)snprintf(ev->why, sizeof(ev->why), "%s", s_refusal);
    QACTIVE_PUBLISH(&ev->super, AO_LowPower);
}

/* ---- The one-shots ------------------------------------------------------------ */

/** @brief Disarm a one-shot, counting an expiry that is already queued. */
static void tmo_stop(Tmo * const t)
{
    if ( kqp_timer_stale(&t->evt, t->live) )
    {
        t->stale += 1U;
    }
    t->live = false;
}

/**
 * @brief Arm a one-shot for @p ms.
 *
 * @param t  The one-shot.
 * @param ms How long to wait.
 */
static void tmo_start(Tmo * const t, uint32_t ms)
{
    uint32_t ticks = KQP_TICKS_PER_MS(ms);

    tmo_stop(t);
    if ( 0U == ticks )
    {
        ticks = 1U;   /* QP resets on an arming of zero */
    }
    QTimeEvt_armX(&t->evt, (QTimeEvtCtr)ticks, 0U);
    t->live = true;
}

/**
 * @brief Whether the expiry just dispatched is the live arming's.
 *
 * An expiry queued before a tmo_stop() belongs to the arming that was
 * stopped, and is dropped here (the idiom of imu_ao.c).
 *
 * @param t The one-shot.
 * @return true when the one-shot really ran out.
 */
static bool tmo_fresh(Tmo * const t)
{
    bool fresh = true;

    if ( 0U < t->stale )
    {
        t->stale -= 1U;
        fresh = false;
    }
    else
    {
        t->live = false;
    }
    return fresh;
}

/**
 * @brief Start the idle time over, or stop counting it.
 *
 * Counted only while AWAKE and only when the board can sleep at all; a
 * sleep_after of "never" counts nothing.
 *
 * @param me The AO.
 */
static void idle_restart(LowPower * const me)
{
    if ( me->power_ok && (KSET_S_NEVER != me->secs[SETTING_SLEEP_AFTER]) )
    {
        tmo_start(&me->idle_tmo, me->secs[SETTING_SLEEP_AFTER] * MS_PER_S);
    }
    else
    {
        tmo_stop(&me->idle_tmo);
    }
}

/* ---- Publishing -------------------------------------------------------------- */

/**
 * @brief Whether a state that no event could carry is published again later.
 *
 * AWAKE is (owners restore on it), RADIO is (Net and Radio start on it), and so
 * is the ASLEEP that ends a radio wake (Net and Radio leave RADIO on it). The
 * ASLEEP and OFF of an ordinary round are not: the owners rested on the round.
 *
 * @param state The state reached.
 * @param prev  The state left.
 * @return true when the change is owed until it is published.
 */
static bool state_is_owed(uint8_t state, uint8_t prev)
{
    bool owed = false;

    if ( ((uint8_t)KODE_LP_AWAKE == state) || ((uint8_t)KODE_LP_RADIO == state) )
    {
        owed = true;
    }
    else if ( ((uint8_t)KODE_LP_ASLEEP == state) && ((uint8_t)KODE_LP_RADIO == prev) )
    {
        owed = true;
    }
    else
    {
        /* ASLEEP and OFF of an ordinary round: not retried. */
    }
    return owed;
}

/**
 * @brief Put one STATE event on the bus.
 *
 * A change that no event could carry and that state_is_owed() is not given up:
 * it is published again on a short timer, the tick being back (or still
 * running) by then (state_retry_fire()). Only one change is owed at a time;
 * a new one replaces it.
 *
 * @param me    The AO.
 * @param state The state now.
 * @param cause The kode_lp_cause_t.
 * @param prev  The state left.
 * @return true when the event went out.
 */
static bool state_send(LowPower * const me, uint8_t state, uint8_t cause,
                       uint8_t prev)
{
    LowPowerStateEvt *ev = Q_NEW_X(LowPowerStateEvt, 0U, LOWPOWER_STATE_SIG);
    bool const first = !me->state_owed;

    (void)QTimeEvt_disarm(&me->state_retry);
    me->state_owed = false;
    if ( NULL != ev )
    {
        ev->state = state;
        ev->cause = cause;
        ev->prev  = prev;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
    else
    {
        if ( first )
        {
            KLOG_E(TAG, "out of events; the state change to %s was not published",
                   state_name(state));
        }
        if ( state_is_owed(state, prev) )
        {
            me->state_owed = true;
            me->owed_state = state;
            me->owed_cause = cause;
            me->owed_prev  = prev;
            QTimeEvt_armX(&me->state_retry,
                          KQP_TICKS_PER_MS(KODE_LOWPOWER_STATE_RETRY_MS), 0U);
        }
    }
    return NULL != ev;
}

/**
 * @brief Publish a change of state (or, with state == prev, an abandoned round).
 *
 * @param me    The AO.
 * @param state The state now.
 * @param cause The kode_lp_cause_t.
 * @param prev  The state left.
 */
static void publish_state(LowPower * const me, uint8_t state, uint8_t cause,
                          uint8_t prev)
{
    /* A round abandoned publishes AWAKE over AWAKE: not a change, so the time
     * in the state and its cause (lowpower_info) stand. */
    if ( state != prev )
    {
        me->since_us   = khal_time_us();
        me->last_cause = cause;
    }
    me->state_owed = false;   /* a new change supersedes one still owed */
    (void)state_send(me, state, cause, prev);
}

/**
 * @brief LOWPOWER_STATE_RETRY_SIG: publish the state that was owed.
 *
 * Does nothing when the board has gone on since: that change's own event is
 * the news. An owed AWAKE needs the board AWAKE; an owed RADIO or the ASLEEP
 * that ends a radio wake needs the radio wake still on.
 *
 * @param me The AO.
 * @return true when the owed state went out on this call.
 */
static bool state_retry_fire(LowPower * const me)
{
    bool sent = false;

    if ( me->state_owed )
    {
        bool still = false;

        if ( (uint8_t)KODE_LP_AWAKE == me->owed_state )
        {
            still = ((uint8_t)KODE_LP_AWAKE == me->state);
        }
        else
        {
            still = me->radio;
        }
        if ( still )
        {
            sent = state_send(me, me->owed_state, me->owed_cause, me->owed_prev);
            if ( sent )
            {
                KLOG_N(TAG, "%s published late", state_name(me->owed_state));
            }
        }
    }
    return sent;
}


/* ---- The way down ------------------------------------------------------------- */

/**
 * @brief Every participant said ready: change the state.
 *
 * @param me The AO.
 * @return The transition to the new state.
 */
static QState round_done(LowPower * const me)
{
    QState status;
    uint8_t const prev = me->state;

    tmo_stop(&me->round_tmo);
    me->waiting = 0U;

    if ( (uint8_t)KODE_LP_ASLEEP == me->target )
    {
        me->state = (uint8_t)KODE_LP_ASLEEP;
        publish_state(me, me->state, me->cause, prev);
        KLOG_N(TAG, "entering ASLEEP (%s): the P4 light-sleeps when every "
                    "task is idle; BOT, BLE, MQTT or a cable wakes it",
               cause_name(me->cause));
        /* After the lines above are published, and the tick before the lock.
         * Every owner has answered ready: no AO is inside an event while the tick is stopped. */
        kqp_tick_rest(true);
        khal_console_rest(true);
        khal_power_hold(false);
        status = Q_TRAN(&LowPower_asleep);
    }
    else
    {
        me->state = (uint8_t)KODE_LP_OFF;
        publish_state(me, me->state, me->cause, prev);
        KLOG_N(TAG, "entering OFF (%s): deep sleep in %u ms; only BOT wakes it",
               cause_name(me->cause), (unsigned)KODE_LOWPOWER_OFF_GRACE_MS);
        tmo_start(&me->round_tmo, KODE_LOWPOWER_OFF_GRACE_MS);
        status = Q_TRAN(&LowPower_off);
    }
    return status;
}

/**
 * @brief Whether the running round is an order: OFF, asked for by somebody.
 *
 * @param me The AO.
 * @return true for a round no owner can refuse.
 */
static bool round_order(const LowPower *me)
{
    return ((uint8_t)KODE_LP_OFF == me->target)
           && ((uint8_t)KODE_LP_CAUSE_REQUEST == me->cause)
           && (!me->rest_on);
}

/**
 * @brief Leave the round: the board is AWAKE and the owners are told.
 *
 * The caller has set the refusal. STATE(AWAKE) goes out (prev AWAKE, or ASLEEP
 * for an OFF round that began from ASLEEP) so an owner that already rested for
 * the round restores. Nobody is answered here.
 *
 * @param me    The AO.
 * @param cause What ended the round, a kode_lp_cause_t.
 */
static void round_leave(LowPower * const me, uint8_t cause)
{
    uint8_t const prev = me->state;

    tmo_stop(&me->round_tmo);
    if ( !me->retry_on )
    {
        /* An order's wait to ask a busy owner again (round_order()). */
        tmo_stop(&me->retry_tmo);
    }
    me->waiting = 0U;
    me->busy    = 0U;
    me->state = (uint8_t)KODE_LP_AWAKE;
    KLOG_N(TAG, "staying AWAKE: round %u to %s abandoned, %s",
           (unsigned)me->round, state_name(me->target), s_refusal);
    publish_state(me, me->state, cause, prev);
}

/**
 * @brief Abandon the round for good: the board stays AWAKE and whoever asked is told.
 *
 * The caller has set the refusal.
 *
 * @param me    The AO.
 * @param cause What ended the round, a kode_lp_cause_t.
 * @return The transition back to AWAKE.
 */
static QState round_abort(LowPower * const me, uint8_t cause)
{
    round_leave(me, cause);
    refusal_publish(me->round_src);
    return Q_TRAN(&LowPower_awake);
}

/**
 * @brief Stop waiting to begin a requested round again.
 *
 * @param me     The AO.
 * @param answer true to tell the requester with the last refusal.
 */
static void retry_stop(LowPower * const me, bool answer)
{
    if ( me->retry_on )
    {
        me->retry_on = false;
        tmo_stop(&me->retry_tmo);
        if ( answer )
        {
            refusal_publish(me->round_src);
        }
    }
}

/**
 * @brief Abandon the round because an owner was busy or did not answer.
 *
 * A round that a request began is not given up while the window of the
 * request lasts: the board is AWAKE and the round begins again after
 * KODE_LOWPOWER_REQUEST_RETRY_MS, and nobody is answered. Any other round,
 * or a request whose window is over, ends as round_abort() ends it.
 *
 * @param me    The AO.
 * @param cause What ended the round, a kode_lp_cause_t.
 * @return The transition back to AWAKE.
 */
static QState round_busy(LowPower * const me, uint8_t cause)
{
    QState status;
    bool const again = ((uint8_t)KODE_LP_CAUSE_REQUEST == me->cause)
                       && ((khal_time_us() - me->request_us) < RETRY_WINDOW_US);

    if ( again )
    {
        round_leave(me, cause);
        me->retry_on = true;
        tmo_start(&me->retry_tmo, KODE_LOWPOWER_REQUEST_RETRY_MS);
        KLOG_N(TAG, "the request for %s is asked again in %u ms",
               state_name(me->target), (unsigned)KODE_LOWPOWER_REQUEST_RETRY_MS);
        status = Q_TRAN(&LowPower_awake);
    }
    else
    {
        status = round_abort(me, cause);
    }
    return status;
}

/**
 * @brief Begin a round for @p target and ask every participant.
 *
 * @param me     The AO, AWAKE, or ASLEEP with the tick and the lock back.
 * @param target The state wanted, ASLEEP or OFF.
 * @param cause  The kode_lp_cause_t.
 * @param src    Who asked, KODE_LOG_SRC_NONE when the cause is not a request;
 *               kept so an abandoned round answers the port that asked.
 * @return The transition to Resting, to the new state when nobody joined, or
 *         to AWAKE when no event was left to ask with.
 */
static QState round_begin(LowPower * const me, uint8_t target, uint8_t cause,
                          uint8_t src)
{
    /* Failing to begin is a transition to AWAKE: a no-op from AWAKE, and the
     * way home for a request for OFF that woke the board from ASLEEP first. */
    QState status = Q_TRAN(&LowPower_awake);
    LowPowerRoundEvt *ev = Q_NEW_X(LowPowerRoundEvt, 0U, LOWPOWER_ROUND_SIG);

    s_refusal[0] = '\0';
    if ( NULL == ev )
    {
        refusal_set(WHO_REQUEST, "out of events");
        KLOG_W(TAG, "no event to ask the round with; staying AWAKE");
        if ( (uint8_t)KODE_LP_AWAKE != me->state )
        {
            /* Began from ASLEEP: the tick and the lock are back, so say so. */
            uint8_t const prev = me->state;

            me->state = (uint8_t)KODE_LP_AWAKE;
            publish_state(me, me->state, cause, prev);
        }
    }
    else
    {
        me->sealed = true;
        me->round += 1U;
        if ( ROUND_NONE == me->round )
        {
            me->round += 1U;
        }
        me->target  = target;
        me->cause   = cause;
        me->round_src = src;
        retry_stop(me, false);
        me->wake_due_us = 0U;   /* the owners say their times again */
        me->app_due_us  = 0U;
        /* A state still owed is dead: a round has begun, and state_retry_fire()
         * only reads the state, which stays AWAKE while the board rests. The
         * round's own end (or its abandonment) says the state. */
        me->state_owed = false;
        (void)QTimeEvt_disarm(&me->state_retry);
        me->waiting = 0U;
        me->busy    = 0U;
        for ( uint8_t i = 0U; i < me->joined; i++ )
        {
            me->waiting |= (1U << i);
        }
        ev->target = target;
        ev->round  = me->round;
        QACTIVE_PUBLISH(&ev->super, &me->super);
        KLOG_I(TAG, "round %u to %s (%s): asking %u participants",
               (unsigned)me->round, state_name(target), cause_name(cause),
               (unsigned)me->joined);

        if ( 0U == me->waiting )
        {
            status = round_done(me);
        }
        else if ( round_order(me) )
        {
            tmo_start(&me->round_tmo, KODE_LOWPOWER_ORDER_MS);
            status = Q_TRAN(&LowPower_resting);
        }
        else
        {
            tmo_start(&me->round_tmo, KODE_LOWPOWER_ROUND_MS);
            status = Q_TRAN(&LowPower_resting);
        }
    }
    return status;
}

/* ---- The summary of the rest ------------------------------------------------------ */

/**
 * @brief Begin the summary of a rest that starts now.
 *
 * No memory for it is not an error worth stopping a sleep for: the rest goes
 * on without a summary.
 *
 * @param me The AO, entering ASLEEP.
 */
static void summary_open(LowPower * const me)
{
    khal_free(me->last);
    me->last       = NULL;
    me->sample_due = false;
    me->save_due   = false;
    khal_power_stats(&me->stats0);
    kdrv_radio_sleep_stats(&me->radio0);
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
    me->last = (lowpower_last_t *)khal_alloc_psram(sizeof(uint32_t),
                                                   sizeof(lowpower_last_t));
    if ( NULL == me->last )
    {
        KLOG_W(TAG, "no memory for the summary of this rest; it is not kept");
    }
    else
    {
        uint32_t wall_s = 0U;

        if ( khal_time_wall_known() )
        {
            wall_s = (uint32_t)(khal_time_wall_us() / US_PER_S);
        }
        lowpower_last_begin(me->last, wall_s);
    }
}

/**
 * @brief Bring the summary's totals up to @p now.
 *
 * @param me  The AO, with a summary.
 * @param now khal_time_us().
 */
static void summary_totals(LowPower * const me, uint64_t now)
{
    khal_power_stats_t       st;
    kdrv_radio_sleep_stats_t rs;
    lowpower_last_t * const  l = me->last;
    uint64_t pct = 0U;
    uint64_t span_us;

    khal_power_stats(&st);
    kdrv_radio_sleep_stats(&rs);
    span_us = st.since_us - me->stats0.since_us;
    if ( 0U != span_us )
    {
        pct = ((st.slept_us - me->stats0.slept_us) * KSET_PCT_MAX) / span_us;
        if ( KSET_PCT_MAX < pct )
        {
            pct = KSET_PCT_MAX;
        }
    }
    l->duration_s = (uint32_t)((now - me->asleep_since_us) / US_PER_S);
    l->slept_pct  = (uint8_t)pct;
    l->sleeps     = st.sleeps - me->stats0.sleeps;
    l->wakes_c5   = rs.wakes_c5 - me->radio0.wakes_c5;
    l->wakes_tx   = rs.wakes_tx - me->radio0.wakes_tx;
    l->deaf       = rs.deaf - me->radio0.deaf;
}

/**
 * @brief The reading the minute asked for arrived: keep it, and write the summary.
 *
 * @param me The AO, ASLEEP.
 * @param p  The reading, with no cable in it.
 */
static void summary_sample(LowPower * const me, const PowerEvt *p)
{
    uint64_t const now = khal_time_us();

    me->sample_due = false;
    if ( NULL != me->last )
    {
        lowpower_last_add(me->last,
                          (uint32_t)((now - me->asleep_since_us) / US_PER_S),
                          p->current_ma, p->vbat_mv);
        summary_totals(me, now);
        me->save_due = true;
    }
}

/**
 * @brief Write the working copy to the store if a sample is waiting there.
 *
 * Called at the start of a minute, before the glass is asked for the time:
 * the store's flash erase and the panel's GRAM write are never in flight
 * together.
 *
 * @param me The AO, ASLEEP.
 */
static void summary_flush(LowPower * const me)
{
    if ( me->save_due && (NULL != me->last) )
    {
        (void)lowpower_last_save(me->last);
    }
    me->save_due = false;
}

/**
 * @brief The rest is over: close the summary with what ended it, and let it go.
 *
 * @param me    The AO, the lock back.
 * @param cause The kode_lp_cause_t that ended it.
 */
static void summary_close(LowPower * const me, uint8_t cause)
{
    me->sample_due = false;
    me->save_due   = false;
    if ( NULL != me->last )
    {
        if ( 0U < me->last->count )
        {
            summary_totals(me, khal_time_us());
            me->last->end_cause = cause;
            (void)lowpower_last_save(me->last);
        }
        khal_free(me->last);
        me->last = NULL;
    }
}

/* ---- The way up ----------------------------------------------------------------- */

/** @brief The tick, then the lock: what the P4 needs before anything else runs. */
static void light_up(void)
{
    kqp_tick_rest(false);
    khal_console_rest(false);
    khal_power_hold(true);
}

/**
 * @brief A radio wake is over, one way or another: forget what it counted.
 *
 * The tick and the lock are the caller's to settle. The round's one-shot is
 * stopped only when this wake's own round is the one it times.
 *
 * @param me The AO.
 */
static void radio_clear(LowPower * const me)
{
    tmo_stop(&me->radio_tmo);
    if ( me->radio_resting )
    {
        tmo_stop(&me->round_tmo);
    }
    me->radio         = false;
    me->radio_resting = false;
    me->radio_closing = false;
    me->radio_pending = 0U;
    me->waiting       = 0U;
}

/**
 * @brief Have the tick and the lock back, from either phase of ASLEEP.
 *
 * A radio wake has them already.
 *
 * @param me The AO, ASLEEP.
 */
static void rest_up(LowPower * const me)
{
    if ( me->radio )
    {
        radio_clear(me);
    }
    else
    {
        light_up();
    }
}

/**
 * @brief Leave ASLEEP: the tick, then the lock, then the news.
 *
 * @param me    The AO, ASLEEP, or in a radio wake.
 * @param cause The kode_lp_cause_t that woke the board.
 */
static void wake_up(LowPower * const me, uint8_t cause)
{
    uint8_t const prev = me->state;
    /* A radio wake counted itself when it began. */
    bool const counted = me->radio && ((uint8_t)KODE_LP_CAUSE_TIMER == cause);

    rest_up(me);
    me->state = (uint8_t)KODE_LP_AWAKE;
    me->wake_due_us = 0U;
    me->app_due_us  = 0U;
    /* An owner's time is nobody at the board; every other wake is somebody. */
    me->used    = ((uint8_t)KODE_LP_CAUSE_TIMER != cause);
    me->rest_on = false;
    if ( (LP_CAUSES > cause) && (!counted) )
    {
        me->wakes[cause] += 1U;
    }
    KLOG_N(TAG, "AWAKE again (%s)", cause_name(cause));
    publish_state(me, me->state, cause, prev);
    summary_close(me, cause);
}

/* ---- Requests --------------------------------------------------------------------- */

/**
 * @brief Refuse a request, saying so.
 *
 * @param r   The request.
 * @param why The reason.
 */
static void lp_request_refuse(const LowPowerReqEvt *r, const char *why)
{
    refusal_set(WHO_REQUEST, why);
    KLOG_N(TAG, "request for %s (source %u) refused: %s",
           state_name(r->target), (unsigned)r->src, why);
    refusal_publish(r->src);
}

/**
 * @brief The request a waiting retry gives way to, or none: end the wait.
 *
 * A new request takes the old one's place, and so does one for AWAKE. The
 * requester that waited is told with the last refusal, unless it is the one
 * asking again: it waits for one answer and the new request gives it.
 *
 * @param me The AO, AWAKE.
 * @param r  The request.
 */
static void awake_request_replaces(LowPower * const me, const LowPowerReqEvt *r)
{
    if ( me->retry_on )
    {
        bool const same = (me->round_src == r->src);

        KLOG_N(TAG, "request for %s (source %u) replaces the one waiting",
               state_name(r->target), (unsigned)r->src);
        retry_stop(me, !same);
    }
}

/**
 * @brief A request while AWAKE.
 *
 * @param me The AO.
 * @param r  The request.
 * @return The QP status.
 */
static QState awake_request(LowPower * const me, const LowPowerReqEvt *r)
{
    QState status = Q_HANDLED();

    me->rest_on = false;
    if ( (uint8_t)KODE_LP_OFF < r->target )
    {
        lp_request_refuse(r, "no such state");
    }
    else
    {
        awake_request_replaces(me, r);
        if ( (uint8_t)KODE_LP_AWAKE == r->target )
        {
            KLOG_I(TAG, "already AWAKE");
        }
        else if ( !me->power_ok )
        {
            lp_request_refuse(r, "power management did not start");
        }
        else if ( me->vbus )
        {
            refusal_set(WHO_CABLE, WHY_CABLE);
            KLOG_N(TAG, "request for %s (source %u) refused: %s",
                   state_name(r->target), (unsigned)r->src, WHY_CABLE);
            refusal_publish(r->src);
        }
        else
        {
            KLOG_N(TAG, "request for %s (source %u)", state_name(r->target),
                   (unsigned)r->src);
            me->request_us = khal_time_us();
            status = round_begin(me, r->target, (uint8_t)KODE_LP_CAUSE_REQUEST,
                                 r->src);
        }
    }
    return status;
}

/**
 * @brief A request to rest once an owner's work is done, while AWAKE.
 *
 * A request for ASLEEP when nobody used the board since it last woke, answered
 * once by the new state or a refusal. It is refused "request: request pending"
 * while another request waits to begin again (which keeps its retry and its
 * refusal), and "input: board used" when somebody used the board.
 *
 * @param me The AO.
 * @param r  The request, for ASLEEP.
 * @return The QP status.
 */
static QState awake_rest(LowPower * const me, const LowPowerReqEvt *r)
{
    QState status = Q_HANDLED();

    if ( me->retry_on && (!me->rest_on) )
    {
        char keep[KODE_LOWPOWER_REFUSAL_MAX];

        /* The waiting request is answered with s_refusal if its window runs
         * out: this refusal is the rest's and must not take its place. */
        (void)memcpy(keep, s_refusal, sizeof(keep));
        KLOG_N(TAG, "rest asked (source %u), but the request for %s waits to begin again",
               (unsigned)r->src, state_name(me->target));
        refusal_set(WHO_REQUEST, WHY_PENDING);
        refusal_publish(r->src);
        (void)memcpy(s_refusal, keep, sizeof(s_refusal));
    }
    else if ( me->used )
    {
        KLOG_I(TAG, "rest asked (source %u), but the board is in use",
               (unsigned)r->src);
        refusal_set(WHO_INPUT, WHY_INPUT);
        refusal_publish(r->src);
    }
    else
    {
        status = awake_request(me, r);
        me->rest_on = true;
    }
    return status;
}

/**
 * @brief A request while ASLEEP.
 *
 * @param me The AO.
 * @param r  The request.
 * @return The QP status.
 */
static QState asleep_request(LowPower * const me, const LowPowerReqEvt *r)
{
    QState status = Q_HANDLED();

    me->rest_on = false;
    if ( (uint8_t)KODE_LP_OFF < r->target )
    {
        lp_request_refuse(r, "no such state");
    }
    else if ( (uint8_t)KODE_LP_ASLEEP == r->target )
    {
        KLOG_I(TAG, "already ASLEEP");
    }
    else if ( (uint8_t)KODE_LP_AWAKE == r->target )
    {
        wake_up(me, (uint8_t)KODE_LP_CAUSE_REQUEST);
        status = Q_TRAN(&LowPower_awake);
    }
    else
    {
        /* Direct, with no STATE(AWAKE): the owners pass from their ASLEEP rest
         * to their OFF rest, and nothing lights between. The state stays
         * ASLEEP until the round decides. */
        KLOG_N(TAG, "request for OFF (source %u) while ASLEEP", (unsigned)r->src);
        rest_up(me);
        summary_close(me, (uint8_t)KODE_LP_CAUSE_REQUEST);
        me->request_us = khal_time_us();
        status = round_begin(me, r->target, (uint8_t)KODE_LP_CAUSE_REQUEST,
                             r->src);
    }
    return status;
}

/* ---- The inputs ----------------------------------------------------------------------- */

/**
 * @brief Whether an expander reading is a fresh press of BOT.
 *
 * The edge and not the level: a thumb that is still down from asking for a
 * sleep is not the user waking it.
 *
 * @param in The reading.
 * @return true when BOT went down in it.
 */
static bool bot_pressed(const IoexpInputEvt *in)
{
    return in->ok && ((in->changed & BOT_MASK) != 0U)
           && ((in->levels & BOT_MASK) == 0U);
}

/**
 * @brief What woke the board, from the port of the line that did.
 *
 * A line from the Kode channel is an MQTT wake and one from BLE a BLE wake.
 * A line on the serial console can only come over a cable, and a cable is why
 * the board answers it.
 *
 * @param src The KODE_CONSOLE_SRC_* port.
 * @return The kode_lp_cause_t.
 */
static uint8_t input_cause(uint8_t src)
{
    uint8_t cause = (uint8_t)KODE_LP_CAUSE_VBUS;

    if ( (uint8_t)KODE_CONSOLE_SRC_NET == src )
    {
        cause = (uint8_t)KODE_LP_CAUSE_MQTT;
    }
    else if ( (uint8_t)KODE_CONSOLE_SRC_BLE == src )
    {
        cause = (uint8_t)KODE_LP_CAUSE_BLE;
    }
    else
    {
        /* The cable. */
    }
    return cause;
}

/**
 * @brief Whether a use of the board abandons the round that is running.
 *
 * A round that idle time began, and one that a rest asked for: a person at the
 * board wins over them. A round somebody else asked for stands (BOT, handled
 * apart, is the exception).
 *
 * @param me The AO, Resting.
 * @return true when an input abandons the round.
 */
static bool input_abandons(const LowPower *me)
{
    return ((uint8_t)KODE_LP_CAUSE_IDLE == me->cause)
           || (((uint8_t)KODE_LP_CAUSE_REQUEST == me->cause) && me->rest_on);
}

/**
 * @brief Whether a console line is the scheduler's own, and so not use of the board.
 *
 * @param src The KODE_CONSOLE_SRC_* port.
 * @return true for KODE_CONSOLE_SRC_SCHED.
 */
static bool input_is_sched(uint8_t src)
{
    return (uint8_t)KODE_CONSOLE_SRC_SCHED == src;
}

/**
 * @brief A real input reached the board while AWAKE.
 *
 * The board is in use from here until its next wake. The idle time starts
 * over, and a rest still waiting to begin again is given up, answered with
 * the refusal: it must not put the board to sleep under the person's hand.
 *
 * @param me The AO, AWAKE.
 */
static void awake_input(LowPower * const me)
{
    me->used = true;
    idle_restart(me);
    if ( me->retry_on && me->rest_on )
    {
        refusal_set(WHO_INPUT, WHY_INPUT);
        KLOG_N(TAG, "%s: the rest is not asked again", WHY_INPUT);
        retry_stop(me, true);
    }
}

/**
 * @brief The idle time ran out while AWAKE.
 *
 * @param me The AO, AWAKE.
 * @return The QP status.
 */
static QState awake_idle(LowPower * const me)
{
    QState status = Q_HANDLED();

    if ( me->vbus )
    {
        /* A cable never sleeps; look again after another idle time. */
        KLOG_I(TAG, "idle, but %s: staying AWAKE", WHY_CABLE);
        idle_restart(me);
    }
    else if ( me->retry_on )
    {
        /* A request is waiting to begin again, and it decides. */
        idle_restart(me);
    }
    else
    {
        KLOG_N(TAG, "no input for %u s", (unsigned)me->secs[SETTING_SLEEP_AFTER]);
        status = round_begin(me, (uint8_t)KODE_LP_ASLEEP,
                             (uint8_t)KODE_LP_CAUSE_IDLE,
                             (uint8_t)KODE_LOG_SRC_NONE);
    }
    return status;
}

/**
 * @brief The wait before a requested round begins again ran out.
 *
 * Past the window the requester is answered with the last refusal. Inside
 * it the round begins again, from the same request.
 *
 * @param me The AO, AWAKE.
 * @return The QP status.
 */
static QState awake_retry(LowPower * const me)
{
    QState status = Q_HANDLED();
    uint8_t const src = me->round_src;

    if ( me->retry_on )
    {
        if ( (khal_time_us() - me->request_us) >= RETRY_WINDOW_US )
        {
            KLOG_N(TAG, "the request for %s gave up after %u ms: %s",
                   state_name(me->target),
                   (unsigned)KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS, s_refusal);
            retry_stop(me, true);
        }
        else
        {
            uint32_t const before = me->round;

            KLOG_N(TAG, "the request for %s (source %u) begins again",
                   state_name(me->target), (unsigned)src);
            me->retry_on = false;
            status = round_begin(me, me->target, (uint8_t)KODE_LP_CAUSE_REQUEST,
                                 src);
            if ( me->round == before )
            {
                /* No event to ask with: the refusal says so, and nobody else
                 * will tell whoever asked. */
                refusal_publish(src);
            }
        }
    }
    return status;
}

/* ---- The minute ------------------------------------------------------------------------- */

/** @brief Runs in the timer task: tell AO_LowPower a one-shot came due. */
static void minute_due_cb(void *arg)
{
    static const QEvt evt = QEVT_INITIALIZER(LOWPOWER_MINUTE_SIG);

    (void)arg;
    QACTIVE_POST(AO_LowPower, &evt, NULL);
}

/**
 * @brief Arm the one-shot that posts LOWPOWER_MINUTE_SIG.
 *
 * Not armed when every slot is taken by timers of an earlier sleep that have
 * not come due: the first of them to do so finds nothing outstanding and
 * arms the rest (minute_heard()).
 *
 * @param me       The AO.
 * @param delay_us How long from now.
 */
static void minute_arm(LowPower * const me, uint64_t delay_us)
{
    if ( KHAL_TIMER_ONCE_SLOTS <= me->minute_out )
    {
        KLOG_D(TAG, "all %u one-shots are out; the minute waits for one",
               (unsigned)KHAL_TIMER_ONCE_SLOTS);
    }
    else if ( khal_timer_once(&minute_due_cb, NULL, delay_us) == KODE_OK )
    {
        me->minute_out += 1U;
    }
    else
    {
        KLOG_W(TAG, "the minute's timer would not start; ASLEEP has no clock");
    }
}

/**
 * @brief Microseconds from @p now to the next minute of the wall clock.
 *
 * A whole minute when the clock is not set, and when it is on the boundary.
 *
 * @return The wait, at least 1.
 */
static uint64_t minute_wait_us(void)
{
    uint64_t wait = US_PER_MINUTE;

    if ( khal_time_wall_known() )
    {
        wait = US_PER_MINUTE - (khal_time_wall_us() % US_PER_MINUTE);
    }
    return wait;
}

/**
 * @brief Whether the time asleep has reached off_after.
 *
 * @param me  The AO.
 * @param now khal_time_us().
 * @return true when the board is to go OFF.
 */
static bool off_due(const LowPower *me, uint64_t now)
{
    uint64_t const limit = (uint64_t)me->secs[SETTING_OFF_AFTER] * US_PER_S;

    return (0U != limit) && ((now - me->off_since_us) >= limit);
}

/**
 * @brief Whether a time has come.
 *
 * @param due_us The time an owner gave; 0 for none.
 * @param now    khal_time_us().
 * @return true when a time was given and it has come.
 */
static bool time_due(uint64_t due_us, uint64_t now)
{
    return (0U != due_us) && (now >= due_us);
}

/**
 * @brief Microseconds from @p now to a time an owner gave.
 *
 * @param due_us The time; 0 for none.
 * @param now    khal_time_us().
 * @return The wait, at least 1 (a time that has come is due at once); 0 when
 *         no time was given.
 */
static uint64_t time_wait_us(uint64_t due_us, uint64_t now)
{
    uint64_t wait = 0U;

    if ( 0U != due_us )
    {
        wait = 1U;
        if ( due_us > now )
        {
            wait = due_us - now;
        }
    }
    return wait;
}

/**
 * @brief Microseconds from @p now to the first owner's time the one-shot serves.
 *
 * Another owner's time always counts. A radio owner's counts only outside a
 * radio wake, which is awake for the radio owners' work already.
 *
 * @param me  The AO.
 * @param now khal_time_us().
 * @return The wait, at least 1; 0 when no owner gave a time that counts.
 */
static uint64_t owner_wait_us(const LowPower *me, uint64_t now)
{
    uint64_t wait = time_wait_us(me->app_due_us, now);

    if ( !me->radio )
    {
        uint64_t const radio = time_wait_us(me->wake_due_us, now);

        if ( (0U != radio) && ((0U == wait) || (radio < wait)) )
        {
            wait = radio;
        }
    }
    return wait;
}

/**
 * @brief Arm the one-shot for the next thing ASLEEP has to do.
 *
 * That is the next minute, the off-after deadline or an owner's work,
 * whichever is first.
 *
 * @param me  The AO, ASLEEP.
 * @param now khal_time_us().
 */
static void minute_schedule(LowPower * const me, uint64_t now)
{
    uint64_t wait = minute_wait_us();
    uint64_t const limit = (uint64_t)me->secs[SETTING_OFF_AFTER] * US_PER_S;
    uint64_t const owner = owner_wait_us(me, now);

    if ( 0U != limit )
    {
        /* At least 1: a count kept across a wake may have run out already. */
        uint64_t const left = time_wait_us(me->off_since_us + limit, now);

        if ( left < wait )
        {
            wait = left;
        }
    }
    if ( (0U != owner) && (owner < wait) )
    {
        wait = owner;
    }
    me->minute_due_us = now + wait;
    me->boundary_us   = now + minute_wait_us();
    minute_arm(me, wait);
}

/** @brief A one-shot's event arrived, whatever the state. */
static void minute_heard(LowPower * const me)
{
    if ( 0U < me->minute_out )
    {
        me->minute_out -= 1U;
    }
}

/** @brief The name of participant @p who, for the log. */
static const char *owner_name(const LowPower *me, uint8_t who)
{
    const char *name = "an owner";

    if ( who < me->joined )
    {
        name = me->parts[who].name;
    }
    return name;
}

/**
 * @brief Whether participant @p who takes part in radio wakes.
 *
 * @param me  The AO.
 * @param who The participant.
 * @return true for a radio owner.
 */
static bool is_radio_owner(const LowPower *me, uint8_t who)
{
    return (WAITING_BITS > who) && ((me->radio_mask & ((uint32_t)1U << who)) != 0U);
}

/**
 * @brief LOWPOWER_WAKE_AT_SIG: keep the earliest time a radio owner gave, and
 *        the earliest any other owner gave, apart.
 *
 * Apart because a radio wake asks the radio owners alone to say their times
 * again: another owner's time must outlive it.
 *
 * @param me The AO.
 * @param w  The note.
 */
static void wake_store(LowPower * const me, const LowPowerWakeAtEvt *w)
{
    if ( 0U != w->due_us )
    {
        if ( is_radio_owner(me, w->who) )
        {
            if ( (0U == me->wake_due_us) || (w->due_us < me->wake_due_us) )
            {
                me->wake_due_us = w->due_us;
                me->wake_who    = w->who;
            }
        }
        else if ( (0U == me->app_due_us) || (w->due_us < me->app_due_us) )
        {
            me->app_due_us = w->due_us;
            me->app_who    = w->who;
        }
        else
        {
            /* Later than the one kept. */
        }
    }
}

/**
 * @brief An owner's time arrived while ASLEEP: the one-shot must not be later.
 *
 * @param me  The AO, ASLEEP.
 */
static void wake_fold(LowPower * const me)
{
    uint64_t const now   = khal_time_us();
    uint64_t const owner = owner_wait_us(me, now);

    if ( (0U != owner) && ((now + owner) < me->minute_due_us) )
    {
        me->minute_due_us = now + owner;
        minute_arm(me, owner);
    }
}

/* ---- The radio wake ------------------------------------------------------------------ */

/**
 * @brief An owner's time came, and it is a radio owner's: wake the radio alone.
 *
 * The one-shot that came was this wake's, so the minute is armed again; the
 * radio owners' time is forgotten, and they say theirs again in the round
 * that ends the wake. Another owner's time is kept: that round does not ask
 * it.
 *
 * @param me  The AO, ASLEEP.
 * @param now khal_time_us().
 * @return The transition into the radio wake.
 */
static QState radio_begin(LowPower * const me, uint64_t now)
{
    KLOG_N(TAG, "%s has work due: a radio wake, the glass stays asleep",
           owner_name(me, me->wake_who));
    me->wake_due_us = 0U;
    minute_schedule(me, now);
    return Q_TRAN(&LowPower_radio);
}

/**
 * @brief Entering the radio wake: the tick, the lock, then the news.
 *
 * @param me The AO, ASLEEP.
 */
static void radio_enter(LowPower * const me)
{
    light_up();
    me->radio         = true;
    me->radio_resting = false;
    me->radio_closing = false;
    me->radio_pending = me->radio_mask;
    me->waiting       = 0U;
    me->wakes[KODE_LP_CAUSE_TIMER] += 1U;
    tmo_start(&me->radio_tmo, KODE_LOWPOWER_RADIO_MAX_MS);
    (void)state_send(me, (uint8_t)KODE_LP_RADIO, (uint8_t)KODE_LP_CAUSE_TIMER,
                     (uint8_t)KODE_LP_ASLEEP);
}

/**
 * @brief Leaving the radio wake by any transition: its one-shot ends with it.
 *
 * What the transition's own handler left to settle (the tick, the lock, a
 * round it began) is not touched here.
 *
 * @param me The AO.
 */
static void radio_exit(LowPower * const me)
{
    tmo_stop(&me->radio_tmo);
    me->radio         = false;
    me->radio_resting = false;
    me->radio_closing = false;
}

/**
 * @brief The radio wake cannot rest again: the board is whole AWAKE.
 *
 * The owners that rested for ASLEEP restore on the STATE(AWAKE); the radio
 * owners restore again, which is idempotent. The caller has set the refusal.
 *
 * @param me The AO, in the radio wake.
 * @return The transition to AWAKE.
 */
static QState radio_abandon(LowPower * const me)
{
    KLOG_N(TAG, "radio wake cannot rest again (%s): AWAKE", s_refusal);
    wake_up(me, (uint8_t)KODE_LP_CAUSE_TIMER);
    refusal_publish((uint8_t)KODE_LOG_SRC_NONE);
    return Q_TRAN(&LowPower_awake);
}

/**
 * @brief Post the round for ASLEEP to the radio owners, one event each.
 *
 * One event for each, because a dynamic event posted to two queues needs a
 * reference held across the posts, and publishing is what holds one.
 *
 * @param me The AO.
 * @return false when an event was missing; some owners may have been asked.
 */
static bool radio_round_post(LowPower * const me)
{
    bool ok = true;

    for ( uint8_t i = 0U; i < me->joined; i++ )
    {
        if ( (me->radio_mask & (1U << i)) != 0U )
        {
            LowPowerRoundEvt *ev = Q_NEW_X(LowPowerRoundEvt, 0U, LOWPOWER_ROUND_SIG);

            if ( NULL == ev )
            {
                ok = false;
            }
            else
            {
                ev->target = (uint8_t)KODE_LP_ASLEEP;
                ev->round  = me->round;
                QACTIVE_POST(me->parts[i].ao, &ev->super, NULL);
            }
        }
    }
    return ok;
}

/**
 * @brief Ask the radio owners to rest again: the end of the radio wake.
 *
 * @param me The AO, in the radio wake.
 * @return The QP status: handled while the round is out, or the way home to
 *         AWAKE when it could not be asked.
 */
static QState radio_rest(LowPower * const me)
{
    QState status = Q_HANDLED();

    if ( !me->radio_resting )
    {
        me->round += 1U;
        if ( ROUND_NONE == me->round )
        {
            me->round += 1U;
        }
        me->target        = (uint8_t)KODE_LP_ASLEEP;
        me->cause         = (uint8_t)KODE_LP_CAUSE_TIMER;
        me->round_src     = (uint8_t)KODE_LOG_SRC_NONE;
        me->wake_due_us   = 0U;   /* the radio owners say their times again */
        me->waiting       = me->radio_mask;
        me->radio_resting = true;
        tmo_stop(&me->radio_tmo);
        if ( radio_round_post(me) )
        {
            KLOG_I(TAG, "round %u to ASLEEP (radio wake): asking the radio owners",
                   (unsigned)me->round);
            tmo_start(&me->round_tmo, KODE_LOWPOWER_ROUND_MS);
        }
        else
        {
            refusal_set(WHO_REQUEST, "out of events");
            status = radio_abandon(me);
        }
    }
    return status;
}

/**
 * @brief The radio wake lets go: the tick stops, then the lock goes.
 *
 * Runs once STATE(ASLEEP) is on the bus, never before.
 *
 * @param me The AO, in the radio wake, its ASLEEP published.
 * @return The transition back to ASLEEP, without leaving it.
 */
static QState radio_release(LowPower * const me)
{
    radio_clear(me);
    KLOG_N(TAG, "radio wake done: ASLEEP again, the glass was not lit");
    wake_fold(me);   /* a time the owners gave in the round */
    kqp_tick_rest(true);
    khal_console_rest(true);
    khal_power_hold(false);
    return Q_TRAN(&LowPower_asleep);
}

/**
 * @brief The radio wake is over: ASLEEP again, nothing else woken.
 *
 * The state is published before the tick stops and the lock goes (rule 6).
 * When no event can carry it, the tick and the lock stay and the state is
 * retried on the timer; radio_retry() releases once it went out, so a rest
 * never begins with Net and Radio still in RADIO.
 *
 * @param me The AO, in the radio wake, every radio owner at rest.
 * @return The transition back to ASLEEP, or handled while the state is owed.
 */
static QState radio_back(LowPower * const me)
{
    QState status;

    tmo_stop(&me->round_tmo);
    me->radio_resting = false;
    me->radio_pending = 0U;
    me->waiting       = 0U;
    if ( state_send(me, (uint8_t)KODE_LP_ASLEEP, (uint8_t)KODE_LP_CAUSE_TIMER,
                   (uint8_t)KODE_LP_RADIO) )
    {
        status = radio_release(me);
    }
    else
    {
        me->radio_closing = true;
        KLOG_W(TAG, "radio wake: ASLEEP not published; the wake holds the "
                    "tick and the lock until it is");
        status = Q_HANDLED();
    }
    return status;
}

/**
 * @brief LOWPOWER_STATE_RETRY_SIG in the radio wake.
 *
 * A RADIO that went out late changes nothing else. The ASLEEP that ends the
 * wake, once out, lets the wake go.
 *
 * @param me The AO, in the radio wake.
 * @return The QP status.
 */
static QState radio_retry(LowPower * const me)
{
    QState status = Q_HANDLED();
    bool const closing = me->radio_closing;

    if ( state_retry_fire(me) && closing )
    {
        status = radio_release(me);
    }
    return status;
}

/**
 * @brief LOWPOWER_RADIO_DONE_SIG: one radio owner's work is finished.
 *
 * @param me The AO, in the radio wake.
 * @param d  The note.
 * @return The QP status.
 */
static QState radio_done(LowPower * const me, const LowPowerRadioDoneEvt *d)
{
    QState status = Q_HANDLED();

    if ( WAITING_BITS > d->who )
    {
        uint32_t const bit = (uint32_t)1U << d->who;

        me->radio_pending &= ~bit;
    }
    if ( (0U == me->radio_pending) && (!me->radio_resting) && (!me->radio_closing) )
    {
        KLOG_I(TAG, "the radio owners are done");
        status = radio_rest(me);
    }
    return status;
}

/**
 * @brief A radio owner's answer to the radio wake's round.
 *
 * @param me The AO, in the radio wake.
 * @param a  The answer.
 * @return The QP status.
 */
static QState radio_ready(LowPower * const me, const LowPowerReadyEvt *a)
{
    QState status = Q_HANDLED();

    if ( (!me->radio_resting) || (a->round != me->round) || (a->who >= me->joined)
        || ((me->waiting & (1U << a->who)) == 0U) )
    {
        KLOG_D(TAG, "answer dropped: participant %u, round %u (this is %u)",
               (unsigned)a->who, (unsigned)a->round, (unsigned)me->round);
    }
    else if ( a->busy )
    {
        char why[KODE_LOWPOWER_WHY_MAX];

        why_copy(why, sizeof(why), a->why);
        refusal_set(me->parts[a->who].name, why);
        status = radio_abandon(me);
    }
    else
    {
        me->waiting &= ~(1U << a->who);
        if ( 0U == me->waiting )
        {
            status = radio_back(me);
        }
    }
    return status;
}

/**
 * @brief The radio wake's round ran out of time: name whoever is silent.
 *
 * @param me The AO, in the radio wake.
 * @return The QP status.
 */
static QState radio_round_timeout(LowPower * const me)
{
    QState status = Q_HANDLED();

    if ( me->radio_resting )
    {
        bool first = true;

        for ( uint8_t i = 0U; i < me->joined; i++ )
        {
            if ( (me->waiting & (1U << i)) != 0U )
            {
                KLOG_W(TAG, "%s did not answer round %u in %u ms",
                       me->parts[i].name, (unsigned)me->round,
                       (unsigned)KODE_LOWPOWER_ROUND_MS);
                if ( first )
                {
                    refusal_set(me->parts[i].name, "no answer");
                    first = false;
                }
            }
        }
        status = radio_abandon(me);
    }
    return status;
}

/**
 * @brief The minute's work: the summary, a reading of the cable, the glass.
 *
 * @param me  The AO, ASLEEP.
 */
static void minute_work(LowPower * const me)
{
    /* The cable shows in the reading, which AO_Power publishes; the glass
     * writes the minute and lets the link go again. An app without AO_Display
     * has no glass to light. */
    summary_flush(me);
    me->sample_due = true;
    power_peek_request();
    if ( kaos_linked(&kaos_display_start) )
    {
        display_minute_request();
    }
}

/**
 * @brief An owner's time has come and the board is not off: wake for it.
 *
 * Another owner's time wakes the whole board, even inside a radio wake. A
 * radio owner's wakes the radio alone; the minute's work, if it falls due in
 * the same wake, is done first, since the glass stays asleep and would
 * otherwise miss it.
 *
 * @param me  The AO, ASLEEP.
 * @param now khal_time_us().
 * @return The QP status.
 */
static QState asleep_owner(LowPower * const me, uint64_t now)
{
    QState status;

    if ( time_due(me->app_due_us, now) )
    {
        KLOG_N(TAG, "%s has work due: waking for it",
               owner_name(me, me->app_who));
        wake_up(me, (uint8_t)KODE_LP_CAUSE_TIMER);
        status = Q_TRAN(&LowPower_awake);
    }
    else
    {
        if ( now >= me->boundary_us )
        {
            minute_work(me);
        }
        status = radio_begin(me, now);
    }
    return status;
}

/**
 * @brief The minute event, ASLEEP: the minute's work, an owner's, or OFF.
 *
 * @param me The AO, ASLEEP.
 * @return The QP status.
 */
static QState asleep_minute(LowPower * const me)
{
    QState status = Q_HANDLED();
    uint64_t const now = khal_time_us();

    minute_heard(me);
    if ( now < me->minute_due_us )
    {
        /* A one-shot of an earlier sleep. If it was the last one out, the
         * minute still needs its own. */
        if ( 0U == me->minute_out )
        {
            minute_arm(me, me->minute_due_us - now);
        }
    }
    else if ( off_due(me, now) )
    {
        KLOG_N(TAG, "asleep for %u s: off-after reached",
               (unsigned)me->secs[SETTING_OFF_AFTER]);
        rest_up(me);
        summary_close(me, (uint8_t)KODE_LP_CAUSE_OFF_AFTER);
        status = round_begin(me, (uint8_t)KODE_LP_OFF,
                             (uint8_t)KODE_LP_CAUSE_OFF_AFTER,
                             (uint8_t)KODE_LOG_SRC_NONE);
    }
    else if ( time_due(me->app_due_us, now)
             || (time_due(me->wake_due_us, now) && (!me->radio)) )
    {
        status = asleep_owner(me, now);
    }
    else
    {
        minute_work(me);
        minute_schedule(me, now);
    }
    return status;
}

/**
 * @brief Entering ASLEEP: start counting the time asleep and the minute.
 *
 * The off-after count starts again only when somebody used the board since its
 * last wake. A wake for an owner's time that nobody used (`used` false: the
 * scheduler's entry, an unattended board) keeps the count it interrupted, so
 * entries that wake the board every hour do not keep it from switching off.
 *
 * @param me The AO.
 */
static void asleep_enter(LowPower * const me)
{
    uint64_t const now = khal_time_us();

    me->asleep_since_us = now;
    if ( me->used )
    {
        me->off_since_us = now;
    }
    me->minute_due_us   = 0U;
    summary_open(me);
    minute_schedule(me, now);
}

/* ---- The settings ------------------------------------------------------------------------- */

/**
 * @brief The setting a `config` key names.
 *
 * @param key The key.
 * @return Its index, or SETTING_COUNT when it is another owner's.
 */
static setting_id_t setting_find(const char *key)
{
    setting_id_t found = SETTING_COUNT;

    for ( uint8_t i = 0U; (uint8_t)SETTING_COUNT > i; i++ )
    {
        if ( strcmp(key, SETTINGS[i].key) == 0 )
        {
            found = (setting_id_t)i;
        }
    }
    return found;
}

/**
 * @brief Which word stands for @p secs.
 *
 * @param s    The setting.
 * @param secs The seconds.
 * @return The index into the words, or s->n when none does.
 */
static uint8_t setting_word(const Setting *s, SettingSecs secs)
{
    uint8_t at = s->n;

    for ( uint8_t i = 0U; i < s->n; i++ )
    {
        if ( s->secs[i] == secs )
        {
            at = i;
        }
    }
    return at;
}

/**
 * @brief Read a setting from the store.
 *
 * A value that is missing, the wrong size or not one the words list is the
 * default.
 *
 * @param s      The setting.
 * @param stored Set to whether the store held a usable value.
 * @return The seconds.
 */
static SettingSecs setting_load(const Setting *s, bool *stored)
{
    SettingSecs secs = s->def_s;
    SettingSecs got  = 0U;
    size_t      len  = sizeof(got);

    *stored = false;
    if ( (khal_nvs_get_blob(KSET_NAMESPACE, s->key, &got, &len) == KODE_OK)
        && (sizeof(got) == len) && (setting_word(s, got) < s->n) )
    {
        secs    = got;
        *stored = true;
    }
    return secs;
}

/**
 * @brief Answer `config key=...`: set it first when asked.
 *
 * @param me The AO, the answer's sender.
 * @param r  The request.
 * @return true when the idle time was changed, so AWAKE starts it over.
 */
static bool config_answer(LowPower * const me, const KodeConfigReqEvt *r)
{
    setting_id_t const id = setting_find(r->key);
    bool idle_changed = false;

    if ( SETTING_COUNT != id )
    {
        const Setting *s = &SETTINGS[id];
        bool stored = false;
        kode_status_t status = KODE_OK;
        const char *word = "";
        uint8_t at;

        if ( r->set && (r->e < s->n) )
        {
            SettingSecs const secs = s->secs[r->e];

            (void)khal_nvs_set_blob(KSET_NAMESPACE, s->key, &secs, sizeof(secs));
            me->secs[id] = secs;
            idle_changed = (SETTING_SLEEP_AFTER == id);
            KLOG_N(TAG, "%s %s (stored)", s->key, s->words[r->e]);
        }
        else if ( r->set )
        {
            status = KODE_DENIED;
        }
        else
        {
            /* A read. */
        }
        (void)setting_load(s, &stored);
        at = setting_word(s, me->secs[id]);
        if ( at < s->n )
        {
            word = s->words[at];
        }
        (void)kode_config_answer(&me->super, r, status, stored, word);
    }
    return idle_changed;
}

/* ---- The answers -------------------------------------------------------------------- */

/**
 * @brief A participant's answer while a round runs.
 *
 * Dropped, and logged at debug: an answer for another round, one from an id
 * that never joined, and a second one from a participant that already spoke.
 *
 * @param me The AO, Resting.
 * @param a  The answer.
 * @return The QP status.
 */
static QState resting_ready(LowPower * const me, const LowPowerReadyEvt *a)
{
    QState status = Q_HANDLED();

    if ( (a->round != me->round) || (a->who >= me->joined)
        || ((me->waiting & (1U << a->who)) == 0U) )
    {
        KLOG_D(TAG, "answer dropped: participant %u, round %u (this is %u)",
               (unsigned)a->who, (unsigned)a->round, (unsigned)me->round);
    }
    else if ( a->busy && round_order(me) )
    {
        char why[KODE_LOWPOWER_WHY_MAX];

        why_copy(why, sizeof(why), a->why);
        refusal_set(me->parts[a->who].name, why);
        me->busy |= (1U << a->who);
        KLOG_N(TAG, "OFF waits for %s (%s): asked again in %u ms",
               me->parts[a->who].name, why, (unsigned)KODE_LOWPOWER_REQUEST_RETRY_MS);
        if ( !me->retry_tmo.live )
        {
            tmo_start(&me->retry_tmo, KODE_LOWPOWER_REQUEST_RETRY_MS);
        }
    }
    else if ( a->busy )
    {
        char why[KODE_LOWPOWER_WHY_MAX];

        why_copy(why, sizeof(why), a->why);
        refusal_set(me->parts[a->who].name, why);
        status = round_busy(me, me->cause);
    }
    else
    {
        me->waiting &= ~(1U << a->who);
        me->busy    &= ~(1U << a->who);
        if ( 0U == me->waiting )
        {
            if ( round_order(me) )
            {
                tmo_stop(&me->retry_tmo);
            }
            status = round_done(me);
        }
    }
    return status;
}

/**
 * @brief The round's budget ran out: name whoever has not answered.
 *
 * An order goes OFF all the same (round_order()); any other round is given up.
 *
 * @param me The AO, Resting.
 * @return The transition back to AWAKE, or to OFF for an order.
 */
static QState resting_timeout(LowPower * const me)
{
    bool const order = round_order(me);
    uint32_t budget = KODE_LOWPOWER_ROUND_MS;
    bool first = true;
    QState status;

    if ( order )
    {
        budget = KODE_LOWPOWER_ORDER_MS;
    }
    for ( uint8_t i = 0U; i < me->joined; i++ )
    {
        if ( (me->waiting & (1U << i)) != 0U )
        {
            KLOG_W(TAG, "%s did not answer round %u in %u ms",
                   me->parts[i].name, (unsigned)me->round, (unsigned)budget);
            if ( first )
            {
                refusal_set(me->parts[i].name, "no answer");
                first = false;
            }
        }
    }
    if ( order )
    {
        KLOG_N(TAG, "OFF was ordered: going OFF without them");
        tmo_stop(&me->retry_tmo);
        me->busy = 0U;
        status   = round_done(me);
    }
    else
    {
        status = round_busy(me, me->cause);
    }
    return status;
}

/**
 * @brief The wait in an order ran out: ask the owners that were busy again.
 *
 * Each is posted its own copy of the round, the same number, and the others
 * are left at rest. One that cannot be posted to keeps its place: the order's
 * budget ends the wait for it (resting_timeout()).
 *
 * @param me The AO, Resting an order.
 */
static void order_ask_again(LowPower * const me)
{
    uint32_t const again = me->busy;

    me->busy = 0U;
    for ( uint8_t i = 0U; i < me->joined; i++ )
    {
        if ( (again & (1U << i)) != 0U )
        {
            LowPowerRoundEvt *ev = Q_NEW_X(LowPowerRoundEvt, 0U, LOWPOWER_ROUND_SIG);

            if ( NULL == ev )
            {
                KLOG_W(TAG, "no event to ask %s again", me->parts[i].name);
            }
            else
            {
                ev->target = me->target;
                ev->round  = me->round;
                if ( !QACTIVE_POST_X(me->parts[i].ao, &ev->super, 1U, &me->super) )
                {
                    KLOG_W(TAG, "%s's queue is full; not asked again", me->parts[i].name);
                }
            }
        }
    }
}

/* ---- States --------------------------------------------------------------------------- */

/**
 * @brief The outermost state: what no state needs to handle itself.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LowPower_on(LowPower * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case LOWPOWER_READY_SIG: {
            /* An answer outside a round: late, or doubled. */
            KLOG_D(TAG, "answer dropped: no round is running");
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_ROUND_TMO_SIG: {
            /* An expiry of a round that is over. */
            (void)tmo_fresh(&me->round_tmo);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_IDLE_TMO_SIG: {
            /* An expiry of an idle time that was started over, or of a board
             * that is no longer AWAKE. */
            (void)tmo_fresh(&me->idle_tmo);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_RADIO_TMO_SIG: {
            /* An expiry of a radio wake that is over. */
            (void)tmo_fresh(&me->radio_tmo);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_RETRY_TMO_SIG: {
            /* An expiry of a wait that was ended, or of a board that is no
             * longer AWAKE. */
            (void)tmo_fresh(&me->retry_tmo);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_RADIO_DONE_SIG: {
            /* A radio owner's word, late or with no radio wake to belong to. */
            KLOG_D(TAG, "radio done dropped: no radio wake is running");
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_RETRY_SIG: {
            (void)state_retry_fire(me);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_MINUTE_SIG: {
            /* A one-shot of a sleep that is over. */
            minute_heard(me);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_WAKE_AT_SIG: {
            /* Awake or resting: kept for the sleep that may follow, where
             * asleep_enter() folds it in. */
            wake_store(me, Q_EVT_CAST(LowPowerWakeAtEvt));
            status = Q_HANDLED();
            break;
        }
        case POWER_EVT_SIG: {
            me->vbus = Q_EVT_CAST(PowerEvt)->vbus;
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            (void)config_answer(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_REST_SIG: {
            /* Asleep, going OFF, or in a round (the board's state is still AWAKE
             * for that): there is nothing to rest from, and whoever asked is
             * told so. */
            const LowPowerReqEvt *r = Q_EVT_CAST(LowPowerReqEvt);

            if ( (uint8_t)KODE_LP_AWAKE == me->state )
            {
                lp_request_refuse(r, WHY_RUNNING);
            }
            else
            {
                lp_request_refuse(r, "board not AWAKE");
            }
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/**
 * @brief AWAKE: everything runs and the lock is held.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LowPower_awake(LowPower * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            idle_restart(me);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            tmo_stop(&me->idle_tmo);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_IDLE_TMO_SIG: {
            if ( tmo_fresh(&me->idle_tmo) )
            {
                status = awake_idle(me);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_RETRY_TMO_SIG: {
            if ( tmo_fresh(&me->retry_tmo) )
            {
                status = awake_retry(me);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case IOEXP_INPUT_SIG: {
            if ( bot_pressed(Q_EVT_CAST(IoexpInputEvt)) )
            {
                me->used = true;
                /* A person at the board wins over the request that waits. */
                if ( me->retry_on )
                {
                    KLOG_N(TAG, "BOT pressed: the request for %s is not asked again",
                           state_name(me->target));
                    retry_stop(me, true);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case POWER_EVT_SIG: {
            /* A cable that appears is somebody at the board. */
            if ( Q_EVT_CAST(PowerEvt)->vbus && (!me->vbus) )
            {
                me->used = true;
            }
            me->vbus = Q_EVT_CAST(PowerEvt)->vbus;
            if ( me->retry_on && me->vbus )
            {
                refusal_set(WHO_CABLE, WHY_CABLE);
                KLOG_N(TAG, "%s: the request for %s is not asked again",
                       WHY_CABLE, state_name(me->target));
                retry_stop(me, true);
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG:
        case TOUCH_EVT_SIG: {
            /* The release of a contact that stopped counting is nobody at
             * the board: the idle time it would start again is what lets a
             * cloth on the glass sleep at last. */
            if ( ((QSignal)BUTTON_EVT_SIG == e->sig) || !Q_EVT_CAST(TouchEvt)->stale )
            {
                awake_input(me);
            }
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_INPUT_SIG: {
            /* The scheduler's own line is not somebody at the board. */
            if ( !input_is_sched(Q_EVT_CAST(LowPowerInputEvt)->src) )
            {
                awake_input(me);
            }
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            if ( config_answer(me, Q_EVT_CAST(KodeConfigReqEvt)) )
            {
                idle_restart(me);
            }
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_REQ_SIG: {
            status = awake_request(me, Q_EVT_CAST(LowPowerReqEvt));
            break;
        }
        case LOWPOWER_REST_SIG: {
            status = awake_rest(me, Q_EVT_CAST(LowPowerReqEvt));
            break;
        }
        default: {
            status = Q_SUPER(&LowPower_on);
            break;
        }
    }
    return status;
}

/**
 * @brief A round is running: the board is still AWAKE until all say ready.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LowPower_resting(LowPower * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case LOWPOWER_READY_SIG: {
            status = resting_ready(me, Q_EVT_CAST(LowPowerReadyEvt));
            break;
        }
        case LOWPOWER_ROUND_TMO_SIG: {
            if ( tmo_fresh(&me->round_tmo) )
            {
                status = resting_timeout(me);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_RETRY_TMO_SIG: {
            if ( tmo_fresh(&me->retry_tmo) && round_order(me) )
            {
                order_ask_again(me);
            }
            status = Q_HANDLED();
            break;
        }
        case IOEXP_INPUT_SIG: {
            IoexpInputEvt const * const in = Q_EVT_CAST(IoexpInputEvt);

            /* A person at the board wins over the sleep, but not over an
             * order: the person asked for it. */
            if ( bot_pressed(in) && round_order(me) )
            {
                me->used = true;
                KLOG_D(TAG, "BOT during an order for OFF: it stands");
                status = Q_HANDLED();
            }
            else if ( bot_pressed(in) )
            {
                me->used = true;
                refusal_set("BOT", "pressed");
                status = round_abort(me, (uint8_t)KODE_LP_CAUSE_BOT);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case BUTTON_EVT_SIG:
        case TOUCH_EVT_SIG:
        case LOWPOWER_INPUT_SIG: {
            /* The scheduler's own line is not somebody at the board, and nor
             * is the release of a contact that stopped counting. */
            if ( (((QSignal)LOWPOWER_INPUT_SIG == e->sig)
                  && input_is_sched(Q_EVT_CAST(LowPowerInputEvt)->src))
                || (((QSignal)TOUCH_EVT_SIG == e->sig) && Q_EVT_CAST(TouchEvt)->stale) )
            {
                status = Q_HANDLED();
            }
            else
            {
                me->used = true;
                if ( input_abandons(me) )
                {
                    refusal_set(WHO_INPUT, WHY_INPUT);
                    status = round_abort(me, me->cause);
                }
                else
                {
                    status = Q_HANDLED();
                }
            }
            break;
        }
        case POWER_EVT_SIG: {
            if ( Q_EVT_CAST(PowerEvt)->vbus && (!me->vbus) )
            {
                me->used = true;
            }
            me->vbus = Q_EVT_CAST(PowerEvt)->vbus;
            if ( me->vbus )
            {
                refusal_set(WHO_CABLE, WHY_CABLE);
                status = round_abort(me, (uint8_t)KODE_LP_CAUSE_VBUS);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_REQ_SIG: {
            const LowPowerReqEvt *r = Q_EVT_CAST(LowPowerReqEvt);

            if ( ((uint8_t)KODE_LP_AWAKE == r->target) && (!round_order(me)) )
            {
                refusal_set(WHO_REQUEST, "cancelled");
                status = round_abort(me, (uint8_t)KODE_LP_CAUSE_REQUEST);
            }
            else
            {
                lp_request_refuse(r, WHY_RUNNING);
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&LowPower_on);
            break;
        }
    }
    return status;
}

/**
 * @brief ASLEEP: the tick is stopped and the lock is let go.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LowPower_asleep(LowPower * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            asleep_enter(me);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_MINUTE_SIG: {
            status = asleep_minute(me);
            break;
        }
        case LOWPOWER_WAKE_AT_SIG: {
            wake_store(me, Q_EVT_CAST(LowPowerWakeAtEvt));
            wake_fold(me);
            status = Q_HANDLED();
            break;
        }
        case IOEXP_INPUT_SIG: {
            if ( bot_pressed(Q_EVT_CAST(IoexpInputEvt)) )
            {
                wake_up(me, (uint8_t)KODE_LP_CAUSE_BOT);
                status = Q_TRAN(&LowPower_awake);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_TOP_SIG: {
            /* The button that wakes the Dot. */
            wake_up(me, (uint8_t)KODE_LP_CAUSE_TOP);
            status = Q_TRAN(&LowPower_awake);
            break;
        }
        case MQTT_MSG_SIG: {
            wake_up(me, (uint8_t)KODE_LP_CAUSE_MQTT);
            status = Q_TRAN(&LowPower_awake);
            break;
        }
        case LOWPOWER_INPUT_SIG: {
            uint8_t const src = Q_EVT_CAST(LowPowerInputEvt)->src;

            if ( input_is_sched(src) )
            {
                /* The scheduler's own line: not a wake. */
                status = Q_HANDLED();
            }
            else
            {
                wake_up(me, input_cause(src));
                status = Q_TRAN(&LowPower_awake);
            }
            break;
        }
        case POWER_EVT_SIG: {
            me->vbus = Q_EVT_CAST(PowerEvt)->vbus;
            if ( me->vbus )
            {
                wake_up(me, (uint8_t)KODE_LP_CAUSE_VBUS);
                status = Q_TRAN(&LowPower_awake);
            }
            else
            {
                if ( me->sample_due )
                {
                    summary_sample(me, Q_EVT_CAST(PowerEvt));
                }
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_REQ_SIG: {
            status = asleep_request(me, Q_EVT_CAST(LowPowerReqEvt));
            break;
        }
        default: {
            status = Q_SUPER(&LowPower_on);
            break;
        }
    }
    return status;
}

/**
 * @brief A radio wake: ASLEEP with the tick and the lock, for the radio owners.
 *
 * A substate of ASLEEP, so the minute, the inputs that wake the board whole
 * and the requests are ASLEEP's own and are handled there.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LowPower_radio(LowPower * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            radio_enter(me);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            radio_exit(me);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_RADIO_DONE_SIG: {
            status = radio_done(me, Q_EVT_CAST(LowPowerRadioDoneEvt));
            break;
        }
        case LOWPOWER_STATE_RETRY_SIG: {
            status = radio_retry(me);
            break;
        }
        case LOWPOWER_RADIO_TMO_SIG: {
            if ( tmo_fresh(&me->radio_tmo) )
            {
                KLOG_N(TAG, "radio wake: %u ms are up; the radio owners are asked "
                            "to rest", (unsigned)KODE_LOWPOWER_RADIO_MAX_MS);
                status = radio_rest(me);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_READY_SIG: {
            status = radio_ready(me, Q_EVT_CAST(LowPowerReadyEvt));
            break;
        }
        case LOWPOWER_ROUND_TMO_SIG: {
            if ( tmo_fresh(&me->round_tmo) )
            {
                status = radio_round_timeout(me);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&LowPower_asleep);
            break;
        }
    }
    return status;
}

/**
 * @brief OFF, pending: the grace for the log, then deep sleep. Terminal.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LowPower_off(LowPower * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case LOWPOWER_ROUND_TMO_SIG: {
            if ( tmo_fresh(&me->round_tmo) )
            {
                khal_power_off_enter();
            }
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_REQ_SIG: {
            KLOG_I(TAG, "OFF is pending; the request is ignored");
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&LowPower_on);
            break;
        }
    }
    return status;
}

/**
 * @brief Take the power-management lock and start AWAKE.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to AWAKE.
 */
static QState LowPower_initial(LowPower * const me, void const * const par)
{
    (void)par;

    /* In this thread, before anything holds or lets go of the lock: the lock
     * starts held, which is AWAKE. */
    me->power_ok = (khal_power_init() == KODE_OK);
    if ( !me->power_ok )
    {
        KLOG_E(TAG, "power management did not start; the board stays AWAKE "
                    "and refuses to sleep");
    }
    me->state    = (uint8_t)KODE_LP_AWAKE;
    me->since_us = khal_time_us();

    for ( uint8_t i = 0U; (uint8_t)SETTING_COUNT > i; i++ )
    {
        bool stored = false;

        me->secs[i] = setting_load(&SETTINGS[i], &stored);
    }
#ifndef CONFIG_KODE_LOWPOWER_IDLE
    /* The app's interface decides when to rest: no idle count here, and the
     * stored value stays for the firmwares that do count. */
    me->secs[SETTING_SLEEP_AFTER] = KSET_S_NEVER;
#endif

    QActive_subscribe(&me->super, IOEXP_INPUT_SIG);
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    QActive_subscribe(&me->super, TOUCH_EVT_SIG);
    QActive_subscribe(&me->super, POWER_EVT_SIG);
    QActive_subscribe(&me->super, MQTT_MSG_SIG);
    KLOG_I(TAG, "up; AWAKE");
    return Q_TRAN(&LowPower_awake);
}

/* ---- Construction ------------------------------------------------------------------------ */

void LowPower_ctor(void)
{
    LowPower * const me = &l_lowpower;

    QActive_ctor(&me->super, Q_STATE_CAST(&LowPower_initial));
    QTimeEvt_ctorX(&me->round_tmo.evt, &me->super, LOWPOWER_ROUND_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->idle_tmo.evt, &me->super, LOWPOWER_IDLE_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->radio_tmo.evt, &me->super, LOWPOWER_RADIO_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->retry_tmo.evt, &me->super, LOWPOWER_RETRY_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->state_retry, &me->super, LOWPOWER_STATE_RETRY_SIG, 0U);
    me->state_owed = false;
    me->owed_state = (uint8_t)KODE_LP_AWAKE;
    me->owed_cause = (uint8_t)KODE_LP_CAUSE_BOOT;
    me->owed_prev  = (uint8_t)KODE_LP_AWAKE;

    me->round_tmo.stale = 0U;
    me->round_tmo.live  = false;
    me->idle_tmo.stale  = 0U;
    me->idle_tmo.live   = false;
    me->radio_tmo.stale = 0U;
    me->radio_tmo.live  = false;
    me->retry_tmo.stale = 0U;
    me->retry_tmo.live  = false;
    me->retry_on        = false;
    me->used            = true;
    me->rest_on         = false;
    me->request_us      = 0U;
    me->radio_mask      = 0U;
    me->radio_pending   = 0U;
    me->radio           = false;
    me->radio_resting   = false;
    me->radio_closing   = false;
    (void)memset(me->parts, 0, sizeof(me->parts));
    me->joined   = 0U;
    me->sealed   = false;
    me->round    = ROUND_NONE;
    me->waiting  = 0U;
    me->state    = (uint8_t)KODE_LP_AWAKE;
    me->target   = (uint8_t)KODE_LP_AWAKE;
    me->cause    = (uint8_t)KODE_LP_CAUSE_BOOT;
    me->round_src = (uint8_t)KODE_LOG_SRC_NONE;
    me->power_ok = false;
    me->vbus     = false;
    for ( uint8_t i = 0U; (uint8_t)SETTING_COUNT > i; i++ )
    {
        me->secs[i] = SETTINGS[i].def_s;
    }
    me->asleep_since_us = 0U;
    me->off_since_us    = 0U;
    me->minute_due_us   = 0U;
    me->boundary_us     = 0U;
    me->wake_due_us     = 0U;
    me->wake_who        = KODE_LOWPOWER_WHO_NONE;
    me->app_due_us      = 0U;
    me->app_who         = KODE_LOWPOWER_WHO_NONE;
    me->minute_out      = 0U;
    me->last            = NULL;
    me->sample_due      = false;
    me->save_due        = false;
    (void)memset(&me->stats0, 0, sizeof(me->stats0));
    (void)memset(&me->radio0, 0, sizeof(me->radio0));
    me->since_us        = 0U;
    me->last_cause      = (uint8_t)KODE_LP_CAUSE_BOOT;
    (void)memset(me->wakes, 0, sizeof(me->wakes));
    s_refusal[0] = '\0';
}

/* A queue for a whole round's answers arriving together, and a small stack:
 * a step here is a publish, a count and a log line. */
AO_STORAGE(lowpower, KODE_LOWPOWER_QUEUE_LEN, KQP_AO_STACK_SMALL);

void kaos_lowpower_register(void)
{
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&lowpower_verbs), "lowpower verbs");
#endif
}

void kaos_lowpower_start(void)
{
    LowPower_ctor();
    kaos_lowpower_register();
    AO_START(AO_LowPower, lowpower, KODE_PRIO_LOWPOWER);
#if CONFIG_KODEOS_CONSOLE
    /* The settings' words, in the order of the seconds this AO keeps. */
#ifdef CONFIG_KODE_LOWPOWER_IDLE
    static const char *const SLEEP_AFTER[] = KSET_SLEEP_AFTER_WORDS;
#endif
    static const char *const OFF_AFTER[]   = KSET_OFF_AFTER_WORDS;
    /* The settings this AO answers, registered once it has the queue their
     * requests are posted to. */
    static const kode_config_t LOWPOWER_CONFIG[] = {
#ifdef CONFIG_KODE_LOWPOWER_IDLE
        { .name = KSET_KEY_SLEEP_AFTER, .type = KODE_TYPE_ENUM,
          .enums = SLEEP_AFTER,
          .n_enums = (uint8_t)(sizeof(SLEEP_AFTER) / sizeof(SLEEP_AFTER[0])) },
#endif
        { .name = KSET_KEY_OFF_AFTER, .type = KODE_TYPE_ENUM,
          .enums = OFF_AFTER,
          .n_enums = (uint8_t)(sizeof(OFF_AFTER) / sizeof(OFF_AFTER[0])) },
    };

    kaos_require(KODE_CONFIG_ADD(AO_LowPower, KQP_LOWPOWER_CMD_MARGIN, LOWPOWER_CONFIG),
                 "lowpower settings");
#endif
}

/* ---- The facade ------------------------------------------------------------------------------ */

QEvt const *lowpower_req_evt(kode_lp_state_t target, uint8_t src)
{
    LowPowerReqEvt *e = Q_NEW_X(LowPowerReqEvt, 0U, LOWPOWER_REQ_SIG);
    QEvt const     *out = NULL;

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the request for %s was dropped",
               state_name((uint8_t)target));
    }
    else
    {
        e->target = (uint8_t)target;
        e->src    = src;
        out = &e->super;
    }
    return out;
}

void lowpower_request(kode_lp_state_t target, uint8_t src)
{
    QEvt const * const e = lowpower_req_evt(target, src);

    if ( NULL != e )
    {
        QACTIVE_POST(AO_LowPower, e, NULL);
    }
}

bool lowpower_request_rest(uint8_t src)
{
    LowPowerReqEvt *e = Q_NEW_X(LowPowerReqEvt, 0U, LOWPOWER_REST_SIG);
    bool posted = false;

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the request to rest was dropped");
    }
    else
    {
        e->target = (uint8_t)KODE_LP_ASLEEP;
        e->src    = src;
        QACTIVE_POST(AO_LowPower, &e->super, NULL);
        posted = true;
    }
    return posted;
}

void lowpower_input_note(uint8_t src)
{
    /* One static event per port, so a wake from BLE or the channel is never
     * lost to a dry pool. */
    static const LowPowerInputEvt notes[] = {
        { QEVT_INITIALIZER(LOWPOWER_INPUT_SIG), (uint8_t)KODE_CONSOLE_SRC_SERIAL },
        { QEVT_INITIALIZER(LOWPOWER_INPUT_SIG), (uint8_t)KODE_CONSOLE_SRC_BLE },
        { QEVT_INITIALIZER(LOWPOWER_INPUT_SIG), (uint8_t)KODE_CONSOLE_SRC_NET },
        { QEVT_INITIALIZER(LOWPOWER_INPUT_SIG), (uint8_t)KODE_CONSOLE_SRC_SCHED },
    };
    _Static_assert((size_t)KODE_CONSOLE_SRC_SCHED == (Q_DIM(notes) - 1U),
                   "the notes are indexed by port: SCHED is the last");
    uint8_t at = src;

    if ( Q_DIM(notes) <= at )
    {
        at = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    }
    QACTIVE_POST(AO_LowPower, &notes[at].super, NULL);
}

const char *lowpower_last_refusal(void)
{
    return s_refusal;
}

void lowpower_info_get(lowpower_info_t *out)
{
    const LowPower * const me = &l_lowpower;

    if ( NULL == out )
    {
        return;
    }
    out->state         = me->state;
    out->cause         = me->last_cause;
    out->for_s         = (uint32_t)((khal_time_us() - me->since_us) / US_PER_S);
    out->sleep_after_s = me->secs[SETTING_SLEEP_AFTER];
    out->off_after_s   = me->secs[SETTING_OFF_AFTER];
    out->wakes_bot     = me->wakes[KODE_LP_CAUSE_BOT];
    out->wakes_ble     = me->wakes[KODE_LP_CAUSE_BLE];
    out->wakes_mqtt    = me->wakes[KODE_LP_CAUSE_MQTT];
    out->wakes_timer   = me->wakes[KODE_LP_CAUSE_TIMER];
}

/* Called on kode_init()'s thread before any round, and refused after the first: the table
 * needs no lock. */
uint8_t lowpower_join(QActive *ao, const char *name)
{
    LowPower * const me = &l_lowpower;
    uint8_t id = KODE_LOWPOWER_WHO_NONE;

    if ( (NULL == ao) || (NULL == name) )
    {
        KLOG_W(TAG, "join refused: an owner without an AO or a name");
    }
    else if ( me->sealed )
    {
        KLOG_W(TAG, "%s joined after the first round and is not asked", name);
    }
    else if ( KODE_LOWPOWER_PARTICIPANTS <= me->joined )
    {
        KLOG_W(TAG, "%s cannot join: all %u places are taken", name,
               (unsigned)KODE_LOWPOWER_PARTICIPANTS);
    }
    else
    {
        id = me->joined;
        me->parts[id].ao   = ao;
        me->parts[id].name = name;
        me->joined += 1U;
        KLOG_D(TAG, "%s joined as %u", name, (unsigned)id);
    }
    return id;
}

uint8_t lowpower_join_radio(QActive *ao, const char *name)
{
    LowPower * const me = &l_lowpower;
    uint8_t const id = lowpower_join(ao, name);

    if ( KODE_LOWPOWER_WHO_NONE != id )
    {
        me->radio_mask |= (1U << id);
    }
    return id;
}

void lowpower_radio_done(uint8_t who)
{
    LowPowerRadioDoneEvt *e = Q_NEW_X(LowPowerRadioDoneEvt, 0U, LOWPOWER_RADIO_DONE_SIG);

    if ( NULL == e )
    {
        /* The bound of the radio wake brings the board back. */
        KLOG_E(TAG, "out of events; participant %u's radio done was dropped",
               (unsigned)who);
        return;
    }
    e->who = who;
    QACTIVE_POST(AO_LowPower, &e->super, NULL);
}

bool lowpower_wake_at(uint8_t who, uint64_t due_us)
{
    LowPowerWakeAtEvt *e = Q_NEW_X(LowPowerWakeAtEvt, 0U, LOWPOWER_WAKE_AT_SIG);
    bool sent = false;

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; participant %u's wake time was dropped",
               (unsigned)who);
    }
    else
    {
        e->who    = who;
        e->due_us = due_us;
        QACTIVE_POST(AO_LowPower, &e->super, NULL);
        sent = true;
    }
    return sent;
}

void lowpower_ready(uint8_t who, uint32_t round, bool busy, const char *why)
{
    LowPowerReadyEvt *e = Q_NEW_X(LowPowerReadyEvt, 0U, LOWPOWER_READY_SIG);

    if ( NULL == e )
    {
        /* The round will time out and name this participant. */
        KLOG_E(TAG, "out of events; participant %u's answer was dropped",
               (unsigned)who);
        return;
    }
    e->who   = who;
    e->round = round;
    e->busy  = busy;
    why_copy(e->why, sizeof(e->why), why);
    QACTIVE_POST(AO_LowPower, &e->super, NULL);
}
