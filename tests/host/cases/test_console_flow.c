/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_flow.c
 * @brief A console command's life: begin, stream, terminate - or time out.
 */
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

#include "unity.h"

#include "qpc.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kaos.h"
#include "kcli_proto.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks. Repeated rather than imported for the
 * reason the parser's token limit is: it is private to console_ao.c, and a
 * test sharing the constant would agree with it by construction. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief Bring AO_Console up once, and leave it idle whatever ran before. */
static void console_idle(void)
{
    qf_support_init();

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

    qf_support_give_queue(AO_Storage);
    fake_klog_reset();
    fake_neighbours_reset();
}

/**
 * @brief Advance the clock and let AO_Console see its own watchdog.
 *
 * Not qf_support_pump(): AO_Console is registered as a background AO there -
 * it has to be, because a command left in flight arms a watchdog that would
 * otherwise post into a queue nobody drains for the rest of the run - and the
 * pump empties those queues on every tick. qf_support_tick() drains every
 * background AO except this one, which is what makes the watchdog observable
 * without the periodic timers of AO_Power, AO_Touch and AO_Buttons filling
 * their own queues in the meantime.
 */
static void tick(unsigned ticks)
{
    (void)qf_support_tick(AO_Console, ticks);
}

/** @brief Send one command line in, as AO_Log publishes it. */
static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief One entry of a storage listing, as AO_Storage publishes it. */
static void send_entry(const char *name, uint32_t size)
{
    StorageEntryEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super   = (QEvt)QEVT_INITIALIZER(STORAGE_ENTRY_SIG);
    e.size    = size;
    e.from_sd = false;
    e.is_dir  = false;
    (void)snprintf(e.name, sizeof(e.name), "%s", name);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief The end marker that closes a storage listing. */
static void send_list_end(void)
{
    static const ListEndEvt e = {
        .super = QEVT_INITIALIZER(STORAGE_LIST_END_SIG), .more = false };
    QASM_DISPATCH(AO_Console, &e.super, 0U);
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

/** @brief Is the console free? Asked the only way a host can: by trying. */
static bool console_free(void)
{
    fake_klog_reset();
    type_line("nosuchverb");
    /* An idle console parses and refuses the verb; a busy one answers busy
     * before it looks at the line at all. */
    return replied(">err code=1");
}

/* ---- A command that completes ---------------------------------------------- */

TEST_CASE("a listing streams its records and closes with its terminator",
          "[console_flow]")
{
    console_idle();

    type_line("fs_ls");
    fake_klog_reset();

    send_entry("kodeos.bin", 2048U);
    send_entry("sleep.bin", 1024U);
    send_list_end();

    TEST_ASSERT_TRUE(replied(">|name=\"kodeos.bin\" size=2048 medium=fs"));
    TEST_ASSERT_TRUE(replied(">|name=\"sleep.bin\" size=1024 medium=fs"));
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"),
        "the listing never closed, so a host would still be reading");

    TEST_ASSERT_TRUE_MESSAGE(console_free(),
        "the terminator did not free the console");
}

/* ---- A line is the board being used ----------------------------------------- */

/** @brief The port of the one note waiting for AO_LowPower, or 0xFF when none. */
static uint8_t lowpower_note_src(void)
{
    const QEvt *e = NULL;
    uint8_t src = 0xFFU;

    if ( xQueueReceive(AO_LowPower->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)LOWPOWER_INPUT_SIG == e->sig )
        {
            src = ((const LowPowerInputEvt *)e)->src;
        }
        QF_gc(e);
    }
    return src;
}

TEST_CASE("every line notes the board was used, on the port it came by; an empty one does not",
          "[console_flow]")
{
    ConsoleLineEvt e;

    console_idle();
    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);

    e.src = (uint8_t)KODE_CONSOLE_SRC_BLE;
    (void)snprintf(e.line, sizeof(e.line), "ping");
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    TEST_ASSERT_EQUAL_UINT8(KODE_CONSOLE_SRC_BLE, lowpower_note_src());
    TEST_ASSERT_TRUE(console_free());
    (void)lowpower_note_src();             /* console_free() typed a line too */

    e.src = (uint8_t)KODE_CONSOLE_SRC_NET;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    TEST_ASSERT_EQUAL_UINT8(KODE_CONSOLE_SRC_NET, lowpower_note_src());

    e.line[0] = '\0';
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(0xFFU, lowpower_note_src(), "an empty line is no command");
}

/* ---- Records nobody asked for ---------------------------------------------- */

/* THE gate. Listings are published, not posted, so the GUI asking for one
 * puts these very events on the bus while the console is idle. Rendering them
 * would write `>|` records into a stream no host is reading, and - worse -
 * into the middle of whatever command runs next. */
TEST_CASE("a record arriving when nothing was asked is not put on the wire",
          "[console_flow]")
{
    console_idle();

    send_entry("somebody-elses.bin", 4096U);
    send_list_end();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "a listing the GUI asked for was answered to the host");
}

/* The same gate one step in: a record for a DIFFERENT command than the one in
 * flight. Each record kind checks the in-flight kind, not merely that
 * something is in flight. */
TEST_CASE("a record for another command is ignored while one is in flight",
          "[console_flow]")
{
    AppEntryEvt e;

    console_idle();

    type_line("fs_ls");
    fake_klog_reset();

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(APP_ENTRY_SIG);
    QASM_DISPATCH(AO_Console, &e.super, 0U);

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "an app record was rendered into a file listing's response stream");

    send_list_end();   /* leave the console idle for the next case */
}

/* ---- The owner that never answers ------------------------------------------- */

/* The watchdog, and the whole reason it exists. Without it the console holds
 * the wire for the rest of the boot and answers busy to everything, with
 * nothing in the log saying why. */
TEST_CASE("an owner that never answers is timed out, and the command named",
          "[console_flow]")
{
    console_idle();

    type_line("fs_ls");
    fake_klog_reset();

    /* Just short of the deadline: still holding the wire. */
    tick(CMD_TIMEOUT_TICKS - 2U);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "the console gave up before its own deadline");

    tick(4U);

    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=6 msg=fs_ls"),
        "the command that died was not named, so a host cannot tell which");
    TEST_ASSERT_TRUE_MESSAGE(console_free(),
        "the console is still holding the wire after its watchdog fired");
}

/* An answer that arrives after the console gave up is ignored rather than
 * appended. A host has already read its `>err` and moved on; a late `>ok`
 * would terminate whatever command it is running now. */
TEST_CASE("an answer arriving after the timeout is ignored", "[console_flow]")
{
    console_idle();

    type_line("fs_ls");
    tick(CMD_TIMEOUT_TICKS + 2U);
    fake_klog_reset();

    send_entry("late.bin", 8U);
    send_list_end();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, fake_klog_count(),
        "a late answer terminated a command the host had given up on");
}

/* The other half. cmd_end() disarms the watchdog, and what that buys is not
 * what it looks like: a stale timer firing into an idle console is already
 * harmless, because the handler checks the in-flight kind before it says
 * anything. What it buys is the NEXT command being able to arm at all -
 * cmd_begin() arms unconditionally, and QP refuses to arm a QTimeEvt that is
 * already armed.
 *
 * So the property is that a completed command leaves the timer free, and the
 * way to see it is to complete one and start another. Note how it fails
 * without the disarm: not as this case failing, but as the whole run stopping
 * at Q_onError qf_time:450 on the second command. A detection, but an ugly
 * one - worth knowing before reading a green run as this case having done
 * work.
 *
 * An earlier version of this case only checked that no `>err code=6` reached
 * an idle console, and passed with the disarm removed. It was asserting the
 * handler's guard, not the disarm. */
TEST_CASE("a closed command leaves its watchdog free for the next one",
          "[console_flow]")
{
    console_idle();

    type_line("fs_ls");
    send_list_end();

    /* The second command has to be able to arm its own watchdog. */
    type_line("fs_ls");
    fake_klog_reset();

    /* And the deadline it gets is its own, not one inherited from the first:
     * exactly one timeout, naming the command actually in flight. */
    tick(CMD_TIMEOUT_TICKS + 4U);

    TEST_ASSERT_TRUE(replied(">err code=6 msg=fs_ls"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_klog_count(),
        "more than one line came out of a single expired command");

    TEST_ASSERT_TRUE(console_free());
}

/* And with nothing in flight the clock may run as long as it likes: a
 * watchdog that fired into an idle console would answer `>err code=6` for a
 * command that does not exist. */
TEST_CASE("an idle console is not timed out", "[console_flow]")
{
    console_idle();

    tick(CMD_TIMEOUT_TICKS + 20U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}
