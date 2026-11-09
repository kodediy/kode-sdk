/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_cron.c
 * @brief `cron`, and the console source SCHED the scheduler's entries run as.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "autotest_support.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kode.h"
#include "kqp.h"
#include "khal_time.h"
#include "qf_support.h"

#if !CONFIG_KODE_SCHEDULER
#error "test_console_cron.c needs CONFIG_KODE_SCHEDULER: main/CMakeLists.txt spells it out"
#endif
#if !CONFIG_KODE_LOWPOWER
#error "test_console_cron.c needs CONFIG_KODE_LOWPOWER: main/CMakeLists.txt spells it out"
#endif

/** @brief The command watchdog, in QP ticks, and a little over it. */
#define CMD_TIMEOUT_TICKS  (((5000U + 100U) * 100U) / 1000U)
#define ANY_BUDGET_TICKS   ((200000U * 100U) / 1000U)

/** @brief Friday 2026-10-30 18:40:00 UTC, Unix seconds: an entry's next minute in a record. */
#define NEXT_1840          INT64_C(1793385600)
/** @brief 18:45:00, the next minute of `mqtt` with its default. */
#define NEXT_1845          INT64_C(1793385900)
/** @brief A line far longer than a record can show next to its other fields. */
#define LONG_CMD_CHARS     200U
/** @brief The longest a record's text can be, newline included. */
#define RECORD_MAX         (KODE_LOG_LINE_MAX - 1U)

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
    mailbox_empty(AO_Scheduler);
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief The sequence the next line typed from NET carries (ConsoleLineEvt::seq). */
static uint8_t s_line_seq;

static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    e.seq   = s_line_seq;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
}

static void type_line(const char *text)
{
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, text);
}

/** @brief Whether some line klog was given contains @p needle. */
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

/** @brief Whether some line klog was given is exactly @p text and its newline. */
static bool replied_exactly(const char *text)
{
    char want[KODE_LOG_LINE_MAX];

    (void)snprintf(want, sizeof(want), "%s\n", text);
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strcmp(l, want) == 0) )
        {
            return true;
        }
    }
    return false;
}

/** @brief Whether any line klog was given is a reply (starts with '>'): one that reached a port. */
static bool a_reply_reached_a_port(void)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && ('>' == l[0]) )
        {
            return true;
        }
    }
    return false;
}

/** @brief The source klog was given with the first line that contains @p needle. */
static unsigned char src_of(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return fake_klog_src(i);
        }
    }
    return (unsigned char)KODE_LOG_SRC_NONE;
}

/** @brief The newest event of signal @p sig in AO_Scheduler's mailbox, or NULL. Others are released. */
static const QEvt *sched_posted(QSignal sig)
{
    const QEvt *e   = NULL;
    const QEvt *got = NULL;

    while ( xQueueReceive(AO_Scheduler->eQueue, &e, 0U) == pdTRUE )
    {
        if ( e->sig == sig )
        {
            if ( NULL != got )
            {
                QF_gc(got);
            }
            got = e;
        }
        else
        {
            QF_gc(e);
        }
    }
    return got;
}

/** @brief How many SCHED_LINE_DONE_SIG AO_Scheduler's mailbox holds; empties it. */
static unsigned line_done_count(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Scheduler->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)SCHED_LINE_DONE_SIG == e->sig )
        {
            ++n;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief Deliver one entry, as AO_Scheduler answers a request. */
static void answer_entry(uint8_t owner, bool stored, int64_t next_s, const char *name,
                         const char *when, const char *cmd)
{
    SchedEntryEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super  = (QEvt)QEVT_INITIALIZER(SCHED_ENTRY_SIG);
    e.owner  = owner;
    e.stored = stored;
    e.next_s = next_s;
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    (void)snprintf(e.when, sizeof(e.when), "%s", when);
    (void)snprintf(e.cmd, sizeof(e.cmd), "%s", cmd);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Deliver the end of an answer. */
static void answer_end(kode_status_t st, const char *why)
{
    SchedEndEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(SCHED_END_SIG);
    e.st    = (uint8_t)st;
    (void)snprintf(e.why, sizeof(e.why), "%s", why);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Deliver a refusal: the end alone, as AO_Scheduler answers one. */
static void answer_refused(const char *why)
{
    answer_end(KODE_DENIED, why);
}

/** @brief Deliver the refusal @p why to a `cron` set typed on the cable. */
static void set_refused(const char *why)
{
    type_line("cron name=\"x\" when=\"* * * * *\" cmd=\"ping\"");
    QF_gc(sched_posted((QSignal)SCHED_SET_REQ_SIG));
    answer_refused(why);
}

/* ---- The records ----------------------------------------------------------------- */

TEST_CASE("cron sets an entry: the request carries the three texts and the answer is a record",
          "[console_cron]")
{
    console_up();
    type_line("cron name=\"riego\" when=\"0 7 * * *\" cmd=\"ping\"");

    const SchedSetReqEvt *r = (const SchedSetReqEvt *)sched_posted((QSignal)SCHED_SET_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "nothing was posted to AO_Scheduler");
    TEST_ASSERT_EQUAL_PTR(AO_Console, r->who);
    TEST_ASSERT_EQUAL_STRING("riego", r->name);
    TEST_ASSERT_EQUAL_STRING("0 7 * * *", r->when);
    TEST_ASSERT_EQUAL_STRING("ping", r->cmd);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SCHED_FLAW_NONE, r->flaw);
    QF_gc(&r->super);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Scheduler answered");

    answer_entry((uint8_t)SCHED_BY_CONSOLE, true, NEXT_1840, "riego", "0 7 * * *", "ping");
    answer_end(KODE_OK, "");

    TEST_ASSERT_TRUE(replied_exactly(
        ">|cron name=\"riego\" when=\"0 7 * * *\" cmd=\"ping\" owner=console"
        " next=1793385600 stored=yes"));
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());
}

TEST_CASE("cron lists every entry in the order they come, mqtt included, then ends", "[console_cron]")
{
    console_up();
    type_line("cron");

    const SchedNameReqEvt *r = (const SchedNameReqEvt *)sched_posted((QSignal)SCHED_LIST_REQ_SIG);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_PTR(AO_Console, r->who);
    TEST_ASSERT_FALSE_MESSAGE(r->has_name, "a bare cron lists them all");
    QF_gc(&r->super);

    answer_entry((uint8_t)SCHED_BY_BUILTIN, false, NEXT_1845, "mqtt", "*/15 * * * *", "");
    answer_entry((uint8_t)SCHED_BY_CONSOLE, true, NEXT_1840, "riego", "0 7 * * *", "ping");
    answer_entry((uint8_t)SCHED_BY_CODE, false, -1, "app.tick", "* * * * *", "");
    answer_end(KODE_OK, "");

    TEST_ASSERT_TRUE(replied_exactly(
        ">|cron name=\"mqtt\" when=\"*/15 * * * *\" cmd=- owner=builtin next=1793385900"
        " stored=no"));
    TEST_ASSERT_TRUE(replied_exactly(
        ">|cron name=\"riego\" when=\"0 7 * * *\" cmd=\"ping\" owner=console next=1793385600"
        " stored=yes"));
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(
        ">|cron name=\"app.tick\" when=\"* * * * *\" cmd=- owner=code next=- stored=no"),
        "no next minute is a bare dash, no line is a bare dash");
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());
}

TEST_CASE("cron name shows one entry", "[console_cron]")
{
    console_up();
    type_line("cron name=\"riego\"");

    const SchedNameReqEvt *r = (const SchedNameReqEvt *)sched_posted((QSignal)SCHED_LIST_REQ_SIG);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_TRUE(r->has_name);
    TEST_ASSERT_EQUAL_STRING("riego", r->name);
    QF_gc(&r->super);

    answer_entry((uint8_t)SCHED_BY_CONSOLE, true, NEXT_1840, "riego", "0 7 * * *", "ping");
    answer_end(KODE_OK, "");
    TEST_ASSERT_TRUE(replied("owner=console"));
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());
}

TEST_CASE("cron when=off removes an entry and mqtt takes a time without a line", "[console_cron]")
{
    console_up();
    type_line("cron name=\"riego\" when=off");

    const SchedSetReqEvt *r = (const SchedSetReqEvt *)sched_posted((QSignal)SCHED_SET_REQ_SIG);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("riego", r->name);
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_WHEN_OFF, r->when);
    TEST_ASSERT_EQUAL_STRING("", r->cmd);
    QF_gc(&r->super);
    TEST_ASSERT_FALSE(replied(">ok"));
    answer_end(KODE_OK, "");
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());

    type_line("cron name=\"mqtt\" when=\"*/5 * * * *\"");
    r = (const SchedSetReqEvt *)sched_posted((QSignal)SCHED_SET_REQ_SIG);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_STRING("mqtt", r->name);
    TEST_ASSERT_EQUAL_STRING("*/5 * * * *", r->when);
    TEST_ASSERT_EQUAL_STRING("", r->cmd);
    QF_gc(&r->super);
    answer_entry((uint8_t)SCHED_BY_BUILTIN, true, NEXT_1845, "mqtt", "*/5 * * * *", "");
    answer_end(KODE_OK, "");
    TEST_ASSERT_TRUE(replied("name=\"mqtt\" when=\"*/5 * * * *\" cmd=- owner=builtin"));
}

/* A line longer than the record has room for is cut and says so, and the
 * fields after it are still there: they are what the reader keys on. */
TEST_CASE("a record whose line does not fit is cut, with a warning, and keeps its other fields",
          "[console_cron]")
{
    char cmd[LONG_CMD_CHARS + 1U];

    console_up();
    (void)memset(cmd, 'a', LONG_CMD_CHARS);
    cmd[LONG_CMD_CHARS] = '\0';
    type_line("cron name=\"riego\"");
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));

    answer_entry((uint8_t)SCHED_BY_CONSOLE, true, NEXT_1840, "riego", "0 7 * * *", cmd);
    answer_end(KODE_OK, "");

    TEST_ASSERT_TRUE(replied("cmd: cut to fit one line"));
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);

        if ( (NULL != l) && (strncmp(l, ">|cron ", 7U) == 0) )
        {
            TEST_ASSERT_TRUE_MESSAGE(strlen(l) <= RECORD_MAX, "the record is cut by the line");
            TEST_ASSERT_NOT_NULL(strstr(l, "cmd=\"aaaa"));
            TEST_ASSERT_NOT_NULL_MESSAGE(strstr(l, "\" owner=console next=1793385600 stored=yes\n"),
                "the closing quote and the fields after it survive");
            return;
        }
    }
    TEST_FAIL_MESSAGE("no record");
}

/* ---- The refusals ----------------------------------------------------------------- */

TEST_CASE("a refused expression is code 3 when, with the reason in a NOTICE", "[console_cron]")
{
    static const char *const reasons[] = { "fields", "range", "step", "syntax", "never", "when" };

    for ( unsigned i = 0U; (sizeof(reasons) / sizeof(reasons[0])) > i; ++i )
    {
        char note[KODE_LOG_LINE_MAX];

        console_up();
        set_refused(reasons[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=3 msg=when"), reasons[i]);
        (void)snprintf(note, sizeof(note), "the expression is refused: %s\n", reasons[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(note), "the reason is in the log");
        TEST_ASSERT_FALSE(replied_exactly(">ok"));
    }
}

TEST_CASE("a full table is code 4 full and another door's entry is code 5 owner", "[console_cron]")
{
    console_up();
    set_refused("full");
    TEST_ASSERT_TRUE(replied_exactly(">err code=4 msg=full"));

    console_up();
    set_refused("owner");
    TEST_ASSERT_TRUE(replied_exactly(">err code=5 msg=owner"));
}

TEST_CASE("the other reasons each end with their own word", "[console_cron]")
{
    console_up();
    set_refused("name");
    TEST_ASSERT_TRUE(replied_exactly(">err code=3 msg=name"));

    console_up();
    set_refused("unknown");
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=3 msg=name"), "no such entry names a bad name");

    console_up();
    set_refused("cmd");
    TEST_ASSERT_TRUE(replied_exactly(">err code=3 msg=cmd"));

    console_up();
    set_refused("memory");
    TEST_ASSERT_TRUE(replied_exactly(">err code=5 msg=memory"));

    console_up();
    set_refused("args");
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=5 msg=cron"), "a reason the console does not know");
}

/* An entry whose line can never run is refused where it is made, not each time
 * it fires: a first word that is no verb, and `cron`, which refuses the
 * scheduler's own lines. Nothing reaches AO_Scheduler. */
TEST_CASE("cron refuses a line that opens with an unknown verb or with cron, and takes a real one",
          "[console_cron]")
{
    static const char *const BAD[] = {
        "hcf_not", "cron", "  cron name=\\\"x\\\" when=off", "pingx", "ping=1", "   ",
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
    };
    static const char *const GOOD[] = { "ping", "  ping extra", "config key=\\\"vol\\\" value=5" };
    char line[KODE_CONSOLE_LINE_MAX];

    for ( unsigned i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        console_up();
        (void)snprintf(line, sizeof(line), "cron name=\"x\" when=\"* * * * *\" cmd=\"%s\"", BAD[i]);
        type_line(line);
        TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=3 msg=cmd"), BAD[i]);
        TEST_ASSERT_NULL_MESSAGE(sched_posted((QSignal)SCHED_SET_REQ_SIG), BAD[i]);
    }
    for ( unsigned i = 0U; (sizeof(GOOD) / sizeof(GOOD[0])) > i; ++i )
    {
        const SchedSetReqEvt *r;

        console_up();
        (void)snprintf(line, sizeof(line), "cron name=\"x\" when=\"* * * * *\" cmd=\"%s\"", GOOD[i]);
        type_line(line);
        r = (const SchedSetReqEvt *)sched_posted((QSignal)SCHED_SET_REQ_SIG);
        TEST_ASSERT_NOT_NULL_MESSAGE(r, GOOD[i]);
        QF_gc(&r->super);
    }
}

/* A list that lost an entry on its way is an error and says how many came. */
TEST_CASE("a partial list ends in an error after the records that arrived", "[console_cron]")
{
    console_up();
    type_line("cron");
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));

    answer_entry((uint8_t)SCHED_BY_BUILTIN, false, NEXT_1845, "mqtt", "*/15 * * * *", "");
    answer_end(KODE_ERROR, "partial");

    TEST_ASSERT_TRUE(replied("name=\"mqtt\""));
    TEST_ASSERT_EQUAL_STRING(">err code=5 msg=partial\n", fake_klog_last());
}

TEST_CASE("a text that does not fit is refused by name, from the flaw AO_Scheduler sets", "[console_cron]")
{
    console_up();
    type_line("cron name=\"a-name-longer-than-fifteen\" when=\"* * * * *\" cmd=\"ping\"");
    const SchedSetReqEvt *r = (const SchedSetReqEvt *)sched_posted((QSignal)SCHED_SET_REQ_SIG);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)SCHED_FLAW_NAME, r->flaw);
    QF_gc(&r->super);
    answer_refused("name");
    TEST_ASSERT_TRUE(replied_exactly(">err code=3 msg=name"));
}

TEST_CASE("the grammar: when is a quoted expression or the bare word off", "[console_cron]")
{
    console_up();
    type_line("cron name=\"x\" when=* cmd=\"ping\"");
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=3 msg=when"), "a bare expression");
    TEST_ASSERT_NULL_MESSAGE(sched_posted((QSignal)SCHED_SET_REQ_SIG), "nothing asked");

    fake_klog_reset();
    type_line("cron name=\"x\" when=\"off\"");
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=3 msg=when"), "off quoted");
    TEST_ASSERT_NULL(sched_posted((QSignal)SCHED_SET_REQ_SIG));

    fake_klog_reset();
    type_line("cron name=\"x\" when=nope");
    TEST_ASSERT_TRUE(replied_exactly(">err code=3 msg=when"));

    fake_klog_reset();
    type_line("cron name=\"x\" when=off cmd=\"ping\"");
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=3 msg=cmd"), "off takes no line");

    fake_klog_reset();
    type_line("cron name=x");
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=3 msg=name"), "a bare name");

    fake_klog_reset();
    type_line("cron when=\"* * * * *\" cmd=\"ping\"");
    TEST_ASSERT_TRUE(replied_exactly(">err code=2 msg=name"));

    fake_klog_reset();
    type_line("cron name=\"x\" cmd=\"ping\"");
    TEST_ASSERT_TRUE(replied_exactly(">err code=2 msg=when"));
    TEST_ASSERT_NULL(sched_posted((QSignal)SCHED_SET_REQ_SIG));
}

TEST_CASE("cron that AO_Scheduler never answers times out like any other verb", "[console_cron]")
{
    console_up();
    type_line("cron");
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS);
    TEST_ASSERT_TRUE(replied_exactly(">err code=6 msg=cron"));

    /* A late end finds nothing in flight and says nothing. */
    fake_klog_reset();
    answer_end(KODE_OK, "");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

TEST_CASE("cron is busy while another command is in flight", "[console_cron]")
{
    console_up();
    type_line("cron");
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));
    type_line("cron name=\"x\"");
    TEST_ASSERT_TRUE(replied_exactly(">err code=4 msg=busy"));
    TEST_ASSERT_NULL(sched_posted((QSignal)SCHED_LIST_REQ_SIG));
    answer_end(KODE_OK, "");
}

TEST_CASE("cron refused when the request cannot be posted", "[console_cron]")
{
    static const QEvt filler = QEVT_INITIALIZER(SCHED_DUE_SIG);

    console_up();
    /* Fill the queue so that even the request's margin is gone. */
    while ( uxQueueSpacesAvailable(AO_Scheduler->eQueue) > 0U )
    {
        const QEvt *f = &filler;

        TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueSend(AO_Scheduler->eQueue, &f, 0U));
    }
    type_line("cron");
    TEST_ASSERT_TRUE(replied_exactly(">err code=5 msg=cron"));
    mailbox_empty(AO_Scheduler);

    /* And the console is free again. */
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());
}

/* ---- The channel may write the table ------------------------------------------------ */

TEST_CASE("cron from the Kode channel is accepted and answered on the channel", "[console_cron]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "cron name=\"x\" when=\"* * * * *\" cmd=\"ping\"");

    const SchedSetReqEvt *r = (const SchedSetReqEvt *)sched_posted((QSignal)SCHED_SET_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(r, "the channel may set an entry");
    TEST_ASSERT_EQUAL_STRING("x", r->name);
    QF_gc(&r->super);

    answer_entry((uint8_t)SCHED_BY_CONSOLE, true, NEXT_1840, "x", "* * * * *", "ping");
    answer_end(KODE_OK, "");

    TEST_ASSERT_TRUE(replied(">|cron name=\"x\""));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_NET, src_of(">|cron name=\"x\""));
    TEST_ASSERT_EQUAL_UINT8_MESSAGE((uint8_t)KODE_CONSOLE_SRC_NET, src_of(">ok"),
                                    "the end goes to the channel too");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, line_done_count(),
                                   "a channel line is not one the scheduler is waiting on");
}

/* ---- The source SCHED -------------------------------------------------------------- */

TEST_CASE("a SCHED line's answers are NOTICE lines and its end tells the scheduler once",
          "[console_cron]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ping");

    TEST_ASSERT_TRUE(replied("sched: >|pong"));
    TEST_ASSERT_TRUE(replied("sched: >ok"));
    TEST_ASSERT_FALSE_MESSAGE(a_reply_reached_a_port(), "no reply may reach a port");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, line_done_count(), "one end, one call");
}

TEST_CASE("a SCHED line that is not a verb still ends once", "[console_cron]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "nonsense");
    TEST_ASSERT_TRUE(replied("sched: >err code=1 msg=nonsense"));
    TEST_ASSERT_FALSE(a_reply_reached_a_port());
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());

    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ping extra=1");
    TEST_ASSERT_TRUE_MESSAGE(replied("sched: >err code=3 msg=extra"), "a key the verb lacks");
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());

    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ping name=\"unterminated");
    TEST_ASSERT_TRUE(replied("sched: >err"));
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());
}

TEST_CASE("a blank SCHED line, which answers nothing, still ends once", "[console_cron]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "   ");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());

    /* From the cable a blank line is nothing at all. */
    type_line("   ");
    TEST_ASSERT_EQUAL_UINT(0U, line_done_count());
}

/** @brief How many SCHED_LINE_BUSY_SIG AO_Scheduler's mailbox holds; empties it. */
static unsigned line_busy_count(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    while ( xQueueReceive(AO_Scheduler->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)SCHED_LINE_BUSY_SIG == e->sig )
        {
            ++n;
        }
        QF_gc(e);
    }
    return n;
}

TEST_CASE("a SCHED line meets a console that is busy: handed back, not answered and not ended",
          "[console_cron]")
{
    console_up();
    type_line("cron");
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ping");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "nothing is answered");
    TEST_ASSERT_EQUAL_UINT(1U, line_busy_count());

    /* The command in flight keeps its own port: its answer is a cable reply. */
    answer_end(KODE_OK, "");
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, line_done_count(), "the cable's end is not the scheduler's");
}

TEST_CASE("a cable line that is refused as busy while a SCHED line runs does not end it", "[console_cron]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "power_off");
    const LowPowerReqEvt *p = NULL;
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)LOWPOWER_REQ_SIG == e->sig )
        {
            p = (const LowPowerReqEvt *)e;
        }
        else
        {
            QF_gc(e);
        }
    }
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SCHED, p->src);
    QF_gc(&p->super);

    type_line("ping");
    TEST_ASSERT_TRUE(replied_exactly(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, line_done_count(), "the scheduler's line is still out");
}

TEST_CASE("a SCHED power_off ends the line on the state it asked for, or on the refusal",
          "[console_cron]")
{
    LowPowerStateEvt s;
    LowPowerRefusedEvt r;

    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "power_off");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, line_done_count(), "not before the board answers");

    (void)memset(&s, 0, sizeof(s));
    s.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    s.state = (uint8_t)KODE_LP_OFF;
    s.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    s.prev  = (uint8_t)KODE_LP_AWAKE;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
    TEST_ASSERT_TRUE(replied("sched: >ok"));
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());
    TEST_ASSERT_FALSE(a_reply_reached_a_port());

    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "sleep");
    (void)memset(&r, 0, sizeof(r));
    r.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_REFUSED_SIG);
    r.src   = (uint8_t)KODE_CONSOLE_SRC_SCHED;
    (void)snprintf(r.why, sizeof(r.why), "%s", "input: board used");
    QASM_DISPATCH(AO_Console, &r.super, 0U);
    TEST_ASSERT_TRUE(replied("sched: >err code=4"));
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());

    /* And a verb that never answers is closed by the console's own watchdog,
     * which is also an end. */
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "power_off");
    (void)qf_support_tick(AO_Console, ANY_BUDGET_TICKS);
    TEST_ASSERT_TRUE(replied("sched: >err code=6"));
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());
}

TEST_CASE("a cable line's answers still reach the port and tell the scheduler nothing", "[console_cron]")
{
    console_up();
    type_line("ping");

    TEST_ASSERT_TRUE(replied_exactly(">|pong"));
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());
    TEST_ASSERT_FALSE(replied("sched:"));
    TEST_ASSERT_EQUAL_UINT(0U, line_done_count());
}

TEST_CASE("cron from SCHED is refused: an entry does not edit the table it runs from", "[console_cron]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "cron");
    TEST_ASSERT_TRUE(replied("sched: >err code=5 msg=sched"));
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());

    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "cron name=\"x\" when=off");
    TEST_ASSERT_TRUE(replied("sched: >err code=5 msg=sched"));
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "cron name=\"x\" when=\"* * * * *\" cmd=\"ping\"");
    TEST_ASSERT_NULL_MESSAGE(sched_posted((QSignal)SCHED_SET_REQ_SIG), "no request was made");
    TEST_ASSERT_NULL(sched_posted((QSignal)SCHED_LIST_REQ_SIG));
    TEST_ASSERT_FALSE(a_reply_reached_a_port());
}

TEST_CASE("the payload verbs are refused from SCHED as from the channel", "[console_cron]")
{
    static const char *const lines[] = {
        "file_get name=\"/storage/x.bin\"",
        "file_put path=\"/storage/x.bin\" size=1 crc32=0",
        "screenshot dst=console",
        "install src=\"terminal\" dst=os size=1 crc32=0",
    };

    for ( unsigned i = 0U; (sizeof(lines) / sizeof(lines[0])) > i; ++i )
    {
        console_up();
        type_from((uint8_t)KODE_CONSOLE_SRC_NET, lines[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied_exactly(">err code=5 msg=net"), lines[i]);

        console_up();
        type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, lines[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied("sched: >err code=5 msg=sched"), lines[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, line_done_count(), lines[i]);
        TEST_ASSERT_FALSE_MESSAGE(a_reply_reached_a_port(), lines[i]);
    }
}

TEST_CASE("a multi-record SCHED answer ends the line on its terminator alone", "[console_cron]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "time");
    TEST_ASSERT_TRUE(replied("sched: >|time"));
    TEST_ASSERT_TRUE(replied("sched: >ok"));
    TEST_ASSERT_EQUAL_UINT(1U, line_done_count());
    TEST_ASSERT_FALSE(a_reply_reached_a_port());
}

/* ---- The real scheduler behind the verb --------------------------------------------- */

/** @brief 18:37:12 on the anchor day, Unix seconds. */
#define T_1837_12          INT64_C(1793385432)
/** @brief Microseconds in a second. */
#define US_PER_S           UINT64_C(1000000)

/** @brief Bring AO_Scheduler up for real, as test_scheduler_ao.c does, with the clock set. */
static void sched_real_up(void)
{
    qf_support_reap_timers();
    Scheduler_ctor();
    qf_support_give_queue(AO_Scheduler);
    AO_Scheduler->prio  = KODE_PRIO_SCHEDULER;
    AO_Scheduler->pthre = KODE_PRIO_SCHEDULER;
    if ( qf_support_claim_ao(AO_Scheduler) )
    {
        QActive_register_(AO_Scheduler);
    }
    mailbox_empty(AO_Scheduler);
    mailbox_empty(AO_Net);
    fake_time_advance_ms(KODE_SCHED_SETTLE_MS);
    fake_time_wall_set((uint64_t)T_1837_12 * US_PER_S);
    QASM_INIT(AO_Scheduler, (void *)0, 0U);
    (void)qf_support_settle(AO_Scheduler);
    sched_system_up();
    (void)qf_support_settle(AO_Scheduler);
    mailbox_empty(AO_Net);
}

/** @brief Let AO_Scheduler answer what the console asked, and the console print the answer. */
static void relay(void)
{
    const QEvt *e = NULL;

    (void)qf_support_settle(AO_Scheduler);
    while ( xQueueReceive(AO_Console->eQueue, &e, 0U) == pdTRUE )
    {
        QASM_DISPATCH(AO_Console, e, 0U);
        QF_gc(e);
    }
}

/*
 * The binary's first construction of AO_Scheduler, on purpose: its table is
 * taken from PSRAM once and kept, so only this one can meet a refusal. A case
 * that builds the scheduler earlier in the link order makes this one fail
 * (the list would be answered), not pass over nothing.
 */
TEST_CASE("a scheduler with no memory for its table refuses every door and holds no round",
          "[console_cron]")
{
    LowPowerReadyEvt a;

    console_up();
    fake_psram_refuse(1U);
    sched_real_up();
    fake_psram_refuse(0U);

    type_line("cron");
    relay();
    TEST_ASSERT_TRUE(replied("msg=memory"));
    fake_klog_reset();
    type_line("cron name=\"tick\" when=\"* * * * *\" cmd=\"ping\"");
    relay();
    TEST_ASSERT_TRUE(replied("msg=memory"));

    /* A round finds nothing held, and nothing to wake for. */
    (void)fake_lowpower_ready_take(&a);
    {
        LowPowerRoundEvt r;

        (void)memset(&r, 0, sizeof(r));
        r.super  = (QEvt)QEVT_INITIALIZER(LOWPOWER_ROUND_SIG);
        r.target = (uint8_t)KODE_LP_ASLEEP;
        r.round  = 1U;
        QASM_DISPATCH(AO_Scheduler, &r.super, 0U);
    }
    TEST_ASSERT_TRUE(fake_lowpower_ready_take(&a));
    TEST_ASSERT_FALSE(a.busy);

    QActive_unsubscribeAll(AO_Scheduler);
    mailbox_empty(AO_Scheduler);
    mailbox_empty(AO_Net);
}

TEST_CASE("mqtt switched off is still shown, and a time switches it back on", "[console_cron]")
{
    console_up();
    sched_real_up();

    type_line("cron name=\"mqtt\" when=off");
    relay();
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());

    fake_klog_reset();
    type_line("cron name=\"mqtt\"");
    relay();
    TEST_ASSERT_TRUE(replied_exactly(
        ">|cron name=\"mqtt\" when=\"off\" cmd=- owner=builtin next=- stored=yes"));
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());

    fake_klog_reset();
    type_line("cron");
    relay();
    TEST_ASSERT_TRUE_MESSAGE(replied_exactly(
        ">|cron name=\"mqtt\" when=\"off\" cmd=- owner=builtin next=- stored=yes"),
        "the bare cron lists it");
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());

    fake_klog_reset();
    type_line("cron name=\"mqtt\" when=\"*/15 * * * *\"");
    relay();
    TEST_ASSERT_TRUE(replied_exactly(
        ">|cron name=\"mqtt\" when=\"*/15 * * * *\" cmd=- owner=builtin next=1793385900"
        " stored=yes"));
    TEST_ASSERT_EQUAL_STRING(">ok\n", fake_klog_last());

    QActive_unsubscribeAll(AO_Scheduler);
    mailbox_empty(AO_Scheduler);
    mailbox_empty(AO_Net);
    mailbox_empty(AO_Console);
}

/* ---- The source NET ------------------------------------------------------------------ */

/** @brief The sequence the last NET_LINE_DONE_SIG that net_signal_count() drained carried. */
static uint8_t s_done_seq;

/** @brief How many events of signal @p sig AO_Net's mailbox holds; empties it. */
static unsigned net_signal_count(enum_t sig)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    qf_support_give_queue(AO_Net);
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)sig == e->sig )
        {
            ++n;
        }
        if ( (QSignal)NET_LINE_DONE_SIG == e->sig )
        {
            s_done_seq = ((const NetLineDoneEvt *)(const void *)e)->seq;
        }
        QF_gc(e);
    }
    return n;
}

/** @brief How many NET_LINE_DONE_SIG AO_Net's mailbox holds; empties it. */
static unsigned net_done_count(void)
{
    return net_signal_count(NET_LINE_DONE_SIG);
}

TEST_CASE("test_console_net_line_done: every NET line tells AO_Net once, at its terminator, "
          "the refused and the blank ones included", "[console_cron]")
{
    console_up();
    (void)net_done_count();

    /* A line that answers: its record, then its end. */
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping");
    TEST_ASSERT_TRUE(replied(">|pong"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_NET, src_of(">ok"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_done_count(), "one end, one call");

    /* Refused: not a verb, a key the verb lacks, an unterminated quote. */
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "nonsense");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_NET, src_of(">err code=1 msg=nonsense"));
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping extra=1");
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping name=\"unterminated");
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());

    /* Blank: no answer at all, and still one end. */
    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "   ");
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
}

TEST_CASE("test_console_net_line_done_seq: the end of a NET line carries the sequence the "
          "line arrived with, whatever the line was", "[console_cron]")
{
    LowPowerStateEvt s;

    console_up();
    (void)net_done_count();

    s_line_seq = 7U;
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping");
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
    TEST_ASSERT_EQUAL_UINT8(7U, s_done_seq);

    s_line_seq = 8U;
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "nonsense");
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
    TEST_ASSERT_EQUAL_UINT8(8U, s_done_seq);

    s_line_seq = 9U;
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "   ");
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
    TEST_ASSERT_EQUAL_UINT8(9U, s_done_seq);

    /* A line that waits for the board, and a second one handed back while it
     * waits: the end is the first line's, not the newest line's. */
    s_line_seq = 21U;
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "power_off");
    TEST_ASSERT_EQUAL_UINT(0U, net_done_count());
    s_line_seq = 22U;
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping");
    TEST_ASSERT_EQUAL_UINT(1U, net_signal_count(NET_LINE_BUSY_SIG));
    (void)memset(&s, 0, sizeof(s));
    s.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    s.state = (uint8_t)KODE_LP_OFF;
    s.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    s.prev  = (uint8_t)KODE_LP_AWAKE;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
    TEST_ASSERT_EQUAL_UINT8(21U, s_done_seq);
    s_line_seq = 0U;
}

TEST_CASE("a NET line the console is too busy for is handed back, not answered and not ended; "
          "the command in flight does not end it either", "[console_cron]")
{
    console_up();
    (void)net_done_count();

    type_line("cron");
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "nothing is answered");
    TEST_ASSERT_EQUAL_UINT(1U, net_signal_count(NET_LINE_BUSY_SIG));
    TEST_ASSERT_EQUAL_UINT(0U, net_done_count());

    answer_end(KODE_OK, "");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, net_done_count(), "the cable's end is not the channel's");
}

TEST_CASE("only a NET line tells AO_Net: a cable line and a SCHED line do not, and a verb "
          "that answers on a state waits for it", "[console_cron]")
{
    LowPowerStateEvt s;

    console_up();
    (void)net_done_count();

    type_line("ping");
    type_line("nonsense");
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ping");
    TEST_ASSERT_EQUAL_UINT(0U, net_done_count());
    (void)line_done_count();

    console_up();
    (void)net_done_count();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "power_off");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, net_done_count(), "not before the board answers");
    (void)memset(&s, 0, sizeof(s));
    s.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    s.state = (uint8_t)KODE_LP_OFF;
    s.cause = (uint8_t)KODE_LP_CAUSE_REQUEST;
    s.prev  = (uint8_t)KODE_LP_AWAKE;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
    TEST_ASSERT_EQUAL_UINT(1U, net_done_count());
}

/** @brief How many input notes AO_LowPower's mailbox holds; empties it. */
static unsigned input_notes(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    qf_support_give_queue(AO_LowPower);
    while ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)LOWPOWER_INPUT_SIG == e->sig )
        {
            ++n;
        }
        QF_gc(e);
    }
    return n;
}

TEST_CASE("a NET line handed back as busy is not the board being used: it notes no input, "
          "and the try that is taken notes it once", "[console_cron]")
{
    console_up();
    (void)input_notes();

    type_line("cron");                                  /* the cable's command, in flight */
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));
    (void)input_notes();                                /* its own */

    for ( unsigned i = 0U; 5U > i; i++ )
    {
        type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping");   /* AO_Net's retries */
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, input_notes(), "five refusals, no input");

    answer_end(KODE_OK, "");
    (void)input_notes();
    type_from((uint8_t)KODE_CONSOLE_SRC_NET, "ping");   /* taken */
    TEST_ASSERT_EQUAL_UINT(1U, input_notes());
}

TEST_CASE("a SCHED line handed back as busy is not the board being used either: no input is noted "
          "for any retry, and the try that is taken notes it once", "[console_cron]")
{
    console_up();
    (void)input_notes();

    type_line("cron");                                  /* the cable's command, in flight */
    QF_gc(sched_posted((QSignal)SCHED_LIST_REQ_SIG));
    (void)input_notes();                                /* its own */
    (void)line_busy_count();

    for ( unsigned i = 0U; 5U > i; i++ )
    {
        type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ping");   /* AO_Scheduler's retries */
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(5U, line_busy_count(), "each is handed back");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, input_notes(), "five refusals, no input");

    answer_end(KODE_OK, "");
    (void)input_notes();
    type_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "ping");   /* taken */
    TEST_ASSERT_EQUAL_UINT(1U, input_notes());
}
