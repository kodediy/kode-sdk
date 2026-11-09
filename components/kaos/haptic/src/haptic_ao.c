/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file haptic_ao.c
 * @brief AO_Haptic: the owner of the AW86233 and the LRA behind it.
 */
#include "sdkconfig.h"
#include "kaos.h"
#include "kaos_start.h"
#include "kaos_claim.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "kbsp_owners.h"
#include "kdrv_aw86233.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"
#include "kset.h"
#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.haptic"

/**
 * @brief Consecutive feed ticks that may find the FIFO full before the buzz
 *        is declared dead, in QP ticks.
 *
 * Twenty feeds is 200 ms: the FIFO drains in about five, so a feed that waits
 * this long is stuck, not slow. FF_AFS for one or two feeds is normal.
 */
#define HAPTIC_STALL_TICKS 20U

/**
 * @brief How long the driver stage is given to start up, in milliseconds.
 *
 * Awinic gives 0.4 ms, so one is a ceiling with margin. It is a busy-wait in
 * the step that begins a buzz: the shortest QP timer is a 10 ms tick.
 */
#define HAPTIC_WAKE_MS      1U

/** @brief The bit that tells an "off" segment from an "on" one. */
#define HAPTIC_SEG_ODD_BIT  1U

/** @brief Where the row is in its chain of windows. */
typedef enum {
    ROW_NONE = 0,      /**< No row is running.                              */
    ROW_FLOOR_ARM,     /**< Waiting for the still window to be armed.       */
    ROW_FLOOR_STILL,   /**< The still window is filling.                    */
    ROW_FLOOR_DRAIN,   /**< Waiting for what the still window held.         */
    ROW_GAIN_ARM,      /**< Waiting for this gain's window to be armed.     */
    ROW_GAIN_PLAY,     /**< The motor is being driven into that window.     */
    ROW_GAIN_DRAIN     /**< Waiting for what the driven window held.        */
} row_phase_t;

/** @brief One pattern or hold, while it is being fed. */
typedef struct {
    uint16_t      seg_ms[KODE_HAPTIC_SEGMENTS_MAX]; /**< On, off, on... in ms. */
    uint8_t       n;        /**< Segments in use.                           */
    bool          hold;     /**< On until a stop: total is not an end.      */
    uint32_t      total;    /**< Samples the pattern is made of.            */
    uint32_t      fed;      /**< How many the chip has taken.               */
    uint32_t      phase;    /**< The sine's sample index, across bursts.    */
    uint32_t      stall;    /**< Consecutive ticks the FIFO took nothing.   */
    uint32_t      t_go;     /**< khal_time_ms() when GO was issued.         */
    bool          starved;  /**< The FIFO refused a burst at least once.    */
    uint8_t       gain;     /**< Per cent of full scale, as asked for.      */
    uint8_t       reg;      /**< The PLAYCFG2 byte the chip actually got.   */
    kode_status_t rc;       /**< What the buzz amounted to.                 */
} Play;

/** @brief The production row, while it is running. */
typedef struct {
    bool        active;      /**< A row owns this AO.                       */
    row_phase_t phase;       /**< Which window it is waiting on.            */
    uint8_t     gain_idx;    /**< Into GAINS.                               */
    int32_t     floor_x10;   /**< What the bench carries at f0, 0.1 mg.     */
    uint32_t    best_x10;    /**< The best on/off ratio any gain reached.   */
    uint8_t     best_gain;   /**< The PLAYCFG2 byte that reached it.        */
} Row;

typedef struct {
    QActive  super;
    QTimeEvt tick;        /**< HAPTIC_TICK_SIG: the RTP feed, periodic.     */
    QTimeEvt tail;        /**< HAPTIC_TAIL_SIG: what the chip has left.     */
    QTimeEvt wait;        /**< HAPTIC_WAIT_SIG: a window, or a reply.       */
    QTimeEvt retry;       /**< HAPTIC_RETRY_SIG: look again for the chip.   */
    uint32_t tail_stale;  /**< Expired HAPTIC_TAIL_SIG events still queued. */
    uint32_t wait_stale;  /**< Expired HAPTIC_WAIT_SIG events still queued. */
    bool     tail_live;   /**< The tail is armed and unexpired.             */
    bool     wait_live;   /**< The wait is armed and unexpired.             */
    bool     said;        /**< Absence has been said at warning level.      */
    bool     looked;      /**< The first request has looked for the chip.   */
    bool     row_owed;    /**< The row was asked for during a buzz: it runs
                           *   when the buzz ends.                       */
    uint8_t  scale;       /**< `haptic`: per cent of every gain an app asks
                           *   for; 0 silences them. The row's are its own. */
    Play     play;
    Row      row;
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;      /**< The id lowpower_join() gave.                 */
#endif
} Haptic;

/** @brief The motor's claim: taken by a request's door, freed on entering idle. */
static _Atomic haptic_state_t s_state;

/** @brief True from a stop's post until the AO dispatches it. */
static atomic_bool s_stop_pending;

/** @brief One burst of the waveform. File-scope: an AO stack is not for buffers. */
static int8_t s_pcm[KDRV_AW_BURST];

/**
 * @brief The amplitudes the row buzzes at, weakest first, in per cent.
 *
 * Per cent, as kdrv_aw86233_rtp_begin() takes; the driver clamps at
 * KDRV_AW_GAIN_MAX_PCT and truncates, and the top here is that ceiling. The
 * records carry the byte the driver reports back: 0x3F, 0x7F, 0xCC.
 */
static const uint8_t GAINS[KODE_HAPTIC_TEST_GAINS_N] = { 25U, 50U, 80U };

/** @brief The row's buzz as a pattern of one segment. */
static const uint16_t ROW_BUZZ_MS = (uint16_t)KODE_HAPTIC_TEST_MS;

static Haptic l_haptic;
QActive * const AO_Haptic = &l_haptic.super;

static QState Haptic_initial(Haptic * const me, void const * const par);
static QState Haptic_absent(Haptic * const me, QEvt const * const e);
static QState Haptic_up(Haptic * const me, QEvt const * const e);
static QState Haptic_idle(Haptic * const me, QEvt const * const e);
static QState Haptic_playing(Haptic * const me, QEvt const * const e);
static QState Haptic_row(Haptic * const me, QEvt const * const e);

/* ---- Timers ---------------------------------------------------------------- */

/**
 * @brief Disarm a one-shot and say whether its expiry is still queued.
 *
 * A timer that already fired has an event queued that belongs to nothing, and
 * acting on it would end the window that replaced the one it was timing.
 *
 * @param te   The one-shot.
 * @param live Whether the caller considers a previous arming outstanding.
 * @return true when an expired event for @p te is still queued.
 */
static bool timer_stale(QTimeEvt * const te, bool live)
{
    bool stale = false;

    if ( !QTimeEvt_disarm(te) )
    {
        stale = live;
    }
    return stale;
}

/** @brief Stop the row's one-shot, counting an expiry that is already queued. */
static void wait_stop(Haptic * const me)
{
    if ( timer_stale(&me->wait, me->wait_live) )
    {
        me->wait_stale += 1U;
    }
    me->wait_live = false;
}

/**
 * @brief Start the row's one-shot for @p ms.
 *
 * @param me The AO.
 * @param ms How long to wait.
 */
static void wait_start(Haptic * const me, uint32_t ms)
{
    uint32_t ticks = KQP_TICKS_PER_MS(ms);

    wait_stop(me);
    if ( 0U == ticks )
    {
        /* A window shorter than one tick is still a window, and QP resets on
         * an arming of zero. */
        ticks = 1U;
    }
    QTimeEvt_armX(&me->wait, (QTimeEvtCtr)ticks, 0U);
    me->wait_live = true;
}

/** @brief Stop the tail, counting an expiry that is already queued. */
static void tail_stop(Haptic * const me)
{
    if ( timer_stale(&me->tail, me->tail_live) )
    {
        me->tail_stale += 1U;
    }
    me->tail_live = false;
}

/**
 * @brief Arm the tail for @p ticks.
 *
 * @param me    The AO.
 * @param ticks How long the chip still has to play.
 */
static void tail_start(Haptic * const me, uint32_t ticks)
{
    uint32_t n = ticks;

    tail_stop(me);
    if ( 0U == n )
    {
        n = 1U;
    }
    QTimeEvt_armX(&me->tail, (QTimeEvtCtr)n, 0U);
    me->tail_live = true;
}

/* ---- Answering ------------------------------------------------------------- */

/**
 * @brief Publish the answer to one field request.
 *
 * @param me    The AO.
 * @param q     The request.
 * @param rc    The verdict.
 * @param value What to report.
 */
static void field_publish(Haptic * const me, HapticFieldReqEvt const * const q,
                          kode_status_t rc, int32_t value)
{
    HapticFieldEvt *e = Q_NEW_X(HapticFieldEvt, 0U, HAPTIC_FIELD_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a field answer is lost");
        return;
    }
    e->op    = q->op;
    e->field = q->field;
    e->rc    = (uint8_t)rc;
    e->value = value;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief Read, write or dump one field, and publish what happened.
 *
 * Answered in every state the chip is in, a buzz and the row included: the
 * driver reads registers, which needs neither an idle motor nor an idle
 * owner. A console asking `haptic` mid-buzz is asking what the chip says
 * right now, and mid-buzz is when that is most interesting.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void field_answer(Haptic * const me, HapticFieldReqEvt const * const q)
{
    int32_t       value = 0;
    kode_status_t rc;

    if ( (uint8_t)FIELD_OP_DUMP == q->op )
    {
        kdrv_aw86233_dump();
        rc = KODE_OK;
    }
    else if ( (uint8_t)FIELD_OP_SET == q->op )
    {
        rc = kdrv_aw86233_set((kdrv_aw86233_field_t)q->field, q->value);
        if ( KODE_OK == rc )
        {
            /* Read back rather than echoed: a write the next rtp_begin() will
             * overwrite must report what the chip holds, not what was asked
             * for. And the read-back's own verdict is the reply's: a bus that
             * took the write and then stopped answering is not a success, and
             * `>ok value=0` sends an operator looking in the wrong place. */
            rc = kdrv_aw86233_get((kdrv_aw86233_field_t)q->field, &value);
        }
    }
    else
    {
        rc = kdrv_aw86233_get((kdrv_aw86233_field_t)q->field, &value);
    }
    field_publish(me, q, rc, value);
}

/**
 * @brief Publish how the buzz that just ended went.
 *
 * @param me The AO.
 */
static void done_publish(Haptic * const me)
{
    HapticDoneEvt *e = Q_NEW_X(HapticDoneEvt, 0U, HAPTIC_DONE_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a buzz's outcome is lost");
        return;
    }
    e->rc      = (uint8_t)me->play.rc;
    e->samples = me->play.fed;
    e->starved = me->play.starved;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/* ---- The requests kaos.h publishes ----------------------------------------- */

/**
 * @brief Take the motor for @p to, if it is free.
 * @param to HAPTIC_STATE_PLAYING or HAPTIC_STATE_HOLDING.
 * @return true when this caller took it.
 */
static bool state_claim(haptic_state_t to)
{
    haptic_state_t free_state = HAPTIC_STATE_IDLE;

    return atomic_compare_exchange_strong(&s_state, &free_state, to);
}

/** @brief Give the motor back. */
static void state_free(void)
{
    atomic_store(&s_state, HAPTIC_STATE_IDLE);
}

haptic_state_t haptic_state(void)
{
    return atomic_load(&s_state);
}

/**
 * @brief Post one request to play, the door every kind of buzz goes through.
 *
 * @param seg_ms   The segments, or NULL for a hold.
 * @param n        How many.
 * @param gain_pct Drive strength, per cent of full scale.
 * @param hold     On until a stop.
 * @return KODE_DENIED while busy, KODE_NO_RESOURCES for a dry pool.
 */
static kode_status_t play_post(const uint16_t *seg_ms, uint8_t n,
                               uint8_t gain_pct, bool hold)
{
    HapticPlayEvt *e;
    haptic_state_t claim = HAPTIC_STATE_PLAYING;

    if ( hold )
    {
        claim = HAPTIC_STATE_HOLDING;
    }
    /* Refused here rather than inside the AO, and only for busy: absence is
     * a fact about the board, and this caller is told about it the same way
     * everyone else is - in the log, once. */
    if ( !state_claim(claim) )
    {
        return KODE_DENIED;
    }
    e = Q_NEW_X(HapticPlayEvt, 0U, HAPTIC_PLAY_REQ_SIG);
    if ( NULL == e )
    {
        state_free();
        return KODE_NO_RESOURCES;
    }
    e->n        = n;
    e->gain_pct = gain_pct;
    e->hold     = hold;
    if ( 0U < n )
    {
        (void)memcpy(e->seg_ms, seg_ms, (size_t)n * sizeof(e->seg_ms[0]));
    }
    QACTIVE_POST(AO_Haptic, &e->super, NULL);
    return KODE_OK;
}

kode_status_t haptic_play_request(uint16_t ms, uint8_t gain_pct)
{
    /* A buzz is a pattern of one segment; a zero length stays the old
     * (empty) buzz rather than a refusal. */
    return play_post(&ms, 1U, gain_pct, false);
}

kode_status_t haptic_pattern_request(const uint16_t *seg_ms, uint8_t n,
                                     uint8_t gain_pct)
{
    if ( (NULL == seg_ms) || (0U == n) || ((uint8_t)KODE_HAPTIC_SEGMENTS_MAX < n) )
    {
        return KODE_DENIED;
    }
    for ( uint8_t i = 0U; i < n; ++i )
    {
        if ( 0U == seg_ms[i] )
        {
            return KODE_DENIED;
        }
    }
    return play_post(seg_ms, n, gain_pct, false);
}

kode_status_t haptic_hold_request(uint8_t gain_pct)
{
    return play_post(NULL, 0U, gain_pct, true);
}

void haptic_stop_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(HAPTIC_STOP_REQ_SIG);
    haptic_state_t    holding = HAPTIC_STATE_HOLDING;

    /* A hold that was told to stop is a buzz ending: no longer one to join. */
    (void)atomic_compare_exchange_strong(&s_state, &holding, HAPTIC_STATE_PLAYING);
    /* One stop is enough: a caller looping on this cannot fill the queue. */
    if ( !atomic_exchange(&s_stop_pending, true) )
    {
        QACTIVE_POST(AO_Haptic, &evt, NULL);
    }
}

void haptic_field_request(uint8_t op, uint16_t field, int32_t value)
{
    HapticFieldReqEvt *e = Q_NEW_X(HapticFieldReqEvt, 0U, HAPTIC_FIELD_REQ_SIG);

    if ( NULL == e )
    {
        return;
    }
    e->op    = op;
    e->field = field;
    e->value = value;
    QACTIVE_POST(AO_Haptic, &e->super, NULL);
}

void haptic_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(HAPTIC_TEST_REQ_SIG);

    QACTIVE_POST(AO_Haptic, &evt, NULL);
}

/* ---- Feeding the FIFO ------------------------------------------------------- */

/**
 * @brief Set a pattern or a hold up, without starting it.
 *
 * @param me       The AO.
 * @param seg_ms   On, off, on... in ms; ignored by a hold.
 * @param n        How many segments, at most KODE_HAPTIC_SEGMENTS_MAX.
 * @param hold     On until a stop.
 * @param gain_pct Amplitude, per cent of full scale. The driver clamps it.
 */
static void play_start(Haptic * const me, const uint16_t *seg_ms, uint8_t n,
                       bool hold, uint8_t gain_pct)
{
    me->play.total = 0U;
    me->play.n     = 0U;
    me->play.hold  = hold;
    if ( !hold )
    {
        me->play.n = n;
        for ( uint8_t i = 0U; i < n; ++i )
        {
            me->play.seg_ms[i] = seg_ms[i];
            me->play.total    += (uint32_t)KDRV_AW_SAMPLES((uint32_t)seg_ms[i]);
        }
    }
    me->play.fed     = 0U;
    me->play.phase   = 0U;
    me->play.stall   = 0U;
    me->play.t_go    = 0U;
    me->play.starved = false;
    me->play.gain    = gain_pct;
    me->play.reg     = 0U;
    me->play.rc      = KODE_OK;
}

/** @brief True once a pattern has been fed whole; a hold never is. */
static bool play_complete(Haptic const * const me)
{
    return (!me->play.hold) && (me->play.fed >= me->play.total);
}

/** @brief True for the "on" segments: the even ones. */
static bool seg_is_on(uint8_t seg)
{
    return (seg & HAPTIC_SEG_ODD_BIT) == 0U;
}

/**
 * @brief Fill s_pcm with @p n samples of the stream, from sample @c play.fed.
 * @param me The AO.
 * @param n  How many, at most KDRV_AW_BURST.
 */
static void play_fill(Haptic * const me, size_t n)
{
    uint32_t pos;
    uint32_t seg_start = 0U;
    size_t   done = 0U;
    uint8_t  seg = 0U;

    if ( me->play.hold )
    {
        kdrv_aw86233_sine_fill(s_pcm, n, &me->play.phase);
        return;
    }
    pos = me->play.fed;
    /* Found again per burst: nothing to put back when a burst is refused. */
    while ( seg < me->play.n )
    {
        uint32_t const len = (uint32_t)KDRV_AW_SAMPLES((uint32_t)me->play.seg_ms[seg]);

        if ( (seg_start + len) > pos )
        {
            break;
        }
        seg_start += len;
        seg++;
    }
    while ( (done < n) && (seg < me->play.n) )
    {
        uint32_t const len  = (uint32_t)KDRV_AW_SAMPLES((uint32_t)me->play.seg_ms[seg]);
        uint32_t const left = (seg_start + len) - pos;
        size_t         run  = n - done;

        if ( (size_t)left < run )
        {
            run = (size_t)left;
        }
        if ( seg_is_on(seg) )
        {
            if ( pos == seg_start )
            {
                /* Zero before it: the old phase would open with a click. */
                me->play.phase = 0U;
            }
            kdrv_aw86233_sine_fill(&s_pcm[done], run, &me->play.phase);
        }
        else
        {
            (void)memset(&s_pcm[done], 0, run);
        }
        done      += run;
        pos       += (uint32_t)run;
        seg_start += len;
        seg++;
    }
}

/**
 * @brief Offer the chip up to @p bursts bursts of the waveform.
 *
 * Stops early on back-pressure rather than spinning: an accepted count of zero
 * is FF_AFS, the FIFO still full. That patience is bounded by
 * HAPTIC_STALL_TICKS.
 *
 * @param me     The AO.
 * @param bursts How many to offer.
 * @return false when the buzz cannot go on: the bus refused, or the FIFO has
 *         not taken a sample for HAPTIC_STALL_TICKS. @c play.rc says which.
 */
static bool play_feed(Haptic * const me, unsigned bursts)
{
    unsigned i;
    size_t   taken = 0U;

    for ( i = 0U; i < bursts; ++i )
    {
        uint32_t      keep;
        size_t        n;
        size_t        accepted = 0U;
        kode_status_t rc;

        if ( play_complete(me) )
        {
            break;
        }
        n = (size_t)KDRV_AW_BURST;
        if ( (!me->play.hold) && ((me->play.total - me->play.fed) < (uint32_t)n) )
        {
            n = (size_t)(me->play.total - me->play.fed);
        }

        /* The phase is put back when the burst is not taken. A waveform that
         * skipped the samples the chip refused would join with a step in it,
         * and a step is a click the motor reproduces faithfully. */
        keep = me->play.phase;
        play_fill(me, n);
        rc = kdrv_aw86233_rtp_feed(s_pcm, n, &accepted);
        if ( KODE_OK != rc )
        {
            me->play.phase = keep;
            me->play.rc    = rc;
            KLOG_E(TAG, "the RTP FIFO stopped taking samples mid-buzz");
            return false;
        }
        if ( 0U == accepted )
        {
            me->play.phase = keep;
            /* The one thing the status code cannot carry: the feed met the
             * FIFO's own back-pressure at least once during this buzz. */
            me->play.starved = true;
            break;
        }
        taken        += accepted;
        me->play.fed += (uint32_t)accepted;
    }

    if ( play_complete(me) )
    {
        return true;
    }
    if ( 0U < taken )
    {
        me->play.stall = 0U;
        return true;
    }

    /* Nothing was taken and there is more to feed. One or two ticks of this
     * is the chip being ahead; HAPTIC_STALL_TICKS of it is a FIFO that has
     * stopped draining, and there is no point offering it a burst every ten
     * milliseconds until somebody reboots the board. */
    me->play.stall += 1U;
    if ( HAPTIC_STALL_TICKS > me->play.stall )
    {
        return true;
    }
    me->play.rc = KODE_TIMEOUT;
    uint32_t const stalled_ms = HAPTIC_STALL_TICKS * KODE_HAPTIC_TICK_MS;
    KLOG_E(TAG, "the RTP FIFO has not taken a sample in %u ms; the chip has "
                "stopped playing",
           (unsigned)stalled_ms);
    return false;
}

/**
 * @brief The feed is over. Wait out what the chip STILL HOLDS, not all of it.
 *
 * Feeding a buzz takes about as long as playing it, so what is left is its
 * length minus the time since GO, not the whole kdrv_aw86233_playing_ms().
 *
 * @param me The AO.
 */
static void play_tail(Haptic * const me)
{
    uint32_t const held = kdrv_aw86233_playing_ms((size_t)me->play.fed);
    uint32_t const gone = khal_time_ms() - me->play.t_go;
    uint32_t       left = 0U;

    if ( held > gone )
    {
        left = held - gone;
    }
    (void)QTimeEvt_disarm(&me->tick);
    /* tail_start() floors this at one tick: the chip is never told to stop in
     * the same step that handed it its last sample. */
    tail_start(me, KQP_TICKS_PER_MS(left));
}

/**
 * @brief End a buzz that cannot be fed, through the door a good one uses.
 *
 * Not a silent abandonment and not an assertion: the chip is stopped, the
 * HAPTIC_DONE_SIG carries the failure, the claim is given back and a row
 * waiting on this buzz gets its drain. All of that lives in the tail handler
 * already, so this only has to make the buzz over and arm the tail.
 *
 * @param me The AO.
 */
static void play_fail(Haptic * const me)
{
    /* @c total comes down to what the chip actually took rather than to zero,
     * so nothing feeds it again AND the DONE event still reports the samples
     * that did go in - a count the caller can act on. */
    me->play.total = me->play.fed;
    me->play.hold  = false;
    (void)QTimeEvt_disarm(&me->tick);
    tail_start(me, 1U);
}

/* ---- The production row ------------------------------------------------------
 * The still surface is measured first: one that already carries 235 Hz would
 * pass any board, so the row refuses it. Then the gains, weakest first, and the
 * row stops at the first that clears the bar: the LRA moves or it does not.
 */

/** @brief Which step of the row the current phase belongs to. */
static uint8_t row_step(Haptic const * const me)
{
    uint8_t step = 2U;

    if ( (ROW_FLOOR_ARM == me->row.phase) || (ROW_FLOOR_STILL == me->row.phase)
        || (ROW_FLOOR_DRAIN == me->row.phase) )
    {
        step = 1U;
    }
    return step;
}

/**
 * @brief Publish one record of the row.
 *
 * @param me        The AO.
 * @param step      1-based; 0 is the absent record.
 * @param phase     HAPTIC_TEST_*
 * @param flags     HAPTIC_TF_*
 * @param gain      GAIN: the register value this record is about.
 * @param floor_x10 The still window's energy at the bin.
 * @param on_x10    GAIN: the driven window's.
 * @param ratio_x10 GAIN: one over the other, times ten.
 * @param n         Witness samples the record rests on.
 */
static void row_publish(Haptic * const me, uint8_t step, uint8_t phase,
                        uint8_t flags, uint8_t gain, int32_t floor_x10,
                        int32_t on_x10, uint32_t ratio_x10, uint16_t n)
{
    HapticTestEvt *e = Q_NEW_X(HapticTestEvt, 0U, HAPTIC_TEST_SIG);

    if ( NULL == e )
    {
        /* Bounded, never fatal: a record nobody sees costs the operator a
         * line, and asserting here would cost them the board. */
        return;
    }
    e->step      = step;
    e->phase     = phase;
    e->flags     = flags;
    e->gain      = gain;
    e->floor_x10 = floor_x10;
    e->on_x10    = on_x10;
    e->ratio_x10 = ratio_x10;
    e->n         = n;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/** @brief Give the motor back: the row is over, whichever way it ended. */
static void row_end(Haptic * const me)
{
    wait_stop(me);
    me->row.active = false;
    me->row.phase  = ROW_NONE;
}

/**
 * @brief End the row and publish its verdict.
 *
 * @param me The AO.
 * @param ok Whether the block passed.
 */
static void verdict_publish(Haptic * const me, bool ok)
{
    TestResultEvt *e;

    row_end(me);

    e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the haptic verdict is lost");
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", "haptic");
    e->ok = ok;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief The verdict, logged before it is published.
 *
 * The console prints `result=fail` and nothing else, so the reason has to be
 * on the line above it.
 *
 * @param me  The AO.
 * @param ok  Whether the motor moved.
 * @param why What to say when it did not.
 */
static void row_verdict(Haptic * const me, bool ok, const char *why)
{
    if ( ok )
    {
        KLOG_N(TAG, "test haptic: PASS - gain 0x%02X moved it %u.%ux the floor",
               (unsigned)me->row.best_gain,
               (unsigned)(me->row.best_x10 / 10U),
               (unsigned)(me->row.best_x10 % 10U));
    }
    else
    {
        KLOG_E(TAG, "test haptic: FAIL - %s", why);
    }
    verdict_publish(me, ok);
}

/**
 * @brief The row could not measure. Say which half stopped it, and judge
 *        neither.
 *
 * The verdict is still a failure, because an operator cannot ship a board
 * nothing was proved about - but the record carries the reason, so the
 * station can tell a bench it should move from a board it should bin.
 *
 * @param me     The AO.
 * @param reason HAPTIC_TF_MUTE, HAPTIC_TF_SHORT or HAPTIC_TF_LOUD.
 * @param why    The line an operator reads.
 */
static void row_noverdict(Haptic * const me, uint8_t reason, const char *why)
{
    row_publish(me, row_step(me), (uint8_t)HAPTIC_TEST_NOVERDICT, reason, 0U,
                0, 0, 0U, 0U);
    KLOG_E(TAG, "test haptic: NO VERDICT - %s", why);
    verdict_publish(me, false);
}

/** @brief Ask the witness for the next window, and put a deadline on it. */
static void row_arm(Haptic * const me, row_phase_t phase)
{
    me->row.phase = phase;
    imu_capture_request(&me->super);
    wait_start(me, KODE_HAPTIC_REPLY_MS);
}

/** @brief Ask the witness what the window it is holding amounted to. */
static void row_drain(Haptic * const me, row_phase_t phase)
{
    me->row.phase = phase;
    imu_drain_request(&me->super, (uint16_t)KDRV_AW_F0_HZ);
    wait_start(me, KODE_HAPTIC_REPLY_MS);
}

/**
 * @brief How much louder the driven window was than the still one, times ten.
 *
 * An amplitude ratio, not a power one, and integer throughout: the event, the
 * log line and the console record all carry the same number.
 *
 * @param floor_x10 The still window's energy at the bin.
 * @param on_x10    The driven window's.
 * @return The ratio, times ten.
 */
static uint32_t row_ratio(int32_t floor_x10, int32_t on_x10)
{
    int32_t f = floor_x10;
    int32_t v = on_x10;

    /* A floor of zero is a detector that read nothing, not a board that is
     * infinitely quiet: dividing by one keeps the ratio a number the record
     * can carry. */
    if ( 1 > f )
    {
        f = 1;
    }
    if ( 0 > v )
    {
        v = 0;
    }
    return ((uint32_t)v * 10U) / (uint32_t)f;
}

/**
 * @brief Judge the still window, and open the first gain's.
 *
 * @param me The AO.
 * @param q  What the witness measured.
 */
static void row_floor_judge(Haptic * const me, ImuEnergyEvt const * const q)
{
    KLOG_N(TAG, "test haptic: floor at %u Hz = %d.%d mg, rms %d.%d mg, n=%u",
           (unsigned)KDRV_AW_F0_HZ, (int)(q->bin_x10 / 10),
           (int)(q->bin_x10 % 10), (int)(q->rms_x10 / 10),
           (int)(q->rms_x10 % 10), (unsigned)q->n);

    if ( (int32_t)KODE_HAPTIC_FLOOR_MAX_X10 < q->bin_x10 )
    {
        row_publish(me, 1U, (uint8_t)HAPTIC_TEST_FLOOR,
                    (uint8_t)HAPTIC_TF_LOUD, 0U, q->bin_x10, 0, 0U, q->n);
        row_noverdict(me, (uint8_t)HAPTIC_TF_LOUD,
                      "the bench already vibrates at 235 Hz; move the board "
                      "off whatever it is standing on");
        return;
    }

    me->row.floor_x10 = q->bin_x10;
    row_publish(me, 1U, (uint8_t)HAPTIC_TEST_FLOOR, (uint8_t)HAPTIC_TF_PASS,
                0U, q->bin_x10, 0, 0U, q->n);
    row_arm(me, ROW_GAIN_ARM);
}

/**
 * @brief Judge one driven window, and either finish or try the next gain.
 *
 * @param me The AO.
 * @param q  What the witness measured.
 */
static void row_gain_judge(Haptic * const me, ImuEnergyEvt const * const q)
{
    /* The byte the chip was actually given, not the percentage the row asked
     * for: the driver clamps and truncates, so the two are different numbers
     * and only one of them describes the buzz that was measured. */
    uint8_t const  g     = me->play.reg;
    uint32_t const ratio = row_ratio(me->row.floor_x10, q->bin_x10);
    uint8_t        flags = 0U;

    KLOG_N(TAG, "test haptic: gain 0x%02X: on %d.%d mg, ratio %u.%ux, n=%u",
           (unsigned)g, (int)(q->bin_x10 / 10), (int)(q->bin_x10 % 10),
           (unsigned)(ratio / 10U), (unsigned)(ratio % 10U), (unsigned)q->n);

    if ( (uint32_t)KODE_HAPTIC_TEST_RATIO_X10 <= ratio )
    {
        flags = (uint8_t)HAPTIC_TF_PASS;
    }
    if ( ratio > me->row.best_x10 )
    {
        me->row.best_x10  = ratio;
        me->row.best_gain = g;
    }
    row_publish(me, 2U, (uint8_t)HAPTIC_TEST_GAIN, flags, g,
                me->row.floor_x10, q->bin_x10, ratio, q->n);

    if ( 0U != flags )
    {
        row_verdict(me, true, NULL);
        return;
    }

    me->row.gain_idx += 1U;
    if ( (uint8_t)KODE_HAPTIC_TEST_GAINS_N > me->row.gain_idx )
    {
        row_arm(me, ROW_GAIN_ARM);
        return;
    }
    row_verdict(me, false,
                "the AW86233 answers, the LRA does not move: not fitted, or "
                "stuck");
}

/**
 * @brief A window came back. Judge it, or say why it cannot be judged.
 *
 * @param me The AO.
 * @param q  What the witness measured.
 */
static void row_energy(Haptic * const me, ImuEnergyEvt const * const q)
{
    bool const  still  = (ROW_FLOOR_DRAIN == me->row.phase);
    uint8_t     reason = 0U;
    const char *why    = NULL;

    if ( (kode_status_t)q->rc != KODE_OK )
    {
        reason = (uint8_t)HAPTIC_TF_MUTE;
        why    = "the IMU could not measure the window; there is no witness";
    }
    else if ( (uint16_t)KODE_HAPTIC_MIN_SAMPLES > q->n )
    {
        reason = (uint8_t)HAPTIC_TF_SHORT;
        why    = "the witness window came back too short to measure";
    }
    else
    {
        reason = 0U;
    }

    if ( 0U != reason )
    {
        /* The record that could not be judged still carries what there was of
         * it: an operator chasing a short window needs the count. */
        if ( still )
        {
            KLOG_W(TAG, "test haptic: the still window held %u samples",
                   (unsigned)q->n);
            row_publish(me, 1U, (uint8_t)HAPTIC_TEST_FLOOR, reason, 0U,
                        q->bin_x10, 0, 0U, q->n);
        }
        else
        {
            uint8_t const g = me->play.reg;

            KLOG_W(TAG, "test haptic: gain 0x%02X's window held %u samples",
                   (unsigned)g, (unsigned)q->n);
            row_publish(me, 2U, (uint8_t)HAPTIC_TEST_GAIN, reason, g,
                        me->row.floor_x10, q->bin_x10,
                        row_ratio(me->row.floor_x10, q->bin_x10), q->n);
        }
        row_noverdict(me, reason, why);
        return;
    }

    if ( still )
    {
        row_floor_judge(me, q);
        return;
    }
    row_gain_judge(me, q);
}

/**
 * @brief Start the row: the bench first, and nothing driven yet.
 *
 * @param me The AO.
 * @return false when a request holds the motor; nothing starts.
 */
static bool row_start(Haptic * const me)
{
    bool const took = state_claim(HAPTIC_STATE_PLAYING);

    if ( took )
    {
        (void)memset(&me->row, 0, sizeof(me->row));
        me->row.active = true;
        row_arm(me, ROW_FLOOR_ARM);
    }
    return took;
}

/* ---- The state machine ------------------------------------------------------ */

/** @brief Initial transition: claim the motor driver, bring it up, go idle or absent. */
/* ---- The setting -------------------------------------------------------------- */

/**
 * @brief The stored `haptic`, or full scale when the store has never held it.
 *
 * @param[out] stored Whether the store held it. May be NULL.
 * @return Per cent, 0..KSET_PCT_MAX.
 */
static uint8_t scale_stored(bool *stored)
{
    uint8_t v = (uint8_t)KSET_PCT_MAX;
    bool    in_store = false;

    if ( khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_HAPTIC, &v) == KODE_OK )
    {
        in_store = true;
    }
    else
    {
        v = (uint8_t)KSET_PCT_MAX;
    }
    if ( NULL != stored )
    {
        *stored = in_store;
    }
    return v;
}

/**
 * @brief Answer `config key="haptic"`, setting it first when asked.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void haptic_config(Haptic * const me, KodeConfigReqEvt const * const r)
{
    bool stored = false;
    char word[KODE_CONFIG_VALUE_MAX + 1U];

    if ( strcmp(r->key, KSET_KEY_HAPTIC) != 0 )
    {
        return;   /* another owner's key */
    }
    if ( r->set )
    {
        me->scale = (uint8_t)r->i;
        (void)khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_HAPTIC, me->scale);
        KLOG_N(TAG, "haptic %u%% (stored)", (unsigned)me->scale);
    }
    me->scale = scale_stored(&stored);
    (void)snprintf(word, sizeof(word), "%u", (unsigned)me->scale);
    (void)kode_config_answer(&me->super, r, KODE_OK, stored, word);
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief LOWPOWER_ROUND_SIG: the chip to standby, and answer once.
 *
 * A buzz or the row owns the motor until it ends; the round is refused rather
 * than cutting a buzz short.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void haptic_round(Haptic const * const me, const LowPowerRoundEvt * const r)
{
    if ( haptic_state() != HAPTIC_STATE_IDLE )
    {
        lowpower_ready(me->lp_who, r->round, true, "haptic busy");
    }
    else
    {
        if ( kdrv_aw86233_standby(true) != KODE_OK )
        {
            KLOG_W(TAG, "the chip would not go into standby for the board's sleep");
        }
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief Claim the motor, read the stored scale, and look for the chip.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Haptic_idle, or to Haptic_absent with the retry
 *         armed when no AW86233 answers.
 */
static QState Haptic_initial(Haptic * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_HAPTIC, me, "AO_Haptic");

    /* No subscription for its setting, which the console posts. This AO
     * publishes and answers; it listens to no other owner, because the motor
     * is reached only through it - and the witness it does ask for answers by
     * post, to this AO alone. */
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "haptic");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif

    state_free();
    me->scale = scale_stored(NULL);
    (void)memset(&me->play, 0, sizeof(me->play));
    (void)memset(&me->row,  0, sizeof(me->row));

    if ( kdrv_aw86233_create() != KODE_OK )
    {
        /* Normal on a board whose Antenna PCB is not fitted, and looked at
         * again in a moment for the board where it was only this once. */
        KLOG_W(TAG, "no AW86233 at 0x5A; looking again in %u ms",
               (unsigned)KODE_HAPTIC_RETRY_MS);
        QTimeEvt_armX(&me->retry, KQP_TICKS_PER_MS(KODE_HAPTIC_RETRY_MS), 0U);
        return Q_TRAN(&Haptic_absent);
    }

    KLOG_I(TAG, "up; RTP at %u Hz, %u Hz LRA, gain capped at %u per cent, "
                "resting in standby until something asks for a buzz",
           (unsigned)KDRV_AW_RATE_HZ, (unsigned)KDRV_AW_F0_HZ,
           (unsigned)KDRV_AW_GAIN_MAX_PCT);
    return Q_TRAN(&Haptic_idle);
}

/**
 * @brief Start a buzz that was asked for and has the chip to play it.
 *
 * @param me The AO.
 * @param q  The request.
 * @return The transition to Haptic_playing, or Q_HANDLED() when `haptic` is 0
 *         and it is over before it began.
 */
static QState play_begin(Haptic * const me, HapticPlayEvt const * const q)
{
    QState status;
    uint32_t const gain = ((uint32_t)q->gain_pct * (uint32_t)me->scale)
                          / (uint32_t)KSET_PCT_MAX;

    play_start(me, q->seg_ms, q->n, q->hold, (uint8_t)gain);
    if ( 0U == gain )
    {
        /* `haptic=0` silences it, and whoever asked still hears that it ended
         * - a hold included, which is over before it began. */
        KLOG_D(TAG, "haptic is 0; the buzz is not played");
        state_free();
        done_publish(me);
        status = Q_HANDLED();
    }
    else
    {
        status = Q_TRAN(&Haptic_playing);
    }
    return status;
}

/**
 * @brief No haptic driver found on this board. Everything is answered, and once.
 *
 * It is looked for on the timer Haptic_initial armed and at the first request.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Haptic_absent(Haptic * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case HAPTIC_RETRY_SIG: {
            if ( kdrv_aw86233_create() == KODE_OK )
            {
                KLOG_N(TAG, "the AW86233 answers now; this board buzzes");
                status = Q_TRAN(&Haptic_idle);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case HAPTIC_PLAY_REQ_SIG: {
            if ( !me->looked )
            {
                me->looked = true;
                if ( kdrv_aw86233_create() == KODE_OK )
                {
                    /* The door's claim stands: this buzz is what ends it. */
                    (void)QTimeEvt_disarm(&me->retry);
                    KLOG_N(TAG, "the AW86233 answers now; this board buzzes");
                    status = play_begin(me, Q_EVT_CAST(HapticPlayEvt));
                    if ( Q_RET_HANDLED == status )
                    {
                        status = Q_TRAN(&Haptic_idle);
                    }
                    break;
                }
            }
            /* The door's claim is given back: nothing will ever end it.
             * Not a refusal at the door, because absence is not busy: the
             * caller asked a board that cannot answer, and this is the board
             * saying so. Once at warning level and at debug after, because an
             * app that buzzes on every press must not fill the log with one
             * sentence. */
            if ( !me->said )
            {
                KLOG_W(TAG, "no AW86233 at 0x5A; nothing to play");
                me->said = true;
            }
            else
            {
                KLOG_D(TAG, "no AW86233 at 0x5A; nothing to play");
            }
            state_free();
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_STOP_REQ_SIG: {
            atomic_store(&s_stop_pending, false);
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_FIELD_REQ_SIG: {
            /* Answered rather than dropped: with no chip every field is the
             * same refusal, and the console prints it instead of timing out
             * on silence. */
            field_publish(me, Q_EVT_CAST(HapticFieldReqEvt), KODE_IO_ERROR, 0);
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TEST_REQ_SIG: {
            /* Step 0 rather than a bare failure: the station has to tell a
             * missing Antenna PCB from a motor that will not move, and this
             * record is the only thing that says which it is. */
            KLOG_E(TAG, "test haptic: ABSENT - no AW86233 at 0x5A");
            row_publish(me, 0U, (uint8_t)HAPTIC_TEST_ABSENT, 0U, 0U, 0, 0, 0U,
                        0U);
            verdict_publish(me, false);
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            /* Stored all the same: the setting outlives this board's lack. */
            haptic_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* No chip: nothing to rest. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           false, NULL);
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/**
 * @brief The chip is there. What every state below answers the same way.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Haptic_up(Haptic * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case CONFIG_REQ_SIG: {
            haptic_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_STOP_REQ_SIG: {
            /* Nothing plays in idle or in the row's waits. */
            atomic_store(&s_stop_pending, false);
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_FIELD_REQ_SIG: {
            field_answer(me, Q_EVT_CAST(HapticFieldReqEvt));
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            haptic_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case HAPTIC_WAIT_SIG: {
            /* The counted drop lives where EVERY state can reach it: an expiry
             * queued before a transition (the row into Haptic_playing) arrives
             * in the new state, and a count left unconsumed would swallow the
             * next genuine deadline. */
            if ( 0U < me->wait_stale )
            {
                me->wait_stale -= 1U;
            }
            else
            {
                /* Nothing armed this and nothing counted it: the bookkeeping
                 * is wrong, which is worth a line rather than silence. */
                KLOG_W(TAG, "a reply deadline expired with nothing waiting on "
                            "it");
            }
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TAIL_SIG: {
            /* The same gate, for the buzz's own one-shot. */
            if ( 0U < me->tail_stale )
            {
                me->tail_stale -= 1U;
            }
            else
            {
                KLOG_W(TAG, "a tail expired with no buzz behind it");
            }
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_RETRY_SIG: {
            /* The first request found the chip as this expired. */
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TICK_SIG: {
            /* No counter for this one, and none is needed: the feed tick is
             * periodic and Haptic_playing disarms it on the way out, so a
             * queued one is merely late rather than a generation behind. */
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
 * @brief Nothing playing, nothing being measured.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Haptic_idle(Haptic * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            /* The one place every way back to rest passes: a buzz, a hold or
             * the row, a failure included. The row keeps the claim across its
             * own buzzes, which go from playing back to the row, not here. */
            state_free();
            if ( me->row_owed )
            {
                me->row_owed = false;
                haptic_test_request();
            }
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_PLAY_REQ_SIG: {
            status = play_begin(me, Q_EVT_CAST(HapticPlayEvt));
            break;
        }
        case HAPTIC_TEST_REQ_SIG: {
            if ( row_start(me) )
            {
                status = Q_TRAN(&Haptic_row);
            }
            else
            {
                /* A request got the motor first and its play is still on its
                 * way: the row waits for the buzz to end, the play is not
                 * swallowed. */
                KLOG_I(TAG, "test haptic: a buzz is in flight; the row runs after it");
                me->row_owed = true;
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&Haptic_up);
            break;
        }
    }
    return status;
}

/**
 * @brief A buzz is being fed, and then played out.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Haptic_playing(Haptic * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            /* Out of standby first, and then a millisecond of nothing. The
             * chip rests parked, and a GO issued into a parked driver stage is
             * acknowledged and never heard. See HAPTIC_WAKE_MS for why this is
             * a busy-wait and not a timer. */
            me->play.rc = kdrv_aw86233_standby(false);
            if ( KODE_OK == me->play.rc )
            {
                khal_delay_ms(HAPTIC_WAKE_MS);
                me->play.rc = kdrv_aw86233_rtp_begin(me->play.gain,
                                                     &me->play.reg);
            }
            if ( KODE_OK != me->play.rc )
            {
                KLOG_E(TAG, "the AW86233 would not start playing");
                /* Out through the same door as a buzz that worked, so the
                 * chip is stopped and the caller is told, once. */
                play_fail(me);
                status = Q_HANDLED();
                break;
            }
            /* The clock starts at GO, because what the tail has to wait for is
             * the playback the chip has not reached yet, and feeding is not
             * free of time. */
            me->play.t_go = khal_time_ms();
            /* Two bursts before the first tick can arrive: ten milliseconds
             * in the FIFO is what makes a ten-millisecond feed period safe. */
            if ( !play_feed(me, (unsigned)KODE_HAPTIC_BURSTS_PER_TICK) )
            {
                play_fail(me);
            }
            else if ( play_complete(me) )
            {
                play_tail(me);
            }
            else
            {
                QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(KODE_HAPTIC_TICK_MS),
                              KQP_TICKS_PER_MS(KODE_HAPTIC_TICK_MS));
            }
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            (void)QTimeEvt_disarm(&me->tick);
            tail_stop(me);
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TICK_SIG: {
            if ( !me->tail_live )
            {
                if ( !play_feed(me, (unsigned)KODE_HAPTIC_BURSTS_PER_TICK) )
                {
                    /* The buzz is over whether the motor heard it or not.
                     * Without this the tick would come back in ten
                     * milliseconds and try again, forever: a log line every
                     * tick, no HAPTIC_DONE_SIG, the claim never given
                     * back, and a row stuck in GAIN_PLAY. */
                    play_fail(me);
                }
                else if ( play_complete(me) )
                {
                    play_tail(me);
                }
                else
                {
                    /* More to feed, and the chip is keeping up. */
                }
            }
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TAIL_SIG: {
            if ( 0U < me->tail_stale )
            {
                /* A tail armed by a buzz that has already ended. The live
                 * flag belongs to the arming that REPLACED it and must not be
                 * touched here - clearing it would mark an armed timer dead,
                 * and its own expiry would then be taken as genuine. */
                me->tail_stale -= 1U;
                status = Q_HANDLED();
                break;
            }
            me->tail_live = false;   /* this event IS its expiry */
            (void)kdrv_aw86233_rtp_end();
            /* And back to rest. This is the one site, because every way a buzz
             * can end arrives here: the good one, a FIFO that stopped
             * draining, a bus that refused a burst, and a start that never
             * happened at all - play_fail() arms this timer rather than
             * unwinding on its own. */
            (void)kdrv_aw86233_standby(true);
            done_publish(me);
            if ( me->row.active && (KODE_OK != me->play.rc) )
            {
                /* Nothing was driven into this gain's window: the bus refused a
                 * burst, or the FIFO stopped draining. Measuring it would blame
                 * a motor no current reached, so the cause is named and no
                 * further gain is asked of the witness. */
                row_noverdict(me, (uint8_t)HAPTIC_TF_MUTE,
                              "the chip stopped taking samples; the motor was "
                              "never driven");
                status = Q_TRAN(&Haptic_idle);
            }
            else if ( me->row.active )
            {
                row_drain(me, ROW_GAIN_DRAIN);
                status = Q_TRAN(&Haptic_row);
            }
            else
            {
                status = Q_TRAN(&Haptic_idle);
            }
            break;
        }
        case HAPTIC_STOP_REQ_SIG: {
            atomic_store(&s_stop_pending, false);
            /* Not the row's buzz, and not one already ending. */
            if ( (!me->row.active) && (!me->tail_live) )
            {
                play_fail(me);
            }
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_PLAY_REQ_SIG: {
            /* The flag already refused this at post time; one that got here
             * some other way is dropped rather than interleaved into the
             * waveform already in the FIFO. */
            KLOG_W(TAG, "busy; one buzz at a time");
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TEST_REQ_SIG: {
            KLOG_I(TAG, "test haptic: a buzz is in flight; the row runs after it");
            me->row_owed = true;
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Haptic_up);
            break;
        }
    }
    return status;
}

/**
 * @brief The production row is running. One window at a time.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Haptic_row(Haptic * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case IMU_ARMED_SIG: {
            ImuArmedEvt const * const q = Q_EVT_CAST(ImuArmedEvt);

            if ( (ROW_FLOOR_ARM != me->row.phase)
                && (ROW_GAIN_ARM != me->row.phase) )
            {
                /* A row acts only on the reply it is waiting for, and it does
                 * not touch its deadline on the way past. Stopping the timer
                 * here would leave the window this row IS waiting on with
                 * nothing to time it out, and the AO would sit on the motor
                 * and the claim until the board was reset. */
                KLOG_W(TAG, "a reply for a phase this row is not in; ignored");
                status = Q_HANDLED();
                break;
            }
            wait_stop(me);
            if ( (kode_status_t)q->rc != KODE_OK )
            {
                row_noverdict(me, (uint8_t)HAPTIC_TF_MUTE,
                              "the IMU would not arm a window; there is no "
                              "witness");
                status = Q_TRAN(&Haptic_idle);
                break;
            }
            if ( ROW_FLOOR_ARM == me->row.phase )
            {
                me->row.phase = ROW_FLOOR_STILL;
                wait_start(me, KODE_HAPTIC_STILL_MS);
                status = Q_HANDLED();
            }
            else
            {
                me->row.phase = ROW_GAIN_PLAY;
                play_start(me, &ROW_BUZZ_MS, 1U, false, GAINS[me->row.gain_idx]);
                status = Q_TRAN(&Haptic_playing);
            }
            break;
        }
        case IMU_ENERGY_SIG: {
            if ( (ROW_FLOOR_DRAIN != me->row.phase)
                && (ROW_GAIN_DRAIN != me->row.phase) )
            {
                /* The same rule, and here it is the more expensive one to get
                 * wrong: a window measured before the motor was driven would
                 * be judged against a floor of zero, which row_ratio() floors
                 * at one - so any reading at all would clear the bar and pass
                 * a motor nothing had asked to move. */
                KLOG_W(TAG, "a reply for a phase this row is not in; ignored");
                status = Q_HANDLED();
                break;
            }
            wait_stop(me);
            row_energy(me, Q_EVT_CAST(ImuEnergyEvt));
            if ( me->row.active )
            {
                status = Q_HANDLED();
            }
            else
            {
                status = Q_TRAN(&Haptic_idle);
            }
            break;
        }
        case HAPTIC_WAIT_SIG: {
            if ( 0U < me->wait_stale )
            {
                /* A deadline belonging to a window that has already been
                 * answered. The live flag belongs to the arming that replaced
                 * it, so it is not touched: clearing it here would mark an
                 * armed timer dead, the next reply-before-expiry would not
                 * count its expiry, and that expiry would then be read as a
                 * genuine deadline - a MUTE on a board that answered. */
                me->wait_stale -= 1U;
                status = Q_HANDLED();
                break;
            }
            me->wait_live = false;   /* this event IS its expiry */
            if ( ROW_FLOOR_STILL == me->row.phase )
            {
                /* The still window is full. Nothing was driven into it, and
                 * that is the number everything else is divided by. */
                row_drain(me, ROW_FLOOR_DRAIN);
                status = Q_HANDLED();
                break;
            }
            row_noverdict(me, (uint8_t)HAPTIC_TF_MUTE,
                          "the IMU did not answer in time; there is no witness");
            status = Q_TRAN(&Haptic_idle);
            break;
        }
        case HAPTIC_PLAY_REQ_SIG: {
            KLOG_W(TAG, "test haptic: a buzz was asked for mid-row and dropped");
            status = Q_HANDLED();
            break;
        }
        case HAPTIC_TEST_REQ_SIG: {
            /* Not deferred: the row this would queue behind is the same row,
             * and running it twice tells the operator nothing the first one
             * did not. */
            KLOG_W(TAG, "test haptic: already running; the request is ignored");
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Haptic_up);
            break;
        }
    }
    return status;
}

void Haptic_ctor(void)
{
    Haptic * const me = &l_haptic;

    QActive_ctor(&me->super, Q_STATE_CAST(&Haptic_initial));
    QTimeEvt_ctorX(&me->tick, &me->super, HAPTIC_TICK_SIG, 0U);
    QTimeEvt_ctorX(&me->tail, &me->super, HAPTIC_TAIL_SIG, 0U);
    QTimeEvt_ctorX(&me->wait, &me->super, HAPTIC_WAIT_SIG, 0U);
    QTimeEvt_ctorX(&me->retry, &me->super, HAPTIC_RETRY_SIG, 0U);

    me->tail_stale = 0U;
    me->wait_stale = 0U;
    me->tail_live  = false;
    me->wait_live  = false;
    me->said       = false;
    me->looked     = false;
    (void)memset(&me->play, 0, sizeof(me->play));
    (void)memset(&me->row,  0, sizeof(me->row));
    state_free();
    atomic_store(&s_stop_pending, false);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = KODE_LOWPOWER_WHO_NONE;
#endif
}

AO_STORAGE(haptic, KQP_AO_QUEUE_LEN, KQP_AO_STACK_SMALL);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_haptic_register() ---------------- */

/**
 * @brief `test component=haptic`: the motor moving, with the IMU watching.
 *
 * The row measures a still window, refuses a surface that already carries the
 * motor's frequency, then requires a buzz to clear that floor. Every reading
 * goes out as a log line before the verdict.
 *
 * @return false: AO_Haptic answers later, with TEST_RESULT_SIG.
 */
static bool haptic_row_haptic(void)
{
    haptic_test_request();
    return false;
}
#endif

void kaos_haptic_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t HAPTIC_ROWS[] = {
        { "haptic", AUTOTEST_ID_HAPTIC, haptic_row_haptic, KROW_ASYNC, AUTOTEST_G_NONE, KODE_HAPTIC_TEST_BUDGET_MS,
          NULL, 0U, "Vibration",
          "Motor not working",
          AUTOTEST_ORDER_HAPTIC },
    };

    kaos_require(krows_add(HAPTIC_ROWS, sizeof(HAPTIC_ROWS) / sizeof(HAPTIC_ROWS[0])),
                 "haptic rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&haptic_verbs), "haptic verbs");
#endif
}

void kaos_haptic_start(void)
{
    Haptic_ctor();
    kaos_haptic_register();
    AO_START(AO_Haptic, haptic, KODE_PRIO_HAPTIC);
#if CONFIG_KODEOS_CONSOLE
    /* The settings this AO answers, registered once it has the queue their
     * requests are posted to: an app that does not link it has no such keys. */
    static const kode_config_t HAPTIC_CONFIG[] = {
        { .name = KSET_KEY_HAPTIC, .type = KODE_TYPE_INT,
          .min = 0, .max = (int32_t)KSET_PCT_MAX },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Haptic, KQP_HAPTIC_CMD_MARGIN, HAPTIC_CONFIG),
                 "haptic settings");
#endif
}
