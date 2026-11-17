/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file storage_fetch.c
 * @brief AO_Storage's download of a URL into a file, chunk by chunk.
 */
#include "kaos.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kaos_ask.h"
#include "kdrv_storage.h"
#include "khal_system.h"
#include "kode_log.h"
#include "kqp.h"
#include "storage_fetch.h"

#define TAG "kaos.storage"

/* In a file of its own so that an app links it only by asking for a download:
 * AO_Storage names storage_fetch_event() weakly, and this file names AO_Net's
 * stream strongly, so a download is what links AO_Net beside AO_Storage.
 * Everything but the two doors runs on AO_Storage's thread. */

/** @brief The one door of the download's asks. */
static kaos_ask_door_t s_fetch_door;

/** @brief The status of a body sent from its start. */
#define FETCH_HTTP_OK       200U

/** @brief The status of a range that lies past the end: the file has it all. */
#define FETCH_HTTP_NO_RANGE 416U

/** @brief Slots a progress event leaves free in its asker's queue, as a
 *         frames tick does: room for the asker's own traffic and the end's
 *         answer, which is posted with no margin. */
#define FETCH_PROGRESS_QUEUE_MARGIN 4U

/** @brief Where a download is. */
typedef enum {
    FETCH_OPENING = 0,  /**< AO_Net has the open; its answer is awaited.      */
    FETCH_READING,      /**< A chunk is awaited, or held for the file's turn. */
} fetch_phase_t;

/** @brief A download's record: what the door copied, and where the download is. */
typedef struct {
    char         url[KODE_NET_FETCH_URL_MAX]; /**< Where from.                         */
    char         path[KODE_PATH_MAX];         /**< Where to.                           */
    QActive     *who;                         /**< The asking AO.                      */
    HttpFileEvt *rsp;                         /**< The end's answer, taken at the ask. */
    uint32_t     had;      /**< The file's length when the download began.   */
    uint32_t     written;  /**< Bytes appended since.                        */
    uint32_t     total;    /**< The length the file will have; 0 unknown.    */
    uint32_t     waited_ms;/**< How long the held chunk has waited its turn. */
    uint16_t     http;     /**< The server's status; 0 before it answers.    */
    uint16_t     held_len; /**< Bytes of the chunk held for the file's turn. */
    uint8_t      held_idx; /**< AO_Net's buffer it is in.                    */
    fetch_phase_t phase;   /**< Where the download is.                       */
    bool         holding;  /**< A chunk waits for the file's turn.           */
    bool         on_card;  /**< A bare name AO_Storage put on the card.      */
    bool         stopping; /**< A stop was asked while the open was out.     */
} FetchJob;

/** @brief The download under way; NULL for none. AO_Storage's thread only. */
static FetchJob *s_job;

/** @brief The download's two one-shots. */
static QTimeEvt s_retry;
static QTimeEvt s_stall;

kode_status_t storage_fetch_ask(QActive *who, const char *url, const char *path)
{
    kode_status_t   rc = KODE_DENIED;
    HttpFileEvt    *a;
    HttpFileReqEvt *e;
    FetchJob       *job;

    if ( (NULL == url) || (NULL == path)
        /* cppcheck-suppress misra-c2012-17.3 ; a POSIX call: the compiler sees picolibc declare it, cppcheck does not */
        || (strnlen(url, KODE_NET_FETCH_URL_MAX) >= KODE_NET_FETCH_URL_MAX)
        /* cppcheck-suppress misra-c2012-17.3 ; a POSIX call: the compiler sees picolibc declare it, cppcheck does not */
        || (strnlen(path, KODE_PATH_MAX) >= KODE_PATH_MAX) )
    {
        return rc;
    }
    rc = kaos_ask_take(&s_fetch_door, who);
    if ( KODE_OK != rc )
    {
        return rc;
    }
    a = Q_NEW_X(HttpFileEvt, 0U, HTTP_FILE_RSP_SIG);
    if ( NULL == a )
    {
        return kaos_ask_cancel(&s_fetch_door, who, NULL);
    }
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
    job = (FetchJob *)khal_alloc_psram(sizeof(void *), sizeof(*job));
    e   = NULL;
    if ( NULL != job )
    {
        e = Q_NEW_X(HttpFileReqEvt, 0U, HTTP_FILE_REQ_SIG);
    }
    if ( NULL == e )
    {
        khal_free(job);
        return kaos_ask_cancel(&s_fetch_door, who, &a->super);
    }
    (void)memset(job, 0, sizeof(*job));
    (void)snprintf(job->url, sizeof(job->url), "%s", url);
    (void)snprintf(job->path, sizeof(job->path), "%s", path);
    job->who = who;
    job->rsp = a;
    e->job   = job;
    e->who   = who;
    e->rsp   = a;
    rc = kaos_ask_post(&s_fetch_door, who, AO_Storage, &e->super, &a->super,
                       KQP_STORAGE_CMD_MARGIN);
    if ( KODE_OK != rc )
    {
        khal_free(job);   /* never handed over: the post gave the events back */
    }
    return rc;
}

kode_status_t storage_fetch_stop(QActive *who)
{
    kode_status_t    rc = KODE_NO_RESOURCES;
    HttpFileStopEvt *e  = Q_NEW_X(HttpFileStopEvt, 0U, HTTP_FILE_STOP_SIG);

    if ( NULL != e )
    {
        e->who = who;
        if ( QACTIVE_POST_X(AO_Storage, &e->super, KQP_STORAGE_CMD_MARGIN, NULL) )
        {
            rc = KODE_OK;
        }
    }
    return rc;
}

/* ---- The steps, on AO_Storage's thread ----------------------------------- */

/** @brief Construct the two one-shots, the first time a download needs them. */
static void fetch_timers_make(void)
{
    /* Whether the two were constructed. */
    static bool s_timers_made;

    if ( !s_timers_made )
    {
        QTimeEvt_ctorX(&s_retry, AO_Storage, HTTP_FILE_RETRY_SIG, 0U);
        QTimeEvt_ctorX(&s_stall, AO_Storage, HTTP_FILE_STALL_SIG, 0U);
        s_timers_made = true;
    }
}

/** @brief Start, or start again, the wait for AO_Net's next chunk. */
static void fetch_stall_arm(void)
{
    (void)QTimeEvt_disarm(&s_stall);
    QTimeEvt_armX(&s_stall, KQP_TICKS_PER_MS(KODE_HTTP_FILE_STALL_MS), 0U);
}

/**
 * @brief Answer an asker's download with its end, and give its record back.
 *
 * @param job     The record; freed here.
 * @param rc      How it ended.
 * @param stopped Whether its asker's stop ended it.
 */
static void fetch_answer(FetchJob *job, kode_status_t rc, bool stopped)
{
    HttpFileEvt * const a   = job->rsp;
    QActive * const     who = job->who;

    a->rc      = (uint8_t)rc;
    a->stopped = stopped;
    a->status  = job->http;
    a->written = job->written;
    a->size    = job->had + job->written;
    a->total   = job->total;
    khal_free(job);
    kaos_ask_answer(&s_fetch_door, who, &a->super, AO_Storage);
}

/**
 * @brief End the download under way: stop its clocks and answer its asker.
 *
 * @param rc      How it ended.
 * @param stopped Whether its asker's stop ended it.
 */
static void fetch_end(kode_status_t rc, bool stopped)
{
    FetchJob * const job = s_job;

    (void)QTimeEvt_disarm(&s_retry);
    (void)QTimeEvt_disarm(&s_stall);
    s_job = NULL;
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "download into '%s' ended with status %d after %lu bytes",
               job->path, (int)rc, (unsigned long)job->written);
    }
    else
    {
        KLOG_N(TAG, "download into '%s': %lu bytes written, %lu in the file",
               job->path, (unsigned long)job->written,
               (unsigned long)(job->had + job->written));
    }
    fetch_answer(job, rc, stopped);
}

/** @brief End the download and have AO_Net close its transfer. */
static void fetch_abort(kode_status_t rc, bool stopped)
{
    net_stream_close_request();
    fetch_end(rc, stopped);
}

/**
 * @brief A download was asked for: read the file's length and ask AO_Net for
 *        the body from there on.
 *
 * One download at a time, as AO_Net has one stream: a second asker is
 * answered KODE_DENIED at once. So is one asked while another transfer holds
 * the file handle or the card is out on a call, as every file request is:
 * reading the file's length may mount the card under that call.
 *
 * @param q    The request; its record is this module's from here on.
 * @param busy Whether a transfer is in flight at AO_Storage.
 */
static void fetch_begin(HttpFileReqEvt const * const q, bool busy)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the door hands over the record it filled, untyped in the event */
    FetchJob * const job = (FetchJob *)q->job;
    kdrv_stat_t      st  = { false, false, 0U };
    kode_status_t    rc;

    if ( (NULL != s_job) || busy )
    {
        KLOG_W(TAG, "a download or a transfer is in flight; '%s' refused", job->path);
        fetch_answer(job, KODE_DENIED, false);
        return;
    }
    if ( '/' != job->path[0] )
    {
        /* No volume named: it goes where a write of that name goes. */
        char placed[KODE_PATH_MAX];

        if ( !storage_bare_path(job->path, placed, sizeof(placed)) )
        {
            KLOG_W(TAG, "'%s' is too long to place on a volume", job->path);
            fetch_answer(job, KODE_DENIED, false);
            return;
        }
        (void)snprintf(job->path, sizeof(job->path), "%s", placed);
        job->on_card = (kdrv_storage_medium_of(job->path) == KDRV_MEDIUM_SD);
    }
    rc = kdrv_storage_stat(job->path, &st);
    if ( (KODE_OK == rc) && st.exists && st.is_dir )
    {
        rc = KODE_IS_DIR;
    }
    if ( KODE_OK != rc )
    {
        fetch_answer(job, rc, false);
        return;
    }
    if ( st.exists )
    {
        job->had = st.size;
    }
    fetch_timers_make();
    if ( !net_stream_open_from_request(job->url, job->had, AO_Storage) )
    {
        fetch_answer(job, KODE_NO_RESOURCES, false);
        return;
    }
    job->phase = FETCH_OPENING;
    s_job      = job;
    /* AO_Net answers every open, unless it has no event for the answer. */
    fetch_stall_arm();
}

/**
 * @brief AO_Net answered the open.
 *
 * A body that starts where the file ends is read. A 416 to the rest of a
 * file says the file has it all when the resource is as long as the file;
 * longer, or of a length the server did not say, the file is not this
 * resource's start. A 200 to the rest of a file is a server that ignores
 * ranges: its body would be appended in the wrong place. Both leave the file
 * as it is and tell the asker KODE_DENIED with that status.
 *
 * @param r The answer.
 */
static void fetch_opened(NetStreamFromRspEvt const * const r)
{
    FetchJob * const    job = s_job;
    kode_status_t const rc  = (kode_status_t)r->status;

    job->http = r->http;
    if ( KODE_OK == rc )
    {
        if ( (0U < r->size) && ((UINT32_MAX - job->had) >= r->size) )
        {
            job->total = job->had + r->size;
        }
        if ( job->stopping )
        {
            fetch_abort(KODE_OK, true);
        }
        else
        {
            job->phase = FETCH_READING;
            fetch_stall_arm();
            net_stream_read_request();
        }
    }
    else if ( job->stopping )
    {
        fetch_end(KODE_OK, true);
    }
    else if ( (0U < job->had) && (FETCH_HTTP_NO_RANGE == r->http) )
    {
        job->total = r->size;
        if ( r->size == job->had )
        {
            fetch_end(KODE_OK, false);
        }
        else
        {
            KLOG_W(TAG, "'%s' has %lu bytes and the resource %lu; the file is left as it is",
                   job->path, (unsigned long)job->had, (unsigned long)r->size);
            fetch_end(KODE_DENIED, false);
        }
    }
    else if ( (0U < job->had) && (FETCH_HTTP_OK == r->http) )
    {
        KLOG_W(TAG, "the server ignores ranges; '%s' keeps its %lu bytes",
               job->path, (unsigned long)job->had);
        fetch_end(KODE_DENIED, false);
    }
    else
    {
        fetch_end(rc, false);
    }
}

/**
 * @brief Append one chunk to the file: open, write and close in this step, so
 *        the one file handle is never held between two chunks.
 *
 * @param path The file.
 * @param data The chunk.
 * @param len  Its bytes.
 * @return KODE_OK when the bytes are on the medium.
 */
static kode_status_t fetch_append(const char *path, const uint8_t *data, uint16_t len)
{
    kode_status_t rc = kdrv_storage_create_path(path, true);

    if ( KODE_OK == rc )
    {
        rc = kdrv_storage_write(data, (size_t)len);
        if ( KODE_OK != rc )
        {
            /* Closes the handle; an append is left as the write left it. */
            kdrv_storage_discard();
        }
        else
        {
            rc = kdrv_storage_close();
        }
    }
    return rc;
}

/** @brief Tell the asker how far the download is; a pool with no block skips one. */
static void fetch_progress(const FetchJob *job)
{
    HttpFileEvt * const p = Q_NEW_X(HttpFileEvt, KQP_FETCH_POOL_MARGIN, HTTP_FILE_PROGRESS_SIG);

    if ( NULL != p )
    {
        p->rc      = (uint8_t)KODE_OK;
        p->stopped = false;
        p->status  = job->http;
        p->written = job->written;
        p->size    = job->had + job->written;
        p->total   = job->total;
        /* Refused by an asker that reads slower than the chunks arrive: QP
         * gives the event back, and the end's answer says the same and more. */
        (void)QACTIVE_POST_X(job->who, &p->super, FETCH_PROGRESS_QUEUE_MARGIN, AO_Storage);
    }
}

/**
 * @brief Put the held chunk in the file, or keep holding it for the file's turn.
 *
 * Another transfer may hold AO_Storage's one handle when a chunk arrives. The
 * chunk stays in AO_Net's buffer and is tried again every
 * KODE_HTTP_FILE_RETRY_MS, for KODE_STORAGE_TURN_MS in all.
 *
 * @param busy Whether the handle is another transfer's now.
 */
static void fetch_write_held(bool busy)
{
    FetchJob * const job = s_job;

    if ( busy )
    {
        if ( KODE_STORAGE_TURN_MS <= job->waited_ms )
        {
            KLOG_W(TAG, "'%s' never had its turn at the file", job->path);
            fetch_abort(KODE_DENIED, false);
        }
        else
        {
            job->waited_ms += KODE_HTTP_FILE_RETRY_MS;
            (void)QTimeEvt_disarm(&s_retry);
            QTimeEvt_armX(&s_retry, KQP_TICKS_PER_MS(KODE_HTTP_FILE_RETRY_MS), 0U);
        }
        return;
    }
    const uint8_t * const data = net_stream_buf(job->held_idx);
    kode_status_t         rc   = KODE_ERROR;

    if ( NULL != data )
    {
        rc = fetch_append(job->path, data, job->held_len);
    }
    if ( KODE_OK != rc )
    {
        if ( job->on_card )
        {
            storage_bare_failed();   /* the card AO_Storage chose failed it */
        }
        fetch_abort(rc, false);
        return;
    }
    job->written  += job->held_len;
    job->holding   = false;
    job->waited_ms = 0U;
    fetch_stall_arm();
    /* The buffer goes back, which is also the request for the next chunk. */
    net_stream_return(job->held_idx);
    fetch_progress(job);
}

/**
 * @brief AO_Net handed a chunk over, or the end of the body, or a failure.
 *
 * @param d    The chunk.
 * @param busy Whether the file handle is another transfer's now.
 */
static void fetch_chunk(StorageDataEvt const * const d, bool busy)
{
    FetchJob * const job = s_job;

    if ( (uint8_t)KODE_OK != d->status )
    {
        fetch_abort((kode_status_t)d->status, false);
    }
    else if ( d->eof || (0U == d->len) )
    {
        fetch_abort(KODE_OK, false);
    }
    else
    {
        job->holding   = true;
        job->held_idx  = d->idx;
        job->held_len  = d->len;
        job->waited_ms = 0U;
        (void)QTimeEvt_disarm(&s_stall);
        fetch_write_held(busy);
    }
}

/**
 * @brief The asker of a download wants it ended.
 *
 * While the open is out the stop waits for AO_Net's answer, which is then
 * this download's and cannot be taken for the next one's.
 *
 * @param who Who asks.
 */
static void fetch_stop(QActive const * const who)
{
    if ( (NULL == s_job) || (s_job->who != who) )
    {
        return;
    }
    if ( FETCH_OPENING == s_job->phase )
    {
        s_job->stopping = true;
    }
    else
    {
        fetch_abort(KODE_OK, true);
    }
}

bool storage_fetch_event(QEvt const * const e, bool busy)
{
    bool mine = true;

    switch ( e->sig )
    {
        case HTTP_FILE_REQ_SIG: {
            fetch_begin(Q_EVT_CAST(HttpFileReqEvt), busy);
            break;
        }
        case HTTP_FILE_STOP_SIG: {
            fetch_stop(Q_EVT_CAST(HttpFileStopEvt)->who);
            break;
        }
        case NET_STREAM_FROM_RSP_SIG: {
            if ( (NULL != s_job) && (FETCH_OPENING == s_job->phase) )
            {
                fetch_opened(Q_EVT_CAST(NetStreamFromRspEvt));
            }
            else if ( (uint8_t)KODE_OK == Q_EVT_CAST(NetStreamFromRspEvt)->status )
            {
                /* A transfer nobody here is waiting for: it is not left open. */
                net_stream_close_request();
            }
            else
            {
                /* A refusal of an open nobody is waiting for. */
            }
            break;
        }
        case NET_STREAM_DATA_SIG: {
            if ( (NULL != s_job) && (FETCH_READING == s_job->phase)
                && (!s_job->holding) )
            {
                fetch_chunk(Q_EVT_CAST(StorageDataEvt), busy);
            }
            /* Else a chunk of a download that has ended: AO_Net was told to
             * close, and forgets the buffer with the transfer. */
            break;
        }
        case HTTP_FILE_RETRY_SIG: {
            if ( (NULL != s_job) && s_job->holding )
            {
                fetch_write_held(busy);
            }
            break;
        }
        case HTTP_FILE_STALL_SIG: {
            /* The open's answer or the next chunk: neither came. */
            if ( (NULL != s_job) && (!s_job->holding) && (QTimeEvt_getCtr(&s_stall) == 0U) )
            {
                KLOG_W(TAG, "AO_Net said nothing of '%s' for %u ms", s_job->path,
                       (unsigned)KODE_HTTP_FILE_STALL_MS);
                fetch_abort(KODE_TIMEOUT, false);
            }
            break;
        }
        default: {
            mine = false;
            break;
        }
    }
    return mine;
}

bool storage_fetch_running(void)
{
    return NULL != s_job;
}
