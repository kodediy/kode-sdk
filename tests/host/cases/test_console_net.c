/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_net.c
 * @brief The console's NET source: what a line that arrived on the Kode channel may run.
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
#include "kode_wire.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

#if !CONFIG_KODEOS_SHOT_HTTP
#error "test_console_net.c needs CONFIG_KODEOS_SHOT_HTTP=1 (main/CMakeLists.txt)"
#endif

#define NET_SRC  ((uint8_t)KODE_CONSOLE_SRC_NET)

/** @brief Empty @p ao's queue and say how many events it held. */
static unsigned drain_queue(QActive *ao)
{
    const QEvt *e;
    unsigned    n = 0U;

    qf_support_give_queue(ao);
    while ( xQueueReceive(ao->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
        n++;
    }
    return n;
}

/** @brief The owners the verbs below would reach, all emptied. */
static unsigned drain_owners(void)
{
    unsigned n = 0U;

    n += drain_queue(AO_Radio);
    n += drain_queue(AO_AppMgr);
    n += drain_queue(AO_Storage);
    n += drain_queue(AO_Display);
    return n;
}

/** @brief Hand the console AO_Net's answer to its upload order. */
static void shot_answer(kshot_result_t result, uint16_t http, uint32_t bytes,
                        const char *id, uint32_t seq, uint8_t fps,
                        uint16_t secs)
{
    NetShotRspEvt r;

    (void)memset(&r, 0, sizeof(r));
    r.super  = (QEvt)QEVT_INITIALIZER(NET_SHOT_RSP_SIG);
    r.result = (uint8_t)result;
    r.http   = http;
    r.bytes  = bytes;
    r.seq    = seq;
    r.fps    = fps;
    r.secs   = secs;
    (void)snprintf(r.id, sizeof(r.id), "%s", id);
    QASM_DISPATCH(AO_Console, &r.super, 0U);
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
    /* Whatever a previous case left in flight is closed the way its owner
     * would close it: a receive that fails, a capture that is refused, an
     * identity request the C5 refuses. */
    {
        static const QEvt fail = QEVT_INITIALIZER(RECV_FAIL_SIG);
        DisplayCaptureRspEvt cap;
        IdEndEvt             end;

        QASM_DISPATCH(AO_Console, &fail, 0U);
        (void)memset(&end, 0, sizeof(end));
        end.super  = (QEvt)QEVT_INITIALIZER(ID_END_SIG);
        end.status = (uint8_t)KODE_ST_REFUSED;
        QASM_DISPATCH(AO_Console, &end.super, 0U);
        (void)memset(&cap, 0, sizeof(cap));
        cap.super  = (QEvt)QEVT_INITIALIZER(DISPLAY_CAPTURE_RSP_SIG);
        cap.status = (uint8_t)KODE_DENIED;
        QASM_DISPATCH(AO_Console, &cap.super, 0U);
        shot_answer(KSHOT_R_BUSY, 0U, 0U, "", 0U, 0U, 0U);
    }
    (void)drain_owners();
    (void)drain_queue(AO_Net);
    fake_klog_reset();
    fake_neighbours_reset();
}

/** @brief Send one line in, from @p src, exactly as its port publishes it. */
static void type_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
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

/** @brief The line was refused from NET, and nothing reached any owner. */
static void assert_net_refused(const char *line)
{
    fake_klog_reset();
    type_from(NET_SRC, line);
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=5 msg=net"), line);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, drain_owners(), line);
}

/* ---- install dst=c5: the cable alone, from any source ------------------- */

TEST_CASE("install to the C5 is refused from NET and BLE, and reaches nobody",
          "[console_net]")
{
    console_up();
    assert_net_refused("install src=\"https://example.com/c5.bin\" dst=c5");

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE,
              "install src=\"sd:c5.bin\" dst=c5");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_EQUAL_UINT(0U, drain_owners());
}

/* ---- The identity verbs and c5_wipe: open to NET, closed to BLE ------------ */

TEST_CASE("from NET, ident_csr passes the source check and asks the C5",
          "[console_net]")
{
    IdReqEvt     q;
    const QEvt  *e = NULL;

    console_up();
    type_from(NET_SRC, "ident_csr");

    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_EQUAL_INT(pdTRUE, (int)xQueueReceive(AO_Radio->eQueue, &e, 0U));
    TEST_ASSERT_EQUAL_INT((int)ID_REQ_SIG, (int)e->sig);
    (void)memcpy(&q, e, sizeof(q));
    QF_gc(e);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)ID_OP_CSR, q.op);
}

TEST_CASE("from NET, ident_lock and c5_wipe pass the source check",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "ident_lock confirm=off");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=confirm"),
        "refused for its value, not for its source");

    fake_klog_reset();
    type_from(NET_SRC, "c5_wipe confirm=off");
    TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=confirm"),
        "refused for its value, not for its source");
    TEST_ASSERT_FALSE(replied("msg=net"));
    TEST_ASSERT_FALSE(replied("msg=cable"));
}

TEST_CASE("from BLE, the identity verbs and c5_wipe are still cable-only",
          "[console_net]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "ident_csr");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_EQUAL_UINT(0U, drain_owners());

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "c5_wipe confirm=on");
    TEST_ASSERT_TRUE(replied(">err code=5 msg=cable"));
    TEST_ASSERT_EQUAL_UINT(0U, drain_owners());
}

/* ---- Raw payloads: the channel carries lines, not bytes -------------------- */

TEST_CASE("from NET, the verbs that stream raw bytes are refused before "
          "anything is posted", "[console_net]")
{
    console_up();
    assert_net_refused("install src=\"terminal\" dst=app size=1234 crc32=99");
    assert_net_refused("install src=\"terminal\" dst=os size=1234 crc32=99");
    assert_net_refused("install src=\"terminal\" dst=c5 size=1234 crc32=99");
    assert_net_refused("install src=\"terminal\" dst=pending size=1234 "
                       "crc32=99");
    assert_net_refused("file_put path=\"/storage/x.bin\" size=8 crc32=1");

    /* And the refusals closed their commands: the console is free. */
    fake_klog_reset();
    type_from(NET_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">|pong"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("the raw verbs still reach their owners from the cable and from BLE",
          "[console_net]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL,
              "install src=\"terminal\" dst=app size=1234 crc32=99");
    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_EQUAL_UINT(1U, drain_queue(AO_AppMgr));
}

/* ---- Payloads carried as records ------------------------------------------- */

TEST_CASE("from NET, screenshot to the console is refused, by default and by "
          "name", "[console_net]")
{
    console_up();
    assert_net_refused("screenshot");
    assert_net_refused("screenshot dst=console");
}

TEST_CASE("from NET, screenshot to a file passes and asks for the capture",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=file name=\"s.bmp\"");

    TEST_ASSERT_FALSE(replied("msg=net"));
    TEST_ASSERT_EQUAL_UINT(1U, drain_queue(AO_Display));
}

TEST_CASE("from the cable, screenshot to the console still asks for the "
          "capture", "[console_net]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "screenshot");

    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_EQUAL_UINT(1U, drain_queue(AO_Display));
}

TEST_CASE("from NET, file_get is refused before AO_Storage is asked",
          "[console_net]")
{
    console_up();
    assert_net_refused("file_get name=\"x\"");
}

TEST_CASE("from NET, mirror_start is refused: its frames are records",
          "[console_net]")
{
    console_up();
    assert_net_refused("mirror_start");

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "mirror_start");
    TEST_ASSERT_TRUE_MESSAGE(replied(">ok"), "the cable keeps its mirror");
    (void)drain_owners();
}

/* ---- Ordinary lines are ordinary --------------------------------------------- */

TEST_CASE("from NET, ordinary verbs run exactly as they do from the cable",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "ping");
    TEST_ASSERT_TRUE(replied(">|pong"));
    TEST_ASSERT_TRUE(replied(">ok"));

    fake_klog_reset();
    type_from(NET_SRC, "nosuchverb");
    TEST_ASSERT_TRUE(replied(">err code=1"));
}

/* ---- Every reply goes out tagged with the port of its command ---------------
 * The Kode channel publishes the replies of the commands that came in on
 * cmd/in, and of no others, so each `>` line the console writes carries the
 * source of the command it answers.
 */

/** @brief The tag of the first line containing @p needle; 0xFE when none does. */
static uint8_t src_of(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return fake_klog_src(i);
        }
    }
    return 0xFEU;
}

/** @brief How many captured lines contain @p needle. */
static unsigned count_of(const char *needle)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            n++;
        }
    }
    return n;
}

TEST_CASE("the replies of a command carry the port the command came from",
          "[console_net]")
{
    console_up();

    type_from(NET_SRC, "ping");
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">|pong"));
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">ok"));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "ping");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, src_of(">|pong"));
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, src_of(">ok"));

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_BLE, "ping");
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_BLE, src_of(">ok"));
}

TEST_CASE("the errors of a NET line, refusals included, are tagged NET",
          "[console_net]")
{
    uint8_t unknown;
    uint8_t refused;

    console_up();

    type_from(NET_SRC, "nosuchverb");
    unknown = src_of(">err code=1");
    fake_klog_reset();
    type_from(NET_SRC, "file_get name=\"x\"");
    refused = src_of(">err code=5 msg=net");

    TEST_ASSERT_EQUAL_UINT8(NET_SRC, unknown);
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, refused);
}

/* Review Focus 2: a NET line that finds the console busy with a cable command
 * is handed back to AO_Net, which keeps it and offers it again: it is not
 * answered, and nothing the cable command goes on to say reaches the channel.
 * The reading and its terminator are the cable's. */
TEST_CASE("a NET line the console is busy for is not answered; the cable command's "
          "replies stay on the cable", "[console_net]")
{
    PowerEvt p;
    uint8_t  record_src;
    uint8_t  ok_src;
    unsigned oks;
    unsigned busy_replies;

    console_up();

    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "bat_info");
    type_from(NET_SRC, "ping");
    busy_replies = count_of(">err code=4 msg=busy");

    (void)memset(&p, 0, sizeof(p));
    p.super   = (QEvt)QEVT_INITIALIZER(POWER_EVT_SIG);
    p.vbat_mv = 3720U;
    QASM_DISPATCH(AO_Console, &p.super, 0U);
    record_src = src_of(">|vbat_mv=");
    ok_src     = src_of(">ok");
    oks        = count_of(">ok");

    TEST_ASSERT_EQUAL_UINT(0U, busy_replies);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, record_src);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, ok_src);
    TEST_ASSERT_EQUAL_UINT(1U, oks);   /* the pong never ran */

    /* And the console is free again, for a NET line this time. */
    fake_klog_reset();
    type_from(NET_SRC, "ping");
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">ok"));
}

TEST_CASE("a cable line refused as busy gets its refusal on the cable while a "
          "NET command runs", "[console_net]")
{
    PowerEvt p;
    uint8_t  busy_src;
    uint8_t  record_src;

    console_up();

    type_from(NET_SRC, "bat_info");
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "ping");
    busy_src = src_of(">err code=4 msg=busy");

    (void)memset(&p, 0, sizeof(p));
    p.super   = (QEvt)QEVT_INITIALIZER(POWER_EVT_SIG);
    p.vbat_mv = 3720U;
    QASM_DISPATCH(AO_Console, &p.super, 0U);
    record_src = src_of(">|vbat_mv=");

    TEST_ASSERT_EQUAL_UINT8((uint8_t)KODE_CONSOLE_SRC_SERIAL, busy_src);
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, record_src);
    TEST_ASSERT_EQUAL_UINT8(NET_SRC, src_of(">ok"));
}

/* ---- screenshot dst=http: the glass up to the Kode channel's shots link ----
 * The console parses and checks the token, asks AO_Net and prints what AO_Net
 * answers. AO_Net is a mailbox here: the order is read off its queue by hand
 * and the answer is handed to the console the way AO_Net posts it.
 */

/** @brief Command-timeout ticks per second the cases were written in. */
#define SHOT_TICKS_PER_S   100U

/** @brief The console's wait for a capture: 20 s, in ticks. */
#define SHOT_WAIT_TICKS    (20U * SHOT_TICKS_PER_S)

/** @brief A token as long as the contract allows. */
#define SHOT_TOKEN_LONGEST 128U

/** @brief Room for a token one character past the longest, and its NUL. */
#define SHOT_TOKEN_PAST_MAX  (SHOT_TOKEN_LONGEST + 2U)

/** @brief Room for `screenshot dst=http token="<token>"` with that token. */
#define SHOT_LINE_MAX  (sizeof("screenshot dst=http token=\"\"") + SHOT_TOKEN_LONGEST + 1U)

/** One NET_SHOT_REQ_SIG as the case found it. */
typedef struct {
    unsigned count;                    /**< Orders found in AO_Net's queue. */
    NetShotReqEvt first;               /**< A copy of the first.            */
} shot_orders_t;

/** @brief Empty AO_Net's queue, keeping a copy of the first upload order. */
static shot_orders_t shot_orders(void)
{
    shot_orders_t out;
    const QEvt   *e = NULL;

    (void)memset(&out, 0, sizeof(out));
    qf_support_give_queue(AO_Net);
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)NET_SHOT_REQ_SIG == e->sig )
        {
            if ( 0U == out.count )
            {
                (void)memcpy(&out.first, e, sizeof(out.first));
            }
            out.count++;
        }
        QF_gc(e);
    }
    return out;
}

/** @brief The console took a new line: it is not busy. */
static void assert_console_free(void)
{
    fake_klog_reset();
    type_from(NET_SRC, "ping");
    TEST_ASSERT_TRUE_MESSAGE(replied(">|pong"), "the console is still busy");
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("screenshot dst=http asks AO_Net and prints a taken capture",
          "[console_net]")
{
    shot_orders_t o;

    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL,
              "screenshot dst=http token=\"Ab3_x-9\"");

    o = shot_orders();
    TEST_ASSERT_EQUAL_UINT(1U, o.count);
    TEST_ASSERT_TRUE(o.first.who == AO_Console);
    TEST_ASSERT_EQUAL_STRING("Ab3_x-9", o.first.token);
    TEST_ASSERT_EQUAL_UINT8(0U, o.first.fps);
    TEST_ASSERT_EQUAL_UINT16(0U, o.first.secs);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, drain_queue(AO_Display),
                                   "the panel is AO_Net's to ask");
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "answered before AO_Net did");

    shot_answer(KSHOT_R_OK, 201U, 24816U, "k7Qx", 1U, 0U, 0U);

    TEST_ASSERT_TRUE(replied(
        ">|screenshot dst=http bytes=24816 id=\"k7Qx\" seq=1"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));
    assert_console_free();
}

TEST_CASE("a taken capture without an id prints the grammar's none",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();

    shot_answer(KSHOT_R_OK, 200U, 100U, "", 0U, 0U, 0U);

    TEST_ASSERT_TRUE(replied(">|screenshot dst=http bytes=100 id=- seq=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("an id from the server is quoted, whatever it holds",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();

    shot_answer(KSHOT_R_OK, 201U, 9U, "a\"b", 2U, 0U, 0U);

    TEST_ASSERT_TRUE(replied(
        ">|screenshot dst=http bytes=9 id=\"a\\\"b\" seq=2"));
}

TEST_CASE("screenshot dst=http answers a refusal with the server's code",
          "[console_net]")
{
    static const uint16_t CODES[] = { 401U, 404U, 409U, 413U, 415U };

    for ( unsigned i = 0U; (sizeof(CODES) / sizeof(CODES[0])) > i; ++i )
    {
        char want[96];

        console_up();
        type_from(NET_SRC, "screenshot dst=http token=\"t\"");
        (void)shot_orders();
        shot_answer(KSHOT_R_REFUSED, CODES[i], 0U, "", 0U, 0U, 0U);

        (void)snprintf(want, sizeof(want),
                       ">|screenshot dst=http result=refused http=%u",
                       (unsigned)CODES[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(want), want);
        TEST_ASSERT_TRUE(replied(">err code=5 msg=http"));
        TEST_ASSERT_FALSE(replied(">ok"));
        assert_console_free();
    }
}

TEST_CASE("screenshot dst=http with no answer from the server is a timeout "
          "error", "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();
    shot_answer(KSHOT_R_NO_ANSWER, 0U, 0U, "", 0U, 0U, 0U);

    TEST_ASSERT_TRUE(replied(">err code=6 msg=http"));
    TEST_ASSERT_FALSE(replied(">|screenshot"));
    TEST_ASSERT_FALSE(replied(">ok"));
    assert_console_free();
}

TEST_CASE("screenshot dst=http names a busy codec, a big frame and a missing "
          "channel", "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();
    shot_answer(KSHOT_R_BUSY, 0U, 0U, "", 0U, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(replied(">ok"));
    assert_console_free();

    fake_klog_reset();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();
    shot_answer(KSHOT_R_SIZE, 0U, 0U, "", 0U, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=size"));
    assert_console_free();

    fake_klog_reset();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();
    shot_answer(KSHOT_R_CHANNEL, 0U, 0U, "", 0U, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=channel"));
    assert_console_free();
}

TEST_CASE("a stream is answered at once and frees the console",
          "[console_net]")
{
    shot_orders_t o;

    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\" fps=4 secs=60");

    o = shot_orders();
    TEST_ASSERT_EQUAL_UINT(1U, o.count);
    TEST_ASSERT_EQUAL_UINT8(4U, o.first.fps);
    TEST_ASSERT_EQUAL_UINT16(60U, o.first.secs);

    shot_answer(KSHOT_R_STREAM, 0U, 0U, "", 0U, 4U, 60U);

    TEST_ASSERT_TRUE(replied(">|screenshot dst=http mode=stream fps=4 secs=60"));
    TEST_ASSERT_TRUE(replied(">ok"));
    assert_console_free();
}

TEST_CASE("a stream prints the rate and length AO_Net will use",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\" fps=10 secs=300");
    (void)shot_orders();
    shot_answer(KSHOT_R_STREAM, 0U, 0U, "", 0U, 7U, 120U);

    TEST_ASSERT_TRUE_MESSAGE(replied("mode=stream fps=7 secs=120"),
                             "the answer's numbers, not the order's");
}

TEST_CASE("a stream with only one of fps and secs defaults the other",
          "[console_net]")
{
    shot_orders_t o;

    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\" fps=5");
    o = shot_orders();
    TEST_ASSERT_EQUAL_UINT8(5U, o.first.fps);
    TEST_ASSERT_EQUAL_UINT16((uint16_t)KSHOT_SECS_DEFAULT, o.first.secs);
    shot_answer(KSHOT_R_STREAM, 0U, 0U, "", 0U, 5U, (uint16_t)KSHOT_SECS_DEFAULT);

    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\" secs=30");
    o = shot_orders();
    TEST_ASSERT_EQUAL_UINT8((uint8_t)KSHOT_FPS_DEFAULT, o.first.fps);
    TEST_ASSERT_EQUAL_UINT16(30U, o.first.secs);
}

TEST_CASE("screenshot dst=http is allowed from the cable, BLE and NET",
          "[console_net]")
{
    static const uint8_t SOURCES[] = {
        (uint8_t)KODE_CONSOLE_SRC_SERIAL, (uint8_t)KODE_CONSOLE_SRC_BLE,
        (uint8_t)KODE_CONSOLE_SRC_NET,
    };

    for ( unsigned i = 0U; (sizeof(SOURCES) / sizeof(SOURCES[0])) > i; ++i )
    {
        shot_orders_t o;

        console_up();
        type_from(SOURCES[i], "screenshot dst=http token=\"t\"");

        TEST_ASSERT_FALSE_MESSAGE(replied(">err"), "refused");
        o = shot_orders();
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, o.count, "AO_Net was not asked");
        shot_answer(KSHOT_R_OK, 201U, 1U, "i", 1U, 0U, 0U);
        TEST_ASSERT_EQUAL_UINT8(SOURCES[i], src_of(">ok"));
    }
}

TEST_CASE("from NET, screenshot to the console stays refused beside dst=http",
          "[console_net]")
{
    console_up();
    assert_net_refused("screenshot dst=console");
    TEST_ASSERT_EQUAL_UINT(0U, shot_orders().count);
}

TEST_CASE("a token with / or % is refused before anything is asked",
          "[console_net]")
{
    static const char *const BAD[] = {
        "screenshot dst=http token=\"a/b\"",
        "screenshot dst=http token=\"..%2f..\"",
        "screenshot dst=http token=\"a%20b\"",
        "screenshot dst=http token=\"a b\"",
        "screenshot dst=http token=\"a?b=c\"",
        "screenshot dst=http token=\"a\\\\b\"",
        "screenshot dst=http token=\"abc\" fps=2 secs=5 name=\"x\"",
    };
    char  longest[SHOT_TOKEN_PAST_MAX];
    char  line[SHOT_LINE_MAX];

    console_up();
    for ( unsigned i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        fake_klog_reset();
        type_from(NET_SRC, BAD[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg="), BAD[i]);
        TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), BAD[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_orders().count, BAD[i]);
    }

    /* The token check itself, by name. */
    fake_klog_reset();
    type_from(NET_SRC, "screenshot dst=http token=\"a/b\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=token"));

    /* One byte past the longest token. */
    (void)memset(longest, 'a', SHOT_TOKEN_LONGEST + 1U);
    longest[SHOT_TOKEN_LONGEST + 1U] = '\0';
    (void)snprintf(line, sizeof(line), "screenshot dst=http token=\"%s\"",
                   longest);
    fake_klog_reset();
    type_from(NET_SRC, line);
    TEST_ASSERT_TRUE(replied(">err code=3 msg=token"));
    TEST_ASSERT_EQUAL_UINT(0U, shot_orders().count);

    /* The longest one is fine. */
    longest[SHOT_TOKEN_LONGEST] = '\0';
    (void)snprintf(line, sizeof(line), "screenshot dst=http token=\"%s\"",
                   longest);
    fake_klog_reset();
    type_from(NET_SRC, line);
    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_EQUAL_UINT(1U, shot_orders().count);
}

TEST_CASE("a token typed bare is refused by the grammar, not asked",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=abc");

    TEST_ASSERT_TRUE(replied(">err code=3 msg=token"));
    TEST_ASSERT_EQUAL_UINT(0U, shot_orders().count);
}

TEST_CASE("screenshot dst=http without a token is a missing key",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=token"));
    TEST_ASSERT_EQUAL_UINT(0U, shot_orders().count);

    fake_klog_reset();
    type_from(NET_SRC, "screenshot dst=http fps=2");
    TEST_ASSERT_TRUE(replied(">err code=2 msg=token"));
    TEST_ASSERT_EQUAL_UINT(0U, shot_orders().count);
    assert_console_free();
}

TEST_CASE("fps and secs outside their ranges are refused by name",
          "[console_net]")
{
    static const struct {
        const char *line;
        const char *msg;
    } BAD[] = {
        { "screenshot dst=http token=\"t\" fps=0",    "msg=fps"  },
        { "screenshot dst=http token=\"t\" fps=11",   "msg=fps"  },
        { "screenshot dst=http token=\"t\" fps=\"2\"", "msg=fps"  },
        { "screenshot dst=http token=\"t\" secs=0",   "msg=secs" },
        { "screenshot dst=http token=\"t\" secs=301", "msg=secs" },
    };

    console_up();
    for ( unsigned i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        char want[32];

        fake_klog_reset();
        type_from(NET_SRC, BAD[i].line);
        (void)snprintf(want, sizeof(want), ">err code=3 %s", BAD[i].msg);
        TEST_ASSERT_TRUE_MESSAGE(replied(want), BAD[i].line);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_orders().count, BAD[i].line);
    }

    /* The edges are fine. */
    fake_klog_reset();
    type_from(NET_SRC, "screenshot dst=http token=\"t\" fps=10 secs=300");
    TEST_ASSERT_FALSE(replied(">err"));
    TEST_ASSERT_EQUAL_UINT(1U, shot_orders().count);
}

TEST_CASE("token, fps and secs mean nothing to dst=console and dst=file",
          "[console_net]")
{
    static const struct {
        const char *line;
        const char *msg;
    } BAD[] = {
        { "screenshot token=\"t\"",               "msg=token" },
        { "screenshot fps=2",                     "msg=fps"   },
        { "screenshot secs=5",                    "msg=secs"  },
        { "screenshot dst=console token=\"t\"",   "msg=token" },
        { "screenshot dst=file token=\"t\"",      "msg=token" },
        { "screenshot dst=file fps=2",            "msg=fps"   },
        { "screenshot dst=file name=\"s.bmp\" secs=5", "msg=secs" },
    };

    console_up();
    for ( unsigned i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        char want[32];

        fake_klog_reset();
        type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, BAD[i].line);
        (void)snprintf(want, sizeof(want), ">err code=3 %s", BAD[i].msg);
        TEST_ASSERT_TRUE_MESSAGE(replied(want), BAD[i].line);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_orders().count, BAD[i].line);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, drain_queue(AO_Display),
                                       BAD[i].line);
    }

    /* The refusal is the key's, even from NET, where dst=console is refused
     * for its source. */
    fake_klog_reset();
    type_from(NET_SRC, "screenshot dst=console token=\"t\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=token"));
    assert_console_free();
}

TEST_CASE("a capture waits 20 s for AO_Net, then gives up as a timeout",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();

    /* Well past the 5 s every other command gets. */
    (void)qf_support_tick(AO_Console, SHOT_WAIT_TICKS - SHOT_TICKS_PER_S);
    TEST_ASSERT_FALSE_MESSAGE(replied(">err"), "gave up before 20 s");

    (void)qf_support_tick(AO_Console, (2U * SHOT_TICKS_PER_S) + 1U);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=screenshot"));
    assert_console_free();
}

TEST_CASE("an answer that arrives after the timeout is dropped in silence",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();
    (void)qf_support_tick(AO_Console, SHOT_WAIT_TICKS + 1U);
    TEST_ASSERT_TRUE(replied(">err code=6 msg=screenshot"));

    fake_klog_reset();
    shot_answer(KSHOT_R_OK, 201U, 24816U, "late", 9U, 0U, 0U);
    shot_answer(KSHOT_R_STREAM, 0U, 0U, "", 0U, 2U, 60U);
    shot_answer(KSHOT_R_BUSY, 0U, 0U, "", 0U, 0U, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
    assert_console_free();
}

TEST_CASE("an upload answer is not another command's answer", "[console_net]")
{
    PowerEvt p;

    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "bat_info");

    shot_answer(KSHOT_R_OK, 201U, 24816U, "stray", 3U, 0U, 0U);
    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());

    /* bat_info is still waiting, and is closed by its own answer. */
    (void)memset(&p, 0, sizeof(p));
    p.super   = (QEvt)QEVT_INITIALIZER(POWER_EVT_SIG);
    p.vbat_mv = 3720U;
    QASM_DISPATCH(AO_Console, &p.super, 0U);
    TEST_ASSERT_TRUE(replied(">|vbat_mv="));
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("an upload answer with nothing in flight prints nothing",
          "[console_net]")
{
    console_up();

    shot_answer(KSHOT_R_OK, 201U, 24816U, "stray", 3U, 0U, 0U);
    shot_answer(KSHOT_R_REFUSED, 404U, 0U, "", 0U, 0U, 0U);

    TEST_ASSERT_EQUAL_UINT(0U, fake_klog_count());
}

TEST_CASE("a capture in flight refuses the next line as busy", "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http token=\"t\"");
    (void)shot_orders();

    fake_klog_reset();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "screenshot dst=http token=\"u\"");
    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_EQUAL_UINT(0U, shot_orders().count);

    /* Closed by its own answer: the next file starts with a free console. */
    shot_answer(KSHOT_R_BUSY, 0U, 0U, "", 0U, 0U, 0U);
    assert_console_free();
}

TEST_CASE("fps and secs quoted, and a token typed bare, are refused by the "
          "grammar before dst is looked at", "[console_net]")
{
    static const struct {
        const char *line;
        const char *msg;
    } BAD[] = {
        { "screenshot fps=\"2\"",               "msg=fps"   },
        { "screenshot dst=file secs=\"5\"",     "msg=secs"  },
        { "screenshot dst=console token=abc",   "msg=token" },
        { "screenshot dst=file token=abc",      "msg=token" },
        { "screenshot dst=http token=\"t\" fps=two", "msg=fps" },
    };

    console_up();
    for ( unsigned i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        char want[32];

        fake_klog_reset();
        type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, BAD[i].line);
        (void)snprintf(want, sizeof(want), ">err code=3 %s", BAD[i].msg);
        TEST_ASSERT_TRUE_MESSAGE(replied(want), BAD[i].line);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_orders().count, BAD[i].line);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, drain_queue(AO_Display),
                                       BAD[i].line);
    }
    assert_console_free();
}

/* ---- screenshot dst=http stop=on: end the running stream ------------------ */

/** @brief Empty AO_Net's queue; count the stop orders, each from the console. */
static unsigned shot_stops(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    qf_support_give_queue(AO_Net);
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)NET_SHOT_STOP_SIG == e->sig )
        {
            TEST_ASSERT_TRUE(((const NetShotStopEvt *)e)->who == AO_Console);
            n++;
        }
        QF_gc(e);
    }
    return n;
}

TEST_CASE("screenshot dst=http stop=on asks AO_Net to stop and prints its >ok",
          "[console_net]")
{
    console_up();
    type_from(NET_SRC, "screenshot dst=http stop=on");
    TEST_ASSERT_EQUAL_UINT(1U, shot_stops());
    TEST_ASSERT_FALSE_MESSAGE(replied(">ok"), "answered before AO_Net did");

    shot_answer(KSHOT_R_STOPPED, 0U, 0U, "", 0U, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">|screenshot"));
    TEST_ASSERT_FALSE(replied(">err"));
    assert_console_free();
}

TEST_CASE("a stop with no stream running is an error naming the stream",
          "[console_net]")
{
    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "screenshot dst=http stop=on");
    TEST_ASSERT_EQUAL_UINT(1U, shot_stops());

    shot_answer(KSHOT_R_NO_STREAM, 0U, 0U, "", 0U, 0U, 0U);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=stream"));
    TEST_ASSERT_FALSE(replied(">ok"));
    assert_console_free();
}

TEST_CASE("stop is dst=http's alone, names nothing else, and takes on only",
          "[console_net]")
{
    static const char *const BAD[] = {
        "screenshot stop=on",
        "screenshot dst=console stop=on",
        "screenshot dst=file stop=on",
        "screenshot dst=http stop=off",
        "screenshot dst=http stop=on token=\"t\"",
        "screenshot dst=http stop=on fps=2",
        "screenshot dst=http stop=on secs=5",
        "screenshot dst=http stop=on name=\"x\"",
    };

    console_up();
    for ( unsigned i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        fake_klog_reset();
        type_from(NET_SRC, BAD[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=stop"), BAD[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_stops(), BAD[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(0U, shot_orders().count, BAD[i]);
    }

    /* A bool is typed bare: the grammar refuses the quotes first. */
    fake_klog_reset();
    type_from(NET_SRC, "screenshot dst=http stop=\"on\"");
    TEST_ASSERT_TRUE(replied(">err code=3 msg=stop"));
    TEST_ASSERT_EQUAL_UINT(0U, shot_stops());
    assert_console_free();
}

TEST_CASE("net_info says how many MQTT messages were dropped for want of room",
          "[console_net]")
{
    NetInfoEvt n;

    console_up();
    type_from((uint8_t)KODE_CONSOLE_SRC_SERIAL, "net_info");

    (void)memset(&n, 0, sizeof(n));
    n.super      = (QEvt)QEVT_INITIALIZER(NET_INFO_SIG);
    n.rx_dropped = 7U;
    QASM_DISPATCH(AO_Console, &n.super, 0U);

    TEST_ASSERT_TRUE(replied(" mqtt=down rx_dropped=7"));
    TEST_ASSERT_TRUE(replied(">ok"));
}
