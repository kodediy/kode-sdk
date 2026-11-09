/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_scheduler_ao.c
 * @brief AO_Scheduler: its table, the minute tick, console lines, the store and the board's rest.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kdrv_clocks.h"
#include "kode_log.h"
#include "khal_time.h"
#include "kqp.h"
#include "lowpower_cases.h"
#include "qf_support.h"

/** @brief 18:37:12 on the anchor day, Unix seconds. */
#define T_1837_12     INT64_C(1793385432)
/** @brief 18:38:00. */
#define T_1838        INT64_C(1793385480)
/** @brief 18:39:00. */
#define T_1839        INT64_C(1793385540)
/** @brief 18:37:30. */
#define T_1837_30     INT64_C(1793385450)
/** @brief Seconds in a minute. */
#define MINUTE_S      INT64_C(60)
/** @brief Seconds in an hour. */
#define HOUR_S        INT64_C(3600)
/** @brief Microseconds in a second. */
#define US_PER_S      UINT64_C(1000000)
/** @brief Microseconds in a millisecond. */
#define US_PER_MS     UINT64_C(1000)
/** @brief One step of simulated time, milliseconds (qf_support_tick's unit). */
#define STEP_MS       10U
/** @brief The same, microseconds. */
#define STEP_US       ((uint64_t)STEP_MS * US_PER_MS)
/** @brief A little after a minute began, microseconds. */
#define LATE_US       (UINT64_C(100) * US_PER_MS)

/** @brief 18:40:00. */
#define T_1840        INT64_C(1793385600)
/** @brief 18:45:00, the `mqtt` default's first minute after 18:37:12. */
#define T_1845        INT64_C(1793385900)
/** @brief 19:00:00. */
#define T_1900        INT64_C(1793386800)

/** @brief Entries a full table holds besides `mqtt`. */
#define USER_SLOTS    (KODE_SCHED_MAX - 1U)

/** @brief Bytes of a stored record: the version, then name, expression and line. */
#define REC_BYTES     (1U + (KODE_SCHED_NAME_MAX + 1U) + (KODE_SCHED_WHEN_MAX + 1U) \
                       + KODE_SCHED_CMD_MAX)
/** @brief Where the name starts in a stored record. */
#define REC_NAME_AT   1U
/** @brief Where the expression starts. */
#define REC_WHEN_AT   (REC_NAME_AT + KODE_SCHED_NAME_MAX + 1U)
/** @brief Where the line starts. */
#define REC_CMD_AT    (REC_WHEN_AT + KODE_SCHED_WHEN_MAX + 1U)
/** @brief The layout version the AO writes. */
#define REC_VERSION   1U
/** @brief A version nobody writes. */
#define REC_ALIEN     9U
/** @brief A record far too short to be one. */
#define REC_SHORT     5U

/** @brief The signal an entry of code asks to be told with. */
#define FIRE_SIG      ((enum_t)MAX_SIG)

/* ---- The clock --------------------------------------------------------------- */

static uint64_t s_wall_us;   /**< The wall clock the fake reports now.                */
static unsigned s_lines;     /**< Lines AO_Console was given since the last mark.     */
static char     s_line[KODE_CONSOLE_LINE_MAX]; /**< The newest line's text.           */
static uint8_t  s_src;       /**< Its source.                                         */
static unsigned s_checks;    /**< Times handed to AO_Net since the last mark.         */
static int64_t  s_check;     /**< The newest of them.                                 */

/** @brief Move the fake wall clock. */
static void wall_set(uint64_t us)
{
    s_wall_us = us;
    fake_time_wall_set(us);
}

/** @brief Unix seconds and a few microseconds more, as microseconds. */
static uint64_t at_us(int64_t secs, uint64_t extra_us)
{
    return ((uint64_t)secs * US_PER_S) + extra_us;
}

/** @brief Take what the scheduler gave AO_Net: the times of its check. */
static void harvest_net(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)NET_CHECK_AT_SIG == e->sig )
        {
            s_checks++;
            s_check = ((const NetCheckAtEvt *)e)->due_s;
        }
        QF_gc(e);
    }
}

/** @brief Take what the scheduler gave AO_Console and AO_Net. */
static void harvest(void)
{
    const QEvt *e = NULL;

    harvest_net();

    while ( xQueueReceive(AO_Console->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)CONSOLE_LINE_SIG == e->sig )
        {
            const ConsoleLineEvt * const l = (const ConsoleLineEvt *)e;

            s_lines++;
            s_src = l->src;
            (void)memcpy(s_line, l->line, sizeof(s_line));
        }
        QF_gc(e);
    }
}

/**
 * @brief Set the wall clock the way a person or a time server does: write it,
 *        then tell kdrv_clocks, whose hook AO_Scheduler registered, and let
 *        the AO hear it.
 */
static void clock_set(uint64_t us)
{
    wall_set(us);
    kdrv_clocks_wall_note_set();
    (void)qf_support_settle(AO_Scheduler);
    harvest();
}

/** @brief One step: wall clock, uptime and ticks together, then whatever the console was given. */
static void step(void)
{
    wall_set(s_wall_us + STEP_US);
    fake_time_advance_ms(STEP_MS);
    (void)qf_support_tick(AO_Scheduler, 1U);
    harvest();
}

/** @brief Run until the wall clock reads @p target_us. */
static void run_until(uint64_t target_us)
{
    while ( s_wall_us < target_us )
    {
        step();
    }
}

/** @brief Run @p secs seconds. */
static void run_s(int64_t secs)
{
    run_until(s_wall_us + ((uint64_t)secs * US_PER_S));
}

/** @brief The console says it has answered the line it was given. */
static void line_done(void)
{
    sched_line_done();
    (void)qf_support_settle(AO_Scheduler);
    harvest();
}

/** @brief The console says it was busy with another command and took nothing. */
static void line_busy(void)
{
    sched_line_busy();
    (void)qf_support_settle(AO_Scheduler);
    harvest();
}

/* ---- Bring-up ------------------------------------------------------------------- */

/** @brief Empty a queue, releasing what it holds. */
static void queue_empty(QActive *ao)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief kode_init() says the system is up, and AO_Scheduler hears it. */
static void system_up(void)
{
    sched_system_up();
    (void)qf_support_settle(AO_Scheduler);
    harvest();
}

/**
 * @brief Register AO_Scheduler once, construct it afresh and run its initial
 *        transition with the wall clock at @p wall_us.
 *
 * The case's NVS is whatever it holds: this is a boot. The boot is long past
 * (KODE_SCHED_SETTLE_MS), unless a case says otherwise afterwards.
 *
 * @param wall_us The wall clock, microseconds; 0 for a clock not set.
 * @param up      Whether kode_init() then says the system is up.
 */
static void sched_start(uint64_t wall_us, bool up)
{
    qf_support_init();
    qf_support_reap_timers();
    (void)ao_spy();
    ao_spy_drain();

    Scheduler_ctor();
    qf_support_give_queue(AO_Scheduler);
    AO_Scheduler->prio  = KODE_PRIO_SCHEDULER;
    AO_Scheduler->pthre = KODE_PRIO_SCHEDULER;
    if ( qf_support_claim_ao(AO_Scheduler) )
    {
        QActive_register_(AO_Scheduler);
    }
    queue_empty(AO_Scheduler);
    qf_support_give_queue(AO_Console);
    queue_empty(AO_Console);
    qf_support_give_queue(AO_Net);
    queue_empty(AO_Net);

    fake_time_advance_ms(KODE_SCHED_SETTLE_MS);
    wall_set(wall_us);
    s_lines = 0U;
    s_line[0] = '\0';
    s_checks = 0U;
    s_check  = -2;
    (void)fake_lowpower_rest_count(NULL);
    fake_klog_reset();
    QASM_INIT(AO_Scheduler, (void *)0, 0U);
    (void)qf_support_settle(AO_Scheduler);
    harvest_net();
    if ( up )
    {
        system_up();
    }
}

/** @brief The same, and the system is up. */
static void sched_up(uint64_t wall_us)
{
    sched_start(wall_us, true);
}

/** @brief The same, at the anchor. */
static void sched_boot(void)
{
    sched_up(at_us(T_1837_12, 0U));
}

/**
 * @brief Leave nothing armed: the entries that have work are removed, the
 *        timer stops with the next arming, and the queues are emptied.
 */
static void sched_down(void);

/* ---- The answers ----------------------------------------------------------------- */

/** @brief What the spy heard to a request: the entries, then the end. */
typedef struct {
    unsigned entries;                          /**< SCHED_ENTRY_SIG events.         */
    char     name[KODE_SCHED_MAX][KODE_SCHED_NAME_MAX + 1U];  /**< Their names.   */
    char     when[KODE_SCHED_MAX][KODE_SCHED_WHEN_MAX + 1U];  /**< Expressions.   */
    char     cmd[KODE_SCHED_MAX][32];          /**< The start of each line.         */
    uint8_t  owner[KODE_SCHED_MAX];            /**< Their owners.                   */
    bool     stored[KODE_SCHED_MAX];           /**< Whether each is stored.         */
    int64_t  next_s[KODE_SCHED_MAX];           /**< Each next minute.               */
    unsigned ends;                             /**< SCHED_END_SIG events.           */
    uint8_t  st;                               /**< The end's status.               */
    uint8_t  count;                            /**< The end's count of entries sent. */
    char     why[KODE_SCHED_WHY_MAX];          /**< The end's reason.               */
} answer_t;

static answer_t s_ans;   /**< The last answer read (static: it is large). */

/** @brief Read everything the spy holds as an answer. */
static const answer_t *collect(void)
{
    const QEvt *e = NULL;

    (void)memset(&s_ans, 0, sizeof(s_ans));
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)SCHED_ENTRY_SIG == e->sig )
        {
            const SchedEntryEvt * const r = (const SchedEntryEvt *)e;
            unsigned const i = s_ans.entries;

            TEST_ASSERT_TRUE(KODE_SCHED_MAX > i);
            (void)memcpy(s_ans.name[i], r->name, sizeof(s_ans.name[i]));
            (void)memcpy(s_ans.when[i], r->when, sizeof(s_ans.when[i]));
            (void)memcpy(s_ans.cmd[i], r->cmd, sizeof(s_ans.cmd[i]) - 1U);
            s_ans.owner[i]  = r->owner;
            s_ans.stored[i] = r->stored;
            s_ans.next_s[i] = r->next_s;
            s_ans.entries++;
        }
        else if ( (QSignal)SCHED_END_SIG == e->sig )
        {
            const SchedEndEvt * const r = (const SchedEndEvt *)e;

            s_ans.st = r->st;
            s_ans.count = r->count;
            (void)memcpy(s_ans.why, r->why, sizeof(s_ans.why));
            s_ans.ends++;
        }
        else
        {
            /* Not an answer. */
        }
    }
    ao_spy_drain();
    return &s_ans;
}

/** @brief Ask for a console entry and read the answer. */
static const answer_t *set(const char *name, const char *when, const char *cmd)
{
    TEST_ASSERT_TRUE(sched_request_set(ao_spy(), name, when, cmd));
    (void)qf_support_settle(AO_Scheduler);
    return collect();
}

/** @brief Ask for an entry of code (the spy is its target) and read the answer. */
static const answer_t *set_ao(const char *name, const char *when)
{
    TEST_ASSERT_TRUE(sched_request_set_ao(ao_spy(), name, when, ao_spy(), FIRE_SIG));
    (void)qf_support_settle(AO_Scheduler);
    return collect();
}

/** @brief Ask to remove an entry of code and read the answer. */
static const answer_t *remove_ao(const char *name)
{
    TEST_ASSERT_TRUE(sched_request_remove(ao_spy(), name));
    (void)qf_support_settle(AO_Scheduler);
    return collect();
}

/** @brief List one entry (or all with NULL) and read the answer. */
static const answer_t *list(const char *name)
{
    TEST_ASSERT_TRUE(sched_request_list(ao_spy(), name));
    (void)qf_support_settle(AO_Scheduler);
    return collect();
}

/** @brief Assert that the answer ended well. */
static void ok(const answer_t *a)
{
    TEST_ASSERT_EQUAL_UINT(1U, a->ends);
    TEST_ASSERT_EQUAL_UINT8(KODE_OK, a->st);
    TEST_ASSERT_EQUAL_STRING("", a->why);
    TEST_ASSERT_EQUAL_UINT(a->entries, a->count);
}

/** @brief Assert that the answer was a refusal for @p why, with no entries. */
static void refused(const answer_t *a, const char *why)
{
    TEST_ASSERT_EQUAL_UINT(1U, a->ends);
    TEST_ASSERT_EQUAL_UINT8(KODE_DENIED, a->st);
    TEST_ASSERT_EQUAL_STRING(why, a->why);
    TEST_ASSERT_EQUAL_UINT(0U, a->entries);
}

/** @brief Where @p name is in the answer, or -1. */
static int index_of(const answer_t *a, const char *name)
{
    int found = -1;

    for ( unsigned i = 0U; (0 > found) && (i < a->entries); i++ )
    {
        if ( strcmp(a->name[i], name) == 0 )
        {
            found = (int)i;
        }
    }
    return found;
}

/** @brief Turn the built-in off, so a list holds only what a case made. */
static void mqtt_off(void)
{
    ok(set(KODE_SCHED_MQTT_NAME, KODE_SCHED_WHEN_OFF, ""));
}

/** @brief Lines in the log that contain @p needle. */
static unsigned logged(const char *needle)
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

static void sched_down(void)
{
    const answer_t *a;

    /* Everything with work to do goes, which stops the minute timer; the
     * console entries by their own door, the entries of code by theirs. */
    a = list(NULL);
    for ( unsigned i = 0U; i < a->entries; i++ )
    {
        char name[KODE_SCHED_NAME_MAX + 1U];
        uint8_t const owner = a->owner[i];

        (void)memcpy(name, a->name[i], sizeof(name));
        if ( (uint8_t)SCHED_BY_CONSOLE == owner )
        {
            (void)set(name, KODE_SCHED_WHEN_OFF, "");
        }
        else if ( (uint8_t)SCHED_BY_CODE == owner )
        {
            (void)remove_ao(name);
        }
        else
        {
            /* `mqtt` does no work awake: it arms nothing. */
        }
    }
    queue_empty(AO_Scheduler);
    queue_empty(AO_Console);
    queue_empty(AO_Net);
    QActive_unsubscribeAll(AO_Scheduler);
    ao_spy_drain();
    /* The AO is gone: no later case may set the clock into its queue. */
    kdrv_clocks_wall_on_set(NULL);
}

/* ---- The table and its doors ----------------------------------------------------- */

/* test_sched_set_list_remove */
TEST_CASE("two entries are listed and one removed", "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    mqtt_off();

    a = set("water", "0 7 * * *", "gpio set pin=1");
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_EQUAL_STRING("water", a->name[0]);
    TEST_ASSERT_EQUAL_STRING("0 7 * * *", a->when[0]);
    TEST_ASSERT_EQUAL_STRING("gpio set pin=1", a->cmd[0]);
    TEST_ASSERT_EQUAL_UINT8(SCHED_BY_CONSOLE, a->owner[0]);
    TEST_ASSERT_TRUE(a->stored[0]);
    /* The next 07:00 after 18:37:12 is tomorrow's. */
    TEST_ASSERT_EQUAL_INT64(INT64_C(1793430000), a->next_s[0]);
    ok(set("probe", "*/5 * * * *", "ping"));

    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, a->entries, "mqtt, switched off, is listed too");
    TEST_ASSERT_TRUE(index_of(a, KODE_SCHED_MQTT_NAME) >= 0);
    TEST_ASSERT_TRUE(index_of(a, "water") >= 0);
    TEST_ASSERT_TRUE(index_of(a, "probe") >= 0);

    a = set("water", KODE_SCHED_WHEN_OFF, "");
    ok(a);
    TEST_ASSERT_EQUAL_UINT(0U, a->entries);

    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(2U, a->entries);
    TEST_ASSERT_TRUE(index_of(a, "probe") >= 0);
    TEST_ASSERT_TRUE(index_of(a, "water") < 0);

    a = list("water");
    refused(a, "unknown");
    a = list("probe");
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);

    sched_down();
}

/* test_sched_refuse_bad_when */
TEST_CASE("an expression that is wrong is refused with the reason", "[scheduler_ao]")
{
    char too_long[KODE_SCHED_NAME_MAX + 8U];
    char long_line[KODE_SCHED_CMD_MAX + 8U];

    sched_boot();
    mqtt_off();

    refused(set("a", "60 * * * *", "ping"), "range");
    refused(set("a", "*/0 * * * *", "ping"), "step");
    refused(set("a", "* * * *", "ping"), "fields");
    refused(set("a", "x * * * *", "ping"), "syntax");
    /* Valid and never matching: refused rather than kept to wait for ever. */
    refused(set("a", "0 0 31 2 *", "ping"), "never");
    refused(set("a", "* * * * *", ""), "cmd");
    refused(set("", "* * * * *", "ping"), "name");
    refused(set("a b", "* * * * *", "ping"), "name");

    (void)memset(too_long, 'n', sizeof(too_long) - 1U);
    too_long[sizeof(too_long) - 1U] = '\0';
    refused(set(too_long, "* * * * *", "ping"), "name");

    (void)memset(long_line, 'c', sizeof(long_line) - 1U);
    long_line[sizeof(long_line) - 1U] = '\0';
    refused(set("a", "* * * * *", long_line), "cmd");

    /* The same on the door of code. */
    refused(set_ao("a", "60 * * * *"), "range");

    /* Nothing was made. */
    TEST_ASSERT_EQUAL_UINT(1U, list(NULL)->entries);   /* mqtt, off, alone */
    TEST_ASSERT_EQUAL_UINT(1U, fake_nvs_count());   /* mqtt's "off" alone */

    sched_down();
}

/* test_sched_full */
TEST_CASE("the table holds sixteen, the built-in included, and refuses the next", "[scheduler_ao]")
{
    const answer_t *a;
    char name[KODE_SCHED_NAME_MAX + 1U];

    sched_boot();

    for ( unsigned i = 0U; USER_SLOTS > i; i++ )
    {
        (void)snprintf(name, sizeof(name), "n%02u", i);
        ok(set(name, "* * * * *", "ping"));
    }
    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(KODE_SCHED_MAX, a->entries);

    refused(set("one-more", "* * * * *", "ping"), "full");
    refused(set_ao("one-more", "* * * * *"), "full");

    /* A change of an entry that is there needs no room. */
    ok(set("n05", "*/2 * * * *", "ping again"));
    a = list("n05");
    ok(a);
    TEST_ASSERT_EQUAL_STRING("*/2 * * * *", a->when[0]);

    /* Room made, room taken. */
    ok(set("n05", KODE_SCHED_WHEN_OFF, ""));
    ok(set("one-more", "* * * * *", "ping"));

    sched_down();
}

/* test_sched_owner */
TEST_CASE("neither door changes the other's entry, and mqtt is reserved", "[scheduler_ao]")
{
    sched_boot();

    ok(set_ao("pump", "*/10 * * * *"));
    ok(set("shell", "0 7 * * *", "ping"));

    /* The console against an entry of code. */
    refused(set("pump", "* * * * *", "ping"), "owner");
    refused(set("pump", KODE_SCHED_WHEN_OFF, ""), "owner");
    /* Code against the console's. */
    refused(set_ao("shell", "* * * * *"), "owner");
    refused(remove_ao("shell"), "owner");
    /* Code may not take the built-in's name or remove it. */
    refused(set_ao(KODE_SCHED_MQTT_NAME, "* * * * *"), "owner");
    refused(remove_ao(KODE_SCHED_MQTT_NAME), "owner");
    /* Nothing by that name. */
    refused(remove_ao("ghost"), "unknown");
    refused(set("ghost", KODE_SCHED_WHEN_OFF, ""), "unknown");

    /* Each door still changes its own. */
    ok(set_ao("pump", "*/20 * * * *"));
    ok(set("shell", "0 8 * * *", "ping"));
    ok(remove_ao("pump"));
    ok(set("shell", KODE_SCHED_WHEN_OFF, ""));
    TEST_ASSERT_EQUAL_UINT(1U, list(NULL)->entries);   /* mqtt alone */

    sched_down();
}

/* test_sched_mqtt_builtin */
TEST_CASE("mqtt is there with nothing stored, changes, goes off and takes no line", "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    TEST_ASSERT_EQUAL_UINT(0U, fake_nvs_count());

    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_MQTT_NAME, a->name[0]);
    TEST_ASSERT_EQUAL_STRING("*/15 * * * *", a->when[0]);
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_MQTT_DEFAULT, a->when[0]);
    TEST_ASSERT_EQUAL_UINT8(SCHED_BY_BUILTIN, a->owner[0]);
    TEST_ASSERT_FALSE(a->stored[0]);
    /* 18:45:00 is the next quarter hour after 18:37:12. */
    TEST_ASSERT_EQUAL_INT64(T_1837_12 + 468, a->next_s[0]);

    /* A line is not its work. */
    refused(set(KODE_SCHED_MQTT_NAME, "*/5 * * * *", "ping"), "cmd");
    TEST_ASSERT_EQUAL_STRING("*/15 * * * *", list(NULL)->when[0]);

    /* Its time changes and is kept. */
    a = set(KODE_SCHED_MQTT_NAME, "*/5 * * * *", "");
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_EQUAL_STRING("*/5 * * * *", a->when[0]);
    TEST_ASSERT_TRUE(a->stored[0]);
    TEST_ASSERT_EQUAL_UINT(1U, fake_nvs_count());
    refused(set(KODE_SCHED_MQTT_NAME, "60 * * * *", ""), "range");

    sched_boot();
    a = list(NULL);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_EQUAL_STRING("*/5 * * * *", a->when[0]);
    TEST_ASSERT_TRUE(a->stored[0]);

    /* Off is not hidden: it is shown as "off", stored, with no next minute,
     * in a show and in a list, and it stays off across a boot. */
    a = set(KODE_SCHED_MQTT_NAME, KODE_SCHED_WHEN_OFF, "");
    ok(a);
    TEST_ASSERT_EQUAL_UINT(0U, a->entries);
    a = list(KODE_SCHED_MQTT_NAME);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_MQTT_NAME, a->name[0]);
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_WHEN_OFF, a->when[0]);
    TEST_ASSERT_EQUAL_STRING("", a->cmd[0]);
    TEST_ASSERT_EQUAL_UINT8(SCHED_BY_BUILTIN, a->owner[0]);
    TEST_ASSERT_TRUE(a->stored[0]);
    TEST_ASSERT_EQUAL_INT64(-1, a->next_s[0]);
    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_WHEN_OFF, a->when[0]);
    sched_boot();
    a = list(NULL);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_WHEN_OFF, a->when[0]);
    TEST_ASSERT_TRUE(a->stored[0]);

    /* And comes back with a time. */
    a = set(KODE_SCHED_MQTT_NAME, "*/15 * * * *", "");
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    sched_boot();
    TEST_ASSERT_EQUAL_UINT(1U, list(NULL)->entries);

    sched_down();
}

/* ---- Firing while awake ------------------------------------------------------------ */

/* test_sched_fires_once */
TEST_CASE("an entry every minute runs its line once at the minute", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    /* Up to the last tick before 18:38:00, nothing. */
    run_until(at_us(T_1838, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_STRING("ping", s_line);
    TEST_ASSERT_EQUAL_UINT8(KODE_CONSOLE_SRC_SCHED, s_src);

    /* The console answers it. Nothing more goes out until 18:39. */
    line_done();
    run_until(at_us(T_1839, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);

    sched_down();
}

/* An entry that is not due at the minute runs nothing. */
TEST_CASE("an entry runs only at the minutes its expression names", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("quarter", "*/15 * * * *", "ping"));

    /* 18:45:00 is the first. */
    run_until(at_us(T_1837_12 + 468, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);
    run_until(at_us(T_1837_12 + 468, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    sched_down();
}

/* test_sched_clock_jump */
TEST_CASE("a clock that jumps forward fires once, one set back repeats nothing", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    line_done();

    /* Three hours forward: the minutes it jumped over are not owed. */
    clock_set(s_wall_us + (uint64_t)(3 * HOUR_S) * US_PER_S);
    run_s(MINUTE_S + 1);
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    line_done();

    /* Onward, one a minute as before. */
    run_s(MINUTE_S);
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    line_done();

    /* One minute back: the minute that was run is not run again. */
    clock_set(s_wall_us - (uint64_t)MINUTE_S * US_PER_S);
    {
        int64_t const back_min = (int64_t)(s_wall_us / US_PER_S) / MINUTE_S;

        /* The clock passes the minute it already ran, and nothing runs. */
        run_until(at_us((back_min + 2) * MINUTE_S, 0U) - STEP_US);
        TEST_ASSERT_EQUAL_UINT(3U, s_lines);
        /* The first minute that is new runs. */
        run_until(at_us((back_min + 2) * MINUTE_S, LATE_US));
        TEST_ASSERT_EQUAL_UINT(4U, s_lines);
    }

    sched_down();
}

/* A jump back inside the minute that just ran, the exact case in the brief. */
TEST_CASE("a clock set back a minute after 18:38 does not run 18:38 twice", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    line_done();

    clock_set(s_wall_us - (uint64_t)MINUTE_S * US_PER_S);
    /* The wall clock passes 18:38 a second time. */
    run_until(at_us(T_1838, 30U * US_PER_S));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    /* And then 18:39, which is new. */
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);

    sched_down();
}

/* test_sched_clock_unknown */
TEST_CASE("no clock, no fire; the next minute after it is known fires", "[scheduler_ao]")
{
    sched_up(0U);
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));
    TEST_ASSERT_EQUAL_INT64(-1, list("tick")->next_s[0]);

    run_s(5 * MINUTE_S);
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);

    /* Told the time at 18:37:30. */
    clock_set(at_us(T_1837_30, 0U));
    run_until(at_us(T_1838, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);
    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    sched_down();
}

/* The clock kept across a power cut is the system clock, restored: it counts. */
TEST_CASE("a clock restored after a power cut counts as set", "[scheduler_ao]")
{
    int64_t const known = (int64_t)KODE_CLOCK_KNOWN_AFTER_S;

    /* At the threshold itself the clock is not known: no next minute. */
    sched_up(at_us(known, 0U));
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));
    TEST_ASSERT_EQUAL_INT64(-1, list("tick")->next_s[0]);
    sched_down();

    /* Past it, as a restored value is, there is one and it fires. */
    sched_up(at_us(known + 30, 0U));
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));
    TEST_ASSERT_EQUAL_INT64(known + MINUTE_S, list("tick")->next_s[0]);
    run_until(at_us(known + MINUTE_S, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    sched_down();
}

/* ---- The lines, one at a time ------------------------------------------------------ */

TEST_CASE("lines of one minute go one at a time, in table order", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("first", "* * * * *", "ping first"));
    ok(set("second", "* * * * *", "ping second"));
    ok(set("third", "0 7 * * *", "ping third"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_STRING("ping first", s_line);

    run_s(5);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);   /* the console has not answered */

    line_done();
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    TEST_ASSERT_EQUAL_STRING("ping second", s_line);

    line_done();
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);   /* nothing else was due */

    sched_down();
}

TEST_CASE("an entry whose line never answered is skipped the next minute, then runs again",
          "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("slow", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    /* The console never answers. The next minute finds the work outlasting its minute. */
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    /* The answer is forgotten once the bound has passed: the minute after runs. */
    run_until(at_us(T_1840, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    line_done();
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);

    /* And answered, it runs on its own the minute after. */
    run_until(at_us(T_1840 + MINUTE_S, LATE_US));
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);

    sched_down();
}

TEST_CASE("an entry of code is posted its event at the minute, not stored", "[scheduler_ao]")
{
    const QEvt *e;
    unsigned fires = 0U;

    sched_boot();
    mqtt_off();
    ok(set_ao("water", "* * * * *"));
    TEST_ASSERT_EQUAL_UINT(1U, fake_nvs_count());   /* mqtt's "off", and nothing for "water" */

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);   /* no console line for it */
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)FIRE_SIG == e->sig )
        {
            const SchedFireEvt * const f = (const SchedFireEvt *)e;

            TEST_ASSERT_EQUAL_STRING("water", f->name);
            fires++;
        }
    }
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT(1U, fires);

    sched_down();
}

TEST_CASE("a minute in the first moments after boot runs when they end, in that minute", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();

    /* The boot is young again, and 18:38 begins three seconds into it. */
    fake_time_reset();
    wall_set(at_us(T_1838 - 3, 0U));
    ok(set("tick", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);            /* the console may not exist yet */
    run_until(at_us(T_1838 + 2, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);            /* five seconds not over */
    run_until(at_us(T_1838 + 2, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);            /* over, and still 18:38 */
    line_done();

    /* And only once: the next is 18:39. */
    run_until(at_us(T_1839, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);

    sched_down();
}

/* A request handled in the instant of the minute must not take the minute away. */
TEST_CASE("a request at the minute's first tick leaves the minute to fire", "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    run_until(at_us(T_1838, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);

    /* Posted ahead of the timer's expiry, handled in the step that begins 18:38. */
    TEST_ASSERT_TRUE(sched_request_set(ao_spy(), "other", "*/5 * * * *", "ping other"));
    step();
    a = collect();
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_STRING("ping", s_line);
    line_done();

    /* The same for a removal, and the entry of code. */
    run_until(at_us(T_1839, 0U) - STEP_US);
    TEST_ASSERT_TRUE(sched_request_set_ao(ao_spy(), "code", "* * * * *", ao_spy(), FIRE_SIG));
    step();
    ok(collect());
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);

    sched_down();
}

/* A clock set back far is not waited for. */
TEST_CASE("a clock set back hours runs the entries from the new time", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    line_done();

    /* Five hours back: a year back would be no different. */
    clock_set(s_wall_us - (uint64_t)(5 * HOUR_S) * US_PER_S);
    {
        int64_t const back_min = (int64_t)(s_wall_us / US_PER_S) / MINUTE_S;

        /* The first minute after the set is the next to run, not the one after. */
        run_until(at_us((back_min + 1) * MINUTE_S, 0U) - STEP_US);
        TEST_ASSERT_EQUAL_UINT(1U, s_lines);
        run_until(at_us((back_min + 1) * MINUTE_S, LATE_US));
        TEST_ASSERT_EQUAL_UINT(2U, s_lines);
        TEST_ASSERT_TRUE(logged("was set back") >= 1U);
        line_done();
        run_until(at_us((back_min + 2) * MINUTE_S, LATE_US));
        TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    }

    sched_down();
}

/* The timer armed before a set stands on the old clock: the first minute the new
 * one reaches that an entry names must still run, whatever minute the timer was
 * armed for, and the list must not promise a minute that does not fire. */
TEST_CASE("a clock set to just before a due minute runs that minute, back or forward",
          "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    mqtt_off();
    ok(set("hourly", "0 * * * *", "ping"));

    /* The timer waits for 19:00. The clock is set 38 minutes back, to 10 s before 18:00. */
    run_s(20);
    clock_set(at_us(T_1900 - HOUR_S - 10, 0U));
    a = list("hourly");
    ok(a);
    TEST_ASSERT_EQUAL_INT64(T_1900 - HOUR_S, a->next_s[0]);
    run_s(MINUTE_S);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "18:00 must run");
    TEST_ASSERT_EQUAL_STRING("ping", s_line);
    line_done();

    /* Four hours back, again to 10 s before a due minute: 14:00. */
    clock_set(at_us(T_1900 - (5 * HOUR_S) - 10, 0U));
    a = list("hourly");
    ok(a);
    TEST_ASSERT_EQUAL_INT64(T_1900 - (5 * HOUR_S), a->next_s[0]);
    run_s(MINUTE_S);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "14:00 must run");
    TEST_ASSERT_TRUE(logged("was set back") >= 1U);
    line_done();

    /* A day forward, to 10 s before 19:00: it runs once, and the next is an hour on. */
    clock_set(at_us(T_1900 + (24 * HOUR_S) - 10, 0U));
    run_s(MINUTE_S);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_lines, "19:00 must run");
    line_done();
    run_s(MINUTE_S);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(3U, s_lines, "and only once");

    sched_down();
}

/* A set that lands in a minute a pass has run for repeats nothing, even when the
 * timer finds the clock moved. */
TEST_CASE("a clock set back a few minutes after an hourly run repeats no minute", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("hourly", "0 * * * *", "ping"));

    run_until(at_us(T_1900, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    line_done();

    /* Ten minutes back: 19:00 is not run a second time when the clock reaches it. */
    clock_set(s_wall_us - (uint64_t)(10 * MINUTE_S) * US_PER_S);
    run_until(at_us(T_1900 + (30 * MINUTE_S), 0U));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    sched_down();
}

/** @brief 06:00:00 the day after the anchor, Unix seconds. */
#define T_0600_NEXT   (T_1900 + (11 * HOUR_S))

/* The clock is told to the scheduler when it is set, so a set to a few seconds
 * before a due minute neither loses that minute nor shows a `next` that will
 * not fire, for the entries and for the time AO_Net is handed. */
TEST_CASE("a clock set to hh:59:55 runs the :00 minute on time, and next names it",
          "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    ok(set("hourly", "0 * * * *", "ping"));
    run_s(20);

    /* Forward: the timer waited for 19:00, and the clock is now 5 s from 06:00. */
    clock_set(at_us(T_0600_NEXT - 5, 0U));
    a = list("hourly");
    ok(a);
    TEST_ASSERT_EQUAL_INT64(T_0600_NEXT, a->next_s[0]);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(T_0600_NEXT, s_check, "mqtt's check follows the new clock too");
    run_until(at_us(T_0600_NEXT, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(0U, s_lines);
    run_until(at_us(T_0600_NEXT, LATE_US));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "06:00 runs at 06:00");
    TEST_ASSERT_EQUAL_STRING("ping", s_line);
    line_done();

    /* The time server's answer is told the same way. */
    wall_set(at_us(T_0600_NEXT + HOUR_S - 5, 0U));
    kdrv_clocks_wall_note_sntp();
    (void)qf_support_settle(AO_Scheduler);
    harvest();
    a = list("hourly");
    ok(a);
    TEST_ASSERT_EQUAL_INT64(T_0600_NEXT + HOUR_S, a->next_s[0]);
    run_until(at_us(T_0600_NEXT + HOUR_S, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    line_done();

    sched_down();
}

/* A set that is not on a minute adds no run of its own: the timer that was
 * waiting on the old clock is gone, not left to expire at an odd second. */
TEST_CASE("an unaligned clock set adds no stray run", "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    line_done();

    /* Twelve minutes on, 36.995 s into 18:50. The old timer would have
     * expired 21 s after the set, with the clock at 18:50:58. */
    clock_set(at_us(T_1838 + (12 * MINUTE_S) + 36, 995000U));
    run_until(at_us(T_1838 + (13 * MINUTE_S), 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "nothing between the set and 18:51");
    run_until(at_us(T_1838 + (13 * MINUTE_S), LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    line_done();

    /* The same set once more, back this time and within the hour. */
    clock_set(at_us(T_1838 + (3 * MINUTE_S) + 52, 797000U));
    run_until(at_us(T_1838 + (14 * MINUTE_S), 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "the minutes that ran are not run again");
    run_until(at_us(T_1838 + (14 * MINUTE_S), LATE_US));
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    line_done();

    sched_down();
}

/* A clock set back by less than an hour repeats no minute, and `next` says the
 * minute that is new rather than one the clock will reach again. */
TEST_CASE("a clock set back twenty minutes shows and runs the first new minute", "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    line_done();
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    line_done();

    clock_set(at_us(T_1839 - (19 * MINUTE_S) + 30, 0U));
    a = list("tick");
    ok(a);
    TEST_ASSERT_EQUAL_INT64(T_1840, a->next_s[0]);
    run_until(at_us(T_1840, 0U) - STEP_US);
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    TEST_ASSERT_EQUAL_UINT(0U, logged("was set back"));
    run_until(at_us(T_1840, LATE_US));
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    line_done();

    sched_down();
}

/* The end of an answer is never the one that is lost. */
TEST_CASE("a list that does not fit the asker's queue ends partial with the count", "[scheduler_ao]")
{
    static const QEvt filler = QEVT_INITIALIZER(FIRE_SIG);
    const answer_t *a;
    char name[KODE_SCHED_NAME_MAX + 1U];
    unsigned room = 5U;

    sched_boot();
    for ( unsigned i = 0U; USER_SLOTS > i; i++ )
    {
        (void)snprintf(name, sizeof(name), "n%02u", i);
        ok(set(name, "*/30 * * * *", "ping"));
    }

    /* The spy's queue holds 32: leave it room for five. */
    while ( (unsigned)ao_spy_count() < (32U - room) )
    {
        QACTIVE_POST(ao_spy(), &filler, NULL);
    }
    TEST_ASSERT_TRUE(sched_request_list(ao_spy(), NULL));
    (void)qf_support_settle(AO_Scheduler);
    a = collect();

    TEST_ASSERT_EQUAL_UINT(1U, a->ends);
    TEST_ASSERT_EQUAL_UINT8(KODE_ERROR, a->st);
    TEST_ASSERT_EQUAL_STRING("partial", a->why);
    /* Each entry leaves a slot for the end: five slots, four entries. */
    TEST_ASSERT_EQUAL_UINT(room - KODE_SCHED_END_MARGIN, a->entries);
    TEST_ASSERT_EQUAL_UINT8(room - KODE_SCHED_END_MARGIN, a->count);

    /* With room, the whole table and an end that says so. */
    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(KODE_SCHED_MAX, a->entries);

    sched_down();
}

TEST_CASE("an answer to a line the queue would not take is logged", "[scheduler_ao]")
{
    static const QEvt filler = QEVT_INITIALIZER(FIRE_SIG);

    sched_boot();
    /* Nothing is dispatched: the queue is filled to the brim. */
    while ( xQueueSend(AO_Scheduler->eQueue, &(const QEvt *){ &filler }, 0U) == pdTRUE )
    {
    }
    fake_klog_reset();
    sched_line_done();
    TEST_ASSERT_EQUAL_UINT(1U, logged("answer to a line was dropped"));
    queue_empty(AO_Scheduler);

    sched_down();
}

/* ---- Asleep: the round, the wake, the rest --------------------------------------- */

/** @brief Tell AO_Scheduler the board is AWAKE, from @p prev, for this cause. */
static void lp_wake(kode_lp_cause_t cause, kode_lp_state_t prev)
{
    LowPowerStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_STATE_SIG);
    e.state = (uint8_t)KODE_LP_AWAKE;
    e.cause = (uint8_t)cause;
    e.prev  = (uint8_t)prev;
    QASM_DISPATCH(AO_Scheduler, &e.super, 0U);
    harvest();
}

/** @brief The board sleeps until the wall clock reads @p target_us: the clocks move, no tick runs. */
static void sleep_until(uint64_t target_us)
{
    fake_time_advance_ms((uint32_t)((target_us - s_wall_us) / US_PER_MS));
    wall_set(target_us);
}

/**
 * @brief Open a round for ASLEEP on AO_Scheduler.
 *
 * @param w Receives the wake time it gave, when it gave one.
 * @return true when it gave one.
 */
static bool round_asleep(LowPowerWakeAtEvt *w)
{
    (void)fake_lowpower_wake_take(w);
    lp_round(AO_Scheduler, KODE_LP_ASLEEP);
    return fake_lowpower_wake_take(w);
}

/** @brief A refusal of the rest the scheduler asked for. */
static void rest_refused(const char *why)
{
    LowPowerRefusedEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(LOWPOWER_REFUSED_SIG);
    e.src   = (uint8_t)KODE_LOWPOWER_SRC_SCHED;
    (void)snprintf(e.why, sizeof(e.why), "%s", why);
    QASM_DISPATCH(AO_Scheduler, &e.super, 0U);
}

/* test_sched_round_gives_time */
TEST_CASE("a round for ASLEEP gives the earliest entry's minute, and nothing for mqtt alone",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    sched_boot();

    /* Only the built-in `mqtt` (default): its work is AO_Net's, so nothing to wake for. */
    TEST_ASSERT_FALSE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    lp_expect_no_more();
    lp_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_ASLEEP);

    ok(set("late", "40 18 * * *", "ping"));
    ok(set("soon", "38 18 * * *", "ping"));
    TEST_ASSERT_TRUE_MESSAGE(round_asleep(&w), "the earliest entry's minute is given");
    TEST_ASSERT_EQUAL_UINT64(khal_time_us() + (at_us(T_1838, 0U) - s_wall_us), w.due_us);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    /* The wake takes the board back; a round abandoned says AWAKE again, and the
     * minute is still there to be given. */
    lp_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_TRUE(round_asleep(&w));
    TEST_ASSERT_EQUAL_UINT64(khal_time_us() + (at_us(T_1838, 0U) - s_wall_us), w.due_us);
    lp_expect_answer(false, NULL);

    /* An OFF round has nothing to wake: the board boots. */
    lp_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    (void)fake_lowpower_wake_take(&w);
    lp_round(AO_Scheduler, KODE_LP_OFF);
    TEST_ASSERT_FALSE(fake_lowpower_wake_take(&w));
    lp_expect_answer(false, NULL);

    sched_down();
}

/* test_sched_rest_after_line */
TEST_CASE("a wake for the entry's own time asks for rest once, when its line has answered",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    uint8_t src = 0U;

    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1838, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);

    run_s(1);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "the entry's line went out in its minute");
    TEST_ASSERT_EQUAL_STRING("ping", s_line);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_rest_count(NULL), "the line has not answered");

    /* A round abandoned says AWAKE again: that is no new wake, and not a timer's. */
    lp_wake(KODE_LP_CAUSE_IDLE, KODE_LP_AWAKE);

    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(&src), "and then, rest");
    TEST_ASSERT_EQUAL_UINT8(KODE_LOWPOWER_SRC_SCHED, src);

    run_s(5);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_rest_count(NULL), "once");

    /* Refused: the board stays awake and nothing more is asked of it. */
    fake_klog_reset();
    rest_refused("input: board used");
    TEST_ASSERT_EQUAL_UINT(1U, logged("the board stays awake: input: board used"));
    run_s(5);
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_rest_count(NULL));

    sched_down();
}

/* test_sched_line_bound */
TEST_CASE("a line that never answers is waited for until the bound, then the rest is asked",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    sched_boot();
    mqtt_off();
    ok(set("stuck", "* * * * *", "ping"));
    ok(set("next", "* * * * *", "ping again"));

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1838, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);

    run_s(1);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    run_s((int64_t)(KODE_SCHED_WORK_BOUND_MS / 1000U) - 3);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_rest_count(NULL), "inside the bound");

    /* A round meanwhile is held by the line waiting behind the one that is
     * out: that one is a plain line, and the round is not its own. */
    lp_round(AO_Scheduler, KODE_LP_ASLEEP);
    lp_expect_answer(true, "sched work");

    fake_klog_reset();
    run_s(4);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(NULL), "at the bound");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("did not answer"), "and the log says why");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("'next' did not run"), "the line that waited is dropped");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "and never goes out late");
    run_s(30);
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_rest_count(NULL));

    /* The work given up, a round is taken. */
    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);

    sched_down();
}

/* A console busy with another command takes the line and answers nothing. The
 * entry keeps its place and the line goes again a retry later, once. */
TEST_CASE("a line the console was busy for goes again after the retry, and runs",
          "[scheduler_ao]")
{
    sched_boot();
    mqtt_off();
    ok(set("busy", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    line_busy();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "not at once");
    run_s((int64_t)(KODE_SCHED_RETRY_MS / 1000U) + 1);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "after the retry");
    TEST_ASSERT_EQUAL_STRING("ping", s_line);

    line_done();
    run_s(5);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "taken, it is not given a third time");

    sched_down();
}

/* The tries end where the work of the minute ends: the line is dropped, said
 * once, and the next minute finds the entry free. */
TEST_CASE("a console that stays busy is tried every retry until the bound, then the line is dropped",
          "[scheduler_ao]")
{
    unsigned seen = 0U;

    sched_boot();
    mqtt_off();
    ok(set("busy", "* * * * *", "ping"));

    fake_klog_reset();
    run_until(at_us(T_1838, LATE_US));
    for ( unsigned i = 0U; ((KODE_SCHED_WORK_BOUND_MS / KODE_SCHED_RETRY_MS) + 4U) > i; i++ )
    {
        if ( seen < s_lines )
        {
            seen = s_lines;
            line_busy();
        }
        run_s(1);
    }
    TEST_ASSERT_TRUE_MESSAGE(s_lines >= (KODE_SCHED_WORK_BOUND_MS / KODE_SCHED_RETRY_MS) - 1U,
                             "a try a second");
    TEST_ASSERT_TRUE_MESSAGE(s_lines <= (KODE_SCHED_WORK_BOUND_MS / KODE_SCHED_RETRY_MS) + 1U,
                             "and none past the bound");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("'busy' did not run"), "said once, at the give-up");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("entry 'busy' is due"), "the hand-over is said once for the minute, not per try");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, logged("' runs"), "a line the console may still refuse is not said to run");

    seen = s_lines;
    run_s(20);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(seen, s_lines, "nothing goes out after the give-up");

    /* The next minute runs it again, and says so again. */
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(seen + 1U, s_lines);
    TEST_ASSERT_EQUAL_UINT(2U, logged("entry 'busy' is due"));
    line_done();

    sched_down();
}

/* A pass that runs while a retry is pending sends the line itself, and a second
 * busy answer then arms the retry on a timer that is not armed. */
TEST_CASE("a pass that finds a retry pending sends the line, and a second busy answer arms it once",
          "[scheduler_ao]")
{
    static const QEvt due = QEVT_INITIALIZER(SCHED_DUE_SIG);
    unsigned before;

    sched_boot();
    mqtt_off();
    ok(set("one", "* * * * *", "ping one"));
    ok(set("two", "* * * * *", "ping two"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    line_busy();                                    /* the retry is armed */

    /* The next minute's expiry arrives before the retry's. */
    wall_set(at_us(T_1839, LATE_US));
    QACTIVE_POST(AO_Scheduler, &due, NULL);
    (void)qf_support_settle(AO_Scheduler);
    harvest();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "the pass sent the line");

    /* The retry's second finds a line out, and sends nothing. */
    run_s(2);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "one line out, one retry");

    line_busy();                                    /* arms the retry again */
    before = s_lines;
    run_s(2);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(before + 1U, s_lines, "and the retry sends it once");

    sched_down();
}

/* test_sched_overlap */
TEST_CASE("an entry every minute whose line answers after 70 s skips the minute it is still out in",
          "[scheduler_ao][lowpower]")
{
    sched_boot();
    mqtt_off();
    ok(set("slow", "* * * * *", "ping"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    /* 18:39: the line is 60 s old and the bound long gone, but it is not answered. */
    fake_klog_reset();
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "nothing goes out over an unanswered line");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("'slow' is skipped"), "one WARN");

    /* 70 s after the fire it answers: it frees the entry and runs nothing late. */
    run_until(at_us(T_1838, 70U * US_PER_S));
    line_done();
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);

    run_until(at_us(T_1840, LATE_US));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "18:40 runs");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("'slow' is skipped"), "still one");

    /* Unanswered at 18:41 as well: skipped again, and said again. */
    run_until(at_us(T_1840 + MINUTE_S, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, logged("'slow' is skipped"), "said again after it ran");

    sched_down();
}

TEST_CASE("an entry of code is done at the bound; a wake that is not a timer's asks for no rest",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    const QEvt *e;
    unsigned fires = 0U;

    sched_boot();
    mqtt_off();
    ok(set_ao("water", "* * * * *"));

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1838, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);

    run_s(1);
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)FIRE_SIG == e->sig )
        {
            fires++;
        }
    }
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT(1U, fires);
    run_s((int64_t)(KODE_SCHED_WORK_BOUND_MS / 1000U) - 3);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_rest_count(NULL), "its work is out until the bound");
    lp_round(AO_Scheduler, KODE_LP_ASLEEP);
    lp_expect_answer(true, "sched work");
    run_s(4);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(NULL), "rest at the bound");

    /* The next wake is the user's (BOT), and the entry fires in it: no rest asked. */
    (void)fake_lowpower_wake_take(&w);
    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1839, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    run_s(30);
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_rest_count(NULL));
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)FIRE_SIG == e->sig )
        {
            fires++;
        }
    }
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, fires, "the entry fired all the same");

    sched_down();
}

/* A line that ends the wake (`sleep`, `power_off`) is answered by the console only after the round it asks for. */
TEST_CASE("a line handed to the console does not make the scheduler refuse the round",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    sched_boot();
    mqtt_off();
    ok(set("off", "* * * * *", "power_off"));
    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_lines, "the line is with the console");

    lp_round(AO_Scheduler, KODE_LP_OFF);
    lp_expect_answer(false, NULL);

    /* And the same for a round to ASLEEP. */
    lp_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);

    sched_down();
}

/* The answer to one line is lost. */
TEST_CASE("a lost answer does not poison the lines after it",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    unsigned before;

    sched_boot();
    mqtt_off();
    ok(set("one", "* * * * *", "ping one"));
    ok(set("two", "* * * * *", "ping two"));

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1838, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);

    /* 18:38: "one" goes out and its answer is lost; "two" waits and is dropped at the bound. */
    run_s(1);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    run_s(12);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(NULL), "the board was told to rest at the bound");

    /* 18:39: "one" is skipped (its work outlasted the minute); "two" runs and is answered. */
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    TEST_ASSERT_EQUAL_STRING("ping two", s_line);
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "nothing else was due");

    /* 18:40 and 18:41: both run, each answered at once, with no wait for the bound and no skip. */
    fake_klog_reset();
    for ( int64_t m = 0; 2 > m; m++ )
    {
        before = s_lines;
        run_until(at_us(T_1840 + (m * MINUTE_S), LATE_US));
        TEST_ASSERT_EQUAL_UINT(before + 1U, s_lines);
        TEST_ASSERT_EQUAL_STRING("ping one", s_line);
        line_done();
        TEST_ASSERT_EQUAL_UINT(before + 2U, s_lines);
        TEST_ASSERT_EQUAL_STRING("ping two", s_line);
        line_done();
    }
    run_s(15);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, logged("did not answer"), "no wait ran out");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, logged("is skipped"), "and nothing was skipped");

    sched_down();
}

/* A refusal that is not a person's leaves the scheduler free to ask again. */
TEST_CASE("a rest refused for another reason than use is asked again after the next pass",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1838, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    run_s(1);
    line_done();
    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_rest_count(NULL));

    /* A round was running: nobody's choice. 18:39 runs and asks again. */
    rest_refused(KODE_LOWPOWER_WHY_RUNNING);
    run_s(5);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_rest_count(NULL), "not at once, at the next pass");
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(NULL), "asked again after 18:39's work");

    /* Another's request waited to begin again: that passes too. */
    rest_refused(KODE_LOWPOWER_WHY_PENDING);
    run_until(at_us(T_1840, LATE_US));
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(NULL), "asked again after 18:40's work");

    /* A person used the board: the wake is theirs, and 18:41 asks nothing. */
    rest_refused(KODE_LOWPOWER_WHY_USED);
    run_until(at_us(T_1840 + MINUTE_S, LATE_US));
    TEST_ASSERT_EQUAL_UINT(4U, s_lines);
    line_done();
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_rest_count(NULL));

    sched_down();
}

/* test_sched_rest_owner_busy: an owner stayed busy for the whole retry window. */
TEST_CASE("a rest an owner refused past the retry window is asked once in the wake, not every minute",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    sched_boot();
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1838, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    run_s(1);
    line_done();
    TEST_ASSERT_EQUAL_UINT(1U, fake_lowpower_rest_count(NULL));

    /* AO_LowPower began it again for the whole window, and the owner stayed busy. */
    rest_refused("net: net job");
    for ( int64_t m = 0; 3 > m; m++ )
    {
        run_until(at_us(T_1839 + (m * MINUTE_S), LATE_US));
        line_done();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_lowpower_rest_count(NULL),
                                       "the refusal stands for the wake: no round a minute");
    }
    TEST_ASSERT_EQUAL_UINT(4U, s_lines);

    /* The next timer wake asks once again. */
    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1840 + (2 * MINUTE_S), 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    run_s(1);
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(NULL), "a new wake, a new request");

    sched_down();
}

/* The board woke late for its entry, in the minute after: the pass fires nothing. */
TEST_CASE("a timer wake whose pass fires nothing asks for rest",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;
    uint8_t src = 0U;

    sched_boot();
    mqtt_off();
    ok(set("once", "38 18 * * *", "ping"));

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1839, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);

    run_s(1);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_lines, "18:38 is gone: nothing runs");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_lowpower_rest_count(&src), "and the board is let go");
    TEST_ASSERT_EQUAL_UINT8(KODE_LOWPOWER_SRC_SCHED, src);
    run_s(5);
    TEST_ASSERT_EQUAL_UINT(0U, fake_lowpower_rest_count(NULL));

    sched_down();
}

/* A late entry skips the minute after its lost answer; the board is not woken for it. */
TEST_CASE("a late entry gives no wake time for the minute it will skip",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    sched_boot();
    mqtt_off();
    ok(set("one", "* * * * *", "ping one"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    run_s((int64_t)(KODE_SCHED_WORK_BOUND_MS / 1000U) + 1);   /* the answer is lost */

    TEST_ASSERT_TRUE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(khal_time_us() + (at_us(T_1840, 0U) - s_wall_us), w.due_us,
                                     "18:40, not the 18:39 it skips");

    /* Woken then, it runs. */
    sleep_until(at_us(T_1840, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_TIMER, KODE_LP_ASLEEP);
    run_s(1);
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    line_done();

    sched_down();
}

/* test_sched_state_ending_line */
TEST_CASE("a power_off with another entry in its minute does not hold its own round",
          "[scheduler_ao][lowpower]")
{
    sched_boot();
    mqtt_off();
    ok(set("off", "* * * * *", "power_off"));
    ok(set("two", "* * * * *", "ping two"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_STRING("power_off", s_line);

    /* The round power_off asks for: the line behind it is not held work. */
    fake_klog_reset();
    lp_round(AO_Scheduler, KODE_LP_OFF);
    lp_expect_answer(false, NULL);

    /* The console answers power_off after the round: nothing goes out behind it. */
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("'two' did not run: the board rests before its turn"),
                                   "it is dropped, and says so");
    run_s(15);
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_UINT(0U, logged("did not answer"));

    /* 18:39, and this time another owner abandons the round: the board stays
     * awake, and the line behind goes out once power_off has answered. */
    lp_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(2U, s_lines);
    lp_round(AO_Scheduler, KODE_LP_OFF);
    lp_expect_answer(false, NULL);
    lp_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);
    line_done();
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    TEST_ASSERT_EQUAL_STRING("ping two", s_line);
    line_done();

    sched_down();
}

/* test_sched_plain_line_holds: the drop is for a line that ends the wake only. */
TEST_CASE("a round while a plain line is out holds the lines behind it, and drops none",
          "[scheduler_ao][lowpower]")
{
    sched_boot();
    mqtt_off();
    ok(set("one", "* * * * *", "net_info"));
    ok(set("two", "* * * * *", "ping two"));

    run_until(at_us(T_1838, LATE_US));
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_STRING("net_info", s_line);

    /* The idle time's round, or somebody else's: it is not the line's. */
    fake_klog_reset();
    lp_round(AO_Scheduler, KODE_LP_ASLEEP);
    lp_expect_answer(true, "sched work");
    lp_round(AO_Scheduler, KODE_LP_OFF);
    lp_expect_answer(true, "sched work");
    lp_wake(KODE_LP_CAUSE_REQUEST, KODE_LP_AWAKE);

    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(2U, s_lines, "the line behind goes out");
    TEST_ASSERT_EQUAL_STRING("ping two", s_line);
    TEST_ASSERT_EQUAL_UINT(0U, logged("did not run"));
    line_done();

    /* A `sleep` with blanks around its verb is a line that ends the wake. */
    ok(set("one", "* * * * *", "  sleep "));
    run_until(at_us(T_1839, LATE_US));
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    lp_round(AO_Scheduler, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, logged("'two' did not run: the board rests before its turn"),
                                   "dropped behind the line that put the board to rest");
    TEST_ASSERT_EQUAL_UINT(3U, s_lines);
    lp_wake(KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);

    /* A verb that only begins like one is a plain line. */
    ok(set("one", "* * * * *", "sleepy"));
    run_until(at_us(T_1840, LATE_US));
    TEST_ASSERT_EQUAL_UINT(4U, s_lines);
    lp_round(AO_Scheduler, KODE_LP_ASLEEP);
    lp_expect_answer(true, "sched work");
    line_done();
    TEST_ASSERT_EQUAL_UINT(5U, s_lines);
    line_done();

    sched_down();
}

/* ---- The mqtt entry, handed to AO_Net ------------------------------------------------ */

TEST_CASE("the next minute of mqtt is handed to AO_Net at start and at every change",
          "[scheduler_ao][lowpower]")
{
    sched_boot();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_checks, "at start");
    TEST_ASSERT_EQUAL_INT64_MESSAGE(T_1845, s_check, "*/15 after 18:37:12");

    ok(set(KODE_SCHED_MQTT_NAME, "*/5 * * * *", ""));
    harvest();
    TEST_ASSERT_EQUAL_UINT(2U, s_checks);
    TEST_ASSERT_EQUAL_INT64(T_1840, s_check);

    /* A request that changes nothing about it hands nothing. */
    ok(set("tick", "0 7 * * *", "ping"));
    harvest();
    TEST_ASSERT_EQUAL_UINT(2U, s_checks);

    ok(set(KODE_SCHED_MQTT_NAME, KODE_SCHED_WHEN_OFF, ""));
    harvest();
    TEST_ASSERT_EQUAL_UINT(3U, s_checks);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(-1, s_check, "off is none");

    ok(set(KODE_SCHED_MQTT_NAME, "*/15 * * * *", ""));
    harvest();
    TEST_ASSERT_EQUAL_UINT(4U, s_checks);
    TEST_ASSERT_EQUAL_INT64(T_1845, s_check);

    sched_down();
}

TEST_CASE("mqtt waits for the clock, and for the system to be up",
          "[scheduler_ao][lowpower]")
{
    /* No clock: none is handed, and the time follows when it is told. */
    sched_up(0U);
    TEST_ASSERT_EQUAL_UINT(1U, s_checks);
    TEST_ASSERT_EQUAL_INT64(-1, s_check);
    wall_set(at_us(T_1837_12, 0U));
    run_s(2);
    TEST_ASSERT_EQUAL_UINT(2U, s_checks);
    TEST_ASSERT_EQUAL_INT64(T_1845, s_check);
    sched_down();

    /* Before kode_init() says the system is up AO_Net may have no queue, so
     * nothing is posted to it, however long that takes; then at once. */
    sched_start(at_us(T_1837_12, 0U), false);
    TEST_ASSERT_EQUAL_UINT(0U, s_checks);
    ok(set(KODE_SCHED_MQTT_NAME, "*/5 * * * *", ""));
    harvest();
    TEST_ASSERT_EQUAL_UINT(0U, s_checks);
    run_s(10);
    TEST_ASSERT_EQUAL_UINT(0U, s_checks);
    system_up();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_checks, "when the system is up");
    TEST_ASSERT_EQUAL_INT64(T_1840, s_check);
    sched_down();
}

/* test_sched_system_up */
TEST_CASE("nothing fires before the system is up; the minute it comes in still runs",
          "[scheduler_ao]")
{
    const QEvt *e;
    unsigned fires = 0U;

    sched_start(at_us(T_1837_12, 0U), false);
    mqtt_off();
    ok(set("tick", "* * * * *", "ping"));
    ok(set_ao("code", "* * * * *"));

    /* 18:38 comes and goes by a few seconds: no line, no event of code. */
    run_until(at_us(T_1838 + 5, 0U));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_lines, "the console may have no queue yet");
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, ao_spy_count(), "nor the entry's AO");

    /* Up inside 18:38: the minute that has begun is kept, and runs. */
    system_up();
    step();
    TEST_ASSERT_EQUAL_UINT(1U, s_lines);
    TEST_ASSERT_EQUAL_STRING("ping", s_line);
    while ( (e = ao_spy_take()) != NULL )
    {
        if ( (QSignal)FIRE_SIG == e->sig )
        {
            fires++;
        }
    }
    ao_spy_drain();
    TEST_ASSERT_EQUAL_UINT(1U, fires);
    line_done();

    sched_down();
}

TEST_CASE("a check made is answered with the next time, even asleep; a change asleep waits for AWAKE",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    sched_boot();
    TEST_ASSERT_EQUAL_INT64(T_1845, s_check);

    TEST_ASSERT_FALSE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    s_checks = 0U;

    /* The radio wake made the check at 18:45:30. */
    sleep_until(at_us(T_1845 + 30, 0U));
    sched_check_done(T_1845);
    (void)qf_support_settle(AO_Scheduler);
    harvest();
    TEST_ASSERT_EQUAL_UINT(1U, s_checks);
    TEST_ASSERT_EQUAL_INT64_MESSAGE(T_1900, s_check, "18:45 and a quarter of an hour");

    /* A change while asleep is held until the AWAKE. */
    s_checks = 0U;
    ok(set(KODE_SCHED_MQTT_NAME, "50 18 * * *", ""));
    harvest();
    TEST_ASSERT_EQUAL_UINT(0U, s_checks);
    lp_wake(KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, s_checks);
    TEST_ASSERT_EQUAL_INT64(T_1845 + (5 * MINUTE_S), s_check);

    sched_down();
}

/* test_sched_check_report_lost: sched_check_done() refused, so nothing comes back. */
TEST_CASE("a check whose report is lost is held a minute and its dwell, then the next time goes out",
          "[scheduler_ao][lowpower]")
{
    LowPowerWakeAtEvt w;

    /* Awake, an entry every minute arms the timer at every pass. */
    sched_boot();
    TEST_ASSERT_EQUAL_INT64(T_1845, s_check);
    ok(set("tick", "* * * * *", "ping"));
    s_checks = 0U;
    run_until(at_us(T_1845, LATE_US));
    line_done();
    run_until(at_us(T_1845 + MINUTE_S, LATE_US));
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_checks, "18:45 is AO_Net's while it is held");
    run_until(at_us(T_1845 + (2 * MINUTE_S), LATE_US));
    line_done();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_checks, "the hold over, the next time goes out");
    TEST_ASSERT_EQUAL_INT64(T_1900, s_check);
    sched_down();

    /* Asleep: a whole wake in the minute keeps it; the next wake after the hold hands 19:00. */
    sched_boot();
    TEST_ASSERT_FALSE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    s_checks = 0U;
    sleep_until(at_us(T_1845, 400U * US_PER_MS));
    lp_wake(KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(0U, s_checks);
    TEST_ASSERT_FALSE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1845 + MINUTE_S, 0U));
    lp_wake(KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, s_checks, "still inside the minute and the dwell");
    TEST_ASSERT_FALSE(round_asleep(&w));
    lp_expect_answer(false, NULL);
    sleep_until(at_us(T_1845 + (2 * MINUTE_S), 0U));
    lp_wake(KODE_LP_CAUSE_BOT, KODE_LP_ASLEEP);
    TEST_ASSERT_EQUAL_UINT(1U, s_checks);
    TEST_ASSERT_EQUAL_INT64(T_1900, s_check);
    sched_down();
}

TEST_CASE("a time AO_Net would not take is handed again at the next chance",
          "[scheduler_ao][lowpower]")
{
    static const QEvt filler = QEVT_INITIALIZER(FIRE_SIG);

    sched_boot();
    while ( xQueueSend(AO_Net->eQueue, &(const QEvt *){ &filler }, 0U) == pdTRUE )
    {
    }
    fake_klog_reset();
    ok(set(KODE_SCHED_MQTT_NAME, "*/5 * * * *", ""));
    TEST_ASSERT_EQUAL_UINT(1U, logged("was refused"));
    queue_empty(AO_Net);
    s_checks = 0U;

    ok(set("tick", "0 7 * * *", "ping"));
    harvest();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_checks, "the next request tries again");
    TEST_ASSERT_EQUAL_INT64(T_1840, s_check);
    ok(set("tock", "0 8 * * *", "ping"));
    harvest();
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, s_checks, "and once it is taken, no more");

    sched_down();
}

/* ---- The store ----------------------------------------------------------------------- */

/* test_sched_store_roundtrip */
TEST_CASE("a console entry survives a boot and an entry of code does not", "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    ok(set("keep", "*/10 * * * *", "ping"));
    ok(set_ao("gone", "*/10 * * * *"));
    TEST_ASSERT_EQUAL_UINT(1U, fake_nvs_count());   /* "keep" alone */

    sched_boot();
    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(2U, a->entries);   /* mqtt and keep */
    TEST_ASSERT_TRUE(index_of(a, KODE_SCHED_MQTT_NAME) >= 0);
    TEST_ASSERT_TRUE(index_of(a, "gone") < 0);
    TEST_ASSERT_TRUE(index_of(a, "keep") >= 0);
    TEST_ASSERT_EQUAL_STRING("*/10 * * * *", a->when[index_of(a, "keep")]);
    TEST_ASSERT_EQUAL_STRING("ping", a->cmd[index_of(a, "keep")]);
    TEST_ASSERT_TRUE(a->stored[index_of(a, "keep")]);
    TEST_ASSERT_EQUAL_UINT8(SCHED_BY_CONSOLE, a->owner[index_of(a, "keep")]);

    /* Removed, it is gone from the store as well. */
    ok(set("keep", KODE_SCHED_WHEN_OFF, ""));
    TEST_ASSERT_EQUAL_UINT(0U, fake_nvs_count());
    sched_boot();
    TEST_ASSERT_EQUAL_UINT(1U, list(NULL)->entries);

    sched_down();
}

TEST_CASE("an entry the store would not take is kept in RAM and says so", "[scheduler_ao]")
{
    const answer_t *a;

    sched_boot();
    mqtt_off();
    fake_nvs_fail_next_set();
    a = set("fragile", "* * * * *", "ping");
    ok(a);
    TEST_ASSERT_EQUAL_UINT(1U, a->entries);
    TEST_ASSERT_FALSE(a->stored[0]);
    TEST_ASSERT_TRUE(logged("could not be stored") >= 1U);

    sched_down();
}

/** @brief A record of the AO's layout with the three texts given. */
static void seed_record(const char *key, uint8_t version, const char *name,
                        const char *when, const char *cmd)
{
    uint8_t rec[REC_BYTES];

    (void)memset(rec, 0, sizeof(rec));
    rec[0] = version;
    (void)strcpy((char *)&rec[REC_NAME_AT], name);
    (void)strcpy((char *)&rec[REC_WHEN_AT], when);
    (void)strcpy((char *)&rec[REC_CMD_AT], cmd);
    fake_nvs_seed("kcron", key, rec, sizeof(rec));
}

/* test_sched_store_corrupt */
TEST_CASE("a stored record that does not parse is skipped and the board boots", "[scheduler_ao]")
{
    static const uint8_t junk[REC_SHORT] = { 0xDEU, 0xADU, 0xBEU, 0xEFU, 0x00U };
    const answer_t *a;

    /* Another layout, another version, an expression that does not parse, a
     * name that is the built-in's, the built-in's own record in pieces, and
     * one good record among them. */
    fake_nvs_seed("kcron", "e1", junk, sizeof(junk));
    seed_record("e2", REC_ALIEN, "alien", "* * * * *", "ping");
    seed_record("e3", REC_VERSION, "badwhen", "60 * * * *", "ping");
    seed_record("e4", REC_VERSION, "mqtt", "* * * * *", "ping");
    seed_record("e5", REC_VERSION, "good", "0 7 * * *", "ping");
    fake_nvs_seed("kcron", "mqtt", junk, sizeof(junk));

    sched_boot();
    a = list(NULL);
    ok(a);
    TEST_ASSERT_EQUAL_UINT(2U, a->entries);
    TEST_ASSERT_TRUE(index_of(a, "good") >= 0);
    /* The built-in is at its default. */
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_MQTT_DEFAULT, a->when[index_of(a, KODE_SCHED_MQTT_NAME)]);
    TEST_ASSERT_EQUAL_UINT(5U, logged("is skipped"));

    /* The skipped slots are free for new entries. */
    ok(set("fresh", "* * * * *", "ping"));
    TEST_ASSERT_EQUAL_UINT(3U, list(NULL)->entries);

    sched_down();
}

/* The board's own record can carry a bad expression too. */
TEST_CASE("a bad expression in mqtt's record leaves its default", "[scheduler_ao]")
{
    sched_boot();
    seed_record("mqtt", REC_VERSION, "mqtt", "*/0 * * * *", "");

    sched_boot();
    TEST_ASSERT_EQUAL_STRING(KODE_SCHED_MQTT_DEFAULT, list(NULL)->when[0]);
    TEST_ASSERT_EQUAL_UINT(1U, logged("is skipped"));

    sched_down();
}
