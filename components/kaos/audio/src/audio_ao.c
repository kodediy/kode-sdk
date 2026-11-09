/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file audio_ao.c
 * @brief AO_Audio: the owner of the ES8311, the microphone and the speaker.
 */
#include "sdkconfig.h"
#include "kaos.h"
#include "kaos_ask.h"
#include "kaos_start.h"
#include "kaos_claim.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "kbsp_owners.h"
#include "kdrv_es8311.h"
#include "kes_fields.h"
#include "ktone.h"
#include "khal_nvs.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_log.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "kset.h"
#include "kverbs.h"
#include "krows.h"

/** @brief Milliseconds in a second. */
#define AUDIO_MS_PER_S 1000U

/** @brief Room the guard leaves a job beyond a whole recording: the codec's open, its settle and the file's write, ms. */
#define AUDIO_GUARD_MARGIN_MS 10000U

_Static_assert(KODE_AUDIO_GUARD_MS > ((KDRV_ES8311_BUF_SECONDS * AUDIO_MS_PER_S) + AUDIO_GUARD_MARGIN_MS),
               "the guard outlasts the longest recording");

#define TAG "kaos.audio"

/** @brief The codec worker's stack, in bytes. The jobs call the I2S and I2C
 *         layers and keep their blocks in static buffers, never on the stack.
 *         PSRAM, where an overflow corrupts a neighbour silently: 8 KB at
 *         least, whatever `ps` reads as free. */
#define AUDIO_WORKER_STACK  8192U

/** @brief Jobs that may wait besides the one running: none is ever queued,
 *         because this AO submits only while no job is in flight. */
#define AUDIO_WORKER_DEPTH  1U

/** @brief The codec worker's task name. */
#define AUDIO_WORKER_NAME   "audio_worker"

/* ---- What the AO is doing ------------------------------------------------ */

/** @brief The one activity in flight, if any. */
typedef enum {
    ACT_NONE = 0,
    ACT_TONE,    /**< A tone somebody asked for.                          */
    ACT_PLAY,    /**< The facade is playing a buffer and waiting.         */
    ACT_REC,     /**< The facade is recording into a buffer and waiting.  */
    ACT_REC_FILE,/**< Recording, and then writing it out as a file.        */
    ACT_ROW,     /**< The production row.                                 */
    ACT_CLOSE,   /**< The board is going to sleep; the codec is closing.  */
    ACT_STREAM,  /**< The facade's stream is open: the codec is its until
                  *   the stream is closed and has drained.               */
} act_t;

/**
 * @brief Whoever is asleep on the activity, when it is a facade call.
 *
 * The IrCaptureEvt arrangement: @c gen is written into @c ack immediately
 * before @c done is signalled, so a caller woken by a token it did not earn
 * reads a generation that is not its own and keeps waiting.
 */
typedef struct {
    bool        busy;  /**< Somebody is waiting.                          */
    uint32_t   *rc;    /**< Where the outcome goes, as a kode_status_t.   */
    uint32_t   *got;   /**< Recording: how many frames arrived. May be NULL. */
    khal_sem_t  done;
    uint32_t    gen;
    uint32_t   *ack;
    QActive    *who;   /**< An ask's AO, answered with @c rsp or @c file; else NULL. */
    AudioDoneEvt    *rsp;   /**< A play's, recording's or stream's answer. */
    AudioFileRspEvt *file;  /**< A file recording's answer.               */
    bool        late;  /**< The guard fired on a lent recording: answered
                        *   KODE_TIMEOUT once the worker lets go of it.   */
} Waiter;

/** @brief The production row, while it is running. */
typedef struct {
    uint8_t attempt;                       /**< 1-based; 0 before the first. */
    uint8_t tone;                          /**< Which tone of this attempt.  */
    uint8_t bin[KODE_AUDIO_TEST_TONES];    /**< The pair this attempt drew.  */
    int16_t floor_x10[KTONE_BIN_N];        /**< Step 1's per-bin floor.      */
} Row;

typedef struct {
    QActive  super;
    QTimeEvt guard;        /**< AUDIO_GUARD_SIG: a job that never answered. */
    uint32_t guard_stale;  /**< Expired guard events still queued.          */
    bool     guard_live;   /**< The guard is armed and unexpired.           */
    bool     up;           /**< The codec came up and the worker started.   */
    bool     open;         /**< The device is open.                         */
    bool     opening;      /**< An OPEN job is with the worker, or is owed
                            *   one as soon as the amplifier is there.      */
#if CONFIG_KODEOS_RADIO
    bool     amp_ready;    /**< AO_Radio has said the C5 is running.        */
    bool     amp_wait;     /**< The activity in flight is waiting for it.   */
#endif
    act_t    act;          /**< What this AO is doing.                      */
    kdrv_es8311_req_t job; /**< The job the activity submits once open.     */
    KqpWorker worker;      /**< Runs the codec's jobs, on a PSRAM stack.    */
    kdrv_es8311_req_t wreq;/**< The job with the worker: the worker's from
                            *   the submit to the done event.               */
    uint16_t wseq;         /**< Sequence number of that job.                */
    bool     winflight;    /**< A job is with the worker: set at submit,
                            *   cleared only by its done event.             */
    Waiter   wait;
    Row      row;
    char     path[KODE_PATH_MAX]; /**< ACT_REC_FILE: where it is going.    */
    uint32_t bytes;               /**< ACT_REC_FILE: how long the file is. */
    uint32_t rec_ms;              /**< ACT_REC_FILE: how much sound.       */
    uint32_t *bytes_out;          /**< A waiting task's copy of it.        */
    bool     rec_stop_held;       /**< A stop came while no RECORD was in flight. */
    QActive *stream_who;          /**< ACT_STREAM: the ask's AO that opened it;
                                   *   NULL for the waiting path.           */
#if CONFIG_KODE_LOWPOWER
    uint8_t  lp_who;              /**< The id lowpower_join() gave.         */
    bool     lp_wait;             /**< A round waits for the close to end.  */
    uint32_t lp_round;            /**< That round.                          */
#endif
} Audio;

static Audio l_audio;
QActive * const AO_Audio = &l_audio.super;

/** @brief True from a recording stop's post until the AO dispatches it. */
static atomic_bool s_rec_stop_pending;

/** @brief The settings with a latest-value door, in s_setting order. */
enum {
    SETTING_VOLUME = 0,  /**< KDRV_ES_VOL      */
    SETTING_MIC_DB,      /**< KDRV_ES_MIC_DB   */
    SETTING_MUTE,        /**< KDRV_ES_OUT_MUTE */
    SETTING_COUNT        /**< How many there are. */
};

/** @brief One setting's door: its latest value, and whether a request for it is queued. */
typedef struct {
    atomic_int_least32_t value;    /**< The latest value asked for.           */
    atomic_bool          pending;  /**< A request is queued, or being handled. */
} setting_slot_t;

/** @brief The doors; static storage starts them zero and closed. */
static setting_slot_t s_setting[SETTING_COUNT];

static QState Audio_initial(Audio * const me, void const * const par);
static QState Audio_dead(Audio * const me, QEvt const * const e);
static QState Audio_ready(Audio * const me, QEvt const * const e);

/* ---- Timers -------------------------------------------------------------- */

/** @brief Stop the guard, counting an expiry that is already queued. */
static void audio_guard_stop(Audio * const me)
{
    if ( kqp_timer_stale(&me->guard, me->guard_live) )
    {
        me->guard_stale += 1U;
    }
    me->guard_live = false;
}

/** @brief Start the guard on a job just handed to the worker. */
static void audio_guard_start(Audio * const me)
{
    audio_guard_stop(me);
    QTimeEvt_armX(&me->guard, KQP_TICKS_PER_MS(KODE_AUDIO_GUARD_MS), 0U);
    me->guard_live = true;
}

/* ---- The asks: kode_sys's requests, answered to the asking AO --------------
 * One door per asker and kind: an ask takes it, the answer opens it again
 * just before it is posted, so the asker may ask again from the answer's
 * handler. The codec's own contention is the handler's refusal, answered.
 */

/** @brief The kinds of ask, one door each per asker. */
enum {
    AUDIO_ASK_INFO = 0,  /**< audio_info_ask()         */
    AUDIO_ASK_PLAY,      /**< audio_play_ask()         */
    AUDIO_ASK_REC,       /**< audio_record_ask()       */
    AUDIO_ASK_FILE,      /**< audio_record_file_ask()  */
    AUDIO_ASK_OPEN,      /**< audio_stream_open_ask()  */
    AUDIO_ASK_CLOSE,     /**< audio_stream_close_ask() */
    AUDIO_ASK_KINDS      /**< How many there are.      */
};

/** @brief The doors, one per kind of ask. */
static kaos_ask_door_t s_audio_doors[AUDIO_ASK_KINDS];

/** @brief The priority of the AO whose ask opened the stream; 0 for none or the waiting path. */
static atomic_uint_least8_t s_audio_stream_asker;

/**
 * @brief The door an answer belongs to, by its signal.
 * @param sig The answer's signal.
 * @return AUDIO_ASK_*.
 */
static uint8_t audio_ask_kind(QSignal sig)
{
    uint8_t kind = (uint8_t)AUDIO_ASK_INFO;

    if ( (QSignal)AUDIO_DONE_SIG == sig )
    {
        kind = (uint8_t)AUDIO_ASK_PLAY;
    }
    else if ( (QSignal)AUDIO_REC_RSP_SIG == sig )
    {
        kind = (uint8_t)AUDIO_ASK_REC;
    }
    else if ( (QSignal)AUDIO_FILE_RSP_SIG == sig )
    {
        kind = (uint8_t)AUDIO_ASK_FILE;
    }
    else if ( (QSignal)AUDIO_STREAM_OPEN_RSP_SIG == sig )
    {
        kind = (uint8_t)AUDIO_ASK_OPEN;
    }
    else if ( (QSignal)AUDIO_STREAM_CLOSE_RSP_SIG == sig )
    {
        kind = (uint8_t)AUDIO_ASK_CLOSE;
    }
    else
    {
        /* AUDIO_INFO_RSP_SIG */
    }
    return kind;
}

/**
 * @brief Open the asker's door of the answer's kind, then post the answer.
 * @param who The asking AO.
 * @param rsp The answer, filled in.
 */
static void audio_ask_answer(QActive *who, QEvt const *rsp)
{
    kaos_ask_answer(&s_audio_doors[audio_ask_kind(rsp->sig)], who, rsp, AO_Audio);
}

/**
 * @brief Answer an ask of the play, recording or stream kind.
 * @param who    The asking AO.
 * @param rsp    Its answer.
 * @param rc     The outcome.
 * @param frames How many frames moved.
 */
static void audio_ask_done(QActive *who, AudioDoneEvt *rsp, kode_status_t rc, uint32_t frames)
{
    rsp->rc     = (uint8_t)rc;
    rsp->frames = frames;
    audio_ask_answer(who, &rsp->super);
}

/* ---- Answering whoever is waiting ---------------------------------------- */

/**
 * @brief Wake the facade caller asleep on the current activity.
 *
 * @param me     The AO.
 * @param rc     What to tell them.
 * @param frames How many frames moved; only a recording reads it.
 */
static void wake(Audio * const me, kode_status_t rc, uint32_t frames)
{
    if ( !me->wait.busy )
    {
        return;
    }
    if ( NULL != me->wait.who )
    {
        /* An ask: its answer is an event, posted. A file's is file_done()'s. */
        if ( NULL != me->wait.rsp )
        {
            audio_ask_done(me->wait.who, me->wait.rsp, rc, frames);
        }
        me->wait.who  = NULL;
        me->wait.rsp  = NULL;
        me->wait.file = NULL;
        me->wait.late = false;
        me->wait.busy = false;
        return;
    }
    if ( NULL != me->wait.rc )
    {
        *me->wait.rc = (uint32_t)rc;
    }
    if ( NULL != me->wait.got )
    {
        *me->wait.got = frames;
    }
    /* The generation goes in before the semaphore, never after: that write is
     * the release the caller's read acquires, and it is what orders
     * everything written above it on a board whose two cores need telling. */
    if ( NULL != me->wait.ack )
    {
        *me->wait.ack = me->wait.gen;
    }
    if ( NULL != me->wait.done )
    {
        khal_sem_signal(me->wait.done);
    }
    me->wait.busy = false;
}

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Answer the round that waits for the codec to close, if one does.
 *
 * Ready even when the close failed: the board can still sleep with a codec
 * left on, and refusing for good would keep it awake over a few milliamps.
 *
 * @param me     The AO.
 * @param closed true when the CLOSE job ended well.
 */
static void audio_rest_closed(Audio * const me, bool closed)
{
    if ( me->lp_wait )
    {
        me->lp_wait = false;
        if ( !closed )
        {
            KLOG_W(TAG, "the codec did not close for the board's sleep");
        }
        lowpower_ready(me->lp_who, me->lp_round, false, NULL);
    }
}
#endif

/**
 * @brief End the activity in flight and let the codec start going idle.
 *
 * Every end of an activity passes through here - the samples ran out, the
 * worker failed, the guard fired - and wake() answers whoever holds it. The
 * refusals do not reach here, because a refused request never held the
 * activity; buf_refuse() answers those.
 *
 * @param me The AO.
 * @param rc What to tell a waiting caller, if there is one.
 * @param frames How many frames moved.
 */
static void act_end(Audio * const me, kode_status_t rc, uint32_t frames)
{
    audio_guard_stop(me);
#if CONFIG_KODE_LOWPOWER
    if ( ACT_CLOSE == me->act )
    {
        /* Only a failure ends a close here (its guard); a good one is
         * answered where the worker reports it. */
        audio_rest_closed(me, false);
    }
#endif
    if ( ACT_STREAM == me->act )
    {
        atomic_store(&s_audio_stream_asker, 0U);
        me->stream_who = NULL;
    }
    wake(me, rc, frames);
    me->act           = ACT_NONE;
    me->opening       = false;
    me->rec_stop_held = false;
#if CONFIG_KODEOS_RADIO
    me->amp_wait = false;
#endif
}

/**
 * @brief What to tell an ask whose lent buffer the worker has just let go of.
 * @param me The AO.
 * @param rc What the job answered.
 * @return KODE_TIMEOUT once its guard fired; @p rc otherwise.
 */
static kode_status_t late_rc(Audio const * const me, kode_status_t rc)
{
    kode_status_t end = rc;

    if ( me->wait.late )
    {
        end = KODE_TIMEOUT;
    }
    return end;
}

/* ---- Handing work to the worker ------------------------------------------ */

/* A codec job never touches internal flash (no NVS, no partition): the worker's stack is PSRAM. */
/**
 * @brief The job as the worker runs it: the driver's blocking call.
 *
 * Runs on the worker's task and touches only its argument, which is the AO's
 * @c wreq and stays valid until the done event.
 *
 * @param arg   The AO's @c wreq.
 * @param value Unused; what a job measured is in the driver's result table.
 * @return What the driver answered.
 */
static kode_status_t audio_job_run(void *arg, uint32_t *value)
{
    *value = 0U;
    /* cppcheck-suppress misra-c2012-11.5 ; arg is the request that was submitted: KqpJobFn carries a void * */
    return kdrv_es8311_run((const kdrv_es8311_req_t *)arg);
}

/**
 * @brief Hand @p req to the worker and start its guard.
 *
 * One job at a time: while one is with the worker - one whose guard fired
 * included, because the worker may still be inside it, using @c wreq and
 * whatever the job lent - nothing is handed over.
 *
 * @param me  The AO.
 * @param req The job. Copied into @c wreq.
 * @return KODE_OK when the worker has it; KODE_DENIED when a job is with the
 *         worker or the worker would not take it, and nothing changed.
 */
static kode_status_t submit(Audio * const me, const kdrv_es8311_req_t *req)
{
    uint16_t seq = 0U;

    if ( me->winflight )
    {
        return KODE_DENIED;
    }
    if ( KDRV_ES8311_JOB_RECORD == req->type )
    {
        /* Cleared here, before the worker has the job, so a stop posted
         * right after this step is never lost to a clear at job start. */
        kdrv_es8311_record_reset();
    }
    me->wreq = *req;
    if ( kqp_worker_submit(&me->worker, &audio_job_run, &me->wreq,
                          AUDIO_JOB_SIG, &seq) != KODE_OK )
    {
        return KODE_DENIED;
    }
    me->wseq      = seq;
    me->winflight = true;
    audio_guard_start(me);
    if ( KDRV_ES8311_JOB_RECORD == req->type )
    {
        if ( me->rec_stop_held )
        {
            /* A stop that came before this job existed, applied once it does. */
            kdrv_es8311_record_end();
        }
        me->rec_stop_held = false;
    }
    return KODE_OK;
}

/**
 * @brief Would act_start() take a new activity right now?
 *
 * The same answer act_start() gives, for a request that must build something
 * before it can name its job: the recording to a file asks before it claims
 * the shared PSRAM buffer. Nothing runs between the two.
 *
 * @param me The AO.
 * @return true when nothing holds the codec.
 */
static bool act_free(Audio const * const me)
{
    if ( !me->up )
    {
        return false;
    }
    return (ACT_NONE == me->act);
}

/**
 * @brief Start an activity, opening the codec first when it is closed.
 *
 * @param me  The AO.
 * @param act What this is.
 * @param job The job to run once the codec is open.
 * @return KODE_OK when something was submitted, KODE_DENIED otherwise.
 */
static kode_status_t act_start(Audio * const me, act_t act,
                               const kdrv_es8311_req_t *job)
{
    kdrv_es8311_req_t open = { 0 };
    kode_status_t     rc;

    if ( !act_free(me) )
    {
        return KODE_DENIED;
    }
    me->act = act;
    me->job = *job;

    if ( me->open )
    {
        rc = submit(me, &me->job);
    }
#if CONFIG_KODEOS_RADIO
    else if ( !me->amp_ready )
    {
        /* The amplifier is the C5's IO4: ask AO_Radio, through its door, which
         * is also what links AO_Radio into an app with sound. The OPEN goes to
         * the worker when it answers that the C5 runs with the pin high - at
         * once in the common case, after a held C5's boot, or after the job
         * that raises a pin the last firmware left low. The guard covers the
         * wait like any job. */
        radio_amp_request(true);
        me->opening  = true;
        me->amp_wait = true;
        audio_guard_start(me);
        rc = KODE_OK;
    }
#endif
    else
    {
        open.type   = KDRV_ES8311_JOB_OPEN;
        me->opening = true;
        rc = submit(me, &open);
    }
    if ( KODE_OK != rc )
    {
        me->act     = ACT_NONE;
        me->opening = false;
    }
    return rc;
}

/* ---- The requests kaos.h publishes --------------------------------------- */

QEvt const *audio_tone_evt(uint16_t hz, uint16_t ms, uint8_t amp_pct)
{
    AudioToneEvt *e   = Q_NEW_X(AudioToneEvt, 0U, AUDIO_TONE_REQ_SIG);
    QEvt const   *evt = NULL;

    if ( NULL != e )
    {
        e->hz      = hz;
        e->ms      = ms;
        e->amp_pct = amp_pct;
        evt        = &e->super;
    }
    return evt;
}

void audio_tone_request(uint16_t hz, uint16_t ms, uint8_t amp_pct)
{
    QEvt const *e = audio_tone_evt(hz, ms, amp_pct);

    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a tone was not emitted");
        return;
    }
    QACTIVE_POST(AO_Audio, e, NULL);
}

void audio_field_request(uint8_t op, uint16_t field, int32_t value)
{
    AudioFieldReqEvt *e = Q_NEW_X(AudioFieldReqEvt, 0U, AUDIO_FIELD_REQ_SIG);

    if ( NULL == e )
    {
        return;
    }
    e->op    = op;
    e->field = field;
    e->value = value;
    QACTIVE_POST(AO_Audio, &e->super, NULL);
}

void audio_test_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(AUDIO_TEST_REQ_SIG);
    QACTIVE_POST(AO_Audio, &evt, NULL);
}

void audio_info_request(void *out, khal_sem_t done, uint32_t gen,
                        uint32_t *ack)
{
    AudioInfoReqEvt *e = Q_NEW_X(AudioInfoReqEvt, 0U, AUDIO_INFO_REQ_SIG);

    if ( NULL == e )
    {
        /* A dry pool must not leave a caller asleep on a request that was
         * never made. Answered here, with @p out left as it was. */
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->out  = out;
    e->done = done;
    e->gen  = gen;
    e->ack  = ack;
    e->who  = NULL;
    e->rsp  = NULL;
    QACTIVE_POST(AO_Audio, &e->super, NULL);
}

/**
 * @brief Post one of a waiter's buffer requests, or wake the caller at once.
 *
 * @param sig    The request: a recording, or the stream's open or close.
 * @param buf    The caller's frames.
 * @param frames How many to play, or the most to record.
 * @param got    Where the count goes. May be NULL.
 * @param rc     Where the outcome goes.
 * @param done   The semaphore to signal.
 * @param gen    The caller's tag.
 * @param ack    Where @p gen is written back.
 */
static void buf_request(enum_t sig, int16_t *buf, uint32_t frames,
                        uint32_t *got, uint32_t *rc, khal_sem_t done,
                        uint32_t gen, uint32_t *ack)
{
    AudioBufEvt *e = Q_NEW_X(AudioBufEvt, 0U, sig);

    if ( NULL == e )
    {
        /* A dry pool must not leave a caller asleep on a request that was
         * never made, so it is answered here, refused. */
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    e->buf    = buf;
    e->frames = frames;
    e->got    = got;
    e->rc     = rc;
    e->done   = done;
    e->gen    = gen;
    e->ack    = ack;
    e->who    = NULL;
    e->rsp    = NULL;
    QACTIVE_POST(AO_Audio, &e->super, NULL);
}

void audio_record_file_request(const char *path, uint32_t ms, uint32_t *rc,
                               uint32_t *bytes, khal_sem_t done, uint32_t gen,
                               uint32_t *ack)
{
    AudioFileReqEvt *e = Q_NEW_X(AudioFileReqEvt, 0U, AUDIO_REC_FILE_REQ_SIG);

    if ( NULL == e )
    {
        if ( NULL != rc )
        {
            *rc = (uint32_t)KODE_NO_RESOURCES;
        }
        if ( NULL != ack )
        {
            *ack = gen;
        }
        if ( NULL != done )
        {
            khal_sem_signal(done);
        }
        return;
    }
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->ms    = ms;
    e->rc    = rc;
    e->bytes = bytes;
    e->done  = done;
    e->gen   = gen;
    e->ack   = ack;
    e->who   = NULL;
    e->rsp   = NULL;
    QACTIVE_POST(AO_Audio, &e->super, NULL);
}

void audio_record_request(int16_t *buf, uint32_t frames, uint32_t *got,
                          uint32_t *rc, khal_sem_t done, uint32_t gen,
                          uint32_t *ack)
{
    buf_request(AUDIO_REC_REQ_SIG, buf, frames, got, rc, done, gen, ack);
}

void audio_stream_open_request(uint32_t *rc, khal_sem_t done, uint32_t gen,
                               uint32_t *ack)
{
    buf_request(AUDIO_STREAM_OPEN_REQ_SIG, NULL, 0U, NULL, rc, done, gen, ack);
}

void audio_stream_close_request(uint32_t *rc, khal_sem_t done, uint32_t gen,
                                uint32_t *ack)
{
    buf_request(AUDIO_STREAM_CLOSE_REQ_SIG, NULL, 0U, NULL, rc, done, gen, ack);
}

void audio_rec_stop_request(void)
{
    static const QEvt evt = QEVT_INITIALIZER(AUDIO_REC_STOP_REQ_SIG);

    /* One stop is enough: a caller looping on this cannot fill the queue. */
    if ( !atomic_exchange(&s_rec_stop_pending, true) )
    {
        QACTIVE_POST(AO_Audio, &evt, NULL);
    }
}

kode_status_t audio_info_ask(QActive *who)
{
    kode_status_t    rc  = kaos_ask_take(&s_audio_doors[AUDIO_ASK_INFO], who);
    AudioInfoRspEvt *a;
    AudioInfoReqEvt *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(AudioInfoRspEvt, 0U, AUDIO_INFO_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_audio_doors[AUDIO_ASK_INFO], who, NULL);
    }
    e = Q_NEW_X(AudioInfoReqEvt, 0U, AUDIO_INFO_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_audio_doors[AUDIO_ASK_INFO], who, &a->super);
    }
    e->out  = NULL;
    e->done = NULL;
    e->gen  = 0U;
    e->ack  = NULL;
    e->who  = who;
    e->rsp  = a;
    return kaos_ask_post(&s_audio_doors[AUDIO_ASK_INFO], who, AO_Audio, &e->super, &a->super,
                         KQP_AUDIO_CMD_MARGIN);
}

/**
 * @brief Post one of the buffer asks: a play, a recording, a stream's open or close.
 * @param sig    The request's signal.
 * @param rsig   Its answer's signal.
 * @param who    The asking AO.
 * @param buf    The lent frames; NULL for the stream.
 * @param frames How many.
 * @return As the asks in kaos.h.
 */
static kode_status_t buf_ask(enum_t sig, enum_t rsig, QActive *who, int16_t *buf,
                             uint32_t frames)
{
    uint8_t const kind = audio_ask_kind((QSignal)rsig);
    kode_status_t rc   = kaos_ask_take(&s_audio_doors[kind], who);
    AudioDoneEvt *a;
    AudioBufEvt  *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(AudioDoneEvt, 0U, rsig);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_audio_doors[kind], who, NULL);
    }
    e = Q_NEW_X(AudioBufEvt, 0U, sig);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_audio_doors[kind], who, &a->super);
    }
    e->buf    = buf;
    e->frames = frames;
    e->got    = NULL;
    e->rc     = NULL;
    e->done   = NULL;
    e->gen    = 0U;
    e->ack    = NULL;
    e->who    = who;
    e->rsp    = a;
    return kaos_ask_post(&s_audio_doors[kind], who, AO_Audio, &e->super, &a->super,
                         KQP_AUDIO_CMD_MARGIN);
}

kode_status_t audio_play_ask(QActive *who, const int16_t *buf, uint32_t frames)
{
    /* cppcheck-suppress misra-c2012-11.8 ; a play only reads the frames: AudioBufEvt has one buffer field for both directions */
    return buf_ask(AUDIO_PLAY_REQ_SIG, AUDIO_DONE_SIG, who, (int16_t *)buf, frames);
}

kode_status_t audio_record_ask(QActive *who, int16_t *buf, uint32_t frames)
{
    return buf_ask(AUDIO_REC_REQ_SIG, AUDIO_REC_RSP_SIG, who, buf, frames);
}

kode_status_t audio_stream_open_ask(QActive *who)
{
    return buf_ask(AUDIO_STREAM_OPEN_REQ_SIG, AUDIO_STREAM_OPEN_RSP_SIG, who, NULL, 0U);
}

kode_status_t audio_stream_close_ask(QActive *who)
{
    return buf_ask(AUDIO_STREAM_CLOSE_REQ_SIG, AUDIO_STREAM_CLOSE_RSP_SIG, who, NULL, 0U);
}

bool audio_stream_writer_is(QActive const *who)
{
    bool is = false;

    if ( (NULL != who) && (0U != who->prio) )
    {
        is = (atomic_load(&s_audio_stream_asker) == who->prio);
    }
    return is;
}

kode_status_t audio_record_file_ask(QActive *who, const char *path, uint32_t ms)
{
    kode_status_t    rc  = kaos_ask_take(&s_audio_doors[AUDIO_ASK_FILE], who);
    AudioFileRspEvt *a;
    AudioFileReqEvt *e;

    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(AudioFileRspEvt, 0U, AUDIO_FILE_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_audio_doors[AUDIO_ASK_FILE], who, NULL);
    }
    e = Q_NEW_X(AudioFileReqEvt, 0U, AUDIO_REC_FILE_REQ_SIG);
    if ( NULL == e )
    {
        return kaos_ask_cancel(&s_audio_doors[AUDIO_ASK_FILE], who, &a->super);
    }
    (void)snprintf(e->path, sizeof(e->path), "%s", path);
    e->ms    = ms;
    e->rc    = NULL;
    e->bytes = NULL;
    e->done  = NULL;
    e->gen   = 0U;
    e->ack   = NULL;
    e->who   = who;
    e->rsp   = a;
    return kaos_ask_post(&s_audio_doors[AUDIO_ASK_FILE], who, AO_Audio, &e->super, &a->super,
                         KQP_AUDIO_CMD_MARGIN);
}

/**
 * @brief The setting a field belongs to.
 * @param field A codec field.
 * @return A SETTING_* value, or SETTING_COUNT when the field has no door.
 */
static uint32_t setting_of(uint16_t field)
{
    static const kdrv_es8311_field_t fields[SETTING_COUNT] = {  /* in the enum's order */
        [SETTING_VOLUME] = KDRV_ES_VOL, [SETTING_MIC_DB] = KDRV_ES_MIC_DB, [SETTING_MUTE] = KDRV_ES_OUT_MUTE
    };
    uint32_t found = (uint32_t)SETTING_COUNT;

    for ( uint32_t i = 0U; ((uint32_t)SETTING_COUNT > i) && ((uint32_t)SETTING_COUNT == found); i++ )
    {
        if ( (uint16_t)fields[i] == field )
        {
            found = i;
        }
    }
    return found;
}

bool audio_setting_request(uint16_t field, int32_t value)
{
    const uint32_t idx   = setting_of(field);
    bool           taken = false;

    if ( (uint32_t)SETTING_COUNT > idx )
    {
        atomic_store(&s_setting[idx].value, value);
        taken = true;
        if ( !atomic_exchange(&s_setting[idx].pending, true) )
        {
            AudioSettingReqEvt *e = Q_NEW_X(AudioSettingReqEvt, 0U, AUDIO_SETTING_REQ_SIG);

            if ( NULL == e )
            {
                atomic_store(&s_setting[idx].pending, false);
                taken = false;
            }
            else
            {
                e->field = field;
                QACTIVE_POST(AO_Audio, &e->super, NULL);
            }
        }
    }
    return taken;
}

int32_t audio_setting_take(uint16_t field)
{
    const uint32_t idx   = setting_of(field);
    int32_t        value = 0;

    if ( (uint32_t)SETTING_COUNT > idx )
    {
        /* Opened before the read: a value stored after it queues a request of its own. */
        atomic_store(&s_setting[idx].pending, false);
        value = atomic_load(&s_setting[idx].value);
    }
    return value;
}

void audio_rec_stop_take(void)
{
    atomic_store(&s_rec_stop_pending, false);
}

/* ---- The production row --------------------------------------------------
 * `test component=audio`: the board emits and listens for itself, as the
 * speaker and the microphone couple hard. Step 1 judges the microphone alone;
 * step 2 the pair, so its failure names the speaker OR the microphone (ktone.h).
 * The nonce is two different frequencies in order, one chance in fifty-six,
 * and a steady interferer on one bin fails the second window.
 */

/**
 * @brief Publish one record of the row.
 *
 * @param me    The AO.
 * @param phase AUDIO_TEST_*
 * @param hz    The frequency this record is about.
 * @param level The level at @p hz, tenth-dBFS. MIC: the broadband RMS.
 * @param other PROBE: the loudest of the other seven.
 * @param floor PROBE: step 1's floor at @p hz. MIC: the peak.
 * @param flags AUDIO_TF_*
 */
static void row_publish(Audio * const me, uint8_t phase, uint16_t hz,
                        int16_t level, int16_t other, int16_t floor,
                        uint8_t flags)
{
    AudioTestEvt *e = Q_NEW_X(AudioTestEvt, 0U, AUDIO_TEST_SIG);

    if ( NULL == e )
    {
        /* Bounded, never fatal: a record nobody sees costs the operator a
         * line, and asserting here would cost them the board. */
        return;
    }
    e->phase     = phase;
    e->total     = 2U;
    e->tries     = (uint8_t)KODE_AUDIO_TEST_TRIES;
    e->attempt   = me->row.attempt;
    e->flags     = flags;
    e->hz        = hz;
    e->level_x10 = level;
    e->other_x10 = other;
    e->floor_x10 = floor;

    e->step = 2U;
    if ( (AUDIO_TEST_MIC == phase) || (AUDIO_TEST_BIN == phase) )
    {
        e->step = 1U;
    }
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief The verdict, and the end of the row.
 *
 * Logged before it is published, because the log is what the operator reads:
 * the console prints `result=fail` and nothing else, and the reason has to be
 * on the line above it.
 *
 * @param me  The AO.
 * @param ok  Whether the block passed.
 * @param why What to say when it did not.
 */
static void row_verdict(Audio * const me, bool ok, const char *why)
{
    TestResultEvt *e;

    if ( ok )
    {
        KLOG_N(TAG, "test audio: PASS");
    }
    else
    {
        KLOG_E(TAG, "test audio: FAIL - %s", why);
    }
    act_end(me, KODE_OK, 0U);

    e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the audio verdict is lost");
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", "audio");
    e->ok = ok;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief Emit the attempt's current tone and wait for its window.
 *
 * @param me The AO.
 */
static void row_probe(Audio * const me)
{
    uint8_t const  bin = me->row.bin[me->row.tone];
    uint16_t const hz  = ktone_bin_hz(bin);

    KLOG_N(TAG, "test audio: attempt %u of %u, tone %u of %u at %u Hz",
           (unsigned)me->row.attempt, (unsigned)KODE_AUDIO_TEST_TRIES,
           (unsigned)(me->row.tone + 1U), (unsigned)KODE_AUDIO_TEST_TONES,
           (unsigned)hz);
    row_publish(me, AUDIO_TEST_TONE, hz, 0, 0, 0, 0U);

    me->job.type = KDRV_ES8311_JOB_PROBE;
    me->job.bin  = bin;
    me->job.amp  = (int16_t)KTONE_TONE_AMP_Q15;
    if ( submit(me, &me->job) != KODE_OK )
    {
        row_verdict(me, false, "the codec would not take a tone");
    }
}

/**
 * @brief Draw a fresh pair of frequencies and start an attempt.
 *
 * The two are forced to differ, which is not a rounding detail: a steady
 * interferer on one bin would otherwise pass both windows of an attempt.
 * Drawing the second out of the remaining seven and stepping over the first
 * keeps it uniform without a retry loop.
 *
 * @param me The AO.
 */
static void row_attempt(Audio * const me)
{
    uint8_t first;
    uint8_t second;

    me->row.attempt += 1U;
    if ( (uint8_t)KODE_AUDIO_TEST_TRIES < me->row.attempt )
    {
        row_verdict(me, false,
                    "step 1 passed and no tone came back; the speaker or the "
                    "microphone");
        return;
    }

    first  = (uint8_t)(khal_random() % (uint32_t)KTONE_BIN_N);
    second = (uint8_t)(khal_random() % ((uint32_t)KTONE_BIN_N - 1U));
    if ( second >= first )
    {
        second += 1U;
    }
    me->row.bin[0] = first;
    me->row.bin[1] = second;
    me->row.tone   = 0U;
    row_probe(me);
}

/**
 * @brief Step 1 came back: judge the floor and keep it.
 *
 * @param me The AO.
 */
static void row_floor_done(Audio * const me)
{
    kdrv_es8311_result_t r;
    ktone_floor_t        verdict;
    uint8_t              flags = 0U;
    uint8_t              b;

    kdrv_es8311_result(&r);

    for ( b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
    {
        me->row.floor_x10[b] = r.win.bin_x10[b];
    }

    verdict = ktone_floor_judge(&r.win);
    if ( KTONE_FLOOR_STUCK == verdict )
    {
        flags |= (uint8_t)AUDIO_TF_STUCK;
    }
    if ( KTONE_FLOOR_CLIPPED == verdict )
    {
        flags |= (uint8_t)AUDIO_TF_CLIP;
    }
    if ( KTONE_FLOOR_LOUD == verdict )
    {
        flags |= (uint8_t)AUDIO_TF_LOUD;
    }
    if ( KTONE_FLOOR_OK == verdict )
    {
        flags |= (uint8_t)AUDIO_TF_PASS;
    }

    KLOG_N(TAG, "test audio: floor rms=%d peak=%d clip=%u stuck=%u",
           (int)r.win.rms_x10, (int)r.win.peak_x10,
           (unsigned)r.win.clipped, (unsigned)r.win.stuck);
    row_publish(me, AUDIO_TEST_MIC, 0U, r.win.rms_x10, 0,
                r.win.peak_x10, flags);
    for ( b = 0U; (uint8_t)KTONE_BIN_N > b; ++b )
    {
        row_publish(me, AUDIO_TEST_BIN, ktone_bin_hz(b),
                    r.win.bin_x10[b], 0, 0, 0U);
    }

    if ( KTONE_FLOOR_OK != verdict )
    {
        row_verdict(me, false, ktone_floor_reason(verdict));
        return;
    }
    row_attempt(me);
}

/**
 * @brief One of step 2's windows came back: judge it.
 *
 * @param me The AO.
 */
static void row_probe_done(Audio * const me)
{
    kdrv_es8311_result_t r;
    uint8_t const  bin   = me->row.bin[me->row.tone];
    int16_t const  floor = me->row.floor_x10[bin];
    uint16_t const hz    = ktone_bin_hz(bin);
    int16_t        other;
    bool           ok;
    uint8_t        flags = 0U;

    kdrv_es8311_result(&r);
    other = ktone_other_max(&r.win, bin);
    ok    = ktone_window_ok(&r.win, bin, floor);
    if ( ok )
    {
        flags |= (uint8_t)AUDIO_TF_PASS;
    }

    if ( ok )
    {
        KLOG_N(TAG, "test audio: %u Hz came back at %d, others %d, floor %d",
               (unsigned)hz, (int)r.win.bin_x10[bin], (int)other, (int)floor);
    }
    else
    {
        KLOG_E(TAG, "test audio: %u Hz did NOT come back: %d, others %d, "
                    "floor %d", (unsigned)hz, (int)r.win.bin_x10[bin],
               (int)other, (int)floor);
    }
    row_publish(me, AUDIO_TEST_PROBE, hz, r.win.bin_x10[bin], other, floor,
                flags);

    if ( !ok )
    {
        row_attempt(me);
        return;
    }
    me->row.tone += 1U;
    if ( (uint8_t)KODE_AUDIO_TEST_TONES > me->row.tone )
    {
        row_probe(me);
        return;
    }
    row_verdict(me, true, NULL);
}

/**
 * @brief The row's first step: prove the chip is not measuring itself.
 *
 * With the internal loopback on, the chip routes its own DAC into the ADC and
 * the row would pass with a dead microphone and a dead speaker. One I2C read.
 * Called from both places a row can begin, as the device may already be open.
 *
 * @param me The AO.
 * @return true when the row may go on. A false has already said why and
 *         published the verdict.
 */
static bool row_loopback_ok(Audio * const me)
{
    int32_t loop = 0;

    if ( kdrv_es8311_get(KDRV_ES_DAC2ADC, &loop) != KODE_OK )
    {
        row_verdict(me, false, "the codec stopped answering on I2C");
        return false;
    }
    if ( (int32_t)KDRV_ES8311_DAC2ADC_OFF != loop )
    {
        KLOG_E(TAG, "test audio: register 0x44 reads 0x%02X, not 0x%02X",
               (unsigned)loop, (unsigned)KDRV_ES8311_DAC2ADC_OFF);
        row_publish(me, AUDIO_TEST_MIC, 0U, 0, 0, 0,
                    (uint8_t)AUDIO_TF_LOOP);
        row_verdict(me, false,
                    "the DAC is wired into the ADC; nothing this row measures "
                    "would mean anything");
        return false;
    }
    return true;
}

/**
 * @brief Start the row.
 *
 * @param me The AO.
 */
static void row_start(Audio * const me)
{
    kdrv_es8311_req_t job = { 0 };

    /* Nothing checks for a codec here: this is only ever reached from
     * Audio_ready, and a board without one never leaves Audio_dead. */
    if ( ACT_NONE != me->act )
    {
        KLOG_W(TAG, "test audio: something else is using the codec; ignored");
        return;
    }
    (void)memset(&me->row, 0, sizeof(me->row));

    /* The warm path, which is now the usual one: the device is open, the
     * floor window is about to go straight to the worker, and nothing else
     * would ever read register 0x44. */
    if ( me->open && !row_loopback_ok(me) )
    {
        return;
    }
    job.type = KDRV_ES8311_JOB_FLOOR;
    if ( act_start(me, ACT_ROW, &job) != KODE_OK )
    {
        row_verdict(me, false, "the codec would not start");
    }
}

/* ---- What arrives -------------------------------------------------------- */

/**
 * @brief Say that a busy codec refused something, at the level it deserves.
 *
 * A fire-and-forget request has no waiter, and a play landing while the last
 * one opens the codec is ordinary, so it goes to debug. A refusal somebody IS
 * waiting on (about to be handed KODE_DENIED) stays a warning.
 *
 * @param what What was refused, named for the tail of the line.
 * @param rc   The refused request's outcome slot. May be NULL.
 * @param done Its semaphore. May be NULL.
 * @param ack  Its generation slot. May be NULL.
 */
static void say_busy(const char *what, uint32_t const *rc, khal_sem_t done,
                     uint32_t const *ack, QActive const *who)
{
    if ( (NULL == rc) && (NULL == done) && (NULL == ack) && (NULL == who) )
    {
        KLOG_D(TAG, "the codec is busy; %s is refused", what);
        return;
    }
    KLOG_W(TAG, "the codec is busy; %s is refused", what);
}

/**
 * @brief Say how a recording to a file ended, to everyone who cares.
 *
 * Published as well as delivered, because the two audiences are different:
 * a task is asleep on the waiter, and the console is holding a command open
 * with no idea who answered it.
 *
 * @param me The AO.
 * @param rc How it went.
 */
static void file_done(Audio * const me, kode_status_t rc)
{
    AudioFileRspEvt *e;
    uint32_t const   bytes = me->bytes;
    QActive * const  who   = me->wait.who;
    AudioFileRspEvt * const a = me->wait.file;

    if ( NULL != me->bytes_out )
    {
        *me->bytes_out = bytes;
    }
    me->bytes_out = NULL;
    if ( KODE_OK != rc )
    {
        me->bytes = 0U;
    }
    act_end(me, rc, 0U);

    if ( (NULL != who) && (NULL != a) )
    {
        /* An ask: posted to its AO instead of published. */
        (void)snprintf(a->path, sizeof(a->path), "%s", me->path);
        a->bytes = me->bytes;
        a->ms    = me->rec_ms;
        a->rc    = (uint8_t)rc;
        audio_ask_answer(who, &a->super);
        return;
    }
    e = Q_NEW_X(AudioFileRspEvt, 0U, AUDIO_FILE_RSP_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a recording's outcome is lost");
        return;
    }
    (void)snprintf(e->path, sizeof(e->path), "%s", me->path);
    e->bytes = me->bytes;
    e->ms    = me->rec_ms;
    e->rc    = (uint8_t)rc;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief Answer a recording request that was refused, leaving the AO alone.
 *
 * Answered through the request's own fields and path. Do not touch the state
 * the activity in flight owns (me->wait, me->act, me->path, the counters, the
 * guard). Both audiences are answered: the waiter, in wake()'s order (outcome,
 * length, generation, semaphore, each optional), and the published response.
 *
 * @param me The AO, as the publisher.
 * @param q  The refused request.
 * @param rc Why it was refused.
 */
static void file_refuse(Audio * const me, AudioFileReqEvt const * const q,
                        kode_status_t rc)
{
    AudioFileRspEvt *e;

    if ( NULL != q->who )
    {
        AudioFileRspEvt * const a = q->rsp;

        (void)snprintf(a->path, sizeof(a->path), "%s", q->path);
        a->bytes = 0U;
        a->ms    = 0U;
        a->rc    = (uint8_t)rc;
        audio_ask_answer(q->who, &a->super);
        return;
    }
    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)rc;
    }
    if ( NULL != q->bytes )
    {
        *q->bytes = 0U;
    }
    if ( NULL != q->ack )
    {
        *q->ack = q->gen;
    }
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }

    e = Q_NEW_X(AudioFileRspEvt, 0U, AUDIO_FILE_RSP_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; a refusal's outcome is lost");
        return;
    }
    (void)snprintf(e->path, sizeof(e->path), "%s", q->path);
    e->bytes = 0U;
    e->ms    = 0U;
    e->rc    = (uint8_t)rc;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief The activity in flight cannot go on. End it, saying why.
 *
 * @param me  The AO.
 * @param rc  What to tell a waiting caller.
 * @param why What to say in the log, and in the row's verdict.
 */
static void act_fail(Audio * const me, kode_status_t rc, const char *why)
{
    if ( ACT_ROW == me->act )
    {
        row_verdict(me, false, why);
        return;
    }
    if ( ACT_REC_FILE == me->act )
    {
        KLOG_E(TAG, "%s", why);
        file_done(me, rc);
        return;
    }
    if ( ACT_NONE != me->act )
    {
        KLOG_E(TAG, "%s", why);
    }
    act_end(me, rc, 0U);
}

/**
 * @brief Answer one field request, whatever the codec is doing.
 *
 * Answered in every state, mid-row included: the control interface needs
 * neither the device open nor the worker idle. A SET is answered with the
 * outcome and value of its READ BACK, not of the write.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void field_answer(Audio const * const me, AudioFieldReqEvt const * const q)
{
    AudioFieldEvt *e;
    int32_t        value = 0;
    kode_status_t  rc;

    if ( (uint8_t)FIELD_OP_DUMP == q->op )
    {
        kdrv_es8311_dump();
        rc = KODE_OK;
    }
    else if ( ((uint8_t)FIELD_OP_GET == q->op)
             && ((uint16_t)KDRV_ES_BUSY == q->field) )
    {
        /* The driver runs its jobs for whoever calls it and keeps no such
         * state: whether a job is with the worker is this AO's to say. */
        if ( me->winflight )
        {
            value = 1;
        }
        rc = KODE_OK;
    }
    else if ( (uint8_t)FIELD_OP_SET == q->op )
    {
        rc = kdrv_es8311_set((kdrv_es8311_field_t)q->field, q->value);
        if ( KODE_OK == rc )
        {
            /* Read back rather than echoed: a field that rounds - the
             * microphone gain has six-decibel steps - must report what the
             * chip will actually do, not what was asked for. The read's
             * outcome IS the answer's, which is what every other owner does:
             * a write that landed but could not be read back has no value to
             * report, and `ok value=0` would be a reading nobody took. */
            rc = kdrv_es8311_get((kdrv_es8311_field_t)q->field, &value);
        }
    }
    else
    {
        rc = kdrv_es8311_get((kdrv_es8311_field_t)q->field, &value);
    }

    e = Q_NEW_X(AudioFieldEvt, 0U, AUDIO_FIELD_SIG);
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
 * @brief Write a setting's latest value, answered like any field write.
 * @param me The AO.
 * @param q  The request.
 */
static void setting_answer(Audio const * const me, AudioSettingReqEvt const * const q)
{
    AudioFieldReqEvt req;

    (void)memset(&req, 0, sizeof(req));
    req.op    = (uint8_t)FIELD_OP_SET;
    req.field = q->field;
    req.value = audio_setting_take(q->field);
    field_answer(me, &req);
}

/**
 * @brief Answer one info request, whatever the codec is doing.
 *
 * Answered inside this step: everything it reports is driver state or a
 * constant, so there is nothing to wait for and nothing to reach past the
 * owner for - this AO is the owner, and it is the one filling it in.
 *
 * @param me The AO, which knows whether a job is with its worker.
 * @param q  The request.
 */
static void info_answer(Audio const * const me, AudioInfoReqEvt const * const q)
{
    if ( NULL != q->who )
    {
        AudioInfoRspEvt * const a = q->rsp;
        kdrv_es8311_info_t      info;

        (void)memset(&info, 0, sizeof(info));
        kdrv_es8311_info(&info);
        a->rc           = (uint8_t)KODE_OK;
        a->info.rate    = info.rate;
        a->info.present = info.present;
        a->info.open    = info.open;
        a->info.busy    = me->winflight;
        a->info.bits    = info.bits;
        a->info.volume  = info.volume;
        a->info.mic_db  = info.mic_db;
        a->info.mute    = info.mute;
        audio_ask_answer(q->who, &a->super);
        return;
    }
    if ( NULL != q->out )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the event carries the caller's table untyped (kqp_signals.h cannot name it) */
        kdrv_es8311_info_t * const out = (kdrv_es8311_info_t *)q->out;

        kdrv_es8311_info(out);
        out->busy = me->winflight;
    }
    if ( NULL != q->ack )
    {
        *q->ack = q->gen;
    }
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }
}

/**
 * @brief Answer a request that was refused, without going through me->wait.
 *
 * A refused request never held the activity, so the refusal goes into its own
 * fields, in wake()'s order: outcome, generation, then semaphore. Every field is
 * optional. An ask's refusal is its answer event, posted to the asking AO
 * (AUDIO_DONE_SIG for a play).
 *
 * @param q  The refused request.
 * @param rc Why: KODE_DENIED for a codec that is busy or shut, or the
 *           status of whatever else refused it.
 */
static void buf_refuse(AudioBufEvt const * const q, kode_status_t rc)
{
    if ( NULL != q->who )
    {
        audio_ask_done(q->who, q->rsp, rc, 0U);
        return;
    }
    if ( NULL != q->rc )
    {
        *q->rc = (uint32_t)rc;
    }
    if ( NULL != q->got )
    {
        *q->got = 0U;
    }
    if ( NULL != q->ack )
    {
        *q->ack = q->gen;
    }
    if ( NULL != q->done )
    {
        khal_sem_signal(q->done);
    }
}

/**
 * @brief Take one of the facade's buffer requests, or refuse it now.
 *
 * act_start() is asked FIRST and me->wait is written only once it said yes,
 * so a refused request leaves the sleeping caller's wait alone and is answered
 * through its own event. The worker answers by posting, so the job cannot
 * finish before this step returns.
 *
 * @param me  The AO.
 * @param q   The request.
 * @param job What to hand the worker.
 * @param act ACT_PLAY or ACT_REC.
 */
static void buf_take(Audio * const me, AudioBufEvt const * const q,
                     kdrv_es8311_req_t *job, act_t act)
{
    job->buf    = q->buf;
    job->frames = q->frames;

    if ( act_start(me, act, job) != KODE_OK )
    {
        say_busy("the request", q->rc, q->done, q->ack, q->who);
        buf_refuse(q, KODE_DENIED);
        return;
    }
    me->wait.busy = true;
    me->wait.rc   = q->rc;
    me->wait.got  = q->got;
    me->wait.done = q->done;
    me->wait.gen  = q->gen;
    me->wait.ack  = q->ack;
    me->wait.who  = q->who;
    me->wait.rsp  = q->rsp;
    me->wait.file = NULL;
    me->wait.late = false;
}

/**
 * @brief The stream job is with the worker: tell whoever opened it.
 *
 * A stream has no end to guard until somebody closes it - it plays for as
 * long as its writer likes - so the guard its submit armed is stopped here,
 * and armed again by the close, around the drain that has to answer.
 *
 * @param me The AO.
 */
static void stream_started(Audio * const me)
{
    audio_guard_stop(me);
    if ( NULL != me->stream_who )
    {
        /* Published before the answer, so the opener's first write finds it. */
        atomic_store(&s_audio_stream_asker, me->stream_who->prio);
    }
    wake(me, KODE_OK, 0U);
}

/**
 * @brief One job came back from the worker.
 *
 * The event carries only a sequence number and a status; the job is this AO's
 * own record. An event whose number is not that job's, or with no job in
 * flight, is dropped and the guard is left alone.
 *
 * @param me The AO.
 * @param j  What finished, and how it went.
 */
static void job_done(Audio * const me, KqpWorkerDoneEvt const * const j)
{
    kode_status_t const  rc = (kode_status_t)j->status;
    kdrv_es8311_result_t r;
    kdrv_es8311_job_t    kind;

    if ( (!me->winflight) || (j->seq != me->wseq) )
    {
        KLOG_D(TAG, "worker answer seq %u is not the job's (%u); dropped",
               (unsigned)j->seq, (unsigned)me->wseq);
        return;
    }
    /* Cleared before anything else is submitted: the steps below hand the
     * worker the next job of the activity, and that overwrites @c wreq. */
    kind          = me->wreq.type;
    me->winflight = false;
    audio_guard_stop(me);

    switch ( kind )
    {
        case KDRV_ES8311_JOB_OPEN:
            me->opening = false;
            /* Once open the codec stays open: no idle close, since every close is a pop. */
            me->open    = (KODE_OK == rc);
            if ( !me->open )
            {
                act_fail(me, rc, "the codec would not open");
            }
            else if ( ACT_ROW == me->act )
            {
                /* The cold path into the row: the loopback is its first step
                 * either way, and this is where it lands when the device had
                 * to be opened for it. */
                if ( row_loopback_ok(me)
                    && (submit(me, &me->job) != KODE_OK) )
                {
                    row_verdict(me, false,
                                "the codec would not take the floor window");
                }
            }
            else if ( ACT_NONE != me->act )
            {
                if ( submit(me, &me->job) != KODE_OK )
                {
                    act_fail(me, KODE_DENIED, "the codec would not take the job");
                }
                else if ( ACT_STREAM == me->act )
                {
                    stream_started(me);
                }
                else
                {
                    /* Answered when the job ends, like every other sound. */
                }
            }
            else
            {
                /* Opened for an activity that has gone. Nothing to do: the
                 * device stays open, as it does after every sound. */
            }
            break;

        case KDRV_ES8311_JOB_CLOSE:
            me->open = false;
#if CONFIG_KODEOS_RADIO
            /* Closed, the amplifier would only play the idle input's noise. */
            me->amp_ready = false;
            radio_amp_request(false);
#endif
            if ( ACT_CLOSE == me->act )
            {
                me->act = ACT_NONE;
            }
#if CONFIG_KODE_LOWPOWER
            audio_rest_closed(me, (KODE_OK == rc));
#endif
            break;

        case KDRV_ES8311_JOB_FLOOR:
            if ( ACT_ROW != me->act )
            {
                break;
            }
            if ( KODE_OK != rc )
            {
                row_verdict(me, false, "the codec would not capture");
                break;
            }
            row_floor_done(me);
            break;

        case KDRV_ES8311_JOB_PROBE:
            if ( ACT_ROW != me->act )
            {
                break;
            }
            if ( KODE_OK != rc )
            {
                row_verdict(me, false, "the codec would not emit and capture");
                break;
            }
            row_probe_done(me);
            break;

        case KDRV_ES8311_JOB_TONE:
            act_end(me, rc, 0U);
            break;

        case KDRV_ES8311_JOB_PLAY:
            act_end(me, late_rc(me, rc), 0U);
            break;

        case KDRV_ES8311_JOB_STREAM:
            kdrv_es8311_result(&r);
            act_end(me, rc, r.frames);
            break;

        case KDRV_ES8311_JOB_RECORD:
            kdrv_es8311_result(&r);
            if ( ACT_REC_FILE != me->act )
            {
                act_end(me, late_rc(me, rc), r.frames);
                break;
            }
            if ( KODE_OK != rc )
            {
                file_done(me, rc);
                break;
            }
            {
                uint32_t        bytes = 0U;
                uint8_t * const image = kdrv_es8311_buf_file(r.frames, &bytes);
                if ( NULL == image )
                {
                    file_done(me, KODE_NO_RESOURCES);
                    break;
                }
                me->bytes  = bytes;
                me->rec_ms = r.frames / (KDRV_ES8311_RATE_HZ / 1000U);
                /* The buffer stays put until the write answers, which is
                 * exactly the promise storage_write_all_request() asks for -
                 * and this AO takes one activity at a time, so nothing else
                 * can reach it meanwhile. The guard is re-armed because the
                 * write is now what has to answer. */
                audio_guard_start(me);
                storage_write_all_request(me->path, image, bytes, &me->super);
            }
            break;

        default:
            KLOG_W(TAG, "a job outcome %u is not one of ours", (unsigned)kind);
            break;
    }
}

/* ---- The state machine --------------------------------------------------- */

#if CONFIG_KODE_LOWPOWER
/**
 * @brief Why the codec cannot rest now.
 *
 * @param me The AO.
 * @return The busy reason, or NULL when nothing holds the codec but a close.
 */
static const char *audio_rest_refusal(Audio const * const me)
{
    const char *why = NULL;

    if ( ACT_ROW == me->act )
    {
        why = "audio test";
    }
    else if ( (ACT_REC == me->act) || (ACT_REC_FILE == me->act) )
    {
        why = "audio recording";
    }
    else if ( (ACT_NONE != me->act) && (ACT_CLOSE != me->act) )
    {
        why = "audio playing";
    }
    else if ( (me->winflight) && (ACT_NONE == me->act) )
    {
        why = "audio busy";
    }
    else
    {
        /* Free, or closing already. */
    }
    return why;
}

/**
 * @brief LOWPOWER_ROUND_SIG: hush and close the codec, answer once it is closed.
 *
 * A close already with the worker (an earlier round's) is waited for, not
 * started twice. A closed codec is answered at once.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void audio_round(Audio * const me, const LowPowerRoundEvt * const r)
{
    const char *const why = audio_rest_refusal(me);

    if ( NULL != why )
    {
        lowpower_ready(me->lp_who, r->round, true, why);
    }
    else if ( ACT_CLOSE == me->act )
    {
        me->lp_wait  = true;
        me->lp_round = r->round;
    }
    else if ( !me->open )
    {
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
    else
    {
        kdrv_es8311_req_t job = { 0 };

        kdrv_es8311_hush();
        job.type = KDRV_ES8311_JOB_CLOSE;
        me->act  = ACT_CLOSE;
        if ( submit(me, &job) == KODE_OK )
        {
            me->lp_wait  = true;
            me->lp_round = r->round;
        }
        else
        {
            me->act = ACT_NONE;
            lowpower_ready(me->lp_who, r->round, true, "audio close refused");
        }
    }
}
#endif /* CONFIG_KODE_LOWPOWER */


/** @brief Initial transition: claim the codec, start its worker, apply the boot defaults. */
/* ---- The settings ---------------------------------------------------------- */

/** @brief One stored setting of this AO: its key, the field it drives and
 *         what it is while the store has never held it. */
typedef struct {
    const char          *key;   /**< KSET_KEY_*.                            */
    kdrv_es8311_field_t  field; /**< The driver field it sets.              */
    uint8_t              dflt;  /**< The absent-store value.                */
} AudioSetting;

/** The settings, in the order `config` lists them. */
static const AudioSetting AUDIO_SETTINGS[] = {
    { KSET_KEY_VOL,    KDRV_ES_VOL,      (uint8_t)KODE_AUDIO_VOL_DEFAULT    },
    { KSET_KEY_MIC_DB, KDRV_ES_MIC_DB,   (uint8_t)KODE_AUDIO_MIC_DB_DEFAULT },
    { KSET_KEY_MUTE,   KDRV_ES_OUT_MUTE, 0U                                 },
};

#define AUDIO_SETTINGS_N  (sizeof(AUDIO_SETTINGS) / sizeof(AUDIO_SETTINGS[0]))

/**
 * @brief A setting's value: the stored one, or its default.
 *
 * @param set         The setting.
 * @param[out] stored Whether the store held it. May be NULL.
 * @return The value.
 */
static uint8_t audio_setting_value(const AudioSetting *set, bool *stored)
{
    uint8_t v = set->dflt;
    bool    in_store = false;

    if ( khal_nvs_get_u8(KSET_NAMESPACE, set->key, &v) == KODE_OK )
    {
        in_store = true;
    }
    else
    {
        v = set->dflt;   /* a failed read leaves nothing to trust in v */
    }
    if ( NULL != stored )
    {
        *stored = in_store;
    }
    return v;
}

/**
 * @brief Hand every setting to the driver: the device is closed, and an open
 *        is what pushes them at the chip.
 */
static void audio_settings_push(void)
{
    for ( size_t i = 0U; AUDIO_SETTINGS_N > i; i++ )
    {
        (void)kdrv_es8311_set(AUDIO_SETTINGS[i].field,
                              (int32_t)audio_setting_value(&AUDIO_SETTINGS[i], NULL));
    }
}

/**
 * @brief Answer `config` for `vol`, `mic_db` or `mute`, setting it first when
 *        asked.
 *
 * A set reaches the driver at once, which applies it at the next open; with
 * no codec it is only stored. A microphone gain off the 6 dB grid is refused,
 * not rounded.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void audio_config(Audio * const me, KodeConfigReqEvt const * const r)
{
    const AudioSetting *set = NULL;
    kode_status_t       st  = KODE_OK;
    bool                stored = false;
    char                word[KODE_CONFIG_VALUE_MAX + 1U];

    for ( size_t i = 0U; AUDIO_SETTINGS_N > i; i++ )
    {
        if ( strcmp(r->key, AUDIO_SETTINGS[i].key) == 0 )
        {
            set = &AUDIO_SETTINGS[i];
        }
    }
    if ( NULL == set )
    {
        return;   /* another owner's key */
    }
    if ( r->set )
    {
        uint8_t v = (uint8_t)r->i;

        if ( KDRV_ES_OUT_MUTE == set->field )
        {
            v = 0U;
            if ( r->b )
            {
                v = 1U;
            }
        }
        if ( (KDRV_ES_MIC_DB == set->field)
            && (((int32_t)v % KSET_MIC_DB_STEP) != 0) )
        {
            st = KODE_DENIED;
        }
        else
        {
            (void)kdrv_es8311_set(set->field, (int32_t)v);
            (void)khal_nvs_set_u8(KSET_NAMESPACE, set->key, v);
            KLOG_N(TAG, "%s %u (stored)", set->key, (unsigned)v);
        }
    }
    uint8_t const v = audio_setting_value(set, &stored);
    if ( KDRV_ES_OUT_MUTE == set->field )
    {
        const char *on_off = "off";

        if ( 0U != v )
        {
            on_off = "on";
        }
        (void)snprintf(word, sizeof(word), "%s", on_off);
    }
    else
    {
        (void)snprintf(word, sizeof(word), "%u", (unsigned)v);
    }
    (void)kode_config_answer(&me->super, r, st, stored, word);
}

/**
 * @brief Claim the audio path, subscribe, and bring up the codec and its worker.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Audio_ready, or to Audio_dead without the codec or
 *         the worker.
 */
static QState Audio_initial(Audio * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_AUDIO, me, "AO_Audio");

    /* One subscription, and it is the one moment this AO cannot be asked
     * about directly: POWER_SHIP_REQ_SIG is posted to AO_Power, not
     * published, so the heads-up AO_Power publishes in the step before the
     * write is how every owner learns the board is going. Everything else
     * about the audio path is reached through this AO, not announced at it. */
    QActive_subscribe(&me->super, POWER_SHIP_SIG);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "audio");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
    }
#endif
#if CONFIG_KODEOS_RADIO
    /* And AO_Radio's word that the C5 - and with it the amplifier - is up. */
    QActive_subscribe(&me->super, C5_RUNNING_SIG);
    /* The C5's boot enables the amplifier, and with the codec closed it plays
     * the noise of an idle input: off until a sound asks for it. */
    radio_amp_request(false);
#endif

    if ( kdrv_es8311_init() != KODE_OK )
    {
        /* Normal on a board whose codec is not fitted. Said once, and every
         * later request is refused rather than retried. */
        KLOG_W(TAG, "no codec on this board; the audio path stays down");
        return Q_TRAN(&Audio_dead);
    }
    if ( kqp_worker_start(&me->worker, &me->super, AUDIO_WORKER_NAME,
                         AUDIO_WORKER_STACK, KODE_PRIO_AUDIO_WORKER,
                         AUDIO_WORKER_DEPTH, true) != KODE_OK )
    {
        KLOG_E(TAG, "no worker; the codec is there and cannot be driven");
        return Q_TRAN(&Audio_dead);
    }
    me->up = true;

    audio_settings_push();

    KLOG_I(TAG, "up; volume %u, microphone %u dB, mute %u, closed until "
                "somebody asks",
           (unsigned)audio_setting_value(&AUDIO_SETTINGS[0], NULL),
           (unsigned)audio_setting_value(&AUDIO_SETTINGS[1], NULL),
           (unsigned)audio_setting_value(&AUDIO_SETTINGS[2], NULL));
    return Q_TRAN(&Audio_ready);
}

/**
 * @brief No codec on this board. Everything is refused, and said once.
 *
 * A state rather than a flag, the AO_IoExp arrangement: a board without the
 * part is a board where every one of these answers is the same, and a state
 * says that once instead of every handler saying it again.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Audio_dead(Audio * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case AUDIO_TEST_REQ_SIG: {
            row_verdict(me, false, "no codec on this board");
            status = Q_HANDLED();
            break;
        }
        case AUDIO_FIELD_REQ_SIG: {
            /* Answered rather than dropped: the driver refuses every field
             * with KODE_DENIED, and the console prints that instead of
             * timing out on silence. */
            field_answer(me, Q_EVT_CAST(AudioFieldReqEvt));
            status = Q_HANDLED();
            break;
        }
        case AUDIO_SETTING_REQ_SIG: {
            /* Taken all the same, so the door opens again. */
            setting_answer(me, Q_EVT_CAST(AudioSettingReqEvt));
            status = Q_HANDLED();
            break;
        }
        case AUDIO_REC_FILE_REQ_SIG: {
            /* The same refusal a busy codec gives, and for the same reason:
             * a request that is never taken leaves no wait record behind it.
             * There is nothing in flight here to protect, but one refusal
             * mechanism is better than two that have to agree. */
            file_refuse(me, Q_EVT_CAST(AudioFileReqEvt), KODE_DENIED);
            status = Q_HANDLED();
            break;
        }
        case AUDIO_PLAY_REQ_SIG:
        case AUDIO_REC_REQ_SIG:
        case AUDIO_STREAM_OPEN_REQ_SIG:
        case AUDIO_STREAM_CLOSE_REQ_SIG: {
            /* The same refusal buf_take() gives a busy codec, and for the
             * same reason: a request that is never taken leaves no wait
             * record behind it. A play is answered here too, so a board
             * with no codec answers an asker waiting on the end of a sound
             * at once rather than making it sit out its own cap. */
            buf_refuse(Q_EVT_CAST(AudioBufEvt), KODE_DENIED);
            status = Q_HANDLED();
            break;
        }
        case AUDIO_INFO_REQ_SIG: {
            /* Answered here too, and honestly: present reads false, which is
             * what a caller asked in order to find out. */
            info_answer(me, Q_EVT_CAST(AudioInfoReqEvt));
            status = Q_HANDLED();
            break;
        }
        case AUDIO_TONE_REQ_SIG: {
            status = Q_HANDLED();
            break;
        }
        case AUDIO_REC_STOP_REQ_SIG: {
            /* Nothing records here; the flag must still come down. */
            audio_rec_stop_take();
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            /* Stored all the same: the setting outlives this board's lack. */
            audio_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* No codec: nothing to rest. */
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
 * @brief The codec is there. One activity at a time, opened on demand.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Audio_ready(Audio * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case AUDIO_TONE_REQ_SIG: {
            AudioToneEvt const * const q = Q_EVT_CAST(AudioToneEvt);
            kdrv_es8311_req_t          job = { 0 };
            int32_t                    amp;

            amp = ((int32_t)q->amp_pct * 32767) / 100;
            job.type = KDRV_ES8311_JOB_TONE;
            job.bin  = ktone_bin_nearest(q->hz);
            job.ms   = q->ms;
            job.amp  = (int16_t)amp;
            if ( act_start(me, ACT_TONE, &job) != KODE_OK )
            {
                /* A warning although a tone has no waiter: nothing posts a
                 * tone per press, so whoever asked is owed the reason. */
                KLOG_W(TAG, "the codec is busy; the tone is refused");
            }
            status = Q_HANDLED();
            break;
        }

        case AUDIO_FIELD_REQ_SIG: {
            field_answer(me, Q_EVT_CAST(AudioFieldReqEvt));
            status = Q_HANDLED();
            break;
        }

        case AUDIO_SETTING_REQ_SIG: {
            setting_answer(me, Q_EVT_CAST(AudioSettingReqEvt));
            status = Q_HANDLED();
            break;
        }

        case AUDIO_INFO_REQ_SIG: {
            info_answer(me, Q_EVT_CAST(AudioInfoReqEvt));
            status = Q_HANDLED();
            break;
        }

        case CONFIG_REQ_SIG: {
            audio_config(me, Q_EVT_CAST(KodeConfigReqEvt));
            status = Q_HANDLED();
            break;
        }

        case AUDIO_TEST_REQ_SIG: {
            row_start(me);
            status = Q_HANDLED();
            break;
        }

        case AUDIO_PLAY_REQ_SIG: {
            kdrv_es8311_req_t job = { 0 };
            job.type = KDRV_ES8311_JOB_PLAY;
            buf_take(me, Q_EVT_CAST(AudioBufEvt), &job, ACT_PLAY);
            status = Q_HANDLED();
            break;
        }

        case AUDIO_REC_REQ_SIG: {
            kdrv_es8311_req_t job = { 0 };
            job.type = KDRV_ES8311_JOB_RECORD;
            buf_take(me, Q_EVT_CAST(AudioBufEvt), &job, ACT_REC);
            status = Q_HANDLED();
            break;
        }

        case AUDIO_REC_STOP_REQ_SIG: {
            audio_rec_stop_take();
            if ( ACT_REC == me->act )
            {
                if ( me->winflight && (KDRV_ES8311_JOB_RECORD == me->wreq.type) )
                {
                    kdrv_es8311_record_end();
                }
                else
                {
                    /* No RECORD in flight yet (the codec is opening, or the
                     * next window is not submitted): applied at its submit. */
                    me->rec_stop_held = true;
                }
            }
            status = Q_HANDLED();
            break;
        }

        case AUDIO_STREAM_OPEN_REQ_SIG: {
            AudioBufEvt const * const q   = Q_EVT_CAST(AudioBufEvt);
            kdrv_es8311_req_t         job = { 0 };

            /* The ring is emptied and opened only once the codec is known to
             * be free, so a refused open leaves the stream of whoever holds
             * the codec exactly as it was. */
            if ( !act_free(me) )
            {
                say_busy("the stream", q->rc, q->done, q->ack, q->who);
                buf_refuse(q, KODE_DENIED);
                status = Q_HANDLED();
                break;
            }
            kode_status_t const ready = kdrv_es8311_stream_reset();
            if ( KODE_OK != ready )
            {
                buf_refuse(q, ready);
                status = Q_HANDLED();
                break;
            }
            job.type = KDRV_ES8311_JOB_STREAM;
            buf_take(me, q, &job, ACT_STREAM);
            me->stream_who = q->who;
            if ( ACT_STREAM != me->act )
            {
                /* Refused by the worker after all: shut the ring again, or a
                 * writer would fill it for a job that never started. */
                kdrv_es8311_stream_end();
            }
            else if ( !me->opening )
            {
                stream_started(me);
            }
            else
            {
                /* The codec is opening first; job_done() starts the stream
                 * and answers once it has. */
            }
            status = Q_HANDLED();
            break;
        }

        case AUDIO_STREAM_CLOSE_REQ_SIG: {
            AudioBufEvt const * const q = Q_EVT_CAST(AudioBufEvt);

            if ( (ACT_STREAM != me->act) || (q->who != me->stream_who) )
            {
                /* No stream to close: it was never opened, the job has
                 * already ended on its own, or another asker holds it. */
                buf_refuse(q, KODE_DENIED);
                status = Q_HANDLED();
                break;
            }
            if ( me->wait.busy || me->opening )
            {
                /* The open is still owed its answer: the codec is opening,
                 * or the stream's job has not started. The open keeps its
                 * wait record and answers once; the close is refused. */
                buf_refuse(q, KODE_DENIED);
                status = Q_HANDLED();
                break;
            }
            /* The job plays what is left in the ring, drains and answers with
             * the end of the stream's job, which wakes this caller. The guard
             * is armed now because the drain is what has to answer. */
            kdrv_es8311_stream_end();
            me->wait.busy = true;
            me->wait.rc   = q->rc;
            me->wait.got  = q->got;
            me->wait.done = q->done;
            me->wait.gen  = q->gen;
            me->wait.ack  = q->ack;
            me->wait.who  = q->who;
            me->wait.rsp  = q->rsp;
            me->wait.file = NULL;
            me->wait.late = false;
            audio_guard_start(me);
            status = Q_HANDLED();
            break;
        }

        case AUDIO_JOB_SIG: {
            job_done(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            status = Q_HANDLED();
            break;
        }

        case STORAGE_WRITE_RSP_SIG: {
            StorageWriteRspEvt const * const w =
                Q_EVT_CAST(StorageWriteRspEvt);
            if ( ACT_REC_FILE == me->act )
            {
                audio_guard_stop(me);
                if ( (uint8_t)KODE_OK == w->status )
                {
                    KLOG_N(TAG, "recorded %u ms into %s, %u bytes",
                           (unsigned)me->rec_ms, me->path,
                           (unsigned)w->written);
                }
                else
                {
                    KLOG_E(TAG, "%s would not take the recording", me->path);
                }
                file_done(me, (kode_status_t)w->status);
            }
            status = Q_HANDLED();
            break;
        }

        case AUDIO_REC_FILE_REQ_SIG: {
            AudioFileReqEvt const * const q = Q_EVT_CAST(AudioFileReqEvt);
            kdrv_es8311_req_t             job = { 0 };
            uint32_t                      cap = 0U;
            int16_t                      *buf;
            uint32_t                      want;

            /* buf_take()'s order: ask whether the codec is free BEFORE writing
             * anything, so a refused recording leaves the activity in flight
             * alone. Asked here, not through act_start(), because the job cannot
             * be named before the shared PSRAM buffer is claimed. */
            if ( !act_free(me) )
            {
                say_busy("the recording", q->rc, q->done, q->ack, q->who);
                file_refuse(me, q, KODE_DENIED);
                status = Q_HANDLED();
                break;
            }

            buf = kdrv_es8311_buf(&cap);
            if ( NULL == buf )
            {
                KLOG_E(TAG, "no PSRAM for a recording");
                file_refuse(me, q, KODE_NO_RESOURCES);
                status = Q_HANDLED();
                break;
            }

            want = q->ms * (KDRV_ES8311_RATE_HZ / 1000U);
            if ( want > cap )
            {
                want = cap;
            }
            job.type   = KDRV_ES8311_JOB_RECORD;
            job.buf    = buf;
            job.frames = want;
            if ( act_start(me, ACT_REC_FILE, &job) != KODE_OK )
            {
                /* act_free() said yes in this same step and nothing runs
                 * between them, so this is the worker turning the job down
                 * rather than a busy codec. */
                KLOG_E(TAG, "the worker would not take the recording");
                file_refuse(me, q, KODE_DENIED);
                status = Q_HANDLED();
                break;
            }

            me->wait.busy  = true;
            me->wait.rc    = q->rc;
            me->wait.got   = NULL;
            me->wait.done  = q->done;
            me->wait.gen   = q->gen;
            me->wait.ack   = q->ack;
            me->wait.who   = q->who;
            me->wait.rsp   = NULL;
            me->wait.file  = q->rsp;
            me->wait.late  = false;
            me->bytes_out  = q->bytes;
            me->bytes      = 0U;
            me->rec_ms     = 0U;
            (void)snprintf(me->path, sizeof(me->path), "%s", q->path);
            status = Q_HANDLED();
            break;
        }

#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            audio_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
#endif

        case POWER_SHIP_SIG: {
            /* The board is about to open the BATFET. The hush comes FIRST and
             * is synchronous (one register write on this thread): on battery
             * the rails go while the worker waits out a ramp. The close is
             * best effort, for when the board stays up on USB. The device is
             * left closed and the hold on, the resting state. */
            if ( me->open )
            {
                kdrv_es8311_hush();
                if ( act_free(me) )
                {
                    kdrv_es8311_req_t job = { 0 };
                    job.type = KDRV_ES8311_JOB_CLOSE;
                    me->act  = ACT_CLOSE;
                    if ( submit(me, &job) != KODE_OK )
                    {
                        me->act = ACT_NONE;
                    }
                }
                else
                {
                    /* Something is with the worker. It is muted, which is
                     * what mattered, and nothing is going to outlive the
                     * write anyway. */
                    KLOG_I(TAG, "going to sleep mid-sound; the codec is muted "
                                "and stays open");
                }
            }
            status = Q_HANDLED();
            break;
        }

#if CONFIG_KODEOS_RADIO
        case C5_RUNNING_SIG: {
            me->amp_ready = true;
            if ( me->amp_wait )
            {
                kdrv_es8311_req_t open = { 0 };

                me->amp_wait = false;
                open.type    = KDRV_ES8311_JOB_OPEN;
                if ( submit(me, &open) != KODE_OK )
                {
                    me->opening = false;
                    act_fail(me, KODE_DENIED,
                             "the codec would not take the open");
                }
            }
            status = Q_HANDLED();
            break;
        }
#endif

        case AUDIO_GUARD_SIG: {
            if ( 0U < me->guard_stale )
            {
                /* The guard of a job that has already answered. The live flag
                 * belongs to the guard armed for the job that REPLACED it and
                 * must not be touched here: clearing it would mark an armed
                 * guard dead, audio_guard_stop() would stop counting, and the next
                 * stale expiry would be taken as genuine and time out a
                 * healthy job. */
                me->guard_stale -= 1U;
                status = Q_HANDLED();
                break;
            }
            me->guard_live = false;   /* this event IS its expiry */
            if ( (NULL != me->wait.who) && me->winflight
                && (((ACT_REC == me->act) && (KDRV_ES8311_JOB_RECORD == me->wreq.type))
                    || ((ACT_PLAY == me->act) && (KDRV_ES8311_JOB_PLAY == me->wreq.type))) )
            {
                /* The worker holds an ask's own buffer, and the answer lends
                 * it back: answered KODE_TIMEOUT when the worker returns,
                 * which each I2S transfer's own wait bounds. A recording is
                 * told to end at its next block. */
                KLOG_E(TAG, "the codec worker did not answer inside its guard; "
                            "answered once it returns");
                me->wait.late = true;
                if ( ACT_REC == me->act )
                {
                    kdrv_es8311_record_end();
                }
                status = Q_HANDLED();
                break;
            }
            /* A worker wedged inside an I2S transfer becomes a named failure
             * here, not a bare console timeout. */
            act_fail(me, KODE_TIMEOUT,
                     "the codec worker did not answer inside its guard");
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

void Audio_ctor(void)
{
    Audio * const me = &l_audio;

    QActive_ctor(&me->super, Q_STATE_CAST(&Audio_initial));
    QTimeEvt_ctorX(&me->guard, &me->super, AUDIO_GUARD_SIG, 0U);

    me->guard_stale = 0U;
    me->guard_live  = false;
    me->up          = false;
    me->open        = false;
    me->opening     = false;
#if CONFIG_KODEOS_RADIO
    me->amp_ready   = false;
    me->amp_wait    = false;
#endif
    me->act         = ACT_NONE;
    (void)memset(&me->job,  0, sizeof(me->job));
    (void)memset(&me->wreq, 0, sizeof(me->wreq));
    me->wseq        = 0U;
    me->winflight   = false;
    (void)memset(&me->wait, 0, sizeof(me->wait));
    (void)memset(&me->row,  0, sizeof(me->row));
    (void)memset(me->path,  0, sizeof(me->path));
    me->bytes     = 0U;
    me->rec_ms    = 0U;
    me->bytes_out = NULL;
    me->rec_stop_held = false;
    me->stream_who    = NULL;
    atomic_store(&s_rec_stop_pending, false);
    atomic_store(&s_audio_stream_asker, 0U);
    for ( uint32_t i = 0U; (uint32_t)SETTING_COUNT > i; i++ )
    {
        atomic_store(&s_setting[i].pending, false);
    }
#if CONFIG_KODE_LOWPOWER
    me->lp_who    = KODE_LOWPOWER_WHO_NONE;
    me->lp_wait   = false;
    me->lp_round  = 0U;
#endif
}

AO_STORAGE(audio, KQP_AO_QUEUE_LEN, KQP_AO_STACK_SMALL);   /* the blocking work runs on its worker */

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_audio_register() ---------------- */

/**
 * @brief `test component=audio`: the audio path, judged by itself.
 *
 * The board plays a tone and hears it, in two steps, so a failure says which
 * half it can. Every reading goes out as a log line before the verdict.
 */
static bool audio_row_audio(void)
{
    audio_test_request();
    return false;   /* answered later, with TEST_RESULT_SIG */
}
#endif

void kaos_audio_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t AUDIO_ROWS[] = {
        { "audio", AUTOTEST_ID_AUDIO, audio_row_audio, KROW_ASYNC, AUTOTEST_G_NONE, KODE_AUDIO_TEST_BUDGET_MS,
          NULL, 0U, "Speaker and mic",
          "Speaker or mic problem",
          AUTOTEST_ORDER_AUDIO },
    };

    kaos_require(krows_add(AUDIO_ROWS, sizeof(AUDIO_ROWS) / sizeof(AUDIO_ROWS[0])),
                 "audio rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&audio_verbs), "audio verbs");
#endif
}

void kaos_audio_start(void)
{
    Audio_ctor();
    kaos_audio_register();
    AO_START(AO_Audio, audio, KODE_PRIO_AUDIO);
#if CONFIG_KODEOS_CONSOLE
    /* The settings this AO answers, registered once it has the queue their
     * requests are posted to: an app that does not link it has no such keys. */
    static const kode_config_t AUDIO_CONFIG[] = {
        { .name = KSET_KEY_VOL, .type = KODE_TYPE_INT,
          .min = 0, .max = (int32_t)KSET_PCT_MAX },
        { .name = KSET_KEY_MIC_DB, .type = KODE_TYPE_INT,
          .min = 0, .max = KSET_MIC_DB_MAX },
        { .name = KSET_KEY_MUTE, .type = KODE_TYPE_BOOL },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Audio, KQP_AUDIO_CMD_MARGIN, AUDIO_CONFIG), "audio settings");
#endif
}
