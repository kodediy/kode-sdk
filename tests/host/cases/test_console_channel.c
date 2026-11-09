/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_channel.c
 * @brief `channel_info`: where the Kode channel stands, as one record.
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
#include "khal_identity.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief A line the console keeps whole: the longest record plus slack. */
#define LINE_MAX_HERE  256U

/** @brief Room for the expected `broker="<uri>" since_s=` text with the
 *         longest URI the event carries. */
#define WANT_BROKER_MAX  (sizeof("broker=\"\" since_s=") + KODE_MQTT_URI_MAX)

static void console_up(void)
{
    qf_support_init();
    autotest_up();
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
}

static void type_line(const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
    autotest_typed(text);
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

static unsigned replied_count(const char *needle)
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

/** @brief How many requests the console posted to AO_Net, emptying its queue. */
static unsigned net_mailbox_drain(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    qf_support_give_queue(AO_Net);
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)CHANNEL_STATE_REQ_SIG == e->sig )
        {
            n++;
        }
        QF_gc(e);
    }
    return n;
}

/** The upload fields AO_Net adds to the state. */
typedef struct {
    uint8_t  mode;      /**< kshot_mode_t.                         */
    uint32_t sent;      /**< Frames the server took.               */
    uint16_t fps_x10;   /**< The rate achieved, times ten.         */
    uint32_t left_s;    /**< Seconds the running stream has left.  */
} shot_state_t;

static void state_from_net_shot(uint8_t state, const char *why,
                                uint32_t since_s, uint32_t next_s,
                                uint32_t lost, const char *broker, bool fast,
                                shot_state_t shot)
{
    ChannelStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CHANNEL_STATE_SIG);
    e.state   = state;
    e.since_s = since_s;
    e.next_s  = next_s;
    e.lost    = lost;
    e.fast    = fast;
    e.shot         = shot.mode;
    e.shot_sent    = shot.sent;
    e.shot_fps_x10 = shot.fps_x10;
    e.shot_left_s  = shot.left_s;
    (void)snprintf(e.why, sizeof(e.why), "%s", why);
    (void)snprintf(e.broker, sizeof(e.broker), "%s", broker);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

static void state_from_net(uint8_t state, const char *why, uint32_t since_s,
                           uint32_t next_s, uint32_t lost, const char *broker,
                           bool fast)
{
    shot_state_t const none = { 0U, 0U, 0U, 0U };

    state_from_net_shot(state, why, since_s, next_s, lost, broker, fast, none);
}

/** @brief The serial as the record spells it: quoted, or `-` when unknown. */
static void expected_serial(char *out, size_t cap)
{
    if ( khal_identity_valid() )
    {
        (void)snprintf(out, cap, "\"%s\"", khal_identity_serial());
    }
    else
    {
        (void)snprintf(out, cap, "-");
    }
}

TEST_CASE("channel_info while waiting says why and when the next try is",
          "[console_channel]")
{
    char serial[32];
    char want[LINE_MAX_HERE];

    console_up();
    (void)net_mailbox_drain();

    type_line("channel_info");
    TEST_ASSERT_EQUAL_UINT(1U, net_mailbox_drain());
    state_from_net((uint8_t)CHAN_WAITING, "channel_unavailable", 0U, 40U, 0U, "",
                   false);

    expected_serial(serial, sizeof(serial));
    (void)snprintf(want, sizeof(want),
                   ">|channel=waiting serial=%s broker=- since_s=0 "
                   "why=channel_unavailable next_s=40 lost=0 fast=off "
                   "shot=idle sent=0 fps_got=0.0 left_s=0", serial);
    TEST_ASSERT_TRUE_MESSAGE(replied(want), want);
    TEST_ASSERT_EQUAL_UINT(1U, replied_count(">|channel="));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("channel_info while up has a broker and no next try",
          "[console_channel]")
{
    char serial[32];
    char want[LINE_MAX_HERE];

    console_up();
    (void)net_mailbox_drain();

    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net((uint8_t)CHAN_UP, "", 125U, 0U, 3U,
                   "mqtts://kodes.kode.diy:8883", true);

    expected_serial(serial, sizeof(serial));
    (void)snprintf(want, sizeof(want),
                   ">|channel=up serial=%s broker=\"mqtts://kodes.kode.diy:8883\" "
                   "since_s=125 why=- next_s=- lost=3 fast=on "
                   "shot=idle sent=0 fps_got=0.0 left_s=0", serial);
    TEST_ASSERT_TRUE_MESSAGE(replied(want), want);
    TEST_ASSERT_TRUE(replied(">ok"));
}

TEST_CASE("channel_info names every state", "[console_channel]")
{
    static const char *const NAMES[] = {
        "off", "no_net", "no_identity", "joining", "up", "waiting", "refused",
    };                                           /* chan_state_t order */

    for ( unsigned s = 0U; 7U > s; ++s )
    {
        char want[48];

        console_up();
        (void)net_mailbox_drain();
        type_line("channel_info");
        (void)net_mailbox_drain();
        state_from_net((uint8_t)s, "", 0U, 0U, 0U, "", false);
        (void)snprintf(want, sizeof(want), ">|channel=%s serial=", NAMES[s]);
        TEST_ASSERT_TRUE_MESSAGE(replied(want), want);
        TEST_ASSERT_TRUE(replied(">ok"));
    }
}

TEST_CASE("channel_info ends the command: the next one runs", "[console_channel]")
{
    console_up();
    (void)net_mailbox_drain();

    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net((uint8_t)CHAN_OFF, "", 0U, 0U, 0U, "", false);

    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("a state nobody asked for is not an answer", "[console_channel]")
{
    console_up();
    (void)net_mailbox_drain();

    state_from_net((uint8_t)CHAN_UP, "", 5U, 0U, 0U, "", false);

    TEST_ASSERT_FALSE(replied(">|channel="));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("channel_info keeps a why that is not a word on one line",
          "[console_channel]")
{
    console_up();
    (void)net_mailbox_drain();

    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net((uint8_t)CHAN_REFUSED, "bad \"cert\" x", 0U, 0U, 0U, "", false);

    TEST_ASSERT_TRUE(replied(">|channel=refused serial="));
    TEST_ASSERT_TRUE(replied(" why=bad__cert__x next_s=- lost=0 fast=off "
                             "shot=idle sent=0 fps_got=0.0 left_s=0"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/** @brief Ask, answer with @p shot, and say whether the record holds @p tail. */
static void shot_record(shot_state_t shot, const char *tail)
{
    console_up();
    (void)net_mailbox_drain();
    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net_shot((uint8_t)CHAN_UP, "", 7U, 0U, 0U,
                        "mqtts://kodes.kode.diy:8883", true, shot);

    TEST_ASSERT_TRUE_MESSAGE(replied(tail), tail);
    TEST_ASSERT_EQUAL_UINT(1U, replied_count(">|channel="));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("channel_info while a stream runs says how it is going",
          "[console_channel]")
{
    shot_state_t const s = { (uint8_t)KSHOT_STREAM, 118U, 39U, 30U };

    shot_record(s, " fast=on shot=stream sent=118 fps_got=3.9 left_s=30");
}

TEST_CASE("channel_info keeps the last stream's numbers once it ended",
          "[console_channel]")
{
    shot_state_t const s = { (uint8_t)KSHOT_IDLE, 240U, 40U, 0U };

    shot_record(s, " shot=idle sent=240 fps_got=4.0 left_s=0");
}

TEST_CASE("channel_info while one capture is under way says one",
          "[console_channel]")
{
    shot_state_t const s = { (uint8_t)KSHOT_ONE, 0U, 0U, 0U };

    shot_record(s, " shot=one sent=0 fps_got=0.0 left_s=0");
}

TEST_CASE("fps_got prints tenths with integers: 0.5 and 10.0",
          "[console_channel]")
{
    shot_state_t const half = { (uint8_t)KSHOT_STREAM, 1U, 5U, 9U };
    shot_state_t const ten  = { (uint8_t)KSHOT_STREAM, 50U, 100U, 9U };

    shot_record(half, " fps_got=0.5 left_s=9");
    shot_record(ten, " fps_got=10.0 left_s=9");
}

TEST_CASE("channel_info with every number at its widest is still one whole "
          "record", "[console_channel]")
{
    shot_state_t const s = { (uint8_t)KSHOT_STREAM, 4294967295U, 65535U,
                             4294967295U };

    console_up();
    (void)net_mailbox_drain();
    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net_shot((uint8_t)CHAN_WAITING, "channel_unavailable",
                        4294967295U, 4294967295U, 4294967295U,
                        "mqtts://kodes.kode.diy:8883", true, s);

    TEST_ASSERT_TRUE(replied(" shot=stream sent=4294967295 fps_got=6553.5 "
                             "left_s=4294967295"));
    TEST_ASSERT_TRUE(replied(">ok"));
}

/** @brief A broker URI of exactly @p chars characters, `mqtts://` first. */
static void broker_of(char *out, size_t cap, size_t chars)
{
    (void)memset(out, 'h', cap);
    (void)memcpy(out, "mqtts://", 8U);
    out[chars] = '\0';
}

TEST_CASE("a 90-character broker with a stream running prints whole, with "
          "every shot field", "[console_channel]")
{
    char broker[KODE_MQTT_URI_MAX];
    char want_broker[WANT_BROKER_MAX];
    shot_state_t const s = { (uint8_t)KSHOT_STREAM, 118U, 39U, 30U };

    broker_of(broker, sizeof(broker), 90U);
    (void)snprintf(want_broker, sizeof(want_broker), "broker=\"%s\" since_s=",
                   broker);

    console_up();
    (void)net_mailbox_drain();
    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net_shot((uint8_t)CHAN_UP, "", 7U, 0U, 3U, broker, true, s);

    TEST_ASSERT_TRUE_MESSAGE(replied(want_broker), want_broker);
    TEST_ASSERT_TRUE(replied(" fast=on shot=stream sent=118 fps_got=3.9 "
                             "left_s=30"));
    TEST_ASSERT_EQUAL_UINT(1U, replied_count(">|channel="));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE_MESSAGE(replied("cut to fit"), "the URI was cut");
}

TEST_CASE("the longest broker the event carries prints whole at typical "
          "numbers while the channel is up", "[console_channel]")
{
    char broker[KODE_MQTT_URI_MAX];
    char want_broker[WANT_BROKER_MAX];
    shot_state_t const s = { (uint8_t)KSHOT_STREAM, 3000U, 100U, 300U };

    broker_of(broker, sizeof(broker), KODE_MQTT_URI_MAX - 1U);
    (void)snprintf(want_broker, sizeof(want_broker), "broker=\"%s\" since_s=",
                   broker);

    console_up();
    (void)net_mailbox_drain();
    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net_shot((uint8_t)CHAN_UP, "", 86400U, 0U, 1000U, broker, true, s);

    TEST_ASSERT_TRUE_MESSAGE(replied(want_broker), want_broker);
    TEST_ASSERT_TRUE(replied(" shot=stream sent=3000 fps_got=10.0 left_s=300"));
    TEST_ASSERT_FALSE(replied("cut to fit"));
}

TEST_CASE("at the widest numbers a long broker is cut, never the fields "
          "after it", "[console_channel]")
{
    char broker[KODE_MQTT_URI_MAX];
    shot_state_t const s = { (uint8_t)KSHOT_STREAM, 4294967295U, 65535U,
                             4294967295U };

    broker_of(broker, sizeof(broker), KODE_MQTT_URI_MAX - 1U);

    console_up();
    (void)net_mailbox_drain();
    type_line("channel_info");
    (void)net_mailbox_drain();
    state_from_net_shot((uint8_t)CHAN_NO_IDENTITY, "abcdefghijklmnopqrstuvw",
                        4294967295U, 0U, 4294967295U, broker, true, s);

    TEST_ASSERT_TRUE(replied("\" since_s=4294967295 why=abcdefghijklmnopqrstuvw"));
    TEST_ASSERT_TRUE(replied(" shot=stream sent=4294967295 fps_got=6553.5 "
                             "left_s=4294967295"));
    TEST_ASSERT_TRUE_MESSAGE(replied("cut to fit"), "a cut says so");
    TEST_ASSERT_TRUE(replied(">ok"));
}

/** @brief Dispatch a state of the channel that carries a session age. */
static void state_with_keep(uint8_t state, uint32_t keep_s, uint8_t session)
{
    ChannelStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CHANNEL_STATE_SIG);
    e.state  = state;
    e.keep_s = keep_s;
    e.session = session;
    (void)snprintf(e.broker, sizeof(e.broker), "mqtts://kodes.kode.diy:8883");
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

TEST_CASE("channel_info ends with the session age and what the broker did with it",
          "[console_channel]")
{
    static const struct {
        uint8_t     state;
        uint32_t    keep_s;
        uint8_t     session;
        const char *tail;
    } rows[] = {
        { (uint8_t)CHAN_UP,      86400U,      (uint8_t)CHAN_SESSION_RESUMED,
          " left_s=0 keep=86400 session=resumed" },
        { (uint8_t)CHAN_UP,      3600U,       (uint8_t)CHAN_SESSION_NEW,
          " left_s=0 keep=3600 session=new" },
        { (uint8_t)CHAN_UP,      0xFFFFFFFFU, (uint8_t)CHAN_SESSION_RESUMED,
          " left_s=0 keep=never session=resumed" },
        { (uint8_t)CHAN_WAITING, 0U,          (uint8_t)CHAN_SESSION_NONE,
          " left_s=0 keep=- session=-" },
    };

    console_up();
    for ( unsigned i = 0U; (sizeof(rows) / sizeof(rows[0])) > i; i++ )
    {
        (void)net_mailbox_drain();
        type_line("channel_info");
        (void)net_mailbox_drain();
        state_with_keep(rows[i].state, rows[i].keep_s, rows[i].session);
        TEST_ASSERT_TRUE_MESSAGE(replied(rows[i].tail), rows[i].tail);
        TEST_ASSERT_TRUE(replied(">ok"));
    }
}
