/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_lowpower.c
 * @brief `sleep`, `power_off`, `ship_mode` and `lowpower_info`, and the facade's two calls.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "Mockkhal_power.h"

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kaos_wire.h"
#include "kcli_proto.h"
#include "kode_sys.h"
#include "kqp.h"
#include "kqp_bsp.h"
#include "kset.h"
#include "config_support.h"
#include "qf_support.h"

#if !CONFIG_KODE_LOWPOWER
#error "test_console_lowpower.c needs CONFIG_KODE_LOWPOWER: main/CMakeLists.txt spells it out"
#endif

/** @brief The command watchdog, in QP ticks (see test_console_power.c). */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)
#define ANY_BUDGET_TICKS   ((200000U * 100U) / 1000U)

/** @brief Empty an Active Object's queue, so a case sees only its own events. */
static void mailbox_empty(QActive *ao)
{
    const QEvt *e = NULL;

    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

static void console_up(void)
{
    qf_support_init();
    autotest_up();
    (void)ao_spy();
    ao_spy_drain();
    if ( qf_support_claim_ao(AO_Console) )
    {
        Console_ctor();
        qf_support_give_queue(AO_Console);
        AO_Console->prio  = KODE_PRIO_CONSOLE;
        AO_Console->pthre = KODE_PRIO_CONSOLE;
        QActive_register_(AO_Console);
        QASM_INIT(AO_Console, (void *)0, 0U);
        qf_support_background(AO_Console);
    }
    /* A command an earlier file left in flight is timed out here. */
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);
    mailbox_empty(AO_LowPower);
    mailbox_empty(AO_Power);
    fake_klog_reset();
    fake_neighbours_reset();
}

static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
}

static void type_line(const char *text)
{
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, text);
}

static bool replied(const char *needle)
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

/** @brief The first event in AO_LowPower's mailbox that is not the console's own input note. */
static const QEvt *posted(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)LOWPOWER_INPUT_SIG != e->sig )
        {
            return e;
        }
        QF_gc(e);
    }
    return NULL;
}

/** @brief Deliver the board's new state, as AO_LowPower publishes it. */
static void state_changed(kode_lp_state_t state)
{
    LowPowerStateEvt s;

    (void)memset(&s, 0, sizeof(s));
    s.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    s.state = (uint8_t)state;
    s.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    s.prev  = (uint8_t)KODE_LP_AWAKE;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
}

/** @brief Deliver a refusal meant for the port @p src, as AO_LowPower publishes it. */
static void refused_for(uint8_t src, const char *why)
{
    LowPowerRefusedEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_REFUSED_SIG);
    r.src   = src;
    (void)snprintf(r.why, sizeof(r.why), "%s", why);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
}

/** @brief Deliver a refusal meant for the serial port, where the cases type. */
static void refused(const char *why)
{
    refused_for((uint8_t)KODE_CONSOLE_SRC_SERIAL, why);
}

/** @brief What khal_power_stats() answers in a case. */
static khal_power_stats_t s_stats;

static void on_stats(khal_power_stats_t *out, int ncalls)
{
    (void)ncalls;
    *out = s_stats;
}

/* ---- sleep and power_off ------------------------------------------------------------- */

TEST_CASE("sleep asks AO_LowPower for ASLEEP and waits for the state", "[console_lowpower]")
{
    console_up();
    type_line("sleep");

    const LowPowerReqEvt *r = (const LowPowerReqEvt *)posted();
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "nothing was posted to AO_LowPower");
    TEST_ASSERT_EQUAL_INT(LOWPOWER_REQ_SIG, r->super.sig);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_ASLEEP, r->target);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, r->src);
    QF_gc(&r->super);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before the board changed state");

    state_changed(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("power_off asks AO_LowPower for OFF and closes on OFF", "[console_lowpower]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "power_off");

    const LowPowerReqEvt *r = (const LowPowerReqEvt *)posted();
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_OFF, r->target);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_CONSOLE_SRC_BLE, r->src,
        "the request must say which port asked");
    QF_gc(&r->super);

    state_changed(KODE_LP_OFF);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A wake is not an answer: the board went AWAKE, not where the verb asked. */
TEST_CASE("sleep is not closed by the board waking", "[console_lowpower]")
{
    console_up();
    type_line("sleep");
    QF_gc(posted());

    state_changed(KODE_LP_AWAKE);
    TEST_ASSERT_FALSE(replied(">ok"));
    refused("cable: external power");
    TEST_ASSERT_TRUE(replied(">err code=4"));
}

/* A radio wake is a state that is not where the verb asked either. */
TEST_CASE("sleep is not closed by a radio wake", "[console_lowpower]")
{
    console_up();
    type_line("sleep");
    QF_gc(posted());

    state_changed(KODE_LP_RADIO);
    TEST_ASSERT_FALSE(replied(">ok"));
    state_changed(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("sleep refused on a cable answers err with the reason", "[console_lowpower]")
{
    console_up();
    type_line("sleep");
    QF_gc(posted());

    refused("cable: external power");

    TEST_ASSERT_TRUE(replied(">err code=4 msg=\"cable: external power\""));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("power_off refused by a busy owner names who and why", "[console_lowpower]")
{
    console_up();
    type_line("power_off");
    QF_gc(posted());

    refused("storage: file open for write");

    TEST_ASSERT_TRUE(replied(">err code=4 msg=\"storage: file open for write\""));
}

/* AO_LowPower asks a request again for 10 s before it refuses it, so the
 * verb outlasts the console's usual 5 s. */
TEST_CASE("sleep waits past the usual deadline for a refusal that comes after the retries",
          "[console_lowpower]")
{
    console_up();
    type_line("sleep");
    QF_gc(posted());

    (void)qf_support_tick(AO_Console,
        ((KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS + KODE_LOWPOWER_ROUND_MS) * 100U) / 1000U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=6"), "the watchdog is longer than 5 s");

    refused("radio: no answer");
    TEST_ASSERT_TRUE(replied(">err code=4 msg=\"radio: no answer\""));
}

TEST_CASE("sleep still has a watchdog when AO_LowPower never answers", "[console_lowpower]")
{
    console_up();
    type_line("power_off");
    QF_gc(posted());

    (void)qf_support_tick(AO_Console,
        ((KODE_LOWPOWER_REQUEST_RETRY_WINDOW_MS + KODE_LOWPOWER_ROUND_MS
          + 5000U) * 100U) / 1000U + 1U);

    TEST_ASSERT_TRUE(replied(">err code=6"));
}

/* AO_LowPower publishes every refusal to every console: a serial `sleep`
 * waiting for its round must not be answered with the err of a BLE request
 * that hit the round and was turned away. */
TEST_CASE("a refusal meant for another port leaves the verb open", "[console_lowpower]")
{
    console_up();
    type_line("sleep");
    QF_gc(posted());

    refused_for((uint8_t)KODE_CONSOLE_SRC_BLE, "request: a round is running");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "another port's refusal answered this port's verb");

    state_changed(KODE_LP_ASLEEP);
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the verb's own outcome still closes it");
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("a refusal of a round nobody asked for closes no verb", "[console_lowpower]")
{
    console_up();
    type_line("sleep");
    QF_gc(posted());

    refused_for((uint8_t)KODE_LOG_SRC_NONE, "storage: file open for write");

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    refused("cable: external power");   /* leave the console idle */
}

TEST_CASE("the console is free again once the verb has been answered", "[console_lowpower]")
{
    console_up();
    type_line("sleep");
    QF_gc(posted());
    type_line("sleep");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4 msg=busy"),
        "a second line while the first waits is busy");

    refused("cable: external power");
    fake_klog_reset();
    type_line("sleep");
    TEST_ASSERT_FALSE(replied("msg=busy"));
    QF_gc(posted());
    refused("cable: external power");
}

TEST_CASE("a refusal that nobody asked for closes nothing", "[console_lowpower]")
{
    console_up();

    refused("cable: external power");
    state_changed(KODE_LP_ASLEEP);

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

/* ---- ship_mode ------------------------------------------------------------------------ */

TEST_CASE("ship_mode posts the ship request to AO_Power and answers ok", "[console_lowpower]")
{
    const QEvt *e = NULL;

    console_up();
    type_line("ship_mode");

    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Power->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(POWER_SHIP_REQ_SIG, e->sig);
    QF_gc(e);
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_NULL_MESSAGE(posted(), "ship mode is not a state of AO_LowPower");
}

/* ---- lowpower_info -------------------------------------------------------------------- */

/** @brief The number of lines of the capture that start with @p prefix. */
static unsigned lines_starting(const char *prefix)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( strncmp(fake_klog_line(i), prefix, strlen(prefix)) == 0 )
        {
            ++n;
        }
    }
    return n;
}

/** @brief The first captured line that starts with @p prefix, or NULL. */
static const char *line_starting(const char *prefix)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( strncmp(fake_klog_line(i), prefix, strlen(prefix)) == 0 )
        {
            return fake_klog_line(i);
        }
    }
    return NULL;
}

TEST_CASE("lowpower_info prints the state record, the counters record and ok",
          "[console_lowpower]")
{
    console_up();
    Mockkhal_power_Init();
    (void)memset(&s_stats, 0, sizeof(s_stats));
    s_stats.slept_us = 30ULL * 1000000ULL;
    s_stats.since_us = 120ULL * 1000000ULL;
    s_stats.sleeps   = 9U;
    khal_power_stats_Stub(&on_stats);
    fake_radio_set_sleep_stats(NULL);

    type_line("lowpower_info");

    TEST_ASSERT_EQUAL_UINT(1U, lines_starting(">|lowpower "));
    TEST_ASSERT_EQUAL_UINT(1U, lines_starting(">|lowpower_stats "));
    TEST_ASSERT_TRUE(replied(" state=awake "));
    TEST_ASSERT_TRUE(replied(" wakes_bot=0 wakes_ble=0 wakes_mqtt=0 wakes_timer=0 "));
    TEST_ASSERT_TRUE(replied(">|lowpower_stats slept_pct=25 sleeps=9 wakes_c5=0 wakes_tx=0 "));
    TEST_ASSERT_TRUE(replied(" knocks_max=0 deaf=0"));
    /* The values are AO_LowPower's, whatever an earlier file left in it:
     * test_lowpower_ao.c owns what they should be. Here they are present. */
    TEST_ASSERT_TRUE(replied(" sleep_after="));
    TEST_ASSERT_TRUE(replied(" off_after="));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the verb must end");
    Mockkhal_power_Destroy();
}

TEST_CASE("lowpower_info with nothing slept reads zero percent, not a division by zero",
          "[console_lowpower]")
{
    console_up();
    Mockkhal_power_Init();
    (void)memset(&s_stats, 0, sizeof(s_stats));
    khal_power_stats_Stub(&on_stats);

    type_line("lowpower_info");

    TEST_ASSERT_TRUE(replied(" slept_pct=0 sleeps=0 "));
    Mockkhal_power_Destroy();
}

TEST_CASE("lowpower_info ends its state record with the last refusal, quoted",
          "[console_lowpower]")
{
    console_up();
    Mockkhal_power_Init();
    (void)memset(&s_stats, 0, sizeof(s_stats));
    khal_power_stats_Stub(&on_stats);

    type_line("lowpower_info");

    /* Whatever the last round left, it is that record's last field, quoted
     * or a bare dash for none. */
    const char *rec = line_starting(">|lowpower ");
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_NOT_NULL(strstr(rec, " last_busy="));
    TEST_ASSERT_TRUE(replied(" last_busy=\"") || replied(" last_busy=-"));
    Mockkhal_power_Destroy();
}

/* The bytes of the first wake note arrive from the C5's sentinel; before one
 * has, the three counters have no value, and the grammar's word for that is `-`,
 * not a zero that reads as "no frames were queued". */
TEST_CASE("lowpower_info prints a dash for the wake note's counters before any note",
          "[console_lowpower]")
{
    RadioSleepNoteEvt note;

    console_up();
    Mockkhal_power_Init();
    (void)memset(&s_stats, 0, sizeof(s_stats));
    khal_power_stats_Stub(&on_stats);
    TEST_ASSERT_FALSE_MESSAGE(radio_sleep_last_note(&note),
        "an earlier file left a wake note in AO_Radio");

    type_line("lowpower_info");

    TEST_ASSERT_TRUE(replied(" tx_sta=- tx_serial=- tx_hci=- knocks_max="));
    Mockkhal_power_Destroy();
}

/* The widest this verb can print: 32-bit counters, a 16-bit knock count and
 * the longest refusal a participant can leave. A record that did not fit
 * would be cut through its closing quote by reply(), so the line must end on
 * the quote and each record must stay inside the line. */
TEST_CASE("lowpower_info keeps both records whole at the widest counters and a long refusal",
          "[console_lowpower]")
{
    static const char LONG_WHO[] = "an_unusually_long_participant_name";
    const kdrv_radio_sleep_stats_t widest = {
        .wakes_c5 = UINT32_MAX, .wakes_tx = UINT32_MAX, .deaf = UINT32_MAX,
        .stood_down = UINT32_MAX, .knocks = UINT32_MAX, .knocks_max = UINT16_MAX,
    };
    const QEvt *stray = NULL;
    const LowPowerRoundEvt *round;

    console_up();
    /* A real refusal, so lowpower_last_refusal() holds it: a participant that
     * answers busy to a round the serial port asked for. */
    qf_support_reap_timers();
    Mockkhal_power_Init();
    khal_power_init_IgnoreAndReturn(KODE_OK);
    khal_power_hold_Ignore();
    ao_spy_subscribe(LOWPOWER_ROUND_SIG);
    kqp_tick_start();
    fake_time_periodic_forget();
    LowPower_ctor();
    qf_support_give_queue(AO_LowPower);
    AO_LowPower->prio  = KODE_PRIO_LOWPOWER;
    AO_LowPower->pthre = KODE_PRIO_LOWPOWER;
    if ( qf_support_claim_ao(AO_LowPower) )
    {
        QActive_register_(AO_LowPower);
    }
    while ( xQueueReceive(AO_LowPower->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    QASM_INIT(AO_LowPower, (void *)0, 0U);
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT8(0U, lowpower_join(ao_spy(), LONG_WHO));
    lowpower_request(KODE_LP_ASLEEP, (uint8_t)KODE_CONSOLE_SRC_SERIAL);
    (void)qf_support_settle(AO_LowPower);
    round = (const LowPowerRoundEvt *)ao_spy_take();
    TEST_ASSERT_NOT_NULL(round);
    lowpower_ready(0U, round->round, true, "file open for write");
    (void)qf_support_settle(AO_LowPower);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(KODE_LOWPOWER_REFUSAL_MAX - 1U,
                                      (unsigned)strlen(lowpower_last_refusal()));

    (void)memset(&s_stats, 0, sizeof(s_stats));
    s_stats.slept_us = 119ULL * 1000000ULL;
    s_stats.since_us = 120ULL * 1000000ULL;
    s_stats.sleeps   = UINT32_MAX;
    khal_power_stats_Stub(&on_stats);
    fake_radio_set_sleep_stats(&widest);
    fake_klog_reset();

    type_line("lowpower_info");

    const char *state = line_starting(">|lowpower ");
    const char *stats = line_starting(">|lowpower_stats ");
    TEST_ASSERT_NOT_NULL(state);
    TEST_ASSERT_NOT_NULL(stats);
    TEST_ASSERT_LESS_THAN_UINT(KODE_LOG_LINE_MAX - 1U, (unsigned)strlen(state));
    TEST_ASSERT_LESS_THAN_UINT(KODE_LOG_LINE_MAX - 1U, (unsigned)strlen(stats));
    /* The refusal is whole: opening quote, the 47 characters, closing quote. */
    const char *busy = strstr(state, " last_busy=\"an_unusually_long_participant_name: ");
    TEST_ASSERT_NOT_NULL_MESSAGE(busy, "last_busy lost its text");
    TEST_ASSERT_EQUAL_UINT(KODE_LOWPOWER_REFUSAL_MAX + 1U,
                           (unsigned)strlen(busy + strlen(" last_busy=")) - 1U);
    /* The capture keeps the line's newline after the value. */
    TEST_ASSERT_EQUAL_CHAR_MESSAGE('"', state[strlen(state) - 2U],
        "the quote that closes last_busy was cut");
    TEST_ASSERT_TRUE(strstr(stats, " sleeps=4294967295 wakes_c5=4294967295 wakes_tx=4294967295") != NULL);
    TEST_ASSERT_TRUE(strstr(stats, " knocks_max=65535 deaf=4294967295") != NULL);
    TEST_ASSERT_TRUE(replied(">ok"));

    /* Put the shared pieces back for the file after this one. */
    fake_radio_set_sleep_stats(NULL);
    {
        const KodeConfigAnsEvt *a = config_ask(AO_LowPower,
            &(config_ask_t){ .key = KSET_KEY_SLEEP_AFTER, .set = true, .e = 5U });
        TEST_ASSERT_NOT_NULL(a);
    }
    fake_time_once_forget();
    QActive_unsubscribeAll(AO_LowPower);
    while ( xQueueReceive(AO_LowPower->eQueue, &stray, 0U) == pdTRUE )
    {
        QF_gc(stray);
    }
    kqp_tick_rest(false);
    ao_spy_drain();
    Mockkhal_power_Destroy();
}

/* ---- lowpower_info last=on -------------------------------------------------------------- */

/** @brief Tick the console until the verb ends, as many pacer gaps as it needs. */
static void tick_until_ok(void)
{
    for ( unsigned i = 0U; (20U > i) && !replied(">ok"); ++i )
    {
        (void)qf_support_tick(AO_Console, KQP_TICKS_PER_MS(11U));
    }
}

/** @brief Put a summary in the store with @p count samples whose current is their order. */
static void summary_seed(uint8_t count, uint8_t next)
{
    static lowpower_last_t last;

    (void)memset(&last, 0, sizeof(last));
    last.version    = (uint8_t)KODE_LOWPOWER_LAST_VERSION;
    last.end_cause  = (uint8_t)KODE_LP_CAUSE_BLE;
    last.slept_pct  = 97U;
    last.count      = count;
    last.next       = next;
    last.start_s    = 1800000000U;
    last.duration_s = 3600U;
    last.sleeps     = 410U;
    last.wakes_c5   = 12U;
    last.wakes_tx   = 30U;
    last.deaf       = 1U;
    for ( uint8_t i = 0U; KODE_LOWPOWER_LAST_SAMPLES > i; ++i )
    {
        last.samples[i].t_s = 60U * (i + 1U);
        last.samples[i].ma  = (int16_t)(-100 + i);
        last.samples[i].mv  = (uint16_t)(3900U - i);
    }
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_LP_LAST, &last, sizeof(last));
}

TEST_CASE("lowpower_info last=on says so when no rest was kept", "[console_lowpower]")
{
    console_up();
    fake_nvs_reset();

    type_line("lowpower_info last=on");

    TEST_ASSERT_TRUE(replied(">|lowpower_last present=no"));
    TEST_ASSERT_EQUAL_UINT(0U, lines_starting(">|lp_sample "));
    TEST_ASSERT_EQUAL_UINT(0U, lines_starting(">|lowpower "));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("lowpower_info last=on prints the header, then a record per sample", "[console_lowpower]")
{
    console_up();
    fake_nvs_reset();
    summary_seed(3U, 3U);

    type_line("lowpower_info last=on");

    TEST_ASSERT_TRUE(replied(">|lowpower_last start=1800000000 for_s=3600 slept_pct=97 sleeps=410"
                             " wakes_c5=12 wakes_tx=30 deaf=1 end=ble samples=3"));
    TEST_ASSERT_EQUAL_UINT(3U, lines_starting(">|lp_sample "));
    TEST_ASSERT_TRUE(replied(">|lp_sample t=60 ma=-100 mv=3900"));
    TEST_ASSERT_TRUE(replied(">|lp_sample t=180 ma=-98 mv=3898"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("lowpower_info last=on reads a wrapped ring from its oldest sample", "[console_lowpower]")
{
    console_up();
    fake_nvs_reset();
    summary_seed(KODE_LOWPOWER_LAST_SAMPLES, 5U);

    type_line("lowpower_info last=on");
    tick_until_ok();

    TEST_ASSERT_EQUAL_UINT(KODE_LOWPOWER_LAST_SAMPLES, lines_starting(">|lp_sample "));
    TEST_ASSERT_TRUE(replied(">|lp_sample t=360 ma=-95 mv=3895"));
    /* The first one printed is slot 5, the last is slot 4. */
    unsigned first = 0U;
    unsigned last  = 0U;
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( strncmp(fake_klog_line(i), ">|lp_sample ", strlen(">|lp_sample ")) == 0 )
        {
            if ( 0U == first )
            {
                first = i;
            }
            last = i;
        }
    }
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_line(first), "t=360 "));
    TEST_ASSERT_NOT_NULL(strstr(fake_klog_line(last), "t=300 "));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A record is one block of the large pool and this AO outranks the one that
 * drains it, so a listing of sixty-odd records goes out in slices. */
TEST_CASE("lowpower_info last=on goes out in slices and keeps the console busy until the last",
          "[console_lowpower]")
{
    console_up();
    fake_nvs_reset();
    summary_seed(KODE_LOWPOWER_LAST_SAMPLES, 0U);

    type_line("lowpower_info last=on");

    TEST_ASSERT_LESS_OR_EQUAL_UINT_MESSAGE(KAOS_WIRE_SLICE, fake_klog_count(),
        "more than a slice left in one step");
    TEST_ASSERT_FALSE(replied(">ok"));
    fake_klog_reset();
    type_line("lowpower_info");
    TEST_ASSERT_TRUE_MESSAGE(replied("msg=busy"), "a second line while the first lists");
    tick_until_ok();
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("lowpower_info last=on keeps every record inside the line at the widest",
          "[console_lowpower]")
{
    console_up();
    fake_nvs_reset();
    summary_seed(KODE_LOWPOWER_LAST_SAMPLES, 0U);
    {
        static lowpower_last_t wide;

        (void)lowpower_last_read(&wide);
        wide.start_s    = UINT32_MAX;
        wide.duration_s = UINT32_MAX;
        wide.slept_pct  = 100U;
        wide.sleeps     = UINT32_MAX;
        wide.wakes_c5   = UINT32_MAX;
        wide.wakes_tx   = UINT32_MAX;
        wide.deaf       = UINT32_MAX;
        wide.end_cause  = (uint8_t)KODE_LP_CAUSE_OFF_AFTER;
        wide.samples[0].t_s = UINT32_MAX;
        wide.samples[0].ma  = INT16_MIN;
        wide.samples[0].mv  = UINT16_MAX;
        fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_LP_LAST, &wide, sizeof(wide));
    }

    type_line("lowpower_info last=on");
    tick_until_ok();

    TEST_ASSERT_TRUE(replied(">|lowpower_last start=4294967295 for_s=4294967295 slept_pct=100"
                             " sleeps=4294967295 wakes_c5=4294967295 wakes_tx=4294967295"
                             " deaf=4294967295 end=off_after samples=60"));
    TEST_ASSERT_TRUE(replied(">|lp_sample t=4294967295 ma=-32768 mv=65535"));
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        TEST_ASSERT_LESS_THAN_UINT(KODE_LOG_LINE_MAX - 1U,
                                   (unsigned)strlen(fake_klog_line(i)));
    }
}

TEST_CASE("lowpower_info last=on names an unfinished rest and an unset clock",
          "[console_lowpower]")
{
    lowpower_last_t open;

    console_up();
    fake_nvs_reset();
    summary_seed(1U, 1U);
    (void)lowpower_last_read(&open);
    open.end_cause = (uint8_t)KODE_LOWPOWER_LAST_OPEN;
    open.start_s   = 0U;
    fake_nvs_seed(KSET_NAMESPACE, KSET_KEY_LP_LAST, &open, sizeof(open));

    type_line("lowpower_info last=on");

    TEST_ASSERT_TRUE(replied(">|lowpower_last start=- for_s=3600"));
    TEST_ASSERT_TRUE(replied(" end=asleep samples=1"));
}

TEST_CASE("lowpower_info last=off is the plain listing", "[console_lowpower]")
{
    console_up();
    Mockkhal_power_Init();
    (void)memset(&s_stats, 0, sizeof(s_stats));
    khal_power_stats_Stub(&on_stats);

    type_line("lowpower_info last=off");

    TEST_ASSERT_EQUAL_UINT(1U, lines_starting(">|lowpower "));
    TEST_ASSERT_EQUAL_UINT(0U, lines_starting(">|lowpower_last "));
    TEST_ASSERT_TRUE(replied(">ok"));
    Mockkhal_power_Destroy();
}

/* ---- The facade ----------------------------------------------------------------------- */

TEST_CASE("kode_sys_power_sleep requests ASLEEP and kode_sys_power_off requests OFF", "[console_lowpower]")
{
    console_up();

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_power_sleep());
    const LowPowerReqEvt *a = (const LowPowerReqEvt *)posted();
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_ASLEEP, a->target);
    QF_gc(&a->super);

    TEST_ASSERT_EQUAL_INT(KODE_OK, kode_sys_power_off());
    const LowPowerReqEvt *o = (const LowPowerReqEvt *)posted();
    TEST_ASSERT_NOT_NULL(o);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LP_OFF, o->target);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_LOWPOWER_SRC_APP, o->src);
    QF_gc(&o->super);
}
