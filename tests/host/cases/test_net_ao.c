/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_net_ao.c
 * @brief AO_Net: what happens to a request when there is no network.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"
#include "Mockkdrv_net.h"
#include "Mockkdrv_slot.h"

#include "ao_spy.h"
#include "ask_support.h"
#include "config_support.h"
#include "fake_khal_worker.h"
#include "fakes.h"
#include "kaos.h"
#include "kaos_wire.h"
#include "kdrv_wifi.h"
#include "khal_identity.h"
#include "khal_nvs.h"
#include "kode_sys.h"
#include "khal_time.h"
#include "kode_wire.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "net_channel.h"
#include "kset.h"
#include "lowpower_cases.h"
#include "qf_support.h"
#include "sdkconfig.h"

#if !CONFIG_KODEOS_CHANNEL || (CONFIG_KODEOS_CHANNEL_RENEW_S != 120)
#error "test_net_ao.c needs CONFIG_KODEOS_CHANNEL=1 and CONFIG_KODEOS_CHANNEL_RENEW_S=120 (main/CMakeLists.txt)"
#endif
#if !CONFIG_KODEOS_SHOT_HTTP
#error "test_net_ao.c needs CONFIG_KODEOS_SHOT_HTTP=1 (main/CMakeLists.txt)"
#endif

static void go_online(void);
static void go_offline(void);
static void answer_end(uint8_t st, uint16_t len, uint16_t unit_len);
static void radio_drain(void);

/* ---- The worker, played from here -----------------------------------------
 * AO_Net hands its jobs to two KqpWorkers - one for the six kinds kdrv_net
 * has, one for the download - and under test a worker is a queue with no task
 * (fake_khal_worker.c): a job waits until pull_jobs() runs it on this thread,
 * and what it calls - kdrv_net_run() or one of the stream calls - is the mock.
 * Every dispatch, tick and settle below ends in pull_jobs(), so a job the AO
 * has just submitted has run by the time the case looks, which is the order
 * the old submit stub gave it.
 *
 * Two modes. In the first, the early cases', a job's done event is delivered
 * the moment the job has run, with the status the mock returned. In the
 * second, the channel and worker cases', the done event is held back and the
 * case delivers it - job_done() for the first worker, deliver_stream() for the
 * download's - saying what the job came to: the job is "still inside the
 * worker" until then, and the AO is busy (sbusy).
 */

/** @brief The name AO_Net gives its first worker. */
#define WORKER_NAME  "net_worker"

/** @brief The name AO_Net gives the download's worker. */
#define STREAM_WORKER_NAME  "net_stream"

/** @brief What kdrv_net_run() answers. */
static kode_status_t s_run_rc = KODE_OK;

/** @brief Whether a job's done event is delivered as soon as it has run. */
static bool          s_auto;

/** @brief The done event of the job the first worker is "still inside", if any. */
static const QEvt   *s_held;

/** @brief The same for the download's worker. */
static const QEvt   *s_held_s;

/** @brief The kdrv_net_job_t that job was, or -1 for a stream job. */
static int           s_ran_job = -1;

/** @brief Take the done event with signal @p sig out of AO_Net's queue, whatever else is in it. */
static const QEvt *take_done(enum_t sig)
{
    const QEvt *found = NULL;
    UBaseType_t n     = uxQueueMessagesWaiting(AO_Net->eQueue);

    for ( UBaseType_t i = 0U; i < n; ++i )
    {
        const QEvt *e = NULL;

        TEST_ASSERT_TRUE(xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE);
        if ( (NULL == found) && ((QSignal)sig == e->sig) )
        {
            found = e;
        }
        else
        {
            TEST_ASSERT_TRUE(xQueueSend(AO_Net->eQueue, &e, 0U) == pdTRUE);
        }
    }
    return found;
}

/** @brief Run every job the AO has handed either worker. */
static void pull_jobs(void)
{
    while ( (fake_khal_worker_pending_for(WORKER_NAME) > 0U)
           || (fake_khal_worker_pending_for(STREAM_WORKER_NAME) > 0U) )
    {
        if ( fake_khal_worker_pending_for(WORKER_NAME) > 0U )
        {
            if ( !s_auto )
            {
                TEST_ASSERT_NULL_MESSAGE(s_held,
                    "a second job reached a worker that is still inside one");
            }
            s_ran_job = -1;
            TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(WORKER_NAME));
            if ( s_auto )
            {
                (void)qf_support_settle(AO_Net);
            }
            else
            {
                s_held = take_done(NET_JOB_SIG);
                TEST_ASSERT_NOT_NULL_MESSAGE(s_held,
                    "the worker's done event was not queued");
            }
        }
        if ( fake_khal_worker_pending_for(STREAM_WORKER_NAME) > 0U )
        {
            if ( !s_auto )
            {
                TEST_ASSERT_NULL_MESSAGE(s_held_s,
                    "a second job reached the download's worker while it is "
                    "still inside one");
            }
            TEST_ASSERT_TRUE(fake_khal_worker_run_next_for(STREAM_WORKER_NAME));
            if ( s_auto )
            {
                (void)qf_support_settle(AO_Net);
            }
            else
            {
                s_held_s = take_done(NET_STREAM_JOB_SIG);
                TEST_ASSERT_NOT_NULL_MESSAGE(s_held_s,
                    "the download's done event was not queued");
            }
        }
    }
}

/**
 * @brief Hand AO_Net the done event held back, carrying @p rc, and let the
 *        worker run what that made it submit.
 *
 * @param rc The status the job "returned".
 */
static void deliver_held(kode_status_t rc)
{
    KqpWorkerDoneEvt *d;

    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "no job is with the worker");
    d      = (KqpWorkerDoneEvt *)(void *)s_held;
    s_held = NULL;
    d->status = (uint8_t)rc;
    QASM_DISPATCH(AO_Net, &d->super, 0U);
    QF_gc(&d->super);
    pull_jobs();
}

/**
 * @brief Hand AO_Net the download's done event held back, carrying @p rc, and
 *        let the workers run what that made it submit.
 *
 * @param rc The status the job "returned".
 */
static void deliver_stream(kode_status_t rc)
{
    KqpWorkerDoneEvt *d;

    TEST_ASSERT_NOT_NULL_MESSAGE(s_held_s, "no job is with the download's worker");
    d        = (KqpWorkerDoneEvt *)(void *)s_held_s;
    s_held_s = NULL;
    d->status = (uint8_t)rc;
    QASM_DISPATCH(AO_Net, &d->super, 0U);
    QF_gc(&d->super);
    pull_jobs();
}

/** @brief Dispatch @p e to AO_Net, then let the workers run what that submitted. */
static void net_dispatch(const QEvt *e)
{
    QASM_DISPATCH(AO_Net, e, 0U);
    pull_jobs();
}

/** @brief Advance @p steps 10 ms steps of QF time, then run the jobs. */
static unsigned net_tick(unsigned steps)
{
    unsigned const n = qf_support_tick(AO_Net, steps);

    pull_jobs();
    return n;
}

/** @brief Dispatch what waits for AO_Net, then run the jobs. */
static unsigned net_settle(void)
{
    unsigned const n = qf_support_settle(AO_Net);

    pull_jobs();
    return n;
}

/** @brief Put AO_Net back to a freshly-booted, offline state. */
/** @brief The callback AO_Net gave the driver: the cases play the driver with it. */
static kdrv_net_mqtt_cb_t s_mqtt_cb;

/** @brief The driver's receive ring, played from here: what the broker sent and AO_Net has not taken. */
#define RXQ_MAX  64U
static kdrv_mqtt_msg_t s_rxq[RXQ_MAX];
static unsigned        s_rxq_head;
static unsigned        s_rxq_len;
static unsigned        s_rxq_takes;   /**< Calls to kdrv_net_mqtt_rx_take(), the empty answer included. */

static bool stub_rx_take(kdrv_mqtt_msg_t *out, int calls)
{
    bool taken = false;

    (void)calls;
    s_rxq_takes++;
    if ( 0U != s_rxq_len )
    {
        *out     = s_rxq[s_rxq_head];
        s_rxq_head = (s_rxq_head + 1U) % RXQ_MAX;
        s_rxq_len--;
        taken = true;
    }
    return taken;
}

/** @brief The driver keeps a message (no notice). */
static void rxq_push(const kdrv_mqtt_msg_t *m)
{
    s_rxq[(s_rxq_head + s_rxq_len) % RXQ_MAX] = *m;
    s_rxq_len++;
}

static unsigned        s_rx_owes;     /**< Calls to kdrv_net_mqtt_rx_owe(). */

static void stub_rx_owe(int calls)
{
    (void)calls;
    s_rx_owes++;
}

static void stub_set_cb(kdrv_net_mqtt_cb_t cb, int calls)
{
    (void)calls;
    s_mqtt_cb = cb;
}

/* The time client, as the driver sees it: how often it was started and how
 * often asked again since the last Net_ctor(), and what it answers. */
static unsigned      s_sntp_starts;
static unsigned      s_sntp_asks;
static kode_status_t s_sntp_start_rc = KODE_OK;

static kode_status_t stub_sntp_start(void (*on_sync)(uint64_t unix_s), int calls)
{
    (void)calls;
    TEST_ASSERT_NOT_NULL(on_sync);
    ++s_sntp_starts;
    return s_sntp_start_rc;
}

static kode_status_t stub_sntp_sync_now(int calls)
{
    (void)calls;
    ++s_sntp_asks;
    return KODE_OK;
}

/** @brief The MAC the unit under test takes from its C5: a documentation address. */
static const uint8_t NET_UNIT_MAC[KHAL_ID_MAC_LEN] = { 0x00U, 0x00U, 0x5EU, 0x00U, 0x53U, 0x01U };

static void net_fresh(void)
{
    static bool s_registered;

    qf_support_init();
    /* The serial the channel checks the api's answer against is the one the
     * identity holds, and that is whatever an earlier file left there. Say it. */
    (void)khal_identity_set_mac(NET_UNIT_MAC);
    (void)ao_spy();
    ao_spy_drain();
    /* Reaching an address makes AO_Net ask the C5 for the unit's certificate,
     * and that request lands in AO_Radio's queue, which is a mailbox here. */
    qf_support_give_queue(AO_Radio);
    radio_drain();

    /* The initial transition starts the AO's worker and asks the radio where
     * it stands. Neither answer matters here; what matters is that the AO
     * lands in Net_offline, which is what a board with no join does. */
    s_auto    = true;
    s_run_rc  = KODE_OK;
    kdrv_net_mqtt_set_cb_Stub(&stub_set_cb);
    kdrv_net_mqtt_rx_take_Stub(&stub_rx_take);
    kdrv_net_mqtt_rx_owe_Stub(&stub_rx_owe);
    s_rxq_head = 0U; s_rxq_len = 0U; s_rxq_takes = 0U; s_rx_owes = 0U;
    kdrv_net_mqtt_rx_dropped_IgnoreAndReturn(0U);
    kdrv_net_info_IgnoreAndReturn(KODE_ERROR);
    kdrv_net_stream_close_Ignore();
    /* Reaching an address also speaks to the time client, on every entry to
     * Net_online: the first starts it, the rest ask it again. Counted rather
     * than expected per case: only the cases about it look, and the rest of
     * this file is about the state machine that surrounds it. */
    s_sntp_starts = 0U;
    s_sntp_asks   = 0U;
    kdrv_net_sntp_start_Stub(&stub_sntp_start);
    kdrv_net_sntp_sync_now_Stub(&stub_sntp_sync_now);
    /* Every way out of the channel tells the driver to drop its copy of the
     * session; the cases that look at sessions stub it again. */
    kdrv_net_mqtt_session_IgnoreAndReturn(KODE_OK);

    qf_support_give_queue(AO_Net);
    /* The console files before this one end lines of source NET, and each
     * end is a post to AO_Net, which nobody dispatched: the first tick below
     * would hand them to an AO that was never started. */
    {
        const QEvt *stale = NULL;

        while ( xQueueReceive(AO_Net->eQueue, &stale, 0U) == pdTRUE )
        {
            QF_gc(stale);
        }
    }
    /* A case that ended with a job still inside the worker leaves its guard
     * armed, and Net_ctor() over an armed time event links it twice. The job
     * is answered - as a failure, which no step reads a table for - and so is
     * every one that answer makes the AO submit, so the guard is disarmed and
     * the next tick unlinks it. */
    if ( NULL != s_held )
    {
        deliver_held(KODE_ERROR);
    }
    if ( NULL != s_held_s )
    {
        deliver_stream(KODE_ERROR);
    }
    pull_jobs();
    (void)net_tick(2U);   /* see net_done() */
    Net_ctor();
    AO_Net->prio  = KODE_PRIO_NET;
    AO_Net->pthre = KODE_PRIO_NET;

    if ( !s_registered )
    {
        QActive_register_(AO_Net);
        s_registered = true;
    }
    QASM_INIT(AO_Net, (void *)0, 0U);

    /* Net_ctor() resets the MQTT phase but not the stream loan - correct for
     * production, where the constructor runs once on a board that has just
     * powered up. Here it means a transfer a previous case left open would
     * still be open, so the slate is wiped through the AO's own front door:
     * online, close, offline. The close is a no-op when nothing is open. */
    kdrv_net_stream_close_Ignore();
    kdrv_net_run_IgnoreAndReturn(KODE_OK);
    go_online();
    {
        static const QEvt close_evt = QEVT_INITIALIZER(NET_STREAM_CLOSE_REQ_SIG);
        net_dispatch(&close_evt);
    }
    go_offline();
    /* The pair above asked the C5 for a certificate nobody will send: say
     * the C5 refused, so the next case does not inherit a request in flight. */
    answer_end((uint8_t)KODE_ST_REFUSED, 0U, 0U);
    radio_drain();
    ao_spy_drain();
}

/** @brief The ms one step of the harness's clock is (net_tick(1U)). */
#define LINE_WAIT_STEP_MS  10U

/** @brief Leave AO_Net offline and AO_Radio's mailbox empty, for the next file. */
static void net_done(void)
{
    go_offline();
    if ( NULL != s_held )
    {
        deliver_held(KODE_ERROR);   /* the guard comes down with the job */
    }
    if ( NULL != s_held_s )
    {
        deliver_stream(KODE_ERROR);
    }
    answer_end((uint8_t)KODE_ST_REFUSED, 0U, 0U);
    radio_drain();
    ao_spy_drain();
    /* QP unlinks a disarmed time event on the NEXT tick, not when it is
     * disarmed. A constructor run over one that is still linked, and an arm
     * after it, link the event twice and the tick walks a cycle for ever. The
     * bound on a command line the console never answered may be armed: it
     * runs out here, with the channel gone, so nothing is sent on. */
    (void)net_tick((KODE_CHAN_LINE_BOUND_MS / LINE_WAIT_STEP_MS) + 2U);
    ao_spy_drain();
}

/** @brief Tell AO_Net the radio has an address, so it goes online. */
static void go_online(void)
{
    WifiStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(WIFI_STATE_SIG);
    e.state = (uint8_t)KDRV_WIFI_GOT_IP;
    e.ip4   = 0x0100A8C0U;   /* 192.168.0.1, any non-zero address */

    net_dispatch(&e.super);
}

/** @brief Tell AO_Net the address is gone. */
static void go_offline(void)
{
    WifiStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(WIFI_STATE_SIG);
    e.state = (uint8_t)KDRV_WIFI_DISCONNECTED;

    net_dispatch(&e.super);
}

/** @brief Ask for a transfer over HTTP, from the spy. */
static void ask_stream_open(const char *url)
{
    NetStreamOpenEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(NET_STREAM_OPEN_REQ_SIG);
    (void)snprintf(e.url, sizeof(e.url), "%s", url);
    e.who = ao_spy();

    net_dispatch(&e.super);
}

/* ---- The time client ------------------------------------------------------ */

/* The first address starts the client, and every one after it asks again: a
 * clock restored from a kept value may be off, and lwIP's own schedule would
 * wait up to an hour. net_fresh() runs Net_ctor(), so a flag that outlives the
 * constructor is caught by the first assert. */
TEST_CASE("the first address starts the time client and each new one asks it again",
          "[net_ao]")
{
    net_fresh();    /* its own online is the first address since the ctor */
    TEST_ASSERT_EQUAL_UINT(1U, s_sntp_starts);
    TEST_ASSERT_EQUAL_UINT(0U, s_sntp_asks);

    go_online();
    TEST_ASSERT_EQUAL_UINT(1U, s_sntp_starts);
    TEST_ASSERT_EQUAL_UINT(1U, s_sntp_asks);

    go_offline();
    go_online();
    TEST_ASSERT_EQUAL_UINT(1U, s_sntp_starts);
    TEST_ASSERT_EQUAL_UINT(2U, s_sntp_asks);

    net_done();
}

/* A client that would not start does not exist, and sync_now would be refused:
 * the next address starts it, it does not ask it. */
TEST_CASE("a time client that would not start is started again by the next address",
          "[net_ao]")
{
    s_sntp_start_rc = KODE_ERROR;
    net_fresh();
    s_sntp_start_rc = KODE_OK;
    TEST_ASSERT_EQUAL_UINT(1U, s_sntp_starts);

    go_online();
    TEST_ASSERT_EQUAL_UINT(2U, s_sntp_starts);
    TEST_ASSERT_EQUAL_UINT(0U, s_sntp_asks);

    go_offline();
    go_online();
    TEST_ASSERT_EQUAL_UINT(2U, s_sntp_starts);
    TEST_ASSERT_EQUAL_UINT(1U, s_sntp_asks);

    net_done();
}

/* ---- The rule ------------------------------------------------------------- */

/* The case the audit found. NET_STREAM_* used to be handled only in
 * Net_online, so offline they fell through to QHsm_top and vanished. Both
 * callers - the app manager and the radio's own updater - wait on the
 * response with no deadline of their own, so the request did not fail: it
 * simply never came back. */
TEST_CASE("a transfer asked for with no network is refused, not swallowed",
          "[net_ao]")
{
    net_fresh();

    ask_stream_open("http://example.invalid/app.bin");

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "the request was ignored, not refused");
    TEST_ASSERT_EQUAL_INT(NET_STREAM_OPEN_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)((const StorageOpenRspEvt *)e)->status);

    net_done();
}

/* Offline, every request is answered and none of them reaches the driver.
 * That second half is the state's whole reason to exist: there is no address,
 * so asking the network anything would block on a socket that cannot open. */
TEST_CASE("offline, a ping is refused without touching the network",
          "[net_ao]")
{
    NetPingReqEvt e;

    net_fresh();

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(NET_PING_REQ_SIG);
    (void)snprintf(e.host, sizeof(e.host), "%s", "example.invalid");
    e.count = 1U;

    /* No kdrv_net_run expectation: reaching the driver fails this case. */
    net_dispatch(&e.super);

    /* It answered with an end marker, which is what closes `net_ping`. */
    TEST_ASSERT_GREATER_THAN_UINT(0U, ao_spy_count() + 1U);

    Mockkdrv_net_Verify();

    net_done();
}

/* ---- Losing the network mid-transfer --------------------------------------- */

/* The other half of the same finding. Net_online had no exit action, so
 * leaving it left `sopen` set with nobody holding the other end - and every
 * later transfer was then refused with "a transfer is already open" for the
 * rest of the boot. One dropped WiFi connection disabled firmware updates
 * until somebody power-cycled the board. */
TEST_CASE("losing the network closes an open transfer instead of stranding it",
          "[net_ao]")
{
    net_fresh();
    go_online();

    /* Open a transfer while there is an address. */
    kdrv_net_stream_open_IgnoreAndReturn(KODE_OK);
    ask_stream_open("http://example.invalid/app.bin");
    ao_spy_drain();

    /* The address goes. Leaving Net_online must tear the transfer down. */
    kdrv_net_stream_close_Ignore();
    kdrv_net_run_IgnoreAndReturn(KODE_OK);
    go_offline();

    /* Back online, a fresh transfer must be possible. Before the fix this
     * came back KODE_DENIED - forever. */
    go_online();
    kdrv_net_stream_open_IgnoreAndReturn(KODE_OK);
    ask_stream_open("http://example.invalid/other.bin");

    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(NET_STREAM_OPEN_RSP_SIG, (int)e->sig);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK,
        (int)((const StorageOpenRspEvt *)e)->status,
        "the previous transfer was never closed, so this one is refused");

    net_done();
}

/* ---- The transition itself -------------------------------------------------- */

/* An address of zero is not an address. The guard is an `&&` and it is
 * load-bearing: the radio reports GOT_IP before the lease is complete, and
 * going online on that would open sockets that cannot bind. */
TEST_CASE("GOT_IP without an address does not bring the network up",
          "[net_ao]")
{
    WifiStateEvt e;

    net_fresh();

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(WIFI_STATE_SIG);
    e.state = (uint8_t)KDRV_WIFI_GOT_IP;
    e.ip4   = 0U;

    net_dispatch(&e.super);

    /* Still offline: a transfer is refused rather than attempted. */
    ask_stream_open("http://example.invalid/x.bin");

    const QEvt * const rsp = ao_spy_take();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)((const StorageOpenRspEvt *)rsp)->status);

    net_done();
}

/* A second address while already online is a re-lease, not a new state: the
 * AO stays where it is rather than tearing everything down and building it
 * again. */
TEST_CASE("a new address while online does not restart the state",
          "[net_ao]")
{
    net_fresh();
    go_online();

    kdrv_net_stream_open_IgnoreAndReturn(KODE_OK);
    ask_stream_open("http://example.invalid/app.bin");
    ao_spy_drain();

    /* Another GOT_IP. If this left and re-entered Net_online, the exit action
     * would tear the transfer down and the open below would succeed. */
    go_online();

    ask_stream_open("http://example.invalid/second.bin");
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED,
        (int)((const StorageOpenRspEvt *)e)->status,
        "the transfer was torn down by a re-lease");

    net_done();
}

/* ---- Refusing a second transfer -------------------------------------------- */

/* One transfer at a time, and the second is refused *with an answer*. The
 * driver keeps one HTTP handle, so this is the same shape as AO_Storage's
 * single file - and the same rule about answering applies. */
TEST_CASE("a second transfer is refused while one is open", "[net_ao]")
{
    net_fresh();
    go_online();

    kdrv_net_stream_open_IgnoreAndReturn(KODE_OK);
    ask_stream_open("http://example.invalid/first.bin");

    const QEvt * const first = ao_spy_take();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        (int)((const StorageOpenRspEvt *)first)->status);

    ask_stream_open("http://example.invalid/second.bin");

    const QEvt * const second = ao_spy_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(second, "the second request went unanswered");
    TEST_ASSERT_EQUAL_INT(KODE_DENIED,
        (int)((const StorageOpenRspEvt *)second)->status);

    net_done();
}

/* Closing gives the loan back, so the next transfer can have it. */
TEST_CASE("closing a transfer frees it for the next one", "[net_ao]")
{
    static const QEvt close_evt = QEVT_INITIALIZER(NET_STREAM_CLOSE_REQ_SIG);

    net_fresh();
    go_online();

    kdrv_net_stream_open_IgnoreAndReturn(KODE_OK);
    kdrv_net_stream_close_Ignore();

    ask_stream_open("http://example.invalid/first.bin");
    ao_spy_drain();

    net_dispatch(&close_evt);

    ask_stream_open("http://example.invalid/second.bin");
    const QEvt * const e = ao_spy_take();
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_EQUAL_INT(KODE_OK,
        (int)((const StorageOpenRspEvt *)e)->status);

    net_done();
}

/* A close arriving with no network is a straggler from a transfer the offline
 * transition already tore down. It has to be swallowed rather than left to
 * QHsm_top, where nothing knows there is nothing to close. */
TEST_CASE("a stray close with no network is harmless", "[net_ao]")
{
    static const QEvt close_evt = QEVT_INITIALIZER(NET_STREAM_CLOSE_REQ_SIG);

    net_fresh();

    net_dispatch(&close_evt);

    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    net_done();
}

/* ======================================================================== *
 * The Kode channel
 *
 * AO_Net joins the channel by itself whenever it has an address. The worker,
 * the C5 and the broker are all played from here: every kdrv_net call is
 * recorded by a stub, the C5's answers are dispatched by hand, and the
 * clock is a loop over QP ticks, one second of simulated time each.
 * ======================================================================== */

#define REC_MAX   16U
#define BODY_MAX  1100U

static kdrv_net_req_t       s_reqs[REC_MAX];
static unsigned             s_nreq;
static char                 s_bodies[REC_MAX][BODY_MAX];
static unsigned             s_nbody;
static kdrv_mqtt_session_t  s_sess;
static unsigned             s_nsess;
static struct {
    char     topic[96];
    char     data[260];
    uint16_t len;
    bool     retain;
    unsigned jobs;     /**< Jobs submitted when it was published.         */
} s_pubs[8];
static unsigned             s_npub;
static char                 s_subs[8][96];
static unsigned             s_nsub;
static unsigned             s_nunsub;
static uint16_t             s_http_status;
static char                 s_http_body[1700];
static kode_status_t        s_publish_rc;
/* What went out on cmd/out, in order: a line counts only when the stub said
 * it was accepted. */
#define OUT_MAX  400U
static char                 s_outs[OUT_MAX][24];
static unsigned             s_nout;
/* What the api names in the connect answer: the token is 379 of this byte,
 * and `shots` is left out when asked. */
static char                 s_token_fill;
static bool                 s_no_shots;
static unsigned             s_nresult;   /**< kdrv_net_http_result() calls. */
static unsigned             s_nforget;   /**< kdrv_net_http_forget() calls. */
/* An upload: what kdrv_net_http_put_prepare() was handed, and whether the
 * display's buffer had been given back when the PUT job went out. */
static struct {
    char   url[KDRV_NET_PUT_URL_MAX];
    size_t len;
    char   type[32];
    char   bearer[KDRV_MQTT_TOKEN_MAX];
} s_put;
static unsigned             s_nput;
static kode_status_t        s_put_rc;
static bool                 s_put_released;

/* Every job's kind and keep flag, in order, past the REC_MAX whole copies: a
 * stream submits hundreds. */
#define JOB_LOG_MAX  1024U
static uint8_t              s_job_type[JOB_LOG_MAX];
static bool                 s_job_keep[JOB_LOG_MAX];

static bool display_end_waiting(void);

/** @brief kdrv_net_run(): records the job the worker has run. */
static kode_status_t stub_run(const kdrv_net_req_t *req, int n)
{
    (void)n;
    if ( REC_MAX > s_nreq )
    {
        s_reqs[s_nreq] = *req;
    }
    if ( JOB_LOG_MAX > s_nreq )
    {
        s_job_type[s_nreq] = (uint8_t)req->type;
        s_job_keep[s_nreq] = req->keep;
    }
    s_nreq++;
    if ( KDRV_NET_JOB_HTTP_PUT == req->type )
    {
        s_put_released = display_end_waiting();
    }
    s_ran_job = (int)req->type;
    return s_run_rc;
}

static kode_status_t stub_put_prepare(const char *url, const uint8_t *body,
                                      size_t len, const char *content_type,
                                      const char *bearer, int n)
{
    (void)n;
    (void)body;   /* display_shot_buf(): nothing is lent in this binary */
    (void)snprintf(s_put.url, sizeof(s_put.url), "%s", url);
    s_put.len = len;
    (void)snprintf(s_put.type, sizeof(s_put.type), "%s", content_type);
    (void)snprintf(s_put.bearer, sizeof(s_put.bearer), "%s", bearer);
    s_nput++;
    return s_put_rc;
}

static void stub_forget(int n)
{
    (void)n;
    s_nforget++;
}

static kode_status_t stub_post_body(const char *json, size_t len, int n)
{
    (void)n;
    if ( REC_MAX > s_nbody )
    {
        (void)snprintf(s_bodies[s_nbody], BODY_MAX, "%.*s", (int)len, json);
    }
    s_nbody++;
    return KODE_OK;
}

/* What the driver says about the client: the helpers below move it the way the
 * real one moves (up on CONNECTED, down on a drop or a stop), and a case moves
 * it by hand to play a link event that was lost. */
static kdrv_mqtt_state_t    s_drv_state;

static kdrv_mqtt_state_t stub_mqtt_state(int n)
{
    (void)n;
    return s_drv_state;
}

/** @brief The broker the driver says it was started with. */
static const char          *s_drv_uri = "";

static void stub_mqtt_uri_copy(char *out, size_t cap, int n)
{
    (void)n;
    (void)snprintf(out, cap, "%s", s_drv_uri);
}

static kode_status_t stub_session(const kdrv_mqtt_session_t *s, int n)
{
    (void)n;
    if ( NULL != s )
    {
        s_sess = *s;
        s_nsess++;
    }
    return KODE_OK;
}

/* Accepts every line the instant it is handed over, so these cases prove what
 * AO_Net decides to publish and when, never how fast esp-mqtt puts it on the
 * wire: that last hop (kdrv_net_mqtt_publish -> esp_mqtt_client_publish) is
 * only visible on the board. */
static kode_status_t stub_publish(const char *topic, const void *data,
                                  uint16_t len, bool retain, int n)
{
    (void)n;
    if ( 8U > s_npub )
    {
        (void)snprintf(s_pubs[s_npub].topic, sizeof(s_pubs[s_npub].topic),
                       "%s", topic);
        (void)snprintf(s_pubs[s_npub].data, sizeof(s_pubs[s_npub].data),
                       "%.*s", (int)len, (const char *)data);
        s_pubs[s_npub].len    = len;
        s_pubs[s_npub].retain = retain;
        s_pubs[s_npub].jobs   = s_nreq;
    }
    s_npub++;
    if ( (KODE_OK == s_publish_rc) && (strcmp(topic, "kode/KODE0001F00MR1C/cmd/out") == 0) )
    {
        if ( OUT_MAX > s_nout )
        {
            (void)snprintf(s_outs[s_nout], sizeof(s_outs[s_nout]), "%.*s",
                           (int)len, (const char *)data);
        }
        s_nout++;
    }
    return s_publish_rc;
}

/** @brief What the subscribe and unsubscribe stubs answer; KODE_OK unless a case says otherwise. */
static kode_status_t s_topic_rc = KODE_OK;

static kode_status_t stub_subscribe(const char *topic, int n)
{
    (void)n;
    if ( 8U > s_nsub )
    {
        (void)snprintf(s_subs[s_nsub], sizeof(s_subs[s_nsub]), "%s", topic);
    }
    s_nsub++;
    return s_topic_rc;
}

static kode_status_t stub_unsubscribe(const char *topic, int n)
{
    (void)topic;
    (void)n;
    s_nunsub++;
    return s_topic_rc;
}

static void stub_http_result(kdrv_net_http_result_t *out, int n)
{
    (void)n;
    s_nresult++;
    (void)memset(out, 0, sizeof(*out));
    out->status         = s_http_status;
    out->content_length = -1;
    out->body_len       = (uint32_t)strlen(s_http_body);
    out->kept           = out->body_len;
}

static uint32_t stub_http_body(uint32_t off, uint8_t *dst, uint32_t cap, int n)
{
    (void)n;
    uint32_t const have = (uint32_t)strlen(s_http_body);
    uint32_t       take = 0U;
    if ( off < have )
    {
        take = have - off;
        if ( take > cap )
        {
            take = cap;
        }
        (void)memcpy(dst, &s_http_body[off], take);
    }
    return take;
}

static kode_status_t stub_running_desc(kdrv_slot_desc_t *out, int n)
{
    (void)n;
    (void)memset(out, 0, sizeof(*out));
    (void)snprintf(out->project, sizeof(out->project), "%s", "kodeos");
    (void)snprintf(out->version, sizeof(out->version), "%s", "v00.00.99");
    return KODE_OK;
}

static const char *const CHAN_CLIENT = "kode-KODE0001F00MR1C";
static const char *const CHAN_NONCE  = "v1.connect.abc-123";

/** @brief Where the api says this unit's screen captures go. */
#define SHOT_LINK  "https://api.kode.diy/kodes/KODE0001F00MR1C/shots/up/"

/** @brief The link connect_answer() names; SHOT_LINK unless a case changes it. */
static const char *s_shots_link = SHOT_LINK;

/** @brief What the api answers connect with: a 379-char token, and the
 *         `shots` link unless s_no_shots says otherwise. */
static void connect_answer(char *out, size_t cap)
{
    char        token[400];
    char        shots[200];

    (void)snprintf(shots, sizeof(shots), ",\"shots\":\"%s\"", s_shots_link);
    if ( s_no_shots )
    {
        shots[0] = '\0';
    }
    (void)memset(token, s_token_fill, 379U);
    token[379] = '\0';
    (void)snprintf(out, cap,
        "{\"serial\":\"KODE0001F00MR1C\",\"brokerUrl\":\"mqtts://kodes.kode.diy:8883\","
        "\"wssUrl\":\"wss://kodes.kode.diy:8084/mqtt\",\"username\":\"KODE0001F00MR1C\","
        "\"clientId\":\"%s\",\"token\":\"%s\",\"expiresAt\":\"2026-10-17T10:52:04.438Z\","
        "\"topics\":{\"in\":\"kode/KODE0001F00MR1C/cmd/in\","
        "\"out\":\"kode/KODE0001F00MR1C/cmd/out\","
        "\"status\":\"kode/KODE0001F00MR1C/status\"}%s}", CHAN_CLIENT, token,
        shots);
}

/**
 * @brief The first request AO_Net posted to AO_Radio; the signal, or 0.
 *
 * The fast mode's notes (WIFI_FAST_REQ_SIG) are passed over: they are static
 * events smaller than any request, so copying @p size bytes of one reads past
 * its end, and a case that takes a request is not asking about them.
 */
static enum_t radio_take(void *out, size_t size)
{
    const QEvt *e = NULL;
    enum_t      sig = 0;

    while ( (0 == sig) && (xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE) )
    {
        if ( (QSignal)WIFI_FAST_REQ_SIG != e->sig )
        {
            sig = e->sig;
            (void)memcpy(out, e, size);
        }
        QF_gc(e);
    }
    return sig;
}

static void radio_drain(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

static void answer_part(uint8_t n, uint8_t of, uint8_t len, uint8_t fill)
{
    IdPartEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super = (QEvt)QEVT_INITIALIZER(ID_PART_SIG);
    a.n   = n;
    a.of  = of;
    a.len = len;
    (void)memset(a.der, fill, len);
    net_dispatch(&a.super);
}

static void answer_end(uint8_t st, uint16_t len, uint16_t unit_len)
{
    IdEndEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super    = (QEvt)QEVT_INITIALIZER(ID_END_SIG);
    a.status   = st;
    a.len      = len;
    a.unit_len = unit_len;
    net_dispatch(&a.super);
}

static void answer_sign(uint8_t st, uint8_t der_len)
{
    IdSignRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super   = (QEvt)QEVT_INITIALIZER(ID_SIGN_RSP_SIG);
    a.status  = st;
    a.der_len = der_len;
    (void)memset(a.der, 0x5A, der_len);
    net_dispatch(&a.super);
}

/** @brief The C5 sends a certificate pair of 400 bytes, 300 of them the unit's. */
static void answer_cert(void)
{
    answer_part(1U, 3U, 144U, 0x11U);
    answer_part(2U, 3U, 144U, 0x22U);
    answer_part(3U, 3U, 112U, 0x33U);
    answer_end((uint8_t)KODE_ST_OK, 400U, 300U);
}

/**
 * @brief The job the worker holds ends with @p rc: the worker reports at last
 *        and the AO works through what follows.
 *
 * @param type The job the case believes the worker holds.
 * @param rc   What it returned.
 */
static void job_done(kdrv_net_job_t type, kode_status_t rc)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "no job is with the worker");
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)type, s_ran_job,
        "the job the worker holds is not the one being finished");
    if ( KDRV_NET_JOB_MQTT_STOP == type )
    {
        s_drv_state = KDRV_MQTT_DOWN;
    }
    deliver_held(rc);
}

/**
 * @brief The HTTP job the worker holds ends with @p rc, whichever kind it is
 *        (a GET, a POST, a PUT or the close of a kept one).
 *
 * @param rc What it returned.
 */
static void job_done_http(kode_status_t rc)
{
    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "no job is with the worker");
    TEST_ASSERT_TRUE_MESSAGE(((int)KDRV_NET_JOB_HTTP_GET == s_ran_job)
                             || ((int)KDRV_NET_JOB_HTTP_POST == s_ran_job)
                             || ((int)KDRV_NET_JOB_HTTP_PUT == s_ran_job)
                             || ((int)KDRV_NET_JOB_HTTP_CLOSE == s_ran_job),
        "the job the worker holds is not an HTTP one");
    deliver_held(rc);
}

/** @brief The api answers the HTTP job in flight. */
static void http_reply(uint16_t status, const char *body)
{
    s_http_status = status;
    (void)snprintf(s_http_body, sizeof(s_http_body), "%s", body);
    job_done_http(KODE_OK);
}

static void mqtt_link_present(kdrv_net_mqtt_evt_t evt, bool session_present)
{
    NetMqttLinkEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(NET_MQTT_LINK_SIG);
    e.evt   = (uint8_t)evt;
    e.session_present = session_present;
    if ( KDRV_NET_MQTT_CONNECTED == evt )
    {
        s_drv_state = KDRV_MQTT_UP;
    }
    else if ( KDRV_NET_MQTT_DISCONNECTED == evt )
    {
        s_drv_state = KDRV_MQTT_DOWN;
    }
    else
    {
        /* A refusal or an error leaves the driver where it was. */
    }
    net_dispatch(&e.super);
}

static void mqtt_link(kdrv_net_mqtt_evt_t evt)
{
    mqtt_link_present(evt, false);
}

/** @brief Simulated seconds: the clock klog and the channel read, and the QP tick. */
static void secs(unsigned s)
{
    for ( unsigned i = 0U; i < s; ++i )
    {
        fake_time_advance_ms(1000U);
        (void)net_tick(100U);
    }
}

static bool logged(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return true;
        }
    }
    return false;
}

static unsigned logged_count(const char *needle)
{
    unsigned n = 0U;
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            ++n;
        }
    }
    return n;
}

/** @brief The newest CHANNEL_STATE_SIG on the spy's queue; state 0xFF if none. */
static ChannelStateEvt last_chan(void)
{
    ChannelStateEvt last;
    const QEvt     *e;

    (void)memset(&last, 0, sizeof(last));
    last.state = 0xFFU;
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)CHANNEL_STATE_SIG == e->sig )
        {
            last = *(const ChannelStateEvt *)e;
        }
    }
    ao_spy_drain();
    return last;
}

/** @brief A fresh AO_Net with the worker, the broker and the C5 played from here. */
static void chan_fresh(void)
{
    net_fresh();

    s_nreq = 0U; s_nbody = 0U; s_nsess = 0U; s_npub = 0U; s_nsub = 0U;
    s_nunsub = 0U;
    s_topic_rc = KODE_OK;
    s_nout = 0U;
    s_publish_rc = KODE_OK;
    s_http_status = 0U;
    s_http_body[0] = '\0';
    (void)memset(&s_sess, 0, sizeof(s_sess));
    s_token_fill = 'x';
    s_no_shots = false;
    s_shots_link = SHOT_LINK;
    s_nresult = 0U;
    s_nforget = 0U;

    s_auto    = false;
    s_run_rc  = KODE_OK;
    kdrv_net_run_StopIgnore();
    kdrv_net_run_Stub(&stub_run);
    kdrv_net_http_post_body_Stub(&stub_post_body);
    kdrv_net_mqtt_session_Stub(&stub_session);
    kdrv_net_mqtt_publish_Stub(&stub_publish);
    kdrv_net_mqtt_subscribe_Stub(&stub_subscribe);
    kdrv_net_mqtt_unsubscribe_Stub(&stub_unsubscribe);
    kdrv_net_http_result_Stub(&stub_http_result);
    kdrv_net_http_body_Stub(&stub_http_body);
    kdrv_net_http_forget_Stub(&stub_forget);
    s_drv_state = KDRV_MQTT_DOWN;
    kdrv_net_mqtt_state_Stub(&stub_mqtt_state);
    s_drv_uri = "";
    kdrv_net_mqtt_uri_copy_Stub(&stub_mqtt_uri_copy);
    kdrv_running_desc_Stub(&stub_running_desc);

    ao_spy_subscribe(CHANNEL_STATE_SIG);
    ao_spy_subscribe(MQTT_STATE_SIG);
    ao_spy_drain();
    fake_klog_reset();
}

/** @brief Where the channel stands now, asked rather than overheard. */
static ChannelStateEvt chan_now(void)
{
    ao_spy_drain();
    TEST_ASSERT_TRUE(channel_state_request());
    (void)net_settle();
    return last_chan();
}

static const kdrv_net_req_t *req_at(unsigned i)
{
    TEST_ASSERT_TRUE_MESSAGE(i < s_nreq, "the job was never submitted");
    return &s_reqs[i];
}

/** @brief online -> the cert request is answered -> the challenge is posted. */
static void drive_to_challenge(void)
{
    IdReqEvt q;

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ID_OP_CERT, q.op);
    answer_cert();
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
}

/** @brief ... -> the nonce is signed -> the connect request is posted. */
static void drive_to_connect(void)
{
    IdSignReqEvt q;

    drive_to_challenge();
    http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\","
                     "\"expiresAt\":\"2026-10-17T10:50:04.438Z\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);
}

/** @brief ... -> the session is handed over -> the broker connects. */
static void drive_to_up(void)
{
    static char answer[1000];

    drive_to_connect();
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
}

/**
 * @brief The wait after a drop is over: the whole join again, to CONNECTED.
 *
 * The client was stopped (its STOP_DONE comes first) and the certificate is
 * still in hand, so it is the challenge, the signature and the connect.
 *
 * @param wait_s Simulated seconds that outlast the wait.
 */
static void rejoin(unsigned wait_s)
{
    static char  answer[1000];
    IdSignReqEvt sq;

    secs(wait_s);
    http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
}

/* ---- The whole join ------------------------------------------------------- */

TEST_CASE("with a certificate, the channel walks challenge, signature, connect "
          "and broker, and says it is up", "[net_ao]")
{
    static char answer[1000];
    IdReqEvt    q;
    IdSignReqEvt sq;

    chan_fresh();

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ID_OP_CERT, q.op);
    TEST_ASSERT_TRUE(q.who == AO_Net);
    TEST_ASSERT_EQUAL_UINT(0U, s_nreq);   /* the api is asked after the C5 */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_JOINING, last_chan().state);

    answer_cert();
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, (int)req_at(0)->type);
    TEST_ASSERT_EQUAL_STRING("https://api.kode.diy/kodes/challenge", req_at(0)->url);
    TEST_ASSERT_EQUAL_STRING("{}", s_bodies[0]);

    http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\","
                     "\"expiresAt\":\"2026-10-17T10:50:04.438Z\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    TEST_ASSERT_EQUAL_STRING(CHAN_NONCE, sq.nonce);

    answer_sign((uint8_t)KODE_ST_OK, 70U);
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, (int)req_at(1)->type);
    TEST_ASSERT_EQUAL_STRING("https://api.kode.diy/kodes/connect", req_at(1)->url);
    {
        /* The body is what Task 3 builds: the unit's 300 bytes, the nonce, the signature. */
        char     cert[1100];
        char     nonce[256];
        char     sig[256];
        uint8_t  unit[300];
        uint8_t  der[70];
        char     want[1100];

        TEST_ASSERT_TRUE(kchan_json_str(s_bodies[1], strlen(s_bodies[1]), "cert", cert, sizeof(cert)));
        TEST_ASSERT_TRUE(kchan_json_str(s_bodies[1], strlen(s_bodies[1]), "nonce", nonce, sizeof(nonce)));
        TEST_ASSERT_TRUE(kchan_json_str(s_bodies[1], strlen(s_bodies[1]), "signature", sig, sizeof(sig)));
        TEST_ASSERT_EQUAL_STRING(CHAN_NONCE, nonce);
        (void)memset(unit, 0x11, 144U);
        (void)memset(&unit[144], 0x22, 144U);
        (void)memset(&unit[288], 0x33, 12U);
        (void)kaos_wire_b64_encode(unit, sizeof(unit), want);
        TEST_ASSERT_EQUAL_STRING(want, cert);
        (void)memset(der, 0x5A, sizeof(der));
        (void)kaos_wire_b64_encode(der, sizeof(der), want);
        TEST_ASSERT_EQUAL_STRING(want, sig);
    }

    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    /* The session the broker is given: the api's words, the will on status, 60 s. */
    TEST_ASSERT_EQUAL_UINT(1U, s_nsess);
    TEST_ASSERT_EQUAL_STRING(CHAN_CLIENT, s_sess.client_id);
    TEST_ASSERT_EQUAL_STRING("KODE0001F00MR1C", s_sess.user);
    TEST_ASSERT_EQUAL_UINT(379U, strlen(s_sess.pass));
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/status", s_sess.will_topic);
    TEST_ASSERT_EQUAL_STRING("{\"online\":false}", s_sess.will_msg);
    TEST_ASSERT_TRUE(s_sess.will_retain);
    TEST_ASSERT_EQUAL_UINT16(60U, s_sess.keepalive_s);
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_START, (int)req_at(2)->type);
    TEST_ASSERT_EQUAL_STRING("mqtts://kodes.kode.diy:8883", req_at(2)->uri);

    TEST_ASSERT_EQUAL_UINT(0U, s_nsub);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);

    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/cmd/in", s_subs[0]);
    TEST_ASSERT_EQUAL_UINT(1U, s_npub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/status", s_pubs[0].topic);
    TEST_ASSERT_EQUAL_STRING("{\"online\":true,\"app\":\"kodeos\",\"version\":\"v00.00.99\"}",
                             s_pubs[0].data);
    TEST_ASSERT_TRUE(s_pubs[0].retain);

    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, st.state);
        TEST_ASSERT_EQUAL_STRING("mqtts://kodes.kode.diy:8883", st.broker);
        /* The ring's count is for the whole run of the binary, and a case
         * in another file may have lost a line on purpose. */
        TEST_ASSERT_EQUAL_UINT32(kchan_out_lost(), st.lost);
    }
    TEST_ASSERT_TRUE(logged("channel: up as kode-KODE0001F00MR1C"));

    net_done();
}

/* ---- No identity, and an identity the api refuses -------------------------- */

TEST_CASE("a C5 without a certificate says so once and the channel asks for nothing",
          "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    answer_end((uint8_t)KODE_ST_NOT_FOUND, 0U, 0U);

    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_NO_IDENTITY, st.state);
    }
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("channel: no identity in the C5"));

    /* Nothing is armed: ten minutes later still no job and no new request. */
    secs(600U);
    TEST_ASSERT_EQUAL_UINT(0U, s_nreq);
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));

    net_done();
}

TEST_CASE("the api refusing the certificate is terminal until the Wi-Fi cycles",
          "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    drive_to_connect();
    http_reply(401U, "{\"error\":\"untrusted_cert\"}");

    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_REFUSED, st.state);
        TEST_ASSERT_EQUAL_STRING("untrusted_cert", st.why);
    }
    TEST_ASSERT_TRUE(logged("channel: refused (untrusted_cert)"));
    secs(600U);
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);   /* the challenge and the connect, no more */

    /* A new Wi-Fi cycle clears it: the certificate is read again. */
    go_offline();
    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_JOINING, last_chan().state);

    net_done();
}

/* ---- Back-off -------------------------------------------------------------- */

TEST_CASE("a 503 waits, asks again, and each failure in a row waits longer",
          "[net_ao]")
{
    static const struct { uint32_t lo; uint32_t hi; } BAND[3] = {
        { 4U, 7U }, { 8U, 13U }, { 15U, 25U },   /* 5, 10, 20 s at 75..125 % */
    };
    IdReqEvt q;

    chan_fresh();

    drive_to_challenge();
    for ( unsigned k = 0U; 3U > k; ++k )
    {
        ChannelStateEvt st;

        http_reply(503U, "{\"error\":\"channel_unavailable\"}");
        st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("channel_unavailable", st.why);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(BAND[k].lo, st.next_s);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(BAND[k].hi, st.next_s);

        /* Nothing goes out before the wait is over... */
        secs(st.next_s - 1U);
        TEST_ASSERT_EQUAL_UINT(k + 1U, s_nreq);
        /* ...and the new attempt is a new challenge: the C5 is not asked
         * for a certificate it already gave. */
        secs(2U);
        TEST_ASSERT_EQUAL_UINT(k + 2U, s_nreq);
        TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    }
    /* The same cause is said once, not once per retry. */
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("channel: waiting"));

    net_done();
}

TEST_CASE("an html 502 to connect leaves the channel waiting with bad_answer",
          "[net_ao]")
{
    chan_fresh();

    drive_to_connect();
    http_reply(502U, "<html><body>Bad gateway</body></html>");

    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("bad_answer", st.why);
    }
    TEST_ASSERT_EQUAL_UINT(0U, s_nsess);   /* no half-configured session */

    net_done();
}

TEST_CASE("a connect answer that is not a session is refused whole",
          "[net_ao]")
{
    chan_fresh();

    drive_to_connect();
    http_reply(200U, "{\"serial\":\"KODE0001F00MR1C\",\"brokerUrl\":\"mqtts://h:1\"}");

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, last_chan().state);
    TEST_ASSERT_EQUAL_UINT(0U, s_nsess);
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);

    net_done();
}

/* ---- Review focus 1: Wi-Fi flapping ---------------------------------------- */

TEST_CASE("five Wi-Fi flaps in one tick make one request and keep the back-off",
          "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    /* One failure first, so there is a back-off to lose. */
    drive_to_challenge();
    http_reply(503U, "{\"error\":\"channel_unavailable\"}");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, last_chan().state);

    for ( unsigned i = 0U; 5U > i; ++i )
    {
        go_offline();
        go_online();
    }
    /* Back on the address, the wait is still honoured: no request yet. */
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, last_chan().state);

    secs(8U);
    /* One request to the C5, however many times the address came and went. */
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    for ( unsigned i = 0U; 5U > i; ++i )
    {
        go_offline();
        go_online();
    }
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    answer_cert();
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);   /* exactly one more job */

    /* The second failure waits like a second failure, not a first. */
    http_reply(503U, "{\"error\":\"channel_unavailable\"}");
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(8U, st.next_s);
    }

    net_done();
}

/* ---- Leaving, renewing, losing the session -------------------------------- */

TEST_CASE("losing the address stops the client and the channel says no network",
          "[net_ao]")
{
    chan_fresh();

    drive_to_up();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);
    s_drv_state = KDRV_MQTT_UP;

    s_nreq = 3U;
    go_offline();

    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(3)->type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_NO_NET, last_chan().state);
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("channel: no network"));
    /* A clean stop sends DISCONNECT and the broker then keeps the will to
     * itself, so the retained "online" would stand: the same message the will
     * carries goes out first, retained, before the STOP job. */
    TEST_ASSERT_EQUAL_UINT(2U, s_npub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/status", s_pubs[1].topic);
    TEST_ASSERT_EQUAL_STRING(KCHAN_WILL, s_pubs[1].data);
    TEST_ASSERT_TRUE(s_pubs[1].retain);
    TEST_ASSERT_EQUAL_UINT(3U, s_pubs[1].jobs);   /* before the STOP, which is job 4 */

    net_done();
}

TEST_CASE("a terminal answer during a renewal says offline before the client is "
          "stopped", "[net_ao]")
{
    IdSignReqEvt sq;

    chan_fresh();

    drive_to_up();
    ao_spy_drain();
    secs(121U);                                   /* the renewal begins */
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    TEST_ASSERT_EQUAL_UINT(5U, s_nreq);           /* the connect is in the air */
    s_npub = 0U;

    http_reply(401U, "{\"error\":\"untrusted_cert\"}");

    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_REFUSED, st.state);
    }
    TEST_ASSERT_EQUAL_UINT(1U, s_npub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/status", s_pubs[0].topic);
    TEST_ASSERT_EQUAL_STRING(KCHAN_WILL, s_pubs[0].data);
    TEST_ASSERT_TRUE(s_pubs[0].retain);
    TEST_ASSERT_EQUAL_UINT(5U, s_pubs[0].jobs);   /* before the STOP */
    TEST_ASSERT_EQUAL_UINT(6U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(5)->type);

    net_done();
}

TEST_CASE("the renewal asks the api again, swaps the session and never says offline",
          "[net_ao]")
{
    IdSignReqEvt sq;
    IdReqEvt     q;
    static char  answer[1000];

    chan_fresh();

    drive_to_up();
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);   /* spare: a reconnect changes nothing here */
    s_npub = 0U;

    /* CONFIG_KODEOS_CHANNEL_RENEW_S is 120 in this binary. */
    secs(119U);
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    secs(2U);
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);   /* a new challenge; the certificate is kept */
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, (int)req_at(3)->type);
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);

    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    TEST_ASSERT_EQUAL_STRING("v1.connect.second", sq.nonce);
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    TEST_ASSERT_EQUAL_UINT(5U, s_nreq);

    s_drv_state = KDRV_MQTT_UP;
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    /* The old client goes first, then the new one starts with the new session. */
    TEST_ASSERT_EQUAL_UINT(6U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(5)->type);
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsess);
    TEST_ASSERT_EQUAL_UINT(7U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_START, (int)req_at(6)->type);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);
    /* Presence again, true; no offline from this side: the renewal's stop of
     * the old client is the one stop that does not publish the will. */
    TEST_ASSERT_EQUAL_UINT(1U, s_npub);
    TEST_ASSERT_NOT_NULL(strstr(s_pubs[0].data, "\"online\":true"));
    TEST_ASSERT_NULL(strstr(s_pubs[0].data, "\"online\":false"));

    net_done();
}

TEST_CASE("a token the broker refuses sends the channel back to the start, and a "
          "session that stayed up resets the back-off",
          "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    /* Two failures first: the counter stands at 2. */
    drive_to_challenge();
    http_reply(503U, "{\"error\":\"channel_unavailable\"}");
    secs(8U);
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);
    http_reply(503U, "{\"error\":\"channel_unavailable\"}");
    secs(14U);
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));

    {
        IdSignReqEvt sq;
        static char  answer[1000];

        http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\"}");
        TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
        answer_sign((uint8_t)KODE_ST_OK, 70U);
        connect_answer(answer, sizeof(answer));
        http_reply(200U, answer);
        job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
        mqtt_link(KDRV_NET_MQTT_CONNECTED);
    }
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);

    /* It stays up well over a minute, then the broker refuses its token. */
    secs(10U);
    mqtt_link(KDRV_NET_MQTT_REFUSED);
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("broker_refused", st.why);
        /* Up for only 10 s: the third failure in a row, 20 s at 75..125 %. */
        TEST_ASSERT_GREATER_OR_EQUAL_UINT32(15U, st.next_s);
    }

    net_done();
}

TEST_CASE("a session that stayed up a minute starts the back-off over",
          "[net_ao]")
{
    chan_fresh();

    drive_to_up();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);
    /* Renewal is 120 s in this binary; the refusal comes well before it. */
    secs(61U);
    mqtt_link(KDRV_NET_MQTT_REFUSED);
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_LESS_OR_EQUAL_UINT32(7U, st.next_s);   /* the first wait again */
    }

    net_done();
}

TEST_CASE("a broker that never answers leaves the channel waiting", "[net_ao]")
{
    static char answer[1000];

    chan_fresh();

    drive_to_connect();
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);

    secs(16U);   /* KODE_MQTT_CONNECT_MS is 15 s */
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("broker_timeout", st.why);
    }

    net_done();
}

/* ---- The client belongs to the channel while it runs ----------------------- */

TEST_CASE("mqtt_up while the channel holds the client is refused and changes nothing",
          "[net_ao]")
{
    chan_fresh();

    drive_to_up();
    ao_spy_drain();
    s_nreq = 3U;

    mqtt_up_request("mqtt://broker.example:1883", "", "");
    net_settle();
    (void)net_tick(1U);
    (void)ao_spy_count();

    /* The refusal is MQTT_STATE_SIG with the reason the console turns into
     * `>err code=4 msg=busy`. */
    {
        const QEvt *e;
        bool        saw = false;
        while ( (e = ao_spy_take()) != NULL )
        {
            if ( (QSignal)MQTT_STATE_SIG == e->sig )
            {
                saw = true;
                TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_BUSY,
                                        ((const MqttStateEvt *)e)->err);
            }
        }
        TEST_ASSERT_TRUE(saw);
    }
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);   /* no job for it */
    TEST_ASSERT_EQUAL_UINT(1U, s_nsess);

    net_done();
}

TEST_CASE("the channel holds the client from its first step until the address goes",
          "[net_ao]")
{
    chan_fresh();
    TEST_ASSERT_FALSE(mqtt_channel_holds());

    drive_to_challenge();
    TEST_ASSERT_TRUE(mqtt_channel_holds());
    go_offline();
    TEST_ASSERT_FALSE(mqtt_channel_holds());

    net_done();
}

TEST_CASE("the channel holds the client while it is up, and the status says so until chan off",
          "[net_ao]")
{
    kode_sys_mqtt_status_t st;

    fake_nvs_reset();
    chan_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_status(&st));
    TEST_ASSERT_FALSE(st.channel);

    drive_to_up();
    TEST_ASSERT_TRUE(mqtt_channel_holds());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_status(&st));
    TEST_ASSERT_TRUE(st.channel);

    s_drv_state = KDRV_MQTT_UP;
    (void)config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true });
    pull_jobs();
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    s_drv_state = KDRV_MQTT_DOWN;
    TEST_ASSERT_FALSE(mqtt_channel_holds());
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_status(&st));
    TEST_ASSERT_FALSE(st.channel);

    fake_nvs_reset();
    net_done();
}

TEST_CASE("channel_state_request publishes where the channel stands", "[net_ao]")
{
    chan_fresh();

    TEST_ASSERT_TRUE(channel_state_request());
    (void)net_settle();
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_NO_NET, st.state);
    }

    drive_to_up();
    ao_spy_drain();
    TEST_ASSERT_TRUE(channel_state_request());
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);

    net_done();
}

/* ---- More ways a step can go wrong ------------------------------------------ */

TEST_CASE("a broker that drops the session is waited for on the channel's own "
          "back-off, and the whole join starts again", "[net_ao]")
{
    ChannelStateEvt st;
    unsigned        subs_before;
    unsigned        drop_pubs;

    chan_fresh();

    drive_to_up();
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);
    secs(61U);                     /* long enough to have earned a fresh back-off */
    ao_spy_drain();
    s_npub = 0U;

    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    st = last_chan();
    drop_pubs = s_npub;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
    TEST_ASSERT_EQUAL_STRING("broker_lost", st.why);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4U, st.next_s);   /* 5 s at 75..125 % */
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(7U, st.next_s);
    /* The client does not redial by itself: it is stopped, and the wait is ours. */
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(3)->type);
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("channel: waiting"));
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);

    secs(st.next_s - 1U);
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);       /* nothing before the wait is over */
    subs_before = s_nsub;

    rejoin(2U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);
    /* Everything is said again to the new session. */
    TEST_ASSERT_EQUAL_UINT(subs_before + 1U, s_nsub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/cmd/in", s_subs[subs_before]);
    TEST_ASSERT_NOT_NULL(strstr(s_pubs[drop_pubs].data, "\"online\":true"));

    net_done();
}

/** @brief Up to the point where the broker is being dialled (phase STARTING). */
static void drive_to_dialling(void)
{
    static char answer[1000];

    drive_to_connect();
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
}

TEST_CASE("a connect that fails at the transport leaves the channel waiting at "
          "once, without the 15 s of silence", "[net_ao]")
{
    ChannelStateEvt st;

    chan_fresh();

    drive_to_dialling();
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);

    /* The transport fails: an error, then the drop. esp-mqtt does not redial. */
    mqtt_link(KDRV_NET_MQTT_ERROR);
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    st = last_chan();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
    TEST_ASSERT_EQUAL_STRING("broker_unreachable", st.why);
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(3)->type);
    /* Nothing was online yet, so there is no presence to take back. */
    TEST_ASSERT_EQUAL_UINT(0U, s_npub);

    net_done();
}

TEST_CASE("a refusal followed by the drop that esp-mqtt reports with it is one "
          "failure, not two", "[net_ao]")
{
    ChannelStateEvt st;
    unsigned        waits;
    unsigned        jobs;

    chan_fresh();

    drive_to_dialling();

    mqtt_link(KDRV_NET_MQTT_REFUSED);
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    st    = last_chan();
    waits = logged_count("channel: waiting");
    jobs  = s_nreq;
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
    TEST_ASSERT_EQUAL_STRING("broker_refused", st.why);
    TEST_ASSERT_EQUAL_UINT(1U, waits);
    TEST_ASSERT_EQUAL_UINT(4U, jobs);   /* one STOP, however many events */
}

/** @brief The MQTT_STATE_SIG events on the spy: how many, and how many said busy. */
static void count_state_events(unsigned *all, unsigned *busy)
{
    const QEvt *e;

    *all  = 0U;
    *busy = 0U;
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)MQTT_STATE_SIG == e->sig )
        {
            (*all)++;
            if ( ((const MqttStateEvt *)e)->err == (uint8_t)KDRV_MQTT_ERR_BUSY )
            {
                (*busy)++;
            }
        }
    }
    ao_spy_drain();
}

TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub while the channel holds the client "
          "are refused busy and touch nothing", "[net_ao]")
{
    unsigned all;
    unsigned busy;
    unsigned pubs;
    unsigned subs;
    unsigned unsubs;

    chan_fresh();

    drive_to_up();
    ao_spy_drain();
    s_npub = 0U;
    s_nsub = 0U;

    mqtt_pub_request("some/topic", "x", 1U);
    mqtt_sub_request("some/topic");
    mqtt_unsub_request("kode/KODE0001F00MR1C/cmd/in");   /* would deafen the channel */
    (void)net_settle();
    count_state_events(&all, &busy);
    pubs   = s_npub;
    subs   = s_nsub;
    unsubs = s_nunsub;
    net_done();

    TEST_ASSERT_EQUAL_UINT(3U, all);
    TEST_ASSERT_EQUAL_UINT(3U, busy);   /* each verb is answered, and refused */
    TEST_ASSERT_EQUAL_UINT(0U, pubs);
    TEST_ASSERT_EQUAL_UINT(0U, subs);
    TEST_ASSERT_EQUAL_UINT(0U, unsubs);
}

TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub run, and are answered, when the "
          "channel is not holding the client", "[net_ao]")
{
    IdReqEvt q;
    unsigned all;
    unsigned busy;
    unsigned pubs;
    unsigned subs;
    unsigned unsubs;

    chan_fresh();

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    answer_end((uint8_t)KODE_ST_NOT_FOUND, 0U, 0U);   /* no identity: the client is free */
    ao_spy_drain();

    mqtt_pub_request("some/topic", "x", 1U);
    mqtt_sub_request("some/topic");
    mqtt_unsub_request("some/topic");
    (void)net_settle();
    count_state_events(&all, &busy);
    pubs   = s_npub;
    subs   = s_nsub;
    unsubs = s_nunsub;
    net_done();

    TEST_ASSERT_EQUAL_UINT(3U, all);
    TEST_ASSERT_EQUAL_UINT(0U, busy);
    TEST_ASSERT_EQUAL_UINT(1U, pubs);
    TEST_ASSERT_EQUAL_UINT(1U, subs);
    TEST_ASSERT_EQUAL_UINT(1U, unsubs);
}

/** @brief The err of the next MQTT_STATE_SIG on the spy, or 0xFF when none came. */
static uint8_t next_state_err(void)
{
    const QEvt *e;

    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)MQTT_STATE_SIG == e->sig )
        {
            return ((const MqttStateEvt *)e)->err;
        }
    }
    return 0xFFU;
}

/* With no address the verb is refused, and the refusal is in the answer: the
 * console words it as an error, not as the `>ok` of a request that was taken. */
TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub with no network are answered no_net",
          "[net_ao]")
{
    net_fresh();
    ao_spy_drain();

    mqtt_pub_request("some/topic", "x", 1U);
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_NO_NET, next_state_err());
    mqtt_sub_request("some/topic");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_NO_NET, next_state_err());
    mqtt_unsub_request("some/topic");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_NO_NET, next_state_err());

    net_done();
}

/* Online and with no client up, the driver takes none of the three. The
 * answer says so, or the console would close a publish that went nowhere
 * with `>ok`. */
TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub with no broker are answered refused",
          "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    answer_end((uint8_t)KODE_ST_NOT_FOUND, 0U, 0U);   /* no identity: the client is free */
    ao_spy_drain();
    s_publish_rc = KODE_ERROR;
    s_topic_rc   = KODE_ERROR;

    mqtt_pub_request("some/topic", "x", 1U);
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_REFUSED, next_state_err());
    mqtt_sub_request("some/topic");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_REFUSED, next_state_err());
    mqtt_unsub_request("some/topic");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_REFUSED, next_state_err());

    s_publish_rc = KODE_OK;
    net_done();
}

/* The reason a client is down outlives the network that caused it. A request
 * that was served says nothing of it, or `mqtt_pub` would be refused for an
 * address the board lost an hour ago. */
TEST_CASE("an mqtt_pub served online is not refused for a no_net left from before",
          "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();
    ao_spy_drain();

    mqtt_up_request("mqtt://broker.example:1883", "", "");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_NO_NET, next_state_err());

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    answer_end((uint8_t)KODE_ST_NOT_FOUND, 0U, 0U);
    ao_spy_drain();

    mqtt_pub_request("some/topic", "x", 1U);
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_NONE, next_state_err());

    net_done();
}

TEST_CASE("every refused MQTT verb names the option that frees the client",
          "[net_ao]")
{
    chan_fresh();

    drive_to_up();
    ao_spy_drain();
    fake_klog_reset();

    mqtt_up_request("mqtt://broker.example:1883", "", "");
    (void)qf_support_settle(AO_Net);
    TEST_ASSERT_TRUE(logged("CONFIG_KODEOS_CHANNEL=n"));
    TEST_ASSERT_TRUE(logged("`mqtt_up` refused"));
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("`mqtt_up` refused"));

    mqtt_pub_request("some/topic", "x", 1U);
    mqtt_sub_request("some/topic");
    mqtt_unsub_request("some/topic");
    mqtt_down_request();
    (void)qf_support_settle(AO_Net);
    TEST_ASSERT_TRUE(logged("`mqtt_pub` refused"));
    TEST_ASSERT_TRUE(logged("`mqtt_sub` refused"));
    TEST_ASSERT_TRUE(logged("`mqtt_unsub` refused"));
    TEST_ASSERT_TRUE(logged("`mqtt_down` refused"));
    TEST_ASSERT_EQUAL_UINT(5U, logged_count("CONFIG_KODEOS_CHANNEL=n"));

    net_done();
}

TEST_CASE("mqtt_down while the channel holds the client is refused too",
          "[net_ao]")
{
    chan_fresh();

    drive_to_up();
    ao_spy_drain();
    mqtt_down_request();
    (void)net_settle();

    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);   /* no STOP went out */
    {
        const QEvt *e;
        bool        saw = false;
        while ( (e = ao_spy_take()) != NULL )
        {
            if ( (QSignal)MQTT_STATE_SIG == e->sig )
            {
                saw = true;
                TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_BUSY,
                                        ((const MqttStateEvt *)e)->err);
            }
        }
        TEST_ASSERT_TRUE(saw);
    }

    net_done();
}

/* ---- The MQTT doors and their margin ------------------------------------------- */

/** @brief Most fillers a case posts to reach a depth, so a bug cannot loop. */
#define DOOR_FILL_GUARD  64U

/** @brief Pools the harness lays out (small and jumbo), ids from 1. */
#define DOOR_POOLS       2U

/** @brief Event blocks a case may hold to run a pool dry. */
#define DOOR_DRY_MAX     256U

/** @brief Topic and payload a door is given. */
#define DOOR_TOPIC       "some/topic"

/** @brief Take every event out of AO_Net's queue and recycle it. */
static void net_queue_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Post fillers until exactly @p spaces slots of AO_Net's queue are free. */
static void net_queue_fill_to(UBaseType_t spaces)
{
    static const QEvt filler = QEVT_INITIALIZER(MQTT_STATE_REQ_SIG);

    for ( uint32_t guard = 0U;
         (DOOR_FILL_GUARD > guard) && (uxQueueSpacesAvailable(AO_Net->eQueue) > spaces);
         ++guard )
    {
        QACTIVE_POST(AO_Net, &filler, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(spaces, uxQueueSpacesAvailable(AO_Net->eQueue));
}

/** @brief Blocks free in every pool: a recycled event leaves it unchanged. */
static uint32_t pools_free(void)
{
    uint32_t total = 0U;

    for ( uint8_t id = 1U; DOOR_POOLS >= id; ++id )
    {
        total += (uint32_t)QF_getPoolFree(id);
    }
    return total;
}

/** @brief Ask all five doors once; the number that said true. */
static uint32_t doors_taken(void)
{
    uint32_t taken = 0U;

    if ( mqtt_up_request("mqtt://broker.example:1883", "", "") ) { ++taken; }
    if ( mqtt_down_request() )                                   { ++taken; }
    if ( mqtt_pub_request(DOOR_TOPIC, "x", 1U) )                 { ++taken; }
    if ( mqtt_sub_request(DOOR_TOPIC) )                          { ++taken; }
    if ( mqtt_unsub_request(DOOR_TOPIC) )                        { ++taken; }
    return taken;
}

TEST_CASE("the MQTT doors post while the queue has room", "[net_ao][doors]")
{
    static const QSignal WANT[5] = {
        MQTT_UP_REQ_SIG, MQTT_DOWN_REQ_SIG, MQTT_PUB_REQ_SIG,
        MQTT_SUB_REQ_SIG, MQTT_UNSUB_REQ_SIG,
    };
    const QEvt *e = NULL;

    chan_fresh();
    net_queue_flush();

    TEST_ASSERT_EQUAL_UINT32(5U, doors_taken());
    for ( uint32_t i = 0U; 5U > i; ++i )
    {
        TEST_ASSERT_TRUE(xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)WANT[i], (uint32_t)e->sig);
        QF_gc(e);
    }
    net_done();
}

TEST_CASE("an MQTT door refuses once the queue is within its margin and keeps the pool whole",
          "[net_ao][doors]")
{
    chan_fresh();
    net_queue_flush();
    net_queue_fill_to(KQP_NET_CMD_MARGIN);
    uint32_t const before = pools_free();

    TEST_ASSERT_EQUAL_UINT32(0U, doors_taken());

    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(before, pools_free(),
                                     "a refused event was not recycled");
    net_queue_flush();
    net_done();
}

TEST_CASE("an MQTT door takes the last slot above its margin and no more",
          "[net_ao][doors]")
{
    chan_fresh();
    net_queue_flush();
    net_queue_fill_to(KQP_NET_CMD_MARGIN + 1U);
    uint32_t const before = pools_free();

    TEST_ASSERT_TRUE(mqtt_pub_request(DOOR_TOPIC, "x", 1U));
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
    TEST_ASSERT_FALSE(mqtt_pub_request(DOOR_TOPIC, "x", 1U));
    TEST_ASSERT_EQUAL_UINT32(before - 1U, pools_free());

    net_queue_flush();
    TEST_ASSERT_EQUAL_UINT32(before, pools_free());
    net_done();
}

TEST_CASE("an MQTT door answers false when the pool is dry", "[net_ao][doors]")
{
    static const QEvt *held[DOOR_DRY_MAX];
    uint32_t           n = 0U;

    chan_fresh();
    net_queue_flush();
    while ( DOOR_DRY_MAX > n )
    {
        MqttPubEvt *const fresh = Q_NEW_X(MqttPubEvt, 0U, MQTT_PUB_REQ_SIG);

        if ( NULL == fresh )
        {
            break;
        }
        held[n] = &fresh->super;
        ++n;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(DOOR_DRY_MAX, n, "the pool never ran dry");

    TEST_ASSERT_FALSE(mqtt_pub_request(DOOR_TOPIC, "x", 1U));
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_Net->eQueue));

    for ( uint32_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
    net_done();
}

/* ---- AO_Net's answers, counted ---------------------------------------------- */

TEST_CASE("the offline refusal of mqtt_up is an answer, with err NO_NET",
          "[net_ao][answers]")
{
    uint8_t err = 0U;

    chan_fresh();
    uint32_t const before = mqtt_answers(NULL);

    TEST_ASSERT_TRUE(mqtt_up_request("mqtt://broker.example:1883", "", ""));
    (void)net_settle();

    TEST_ASSERT_EQUAL_UINT32(before + 1U, mqtt_answers(&err));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_NO_NET, err);
    net_done();
}

TEST_CASE("every offline MQTT request, and a state request, is one answer each",
          "[net_ao][answers]")
{
    chan_fresh();
    uint32_t const before = mqtt_answers(NULL);

    TEST_ASSERT_EQUAL_UINT32(5U, doors_taken());
    mqtt_state_request();
    (void)net_settle();

    TEST_ASSERT_EQUAL_UINT32(before + 6U, mqtt_answers(NULL));
    net_done();
}

TEST_CASE("the channel's busy refusal is an answer, with err BUSY", "[net_ao][answers]")
{
    uint8_t err = 0U;

    chan_fresh();
    drive_to_up();
    ao_spy_drain();
    uint32_t const before = mqtt_answers(NULL);

    TEST_ASSERT_TRUE(mqtt_up_request("mqtt://broker.example:1883", "", ""));
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT32(before + 1U, mqtt_answers(&err));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_BUSY, err);

    TEST_ASSERT_TRUE(mqtt_pub_request(DOOR_TOPIC, "x", 1U));
    TEST_ASSERT_TRUE(mqtt_sub_request(DOOR_TOPIC));
    TEST_ASSERT_TRUE(mqtt_unsub_request(DOOR_TOPIC));
    TEST_ASSERT_TRUE(mqtt_down_request());
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT32(before + 5U, mqtt_answers(&err));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_BUSY, err);
    net_done();
}

TEST_CASE("an answer the pool could not carry is not counted", "[net_ao][answers]")
{
    static const QEvt *held[DOOR_DRY_MAX];
    uint32_t           n = 0U;

    chan_fresh();
    uint32_t const before = mqtt_answers(NULL);
    while ( DOOR_DRY_MAX > n )
    {
        MqttStateEvt *const fresh = Q_NEW_X(MqttStateEvt, 0U, MQTT_STATE_SIG);

        if ( NULL == fresh )
        {
            break;
        }
        held[n] = &fresh->super;
        ++n;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(DOOR_DRY_MAX, n, "the pool never ran dry");

    mqtt_state_request();
    (void)qf_support_settle(AO_Net);
    TEST_ASSERT_EQUAL_UINT32(before, mqtt_answers(NULL));

    for ( uint32_t i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
    net_done();
}

TEST_CASE("kode_sys_mqtt_status carries the state, the last err, the answers and the broker",
          "[net_ao][answers]")
{
    kode_sys_mqtt_status_t st;

    chan_fresh();
    s_drv_state = KDRV_MQTT_CONNECTING;
    s_drv_uri   = "mqtt://broker.example:1883";
    uint32_t const before = mqtt_answers(NULL);

    TEST_ASSERT_TRUE(mqtt_up_request("mqtt://broker.example:1883", "", ""));
    (void)net_settle();   /* offline: answered NO_NET */

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_mqtt_status(&st));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_CONNECTING, st.state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MQTT_ERR_NO_NET, st.err);
    TEST_ASSERT_EQUAL_UINT32(before + 1U, st.answers);
    TEST_ASSERT_EQUAL_STRING("mqtt://broker.example:1883", st.uri);
    net_done();
}

TEST_CASE("a certificate too big for the request is refused for good",
          "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    for ( uint8_t n = 1U; 11U >= n; ++n )
    {
        answer_part(n, 11U, 144U, n);
    }
    answer_end((uint8_t)KODE_ST_OK, 1584U, 1500U);
    http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\"}");
    answer_sign((uint8_t)KODE_ST_OK, 70U);

    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_REFUSED, st.state);
        TEST_ASSERT_EQUAL_STRING("request_size", st.why);
    }
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);   /* the challenge only */

    net_done();
}

TEST_CASE("a C5 that does not answer in time fails the step; one that is busy "
          "says so", "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    secs(14U);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_JOINING, chan_now().state);
    secs(2U);
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("c5_timeout", st.why);
    }

    /* The next attempt, and this time the C5 refuses: link down or busy. */
    secs(8U);
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    answer_end((uint8_t)KODE_ST_REFUSED, 0U, 0U);
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("c5_busy", st.why);
    }

    net_done();
}

TEST_CASE("a request that never got an answer is a failure with a reason",
          "[net_ao]")
{
    chan_fresh();

    drive_to_challenge();
    job_done(KDRV_NET_JOB_HTTP_POST, KODE_TIMEOUT);

    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("timeout", st.why);
    }

    net_done();
}

TEST_CASE("an answer from a join that was left is dropped, and the new join "
          "waits for the worker", "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();

    drive_to_challenge();            /* the challenge is with the worker */
    go_offline();                    /* ... and the address goes */
    go_online();
    TEST_ASSERT_EQUAL_INT(ID_REQ_SIG, (int)radio_take(&q, sizeof(q)));
    answer_cert();
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);   /* the worker is still busy */

    job_done(KDRV_NET_JOB_HTTP_POST, KODE_OK);   /* the stale answer */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_JOINING, chan_now().state);
    secs(1U);
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);   /* the new challenge, once */

    net_done();
}

/* ---- Fix round 1: a renewal that goes wrong must not hurt the live session -- */

/** @brief Up, then the renewal timer fires: the challenge is posted. */
static void drive_to_renewal(void)
{
    drive_to_up();
    ao_spy_drain();
    secs(121U);
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);
}

TEST_CASE("a renewal whose connect answer is malformed leaves the live session "
          "intact and the old client served", "[net_ao]")
{
    IdSignReqEvt sq;

    chan_fresh();

    drive_to_renewal();
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    TEST_ASSERT_EQUAL_UINT(5U, s_nreq);

    http_reply(200U, "{\"serial\":\"KODE0001F00MR1C\",\"brokerUrl\":\"mqtts://h:1\"}");
    {
        ChannelStateEvt const st = last_chan();
        TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
        TEST_ASSERT_EQUAL_STRING("bad_answer", st.why);
    }
    TEST_ASSERT_EQUAL_UINT(5U, s_nreq);   /* the old client was not stopped */
    TEST_ASSERT_EQUAL_UINT(1U, s_nsess);

    /* The old client reconnects: it is served with ITS topics, not blanks. */
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/cmd/in", s_subs[1]);
    TEST_ASSERT_EQUAL_UINT(2U, s_npub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/status", s_pubs[1].topic);

    net_done();
}

TEST_CASE("the old client reconnecting while a renewal asks for the signature "
          "is subscribed and announced again", "[net_ao]")
{
    IdSignReqEvt sq;

    chan_fresh();

    drive_to_renewal();
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));

    s_nsub = 0U;
    s_npub = 0U;
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/cmd/in", s_subs[0]);
    TEST_ASSERT_EQUAL_UINT(1U, s_npub);
    TEST_ASSERT_TRUE(s_pubs[0].retain);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);

    net_done();
}

TEST_CASE("losing the certificate during a renewal stops the client",
          "[net_ao]")
{
    IdSignReqEvt sq;

    chan_fresh();

    drive_to_renewal();
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_NOT_FOUND, 0U);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_NO_IDENTITY, last_chan().state);
    TEST_ASSERT_EQUAL_UINT(5U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(4)->type);
    /* The stop is final, so the presence goes false first. */
    TEST_ASSERT_EQUAL_STRING(KCHAN_WILL, s_pubs[s_npub - 1U].data);
    TEST_ASSERT_EQUAL_UINT(4U, s_pubs[s_npub - 1U].jobs);
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    secs(600U);
    TEST_ASSERT_EQUAL_UINT(5U, s_nreq);   /* nothing is armed, nothing restarts */

    net_done();
}

TEST_CASE("a stale channel timer while the broker is being dialled does not "
          "start a second client", "[net_ao]")
{
    static char        answer[1000];
    static const QEvt  stale = QEVT_INITIALIZER(NET_CHAN_TMO_SIG);

    chan_fresh();

    drive_to_connect();
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    TEST_ASSERT_EQUAL_UINT(1U, s_nsess);

    net_dispatch(&stale);

    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    TEST_ASSERT_EQUAL_UINT(1U, s_nsess);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);

    net_done();
}

/* ======================================================================== *
 * Commands in: a line on cmd/in is a console line
 *
 * A message on the live session's cmd/in topic becomes a ConsoleLineEvt whose
 * source is NET, posted to AO_Console with a margin, and is NOT republished as
 * MQTT_MSG_SIG:
 * the channel's topic belongs to the channel, and a subscriber to the general
 * stream must not see the owner's commands. Everything else keeps its path.
 * ======================================================================== */

static const char *const CHAN_T_IN = "kode/KODE0001F00MR1C/cmd/in";

/** @brief What AO_Console's queue and the spy heard since they were last drained. */
typedef struct {
    unsigned       lines;   /**< ConsoleLineEvt count.                     */
    unsigned       msgs;    /**< MQTT_MSG_SIG count.                       */
    ConsoleLineEvt first;   /**< The first console line, if any.           */
} heard_t;

/** @brief Throw away whatever waits in AO_Console's queue. */
static void console_mailbox_drain(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Console);
    while ( xQueueReceive(AO_Console->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Take the lines off AO_Console's queue and the messages off the spy. */
static heard_t heard(void)
{
    heard_t     h;
    const QEvt *e = NULL;

    (void)memset(&h, 0, sizeof(h));
    /* A line off the channel is posted to the console, not published. */
    while ( xQueueReceive(AO_Console->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)CONSOLE_LINE_SIG == e->sig )
        {
            if ( 0U == h.lines )
            {
                h.first = *(const ConsoleLineEvt *)e;
            }
            h.lines++;
        }
        QF_gc(e);
    }
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)MQTT_MSG_SIG == e->sig )
        {
            h.msgs++;
        }
        else
        {
            /* The channel's own state events are not what this is about. */
        }
    }
    ao_spy_drain();
    return h;
}

/** @brief A message as the driver keeps it. */
static kdrv_mqtt_msg_t rx_msg(const char *topic, const void *data, uint16_t len,
                              bool cut, bool retained)
{
    kdrv_mqtt_msg_t m;

    (void)memset(&m, 0, sizeof(m));
    (void)snprintf(m.topic, sizeof(m.topic), "%s", topic);
    (void)memcpy(m.data, data, len);
    m.len      = len;
    m.data_cut = cut;
    m.retained = retained;
    m.channel  = (strcmp(topic, CHAN_T_IN) == 0);   /* the driver tags the channel's client */
    return m;
}

/** @brief The notice the driver's callback posts, delivered. */
static void rx_notice(void)
{
    static const QEvt notice = QEVT_INITIALIZER(NET_MQTT_RX_SIG);

    net_dispatch(&notice);
}

/** @brief The broker delivers one message: the driver keeps it and posts the notice. */
static void rx_mqtt_flags(const char *topic, const void *data, uint16_t len,
                          bool cut, bool retained)
{
    kdrv_mqtt_msg_t const m = rx_msg(topic, data, len, cut, retained);

    rxq_push(&m);
    rx_notice();
}

/** @brief A live message: not retained. */
static void rx_mqtt(const char *topic, const void *data, uint16_t len, bool cut)
{
    rx_mqtt_flags(topic, data, len, cut, false);
}

/** @brief What the log said about refused commands, taken before net_done()
 *         wipes it: every assertion in this section runs AFTER net_done(), so
 *         a failing one cannot leave AO_Net online with armed timers for the
 *         next case to trip over (a re-initialised, still linked time event
 *         is a cycle the tick walks for ever). */
static unsigned refused_count(void)
{
    return logged_count("command refused");
}

/** @brief A fresh channel, up, with AO_Console's queue empty and the spy on the messages. */
static void cmd_fresh(void)
{
    chan_fresh();
    console_mailbox_drain();
    ao_spy_subscribe(MQTT_MSG_SIG);
    drive_to_up();
    ao_spy_drain();
    fake_klog_reset();
}

TEST_CASE("a message on cmd/in becomes a NET console line and is not "
          "republished as MQTT_MSG", "[net_ao]")
{
    heard_t h;

    cmd_fresh();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    h = heard();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, h.lines);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_NET, h.first.src);
    TEST_ASSERT_EQUAL_STRING("ping", h.first.line);
    TEST_ASSERT_EQUAL_UINT(0U, h.msgs);
}

TEST_CASE("a message on any other topic keeps today's path and runs nothing",
          "[net_ao]")
{
    heard_t h;

    cmd_fresh();

    rx_mqtt("kode/KODE0001F00MR1C/cmd/out", "ping", 4U, false);
    rx_mqtt("some/other/topic", "ping", 4U, false);
    h = heard();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, h.lines);
    TEST_ASSERT_EQUAL_UINT(2U, h.msgs);
}

TEST_CASE("the longest line the console takes passes whole", "[net_ao]")
{
    char    line[KODE_CONSOLE_LINE_MAX];
    heard_t h;

    cmd_fresh();

    (void)memset(line, 'a', sizeof(line));
    rx_mqtt(CHAN_T_IN, line, (uint16_t)(KODE_CONSOLE_LINE_MAX - 1U), false);
    h = heard();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, h.lines);
    TEST_ASSERT_EQUAL_UINT((unsigned)(KODE_CONSOLE_LINE_MAX - 1U),
                           (unsigned)strlen(h.first.line));
}

/* Review Focus 3: a payload the console cannot hold whole is never run cut. */
TEST_CASE("a cmd/in payload cut by the driver runs nothing and is logged",
          "[net_ao]")
{
    char     line[KODE_CONSOLE_LINE_MAX];
    heard_t  h;
    unsigned refused;
    bool     said_why;

    cmd_fresh();

    (void)memset(line, 'a', sizeof(line));
    rx_mqtt(CHAN_T_IN, line, (uint16_t)KODE_CONSOLE_LINE_MAX, true);
    h        = heard();
    refused  = refused_count();
    said_why = logged("too long");
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, h.lines);
    TEST_ASSERT_EQUAL_UINT(0U, h.msgs);
    TEST_ASSERT_EQUAL_UINT(1U, refused);
    TEST_ASSERT_TRUE(said_why);
}

TEST_CASE("a cmd/in payload as long as the line buffer has no room for its "
          "terminator and runs nothing", "[net_ao]")
{
    char     line[KODE_CONSOLE_LINE_MAX];
    heard_t  h;
    unsigned refused;

    cmd_fresh();

    (void)memset(line, 'a', sizeof(line));
    rx_mqtt(CHAN_T_IN, line, (uint16_t)KODE_CONSOLE_LINE_MAX, false);
    h       = heard();
    refused = refused_count();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, h.lines);
    TEST_ASSERT_EQUAL_UINT(1U, refused);
}

TEST_CASE("a cmd/in payload with a line break, a NUL or nothing at all runs "
          "nothing and is logged once each", "[net_ao]")
{
    heard_t  h;
    unsigned refused;

    cmd_fresh();

    rx_mqtt(CHAN_T_IN, "ping\nps", 7U, false);
    rx_mqtt(CHAN_T_IN, "ping\r", 5U, false);
    rx_mqtt(CHAN_T_IN, "pi\0ng", 5U, false);
    rx_mqtt(CHAN_T_IN, "", 0U, false);
    h       = heard();
    refused = refused_count();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, h.lines);
    TEST_ASSERT_EQUAL_UINT(0U, h.msgs);
    TEST_ASSERT_EQUAL_UINT(4U, refused);
}

/** @brief A message of a bench client: the driver does not tag it as the channel's. */
static void rx_bench(const char *topic, const void *data, uint16_t len)
{
    kdrv_mqtt_msg_t m = rx_msg(topic, data, len, false, false);

    m.channel = false;
    rxq_push(&m);
    rx_notice();
}

TEST_CASE("a bench client's message on the channel's topic is just a message",
          "[net_ao]")
{
    heard_t h_before;
    heard_t h_after;

    chan_fresh();
    console_mailbox_drain();
    ao_spy_subscribe(MQTT_MSG_SIG);
    ao_spy_drain();

    /* Before any join: no work area, no session. */
    rx_bench(CHAN_T_IN, "ping", 4U);
    h_before = heard();

    /* After one that ended: the session is wiped with the token. */
    drive_to_up();
    go_offline();
    ao_spy_drain();
    rx_bench(CHAN_T_IN, "ping", 4U);
    h_after = heard();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, h_before.lines);
    TEST_ASSERT_EQUAL_UINT(1U, h_before.msgs);
    TEST_ASSERT_EQUAL_UINT(0U, h_after.lines);
    TEST_ASSERT_EQUAL_UINT(1U, h_after.msgs);
}

/* ============================================================================
 * Replies of the commands that came in on cmd/in go out on cmd/out.
 *
 * AO_Log fills a ring in PSRAM; AO_Net drains it onto the live session's
 * out topic, a few lines to a step and one line to a message. These cases
 * fill the ring the way AO_Log does and read what the (stubbed) client was
 * handed. Every case captures what it needs and calls net_done() BEFORE it
 * asserts, for the reason given at refused_count().
 * ======================================================================== */

/** @brief The ring, empty, and what it had lost so far as the baseline. */
static uint32_t ring_clean(void)
{
    kchan_out_clear();
    return kchan_out_lost();
}

/** @brief Put @p n numbered lines into the ring, as AO_Log would. */
static void push_rows(unsigned first, unsigned n)
{
    char line[24];

    for ( unsigned i = 0U; i < n; i++ )
    {
        (void)snprintf(line, sizeof(line), ">|row=%03u", first + i);
        (void)kchan_out_push(line, strlen(line));
    }
}

/** @brief Whether the published lines are rows @p first, @p first + 1, ... */
static bool outs_are_rows(unsigned first, unsigned n)
{
    char want[24];

    if ( s_nout != n )
    {
        return false;
    }
    for ( unsigned i = 0U; i < n; i++ )
    {
        (void)snprintf(want, sizeof(want), ">|row=%03u", first + i);
        if ( strcmp(s_outs[i], want) != 0 )
        {
            return false;
        }
    }
    return true;
}

TEST_CASE("300 reply lines are all published, in order, a few to a step, "
          "with none lost", "[net_ao]")
{
    static const QEvt step = QEVT_INITIALIZER(NET_CHAN_OUT_SIG);
    uint32_t          lost0;
    unsigned          after_one_step;
    unsigned          before_any;
    unsigned          after_all;
    bool              in_order;
    uint32_t          lost1;

    cmd_fresh();
    lost0 = ring_clean();

    push_rows(0U, 300U);
    before_any = s_nout;                 /* nothing moves until AO_Net runs */

    net_dispatch(&step);
    after_one_step = s_nout;
    (void)net_settle();     /* the self-posted steps */
    after_all = s_nout;
    in_order  = outs_are_rows(0U, 300U);
    lost1     = kchan_out_lost();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, before_any);
    TEST_ASSERT_EQUAL_UINT(8U, after_one_step);
    TEST_ASSERT_EQUAL_UINT(300U, after_all);
    TEST_ASSERT_TRUE(in_order);
    TEST_ASSERT_EQUAL_UINT32(lost0, lost1);
}

/* Review Focus 4: a renewal keeps the state UP while the old client is
 * replaced. The old client serves until the moment it is told to stop; a line
 * published after that would go into a client that is stopped or not yet
 * started. */
TEST_CASE("lines pushed while a renewal replaces the client go out on the old "
          "one until it is told to stop, then wait for the new CONNECTED",
          "[net_ao]")
{
    IdSignReqEvt sq;
    IdReqEvt     q;
    static char  answer[1000];
    unsigned     n_renewing;
    unsigned     n_stopped;
    unsigned     n_started;
    unsigned     n_up;
    bool         in_order;
    uint32_t     lost0;
    uint32_t     lost1;

    cmd_fresh();
    s_drv_state = KDRV_MQTT_UP;
    lost0 = ring_clean();

    secs(121U);                           /* the renewal begins */
    (void)radio_take(&q, sizeof(q));
    push_rows(0U, 2U);
    (void)net_settle();
    n_renewing = s_nout;

    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    (void)radio_take(&sq, sizeof(sq));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);             /* the old client is told to stop */
    push_rows(2U, 2U);
    (void)net_settle();
    n_stopped = s_nout;

    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);   /* dialling the broker */
    push_rows(4U, 1U);
    (void)net_settle();
    n_started = s_nout;

    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    (void)net_settle();
    n_up     = s_nout;
    in_order = outs_are_rows(0U, 5U);
    lost1    = kchan_out_lost();
    net_done();

    TEST_ASSERT_EQUAL_UINT(2U, n_renewing);   /* the old client still serves */
    TEST_ASSERT_EQUAL_UINT(2U, n_stopped);    /* told to stop: nothing more */
    TEST_ASSERT_EQUAL_UINT(2U, n_started);
    TEST_ASSERT_EQUAL_UINT(5U, n_up);
    TEST_ASSERT_TRUE(in_order);
    TEST_ASSERT_EQUAL_UINT32(lost0, lost1);
}

/* Item 8: the old client keeps serving a renewal that failed, and a failed
 * renewal can wait ten minutes for its next try. */
TEST_CASE("replies go out on the old client while a renewal is under way and "
          "after it failed", "[net_ao]")
{
    unsigned n_asking;
    unsigned n_failed;
    bool     in_order;
    uint32_t lost0;
    uint32_t lost1;
    ChannelStateEvt st;

    cmd_fresh();
    lost0 = ring_clean();

    secs(121U);                           /* the renewal asks the api */
    push_rows(0U, 2U);
    (void)net_settle();
    n_asking = s_nout;

    http_reply(503U, "{\"error\":\"channel_unavailable\"}");   /* it failed */
    st = last_chan();
    push_rows(2U, 2U);
    (void)net_settle();
    n_failed = s_nout;
    in_order = outs_are_rows(0U, 4U);
    lost1    = kchan_out_lost();
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
    TEST_ASSERT_EQUAL_UINT(2U, n_asking);
    TEST_ASSERT_EQUAL_UINT(4U, n_failed);
    TEST_ASSERT_TRUE(in_order);
    TEST_ASSERT_EQUAL_UINT32(lost0, lost1);
}

TEST_CASE("lines pushed while the broker connection is down wait for the join "
          "that follows and go out when it is back", "[net_ao]")
{
    unsigned n_down;
    unsigned n_back;
    bool     in_order;

    cmd_fresh();
    (void)ring_clean();

    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);   /* the client is stopped, the wait begins */
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    push_rows(0U, 3U);
    (void)net_settle();
    n_down = s_nout;

    rejoin(8U);
    (void)net_settle();
    n_back   = s_nout;
    in_order = outs_are_rows(0U, 3U);
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, n_down);
    TEST_ASSERT_EQUAL_UINT(3U, n_back);
    TEST_ASSERT_TRUE(in_order);
}

TEST_CASE("a client that refuses a line because it just went down keeps it; "
          "one that has no room loses it, counted", "[net_ao]")
{
    unsigned n_denied;
    unsigned n_back;
    bool     in_order;
    uint32_t lost0;
    uint32_t lost_denied;
    uint32_t lost_full;
    unsigned n_full;

    cmd_fresh();
    lost0 = ring_clean();

    s_publish_rc = KODE_DENIED;
    push_rows(0U, 2U);
    (void)net_settle();
    n_denied    = s_nout;
    lost_denied = kchan_out_lost();

    s_publish_rc = KODE_OK;
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);   /* the link event that follows */
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    rejoin(8U);
    (void)net_settle();
    n_back   = s_nout;
    in_order = outs_are_rows(0U, 2U);

    s_publish_rc = KODE_NO_RESOURCES;
    push_rows(2U, 3U);
    (void)net_settle();
    n_full    = s_nout;
    lost_full = kchan_out_lost();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, n_denied);
    TEST_ASSERT_EQUAL_UINT32(lost0, lost_denied);
    TEST_ASSERT_EQUAL_UINT(2U, n_back);
    TEST_ASSERT_TRUE(in_order);
    TEST_ASSERT_EQUAL_UINT(2U, n_full);
    TEST_ASSERT_EQUAL_UINT32(lost0 + 3U, lost_full);
}

TEST_CASE("losing the network drops the waiting lines and says how many",
          "[net_ao]")
{
    ChannelStateEvt st;
    unsigned        n_out;
    uint32_t        lost0;
    uint32_t        lost1;
    bool            ring_empty;

    cmd_fresh();
    lost0 = ring_clean();

    push_rows(0U, 5U);                    /* AO_Net has not run yet */
    ao_spy_drain();
    go_offline();
    st = last_chan();
    (void)net_settle();      /* the wake finds nothing to send */
    n_out      = s_nout;
    lost1      = kchan_out_lost();
    ring_empty = !kchan_out_pending();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, n_out);
    TEST_ASSERT_EQUAL_UINT32(lost0 + 5U, lost1);
    TEST_ASSERT_TRUE(ring_empty);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_NO_NET, st.state);
    TEST_ASSERT_EQUAL_UINT32(lost1, st.lost);
}

TEST_CASE("replies that arrive after the network went are dropped, not kept "
          "for the next session", "[net_ao]")
{
    unsigned n_out;
    uint32_t lost0;
    uint32_t lost1;
    bool     ring_empty;

    cmd_fresh();
    go_offline();
    lost0 = ring_clean();

    push_rows(0U, 2U);                    /* a command still answering */
    (void)net_settle();
    n_out      = s_nout;
    lost1      = kchan_out_lost();
    ring_empty = !kchan_out_pending();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, n_out);
    TEST_ASSERT_EQUAL_UINT32(lost0 + 2U, lost1);
    TEST_ASSERT_TRUE(ring_empty);
}

TEST_CASE("a refused cmd/in payload answers >err code=3 msg=line on the "
          "channel's path, tagged NET", "[net_ao]")
{
    char     line[KODE_CONSOLE_LINE_MAX];
    unsigned replies;
    unsigned tagged_net;

    cmd_fresh();

    (void)memset(line, 'a', sizeof(line));
    rx_mqtt(CHAN_T_IN, line, (uint16_t)KODE_CONSOLE_LINE_MAX, true);    /* cut */
    rx_mqtt(CHAN_T_IN, line, (uint16_t)KODE_CONSOLE_LINE_MAX, false);   /* no room */
    rx_mqtt(CHAN_T_IN, "ping\nps", 7U, false);
    rx_mqtt(CHAN_T_IN, "ping\r", 5U, false);
    rx_mqtt(CHAN_T_IN, "pi\0ng", 5U, false);
    rx_mqtt(CHAN_T_IN, "", 0U, false);

    replies    = logged_count(">err code=3 msg=line");
    tagged_net = 0U;
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, ">err code=3 msg=line") != NULL)
            && (fake_klog_src(i) == (uint8_t)KODE_CONSOLE_SRC_NET) )
        {
            tagged_net++;
        }
    }
    net_done();

    TEST_ASSERT_EQUAL_UINT(6U, replies);
    TEST_ASSERT_EQUAL_UINT(6U, tagged_net);
}

TEST_CASE("a line that runs gets no >err of its own from the channel",
          "[net_ao]")
{
    unsigned replies;

    cmd_fresh();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    replies = logged_count(">err");
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, replies);
}

/* A retained command is the broker replaying a stored message on every
 * (re)connect: `app_run` or `install` would reset the board into the same
 * replay for ever. */
TEST_CASE("a retained cmd/in payload runs nothing and answers >err code=3 "
          "msg=retained on the channel's path, tagged NET", "[net_ao]")
{
    heard_t  h;
    heard_t  h_live;
    unsigned refused;
    unsigned replies;
    unsigned tagged_net;
    bool     said_why;

    cmd_fresh();

    rx_mqtt_flags(CHAN_T_IN, "app_run name=\"x\"", 14U, false, true);
    h          = heard();
    refused    = refused_count();
    said_why   = logged("retained");
    replies    = logged_count(">err code=3 msg=retained");
    tagged_net = 0U;
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, ">err code=3 msg=retained") != NULL)
            && (fake_klog_src(i) == (uint8_t)KODE_CONSOLE_SRC_NET) )
        {
            tagged_net++;
        }
    }

    /* The next, live, message is not tainted by it. */
    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    h_live = heard();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, h.lines);
    TEST_ASSERT_EQUAL_UINT(0U, h.msgs);
    TEST_ASSERT_EQUAL_UINT(1U, refused);
    TEST_ASSERT_TRUE(said_why);
    TEST_ASSERT_EQUAL_UINT(1U, replies);
    TEST_ASSERT_EQUAL_UINT(1U, tagged_net);
    TEST_ASSERT_EQUAL_UINT(1U, h_live.lines);
    TEST_ASSERT_EQUAL_STRING("ping", h_live.first.line);
}

/**
 * @brief Run AO_Net's clock until a line waits at the console, 10 ms at a time.
 *
 * The tick drains the background AOs' queues before each step, so a line the
 * timer gives is only there to read right after the step that gave it, which
 * is where this stops.
 *
 * @param max_ms The longest to wait.
 * @return The milliseconds that went by; @p max_ms plus a step when no line came.
 */
static unsigned ticks_to_line(unsigned max_ms)
{
    unsigned ran = 0U;

    while ( (ran <= max_ms) && (uxQueueMessagesWaiting(AO_Console->eQueue) == 0U) )
    {
        fake_time_advance_ms(LINE_WAIT_STEP_MS);
        (void)net_tick(1U);   /* one step of the harness is 10 ms */
        ran += LINE_WAIT_STEP_MS;
    }
    return ran;
}

/* The line is posted to AO_Console with a margin of two slots, so a console
 * that is behind cannot assert. A line it has no room for stays at the head
 * of the queue and is offered again on a timer: nothing off the internet is
 * dropped for a console that is behind. */
TEST_CASE("a command the console has no room for waits at the head, with one "
          "warning, and goes when there is room", "[net_ao]")
{
    static const QEvt filler = QEVT_INITIALIZER(GUI_STATE_SIG);
    heard_t           h_room;
    heard_t           h_full;
    heard_t           h_late;
    unsigned          refused;
    unsigned          busy;
    unsigned          said;
    unsigned          waited_ms;

    cmd_fresh();

    /* The harness queue holds 32: three free slots are room for a line. */
    for ( unsigned i = 0U; 29U > i; i++ )
    {
        QACTIVE_POST(AO_Console, &filler, NULL);
    }
    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    h_room = heard();
    net_line_done(h_room.first.seq);
    (void)net_settle();

    /* Two free slots are the margin: the line is not taken. */
    for ( unsigned i = 0U; 30U > i; i++ )
    {
        QACTIVE_POST(AO_Console, &filler, NULL);
    }
    fake_klog_reset();
    rx_mqtt(CHAN_T_IN, "pong", 4U, false);
    h_full  = heard();         /* the fillers go, and nothing else was posted */
    refused = refused_count();
    busy    = logged_count(">err code=4 msg=busy");
    said    = logged_count("has no room");

    waited_ms = ticks_to_line(1000U);   /* the retry finds room */
    h_late    = heard();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, h_room.lines);
    TEST_ASSERT_EQUAL_UINT(0U, h_full.lines);
    TEST_ASSERT_EQUAL_UINT(0U, refused);
    TEST_ASSERT_EQUAL_UINT(0U, busy);
    TEST_ASSERT_EQUAL_UINT(1U, said);
    TEST_ASSERT_LESS_THAN_UINT(1000U, waited_ms);
    TEST_ASSERT_EQUAL_UINT(1U, h_late.lines);
    TEST_ASSERT_EQUAL_STRING("pong", h_late.first.line);
}

/* ---- The queue of cmd/in lines ------------------------------------------- */

/** @brief What the console was given since the last call: how many lines, and the first. */
typedef struct {
    unsigned lines;                     /**< How many.                          */
    char     first[KODE_CONSOLE_LINE_MAX]; /**< The first, empty when none.    */
    uint8_t  seq;                       /**< The sequence of the last one.      */
} given_t;

/** @brief The sequence of the line the console was last given: what its end must return. */
static uint8_t s_seq_out;

/** @brief Take the lines off AO_Console's queue, in the order they were given. */
static given_t given(void)
{
    given_t     g;
    const QEvt *e = NULL;

    (void)memset(&g, 0, sizeof(g));
    while ( xQueueReceive(AO_Console->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)CONSOLE_LINE_SIG == e->sig )
        {
            if ( 0U == g.lines )
            {
                (void)snprintf(g.first, sizeof(g.first), "%s",
                               ((const ConsoleLineEvt *)e)->line);
            }
            g.seq    = ((const ConsoleLineEvt *)e)->seq;
            s_seq_out = g.seq;
            g.lines++;
        }
        QF_gc(e);
    }
    ao_spy_drain();
    return g;
}

/** @brief The console ends the line it held, with the sequence it was given: AO_Net hears it and acts. */
static void line_ends(void)
{
    net_line_done(s_seq_out);
    (void)net_settle();
}

/** @brief Send the line "l<i>". */
static void rx_numbered(unsigned i)
{
    char line[16];

    (void)snprintf(line, sizeof(line), "l%u", i);
    rx_mqtt(CHAN_T_IN, line, (uint16_t)strlen(line), false);
}

TEST_CASE("test_chan_burst_in_order: five lines at once go to the console one at "
          "a time, each after the last one's end, none refused", "[net_ao]")
{
    given_t  g[6];
    unsigned busy;
    unsigned refused;

    cmd_fresh();

    for ( unsigned i = 0U; 5U > i; i++ )
    {
        rx_numbered(i);
    }
    g[0] = given();               /* only the first has gone */
    for ( unsigned i = 1U; 5U > i; i++ )
    {
        line_ends();
        g[i] = given();           /* the end of one gives the next, and only it */
    }
    line_ends();
    g[5]    = given();
    busy    = logged_count(">err code=4 msg=busy");
    refused = refused_count();
    net_done();

    for ( unsigned i = 0U; 5U > i; i++ )
    {
        char want[16];

        (void)snprintf(want, sizeof(want), "l%u", i);
        TEST_ASSERT_EQUAL_UINT(1U, g[i].lines);
        TEST_ASSERT_EQUAL_STRING(want, g[i].first);
    }
    TEST_ASSERT_EQUAL_UINT(0U, g[5].lines);
    TEST_ASSERT_EQUAL_UINT(0U, busy);
    TEST_ASSERT_EQUAL_UINT(0U, refused);
}

TEST_CASE("test_chan_line_bound: a line the console never ends is given up on "
          "after KODE_CHAN_LINE_BOUND_MS and the next goes", "[net_ao]")
{
    given_t  first;
    given_t  next;
    unsigned waited_ms;
    unsigned said;

    cmd_fresh();

    rx_numbered(0U);
    rx_numbered(1U);
    first     = given();
    waited_ms = ticks_to_line(KODE_CHAN_LINE_BOUND_MS + 1000U);
    next      = given();
    said      = logged_count("did not answer");
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, first.lines);
    TEST_ASSERT_EQUAL_STRING("l0", first.first);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(KODE_CHAN_LINE_BOUND_MS - LINE_WAIT_STEP_MS, waited_ms);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(KODE_CHAN_LINE_BOUND_MS + LINE_WAIT_STEP_MS, waited_ms);
    TEST_ASSERT_EQUAL_UINT(1U, next.lines);
    TEST_ASSERT_EQUAL_STRING("l1", next.first);
    TEST_ASSERT_EQUAL_UINT(1U, said);
}

TEST_CASE("test_chan_queue_full: the line past KODE_CHAN_QUEUE_MAX is answered "
          "busy on the channel's path, and the first ones all run", "[net_ao]")
{
    given_t  g;
    given_t  after;
    unsigned replies;
    unsigned tagged_net;
    unsigned refused;
    unsigned ran_in_order = 0U;

    cmd_fresh();

    for ( unsigned i = 0U; KODE_CHAN_QUEUE_MAX >= i; i++ )
    {
        rx_numbered(i);
    }
    replies    = logged_count(">err code=4 msg=busy");
    refused    = refused_count();
    tagged_net = 0U;
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, ">err code=4 msg=busy") != NULL)
            && (fake_klog_src(i) == (uint8_t)KODE_CONSOLE_SRC_NET) )
        {
            tagged_net++;
        }
    }
    for ( unsigned i = 0U; KODE_CHAN_QUEUE_MAX > i; i++ )
    {
        char want[16];

        (void)snprintf(want, sizeof(want), "l%u", i);
        g = given();
        if ( (1U == g.lines) && (strcmp(g.first, want) == 0) )
        {
            ran_in_order++;
        }
        line_ends();
    }
    after = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, replies);
    TEST_ASSERT_EQUAL_UINT(1U, tagged_net);
    TEST_ASSERT_EQUAL_UINT(1U, refused);
    TEST_ASSERT_EQUAL_UINT((unsigned)KODE_CHAN_QUEUE_MAX, ran_in_order);
    TEST_ASSERT_EQUAL_UINT(0U, after.lines);   /* the one refused never runs */
}

TEST_CASE("test_chan_queue_survives_reconnect: three lines waiting when the channel "
          "drops and rejoins run once each, after it is back", "[net_ao]")
{
    static const QEvt busy = QEVT_INITIALIZER(NET_LINE_BUSY_SIG);
    given_t  before;
    given_t  down;
    given_t  back;
    given_t  second;
    given_t  third;
    given_t  rest;

    cmd_fresh();

    rx_numbered(0U);
    rx_numbered(1U);
    rx_numbered(2U);
    before = given();                       /* l0 went out ... */
    net_dispatch(&busy);                    /* ... the console was busy and took nothing: all three wait */

    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    (void)net_settle();
    down = given();                         /* nothing goes into a channel nobody can answer on */

    radio_drain();                          /* the fast mode's note to the radio */
    rejoin(8U);
    (void)net_settle();
    back = given();                         /* back: l0 goes, once */
    line_ends();
    second = given();
    line_ends();
    third  = given();
    line_ends();
    rest = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, before.lines);
    TEST_ASSERT_EQUAL_STRING("l0", before.first);
    TEST_ASSERT_EQUAL_UINT(0U, down.lines);
    TEST_ASSERT_EQUAL_UINT(1U, back.lines);
    TEST_ASSERT_EQUAL_STRING("l0", back.first);
    TEST_ASSERT_EQUAL_UINT(1U, second.lines);
    TEST_ASSERT_EQUAL_STRING("l1", second.first);
    TEST_ASSERT_EQUAL_UINT(1U, third.lines);
    TEST_ASSERT_EQUAL_STRING("l2", third.first);
    TEST_ASSERT_EQUAL_UINT(0U, rest.lines);
}

TEST_CASE("a console end with no line out runs nothing", "[net_ao]")
{
    given_t g;

    cmd_fresh();

    line_ends();
    rx_numbered(0U);
    g = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, g.lines);
}

/** @brief Run AO_Net's clock for @p ms without the final 10 ms step being dispatched: its timers fire and queue. */
static void tick_queue_only(unsigned ms)
{
    unsigned const steps = ms / LINE_WAIT_STEP_MS;

    if ( 1U < steps )
    {
        (void)net_tick(steps - 1U);
    }
    (void)qf_support_pump(AO_Net, 0U);   /* the ticks alone: an expiry is queued, not dispatched */
}

/* The bound of a line and the console's end of it can meet: the bound expired
 * and queued its event, and the end was already ahead of it in the queue. The
 * end pops the line and offers the next, which arms the timer again, and the
 * expiry that was queued is one arming behind: it must not be read as the
 * console not answering the line that is out now. */
TEST_CASE("test_chan_stale_bound: an expiry queued behind the console's end of the line is "
          "ignored; the next line is not popped and nothing is lost", "[net_ao]")
{
    given_t  l0;
    given_t  l1;
    given_t  l2;
    given_t  rest;
    unsigned said;

    cmd_fresh();

    rx_numbered(0U);
    rx_numbered(1U);
    rx_numbered(2U);
    l0 = given();
    tick_queue_only(KODE_CHAN_LINE_BOUND_MS - LINE_WAIT_STEP_MS);   /* one step short of the bound */
    net_line_done(l0.seq);                                          /* queued first ... */
    (void)qf_support_pump(AO_Net, 0U);                              /* ... then the bound expires */
    (void)net_settle();                                             /* done, then the stale expiry */
    l1   = given();
    said = logged_count("did not answer");
    line_ends();                                                    /* l1 ends: l2 goes */
    l2   = given();
    line_ends();
    rest = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, l0.lines);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, l1.lines, "the stale expiry gave l2 the console while l1 ran");
    TEST_ASSERT_EQUAL_STRING("l1", l1.first);
    TEST_ASSERT_EQUAL_UINT(0U, said);
    TEST_ASSERT_EQUAL_UINT(1U, l2.lines);
    TEST_ASSERT_EQUAL_STRING("l2", l2.first);
    TEST_ASSERT_EQUAL_UINT(0U, rest.lines);
}

/* The retry of a line the console did not take has the same two meanings and
 * the same hazard: its expiry, queued, must not read as the 30 s bound. */
TEST_CASE("test_chan_stale_retry: a retry expiry queued behind a new line does not "
          "drop the head that was handed back", "[net_ao]")
{
    static const QEvt busy = QEVT_INITIALIZER(NET_LINE_BUSY_SIG);
    given_t  l0;
    given_t  again;
    given_t  rest;
    unsigned said;

    cmd_fresh();

    rx_numbered(0U);
    l0 = given();
    net_dispatch(&busy);                         /* handed back: the retry is armed */
    tick_queue_only(100U);                       /* the retry expires and queues its event */
    rx_numbered(1U);                             /* a new line: the pump offers l0 again first */
    (void)net_settle();                          /* ... then the queued retry expiry */
    again = given();
    said  = logged_count("did not answer");
    line_ends();
    rest  = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, l0.lines);
    TEST_ASSERT_EQUAL_UINT(1U, again.lines);
    TEST_ASSERT_EQUAL_STRING("l0", again.first);
    TEST_ASSERT_EQUAL_UINT(0U, said);
    TEST_ASSERT_EQUAL_UINT(1U, rest.lines);
    TEST_ASSERT_EQUAL_STRING("l1", rest.first);
}

/* A verb that outlives the bound: its end comes when a later line is out. */
TEST_CASE("test_chan_late_done_sequence: the late end of a line the bound gave up on does "
          "not advance the line that took its place", "[net_ao]")
{
    given_t  l0;
    given_t  l1;
    given_t  after_late;
    given_t  l2;

    cmd_fresh();

    rx_numbered(0U);
    rx_numbered(1U);
    rx_numbered(2U);
    l0 = given();
    (void)ticks_to_line(KODE_CHAN_LINE_BOUND_MS + 1000U);   /* the bound passes: l1 goes */
    l1 = given();
    net_line_done(l0.seq);                                  /* l0's verb finally ends */
    (void)net_settle();
    after_late = given();
    line_ends();                                            /* l1's own end */
    l2 = given();
    net_done();

    TEST_ASSERT_EQUAL_STRING("l1", l1.first);
    TEST_ASSERT_NOT_EQUAL_UINT8(l0.seq, l1.seq);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, after_late.lines, "a late end advanced the next line");
    TEST_ASSERT_EQUAL_UINT(1U, l2.lines);
    TEST_ASSERT_EQUAL_STRING("l2", l2.first);
}

TEST_CASE("an end that names the line before the one handed back is ignored too", "[net_ao]")
{
    static const QEvt busy = QEVT_INITIALIZER(NET_LINE_BUSY_SIG);
    given_t  l0;
    given_t  again;
    given_t  next;

    cmd_fresh();

    rx_numbered(0U);
    rx_numbered(1U);
    l0 = given();
    net_dispatch(&busy);
    (void)ticks_to_line(1000U);                  /* offered again, with a new sequence */
    again = given();
    net_line_done(l0.seq);                       /* the first offer's end, late */
    (void)net_settle();
    next = given();
    net_done();

    TEST_ASSERT_EQUAL_STRING("l0", again.first);
    TEST_ASSERT_NOT_EQUAL_UINT8(l0.seq, again.seq);
    TEST_ASSERT_EQUAL_UINT(0U, next.lines);
}

/* The drain takes one ring's worth a step and posts itself for the rest. That post
 * leaves a free slot to spare, so a queue that is nearly full refuses it, and nothing
 * else looks at the ring of a client that is not the channel's: the driver is told. */
TEST_CASE("test_chan_again_refused: a refused self-post of the drain tells the driver a "
          "notice is owed, and the next notice takes the rest", "[net_ao]")
{
    static const QEvt filler = QEVT_INITIALIZER(GUI_STATE_SIG);
    unsigned          owes;
    unsigned          left_stuck;
    unsigned          left_after;

    cmd_fresh();
    for ( unsigned i = 0U; (unsigned)KDRV_MQTT_RX_RING + 2U > i; i++ )
    {
        char line[16];

        (void)snprintf(line, sizeof(line), "l%u", i);
        kdrv_mqtt_msg_t const m = rx_msg(CHAN_T_IN, line, (uint16_t)strlen(line), false, false);
        rxq_push(&m);
    }
    while ( QACTIVE_POST_X(AO_Net, &filler, 0U, NULL) )
    {
        /* until the queue refuses: no slot to spare */
    }
    s_rx_owes = 0U;
    rx_notice();
    owes       = s_rx_owes;
    left_stuck = s_rxq_len;
    (void)net_settle();

    rx_numbered(99U);                    /* the next message: its notice is sent, and takes the rest */
    left_after = s_rxq_len;
    (void)given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, owes);
    TEST_ASSERT_EQUAL_UINT(2U, left_stuck);
    TEST_ASSERT_EQUAL_UINT(0U, left_after);
}

/* The channel ends (chan=off, the network going): commands that have not run are not run
 * later, and none of them reaches the subscribers of a general message. */
TEST_CASE("test_chan_halt_drops_off: chan=off drops the queued lines and the driver's cmd/in "
          "messages, publishes none of them and says how many", "[net_ao]")
{
    given_t  out;
    given_t  after;
    heard_t  h;
    kdrv_mqtt_msg_t m;
    unsigned dropped_notes;
    bool     counted;
    unsigned left;
    unsigned published;
    bool     other_topic;

    fake_nvs_reset();
    cmd_fresh();
    rx_numbered(0U);
    rx_numbered(1U);
    rx_numbered(2U);
    out = given();                                         /* l0 is out; l1 and l2 wait */
    m = rx_msg(CHAN_T_IN, "x0", 2U, false, false);
    rxq_push(&m);
    m = rx_msg("kode/someone/else", "hi", 2U, false, false);
    rxq_push(&m);
    m = rx_msg(CHAN_T_IN, "x1", 2U, false, false);
    rxq_push(&m);                                          /* their notice is behind the halt */
    fake_klog_reset();

    (void)config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true, .b = false });
    h             = heard();
    left          = s_rxq_len;
    dropped_notes = logged_count("had not run are dropped");
    counted       = logged("5 command(s)");
    /* config_ask() passes over what the spy heard before the answer, so the
     * publishes are read from the log: one line for each message published. */
    published     = logged_count("mqtt rx topic=");
    other_topic   = logged("mqtt rx topic=kode/someone/else");
    net_line_done(out.seq);
    (void)net_settle();
    after = given();
    fake_nvs_reset();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, out.lines);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, published, "only the other topic's message is published");
    TEST_ASSERT_TRUE(other_topic);
    TEST_ASSERT_EQUAL_UINT(0U, h.lines);
    TEST_ASSERT_EQUAL_UINT(0U, left);
    TEST_ASSERT_EQUAL_UINT(1U, dropped_notes);
    TEST_ASSERT_TRUE_MESSAGE(counted, "three queued and two in the driver's ring");
    TEST_ASSERT_EQUAL_UINT(0U, after.lines);
}

/* A network loss is not the channel being told to go: the queue waits, nothing is handed
 * to the console while the channel is down, and the lines run once, in order, after it is up. */
TEST_CASE("test_chan_network_loss_keeps_queue: three queued lines survive the network going "
          "and run once each after the channel is up again", "[net_ao]")
{
    static const QEvt busy = QEVT_INITIALIZER(NET_LINE_BUSY_SIG);
    kdrv_mqtt_msg_t m;
    given_t  before;
    given_t  down;
    given_t  back;
    given_t  second;
    given_t  third;
    given_t  rest;
    unsigned dropped_notes;

    cmd_fresh();
    rx_numbered(0U);
    rx_numbered(1U);
    rx_numbered(2U);
    before = given();
    net_dispatch(&busy);                         /* none is out */
    m = rx_msg(CHAN_T_IN, "l3", 2U, false, false);
    rxq_push(&m);                                /* one more still in the driver's ring */
    fake_klog_reset();

    go_offline();
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    (void)net_settle();
    rx_notice();                                 /* the notice that was behind the loss */
    down          = given();
    dropped_notes = logged_count("had not run are dropped");

    go_online();
    {
        static char  answer[1000];
        IdReqEvt     q;
        IdSignReqEvt sq;

        (void)radio_take(&q, sizeof(q));
        answer_cert();

        http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\"}");
        (void)radio_take(&sq, sizeof(sq));
        answer_sign((uint8_t)KODE_ST_OK, 70U);
        connect_answer(answer, sizeof(answer));
        http_reply(200U, answer);
        job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
        mqtt_link(KDRV_NET_MQTT_CONNECTED);
    }
    (void)net_settle();
    back = given();
    line_ends();
    second = given();
    line_ends();
    third = given();
    line_ends();
    rest = given();
    line_ends();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, before.lines);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, down.lines, "a line went to the console while the channel was down");
    TEST_ASSERT_EQUAL_UINT(0U, dropped_notes);
    TEST_ASSERT_EQUAL_UINT(1U, back.lines);
    TEST_ASSERT_EQUAL_STRING("l0", back.first);
    TEST_ASSERT_EQUAL_STRING("l1", second.first);
    TEST_ASSERT_EQUAL_STRING("l2", third.first);
    TEST_ASSERT_EQUAL_STRING("l3", rest.first);
}

/* The tag, not the topic: after the session is wiped, a channel entry still in the ring is not
 * a general message. */
TEST_CASE("test_chan_tagged_entry_never_publishes: a channel entry drained after the session is "
          "wiped joins the queue and is not published", "[net_ao]")
{
    kdrv_mqtt_msg_t m;
    heard_t  h;
    given_t  down;
    unsigned published;

    cmd_fresh();
    go_offline();                                /* the session is wiped */
    (void)net_settle();
    ao_spy_drain();
    fake_klog_reset();
    m = rx_msg("kode/other/cmd/in", "late", 4U, false, false);
    m.channel = true;                            /* the driver tagged it: the channel's client */
    rxq_push(&m);
    rx_notice();
    h         = heard();
    published = logged_count("mqtt rx topic=");
    down      = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, h.msgs);
    TEST_ASSERT_EQUAL_UINT(0U, published);
    TEST_ASSERT_EQUAL_UINT(0U, down.lines);
}

TEST_CASE("test_chan_off_offline_drops_queue: chan=off with no network drops the queued lines, "
          "and none runs when the channel is switched on and joins", "[net_ao]")
{
    given_t  out;
    given_t  after;
    given_t  later;
    unsigned noted;
    bool     counted;

    fake_nvs_reset();
    cmd_fresh();
    rx_numbered(0U);
    rx_numbered(1U);
    rx_numbered(2U);
    out = given();                               /* l0 out; l1 and l2 wait */
    go_offline();                                /* no network: the queue is kept */
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    (void)net_settle();
    fake_klog_reset();

    (void)config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true, .b = false });
    noted   = logged_count("had not run are dropped");
    counted = logged("3 command(s)");
    net_line_done(out.seq);
    (void)net_settle();
    after = given();

    (void)config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true, .b = true });
    go_online();
    {
        static char  answer[1000];
        IdReqEvt     q;
        IdSignReqEvt sq;

        (void)radio_take(&q, sizeof(q));
        answer_cert();
        http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\"}");
        (void)radio_take(&sq, sizeof(sq));
        answer_sign((uint8_t)KODE_ST_OK, 70U);
        connect_answer(answer, sizeof(answer));
        http_reply(200U, answer);
        job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
        mqtt_link(KDRV_NET_MQTT_CONNECTED);
    }
    (void)net_settle();
    later = given();
    fake_nvs_reset();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, out.lines);
    TEST_ASSERT_EQUAL_UINT(1U, noted);
    TEST_ASSERT_TRUE_MESSAGE(counted, "the NOTICE counts the three lines");
    TEST_ASSERT_EQUAL_UINT(0U, after.lines);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, later.lines, "a dropped line ran after the channel came back");
}

TEST_CASE("test_chan_off_drops_late_entry: a channel entry that reaches AO_Net after chan=off is "
          "dropped, counted in a NOTICE and not published", "[net_ao]")
{
    kdrv_mqtt_msg_t m;
    unsigned noted;
    unsigned published;
    given_t  g;

    fake_nvs_reset();
    cmd_fresh();
    (void)config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true, .b = false });
    fake_klog_reset();
    m = rx_msg(CHAN_T_IN, "late", 4U, false, false);
    rxq_push(&m);
    rx_notice();
    noted     = logged_count("halted; 1 command(s)");
    published = logged_count("mqtt rx topic=");
    g         = given();
    fake_nvs_reset();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, noted);
    TEST_ASSERT_EQUAL_UINT(0U, published);
    TEST_ASSERT_EQUAL_UINT(0U, g.lines);
}

/* A bench client's message keeps its path, whatever the channel does. */
TEST_CASE("an untagged message on the channel's own topic is a general message", "[net_ao]")
{
    kdrv_mqtt_msg_t m;
    unsigned published;

    cmd_fresh();
    m = rx_msg(CHAN_T_IN, "x", 1U, false, false);
    m.channel = false;
    rxq_push(&m);
    rx_notice();
    published = logged_count("mqtt rx topic=");
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, published);
}

TEST_CASE("a line the console was too busy for stays at the head, is offered again "
          "shortly and is not answered busy", "[net_ao]")
{
    static const QEvt busy = QEVT_INITIALIZER(NET_LINE_BUSY_SIG);
    given_t  first;
    given_t  again;
    given_t  last;
    unsigned waited_ms;
    unsigned answered;
    unsigned said;

    cmd_fresh();

    rx_numbered(0U);
    rx_numbered(1U);
    first = given();
    net_dispatch(&busy);                  /* a cable command was running */
    waited_ms = ticks_to_line(1000U);
    again     = given();
    answered  = logged_count(">err code=4 msg=busy");
    net_dispatch(&busy);                  /* still busy: the same episode */
    (void)ticks_to_line(1000U);
    (void)given();
    said      = logged_count("console is busy");
    line_ends();
    last      = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, first.lines);
    TEST_ASSERT_EQUAL_STRING("l0", first.first);
    TEST_ASSERT_GREATER_THAN_UINT(0U, waited_ms);
    TEST_ASSERT_LESS_THAN_UINT(1000U, waited_ms);
    TEST_ASSERT_EQUAL_UINT(1U, again.lines);
    TEST_ASSERT_EQUAL_STRING("l0", again.first);   /* the same line, ahead of l1 */
    TEST_ASSERT_EQUAL_UINT(0U, answered);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, said, "one WARN for the whole episode");
    TEST_ASSERT_EQUAL_UINT(1U, last.lines);
    TEST_ASSERT_EQUAL_STRING("l1", last.first);
}

/** @brief Count what the console was given: lines run, in the order sent, and any two at once. */
static void run_check(const given_t *g, unsigned *ran, unsigned *in_order, unsigned *doubled)
{
    if ( 1U < g->lines )
    {
        (*doubled)++;
    }
    if ( 0U != g->lines )
    {
        char want[16];

        (void)snprintf(want, sizeof(want), "l%u", *ran);
        if ( strcmp(want, g->first) == 0 )
        {
            (*in_order)++;
        }
        (*ran)++;
    }
}

/* The driver's callback never waits. A burst is kept in the driver's ring and
 * AO_Net takes it on the notice. This plays the driver's side of the burst
 * (the ring is the fake above: push, and the callback only when the ring was
 * empty) and AO_Net's side for real. It does NOT cover the driver itself: the
 * real ring, its lock, esp-mqtt's task and the PUBACK are in kdrv_net.c, which
 * needs esp-mqtt and so only runs on a board. */
TEST_CASE("a burst of 40 kept by the driver and taken on one notice loses nothing: "
          "32 run in order, the rest are answered busy", "[net_ao]")
{
    unsigned notices  = 0U;
    unsigned ran      = 0U;
    unsigned in_order = 0U;
    unsigned doubled  = 0U;
    unsigned busy_answers;
    unsigned refused;

    cmd_fresh();
    TEST_ASSERT_NOT_NULL(s_mqtt_cb);

    for ( unsigned i = 0U; 40U > i; i++ )
    {
        bool const was_empty = (0U == s_rxq_len);
        char       line[16];
        kdrv_mqtt_msg_t m;

        (void)snprintf(line, sizeof(line), "l%u", i);
        m = rx_msg(CHAN_T_IN, line, (uint16_t)strlen(line), false, false);
        rxq_push(&m);
        if ( was_empty )
        {
            /* The callback: a post, and it says it was taken. */
            TEST_ASSERT_TRUE(s_mqtt_cb(KDRV_NET_MQTT_MSG, NULL, false));
            notices++;
        }
    }
    (void)net_settle();                         /* AO_Net takes the ring */
    for ( unsigned guard = 0U; 100U > guard; guard++ )
    {
        given_t g = given();

        if ( 1U < g.lines )
        {
            doubled++;
        }
        if ( 0U != g.lines )
        {
            char want[16];

            (void)snprintf(want, sizeof(want), "l%u", ran);
            if ( strcmp(want, g.first) == 0 )
            {
                in_order++;
            }
            ran++;
        }
        line_ends();
    }
    busy_answers = logged_count(">err code=4 msg=busy");
    refused      = refused_count();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, notices);        /* one notice for the burst */
    TEST_ASSERT_EQUAL_UINT((unsigned)KODE_CHAN_QUEUE_MAX, ran);
    TEST_ASSERT_EQUAL_UINT((unsigned)KODE_CHAN_QUEUE_MAX, in_order);
    TEST_ASSERT_EQUAL_UINT(0U, doubled);
    TEST_ASSERT_EQUAL_UINT(40U - (unsigned)KODE_CHAN_QUEUE_MAX, busy_answers);
    TEST_ASSERT_EQUAL_UINT(busy_answers, refused);
    TEST_ASSERT_EQUAL_UINT(0U, s_rxq_len);
}

TEST_CASE("AO_Net takes from the driver on each notice, until the ring is empty, and "
          "takes nothing without one", "[net_ao]")
{
    kdrv_mqtt_msg_t m;
    unsigned        takes_idle;
    unsigned        takes_notice;
    unsigned        left;
    given_t         g;

    cmd_fresh();
    s_rxq_takes = 0U;

    m = rx_msg(CHAN_T_IN, "l0", 2U, false, false);
    rxq_push(&m);
    m = rx_msg(CHAN_T_IN, "l1", 2U, false, false);
    rxq_push(&m);
    m = rx_msg(CHAN_T_IN, "l2", 2U, false, false);
    rxq_push(&m);
    (void)net_settle();
    takes_idle = s_rxq_takes;                   /* no notice, no take */

    rx_notice();
    takes_notice = s_rxq_takes;                 /* three messages and the empty answer */
    left         = s_rxq_len;
    g            = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, takes_idle);
    TEST_ASSERT_EQUAL_UINT(4U, takes_notice);
    TEST_ASSERT_EQUAL_UINT(0U, left);
    TEST_ASSERT_EQUAL_UINT(1U, g.lines);        /* l0 is out; l1 and l2 wait their turn */
    TEST_ASSERT_EQUAL_STRING("l0", g.first);
}

/** @brief CHAN_WATCH_MS of net_ao.c, which is private to it: how often the channel looks at its client. */
#define WATCH_MS_HERE  15000U

TEST_CASE("a notice the driver could not post is made up for by the watch: the "
          "ring is emptied within CHAN_WATCH_MS", "[net_ao]")
{
    kdrv_mqtt_msg_t m;
    given_t         early;
    given_t         late;
    unsigned        waited_ms;

    cmd_fresh();

    m = rx_msg(CHAN_T_IN, "l0", 2U, false, false);
    rxq_push(&m);                               /* the notice was refused */
    early     = given();
    waited_ms = ticks_to_line(WATCH_MS_HERE + 1000U);
    late      = given();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, early.lines);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(WATCH_MS_HERE + LINE_WAIT_STEP_MS, waited_ms);
    TEST_ASSERT_EQUAL_UINT(1U, late.lines);
    TEST_ASSERT_EQUAL_STRING("l0", late.first);
}

/* The drain's own post asks for a free slot to spare, so a full queue refuses
 * it. The lines must not be left in the ring until some other event happens by:
 * the drain comes back on its timer. */
TEST_CASE("a refused self-post of the drain is retried on a timer and every "
          "line still goes out", "[net_ao]")
{
    static const QEvt step   = QEVT_INITIALIZER(NET_CHAN_OUT_SIG);
    static const QEvt filler = QEVT_INITIALIZER(GUI_STATE_SIG);
    unsigned          n_first;
    unsigned          n_stuck;
    unsigned          n_done;
    bool              in_order;
    uint32_t          lost0;
    uint32_t          lost1;

    cmd_fresh();
    lost0 = ring_clean();

    push_rows(0U, 20U);                   /* one wake is queued */
    for ( unsigned i = 0U; 31U > i; i++ )
    {
        QACTIVE_POST(AO_Net, &filler, NULL);   /* now the queue is full */
    }
    net_dispatch(&step);
    n_first = s_nout;                     /* 8, and the post was refused */
    (void)net_settle();      /* the queue empties, and the wake that
                                           * was first in it is refused again */
    n_stuck = s_nout;

    secs(1U);                             /* the retry fires */
    n_done   = s_nout;
    in_order = outs_are_rows(0U, 20U);
    lost1    = kchan_out_lost();
    net_done();

    TEST_ASSERT_EQUAL_UINT(8U, n_first);
    TEST_ASSERT_EQUAL_UINT(16U, n_stuck);
    TEST_ASSERT_EQUAL_UINT(20U, n_done);
    TEST_ASSERT_TRUE(in_order);
    TEST_ASSERT_EQUAL_UINT32(lost0, lost1);
}

/* ======================================================================== *
 * The fast mode, and the two ways the channel could be left deaf
 *
 * The first cmd/in line of a burst asks AO_Radio for no Wi-Fi power save, every
 * further line holds it, and CHAN_FAST_HOLD_MS (60 s) after the last one it
 * goes back. Leaving the channel ends it at once. Beside it, a channel that
 * says "up" over a client the driver no longer has is a broker lost (the link
 * event that would have said so can be dropped), and so is the drop of the old
 * client of a renewal.
 * ======================================================================== */

/** @brief What AO_Net asked AO_Radio about the fast mode since the mailbox was emptied. */
typedef struct {
    unsigned on;      /**< Requests for no power save.            */
    unsigned off;     /**< Requests for the default.              */
    unsigned other;   /**< Anything else AO_Net posted.           */
    bool     last_on; /**< The wish of the last fast request.     */
} fast_posts_t;

/** @brief Empty AO_Radio's mailbox, counting the fast requests in it. */
static fast_posts_t radio_fast_posts(void)
{
    fast_posts_t f;
    const QEvt  *e = NULL;

    (void)memset(&f, 0, sizeof(f));
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)WIFI_FAST_REQ_SIG == e->sig )
        {
            f.last_on = ((const WifiFastEvt *)e)->on;
            if ( f.last_on )
            {
                f.on++;
            }
            else
            {
                f.off++;
            }
        }
        else
        {
            f.other++;
        }
        QF_gc(e);
    }
    return f;
}

/** @brief AO_Net's NET_FAST_JOB_HOLD_MS: how long the fast mode outlives the last job. */
#define JOB_FAST_HOLD_MS  5000U

/**
 * @brief Let the tail of the join's last job run out, and empty AO_Radio's mailbox.
 *
 * What a join's jobs ask for is not what the cases that follow are about: they
 * look at the channel's own hold.
 */
static void fast_idle(void)
{
    secs((JOB_FAST_HOLD_MS / 1000U) + 1U);
    radio_drain();
}

TEST_CASE("the first cmd/in line asks for the fast mode once, and later lines "
          "only hold it", "[net_ao][fast]")
{
    fast_posts_t    after_first;
    fast_posts_t    after_more;
    ChannelStateEvt before;
    ChannelStateEvt st;
    unsigned        notices;

    cmd_fresh();
    fast_idle();
    before = chan_now();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    after_first = radio_fast_posts();
    st = chan_now();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    secs(10U);
    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    after_more = radio_fast_posts();
    notices = logged_count("fast mode on");
    net_done();

    TEST_ASSERT_FALSE(before.fast);
    TEST_ASSERT_EQUAL_UINT(1U, after_first.on);
    TEST_ASSERT_EQUAL_UINT(0U, after_first.off);
    TEST_ASSERT_TRUE(st.fast);
    TEST_ASSERT_EQUAL_UINT(0U, after_more.on);
    TEST_ASSERT_EQUAL_UINT(0U, after_more.off);
    TEST_ASSERT_EQUAL_UINT(1U, notices);
}

TEST_CASE("the fast mode ends 60 s after the last line, and every line moves "
          "that back", "[net_ao][fast]")
{
    fast_posts_t    early;
    fast_posts_t    late;
    ChannelStateEvt held;
    ChannelStateEvt gone;
    unsigned        notices;

    cmd_fresh();
    fast_idle();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    radio_drain();
    secs(30U);
    rx_mqtt(CHAN_T_IN, "ping", 4U, false);   /* the hold restarts here */
    secs(59U);                               /* 89 s after the first line */
    early = radio_fast_posts();
    held  = chan_now();

    secs(2U);                                /* 61 s after the last one */
    late = radio_fast_posts();
    gone = chan_now();
    notices = logged_count("fast mode off");
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, early.off);
    TEST_ASSERT_TRUE(held.fast);
    TEST_ASSERT_EQUAL_UINT(1U, late.off);
    TEST_ASSERT_EQUAL_UINT(0U, late.on);
    TEST_ASSERT_FALSE(gone.fast);
    TEST_ASSERT_EQUAL_UINT(1U, notices);
}

TEST_CASE("a line after the fast mode ended turns it on again", "[net_ao][fast]")
{
    fast_posts_t again;

    cmd_fresh();
    fast_idle();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    secs(61U);
    radio_drain();
    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    again = radio_fast_posts();
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, again.on);
}

TEST_CASE("lines the channel refuses do not turn the fast mode on",
          "[net_ao][fast]")
{
    fast_posts_t f;

    cmd_fresh();
    fast_idle();

    rx_mqtt_flags(CHAN_T_IN, "ping", 4U, false, true);   /* retained */
    rx_mqtt(CHAN_T_IN, "", 0U, false);                   /* empty */
    rx_mqtt(CHAN_T_IN, "pi\nng", 5U, false);             /* two lines */
    rx_mqtt("some/other/topic", "ping", 4U, false);      /* not the channel's */
    f = radio_fast_posts();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, f.on);
}

TEST_CASE("losing the address ends the fast mode at once", "[net_ao][fast]")
{
    fast_posts_t f;

    cmd_fresh();
    fast_idle();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    radio_drain();
    go_offline();
    f = radio_fast_posts();
    secs(120U);                         /* the hold has nothing left to end */
    net_done();

    TEST_ASSERT_EQUAL_UINT(1U, f.off);
}

TEST_CASE("a refusal by the api ends the channel's hold at once, and the fast "
          "mode when the jobs' tail does", "[net_ao][fast]")
{
    IdSignReqEvt    sq;
    fast_posts_t    f;
    fast_posts_t    tail;
    ChannelStateEvt st;

    cmd_fresh();
    fast_idle();
    secs(100U);
    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    radio_drain();
    secs(21U);                                    /* the renewal begins */
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    http_reply(401U, "{\"error\":\"untrusted_cert\"}");
    f  = radio_fast_posts();              /* the client's stop is with the worker */
    st = last_chan();
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    secs(6U);                             /* the jobs' tail runs out */
    tail = radio_fast_posts();
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_REFUSED, st.state);
    TEST_ASSERT_FALSE(st.fast);
    TEST_ASSERT_EQUAL_UINT(0U, f.off);
    TEST_ASSERT_EQUAL_UINT(1U, tail.off);
}

TEST_CASE("losing the certificate ends the channel's hold at once, and the fast "
          "mode when the jobs' tail does", "[net_ao][fast]")
{
    IdSignReqEvt sq;
    fast_posts_t f;
    fast_posts_t tail;

    cmd_fresh();
    fast_idle();
    secs(100U);
    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    radio_drain();
    secs(21U);                                    /* the renewal begins */
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_NOT_FOUND, 0U);
    f = radio_fast_posts();               /* the client's stop is with the worker */
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    secs(6U);                             /* the jobs' tail runs out */
    tail = radio_fast_posts();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, f.off);
    TEST_ASSERT_EQUAL_UINT(1U, tail.off);
}

/* ---- M1: a lost DISCONNECTED must not leave the channel deaf ---------------- */

TEST_CASE("a channel that says up over a client the driver lost is a broker "
          "lost within the watch interval, and joins again", "[net_ao][watch]")
{
    ChannelStateEvt before;
    ChannelStateEvt after;
    ChannelStateEvt again;
    unsigned        jobs;

    chan_fresh();
    drive_to_up();
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);

    s_drv_state = KDRV_MQTT_DOWN;      /* the DISCONNECTED never reached AO_Net */
    secs(14U);
    before = chan_now();
    secs(2U);
    after = last_chan();
    jobs  = s_nreq;
    (void)memset(&again, 0, sizeof(again));
    if ( (uint8_t)CHAN_WAITING == after.state )
    {
        job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
        rejoin(8U);
        again = chan_now();
    }
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, before.state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, after.state);
    TEST_ASSERT_EQUAL_STRING("broker_lost", after.why);
    TEST_ASSERT_EQUAL_UINT(4U, jobs);                  /* the dead client is stopped */
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, again.state);
}

TEST_CASE("a client that is still up is left alone by the watch", "[net_ao][watch]")
{
    ChannelStateEvt st;
    unsigned        jobs;

    chan_fresh();
    drive_to_up();
    secs(100U);
    st   = chan_now();
    jobs = s_nreq;
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, st.state);
    TEST_ASSERT_EQUAL_UINT(3U, jobs);
}

/* ---- M2: the old client of a renewal drops ---------------------------------- */

TEST_CASE("the old client dropping while a renewal asks the api ends the "
          "renewal and joins again through the back-off", "[net_ao][watch]")
{
    ChannelStateEvt waiting;
    ChannelStateEvt again;
    unsigned        jobs_at_drop;
    unsigned        subs_before;
    unsigned        subs;
    int             stop_type = -1;

    chan_fresh();
    drive_to_renewal();                        /* the challenge is in the air */
    subs_before = s_nsub;

    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    waiting = last_chan();
    jobs_at_drop = s_nreq;
    (void)memset(&again, 0, sizeof(again));
    subs = subs_before;
    if ( (uint8_t)CHAN_WAITING == waiting.state )
    {
        /* The worker still holds the challenge: its answer belongs to the
         * renewal that just ended, and it frees the worker for the stop. */
        http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
        if ( 4U < s_nreq )
        {
            stop_type = (int)req_at(4)->type;
        }
        job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
        rejoin(8U);
        again = chan_now();
        subs  = s_nsub;
    }
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, waiting.state);
    TEST_ASSERT_EQUAL_STRING("broker_lost", waiting.why);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4U, waiting.next_s);
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(7U, waiting.next_s);
    TEST_ASSERT_EQUAL_UINT(4U, jobs_at_drop);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, stop_type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, again.state);
    TEST_ASSERT_EQUAL_UINT(subs_before + 1U, subs);
}

TEST_CASE("an answer still out when the old client drops is dropped, not "
          "taken by the next try", "[net_ao][watch]")
{
    ChannelStateEvt waiting;
    IdSignReqEvt    sq;
    enum_t          asked;

    chan_fresh();
    drive_to_renewal();                        /* the challenge is in the air */
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    waiting = last_chan();
    secs(8U);                                  /* the wait ends first        */
    radio_drain();
    http_reply(200U, "{\"nonce\":\"v1.connect.stale\"}");
    asked = radio_take(&sq, sizeof(sq));
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, waiting.state);
    TEST_ASSERT_NOT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)asked);
}

TEST_CASE("the old client dropping while a failed renewal waits for its next "
          "try ends that wait too", "[net_ao][watch]")
{
    ChannelStateEvt failed;
    ChannelStateEvt waiting;
    ChannelStateEvt again;
    int             stop_type = -1;

    chan_fresh();
    drive_to_renewal();
    http_reply(503U, "{\"error\":\"channel_unavailable\"}");   /* the renewal failed */
    failed = last_chan();

    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    waiting = last_chan();
    (void)memset(&again, 0, sizeof(again));
    if ( strcmp(waiting.why, "broker_lost") == 0 )
    {
        if ( 4U < s_nreq )
        {
            stop_type = (int)req_at(4)->type;
        }
        job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
        rejoin(26U);   /* the second failure in a row: 20 s at 75..125 % */
        again = chan_now();
    }
    net_done();

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, failed.state);
    TEST_ASSERT_EQUAL_STRING("channel_unavailable", failed.why);
    TEST_ASSERT_EQUAL_STRING("broker_lost", waiting.why);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, stop_type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, again.state);
}

/* ---- The setting ------------------------------------------------------------- */

/* `chan=off` leaves at once - the goodbye, the client's STOP, the state OFF -
 * and an address after it joins nothing; `on` joins again with an address. */
TEST_CASE("chan off leaves the channel at once and an address joins nothing",
          "[net_ao]")
{
    const KodeConfigAnsEvt *a;
    IdReqEvt                q;

    fake_nvs_reset();
    chan_fresh();
    drive_to_up();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);
    s_drv_state = KDRV_MQTT_UP;
    s_nreq = 3U;

    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true });
    pull_jobs();   /* the STOP the setting made the AO submit */
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    TEST_ASSERT_TRUE(a->stored);
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(3)->type);
    TEST_ASSERT_TRUE(logged("channel: off"));
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    s_drv_state = KDRV_MQTT_DOWN;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_OFF, chan_now().state);

    go_offline();
    radio_drain();
    go_online();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&q, sizeof(q)),
                                  "an address with chan=off asked the C5 for the "
                                  "certificate");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_OFF, chan_now().state);

    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true,
                                            .b = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("on", a->value);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)ID_REQ_SIG, (int)radio_take(&q, sizeof(q)),
                                  "chan=on with an address did not join");

    fake_nvs_reset();
    net_done();
}

TEST_CASE("chan stored off: the board boots with the channel off", "[net_ao]")
{
    uint8_t const off = 0U;
    IdReqEvt      q;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_CHAN, off));
    chan_fresh();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_OFF, chan_now().state);
    go_online();
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));

    fake_nvs_reset();
    net_done();
}

/* ---- The persistent session ------------------------------------------------- */

/* The channel asks the broker to keep what is sent to cmd/in while the Kode is
 * away: MQTT 5, no clean start, the age `chan_keep` holds and a QoS 1
 * subscribe. */
TEST_CASE("the channel's session asks for MQTT 5, a kept session and QoS 1",
          "[net_ao]")
{
    fake_nvs_reset();
    chan_fresh();
    drive_to_up();

    TEST_ASSERT_EQUAL_UINT(1U, s_nsess);
    TEST_ASSERT_TRUE(s_sess.mqtt5);
    TEST_ASSERT_TRUE(s_sess.keep_session);
    TEST_ASSERT_EQUAL_UINT32(86400U, s_sess.session_expiry_s);   /* 1d, the default */
    TEST_ASSERT_EQUAL_UINT8(1U, s_sess.sub_qos);
    TEST_ASSERT_EQUAL_UINT(1U, s_nsub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/cmd/in", s_subs[0]);
    TEST_ASSERT_EQUAL_UINT32(86400U, chan_now().keep_s);

    fake_nvs_reset();
    net_done();
}

/* A bench client is not the channel's: AO_Net hands the driver no session for
 * it. What the driver then starts (3.1.1, a clean start, QoS 0 subscribes) is
 * checked on the bench: the MQTT half of kdrv_net.c is not built on the host. */
TEST_CASE("mqtt_up hands the driver no session", "[net_ao]")
{
    uint8_t const off = 0U;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_CHAN, off));
    chan_fresh();
    go_online();

    mqtt_up_request("mqtt://broker.example:1883", "", "");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_START, (int)req_at(0)->type);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nsess, "a bench client was given the channel's session");

    fake_nvs_reset();
    net_done();
}

/* The console closes `mqtt_down` on the answer: a refusal that is published
 * as the client's state reads as a stop that worked. */
TEST_CASE("mqtt_down while a network job runs is refused as busy, not answered with a state",
          "[net_ao][answers]")
{
    uint8_t       err = 0U;
    uint8_t const off = 0U;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_CHAN, off));
    chan_fresh();
    go_online();

    mqtt_up_request("mqtt://broker.example:1883", "", "");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);                  /* the start is in flight */
    uint32_t const before = mqtt_answers(NULL);
    fake_klog_reset();

    TEST_ASSERT_TRUE(mqtt_down_request());
    (void)net_settle();

    TEST_ASSERT_TRUE(logged("a network job is already running"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nreq, "a stop went out over the running job");
    TEST_ASSERT_EQUAL_UINT32(before + 1U, mqtt_answers(&err));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KDRV_MQTT_ERR_BUSY, err,
        "the refusal was answered as the state of a client that is fine");

    fake_nvs_reset();
    net_done();
}

/* The same for `mqtt_up`: asked while another network job holds the worker, it
 * is refused, and the console closes it on the answer. Answered with the
 * client's state it read as a client that started. */
TEST_CASE("mqtt_up while a network job runs is refused as busy, not answered with a state",
          "[net_ao][answers]")
{
    uint8_t       err = 0U;
    uint8_t const off = 0U;

    fake_nvs_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_CHAN, off));
    chan_fresh();
    go_online();

    mqtt_up_request("mqtt://broker.example:1883", "", "");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);                  /* the start is in flight */
    uint32_t const before = mqtt_answers(NULL);
    fake_klog_reset();

    TEST_ASSERT_TRUE(mqtt_up_request("mqtt://other.example:1883", "", ""));
    (void)net_settle();

    TEST_ASSERT_TRUE(logged("a network job is already running"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nreq, "a second start went out over the running job");
    TEST_ASSERT_EQUAL_UINT32(before + 1U, mqtt_answers(&err));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KDRV_MQTT_ERR_BUSY, err,
        "the refusal was answered as the state of a client that started");

    fake_nvs_reset();
    net_done();
}

/* The CONNACK's session_present is what tells a Kode that its commands were
 * kept: channel_info says it, and one NOTICE per connect writes it down. */
TEST_CASE("a connect the broker resumed says session resumed", "[net_ao]")
{
    static char answer[1000];

    fake_nvs_reset();
    chan_fresh();
    drive_to_connect();
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_SESSION_NONE, chan_now().session);
    mqtt_link_present(KDRV_NET_MQTT_CONNECTED, true);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_SESSION_RESUMED, chan_now().session);
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("channel: session resumed"));
    TEST_ASSERT_FALSE(logged("channel: new session"));

    fake_nvs_reset();
    net_done();
}

TEST_CASE("a connect with nothing kept says new session, and the next one is judged on its own",
          "[net_ao]")
{
    fake_nvs_reset();
    chan_fresh();
    drive_to_up();   /* the CONNACK said session_present = 0 */

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_SESSION_NEW, chan_now().session);
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("channel: new session"));
    TEST_ASSERT_FALSE(logged("channel: session resumed"));

    /* A second CONNECTED of the live client is not a new connect. */
    mqtt_link_present(KDRV_NET_MQTT_CONNECTED, true);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_SESSION_NEW, chan_now().session);
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("channel: new session"));

    /* The channel goes down: no connect to speak of. */
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_SESSION_NONE, chan_now().session);

    fake_nvs_reset();
    net_done();
}

/* The six words and the seconds each stands for. */
TEST_CASE("chan_keep: default, every word, the seconds sent, a bad value refused",
          "[net_ao]")
{
    static const char * const words[] = { "1h", "6h", "1d", "3d", "7d", "never" };
    static const uint32_t     secs_of[] = { 3600U, 21600U, 86400U, 259200U,
                                            604800U, 0xFFFFFFFFU };
    const KodeConfigAnsEvt   *a;

    fake_nvs_reset();
    chan_fresh();

    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("1d", a->value);
    TEST_ASSERT_FALSE(a->stored);

    for ( uint8_t i = 0U; 6U > i; i++ )
    {
        uint32_t got = 0U;
        size_t   len = sizeof(got);

        a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP,
                                                .set = true, .e = i });
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
        TEST_ASSERT_EQUAL_STRING(words[i], a->value);
        TEST_ASSERT_TRUE(a->stored);
        TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_blob(KSET_NAMESPACE,
                                                         KSET_KEY_CHAN_KEEP, &got, &len));
        TEST_ASSERT_EQUAL_UINT(sizeof(got), len);
        TEST_ASSERT_EQUAL_UINT32(secs_of[i], got);
    }

    /* A value that is not one of the six: the owner refuses, which the console
     * turns into `>err code=5 msg=value`, and the stored one stands. */
    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP,
                                            .set = true, .e = 6U });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->status);
    TEST_ASSERT_EQUAL_STRING("never", a->value);

    fake_nvs_reset();
    net_done();
}

TEST_CASE("chan_keep: a write the store refuses is answered stored=no, not the old value's yes",
          "[net_ao]")
{
    const KodeConfigAnsEvt *a;
    uint32_t                got = 0U;
    size_t                  len = sizeof(got);

    fake_nvs_reset();
    chan_fresh();

    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP, .set = true, .e = 3U });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_TRUE(a->stored);

    fake_nvs_fail_next_set();
    fake_klog_reset();
    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP, .set = true, .e = 0U });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->status);
    TEST_ASSERT_EQUAL_STRING("1h", a->value);       /* used by the next connect */
    TEST_ASSERT_FALSE_MESSAGE(a->stored, "the answer vouched for an old value");
    TEST_ASSERT_TRUE(logged("not stored"));
    TEST_ASSERT_FALSE(logged("1h (stored)"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, khal_nvs_get_blob(KSET_NAMESPACE, KSET_KEY_CHAN_KEEP,
                                                     &got, &len));
    TEST_ASSERT_EQUAL_UINT32(259200U, got);         /* the store still holds 3d */

    fake_nvs_reset();
    net_done();
}

TEST_CASE("chan_keep: a stored value is read at boot, a foreign one is the default",
          "[net_ao]")
{
    const uint32_t three_days = 259200U;
    const uint32_t odd = 1234U;
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_CHAN_KEEP, &three_days, sizeof(three_days));
    chan_fresh();
    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("3d", a->value);
    TEST_ASSERT_TRUE(a->stored);
    drive_to_up();
    TEST_ASSERT_EQUAL_UINT32(259200U, s_sess.session_expiry_s);
    net_done();

    fake_nvs_reset();
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_CHAN_KEEP, &odd, sizeof(odd));
    chan_fresh();
    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("1d", a->value);
    TEST_ASSERT_FALSE(a->stored);

    fake_nvs_reset();
    net_done();
}

/* A change while the channel is up is stored at once and touches nothing else;
 * the next connect - here a renewal's - asks the broker for it. */
TEST_CASE("chan_keep changed while up: stored now, used at the next connect, no reconnect",
          "[net_ao]")
{
    const KodeConfigAnsEvt *a;
    IdSignReqEvt            sq;
    static char             answer[1000];

    fake_nvs_reset();
    chan_fresh();
    drive_to_up();
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    TEST_ASSERT_EQUAL_UINT32(86400U, s_sess.session_expiry_s);

    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN_KEEP,
                                            .set = true, .e = 0U });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("1h", a->value);
    TEST_ASSERT_TRUE(a->stored);
    pull_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_nreq, "the change made the channel reconnect");
    TEST_ASSERT_EQUAL_UINT(1U, s_nsess);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(86400U, chan_now().keep_s,
                                     "the live session reported an age it was not asked for");

    /* The renewal: the same sequence as the case above. */
    secs(121U);
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    s_drv_state = KDRV_MQTT_UP;
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_nsess);
    TEST_ASSERT_EQUAL_UINT32(3600U, s_sess.session_expiry_s);
    TEST_ASSERT_TRUE(s_sess.mqtt5);
    TEST_ASSERT_TRUE(s_sess.keep_session);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    /* Started, not connected: the old session's age is still the one shown. */
    TEST_ASSERT_EQUAL_UINT32(86400U, chan_now().keep_s);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    TEST_ASSERT_EQUAL_UINT32(3600U, chan_now().keep_s);

    fake_nvs_reset();
    net_done();
}

/* ======================================================================== *
 * The worker
 *
 * AO_Net's blocking calls - the six kinds kdrv_net has on one KqpWorker, and
 * the download's open, reads and close on another - are jobs. The cases below
 * play the workers from here (see pull_jobs()) and keep each job "inside" its
 * worker until the case says how it came out.
 * ======================================================================== */

/** @brief Calls of the stream's three driver functions, since the last fixture. */
static unsigned s_n_open;
static unsigned s_n_read;
static unsigned s_n_close;

/** @brief What the next stream open reports as Content-Length, and the next read as bytes. */
static uint32_t s_open_total;
static size_t   s_read_got;

/** @brief The URL the last open was asked for. */
static char     s_open_url[KODE_FLASH_SRC_MAX];

static kode_status_t stub_stream_open(const char *url, uint32_t *total,
                                      uint16_t *status, int n)
{
    (void)n;
    s_n_open++;
    (void)snprintf(s_open_url, sizeof(s_open_url), "%s", url);
    *total  = s_open_total;
    *status = 200U;
    return KODE_OK;
}

static kode_status_t stub_stream_read(void *buf, size_t max, size_t *got, int n)
{
    (void)n;
    s_n_read++;
    TEST_ASSERT_EQUAL_UINT32(32U * 1024U, (uint32_t)max);
    (void)memset(buf, 0xA5, s_read_got);
    *got = s_read_got;
    return KODE_OK;
}

static void stub_stream_close(int n)
{
    (void)n;
    s_n_close++;
}

static kode_status_t stub_info(kdrv_net_info_t *out, int n)
{
    (void)n;
    (void)memset(out, 0, sizeof(*out));
    out->ip4 = 0x0200A8C0U;
    out->gw4 = 0x0100A8C0U;
    return KODE_OK;
}

static kdrv_net_ping_reply_t s_echo[3];
static uint8_t               s_echo_n;

static uint8_t stub_ping_count(int n)
{
    (void)n;
    return s_echo_n;
}

static bool stub_ping_get(uint8_t idx, kdrv_net_ping_reply_t *out, int n)
{
    (void)n;
    if ( idx >= s_echo_n )
    {
        return false;
    }
    *out = s_echo[idx];
    return true;
}

static void stub_ping_stats(kdrv_net_ping_stats_t *out, int n)
{
    (void)n;
    (void)memset(out, 0, sizeof(*out));
    out->ip4 = 0x0100A8C0U;
    out->tx  = s_echo_n;
    out->rx  = s_echo_n;
}

/** @brief A fresh AO_Net, online, with the worker played from here and every call recorded. */
static void worker_fresh(void)
{
    net_fresh();
    s_auto = false;

    s_n_open = 0U; s_n_read = 0U; s_n_close = 0U;
    s_open_total = 4242U;
    s_read_got   = 100U;
    s_nreq       = 0U;
    s_echo_n     = 0U;

    kdrv_net_run_StopIgnore();
    kdrv_net_run_Stub(&stub_run);
    kdrv_net_stream_open_StopIgnore();
    kdrv_net_stream_open_Stub(&stub_stream_open);
    kdrv_net_stream_read_Stub(&stub_stream_read);
    kdrv_net_stream_close_StopIgnore();
    kdrv_net_stream_close_Stub(&stub_stream_close);
    kdrv_net_ping_count_Stub(&stub_ping_count);
    kdrv_net_ping_get_Stub(&stub_ping_get);
    kdrv_net_ping_stats_Stub(&stub_ping_stats);
    kdrv_net_http_forget_Ignore();
    kdrv_net_mqtt_state_IgnoreAndReturn(KDRV_MQTT_DOWN);
    s_drv_uri = "";
    kdrv_net_mqtt_uri_copy_Stub(&stub_mqtt_uri_copy);

    ao_spy_subscribe(NET_PING_ENTRY_SIG);
    ao_spy_subscribe(NET_PING_END_SIG);
    ao_spy_subscribe(TEST_RESULT_SIG);
    /* The channel joins by itself whenever there is an address, and what it
     * says and asks for over a minute of simulated time is not what these
     * cases are about. It is switched off, which is a setting the AO owns. */
    fake_nvs_reset();
    (void)config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true,
                                              .b = false });
    go_online();
    radio_drain();
    ao_spy_drain();
    fake_klog_reset();
}

/** @brief Leave AO_Net as net_done() does, and the settings store as it was found. */
static void worker_done(void)
{
    net_done();
    fake_nvs_reset();
}

/** @brief Ask for a read, from the reader. */
static void ask_stream_read(void)
{
    static const QEvt read_evt = QEVT_INITIALIZER(NET_STREAM_READ_REQ_SIG);

    net_dispatch(&read_evt);
}

/** @brief Hand buffer @p idx back, which is the request for the next chunk. */
static void give_back(uint8_t idx)
{
    StorageBufEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(NET_STREAM_NEXT_SIG);
    e.idx   = idx;
    net_dispatch(&e.super);
}

/** @brief Ask for a ping. */
static void ask_ping(const char *host, uint8_t count)
{
    NetPingReqEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(NET_PING_REQ_SIG);
    (void)snprintf(e.host, sizeof(e.host), "%s", host);
    e.count = count;
    net_dispatch(&e.super);
}

/** @brief The next event the spy holds, or NULL. */
static const QEvt *spy_next(void)
{
    return ao_spy_take();
}

/** @brief A URL past kdrv_net's 80 characters and within a request's 256. */
#define LONG_URL  "http://example.invalid/a/long/path/that/is/over/eighty/" \
                  "characters/because/a/request/carries/up/to/256/app.bin"

_Static_assert((sizeof(LONG_URL) > KDRV_NET_URL_MAX) && (sizeof(LONG_URL) <= KODE_FLASH_SRC_MAX),
               "the long URL must be longer than a driver URL and still fit a request");

/** @brief Simulated steps (10 ms each) that outlast a guard of @p ms. */
#define GUARD_STEPS(ms_)  ((((ms_) / 10U)) + 5U)


/** @brief Ask for the transfer to be closed, from whoever has it. */
static void ask_close(void)
{
    static const QEvt close_evt = QEVT_INITIALIZER(NET_STREAM_CLOSE_REQ_SIG);

    net_dispatch(&close_evt);
}

/** @brief Open a transfer all the way, with the spy as its reader. */
static void open_transfer(void)
{
    ask_stream_open("http://example.invalid/app.bin");
    deliver_stream(KODE_OK);
    ao_spy_drain();
}

TEST_CASE("the network worker and the download's have their own tasks and "
          "their stacks in PSRAM", "[net_ao]")
{
    worker_fresh();

    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_last_psram_for(WORKER_NAME),
        "a job that touches no flash keeps its stack in PSRAM");
    TEST_ASSERT_TRUE_MESSAGE(fake_khal_worker_last_psram_for(STREAM_WORKER_NAME),
        "the download's too");
    worker_done();
}

TEST_CASE("the stream's open runs as a job, and the opener is told when it ends",
          "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();

    ask_stream_open(LONG_URL);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_open,
        "the open is a job: the worker ran it");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the answer comes with the job's end, not before");
    TEST_ASSERT_NOT_NULL(s_held_s);
    TEST_ASSERT_NULL_MESSAGE(s_held, "the first worker has nothing to do with it");
    TEST_ASSERT_EQUAL_STRING_MESSAGE(LONG_URL, s_open_url,
        "a URL longer than a driver URL still reaches the driver whole");

    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_INT(NET_STREAM_OPEN_RSP_SIG, (int)rsp->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp->status);
    TEST_ASSERT_EQUAL_UINT32(4242U, rsp->size);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KDRV_MEDIUM_COUNT, rsp->medium);

    worker_done();
}

TEST_CASE("a failed open is reported as it failed, and the transfer is not held",
          "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();

    ask_stream_open("http://example.invalid/missing.bin");
    deliver_stream(KODE_ERROR);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_ERROR, rsp->status);
    TEST_ASSERT_EQUAL_UINT32(0U, rsp->size);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_n_close,
        "the driver says it opened nothing: there is nothing to close");

    /* The next transfer is not refused as "already open". */
    ao_spy_drain();
    ask_stream_open("http://example.invalid/again.bin");
    TEST_ASSERT_EQUAL_UINT(2U, s_n_open);
    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp->status);

    worker_done();
}

TEST_CASE("a second read asked for while the first is inside the worker waits "
          "for it, then runs", "[net_ao]")
{
    const StorageDataEvt *d;

    worker_fresh();
    open_transfer();

    ask_stream_read();                 /* the app manager primes both buffers */
    ask_stream_read();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_read,
        "one read at a time with the download's worker");

    deliver_stream(KODE_OK);           /* the first ends; the second starts */
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_INT(NET_STREAM_DATA_SIG, (int)d->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, d->status);
    TEST_ASSERT_EQUAL_UINT16(100U, d->len);
    TEST_ASSERT_EQUAL_UINT8(0U, d->idx);
    TEST_ASSERT_FALSE(d->eof);
    TEST_ASSERT_EQUAL_HEX8(0xA5U, net_stream_buf(0U)[99]);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_n_read,
        "the kept read ran as soon as the first was done");

    deliver_stream(KODE_OK);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(1U, d->idx, "buffer 0 is out on loan");

    worker_done();
}

TEST_CASE("the stream's chunks come in turn: two buffers out, then one back, "
          "then the end of the body", "[net_ao]")
{
    const StorageDataEvt *d;

    worker_fresh();
    open_transfer();

    ask_stream_read();
    deliver_stream(KODE_OK);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(0U, d->idx);

    ask_stream_read();
    deliver_stream(KODE_OK);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(1U, d->idx);
    TEST_ASSERT_EQUAL_UINT(2U, s_n_read);

    /* Both out on loan: a read is not started. */
    ask_stream_read();
    TEST_ASSERT_EQUAL_UINT(2U, s_n_read);
    TEST_ASSERT_NULL(s_held_s);

    /* Giving one back IS the request for the next chunk. */
    give_back(0U);
    TEST_ASSERT_EQUAL_UINT(3U, s_n_read);
    deliver_stream(KODE_OK);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0U, d->idx,
        "buffer 0 was handed back, so it is the free one");

    /* The end of the body is a read of nothing that went well. */
    s_read_got = 0U;
    give_back(1U);
    deliver_stream(KODE_OK);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT16(0U, d->len);
    TEST_ASSERT_TRUE(d->eof);

    worker_done();
}

TEST_CASE("a buffer given back while a read is inside the worker is free at "
          "once and its read is not lost", "[net_ao]")
{
    const StorageDataEvt *d;

    worker_fresh();
    open_transfer();

    ask_stream_read();                 /* buffer 0 */
    deliver_stream(KODE_OK);
    ao_spy_drain();
    ask_stream_read();                 /* buffer 1, inside the worker */
    TEST_ASSERT_EQUAL_UINT(2U, s_n_read);

    give_back(0U);                     /* the reader is done with chunk 1 */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_n_read,
        "the worker is inside the second read; the third waits");

    deliver_stream(KODE_OK);           /* the second ends and the third starts */
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(1U, d->idx);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_n_read,
        "the give-back asked for a chunk, and it was not dropped");
    deliver_stream(KODE_OK);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8(0U, d->idx);

    worker_done();
}

TEST_CASE("the stream's close is a job, and the next open waits behind it and "
          "then runs by itself", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();
    open_transfer();

    ask_close();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_close, "the close ran as a job");
    ask_stream_open("http://example.invalid/second.bin");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_open,
        "the second open is not handed to the driver before the first is closed");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    deliver_stream(KODE_OK);           /* the close is done: the open follows */
    TEST_ASSERT_EQUAL_UINT(2U, s_n_open);
    TEST_ASSERT_EQUAL_STRING("http://example.invalid/second.bin", s_open_url);
    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp->status);

    worker_done();
}

TEST_CASE("a close that lands while the open is inside the worker wins: the "
          "late success is closed, and the next open succeeds", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();

    ask_stream_open("http://example.invalid/app.bin");
    ask_close();                       /* the opener gave up */
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the opener was left waiting");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp->status);
    TEST_ASSERT_EQUAL_UINT(0U, s_n_close);

    deliver_stream(KODE_OK);           /* the driver opened it after all */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the late success answers nobody twice");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_close,
        "a transfer nobody has is closed, not adopted");
    deliver_stream(KODE_OK);

    ask_stream_open("http://example.invalid/next.bin");
    TEST_ASSERT_EQUAL_UINT(2U, s_n_open);
    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_OK, rsp->status,
        "the abandoned open wedged the next one as 'already open'");

    worker_done();
}

TEST_CASE("an open that waited for a close, and whose asker closed before it "
          "ran, never runs", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();
    open_transfer();

    ask_close();                       /* the close job is inside the worker */
    ask_stream_open("http://example.invalid/waiting.bin");
    TEST_ASSERT_EQUAL_UINT(1U, s_n_open);
    ao_spy_drain();

    ask_close();                       /* its asker gave up before its turn */
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the waiting open was left waiting");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp->status);

    deliver_stream(KODE_OK);           /* the first close is done */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_open,
        "an open nobody waits for reached the driver");
    TEST_ASSERT_NULL(s_held_s);

    ask_stream_open("http://example.invalid/next.bin");
    TEST_ASSERT_EQUAL_UINT(2U, s_n_open);
    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp->status);

    worker_done();
}

TEST_CASE("an open asked for while one is being opened, or one is open, is "
          "refused with its answer", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();

    ask_stream_open("http://example.invalid/a.bin");
    ask_stream_open("http://example.invalid/b.bin");
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp->status);
    TEST_ASSERT_EQUAL_UINT(1U, s_n_open);

    deliver_stream(KODE_OK);
    ao_spy_drain();
    ask_stream_open("http://example.invalid/c.bin");
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp->status);

    worker_done();
}

TEST_CASE("an open that outlasts its guard reports a timeout, and a client "
          "that opens late is closed", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;
    unsigned const guard_steps = GUARD_STEPS(KDRV_NET_STREAM_OPEN_MAX_MS
                                             + KODE_NET_STREAM_SLACK_MS);

    worker_fresh();

    ask_stream_open("http://example.invalid/app.bin");
    TEST_ASSERT_EQUAL_UINT(1U, s_n_open);
    (void)net_tick(guard_steps - 20U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the cap in the driver answers first; the guard has not fired yet");
    (void)net_tick(30U);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the opener was left waiting");
    TEST_ASSERT_EQUAL_INT(NET_STREAM_OPEN_RSP_SIG, (int)rsp->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_TIMEOUT, rsp->status);

    /* The worker is still inside the open, so the next open waits for it. */
    ask_stream_open("http://example.invalid/other.bin");
    TEST_ASSERT_EQUAL_UINT(1U, s_n_open);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    /* The late answer: the driver did open it. Nobody is waiting for that
     * transfer, so it is closed rather than adopted, and the open that waited
     * runs after the close. */
    deliver_stream(KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the late answer answers nobody twice");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_close,
        "a transfer opened for nobody is closed");
    deliver_stream(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(2U, s_n_open);
    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp->status);

    worker_done();
}

TEST_CASE("a read that outlasts its guard tells the reader it timed out",
          "[net_ao]")
{
    const StorageDataEvt *d;
    unsigned const guard_steps = GUARD_STEPS(KDRV_NET_STREAM_READ_MAX_MS
                                             + KODE_NET_STREAM_SLACK_MS);

    worker_fresh();
    open_transfer();
    ask_stream_read();
    TEST_ASSERT_EQUAL_UINT(1U, s_n_read);

    (void)net_tick(guard_steps + 10U);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(d, "the reader was left waiting");
    TEST_ASSERT_EQUAL_INT(NET_STREAM_DATA_SIG, (int)d->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_TIMEOUT, d->status);
    TEST_ASSERT_EQUAL_UINT16(0U, d->len);
    TEST_ASSERT_FALSE(d->eof);

    deliver_stream(KODE_TIMEOUT);      /* the late end of the read */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the late end of the read answers nobody twice");

    worker_done();
}

TEST_CASE("a read that ends after the transfer was closed posts nothing to "
          "the old reader", "[net_ao]")
{
    worker_fresh();
    open_transfer();
    ask_stream_read();
    TEST_ASSERT_EQUAL_UINT(1U, s_n_read);

    ask_close();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_n_close,
        "the driver is mid-read: the close cannot be handed to the worker yet");
    deliver_stream(KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "a chunk for a reader that closed was posted to it");
    TEST_ASSERT_EQUAL_UINT(1U, s_n_close);
    deliver_stream(KODE_OK);

    worker_done();
}

TEST_CASE("losing the network with a read inside the worker closes the "
          "transfer when the read is done", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();
    open_transfer();
    ask_stream_read();
    TEST_ASSERT_EQUAL_UINT(1U, s_n_read);

    go_offline();                      /* an exit cannot wait for the read */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_n_close,
        "the driver is mid-read: the close cannot be handed to the worker yet");

    deliver_stream(KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the reader of a forgotten transfer was sent its chunk");
    TEST_ASSERT_EQUAL_UINT(1U, s_n_close);
    deliver_stream(KODE_OK);

    /* Back online, a transfer is not refused as "already open". */
    go_online();
    radio_drain();
    ao_spy_drain();
    ask_stream_open("http://example.invalid/other.bin");
    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp->status);

    worker_done();
}

TEST_CASE("losing the network with the open inside the worker refuses the "
          "opener at once and closes what the driver opens", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();

    ask_stream_open("http://example.invalid/app.bin");
    go_offline();
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the opener was left waiting");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp->status);

    deliver_stream(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_close,
        "the transfer the driver opened for nobody is closed");
    deliver_stream(KODE_OK);

    worker_done();
}

TEST_CASE("losing the network refuses an open that waits for a close", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    worker_fresh();
    open_transfer();
    ask_close();
    ask_stream_open("http://example.invalid/waiting.bin");
    ao_spy_drain();

    go_offline();
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(rsp, "the waiting open was left waiting");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_DENIED, rsp->status);
    deliver_stream(KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_open, "the open ran with no network");

    worker_done();
}

/* ---- The six kinds ---------------------------------------------------------- */

TEST_CASE("a ping runs as a job and is answered with one event per echo and "
          "the summary", "[net_ao]")
{
    const NetPingEntryEvt *a;
    const NetPingEntryEvt *b;
    const NetPingEndEvt   *end;

    worker_fresh();

    s_echo_n = 2U;
    s_echo[0] = (kdrv_net_ping_reply_t){ .seq = 1U, .time_ms = 12U, .ttl = 64U, .ok = true };
    s_echo[1] = (kdrv_net_ping_reply_t){ .seq = 2U, .time_ms = 0U,  .ttl = 0U,  .ok = false };

    ask_ping("192.168.0.1", 2U);
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_PING, (int)req_at(0)->type);
    TEST_ASSERT_EQUAL_STRING("192.168.0.1", req_at(0)->host);
    TEST_ASSERT_EQUAL_UINT8(2U, req_at(0)->count);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nothing is said before the job ends");

    job_done(KDRV_NET_JOB_PING, KODE_OK);
    a   = (const NetPingEntryEvt *)spy_next();
    b   = (const NetPingEntryEvt *)spy_next();
    end = (const NetPingEndEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_EQUAL_INT(NET_PING_ENTRY_SIG, (int)a->super.sig);
    TEST_ASSERT_EQUAL_UINT16(1U, a->seq);
    TEST_ASSERT_EQUAL_UINT16(12U, a->time_ms);
    TEST_ASSERT_TRUE(a->ok);
    TEST_ASSERT_FALSE(b->ok);
    TEST_ASSERT_EQUAL_INT(NET_PING_END_SIG, (int)end->super.sig);
    TEST_ASSERT_TRUE(end->ok);
    TEST_ASSERT_EQUAL_UINT16(2U, end->tx);

    worker_done();
}

TEST_CASE("a ping while one is running is refused with its own end marker",
          "[net_ao]")
{
    const NetPingEndEvt *end;

    worker_fresh();

    ask_ping("192.168.0.1", 1U);
    ask_ping("192.168.0.2", 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nreq, "the second one never reached the driver");
    end = (const NetPingEndEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "the refused ping was left waiting");
    TEST_ASSERT_EQUAL_INT(NET_PING_END_SIG, (int)end->super.sig);
    TEST_ASSERT_FALSE(end->ok);

    job_done(KDRV_NET_JOB_PING, KODE_ERROR);
    end = (const NetPingEndEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_FALSE_MESSAGE(end->ok, "a ping that failed ends without a session");

    worker_done();
}

TEST_CASE("a ping is not held up by a read inside the download's worker, nor "
          "a read by a ping", "[net_ao]")
{
    const NetPingEndEvt  *end;
    const StorageDataEvt *d;

    worker_fresh();
    open_transfer();
    ask_stream_read();
    TEST_ASSERT_EQUAL_UINT(1U, s_n_read);

    ask_ping("192.168.0.1", 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nreq,
        "the ping has a worker of its own and runs at once");
    TEST_ASSERT_NOT_NULL(s_held);
    TEST_ASSERT_NOT_NULL(s_held_s);

    s_echo_n = 0U;
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    end = (const NetPingEndEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(end);
    TEST_ASSERT_TRUE(end->ok);
    TEST_ASSERT_NOT_NULL_MESSAGE(s_held_s, "the read is still inside");

    deliver_stream(KODE_OK);
    d = (const StorageDataEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(d);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, d->status);

    worker_done();
}

TEST_CASE("a job's answer with a number that is not the job's is dropped",
          "[net_ao]")
{
    KqpWorkerDoneEvt stale;

    worker_fresh();

    ask_ping("192.168.0.1", 1U);
    TEST_ASSERT_NOT_NULL(s_held);

    (void)memset(&stale, 0, sizeof(stale));
    stale.super  = (QEvt)QEVT_INITIALIZER(NET_JOB_SIG);
    stale.seq    = 0xFFF0U;
    stale.status = (uint8_t)KODE_OK;
    net_dispatch(&stale.super);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "an answer to no job this AO waits for answered someone");

    ask_ping("192.168.0.2", 1U);       /* still busy: the stale answer freed nothing */
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    TEST_ASSERT_FALSE(((const NetPingEndEvt *)spy_next())->ok);

    job_done(KDRV_NET_JOB_PING, KODE_OK);
    worker_done();
}

TEST_CASE("a download answer with a number that is not the job's is dropped",
          "[net_ao]")
{
    KqpWorkerDoneEvt stale;

    worker_fresh();

    ask_stream_open("http://example.invalid/app.bin");
    TEST_ASSERT_NOT_NULL(s_held_s);

    (void)memset(&stale, 0, sizeof(stale));
    stale.super  = (QEvt)QEVT_INITIALIZER(NET_STREAM_JOB_SIG);
    stale.seq    = 0xFFF0U;
    stale.status = (uint8_t)KODE_OK;
    net_dispatch(&stale.super);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "an answer to no job this AO waits for adopted a transfer");

    deliver_stream(KODE_OK);
    TEST_ASSERT_NOT_NULL(spy_next());

    worker_done();
}

TEST_CASE("a ping that outlasts its guard ends without a session, once",
          "[net_ao]")
{
    const NetPingEndEvt *end;

    worker_fresh();

    ask_ping("192.168.0.1", 1U);
    (void)net_tick(GUARD_STEPS(KODE_NET_GUARD_MS) - 100U);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    (void)net_tick(120U);
    end = (const NetPingEndEvt *)spy_next();
    TEST_ASSERT_NOT_NULL_MESSAGE(end, "the console was left waiting");
    TEST_ASSERT_EQUAL_INT(NET_PING_END_SIG, (int)end->super.sig);
    TEST_ASSERT_FALSE(end->ok);

    ask_ping("192.168.0.2", 1U);       /* the worker is still inside the first */
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    TEST_ASSERT_FALSE(((const NetPingEndEvt *)spy_next())->ok);

    job_done(KDRV_NET_JOB_PING, KODE_OK);   /* the late end */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
        "the late end of the ping answers nobody twice");

    /* And the worker is free again. */
    ask_ping("192.168.0.3", 1U);
    TEST_ASSERT_EQUAL_UINT(2U, s_nreq);

    worker_done();
}

TEST_CASE("the self-test is a ping to the gateway, judged on the echo",
          "[net_ao]")
{
    static const QEvt test_evt = QEVT_INITIALIZER(NET_TEST_REQ_SIG);
    const TestResultEvt *r;

    worker_fresh();

    kdrv_net_info_StopIgnore();
    kdrv_net_info_Stub(&stub_info);

    s_echo_n = 1U;
    s_echo[0] = (kdrv_net_ping_reply_t){ .seq = 1U, .time_ms = 3U, .ttl = 64U, .ok = true };
    net_dispatch(&test_evt);
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    TEST_ASSERT_EQUAL_STRING("192.168.0.1", req_at(0)->host);
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    r = (const TestResultEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_INT(TEST_RESULT_SIG, (int)r->super.sig);
    TEST_ASSERT_TRUE(r->ok);

    worker_done();
}

TEST_CASE("the channel's handshake keeps its order on the worker", "[net_ao]")
{
    chan_fresh();

    drive_to_up();
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, (int)req_at(0)->type);
    TEST_ASSERT_EQUAL_STRING("https://api.kode.diy/kodes/challenge", req_at(0)->url);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, (int)req_at(1)->type);
    TEST_ASSERT_EQUAL_STRING("https://api.kode.diy/kodes/connect", req_at(1)->url);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_START, (int)req_at(2)->type);
    TEST_ASSERT_EQUAL_STRING("mqtts://kodes.kode.diy:8883", req_at(2)->uri);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, last_chan().state);

    net_done();
}

/* ---- Leaving for a jump ---------------------------------------------------- */

/**
 * @brief Empty AO_Boot's mailbox.
 *
 * AO_Boot is not dispatched here: its queue is the record of the answers
 * AO_Net owes a jump.
 *
 * @return How many BOOT_NET_LEFT_SIG it held.
 */
static unsigned boot_left_taken(void)
{
    const QEvt *e;
    unsigned    n = 0U;

    if ( NULL == AO_Boot->eQueue )
    {
        return 0U;
    }
    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)BOOT_NET_LEFT_SIG == e->sig )
        {
            n += 1U;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief AO_Boot asks AO_Net to leave, through the front door. */
static void leave_ask(void)
{
    net_leave_request();
    (void)net_settle();
}

TEST_CASE("a leave with the channel live says offline, stops the client and then "
          "answers", "[net_ao]")
{
    chan_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();

    drive_to_up();
    s_drv_state = KDRV_MQTT_UP;
    s_npub = 0U;
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);

    leave_ask();

    TEST_ASSERT_EQUAL_UINT(1U, s_npub);
    TEST_ASSERT_EQUAL_STRING("kode/KODE0001F00MR1C/status", s_pubs[0].topic);
    TEST_ASSERT_EQUAL_STRING(KCHAN_WILL, s_pubs[0].data);
    TEST_ASSERT_TRUE(s_pubs[0].retain);
    TEST_ASSERT_EQUAL_UINT(3U, s_pubs[0].jobs);   /* before the STOP */
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(3)->type);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, boot_left_taken(),
                                   "answered before the client was stopped");

    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_npub, "offline was said twice");

    net_done();
    (void)boot_left_taken();
}

TEST_CASE("a leave with no channel answers at once", "[net_ao]")
{
    net_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();

    leave_ask();
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());
    leave_ask();                                  /* one answer per request */
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());

    net_done();
    (void)boot_left_taken();
}

TEST_CASE("a leave with the channel switched off answers at once", "[net_ao]")
{
    const KodeConfigAnsEvt *a;

    fake_nvs_reset();
    chan_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();

    a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true });
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_STRING("off", a->value);
    go_online();
    TEST_ASSERT_EQUAL_UINT(0U, s_nreq);

    leave_ask();
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());
    TEST_ASSERT_EQUAL_UINT(0U, s_nreq);           /* nothing to stop */

    fake_nvs_reset();
    net_done();
    (void)boot_left_taken();
}

TEST_CASE("a leave while a job is in flight answers after the stop it owes",
          "[net_ao]")
{
    static char answer[1000];

    chan_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();

    drive_to_connect();
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);                   /* the client is starting */
    TEST_ASSERT_EQUAL_UINT(3U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_START, (int)req_at(2)->type);
    s_npub = 0U;

    leave_ask();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_nreq, "the worker is busy: no new job");
    TEST_ASSERT_EQUAL_UINT(0U, boot_left_taken());

    leave_ask();                                  /* a second ask while owed */
    TEST_ASSERT_EQUAL_UINT(0U, boot_left_taken());

    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(4U, s_nreq);           /* the stop it owes, now */
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(3)->type);
    TEST_ASSERT_EQUAL_UINT(0U, boot_left_taken());

    mqtt_link(KDRV_NET_MQTT_CONNECTED);           /* the broker was quicker */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nsub, "a left channel subscribed");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_npub, "a left channel said online");
    TEST_ASSERT_EQUAL_UINT(0U, boot_left_taken());

    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());

    net_done();
    (void)boot_left_taken();
}

TEST_CASE("a leave during a join's HTTP answers at once and the late reply "
          "starts nothing", "[net_ao]")
{
    IdSignReqEvt sq;

    chan_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();

    drive_to_challenge();                         /* the challenge is in the air */
    leave_ask();
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());

    http_reply(200U, "{\"nonce\":\"v1.connect.abc-123\"}");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&sq, sizeof(sq)),
                                  "the late challenge went on to be signed");
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);

    net_done();
    (void)boot_left_taken();
}

TEST_CASE("nothing rejoins after a leave", "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();

    drive_to_up();
    s_drv_state = KDRV_MQTT_UP;
    leave_ask();
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());
    radio_drain();
    s_nreq = 0U;

    /* The address flaps: it comes back, and the join does not start. */
    go_offline();
    radio_drain();
    go_online();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&q, sizeof(q)),
                                  "an address after a leave asked for the "
                                  "certificate");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nreq, "an address after a leave posted");
    secs(700U);
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT(0U, s_nreq);

    /* `chan=on` is no way back either. */
    {
        const KodeConfigAnsEvt *a;

        fake_nvs_reset();
        a = config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN,
                                                .set = true, .b = true });
        TEST_ASSERT_NOT_NULL(a);
        TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
        TEST_ASSERT_EQUAL_UINT(0U, s_nreq);
        fake_nvs_reset();
    }

    net_done();
    (void)boot_left_taken();
}

TEST_CASE("a retry waiting when the leave comes never fires", "[net_ao]")
{
    IdReqEvt q;

    chan_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();

    drive_to_up();
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);        /* a drop: the wait begins */
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, chan_now().state);
    radio_drain();

    leave_ask();
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());

    s_nreq = 0U;
    secs(700U);
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nreq, "the retry fired after the leave");

    net_done();
    (void)boot_left_taken();
}

/* ======================================================================== *
 * Screen uploads
 *
 * One capture: AO_Net asks AO_Display for the glass as a JPEG, hands it to
 * kdrv_net and PUTs it to the session's `shots` link with the upload token on
 * the end and the channel's JWT as the bearer. AO_Display's queue is a mailbox
 * here, the way AO_Radio's is: the case reads the request and plays the
 * answer.
 * ======================================================================== */

/** @brief An upload token of the length the api hands out today (66). */
#define SHOT_TOKEN "Ab3dEf_ghIjkl-MNopQRstuVWxyz0123456789_abcdefghijklmnopqrstuvwxYZ"

/** @brief Empty AO_Display's mailbox. */
static void display_drain(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Display);
    while ( xQueueReceive(AO_Display->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief Whatever AO_Net posted to AO_Display; the signal, or 0. */
static enum_t display_take(void *out, size_t size)
{
    const QEvt *e = NULL;
    enum_t      sig = 0;

    if ( xQueueReceive(AO_Display->eQueue, &e, 0U) == pdTRUE )
    {
        sig = e->sig;
        if ( NULL != out )
        {
            (void)memcpy(out, e, size);
        }
        QF_gc(e);
    }
    return sig;
}

/** @brief Whether the shot was given back: its release waits in the mailbox. */
static bool display_end_waiting(void)
{
    const QEvt *e = NULL;
    bool        end = false;

    if ( (NULL != AO_Display->eQueue)
        && (xQueuePeek(AO_Display->eQueue, &e, 0U) == pdTRUE) )
    {
        end = ((QSignal)DISPLAY_SHOT_END_SIG == e->sig);
    }
    return end;
}

/** @brief AO_Display answers the shot it was asked for. */
static void display_answer(kode_status_t st, uint32_t len)
{
    DisplayShotRspEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super  = (QEvt)QEVT_INITIALIZER(DISPLAY_SHOT_RSP_SIG);
    a.len    = len;
    a.status = (uint8_t)st;
    net_dispatch(&a.super);
}

/** @brief Ask for one capture, through the front door, answered to the spy. */
static void shot_ask(const char *token)
{
    net_shot_request(ao_spy(), token, 0U, 0U);
    (void)net_settle();
}

/**
 * @brief How many shot answers reached the spy since the last look.
 *
 * @param[out] out The last one.
 * @return The count; everything else waiting is dropped.
 */
static unsigned shot_answers(NetShotRspEvt *out)
{
    const QEvt *e;
    unsigned    n = 0U;

    (void)memset(out, 0, sizeof(*out));
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)NET_SHOT_RSP_SIG == e->sig )
        {
            *out = *(const NetShotRspEvt *)e;
            n++;
        }
    }
    ao_spy_drain();
    return n;
}

/** @brief Ask for a capture and have the display answer it with @p len bytes. */
static void shot_to_put(uint32_t len)
{
    DisplayShotReqEvt q;

    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    display_answer(KODE_OK, len);
    display_drain();   /* the release */
}

/** @brief A channel AO_Net joined, with uploads stubbed and the display a mailbox. */
static void shot_fresh(void)
{
    chan_fresh();
    (void)memset(&s_put, 0, sizeof(s_put));
    s_nput = 0U;
    s_put_rc = KODE_OK;
    s_put_released = false;
    kdrv_net_http_put_prepare_Stub(&stub_put_prepare);
    display_drain();
}

TEST_CASE("a capture asks the display, PUTs the JPEG to shots+token with the "
          "bearer, and answers 201", "[net_ao][shot]")
{
    DisplayShotReqEvt q;
    NetShotRspEvt     r;
    char              jwt[400];
    unsigned          jobs;

    shot_fresh();
    drive_to_up();   /* the MQTT start wipes the session's own copy of the token */
    ao_spy_drain();
    jobs = s_nreq;

    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    TEST_ASSERT_TRUE(q.who == AO_Net);
    TEST_ASSERT_EQUAL_UINT8(KSHOT_QUALITY, q.quality);
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);
    TEST_ASSERT_EQUAL_UINT(0U, s_nput);
    TEST_ASSERT_EQUAL_UINT(0U, shot_answers(&r));

    display_answer(KODE_OK, 24816U);
    TEST_ASSERT_EQUAL_UINT(1U, s_nput);
    TEST_ASSERT_EQUAL_STRING(SHOT_LINK SHOT_TOKEN, s_put.url);
    TEST_ASSERT_EQUAL_UINT(24816U, s_put.len);
    TEST_ASSERT_EQUAL_STRING("image/jpeg", s_put.type);
    /* The bare JWT: the driver writes "Bearer " in front of it. */
    (void)memset(jwt, 'x', 379U);
    jwt[379] = '\0';
    TEST_ASSERT_EQUAL_STRING(jwt, s_put.bearer);
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_PUT, (int)req_at(jobs)->type);
    TEST_ASSERT_FALSE(req_at(jobs)->keep);
    TEST_ASSERT_TRUE_MESSAGE(s_put_released,
                             "the job went out with the display's shot still lent");
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_END_SIG, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_INT(0, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, shot_answers(&r));

    s_nforget = 0U;
    http_reply(201U, "{\"ok\":true,\"id\":\"sh_7Kq\",\"seq\":3}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, r.result);
    TEST_ASSERT_EQUAL_UINT16(201U, r.http);
    TEST_ASSERT_EQUAL_UINT32(24816U, r.bytes);
    TEST_ASSERT_EQUAL_STRING("sh_7Kq", r.id);
    TEST_ASSERT_EQUAL_UINT32(3U, r.seq);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nforget, "the answer was left in the driver");
    TEST_ASSERT_TRUE(logged("shot: uploaded"));

    /* The worker is free again: a second capture goes the same way. */
    shot_to_put(100U);
    http_reply(201U, "{\"ok\":true,\"id\":\"sh_8\",\"seq\":4}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT32(4U, r.seq);

    net_done();
}

TEST_CASE("a capture with the channel down or without shots is refused and "
          "asks nothing", "[net_ao][shot]")
{
    NetShotRspEvt r;
    unsigned      jobs;

    shot_fresh();

    /* No address, so no channel. */
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_CHANNEL, r.result);
    TEST_ASSERT_EQUAL_UINT16(0U, r.http);
    TEST_ASSERT_EQUAL_INT(0, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, s_nreq);

    /* Joining: not up yet. */
    drive_to_challenge();
    ao_spy_drain();
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_CHANNEL, r.result);
    TEST_ASSERT_EQUAL_INT(0, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(1U, s_nreq);
    net_done();

    /* Up, but the api named no link: said once, and nothing is asked. */
    shot_fresh();
    s_no_shots = true;
    drive_to_up();
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("names no upload link"));
    ao_spy_drain();
    jobs = s_nreq;
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_CHANNEL, r.result);
    TEST_ASSERT_EQUAL_INT(0, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);
    TEST_ASSERT_EQUAL_UINT(0U, s_nput);

    net_done();
}

TEST_CASE("each refusal code comes back with its http status", "[net_ao][shot]")
{
    static const uint16_t codes[] = { 401U, 404U, 409U, 413U, 415U, 302U };
    NetShotRspEvt         r;

    shot_fresh();
    drive_to_up();
    ao_spy_drain();

    for ( unsigned i = 0U; (sizeof(codes) / sizeof(codes[0])) > i; i++ )
    {
        shot_to_put(1000U);
        http_reply(codes[i], "{\"error\":\"no\"}");
        TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
        TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_REFUSED, r.result);
        TEST_ASSERT_EQUAL_UINT16(codes[i], r.http);
        TEST_ASSERT_EQUAL_STRING("", r.id);
        TEST_ASSERT_EQUAL_UINT32(0U, r.seq);
    }

    /* No answer at all: the transport failed. */
    shot_to_put(1000U);
    job_done_http(KODE_TIMEOUT);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_NO_ANSWER, r.result);
    TEST_ASSERT_EQUAL_UINT16(0U, r.http);

    /* A job that says done with no status line is no answer either. */
    shot_to_put(1000U);
    http_reply(0U, "");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_NO_ANSWER, r.result);
    TEST_ASSERT_EQUAL_UINT16(0U, r.http);

    net_done();
}

TEST_CASE("a busy codec and a frame too big come back as such", "[net_ao][shot]")
{
    DisplayShotReqEvt q;
    NetShotRspEvt     r;
    unsigned          jobs;

    shot_fresh();
    drive_to_up();
    ao_spy_drain();
    jobs = s_nreq;

    /* The codec is busy, or a shot is lent elsewhere. Nothing was lent, so
     * nothing is given back. */
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    display_answer(KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_BUSY, r.result);
    TEST_ASSERT_EQUAL_INT(0, (int)display_take(NULL, 0U));

    /* No codec, no framebuffer or no PSRAM: busy too. */
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    display_answer(KODE_NO_RESOURCES, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_BUSY, r.result);

    /* Past KSHOT_JPEG_CAP. */
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    display_answer(KODE_ERROR, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_SIZE, r.result);
    TEST_ASSERT_EQUAL_UINT(0U, s_nput);
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);

    /* One capture at a time: a second while the first is at the display. */
    shot_ask(SHOT_TOKEN);
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_BUSY, r.result);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)display_take(NULL, 0U),
                                  "the second capture asked the display too");

    /* The driver refuses the upload: the shot is still given back. */
    s_put_rc = KODE_DENIED;
    display_answer(KODE_OK, 500U);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_BUSY, r.result);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_END_SIG, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);
    s_put_rc = KODE_OK;

    /* The worker holds somebody else's job when the shot lands. */
    net_http_get_request("http://example.invalid/");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    s_nput = 0U;
    display_answer(KODE_OK, 500U);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_BUSY, r.result);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_END_SIG, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, s_nput);
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    http_reply(200U, "fine");   /* the GET's own answer, published as ever */
    ao_spy_drain();

    net_done();
}

TEST_CASE("the channel leaving while the shot is at the display answers at once "
          "and releases the shot when it lands", "[net_ao][shot]")
{
    DisplayShotReqEvt q;
    NetShotRspEvt     r;
    unsigned          jobs;

    shot_fresh();
    drive_to_up();
    ao_spy_drain();

    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    go_offline();
    jobs = s_nreq;   /* the client's stop, if it went out */
    /* Answered at the leave: nobody waits on the display for it. */
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_CHANNEL, r.result);

    display_answer(KODE_OK, 500U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_answers(&r), "answered twice");
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISPLAY_SHOT_END_SIG, (int)display_take(NULL, 0U),
                                  "the shot was never given back");
    TEST_ASSERT_EQUAL_UINT(0U, s_nput);
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);

    net_done();
}

TEST_CASE("the channel leaving while the PUT is in flight drops its late answer",
          "[net_ao][shot]")
{
    NetShotRspEvt r;
    unsigned      jobs;

    shot_fresh();
    ao_spy_subscribe(NET_HTTP_DONE_SIG);
    ao_spy_subscribe(NET_HTTP_DATA_SIG);
    drive_to_up();
    ao_spy_drain();
    jobs = s_nreq;

    shot_to_put(500U);
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_PUT, (int)req_at(jobs)->type);
    go_offline();
    /* Answered at once: nobody waits on an upload the channel outlived. */
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_CHANNEL, r.result);
    jobs = s_nreq;   /* the client's stop waits: the worker is inside the PUT */

    s_nresult = 0U;
    s_nforget = 0U;
    fake_klog_reset();
    http_reply(201U, "{\"ok\":true,\"id\":\"late\",\"seq\":9}");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nresult, "the late answer was read");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nforget, "the late answer was left in the driver");
    {
        const QEvt *e;
        unsigned    shots = 0U;
        unsigned    fetches = 0U;

        while ( (e = ao_spy_take()) != NULL )
        {
            if ( (QSignal)NET_SHOT_RSP_SIG == e->sig )
            {
                shots++;
            }
            if ( ((QSignal)NET_HTTP_DONE_SIG == e->sig)
                || ((QSignal)NET_HTTP_DATA_SIG == e->sig) )
            {
                fetches++;
            }
        }
        ao_spy_drain();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shots, "answered twice");
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fetches, "published as somebody's fetch");
    }
    TEST_ASSERT_TRUE(logged("dropped"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs + 1U, s_nreq,
        "the late answer started something besides the stop it was holding");
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, (int)req_at(jobs)->type);

    net_done();
}

TEST_CASE("a renewal gives the next capture the new session's token",
          "[net_ao][shot]")
{
    static char  answer[1000];
    IdSignReqEvt sq;
    NetShotRspEvt r;
    char         jwt[400];

    shot_fresh();
    drive_to_up();
    secs(121U);   /* the renewal's challenge */
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    s_token_fill = 'y';
    s_drv_state = KDRV_MQTT_UP;
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    ao_spy_drain();

    shot_to_put(500U);
    (void)memset(jwt, 'y', 379U);
    jwt[379] = '\0';
    TEST_ASSERT_EQUAL_STRING(jwt, s_put.bearer);
    http_reply(201U, "{\"ok\":true,\"id\":\"a\",\"seq\":1}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, r.result);

    net_done();
}

TEST_CASE("no log line carries the token, the link or the bearer",
          "[net_ao][shot]")
{
    NetShotRspEvt r;

    shot_fresh();
    fake_klog_reset();
    drive_to_up();
    ao_spy_drain();

    shot_to_put(24816U);
    http_reply(201U, "{\"ok\":true,\"id\":\"sh_1\",\"seq\":1}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    shot_to_put(24816U);
    http_reply(409U, "{\"error\":\"used\"}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    shot_to_put(24816U);
    job_done_http(KODE_ERROR);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));

    TEST_ASSERT_TRUE(logged("shot:"));   /* something was said */
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( NULL != l )
        {
            TEST_ASSERT_NULL_MESSAGE(strstr(l, SHOT_TOKEN), l);
            TEST_ASSERT_NULL_MESSAGE(strstr(l, "shots/up"), l);
            TEST_ASSERT_NULL_MESSAGE(strstr(l, "xxxxxxxxxxxxxxxx"), l);
        }
    }

    net_done();
}

TEST_CASE("a display that never answers ends the upload as busy, and the next "
          "order goes through", "[net_ao][shot]")
{
    DisplayShotReqEvt q;
    NetShotRspEvt     r;
    unsigned          jobs;

    shot_fresh();
    drive_to_up();
    ao_spy_drain();
    jobs = s_nreq;

    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(&q, sizeof(q)));
    fake_klog_reset();
    secs(1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_answers(&r), "the deadline came early");
    secs(2U);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_BUSY, r.result);
    TEST_ASSERT_TRUE(logged("the display did not answer"));

    /* The shot lands late: given back, answered to nobody. */
    display_answer(KODE_OK, 500U);
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_END_SIG, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT(0U, s_nput);
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);

    /* The next order is not refused as busy. */
    shot_to_put(500U);
    TEST_ASSERT_EQUAL_STRING(SHOT_LINK SHOT_TOKEN, s_put.url);
    http_reply(201U, "{\"ok\":true,\"id\":\"b\",\"seq\":2}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, r.result);

    /* An answer in time disarms the deadline: nothing fires later. */
    secs(3U);
    TEST_ASSERT_EQUAL_UINT(0U, shot_answers(&r));

    net_done();
}

TEST_CASE("the longest link and the longest token go out whole", "[net_ao][shot]")
{
    char          link[KSHOT_PREFIX_MAX + 1U];
    char          token[KSHOT_TOKEN_MAX + 1U];
    char          want[KDRV_NET_PUT_URL_MAX];
    NetShotRspEvt r;
    size_t        n;

    n = (size_t)snprintf(link, sizeof(link), "%s", "https://api.kode.diy/");
    (void)memset(&link[n], 'p', KSHOT_PREFIX_MAX - n - 1U);
    link[KSHOT_PREFIX_MAX - 1U] = '/';
    link[KSHOT_PREFIX_MAX]      = '\0';
    (void)memset(token, 'T', KSHOT_TOKEN_MAX);
    token[KSHOT_TOKEN_MAX] = '\0';
    TEST_ASSERT_TRUE(kchan_shots_valid(link));
    TEST_ASSERT_TRUE(kshot_token_valid(token));

    shot_fresh();
    s_shots_link = link;
    drive_to_up();
    ao_spy_drain();

    net_shot_request(ao_spy(), token, 0U, 0U);
    (void)net_settle();
    TEST_ASSERT_EQUAL_INT(DISPLAY_SHOT_REQ_SIG, (int)display_take(NULL, 0U));
    display_answer(KODE_OK, 700U);
    display_drain();
    (void)snprintf(want, sizeof(want), "%s%s", link, token);
    TEST_ASSERT_EQUAL_UINT(KSHOT_PREFIX_MAX + KSHOT_TOKEN_MAX, strlen(want));
    TEST_ASSERT_EQUAL_UINT(1U, s_nput);
    TEST_ASSERT_EQUAL_STRING(want, s_put.url);
    http_reply(201U, "{\"ok\":true,\"id\":\"c\",\"seq\":1}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, r.result);

    net_done();
}

/* ---- Streams ---------------------------------------------------------------
 * The same order with a rate and a length: AO_Net answers at once, then
 * uploads one frame per tick of its own timer, one at a time and never
 * queued, until an end. Time moves here in steps of 10 ms, the clock and the
 * QP tick together, so a frame's tick can be pinned to the step.
 * ======================================================================== */

/** @brief Where the api says this unit's captures go after a renewal. */
#define SHOT_LINK2 "https://api.kode.diy/kodes/KODE0001F00MR1C/shots/v2/"

/** @brief Simulated milliseconds, in steps of 10: the clock and the QP tick. */
static void ms(unsigned n)
{
    for ( unsigned i = 0U; (n / 10U) > i; ++i )
    {
        fake_time_advance_ms(10U);
        (void)net_tick(1U);
    }
}

/** @brief How many shots AO_Net asked AO_Display for; the mailbox is emptied. */
static unsigned display_asks(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Display->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)DISPLAY_SHOT_REQ_SIG == e->sig )
        {
            n++;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief The kind of the last job submitted. */
static int last_job(void)
{
    TEST_ASSERT_TRUE(0U < s_nreq);
    TEST_ASSERT_TRUE(JOB_LOG_MAX >= s_nreq);
    return (int)s_job_type[s_nreq - 1U];
}

/** @brief Jobs of @p type submitted from index @p from on. */
static unsigned jobs_of(kdrv_net_job_t type, unsigned from)
{
    unsigned n = 0U;

    for ( unsigned i = from; (i < s_nreq) && (JOB_LOG_MAX > i); i++ )
    {
        if ( (uint8_t)type == s_job_type[i] )
        {
            n++;
        }
    }
    return n;
}

/** @brief The frame asked for lands with @p len bytes, and its PUT goes out. */
static void frame_lands(uint32_t len)
{
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, display_asks(), "no frame was asked for");
    display_answer(KODE_OK, len);
    display_drain();   /* the release */
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_PUT, last_job());
}

/** @brief The server takes the frame in flight. */
static void frame_taken(void)
{
    http_reply(201U, "{\"ok\":true,\"id\":\"f\",\"seq\":1}");
}

/** @brief Ask for a stream through the front door, answered to the spy. */
static void stream_ask(uint8_t fps, uint16_t secs)
{
    net_shot_request(ao_spy(), SHOT_TOKEN, fps, secs);
    (void)net_settle();
}

/** @brief A joined channel, nothing waiting in any mailbox, the log empty. */
static void stream_fresh(void)
{
    shot_fresh();
    drive_to_up();
    ao_spy_drain();
    display_drain();
    radio_drain();
    fake_klog_reset();
}

/** @brief What reached the spy since the last look. */
typedef struct {
    unsigned      answers;  /**< NET_SHOT_RSP_SIG events.        */
    NetShotRspEvt last;     /**< The last of them.               */
    unsigned      states;   /**< CHANNEL_STATE_SIG publications. */
} spied_t;

/** @brief Read and empty the spy's queue. */
static spied_t spied(void)
{
    spied_t     s;
    const QEvt *e;

    (void)memset(&s, 0, sizeof(s));
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)NET_SHOT_RSP_SIG == e->sig )
        {
            s.last = *(const NetShotRspEvt *)e;
            s.answers++;
        }
        else if ( (QSignal)CHANNEL_STATE_SIG == e->sig )
        {
            s.states++;
        }
        else
        {
            /* Not looked at here. */
        }
    }
    ao_spy_drain();
    return s;
}

/** @brief One NOTICE says the stream ended for @p why, and the close is the
 *         last job: nothing of the stream went out after it. */
static void assert_ended(const char *why, unsigned from)
{
    char want[48];

    (void)snprintf(want, sizeof(want), "shot: stream ended (%s)", why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged_count(want), why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged_count("stream ended"), why);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, jobs_of(KDRV_NET_JOB_HTTP_CLOSE, from), why);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_NET_JOB_HTTP_CLOSE, last_job(), why);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KSHOT_IDLE, chan_now().shot, why);
}

TEST_CASE("a stream answers at once and paces frames at 1000/fps ms",
          "[net_ao][shot]")
{
    spied_t  s;
    unsigned from;

    stream_fresh();
    from = s_nreq;
    stream_ask(4U, 60U);
    s = spied();
    TEST_ASSERT_EQUAL_UINT(1U, s.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_STREAM, s.last.result);
    TEST_ASSERT_EQUAL_UINT8(4U, s.last.fps);
    TEST_ASSERT_EQUAL_UINT16(60U, s.last.secs);
    TEST_ASSERT_TRUE(logged("shot: stream started"));

    /* The first frame at once, then one every 250 ms and never sooner. */
    for ( unsigned i = 0U; 8U > i; i++ )
    {
        frame_lands(20000U);
        TEST_ASSERT_TRUE_MESSAGE(s_job_keep[s_nreq - 1U],
                                 "a stream's PUT closes its connection");
        TEST_ASSERT_EQUAL_STRING(SHOT_LINK SHOT_TOKEN, s_put.url);
        frame_taken();
        ms(240U);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, display_asks(), "a frame came before its tick");
        ms(10U);
    }
    TEST_ASSERT_EQUAL_UINT(8U, jobs_of(KDRV_NET_JOB_HTTP_PUT, from));
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_CLOSE, from));
    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));
    s = spied();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.answers, "a stream is answered once");

    net_done();
}

TEST_CASE("a download is not held up by the channel's POST inside the other "
          "worker", "[net_ao]")
{
    const StorageOpenRspEvt *rsp;

    chan_fresh();
    kdrv_net_stream_open_StopIgnore();
    kdrv_net_stream_open_Stub(&stub_stream_open);
    kdrv_net_stream_read_Stub(&stub_stream_read);
    kdrv_net_stream_close_StopIgnore();
    kdrv_net_stream_close_Stub(&stub_stream_close);
    s_open_total = 7U;

    drive_to_challenge();              /* the challenge POST is inside the worker */
    TEST_ASSERT_NOT_NULL(s_held);

    ask_stream_open("http://example.invalid/app.bin");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_n_open,
        "the open waited for a POST that has a minute to live");
    deliver_stream(KODE_OK);
    rsp = (const StorageOpenRspEvt *)spy_next();
    TEST_ASSERT_NOT_NULL(rsp);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_OK, rsp->status);
    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "the POST is still inside its worker");

    net_done();
}

TEST_CASE("a stream order fills a missing rate or length with its default and "
          "cuts one past the most", "[net_ao][shot]")
{
    spied_t s;

    /* No channel: refused, nothing asked. */
    shot_fresh();
    stream_ask(2U, 60U);
    s = spied();
    TEST_ASSERT_EQUAL_UINT(1U, s.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_CHANNEL, s.last.result);
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    net_done();

    stream_fresh();
    stream_ask(0U, 30U);
    s = spied();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_STREAM, s.last.result);
    TEST_ASSERT_EQUAL_UINT8(KSHOT_FPS_DEFAULT, s.last.fps);
    TEST_ASSERT_EQUAL_UINT16(30U, s.last.secs);

    stream_ask((uint8_t)(KSHOT_FPS_MAX + 1U), 0U);
    s = spied();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_STREAM, s.last.result);
    TEST_ASSERT_EQUAL_UINT8(KSHOT_FPS_MAX, s.last.fps);
    TEST_ASSERT_EQUAL_UINT16(KSHOT_SECS_DEFAULT, s.last.secs);

    stream_ask(3U, (uint16_t)(KSHOT_SECS_MAX + 1U));
    s = spied();
    TEST_ASSERT_EQUAL_UINT8(3U, s.last.fps);
    TEST_ASSERT_EQUAL_UINT16(KSHOT_SECS_MAX, s.last.secs);

    net_done();
}

TEST_CASE("the channel's POST that outlasts its guard fails the step with a "
          "timeout and the late answer is dropped", "[net_ao]")
{
    ChannelStateEvt st;

    chan_fresh();

    drive_to_challenge();              /* the challenge is inside the worker */
    secs(GUARD_STEPS(KODE_NET_GUARD_MS) / 100U);
    st = last_chan();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
    TEST_ASSERT_EQUAL_STRING("timeout", st.why);

    job_done(KDRV_NET_JOB_HTTP_POST, KODE_OK);   /* the late answer */
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)CHAN_WAITING, chan_now().state,
        "an answer to a step that already failed moved the channel");

    net_done();
}
TEST_CASE("a frame starts only when the tick has fired and the last PUT is done",
          "[net_ao][shot]")
{
    ChannelStateEvt st;

    stream_fresh();
    stream_ask(4U, 60U);
    (void)spied();
    frame_lands(20000U);   /* t = 0; its PUT stays out */

    ms(750U);              /* three ticks go by */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, display_asks(),
                                   "a frame started over the PUT in flight");
    frame_taken();         /* t = 750: the ticks owe one frame, not three */
    frame_lands(20000U);
    frame_taken();
    ms(10U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, display_asks(), "the missed ticks queued frames");
    ms(240U);              /* t = 1000: the next tick */
    frame_lands(20000U);
    frame_taken();

    st = chan_now();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_STREAM, st.shot);
    TEST_ASSERT_EQUAL_UINT32(3U, st.shot_sent);
    TEST_ASSERT_EQUAL_UINT16_MESSAGE(30U, st.shot_fps_x10,
                                     "3 frames in 1 s is 3.0 fps, not the 4 asked");

    net_done();
}

TEST_CASE("a stream ends at secs, on 404, on 401, 413 or 415, on channel loss, "
          "and on three network errors in a row", "[net_ao][shot]")
{
    static const uint16_t refused[] = { 401U, 413U, 415U };
    unsigned              from;
    unsigned              stop;

    /* secs ran out. */
    stream_fresh();
    from = s_nreq;
    stream_ask(1U, 3U);
    for ( unsigned i = 0U; 3U > i; i++ )
    {
        frame_lands(1000U);
        frame_taken();
        ms(1000U);
    }
    assert_ended("time", from);
    TEST_ASSERT_TRUE(logged("shot: stream ended (time): 3 frames in 3 s, 1.0 fps"));
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    net_done();

    /* The link expired. */
    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    frame_lands(1000U);
    http_reply(404U, "{\"error\":\"gone\"}");
    assert_ended("expired", from);
    ms(1000U);
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    net_done();

    /* Refused: no use asking again. */
    for ( unsigned i = 0U; (sizeof(refused) / sizeof(refused[0])) > i; i++ )
    {
        stream_fresh();
        from = s_nreq;
        stream_ask(2U, 60U);
        frame_lands(1000U);
        http_reply(refused[i], "{\"error\":\"no\"}");
        assert_ended("refused", from);
        net_done();
    }

    /* The channel left between frames: its client's stop has the worker
     * first, the close follows it. */
    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    frame_lands(1000U);
    frame_taken();
    go_offline();
    stop = s_nreq;
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, last_job());
    ms(20U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, jobs_of(KDRV_NET_JOB_HTTP_CLOSE, from),
                                   "the close went out over the client's stop");
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(stop + 1U, s_nreq);
    assert_ended("channel", from);
    net_done();

    /* Three network errors in a row; an answer in between starts the count over. */
    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    frame_lands(1000U);
    job_done_http(KODE_ERROR);
    ms(500U);
    frame_lands(1000U);
    job_done_http(KODE_TIMEOUT);
    ms(500U);
    frame_lands(1000U);
    frame_taken();
    ms(500U);
    frame_lands(1000U);
    job_done_http(KODE_ERROR);
    ms(500U);
    frame_lands(1000U);
    http_reply(0U, "");   /* a status line never came */
    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));
    ms(500U);
    frame_lands(1000U);
    job_done_http(KODE_ERROR);
    assert_ended("network", from);
    net_done();
}

TEST_CASE("a 429 drops that frame and the stream goes on", "[net_ao][shot]")
{
    ChannelStateEvt st;

    stream_fresh();
    stream_ask(2U, 60U);
    (void)spied();
    frame_lands(1000U);
    http_reply(429U, "{\"error\":\"slow_down\"}");
    ms(500U);
    frame_lands(1000U);
    frame_taken();

    /* A 429 is an answer: two network errors after it are not three. */
    ms(500U);
    frame_lands(1000U);
    job_done_http(KODE_ERROR);
    ms(500U);
    frame_lands(1000U);
    http_reply(429U, "{\"error\":\"slow_down\"}");
    ms(500U);
    frame_lands(1000U);
    job_done_http(KODE_ERROR);
    ms(500U);
    frame_lands(1000U);
    job_done_http(KODE_ERROR);

    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));
    st = chan_now();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_STREAM, st.shot);
    TEST_ASSERT_EQUAL_UINT32(1U, st.shot_sent);
    ms(500U);
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());

    net_done();
}

TEST_CASE("a busy codec or a frame too big skips that frame and the stream goes on",
          "[net_ao][shot]")
{
    ChannelStateEvt st;
    unsigned        from;

    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    (void)spied();

    /* The mirror holds the codec: nothing lent, nothing sent. */
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    display_answer(KODE_DENIED, 0U);
    TEST_ASSERT_EQUAL_INT(0, (int)display_take(NULL, 0U));
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_PUT, from));

    /* A very busy screen, past KSHOT_JPEG_CAP. */
    ms(500U);
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    display_answer(KODE_ERROR, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_PUT, from));

    /* A display that does not answer at all: the frame is given up. */
    ms(500U);
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    ms(2000U);
    TEST_ASSERT_TRUE(logged("the display did not answer"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, display_asks(),
                                   "the next frame did not follow the lost one");
    display_answer(KODE_OK, 1000U);
    display_drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_PUT, last_job());
    frame_taken();

    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));
    st = chan_now();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_STREAM, st.shot);
    TEST_ASSERT_EQUAL_UINT32(1U, st.shot_sent);

    /* A second frame the display never answers: skipped, not said again. */
    ms(500U);
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    ms(2000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged_count("the display did not answer"),
                                   "a per-frame warning was said twice in a stream");
    (void)display_asks();

    /* The end says how many went nowhere: busy, too big, two lost. */
    go_offline();
    TEST_ASSERT_TRUE(logged("shot: stream ended (channel): 1 frames in "));
    TEST_ASSERT_TRUE(logged(" fps, 4 skipped"));

    net_done();
}

TEST_CASE("the channel renewing mid-stream gives the next frame the new token "
          "and shots", "[net_ao][shot]")
{
    static char  answer[1000];
    IdSignReqEvt sq;
    char         jwt[400];
    unsigned     mark;

    stream_fresh();
    ms(500U);              /* the frames fall half a second off the renewal */
    stream_ask(1U, 300U);
    (void)spied();
    radio_drain();         /* the fast mode's request */

    /* Frames at 0.5 s, 1.5 s ... 119.5 s; the last one's PUT stays out. */
    for ( unsigned i = 0U; 120U > i; i++ )
    {
        frame_lands(500U);
        if ( 119U > i )
        {
            frame_taken();
            ms(1000U);
        }
    }
    mark = s_nreq;

    /* 120 s: the renewal's challenge finds the worker taken, and polls. */
    ms(600U);
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_POST, mark));
    ms(500U);              /* a tick at 120.5 s: the frame is owed */
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    frame_taken();         /* the worker is free, but the channel waits for it */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, display_asks(),
                                   "a frame took the worker the renewal waits for");
    ms(250U);
    TEST_ASSERT_EQUAL_UINT(1U, jobs_of(KDRV_NET_JOB_HTTP_POST, mark));
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, last_job());
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());

    /* The challenge's answer: the C5 signs, and meanwhile the owed frame goes. */
    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    frame_lands(500U);
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);   /* the connect waits for that PUT */
    frame_taken();
    ms(250U);              /* 121.1 s: the connect's poll finds the worker free */
    TEST_ASSERT_EQUAL_UINT(2U, jobs_of(KDRV_NET_JOB_HTTP_POST, mark));
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, last_job());
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());

    /* A new session, with a new token and a new link. */
    s_token_fill = 'y';
    s_shots_link = SHOT_LINK2;
    s_drv_state  = KDRV_MQTT_UP;
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, last_job());
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_START, last_job());
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    ms(400U);              /* 121.5 s: the next tick */
    frame_lands(500U);

    TEST_ASSERT_EQUAL_STRING(SHOT_LINK2 SHOT_TOKEN, s_put.url);
    (void)memset(jwt, 'y', 379U);
    jwt[379] = '\0';
    TEST_ASSERT_EQUAL_STRING(jwt, s_put.bearer);
    frame_taken();
    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_STREAM, chan_now().shot);
    ms(1000U);
    frame_lands(500U);
    TEST_ASSERT_EQUAL_STRING(SHOT_LINK2 SHOT_TOKEN, s_put.url);

    net_done();
}

TEST_CASE("a new order replaces a running stream: close first, then the new "
          "first PUT", "[net_ao][shot]")
{
    spied_t       s;
    NetShotRspEvt r;
    unsigned      mark;

    stream_fresh();
    stream_ask(2U, 60U);
    (void)spied();
    frame_lands(800U);     /* the old stream's PUT is out */
    mark = s_nreq;

    /* Another stream, on another link. */
    net_shot_request(ao_spy(), "NEWtoken_2", 4U, 30U);
    (void)net_settle();
    s = spied();
    TEST_ASSERT_EQUAL_UINT(1U, s.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_STREAM, s.last.result);
    TEST_ASSERT_EQUAL_UINT8(4U, s.last.fps);
    TEST_ASSERT_EQUAL_UINT16(30U, s.last.secs);
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("shot: stream ended (replaced)"));
    TEST_ASSERT_EQUAL_UINT(mark, s_nreq);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, display_asks(), "a frame over the old PUT");

    frame_taken();         /* the old PUT's answer: nobody's */
    TEST_ASSERT_TRUE(logged("dropped"));
    TEST_ASSERT_EQUAL_UINT(mark + 1U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_CLOSE, last_job());
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    s = spied();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.answers, "the late answer was answered");

    job_done_http(KODE_OK);   /* the close is done */
    frame_lands(800U);
    TEST_ASSERT_EQUAL_UINT(mark + 2U, s_nreq);
    TEST_ASSERT_TRUE(s_job_keep[mark + 1U]);
    TEST_ASSERT_EQUAL_STRING(SHOT_LINK "NEWtoken_2", s_put.url);
    frame_taken();

    /* A capture replaces it too, and waits for the close the same way. */
    ms(250U);
    frame_lands(800U);
    mark = s_nreq;
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_UINT(2U, logged_count("shot: stream ended (replaced)"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_ONE, chan_now().shot);
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    frame_taken();
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_CLOSE, last_job());
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());
    job_done_http(KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    display_answer(KODE_OK, 900U);
    display_drain();
    TEST_ASSERT_EQUAL_UINT(mark + 2U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_PUT, last_job());
    TEST_ASSERT_FALSE(s_job_keep[mark + 1U]);
    TEST_ASSERT_EQUAL_STRING(SHOT_LINK SHOT_TOKEN, s_put.url);
    http_reply(201U, "{\"ok\":true,\"id\":\"one\",\"seq\":1}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, r.result);
    TEST_ASSERT_EQUAL_UINT32(900U, r.bytes);

    net_done();
}

TEST_CASE("the stream holds the fast mode and lets it go when it ends",
          "[net_ao][shot]")
{
    fast_posts_t f;

    stream_fresh();
    fast_idle();
    stream_ask(1U, 50U);
    (void)spied();
    f = radio_fast_posts();
    TEST_ASSERT_EQUAL_UINT(1U, f.on);
    TEST_ASSERT_EQUAL_UINT(0U, f.off);

    /* 50 frames, one a second: each holds the fast mode 60 s more. */
    for ( unsigned i = 0U; 50U > i; i++ )
    {
        frame_lands(1000U);
        frame_taken();
        ms(1000U);
    }
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("shot: stream ended (time)"));
    job_done_http(KODE_OK);   /* the stream's close */
    f = radio_fast_posts();
    TEST_ASSERT_EQUAL_UINT(0U, f.on);
    TEST_ASSERT_EQUAL_UINT(0U, f.off);
    TEST_ASSERT_TRUE(chan_now().fast);

    /* The last frame went at 49 s: held until 109 s, not 60 s. */
    ms(58000U);
    f = radio_fast_posts();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, f.off, "the hold was not moved by the frames");
    ms(2000U);
    f = radio_fast_posts();
    TEST_ASSERT_EQUAL_UINT(1U, f.off);
    TEST_ASSERT_FALSE(chan_now().fast);

    net_done();
}

TEST_CASE("channel_info's state carries shot, sent, fps and left",
          "[net_ao][shot]")
{
    ChannelStateEvt st;
    spied_t         s;
    NetShotRspEvt   r;

    stream_fresh();
    st = chan_now();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_IDLE, st.shot);
    TEST_ASSERT_EQUAL_UINT32(0U, st.shot_sent);
    TEST_ASSERT_EQUAL_UINT16(0U, st.shot_fps_x10);
    TEST_ASSERT_EQUAL_UINT32(0U, st.shot_left_s);

    stream_ask(2U, 10U);
    s = spied();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.states, "the start was not published");

    for ( unsigned i = 0U; 4U > i; i++ )
    {
        frame_lands(1000U);
        frame_taken();
        ms(500U);
    }
    frame_lands(1000U);
    frame_taken();         /* 5 frames by 2 s */
    s = spied();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.states, "a frame was published");
    st = chan_now();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_STREAM, st.shot);
    TEST_ASSERT_EQUAL_UINT32(5U, st.shot_sent);
    TEST_ASSERT_EQUAL_UINT16(25U, st.shot_fps_x10);
    TEST_ASSERT_EQUAL_UINT32(8U, st.shot_left_s);

    for ( unsigned i = 0U; 15U > i; i++ )
    {
        ms(500U);
        frame_lands(1000U);
        frame_taken();
    }
    ms(500U);              /* 10 s: the end */
    s = spied();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s.states, "the end was not published once");
    TEST_ASSERT_TRUE(logged("shot: stream ended (time): 20 frames in 10 s, 2.0 fps, "
                            "0 skipped"));

    /* After it, the last stream's numbers. */
    st = chan_now();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_IDLE, st.shot);
    TEST_ASSERT_EQUAL_UINT32(20U, st.shot_sent);
    TEST_ASSERT_EQUAL_UINT16(20U, st.shot_fps_x10);
    TEST_ASSERT_EQUAL_UINT32(0U, st.shot_left_s);

    /* A capture reads `one` while it runs. */
    job_done_http(KODE_OK);   /* the stream's close */
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_ONE, chan_now().shot);
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    display_answer(KODE_OK, 900U);
    display_drain();
    http_reply(201U, "{\"ok\":true,\"id\":\"one\",\"seq\":1}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_IDLE, chan_now().shot);

    net_done();
}

TEST_CASE("a display that cannot encode ends the stream with display, closes "
          "and lets the fast mode go", "[net_ao][shot]")
{
    fast_posts_t f;
    unsigned     from;

    stream_fresh();
    fast_idle();
    from = s_nreq;
    stream_ask(2U, 300U);
    (void)spied();
    TEST_ASSERT_EQUAL_UINT(1U, radio_fast_posts().on);

    /* No codec, no framebuffer or no PSRAM: it will not change this boot. */
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    display_answer(KODE_NO_RESOURCES, 0U);
    TEST_ASSERT_TRUE(logged("shot: stream ended (display): 0 frames in "));
    ms(10U);               /* the close, on the next tick */
    assert_ended("display", from);
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_PUT, from));
    ms(1000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, display_asks(), "the ended stream asked again");
    job_done_http(KODE_OK);   /* the stream's close */

    /* The fast mode is the hold's to end, 60 s after the frame that asked. */
    ms(57000U);
    f = radio_fast_posts();
    TEST_ASSERT_EQUAL_UINT(0U, f.off);
    ms(3000U);
    f = radio_fast_posts();
    TEST_ASSERT_EQUAL_UINT(1U, f.off);

    net_done();
}

TEST_CASE("any 2xx is a frame or a capture taken; a 5xx is a network error",
          "[net_ao][shot]")
{
    NetShotRspEvt r;
    unsigned      from;

    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    (void)spied();
    frame_lands(1000U);
    http_reply(200U, "{\"ok\":true}");
    ms(500U);
    frame_lands(1000U);
    http_reply(204U, "");
    TEST_ASSERT_EQUAL_UINT32(2U, chan_now().shot_sent);

    /* Three server errors in a row are three network errors. */
    for ( unsigned i = 0U; 3U > i; i++ )
    {
        ms(500U);
        frame_lands(1000U);
        http_reply(503U, "{\"error\":\"later\"}");
    }
    assert_ended("network", from);
    TEST_ASSERT_TRUE(logged("shot: stream ended (network): 2 frames in "));
    job_done_http(KODE_OK);   /* the close */

    /* A capture answered 200 is a capture taken. */
    shot_to_put(700U);
    http_reply(200U, "{\"ok\":true,\"id\":\"c2\",\"seq\":7}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, r.result);
    TEST_ASSERT_EQUAL_UINT16(200U, r.http);
    TEST_ASSERT_EQUAL_STRING("c2", r.id);
    TEST_ASSERT_EQUAL_UINT32(7U, r.seq);

    net_done();
}

TEST_CASE("a frame that lands while a channel step polls for the worker is not "
          "PUT, and the step gets the worker", "[net_ao][shot]")
{
    unsigned mark;

    stream_fresh();
    ms(500U);
    stream_ask(1U, 300U);
    (void)spied();
    radio_drain();

    /* Frames at 0.5 s ... 118.5 s; the one at 119.5 s stays at the display. */
    for ( unsigned i = 0U; 119U > i; i++ )
    {
        frame_lands(500U);
        frame_taken();
        ms(1000U);
    }
    ms(100U);
    net_http_get_request("http://example.invalid/");   /* somebody's job */
    (void)net_settle();
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_GET, last_job());
    mark = s_nreq;

    ms(500U);              /* 120.1 s: the renewal finds the worker taken */
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_POST, mark));
    http_reply(200U, "fine");   /* the GET is done; the challenge still polls */
    ao_spy_drain();

    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    display_answer(KODE_OK, 500U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISPLAY_SHOT_END_SIG, (int)display_take(NULL, 0U),
                                  "the frame was not given back");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, jobs_of(KDRV_NET_JOB_HTTP_PUT, mark),
                                   "a frame took the worker the renewal polls for");
    TEST_ASSERT_EQUAL_UINT(mark, s_nreq);

    ms(250U);              /* the poll: the challenge goes out */
    TEST_ASSERT_EQUAL_UINT(1U, jobs_of(KDRV_NET_JOB_HTTP_POST, mark));
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, last_job());
    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));

    net_done();
}

TEST_CASE("a leave mid-stream ends it with channel, and the client's stop goes "
          "before the close", "[net_ao][shot]")
{
    unsigned from;
    unsigned stop;

    stream_fresh();
    qf_support_give_queue(AO_Boot);
    (void)boot_left_taken();
    s_drv_state = KDRV_MQTT_UP;
    from = s_nreq;
    stream_ask(2U, 60U);
    (void)spied();
    frame_lands(1000U);    /* its PUT is out when the leave comes */

    leave_ask();
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("shot: stream ended (channel)"));
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)KDRV_NET_JOB_HTTP_PUT, last_job(),
                                  "something went over the PUT in flight");
    ms(20U);
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_CLOSE, from));

    frame_taken();         /* the stale PUT: the owed stop takes the worker */
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, last_job());
    stop = s_nreq;
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_CLOSE, from));
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    TEST_ASSERT_EQUAL_UINT(1U, boot_left_taken());
    TEST_ASSERT_EQUAL_UINT(stop + 1U, s_nreq);
    assert_ended("channel", from);

    net_done();
}

TEST_CASE("a stream that ends while its frame is at the display gives the late "
          "frame back and sends nothing", "[net_ao][shot]")
{
    unsigned from;

    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    (void)spied();
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());   /* at the display */

    go_offline();
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("shot: stream ended (channel)"));
    display_answer(KODE_OK, 1000U);
    TEST_ASSERT_EQUAL_INT_MESSAGE(DISPLAY_SHOT_END_SIG, (int)display_take(NULL, 0U),
                                  "the late frame was never given back");
    TEST_ASSERT_EQUAL_UINT(0U, jobs_of(KDRV_NET_JOB_HTTP_PUT, from));
    TEST_ASSERT_EQUAL_UINT(0U, s_nput);

    net_done();
}

/** @brief Ask AO_Net to stop the uploads, answered to the spy. */
static void stream_stop(void)
{
    net_shot_stop_request(ao_spy());
    (void)net_settle();
}

TEST_CASE("a stop ends a running stream with stopped and answers it; its PUT "
          "out is dropped and the close follows", "[net_ao][shot]")
{
    spied_t  s;
    unsigned from;
    unsigned mark;

    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    (void)spied();
    frame_lands(800U);     /* its PUT is out */
    mark = s_nreq;

    stream_stop();
    s = spied();
    TEST_ASSERT_EQUAL_UINT(1U, s.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_STOPPED, s.last.result);
    TEST_ASSERT_EQUAL_UINT(1U, logged_count("shot: stream ended (stopped)"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_IDLE, chan_now().shot);
    TEST_ASSERT_EQUAL_UINT(mark, s_nreq);   /* the worker still has the PUT */

    frame_taken();         /* its answer is nobody's */
    TEST_ASSERT_TRUE(logged("dropped"));
    assert_ended("stopped", from);
    job_done_http(KODE_OK);
    ms(1000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, display_asks(), "a frame after the stop");
    s = spied();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s.answers, "the stop was answered twice");

    net_done();
}

TEST_CASE("a stop between frames closes the stream's connection on the next "
          "tick", "[net_ao][shot]")
{
    spied_t  s;
    unsigned from;

    stream_fresh();
    from = s_nreq;
    stream_ask(2U, 60U);
    (void)spied();
    frame_lands(800U);
    frame_taken();         /* nothing out now */

    stream_stop();
    s = spied();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_STOPPED, s.last.result);
    ms(10U);
    assert_ended("stopped", from);
    job_done_http(KODE_OK);
    ms(1000U);
    TEST_ASSERT_EQUAL_UINT(0U, display_asks());

    net_done();
}

TEST_CASE("a stop with no stream running is refused and touches a capture in "
          "hand not at all", "[net_ao][shot]")
{
    spied_t  s;
    unsigned from;

    /* Nothing runs. */
    stream_fresh();
    from = s_nreq;
    stream_stop();
    s = spied();
    TEST_ASSERT_EQUAL_UINT(1U, s.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_NO_STREAM, s.last.result);
    TEST_ASSERT_EQUAL_UINT(from, s_nreq);
    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));

    /* A capture at the display: not a stream, so it goes on. */
    shot_ask(SHOT_TOKEN);
    TEST_ASSERT_EQUAL_UINT(1U, display_asks());
    stream_stop();
    s = spied();
    TEST_ASSERT_EQUAL_UINT(1U, s.answers);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_NO_STREAM, s.last.result);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_ONE, chan_now().shot);
    display_answer(KODE_OK, 900U);
    display_drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_PUT, last_job());
    http_reply(201U, "{\"ok\":true,\"id\":\"one\",\"seq\":1}");
    s = spied();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, s.last.result);

    net_done();
}

/* ---- A PUT that never comes back -------------------------------------------
 * The uploads' PUT runs on the first worker, which has a guard like any other
 * job's. When it fires, whoever waits is answered, and the worker - which
 * cannot be recalled - stays busy until the PUT returns; that late end only
 * wipes the answer, which the AO never read.
 */

TEST_CASE("a PUT that outlasts its guard answers the capture with no answer, "
          "and its late end only wipes", "[net_ao][shot]")
{
    NetShotRspEvt r;

    shot_fresh();
    drive_to_up();
    ao_spy_drain();

    shot_to_put(500U);
    TEST_ASSERT_EQUAL_UINT(0U, shot_answers(&r));
    secs(GUARD_STEPS(KODE_NET_GUARD_MS) / 100U + 1U);
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_NO_ANSWER, r.result);
    TEST_ASSERT_EQUAL_UINT32(0U, r.bytes);

    s_nresult = 0U;
    s_nforget = 0U;
    http_reply(201U, "{\"ok\":true,\"id\":\"late\",\"seq\":9}");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_nresult, "the late answer was read");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nforget,
                                   "the late answer was left in the driver");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_answers(&r), "answered twice");

    /* The worker is free again: the next capture goes out. */
    shot_to_put(500U);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_PUT, last_job());
    http_reply(201U, "{\"ok\":true,\"id\":\"a\",\"seq\":1}");
    TEST_ASSERT_EQUAL_UINT(1U, shot_answers(&r));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_R_OK, r.result);

    net_done();
}

TEST_CASE("a stream's PUT that outlasts its guard costs the frame, not the "
          "stream", "[net_ao][shot]")
{
    unsigned from;

    stream_fresh();
    from = s_nreq;
    stream_ask(1U, 300U);
    (void)spied();

    frame_lands(20000U);
    secs(GUARD_STEPS(KODE_NET_GUARD_MS) / 100U + 1U);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KSHOT_STREAM, chan_now().shot,
        "one frame lost to the guard ended the stream");
    TEST_ASSERT_EQUAL_UINT(0U, logged_count("stream ended"));

    s_nforget = 0U;
    frame_taken();   /* the late end of the PUT */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_nforget,
        "the late answer was left in the driver");
    TEST_ASSERT_EQUAL_UINT(1U, jobs_of(KDRV_NET_JOB_HTTP_PUT, from));

    net_done();
}

/* ---- kode_sys's requests: answered to the asking AO ---------------------------
 * The network requests post and return; AO_Net hands the I/O to its worker and
 * posts one answer to the AO that asked, the spy here, its status inside. A
 * GET's buffer and a POST's or a datagram's bytes are lent: the worker uses
 * them in place, and the answer waits for the worker to let go.
 */

/** @brief A static event AO_Net ignores, to fill a queue with. */
static const QEvt s_sys_filler = QEVT_INITIALIZER(IMU_TEST_REQ_SIG);

/** @brief What a lent buffer holds before the owner touches it. */
#define SYS_LENT_FILL  0x5AU

/** @brief Most blocks a case takes out of a pool to leave it dry. */
#define SYS_DRY_MAX  256U

/** @brief Bytes of the lent buffers below. */
#define SYS_BUF  16U

/** @brief The address the lookup stub resolves to. */
#define SYS_IP4  0x0201A8C0U

/** @brief What the GET stub puts in the body. */
#define SYS_BODY  "hello"

/** @brief Its bytes. */
#define SYS_BODY_LEN  5U

/** @brief The HTTP status the stubs answer. */
#define SYS_HTTP_STATUS  200U

/** @brief What the stubs saw last. */
static const void *s_sys_seen_body;
static const void *s_sys_seen_buf;
static const void *s_sys_seen_data;
static char        s_sys_seen_ctype[KODE_NET_CTYPE_MAX];
static uint32_t    s_sys_calls;
static kode_status_t s_sys_rc;

static kode_status_t sys_stub_lookup(const char *host, uint32_t *ip4, int n)
{
    (void)host; (void)n;
    s_sys_calls++;
    *ip4 = SYS_IP4;
    return s_sys_rc;
}

static kode_status_t sys_stub_ping(const char *host, uint8_t count, kdrv_net_ping_reply_t *rows,
                                   uint8_t cap, uint8_t *rows_written,
                                   kdrv_net_ping_stats_t *stats, int n)
{
    (void)host; (void)rows; (void)cap; (void)rows_written; (void)n;
    s_sys_calls++;
    stats->ip4    = SYS_IP4;
    stats->tx     = count;
    stats->rx     = count;
    stats->min_ms = 3U;
    stats->avg_ms = 4U;
    stats->max_ms = 5U;
    return s_sys_rc;
}

static void sys_stub_result(kdrv_net_http_result_t *out, uint8_t *buf, uint32_t cap)
{
    uint32_t keep = SYS_BODY_LEN;

    if ( (NULL == buf) || (cap < keep) )
    {
        keep = 0U;
    }
    if ( 0U < keep )
    {
        (void)memcpy(buf, SYS_BODY, keep);
    }
    out->status         = SYS_HTTP_STATUS;
    out->content_length = (int32_t)SYS_BODY_LEN;
    out->body_len       = SYS_BODY_LEN;
    out->kept           = keep;
    out->truncated      = SYS_BODY_LEN > keep;
}

static kode_status_t sys_stub_get(const char *url, uint8_t *buf, uint32_t cap,
                                  kdrv_net_http_result_t *out, uint32_t timeout_ms, int n)
{
    (void)url; (void)timeout_ms; (void)n;
    s_sys_calls++;
    s_sys_seen_buf = buf;
    sys_stub_result(out, buf, cap);
    return s_sys_rc;
}

static kode_status_t sys_stub_post(const char *url, const char *body, uint32_t len,
                                   const char *ctype, uint8_t *buf, uint32_t cap,
                                   kdrv_net_http_result_t *out, uint32_t timeout_ms, int n)
{
    (void)url; (void)len; (void)timeout_ms; (void)n;
    s_sys_calls++;
    s_sys_seen_body = body;
    s_sys_seen_buf  = buf;
    s_sys_seen_ctype[0] = '\0';
    if ( NULL != ctype )
    {
        (void)snprintf(s_sys_seen_ctype, sizeof(s_sys_seen_ctype), "%s", ctype);
    }
    sys_stub_result(out, buf, cap);
    return s_sys_rc;
}

static kode_status_t sys_stub_udp(const char *host, uint16_t port, const void *data,
                                  uint16_t len, int n)
{
    (void)host; (void)port; (void)len; (void)n;
    s_sys_calls++;
    s_sys_seen_data = data;
    return s_sys_rc;
}

/** @brief AO_Net online, the worker played from here, the asks' driver calls stubbed. */
static void sys_fresh(void)
{
    worker_fresh();
    ao_spy_other_drain();
    s_sys_calls     = 0U;
    s_sys_rc        = KODE_OK;
    s_sys_seen_body = NULL;
    s_sys_seen_buf  = NULL;
    s_sys_seen_data = NULL;
    kdrv_net_lookup_Stub(&sys_stub_lookup);
    kdrv_net_ping_Stub(&sys_stub_ping);
    kdrv_net_http_get_Stub(&sys_stub_get);
    kdrv_net_http_post_Stub(&sys_stub_post);
    kdrv_net_udp_send_Stub(&sys_stub_udp);
}

/** @brief Take the next answer the spy holds, which must be @p sig. */
static const QEvt *sys_take(enum_t sig)
{
    const QEvt *e = ao_spy_take();

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "every accepted request is answered");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE((uint32_t)sig, (uint32_t)e->sig, "the wrong answer");
    return e;
}

TEST_CASE("kode_sys_net_lookup runs on the worker and answers the asking AO once",
          "[net_ao][ask]")
{
    const NetHostRspEvt *a;

    sys_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_DENIED, kode_sys_net_lookup(ao_spy(), "example.com"),
                                  "a second request of the kind waits for the first's answer");
    (void)net_settle();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_sys_calls, "the lookup ran on the worker");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered before the worker returned");
    deliver_held(KODE_OK);

    a = (const NetHostRspEvt *)(const void *)sys_take(NET_LOOKUP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_HEX32(SYS_IP4, a->stats.ip4);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "posted once, not published as well");

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"),
                                  "the answer opens the door again");
    (void)net_settle();
    deliver_held(KODE_ERROR);
    a = (const NetHostRspEvt *)(const void *)sys_take(NET_LOOKUP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_ERROR, a->rc, "a name that does not resolve");
    TEST_ASSERT_EQUAL_HEX32(0U, a->stats.ip4);
    worker_done();
}

TEST_CASE("kode_sys_net_ping answers the session's numbers",
          "[net_ao][ask]")
{
    const NetHostRspEvt *a;

    sys_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_ping(ao_spy(), "example.com", 3U));
    (void)net_settle();
    deliver_held(KODE_OK);
    a = (const NetHostRspEvt *)(const void *)sys_take(NET_PING_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT16(3U, a->stats.tx);
    TEST_ASSERT_EQUAL_UINT16(3U, a->stats.rx);
    TEST_ASSERT_EQUAL_UINT32(4U, a->stats.avg_ms);
    TEST_ASSERT_EQUAL_UINT32(5U, a->stats.max_ms);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "no entry was published for an ask");
    worker_done();
}

TEST_CASE("with no network every kode_sys network request is answered KODE_IO_ERROR, the driver untouched",
          "[net_ao][ask]")
{
    uint8_t buf[SYS_BUF];
    uint8_t sentinel[SYS_BUF];

    sys_fresh();
    go_offline();
    ao_spy_drain();
    (void)memset(buf, SYS_LENT_FILL, sizeof(buf));
    (void)memcpy(sentinel, buf, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_ping(ao_spy(), "example.com", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_http_get(ao_spy(), "http://example.com/", buf, SYS_BUF));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_udp_send(ao_spy(), "example.com", 9U, buf, 4U));
    (void)net_settle();

    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR,
                           ((const NetHostRspEvt *)(const void *)sys_take(NET_LOOKUP_RSP_SIG))->rc);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR,
                           ((const NetHostRspEvt *)(const void *)sys_take(NET_PING_RSP_SIG))->rc);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR,
                           ((const NetHttpRspEvt *)(const void *)sys_take(NET_HTTP_RSP_SIG))->rc);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_IO_ERROR,
                           ((const NetUdpDoneEvt *)(const void *)sys_take(NET_UDP_RSP_SIG))->rc);
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_sys_calls, "the driver was asked with no network");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(sentinel, buf, sizeof(buf), "a refused GET wrote its buffer");
    worker_done();
}

TEST_CASE("kode_sys_http_get fills the lent buffer on the worker and answers the status",
          "[net_ao][ask]")
{
    uint8_t              buf[SYS_BUF];
    const NetHttpRspEvt *a;

    sys_fresh();
    (void)memset(buf, SYS_LENT_FILL, sizeof(buf));
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_http_get(ao_spy(), "https://example.com/x", buf, SYS_BUF));
    (void)net_settle();
    TEST_ASSERT_EQUAL_PTR_MESSAGE(buf, s_sys_seen_buf, "the worker wrote somewhere else");
    deliver_held(KODE_OK);

    a = (const NetHttpRspEvt *)(const void *)sys_take(NET_HTTP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_FALSE(a->post);
    TEST_ASSERT_EQUAL_UINT16(SYS_HTTP_STATUS, a->result.status);
    TEST_ASSERT_EQUAL_UINT32(SYS_BODY_LEN, a->result.kept);
    TEST_ASSERT_FALSE(a->result.truncated);
    TEST_ASSERT_EQUAL_MEMORY(SYS_BODY, buf, SYS_BODY_LEN);
    TEST_ASSERT_EQUAL_UINT8(SYS_LENT_FILL, buf[SYS_BODY_LEN]);
    worker_done();
}

TEST_CASE("kode_sys_http_post sends the lent body in place with its type, and a refused server is a status",
          "[net_ao][ask]")
{
    static const char    body[] = "{\"a\":1}";
    uint8_t              buf[SYS_BUF];
    const NetHttpRspEvt *a;

    sys_fresh();
    s_sys_rc = KODE_ERROR;
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_http_post(ao_spy(), "https://example.com/p", body,
                                                      (uint32_t)(sizeof(body) - 1U),
                                                      "application/json", buf, SYS_BUF));
    (void)net_settle();
    TEST_ASSERT_EQUAL_PTR_MESSAGE(body, s_sys_seen_body, "the body was not read in place");
    TEST_ASSERT_EQUAL_STRING("application/json", s_sys_seen_ctype);
    deliver_held(KODE_ERROR);

    a = (const NetHttpRspEvt *)(const void *)sys_take(NET_HTTP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_ERROR, a->rc);
    TEST_ASSERT_TRUE(a->post);
    TEST_ASSERT_EQUAL_STRING("{\"a\":1}", body);
    worker_done();
}

TEST_CASE("kode_sys_udp_send sends the lent datagram and answers what left",
          "[net_ao][ask]")
{
    static const uint8_t data[4] = { 1U, 2U, 3U, 4U };
    const NetUdpDoneEvt *a;

    sys_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_udp_send(ao_spy(), "example.com", 9U, data, 4U));
    (void)net_settle();
    TEST_ASSERT_EQUAL_PTR(data, s_sys_seen_data);
    deliver_held(KODE_OK);
    a = (const NetUdpDoneEvt *)(const void *)sys_take(NET_UDP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, a->rc);
    TEST_ASSERT_EQUAL_UINT16(4U, a->sent);
    worker_done();
}

TEST_CASE("a GET whose guard fires is answered KODE_TIMEOUT only once the worker lets go of the buffer",
          "[net_ao][ask]")
{
    uint8_t              buf[SYS_BUF];
    const NetHttpRspEvt *a;

    sys_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_http_get(ao_spy(), "https://example.com/slow", buf, SYS_BUF));
    (void)net_settle();
    (void)net_tick(GUARD_STEPS(KODE_NET_GUARD_MS));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(),
                                   "answered while the worker still holds the lent buffer");

    deliver_held(KODE_OK);   /* the worker returns at last */
    a = (const NetHttpRspEvt *)(const void *)sys_take(NET_HTTP_RSP_SIG);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT, a->rc);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, a->result.kept, "a late body is not vouched for");
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_http_get(ao_spy(), "https://example.com/", buf, SYS_BUF),
                                  "the late answer opened the door and freed the worker");
    (void)net_settle();
    deliver_held(KODE_OK);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK,
                           ((const NetHttpRspEvt *)(const void *)sys_take(NET_HTTP_RSP_SIG))->rc);
    worker_done();
}

TEST_CASE("a lookup whose guard fires is answered then, and its late return answers nobody",
          "[net_ao][ask]")
{
    sys_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"));
    (void)net_settle();
    (void)net_tick(GUARD_STEPS(KODE_NET_GUARD_MS));
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_TIMEOUT,
                           ((const NetHostRspEvt *)(const void *)sys_take(NET_LOOKUP_RSP_SIG))->rc);
    deliver_held(KODE_OK);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "answered twice");
    worker_done();
}

TEST_CASE("two AOs asking at once are both answered: the second with the worker busy",
          "[net_ao][ask]")
{
    const QEvt *o;

    sys_fresh();
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"));
    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_net_lookup(ao_spy_other(), "example.com"),
                                  "the door is the asker's, not the kind's");
    (void)net_settle();
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL_MESSAGE(o, "the second asker is answered");
    TEST_ASSERT_EQUAL_UINT32((uint32_t)NET_LOOKUP_RSP_SIG, (uint32_t)o->sig);
    TEST_ASSERT_EQUAL_UINT_MESSAGE((uint32_t)KODE_DENIED, ((const NetHostRspEvt *)(const void *)o)->rc,
                                   "one job at a time: the worker was busy");
    QF_gc(o);
    deliver_held(KODE_OK);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK,
                           ((const NetHostRspEvt *)(const void *)sys_take(NET_LOOKUP_RSP_SIG))->rc);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_lookup(ao_spy_other(), "example.com"));
    (void)net_settle();
    deliver_held(KODE_OK);
    o = ao_spy_other_take();
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_UINT((uint32_t)KODE_OK, ((const NetHostRspEvt *)(const void *)o)->rc);
    QF_gc(o);
    worker_done();
}

TEST_CASE("a plain task's or a badly asked network request is refused at once and posts nothing",
          "[net_ao][ask]")
{
    char    longurl[KODE_SYS_NET_URL_MAX + 8U];
    uint8_t buf[SYS_BUF];

    sys_fresh();
    (void)memset(longurl, 'u', sizeof(longurl));
    longurl[sizeof(longurl) - 1U] = '\0';

    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_net_lookup(NULL, "example.com"));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_net_lookup(ao_spy(), NULL));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_net_lookup(ao_spy(), ""));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_net_ping(ao_spy(), "example.com", 0U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_net_ping(ao_spy(), "example.com",
                                                         KODE_SYS_NET_PING_MAX + 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_http_get(NULL, "http://x/", buf, SYS_BUF));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_http_get(ao_spy(), longurl, buf, SYS_BUF));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_http_post(ao_spy(), "http://x/", NULL, 1U, NULL,
                                                          buf, SYS_BUF));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_udp_send(ao_spy(), "example.com", 9U, NULL, 1U));
    TEST_ASSERT_EQUAL_INT(KODE_DENIED, kode_sys_udp_send(NULL, "example.com", 9U, buf, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Net->eQueue));
    TEST_ASSERT_EQUAL_UINT(0U, s_sys_calls);
    worker_done();
}

TEST_CASE("a full AO_Net queue refuses kode_sys's network requests with no resources",
          "[net_ao][ask]")
{
    uint8_t buf[SYS_BUF];

    sys_fresh();
    TEST_ASSERT_TRUE_MESSAGE(qf_support_queue_stuff(AO_Net, &s_sys_filler), "the queue never filled");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_net_lookup(ao_spy(), "example.com"));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_net_ping(ao_spy(), "example.com", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_http_get(ao_spy(), "http://x/", buf, SYS_BUF));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_udp_send(ao_spy(), "example.com", 9U, buf, 1U));
    (void)net_settle();

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"),
                                  "nothing was posted, so the door is open");
    (void)net_settle();
    deliver_held(KODE_OK);
    (void)sys_take(NET_LOOKUP_RSP_SIG);
    worker_done();
}

TEST_CASE("a dry pool refuses kode_sys's network request with no resources and posts nothing",
          "[net_ao][ask]")
{
    static NetHostRspEvt *held[SYS_DRY_MAX];
    uint32_t              n = 0U;

    sys_fresh();
    while ( SYS_DRY_MAX > n )
    {
        held[n] = Q_NEW_X(NetHostRspEvt, 0U, NET_LOOKUP_RSP_SIG);
        if ( NULL == held[n] )
        {
            break;
        }
        n++;
    }
    TEST_ASSERT_LESS_THAN_UINT32_MESSAGE(SYS_DRY_MAX, n, "the pool never ran dry");
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_net_lookup(ao_spy(), "example.com"));
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)uxQueueMessagesWaiting(AO_Net->eQueue));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        QF_gc(&held[i]->super);
    }

    TEST_ASSERT_EQUAL_INT_MESSAGE(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"),
                                  "nothing was posted, so the door is open");
    (void)net_settle();
    deliver_held(KODE_OK);
    (void)sys_take(NET_LOOKUP_RSP_SIG);
    worker_done();
}

/** @brief The body of the full-queue case, run in a child: the asker's queue is full, then AO_Net answers into it. */
static void sys_full_asker_body(void *arg)
{
    (void)arg;
    (void)qf_support_queue_stuff(ao_spy_other(), &s_sys_filler);
    (void)kode_sys_net_lookup(ao_spy_other(), "example.com");
    (void)net_settle();
    deliver_held(KODE_OK);
}

TEST_CASE("a network answer into an asker's full queue is QP's assert, not a drop",
          "[net_ao][ask]")
{
    sys_fresh();
    TEST_ASSERT_EQUAL_INT(ASK_CHILD_ABORTED, ask_support_fork_run(&sys_full_asker_body, NULL));
    ao_spy_other_drain();
    worker_done();
}

#if CONFIG_KODEOS_MQTT
TEST_CASE("an app's command loop to AO_Net leaves inbound MQTT its slots", "[net_ao]")
{
    kdrv_mqtt_msg_t msg;
    uint32_t        refused = 0U;
    uint32_t        msgs    = 0U;
    const QEvt     *e       = NULL;

    net_fresh();
    TEST_ASSERT_NOT_NULL_MESSAGE(s_mqtt_cb, "AO_Net gave kdrv_net its callback");
    ao_spy_subscribe(MQTT_MSG_SIG);

    /* An app above AO_Net publishing in a loop, AO_Net not yet running. */
    for ( uint32_t i = 0U; (2U * KQP_AO_QUEUE_LEN) > i; ++i )
    {
        if ( kode_sys_mqtt_publish("app/out", "x", 1U) != KODE_OK )
        {
            ++refused;
        }
    }
    TEST_ASSERT_GREATER_THAN_UINT32(0U, refused);
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));

    /* The broker delivers: the driver keeps the message and esp-mqtt's task
     * posts the notice with KQP_NET_MQTT_MARGIN. */
    (void)memset(&msg, 0, sizeof(msg));
    (void)snprintf(msg.topic, sizeof(msg.topic), "%s", "app/in");
    msg.data[0] = (uint8_t)'y';
    msg.len     = 1U;
    rxq_push(&msg);
    (void)s_mqtt_cb(KDRV_NET_MQTT_MSG, NULL, false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KQP_NET_CMD_MARGIN - 1U, uxQueueSpacesAvailable(AO_Net->eQueue),
                                   "the message took a slot the commands left");

    /* AO_Net runs: the publishes go back unread, the message is handled. */
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)NET_MQTT_RX_SIG == e->sig )
        {
            net_dispatch(e);
        }
        QF_gc(e);
    }
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)MQTT_MSG_SIG == e->sig )
        {
            ++msgs;
        }
    }
    net_done();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, msgs, "the message still arrived");
}

TEST_CASE("an app's requests to AO_Net stop where its commands do, leaving inbound MQTT its slots",
          "[net_ao][ask]")
{
    kdrv_mqtt_msg_t msg;
    uint32_t        msgs     = 0U;
    uint32_t        answered = 0U;
    const QEvt     *e        = NULL;

    net_fresh();
    ao_spy_other_drain();
    TEST_ASSERT_NOT_NULL_MESSAGE(s_mqtt_cb, "AO_Net gave kdrv_net its callback");
    ao_spy_subscribe(MQTT_MSG_SIG);
    net_queue_fill_to(KQP_NET_CMD_MARGIN + 1U);

    /* Two app AOs above AO_Net, each asking every kind, AO_Net not yet running. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_net_lookup(ao_spy(), "example.com"));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_net_ping(ao_spy(), "example.com", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_udp_send(ao_spy(), "example.com", 9U, "x", 1U));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_net_lookup(ao_spy_other(), "example.com"));
    TEST_ASSERT_EQUAL_INT(KODE_NO_RESOURCES, kode_sys_net_ping(ao_spy_other(), "example.com", 1U));
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));

    (void)memset(&msg, 0, sizeof(msg));
    (void)snprintf(msg.topic, sizeof(msg.topic), "%s", "app/in");
    msg.data[0] = (uint8_t)'y';
    msg.len     = 1U;
    rxq_push(&msg);
    (void)s_mqtt_cb(KDRV_NET_MQTT_MSG, NULL, false);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KQP_NET_CMD_MARGIN - 1U, uxQueueSpacesAvailable(AO_Net->eQueue),
                                   "the message took a slot the requests left");

    /* AO_Net runs: the fillers go back unread, the ask and the message are handled. */
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)MQTT_STATE_REQ_SIG != e->sig )
        {
            net_dispatch(e);
        }
        QF_gc(e);
    }
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)MQTT_MSG_SIG == e->sig )
        {
            ++msgs;
        }
        if ( (QSignal)NET_LOOKUP_RSP_SIG == e->sig )
        {
            ++answered;
        }
    }
    net_done();
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, msgs, "the message still arrived");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1U, answered, "the request taken is answered");
}
#endif

/* ---- The board's rest ------------------------------------------------------- */

/** @brief Whether the spy saw a ping end that says no session. */
static bool ping_refused(void)
{
    const QEvt *e;
    bool        refused = false;

    while ( (e = ao_spy_take()) != NULL )
    {
        if ( ((QSignal)NET_PING_END_SIG == e->sig)
            && (!((const NetPingEndEvt *)e)->ok) )
        {
            refused = true;
        }
    }
    return refused;
}

/** @brief The Kode channel up, AO_Radio's mailbox and the spy empty. */
static void lp_channel_up(void)
{
    chan_fresh();
    drive_to_up();
    s_drv_state = KDRV_MQTT_UP;
    radio_drain();
    ao_spy_subscribe(NET_PING_END_SIG);
    ao_spy_drain();
}

TEST_CASE("an ASLEEP round keeps the channel's session, starts nothing, and wakes to work again",
          "[net_ao][lowpower]")
{
    RadioNetLeftEvt left;
    unsigned        jobs;

    lp_channel_up();
    jobs = s_nreq;

    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs, s_nreq, "no job: the session is not touched");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&left, sizeof(left)),
                                  "the link is not told anything: it stays");

    ask_ping("192.168.0.1", 1U);
    TEST_ASSERT_TRUE_MESSAGE(ping_refused(), "new work is refused with its own answer");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs, s_nreq, "and nothing reached the driver");

    lp_awake(AO_Net);
    ask_ping("192.168.0.1", 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs + 1U, s_nreq, "awake, the ping runs");
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_PING, s_ran_job);
    net_done();
}

TEST_CASE("an OFF round closes the channel and the client, then tells AO_Radio",
          "[net_ao][lowpower]")
{
    RadioNetLeftEvt left;
    unsigned        jobs;

    lp_channel_up();
    jobs = s_nreq;
    s_npub = 0U;

    lp_round(AO_Net, KODE_LP_OFF);
    pull_jobs();
    lp_expect_silence();                      /* the stop is with the worker */
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, s_ran_job);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_NO_NET, chan_now().state);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_npub, "offline is said before the client goes");
    TEST_ASSERT_EQUAL_STRING(KCHAN_WILL, s_pubs[0].data);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&left, sizeof(left)),
                                  "the link waits for the stop");

    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_INT((int)RADIO_NET_LEFT_SIG, (int)radio_take(&left, sizeof(left)));
    TEST_ASSERT_EQUAL_UINT32(s_lp_round_no, left.round);
    net_done();
}

TEST_CASE("an OFF round abandoned by another owner lets the channel join again",
          "[net_ao][lowpower]")
{
    IdReqEvt q;

    lp_channel_up();
    lp_round(AO_Net, KODE_LP_OFF);
    pull_jobs();
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    lp_expect_answer(false, NULL);
    radio_drain();

    lp_awake(AO_Net);
    pull_jobs();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&q, sizeof(q)),
                                  "the join waits the settle after AWAKE");
    secs(3U);
    pull_jobs();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_JOINING, chan_now().state);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)ID_REQ_SIG, (int)radio_take(&q, sizeof(q)),
                                  "the join begins where it always does: the C5's certificate");

    lp_awake(AO_Net);                         /* it repeats: nothing starts twice */
    pull_jobs();
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    net_done();
}

TEST_CASE("an OFF round with no session answers at once and tells AO_Radio",
          "[net_ao][lowpower]")
{
    RadioNetLeftEvt left;

    chan_fresh();
    radio_drain();

    lp_round(AO_Net, KODE_LP_OFF);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_EQUAL_INT((int)RADIO_NET_LEFT_SIG, (int)radio_take(&left, sizeof(left)));
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("a job or a transfer in flight refuses the round", "[net_ao][lowpower]")
{
    chan_fresh();
    drive_to_challenge();                     /* the challenge's POST is with the worker */
    TEST_ASSERT_NOT_NULL(s_held);

    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(true, "net job");
    lp_round(AO_Net, KODE_LP_OFF);
    lp_expect_answer(true, "net job");
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, s_ran_job);
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("an AWAKE that comes while the client is still stopping rejoins when the stop returns",
          "[net_ao][lowpower]")
{
    IdReqEvt q;

    lp_channel_up();
    lp_round(AO_Net, KODE_LP_OFF);            /* the stop is with the worker */
    pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, s_ran_job);
    radio_drain();

    lp_awake(AO_Net);                         /* another owner was busy: abandoned */
    pull_jobs();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&q, sizeof(q)),
                                  "the client is still stopping: nothing starts over it");

    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    lp_expect_no_more();                      /* the round is over: no answer */
    secs(3U);                                 /* the settle after AWAKE */
    pull_jobs();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_JOINING, chan_now().state);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)ID_REQ_SIG, (int)radio_take(&q, sizeof(q)),
                                  "the channel must not stay closed for the rest of the boot");
    net_done();
}

/* ---- The fast mode and the jobs ----------------------------------------------
 * A job with either worker holds the fast mode, and for NET_FAST_JOB_HOLD_MS
 * after the last one comes back. AO_Radio is asked once for each edge of the
 * channel's hold and the jobs' together: on when the first begins, off when
 * the last ends. Nothing of the jobs' hold outlives the address or the rest.
 */

/** @brief A download's address, any. */
#define JOB_FAST_URL      "http://example.invalid/app.bin"

TEST_CASE("a job handed to the worker asks AO_Radio for the fast mode once",
          "[net_ao][fast]")
{
    fast_posts_t f;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    f = radio_fast_posts();
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    worker_done();

    TEST_ASSERT_EQUAL_UINT(1U, f.on);
    TEST_ASSERT_EQUAL_UINT(0U, f.off);
}

TEST_CASE("a job handed to the download's worker asks for the fast mode too",
          "[net_ao][fast]")
{
    fast_posts_t f;

    worker_fresh();
    ask_stream_open(JOB_FAST_URL);
    f = radio_fast_posts();
    deliver_stream(KODE_OK);
    worker_done();

    TEST_ASSERT_EQUAL_UINT(1U, f.on);
    TEST_ASSERT_EQUAL_UINT(0U, f.off);
}

TEST_CASE("two jobs back to back ask for the fast mode once, not twice",
          "[net_ao][fast]")
{
    fast_posts_t first;
    fast_posts_t second;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    first = radio_fast_posts();
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    ask_ping("192.168.0.2", 1U);
    second = radio_fast_posts();
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    worker_done();

    TEST_ASSERT_EQUAL_UINT(1U, first.on);
    TEST_ASSERT_EQUAL_UINT(0U, second.on);
    TEST_ASSERT_EQUAL_UINT(0U, second.off);
}

TEST_CASE("the fast mode is given back NET_FAST_JOB_HOLD_MS after the last "
          "job ends, and not before", "[net_ao][fast]")
{
    fast_posts_t before;
    fast_posts_t at;
    fast_posts_t later;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    radio_drain();

    ms(JOB_FAST_HOLD_MS - 100U);
    before = radio_fast_posts();
    ms(200U);
    at = radio_fast_posts();
    ms(2U * JOB_FAST_HOLD_MS);
    later = radio_fast_posts();
    worker_done();

    TEST_ASSERT_EQUAL_UINT(0U, before.off);
    TEST_ASSERT_EQUAL_UINT(1U, at.off);
    TEST_ASSERT_EQUAL_UINT(0U, at.on);
    TEST_ASSERT_EQUAL_UINT(0U, later.off);
    TEST_ASSERT_EQUAL_UINT(0U, later.on);
}

TEST_CASE("a job that starts during the tail keeps the fast mode and sends "
          "no new request", "[net_ao][fast]")
{
    fast_posts_t restart;
    fast_posts_t held;
    fast_posts_t end;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    radio_drain();

    ms(3000U);
    ask_ping("192.168.0.2", 1U);
    restart = radio_fast_posts();
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    ms(3000U);                /* 6 s after the first end: its tail is gone */
    held = radio_fast_posts();
    ms(2100U);                /* 5.1 s after the second */
    end = radio_fast_posts();
    worker_done();

    TEST_ASSERT_EQUAL_UINT(0U, restart.on);
    TEST_ASSERT_EQUAL_UINT(0U, restart.off);
    TEST_ASSERT_EQUAL_UINT(0U, held.off);
    TEST_ASSERT_EQUAL_UINT(1U, end.off);
}

TEST_CASE("the tail starts only when neither worker holds a job",
          "[net_ao][fast]")
{
    fast_posts_t first;
    fast_posts_t second;
    fast_posts_t inside;
    fast_posts_t before;
    fast_posts_t at;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    first = radio_fast_posts();
    ask_stream_open(JOB_FAST_URL);
    second = radio_fast_posts();
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    ms(2U * JOB_FAST_HOLD_MS);            /* the download's job is still inside */
    inside = radio_fast_posts();

    deliver_stream(KODE_OK);
    ms(JOB_FAST_HOLD_MS - 100U);
    before = radio_fast_posts();
    ms(200U);
    at = radio_fast_posts();
    worker_done();

    TEST_ASSERT_EQUAL_UINT(1U, first.on);
    TEST_ASSERT_EQUAL_UINT(0U, second.on);
    TEST_ASSERT_EQUAL_UINT(0U, inside.off);
    TEST_ASSERT_EQUAL_UINT(0U, before.off);
    TEST_ASSERT_EQUAL_UINT(1U, at.off);
}

TEST_CASE("the channel's hold running when the jobs' tail ends keeps the "
          "fast mode", "[net_ao][fast]")
{
    fast_posts_t    joined;
    fast_posts_t    mid;
    fast_posts_t    end;
    ChannelStateEvt held;

    cmd_fresh();                          /* the join's last job ended just now */
    radio_drain();

    rx_mqtt(CHAN_T_IN, "ping", 4U, false);
    joined = radio_fast_posts();
    ms(JOB_FAST_HOLD_MS + 500U);          /* the jobs' tail ends here */
    mid  = radio_fast_posts();
    held = chan_now();
    secs(60U);                            /* the channel's own hold ends */
    end = radio_fast_posts();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, joined.on);
    TEST_ASSERT_EQUAL_UINT(0U, joined.off);
    TEST_ASSERT_EQUAL_UINT(0U, mid.off);
    TEST_ASSERT_TRUE(held.fast);
    TEST_ASSERT_EQUAL_UINT(1U, end.off);
    TEST_ASSERT_EQUAL_UINT(0U, end.on);
}

TEST_CASE("losing the network ends the jobs' hold without a request, and a "
          "job after it asks again", "[net_ao][fast]")
{
    fast_posts_t gone;
    fast_posts_t late;
    fast_posts_t again;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    radio_drain();

    go_offline();                         /* the job is still with the worker */
    gone = radio_fast_posts();
    job_done(KDRV_NET_JOB_PING, KODE_OK); /* it comes back with no address */
    ms(2U * JOB_FAST_HOLD_MS);
    late = radio_fast_posts();

    go_online();
    ask_ping("192.168.0.2", 1U);
    again = radio_fast_posts();
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    worker_done();

    TEST_ASSERT_EQUAL_UINT(0U, gone.on);
    TEST_ASSERT_EQUAL_UINT(0U, gone.off);
    TEST_ASSERT_EQUAL_UINT(0U, late.on);
    TEST_ASSERT_EQUAL_UINT(0U, late.off);
    TEST_ASSERT_EQUAL_UINT(1U, again.on);
}

TEST_CASE("the stop of a lost network does not ask for the fast mode",
          "[net_ao][fast]")
{
    fast_posts_t gone;
    fast_posts_t late;

    lp_channel_up();
    ms(2U * JOB_FAST_HOLD_MS);            /* the join's tail is over */
    radio_drain();

    go_offline();                         /* the client is stopped: a job */
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, s_ran_job);
    gone = radio_fast_posts();
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    ms(2U * JOB_FAST_HOLD_MS);
    late = radio_fast_posts();
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, gone.on);
    TEST_ASSERT_EQUAL_UINT(0U, gone.off);
    TEST_ASSERT_EQUAL_UINT(0U, late.on);
    TEST_ASSERT_EQUAL_UINT(0U, late.off);
}

TEST_CASE("an ASLEEP round drops the jobs' tail at once", "[net_ao][fast]")
{
    fast_posts_t rest;
    fast_posts_t late;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    radio_drain();

    lp_round(AO_Net, KODE_LP_ASLEEP);
    rest = radio_fast_posts();
    lp_expect_answer(false, NULL);
    ms(2U * JOB_FAST_HOLD_MS);            /* nothing is left to fire */
    late = radio_fast_posts();
    lp_awake(AO_Net);
    worker_done();

    TEST_ASSERT_EQUAL_UINT(1U, rest.off);
    TEST_ASSERT_EQUAL_UINT(0U, rest.on);
    TEST_ASSERT_EQUAL_UINT(0U, late.off);
    TEST_ASSERT_EQUAL_UINT(0U, late.on);
}

TEST_CASE("an OFF round drops the jobs' tail at once", "[net_ao][fast]")
{
    fast_posts_t rest;
    fast_posts_t late;

    worker_fresh();
    ask_ping("192.168.0.1", 1U);
    job_done(KDRV_NET_JOB_PING, KODE_OK);
    radio_drain();

    lp_round(AO_Net, KODE_LP_OFF);
    rest = radio_fast_posts();
    lp_expect_answer(false, NULL);
    ms(2U * JOB_FAST_HOLD_MS);
    late = radio_fast_posts();
    lp_awake(AO_Net);
    worker_done();

    TEST_ASSERT_EQUAL_UINT(1U, rest.off);
    TEST_ASSERT_EQUAL_UINT(0U, rest.on);
    TEST_ASSERT_EQUAL_UINT(0U, late.off);
    TEST_ASSERT_EQUAL_UINT(0U, late.on);
}

TEST_CASE("the stop of an OFF round does not ask for the fast mode",
          "[net_ao][fast]")
{
    fast_posts_t f;

    lp_channel_up();
    ms(2U * JOB_FAST_HOLD_MS);            /* the join's tail is over */
    radio_drain();

    lp_round(AO_Net, KODE_LP_OFF);        /* the client is stopped: a job */
    pull_jobs();
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_MQTT_STOP, s_ran_job);
    f = radio_fast_posts();
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    lp_awake(AO_Net);
    net_done();

    TEST_ASSERT_EQUAL_UINT(0U, f.on);
    TEST_ASSERT_EQUAL_UINT(0U, f.off);
}

/* ---- Real time across the rest ----------------------------------------------
 * A QTimeEvt does not count while the tick is stopped, so the channel's renewal
 * and back-off carry a deadline in khal_time_us(). The cases below play the
 * rest by advancing the clock and not running the tick. */

/** @brief The renewal of a session, microseconds (CONFIG_KODEOS_CHANNEL_RENEW_S is 120). */
#define RENEW_US        120000000ULL

/** @brief A short stay asleep, milliseconds. */
#define NAP_MS          5000U

/** @brief A long stay asleep: an hour, milliseconds. */
#define NIGHT_MS        3600000U

/** @brief Run the clock and the tick until the channel submits a job, or give up. */
static void secs_until_job(unsigned before, unsigned limit_s)
{
    for ( unsigned i = 0U; (i < limit_s) && (s_nreq == before); ++i )
    {
        secs(1U);
    }
}

TEST_CASE("a renewal that falls due asleep is given to AO_LowPower as a real time, "
          "and no timer is armed for it", "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    uint64_t          up_at;
    unsigned          jobs;

    lp_channel_up();
    up_at = khal_time_us();
    jobs  = s_nreq;

    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the renewal's time was not given");
    TEST_ASSERT_TRUE_MESSAGE((up_at + RENEW_US) == w.due_us,
                             "the time is the connect plus the renewal, in real time");

    /* Run the tick here, which the board does not: a timer left armed would
     * fire in the rest and give its (passed) deadline again. */
    secs((unsigned)(RENEW_US / 1000000ULL) + 10U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs, s_nreq, "nothing starts while the board sleeps");
    TEST_ASSERT_FALSE_MESSAGE(fake_lowpower_wake_take(&w),
                              "the timer was left armed: it fired in the rest");
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("a renewal overdue when the board wakes begins after the settle, not a whole "
          "renewal later", "[net_ao][lowpower]")
{
    unsigned jobs;

    lp_channel_up();
    jobs = s_nreq;
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);

    fake_time_advance_ms(NIGHT_MS);           /* the tick was stopped all along */
    lp_awake(AO_Net);
    pull_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs, s_nreq, "the C5 link is still coming back");

    secs(1U);
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);
    secs(2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs + 1U, s_nreq, "the renewal asks the api again");
    TEST_ASSERT_EQUAL_INT((int)KDRV_NET_JOB_HTTP_POST, (int)req_at(jobs)->type);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);
    net_done();
}

TEST_CASE("an AWAKE that abandons a round leaves the renewal at its real time",
          "[net_ao][lowpower]")
{
    unsigned jobs;

    lp_channel_up();
    jobs = s_nreq;
    secs(30U);
    lp_round(AO_Net, KODE_LP_ASLEEP);         /* another owner refused: the tick never stopped */
    lp_awake(AO_Net);
    lp_awake(AO_Net);                         /* and it repeats */

    secs(88U);                                /* 118 s after the connect */
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);
    secs(3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs + 1U, s_nreq, "on time: not earlier, not later");
    net_done();
}

TEST_CASE("a back-off pending across a sleep is given as a real time and resumes from it",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    ChannelStateEvt   st;
    uint64_t          dropped_at;
    uint64_t          due;
    unsigned          jobs;

    chan_fresh();
    drive_to_up();
    secs(61U);                                /* a session that earned a fresh back-off */
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    dropped_at = khal_time_us();
    st = last_chan();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, st.state);
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    radio_drain();
    ao_spy_drain();
    jobs = s_nreq;

    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the back-off's end was not given");
    due = w.due_us;
    TEST_ASSERT_TRUE((dropped_at + 3000000ULL) < due);    /* 5 s at 75..125 % */
    TEST_ASSERT_TRUE((dropped_at + 7000000ULL) > due);

    fake_time_advance_ms(NAP_MS - 1000U);     /* still waiting when the board wakes */
    lp_awake(AO_Net);
    secs_until_job(jobs, 10U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs + 1U, s_nreq, "the whole join starts again");
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() >= due, "not before the real end of the wait");
    net_done();
}

TEST_CASE("a back-off that ends asleep is not waited for again when the board wakes",
          "[net_ao][lowpower]")
{
    unsigned jobs;

    chan_fresh();
    drive_to_up();
    secs(61U);
    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    jobs = s_nreq;

    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    fake_time_advance_ms(NIGHT_MS);
    lp_awake(AO_Net);
    secs(1U);
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);
    secs(2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs + 1U, s_nreq, "the settle, not another back-off");
    net_done();
}

TEST_CASE("a drop while the board sleeps waits in real time and starts nothing",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    unsigned          jobs;

    lp_channel_up();
    secs(61U);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);        /* the renewal's */

    mqtt_link(KDRV_NET_MQTT_DISCONNECTED);    /* the C5 woke the P4 to say so */
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    jobs = s_nreq;
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_WAITING, chan_now().state);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w),
                             "the new wait is given to AO_LowPower");
    TEST_ASSERT_TRUE(khal_time_us() < w.due_us);

    secs(30U);                                /* were a QTimeEvt armed, it would fire */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs, s_nreq, "no retry while the board sleeps");

    lp_awake(AO_Net);
    secs_until_job(jobs, 10U);
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    net_done();
}

TEST_CASE("a timer that falls due as the board goes to sleep waits for the next AWAKE",
          "[net_ao][lowpower]")
{
    static const QEvt timer = QEVT_INITIALIZER(NET_CHAN_TMO_SIG);
    LowPowerWakeAtEvt w;
    unsigned          jobs;

    lp_channel_up();
    jobs = s_nreq;
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);

    fake_time_advance_ms(NIGHT_MS);
    net_dispatch(&timer);                     /* queued before the tick stopped */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs, s_nreq, "no renewal begins in the rest");
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the board is asked to wake for it");
    TEST_ASSERT_TRUE(khal_time_us() >= w.due_us);

    lp_awake(AO_Net);
    secs(3U);
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    net_done();
}

TEST_CASE("the address that arrives while the board sleeps joins at the next AWAKE",
          "[net_ao][lowpower]")
{
    IdReqEvt          q;
    LowPowerWakeAtEvt w;

    chan_fresh();
    radio_drain();
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_FALSE_MESSAGE(fake_lowpower_wake_take(&w), "nothing is due, so nothing is given");

    go_online();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&q, sizeof(q)),
                                  "the C5 is not asked for anything in the rest");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_NO_NET, chan_now().state);

    (void)fake_lowpower_wake_take(&w);        /* the "now" note of the next test */

    lp_awake(AO_Net);
    pull_jobs();
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, (int)radio_take(&q, sizeof(q)),
                                  "AO_Radio is bringing the C5 link back: not on the instant");
    secs(1U);
    pull_jobs();
    TEST_ASSERT_EQUAL_INT(0, (int)radio_take(&q, sizeof(q)));
    secs(2U);
    pull_jobs();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_JOINING, chan_now().state);
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)ID_REQ_SIG, (int)radio_take(&q, sizeof(q)),
                                  "the join begins after the settle");
    net_done();
}

TEST_CASE("an address that arrives while the board sleeps wakes it, now", "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    chan_fresh();
    radio_drain();
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);

    go_online();
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w),
                             "nothing would wake the board for the start");
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() >= w.due_us, "the time is now, not later");
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("a join between steps refuses the round to ASLEEP and says so", "[net_ao][lowpower]")
{
    chan_fresh();
    go_online();                              /* the C5 owes the certificate */
    radio_drain();

    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(true, "net joining");
    lp_awake(AO_Net);
    net_done();
}

/* ---- A radio wake: the channel's work with the glass asleep -------------------- */

TEST_CASE("a radio wake with nothing due is done at once, and the next round is the ordinary one",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    unsigned          jobs;

    lp_channel_up();
    jobs = s_nreq;
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Net);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(),
                                   "the renewal is a long way off: the work is done");
    TEST_ASSERT_EQUAL_UINT(jobs, s_nreq);

    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the deadline goes back to AO_LowPower");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(), "and nothing is said twice");
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("a radio wake whose renewal is due works through it and is done at its end",
          "[net_ao][lowpower]")
{
    IdSignReqEvt      sq;
    IdReqEvt          q;
    LowPowerWakeAtEvt w;
    static char       answer[1000];
    unsigned          jobs;

    lp_channel_up();
    jobs = s_nreq;
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);
    fake_time_advance_ms(NIGHT_MS);           /* the renewal fell due asleep */
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Net);
    pull_jobs();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(),
                                   "the renewal is due: not done");
    secs(3U);                                 /* the settle, then the challenge */
    TEST_ASSERT_EQUAL_UINT_MESSAGE(jobs + 1U, s_nreq, "the renewal asks the api again");
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_radio_done_count());

    http_reply(200U, "{\"nonce\":\"v1.connect.second\"}");
    TEST_ASSERT_EQUAL_INT(ID_SIGN_REQ_SIG, (int)radio_take(&sq, sizeof(sq)));
    answer_sign((uint8_t)KODE_ST_OK, 70U);
    s_drv_state = KDRV_MQTT_UP;
    connect_answer(answer, sizeof(answer));
    http_reply(200U, answer);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(),
                                   "the new session is still being swapped in");
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    job_done(KDRV_NET_JOB_MQTT_START, KODE_OK);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)CHAN_UP, chan_now().state);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(),
                                   "the session is up for another renewal: done");
    (void)q;
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("a radio wake whose renewal fails is done once it waits out a back-off",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    unsigned          jobs;

    lp_channel_up();
    jobs = s_nreq;
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);
    fake_time_advance_ms(NIGHT_MS);
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Net);
    secs(3U);
    TEST_ASSERT_EQUAL_UINT(jobs + 1U, s_nreq);
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_radio_done_count());

    http_reply(503U, "{\"error\":\"channel_unavailable\"}");   /* the api is unreachable */

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(),
                                   "the back-off is longer than the settle: done");
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the retry's time is given to AO_LowPower");
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("a round that arrives in a radio wake ends its work: no done is said afterwards",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    lp_channel_up();
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);
    fake_time_advance_ms(NIGHT_MS);
    (void)fake_lowpower_radio_done_count();

    lp_radio(AO_Net);
    lp_round(AO_Net, KODE_LP_ASLEEP);          /* the renewal is still due */
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);
    secs(3U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_radio_done_count());
    lp_awake(AO_Net);
    net_done();
}

/* ---- The scheduler's mqtt check ------------------------------------------------
 * The scheduler hands AO_Net the time of its check (net_check_at()). A round
 * for ASLEEP gives it to AO_LowPower, a radio wake that finds it due waits the
 * dwell, and a check made is told back (sched_check_done()). */

/** @brief The wall clock the cases below start from: 18:37:12 on 2026-10-30, Unix seconds. */
#define CHK_T0          INT64_C(1793385432)

/** @brief A quarter of an hour, seconds. */
#define CHK_QUARTER_S   INT64_C(900)

/** @brief Microseconds in a second, for the wall clock. */
#define CHK_US_PER_S    UINT64_C(1000000)

/** @brief The wall clock at the start, AO_Scheduler's queue empty, and no note of AO_LowPower's left over. */
static void chk_prepare(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Scheduler);
    while ( xQueueReceive(AO_Scheduler->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    fake_time_wall_set((uint64_t)CHK_T0 * CHK_US_PER_S);
    (void)fake_lowpower_wake_take(&(LowPowerWakeAtEvt){ 0 });
    (void)fake_lowpower_radio_done_count();
}

/** @brief A fresh AO_Net, offline: no Wi-Fi address, so nothing to listen to. */
static void chk_fresh(void)
{
    net_fresh();
    chk_prepare();
}

/** @brief A fresh AO_Net with the Kode channel up, so that something listens (the link is the case's to say). */
static void chk_listening(void)
{
    lp_channel_up();
    chk_prepare();
}

/** @brief A fresh AO_Net online with the channel switched off and no client: nothing listens. */
static void chk_online_unused(void)
{
    net_fresh();
    fake_nvs_reset();
    (void)config_ask(AO_Net, &(config_ask_t){ .key = KSET_KEY_CHAN, .set = true, .b = false });
    go_online();
    radio_drain();
    chk_prepare();
}

/** @brief Tell AO_Net where the C5 link stands. */
static void chk_link(kradio_link_t link)
{
    RadioStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(RADIO_STATE_SIG);
    e.link  = (uint8_t)link;
    net_dispatch(&e.super);
}

/** @brief The scheduler gives the time of the check, and AO_Net takes it. */
static void chk_give(int64_t due_s)
{
    TEST_ASSERT_TRUE(net_check_at(due_s));
    (void)net_settle();
}

/**
 * @brief Take what AO_Net told AO_Scheduler: the checks it made.
 *
 * @param passed Receives the time of the newest.
 * @return How many were told.
 */
static unsigned chk_told(int64_t *passed)
{
    const QEvt *e = NULL;
    unsigned n = 0U;

    while ( xQueueReceive(AO_Scheduler->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)SCHED_CHECK_DONE_SIG == e->sig )
        {
            n++;
            *passed = ((const SchedCheckDoneEvt *)e)->passed_s;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief The wall clock and the uptime move together to @p wall_s seconds. */
static void chk_clock_to(int64_t wall_s)
{
    uint64_t const now_us = (uint64_t)wall_s * CHK_US_PER_S;

    fake_time_advance_ms((uint32_t)((now_us - (uint64_t)CHK_T0 * CHK_US_PER_S) / 1000U));
    fake_time_wall_set(now_us);
}

/* test_net_check_wake_time */
TEST_CASE("a round gives AO_LowPower the check's time, and the channel's when that is earlier",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    /* The renewal is two minutes off: it is earlier than a quarter of an hour. */
    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + CHK_QUARTER_S);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE(fake_lowpower_wake_take(&w));
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() + RENEW_US >= w.due_us, "the channel's deadline");
    lp_expect_no_more();
    lp_awake(AO_Net);
    net_done();

    /* And the check is earlier than the renewal. */
    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 30);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the check's time is given");
    TEST_ASSERT_EQUAL_UINT64(khal_time_us() + (30U * CHK_US_PER_S), w.due_us);
    lp_expect_no_more();
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("the check wakes nothing when nothing listens or no time was given",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    /* No C5 link. */
    chk_listening();
    chk_give(CHK_T0 + 30);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the channel's own deadline is still given");
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() + (30U * CHK_US_PER_S) < w.due_us, "but not the check's");
    lp_awake(AO_Net);
    net_done();

    /* No Wi-Fi address: the link is up and nothing is joined. */
    chk_fresh();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + CHK_QUARTER_S);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_FALSE_MESSAGE(fake_lowpower_wake_take(&w), "Wi-Fi off, nothing to wake");
    lp_awake(AO_Net);
    net_done();

    /* The channel switched off and no client of the user's. */
    chk_online_unused();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + CHK_QUARTER_S);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    TEST_ASSERT_FALSE_MESSAGE(fake_lowpower_wake_take(&w), "channel off, no client, nothing to wake");
    lp_awake(AO_Net);
    net_done();
    fake_nvs_reset();

    /* No time given. */
    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(-1);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE(fake_lowpower_wake_take(&w));
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() + (30U * CHK_US_PER_S) < w.due_us, "only the channel's");
    lp_awake(AO_Net);
    net_done();
}

/* net_chk_listens(): a client of the user's listens with the channel off. */
TEST_CASE("a client of the user's is something to listen to, with the channel off",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    int64_t passed = 0;

    chk_online_unused();
    chk_link(KRADIO_UP);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);       /* a session that is not the channel's */
    chk_give(CHK_T0 + 60);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "the check's time is given");
    TEST_ASSERT_EQUAL_UINT64(khal_time_us() + (60U * CHK_US_PER_S), w.due_us);

    chk_clock_to(CHK_T0 + 60);
    lp_radio(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(), "the dwell is on");
    secs((KODE_NET_CHECK_DWELL_MS / 1000U) + 1U);
    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_radio_done_count());
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    lp_awake(AO_Net);
    net_done();
    fake_nvs_reset();
}

/* net_chk_listens(): with no Wi-Fi address nothing listens, a session or not. */
TEST_CASE("a session left over without a Wi-Fi address is nothing to listen to",
          "[net_ao][lowpower]")
{
    int64_t passed = 0;

    chk_online_unused();
    chk_link(KRADIO_UP);
    mqtt_link(KDRV_NET_MQTT_CONNECTED);
    s_auto = false;                           /* the worker keeps the next job */
    net_http_get_request("http://example.invalid/");   /* the worker is taken */
    (void)net_settle();
    TEST_ASSERT_NOT_NULL_MESSAGE(s_held, "the GET holds the worker");
    go_offline();                             /* so the client's stop waits: it is still up */
    chk_give(CHK_T0 + 60);

    chk_clock_to(CHK_T0 + 60);
    lp_radio(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, chk_told(&passed), "no address: no dwell, made at once");
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 60, passed);
    lp_awake(AO_Net);
    net_done();
    fake_nvs_reset();
}

TEST_CASE("a time given while the board sleeps is told to AO_LowPower at once", "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    chk_listening();
    chk_link(KRADIO_UP);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);

    chk_give(CHK_T0 + 30);
    TEST_ASSERT_TRUE_MESSAGE(fake_lowpower_wake_take(&w), "nothing else would wake the board for it");
    TEST_ASSERT_EQUAL_UINT64(khal_time_us() + (30U * CHK_US_PER_S), w.due_us);
    lp_awake(AO_Net);
    net_done();
}

/* test_net_check_dwell */
TEST_CASE("a radio wake with the check due is not done before the dwell, and is after it",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    int64_t passed = 0;

    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 60);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);

    chk_clock_to(CHK_T0 + 60);                /* the board slept until it was due */
    lp_radio(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(), "the dwell is on");
    secs((KODE_NET_CHECK_DWELL_MS / 1000U) - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(), "not before it has passed");
    TEST_ASSERT_EQUAL_UINT(0U, chk_told(&passed));

    secs(2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(), "once after it");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, chk_told(&passed), "and the scheduler is told");
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 60, passed);
    secs(5U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(), "only once");

    /* The round that ends the wake finds the time spent: it asks to wake for none of the check's. */
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE(fake_lowpower_wake_take(&w));
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() + (30U * CHK_US_PER_S) < w.due_us, "only the channel's");
    lp_awake(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, chk_told(&passed), "the check is not made twice");
    net_done();
}

/* test_net_check_no_link */
TEST_CASE("a due check with nothing to listen to is made at once", "[net_ao][lowpower]")
{
    int64_t passed = 0;

    /* The C5 link down. */
    chk_listening();
    chk_give(CHK_T0 + 60);
    chk_clock_to(CHK_T0 + 60);
    lp_radio(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(), "nothing to wake: done");
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 60, passed);
    lp_awake(AO_Net);
    net_done();

    /* Wi-Fi off: the link is up, nothing is joined. */
    chk_fresh();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 60);
    chk_clock_to(CHK_T0 + 60);
    lp_radio(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(), "no Wi-Fi: done");
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    lp_awake(AO_Net);
    net_done();

    /* The channel off, no client. */
    chk_online_unused();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 60);
    chk_clock_to(CHK_T0 + 60);
    lp_radio(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_radio_done_count(), "channel off, no client: done");
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 60, passed);
    lp_awake(AO_Net);
    net_done();
    fake_nvs_reset();
}

TEST_CASE("a radio wake that finds the check not due is done at once and tells nothing",
          "[net_ao][lowpower]")
{
    int64_t passed = 0;

    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + CHK_QUARTER_S);
    lp_radio(AO_Net);                         /* the channel's time, say, and not the check's */
    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_radio_done_count());
    TEST_ASSERT_EQUAL_UINT(0U, chk_told(&passed));
    lp_awake(AO_Net);
    net_done();
}

/** @brief Tell AO_Net the board is AWAKE once more, from AWAKE: a round abandoned, no new wake. */
static void chk_awake_again(void)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_AWAKE;
    e.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    e.prev  = (uint8_t)KODE_LP_AWAKE;
    net_dispatch(&e.super);
}

/* test_net_check_whole_wake_dwell: an entry and `mqtt` due in the same minute. */
TEST_CASE("a whole wake with the check due dwells, and a round is busy until the dwell ends",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    int64_t passed = 0;

    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 60);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);

    /* The scheduler's entry woke the board whole in the check's minute. */
    chk_clock_to(CHK_T0 + 60);
    lp_awake(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, chk_told(&passed), "the dwell is on: nothing slept is served yet");

    /* The rest the scheduler asks for is a round: refused while the dwell runs. */
    secs(1U);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(true, "check dwell");
    lp_expect_no_more();
    chk_awake_again();                        /* the round abandoned: no new wake */
    TEST_ASSERT_EQUAL_UINT(0U, chk_told(&passed));
    secs((KODE_NET_CHECK_DWELL_MS / 1000U) - 2U);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(true, "check dwell");
    chk_awake_again();
    TEST_ASSERT_EQUAL_UINT(0U, chk_told(&passed));

    /* The dwell over: the check is made, told once, and the retried round goes through. */
    secs(2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, chk_told(&passed), "made at the end of the dwell");
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 60, passed);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_radio_done_count(), "a whole wake says no radio done");
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE(fake_lowpower_wake_take(&w));
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() + (30U * CHK_US_PER_S) < w.due_us, "only the channel's");
    lp_awake(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, chk_told(&passed), "and not again");
    net_done();
}

TEST_CASE("a whole wake with nothing to listen to, or a round with the board awake, makes the check at once",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    int64_t passed = 0;

    /* A whole wake, the C5 link down: nothing to dwell for. */
    chk_listening();
    chk_give(CHK_T0 + 60);
    chk_clock_to(CHK_T0 + 60);
    lp_awake(AO_Net);
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 60, passed);
    lp_awake(AO_Net);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, chk_told(&passed), "and not again");
    net_done();

    /* AWAKE again with the board awake all along (a round abandoned): no dwell. */
    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 30);
    chk_clock_to(CHK_T0 + 60);
    chk_awake_again();
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 30, passed);
    net_done();

    /* The board was awake when it fell due; the next round finds it so. */
    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 30);
    chk_clock_to(CHK_T0 + 60);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    TEST_ASSERT_TRUE(fake_lowpower_wake_take(&w));
    TEST_ASSERT_TRUE_MESSAGE(khal_time_us() < w.due_us, "no wake for a time already served");
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 30, passed);
    lp_awake(AO_Net);
    net_done();
}

TEST_CASE("a round in a radio wake's dwell is busy, and the dwell still ends in one done",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    int64_t passed = 0;

    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 60);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);
    chk_clock_to(CHK_T0 + 60);

    lp_radio(AO_Net);
    secs(1U);
    lp_round(AO_Net, KODE_LP_ASLEEP);         /* somebody asked early */
    lp_expect_answer(true, "check dwell");
    TEST_ASSERT_EQUAL_UINT(0U, chk_told(&passed));
    lp_awake(AO_Net);                         /* the busy radio round made it a whole wake */
    secs(KODE_NET_CHECK_DWELL_MS / 1000U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, chk_told(&passed), "the dwell kept, and ended once");
    secs(5U);
    TEST_ASSERT_EQUAL_UINT(0U, chk_told(&passed));
    net_done();
}

/* test_net_check_off_round: a round to OFF closes the sessions the dwell listens to. */
TEST_CASE("a round to OFF is not refused for the check's dwell",
          "[net_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    int64_t passed = 0;

    chk_listening();
    chk_link(KRADIO_UP);
    chk_give(CHK_T0 + 60);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);
    chk_clock_to(CHK_T0 + 60);
    lp_awake(AO_Net);                         /* a whole wake: the dwell is on */

    secs(1U);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(true, "check dwell");
    chk_awake_again();

    lp_round(AO_Net, KODE_LP_OFF);
    pull_jobs();
    lp_expect_silence();                      /* the sessions close first */
    job_done(KDRV_NET_JOB_MQTT_STOP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    radio_drain();

    /* The dwell is not cut: it ends as it would, and the check is told once. */
    TEST_ASSERT_EQUAL_UINT(0U, chk_told(&passed));
    secs(KODE_NET_CHECK_DWELL_MS / 1000U);
    TEST_ASSERT_EQUAL_UINT(1U, chk_told(&passed));
    TEST_ASSERT_EQUAL_INT64(CHK_T0 + 60, passed);
    lp_awake(AO_Net);
    net_done();
}

/* ---- AO_Scheduler and AO_Net together -------------------------------------------
 * The scheduler is the real one here, and the two queues are carried by hand:
 * what one posts the other dispatches, until both are empty. */

/** @brief 18:45:00 on the anchor day: `mqtt`'s first minute after CHK_T0, and the entry's. */
#define CHK_T1845       (CHK_T0 + INT64_C(468))

/** @brief 19:00:00, `mqtt`'s next minute after 18:45. */
#define CHK_T1900       (CHK_T0 + INT64_C(1368))

/** @brief How far into 18:45 the board wakes, milliseconds. */
#define CHK_LATE_MS     400U

static unsigned s_pair_checks;   /**< Times AO_Scheduler handed AO_Net since the last look. */
static int64_t  s_pair_check;    /**< The newest of them.                                 */

/** @brief Carry events between AO_Scheduler and AO_Net until neither has any. */
static void pair_relay(void)
{
    bool moved = true;

    while ( moved )
    {
        const QEvt *e = NULL;

        moved = (qf_support_settle(AO_Scheduler) > 0U);
        while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
        {
            if ( (QSignal)NET_CHECK_AT_SIG == e->sig )
            {
                s_pair_checks++;
                s_pair_check = ((const NetCheckAtEvt *)e)->due_s;
            }
            QASM_DISPATCH(AO_Net, e, 0U);
            QF_gc(e);
            moved = true;
        }
        pull_jobs();
    }
}

/** @brief One second of both clocks and the ticks, then whatever the two posted. */
static void pair_secs(unsigned n)
{
    for ( unsigned i = 0U; i < n; ++i )
    {
        fake_time_wall_set(khal_time_wall_us() + CHK_US_PER_S);
        secs(1U);
        pair_relay();
    }
}

/** @brief The real AO_Scheduler, up, its first time handed to AO_Net. */
static void pair_sched_up(void)
{
    qf_support_reap_timers();
    Scheduler_ctor();
    qf_support_give_queue(AO_Scheduler);
    qf_support_give_queue(AO_Console);
    AO_Scheduler->prio  = KODE_PRIO_SCHEDULER;
    AO_Scheduler->pthre = KODE_PRIO_SCHEDULER;
    if ( qf_support_claim_ao(AO_Scheduler) )
    {
        QActive_register_(AO_Scheduler);
    }
    QASM_INIT(AO_Scheduler, (void *)0, 0U);
    s_pair_checks = 0U;
    s_pair_check  = -2;
    sched_system_up();
    pair_relay();
}

/** @brief Tell both the board woke whole, the scheduler first as its priority has it. */
static void pair_wake(kode_lp_cause_t cause, kode_lp_state_t prev)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_AWAKE;
    e.cause = (uint8_t)cause;
    e.prev  = (uint8_t)prev;
    QASM_DISPATCH(AO_Scheduler, &e.super, 0U);
    net_dispatch(&e.super);
    pair_relay();
}

/** @brief Leave the scheduler with no entry and no subscription, and both queues empty. */
static void pair_down(void)
{
    const QEvt *e = NULL;

    (void)sched_request_set(NULL, "beat", KODE_SCHED_WHEN_OFF, "");
    pair_relay();
    QActive_unsubscribeAll(AO_Scheduler);
    while ( xQueueReceive(AO_Scheduler->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    while ( xQueueReceive(AO_Console->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    (void)fake_lowpower_rest_count(NULL);
    (void)fake_lowpower_wake_take(&(LowPowerWakeAtEvt){ 0 });
    while ( fake_lowpower_ready_take(&(LowPowerReadyEvt){ 0 }) )
    {
    }
}

/* test_sched_net_check_with_entry: an entry and `mqtt` due in the same minute. */
TEST_CASE("an entry's whole wake in the check's minute dwells, and only then is the next time given",
          "[net_ao][lowpower][scheduler_ao]")
{
    LowPowerWakeAtEvt w;
    unsigned dwell_s = 0U;

    chk_listening();
    chk_link(KRADIO_UP);
    pair_sched_up();
    TEST_ASSERT_EQUAL_UINT(1U, s_pair_checks);
    TEST_ASSERT_EQUAL_INT64(CHK_T1845, s_pair_check);
    TEST_ASSERT_TRUE(sched_request_set(NULL, "beat", "45 18 * * *", "ping"));
    pair_relay();

    /* Both rest; the entry's wake time and the check's are one minute. */
    lp_round(AO_Scheduler, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    pull_jobs();
    lp_expect_answer(false, NULL);
    (void)fake_lowpower_wake_take(&w);

    /* The scheduler's timer wakes the board whole at 18:45. */
    fake_time_advance_ms((uint32_t)((CHK_T1845 - CHK_T0) * 1000) + CHK_LATE_MS);
    fake_time_wall_set(((uint64_t)CHK_T1845 * CHK_US_PER_S) + ((uint64_t)CHK_LATE_MS * 1000U));
    pair_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_pair_checks, "18:45 stays AO_Net's: no later time cuts its dwell");

    /* The entry runs and answers; the scheduler asks to rest. */
    pair_secs(1U);
    sched_line_done();
    pair_relay();
    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_rest_count(NULL));

    /* Its round meets the dwell, begun again each second until the dwell ends. */
    for ( ;; )
    {
        LowPowerReadyEvt a;

        lp_round(AO_Net, KODE_LP_ASLEEP);
        pull_jobs();
        TEST_ASSERT_TRUE(fake_lowpower_ready_take(&a));
        if ( !a.busy )
        {
            break;
        }
        TEST_ASSERT_EQUAL_STRING("check dwell", a.why);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_pair_checks, "the dwell runs: nothing new is handed");
        pair_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);   /* the round abandoned */
        pair_secs(1U);
        dwell_s++;
        TEST_ASSERT_TRUE_MESSAGE((KODE_NET_CHECK_DWELL_MS / 1000U) >= dwell_s, "the dwell never ended");
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE((KODE_NET_CHECK_DWELL_MS / 1000U) - 1U, dwell_s,
                                   "busy for the whole dwell (one second of it went by the line)");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_pair_checks, "the check made, the next time goes out");
    TEST_ASSERT_EQUAL_INT64(CHK_T1900, s_pair_check);

    /* A BOT wake later in a check's minute is the same: 19:00 stays until made. */
    fake_time_wall_set((uint64_t)CHK_T1900 * CHK_US_PER_S);
    pair_wake(KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(2U, s_pair_checks);
    lp_round(AO_Net, KODE_LP_ASLEEP);
    lp_expect_answer(true, "check dwell");
    pair_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    pair_secs(KODE_NET_CHECK_DWELL_MS / 1000U);
    TEST_ASSERT_EQUAL_UINT(3U, s_pair_checks);
    TEST_ASSERT_EQUAL_INT64(CHK_T1900 + INT64_C(900), s_pair_check);

    pair_down();
    net_done();
}
