/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_ao.c
 * @brief AO_LowPower: the one place that decides the board's state.
 */
#include "kaos.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kbsp.h"
#include "kdrv_radio.h"
#include "khal_console.h"
#include "khal_exp.h"
#include "khal_nvs.h"
#include "khal_power.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"
#include "kset.h"
#include "lowpower_last.h"
#include "lowpower_part.h"
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

/** @brief The expander's button bits, in an IoexpInputEvt: BOT and the pad. */
#define PART_BUTTONS_MASK  ((1U << KBSP_IOEXP_BIT_BTN_BOT) | (1U << KBSP_IOEXP_BIT_PAD_UP) \
                            | (1U << KBSP_IOEXP_BIT_PAD_LEFT) | (1U << KBSP_IOEXP_BIT_PAD_DOWN) \
                            | (1U << KBSP_IOEXP_BIT_PAD_RIGHT))

/** @brief The expander's touch interrupt bit, in an IoexpInputEvt. Active low. */
#define PART_TOUCH_MASK    (1U << KBSP_IOEXP_BIT_TP_INT)

/** @brief Where the count of answers starts in the word a task reads of a rest. */
#define PART_WORD_ANSWERS_SHIFT  16U

/** @brief Where the last answer's status starts in that word; the parts are below it. */
#define PART_WORD_RC_SHIFT       8U

/** @brief The bit of that word that says a request is being arranged; the parts take the seven below. */
#define PART_WORD_OPEN           0x80U

_Static_assert((KODE_LP_PART_ALL & PART_WORD_OPEN) == 0U, "the parts leave the word its open bit");

/** @brief The owners the CPU part asks, in order: the wake lines, then the scheduler's next minute. */
#define PART_CPU_OWNERS          { "ioexp", "scheduler" }

/** @brief The KODE_LP_PART_* bits there are: a late answer can be owed for each of them at once. */
#define PART_COUNT               7U

_Static_assert(((1U << PART_COUNT) - 1U) == KODE_LP_PART_ALL, "one late answer for every part");

/** @brief The bits of one byte of that word. */
#define PART_WORD_BYTE_MASK      0xFFU

/** @brief Milliseconds in a second, for the idle time. */
#define MS_PER_S        1000U

/** @brief Microseconds in a second. */
#define US_PER_S        1000000ULL

/** @brief Microseconds in a millisecond. */
#define US_PER_MS       1000ULL

/** @brief How long a request is begun again, in microseconds. */
#define RETRY_WINDOW_US ((uint64_t)KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS * US_PER_MS)

/** @brief The wait before a request is begun again, in microseconds. */
#define RETRY_US        ((uint64_t)KODE_LOWPOWER_REQUEST_RETRY_MS * US_PER_MS)

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
    Tmo         round_tmo;        /**< LOWPOWER_ROUND_TMO_SIG: the round's budget, or OFF's grace; in
                                   *   an app's rest the step's budget, or the wait before its busy
                                   *   owner is asked again. */
    Tmo         idle_tmo;         /**< LOWPOWER_IDLE_TMO_SIG: the idle time, AWAKE only. */
    Tmo         dark_tmo;         /**< LOWPOWER_DARK_TMO_SIG: the bound of a dark wake. */
    Tmo         retry_tmo;        /**< LOWPOWER_RETRY_TMO_SIG: the wait before a requested round begins again. */
    Participant parts[KODE_LOWPOWER_PARTICIPANTS]; /**< The table lowpower_join() fills. */
    uint8_t     joined;           /**< Entries of parts in use.              */
    bool        sealed;           /**< A round has begun: joins are refused. */
    uint32_t    round;            /**< The number of the last round begun.   */
    uint32_t    waiting;          /**< Bit n: participant n has not answered.*/
    uint32_t    busy;             /**< Bit n: participant n answered busy to an order for OFF and is asked
                                   *   again when retry_tmo runs out (round_order()). */
    uint32_t    held;             /**< Bit n: participant n owns what an app has at rest, and the
                                   *   round asks it last (round_begin()). */
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
    uint64_t    wake_due_us;      /**< When a dark-wake owner's work is due; 0 for none. */
    uint8_t     wake_who;         /**< The dark-wake owner whose time that is.   */
    uint64_t    app_due_us;       /**< When any other owner's work is due; 0 for none. A
                                   *   dark wake does not touch it.          */
    uint8_t     app_who;          /**< The owner whose time that is.         */
    uint32_t    dark_mask;        /**< Bit n: participant n takes part in dark wakes. */
    uint32_t    dark_pending;     /**< Bit n: dark-wake owner n has not said it is done. */
    bool        dark;             /**< A dark wake is on: the tick runs and the lock is held inside ASLEEP. */
    bool        dark_resting;     /**< The dark wake's round for ASLEEP is open. */
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
    bool        dark_closing;     /**< The dark wake's STATE(ASLEEP) is owed: the tick and the lock stay until it goes out. */
    bool        used;             /**< Something other than an owner's time used the board since it last woke:
                                   *   every wake but a timer's, and each real input after. */
    bool        rest_on;          /**< The request in hand, round or retry, is a rest (lowpower_request_rest()). */
    uint8_t     part_rest;        /**< The KODE_LP_PART_* an app's rest has put away. */
    uint8_t     part_want;        /**< The parts the app's last request for a rest asked for: the
                                   *   sequence in hand puts them away, and they stay wanted until
                                   *   the next request or the board takes the rest. */
    uint8_t     part_step;        /**< The part whose owner is being asked; 0 with no sequence open. */
    uint8_t     part_who;         /**< The participant asked in that step.     */
    uint8_t     part_src;         /**< Who asked for the sequence in hand.     */
    bool        part_up;          /**< The sequence in hand brings the parts back. */
    bool        part_again;       /**< The owner of part_step answered busy and is asked again when
                                   *   round_tmo runs out (part_busy()). */
    uint64_t    part_us;          /**< khal_time_us() when the sequence in hand was asked for: the
                                   *   request's window counts from it. */
    bool        part_wake_touch;  /**< A touch ends the CPU part.              */
    bool        part_wake_level;  /**< The level of the header's line that ends it. */
    bool        part_pin_armed;   /**< That line is armed as a wake line.      */
    uint8_t     part_wake_pin;    /**< The header's position; 0 for none.      */
    uint8_t     part_timers;      /**< One-shots of a wake time out and not yet heard. */
    uint8_t     part_rc;          /**< The last answer's kode_status_t.        */
    uint16_t    part_answers;     /**< Requests for parts answered; it wraps.  */
    uint32_t    part_gpio;        /**< The P4 pin behind that position.        */
    uint32_t    part_after_ms;    /**< The time that ends the CPU part; 0 for none. */
    uint64_t    part_due_us;      /**< khal_time_us() when that time comes; 0 for none. */
    uint64_t    part_hop_us;      /**< khal_time_us() when the last one-shot armed for it comes due. */
    uint64_t    part_since_us;    /**< khal_time_us() when the CPU part last went to rest: off_after
                                   *   counts from it.                          */
    uint32_t    part_late_round[PART_COUNT]; /**< By the part's bit (part_slot()): the round of its
                                   *   step that ran out of time. */
    uint8_t     part_done;        /**< The parts the sequence in hand has put away, the ones that do
                                   *   not stay at rest among them.             */
    uint8_t     part_cpu_stage;   /**< Owners of the CPU part asked so far.     */
    uint8_t     part_late;        /**< The parts whose owners ran out of time: an answer that comes
                                   *   late still counts, whatever was asked since. A part's bit is
                                   *   cleared once its answer comes; all of them when the board
                                   *   takes the rest. */
    uint8_t     part_late_who[PART_COUNT]; /**< By the part's bit: the participant that owes its answer. */
    bool        part_pin_free;    /**< The header's position was free, not an input, before it was armed. */
    khal_gpio_pull_t part_pin_pulled; /**< The pull that input had: it gets it back. */
    bool        cable_check;      /**< The board woke from ASLEEP or the CPU part's rest, and AWAKE has had
                                   *   no reading since: a cable in that reading came while the cable port
                                   *   was off the bus. */
} LowPower;

static LowPower l_lowpower;
QActive * const AO_LowPower = &l_lowpower.super;

/** @brief The last refusal or busy reason; written by this AO, read by the console. */
static char s_refusal[KODE_LOWPOWER_REFUSAL_MAX];

/** @brief Why the app's last request for parts was refused; written by this AO, read by the
 *         task that asked. No other source's answer writes it. */
static char s_part_why[KODE_LOWPOWER_REFUSAL_MAX];

/** @brief Why the last request for parts was refused, whoever asked; for `lowpower_info`. */
static char s_part_said[KODE_LOWPOWER_REFUSAL_MAX];

/** @brief What a task reads of an app's rest: the facade's answers, the last status, whether a
 *         request is being arranged and the parts held at rest. */
static _Atomic uint32_t s_part_word;

static QState LowPower_initial(LowPower * const me, void const * const par);
static QState LowPower_on(LowPower * const me, QEvt const * const e);
static QState LowPower_awake(LowPower * const me, QEvt const * const e);
static QState LowPower_resting(LowPower * const me, QEvt const * const e);
static QState LowPower_asleep(LowPower * const me, QEvt const * const e);
static QState LowPower_dark(LowPower * const me, QEvt const * const e);
static QState LowPower_off(LowPower * const me, QEvt const * const e);
static void part_take(LowPower * const me);
static uint32_t part_round_keep(LowPower * const me);
static void part_awake_post(LowPower * const me, uint8_t who);

/* ---- Names ---------------------------------------------------------------- */

/** @brief A state's name for the log. */
static const char *state_name(uint8_t state)
{
    static const char * const names[] = { "AWAKE", "ASLEEP", "OFF", "DARK" };
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
 * AWAKE is (owners restore on it), DARK is (the dark-wake owners start on it), and so
 * is the ASLEEP that ends a dark wake (they leave DARK on it). The
 * ASLEEP and OFF of an ordinary round are not: the owners rested on the round.
 *
 * @param state The state reached.
 * @param prev  The state left.
 * @return true when the change is owed until it is published.
 */
static bool state_is_owed(uint8_t state, uint8_t prev)
{
    bool owed = false;

    if ( ((uint8_t)KODE_LP_AWAKE == state) || ((uint8_t)KODE_LP_DARK == state) )
    {
        owed = true;
    }
    else if ( ((uint8_t)KODE_LP_ASLEEP == state) && ((uint8_t)KODE_LP_DARK == prev) )
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
 * the news. An owed AWAKE needs the board AWAKE; an owed DARK or the ASLEEP
 * that ends a dark wake needs the dark wake still on.
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
            still = me->dark;
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


/* ---- The look for a cable ------------------------------------------------------- */

/**
 * @brief Runs in the timer task: ask AO_Power for a reading.
 *
 * The reading is published, and one with a cable in it wakes the board
 * (POWER_EVT_SIG in LowPower_asleep) or ends the CPU part's rest
 * (part_wake_cause()).
 *
 * @param arg Unused.
 */
static void cable_look_due_cb(void *arg)
{
    (void)arg;
    power_peek_request();
}

/**
 * @brief Begin or end the look for a cable while the P4 sleeps.
 *
 * No line of the board wakes the P4 for a cable, and every light sleep takes
 * the cable port off the bus, in ASLEEP and in the CPU part's rest alike: a
 * host that plugs in then is not answered and gives the port up. While the
 * lock is let go, AO_Power is asked for a reading every
 * KODE_LOWPOWER_CABLE_LOOK_MS. The timer does not wake the P4 by itself: it
 * runs at the first wake after it is due, and the console reader wakes the P4
 * once a second while it sleeps (khal_console_rest()). It is a periodic timer
 * of its own, so the minute keeps every one-shot.
 *
 * @param on true when the lock is let go, false when it is taken again.
 */
static void cable_look(bool on)
{
    static bool s_cable_timer;   /* The periodic timer exists: khal_timer_periodic() took it. */

    if ( s_cable_timer )
    {
        if ( khal_timer_periodic_run(&cable_look_due_cb, on) != KODE_OK )
        {
            KLOG_W(TAG, "the look for a cable would not change");
        }
    }
    else if ( on )
    {
        if ( khal_timer_periodic(&cable_look_due_cb, NULL,
                                 (uint64_t)KODE_LOWPOWER_CABLE_LOOK_MS * US_PER_MS) == KODE_OK )
        {
            s_cable_timer = true;
        }
        else
        {
            KLOG_W(TAG, "the look for a cable would not start; the minute's reading finds a cable");
        }
    }
    else
    {
        /* Never started: nothing to stop. */
    }
}

/* ---- The way down ------------------------------------------------------------- */

/**
 * @brief The tick, then the look for a cable, then the lock: what ASLEEP and
 *        the CPU part's rest let go of.
 *
 * Every owner asked has answered ready, so none is inside an event while the
 * tick is stopped. light_up() takes back all of it.
 */
static void light_down(void)
{
    kqp_tick_rest(true);
    khal_console_rest(true);
    cable_look(true);
    khal_power_hold(false);
}

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
    /* The board has every part from here: an app's rest is over. */
    me->held = 0U;
    part_take(me);

    if ( (uint8_t)KODE_LP_ASLEEP == me->target )
    {
        me->state = (uint8_t)KODE_LP_ASLEEP;
        publish_state(me, me->state, me->cause, prev);
        KLOG_N(TAG, "entering ASLEEP (%s): the P4 light-sleeps when every "
                    "task is idle; BOT, BLE, MQTT or a cable wakes it",
               cause_name(me->cause));
        /* After the lines above are published, and the tick before the lock.
         * Every owner has answered ready: no AO is inside an event while the tick is stopped. */
        light_down();
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
 * A round given up before it asked the owners of what an app has at rest
 * (round_begin()) tells each owner it did ask, and nobody else: those devices
 * stay at rest, and the app's rest is as it was.
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
    if ( 0U == me->held )
    {
        publish_state(me, me->state, cause, prev);
    }
    else
    {
        uint32_t const spared = me->held;

        me->held = 0U;
        for ( uint8_t i = 0U; i < me->joined; i++ )
        {
            if ( (spared & (1U << i)) == 0U )
            {
                part_awake_post(me, i);
            }
        }
        KLOG_N(TAG, "the app's rest is as it was: its owners were not asked");
    }
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
 * @brief Whether a moment falls inside the window a request is asked again in.
 *
 * One window for both kinds of request: a requested round is begun again
 * inside it (round_busy(), awake_retry()), and so is the busy step of an
 * app's rest (part_busy()).
 *
 * @param since_us khal_time_us() when the request came in.
 * @param at_us    The moment judged.
 * @return true when @p at_us is less than KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS
 *         after @p since_us.
 */
static bool retry_window_open(uint64_t since_us, uint64_t at_us)
{
    return (at_us - since_us) < RETRY_WINDOW_US;
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
                       && retry_window_open(me->request_us, khal_time_us());

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
 * @brief Post the running round to some participants, each its own copy.
 *
 * One that cannot be posted to keeps its place among those waited for: the
 * round's budget ends the wait for it (resting_timeout()).
 *
 * @param me   The AO, a round running.
 * @param whom Bit n: participant n is asked.
 */
static void round_post(LowPower * const me, uint32_t whom)
{
    for ( uint8_t i = 0U; i < me->joined; i++ )
    {
        if ( (whom & (1U << i)) != 0U )
        {
            LowPowerRoundEvt *ev = Q_NEW_X(LowPowerRoundEvt, 0U, LOWPOWER_ROUND_SIG);

            if ( NULL == ev )
            {
                KLOG_W(TAG, "no event to ask %s", me->parts[i].name);
            }
            else
            {
                ev->target = me->target;
                ev->parts  = 0U;
                ev->round  = me->round;
                if ( !QACTIVE_POST_X(me->parts[i].ao, &ev->super, 1U, &me->super) )
                {
                    KLOG_W(TAG, "%s's queue is full; not asked", me->parts[i].name);
                }
            }
        }
    }
}

/**
 * @brief Every owner asked so far is ready: ask the ones kept for last, if any.
 *
 * The round is no longer one that may leave the app's rest as it was: the
 * board takes it, and the owners of those devices hear the round.
 *
 * @param me The AO, a round running and nobody waited for.
 * @return true when owners were asked, and the round waits for them.
 */
static bool round_held_ask(LowPower * const me)
{
    bool asked = false;

    if ( 0U != me->held )
    {
        me->waiting = me->held;
        me->held    = 0U;
        part_take(me);
        round_post(me, me->waiting);
        asked = true;
    }
    return asked;
}

/**
 * @brief Begin a round for @p target and ask every participant.
 *
 * With devices at rest for an app, a round that may be given up asks their
 * owners last, each with its own copy, and takes the app's rest only then: a
 * round an owner refuses before that leaves the rest as it was. An order, and
 * a round begun while a rest is being arranged, take it at once.
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
        bool over;

        me->target = target;
        me->cause  = cause;
        me->held   = 0U;
        if ( (0U == me->part_step) && (!round_order(me)) )
        {
            me->held = part_round_keep(me);
        }
        if ( 0U == me->held )
        {
            /* The board has every part from here: an app's rest is over. */
            part_take(me);
        }
        me->sealed = true;
        me->round += 1U;
        if ( ROUND_NONE == me->round )
        {
            me->round += 1U;
        }
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
        if ( 0U == me->held )
        {
            ev->target = target;
            ev->parts  = 0U;
            ev->round  = me->round;
            QACTIVE_PUBLISH(&ev->super, &me->super);
        }
        else
        {
            /* Nobody but the owners asked now may hear this round yet. */
            QF_gc(&ev->super);
            me->waiting &= ~me->held;
            round_post(me, me->waiting);
            KLOG_N(TAG, "round %u asks the owners of the app's rest last (0x%08x)",
                   (unsigned)me->round, (unsigned)me->held);
        }
        KLOG_I(TAG, "round %u to %s (%s): asking %u participants",
               (unsigned)me->round, state_name(target), cause_name(cause),
               (unsigned)me->joined);

        over = (0U == me->waiting);
        if ( over )
        {
            over = !round_held_ask(me);
        }
        if ( over )
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

/** @brief The tick, then the lock, and no more look for a cable: what the P4 needs before anything else runs. */
static void light_up(void)
{
    kqp_tick_rest(false);
    khal_console_rest(false);
    cable_look(false);
    khal_power_hold(true);
}

/**
 * @brief A dark wake is over, one way or another: forget what it counted.
 *
 * The tick and the lock are the caller's to settle. The round's one-shot is
 * stopped only when this wake's own round is the one it times.
 *
 * @param me The AO.
 */
static void dark_clear(LowPower * const me)
{
    tmo_stop(&me->dark_tmo);
    if ( me->dark_resting )
    {
        tmo_stop(&me->round_tmo);
    }
    me->dark          = false;
    me->dark_resting  = false;
    me->dark_closing  = false;
    me->dark_pending  = 0U;
    me->waiting       = 0U;
}

/**
 * @brief Have the tick and the lock back, from either phase of ASLEEP.
 *
 * A dark wake has them already.
 *
 * @param me The AO, ASLEEP.
 */
static void rest_up(LowPower * const me)
{
    if ( me->dark )
    {
        dark_clear(me);
    }
    else
    {
        light_up();
    }
}

/**
 * @brief Leave ASLEEP: the tick, then the lock, then the news.
 *
 * @param me    The AO, ASLEEP, or in a dark wake.
 * @param cause The kode_lp_cause_t that woke the board.
 */
static void wake_up(LowPower * const me, uint8_t cause)
{
    uint8_t const prev = me->state;
    /* A dark wake counted itself when it began. */
    bool const counted = me->dark && ((uint8_t)KODE_LP_CAUSE_TIMER == cause);

    rest_up(me);
    me->state = (uint8_t)KODE_LP_AWAKE;
    me->wake_due_us = 0U;
    me->app_due_us  = 0U;
    /* An owner's time is nobody at the board; every other wake is somebody. */
    me->used    = ((uint8_t)KODE_LP_CAUSE_TIMER != cause);
    me->rest_on = false;
    me->cable_check = true;
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
        if ( !retry_window_open(me->request_us, khal_time_us()) )
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
 * @brief Whether the time at rest has reached off_after.
 *
 * @param me       The AO.
 * @param since_us khal_time_us() the time counts from.
 * @param now      khal_time_us().
 * @return true when the board is to go OFF.
 */
static bool off_due(const LowPower *me, uint64_t since_us, uint64_t now)
{
    uint64_t const limit = (uint64_t)me->secs[SETTING_OFF_AFTER] * US_PER_S;

    return (0U != limit) && ((now - since_us) >= limit);
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
 * Another owner's time always counts. A dark-wake owner's counts only outside a
 * dark wake, which is awake for the dark-wake owners' work already.
 *
 * @param me  The AO.
 * @param now khal_time_us().
 * @return The wait, at least 1; 0 when no owner gave a time that counts.
 */
static uint64_t owner_wait_us(const LowPower *me, uint64_t now)
{
    uint64_t wait = time_wait_us(me->app_due_us, now);

    if ( !me->dark )
    {
        uint64_t const dark = time_wait_us(me->wake_due_us, now);

        if ( (0U != dark) && ((0U == wait) || (dark < wait)) )
        {
            wait = dark;
        }
    }
    return wait;
}

/**
 * @brief Arm the one-shot for the next thing a board at rest has to do.
 *
 * That is the next minute, the off-after deadline or an owner's work,
 * whichever is first.
 *
 * @param me       The AO, ASLEEP, or AWAKE with the CPU part of an app's rest away.
 * @param now      khal_time_us().
 * @param since_us khal_time_us() off_after counts from.
 */
static void minute_schedule(LowPower * const me, uint64_t now, uint64_t since_us)
{
    uint64_t wait = minute_wait_us();
    uint64_t const limit = (uint64_t)me->secs[SETTING_OFF_AFTER] * US_PER_S;
    uint64_t const owner = owner_wait_us(me, now);

    if ( 0U != limit )
    {
        /* At least 1: a count kept across a wake may have run out already. */
        uint64_t const left = time_wait_us(since_us + limit, now);

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
 * @brief Whether participant @p who takes part in dark wakes.
 *
 * @param me  The AO.
 * @param who The participant.
 * @return true for a dark-wake owner.
 */
static bool is_dark_owner(const LowPower *me, uint8_t who)
{
    return (WAITING_BITS > who) && ((me->dark_mask & ((uint32_t)1U << who)) != 0U);
}

/**
 * @brief LOWPOWER_WAKE_AT_SIG: keep the earliest time a dark-wake owner gave, and
 *        the earliest any other owner gave, apart.
 *
 * Apart because a dark wake asks the dark-wake owners alone to say their times
 * again: another owner's time must outlive it.
 *
 * @param me The AO.
 * @param w  The note.
 */
static void wake_store(LowPower * const me, const LowPowerWakeAtEvt *w)
{
    if ( 0U != w->due_us )
    {
        if ( is_dark_owner(me, w->who) )
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

/* ---- The dark wake ------------------------------------------------------------------- */

/**
 * @brief An owner's time came, and it is a dark-wake owner's: wake with the glass dark.
 *
 * The one-shot that came was this wake's, so the minute is armed again; the
 * dark-wake owners' time is forgotten, and they say theirs again in the round
 * that ends the wake. Another owner's time is kept: that round does not ask
 * it.
 *
 * @param me  The AO, ASLEEP.
 * @param now khal_time_us().
 * @return The transition into the dark wake.
 */
static QState dark_begin(LowPower * const me, uint64_t now)
{
    KLOG_N(TAG, "%s has work due: a dark wake, the glass stays asleep",
           owner_name(me, me->wake_who));
    me->wake_due_us = 0U;
    minute_schedule(me, now, me->off_since_us);
    return Q_TRAN(&LowPower_dark);
}

/**
 * @brief Entering the dark wake: the tick, the lock, then the news.
 *
 * @param me The AO, ASLEEP.
 */
static void dark_enter(LowPower * const me)
{
    light_up();
    me->dark          = true;
    me->dark_resting  = false;
    me->dark_closing  = false;
    me->dark_pending  = me->dark_mask;
    me->waiting       = 0U;
    me->wakes[KODE_LP_CAUSE_TIMER] += 1U;
    tmo_start(&me->dark_tmo, KODE_LOWPOWER_DARK_MAX_MS);
    (void)state_send(me, (uint8_t)KODE_LP_DARK, (uint8_t)KODE_LP_CAUSE_TIMER,
                     (uint8_t)KODE_LP_ASLEEP);
}

/**
 * @brief Leaving the dark wake by any transition: its one-shot ends with it.
 *
 * What the transition's own handler left to settle (the tick, the lock, a
 * round it began) is not touched here.
 *
 * @param me The AO.
 */
static void dark_exit(LowPower * const me)
{
    tmo_stop(&me->dark_tmo);
    me->dark         = false;
    me->dark_resting = false;
    me->dark_closing = false;
}

/**
 * @brief The dark wake cannot rest again: the board is whole AWAKE.
 *
 * The owners that rested for ASLEEP restore on the STATE(AWAKE); the dark-wake
 * owners restore again, which is idempotent. The caller has set the refusal.
 *
 * @param me The AO, in the dark wake.
 * @return The transition to AWAKE.
 */
static QState dark_abandon(LowPower * const me)
{
    KLOG_N(TAG, "dark wake cannot rest again (%s): AWAKE", s_refusal);
    wake_up(me, (uint8_t)KODE_LP_CAUSE_TIMER);
    refusal_publish((uint8_t)KODE_LOG_SRC_NONE);
    return Q_TRAN(&LowPower_awake);
}

/**
 * @brief Post the round for ASLEEP to the dark-wake owners, one event each.
 *
 * One event for each, because a dynamic event posted to two queues needs a
 * reference held across the posts, and publishing is what holds one.
 *
 * @param me The AO.
 * @return false when an event was missing; some owners may have been asked.
 */
static bool dark_round_post(LowPower * const me)
{
    bool ok = true;

    for ( uint8_t i = 0U; i < me->joined; i++ )
    {
        if ( (me->dark_mask & (1U << i)) != 0U )
        {
            LowPowerRoundEvt *ev = Q_NEW_X(LowPowerRoundEvt, 0U, LOWPOWER_ROUND_SIG);

            if ( NULL == ev )
            {
                ok = false;
            }
            else
            {
                ev->target = (uint8_t)KODE_LP_ASLEEP;
                ev->parts  = 0U;
                ev->round  = me->round;
                QACTIVE_POST(me->parts[i].ao, &ev->super, NULL);
            }
        }
    }
    return ok;
}

/**
 * @brief Ask the dark-wake owners to rest again: the end of the dark wake.
 *
 * @param me The AO, in the dark wake.
 * @return The QP status: handled while the round is out, or the way home to
 *         AWAKE when it could not be asked.
 */
static QState dark_rest(LowPower * const me)
{
    QState status = Q_HANDLED();

    if ( !me->dark_resting )
    {
        me->round += 1U;
        if ( ROUND_NONE == me->round )
        {
            me->round += 1U;
        }
        me->target        = (uint8_t)KODE_LP_ASLEEP;
        me->cause         = (uint8_t)KODE_LP_CAUSE_TIMER;
        me->round_src     = (uint8_t)KODE_LOG_SRC_NONE;
        me->wake_due_us   = 0U;   /* the dark-wake owners say their times again */
        me->waiting       = me->dark_mask;
        me->dark_resting  = true;
        tmo_stop(&me->dark_tmo);
        if ( dark_round_post(me) )
        {
            KLOG_I(TAG, "round %u to ASLEEP (dark wake): asking the dark-wake owners",
                   (unsigned)me->round);
            tmo_start(&me->round_tmo, KODE_LOWPOWER_ROUND_MS);
        }
        else
        {
            refusal_set(WHO_REQUEST, "out of events");
            status = dark_abandon(me);
        }
    }
    return status;
}

/**
 * @brief The dark wake lets go: the tick stops, then the lock goes.
 *
 * Runs once STATE(ASLEEP) is on the bus, never before.
 *
 * @param me The AO, in the dark wake, its ASLEEP published.
 * @return The transition back to ASLEEP, without leaving it.
 */
static QState dark_release(LowPower * const me)
{
    dark_clear(me);
    KLOG_N(TAG, "dark wake done: ASLEEP again, the glass was not lit");
    wake_fold(me);   /* a time the owners gave in the round */
    light_down();
    return Q_TRAN(&LowPower_asleep);
}

/**
 * @brief The dark wake is over: ASLEEP again, nothing else woken.
 *
 * The state is published before the tick stops and the lock goes (rule 6).
 * When no event can carry it, the tick and the lock stay and the state is
 * retried on the timer; dark_retry() releases once it went out, so a rest
 * never begins with a dark-wake owner still in DARK.
 *
 * @param me The AO, in the dark wake, every dark-wake owner at rest.
 * @return The transition back to ASLEEP, or handled while the state is owed.
 */
static QState dark_back(LowPower * const me)
{
    QState status;

    tmo_stop(&me->round_tmo);
    me->dark_resting  = false;
    me->dark_pending  = 0U;
    me->waiting       = 0U;
    if ( state_send(me, (uint8_t)KODE_LP_ASLEEP, (uint8_t)KODE_LP_CAUSE_TIMER,
                   (uint8_t)KODE_LP_DARK) )
    {
        status = dark_release(me);
    }
    else
    {
        me->dark_closing = true;
        KLOG_W(TAG, "dark wake: ASLEEP not published; the wake holds the "
                    "tick and the lock until it is");
        status = Q_HANDLED();
    }
    return status;
}

/**
 * @brief LOWPOWER_STATE_RETRY_SIG in the dark wake.
 *
 * A DARK that went out late changes nothing else. The ASLEEP that ends the
 * wake, once out, lets the wake go.
 *
 * @param me The AO, in the dark wake.
 * @return The QP status.
 */
static QState dark_retry(LowPower * const me)
{
    QState status = Q_HANDLED();
    bool const closing = me->dark_closing;

    if ( state_retry_fire(me) && closing )
    {
        status = dark_release(me);
    }
    return status;
}

/**
 * @brief LOWPOWER_DARK_DONE_SIG: one dark-wake owner's work is finished.
 *
 * @param me The AO, in the dark wake.
 * @param d  The note.
 * @return The QP status.
 */
static QState dark_done(LowPower * const me, const LowPowerDarkDoneEvt *d)
{
    QState status = Q_HANDLED();

    if ( WAITING_BITS > d->who )
    {
        uint32_t const bit = (uint32_t)1U << d->who;

        me->dark_pending &= ~bit;
    }
    if ( (0U == me->dark_pending) && (!me->dark_resting) && (!me->dark_closing) )
    {
        KLOG_I(TAG, "the dark-wake owners are done");
        status = dark_rest(me);
    }
    return status;
}

/**
 * @brief A dark-wake owner's answer to the dark wake's round.
 *
 * @param me The AO, in the dark wake.
 * @param a  The answer.
 * @return The QP status.
 */
static QState dark_ready(LowPower * const me, const LowPowerReadyEvt *a)
{
    QState status = Q_HANDLED();

    if ( (!me->dark_resting) || (a->round != me->round) || (a->who >= me->joined)
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
        status = dark_abandon(me);
    }
    else
    {
        me->waiting &= ~(1U << a->who);
        if ( 0U == me->waiting )
        {
            status = dark_back(me);
        }
    }
    return status;
}

/**
 * @brief The dark wake's round ran out of time: name whoever is silent.
 *
 * @param me The AO, in the dark wake.
 * @return The QP status.
 */
static QState dark_round_timeout(LowPower * const me)
{
    QState status = Q_HANDLED();

    if ( me->dark_resting )
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
        status = dark_abandon(me);
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
 * Another owner's time wakes the whole board, even inside a dark wake. A
 * dark-wake owner's wakes it with the glass dark; the minute's work, if it falls due in
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
        status = dark_begin(me, now);
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
    else if ( off_due(me, me->off_since_us, now) )
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
             || (time_due(me->wake_due_us, now) && (!me->dark)) )
    {
        status = asleep_owner(me, now);
    }
    else
    {
        minute_work(me);
        minute_schedule(me, now, me->off_since_us);
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
    minute_schedule(me, now, me->off_since_us);
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
        if ( 0U != me->waiting )
        {
            /* Still waited for. */
        }
        else if ( round_held_ask(me) )
        {
            /* The owners of what an app had at rest answer now. */
        }
        else
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
    round_post(me, again);
}

/* ---- An app's rest, part by part ---------------------------------------------------
 * kode_sys_rest() names parts and this AO puts them away one at a time, in
 * lowpower_part.h's order, while the board stays AWAKE: no state is published
 * and the owners that are not named hear nothing. Each owner is asked alone,
 * with the round it already knows posted to it, or with the glass's own door,
 * and the next part is asked only when it has answered. The CPU part is this
 * AO's own: the tick stops and the lock goes, as in ASLEEP, until a wake.
 * A round of the board that goes through ends the rest: the board has every
 * part. One that may be given up asks the owners of the rest last
 * (round_begin()), so that given up it leaves the rest as it was.
 */

/** @brief Runs in interrupt context: the header's line the app named is at its level. */
static void part_pin_isr(void *arg)
{
    static const LowPowerPartWakeEvt evt = {
        QEVT_INITIALIZER(LOWPOWER_PART_WAKE_SIG), (uint8_t)KODE_LP_WAKE_PIN
    };

    (void)arg;
    kqp_post_from_isr(AO_LowPower, &evt.super);
}

/** @brief Runs in the timer task: a one-shot of the wake time came due. */
static void part_timer_cb(void *arg)
{
    static const LowPowerPartWakeEvt evt = {
        QEVT_INITIALIZER(LOWPOWER_PART_WAKE_SIG), (uint8_t)KODE_LP_WAKE_TIMER
    };

    (void)arg;
    QACTIVE_POST(AO_LowPower, &evt.super, NULL);
}

/**
 * @brief The participant that joined under @p name.
 *
 * @param me   The AO.
 * @param name The name an owner joined with.
 * @return Its id; KODE_LOWPOWER_WHO_NONE when no owner of that name is in the
 *         build, its device parked at boot.
 */
static uint8_t part_who(const LowPower *me, const char *name)
{
    uint8_t who = KODE_LOWPOWER_WHO_NONE;

    for ( uint8_t i = 0U; i < me->joined; i++ )
    {
        if ( strcmp(me->parts[i].name, name) == 0 )
        {
            who = i;
        }
    }
    return who;
}

/**
 * @brief Keep what a task reads of the rest: the facade's answers so far, the
 *        last of them, whether a request is being arranged and the parts held.
 *
 * @param me The AO.
 */
static void part_word_set(const LowPower *me)
{
    uint32_t word = ((uint32_t)me->part_answers << PART_WORD_ANSWERS_SHIFT)
                    | ((uint32_t)me->part_rc << PART_WORD_RC_SHIFT)
                    | (uint32_t)me->part_rest;

    if ( 0U != me->part_step )
    {
        word |= PART_WORD_OPEN;
    }
    atomic_store(&s_part_word, word);
}

/**
 * @brief Put one LOWPOWER_PART_SIG on the bus.
 *
 * @param me    The AO.
 * @param what  The kode_lp_part_what_t.
 * @param rc    An answer's kode_status_t.
 * @param cause A wake's kode_lp_wake_t.
 * @param src   The source an answer is for.
 * @param why   Why an answer is not KODE_OK, "part: why"; NULL for none.
 */
static void part_publish(const LowPower *me, uint8_t what, kode_status_t rc,
                         uint8_t cause, uint8_t src, const char *why)
{
    LowPowerPartEvt *ev = Q_NEW_X(LowPowerPartEvt, 0U, LOWPOWER_PART_SIG);

    if ( NULL == ev )
    {
        /* A task still reads the word; an AO asks again when it hears nothing. */
        KLOG_W(TAG, "out of events; the rest's news was not published");
    }
    else
    {
        ev->what  = what;
        ev->rc    = (uint8_t)rc;
        ev->parts = me->part_rest;
        ev->cause = cause;
        ev->src   = src;
        why_copy(ev->why, sizeof(ev->why), why);
        QACTIVE_PUBLISH(&ev->super, AO_LowPower);
    }
}

/**
 * @brief Answer a request for parts: the word a task reads, then the event.
 *
 * The word counts the facade's answers alone, so a task that waits for its
 * own does not take the console's for it.
 *
 * @param me  The AO.
 * @param src The request's source.
 * @param rc  KODE_OK when every part is as asked.
 * @param why Why not, "part: why"; NULL with KODE_OK.
 */
static void part_answer(LowPower * const me, uint8_t src, kode_status_t rc,
                        const char *why)
{
    why_copy(s_part_said, sizeof(s_part_said), why);
    if ( KODE_OK != rc )
    {
        KLOG_N(TAG, "rest (source %u) refused: %s", (unsigned)src, s_part_said);
    }
    if ( (uint8_t)KODE_LOWPOWER_SRC_APP == src )
    {
        /* Before the count moves: the task reads the reason once it has seen the count. */
        why_copy(s_part_why, sizeof(s_part_why), why);
        me->part_answers += 1U;
        me->part_rc       = (uint8_t)rc;
    }
    part_word_set(me);
    part_publish(me, (uint8_t)KODE_LP_PART_ANSWER, rc, (uint8_t)KODE_LP_WAKE_NONE, src, why);
}

/**
 * @brief Let go of the wake sources the app chose.
 *
 * The header's position goes back, through its owner's table, to what it was
 * before the wake line took it: free, or an input with the pull it had. An
 * input with an interrupt attached is never taken, so there is none to put
 * back. A one-shot that is out cannot be taken back: its event finds no time
 * due.
 *
 * @param me The AO.
 */
static void part_wakes_off(LowPower * const me)
{
    if ( me->part_pin_armed )
    {
        kode_status_t rc;

        khal_power_wake_disarm(me->part_gpio);
        me->part_pin_armed = false;
        if ( me->part_pin_free )
        {
            rc = khal_exp_release(me->part_wake_pin);
        }
        else
        {
            rc = khal_exp_input(me->part_wake_pin, me->part_pin_pulled);
        }
        if ( KODE_OK != rc )
        {
            KLOG_W(TAG, "rest: header position %u did not go back to what it was (rc %d)",
                   (unsigned)me->part_wake_pin, (int)rc);
        }
    }
    me->part_due_us = 0U;
}

/**
 * @brief Tell one owner the board is AWAKE, and nobody else.
 *
 * What an owner restores its device on. It is posted, not published: the
 * other owners never rested.
 *
 * @param me  The AO.
 * @param who The participant.
 */
static void part_awake_post(LowPower * const me, uint8_t who)
{
    LowPowerStateEvt *ev = Q_NEW_X(LowPowerStateEvt, 0U, LOWPOWER_STATE_SIG);

    if ( NULL == ev )
    {
        KLOG_E(TAG, "out of events; %s was not told to come back", me->parts[who].name);
    }
    else
    {
        ev->state = (uint8_t)KODE_LP_AWAKE;
        ev->cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
        ev->prev  = (uint8_t)KODE_LP_AWAKE;
        if ( !QACTIVE_POST_X(me->parts[who].ao, &ev->super, 1U, &me->super) )
        {
            KLOG_E(TAG, "%s's queue is full; it was not told to come back",
                   me->parts[who].name);
        }
    }
}

/**
 * @brief Tell the owners of the CPU part the board is AWAKE.
 *
 * The expander's line is the plain interrupt again and the scheduler arms its
 * minute. Both restore on any AWAKE, rested or not.
 *
 * @param me The AO.
 */
static void part_cpu_restore(LowPower * const me)
{
    static const char * const owners[] = PART_CPU_OWNERS;

    for ( uint8_t i = 0U; Q_DIM(owners) > i; i++ )
    {
        uint8_t const who = part_who(me, owners[i]);

        if ( KODE_LOWPOWER_WHO_NONE != who )
        {
            part_awake_post(me, who);
        }
    }
}

/**
 * @brief End the CPU part: the tick and the lock back, the wake sources let go.
 *
 * A cable in the first reading after this came while the cable port was off
 * the bus, as after ASLEEP (POWER_EVT_SIG in LowPower_awake).
 *
 * @param me The AO, with the CPU part at rest.
 */
static void part_cpu_up(LowPower * const me)
{
    light_up();
    me->cable_check = true;
    part_wakes_off(me);
    me->part_rest &= (uint8_t)~KODE_LP_PART_CPU;
    part_cpu_restore(me);
}

/**
 * @brief A wake source ended the CPU part: say so. The other parts stay away.
 *
 * @param me    The AO, with the CPU part at rest.
 * @param cause The kode_lp_wake_t.
 */
static void part_cpu_wake(LowPower * const me, uint8_t cause)
{
    static const char * const names[] = {
        "nothing", "a button", "a touch", "the header's line", "its time",
        "a console line", "an MQTT message", "a cable", "a request",
        "work of the board's fell due", "the cell is at its cut"
    };
    _Static_assert(Q_DIM(names) == ((size_t)KODE_LP_WAKE_BATTERY + 1U),
                   "a name for every wake, BATTERY being the last");
    const char *name = "?";

    if ( Q_DIM(names) > cause )
    {
        name = names[cause];
    }
    part_cpu_up(me);
    KLOG_N(TAG, "rest: the P4 is awake again (%s); parts 0x%02x stay at rest",
           name, (unsigned)me->part_rest);
    part_word_set(me);
    part_publish(me, (uint8_t)KODE_LP_PART_WOKE, KODE_OK, cause,
                 (uint8_t)KODE_LOG_SRC_NONE, NULL);
}

/**
 * @brief The sequence in hand is over: answer it, and let the P4 sleep if it asked.
 *
 * The answer goes out before the tick stops and the lock goes (rule 6). A
 * sleeping P4 keeps what ASLEEP keeps: the minute, which reads the charger,
 * counts off_after and serves an owner's time (part_minute()), and the look
 * for a cable (light_down()).
 *
 * @param me  The AO.
 * @param rc  KODE_OK when every part is as asked.
 * @param why Why not; NULL with KODE_OK.
 */
static void part_finish(LowPower * const me, kode_status_t rc, const char *why)
{
    bool const sleeps = (KODE_OK == rc) && (!me->part_up)
                        && ((me->part_rest & KODE_LP_PART_CPU) != 0U);

    tmo_stop(&me->round_tmo);
    me->part_step  = 0U;
    me->part_again = false;
    me->part_done  = 0U;
    if ( (KODE_OK == rc) && me->part_up )
    {
        KLOG_N(TAG, "rest ended: every part is back");
    }
    else if ( KODE_OK == rc )
    {
        KLOG_N(TAG, "rest: parts 0x%02x stay at rest", (unsigned)me->part_rest);
    }
    else
    {
        /* part_answer() says why. */
    }
    part_answer(me, me->part_src, rc, why);
    if ( sleeps )
    {
        uint64_t const now = khal_time_us();

        KLOG_N(TAG, "rest: the P4 light-sleeps when every task is idle; a button, a cable, "
                    "the cell at its cut or off_after ends it (touch %u, header position %u, "
                    "after %u ms)",
               (unsigned)me->part_wake_touch, (unsigned)me->part_wake_pin,
               (unsigned)me->part_after_ms);
        me->part_since_us = now;
        minute_schedule(me, now, me->part_since_us);
        light_down();
    }
}

/**
 * @brief A step failed: the sequence stops there, and what rested stays at rest.
 *
 * @param me  The AO.
 * @param who Whose reason: the part's name.
 * @param why The reason.
 */
static void part_fail(LowPower * const me, const char *who, const char *why)
{
    char text[KODE_LOWPOWER_REFUSAL_MAX];

    (void)snprintf(text, sizeof(text), "%s: %s", who, why);
    part_finish(me, KODE_DENIED, text);
}

/**
 * @brief Arm a one-shot toward the wake time: the whole wait, or one hop of it.
 *
 * A one-shot cannot be taken back, so none runs longer than
 * KODE_LOWPOWER_PART_HOP_MS: a longer time is reached in hops, each armed
 * when the one before it is heard (part_wake_heard()). One that is already
 * out and comes due before the wake time serves as the first hop.
 *
 * @param me  The AO, with part_due_us set.
 * @param now khal_time_us().
 * @return false when every one-shot a rest may have is out, or the timer
 *         would not start.
 */
static bool part_timer_hop(LowPower * const me, uint64_t now)
{
    uint64_t wait = time_wait_us(me->part_due_us, now);
    bool     ok   = false;

    if ( ((uint64_t)KODE_LOWPOWER_PART_HOP_MS * US_PER_MS) < wait )
    {
        wait = (uint64_t)KODE_LOWPOWER_PART_HOP_MS * US_PER_MS;
    }
    if ( (0U < me->part_timers) && (me->part_hop_us > now)
        && (me->part_hop_us <= me->part_due_us) )
    {
        ok = true;
    }
    else if ( KODE_LOWPOWER_PART_TIMERS <= me->part_timers )
    {
        KLOG_W(TAG, "rest: %u wake timers are out already", (unsigned)me->part_timers);
    }
    else if ( khal_timer_once(&part_timer_cb, NULL, wait) == KODE_OK )
    {
        me->part_timers += 1U;
        me->part_hop_us  = now + wait;
        ok = true;
    }
    else
    {
        KLOG_W(TAG, "rest: the wake timer would not start");
    }
    return ok;
}

/**
 * @brief Start counting the wake time the app gave.
 *
 * @param me The AO.
 * @return false when no one-shot could be armed for it.
 */
static bool part_timer_arm(LowPower * const me)
{
    uint64_t const now = khal_time_us();
    bool           ok;

    me->part_due_us = now + ((uint64_t)me->part_after_ms * US_PER_MS);
    ok = part_timer_hop(me, now);
    if ( !ok )
    {
        me->part_due_us = 0U;
    }
    return ok;
}

/**
 * @brief The last thing before the CPU part rests: arm what the app chose.
 *
 * @param me The AO, the expander's line armed.
 * @return NULL when the sources are armed; otherwise whose refusal and why.
 */
static const char *part_cpu_arm(LowPower * const me)
{
    const char *why = NULL;

    if ( 0U != me->part_wake_pin )
    {
        if ( khal_power_wake_arm(me->part_gpio, me->part_wake_level, &part_pin_isr,
                                 NULL) == KODE_OK )
        {
            me->part_pin_armed = true;
        }
        else
        {
            why = "pin: no wake line";
        }
    }
    if ( (NULL == why) && (0U != me->part_after_ms) && !part_timer_arm(me) )
    {
        why = "timer: busy";
    }
    if ( NULL != why )
    {
        part_wakes_off(me);
    }
    return why;
}

/**
 * @brief Post a round to one owner alone: rest your device, the board stays AWAKE.
 *
 * @param me   The AO.
 * @param who  The participant.
 * @param part The KODE_LP_PART_* it is asked for.
 * @return true when the owner has the round.
 */
static bool part_round_post(LowPower * const me, uint8_t who, uint8_t part)
{
    LowPowerRoundEvt *ev = Q_NEW_X(LowPowerRoundEvt, 0U, LOWPOWER_ROUND_SIG);
    bool posted = false;

    if ( NULL != ev )
    {
        ev->target = (uint8_t)KODE_LP_ASLEEP;
        ev->parts  = part;
        ev->round  = me->round;
        posted = QACTIVE_POST_X(me->parts[who].ao, &ev->super, 1U, &me->super);
    }
    return posted;
}

/**
 * @brief Post the glass's owner a request to sleep or wake, through this AO's door.
 *
 * @param me    The AO.
 * @param who   The glass's owner.
 * @param sleep true to sleep, false to wake.
 * @return true when the owner has the request.
 */
static bool part_glass_post(LowPower * const me, uint8_t who, bool sleep)
{
    DisplaySleepReqEvt *ev = Q_NEW_X(DisplaySleepReqEvt, 0U, DISPLAY_SLEEP_REQ_SIG);
    bool posted = false;

    if ( NULL != ev )
    {
        ev->sleep = sleep;
        ev->who   = DISPLAY_SLEEP_WHO_REST;
        posted = QACTIVE_POST_X(me->parts[who].ao, &ev->super, 1U, &me->super);
    }
    return posted;
}

/**
 * @brief Post the glass's owner a request to take the panel's supply away.
 *
 * @param me  The AO.
 * @param who The glass's owner, the glass asleep.
 * @return true when the owner has the request.
 */
static bool part_supply_post(LowPower * const me, uint8_t who)
{
    DisplaySupplyReqEvt *ev = Q_NEW_X(DisplaySupplyReqEvt, 0U, DISPLAY_SUPPLY_REQ_SIG);
    bool posted = false;

    if ( NULL != ev )
    {
        ev->round = me->round;
        posted = QACTIVE_POST_X(me->parts[who].ao, &ev->super, 1U, &me->super);
    }
    return posted;
}

/**
 * @brief Ask the owner of @p part to put it away.
 *
 * @param me   The AO.
 * @param who  The owner.
 * @param part One KODE_LP_PART_* bit.
 * @return true when the owner has been asked.
 */
static bool part_ask_down(LowPower * const me, uint8_t who, uint8_t part)
{
    bool asked;

    if ( KODE_LP_PART_SCREEN == part )
    {
        asked = part_glass_post(me, who, true);
    }
    else if ( KODE_LP_PART_PANEL == part )
    {
        asked = part_supply_post(me, who);
    }
    else
    {
        asked = part_round_post(me, who, part);
    }
    return asked;
}

/**
 * @brief Bring @p part back.
 *
 * Only the glass answers: the others restore on the AWAKE posted to them,
 * with retries of their own.
 *
 * @param me   The AO.
 * @param who  The owner; KODE_LOWPOWER_WHO_NONE when the build has none.
 * @param part One KODE_LP_PART_* bit.
 * @return true when an answer is awaited.
 */
static bool part_ask_up(LowPower * const me, uint8_t who, uint8_t part)
{
    bool const glass = (KODE_LP_PART_SCREEN == part) || (KODE_LP_PART_PANEL == part);
    bool waiting = false;

    if ( KODE_LP_PART_CPU == part )
    {
        part_cpu_up(me);
    }
    else if ( KODE_LOWPOWER_WHO_NONE == who )
    {
        me->part_rest &= (uint8_t)~part;
    }
    else if ( glass )
    {
        if ( !part_glass_post(me, who, false) )
        {
            /* Left at rest: the request is answered with the reason. */
            part_fail(me, lowpower_part_name(part), "out of events");
        }
        waiting = true;
    }
    else
    {
        part_awake_post(me, who);
        me->part_rest &= (uint8_t)~part;
    }
    return waiting;
}

/**
 * @brief Note that the part of the step in hand is put away.
 *
 * A part that does not stay at rest (the sound, the card) is done for this
 * request and held nowhere: asked again, its owner is asked again.
 *
 * @param me   The AO.
 * @param part One KODE_LP_PART_* bit.
 */
static void part_put_away(LowPower * const me, uint8_t part)
{
    me->part_done |= part;
    if ( lowpower_part_stays(part) )
    {
        me->part_rest |= part;
    }
}

/**
 * @brief Open the next round of the sequence for one owner.
 *
 * @param me   The AO.
 * @param who  The owner.
 * @param part One KODE_LP_PART_* bit.
 * @return true when the owner has been asked and its step's time is running.
 */
static bool part_step_open(LowPower * const me, uint8_t who, uint8_t part)
{
    bool asked;

    me->round += 1U;
    if ( ROUND_NONE == me->round )
    {
        me->round += 1U;
    }
    me->sealed   = true;
    me->part_who = who;
    asked = part_ask_down(me, who, part);
    if ( asked )
    {
        tmo_start(&me->round_tmo, KODE_LOWPOWER_PART_STEP_MS);
    }
    return asked;
}

/**
 * @brief Go on with the CPU part: ask its next owner, or arm the wake sources.
 *
 * The expander's owner arms the wake lines and the scheduler gives its next
 * minute, each in a step of its own. With nobody left to ask the sources the
 * app chose are armed and the part is put away.
 *
 * @param me The AO, the CPU part the step in hand.
 * @return true when the sequence waits for an owner or was ended by a
 *         refusal; false when the CPU part is put away.
 */
static bool part_cpu_go(LowPower * const me)
{
    static const char * const owners[] = PART_CPU_OWNERS;
    bool        settled = false;
    const char *why     = NULL;

    while ( (!settled) && (Q_DIM(owners) > me->part_cpu_stage) )
    {
        uint8_t const who = part_who(me, owners[me->part_cpu_stage]);

        me->part_cpu_stage += 1U;
        if ( KODE_LOWPOWER_WHO_NONE == who )
        {
            /* Not in the build: nothing of its to arm. */
        }
        else if ( part_step_open(me, who, KODE_LP_PART_CPU) )
        {
            settled = true;
        }
        else
        {
            why     = "cpu: out of events";
            settled = true;
        }
    }
    if ( (!settled) && (NULL == why) && me->vbus )
    {
        /* The request was judged on its arrival; a busy owner asked again
         * may have let a cable come in since, and the P4 never light-sleeps
         * on a cable. */
        why = LOWPOWER_PART_WHY_CABLE;
    }
    if ( (!settled) && (NULL == why) )
    {
        why = part_cpu_arm(me);
        if ( NULL == why )
        {
            part_put_away(me, KODE_LP_PART_CPU);
        }
    }
    if ( NULL != why )
    {
        /* Whatever an owner armed was for a sleep that does not come. */
        part_cpu_restore(me);
        part_finish(me, KODE_DENIED, why);
        settled = true;
    }
    return settled;
}

/**
 * @brief Go on with the sequence in hand until an owner has to answer, or it is over.
 *
 * @param me The AO, AWAKE, with a sequence open (part_src and part_up set).
 */
static void part_advance(LowPower * const me)
{
    bool waiting = false;

    while ( !waiting )
    {
        uint8_t part;

        if ( me->part_up )
        {
            part = lowpower_part_next_up(me->part_rest);
        }
        else
        {
            part = lowpower_part_next_down(me->part_want, me->part_rest | me->part_done);
        }
        if ( 0U == part )
        {
            part_finish(me, KODE_OK, NULL);
            waiting = true;
        }
        else
        {
            uint8_t const who = part_who(me, lowpower_part_owner(part));

            me->part_step = part;
            me->part_who  = who;
            if ( me->part_up )
            {
                waiting = part_ask_up(me, who, part);
                if ( waiting && (0U != me->part_step) )
                {
                    tmo_start(&me->round_tmo, KODE_LOWPOWER_PART_STEP_MS);
                }
            }
            else if ( KODE_LP_PART_CPU == part )
            {
                me->part_cpu_stage = 0U;
                /* The owners say their times again: one kept from an earlier
                 * rest may have passed, and would end this one at its first minute. */
                me->wake_due_us    = 0U;
                me->app_due_us     = 0U;
                waiting = part_cpu_go(me);
            }
            else if ( KODE_LOWPOWER_WHO_NONE == who )
            {
                /* No such owner in the build: its device was parked at boot. */
                part_put_away(me, part);
            }
            else
            {
                if ( !part_step_open(me, who, part) )
                {
                    part_fail(me, lowpower_part_name(part), "out of events");
                }
                waiting = true;
            }
        }
        /* A step is open from here on: a task reads that in the word. */
        part_word_set(me);
    }
}

/**
 * @brief The owner asked in this step says its part is as asked.
 *
 * @param me The AO, a step open.
 */
static void part_step_done(LowPower * const me)
{
    bool over = false;

    tmo_stop(&me->round_tmo);
    if ( me->part_up )
    {
        /* The glass's wake brings the supply back with it. */
        me->part_rest &= (uint8_t)~(KODE_LP_PART_SCREEN | KODE_LP_PART_PANEL);
    }
    else if ( KODE_LP_PART_CPU == me->part_step )
    {
        over = part_cpu_go(me);
    }
    else
    {
        part_put_away(me, me->part_step);
    }
    if ( !over )
    {
        part_advance(me);
    }
    else
    {
        part_word_set(me);
    }
}

/**
 * @brief The slot of one part among the late answers: the position of its bit.
 *
 * @param part One KODE_LP_PART_* bit.
 * @return 0 to PART_COUNT - 1.
 */
static uint8_t part_slot(uint8_t part)
{
    uint8_t slot = 0U;

    while ( ((PART_COUNT - 1U) > slot) && (((uint32_t)part >> slot) > 1U) )
    {
        slot++;
    }
    return slot;
}

/**
 * @brief Note that the step in hand ran out of time: its answer is owed.
 *
 * Each part has its own slot, so a second late step leaves the first one's
 * answer owed as well.
 *
 * @param me The AO, a step open.
 */
static void part_late_note(LowPower * const me)
{
    uint8_t const slot = part_slot(me->part_step);

    me->part_late             |= me->part_step;
    me->part_late_round[slot]  = me->round;
    me->part_late_who[slot]    = me->part_who;
}

/**
 * @brief The late step an answer belongs to.
 *
 * @param me    The AO.
 * @param round The round the answer names.
 * @param who   The participant that answered.
 * @return The part's KODE_LP_PART_* bit; 0 when no late step is the answer's.
 */
static uint8_t part_late_find(const LowPower * const me, uint32_t round, uint8_t who)
{
    uint8_t found = 0U;

    for ( uint8_t slot = 0U; (PART_COUNT > slot) && (0U == found); slot++ )
    {
        uint8_t const part = (uint8_t)(1U << slot);

        if ( ((me->part_late & part) != 0U) && (round == me->part_late_round[slot])
            && (who == me->part_late_who[slot]) )
        {
            found = part;
        }
    }
    return found;
}

/**
 * @brief Whether a part put away after its time comes back at once.
 *
 * It does when the app has ended its rest since, and when the app's rest
 * neither holds it nor asks for it: nothing else would ever bring it back.
 * Otherwise it is held with that rest, and the rest's end brings it back.
 *
 * @param me   The AO.
 * @param part One KODE_LP_PART_* bit.
 * @return true when its owner is to be told to come back.
 */
static bool part_late_back(const LowPower * const me, uint8_t part)
{
    return me->part_up || (((me->part_want | me->part_rest) & part) == 0U);
}

/**
 * @brief LOWPOWER_READY_SIG outside a step: the answer of one that ran out of time.
 *
 * An owner that did put its part away after its time has it away all the
 * same. While the app's rest wants the part, it is held from here and the
 * end of the rest brings it back. When the app has ended its rest since, or
 * asked for a rest without it, the owner is told to come back at once
 * (part_late_back()). A busy answer changes nothing.
 *
 * @param me The AO, AWAKE.
 * @param a  The answer.
 * @return true when the answer was a late step's.
 */
static bool part_late_ready(LowPower * const me, const LowPowerReadyEvt *a)
{
    uint8_t const late = part_late_find(me, a->round, a->who);

    if ( 0U != late )
    {
        me->part_late &= (uint8_t)~late;
        if ( a->busy || !lowpower_part_stays(late) )
        {
            /* Nothing to hold, nothing to bring back. */
        }
        else if ( part_late_back(me, late) )
        {
            KLOG_N(TAG, "rest: the %s rested after its time and no rest wants it; it comes back",
                   lowpower_part_name(late));
            part_awake_post(me, a->who);
        }
        else
        {
            me->part_rest |= late;
            KLOG_N(TAG, "rest: the %s rested after its time; parts 0x%02x stay at rest",
                   lowpower_part_name(late), (unsigned)me->part_rest);
            part_word_set(me);
        }
    }
    return (0U != late);
}

/**
 * @brief The owner asked in this step is busy: ask it again later, or stop there.
 *
 * As a requested round is begun again (round_busy()), the owner is asked
 * again KODE_LOWPOWER_REQUEST_RETRY_MS later, on the step's own one-shot, as
 * long as that ask falls inside the request's window. Past it the sequence
 * stops with this reason, and what rested stays at rest.
 *
 * @param me  The AO, a step open.
 * @param why The owner's reason.
 */
static void part_busy(LowPower * const me, const char *why)
{
    if ( retry_window_open(me->part_us, khal_time_us() + RETRY_US) )
    {
        me->part_again = true;
        tmo_start(&me->round_tmo, KODE_LOWPOWER_REQUEST_RETRY_MS);
        KLOG_N(TAG, "rest: the %s is busy (%s); its owner is asked again in %u ms",
               lowpower_part_name(me->part_step), why,
               (unsigned)KODE_LOWPOWER_REQUEST_RETRY_MS);
    }
    else
    {
        part_fail(me, lowpower_part_name(me->part_step), why);
    }
}

/**
 * @brief The wait after a busy answer ran out: ask the owner of the step again.
 *
 * The part is not put away, so the sequence opens its step again, a round of
 * a new number; the CPU part asks its owners from the first.
 *
 * @param me The AO, a step waiting (part_again).
 */
static void part_ask_again(LowPower * const me)
{
    me->part_again = false;
    part_advance(me);
}

/**
 * @brief LOWPOWER_READY_SIG while a step is open: the owner's answer to its round.
 *
 * An answer that comes while the step waits to be asked again is not the
 * step's: the round it answers was refused already.
 *
 * @param me The AO, AWAKE.
 * @param a  The answer.
 * @return true when the answer was this step's.
 */
static bool part_ready(LowPower * const me, const LowPowerReadyEvt *a)
{
    bool const mine = (0U != me->part_step) && (!me->part_again)
                      && (a->round == me->round) && (a->who == me->part_who);

    if ( mine && a->busy )
    {
        char why[KODE_LOWPOWER_WHY_MAX];

        why_copy(why, sizeof(why), a->why);
        if ( KODE_LP_PART_CPU == me->part_step )
        {
            /* What an earlier owner of the part armed is given back. */
            part_cpu_restore(me);
        }
        part_busy(me, why);
    }
    else if ( mine )
    {
        part_step_done(me);
    }
    else
    {
        /* Another round's answer: the caller looks further. */
    }
    return mine;
}

/**
 * @brief DISPLAY_SLEEP_SIG for an app's own request: a glass it woke is not at rest.
 *
 * kode_sys_screen_sleep(false) wakes a glass the rest put to sleep, and its
 * supply with it. The DSI link is up from then and holds the P4 awake, so the
 * screen and the panel leave the parts held: the CPU part asked for next is
 * refused until the screen rests again.
 *
 * @param me The AO.
 * @param s  The answer.
 */
static void part_glass_woken(LowPower * const me, const DisplaySleepEvt *s)
{
    uint8_t const glass = (uint8_t)(KODE_LP_PART_SCREEN | KODE_LP_PART_PANEL);

    if ( (DISPLAY_SLEEP_WHO_REST != s->who) && ((uint8_t)KODE_OK == s->rc) && (!s->asleep)
        && ((me->part_rest & glass) != 0U) )
    {
        me->part_rest &= (uint8_t)~glass;
        KLOG_N(TAG, "rest: the app woke the screen through its own door; parts 0x%02x stay at rest",
               (unsigned)me->part_rest);
        part_word_set(me);
    }
}

/**
 * @brief DISPLAY_SLEEP_SIG: the glass's owner answered a request to sleep or wake.
 *
 * Only the answers to this AO's own requests are judged (DISPLAY_SLEEP_WHO_REST):
 * an app's own door has its own, and is read only for a glass it woke
 * (part_glass_woken()). A sleep that ran out of time and came after it still
 * leaves the glass asleep: held from then while the app's rest wants it,
 * woken otherwise (part_late_back()), and held all the same when its owner
 * has no room for the wake, so the next end of the rest wakes it.
 *
 * @param me The AO, AWAKE.
 * @param s  The answer.
 */
static void part_glass_answer(LowPower * const me, const DisplaySleepEvt *s)
{
    bool const up_step   = me->part_up && ((KODE_LP_PART_SCREEN == me->part_step)
                                           || (KODE_LP_PART_PANEL == me->part_step));
    bool const down_step = (!me->part_up) && (KODE_LP_PART_SCREEN == me->part_step);

    if ( DISPLAY_SLEEP_WHO_REST != s->who )
    {
        /* An app's own request: not this AO's to judge. */
        part_glass_woken(me, s);
    }
    else if ( up_step || down_step )
    {
        if ( (uint8_t)KODE_OK != s->rc )
        {
            part_fail(me, lowpower_part_name(me->part_step), display_sleep_why_text(s->why));
        }
        else if ( s->asleep == down_step )
        {
            part_step_done(me);
        }
        else
        {
            /* The answer of an earlier request: this step's is still to come. */
        }
    }
    else if ( ((me->part_late & KODE_LP_PART_SCREEN) != 0U) && ((uint8_t)KODE_OK == s->rc)
              && s->asleep )
    {
        uint8_t const who = me->part_late_who[part_slot(KODE_LP_PART_SCREEN)];

        me->part_late &= (uint8_t)~KODE_LP_PART_SCREEN;
        if ( part_late_back(me, KODE_LP_PART_SCREEN) )
        {
            /* No rest wants the glass asleep: it wakes, as the end would have woken it. */
            KLOG_N(TAG, "rest: the screen rested after its time and no rest wants it; it wakes");
            if ( !part_glass_post(me, who, false) )
            {
                /* Owed, not dropped: held, so the next end of the rest wakes it. */
                me->part_rest |= KODE_LP_PART_SCREEN;
                KLOG_E(TAG, "rest: out of events; the screen stays asleep until the rest ends again");
                part_word_set(me);
            }
        }
        else
        {
            me->part_rest |= KODE_LP_PART_SCREEN;
            KLOG_N(TAG, "rest: the screen rested after its time; parts 0x%02x stay at rest",
                   (unsigned)me->part_rest);
            part_word_set(me);
        }
    }
    else
    {
        /* Nothing was waiting for it. */
    }
}

/**
 * @brief The owner asked in this step did not answer in time.
 *
 * Its answer may still come (part_late_ready()). The CPU part is given back
 * whole instead: nothing of it is left armed for a late answer to find.
 *
 * @param me The AO, a step open.
 */
static void part_step_timeout(LowPower * const me)
{
    KLOG_W(TAG, "rest: %s did not answer for the %s in %u ms",
           me->parts[me->part_who].name, lowpower_part_name(me->part_step),
           (unsigned)KODE_LOWPOWER_PART_STEP_MS);
    if ( KODE_LP_PART_CPU == me->part_step )
    {
        part_cpu_restore(me);
    }
    else if ( !me->part_up )
    {
        part_late_note(me);
    }
    else
    {
        /* A wake that comes late finds the part still held: asked again, it is as asked. */
    }
    part_fail(me, lowpower_part_name(me->part_step), "no answer");
}

/**
 * @brief LOWPOWER_PART_REQ_SIG while AWAKE: put parts away, or bring them back.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void part_request(LowPower * const me, const LowPowerPartReqEvt *r)
{
    if ( 0U != me->part_step )
    {
        /* One sequence at a time, and the one in hand goes on. */
        if ( ((uint8_t)KODE_LOWPOWER_SRC_APP == r->src)
            && ((uint8_t)KODE_LOWPOWER_SRC_APP != me->part_src) )
        {
            /* The one in hand is not an app's, so no task waits for an answer
             * of its own: this refusal is counted, and its task hears it now. */
            part_answer(me, r->src, KODE_DENIED, "rest: underway");
        }
        else
        {
            /* The one in hand has its own answer to come: this refusal is told
             * on the bus and changes nothing a task reads of it. */
            KLOG_N(TAG, "rest (source %u) refused: a rest is being arranged", (unsigned)r->src);
            part_publish(me, (uint8_t)KODE_LP_PART_ANSWER, KODE_DENIED,
                         (uint8_t)KODE_LP_WAKE_NONE, r->src, "rest: underway");
        }
    }
    else if ( r->end )
    {
        KLOG_N(TAG, "rest ends (source %u): parts 0x%02x come back",
               (unsigned)r->src, (unsigned)me->part_rest);
        me->part_src  = r->src;
        me->part_up   = true;
        me->part_us   = khal_time_us();
        part_advance(me);
    }
    else
    {
        khal_exp_info_t     info  = { 0 };
        uint8_t             parts = r->parts;
        lowpower_part_ask_t ask   = { 0 };
        const char         *why;

        ask.at_rest    = me->part_rest;
        ask.glass      = (KODE_LOWPOWER_WHO_NONE
                          != part_who(me, lowpower_part_owner(KODE_LP_PART_SCREEN)));
        ask.cable      = me->vbus;
        ask.wake_touch = r->wake_touch;
        ask.wake_pin   = (0U != r->wake_pin);
        ask.pin_known  = (KHAL_EXP_POS_MIN <= r->wake_pin) && (KHAL_EXP_POS_MAX >= r->wake_pin);
        if ( ask.pin_known )
        {
            khal_exp_info(r->wake_pin, &info);
            /* An input somebody listens to keeps its interrupt: a wake line would take it. */
            ask.pin_free = (KHAL_EXP_FREE == info.mode)
                           || ((KHAL_EXP_IN == info.mode) && (!info.irq));
        }
        why = lowpower_part_check(&parts, &ask);
        if ( (NULL == why) && ((parts & KODE_LP_PART_CPU) != 0U) && !me->power_ok )
        {
            why = "cpu: power management did not start";
        }
        if ( NULL != why )
        {
            part_answer(me, r->src, KODE_DENIED, why);
        }
        else
        {
            KLOG_N(TAG, "rest asked (source %u): parts 0x%02x", (unsigned)r->src,
                   (unsigned)parts);
            me->part_src  = r->src;
            me->part_up   = false;
            me->part_us   = khal_time_us();
            me->part_want = parts;
            me->part_done = 0U;
            if ( (parts & KODE_LP_PART_CPU) != 0U )
            {
                part_wakes_off(me);
                me->part_wake_touch = r->wake_touch;
                me->part_wake_level = r->wake_level;
                me->part_wake_pin   = r->wake_pin;
                me->part_after_ms   = r->wake_after_ms;
                me->part_gpio       = info.gpio;
                me->part_pin_free   = (KHAL_EXP_FREE == info.mode);
                me->part_pin_pulled = info.pulled;
            }
            part_advance(me);
        }
    }
}

/**
 * @brief The board takes every part for a state of its own: the app's rest is over.
 *
 * Nothing is brought back here. The round asks the owners of the rest, the
 * ones at rest answer ready, and the AWAKE that ends it restores them all.
 *
 * @param me The AO, in a round or about to begin one.
 */
static void part_take(LowPower * const me)
{
    if ( (0U != me->part_rest) || (0U != me->part_step) )
    {
        bool const open = (0U != me->part_step);

        if ( open )
        {
            tmo_stop(&me->round_tmo);
            me->part_step  = 0U;
            me->part_again = false;
            me->part_want  = 0U;
            me->part_done  = 0U;
        }
        if ( (me->part_rest & KODE_LP_PART_CPU) != 0U )
        {
            /* The minute found off_after reached: the round needs the clock. */
            light_up();
        }
        part_wakes_off(me);
        me->part_late = 0U;
        if ( open )
        {
            part_answer(me, me->part_src, KODE_DENIED, "board: takes the rest");
        }
        KLOG_N(TAG, "rest over: the board takes parts 0x%02x", (unsigned)me->part_rest);
        me->part_rest = 0U;
        part_word_set(me);
        part_publish(me, (uint8_t)KODE_LP_PART_TAKEN, KODE_OK, (uint8_t)KODE_LP_WAKE_NONE,
                     (uint8_t)KODE_LOG_SRC_NONE, NULL);
    }
}

/**
 * @brief Before a round that may be given up: what of the app's rest it must not touch yet.
 *
 * The round needs the system's clock, so a P4 still asleep wakes here and
 * the app is told. The devices the app put away stay as they are, and their
 * owners are the ones the round asks last.
 *
 * @param me The AO, about to begin a round, with no rest being arranged.
 * @return Bit n: participant n owns a device the app has at rest.
 */
static uint32_t part_round_keep(LowPower * const me)
{
    static const uint8_t devices[] = {
        KODE_LP_PART_TOUCH, KODE_LP_PART_SCREEN, KODE_LP_PART_PANEL, KODE_LP_PART_RADIO
    };
    uint32_t held = 0U;

    if ( (me->part_rest & KODE_LP_PART_CPU) != 0U )
    {
        /* The minute found off_after reached. */
        part_cpu_wake(me, (uint8_t)KODE_LP_WAKE_OWNER);
    }
    for ( uint8_t i = 0U; Q_DIM(devices) > i; i++ )
    {
        if ( (me->part_rest & devices[i]) != 0U )
        {
            uint8_t const who = part_who(me, lowpower_part_owner(devices[i]));

            if ( who < me->joined )
            {
                held |= ((uint32_t)1U << who);
            }
        }
    }
    return held;
}

/**
 * @brief Whether an event ends the CPU part, and as what.
 *
 * Any button does, on the expander's reading itself: with the tick stopped
 * no settle runs and AO_Buttons publishes nothing. A touch does when the app
 * asked for it. Everything that needs the system's clock to be served does:
 * a console line, an MQTT message, a cable, a cell at its cut and any request
 * to this AO. The cable and the cell show in the readings the look for a
 * cable and the minute ask for (cable_look(), part_minute()).
 *
 * @param me The AO, AWAKE, with the CPU part at rest.
 * @param e  The event.
 * @return The kode_lp_wake_t; KODE_LP_WAKE_NONE for an event that wakes nothing.
 */
static uint8_t part_wake_cause(const LowPower *me, QEvt const * const e)
{
    uint8_t cause = (uint8_t)KODE_LP_WAKE_NONE;

    switch ( e->sig )
    {
        case IOEXP_INPUT_SIG: {
            IoexpInputEvt const * const in = Q_EVT_CAST(IoexpInputEvt);

            if ( in->ok && ((in->changed & PART_BUTTONS_MASK) != 0U) )
            {
                cause = (uint8_t)KODE_LP_WAKE_BUTTON;
            }
            else if ( in->ok && me->part_wake_touch && ((in->changed & PART_TOUCH_MASK) != 0U)
                     && ((in->levels & PART_TOUCH_MASK) == 0U) )
            {
                cause = (uint8_t)KODE_LP_WAKE_TOUCH;
            }
            else
            {
                /* Another pin of the expander. */
            }
            break;
        }
        case LOWPOWER_TOP_SIG:
        case BUTTON_EVT_SIG: {
            cause = (uint8_t)KODE_LP_WAKE_BUTTON;
            break;
        }
        case TOUCH_EVT_SIG: {
            if ( me->part_wake_touch && !Q_EVT_CAST(TouchEvt)->stale )
            {
                cause = (uint8_t)KODE_LP_WAKE_TOUCH;
            }
            break;
        }
        case LOWPOWER_PART_WAKE_SIG: {
            uint8_t const src = Q_EVT_CAST(LowPowerPartWakeEvt)->cause;

            if ( (uint8_t)KODE_LP_WAKE_PIN == src )
            {
                if ( me->part_pin_armed )
                {
                    cause = src;
                }
            }
            else if ( time_due(me->part_due_us, khal_time_us()) )
            {
                cause = (uint8_t)KODE_LP_WAKE_TIMER;
            }
            else
            {
                /* A one-shot of an earlier rest. */
            }
            break;
        }
        case LOWPOWER_INPUT_SIG: {
            cause = (uint8_t)KODE_LP_WAKE_CONSOLE;
            break;
        }
        case MQTT_MSG_SIG: {
            cause = (uint8_t)KODE_LP_WAKE_MQTT;
            break;
        }
        case POWER_EVT_SIG: {
            if ( Q_EVT_CAST(PowerEvt)->vbus )
            {
                cause = (uint8_t)KODE_LP_WAKE_CABLE;
            }
            break;
        }
        case POWER_SHIP_SIG: {
            /* AO_Power says it before it opens the battery path, and what it
             * does next takes time. */
            cause = (uint8_t)KODE_LP_WAKE_BATTERY;
            break;
        }
        case LOWPOWER_REQ_SIG:
        case LOWPOWER_REST_SIG:
        case LOWPOWER_PART_REQ_SIG:
        case CONFIG_REQ_SIG: {
            cause = (uint8_t)KODE_LP_WAKE_REQUEST;
            break;
        }
        default: {
            /* Nothing that needs the clock. */
            break;
        }
    }
    return cause;
}

/**
 * @brief LOWPOWER_PART_WAKE_SIG: count a one-shot heard, and arm the next hop of a later time.
 *
 * The header's line disarmed itself in its interrupt, so it is no longer
 * armed once its event is here.
 *
 * @param me The AO.
 * @param w  The note.
 */
static void part_wake_heard(LowPower * const me, const LowPowerPartWakeEvt *w)
{
    if ( (uint8_t)KODE_LP_WAKE_TIMER == w->cause )
    {
        if ( 0U < me->part_timers )
        {
            me->part_timers -= 1U;
        }
        if ( ((me->part_rest & KODE_LP_PART_CPU) != 0U) && (0U != me->part_due_us)
            && (0U == me->part_timers) && !part_timer_hop(me, khal_time_us()) )
        {
            /* A hop on the way, or the one-shot of an earlier rest, and the
             * next would not arm: the buttons and the minute still wake it. */
            KLOG_W(TAG, "rest: the wake time lost its timer; the P4 sleeps on without it");
        }
    }
}

/**
 * @brief LOWPOWER_MINUTE_SIG with the CPU part at rest: what a sleeping P4 still owes the board.
 *
 * The same one-shot ASLEEP keeps, because the tick is stopped here as well.
 * The charger is read, so a cable shows and wakes the part and a cell at its
 * cut is seen by its owner; off_after is counted from the moment the part
 * went to rest, and sends the board OFF; and an owner's time that has come
 * ends the part, so the owner can do its work.
 *
 * @param me The AO, AWAKE, with the CPU part at rest.
 * @return The QP status.
 */
static QState part_minute(LowPower * const me)
{
    QState status = Q_HANDLED();
    uint64_t const now = khal_time_us();

    minute_heard(me);
    if ( now < me->minute_due_us )
    {
        /* A one-shot of an earlier rest. If it was the last one out, the
         * minute still needs its own. */
        if ( 0U == me->minute_out )
        {
            minute_arm(me, me->minute_due_us - now);
        }
    }
    else if ( off_due(me, me->part_since_us, now) )
    {
        KLOG_N(TAG, "the P4 at rest for %u s: off-after reached",
               (unsigned)me->secs[SETTING_OFF_AFTER]);
        status = round_begin(me, (uint8_t)KODE_LP_OFF, (uint8_t)KODE_LP_CAUSE_OFF_AFTER,
                             (uint8_t)KODE_LOG_SRC_NONE);
    }
    else if ( time_due(me->app_due_us, now) || time_due(me->wake_due_us, now) )
    {
        me->app_due_us  = 0U;
        me->wake_due_us = 0U;
        part_cpu_wake(me, (uint8_t)KODE_LP_WAKE_OWNER);
    }
    else
    {
        power_peek_request();
        minute_schedule(me, now, me->part_since_us);
    }
    return status;
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
        case LOWPOWER_DARK_TMO_SIG: {
            /* An expiry of a dark wake that is over. */
            (void)tmo_fresh(&me->dark_tmo);
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
        case LOWPOWER_DARK_DONE_SIG: {
            /* A dark-wake owner's word, late or with no dark wake to belong to. */
            KLOG_D(TAG, "dark done dropped: no dark wake is running");
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
        case DISPLAY_SLEEP_SIG: {
            /* In a round: a glass the app wakes now is awake when it ends. */
            part_glass_woken(me, Q_EVT_CAST(DisplaySleepEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_PART_REQ_SIG: {
            /* Asleep, going OFF or in a round: the board has every part. */
            part_answer(me, Q_EVT_CAST(LowPowerPartReqEvt)->src, KODE_DENIED,
                        "board: not awake");
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_PART_WAKE_SIG: {
            /* A wake source of a rest that is over. */
            part_wake_heard(me, Q_EVT_CAST(LowPowerPartWakeEvt));
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

    if ( (me->part_rest & KODE_LP_PART_CPU) != 0U )
    {
        /* The tick is stopped: whatever needs it ends the CPU part first, and
         * is then handled as on any awake board. */
        uint8_t const cause = part_wake_cause(me, e);

        if ( (uint8_t)KODE_LP_WAKE_NONE != cause )
        {
            part_cpu_wake(me, cause);
        }
    }
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            idle_restart(me);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_PART_REQ_SIG: {
            part_request(me, Q_EVT_CAST(LowPowerPartReqEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_PART_WAKE_SIG: {
            part_wake_heard(me, Q_EVT_CAST(LowPowerPartWakeEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_MINUTE_SIG: {
            if ( (me->part_rest & KODE_LP_PART_CPU) != 0U )
            {
                status = part_minute(me);
            }
            else
            {
                /* A one-shot of a rest that is over. */
                minute_heard(me);
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_WAKE_AT_SIG: {
            wake_store(me, Q_EVT_CAST(LowPowerWakeAtEvt));
            if ( (me->part_rest & KODE_LP_PART_CPU) != 0U )
            {
                /* The tick is stopped: the one-shot must not be later. */
                wake_fold(me);
            }
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_READY_SIG: {
            if ( part_ready(me, Q_EVT_CAST(LowPowerReadyEvt))
                || part_late_ready(me, Q_EVT_CAST(LowPowerReadyEvt)) )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_SUPER(&LowPower_on);
            }
            break;
        }
        case LOWPOWER_ROUND_TMO_SIG: {
            if ( tmo_fresh(&me->round_tmo) && (0U != me->part_step) )
            {
                if ( me->part_again )
                {
                    part_ask_again(me);
                }
                else
                {
                    part_step_timeout(me);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_SLEEP_SIG: {
            part_glass_answer(me, Q_EVT_CAST(DisplaySleepEvt));
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
            bool const plugged = Q_EVT_CAST(PowerEvt)->vbus && (!me->vbus);

            /* A cable that appears is somebody at the board. */
            if ( plugged )
            {
                me->used = true;
            }
            if ( plugged && me->cable_check )
            {
                /* It came while the P4 slept, and a host on it may have
                 * given the cable port up. */
                khal_console_cable();
            }
            me->cable_check = false;
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
                /* The cable came while the cable port was off the bus. */
                khal_console_cable();
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
 * @brief A dark wake: ASLEEP with the tick and the lock, for the dark-wake owners.
 *
 * A substate of ASLEEP, so the minute, the inputs that wake the board whole
 * and the requests are ASLEEP's own and are handled there.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState LowPower_dark(LowPower * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            dark_enter(me);
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            dark_exit(me);
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_DARK_DONE_SIG: {
            status = dark_done(me, Q_EVT_CAST(LowPowerDarkDoneEvt));
            break;
        }
        case LOWPOWER_STATE_RETRY_SIG: {
            status = dark_retry(me);
            break;
        }
        case LOWPOWER_DARK_TMO_SIG: {
            if ( tmo_fresh(&me->dark_tmo) )
            {
                KLOG_N(TAG, "dark wake: %u ms are up; the dark-wake owners are asked "
                            "to rest", (unsigned)KODE_LOWPOWER_DARK_MAX_MS);
                status = dark_rest(me);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case LOWPOWER_READY_SIG: {
            status = dark_ready(me, Q_EVT_CAST(LowPowerReadyEvt));
            break;
        }
        case LOWPOWER_ROUND_TMO_SIG: {
            if ( tmo_fresh(&me->round_tmo) )
            {
                status = dark_round_timeout(me);
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
    QActive_subscribe(&me->super, DISPLAY_SLEEP_SIG);
    QActive_subscribe(&me->super, POWER_SHIP_SIG);
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
    QTimeEvt_ctorX(&me->dark_tmo.evt, &me->super, LOWPOWER_DARK_TMO_SIG, 0U);
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
    me->dark_tmo.stale  = 0U;
    me->dark_tmo.live   = false;
    me->retry_tmo.stale = 0U;
    me->retry_tmo.live  = false;
    me->retry_on        = false;
    me->used            = true;
    me->rest_on         = false;
    me->request_us      = 0U;
    me->dark_mask       = 0U;
    me->dark_pending    = 0U;
    me->dark            = false;
    me->dark_resting    = false;
    me->dark_closing    = false;
    (void)memset(me->parts, 0, sizeof(me->parts));
    me->joined   = 0U;
    me->sealed   = false;
    me->round    = ROUND_NONE;
    me->waiting  = 0U;
    me->held     = 0U;
    me->state    = (uint8_t)KODE_LP_AWAKE;
    me->target   = (uint8_t)KODE_LP_AWAKE;
    me->cause    = (uint8_t)KODE_LP_CAUSE_BOOT;
    me->round_src = (uint8_t)KODE_LOG_SRC_NONE;
    me->power_ok = false;
    me->vbus     = false;
    me->cable_check = false;
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
    me->part_rest       = 0U;
    me->part_want       = 0U;
    me->part_step       = 0U;
    me->part_who        = KODE_LOWPOWER_WHO_NONE;
    me->part_src        = (uint8_t)KODE_LOG_SRC_NONE;
    me->part_up         = false;
    me->part_again      = false;
    me->part_us         = 0U;
    me->part_wake_touch = false;
    me->part_wake_level = false;
    me->part_pin_armed  = false;
    me->part_wake_pin   = 0U;
    me->part_timers     = 0U;
    me->part_rc         = (uint8_t)KODE_OK;
    me->part_answers    = 0U;
    me->part_gpio       = 0U;
    me->part_after_ms   = 0U;
    me->part_due_us     = 0U;
    me->part_hop_us     = 0U;
    me->part_since_us   = 0U;
    for ( uint8_t slot = 0U; PART_COUNT > slot; slot++ )
    {
        me->part_late_round[slot] = ROUND_NONE;
        me->part_late_who[slot]   = KODE_LOWPOWER_WHO_NONE;
    }
    me->part_done       = 0U;
    me->part_cpu_stage  = 0U;
    me->part_late       = 0U;
    me->part_pin_free   = false;
    me->part_pin_pulled = KHAL_GPIO_PULL_NONE;
    s_part_why[0]  = '\0';
    s_part_said[0] = '\0';
    atomic_store(&s_part_word, 0U);
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

QEvt const *lowpower_part_req_evt(const lowpower_part_req_t *req, uint8_t src)
{
    LowPowerPartReqEvt *e   = NULL;
    QEvt const         *out = NULL;

    if ( NULL != req )
    {
        e = Q_NEW_X(LowPowerPartReqEvt, 0U, LOWPOWER_PART_REQ_SIG);
    }
    if ( NULL == e )
    {
        KLOG_E(TAG, "no event for a request for parts; it was dropped");
    }
    else
    {
        e->parts         = req->parts;
        e->end           = req->end;
        e->wake_touch    = req->wake_touch;
        e->wake_level    = req->wake_level;
        e->wake_pin      = req->wake_pin;
        e->wake_after_ms = req->wake_after_ms;
        e->src           = src;
        out = &e->super;
    }
    return out;
}

void lowpower_part_last(lowpower_part_last_t *out)
{
    if ( NULL != out )
    {
        uint32_t const word = atomic_load(&s_part_word);

        out->answers = (uint16_t)(word >> PART_WORD_ANSWERS_SHIFT);
        out->rc      = (uint8_t)((word >> PART_WORD_RC_SHIFT) & PART_WORD_BYTE_MASK);
        out->parts   = (uint8_t)(word & KODE_LP_PART_ALL);
        out->open    = ((word & PART_WORD_OPEN) != 0U);
    }
}

const char *lowpower_part_why(void)
{
    return s_part_why;
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
    out->parts         = me->part_rest;
    out->part_why      = s_part_said;
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

uint8_t lowpower_join_dark(QActive *ao, const char *name)
{
    LowPower * const me = &l_lowpower;
    uint8_t const id = lowpower_join(ao, name);

    if ( KODE_LOWPOWER_WHO_NONE != id )
    {
        me->dark_mask |= (1U << id);
    }
    return id;
}

void lowpower_dark_done(uint8_t who)
{
    LowPowerDarkDoneEvt *e = Q_NEW_X(LowPowerDarkDoneEvt, 0U, LOWPOWER_DARK_DONE_SIG);

    if ( NULL == e )
    {
        /* The bound of the dark wake brings the board back. */
        KLOG_E(TAG, "out of events; participant %u's dark done was dropped",
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
