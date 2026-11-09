/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_answers.c
 * @brief Verbs that used to answer only in the log answer in records.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kode_functions.h"
#include "kregistry.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief How long the answering rows below keep the line open, in ms. */
#define ANSWER_MS  2000U

#define SERIAL_SRC  ((uint8_t)KODE_CONSOLE_SRC_SERIAL)
#define NET_SRC     ((uint8_t)KODE_CONSOLE_SRC_NET)

/* ---- The rows under test ---------------------------------------------------- */

static unsigned      s_asks;                     /**< times `ask_one` ran     */
static kode_status_t s_ask_rc = KODE_OK;         /**< what `ask_one` answers  */
static char          s_arg[KODE_ARG_STR_MAX + 1U];
static uint32_t      s_ticket;                   /**< what `ask_one` took    */

static kode_status_t run_ask(const kode_value_t *a)
{
    ++s_asks;
    s_ticket = function_ticket();
    (void)snprintf(s_arg, sizeof(s_arg), "%s", "-");
    if ( NULL != a )
    {
        (void)snprintf(s_arg, sizeof(s_arg), "%s", a->s);
    }
    return s_ask_rc;
}

static kode_status_t run_plain(const kode_value_t *a)
{
    (void)a;
    return KODE_OK;
}

static const kode_function_t ANSWERS[] = {
    { .name = "ask_one", .arg = KODE_TYPE_STRING, .run = run_ask,
      .desc = "Answers in records", .answer_ms = ANSWER_MS },
    { .name = "plain_one", .run = run_plain, .desc = "Answers >ok" },
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
    if ( kfunctions_find("ask_one") != &ANSWERS[0] )
    {
        kfunctions_reset();
        TEST_ASSERT_EQUAL_INT(KODE_OK, KODE_FUNCTIONS_ADD(ANSWERS));
    }
    s_asks   = 0U;
    s_ask_rc = KODE_OK;
}

static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
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

/** @brief The port tag of the first line containing @p needle; 0xFE if none. */
static uint8_t src_of(const char *needle)
{
    int const i = line_of(needle);

    if ( 0 > i )
    {
        return 0xFEU;
    }
    return fake_klog_src((unsigned)i);
}

/** @brief Let AO_Console take what the answering AO posted. */
static void settle(void)
{
    (void)qf_support_settle(AO_Console);
}

/* ---- A function that answers ------------------------------------------------ */

TEST_CASE("an answering function holds the line open until the owner ends it",
          "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_EQUAL_UINT(1U, s_asks);
    TEST_ASSERT_EQUAL_STRING("x", s_arg);
    /* Accepted is not answered: no `>ok` yet, and the console is busy. */
    TEST_ASSERT_FALSE(replied(">ok"));
    type_from(SERIAL_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(replied(">|pong"));

    TEST_ASSERT_TRUE(function_record(s_ticket, "ask one=1"));
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask two=\"b\""));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_OK, NULL));
    settle();

    TEST_ASSERT_TRUE(replied(">|ask one=1"));
    TEST_ASSERT_TRUE(replied(">|ask two=\"b\""));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_TRUE(line_of(">|ask one=1") < line_of(">|ask two=\"b\""));
    TEST_ASSERT_TRUE(line_of(">|ask two=\"b\"") < line_of(">ok"));

    /* Free again. */
    fake_klog_reset();
    type_from(SERIAL_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">|pong"));
}

TEST_CASE("an answering function refused by its run answers at once",
          "[console_answers]")
{
    console_up();
    s_ask_rc = KODE_DENIED;
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=arg"));
    fake_klog_reset();
    type_from(SERIAL_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">|pong"));

    s_ask_rc = KODE_NO_RESOURCES;
    fake_klog_reset();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=ask_one"));
    fake_klog_reset();
    type_from(SERIAL_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">|pong"));
}

TEST_CASE("a function ended with a failure closes with code 5 after its records",
          "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask result=refused why=busy"));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_ERROR, NULL));
    settle();
    TEST_ASSERT_TRUE(replied(">|ask result=refused why=busy"));
    TEST_ASSERT_TRUE(replied(">err code=5 msg=ask_one"));
    TEST_ASSERT_FALSE(replied(">ok"));
    TEST_ASSERT_TRUE(line_of(">|ask result=refused") < line_of(">err code=5"));
}

TEST_CASE("a failure can name its own word after msg=", "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask result=refused name=\"x\""));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_ERROR, "ask"));
    settle();
    TEST_ASSERT_TRUE(replied(">err code=5 msg=ask"));
    TEST_ASSERT_FALSE(replied("msg=ask_one"));
}

TEST_CASE("an owner that never answers is code 6 after answer_ms, and a late record is dropped",
          "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    (void)qf_support_tick(AO_Console, ((ANSWER_MS * 100U) / 1000U) + 1U);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=ask_one"));

    fake_klog_reset();
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask late=1"));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_OK, NULL));
    settle();
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    type_from(SERIAL_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">|pong"));
}

TEST_CASE("the late answer of a function that timed out cannot close the next one",
          "[console_answers]")
{
    uint32_t a_ticket;

    console_up();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"a\"");
    a_ticket = s_ticket;
    (void)qf_support_tick(AO_Console, ((ANSWER_MS * 100U) / 1000U) + 1U);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=ask_one"));

    fake_klog_reset();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"b\"");
    TEST_ASSERT_TRUE(s_ticket != a_ticket);
    /* A's owner wakes up late, with A's ticket. */
    TEST_ASSERT_TRUE(function_record(a_ticket, "ask late=a"));
    TEST_ASSERT_TRUE(function_end(a_ticket, KODE_OK, NULL));
    settle();
    TEST_ASSERT_FALSE(replied("late=a"));
    TEST_ASSERT_FALSE(replied(">ok"));
    /* B is still in flight, and its own answer closes it. */
    type_from(SERIAL_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask b=1"));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_OK, NULL));
    settle();
    TEST_ASSERT_TRUE(replied(">|ask b=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a record that is NULL or does not fit is refused, never cut",
          "[console_answers]")
{
    char big[KODE_FUNCTION_REC_MAX + 8U];
    char fits[KODE_FUNCTION_REC_MAX];

    console_up();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_FALSE(function_record(s_ticket, NULL));
    (void)memset(big, 'x', sizeof(big));
    big[sizeof(big) - 1U] = '\0';
    TEST_ASSERT_FALSE(function_record(s_ticket, big));
    (void)memset(fits, 'y', sizeof(fits));
    fits[sizeof(fits) - 1U] = '\0';
    TEST_ASSERT_TRUE(function_record(s_ticket, fits));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_OK, NULL));
    settle();
    TEST_ASSERT_TRUE(replied(fits));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a record with no function waiting prints nothing", "[console_answers]")
{
    console_up();
    s_ticket = 1U;
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask nobody=1"));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_OK, NULL));
    settle();
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

TEST_CASE("a function without answer_ms is accepted with >ok as before",
          "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "function name=\"plain_one\"");
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_EQUAL_UINT(1U, fake_klog_count());
}

TEST_CASE("the records of an answer carry the port of their command",
          "[console_answers]")
{
    console_up();
    type_from(NET_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask over=net"));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_OK, NULL));
    settle();
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">|ask over=net"));
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">ok"));

    fake_klog_reset();
    type_from(SERIAL_SRC, "function name=\"ask_one\" arg=\"x\"");
    TEST_ASSERT_TRUE(function_record(s_ticket, "ask over=cable"));
    TEST_ASSERT_TRUE(function_end(s_ticket, KODE_OK, NULL));
    settle();
    TEST_ASSERT_EQUAL_UINT8(SERIAL_SRC, src_of(">|ask over=cable"));
    TEST_ASSERT_EQUAL_UINT8(SERIAL_SRC, src_of(">ok"));
}

TEST_CASE("function_quote writes a text the way every record does", "[console_answers]")
{
    char q[FUNCTION_QUOTED_MAX(16U)];

    TEST_ASSERT_EQUAL_STRING("\"plain\"", function_quote(q, sizeof(q), "plain"));
    TEST_ASSERT_EQUAL_STRING("\"a\\\"b\\\\c\"", function_quote(q, sizeof(q), "a\"b\\c"));
    TEST_ASSERT_EQUAL_STRING("\"\"", function_quote(q, sizeof(q), NULL));
}

/* ---- gui_info --------------------------------------------------------------- */

/** @brief Hand the console the event AO_Gui answers gui_info with. */
static void gui_answers(const char *sel, const char *sheet, const char *kody,
                        uint32_t lock_s, uint32_t stack_free)
{
    GuiStateEvt g;

    (void)memset(&g, 0, sizeof(g));
    g.super = (QEvt)QEVT_INITIALIZER(GUI_STATE_SIG);
    (void)snprintf(g.state, sizeof(g.state), "%s", "browsing");
    g.screen_up = true;
    g.pills     = 3U;
    g.installed = 2U;
    g.files     = 1U;
    g.level     = KODE_GUI_LEVEL_GROUP;
    (void)snprintf(g.group, sizeof(g.group), "%s", "games");
    (void)snprintf(g.sel, sizeof(g.sel), "%s", sel);
    (void)snprintf(g.sheet, sizeof(g.sheet), "%s", sheet);
    (void)snprintf(g.kody, sizeof(g.kody), "%s", kody);
    g.lock_s     = lock_s;
    g.stack_free = stack_free;
    QASM_DISPATCH(AO_Console, &g.super, 0U);
}

TEST_CASE("gui_info keeps its first record and adds the gui record before >ok",
          "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "gui_info");
    gui_answers("Snake \"Pro\"", "wifi", "playing", 120U, 3608U);

    TEST_ASSERT_TRUE(replied(">|state=browsing lvgl=on pills=3 installed=2 files=1 writing=off"));
    TEST_ASSERT_TRUE(replied(">|gui state=browsing sel=\"Snake \\\"Pro\\\"\" level=group group=\"games\" sheet=wifi kody=playing lock_s=120 stack_free=3608"));
    TEST_ASSERT_TRUE(line_of(">|state=") < line_of(">|gui "));
    TEST_ASSERT_TRUE(line_of(">|gui ") < line_of(">ok"));
}

/** @brief Hand the console a gui_info answer whose gesture record is full:
 *         taps 1, claims 2, ... blocked 8, and @p drag as the phase. */
static void gui_gesture_answers(uint8_t drag)
{
    GuiStateEvt g;

    (void)memset(&g, 0, sizeof(g));
    g.super = (QEvt)QEVT_INITIALIZER(GUI_STATE_SIG);
    (void)snprintf(g.state, sizeof(g.state), "%s", "browsing");
    g.gesture.drag     = drag;
    g.gesture.taps     = 1U;
    g.gesture.claims   = 2U;
    g.gesture.commits  = 3U;
    g.gesture.shorts   = 4U;
    g.gesture.diagonal = 5U;
    g.gesture.axis     = 6U;
    g.gesture.zone     = 7U;
    g.gesture.blocked  = 8U;
    QASM_DISPATCH(AO_Console, &g.super, 0U);
}

TEST_CASE("gui_info prints the gesture record after the gui record", "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "gui_info");
    gui_gesture_answers(KODE_GUI_DRAG_SETTLE);

    TEST_ASSERT_TRUE(replied(">|gesture drag=settle taps=1 claims=2 commits=3 shorts=4 diagonal=5 axis=6 zone=7 blocked=8"));
    TEST_ASSERT_TRUE(line_of(">|gui ") < line_of(">|gesture "));
    TEST_ASSERT_TRUE(line_of(">|gesture ") < line_of(">ok"));
}

TEST_CASE("gui_info names the idle, pending and follow phases", "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "gui_info");
    gui_gesture_answers(KODE_GUI_DRAG_IDLE);
    TEST_ASSERT_TRUE(replied(">|gesture drag=idle taps=1"));

    fake_klog_reset();
    type_from(SERIAL_SRC, "gui_info");
    gui_gesture_answers(KODE_GUI_DRAG_PENDING);
    TEST_ASSERT_TRUE(replied(">|gesture drag=pending taps=1"));

    fake_klog_reset();
    type_from(SERIAL_SRC, "gui_info");
    gui_gesture_answers(KODE_GUI_DRAG_FOLLOW);
    TEST_ASSERT_TRUE(replied(">|gesture drag=follow taps=1"));
}

TEST_CASE("an unknown drag phase prints a question mark", "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "gui_info");
    gui_gesture_answers((uint8_t)(KODE_GUI_DRAG_SETTLE + 1U));

    TEST_ASSERT_TRUE(replied(">|gesture drag=? taps=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("a gui answer with no gesture counted prints zeros and idle", "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "gui_info");
    gui_answers("", "", "none", 0U, 100U);

    TEST_ASSERT_TRUE(replied(">|gesture drag=idle taps=0 claims=0 commits=0 shorts=0 diagonal=0 axis=0 zone=0 blocked=0"));
}

TEST_CASE("gui_info says - for no selection and no sheet, and never for no lock",
          "[console_answers]")
{
    console_up();
    type_from(SERIAL_SRC, "gui_info");
    gui_answers("", "", "none", 0U, 100U);
    TEST_ASSERT_TRUE(replied(">|gui state=browsing sel=- level=group group=\"games\" sheet=- kody=none lock_s=never stack_free=100"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("gui_info says the root level with - for no group", "[console_answers]")
{
    GuiStateEvt g;

    console_up();
    type_from(SERIAL_SRC, "gui_info");
    (void)memset(&g, 0, sizeof(g));
    g.super = (QEvt)QEVT_INITIALIZER(GUI_STATE_SIG);
    (void)snprintf(g.state, sizeof(g.state), "%s", "browsing");
    g.level = KODE_GUI_LEVEL_ROOT;
    QASM_DISPATCH(AO_Console, &g.super, 0U);

    TEST_ASSERT_TRUE(replied(">|gui state=browsing sel=- level=root group=- sheet=- kody= lock_s=never stack_free=0"));
}

TEST_CASE("gui_info answers the Kode channel with the same records", "[console_answers]")
{
    console_up();
    type_from(NET_SRC, "gui_info");
    gui_answers("Snake", "", "asleep", 30U, 4000U);
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">|state=browsing"));
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">|gui state=browsing"));
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">ok"));
}

/* ---- app_cat ---------------------------------------------------------------- */

/** @brief Hand the console the answer AO_AppMgr gives an app_cat. */
static void app_cat_answers(kode_status_t status)
{
    AppCatRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(APP_CAT_RSP_SIG);
    r.status = (uint8_t)status;
    QASM_DISPATCH(AO_Console, &r.super, 0U);
}

TEST_CASE("an app_cat that is refused says the status in a record before its >err",
          "[console_answers]")
{
    console_up();
    type_from(NET_SRC, "app_cat name=\"x\" set=\"a\"");
    app_cat_answers(KODE_ERROR);
    TEST_ASSERT_TRUE(replied(">|app_cat result=refused status=error"));
    TEST_ASSERT_TRUE(replied(">err code=3 msg=app_cat"));
    TEST_ASSERT_TRUE(line_of(">|app_cat result=refused") < line_of(">err code=3"));
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">|app_cat result=refused"));

    fake_klog_reset();
    type_from(SERIAL_SRC, "app_cat name=\"x\" set=\"a\"");
    app_cat_answers(KODE_DENIED);
    TEST_ASSERT_TRUE(replied(">|app_cat result=refused status=denied"));
    TEST_ASSERT_TRUE(replied(">err code=5 msg=app_cat"));
}
