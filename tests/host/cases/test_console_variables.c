/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_variables.c
 * @brief `variable` and `variable_list`: an app's state, from the wire.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kregistry.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kode_variables.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/* ---- The app under test ----------------------------------------------------- */

static int32_t       s_temp     = 235;
static kode_status_t s_temp_rc  = KODE_OK;     /**< what `temp_dc`'s get answers */
static bool          s_led;
static const char   *s_label    = "kode";      /**< what `label`'s get writes    */

static kode_status_t get_temp(char *b, size_t n)   { (void)snprintf(b, n, "%ld", (long)s_temp); return s_temp_rc; }
static kode_status_t get_led(char *b, size_t n)    { (void)snprintf(b, n, "%s", s_led ? "on" : "off"); return KODE_OK; }
static kode_status_t get_mode(char *b, size_t n)   { (void)snprintf(b, n, "slow"); return KODE_OK; }
static kode_status_t get_label(char *b, size_t n)  { (void)snprintf(b, n, "%s", s_label); return KODE_OK; }
static kode_status_t get_filler(char *b, size_t n) { (void)snprintf(b, n, "0"); return KODE_OK; }

/* Fourteen rows: KCLI_SLICE is 12, so variable_list needs two slices. The
 * names stay clear of test_console_functions.c's: both tables can be in at
 * once, and a name is one kind or the other. */
static const kode_variable_t VARIABLES[] = {
    { .name = "temp_dc", .get = get_temp, .desc = "Board temperature, deci-degrees" },
    { .name = "led",     .get = get_led },
    { .name = "mode",    .get = get_mode, .desc = "Fan mode: off, slow or fast" },
    { .name = "label",   .get = get_label },
    { .name = "f0", .get = get_filler },
    { .name = "f1", .get = get_filler },
    { .name = "f2", .get = get_filler },
    { .name = "f3", .get = get_filler },
    { .name = "f4", .get = get_filler },
    { .name = "f5", .get = get_filler },
    { .name = "f6", .get = get_filler },
    { .name = "f7", .get = get_filler },
    { .name = "f8", .get = get_filler },
    { .name = "f9", .get = get_filler },
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
    fake_klog_reset();
    fake_neighbours_reset();
    /* This file's table and nothing else among the variables: another file
     * may have left one of its own, or emptied the registry. */
    if ( kvariables_find("temp_dc") != &VARIABLES[0] )
    {
        kvariables_reset();
        TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_VARIABLES_ADD(VARIABLES));
    }
    s_temp = 235; s_temp_rc = KODE_OK; s_led = false; s_label = "kode";
}

static void tick(unsigned ticks)
{
    (void)qf_support_tick(AO_Console, ticks);
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

/** @brief Index of the first captured line containing @p needle, or -1. */
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

/** @brief Whether captured line @p idx exists and contains @p needle. */
static bool line_is(unsigned idx, const char *needle)
{
    const char * const l = fake_klog_line(idx);
    return (NULL != l) && (strstr(l, needle) != NULL);
}

/** @brief How many captured lines contain @p needle. */
static unsigned lines_with(const char *needle)
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

/** @brief One line typed, with the capture cleared first. */
static void ask(const char *text)
{
    fake_klog_reset();
    type_line(text);
}

/* ---- variable_list ---------------------------------------------------------- */

TEST_CASE("variable_list names every row, sliced, with desc last", "[console_variables]")
{
    console_up();
    ask("variable_list");
    /* The first slice went out at once; the second waits a tick. */
    TEST_ASSERT_FALSE(replied(">ok"));
    tick(2U);
    TEST_ASSERT_TRUE(replied(">|variable=\"temp_dc\" watch=0 desc=\"Board temperature, deci-degrees\""));
    TEST_ASSERT_TRUE(replied(">|variable=\"led\" watch=0 desc=-"));
    TEST_ASSERT_TRUE(replied(">|variable=\"mode\" watch=0 desc=\"Fan mode: off, slow or fast\""));
    TEST_ASSERT_EQUAL_UINT(14U, lines_with(">|variable="));
    /* A variable is read and nothing else: no type, no bounds, no rw. */
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(" type="));
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(" rw="));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE(line_of(">|variable=\"f9\"") < line_of(">ok"));
}

TEST_CASE("variable_list with no rows is a bare >ok", "[console_variables]")
{
    console_up();
    kvariables_reset();
    ask("variable_list");
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(">|variable="));
    TEST_ASSERT_TRUE(replied(">ok"));
    /* Put the table back for everyone after us. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_VARIABLES_ADD(VARIABLES));
}

/* ---- read ------------------------------------------------------------------ */

TEST_CASE("a bare variable answers one record and >ok", "[console_variables]")
{
    console_up();
    ask("variable name=\"temp_dc\"");
    TEST_ASSERT_TRUE_MESSAGE(line_is(0U, ">|variable=\"temp_dc\" value=235"),
                             "the record is not the first line");
    TEST_ASSERT_TRUE(line_is(1U, ">ok"));
    TEST_ASSERT_EQUAL_UINT(2U, fake_klog_count());

    s_temp = -40;
    ask("variable name=\"temp_dc\"");
    TEST_ASSERT_TRUE(replied(">|variable=\"temp_dc\" value=-40"));

    ask("variable name=\"led\"");
    TEST_ASSERT_TRUE(replied(">|variable=\"led\" value=off"));
    ask("variable name=\"mode\"");
    TEST_ASSERT_TRUE(replied(">|variable=\"mode\" value=slow"));
    ask("variable name=\"label\"");
    TEST_ASSERT_TRUE(replied(">|variable=\"label\" value=kode"));
}

TEST_CASE("a word with spaces goes out as one word", "[console_variables]")
{
    console_up();
    s_label = "kode dot";
    ask("variable name=\"label\"");
    TEST_ASSERT_TRUE(replied(">|variable=\"label\" value=kode_dot"));
    TEST_ASSERT_TRUE(replied(">ok"));
    /* A value goes out bare, so a quote in it would read to a host as the
     * start of a string: it is made safe the way a space is. */
    s_label = "say \"hi\"";
    ask("variable name=\"label\"");
    TEST_ASSERT_TRUE(replied(">|variable=\"label\" value=say__hi_"));
}

TEST_CASE("a get that fails is code 5 named after the row", "[console_variables]")
{
    console_up();
    s_temp_rc = KODE_IO_ERROR;
    ask("variable name=\"temp_dc\"");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=temp_dc"));
    TEST_ASSERT_FALSE(replied(">|variable="));
}

TEST_CASE("a word too long or empty is code 5, and nothing half of it goes out",
          "[console_variables]")
{
    console_up();
    s_label = "abcdefghijklmnopqrstuvwxyz0123456";   /* 33 */
    ask("variable name=\"label\"");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=label"));
    TEST_ASSERT_FALSE(replied(">|variable="));
    s_label = "";
    ask("variable name=\"label\"");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=label"));
    TEST_ASSERT_FALSE(replied(">|variable="));
}

/* ---- The parser's own refusals ---------------------------------------------- */

TEST_CASE("a variable takes no value: writing one is not a thing", "[console_variables]")
{
    console_up();
    ask("variable name=\"temp_dc\" value=5");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    TEST_ASSERT_FALSE(replied(">|variable="));
    /* `arg=` is a function's key, not a variable's. */
    ask("variable name=\"temp_dc\" arg=5");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
}

TEST_CASE("an unknown variable is code 3 msg=name, and name is required",
          "[console_variables]")
{
    console_up();
    ask("variable name=\"nosuch\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=name"));
    ask("variable");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=name"));
}

/* ---- watch ------------------------------------------------------------------
 * Every case here stops what it started before it returns: AO_Console lives
 * for the whole binary, so a watch left running would tick into somebody
 * else's capture.
 */

/** @brief 100 ms is ten QP ticks; the watch floor, and the period used here. */
#define WATCH_TICKS  10U

static void watches_off(void)
{
    static const char *const ROWS[] = { "temp_dc", "led", "mode", "label", "f0", "f1" };
    for ( unsigned i = 0U; (sizeof(ROWS) / sizeof(ROWS[0])) > i; ++i )
    {
        char line[40];
        (void)snprintf(line, sizeof(line), "variable name=\"%s\" watch=0", ROWS[i]);
        type_line(line);
    }
    fake_klog_reset();
}

TEST_CASE("watch answers >ok at once and a record every period", "[console_variables]")
{
    console_up();
    watches_off();
    ask("variable name=\"temp_dc\" watch=100");
    TEST_ASSERT_TRUE(line_is(0U, ">ok"));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());

    fake_klog_reset();
    tick(WATCH_TICKS - 1U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "a record before the period ran");
    tick(1U);
    TEST_ASSERT_TRUE(line_is(0U, ">|variable=\"temp_dc\" value=235"));
    s_temp = 236;
    tick(WATCH_TICKS * 2U);
    TEST_ASSERT_EQUAL_UINT(3U, lines_with(">|variable=\"temp_dc\" value="));
    TEST_ASSERT_TRUE(line_is(2U, ">|variable=\"temp_dc\" value=236"));
    /* No terminator ever follows a tick: the >ok was the accept. */
    TEST_ASSERT_EQUAL_UINT(3U, fake_klog_count());

    ask("variable name=\"temp_dc\" watch=0");
    TEST_ASSERT_TRUE(replied(">ok"));
    fake_klog_reset();
    tick(WATCH_TICKS * 3U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    watches_off();
}

TEST_CASE("variable_list reports the period of a followed row", "[console_variables]")
{
    console_up();
    watches_off();
    ask("variable name=\"led\" watch=500");
    ask("variable_list");
    tick(2U);
    TEST_ASSERT_TRUE(replied(">|variable=\"led\" watch=500 desc="));
    TEST_ASSERT_TRUE(replied(">|variable=\"temp_dc\" watch=0 desc="));
    watches_off();
}

TEST_CASE("a second watch on the same row changes its period in place",
          "[console_variables]")
{
    console_up();
    watches_off();
    ask("variable name=\"temp_dc\" watch=100");
    ask("variable name=\"temp_dc\" watch=200");
    TEST_ASSERT_TRUE(replied(">ok"));
    fake_klog_reset();
    tick(WATCH_TICKS);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "the old period still fires");
    tick(WATCH_TICKS);
    TEST_ASSERT_EQUAL_UINT(1U, lines_with(">|variable=\"temp_dc\" value="));
    /* And it took one slot, not two: three more fit. */
    ask("variable name=\"led\" watch=100");   TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"mode\" watch=100");  TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"label\" watch=100"); TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"f0\" watch=100");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=slots"));
    watches_off();
}

TEST_CASE("watch=0 frees a slot, and stopping an unwatched row is still >ok",
          "[console_variables]")
{
    console_up();
    watches_off();
    ask("variable name=\"temp_dc\" watch=100");
    ask("variable name=\"led\" watch=100");
    ask("variable name=\"mode\" watch=100");
    ask("variable name=\"label\" watch=100");
    ask("variable name=\"f0\" watch=100");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=slots"));
    ask("variable name=\"led\" watch=0");
    TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"f0\" watch=100");
    TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"f1\" watch=0");
    TEST_ASSERT_TRUE(replied(">ok"));
    watches_off();
}

TEST_CASE("watch is refused where it cannot mean anything", "[console_variables]")
{
    console_up();
    watches_off();
    /* Stopping a row nobody follows is still the truth the host asked for. */
    ask("variable name=\"f1\" watch=0");
    TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"temp_dc\" watch=100 value=5"); /* not a variable's key */
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    ask("variable name=\"temp_dc\" watch=99");          /* under the floor */
    TEST_ASSERT_TRUE(replied(">err code=3 msg=watch"));
    ask("variable name=\"temp_dc\" watch=60001");       /* over the spec's cap */
    TEST_ASSERT_TRUE(replied(">err code=3 msg=watch"));
    fake_klog_reset();
    tick(WATCH_TICKS * 2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "a refused watch is ticking");
    watches_off();
}

TEST_CASE("a get that fails ends the watch and says so once", "[console_variables]")
{
    console_up();
    watches_off();
    ask("variable name=\"temp_dc\" watch=100");
    fake_klog_reset();
    tick(WATCH_TICKS);
    TEST_ASSERT_EQUAL_UINT(1U, lines_with(">|variable=\"temp_dc\" value="));
    s_temp_rc = KODE_IO_ERROR;
    fake_klog_reset();
    tick(WATCH_TICKS);
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(">|variable="));
    TEST_ASSERT_TRUE(replied("!W"));
    TEST_ASSERT_TRUE(replied("temp_dc: watch stopped"));
    fake_klog_reset();
    tick(WATCH_TICKS * 3U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(), "the watch outlived its source");
    /* The slot is free again. */
    ask("variable name=\"led\" watch=100");   TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"mode\" watch=100");  TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"label\" watch=100"); TEST_ASSERT_TRUE(replied(">ok"));
    ask("variable name=\"f0\" watch=100");    TEST_ASSERT_TRUE(replied(">ok"));
    watches_off();
}

TEST_CASE("a tick lands between another command's records, and that command still closes",
          "[console_variables]")
{
    console_up();
    watches_off();
    /* help is sliced at 12 verbs a tick and the table is over a hundred
     * verbs long, so typed five ticks into the watch's period it is still
     * emitting when the watch fires at tick ten. */
    ask("variable name=\"temp_dc\" watch=100");
    tick(5U);
    ask("help");
    tick(5U);
    TEST_ASSERT_TRUE(replied(">|cmd=variable_list args=-"));
    TEST_ASSERT_TRUE(replied(">|variable=\"temp_dc\" value="));
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "help closed before the tick could interleave");
    /* The record is out while help is still mid-listing, which is the whole
     * claim. The close is looked for a tick at a time because the sink keeps
     * eighty lines and help is longer than that: one window cannot hold both
     * ends of this listing, and a verb more or less must not decide it. */
    bool closed = false;
    for ( unsigned t = 0U; ((WATCH_TICKS * 2U) > t) && !closed; ++t )
    {
        fake_klog_reset();
        tick(1U);
        closed = replied(">ok");
    }
    TEST_ASSERT_TRUE(closed);
    watches_off();
}
