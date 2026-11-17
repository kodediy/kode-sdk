/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_mqtt.c
 * @brief The MQTT verbs when the Kode channel holds the client.
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
#include "kdrv_net.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

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

/** @brief The request the console posted to AO_Net is not AO_Net's to read here. */
static void net_mailbox_drain(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Net);
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

static void state_from_net(uint8_t state, uint8_t err)
{
    MqttStateEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(MQTT_STATE_SIG);
    (void)snprintf(e.uri, sizeof(e.uri), "%s", "mqtts://kodes.kode.diy:8883");
    e.state = state;
    e.err   = err;
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

TEST_CASE("mqtt_up refused because the channel holds the client answers busy",
          "[console_mqtt]")
{
    console_up();
    net_mailbox_drain();

    type_line("mqtt_up uri=\"mqtt://127.0.0.1:1883\"");
    net_mailbox_drain();
    state_from_net((uint8_t)KDRV_MQTT_UP, (uint8_t)KDRV_MQTT_ERR_BUSY);

    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(replied(">|mqtt="));
    TEST_ASSERT_FALSE(replied(">ok"));

    /* The command is over: the next one runs. */
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("mqtt_down refused because the channel holds the client answers busy",
          "[console_mqtt]")
{
    console_up();
    net_mailbox_drain();

    type_line("mqtt_down");
    net_mailbox_drain();
    state_from_net((uint8_t)KDRV_MQTT_UP, (uint8_t)KDRV_MQTT_ERR_BUSY);

    TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(replied(">ok"));
}

TEST_CASE("an ordinary state is still a record and an ok", "[console_mqtt]")
{
    console_up();
    net_mailbox_drain();

    type_line("mqtt_info");
    net_mailbox_drain();
    state_from_net((uint8_t)KDRV_MQTT_UP, (uint8_t)KDRV_MQTT_ERR_NONE);

    TEST_ASSERT_TRUE(replied(">|mqtt=up"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub refused because the channel holds "
          "the client answer busy", "[console_mqtt]")
{
    static const char *const LINES[3] = {
        "mqtt_pub topic=\"a/b\" data=\"x\"",
        "mqtt_sub topic=\"a/b\"",
        "mqtt_unsub topic=\"a/b\"",
    };

    console_up();
    net_mailbox_drain();

    for ( unsigned i = 0U; 3U > i; ++i )
    {
        fake_klog_reset();
        type_line(LINES[i]);
        TEST_ASSERT_FALSE_MESSAGE(replied(">ok"),
                                  "the verb must wait for AO_Net's word");
        net_mailbox_drain();
        state_from_net((uint8_t)KDRV_MQTT_UP, (uint8_t)KDRV_MQTT_ERR_BUSY);

        TEST_ASSERT_TRUE(replied(">err code=4 msg=busy"));
        TEST_ASSERT_FALSE(replied(">ok"));
    }
}

TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub are still an ok when AO_Net took "
          "them", "[console_mqtt]")
{
    static const char *const LINES[3] = {
        "mqtt_pub topic=\"a/b\" data=\"x\"",
        "mqtt_sub topic=\"a/b\"",
        "mqtt_unsub topic=\"a/b\"",
    };

    console_up();
    net_mailbox_drain();

    for ( unsigned i = 0U; 3U > i; ++i )
    {
        fake_klog_reset();
        type_line(LINES[i]);
        net_mailbox_drain();
        state_from_net((uint8_t)KDRV_MQTT_UP, (uint8_t)KDRV_MQTT_ERR_NONE);

        TEST_ASSERT_TRUE(replied(">ok"));
        TEST_ASSERT_FALSE(replied(">|mqtt="));   /* an accept, not a state record */
        TEST_ASSERT_FALSE(replied(">err"));
    }
}

TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub with no network are an error, not an ok",
          "[console_mqtt]")
{
    static const char *const LINES[3] = {
        "mqtt_pub topic=\"a/b\" data=\"x\"",
        "mqtt_sub topic=\"a/b\"",
        "mqtt_unsub topic=\"a/b\"",
    };

    console_up();
    net_mailbox_drain();

    for ( unsigned i = 0U; 3U > i; ++i )
    {
        fake_klog_reset();
        type_line(LINES[i]);
        net_mailbox_drain();
        state_from_net((uint8_t)KDRV_MQTT_DOWN, (uint8_t)KDRV_MQTT_ERR_NO_NET);

        TEST_ASSERT_TRUE(replied(">err code=5 msg=no_net"));
        TEST_ASSERT_FALSE(replied(">ok"));
    }
}

TEST_CASE("mqtt_pub, mqtt_sub and mqtt_unsub with no broker are an error, not an ok",
          "[console_mqtt]")
{
    static const char *const LINES[3] = {
        "mqtt_pub topic=\"a/b\" data=\"x\"",
        "mqtt_sub topic=\"a/b\"",
        "mqtt_unsub topic=\"a/b\"",
    };

    console_up();
    net_mailbox_drain();

    for ( unsigned i = 0U; 3U > i; ++i )
    {
        fake_klog_reset();
        type_line(LINES[i]);
        net_mailbox_drain();
        state_from_net((uint8_t)KDRV_MQTT_DOWN, (uint8_t)KDRV_MQTT_ERR_REFUSED);

        TEST_ASSERT_TRUE(replied(">err code=5 msg=no_broker"));
        TEST_ASSERT_FALSE(replied(">ok"));
    }
}

TEST_CASE("mqtt_up with no network is an error; mqtt_info and mqtt_down still report the state",
          "[console_mqtt]")
{
    console_up();
    net_mailbox_drain();

    type_line("mqtt_up uri=\"mqtt://127.0.0.1:1883\"");
    net_mailbox_drain();
    state_from_net((uint8_t)KDRV_MQTT_DOWN, (uint8_t)KDRV_MQTT_ERR_NO_NET);
    TEST_ASSERT_TRUE(replied(">err code=5 msg=no_net"));
    TEST_ASSERT_FALSE(replied(">ok"));

    fake_klog_reset();
    type_line("mqtt_info");
    net_mailbox_drain();
    state_from_net((uint8_t)KDRV_MQTT_DOWN, (uint8_t)KDRV_MQTT_ERR_NO_NET);
    TEST_ASSERT_TRUE(replied(">|mqtt=down"));
    TEST_ASSERT_TRUE(replied("why=no_net"));
    TEST_ASSERT_TRUE(replied(">ok"));
    TEST_ASSERT_FALSE(replied(">err"));
}

TEST_CASE("mqtt_up refuses a uri that cannot be a broker and posts nothing",
          "[console_mqtt]")
{
    static const char *const BAD[] = {
        "mqtt_up uri=\"x\"",
        "mqtt_up uri=\"mqtt://\"",
        "mqtt_up uri=\"mqtts://\"",
        "mqtt_up uri=\"mqtt://:1883\"",
        "mqtt_up uri=\"mqtt:///x\"",
        "mqtt_up uri=\"http://broker\"",
        "mqtt_up uri=\"\"",
        "mqtt_up uri=\"mqtt://@\"",
        "mqtt_up uri=\"mqtt://user:pw@\"",
        "mqtt_up uri=\"mqtt://?\"",
        "mqtt_up uri=\"mqtt://[\"",
        "mqtt_up uri=\"mqtt://[]:1883\"",
        "mqtt_up uri=\"mqtt://broker:abc\"",
        "mqtt_up uri=\"mqtt://broker:0\"",
        "mqtt_up uri=\"mqtt://broker:65536\"",
        "mqtt_up uri=\"mqtt://broker:-1\"",
        "mqtt_up uri=\"mqtt://broker:\"",
        "mqtt_up uri=\"mqtt://bro]ker\"",
        "mqtt_up uri=\"mqtt:// broker.example\"",
        "mqtt_up uri=\"mqtt://broker .example\"",
        "mqtt_up uri=\"mqtt://.\"",
        "mqtt_up uri=\"mqtt://..\"",
        "mqtt_up uri=\"mqtt://-\"",
        "mqtt_up uri=\"mqtt://%\"",
        "mqtt_up uri=\"mqtt://broker.example.\"",
        "mqtt_up uri=\"mqtt://[fe80::g]:1883\"",
    };

    console_up();
    net_mailbox_drain();

    for ( unsigned i = 0U; (sizeof(BAD) / sizeof(BAD[0])) > i; ++i )
    {
        fake_klog_reset();
        type_line(BAD[i]);
        TEST_ASSERT_TRUE_MESSAGE(replied(">err code=3 msg=uri"), BAD[i]);
        TEST_ASSERT_FALSE(replied(">ok"));
        TEST_ASSERT_EQUAL_UINT(0U, (unsigned)uxQueueMessagesWaiting(AO_Net->eQueue));
    }

    /* No command was left open: a good one runs. */
    fake_klog_reset();
    type_line("mqtt_up uri=\"mqtts://broker.example\"");
    TEST_ASSERT_FALSE(replied(">err"));
    net_mailbox_drain();
    state_from_net((uint8_t)KDRV_MQTT_UP, (uint8_t)KDRV_MQTT_ERR_NONE);
    TEST_ASSERT_TRUE(replied(">|mqtt=up"));
}

TEST_CASE("mqtt_up takes a host with a user, a port or brackets", "[console_mqtt]")
{
    static const char *const GOOD[] = {
        "mqtt_up uri=\"mqtt://broker.hivemq.com\"",
        "mqtt_up uri=\"mqtt://broker.emqx.io:1883\"",
        "mqtt_up uri=\"mqtts://user:pw@broker.example:8883/x\"",
        "mqtt_up uri=\"mqtt://[fe80::1]:1883\"",
        "mqtt_up uri=\"mqtt://10.0.0.2:65535\"",
        "mqtt_up uri=\"mqtt://my_broker-1.local\"",
    };

    for ( unsigned i = 0U; (sizeof(GOOD) / sizeof(GOOD[0])) > i; ++i )
    {
        console_up();
        net_mailbox_drain();
        fake_klog_reset();
        type_line(GOOD[i]);
        TEST_ASSERT_FALSE_MESSAGE(replied(">err"), GOOD[i]);
        net_mailbox_drain();
        state_from_net((uint8_t)KDRV_MQTT_UP, (uint8_t)KDRV_MQTT_ERR_NONE);
        TEST_ASSERT_TRUE_MESSAGE(replied(">|mqtt=up"), GOOD[i]);
    }
}

/** @brief Most fillers a case posts to reach a depth, so a bug cannot loop. */
#define MQTT_FILL_GUARD  64U

TEST_CASE("the MQTT verbs answer an error and free the console when AO_Net is within "
          "its margin", "[console_mqtt]")
{
    static const QEvt filler = QEVT_INITIALIZER(MQTT_STATE_REQ_SIG);
    static const char *const LINES[5] = {
        "mqtt_up uri=\"mqtt://127.0.0.1:1883\"",
        "mqtt_down",
        "mqtt_pub topic=\"a/b\" data=\"x\"",
        "mqtt_sub topic=\"a/b\"",
        "mqtt_unsub topic=\"a/b\"",
    };

    console_up();
    net_mailbox_drain();
    for ( uint32_t guard = 0U;
         (MQTT_FILL_GUARD > guard)
         && (uxQueueSpacesAvailable(AO_Net->eQueue) > KQP_NET_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Net, &filler, NULL);
    }

    for ( uint32_t i = 0U; 5U > i; ++i )
    {
        fake_klog_reset();
        type_line(LINES[i]);
        TEST_ASSERT_TRUE(replied(">err"));
        TEST_ASSERT_FALSE(replied(">ok"));
    }
    net_mailbox_drain();

    /* The last refusal left no command open: the next one runs. */
    fake_klog_reset();
    type_line("ping");
    TEST_ASSERT_FALSE(replied(">err"));
}
