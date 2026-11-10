/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_frames.c
 * @brief AO_Radio's Wi-Fi frames door: the request, the C5's answers and batches, and what reaches the asker.
 *
 * The real AO_Radio and the real kdrv_frames; the C5 is the fake peer, which
 * records what the P4 sends and delivers what the C5 would say.
 */
#include <stdio.h>
#include <string.h>

#include "radio_harness.h"

#include "kdrv_frames.h"

/** @brief The channel the cases ask for. */
#define FR_CH            6U

/** @brief The channel a joined C5 reports. */
#define FR_JOINED_CH     11U

/** @brief The frames the cases put in a batch. */
#define FR_TWO           2U

/** @brief The strongest level a case's batch reports, dBm. */
#define FR_RSSI_MAX      (-40)

/** @brief Frames heard since START in a case's batch. */
#define FR_HEARD         9U

/** @brief Management and data frames in a case's batch. */
#define FR_MGMT          5U
#define FR_DATA          3U

/** @brief The longest message a case builds: a header and a full batch. */
#define FR_MSG_MAX       (sizeof(kode_hdr_t) + sizeof(kode_sniff_batch_t) \
                          + ((size_t)KODE_SNIFF_BATCH_MAX * sizeof(kode_sniff_rec_t)))

/** @brief Slots of a queue with no room left. */
#define FR_FULL          0U

/* ---- The C5's side ---------------------------------------------------------- */

/**
 * @brief The C5 sends one message on the KODE wire.
 * @param op     The op; with the event bit it is an event.
 * @param seq    Echoed from the request, or 0 for an event.
 * @param status KODE_ST_*.
 * @param pay    The payload, or NULL.
 * @param n      Its bytes.
 */
static void c5_sends(uint8_t op, uint8_t seq, uint8_t status, const void *pay, size_t n)
{
    uint8_t m[FR_MSG_MAX];
    kode_hdr_t const h = { KODE_WIRE_VERSION, KODE_SVC_SNIFF, op, seq, status, 0U, (uint16_t)n };

    (void)memcpy(m, &h, sizeof(h));
    if ( 0U < n )
    {
        (void)memcpy(&m[sizeof(h)], pay, n);
    }
    fake_peer_deliver(KODE_MSG_RSP, m, sizeof(h) + n);
}

/** @brief The last message the P4 sent, which must be the frames service's. */
static const kode_hdr_t *sent_sniff(void)
{
    size_t               n   = 0U;
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_last_sent(&n);

    TEST_ASSERT_TRUE_MESSAGE(sizeof(*req) <= n, "nothing was sent to the C5");
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_SNIFF, req->service);
    return req;
}

/** @brief Whether the P4 sent anything since fake_peer_reset(). */
static bool anything_sent(void)
{
    size_t n = 0U;

    (void)fake_peer_last_sent(&n);
    return 0U < n;
}

/** @brief The C5 answers the frames request in flight. */
static void c5_answers(uint8_t status, uint8_t on, uint8_t ch, uint8_t joined, uint8_t why)
{
    kode_sniff_state_t const s = { on, ch, joined, why };
    const kode_hdr_t *const  req = sent_sniff();

    c5_sends(req->op, req->seq, status, &s, sizeof(s));
}

/** @brief The C5 says a batch of @p count frames. */
static void c5_batch(uint8_t count)
{
    uint8_t              p[sizeof(kode_sniff_batch_t)
                           + ((size_t)KODE_SNIFF_BATCH_MAX * sizeof(kode_sniff_rec_t))];
    kode_sniff_batch_t const h = { FR_CH, count, (int8_t)FR_RSSI_MAX, 0U, FR_HEARD, FR_MGMT, FR_DATA };

    (void)memcpy(p, &h, sizeof(h));
    for ( uint8_t i = 0U; count > i; i++ )
    {
        kode_sniff_rec_t r;

        (void)memset(&r, 0, sizeof(r));
        r.rssi     = (int8_t)(-50 - (int)i);
        r.fc[0]    = 0x80U;
        r.fc[1]    = i;
        r.addr1[0] = (uint8_t)(0x10U + i);
        r.addr2[5] = (uint8_t)(0x20U + i);
        r.len      = (uint16_t)(100U + i);
        r.head[0]  = 0xA0U;
        r.head[KODE_SNIFF_HEAD_LEN - 1U] = i;
        (void)memcpy(&p[sizeof(h) + ((size_t)i * sizeof(r))], &r, sizeof(r));
    }
    c5_sends(KODE_SNIFF_EVT_BATCH, 0U, KODE_ST_OK, p,
             sizeof(h) + ((size_t)count * sizeof(kode_sniff_rec_t)));
}

/* ---- The fixture ------------------------------------------------------------ */

/** @brief Events the second asker's queue gave up, held until the case ends: the caller of ao_spy_other_take() recycles them. */
#define FR_OTHER_HELD_MAX  16U
static const QEvt *s_other_held[FR_OTHER_HELD_MAX];
static unsigned    s_other_held_n;

/** @brief Recycle what the second asker's reads kept. */
static void other_release(void)
{
    for ( unsigned i = 0U; s_other_held_n > i; ++i )
    {
        QF_gc(s_other_held[i]);
    }
    s_other_held_n = 0U;
}


/** @brief The link up, the peer's record empty, both askers' queues empty. */
static void frames_up(void)
{
    radio_fresh();
    kdrv_frames_forget();
    ao_spy_drain();
    ao_spy_other_drain();
    other_release();
    go_up();
    pump();
    fake_peer_reset();
}

/** @brief Leave the AO off and the askers' queues empty. */
static void frames_end(void)
{
    settle_to_off();
    storage_drain();
    net_drain();
    (void)boot_parked_taken();
    ao_spy_drain();
    ao_spy_other_drain();
    other_release();
    kdrv_frames_forget();
}

/** @brief Ask AO_Radio, and let it take the request. */
static void ask(QActive *who, bool on, uint8_t ch)
{
    TEST_ASSERT_TRUE(wifi_frames_request(who, on, ch));
    pump();
}

/** @brief The next event @p who holds, whatever it is, or NULL. */
static const QEvt *spy_next(QActive *who)
{
    const QEvt *e;

    if ( who == ao_spy() )
    {
        return ao_spy_take();
    }
    e = ao_spy_other_take();
    if ( (NULL != e) && (FR_OTHER_HELD_MAX > s_other_held_n) )
    {
        s_other_held[s_other_held_n++] = e;
    }
    return e;
}

/** @brief Check the next event of @p who is a state answer, and its fields. */
static void expect_state(QActive *who, bool on, uint8_t ch, bool joined, kwifi_frames_why_t why)
{
    const QEvt *const e = spy_next(who);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no answer reached the asker");
    TEST_ASSERT_EQUAL_INT((int)WIFI_FRAMES_STATE_SIG, (int)e->sig);

    WifiFramesStateEvt const *const s = (WifiFramesStateEvt const *)(const void *)e;

    TEST_ASSERT_EQUAL_INT(on, s->on);
    TEST_ASSERT_EQUAL_UINT8(ch, s->channel);
    TEST_ASSERT_EQUAL_INT(joined, s->joined);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)why, s->why);
}

/** @brief Check @p who holds nothing. */
static void expect_nothing(QActive *who)
{
    TEST_ASSERT_NULL_MESSAGE(spy_next(who), "the asker was told something");
}

/** @brief Start from @p who on @p ch and let the C5 say it listens. */
static void start_listening(QActive *who, uint8_t ch)
{
    ask(who, true, ch);
    c5_answers(KODE_ST_OK, 1U, ch, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    expect_state(who, true, ch, false, KWIFI_FRAMES_OK);
}

/** @brief Check the next event of the spy is a tick, and return it. */
static const WifiFramesTickEvt *take_tick(QActive *who)
{
    const QEvt *const e = spy_next(who);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no tick reached the asker");
    TEST_ASSERT_EQUAL_INT((int)WIFI_FRAMES_TICK_SIG, (int)e->sig);
    return (const WifiFramesTickEvt *)(const void *)e;
}

/** @brief Check the next event of the spy is a frame, and return it. */
static const WifiFrameEvt *take_frame(QActive *who)
{
    const QEvt *const e = spy_next(who);

    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no frame reached the asker");
    TEST_ASSERT_EQUAL_INT((int)WIFI_FRAME_SIG, (int)e->sig);
    return (const WifiFrameEvt *)(const void *)e;
}

/* ---- The door ---------------------------------------------------------------- */

TEST_CASE("a frames request with the link down is answered no_link and sends nothing",
          "[radio_frames]")
{
    radio_fresh();
    ao_spy_drain();
    fake_peer_reset();

    ask(ao_spy(), true, FR_CH);

    TEST_ASSERT_FALSE(anything_sent());
    expect_state(ao_spy(), false, 0U, false, KWIFI_FRAMES_NO_LINK);
    expect_nothing(ao_spy());
    frames_end();
}

TEST_CASE("a start is sent to the C5 and its answer reaches the asker", "[radio_frames]")
{
    frames_up();

    ask(ao_spy(), true, FR_CH);

    const kode_hdr_t *const req = sent_sniff();
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, req->op);
    TEST_ASSERT_EQUAL_UINT16(sizeof(kode_sniff_start_t), req->len);
    TEST_ASSERT_EQUAL_UINT8(FR_CH, ((const uint8_t *)&req[1])[0]);
    TEST_ASSERT_EQUAL_UINT8(0U, ((const uint8_t *)&req[1])[1]);
    expect_nothing(ao_spy());

    c5_answers(KODE_ST_OK, 1U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();

    expect_state(ao_spy(), true, FR_CH, false, KWIFI_FRAMES_OK);
    expect_nothing(ao_spy());
    frames_end();
}

TEST_CASE("an unanswered start is answered no_link and frees the door", "[radio_frames]")
{
    frames_up();

    ask(ao_spy(), true, FR_CH);
    expect_nothing(ao_spy());

    tick(QF_SUPPORT_STEPS(3000U) + 2U);
    pump();

    expect_state(ao_spy(), false, 0U, false, KWIFI_FRAMES_NO_LINK);

    /* The door is free: another asker starts at once. */
    fake_peer_reset();
    ask(ao_spy_other(), true, FR_CH);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, sent_sniff()->op);
    frames_end();
}

TEST_CASE("an event does not disturb a request in flight", "[radio_frames]")
{
    frames_up();

    /* The C5's answer and then a batch, both before AO_Radio looks at either:
     * the batch must not overwrite what the answer left for it. */
    ask(ao_spy(), true, FR_CH);
    c5_answers(KODE_ST_OK, 1U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    c5_batch(1U);
    pump();

    expect_state(ao_spy(), true, FR_CH, false, KWIFI_FRAMES_OK);
    (void)take_tick(ao_spy());
    (void)take_frame(ao_spy());

    /* A stop is waiting for its answer; a batch arrives; the deadline is
     * still armed and still fires. */
    ask(ao_spy(), false, 0U);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);
    c5_batch(0U);
    pump();
    (void)take_tick(ao_spy());
    expect_nothing(ao_spy());

    tick(QF_SUPPORT_STEPS(3000U) + 2U);
    pump();
    expect_state(ao_spy(), false, 0U, false, KWIFI_FRAMES_NO_LINK);
    frames_end();
}

TEST_CASE("a batch with no asker sends one stop, and no second until it is answered",
          "[radio_frames]")
{
    frames_up();

    c5_batch(FR_TWO);
    pump();

    TEST_ASSERT_TRUE(anything_sent());
    const kode_hdr_t *const stop = sent_sniff();
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, stop->op);
    TEST_ASSERT_EQUAL_UINT16(0U, stop->len);
    uint8_t const stop_seq = stop->seq;

    /* A second batch before the answer: no second stop. */
    fake_peer_reset();
    c5_batch(FR_TWO);
    pump();
    TEST_ASSERT_FALSE(anything_sent());

    /* The ring was emptied, and nobody was told. */
    kdrv_frame_t f;
    TEST_ASSERT_FALSE(kdrv_frames_next(&f));
    expect_nothing(ao_spy());

    /* Answered: a batch after that is quieted again. */
    kode_sniff_state_t const off = { 0U, FR_CH, 0U, KODE_SNIFF_WHY_NONE };
    c5_sends(KODE_SNIFF_STOP, stop_seq, KODE_ST_OK, &off, sizeof(off));
    pump();
    c5_batch(1U);
    pump();
    TEST_ASSERT_TRUE(anything_sent());
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);
    frames_end();
}

TEST_CASE("an answer from a C5 without the service is no_service", "[radio_frames]")
{
    frames_up();

    ask(ao_spy(), true, FR_CH);
    const kode_hdr_t *const req = sent_sniff();
    c5_sends(req->op, req->seq, KODE_ST_BAD_REQUEST, NULL, 0U);
    pump();

    expect_state(ao_spy(), false, 0U, false, KWIFI_FRAMES_NO_SERVICE);
    expect_nothing(ao_spy());
    frames_end();
}

TEST_CASE("each reason the C5 refuses with reaches the asker as its own", "[radio_frames]")
{
    static const struct {
        uint8_t            wire;     /**< KODE_SNIFF_WHY_*.      */
        uint8_t            channel;  /**< What the C5 reports.   */
        uint8_t            joined;   /**< What the C5 reports.   */
        kwifi_frames_why_t why;      /**< What the asker hears.  */
    } CASES[] = {
        { KODE_SNIFF_WHY_WIFI_OFF, 0U,            0U, KWIFI_FRAMES_WIFI_OFF },
        { KODE_SNIFF_WHY_JOINED,   FR_JOINED_CH,  1U, KWIFI_FRAMES_JOINED   },
        { KODE_SNIFF_WHY_CHANNEL,  FR_CH,         0U, KWIFI_FRAMES_CHANNEL  },
        { KODE_SNIFF_WHY_MEMORY,   FR_CH,         0U, KWIFI_FRAMES_MEMORY   },
    };

    frames_up();
    for ( unsigned i = 0U; (sizeof(CASES) / sizeof(CASES[0])) > i; ++i )
    {
        ask(ao_spy(), true, FR_CH);
        c5_answers(KODE_ST_REFUSED, 0U, CASES[i].channel, CASES[i].joined, CASES[i].wire);
        pump();
        expect_state(ao_spy(), false, CASES[i].channel, 0U != CASES[i].joined, CASES[i].why);
        expect_nothing(ao_spy());
        fake_peer_reset();
    }
    frames_end();
}

TEST_CASE("a channel past the last is refused by the door and sends nothing", "[radio_frames]")
{
    frames_up();

    ask(ao_spy(), true, KWIFI_FRAMES_CHANNEL_MAX + 1U);

    TEST_ASSERT_FALSE(anything_sent());
    expect_state(ao_spy(), false, 0U, false, KWIFI_FRAMES_CHANNEL);
    frames_end();
}

TEST_CASE("a second asker is busy and the first keeps its stream", "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy(), FR_CH);
    fake_peer_reset();

    ask(ao_spy_other(), true, FR_CH);
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_BUSY);
    TEST_ASSERT_FALSE(anything_sent());

    /* Nor can it stop what is not its own. */
    ask(ao_spy_other(), false, 0U);
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_BUSY);
    TEST_ASSERT_FALSE(anything_sent());

    c5_batch(1U);
    pump();
    (void)take_tick(ao_spy());
    (void)take_frame(ao_spy());
    expect_nothing(ao_spy_other());
    frames_end();
}

TEST_CASE("a tick and its frames are posted to the asker and published to nobody",
          "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy(), FR_CH);

    c5_batch(FR_TWO);
    pump();

    WifiFramesTickEvt const *const t = take_tick(ao_spy());
    TEST_ASSERT_EQUAL_UINT8(FR_CH, t->channel);
    TEST_ASSERT_FALSE(t->joined);
    TEST_ASSERT_EQUAL_INT8(FR_RSSI_MAX, t->rssi_max);
    TEST_ASSERT_EQUAL_UINT32(FR_HEARD, t->heard);
    TEST_ASSERT_EQUAL_UINT16(FR_MGMT, t->mgmt);
    TEST_ASSERT_EQUAL_UINT16(FR_DATA, t->data);
    TEST_ASSERT_EQUAL_UINT32(0U, t->dropped);

    for ( uint8_t i = 0U; FR_TWO > i; ++i )
    {
        WifiFrameEvt const *const f = take_frame(ao_spy());

        TEST_ASSERT_EQUAL_UINT8(FR_CH, f->channel);
        TEST_ASSERT_EQUAL_INT8((int8_t)(-50 - (int)i), f->rssi);
        TEST_ASSERT_EQUAL_UINT8(0x80U, f->fc[0]);
        TEST_ASSERT_EQUAL_UINT8(i, f->fc[1]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x10U + i), f->addr1[0]);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x20U + i), f->addr2[5]);
        TEST_ASSERT_EQUAL_UINT16((uint16_t)(100U + i), f->len);
        TEST_ASSERT_EQUAL_UINT8(0xA0U, f->head[0]);
        TEST_ASSERT_EQUAL_UINT8(i, f->head[KODE_SNIFF_HEAD_LEN - 1U]);
    }
    expect_nothing(ao_spy());
    /* Direct-post signals cannot be subscribed to (QP asserts), so what the
     * bus would have carried is read off the one AO that did not ask. */
    expect_nothing(ao_spy_other());
    frames_end();
}

TEST_CASE("an asker with no room for them is not asserted at, and the next tick counts what was lost",
          "[radio_frames]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    frames_up();
    start_listening(ao_spy_other(), FR_CH);

    for ( unsigned guard = 0U;
         (FR_FULL < uxQueueSpacesAvailable(ao_spy_other()->eQueue)) && (64U > guard);
         ++guard )
    {
        QACTIVE_POST(ao_spy_other(), &filler, NULL);
    }
    c5_batch(FR_TWO);
    pump();

    /* The queue is as it was left: full of the filler, nothing of the batch. */
    unsigned fillers = 0U;
    for ( const QEvt *e = ao_spy_other_take(); NULL != e; e = ao_spy_other_take() )
    {
        TEST_ASSERT_EQUAL_INT((int)WIFI_STATE_REQ_SIG, (int)e->sig);   /* static: nothing to recycle */
        ++fillers;
    }
    TEST_ASSERT_TRUE(0U < fillers);

    c5_batch(1U);
    pump();
    WifiFramesTickEvt const *const t = take_tick(ao_spy_other());
    TEST_ASSERT_EQUAL_UINT32(FR_TWO, t->dropped);
    (void)take_frame(ao_spy_other());
    frames_end();
}

TEST_CASE("a stop is answered off and frees the door", "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy(), FR_CH);
    fake_peer_reset();

    ask(ao_spy(), false, 0U);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);
    expect_nothing(ao_spy());

    c5_answers(KODE_ST_OK, 0U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    expect_state(ao_spy(), false, FR_CH, false, KWIFI_FRAMES_OK);

    /* Another asker may now start. */
    fake_peer_reset();
    ask(ao_spy_other(), true, FR_CH);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, sent_sniff()->op);
    frames_end();
}

TEST_CASE("a stop asked while the start waits goes out when the start is answered",
          "[radio_frames]")
{
    frames_up();

    ask(ao_spy(), true, FR_CH);
    ask(ao_spy(), false, 0U);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, sent_sniff()->op);   /* nothing new went out */

    c5_answers(KODE_ST_OK, 1U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    expect_state(ao_spy(), true, FR_CH, false, KWIFI_FRAMES_OK);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);

    c5_answers(KODE_ST_OK, 0U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    expect_state(ao_spy(), false, FR_CH, false, KWIFI_FRAMES_OK);
    frames_end();
}

TEST_CASE("a stop with nobody listening is answered off and sends nothing", "[radio_frames]")
{
    frames_up();

    ask(ao_spy(), false, 0U);

    TEST_ASSERT_FALSE(anything_sent());
    expect_state(ao_spy(), false, 0U, false, KWIFI_FRAMES_OK);
    frames_end();
}

TEST_CASE("a lost link tells the asker, empties the ring and frees the door", "[radio_frames]")
{
    uint8_t p[sizeof(kode_sniff_batch_t) + sizeof(kode_sniff_rec_t)];
    kode_sniff_batch_t const h = { FR_CH, 1U, FR_RSSI_MAX, 0U, 1U, 1U, 0U };
    kode_sniff_rec_t         r;

    frames_up();
    start_listening(ao_spy(), FR_CH);

    /* A frame waits in the ring when the link goes. */
    (void)memset(&r, 0, sizeof(r));
    (void)memcpy(p, &h, sizeof(h));
    (void)memcpy(&p[sizeof(h)], &r, sizeof(r));
    TEST_ASSERT_TRUE(kdrv_frames_feed(p, sizeof(p)));

    finish_lost(KODE_ERROR);

    expect_state(ao_spy(), false, 0U, false, KWIFI_FRAMES_NO_LINK);
    kdrv_frame_t f;
    kdrv_frames_tick_t t;
    TEST_ASSERT_FALSE(kdrv_frames_next(&f));
    TEST_ASSERT_FALSE(kdrv_frames_tick_take(&t));

    /* The door is free: after the link is back, another asker starts. */
    settle_to_off();
    fake_peer_reset();
    go_up();
    pump();
    fake_peer_reset();
    ask(ao_spy_other(), true, FR_CH);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, sent_sniff()->op);
    frames_end();
}

/* ---- The board's rest, Wi-Fi and the radio's other work ---------------------- */

/** @brief How many knocks wait in AO_Radio's queue; the queue is left as it was. */
static unsigned knocks_queued(void)
{
    unsigned    knocks = 0U;
    UBaseType_t n      = uxQueueMessagesWaiting(AO_Radio->eQueue);

    for ( UBaseType_t i = 0U; i < n; ++i )
    {
        const QEvt *e = NULL;

        TEST_ASSERT_TRUE(xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE);
        if ( (QSignal)RADIO_FRAMES_KNOCK_SIG == e->sig )
        {
            knocks += 1U;
        }
        TEST_ASSERT_TRUE(xQueueSend(AO_Radio->eQueue, &e, 0U) == pdTRUE);
    }
    return knocks;
}

/** @brief Leave AO_Radio's queue with no more free slots than a door's margin. */
static void radio_queue_to_margin(void)
{
    static const QEvt filler = QEVT_INITIALIZER(RADIO_STATE_REQ_SIG);

    for ( unsigned guard = 0U;
         (64U > guard) && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_POST_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
}

/** @brief Empty AO_Radio's queue without dispatching it. */
static void radio_queue_empty(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The board is awake again, and the sentinel a round started is stopped. */
static void frames_awake(void)
{
    lp_awake(AO_Radio);
    drain();
    while ( job_held() && (KDRV_RADIO_JOB_SLEEP == held_req()->type) )
    {
        finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    }
}

TEST_CASE("a rest round with a listener is busy, stops the stream and tells the asker; the next is ready",
          "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy_other(), FR_CH);
    fake_peer_reset();

    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();

    lp_expect_answer(true, "frames");
    lp_expect_no_more();
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);
    TEST_ASSERT_EQUAL_INT_MESSAGE(-1, running_job(), "the sentinel started under a listener");
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_RESTING);

    /* The STOP's answer is nobody's: the asker was told already. */
    c5_answers(KODE_ST_OK, 0U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    expect_nothing(ao_spy_other());

    /* The round's retry finds the door free. */
    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_SLEEP, running_job());
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    frames_awake();
    frames_end();
}

TEST_CASE("a start asked while the board rests is refused resting and sends nothing",
          "[radio_frames]")
{
    frames_up();
    lp_round(AO_Radio, KODE_LP_ASLEEP);
    drain();
    finish(KDRV_RADIO_JOB_SLEEP, KODE_OK);
    lp_expect_answer(false, NULL);
    fake_peer_reset();

    ask(ao_spy_other(), true, FR_CH);
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_RESTING);
    TEST_ASSERT_FALSE(anything_sent());

    /* Awake again: the door takes a start. */
    frames_awake();
    fake_peer_reset();
    ask(ao_spy_other(), true, FR_CH);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, sent_sniff()->op);
    frames_end();
}

TEST_CASE("Wi-Fi stopped under a listener ends its stream first and frees the door",
          "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy_other(), FR_CH);
    fake_peer_reset();

    dispatch(WIFI_STOP_REQ_SIG);
    pump();

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_STOP, running_job());
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_WIFI_OFF);
    finish(KDRV_RADIO_JOB_WIFI_STOP, KODE_OK);

    /* Free: another asker is taken. */
    c5_answers(KODE_ST_OK, 0U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    fake_peer_reset();
    ask(ao_spy(), true, FR_CH);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, sent_sniff()->op);
    frames_end();
}

TEST_CASE("a join, which may restart the station, ends a stream before it goes to the worker",
          "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy_other(), FR_CH);
    fake_peer_reset();

    TEST_ASSERT_TRUE(wifi_join_request("net", "pw12345678", false));
    pump();

    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_JOIN, running_job());
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_WIFI_OFF);
    finish(KDRV_RADIO_JOB_WIFI_JOIN, KODE_ERROR);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    frames_end();
}

TEST_CASE("batches that land before AO_Radio runs knock once, and all their frames are handed over",
          "[radio_frames]")
{
    /* The spy with the deep queue: a tick and six frames above the margin. */
    frames_up();
    start_listening(ao_spy(), FR_CH);

    c5_batch(FR_TWO);
    c5_batch(FR_TWO);
    c5_batch(FR_TWO);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, knocks_queued(), "one knock per batch");

    pump();
    (void)take_tick(ao_spy());
    for ( unsigned i = 0U; (3U * FR_TWO) > i; ++i )
    {
        (void)take_frame(ao_spy());
    }
    expect_nothing(ao_spy());
    ao_spy_release();   /* the spy holds what it took: give the pool back, as a dispatch would */

    /* Taken: the next batch knocks again. */
    c5_batch(1U);
    TEST_ASSERT_EQUAL_UINT(1U, knocks_queued());
    pump();
    (void)take_tick(ao_spy());
    (void)take_frame(ao_spy());
    frames_end();
}

TEST_CASE("a knock AO_Radio has no room for is not left pending: the next batch knocks",
          "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy(), FR_CH);

    radio_queue_to_margin();
    c5_batch(FR_TWO);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, knocks_queued(), "a knock went past the margin");
    radio_queue_empty();

    c5_batch(FR_TWO);
    TEST_ASSERT_EQUAL_UINT(1U, knocks_queued());
    pump();
    (void)take_tick(ao_spy());
    for ( unsigned i = 0U; (2U * FR_TWO) > i; ++i )
    {
        (void)take_frame(ao_spy());
    }
    expect_nothing(ao_spy());
    frames_end();
}

/** @brief Ask for a C5 update from the card: Radio_updating, waiting for the file. */
static void c5_update_begins(void)
{
    FlashReqEvt f;

    (void)memset(&f, 0, sizeof(f));
    f.super    = (QEvt)QEVT_INITIALIZER(FLASH_REQ_SIG);
    f.src_kind = (uint8_t)KFW_SRC_FS;
    f.dst_kind = (uint8_t)KFW_TARGET_C5;
    (void)snprintf(f.src, sizeof(f.src), "%s", "c5_slave.bin");
    dispatch_evt(&f.super);
    TEST_ASSERT_EQUAL_INT(-2, next_storage_op());   /* the open: the update has begun */
}

/** @brief The update's file was not there: back to Radio_up. */
static void c5_update_ends(void)
{
    StorageOpenRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(STORAGE_OPEN_RSP_SIG);
    r.status = (uint8_t)KODE_ERROR;
    dispatch_evt(&r.super);
    pump();
}

TEST_CASE("a C5 update ends a stream first, and a start during it is busy",
          "[radio_frames]")
{
    frames_up();
    start_listening(ao_spy_other(), FR_CH);
    fake_peer_reset();

    c5_update_begins();
    pump();
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_BUSY);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_STOP, sent_sniff()->op);
    c5_answers(KODE_ST_OK, 0U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    fake_peer_reset();

    ask(ao_spy_other(), true, FR_CH);
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_BUSY);
    TEST_ASSERT_FALSE(anything_sent());

    c5_update_ends();
    fake_peer_reset();
    ask(ao_spy_other(), true, FR_CH);
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, sent_sniff()->op);
    frames_end();
}

TEST_CASE("while the radio joins a network a start that names a channel is busy; channel 0 is taken",
          "[radio_frames]")
{
    kode_vault_list_t l;
    uint8_t           m[sizeof(kode_hdr_t) + sizeof(kode_vault_list_t)];

    radio_fresh();
    kdrv_frames_forget();
    ao_spy_drain();
    ao_spy_other_drain();
    other_release();
    go_up();

    /* The vault's LIST, asked at link up, answers with one name. */
    const kode_hdr_t *const req = (const kode_hdr_t *)fake_peer_last_sent(NULL);
    TEST_ASSERT_EQUAL_UINT8(KODE_SVC_VAULT, req->service);
    TEST_ASSERT_EQUAL_UINT8(KODE_VAULT_LIST, req->op);
    (void)memset(&l, 0, sizeof(l));
    l.count = 1U;
    (void)snprintf(l.ssid[0], sizeof(l.ssid[0]), "%s", "line-ap-0");
    {
        kode_hdr_t const h = { KODE_WIRE_VERSION, req->service, req->op, req->seq,
                               KODE_ST_OK, 0U, (uint16_t)sizeof(l) };
        (void)memcpy(m, &h, sizeof(h));
        (void)memcpy(&m[sizeof(h)], &l, sizeof(l));
        fake_peer_deliver(KODE_MSG_RSP, m, sizeof(m));
    }
    pump();
    dispatch(WIFI_START_REQ_SIG);
    pump();
    TEST_ASSERT_EQUAL_INT((int)KDRV_RADIO_JOB_WIFI_SCAN, running_job());
    fake_peer_reset();

    ask(ao_spy_other(), true, FR_CH);
    expect_state(ao_spy_other(), false, 0U, false, KWIFI_FRAMES_BUSY);
    TEST_ASSERT_FALSE(anything_sent());

    ask(ao_spy_other(), true, 0U);
    const kode_hdr_t *const start = sent_sniff();
    TEST_ASSERT_EQUAL_UINT8(KODE_SNIFF_START, start->op);
    TEST_ASSERT_EQUAL_UINT8(0U, ((const uint8_t *)&start[1])[0]);
    c5_answers(KODE_ST_OK, 1U, FR_CH, 0U, KODE_SNIFF_WHY_NONE);
    pump();
    expect_state(ao_spy_other(), true, FR_CH, false, KWIFI_FRAMES_OK);

    finish(KDRV_RADIO_JOB_WIFI_SCAN, KODE_ERROR);
    dispatch(WIFI_STOP_REQ_SIG);
    pump();
    while ( job_held() )
    {
        finish_whatever_runs();
    }
    frames_end();
}
