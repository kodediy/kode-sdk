/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_functions.c
 * @brief `function` and `function_list`: an app's functions, from the wire.
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
#include "kode_functions.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/* ---- The app under test ----------------------------------------------------- */

static unsigned      s_blinks;
static int32_t       s_moved;
static kode_status_t s_move_rc  = KODE_OK;     /**< what `move`'s run answers */
static uint8_t       s_fan;
static char          s_said[KODE_ARG_STR_MAX + 1U];

static kode_status_t run_blink(const kode_value_t *a){ (void)a; ++s_blinks; return KODE_OK; }
static kode_status_t run_move(const kode_value_t *a) { if ( NULL == a ) { return KODE_DENIED; } s_moved = a->i; return s_move_rc; }
static kode_status_t run_fan(const kode_value_t *a)  { if ( NULL == a ) { return KODE_DENIED; } s_fan = a->e; return KODE_OK; }
static kode_status_t run_say(const kode_value_t *a)  { (void)snprintf(s_said, sizeof(s_said), "%s", (NULL != a) ? a->s : "-"); return KODE_OK; }
static kode_status_t run_filler(const kode_value_t *a){ (void)a; return KODE_OK; }

static const char *const SPEEDS[] = { "off", "slow", "fast" };

/* Fourteen rows: KCLI_SLICE is 12, so function_list needs two slices. The
 * names stay clear of test_console_variables.c's: both tables can be in at
 * once, and a name is one kind or the other. */
static const kode_function_t FUNCTIONS[] = {
    { .name = "blink", .run = run_blink, .desc = "Blink the LED once" },
    { .name = "move",  .arg = KODE_TYPE_INT, .run = run_move, .desc = "Move by arg" },
    { .name = "fan",   .arg = KODE_TYPE_ENUM, .enums = SPEEDS, .n_enums = 3U,
      .run = run_fan, .desc = "Spin the fan" },
    { .name = "say",   .arg = KODE_TYPE_STRING, .run = run_say },
    { .name = "x0", .run = run_filler },
    { .name = "x1", .run = run_filler },
    { .name = "x2", .run = run_filler },
    { .name = "x3", .run = run_filler },
    { .name = "x4", .run = run_filler },
    { .name = "x5", .run = run_filler },
    { .name = "x6", .run = run_filler },
    { .name = "x7", .run = run_filler },
    { .name = "x8", .run = run_filler },
    { .name = "x9", .run = run_filler },
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
    /* This file's table and nothing else among the functions: another file
     * may have left one of its own, or emptied the registry. */
    if ( kfunctions_find("blink") != &FUNCTIONS[0] )
    {
        kfunctions_reset();
        TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_FUNCTIONS_ADD(FUNCTIONS));
    }
    s_blinks = 0U; s_moved = 0; s_move_rc = KODE_OK; s_fan = 0U;
    (void)snprintf(s_said, sizeof(s_said), "%s", "");
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

/* ---- function_list ---------------------------------------------------------- */

TEST_CASE("function_list names every row, sliced, with its argument and desc last",
          "[console_functions]")
{
    console_up();
    ask("function_list");
    /* The first slice went out at once; the second waits a tick. */
    TEST_ASSERT_FALSE(replied(">ok"));
    tick(2U);
    TEST_ASSERT_TRUE(replied(">|function=\"blink\" arg=none desc=\"Blink the LED once\""));
    TEST_ASSERT_TRUE(replied(">|function=\"move\" arg=int min=0 max=0 desc=\"Move by arg\""));
    TEST_ASSERT_TRUE(replied(">|function=\"fan\" arg=enum enum=off,slow,fast desc=\"Spin the fan\""));
    TEST_ASSERT_TRUE(replied(">|function=\"say\" arg=string desc=-"));
    TEST_ASSERT_EQUAL_UINT(14U, lines_with(">|function="));
    /* Nothing about reading or following: a function is run and that is all. */
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(" watch="));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE(line_of(">|function=\"x9\"") < line_of(">ok"));
}

TEST_CASE("function_list with no rows is a bare >ok", "[console_functions]")
{
    console_up();
    kfunctions_reset();
    ask("function_list");
    TEST_ASSERT_EQUAL_UINT(0U, lines_with(">|function="));
    TEST_ASSERT_TRUE(replied(">ok"));
    /* Put the table back for everyone after us. */
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_FUNCTIONS_ADD(FUNCTIONS));
}

/* ---- run ------------------------------------------------------------------- */

TEST_CASE("a bare function runs it, and answers nothing but >ok", "[console_functions]")
{
    console_up();
    ask("function name=\"blink\"");
    TEST_ASSERT_EQUAL_UINT(1U, s_blinks);
    TEST_ASSERT_TRUE(line_is(0U, ">ok"));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
}

TEST_CASE("a function with an argument gets it, and refuses NULL as a bad arg",
          "[console_functions]")
{
    console_up();
    ask("function name=\"move\" arg=10");
    TEST_ASSERT_EQUAL_INT32(10, s_moved);
    TEST_ASSERT_TRUE(line_is(0U, ">ok"));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
    ask("function name=\"move\" arg=-7");
    TEST_ASSERT_EQUAL_INT32(-7, s_moved);
    ask("function name=\"move\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
    s_move_rc = KODE_TIMEOUT;
    ask("function name=\"move\" arg=1");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=move"));
}

TEST_CASE("enum and string arguments arrive typed", "[console_functions]")
{
    console_up();
    ask("function name=\"fan\" arg=fast");
    TEST_ASSERT_EQUAL_UINT8(2U, s_fan);
    TEST_ASSERT_TRUE(replied(">ok"));
    ask("function name=\"say\" arg=\"hello\"");
    TEST_ASSERT_EQUAL_STRING("hello", s_said);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a string argument longer than a variable's word arrives whole",
          "[console_functions]")
{
    console_up();
    ask("function name=\"say\" arg=\"https://assets.kode.diy/kody/v1/kode.json\"");
    TEST_ASSERT_EQUAL_STRING("https://assets.kode.diy/kody/v1/kode.json", s_said);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("an argument that does not fit is code 3 msg=arg, and nothing runs",
          "[console_functions]")
{
    console_up();
    ask("function name=\"move\" arg=abc");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
    TEST_ASSERT_EQUAL_INT32(0, s_moved);
    ask("function name=\"fan\" arg=faster");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
    TEST_ASSERT_EQUAL_UINT8(0U, s_fan);
    /* An argument handed to a row that takes none. */
    ask("function name=\"blink\" arg=1");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
    TEST_ASSERT_EQUAL_UINT(0U, s_blinks);
    /* The wrong form for the row's type: a string row takes its argument
     * quoted and nothing else does, so each form is refused where the other
     * one belongs. */
    ask("function name=\"say\" arg=hello");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
    TEST_ASSERT_EQUAL_STRING("", s_said);
    ask("function name=\"move\" arg=\"10\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
    TEST_ASSERT_EQUAL_INT32(0, s_moved);
}

/* ---- The parser's own refusals ---------------------------------------------- */

TEST_CASE("an unknown function is code 3 msg=name, and name is required",
          "[console_functions]")
{
    console_up();
    ask("function name=\"nosuch\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=name"));
    ask("function");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=name"));
}

TEST_CASE("value= and watch= are a variable's keys, and a function refuses them unrun",
          "[console_functions]")
{
    console_up();
    ask("function name=\"blink\" value=1");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    ask("function name=\"blink\" watch=100");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=watch"));
    TEST_ASSERT_EQUAL_UINT(0U, s_blinks);
}

/* ---- The two kinds never cross ---------------------------------------------- */

static kode_status_t get_level(char *buf, size_t cap)
{
    (void)snprintf(buf, cap, "3");
    return KODE_OK;
}

TEST_CASE("a variable's name is not a function's, and a function's is not a variable's",
          "[console_functions]")
{
    static const kode_variable_t LEVEL[] = {
        { .name = "level", .get = get_level },
    };
    console_up();
    kvariables_reset();
    TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_VARIABLES_ADD(LEVEL));

    /* The whole point: asking for a function by a variable's name runs
     * nothing, and asking for a variable by a function's name reads nothing -
     * and in particular does not run it. */
    ask("function name=\"level\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=name"));
    ask("variable name=\"blink\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=name"));
    TEST_ASSERT_EQUAL_UINT(0U, s_blinks);
    ask("variable name=\"level\"");
    TEST_ASSERT_TRUE(replied(">|variable=\"level\" value=3"));

    /* test_console_variables.c puts its own table back when it finds this
     * one in its place. */
    kvariables_reset();
}
