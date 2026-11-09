/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_ir.c
 * @brief `ir`, `ir_help`, `ir_send` and `ir_rx`: the infrared block, from the wire.
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
#include "kdrv_ir.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks - repeated rather than imported for the
 * same reason the other console files repeat it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief @p ms_ as whole QP ticks, the same rounding cmd_extend() uses. */
#define IR_RX_TICKS(ms_)   (((ms_) * 100U) / 1000U)

static void console_up(void)
{
    const QEvt *e;

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

    /* AO_Ir's queue is the mailbox the verbs post into; emptied here so a
     * request left by an earlier case cannot be mistaken for this one's. */
    qf_support_give_queue(AO_Ir);
    while ( xQueueReceive(AO_Ir->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    fake_klog_reset();
    fake_neighbours_reset();
    fake_time_reset();
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

/**
 * @brief Take the status request the console posted to AO_Ir, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted_info(IrInfoReqEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Ir->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(IR_INFO_REQ_SIG, e->sig);
    *out = *(const IrInfoReqEvt *)e;
    QF_gc(e);
    return true;
}

/**
 * @brief Take the transmit request the console posted to AO_Ir, if any.
 * @param[out] out A copy of it.
 * @return false when nothing was posted.
 */
static bool posted_send(IrSendEvt *out)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(AO_Ir->eQueue, &e, 0U) != pdTRUE )
    {
        return false;
    }
    TEST_ASSERT_EQUAL_INT(IR_SEND_REQ_SIG, e->sig);
    *out = *(const IrSendEvt *)e;
    QF_gc(e);
    return true;
}

/** @brief Deliver AO_Ir's status answer, the way info_serve() posts it. */
static void answer_info(bool up, bool deaf, bool rx_high, bool have_last,
                        uint16_t addr, uint16_t cmd, uint16_t symbols,
                        bool nec, uint32_t age_ms)
{
    IrInfoEvt a;

    (void)memset(&a, 0, sizeof(a));
    a.super     = (QEvt)QEVT_INITIALIZER(IR_INFO_RSP_SIG);
    a.up        = up;
    a.deaf      = deaf;
    a.rx_high   = rx_high;
    a.have_last = have_last;
    a.addr      = addr;
    a.cmd       = cmd;
    a.symbols   = symbols;
    a.nec       = nec;
    a.age_ms    = age_ms;
    QASM_DISPATCH(AO_Console, &a.super, 0U);
}

/** @brief Deliver one frame the way AO_Ir publishes IR_RX_SIG. */
static void deliver_rx(uint16_t addr, uint16_t cmd, uint16_t symbols, bool nec)
{
    IrRxEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super   = (QEvt)QEVT_INITIALIZER(IR_RX_SIG);
    e.addr    = addr;
    e.cmd     = cmd;
    e.symbols = symbols;
    e.nec     = nec;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/* ---- ir ------------------------------------------------------------------- */

TEST_CASE("ir asks AO_Ir and prints its status once answered", "[console_ir]")
{
    IrInfoReqEvt q;

    console_up();
    type_line("ir");

    TEST_ASSERT_TRUE(posted_info(&q));
    TEST_ASSERT_EQUAL_PTR_MESSAGE(AO_Console, q.who,
                                  "the answer must come back here");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "closed before AO_Ir answered");

    answer_info(true, false, true, false, 0U, 0U, 0U, false, 0U);

    TEST_ASSERT_TRUE(replied(">|ir=tx state=up"));
    TEST_ASSERT_TRUE(replied(">|ir=rx state=up deaf=no"));
    TEST_ASSERT_TRUE(replied(">|ir=carrier hz=38000"));
    TEST_ASSERT_TRUE(replied(">|ir=level high=1"));
    TEST_ASSERT_TRUE(replied(">|ir=last state=none"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* Every hardware status verb in this console answers whatever state it
 * finds, block never having come up included - `ir` is no exception, and a
 * board with no infrared fitted must still get a clean answer rather than a
 * hang or a crash reading a level nobody measured. */
TEST_CASE("ir answers even when the block never came up", "[console_ir]")
{
    IrInfoReqEvt q;

    console_up();
    type_line("ir");
    TEST_ASSERT_TRUE(posted_info(&q));

    answer_info(false, false, false, false, 0U, 0U, 0U, false, 0U);

    TEST_ASSERT_TRUE(replied(">|ir=tx state=down"));
    TEST_ASSERT_TRUE(replied(">|ir=rx state=down deaf=no"));
    /* The carrier is a fixed board fact, not a reading - it is printed
     * whether or not the block came up. */
    TEST_ASSERT_TRUE(replied(">|ir=carrier hz=38000"));
    TEST_ASSERT_TRUE(replied(">|ir=level state=unknown"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ir prints the last frame heard, decoded, with its age",
          "[console_ir]")
{
    IrInfoReqEvt q;

    console_up();
    type_line("ir");
    TEST_ASSERT_TRUE(posted_info(&q));

    answer_info(true, false, true, true, 0x0004U, 0x0008U, 34U, true, 1234U);

    TEST_ASSERT_TRUE(replied(">|ir=last state=heard nec=1 addr=0x0004 "
                             "cmd=0x0008 symbols=34 age_ms=1234"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ir prints an undecoded last frame without inventing addr/cmd",
          "[console_ir]")
{
    IrInfoReqEvt q;

    console_up();
    type_line("ir");
    TEST_ASSERT_TRUE(posted_info(&q));

    answer_info(true, true, true, true, 0U, 0U, 12U, false, 5U);

    TEST_ASSERT_TRUE(replied(">|ir=last state=heard nec=0 symbols=12 "
                             "age_ms=5"));
    TEST_ASSERT_FALSE(replied("addr="));
    /* deaf=yes carried through from the answer, on the rx= line. */
    TEST_ASSERT_TRUE(replied(">|ir=rx state=up deaf=yes"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("another chip's answer does not close an ir question", "[console_ir]")
{
    IrInfoReqEvt   q;
    IoexpFieldEvt  stray;

    console_up();
    type_line("ir");
    TEST_ASSERT_TRUE(posted_info(&q));

    (void)memset(&stray, 0, sizeof(stray));
    stray.super = (QEvt)QEVT_INITIALIZER(IOEXP_FIELD_RSP_SIG);
    stray.op    = FIELD_OP_GET;
    stray.rc    = (uint8_t)KODE_OK;
    stray.value = 1;
    QASM_DISPATCH(AO_Console, &stray.super, 0U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the expander's answer closed it");

    answer_info(true, false, true, false, 0U, 0U, 0U, false, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- ir_help ---------------------------------------------------------------- */

TEST_CASE("ir_help lists every field and closes with ok, in one pass",
          "[console_ir]")
{
    console_up();
    type_line("ir_help");

    TEST_ASSERT_TRUE(replied(">|field=\"tx\" desc="));
    TEST_ASSERT_TRUE(replied(">|field=\"rx\" desc="));
    TEST_ASSERT_TRUE(replied(">|field=\"carrier\" desc="));
    TEST_ASSERT_TRUE(replied(">|field=\"level\" desc="));
    TEST_ASSERT_TRUE(replied(">|field=\"last\" desc="));
    /* Five rows sit well under one KCLI_SLICE, so unlike c5_help this
     * answers whole in the same run-to-completion step - no pump needed. */
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* ---- ir_send ---------------------------------------------------------------- */

TEST_CASE("ir_send posts addr and cmd to AO_Ir and answers ok", "[console_ir]")
{
    IrSendEvt q;

    console_up();
    type_line("ir_send addr=4 cmd=8");

    TEST_ASSERT_TRUE(posted_send(&q));
    TEST_ASSERT_EQUAL_HEX16(4U, q.addr);
    TEST_ASSERT_EQUAL_HEX16(8U, q.cmd);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ir_send with no addr defaults it to zero", "[console_ir]")
{
    IrSendEvt q;

    console_up();
    type_line("ir_send cmd=8");

    TEST_ASSERT_TRUE(posted_send(&q));
    TEST_ASSERT_EQUAL_HEX16(0U, q.addr);
    TEST_ASSERT_EQUAL_HEX16(8U, q.cmd);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ir_send rejects a line with no cmd=", "[console_ir]")
{
    IrSendEvt q;

    console_up();
    type_line("ir_send addr=4");

    TEST_ASSERT_FALSE_MESSAGE(posted_send(&q), "AO_Ir was asked anyway");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=cmd"));
}

/* NEC's command is sixteen bits; the argument bounds refuse anything past
 * 0xFFFF before AO_Ir is ever asked - the same shape as `c5 field=` refusing
 * an unknown name before the owner is asked. */
TEST_CASE("an out-of-range cmd= is refused by the argument bounds",
          "[console_ir]")
{
    IrSendEvt q;

    console_up();
    type_line("ir_send cmd=70000");

    TEST_ASSERT_FALSE_MESSAGE(posted_send(&q), "AO_Ir was asked anyway");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=cmd"));
}

TEST_CASE("an out-of-range addr= is refused the same way", "[console_ir]")
{
    IrSendEvt q;

    console_up();
    type_line("ir_send addr=100000 cmd=8");

    TEST_ASSERT_FALSE_MESSAGE(posted_send(&q), "AO_Ir was asked anyway");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=addr"));
}

TEST_CASE("proto is the only member there is today, and it need not be given",
          "[console_ir]")
{
    IrSendEvt q;

    console_up();
    type_line("ir_send proto=nec cmd=8");
    TEST_ASSERT_TRUE(posted_send(&q));
    TEST_ASSERT_TRUE(replied(">ok"));

    console_up();
    type_line("ir_send proto=sirc cmd=8");
    TEST_ASSERT_FALSE_MESSAGE(posted_send(&q), "AO_Ir was asked anyway");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=proto"));
}

/* ---- ir_rx ------------------------------------------------------------------ */

TEST_CASE("ir_rx reports each frame as it arrives", "[console_ir]")
{
    console_up();
    type_line("ir_rx ms=1000");

    deliver_rx(0x0004U, 0x0008U, 34U, true);
    TEST_ASSERT_TRUE(replied(">|ir=rx nec=1 addr=0x0004 cmd=0x0008 "
                             "symbols=34"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the window has not run out");

    fake_klog_reset();
    deliver_rx(0U, 0U, 9U, false);
    TEST_ASSERT_TRUE(replied(">|ir=rx nec=0 symbols=9"));
    TEST_ASSERT_FALSE(replied(">ok"));
}

/* Running the window out is this verb's normal end, not the protocol
 * timeout every other command answers with code 6 - the one behaviour this
 * file exists to pin, since no other console case ever needs it. */
TEST_CASE("ir_rx closes with ok when its window runs out, not a timeout",
          "[console_ir]")
{
    console_up();
    type_line("ir_rx ms=1000");

    (void)qf_support_tick(AO_Console, IR_RX_TICKS(1000U) - 1U);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "the window has not run out yet");
    TEST_ASSERT_FALSE(replied(">err"));

    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">err code=6"),
                              "listening out its own window is not a timeout");
}

TEST_CASE("ir_rx with no ms defaults to the ordinary command window",
          "[console_ir]")
{
    console_up();
    type_line("ir_rx");

    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS - 1U);
    TEST_ASSERT_FALSE(replied(">ok"));

    (void)qf_support_tick(AO_Console, 1U);
    TEST_ASSERT_TRUE(replied(">ok"));
}

/* The argument bounds refuse an out-of-range ms= before cmd_begin() ever
 * runs - the console never opens a window it will not listen inside, and a
 * mistaken line is answered at once rather than left to time out. */
TEST_CASE("ir_rx ms= outside its floor or ceiling is refused, not accepted",
          "[console_ir]")
{
    console_up();
    type_line("ir_rx ms=99");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "a refused line opens no window");

    fake_klog_reset();
    type_line("ir_rx ms=30001");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=ms"));
    TEST_ASSERT_FALSE(replied(">ok"));

    /* And the console is genuinely idle again - a real ir_rx runs at once,
     * rather than answering `busy` behind a window that was never opened. */
    fake_klog_reset();
    type_line("ir_rx ms=100");
    TEST_ASSERT_FALSE(replied(">err code=4"));
    (void)qf_support_tick(AO_Console, IR_RX_TICKS(100U) + 1U);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("ir_rx ms= at its floor and ceiling is accepted", "[console_ir]")
{
    console_up();
    type_line("ir_rx ms=100");
    TEST_ASSERT_FALSE(replied(">err"));

    (void)qf_support_tick(AO_Console, IR_RX_TICKS(100U) + 1U);
    TEST_ASSERT_TRUE(replied(">ok"));

    /* Ticked all the way to its own close, not left running: a window this
     * long would otherwise still be open when the next case's console_up()
     * only clears the ordinary 5 s watchdog. */
    console_up();
    fake_klog_reset();
    type_line("ir_rx ms=30000");
    TEST_ASSERT_FALSE(replied(">err"));
    (void)qf_support_tick(AO_Console, IR_RX_TICKS(30000U) + 1U);
    TEST_ASSERT_TRUE(replied(">ok"));
}
