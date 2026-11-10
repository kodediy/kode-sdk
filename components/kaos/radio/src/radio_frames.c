/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_frames.c
 * @brief AO_Radio's Wi-Fi frames door: one asker, the C5's service behind it, the ticks and frames posted to it.
 */
#include "radio_frames.h"

#include <string.h>

#include "kaos.h"
#include "kdrv_frames.h"
#include "kode_log.h"
#include "kode_wire.h"
#include "kqp.h"

#define TAG "kaos.radio"

_Static_assert(KWIFI_FRAME_HEAD_LEN == KODE_SNIFF_HEAD_LEN,
               "the frame event keeps the bytes the wire carries");

/** @brief Whether a request of the frames service waits for its answer. */
static bool frames_waiting(const RadioFrames *f)
{
    return (uint8_t)RADIO_FRAMES_WAIT_NONE != f->wait;
}

/**
 * @brief Tell @p who how its request stands.
 *
 * With no margin: an answer is always owed, and an asker whose queue has not
 * one slot for it has a design fault QP reports.
 *
 * @param who     The asker.
 * @param on      Listening now.
 * @param channel The radio's channel, when known.
 * @param joined  The station holds an association.
 * @param why     The reason.
 */
static void frames_tell(QActive *who, bool on, uint8_t channel, bool joined, kwifi_frames_why_t why)
{
    WifiFramesStateEvt *const e = Q_NEW_X(WifiFramesStateEvt, 0U, WIFI_FRAMES_STATE_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "a Wi-Fi frames answer could not be allocated");
        return;
    }
    e->on      = on;
    e->channel = channel;
    e->joined  = joined;
    e->why     = (uint8_t)why;
    if ( !QACTIVE_POST_X(who, &e->super, KQP_FRAMES_ANSWER_MARGIN, AO_Radio) )
    {
        KLOG_E(TAG, "the asker's queue is full; a Wi-Fi frames answer is lost");
    }
}

/**
 * @brief Free the door: nobody asks, nothing waits, nothing is counted.
 * @param f The door.
 */
static void frames_release(RadioFrames *f)
{
    f->who       = NULL;
    f->wait      = (uint8_t)RADIO_FRAMES_WAIT_NONE;
    f->stop_owed = false;
    f->lost      = 0U;
}

/**
 * @brief What the asker hears for a reason the C5 gave.
 * @param wire KODE_SNIFF_WHY_*.
 * @return The reason; KWIFI_FRAMES_CHANNEL for one that names none, since a
 *         START that is not listening was refused at the channel.
 */
static kwifi_frames_why_t frames_why_of(uint8_t wire)
{
    kwifi_frames_why_t why = KWIFI_FRAMES_CHANNEL;

    switch ( wire )
    {
        case KODE_SNIFF_WHY_WIFI_OFF:
            why = KWIFI_FRAMES_WIFI_OFF;
            break;
        case KODE_SNIFF_WHY_JOINED:
            why = KWIFI_FRAMES_JOINED;
            break;
        case KODE_SNIFF_WHY_MEMORY:
            why = KWIFI_FRAMES_MEMORY;
            break;
        default:
            break;
    }
    return why;
}

/**
 * @brief Send the STOP of the asker's stream.
 * @param f The door; its asker holds a stream.
 * @return true when it is on its way.
 */
static bool frames_stop_send(RadioFrames *f)
{
    bool const sent = KODE_OK == f->send(f->ctx, KODE_SNIFF_STOP, NULL, 0U);

    if ( sent )
    {
        f->wait = (uint8_t)RADIO_FRAMES_WAIT_STOP;
    }
    return sent;
}

void radio_frames_init(RadioFrames *f, RadioFramesSend send, void *ctx)
{
    frames_release(f);
    f->ring_ok = false;
    f->send    = send;
    f->ctx     = ctx;
}

void radio_frames_link(RadioFrames *f)
{
    f->ring_ok = KODE_OK == kdrv_frames_init();
    if ( !f->ring_ok )
    {
        KLOG_W(TAG, "no room for the Wi-Fi frames ring; frames are refused");
    }
}

/**
 * @brief A start from @p who.
 * @param f       The door.
 * @param who     The asker.
 * @param channel What it asked for.
 */
static void frames_start(RadioFrames *f, QActive *who, uint8_t channel)
{
    kode_sniff_start_t req;
    kode_status_t      rc;

    if ( !f->ring_ok )
    {
        frames_tell(who, false, 0U, false, KWIFI_FRAMES_MEMORY);
        return;
    }
    if ( KWIFI_FRAMES_CHANNEL_MAX < channel )
    {
        frames_tell(who, false, 0U, false, KWIFI_FRAMES_CHANNEL);
        return;
    }
    if ( ((NULL != f->who) && (who != f->who)) || frames_waiting(f) )
    {
        frames_tell(who, false, 0U, false, KWIFI_FRAMES_BUSY);
        return;
    }
    (void)memset(&req, 0, sizeof(req));
    req.channel = channel;
    kdrv_frames_forget();
    rc = f->send(f->ctx, KODE_SNIFF_START, &req, sizeof(req));
    if ( KODE_OK != rc )
    {
        kwifi_frames_why_t why = KWIFI_FRAMES_NO_LINK;

        if ( KODE_DENIED == rc )
        {
            why = KWIFI_FRAMES_BUSY;
        }
        frames_tell(who, false, 0U, false, why);
        return;
    }
    f->who       = who;
    f->wait      = (uint8_t)RADIO_FRAMES_WAIT_START;
    f->stop_owed = false;
    f->lost      = 0U;
}

/**
 * @brief A stop from @p who.
 * @param f   The door.
 * @param who The AO that asked.
 */
static void frames_stop(RadioFrames *f, QActive *who)
{
    if ( NULL == f->who )
    {
        frames_tell(who, false, 0U, false, KWIFI_FRAMES_OK);   /* nobody listens */
    }
    else if ( who != f->who )
    {
        frames_tell(who, false, 0U, false, KWIFI_FRAMES_BUSY);
    }
    else if ( (uint8_t)RADIO_FRAMES_WAIT_NONE == f->wait )
    {
        if ( !frames_stop_send(f) )
        {
            frames_release(f);
            kdrv_frames_forget();
            frames_tell(who, false, 0U, false, KWIFI_FRAMES_NO_LINK);
        }
    }
    else if ( (uint8_t)RADIO_FRAMES_WAIT_START == f->wait )
    {
        f->stop_owed = true;   /* goes out when the START is answered */
    }
    else
    {
        /* A stop already waits: its answer is this one's. */
    }
}

void radio_frames_request(RadioFrames *f, WifiFramesReqEvt const *e)
{
    if ( e->on )
    {
        frames_start(f, e->who, e->channel);
    }
    else
    {
        frames_stop(f, e->who);
    }
}

void radio_frames_refuse(WifiFramesReqEvt const *e)
{
    frames_tell(e->who, false, 0U, false, KWIFI_FRAMES_NO_LINK);
}

/**
 * @brief The START was answered.
 * @param f  The door.
 * @param st The answer, read.
 */
static void frames_start_answered(RadioFrames *f, const kdrv_frames_state_t *st)
{
    QActive *const who = f->who;

    if ( st->present && st->on )
    {
        frames_tell(who, true, st->channel, st->joined, KWIFI_FRAMES_OK);
        if ( f->stop_owed )
        {
            f->stop_owed = false;
            if ( !frames_stop_send(f) )
            {
                frames_release(f);
                kdrv_frames_forget();
                frames_tell(who, false, 0U, false, KWIFI_FRAMES_NO_LINK);
            }
        }
        return;
    }
    kwifi_frames_why_t why = KWIFI_FRAMES_NO_SERVICE;

    if ( st->present )
    {
        why = frames_why_of(st->why);
    }
    frames_release(f);
    kdrv_frames_forget();
    frames_tell(who, false, st->channel, st->joined, why);
}

/**
 * @brief The STOP was answered: the stream is over.
 * @param f  The door.
 * @param st The answer, read.
 */
static void frames_stop_answered(RadioFrames *f, const kdrv_frames_state_t *st)
{
    QActive *const     who = f->who;
    kwifi_frames_why_t why = KWIFI_FRAMES_NO_SERVICE;

    if ( st->present )
    {
        why = KWIFI_FRAMES_OK;
    }
    frames_release(f);
    kdrv_frames_forget();
    frames_tell(who, false, st->channel, st->joined, why);
}

void radio_frames_answer(RadioFrames *f, uint8_t status, const uint8_t *payload, size_t len)
{
    kdrv_frames_state_t st;
    uint8_t const       wait = f->wait;

    f->wait = (uint8_t)RADIO_FRAMES_WAIT_NONE;
    kdrv_frames_answer(status, payload, len, &st);
    if ( (uint8_t)RADIO_FRAMES_WAIT_START == wait )
    {
        frames_start_answered(f, &st);
    }
    else if ( (uint8_t)RADIO_FRAMES_WAIT_STOP == wait )
    {
        frames_stop_answered(f, &st);
    }
    else
    {
        /* A quiet stop, or an answer nobody waits for: nothing is owed. */
    }
}

void radio_frames_lost(RadioFrames *f)
{
    uint8_t const wait = f->wait;

    if ( ((uint8_t)RADIO_FRAMES_WAIT_START == wait)
        || ((uint8_t)RADIO_FRAMES_WAIT_STOP == wait) )
    {
        QActive *const who = f->who;

        frames_release(f);
        kdrv_frames_forget();
        frames_tell(who, false, 0U, false, KWIFI_FRAMES_NO_LINK);
    }
    else
    {
        f->wait = (uint8_t)RADIO_FRAMES_WAIT_NONE;
    }
}

void radio_frames_gone(RadioFrames *f)
{
    QActive *const who = f->who;

    frames_release(f);
    kdrv_frames_forget();
    if ( NULL != who )
    {
        frames_tell(who, false, 0U, false, KWIFI_FRAMES_NO_LINK);
    }
}

/**
 * @brief Post one frame to the asker.
 * @param who The asker.
 * @param fr  The frame.
 * @return false when it could not be posted.
 */
static bool frames_frame_post(QActive *who, const kdrv_frame_t *fr)
{
    WifiFrameEvt *const e = Q_NEW_X(WifiFrameEvt, KQP_FRAMES_POOL_MARGIN, WIFI_FRAME_SIG);

    if ( NULL == e )
    {
        return false;
    }
    e->channel = fr->channel;
    e->rssi    = fr->rssi;
    e->len     = fr->len;
    (void)memcpy(e->fc, fr->fc, sizeof(e->fc));
    (void)memcpy(e->addr1, fr->addr1, sizeof(e->addr1));
    (void)memcpy(e->addr2, fr->addr2, sizeof(e->addr2));
    (void)memcpy(e->head, fr->head, sizeof(e->head));
    return QACTIVE_POST_X(who, &e->super, KQP_FRAMES_MARGIN, AO_Radio);
}

/**
 * @brief Post one period's counts to the asker.
 * @param f The door.
 * @param t The tick.
 */
static void frames_tick_post(const RadioFrames *f, const kdrv_frames_tick_t *t)
{
    WifiFramesTickEvt *const e = Q_NEW_X(WifiFramesTickEvt, KQP_FRAMES_POOL_MARGIN, WIFI_FRAMES_TICK_SIG);

    if ( NULL == e )
    {
        return;
    }
    e->channel  = t->channel;
    e->joined   = t->joined;
    e->rssi_max = t->rssi_max;
    e->heard    = t->heard;
    e->mgmt     = t->mgmt;
    e->data     = t->data;
    e->dropped  = kdrv_frames_dropped() + f->lost;
    (void)QACTIVE_POST_X(f->who, &e->super, KQP_FRAMES_MARGIN, AO_Radio);
}

/**
 * @brief A batch waits and somebody listens: the tick first, then every frame.
 * @param f The door.
 */
static void frames_hand_over(RadioFrames *f)
{
    kdrv_frames_tick_t t;
    kdrv_frame_t       fr;

    if ( kdrv_frames_tick_take(&t) )
    {
        frames_tick_post(f, &t);
    }
    /* The ring holds at most KDRV_FRAMES_RING_LEN; a batch that lands while
     * this runs waits for its own knock. */
    bool more = true;

    for ( uint32_t n = 0U; (KDRV_FRAMES_RING_LEN > n) && more; n++ )
    {
        more = kdrv_frames_next(&fr);
        if ( more && !frames_frame_post(f->who, &fr) )
        {
            f->lost++;
        }
    }
}

/**
 * @brief A batch came and nobody listens: the C5 is still listening from
 *        before this chip's reset. Empty the ring and tell it to stop, once.
 * @param f The door.
 */
static void frames_quiet(RadioFrames *f)
{
    kdrv_frames_forget();
    if ( (uint8_t)RADIO_FRAMES_WAIT_NONE == f->wait )
    {
        if ( KODE_OK == f->send(f->ctx, KODE_SNIFF_STOP, NULL, 0U) )
        {
            f->wait = (uint8_t)RADIO_FRAMES_WAIT_QUIET;
            KLOG_W(TAG, "Wi-Fi frames arrive and nobody asked for them; the C5 is told to stop");
        }
    }
}

void radio_frames_knock(RadioFrames *f)
{
    if ( NULL == f->who )
    {
        frames_quiet(f);
    }
    else
    {
        frames_hand_over(f);
    }
}

void radio_frames_drop(void)
{
    kdrv_frames_forget();
}

void radio_frames_heard(uint8_t op, const uint8_t *data, size_t len)
{
    static const QEvt knock = QEVT_INITIALIZER(RADIO_FRAMES_KNOCK_SIG);

    if ( KODE_SNIFF_EVT_BATCH != op )
    {
        KLOG_D(TAG, "a Wi-Fi frames event %u is not one this build knows", (unsigned)op);
        return;
    }
    if ( kdrv_frames_feed(data, len) )
    {
        /* Static and posted with a margin: a batch is never worth a block
         * from the pool, and a queue with no room for the knock keeps the
         * batch in the ring for the next one. */
        (void)QACTIVE_POST_X(AO_Radio, &knock, KQP_RADIO_POST_MARGIN, NULL);
    }
}
