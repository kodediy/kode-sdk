/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_config.c
 * @brief `config`: the settings, asked of their owners one at a time.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kregistry.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kode_config.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks, as the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)
/* KODE_CONFIG_ANSWER_MS, in QP ticks, and one more. */
#define ANSWER_TICKS       (((KODE_CONFIG_ANSWER_MS * 100U) / 1000U) + 1U)

static const char *const MODES[] = { "off", "slow", "fast" };

/* Three rows, one of each spelling a record has: bare number, bare word,
 * quoted text. */
static const kode_config_t CONFIG[] = {
    { .name = "level", .type = KODE_TYPE_INT, .min = 1, .max = 9 },
    { .name = "mode",  .type = KODE_TYPE_ENUM, .enums = MODES, .n_enums = 3U },
    { .name = "name",  .type = KODE_TYPE_STRING },
};

/* ---- The harness ------------------------------------------------------------ */

static void console_up(void)
{
    qf_support_init();
    (void)ao_spy();
    ao_spy_drain();
    owners_up();
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
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);
    ao_spy_drain();
    fake_klog_reset();
    fake_neighbours_reset();
    /* This file's table alone, every time, answered by the spy. */
    kconfig_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_CONFIG_ADD(ao_spy(), KODE_CONFIG_MARGIN, CONFIG));
}

static void type_line(const char *text)
{
    ConsoleLineEvt e;
    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/**
 * @brief Empty AO_LowPower's mailbox. Every line posts it an input note, and
 *        AO_LowPower is not dispatched here: on a board it reads them.
 */
static void notes_drop(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_LowPower);
    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

static void ask(const char *text)
{
    fake_klog_reset();
    type_line(text);
    notes_drop();
}

static int line_of(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return (int)i;
        }
    }
    return -1;
}

static bool replied(const char *needle)
{
    return line_of(needle) >= 0;
}

/** @brief The request the console posted last, copied; the case fails if none. */
static KodeConfigReqEvt requested(void)
{
    KodeConfigReqEvt r;
    const QEvt *e = NULL;

    (void)memset(&r, 0, sizeof(r));
    while ( ao_spy_count() > 0U )
    {
        const QEvt * const n = ao_spy_take();
        if ( (QSignal)CONFIG_REQ_SIG == n->sig )
        {
            e = n;
        }
    }
    TEST_ASSERT_NOT_NULL_MESSAGE(e, "no CONFIG_REQ_SIG was posted");
    (void)memcpy(&r, e, sizeof(r));
    ao_spy_release();
    return r;
}

/** @brief Answer @p r the way its owner would. */
static void answer(const KodeConfigReqEvt *r, kode_status_t st, bool stored,
                   const char *value)
{
    KodeConfigAnsEvt a;
    (void)memset(&a, 0, sizeof(a));
    a.super  = (QEvt)QEVT_INITIALIZER(CONFIG_ANS_SIG);
    a.seq    = r->seq;
    a.status = (uint8_t)st;
    a.stored = stored;
    (void)snprintf(a.value, sizeof(a.value), "%s", value);
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/* ---- Listing ---------------------------------------------------------------- */

TEST_CASE("config lists every setting in order, one owner asked at a time",
          "[console_config]")
{
    console_up();
    ask("config");
    KodeConfigReqEvt r = requested();
    TEST_ASSERT_EQUAL_STRING("level", r.key);
    TEST_ASSERT_FALSE(r.set);
    answer(&r, KODE_OK, true, "4");
    r = requested();
    TEST_ASSERT_EQUAL_STRING("mode", r.key);
    answer(&r, KODE_OK, false, "slow");
    r = requested();
    TEST_ASSERT_EQUAL_STRING("name", r.key);
    TEST_ASSERT_FALSE(replied(">ok"));
    answer(&r, KODE_OK, true, "a b");
    TEST_ASSERT_TRUE(replied(">|config key=\"level\" value=4 stored=yes"));
    TEST_ASSERT_TRUE(replied(">|config key=\"mode\" value=slow stored=no"));
    TEST_ASSERT_TRUE(replied(">|config key=\"name\" value=\"a b\" stored=yes"));
    TEST_ASSERT_TRUE(line_of(">|config key=\"name\"") < line_of(">ok"));
}

TEST_CASE("an owner that does not answer leaves a dash, and the listing goes on",
          "[console_config]")
{
    console_up();
    ask("config");
    KodeConfigReqEvt r = requested();
    (void)qf_support_tick(AO_Console, ANSWER_TICKS);
    TEST_ASSERT_TRUE(replied(">|config key=\"level\" value=- stored=-"));
    /* Its late answer belongs to a row already printed. */
    answer(&r, KODE_OK, true, "4");
    TEST_ASSERT_FALSE(replied("value=4"));
    r = requested();
    TEST_ASSERT_EQUAL_STRING("mode", r.key);
    answer(&r, KODE_OK, false, "off");
    r = requested();
    answer(&r, KODE_OK, false, "x");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("config with no settings is a bare >ok", "[console_config]")
{
    console_up();
    kconfig_reset();
    ask("config");
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_CONFIG_ADD(ao_spy(), KODE_CONFIG_MARGIN, CONFIG));
}

/* ---- One key ---------------------------------------------------------------- */

TEST_CASE("config key= reads one setting", "[console_config]")
{
    console_up();
    ask("config key=\"mode\"");
    KodeConfigReqEvt r = requested();
    TEST_ASSERT_EQUAL_STRING("mode", r.key);
    TEST_ASSERT_FALSE(r.set);
    answer(&r, KODE_OK, true, "fast");
    TEST_ASSERT_TRUE(replied(">|config key=\"mode\" value=fast stored=yes"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a queue more than half full anywhere answers busy, and asks nobody",
          "[console_config]")
{
    /* A static event: the spy only holds it, nothing collects it. */
    static const QEvt filler = QEVT_INITIALIZER(GUI_DUMP_REQ_SIG);

    console_up();
    for ( unsigned i = 0U; ((AO_SPY_QUEUE_LEN / 2U) + 1U) > i; ++i )
    {
        QACTIVE_POST(ao_spy(), &filler, NULL);
    }
    TEST_ASSERT_FALSE(kqp_queues_half_free());

    ask("config key=\"mode\" value=fast");
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT((AO_SPY_QUEUE_LEN / 2U) + 1U, ao_spy_count());

    ao_spy_drain();
    TEST_ASSERT_TRUE(kqp_queues_half_free());
    ask("config key=\"mode\" value=fast");
    KodeConfigReqEvt r = requested();
    answer(&r, KODE_OK, true, "fast");
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("config key= value= hands the owner the value, typed", "[console_config]")
{
    console_up();
    ask("config key=\"level\" value=7");
    KodeConfigReqEvt r = requested();
    TEST_ASSERT_TRUE(r.set);
    TEST_ASSERT_EQUAL_INT32(7, r.i);
    answer(&r, KODE_OK, true, "7");
    TEST_ASSERT_TRUE(replied(">|config key=\"level\" value=7 stored=yes"));
    TEST_ASSERT_TRUE(replied(">ok"));

    ask("config key=\"mode\" value=fast");
    r = requested();
    TEST_ASSERT_EQUAL_UINT8(2U, r.e);
    answer(&r, KODE_OK, true, "fast");

    ask("config key=\"name\" value=\"UTC0\"");
    r = requested();
    TEST_ASSERT_EQUAL_STRING("UTC0", r.s);
    answer(&r, KODE_OK, true, "UTC0");
    TEST_ASSERT_TRUE(replied(">|config key=\"name\" value=\"UTC0\" stored=yes"));
}

TEST_CASE("an owner's refusal is code 5 msg=value, with no record", "[console_config]")
{
    console_up();
    ask("config key=\"name\" value=\"nope\"");
    KodeConfigReqEvt r = requested();
    answer(&r, KODE_DENIED, false, "UTC0");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=value"));
    TEST_ASSERT_FALSE(replied(">|config"));
}

TEST_CASE("an unknown key, a value with no key, or a value of the wrong kind asks nobody",
          "[console_config]")
{
    console_up();
    ask("config key=\"nosuch\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=key"));
    ask("config value=4");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=key"));
    ask("config key=\"level\" value=\"4\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    ask("config key=\"level\" value=12");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    ask("config key=\"mode\" value=medium");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    ask("config key=\"name\" value=bare");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

TEST_CASE("a single key nobody answers times out as code 6", "[console_config]")
{
    console_up();
    ask("config key=\"level\"");
    (void)requested();
    (void)qf_support_tick(AO_Console, ANSWER_TICKS);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=config"));
}

/* ---- Settings of an owner the app did not link -------------------------------
 * Each owner adds its own settings once it has started, with itself as their
 * owner. An owner the app did not link never starts, so its keys are no
 * settings in that firmware: the listing has nothing to wait for, and a
 * direct read is the unknown key's answer. */

TEST_CASE("an absent owner's key is unknown, and nobody is asked", "[console_config]")
{
    console_up();
    ask("config key=\"vol\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=key"));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* A register adds verbs and rows, which need no queue: the settings wait for
 * the start, whose queue their requests are posted to. A case can therefore
 * register any owner without starting it. */
TEST_CASE("an owner's register adds no settings, and the same table twice adds "
          "nothing", "[console_config]")
{
    console_up();
    kaos_audio_register();
    TEST_ASSERT_NULL(kconfig_find("vol"));
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_CONFIG_ADD(ao_spy(), KODE_CONFIG_MARGIN, CONFIG));
    TEST_ASSERT_EQUAL_size_t(3U, kconfig_count());
}

/* A zone is a string of at most 32 characters, the most a setting's value
 * holds: a longer one is the console's to refuse, as every string past that
 * length is, before any owner is asked. What reaches AO_Boot it judges itself
 * (test_boot_ao.c), and its refusal is code 5. */
TEST_CASE("a zone of 33 characters is refused by the console, and nobody is asked",
          "[console_config]")
{
    static const kode_config_t ZONE[] = {
        { .name = "zone", .type = KODE_TYPE_STRING },
    };

    console_up();
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_CONFIG_ADD(ao_spy(), KODE_CONFIG_MARGIN, ZONE));

    ask("config key=\"zone\" value=\"CET-1CEST,M3.5.0,M10.5.0/3,ABCDEF\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    TEST_ASSERT_EQUAL_UINT(0U, ao_spy_count());
}

/* ---- A burst to an owner that does not drain ---------------------------------
 * Each request is posted with the margin its owner registered, and an owner
 * busy elsewhere reads none of them. Every line the owner never answers times
 * out, the next one is posted behind it, and once the margin is reached the
 * console answers busy: the queue never fills, so the post never asserts. */

/** @brief Lines in the burst: more than the spy's queue holds. */
#define BURST_LINES  40U
/** @brief Requests the owner's queue takes before its margin: fewer than half
 *         of it, so the margin refuses before kqp_queues_half_free() does. */
#define BURST_ROOM   12U

TEST_CASE("a burst of sets to an owner that does not drain answers busy at its "
          "margin, and never asserts", "[console_config]")
{
    unsigned posted = 0U;
    unsigned busy   = 0U;

    console_up();
    kconfig_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK,
                          KODE_CONFIG_ADD(ao_spy(), AO_SPY_QUEUE_LEN - BURST_ROOM, CONFIG));
    for ( unsigned i = 0U; BURST_LINES > i; ++i )
    {
        ask("config key=\"level\" value=5");
        if ( replied(">err code=4 msg=busy") )
        {
            busy += 1U;
        }
        else
        {
            posted += 1U;
            (void)qf_support_tick(AO_Console, ANSWER_TICKS);
            TEST_ASSERT_TRUE(replied(">err code=6 msg=config"));
        }
    }
    TEST_ASSERT_EQUAL_UINT(BURST_ROOM, posted);
    TEST_ASSERT_EQUAL_UINT(BURST_LINES - BURST_ROOM, busy);
    TEST_ASSERT_EQUAL_UINT(BURST_ROOM, ao_spy_count());
    TEST_ASSERT_TRUE(kqp_queues_half_free());

    /* The owner reads its queue: the next line is posted again. */
    ao_spy_drain();
    ask("config key=\"level\" value=5");
    KodeConfigReqEvt r = requested();
    answer(&r, KODE_OK, true, "5");
    TEST_ASSERT_TRUE(replied(">ok"));
}
