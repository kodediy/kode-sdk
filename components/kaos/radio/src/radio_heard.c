/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_heard.c
 * @brief AO_Radio's BLE advertisement door: one asker, the hear job behind it, the records posted to it.
 */
#include "radio_heard.h"

#include <string.h>

#include "kaos.h"
#include "kdrv_ble_heard.h"
#include "kode_log.h"
#include "kqp.h"
#include "radio_knocks.h"

#define TAG "kaos.radio"

_Static_assert(KBLE_HEARD_DATA_MAX == KDRV_BLE_HEARD_DATA_MAX,
               "the advertisement event keeps the payload the ring keeps");
_Static_assert(KBLE_HEARD_ADDR_LEN == KDRV_BLE_HEARD_ADDR_LEN,
               "the advertisement event keeps the address the ring keeps");
_Static_assert((KBLE_HEARD_ADDR_PUBLIC == (uint32_t)KDRV_BLE_ADDR_PUBLIC)
               && (KBLE_HEARD_ADDR_RANDOM == (uint32_t)KDRV_BLE_ADDR_RANDOM),
               "the advertisement event names the address types the ring keeps");
_Static_assert((KBLE_HEARD_PDU_CONN == (uint32_t)KDRV_BLE_HEARD_PDU_CONN)
               && (KBLE_HEARD_PDU_DIRECT == (uint32_t)KDRV_BLE_HEARD_PDU_DIRECT)
               && (KBLE_HEARD_PDU_SCAN == (uint32_t)KDRV_BLE_HEARD_PDU_SCAN)
               && (KBLE_HEARD_PDU_NONCONN == (uint32_t)KDRV_BLE_HEARD_PDU_NONCONN)
               && (KBLE_HEARD_PDU_SCAN_RSP == (uint32_t)KDRV_BLE_HEARD_PDU_SCAN_RSP)
               && (KBLE_HEARD_PDU_OTHER == (uint32_t)KDRV_BLE_HEARD_PDU_OTHER),
               "the advertisement event names the kinds of packet the ring keeps");

/** @brief The part of its asker's queue the stream leaves free: one half. */
#define HEARD_QUEUE_SHARE  2U

/** @brief The stack ended the passive scan by itself: set on the host's task, taken by the knock's handler. */
static bool s_heard_ended = false;

/**
 * @brief Tell @p who how its request stands.
 *
 * The event is taken with no pool margin and posted with
 * KQP_HEARD_ANSWER_MARGIN; neither failure asserts. An empty pool or an asker's
 * queue without that room is logged as an error and the answer is lost: the
 * asker hears nothing for its request, and the door's state is what the
 * caller left it.
 *
 * @param who     The asker.
 * @param on      Listening now.
 * @param why     The reason.
 * @param heard   Advertisements heard since the start.
 * @param dropped Not handed over since the start.
 */
static void heard_tell(QActive *who, bool on, kble_heard_why_t why, uint32_t heard, uint32_t dropped)
{
    BleHeardStateEvt *const e = Q_NEW_X(BleHeardStateEvt, 0U, BLE_HEARD_STATE_SIG);

    if ( NULL == e )
    {
        KLOG_E(TAG, "a BLE advertisement answer could not be allocated");
        return;
    }
    e->on      = on;
    e->why     = (uint8_t)why;
    e->heard   = heard;
    e->dropped = dropped;
    if ( !QACTIVE_POST_X(who, &e->super, KQP_HEARD_ANSWER_MARGIN, AO_Radio) )
    {
        KLOG_E(TAG, "the asker's queue is full; a BLE advertisement answer is lost");
    }
}

/**
 * @brief The stream's totals so far.
 * @param h       The door.
 * @param heard   Receives what the stack heard since the start.
 * @param dropped Receives what the ring refused and the asker's queue or the pool had no room for.
 */
static void heard_totals(const RadioHeard *h, uint32_t *heard, uint32_t *dropped)
{
    *heard   = kdrv_ble_heard_total();
    *dropped = kdrv_ble_heard_dropped() + h->lost;
}

/**
 * @brief Tell @p who the stream's state with the totals so far.
 * @param h   The door.
 * @param who The asker.
 * @param on  Listening now.
 * @param why The reason.
 */
static void heard_tell_totals(const RadioHeard *h, QActive *who, bool on, kble_heard_why_t why)
{
    uint32_t heard;
    uint32_t dropped;

    heard_totals(h, &heard, &dropped);
    heard_tell(who, on, why, heard, dropped);
}

/**
 * @brief Free the door: nobody asks, nothing is owed, nothing is counted.
 * @param h The door.
 */
static void heard_release(RadioHeard *h)
{
    h->who        = NULL;
    h->start_owed = false;
    h->stop_owed  = false;
    h->lost       = 0U;
}

/**
 * @brief Hand one hear job to the worker.
 * @param h  The door.
 * @param on true for a start, false for a stop.
 * @return KODE_OK when the job is with the worker; KODE_NO_RESOURCES when the
 *         worker is inside another job; another status when it would not
 *         take it.
 */
static kode_status_t heard_job(RadioHeard *h, bool on)
{
    kode_status_t const rc = h->submit(h->ctx, on);

    if ( KODE_OK == rc )
    {
        h->job = true;
    }
    return rc;
}

/**
 * @brief Whether the stack ended the passive scan since the last time this was asked; asking clears it.
 * @return true when it did.
 */
static bool heard_ended_take(void)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    return __atomic_exchange_n(&s_heard_ended, false, __ATOMIC_ACQ_REL);
}

void radio_heard_init(RadioHeard *h, RadioHeardSubmit submit, void *ctx)
{
    heard_release(h);
    (void)heard_ended_take();
    h->scanning = false;
    h->job      = false;
    h->margin   = (uint16_t)KQP_HEARD_MARGIN;
    h->submit   = submit;
    h->ctx      = ctx;
}

/**
 * @brief The slots an advertisement leaves free in @p who's queue.
 *
 * Half of the queue, rounded up, so the stream alone never takes an asker
 * past half full: the console asks a setting's owner only while every queue is half
 * free (kqp_queues_half_free()), and a slow asker would otherwise hold
 * `config` busy for as long as it listens. KQP_HEARD_MARGIN is the floor,
 * for a short queue and for an AO whose queue length is not known.
 *
 * @param who The asker.
 * @return The margin of its advertisements.
 */
static uint16_t heard_margin_of(QActive const *who)
{
    uint16_t margin = (uint16_t)((kqp_queue_len(who) + (HEARD_QUEUE_SHARE - 1U)) / HEARD_QUEUE_SHARE);

    if ( KQP_HEARD_MARGIN > margin )
    {
        margin = (uint16_t)KQP_HEARD_MARGIN;
    }
    return margin;
}

/**
 * @brief Give the door to @p who: its stream starts with nothing owed and nothing lost.
 * @param h   The door.
 * @param who The asker.
 */
static void heard_hold(RadioHeard *h, QActive *who)
{
    h->who        = who;
    h->margin     = heard_margin_of(who);
    h->start_owed = false;
    h->stop_owed  = false;
    h->lost       = 0U;
}

/**
 * @brief Another asker's start while @p h is held: the newest request has the stream.
 *
 * The passive scan is left as it is, on, on its way or owed: the holder is
 * told `{on false, KBLE_HEARD_CANCELLED}` with its totals, and what is heard
 * from here on is counted from zero for @p who. A start still with the
 * worker, or one that waits for it, answers to @p who when it ends, and a
 * stop its first asker owed behind it is forgotten. With a stop on the worker
 * the stream is ending: @p who is told busy and asks again.
 *
 * @param h   The door; another asker holds it.
 * @param who The new asker.
 */
static void heard_take_over(RadioHeard *h, QActive *who)
{
    QActive *const old   = h->who;
    bool const     owed  = h->start_owed;
    uint32_t       heard;
    uint32_t       dropped;

    if ( h->job && h->scanning )
    {
        heard_tell(who, false, KBLE_HEARD_BUSY, 0U, 0U);
        return;
    }
    heard_totals(h, &heard, &dropped);
    kdrv_ble_heard_forget();
    heard_hold(h, who);
    h->start_owed = owed;
    KLOG_I(TAG, "a newer asker takes the BLE advertisement stream");
    heard_tell(old, false, KBLE_HEARD_CANCELLED, heard, dropped);
    if ( h->scanning && (!h->job) )
    {
        heard_tell(who, true, KBLE_HEARD_OK, 0U, 0U);
    }
}

/**
 * @brief A start from @p who.
 * @param h   The door.
 * @param who The asker.
 */
static void heard_start(RadioHeard *h, QActive *who)
{
    if ( NULL != h->who )
    {
        if ( who != h->who )
        {
            heard_take_over(h, who);
        }
        else if ( h->scanning && (!h->job) )
        {
            heard_tell_totals(h, who, true, KBLE_HEARD_OK);   /* already listening for it */
        }
        else if ( h->job && h->scanning )
        {
            /* Its own stop is with the worker. */
            heard_tell(who, false, KBLE_HEARD_BUSY, 0U, 0U);
        }
        else
        {
            /* Its own start is with the worker, or waits for it: its answer is this one's. */
        }
        return;
    }
    if ( h->job || h->scanning )
    {
        /* A passive scan that was ended is still being stopped. */
        heard_tell(who, false, KBLE_HEARD_BUSY, 0U, 0U);
        return;
    }
    if ( KODE_OK != kdrv_ble_heard_init() )
    {
        KLOG_W(TAG, "no room for the BLE advertisements ring; the stream is refused");
        heard_tell(who, false, KBLE_HEARD_MEMORY, 0U, 0U);
        return;
    }
    kdrv_ble_heard_forget();
    /* An end the host task noted for an earlier stream, which was stopped
     * before its knock was read, is not this stream's: it would end it at
     * its first record. */
    (void)heard_ended_take();

    kode_status_t const rc = heard_job(h, true);

    if ( KODE_OK == rc )
    {
        heard_hold(h, who);
        KLOG_I(TAG, "a BLE advertisement stream is started for an asker");
    }
    else if ( KODE_NO_RESOURCES == rc )
    {
        /* The worker is inside a job AO_Radio waits for: a listen this start
         * ended, which is leaving, or a join. The start goes when the worker
         * is back (radio_heard_settle()), and answers then. */
        heard_hold(h, who);
        h->start_owed = true;
        KLOG_I(TAG, "a BLE advertisement stream waits for the worker");
    }
    else
    {
        heard_tell(who, false, KBLE_HEARD_BUSY, 0U, 0U);
    }
}

/**
 * @brief A stop from @p who.
 * @param h   The door.
 * @param who The asker.
 */
static void heard_stop(RadioHeard *h, QActive *who)
{
    if ( NULL == h->who )
    {
        if ( h->scanning && (!h->job) )
        {
            (void)heard_job(h, false);   /* a passive scan nobody asks for: best effort */
        }
        heard_tell(who, false, KBLE_HEARD_OK, 0U, 0U);   /* nobody listens for it */
    }
    else if ( (who == h->who) && h->start_owed )
    {
        /* Its start still waited for the worker: nothing was started. */
        heard_release(h);
        heard_tell(who, false, KBLE_HEARD_OK, 0U, 0U);
    }
    else if ( who != h->who )
    {
        /* Not its stream, or no longer: nothing listens for it, and the holder is not touched. */
        heard_tell(who, false, KBLE_HEARD_OK, 0U, 0U);
    }
    else if ( !h->job )
    {
        if ( KODE_OK != heard_job(h, false) )
        {
            /* The worker is inside another job: still on, and the asker may ask again. */
            heard_tell_totals(h, who, true, KBLE_HEARD_BUSY);
        }
    }
    else if ( !h->scanning )
    {
        h->stop_owed = true;   /* goes out when the start's job is answered */
    }
    else
    {
        /* A stop already waits: its answer is this one's. */
    }
}

void radio_heard_request(RadioHeard *h, BleHeardReqEvt const *e)
{
    if ( e->on )
    {
        heard_start(h, e->who);
    }
    else
    {
        heard_stop(h, e->who);
    }
}

void radio_heard_refuse(BleHeardReqEvt const *e, kble_heard_why_t why)
{
    heard_tell(e->who, false, why, 0U, 0U);
}

/**
 * @brief Post one record to the asker.
 * @param h   The door.
 * @param rec The record.
 * @return false when it could not be posted.
 */
static bool heard_record_post(const RadioHeard *h, const kdrv_ble_heard_t *rec)
{
    BleHeardEvt *const e = Q_NEW_X(BleHeardEvt, KQP_HEARD_POOL_MARGIN, BLE_HEARD_SIG);

    if ( NULL == e )
    {
        return false;
    }
    (void)memcpy(e->addr, rec->addr, sizeof(e->addr));
    e->addr_type = rec->addr_type;
    e->rssi      = rec->rssi;
    e->pdu       = rec->pdu;
    e->len       = rec->len;
    (void)memcpy(e->data, rec->data, sizeof(e->data));
    e->dropped   = kdrv_ble_heard_dropped() + h->lost;
    return QACTIVE_POST_X(h->who, &e->super, h->margin, AO_Radio);
}

/**
 * @brief Every record the ring holds goes to the asker, oldest first.
 *
 * A record the asker's queue or the pool has no room for is dropped and
 * counted, never re-queued. The ring holds at most KDRV_BLE_HEARD_RING_LEN;
 * a record that lands while this runs waits for its own knock.
 *
 * @param h The door; its asker is held.
 */
static void heard_hand_over(RadioHeard *h)
{
    kdrv_ble_heard_t rec;
    bool             more = true;

    for ( uint32_t n = 0U; (KDRV_BLE_HEARD_RING_LEN > n) && more; n++ )
    {
        more = kdrv_ble_heard_next(&rec);
        if ( more && !heard_record_post(h, &rec) )
        {
            h->lost++;
        }
    }
}

bool radio_heard_end(RadioHeard *h, kble_heard_why_t why)
{
    QActive *const who = h->who;

    if ( NULL != who )
    {
        uint32_t heard;
        uint32_t dropped;

        heard_totals(h, &heard, &dropped);
        heard_release(h);
        kdrv_ble_heard_forget();
        KLOG_I(TAG, "the BLE advertisement stream is ended by the radio (why %u)", (unsigned)why);
        heard_tell(who, false, why, heard, dropped);
    }
    if ( h->scanning && (!h->job) )
    {
        /* Best effort: a stop the worker has no room for leaves the scan on,
         * and is tried again by the next end or the next record. */
        (void)heard_job(h, false);
    }
    return h->scanning || h->job;
}

bool radio_heard_settle(RadioHeard *h)
{
    bool started = false;

    if ( h->start_owed )
    {
        kode_status_t const rc = heard_job(h, true);

        if ( KODE_OK == rc )
        {
            h->start_owed = false;
            started       = true;
        }
        else if ( KODE_NO_RESOURCES == rc )
        {
            /* Still inside a job: the next one that ends pays it. */
        }
        else
        {
            QActive *const who = h->who;

            heard_release(h);
            heard_tell(who, false, KBLE_HEARD_BUSY, 0U, 0U);
        }
    }
    return started;
}

bool radio_heard_waits(RadioHeard const *h)
{
    return h->start_owed;
}

void radio_heard_cancel(RadioHeard *h)
{
    QActive *const who = h->who;

    if ( h->job && h->scanning )
    {
        /* A stop is with the worker already: its answer stands, and the
         * newer request waits for the worker as it does for any job. */
        return;
    }
    if ( NULL != who )
    {
        uint32_t heard;
        uint32_t dropped;

        heard_totals(h, &heard, &dropped);
        heard_release(h);
        kdrv_ble_heard_forget();
        KLOG_I(TAG, "a newer Bluetooth request takes the radio; the BLE advertisement stream ends early");
        heard_tell(who, false, KBLE_HEARD_CANCELLED, heard, dropped);
    }
    /* The listen that follows cancels the passive scan as it starts its own.
     * A start still with the worker says the scan is on when it answers, and
     * with nobody asking it is stopped then (heard_started()). */
    h->scanning = false;
}

void radio_heard_quiet(RadioHeard *h)
{
    QActive *const who = h->who;

    h->scanning = false;
    if ( NULL != who )
    {
        heard_tell_totals(h, who, false, KBLE_HEARD_BLE_OFF);
    }
    heard_release(h);
    kdrv_ble_heard_forget();
}

void radio_heard_gone(RadioHeard *h)
{
    QActive *const who = h->who;

    if ( NULL != who )
    {
        heard_tell_totals(h, who, false, KBLE_HEARD_NO_LINK);
    }
    heard_release(h);
    kdrv_ble_heard_forget();
    h->scanning = false;
    h->job      = false;
}

/**
 * @brief The start's job answered.
 * @param h  The door.
 * @param rc The worker's status.
 */
static void heard_started(RadioHeard *h, kode_status_t rc)
{
    QActive *const who = h->who;

    if ( KODE_OK != rc )
    {
        /* A start the guard gave up on may still have started the scan: it
         * "may be on", and the next record that finds nobody asking stops it. */
        h->scanning = (KODE_TIMEOUT == rc);
        if ( NULL != who )
        {
            heard_release(h);
            kdrv_ble_heard_forget();
            heard_tell(who, false, KBLE_HEARD_REFUSED, 0U, 0U);
        }
        return;
    }
    h->scanning = true;
    if ( NULL == who )
    {
        /* The radio ended the stream while its start ran: nobody listens for it. */
        (void)heard_job(h, false);
        return;
    }
    heard_tell_totals(h, who, true, KBLE_HEARD_OK);
    /* What was heard while the start ran, after the answer and never before it. */
    heard_hand_over(h);
    if ( h->stop_owed )
    {
        h->stop_owed = false;
        if ( KODE_OK != heard_job(h, false) )
        {
            heard_tell_totals(h, who, true, KBLE_HEARD_BUSY);
        }
    }
}

/**
 * @brief The stop's job answered.
 * @param h  The door.
 * @param rc The worker's status.
 */
static void heard_stopped(RadioHeard *h, kode_status_t rc)
{
    QActive *const who = h->who;

    h->scanning = false;
    if ( NULL == who )
    {
        return;   /* ended by the radio: its asker was told then */
    }
    /* Whatever landed while the stop was on the worker is the asker's too,
     * and comes before the answer that counts it. */
    heard_hand_over(h);

    uint32_t heard;
    uint32_t dropped;

    heard_totals(h, &heard, &dropped);
    heard_release(h);
    kdrv_ble_heard_forget();
    if ( KODE_OK == rc )
    {
        heard_tell(who, false, KBLE_HEARD_OK, heard, dropped);
    }
    else
    {
        heard_tell(who, false, KBLE_HEARD_REFUSED, heard, dropped);
    }
}

void radio_heard_done(RadioHeard *h, bool on, kode_status_t rc)
{
    h->job = false;
    if ( on )
    {
        heard_started(h, rc);
    }
    else
    {
        heard_stopped(h, rc);
    }
}

/**
 * @brief The stack ended the passive scan by itself.
 * @param h The door.
 */
static void heard_stack_ended(RadioHeard *h)
{
    QActive *const who = h->who;

    if ( h->job && h->scanning )
    {
        return;   /* a stop is with the worker: its answer says it */
    }
    if ( !h->job )
    {
        h->scanning = false;
    }
    if ( NULL != who )
    {
        /* A start still with the worker is answered by its own end: nobody is left to tell. */
        uint32_t heard;
        uint32_t dropped;

        heard_totals(h, &heard, &dropped);
        heard_release(h);
        kdrv_ble_heard_forget();
        KLOG_W(TAG, "the BLE stack ended the advertisement stream");
        heard_tell(who, false, KBLE_HEARD_REFUSED, heard, dropped);
    }
}

void radio_heard_knock(RadioHeard *h)
{
    /* Taken before the ring is read: a record that lands from here on makes
     * a knock of its own, so none waits unseen. */
    (void)radio_knocks_take(RADIO_KNOCK_HEARD, NULL);

    bool const ended = heard_ended_take();

    bool records = false;

    if ( NULL == h->who )
    {
        kdrv_ble_heard_t rec;

        records = kdrv_ble_heard_next(&rec);
        kdrv_ble_heard_forget();
    }
    else if ( !h->scanning )
    {
        /* The start is still with the worker, or waits for it: what the scan
         * already hears waits in the ring, and comes after the answer that
         * says it is on (heard_started()). */
    }
    else
    {
        heard_hand_over(h);
    }
    if ( ended )
    {
        heard_stack_ended(h);
    }
    else if ( (NULL == h->who) && (h->scanning || records) && (!h->job) )
    {
        /* Records come and nobody asked for them: a record proves a passive scan,
         * whatever the door believes, and it is quieted, once. */
        if ( KODE_OK == heard_job(h, false) )
        {
            KLOG_W(TAG, "BLE advertisements arrive and nobody asked for them; the passive scan is stopped");
        }
    }
    else
    {
        /* Handed over, or nothing is left to do. */
    }
}

void radio_heard_drop(void)
{
    (void)radio_knocks_take(RADIO_KNOCK_HEARD, NULL);
    (void)heard_ended_take();
    kdrv_ble_heard_forget();
}

void radio_heard_note(kdrv_ble_hear_note_t note)
{
    if ( KDRV_BLE_HEAR_ENDED == note )
    {
        /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
        __atomic_store_n(&s_heard_ended, true, __ATOMIC_RELEASE);
    }
    /* One knock waits at a time: its handler drains every record the ring
     * holds. A queue with no room owes it, so neither a record nor the end
     * of the scan waits for the next advertisement to be told. */
    radio_knocks_post(RADIO_KNOCK_HEARD, 0U);
}
