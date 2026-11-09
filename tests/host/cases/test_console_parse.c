/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_parse.c
 * @brief The console's parser: the half of AO_Console that is pure text.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "console_grammar.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kode_log.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* Tokens per line, verb included. Repeated here rather than imported - it is
 * private to console_ao.c, and a test that shared the parser's own #define
 * would agree with it by construction. Eight means a verb and seven keys; the
 * ninth token is what the cases below are about. */
#define TOKEN_MAX 8

/* The command watchdog, in QP ticks - repeated rather than imported for the
 * same reason test_console_power.c repeats it. nfc_write's request never
 * answers in this file (AO_Nfc is given a queue, not a dispatcher), so every
 * case that validates one leaves AO_Console in flight; without expiring that
 * watchdog before the case ends, the NEXT case - one of this file's own, or
 * the "Busy" section's `fs_ls`, which is documented as leaving the ONE
 * command this file ends busy on - would start against a console that was
 * already busy for a reason of this file's own making, not that section's. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/**
 * @brief Bring AO_Console up, once for the whole binary.
 *
 * Once, because QP's registry has no undo. The AO keeps one piece of state
 * that matters between cases - `inflight`, the command it is waiting on a
 * terminator for - and every case below either starts from idle or is about
 * what happens when it is not.
 */
static void console_up_once(void)
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
        /* cmd_begin() arms a watchdog QTimeEvt for every command that waits
         * on an owner, and the cases below deliberately leave one waiting.
         * The tick inside qf_support_pump() is global, so without this that
         * timer would post into a queue nobody drains for the rest of the
         * run - and some unrelated file would die of a full queue. */
        qf_support_background(AO_Console);
    }

    /* AO_Nfc is never dispatched here - this file stays about the parsing
     * gate, not what a handler does once it runs - but nfc_write's `text=`
     * is a KARG_TEXT whose line goes in flight once it validates, which
     * makes it this file's way to see a quoted value ACCEPTED through a
     * genuine line. Giving it a queue is enough: a post to an Active Object with no queue aborts inside
     * FreeRTOS rather than failing a case, and draining it after keeps one
     * case's request from leaking into the next one's assertions. */
    qf_support_give_queue(AO_Nfc);
    {
        const QEvt *e;
        while ( xQueueReceive(AO_Nfc->eQueue, &e, 0U) == pdTRUE )
        {
            QF_gc(e);
        }
    }

    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Send one line in, exactly as AO_Log publishes it. */
static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Every reply the console produced, joined, for a substring search. */
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


/** @brief How many lines the console produced at all. */
static unsigned replies(void)
{
    return fake_klog_count();
}

/* ---- Splitting a line ------------------------------------------------------ */

/* Pressing return at an idle prompt is not a command. Answering it with
 * anything at all would make a stray keystroke look like a failure, and a
 * host that sends a blank line between commands would collect a terminator
 * it never asked for. */
TEST_CASE("an empty line is not a command, and is not an error",
          "[console_parse]")
{
    console_up_once();

    type_line("");
    type_line("   ");
    type_line("\t \t");

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, replies(),
        "a blank line produced a response a script would have to consume");
}

/* Tabs separate tokens as well as spaces, because a terminal expands one and
 * a script may send the other. */
TEST_CASE("tabs separate tokens as well as spaces", "[console_parse]")
{
    console_up_once();

    type_line("\tnosuchverb\t");

    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=1 msg=nosuchverb"),
        "the verb was not found once tabs were in play");
}

/* An unknown verb is named back. A bare refusal would leave the operator
 * guessing whether the console heard them at all. */
TEST_CASE("an unknown verb is refused, and named", "[console_parse]")
{
    console_up_once();

    type_line("frobnicate key=value");

    TEST_ASSERT_TRUE(replied(">err code=1 msg=frobnicate"));
}

/* Verbs are matched exactly. A prefix that happens to start a real verb is
 * not that verb, or `ps` would answer for `psx`. */
TEST_CASE("a verb is matched whole, never by prefix", "[console_parse]")
{
    console_up_once();

    type_line("p");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=p"));

    fake_klog_reset();
    type_line("psx");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=psx"));
}

/* The ninth token. It would be dropped by the split, and a token silently
 * dropped is a protocol lie - the command would run having ignored part of
 * what was typed. No command declares that many keys, so it is refused the
 * way an unknown key is. */
TEST_CASE("a token past the limit is refused, never dropped",
          "[console_parse]")
{
    char line[KODE_CONSOLE_LINE_MAX];
    int  n = snprintf(line, sizeof(line), "ps");

    console_up_once();

    /* The verb plus TOKEN_MAX keys: one more token than the splitter holds. */
    for ( int i = 0; TOKEN_MAX > i; ++i )
    {
        n += snprintf(&line[n], sizeof(line) - (size_t)n, " k%d=v", i);
    }
    type_line(line);

    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3"),
        "the extra token was dropped and the command ran anyway");
}

/* And the token it names is the KEY, not the whole `key=value` pair: the
 * value may be a secret, and the protocol's own grammar has no room for an
 * '=' inside a msg field. */
TEST_CASE("the refused token is reported as its key alone", "[console_parse]")
{
    char line[KODE_CONSOLE_LINE_MAX];
    int  n = snprintf(line, sizeof(line), "ps");

    console_up_once();

    /* Fill the splitter exactly - verb plus TOKEN_MAX-1 keys - so the pair
     * below is the first token it cannot hold, and therefore the one named. */
    for ( int i = 0; (TOKEN_MAX - 1) > i; ++i )
    {
        n += snprintf(&line[n], sizeof(line) - (size_t)n, " k%d=v", i);
    }
    n += snprintf(&line[n], sizeof(line) - (size_t)n, " secret=hunter2");
    type_line(line);

    TEST_ASSERT_TRUE(replied("msg=secret"));
    TEST_ASSERT_FALSE_MESSAGE(replied("hunter2"),
        "the value travelled back out in the refusal");
}

/* ---- Checking the keys ------------------------------------------------------ */

/* Every argument is key=value. A bare word is refused rather than guessed at
 * as a positional argument, because two commands would then disagree about
 * what position means. */
TEST_CASE("an argument with no '=' is refused, and named", "[console_parse]")
{
    console_up_once();

    type_line("log_level debug");

    TEST_ASSERT_TRUE(replied(">err code=2 msg=debug"));
}

/**
 * @brief The one LogLevelEvt the console left for AO_Log, taken off its
 *        queue, or a failed case when there is not exactly one.
 */
static LogLevelEvt level_asked(void)
{
    LogLevelEvt out;
    const QEvt *e = NULL;

    (void)memset(&out, 0, sizeof(out));
    TEST_ASSERT_EQUAL_UINT(1U, fake_neighbour_log_posts());
    TEST_ASSERT_EQUAL_INT(pdTRUE, xQueueReceive(AO_Log->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT(LOG_LEVEL_SET_SIG, e->sig);
    out = *(LogLevelEvt const *)e;
    QF_gc(e);
    return out;
}

/* AO_Log owns the level, so the verb asks it and answers at once. One tag at
 * a time: the global level is a setting, `config key="log"`, so `*` and
 * `all` are refused and AO_Log is not asked. */
TEST_CASE("log_level asks AO_Log for one tag, and refuses `*` and `all`",
          "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"kaos.boot\" level=d");
    TEST_ASSERT_TRUE(replied(">ok"));
    LogLevelEvt l = level_asked();
    TEST_ASSERT_EQUAL_STRING("kaos.boot", l.tag);
    TEST_ASSERT_EQUAL_UINT8(KODE_LOG_DEBUG, l.level);

    type_line("log_level tag=\"*\" level=off");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));
    type_line("log_level tag=\"all\" level=n");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));
}

/* A key the command does not declare is refused. Ignoring it would run the
 * command as if the operator had not typed it. */
TEST_CASE("a key the command does not declare is refused", "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"kaos.boot\" level=d nosuchkey=1");

    TEST_ASSERT_TRUE(replied(">err code=3 msg=nosuchkey"));
}

/* A key given twice is refused, naming it: taking the last would run a
 * command the operator did not mean, and the first would hide the second. */
TEST_CASE("a key given twice is refused, naming the key", "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"kaos.boot\" tag=\"kaos.net\" level=d");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));
    type_line("log_level tag=\"kaos.boot\" level=d level=e");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=level"));
}

/* Only the three escapes exist; a backslash before anything else is a
 * malformed value, refused by its key like an unterminated quote. */
TEST_CASE("an unknown escape is refused, naming the key", "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"kaos\\q\" level=d");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));
    type_line("log_level tag=\"kaos\\\\boot\" level=d");
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* A required key that is absent is refused BY NAME - the key's, not the
 * verb's. An operator who mistyped one key should not have to re-read the
 * help to find out which. */
TEST_CASE("a missing required key is named, not just reported",
          "[console_parse]")
{
    console_up_once();

    type_line("log_level level=d");

    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=2 msg=tag"),
        "the refusal did not say which key was missing");
}

/* An enum takes the words it declares and nothing else. A value outside the
 * set is a typo, and mapping it to a default would run something other than
 * what was asked. */
TEST_CASE("a value outside an enum is refused, naming its key",
          "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"kaos.boot\" level=purple");

    TEST_ASSERT_TRUE(replied(">err code=3 msg=level"));
}

/* An empty value is not a value. `key=` reads as a key the operator meant to
 * fill in and did not. */
TEST_CASE("an empty value is refused", "[console_parse]")
{
    console_up_once();

    type_line("log_level tag= level=d");

    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));
}

/* ---- Quoted strings ---------------------------------------------------------- */

/* nfc_write is used for the cases that need a line to be ACCEPTED: its
 * `text=` is a KARG_TEXT and its request goes to AO_Nfc, which only has a
 * queue in this file - console_up_once() gives it one so the post does not
 * abort, and drains it so one case's request cannot be mistaken for the
 * next one's. So every case below reaches exactly as far as cmd_validate()
 * does: no `>err` means the line parsed, and whatever nfc_write goes on to do
 * with it is somebody else's file to test. test_console_grammar.c is where
 * the bytes a value arrives as are compared.
 *
 * These want "no refusal happened", which is why they use replied() rather
 * than the end-anchored check test_console_fs.c relies on for a VALUE
 * assertion: an anchor is the wrong tool for an ABSENCE. ">err code=" is a
 * prefix, not a whole line - a real refusal is always
 * ">err code=<n> msg=<token>" - so anchoring it to end-of-line would make it
 * match nothing, ever, and the assertion would read as passing no matter
 * what the console did. */

TEST_CASE("a quoted text keeps its spaces and its equals signs",
          "[console_parse]")
{
    console_up_once();

    type_line("nfc_write text=\"hello there = world\"");
    TEST_ASSERT_FALSE(replied(">err code="));

    /* A validated nfc_write went in flight - see above - and has to be
     * expired before the next case runs, or it starts against a console
     * that is busy for no reason of its own. */
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS);
}

TEST_CASE("an empty quoted text is a value, not a missing key",
          "[console_parse]")
{
    console_up_once();

    type_line("nfc_write text=\"\"");
    TEST_ASSERT_FALSE(replied(">err code="));

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS);
}

/* The token cap counts keys, never words of text: however many spaces a
 * quoted value carries, it is one token. */
TEST_CASE("a quoted value is one token, however many words it holds",
          "[console_parse]")
{
    console_up_once();

    type_line("nfc_write text=\"one two three four five six seven eight "
              "nine ten\"");
    TEST_ASSERT_FALSE(replied(">err code="));

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS);
}

/* The one rule of the grammar, from its first side: a string is quoted.
 * `tag=kaos.boot` is a typo, refused by its key, never guessed at. */
TEST_CASE("a string typed bare is refused, naming its key", "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=kaos.boot level=d");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));

    fake_klog_reset();
    type_line("nfc_write text=hello");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=text"));
}

/* And from its second side: nothing else is. A number, a boolean or an enum
 * member typed in quotes is refused the same way, so each value has exactly
 * one form. */
TEST_CASE("a number, a boolean or an enum typed in quotes is refused",
          "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"kaos.boot\" level=\"d\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=level"));

    fake_klog_reset();
    type_line("nfc_read ms=\"500\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
}

/* A quoted string may not be empty either: a name or a path that is nothing
 * is a key the operator meant to fill in. */
TEST_CASE("an empty quoted string is refused", "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"\" level=d");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));
}

/* A quote that never closes is refused by its key, and the value - which may
 * be a password - does not travel back out in the refusal. */
TEST_CASE("an unterminated quote is refused, and the value is not echoed",
          "[console_parse]")
{
    console_up_once();

    type_line("nvs_set key=\"k\" value=\"hunter2 and the rest");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=value"));
    TEST_ASSERT_FALSE_MESSAGE(replied("hunter2"),
        "the value travelled back out in the refusal");
}

/* A closing quote ends the value; bytes glued to it are a typo. */
TEST_CASE("bytes glued to a closing quote are refused", "[console_parse]")
{
    console_up_once();

    type_line("log_level tag=\"kaos\".boot level=d");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=tag"));
}

/* An unknown verb is named back as far as its `=`: a first token carrying a
 * quoted value would otherwise put the value's spaces into a `msg=` that is
 * one word by contract. */
TEST_CASE("an unknown verb carrying a quoted value is named by its key",
          "[console_parse]")
{
    console_up_once();

    type_line("frob=\"a b c\"");
    TEST_ASSERT_TRUE(replied(">err code=1 msg=frob"));
    TEST_ASSERT_FALSE(replied("a b c"));
}

/* console_specs_ok() is what Console_ctor() asserts: no row of the command
 * table declares more keys than cmd_validate()'s value array holds. */
TEST_CASE("every command's keys fit the value array", "[console_parse]")
{
    TEST_ASSERT_TRUE(console_specs_ok());
}

/* ---- Busy ------------------------------------------------------------------- */

/* A command that is still in flight makes the console answer busy to
 * ANYTHING that arrives, and that ordering is deliberate: the check runs
 * before the line is even looked at. A host that pipelines gets one
 * unambiguous refusal per line rather than a mixture of parse errors and
 * busy, which it would have to tell apart to know what to retry.
 *
 * `fs_ls` opens the window because it is a multi-record command answered by
 * AO_Storage - which has a queue in this binary but nothing dispatching it,
 * so the terminator never arrives and the command stays in flight for as
 * long as the case needs. */
TEST_CASE("a second command while one is in flight answers busy, whatever "
          "else is wrong with it", "[console_parse]")
{
    console_up_once();

    type_line("fs_ls");
    fake_klog_reset();

    /* Each of these would be a different refusal at an idle console: an
     * unknown verb, a missing '=', an unknown key. All three answer busy. */
    type_line("frobnicate");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4"),
        "an unknown verb was parsed while a command was in flight");

    fake_klog_reset();
    type_line("log_level debug");
    TEST_ASSERT_TRUE(replied(">err code=4"));

    fake_klog_reset();
    type_line("ps");
    TEST_ASSERT_TRUE(replied(">err code=4"));
}

/* And a blank line is still nothing, even while busy: it is discarded before
 * the busy check, because there is no command to be busy about. */
TEST_CASE("a blank line while busy is still not a command", "[console_parse]")
{
    console_up_once();

    type_line("fs_ls");
    fake_klog_reset();

    type_line("");

    TEST_ASSERT_EQUAL_UINT(0U, replies());
}
