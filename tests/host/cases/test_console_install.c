/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_install.c
 * @brief `install`: one verb for every image, from the wire or a medium to its owner.
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
#include "kdrv_kfw.h"
#include "kqp.h"
#include "lowpower_cases.h"
#include "qf_support.h"
#include "owners_support.h"

/** @brief Empty @p ao's queue. */
static void drain_queue(QActive *ao)
{
    const QEvt *e;

    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

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
    /* A command a previous case left in flight is closed the way its owner
     * would close it, so this case starts at an idle prompt. */
    {
        static const QEvt fail = QEVT_INITIALIZER(RECV_FAIL_SIG);
        QASM_DISPATCH(AO_Console, &fail, 0U);
    }
    drain_queue(AO_Radio);
    drain_queue(AO_AppMgr);
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Send one line in, from @p src, exactly as AO_Log publishes it. */
static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
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

/** @brief Deliver one of the receive protocol's published answers. */
static void answer(enum_t sig)
{
    QEvt e = QEVT_INITIALIZER(sig);
    QASM_DISPATCH(AO_Console, &e, 0U);
}

/** @brief The one event in @p ao's queue if it is @p sig, or NULL; the
 *         caller frees. */
static const QEvt *take(QActive *ao, enum_t sig)
{
    const QEvt *e = NULL;

    if ( xQueueReceive(ao->eQueue, &e, 0U) != pdTRUE )
    {
        return NULL;
    }
    if ( (enum_t)e->sig != sig )
    {
        QF_gc(e);
        return NULL;
    }
    return e;
}

/** @brief The one receive request in @p ao's queue, or NULL; the caller frees. */
static const AppRecvReqEvt *take_request(QActive *ao, enum_t sig)
{
    return (const AppRecvReqEvt *)take(ao, sig);
}

/** @brief Type @p line and check it is refused with @p err before anything
 *         reaches either owner. */
static void assert_refused(const char *line, const char *err)
{
    console_up();
    type_line(line);
    TEST_ASSERT_TRUE_MESSAGE(replied(err), line);
    TEST_ASSERT_NULL_MESSAGE(take(AO_AppMgr, APP_RECV_REQ_SIG), line);
    TEST_ASSERT_NULL_MESSAGE(take(AO_Radio, RADIO_RECV_REQ_SIG), line);
}

/* ---- src="terminal" dst=c5 --------------------------------------------- */

TEST_CASE("install from the terminal to the C5 hands the size, the CRC and the "
          "port to AO_Radio and holds the wire until the receive closes",
          "[console_install]")
{
    const AppRecvReqEvt *req;

    console_up();
    type_line("install src=\"terminal\" dst=c5 size=1234 crc32=99");

    req = take_request(AO_Radio, RADIO_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(req, "the request must reach the C5's owner");
    TEST_ASSERT_EQUAL_UINT32(1234U, req->size);
    TEST_ASSERT_EQUAL_UINT32(99U, req->crc32);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_TARGET_C5, req->dst);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, req->src);
    QF_gc(&req->super);
    TEST_ASSERT_NULL_MESSAGE(take(AO_AppMgr, APP_RECV_REQ_SIG),
        "the C5's image is AO_Radio's alone");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "nothing is a result yet");
    TEST_ASSERT_FALSE(replied(">err"));

    answer(RECV_READY_SIG);
    TEST_ASSERT_TRUE(replied(">|ready chunk=4096"));
    answer(RECV_ACK_SIG);
    TEST_ASSERT_TRUE(replied(">|ack"));
    answer(RECV_DONE_SIG);
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the activate is the result");
}

TEST_CASE("a terminal install's failure closes with the verb named, and a "
          "stall with code 6", "[console_install]")
{
    console_up();
    type_line("install src=\"terminal\" dst=c5 size=8 crc32=0");
    drain_queue(AO_Radio);
    answer(RECV_FAIL_SIG);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=install"));

    fake_klog_reset();
    type_line("install src=\"terminal\" dst=app size=8 crc32=0");
    drain_queue(AO_AppMgr);
    answer(RECV_STALL_SIG);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=install"));
}

TEST_CASE("install from the terminal to the C5 typed at the BLE console is "
          "refused as cable-only before anything is posted",
          "[console_install]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE,
              "install src=\"terminal\" dst=c5 size=1234 crc32=99");

    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=5 msg=cable"),
        "the C5's reboot takes the BLE link with it; the >ok could never arrive");
    TEST_ASSERT_NULL_MESSAGE(take_request(AO_Radio, RADIO_RECV_REQ_SIG),
        "nothing reached AO_Radio");

    /* And the console is free: the refusal closed the command. */
    fake_klog_reset();
    type_line("nosuchverb");
    TEST_ASSERT_TRUE(replied(">err code=1"));
}

TEST_CASE("install from the terminal to an app typed at the BLE console goes "
          "ahead, with BLE as the wire", "[console_install]")
{
    const AppRecvReqEvt *req;

    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE,
              "install src=\"terminal\" dst=app size=4096 crc32=7");
    req = take_request(AO_AppMgr, APP_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_BLE, req->src);
    QF_gc(&req->super);
    answer(RECV_FAIL_SIG);
}

/* ---- src="terminal" to AO_AppMgr's destinations ------------------------ */

TEST_CASE("install from the terminal to the OS hands the image to AO_AppMgr "
          "with the kodeOS slot as its destination and closes on the verdict",
          "[console_install]")
{
    const AppRecvReqEvt *req;

    console_up();
    type_line("install src=\"terminal\" dst=os size=4096 crc32=7");

    req = take_request(AO_AppMgr, APP_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(req, "the request must reach the slot's owner");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_TARGET_OS, req->dst);
    TEST_ASSERT_EQUAL_UINT32(4096U, req->size);
    TEST_ASSERT_EQUAL_UINT32(7U, req->crc32);
    TEST_ASSERT_FALSE_MESSAGE(req->launch, "the switch is the launch");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, req->src);
    QF_gc(&req->super);

    answer(RECV_READY_SIG);
    TEST_ASSERT_TRUE(replied(">|ready chunk=4096"));
    answer(RECV_DONE_SIG);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("install from the terminal to an app does not run it unless asked",
          "[console_install]")
{
    const AppRecvReqEvt *req;

    console_up();
    type_line("install src=\"terminal\" dst=app size=4096 crc32=7");
    req = take_request(AO_AppMgr, APP_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_TARGET_APP, req->dst);
    TEST_ASSERT_FALSE_MESSAGE(req->launch,
                              "run defaults off, for every source");
    QF_gc(&req->super);
    answer(RECV_FAIL_SIG);

    fake_klog_reset();
    type_line("install src=\"terminal\" dst=app size=4096 crc32=7 run=on");
    req = take_request(AO_AppMgr, APP_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_TRUE(req->launch);
    QF_gc(&req->super);
    answer(RECV_FAIL_SIG);
}

TEST_CASE("install from the terminal to an app hands AO_AppMgr the name the "
          "host read, and says every app it evicts", "[console_install]")
{
    const AppRecvReqEvt *req;
    AppEvictedEvt        ev;

    console_up();
    type_line("install src=\"terminal\" dst=app size=4096 crc32=7 "
              "name=\"blink\"");
    req = take_request(AO_AppMgr, APP_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_STRING("blink", req->name);
    QF_gc(&req->super);

    (void)memset(&ev, 0, sizeof(ev));
    ev.super = (QEvt)QEVT_INITIALIZER(APP_EVICTED_SIG);
    (void)snprintf(ev.name, sizeof(ev.name), "%s", "old");
    QASM_DISPATCH(AO_Console, &ev.super, 0U);
    TEST_ASSERT_TRUE(replied(">|evicted name=\"old\" kept=off"));
    answer(RECV_READY_SIG);
    TEST_ASSERT_TRUE(replied(">|ready chunk=4096"));
    answer(RECV_DONE_SIG);
    TEST_ASSERT_TRUE(replied(">ok"));

    /* With no install on the wire, an eviction is not this console's to say. */
    fake_klog_reset();
    ev.kept = true;
    QASM_DISPATCH(AO_Console, &ev.super, 0U);
    TEST_ASSERT_FALSE(replied(">|evicted"));
}

TEST_CASE("a name is refused on any install but a terminal one to an app",
          "[console_install]")
{
    assert_refused("install src=\"terminal\" dst=os size=8 crc32=0 "
                   "name=\"x\"", ">err code=3 msg=name");
    assert_refused("install src=\"pending\" dst=app name=\"x\"",
                   ">err code=3 msg=name");
}

TEST_CASE("install from the terminal to the mailbox hands AO_AppMgr the park",
          "[console_install]")
{
    const AppRecvReqEvt *req;

    console_up();
    type_line("install src=\"terminal\" dst=pending size=4096 crc32=7");
    req = take_request(AO_AppMgr, APP_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_FLASH_DST_PENDING, req->dst);
    TEST_ASSERT_FALSE(req->launch);
    QF_gc(&req->super);
    answer(RECV_FAIL_SIG);
}

TEST_CASE("install from the terminal without its size or CRC is refused, "
          "naming the missing key", "[console_install]")
{
    assert_refused("install src=\"terminal\" dst=app crc32=7",
                   ">err code=2 msg=size");
    assert_refused("install src=\"terminal\" dst=c5 size=1234",
                   ">err code=2 msg=crc32");
}

/* ---- The rules every source shares --------------------------------------- */

TEST_CASE("run is refused on every destination but an app, whichever way it "
          "is typed", "[console_install]")
{
    assert_refused("install src=\"terminal\" dst=os size=4096 crc32=7 run=off",
                   ">err code=3 msg=run");
    assert_refused("install src=\"terminal\" dst=c5 size=4096 crc32=7 run=on",
                   ">err code=3 msg=run");
    assert_refused("install src=\"terminal\" dst=pending size=4096 crc32=7 "
                   "run=off", ">err code=3 msg=run");
    assert_refused("install src=\"sd:kodeos.bin\" dst=os run=on",
                   ">err code=3 msg=run");
    TEST_ASSERT_NULL(take(AO_AppMgr, FLASH_REQ_SIG));
}

TEST_CASE("the mailbox into itself is refused", "[console_install]")
{
    assert_refused("install src=\"pending\" dst=pending", ">err code=3 msg=dst");
    TEST_ASSERT_NULL(take(AO_AppMgr, FLASH_REQ_SIG));
}

TEST_CASE("size and crc32 belong to the terminal alone", "[console_install]")
{
    assert_refused("install src=\"sd:app.bin\" dst=app size=4096",
                   ">err code=3 msg=size");
    TEST_ASSERT_NULL(take(AO_AppMgr, FLASH_REQ_SIG));
    assert_refused("install src=\"pending\" dst=app crc32=7",
                   ">err code=3 msg=crc32");
    TEST_ASSERT_NULL(take(AO_AppMgr, FLASH_REQ_SIG));
}

/** @brief Hand the console AO_AppMgr's verdict, as AO_AppMgr posts it. */
static void verdict(app_req_verdict_t v)
{
    AppReqRspEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super   = (QEvt)QEVT_INITIALIZER(APP_REQ_RSP_SIG);
    e.verdict = (uint8_t)v;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Type an install off a medium and take its request out of AO_AppMgr's queue. */
static const FlashReqEvt *install_asked(const char *line)
{
    console_up();
    type_line(line);
    return (const FlashReqEvt *)take(AO_AppMgr, FLASH_REQ_SIG);
}

/* The `>ok` of an install off a medium says AO_AppMgr took it. Posting the
 * request says nothing of the kind: it may be busy, or the file may not be
 * there. The console holds the line until AO_AppMgr's verdict. */
TEST_CASE("install off a medium waits for AO_AppMgr's verdict and hands it the "
          "source, the destination, run and who asked", "[console_install]")
{
    const FlashReqEvt *req = install_asked("install src=\"sd:app.bin\" dst=app");

    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_SD, req->src_kind);
    TEST_ASSERT_EQUAL_STRING("app.bin", req->src);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_TARGET_APP, req->dst_kind);
    TEST_ASSERT_FALSE_MESSAGE(req->launch, "run defaults off, as from the wire");
    TEST_ASSERT_TRUE_MESSAGE(AO_Console == req->who,
        "the request does not say who is waiting for the verdict");
    QF_gc(&req->super);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"),
        "the line answered before AO_AppMgr said whether it took the install");
    TEST_ASSERT_FALSE(replied(">err"));

    verdict(APP_REQ_TAKEN);
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    req = install_asked("install src=\"https://example.com/app.bin\" dst=app run=on");
    TEST_ASSERT_NOT_NULL(req);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KFW_SRC_HTTP, req->src_kind);
    TEST_ASSERT_TRUE(req->launch);
    QF_gc(&req->super);
    verdict(APP_REQ_TAKEN);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("install off a medium says why AO_AppMgr did not take it",
          "[console_install]")
{
    static const struct {
        const char        *line;
        app_req_verdict_t  verdict;
        const char        *reply;
    } cases[] = {
        { "install src=\"sd:app.bin\" dst=app",  APP_REQ_BUSY,    ">err code=4 msg=busy"    },
        { "install src=\"sd:app.bin\" dst=app",  APP_REQ_MISSING, ">err code=5 msg=missing" },
        { "install src=\"fs:app.bin\" dst=os",   APP_REQ_MISSING, ">err code=5 msg=missing" },
        { "install src=\"pending\" dst=app",     APP_REQ_EMPTY,   ">err code=5 msg=empty"   },
        { "install src=\"sd:app.bin\" dst=app",  APP_REQ_REFUSED, ">err code=5 msg=refused" },
    };

    for ( unsigned i = 0U; (sizeof(cases) / sizeof(cases[0])) > i; ++i )
    {
        const FlashReqEvt * const req = install_asked(cases[i].line);

        TEST_ASSERT_NOT_NULL_MESSAGE(req, cases[i].line);
        QF_gc(&req->super);
        verdict(cases[i].verdict);
        TEST_ASSERT_TRUE_MESSAGE(replied(cases[i].reply), cases[i].reply);
        TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), cases[i].reply);
    }

    /* The command is closed: another one is not met with busy. */
    fake_klog_reset();
    type_line("nosuchverb");
    TEST_ASSERT_TRUE(replied(">err code=1"));
}

TEST_CASE("an install that waits holds the console, and a verdict nobody waits "
          "for says nothing", "[console_install]")
{
    const FlashReqEvt *req = install_asked("install src=\"sd:app.bin\" dst=app");

    TEST_ASSERT_NOT_NULL(req);
    QF_gc(&req->super);
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=4"),
        "a second command ran while the first waited for its verdict");

    verdict(APP_REQ_TAKEN);
    fake_klog_reset();
    verdict(APP_REQ_TAKEN);
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "a late verdict answered a command nobody typed");
}

/** @brief The console's wait for a verdict, in QP ticks, past which it gives up. */
#define VERDICT_WAIT_TICKS  (((KODE_APPMGR_OPEN_MS + 5000U) / 10U) + 1U)

TEST_CASE("an install whose verdict never comes ends with the verb named, not "
          "in silence", "[console_install]")
{
    const FlashReqEvt *req = install_asked("install src=\"sd:app.bin\" dst=app");

    TEST_ASSERT_NOT_NULL(req);
    QF_gc(&req->super);
    (void)qf_support_tick(AO_Console, VERDICT_WAIT_TICKS);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=install"));
}

TEST_CASE("the old verbs are gone", "[console_install]")
{
    assert_refused("flash src=\"sd:app.bin\" dst=app", ">err code=1 msg=flash");
    assert_refused("app_install format=raw size=4096 crc32=7",
                   ">err code=1 msg=app_install");
    assert_refused("ota_install format=raw size=4096 crc32=7",
                   ">err code=1 msg=ota_install");
    assert_refused("c5_install format=raw size=4096 crc32=7",
                   ">err code=1 msg=c5_install");
}

/* ---- The board's rest ------------------------------------------------------- */

TEST_CASE("the console is ready for the round except while a payload crosses the wire",
          "[console_install][lowpower]")
{
    const AppRecvReqEvt *req;

    console_up();
    lp_round(AO_Console, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
    lp_expect_no_more();

    type_line("install src=\"terminal\" dst=os size=4096 crc32=7");
    req = take_request(AO_AppMgr, APP_RECV_REQ_SIG);
    TEST_ASSERT_NOT_NULL(req);
    QF_gc(&req->super);
    answer(RECV_READY_SIG);

    lp_round(AO_Console, KODE_LP_OFF);
    lp_expect_answer(true, "console transfer");
    lp_expect_no_more();

    answer(RECV_DONE_SIG);
    lp_round(AO_Console, KODE_LP_ASLEEP);
    lp_expect_answer(false, NULL);
}
