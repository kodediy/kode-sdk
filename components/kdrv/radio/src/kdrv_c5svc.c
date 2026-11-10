/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_c5svc.c
 * @brief The P4's end of the KODE services: framing, one request per service, answers routed back.
 */
#include "kdrv_c5svc.h"

#include <stdbool.h>
#include <string.h>

#include "kdrv_radio.h"
#include "kode_log.h"
#include "kode_wire.h"
#include "khal_sync.h"
#include "khal_system.h"

#define TAG "kdrv.c5svc"

/** @brief One service's request in flight, and who hears its answer. */
typedef struct {
    kdrv_c5svc_cb_t cb;         /**< The service's listener, or NULL.      */
    void           *ctx;        /**< Handed back to cb.                    */
    bool            in_flight;  /**< A request waits for its answer.       */
    uint8_t         seq;        /**< That request's sequence number.       */
    uint8_t         op;         /**< And its op.                           */
    bool            waiting;    /**< kdrv_c5svc_call() sleeps on done.     */
    khal_sem_t      done;       /**< Given when a waited answer is copied. */
    uint8_t         w_status;   /**< The waited answer's status.           */
    uint8_t        *w_out;      /**< Where its payload goes.               */
    size_t          w_cap;      /**< Bytes available there.                */
    size_t          w_len;      /**< Bytes copied.                         */
} svc_slot_t;

static struct {
    bool         ready;                     /**< kdrv_c5svc_init() ran.    */
    khal_mutex_t state;                     /**< Guards slot[] and peer.   */
    /* Never taken on esp-hosted's thread: a send holds it while it waits on that thread. */
    khal_mutex_t tx_lock;                   /**< Guards tx across a send.  */
    uint8_t     *tx;                        /**< Header + payload, PSRAM.  */
    svc_slot_t   slot[KODE_SVC_COUNT];      /**< One per service.          */
    bool         info_ok;                   /**< info holds an answer.     */
    kode_info_t  info;                      /**< The last INFO answer.     */
} s_svc;

/**
 * @brief Copy a waited answer out and wake the caller, or hand it to the
 *        listener. Called with the state lock held; releases it.
 */
static void deliver(svc_slot_t *slot, const kode_hdr_t *h, const uint8_t *pay)
{
    if ( slot->waiting )
    {
        size_t n = (size_t)h->len;
        if ( n > slot->w_cap )
        {
            n = slot->w_cap;
        }
        if ( 0U < n )
        {
            (void)memcpy(slot->w_out, pay, n);
        }
        slot->w_len    = n;
        slot->w_status = h->status;
        slot->waiting  = false;
        khal_mutex_unlock(s_svc.state);
        khal_sem_signal(slot->done);
    }
    else
    {
        kdrv_c5svc_cb_t const cb  = slot->cb;
        void           *const ctx = slot->ctx;
        khal_mutex_unlock(s_svc.state);
        if ( NULL != cb )
        {
            cb(h->service, h->op, h->status, pay, (size_t)h->len, ctx);
        }
    }
}

/**
 * @brief Hand an event to its service's listener; with none, drop it quietly.
 *        The service is range-checked by the caller.
 */
static void deliver_event(const kode_hdr_t *h, const uint8_t *pay)
{
    khal_mutex_lock(s_svc.state);
    kdrv_c5svc_cb_t const cb  = s_svc.slot[h->service].cb;
    void           *const ctx = s_svc.slot[h->service].ctx;
    khal_mutex_unlock(s_svc.state);
    if ( NULL == cb )
    {
        KLOG_D(TAG, "an event for service %u op %u has no listener",
               (unsigned)h->service, (unsigned)h->op);
        return;
    }
    cb(h->service, h->op, h->status, pay, (size_t)h->len, ctx);
}

/** @brief Every answer and event from the C5, on esp-hosted's thread. */
static void on_peer(const uint8_t *data, size_t len, void *ctx)
{
    kode_hdr_t h;

    (void)ctx;
    if ( (NULL == data) || (sizeof(h) > len) )
    {
        KLOG_W(TAG, "an answer of %u bytes is shorter than its header",
               (unsigned)len);
        return;
    }
    /* cppcheck-suppress misra-c2012-21.15 ; the wire is bytes; kode_hdr_t is packed and its size asserted */
    (void)memcpy(&h, data, sizeof(h));
    if ( KODE_WIRE_VERSION != h.version )
    {
        KLOG_W(TAG, "the C5 speaks KODE wire v%u, this build v%u",
               (unsigned)h.version, (unsigned)KODE_WIRE_VERSION);
        return;
    }
    if ( (KODE_SVC_COUNT <= h.service) || (((size_t)h.len + sizeof(h)) != len) )
    {
        KLOG_W(TAG, "a malformed answer (service %u, %u of %u bytes)",
               (unsigned)h.service, (unsigned)h.len, (unsigned)len);
        return;
    }

    if ( 0U != (h.op & KODE_OP_EVENT) )
    {
        /* Unasked: the C5 says it, whether or not a request is out. It is
         * never an answer, so it neither completes nor is judged by one. */
        deliver_event(&h, &data[sizeof(h)]);
        return;
    }

    khal_mutex_lock(s_svc.state);
    svc_slot_t *const slot = &s_svc.slot[h.service];
    if ( (!slot->in_flight) || (slot->seq != h.seq) || (slot->op != h.op) )
    {
        khal_mutex_unlock(s_svc.state);
        KLOG_W(TAG, "a late answer for service %u op %u dropped",
               (unsigned)h.service, (unsigned)h.op);
        return;
    }
    slot->in_flight = false;
    if ( (KODE_SVC_SELF == h.service) && (KODE_SELF_INFO == h.op)
        && (KODE_ST_OK == h.status) && (sizeof(kode_info_t) <= h.len) )
    {
        /* cppcheck-suppress misra-c2012-21.15 ; the wire is bytes; kode_info_t is packed and its size asserted */
        (void)memcpy(&s_svc.info, &data[sizeof(h)], sizeof(s_svc.info));
        s_svc.info_ok      = true;
    }
    deliver(slot, &h, &data[sizeof(h)]);
}

kode_status_t kdrv_c5svc_init(void)
{
    if ( s_svc.ready )
    {
        return KODE_OK;
    }
    s_svc.state   = khal_mutex_create();
    s_svc.tx_lock = khal_mutex_create();
    /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the buffer is bytes */
    s_svc.tx      = (uint8_t *)khal_alloc_psram(4U,
                        sizeof(kode_hdr_t) + (size_t)KODE_MAX_PAYLOAD);
    if ( (NULL == s_svc.state) || (NULL == s_svc.tx_lock) || (NULL == s_svc.tx) )
    {
        return KODE_NO_RESOURCES;
    }
    for ( size_t i = 0U; (size_t)KODE_SVC_COUNT > i; i++ )
    {
        s_svc.slot[i].done = khal_sem_create();
        if ( NULL == s_svc.slot[i].done )
        {
            return KODE_NO_RESOURCES;
        }
    }
    if ( kdrv_radio_peer_listen(KODE_MSG_RSP, &on_peer, NULL, NULL) != KODE_OK )
    {
        return KODE_NO_RESOURCES;
    }
    s_svc.ready = true;
    return KODE_OK;
}

kode_status_t kdrv_c5svc_listen(uint8_t service, kdrv_c5svc_cb_t cb, void *ctx)
{
    if ( (!s_svc.ready) || (KODE_SVC_COUNT <= service) )
    {
        return KODE_DENIED;
    }
    khal_mutex_lock(s_svc.state);
    s_svc.slot[service].cb  = cb;
    s_svc.slot[service].ctx = ctx;
    khal_mutex_unlock(s_svc.state);
    return KODE_OK;
}

kode_status_t kdrv_c5svc_send(uint8_t service, uint8_t op,
                              const void *payload, size_t len)
{
    if ( !s_svc.ready )
    {
        return KODE_NO_RESOURCES;
    }
    if ( (KODE_SVC_COUNT <= service) || ((size_t)KODE_MAX_PAYLOAD < len)
        || ((NULL == payload) && (0U < len)) )
    {
        return KODE_DENIED;
    }

    khal_mutex_lock(s_svc.state);
    svc_slot_t *const slot = &s_svc.slot[service];
    if ( slot->in_flight )
    {
        khal_mutex_unlock(s_svc.state);
        return KODE_DENIED;
    }
    slot->seq++;
    slot->op        = op;
    slot->in_flight = true;
    kode_hdr_t const h = { KODE_WIRE_VERSION, service, op, slot->seq,
                           KODE_ST_OK, 0U, (uint16_t)len };
    khal_mutex_unlock(s_svc.state);

    khal_mutex_lock(s_svc.tx_lock);
    /* cppcheck-suppress misra-c2012-21.15 ; the wire is bytes; kode_hdr_t is packed and its size asserted */
    (void)memcpy(s_svc.tx, &h, sizeof(h));
    if ( 0U < len )
    {
        (void)memcpy(&s_svc.tx[sizeof(h)], payload, len);
    }
    kode_status_t const rc = kdrv_radio_peer_send(KODE_MSG_REQ, s_svc.tx,
                                                  sizeof(h) + len);
    /* The request may have been a password or a key: it does not stay. */
    (void)memset(s_svc.tx, 0, sizeof(h) + len);
    khal_mutex_unlock(s_svc.tx_lock);

    if ( KODE_OK != rc )
    {
        khal_mutex_lock(s_svc.state);
        slot->in_flight = false;
        khal_mutex_unlock(s_svc.state);
    }
    return rc;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by kdrv_ble.c, under CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
kode_status_t kdrv_c5svc_call(uint8_t service, uint8_t op,
                              const void *payload, size_t len,
                              uint8_t *status, void *out, size_t cap,
                              size_t *out_len, uint32_t timeout_ms)
{
    if ( (NULL == status) || (NULL == out_len) || ((NULL == out) && (0U < cap)) )
    {
        return KODE_DENIED;
    }
    if ( !s_svc.ready )
    {
        return KODE_NO_RESOURCES;
    }
    if ( KODE_SVC_COUNT <= service )
    {
        return KODE_DENIED;
    }
    svc_slot_t *const slot = &s_svc.slot[service];

    /* A token left by an answer nobody waited for must not end this wait. */
    (void)khal_sem_wait_timeout(slot->done, 0U);
    khal_mutex_lock(s_svc.state);
    if ( slot->in_flight )
    {
        /* Another request of this service is out: its answer must not end
         * this wait, so this call is refused before it waits at all. */
        khal_mutex_unlock(s_svc.state);
        return KODE_DENIED;
    }
    slot->waiting = true;
    /* cppcheck-suppress misra-c2012-11.5 ; the caller's buffer, written as bytes */
    slot->w_out   = (uint8_t *)out;
    slot->w_cap   = cap;
    slot->w_len   = 0U;
    khal_mutex_unlock(s_svc.state);

    kode_status_t const rc = kdrv_c5svc_send(service, op, payload, len);
    if ( KODE_OK != rc )
    {
        khal_mutex_lock(s_svc.state);
        slot->waiting = false;
        khal_mutex_unlock(s_svc.state);
        return rc;
    }
    if ( !khal_sem_wait_timeout(slot->done, timeout_ms) )
    {
        khal_mutex_lock(s_svc.state);
        slot->waiting   = false;
        slot->in_flight = false;
        khal_mutex_unlock(s_svc.state);
        KLOG_W(TAG, "service %u op %u: no answer in %u ms",
               (unsigned)service, (unsigned)op, (unsigned)timeout_ms);
        return KODE_TIMEOUT;
    }
    *status  = slot->w_status;
    *out_len = slot->w_len;
    return KODE_OK;
}

void kdrv_c5svc_abandon(uint8_t service)
{
    if ( (!s_svc.ready) || (KODE_SVC_COUNT <= service) )
    {
        return;
    }
    khal_mutex_lock(s_svc.state);
    s_svc.slot[service].in_flight = false;
    khal_mutex_unlock(s_svc.state);
}

void kdrv_c5svc_forget(void)
{
    if ( !s_svc.ready )
    {
        return;
    }
    khal_mutex_lock(s_svc.state);
    for ( size_t i = 0U; (size_t)KODE_SVC_COUNT > i; i++ )
    {
        s_svc.slot[i].in_flight = false;
    }
    s_svc.info_ok      = false;
    khal_mutex_unlock(s_svc.state);
}

bool kdrv_c5svc_info(kode_info_t *out)
{
    bool ok = false;

    if ( (NULL == out) || (!s_svc.ready) )
    {
        return false;
    }
    khal_mutex_lock(s_svc.state);
    if ( s_svc.info_ok )
    {
        (void)memcpy(out, &s_svc.info, sizeof(*out));
        ok = true;
    }
    khal_mutex_unlock(s_svc.state);
    return ok;
}
