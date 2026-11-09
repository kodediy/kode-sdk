/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_support.c
 * @brief See test_dot_support.h.
 */
#include "test_dot_support.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "Mockkdrv_station_i2c.h"

#include "ao_spy.h"
#include "fake_khal_worker.h"
#include "fake_station.h"
#include "fakes.h"
#include "kbsp_station.h"
#include "khal_nvs.h"
#include "kqp_config.h"
#include "probe_ao.h"
#include "qf_support.h"
#include "test_dot_priv.h"

/** @brief The worker's name, as test_dot_ao.c starts it. */
#define TD_WORKER        "test_dot_worker"
/** @brief Most dispatches td_settle() runs, so a loop in the AO cannot hang a case. */
#define TD_SETTLE_MAX    256U
/**
 * @brief AO_TestDot's level here, lent for its dispatches only (slot_borrow()).
 *        Its own, KODE_PRIO_TEST_DOT, is ao_spy's, and every other level is
 *        held in this binary; the probe's subscribes to nothing.
 */
#define TD_PRIO          ((uint8_t)PROBE_AO_PRIO)
/** @brief Most rounds of the end: each owner's answer and the close. */
#define TD_END_ROUNDS    8U

static char     s_lines[TD_LINES_MAX][TD_LINE_MAX]; /**< What the AO said.               */
static unsigned s_n;                                /**< How many lines.                 */
static bool     s_answer = true;                    /**< Screens are answered.           */
static uint8_t  s_seq;                              /**< The last command number given.  */
static bool     s_built;                            /**< It was built before.            */
static int      s_relays = -1;                      /**< The relay byte last written.    */
static unsigned s_writes;                           /**< Relay writes since the up.      */
static bool     s_station = true;                   /**< The station's chips answer.     */
static bool     s_encodes;                          /**< td_jpeg_encode() makes a JPEG.  */
static uint8_t  s_scr_kind;                         /**< The last screen asked: its kind. */
static uint8_t  s_scr_seq;                          /**< Its number.                     */
static bool     s_scr_seen;                         /**< One was asked since the up.     */

/** @brief The relay expander's transfers, recorded: the mock's callback. */
static kode_status_t relays_write(uint8_t addr, const uint8_t *tx, size_t n, int calls)
{
    (void)calls;
    if ( (KBSP_STATION_PCF8574_ADDR == addr) && (NULL != tx) && (1U == n) )
    {
        s_relays = (int)tx[0];
        s_writes++;
    }
    return KODE_OK;
}

/** @brief The station's chips answer as td_station_answers() last said. */
static bool station_probe(uint8_t addr, int calls)
{
    (void)addr;
    (void)calls;
    return s_station;
}

void td_station_answers(bool on)
{
    s_station = on;
}

void td_jpeg_encodes(bool on)
{
    s_encodes = on;
}

/**
 * @brief The worker's encodes, test_dot_jpeg.c's place: refused, so no blob is
 *        made of a photo, unless a case asked for encodes; then every one is
 *        the smallest JPEG there is, its start and end markers. The real one
 *        waits for AO_Display, which nobody dispatches while a job runs on the
 *        test's thread; AO_Display's own answer is test_display_mirror.c's.
 */
kode_status_t td_jpeg_encode(const uint16_t *src, uint32_t w, uint32_t h, uint32_t quality, uint8_t *dst,
                             uint32_t cap, uint32_t *len)
{
    static const uint8_t JPEG[] = { 0xFFU, 0xD8U, 0xFFU, 0xD9U };   /* SOI, EOI */

    (void)src;
    (void)w;
    (void)h;
    (void)quality;
    *len = 0U;
    if ( !s_encodes || (sizeof(JPEG) > cap) )
    {
        return KODE_ERROR;
    }
    (void)memcpy(dst, JPEG, sizeof(JPEG));
    *len = (uint32_t)sizeof(JPEG);
    return KODE_OK;
}

/**
 * @brief The station's I2C answered by callbacks, or the callbacks taken away.
 *
 * Callbacks and never Mock*_Init(): an Init frees the one pool every mock
 * chains its expectations from, under the files that set theirs once.
 */
static void mocks_set(bool on)
{
    if ( on )
    {
        kdrv_station_i2c_probe_Stub(&station_probe);
        kdrv_station_i2c_write_Stub(&relays_write);
    }
    else
    {
        kdrv_station_i2c_probe_Stub(NULL);
        kdrv_station_i2c_write_Stub(NULL);
    }
}

/** @brief Empty @p ao's mailbox, making one if it has none. */
static void mailbox_empty(QActive *ao)
{
    const QEvt *e = NULL;

    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/* QP keeps its registry of started Active Objects private; the support
 * reads and writes it here, as test_console_ps.c does, to lend AO_TestDot a
 * level. */
extern QActive *QActive_registry_[QF_MAX_ACTIVE + 1U];

/* QF's lists of armed time events, private too: one head per tick rate, its
 * next the counted list and its act the ones armed since the last tick. */
extern QTimeEvt QTimeEvt_timeEvtHead_[QF_MAX_TICK_RATE];

/** @brief Disarm every time event in list @p te that posts to AO_TestDot. */
static void timers_disarm_list(QTimeEvt *te)
{
    while ( NULL != te )
    {
        if ( (void *)AO_TestDot == te->act )
        {
            (void)QTimeEvt_disarm(te);
        }
        te = te->next;
    }
}

/**
 * @brief Every time event of AO_TestDot's disarmed, then taken out of QF's
 *        lists by one tick.
 *
 * The constructor zeroes the AO, its time events included: one QF still
 * links would lose its linked flag, be linked a second time at its next arm
 * and turn the list into a loop that the next tick never leaves.
 */
static void timers_unlink(void)
{
    timers_disarm_list(QTimeEvt_timeEvtHead_[0].next);
    timers_disarm_list((QTimeEvt *)QTimeEvt_timeEvtHead_[0].act);
    qf_support_reap_timers();
}

static QActive *s_slot_owner;  /**< Who held TD_PRIO's slot before the loan. */
static unsigned s_slot_depth;  /**< Nested loans: the outer one gives it back. */

/**
 * @brief Lend AO_TestDot the slot of TD_PRIO for one dispatch.
 *
 * Every level is held in this binary, by an owner some other file starts
 * whenever it first needs it, so AO_TestDot cannot keep one: a later
 * QActive_register_() would find it taken. It holds the probe's slot only
 * while it runs, which is when QP asks for it (QActive_subscribe() requires
 * the subscriber in its slot). The events it subscribes to are delivered by
 * hand here, never published to it, so slot_return() drops its subscriptions
 * with the slot: no other file's publish reaches the level afterwards.
 */
static void slot_borrow(void)
{
    if ( 0U == s_slot_depth )
    {
        s_slot_owner = QActive_registry_[TD_PRIO];
        QActive_registry_[TD_PRIO] = AO_TestDot;
    }
    s_slot_depth++;
}

/** @brief Give TD_PRIO's slot back, AO_TestDot's subscriptions dropped. */
static void slot_return(void)
{
    s_slot_depth--;
    if ( 0U == s_slot_depth )
    {
        QActive_unsubscribeAll(AO_TestDot);
        QActive_registry_[TD_PRIO] = s_slot_owner;
    }
}

/**
 * @brief The AO left with nothing armed and nothing queued: a timer left
 *        armed would post into a queue nobody dispatches while the files
 *        after this one tick, and the next build would zero it while QF
 *        links it.
 */
static void quiet(void)
{
    const QEvt *e = NULL;

    if ( !s_built )
    {
        return;
    }
    timers_unlink();
    while ( (NULL != AO_TestDot->eQueue) && (xQueueReceive(AO_TestDot->eQueue, &e, 0U) == pdTRUE) )
    {
        QF_gc(e);
    }
}

static bool s_autotest_relay;  /**< AO_Autotest was a relay before td_up() dropped it. */

/** @brief The mailboxes of the owners AO_TestDot asks, emptied. */
static void neighbours_empty(void)
{
    fake_neighbours_reset();
    mailbox_empty(AO_Autotest);
    mailbox_empty(AO_Buttons);
    mailbox_empty(AO_Touch);
    mailbox_empty(AO_Boot);
    qf_support_margin_queue(AO_Log);
    mailbox_empty(AO_Log);
    /* Started by a file before this one, AO_LedMgr hears every colour the
     * camera part sets and nobody here dispatches it: left alone, its queue
     * holds the pool's blocks until the records find none. */
    if ( NULL != AO_LedMgr->eQueue )
    {
        mailbox_empty(AO_LedMgr);
    }
}

static void up(bool locked)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_subscribe(TEST_DOT_SCREEN_REQ_SIG);
    ao_spy_subscribe(LED_RGB_SIG);

    quiet();
    ao_spy_drain();

    fake_khal_worker_reset();
    fake_station_reset();
    /* The station's own namespace only: the rest of the store holds what
     * other files seeded once for the whole binary. */
    (void)khal_nvs_erase_namespace(KODE_STATION_NVS_NS);
    fake_psram_reset();
    if ( locked )
    {
        uint8_t const on = (uint8_t)KODE_TEST_DOT_LOCK_ON;
        fake_nvs_seed(KODE_STATION_NVS_NS, KODE_TEST_DOT_LOCK_KEY, &on, 1U);
    }
    mocks_set(true);

    /* AO_Autotest is real in this binary and dispatched beside every subject
     * once its file is up; here its queue is the mailbox the rows are read
     * from, so a tick must not hand them to the real one. */
    if ( qf_support_relay_drop(AO_Autotest) )
    {
        s_autotest_relay = true;
    }
    neighbours_empty();

    TestDot_ctor();
    AO_TestDot->prio  = TD_PRIO;
    AO_TestDot->pthre = TD_PRIO;
    qf_support_margin_queue(AO_TestDot);
    kaos_test_dot_register();
    slot_borrow();
    QASM_INIT(AO_TestDot, (void *)0, 0U);
    slot_return();
    s_built = true;

    s_n      = 0U;
    s_answer = true;
    s_relays = -1;
    s_writes = 0U;
    s_station = true;
    s_encodes  = false;
    s_scr_seen = false;
    fake_klog_reset();
}

void td_up(void)
{
    up(false);
}

void td_up_locked(void)
{
    up(true);
}

void td_down(void)
{
    quiet();
    ao_spy_drain();
    neighbours_empty();
    fake_khal_worker_reset();
    mocks_set(false);
    if ( s_autotest_relay )
    {
        qf_support_relay(AO_Autotest);
        s_autotest_relay = false;
    }
}

void td_settle(void)
{
    const QEvt *e = NULL;
    unsigned n = 0U;

    slot_borrow();
    while ( (TD_SETTLE_MAX > n) && (xQueueReceive(AO_TestDot->eQueue, &e, 0U) == pdTRUE) )
    {
        QASM_DISPATCH(AO_TestDot, e, 0U);
        QF_gc(e);
        n++;
    }
    slot_return();
}

void td_dispatch(QEvt const *e)
{
    slot_borrow();
    QASM_DISPATCH(AO_TestDot, e, 0U);
    td_settle();
    slot_return();
}

void td_verb(TestDotReqEvt const *proto)
{
    TestDotReqEvt r = *proto;

    r.super = (QEvt)QEVT_INITIALIZER(TEST_DOT_REQ_SIG);
    r.who   = ao_spy();
    s_seq++;
    r.seq = s_seq;
    td_dispatch(&r.super);
}

void td_verb_plain(uint8_t verb)
{
    TestDotReqEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.verb = verb;
    td_verb(&r);
}

bool td_job(void)
{
    bool const ran = fake_khal_worker_run_next_for(TD_WORKER);

    td_settle();
    return ran;
}

unsigned td_jobs(void)
{
    return fake_khal_worker_pending_for(TD_WORKER);
}

unsigned td_jobs_run(unsigned max)
{
    unsigned n = 0U;

    while ( (n < max) && td_job() )
    {
        n++;
    }
    return n;
}

void td_wait_ms(uint32_t ms)
{
    slot_borrow();
    (void)qf_support_tick(AO_TestDot, QF_SUPPORT_STEPS(ms));
    td_settle();
    slot_return();
}

/** @brief Keep one line. */
static void line(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void line(const char *fmt, ...)
{
    va_list ap;

    if ( TD_LINES_MAX > s_n )
    {
        va_start(ap, fmt);
        (void)vsnprintf(s_lines[s_n], TD_LINE_MAX, fmt, ap);
        va_end(ap);
        s_n++;
    }
}

void td_take(void)
{
    bool more = true;

    while ( more )
    {
        const QEvt * const e = ao_spy_take();
        if ( NULL == e )
        {
            more = false;
        }
        else if ( (QSignal)TEST_DOT_REC_SIG == e->sig )
        {
            line("rec %s", ((const TestDotRecEvt *)e)->text);
        }
        else if ( (QSignal)TEST_DOT_ANS_SIG == e->sig )
        {
            const TestDotAnsEvt * const a = (const TestDotAnsEvt *)e;
            if ( 0U == a->code )
            {
                line(">ok");
            }
            else
            {
                line(">err code=%u msg=%s", (unsigned)a->code, a->msg);
            }
        }
        else if ( (QSignal)TEST_DOT_LEND_SIG == e->sig )
        {
            const TestDotLendEvt * const l = (const TestDotLendEvt *)e;
            line("blob %s %u", l->name, (unsigned)l->len);
            test_dot_blob_back();
        }
        else if ( (QSignal)TEST_DOT_SCREEN_REQ_SIG == e->sig )
        {
            const TestDotScreenEvt * const s = (const TestDotScreenEvt *)e;
            line("screen 0x%02x %c", (unsigned)s->kind, (int)s->arg[0]);
            s_scr_kind = s->kind;
            s_scr_seq  = s->seq;
            s_scr_seen = true;
            if ( s_answer && (TEST_DOT_SCR_END != s->kind) )
            {
                test_dot_screen_done(s->kind, s->seq);
            }
        }
        else if ( (QSignal)LED_RGB_SIG == e->sig )
        {
            const LedRgbEvt * const l = (const LedRgbEvt *)e;
            line("led %u,%u,%u", (unsigned)l->r, (unsigned)l->g, (unsigned)l->b);
        }
        else
        {
            /* Not the AO's. */
        }
    }
    ao_spy_release();
    td_settle();
}

void td_clear(void)
{
    s_n = 0U;
}

unsigned td_said_at(const char *needle)
{
    unsigned at = TD_LINES_MAX;

    for ( unsigned i = 0U; (i < s_n) && (TD_LINES_MAX == at); i++ )
    {
        if ( strstr(s_lines[i], needle) != NULL )
        {
            at = i;
        }
    }
    return at;
}

bool td_said(const char *needle)
{
    return td_said_at(needle) != TD_LINES_MAX;
}

unsigned td_said_n(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; i < s_n; i++ )
    {
        if ( strstr(s_lines[i], needle) != NULL )
        {
            n++;
        }
    }
    return n;
}

void td_dump(void)
{
    for ( unsigned i = 0U; i < s_n; i++ )
    {
        printf("  td[%u] %s\n", i, s_lines[i]);
    }
}

void td_screens_answer(bool on)
{
    s_answer = on;
}

bool td_screen_last(uint8_t *kind, uint8_t *seq)
{
    *kind = s_scr_kind;
    *seq  = s_scr_seq;
    return s_scr_seen;
}

unsigned td_posted(QActive *ao, enum_t sig)
{
    const QEvt *e = NULL;
    unsigned n = 0U;

    while ( (NULL != ao->eQueue) && (xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE) )
    {
        if ( (QSignal)sig == e->sig )
        {
            n++;
        }
        QF_gc(e);
    }
    return n;
}

int td_brightness(void)
{
    const QEvt *e = NULL;
    int pct = -1;

    while ( (NULL != AO_Display->eQueue) && (xQueueReceive(AO_Display->eQueue, &e, 0U) == pdTRUE) )
    {
        if ( (QSignal)DISPLAY_BRIGHTNESS_SIG == e->sig )
        {
            pct = (int)((const BrightnessEvt *)e)->percent;
        }
        QF_gc(e);
    }
    return pct;
}

bool td_rail_asked(bool *seen)
{
    const QEvt *e = NULL;
    bool on = false;

    *seen = false;
    while ( (NULL != AO_Radio->eQueue) && (xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE) )
    {
        if ( (QSignal)EXP_POWER_REQ_SIG == e->sig )
        {
            *seen = true;
            on    = ((const ExpPowerEvt *)e)->on;
        }
        QF_gc(e);
    }
    return on;
}

unsigned td_rows_asked(char *out, size_t cap)
{
    const QEvt *e = NULL;
    unsigned n = 0U;
    size_t at = 0U;

    out[0] = '\0';
    while ( (NULL != AO_Autotest->eQueue) && (xQueueReceive(AO_Autotest->eQueue, &e, 0U) == pdTRUE) )
    {
        if ( (QSignal)AUTOTEST_RUN_REQ_SIG == e->sig )
        {
            const char *sep = "";
            if ( 0U < n )
            {
                sep = " ";
            }
            int const w = snprintf(&out[at], cap - at, "%s%s", sep, ((const AutotestRunReqEvt *)e)->name);
            if ( (0 < w) && ((size_t)w < (cap - at)) )
            {
                at += (size_t)w;
            }
            n++;
        }
        QF_gc(e);
    }
    return n;
}

void td_row_answer(uint8_t state)
{
    AutotestBeginEvt b;
    AutotestRowEvt   r;
    AutotestEndEvt   d;

    (void)memset(&b, 0, sizeof(b));
    b.super  = (QEvt)QEVT_INITIALIZER(AUTOTEST_BEGIN_SIG);
    b.client = AUTOTEST_CLIENT_TEST_DOT;
    b.status = AUTOTEST_TAKEN;
    b.rows   = 1U;
    td_dispatch(&b.super);
    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(AUTOTEST_ROW_SIG);
    r.client = AUTOTEST_CLIENT_TEST_DOT;
    r.state  = AUTOTEST_ROW_RUNNING;
    td_dispatch(&r.super);
    r.state = state;
    td_dispatch(&r.super);
    (void)memset(&d, 0, sizeof(d));
    d.super  = (QEvt)QEVT_INITIALIZER(AUTOTEST_END_SIG);
    d.client = AUTOTEST_CLIENT_TEST_DOT;
    td_dispatch(&d.super);
}

void td_rail_answer(bool on, kode_status_t rc)
{
    ExpPowerDoneEvt d;

    (void)memset(&d, 0, sizeof(d));
    d.super = (QEvt)QEVT_INITIALIZER(EXP_POWER_SIG);
    d.on    = on;
    d.rc    = (uint8_t)rc;
    td_dispatch(&d.super);
}

void td_wake_ok(void)
{
    bool seen = false;

    (void)td_rail_asked(&seen);   /* the ask, read so the next case sees only its own */
    td_rail_answer(true, KODE_OK);
    (void)td_job();   /* the probe */
    (void)td_job();   /* the station */
    td_take();
}

void td_end(void)
{
    td_verb_plain((uint8_t)TEST_DOT_VERB_END);
    for ( unsigned i = 0U; TD_END_ROUNDS > i; i++ )
    {
        PowerBoostTestEvt b;
        LogDockEvt d;

        if ( td_posted(AO_Power, POWER_BOOST_TEST_REQ_SIG) > 0U )
        {
            (void)memset(&b, 0, sizeof(b));
            b.super  = (QEvt)QEVT_INITIALIZER(POWER_BOOST_TEST_SIG);
            b.status = (uint8_t)KODE_OK;
            b.boost  = false;
            td_dispatch(&b.super);
        }
        if ( td_posted(AO_Log, LOG_DOCK_REQ_SIG) > 0U )
        {
            (void)memset(&d, 0, sizeof(d));
            d.super  = (QEvt)QEVT_INITIALIZER(LOG_DOCK_SIG);
            d.status = (uint8_t)KODE_OK;
            d.lent   = false;
            td_dispatch(&d.super);
        }
        (void)td_jobs_run(TD_END_ROUNDS);
    }
    td_take();
}

int td_relays_last(void)
{
    return s_relays;
}

unsigned td_relays_writes(void)
{
    return s_writes;
}
