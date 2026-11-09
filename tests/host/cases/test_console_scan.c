/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_console_scan.c
 * @brief `ble_scan` and `wifi_scan`, from the wire.
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
#include "kdrv_ble.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"

/* The command watchdog, in QP ticks, repeated rather than imported for the
 * reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

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
    /* A scan disarms the console's watchdog, so a listing an earlier case
     * left open is closed here by its end marker, not by the clock. */
    {
        WifiScanEndEvt end;

        (void)memset(&end, 0, sizeof(end));
        end.super = (QEvt)QEVT_INITIALIZER(BLE_SCAN_END_SIG);
        QASM_DISPATCH(AO_Console, &end.super, 0U);
        end.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_END_SIG);
        QASM_DISPATCH(AO_Console, &end.super, 0U);
    }
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);

    qf_support_give_queue(AO_Radio);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    fake_klog_reset();
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

/** @brief The first console line holding @p needle, or NULL. */
static const char *line_with(const char *needle)
{
    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        const char * const l = fake_klog_line(i);
        if ( (NULL != l) && (strstr(l, needle) != NULL) )
        {
            return l;
        }
    }
    return NULL;
}

/** @brief The BLE scan length the console posted, or UINT32_MAX for none. */
static uint32_t posted_ble_ms(void)
{
    const QEvt *e  = NULL;
    uint32_t    ms = UINT32_MAX;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)BLE_SCAN_REQ_SIG == e->sig )
        {
            ms = ((const BleScanReqEvt *)e)->ms;
        }
        QF_gc(e);
    }
    return ms;
}

/** @brief Whether the console posted a request of @p sig to AO_Radio. */
static bool posted(enum_t sig)
{
    const QEvt *e   = NULL;
    bool        hit = false;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)sig == e->sig )
        {
            hit = true;
        }
        QF_gc(e);
    }
    return hit;
}

/** @brief Close a listing the way AO_Radio does. */
static void end_listing(enum_t sig)
{
    WifiScanEndEvt end;

    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(sig);
    end.ok    = true;
    QASM_DISPATCH(AO_Console, &end.super, 0U);
}

/** @brief A tracker's advert: a random address, Apple's id, two services. */
static void tracker(BleScanEvt *d)
{
    static const uint8_t ADDR[6] = { 0xC0U, 0x11U, 0x22U, 0x33U, 0x44U, 0x55U };
    static const uint8_t MFG[4]  = { 0x4CU, 0x00U, 0x12U, 0x19U };

    (void)memset(d, 0, sizeof(*d));
    d->super = (QEvt)QEVT_INITIALIZER(BLE_SCAN_ENTRY_SIG);
    (void)memcpy(d->addr, ADDR, sizeof(ADDR));
    d->rssi      = -60;
    (void)snprintf(d->name, sizeof(d->name), "tag");
    d->addr_type = (uint8_t)KDRV_BLE_ADDR_RANDOM;
    d->mfg_len   = (uint8_t)sizeof(MFG);
    (void)memcpy(d->mfg, MFG, sizeof(MFG));
    d->svc_n     = 2U;
    d->svc16[0]  = 0xFD5AU;
    d->svc16[1]  = 0xFEAAU;
}

TEST_CASE("ble_scan prints the address type, maker data and services",
          "[console_scan]")
{
    BleScanEvt d;

    console_up();
    type_line("ble_scan");
    TEST_ASSERT_TRUE(posted(BLE_SCAN_REQ_SIG));

    tracker(&d);
    QASM_DISPATCH(AO_Console, &d.super, 0U);
    end_listing(BLE_SCAN_END_SIG);

    TEST_ASSERT_NOT_NULL_MESSAGE(
        line_with(">|addr=c0:11:22:33:44:55 rssi=-60 name=\"tag\" type=random "
                  "mfg=4c001219 svc=fd5a,feaa"),
        "the record a host parses");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("ble_scan prints - for a device that sent none", "[console_scan]")
{
    BleScanEvt d;

    console_up();
    type_line("ble_scan");
    TEST_ASSERT_TRUE(posted(BLE_SCAN_REQ_SIG));

    tracker(&d);
    d.addr_type = (uint8_t)KDRV_BLE_ADDR_PUBLIC;
    d.mfg_len   = 0U;
    d.svc_n     = 0U;
    QASM_DISPATCH(AO_Console, &d.super, 0U);
    end_listing(BLE_SCAN_END_SIG);

    TEST_ASSERT_NOT_NULL(line_with("name=\"tag\" type=public mfg=- svc=-"));
}

/* The worst a device can send: a name of quotes, which the record escapes to
 * twice its length, the whole maker data and every service. A NET command's
 * replies travel one per MQTT message, so the record must fit one. */
TEST_CASE("the longest ble_scan record fits one channel message",
          "[console_scan]")
{
    BleScanEvt  d;
    const char *l;

    console_up();
    type_line("ble_scan");
    TEST_ASSERT_TRUE(posted(BLE_SCAN_REQ_SIG));

    tracker(&d);
    d.rssi = -100;
    (void)memset(d.name, '"', sizeof(d.name) - 1U);
    d.name[sizeof(d.name) - 1U] = '\0';
    d.mfg_len = (uint8_t)KDRV_BLE_MFG_MAX;
    (void)memset(d.mfg, 0xABU, sizeof(d.mfg));
    d.svc_n = (uint8_t)KDRV_BLE_SVC_MAX;
    QASM_DISPATCH(AO_Console, &d.super, 0U);
    end_listing(BLE_SCAN_END_SIG);

    l = line_with(">|addr=");
    TEST_ASSERT_NOT_NULL(l);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(l, " svc="), "the record was cut short");
    TEST_ASSERT_LESS_OR_EQUAL_size_t((size_t)KODE_MQTT_PAYLOAD_MAX, strlen(l) + 1U);
}

TEST_CASE("wifi_scan prints the BSSID", "[console_scan]")
{
    static const uint8_t BSSID[6] = { 0x3CU, 0x84U, 0x6AU, 0x0AU, 0x0BU, 0x0CU };
    WifiScanEntryEvt n;

    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));

    (void)memset(&n, 0, sizeof(n));
    n.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_ENTRY_SIG);
    (void)snprintf(n.ssid, sizeof(n.ssid), "home");
    n.rssi     = -40;
    n.channel  = 6U;
    n.authmode = 3U;
    (void)memcpy(n.bssid, BSSID, sizeof(BSSID));
    QASM_DISPATCH(AO_Console, &n.super, 0U);
    end_listing(WIFI_SCAN_END_SIG);

    TEST_ASSERT_NOT_NULL(
        line_with(">|ssid=\"home\" rssi=-40 ch=6 auth=3 bssid=3c:84:6a:0a:0b:0c"));
}

/** @brief Close a listing the C5 had no time for, the way AO_Radio does. */
static void end_busy(enum_t sig)
{
    WifiScanEndEvt end;

    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(sig);
    end.busy  = true;
    QASM_DISPATCH(AO_Console, &end.super, 0U);
}

TEST_CASE("a scan the C5 had no time for is answered busy, code 4, not rejected",
          "[console_scan]")
{
    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    end_busy(WIFI_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));

    type_line("ble_scan");
    end_busy(BLE_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
}

TEST_CASE("ble_scan ms= posts that length, and none posts the default",
          "[console_scan]")
{
    console_up();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32(1500U, posted_ble_ms());
    end_listing(BLE_SCAN_END_SIG);

    type_line("ble_scan");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, posted_ble_ms(),
                                     "AO_Radio picks the default");
    end_listing(BLE_SCAN_END_SIG);
}

TEST_CASE("ble_scan ms= out of range is refused before anything is posted",
          "[console_scan]")
{
    char line[48];

    console_up();
    (void)snprintf(line, sizeof(line), "ble_scan ms=%u",
                   (unsigned)(KODE_BLE_SCAN_MIN_MS - 1U));
    type_line(line);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, posted_ble_ms());
    TEST_ASSERT_NOT_NULL(line_with(">err"));

    fake_klog_reset();
    (void)snprintf(line, sizeof(line), "ble_scan ms=%u",
                   (unsigned)(KODE_BLE_SCAN_MAX_MS + 1U));
    type_line(line);
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, posted_ble_ms());
    TEST_ASSERT_NOT_NULL(line_with(">err"));
}

/* The request is an event of the small pool now, and a dry pool drops it.
 * The console has disarmed its watchdog for a scan, so a drop it did not
 * hear about would leave it busy until a reset: it answers at once. */
TEST_CASE("ble_scan on a dry event pool is refused and leaves the console free",
          "[console_scan]")
{
    const QEvt *held[KQP_SMALL_POOL_COUNT + 1U];
    unsigned    n = 0U;

    console_up();
    while ( (unsigned)(KQP_SMALL_POOL_COUNT + 1U) > n )
    {
        BleScanReqEvt * const e = Q_NEW_X(BleScanReqEvt, 0U, BLE_SCAN_REQ_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT((unsigned)KQP_SMALL_POOL_COUNT, n);

    type_line("ble_scan");
    for ( unsigned i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, posted_ble_ms());
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err"), "the drop was not reported");

    fake_klog_reset();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1500U, posted_ble_ms(),
                                     "the console is free for the next command");
    end_listing(BLE_SCAN_END_SIG);
}

/* The same hole in wifi_join: its request is a pool event, the console
 * disarms its watchdog for the join, and a request the pool dropped would
 * leave it busy until a reset. */
TEST_CASE("wifi_join on a dry event pool is refused and leaves the console free",
          "[console_scan]")
{
    const QEvt *held[KQP_MEDIUM_POOL_COUNT + 1U];
    unsigned    n = 0U;

    console_up();
    while ( (unsigned)(KQP_MEDIUM_POOL_COUNT + 1U) > n )
    {
        WifiJoinEvt * const e = Q_NEW_X(WifiJoinEvt, 0U, WIFI_JOIN_REQ_SIG);
        if ( NULL == e )
        {
            break;
        }
        held[n] = &e->super;
        ++n;
    }
    TEST_ASSERT_LESS_OR_EQUAL_UINT((unsigned)KQP_MEDIUM_POOL_COUNT, n);

    type_line("wifi_join ssid=\"home\"");
    for ( unsigned i = 0U; i < n; ++i )
    {
        QF_gc(held[i]);
    }
    TEST_ASSERT_FALSE(posted(WIFI_JOIN_REQ_SIG));
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err"), "the drop was not reported");

    fake_klog_reset();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1500U, posted_ble_ms(),
                                     "the console is free for the next command");
    end_listing(BLE_SCAN_END_SIG);
}

/* The Wi-Fi doors post with a margin, so a radio queue within it is the same
 * refusal as a dry pool, and the console reports it the same way: all three
 * verbs have disarmed or would hold their command open for an answer that a
 * refused request never brings. */
TEST_CASE("wifi_join, wifi_leave and wifi_auto on a full radio queue are refused and leave the console free",
          "[console_scan]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);
    static const char *const lines[] = {
        "wifi_join ssid=\"home\"", "wifi_leave", "wifi_auto"
    };

    console_up();
    for ( uint32_t i = 0U; (uint32_t)(sizeof(lines) / sizeof(lines[0])) > i; ++i )
    {
        for ( uint32_t guard = 0U;
             (64U > guard)
             && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_POST_MARGIN);
             ++guard )
        {
            QACTIVE_POST(AO_Radio, &filler, NULL);
        }
        fake_klog_reset();
        type_line(lines[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err"), lines[i]);
        TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN,
                               uxQueueSpacesAvailable(AO_Radio->eQueue));
        TEST_ASSERT_FALSE(posted(WIFI_JOIN_REQ_SIG));   /* also empties the queue */
    }

    fake_klog_reset();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1500U, posted_ble_ms(),
                                     "the console is free for the next command");
    end_listing(BLE_SCAN_END_SIG);
}

TEST_CASE("wifi_leave and wifi_auto with room post their request without an error",
          "[console_scan]")
{
    console_up();
    type_line("wifi_leave");
    TEST_ASSERT_NULL(line_with(">err"));
    TEST_ASSERT_TRUE(posted(WIFI_LEAVE_REQ_SIG));
    (void)qf_support_tick(AO_Console, CMD_TIMEOUT_TICKS + 1U);   /* closes the open command */
    fake_klog_reset();

    type_line("wifi_auto");
    TEST_ASSERT_NULL(line_with(">err"));
    TEST_ASSERT_TRUE(posted(WIFI_AUTO_REQ_SIG));
    {
        WifiStateEvt state;

        (void)memset(&state, 0, sizeof(state));
        state.super = (QEvt)QEVT_INITIALIZER(WIFI_STATE_SIG);
        QASM_DISPATCH(AO_Console, &state.super, 0U);   /* AO_Radio's answer */
    }
}
