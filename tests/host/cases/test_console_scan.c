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
#include "kdrv_wifi.h"
#include "kqp.h"
#include "qf_support.h"
#include "owners_support.h"
#include "radio_knocks.h"

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
    /* The BLE state ask is a knock, which AO_Radio's initial transition makes
     * on a board before anyone asks; posted() takes it as AO_Radio would. */
    radio_knocks_reset();
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
        if ( (QSignal)BLE_STATE_REQ_SIG == e->sig )
        {
            (void)radio_knocks_take(RADIO_KNOCK_STATE, NULL);
        }
        QF_gc(e);
    }
    return hit;
}

/**
 * @brief The generation of the join the console posted to AO_Radio; empties
 *        the queue.
 * @return It, or KQP_SCAN_GEN_NONE when no join was posted.
 */
static uint8_t posted_join_gen(void)
{
    const QEvt *e   = NULL;
    uint8_t     gen = (uint8_t)KQP_SCAN_GEN_NONE;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)WIFI_JOIN_REQ_SIG == e->sig )
        {
            gen = ((const WifiJoinEvt *)(const void *)e)->gen;
        }
        QF_gc(e);
    }
    return gen;
}

/**
 * @brief Publish to the console a Wi-Fi state, the way AO_Radio does.
 * @param state   kdrv_wifi_state_t.
 * @param gen     The join it ends; KQP_SCAN_GEN_NONE for none.
 * @param dropped That join never ran.
 */
static void wifi_state_to_console(uint8_t state, uint8_t gen, bool dropped)
{
    WifiStateEvt st;

    (void)memset(&st, 0, sizeof(st));
    st.super        = (QEvt)QEVT_INITIALIZER(WIFI_STATE_SIG);
    st.state        = state;
    st.join_gen     = gen;
    st.join_dropped = dropped;
    QASM_DISPATCH(AO_Console, &st.super, 0U);
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

/* `ble_info` is the one record that says what the board calls itself on the
 * air, so a script can check a change of name without a scanner. */
TEST_CASE("ble_info says the name the board goes by, quoted as every text is",
          "[console_scan]")
{
    BleStateEvt s;

    console_up();
    type_line("ble_info");

    (void)memset(&s, 0, sizeof(s));
    s.super = (QEvt)QEVT_INITIALIZER(BLE_STATE_SIG);
    s.state = (uint8_t)KDRV_BLE_ADVERTISING;
    (void)snprintf(s.name, sizeof(s.name), "%s", "KODE a\"b");
    QASM_DISPATCH(AO_Console, &s.super, 0U);

    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(" name=\"KODE a\\\"b\""),
                                 "the name a host parses");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    (void)posted(BLE_STATE_REQ_SIG);
}

/* `pair=` has three words: a time to pair that waits for the connected device
 * to leave is neither on nor off. */
TEST_CASE("ble_info says pair=wait while Pair new device waits for the link",
          "[console_scan]")
{
    BleStateEvt s;

    console_up();
    type_line("ble_info");
    (void)memset(&s, 0, sizeof(s));
    s.super     = (QEvt)QEVT_INITIALIZER(BLE_STATE_SIG);
    s.state     = (uint8_t)KDRV_BLE_CONNECTED;
    s.pair_wait = true;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
    TEST_ASSERT_NOT_NULL(line_with(" pair=wait"));

    /* A time that runs with a device on the link shows the board no more
     * than one that has not started. */
    fake_klog_reset();
    type_line("ble_info");
    s.pair_wait = false;
    s.pair_open = true;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(" pair=wait"), "said on with nothing on the air");

    fake_klog_reset();
    type_line("ble_info");
    s.state = (uint8_t)KDRV_BLE_ADVERTISING;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
    TEST_ASSERT_NOT_NULL(line_with(" pair=on"));

    /* And a board switched off shows itself to nobody. */
    fake_klog_reset();
    type_line("ble_info");
    s.state = (uint8_t)KDRV_BLE_OFF;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(" pair=off"), "said on with Bluetooth off");
    (void)posted(BLE_STATE_REQ_SIG);
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

/** @brief Close a listing an app's rest of the radio turned away, the way AO_Radio does. */
static void end_rest(enum_t sig, uint8_t gen)
{
    WifiScanEndEvt end;

    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(sig);
    end.rest  = true;
    end.gen   = gen;
    QASM_DISPATCH(AO_Console, &end.super, 0U);
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

/* ---- wifi_scan_stop ----------------------------------------------------------------
 * The one verb the console takes while another command is in flight: a scan
 * holds the console until its last channel, and the line that ends it early
 * cannot wait for that.
 */

/** @brief One network of a listing, the way AO_Radio publishes it. */
static void list_network(const char *ssid)
{
    WifiScanEntryEvt n;

    (void)memset(&n, 0, sizeof(n));
    n.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_ENTRY_SIG);
    (void)snprintf(n.ssid, sizeof(n.ssid), "%s", ssid);
    n.rssi     = -40;
    n.channel  = 6U;
    n.authmode = 3U;
    QASM_DISPATCH(AO_Console, &n.super, 0U);
}

/** @brief Close a listing whose sweep was cut, the way AO_Radio does. */
static void end_cut(void)
{
    WifiScanEndEvt end;

    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_END_SIG);
    end.cut   = true;
    QASM_DISPATCH(AO_Console, &end.super, 0U);
}

TEST_CASE("wifi_scan_stop posts its request and answers ok", "[console_scan]")
{
    console_up();
    type_line("wifi_scan_stop");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_NULL(line_with(">err"));
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_STOP_REQ_SIG));

    /* An accept, not a command that waits: the console is free at once. */
    fake_klog_reset();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32(1500U, posted_ble_ms());
    end_listing(BLE_SCAN_END_SIG);
}

TEST_CASE("wifi_scan_stop on a full radio queue is refused busy and leaves the console free",
          "[console_scan]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    console_up();
    for ( uint32_t guard = 0U;
         (64U > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_POST_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    fake_klog_reset();
    type_line("wifi_scan_stop");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NULL(line_with(">ok"));
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_STOP_REQ_SIG));   /* also empties the queue */

    fake_klog_reset();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1500U, posted_ble_ms(),
                                     "the console is free for the next command");
    end_listing(BLE_SCAN_END_SIG);
}

TEST_CASE("ble_pair on a full radio queue is refused busy and leaves the console free",
          "[console_scan]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);

    console_up();
    for ( uint32_t guard = 0U;
         (64U > guard)
         && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_POST_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    fake_klog_reset();
    type_line("ble_pair");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NULL(line_with(">ok"));
    TEST_ASSERT_EQUAL_UINT(KQP_RADIO_POST_MARGIN, uxQueueSpacesAvailable(AO_Radio->eQueue));
    TEST_ASSERT_FALSE(posted(BLE_PAIR_OPEN_REQ_SIG));   /* also empties the queue */

    fake_klog_reset();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1500U, posted_ble_ms(),
                                     "the console is free for the next command");
    end_listing(BLE_SCAN_END_SIG);
}

TEST_CASE("wifi_scan_stop is taken while wifi_scan holds the console, which keeps its command",
          "[console_scan]")
{
    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    list_network("one");
    fake_klog_reset();

    type_line("wifi_scan_stop");
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "the stop was refused behind the scan");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_STOP_REQ_SIG));

    /* The scan is still the command in flight: its step on the air is listed,
     * its end closes it, and a line typed meanwhile is still turned away. */
    fake_klog_reset();
    list_network("two");
    TEST_ASSERT_NOT_NULL(line_with(">|ssid=\"two\""));
    type_line("wifi_leave");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(posted(WIFI_LEAVE_REQ_SIG));

    fake_klog_reset();
    end_cut();
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stopped"));

    fake_klog_reset();
    type_line("ble_scan ms=1500");
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1500U, posted_ble_ms(),
                                     "the scan's end did not free the console");
    end_listing(BLE_SCAN_END_SIG);
}

TEST_CASE("a stopped wifi_scan prints its networks and ends stopped, code 5",
          "[console_scan]")
{
    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    list_network("one");
    end_cut();

    TEST_ASSERT_NOT_NULL(line_with(">|ssid=\"one\""));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stopped"));
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "a cut list was closed as a whole one");
    TEST_ASSERT_NULL(line_with("msg=wifi_scan"));
}

TEST_CASE("a line that ends no scan is still answered busy while wifi_scan holds the console",
          "[console_scan]")
{
    static const char *const lines[] = {
        "wifi_leave", "wifi_auto", "wifi_info", "wifi_up", "ble_scan ms=1500", "ble_down",
        "ping", "wifi_scan_stopp"
    };

    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    for ( uint32_t i = 0U; (uint32_t)(sizeof(lines) / sizeof(lines[0])) > i; ++i )
    {
        fake_klog_reset();
        type_line(lines[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err code=4 msg=busy"), lines[i]);
        TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), lines[i]);
    }
    TEST_ASSERT_FALSE(posted(WIFI_LEAVE_REQ_SIG));
    end_listing(WIFI_SCAN_END_SIG);
}

TEST_CASE("wifi_scan_stop with a key is refused", "[console_scan]")
{
    console_up();
    type_line("wifi_scan_stop now=on");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=now"));
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_STOP_REQ_SIG));

    /* Behind a command in flight the line is not looked at any further:
     * busy, as every line but the bare verb is. */
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    fake_klog_reset();
    type_line("wifi_scan_stop now=on");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_STOP_REQ_SIG));
    end_listing(WIFI_SCAN_END_SIG);
}

/* The Kode channel's lines run in order: its sender holds a line the console
 * is busy for and offers it again, the stop included. */
TEST_CASE("wifi_scan_stop off the Kode channel waits its turn behind a command in flight",
          "[console_scan]")
{
    ConsoleLineEvt e;

    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    fake_klog_reset();

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = (uint8_t)KODE_CONSOLE_SRC_NET;
    (void)snprintf(e.line, sizeof(e.line), "wifi_scan_stop");
    QASM_DISPATCH(AO_Console, &e.super, 0U);

    TEST_ASSERT_NULL(line_with(">ok"));
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_STOP_REQ_SIG));
    end_listing(WIFI_SCAN_END_SIG);
}

/** @brief The BLE state AO_Radio answers a refused request with. */
static void refuse_ble(uint8_t why)
{
    BleStateEvt s;

    (void)memset(&s, 0, sizeof(s));
    s.super   = (QEvt)QEVT_INITIALIZER(BLE_STATE_SIG);
    s.state   = (uint8_t)KDRV_BLE_CONNECTED;
    s.refused = why;
    QASM_DISPATCH(AO_Console, &s.super, 0U);
}

/* The warning goes to the log, and on the BLE console the terminator is all a
 * client has: a refusal that ends in `>ok` reads as done. */
TEST_CASE("a BLE verb AO_Radio refuses ends in an error that says why, never in ok",
          "[console_scan]")
{
    console_up();
    type_line("ble_unpair");
    refuse_ble((uint8_t)KQP_BLE_REFUSED_ATTACHED);
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=attached"));
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "a refusal read as done");
    TEST_ASSERT_NULL_MESSAGE(line_with(">|ble="), "a refusal carries no record");

    fake_klog_reset();
    type_line("ble_pair");
    refuse_ble((uint8_t)KQP_BLE_REFUSED_OFF);
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=off"));
    TEST_ASSERT_NULL(line_with(">ok"));

    fake_klog_reset();
    type_line("ble_unpair");
    refuse_ble((uint8_t)KQP_BLE_REFUSED_BUSY);
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NULL(line_with(">ok"));

    /* The console is free again: a state that refuses nothing answers as before. */
    fake_klog_reset();
    type_line("ble_info");
    refuse_ble((uint8_t)KQP_BLE_REFUSED_NONE);
    TEST_ASSERT_NOT_NULL(line_with(">|ble=connected"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
    (void)posted(BLE_STATE_REQ_SIG);
}

/* ---- A newer radio line takes the scan's place --------------------------------------
 * The console runs one command at a time, and a scan holds it for tens of
 * seconds. The lines whose request ends the sweep in AO_Radio end the command
 * too: it is closed on its own port, stopped, and the new line runs.
 */

/** @brief The sequence the Kode channel gives the next line typed from it. */
static uint8_t s_line_seq;

/** @brief A line from @p src, as its port's reader posts it. */
static void type_line_from(uint8_t src, const char *text)
{
    ConsoleLineEvt e;

    (void)memset(&e, 0, sizeof(e));
    e.super = (QEvt)QEVT_INITIALIZER(CONSOLE_LINE_SIG);
    e.src   = src;
    e.seq   = s_line_seq;
    (void)snprintf(e.line, sizeof(e.line), "%s", text);
    QASM_DISPATCH(AO_Console, &e.super, 0U);
}

/** @brief Tell the listeners the listing starts again, the way AO_Radio does. */
static void listing_again(void)
{
    WifiScanEndEvt again;

    (void)memset(&again, 0, sizeof(again));
    again.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_AGAIN_SIG);
    again.cut   = true;
    QASM_DISPATCH(AO_Console, &again.super, 0U);
}

TEST_CASE("a second wifi_scan ends the first, stopped, and holds the console itself",
          "[console_scan][newest]")
{
    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    list_network("one");
    fake_klog_reset();

    type_line("wifi_scan");
    TEST_ASSERT_NULL_MESSAGE(line_with(">err code=4 msg=busy"), "the newer scan was turned away");
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err code=5 msg=stopped"),
                                 "the first scan was not told it ended early");
    TEST_ASSERT_TRUE_MESSAGE(posted(WIFI_SCAN_REQ_SIG), "the newer scan was not asked for");

    /* The new command is in flight: its listing is printed and its end closes it. */
    fake_klog_reset();
    listing_again();
    TEST_ASSERT_NULL_MESSAGE(line_with(">|scan=again"), "a listing with no network yet has nothing to restart");
    list_network("two");
    TEST_ASSERT_NOT_NULL(line_with(">|ssid=\"two\""));
    end_listing(WIFI_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("wifi_join ends the wifi_scan in flight, stopped, and is asked for",
          "[console_scan][newest]")
{
    uint8_t gen;

    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    fake_klog_reset();

    type_line("wifi_join ssid=\"net\" pass=\"pw12345678\"");
    TEST_ASSERT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stopped"));
    gen = posted_join_gen();
    TEST_ASSERT_NOT_EQUAL_MESSAGE((int)KQP_SCAN_GEN_NONE, (int)gen, "the join was not asked for");

    /* The cut sweep's end is not the join's answer; the state is. */
    fake_klog_reset();
    end_cut();
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "the scan's end closed the join");
    wifi_state_to_console((uint8_t)KDRV_WIFI_IDLE, gen, false);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("a second ble_scan ends the first, stopped, and its own length is posted",
          "[console_scan][newest]")
{
    console_up();
    type_line("ble_scan ms=3000");
    TEST_ASSERT_EQUAL_UINT32(3000U, posted_ble_ms());
    fake_klog_reset();

    type_line("ble_scan ms=1500");
    TEST_ASSERT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stopped"));
    TEST_ASSERT_EQUAL_UINT32(1500U, posted_ble_ms());

    fake_klog_reset();
    end_listing(BLE_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("a scan of one radio ends no scan of the other: the line is answered busy",
          "[console_scan][newest]")
{
    console_up();
    type_line("ble_scan ms=3000");
    TEST_ASSERT_EQUAL_UINT32(3000U, posted_ble_ms());
    fake_klog_reset();

    type_line("wifi_scan");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NULL(line_with("msg=stopped"));
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_REQ_SIG));

    type_line("wifi_join ssid=\"net\"");
    TEST_ASSERT_FALSE_MESSAGE(posted(WIFI_JOIN_REQ_SIG), "a Wi-Fi line ended the Bluetooth listen");

    /* The listen is still the command in flight. */
    fake_klog_reset();
    end_listing(BLE_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("a radio line ends no command but a scan: behind a join it is answered busy",
          "[console_scan][newest]")
{
    uint8_t gen;

    console_up();
    type_line("wifi_join ssid=\"net\" pass=\"pw12345678\"");
    gen = posted_join_gen();
    TEST_ASSERT_NOT_EQUAL((int)KQP_SCAN_GEN_NONE, (int)gen);
    fake_klog_reset();

    type_line("wifi_scan");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NULL(line_with("msg=stopped"));
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_REQ_SIG));

    wifi_state_to_console((uint8_t)KDRV_WIFI_IDLE, gen, false);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

/* While a join waits for the radio, AO_Radio still says where Wi-Fi stands:
 * a refresh of the strength the Wi-Fi page asks for, the automatic round
 * asked again. Those states, and the end of somebody else's join, are not
 * this join's answer. */
TEST_CASE("wifi_join is answered by its own join's end, never by another state",
          "[console_scan][join_gen]")
{
    uint8_t gen;

    console_up();
    type_line("wifi_join ssid=\"net\" pass=\"wrong-pass\"");
    gen = posted_join_gen();
    fake_klog_reset();

    wifi_state_to_console((uint8_t)KDRV_WIFI_GOT_IP, (uint8_t)KQP_SCAN_GEN_NONE, false);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the state from before the join answered it");
    TEST_ASSERT_NULL(line_with(">|wifi="));

    TEST_ASSERT_NOT_EQUAL((int)KQP_SCAN_GEN_NONE, (int)gen);
    wifi_state_to_console((uint8_t)KDRV_WIFI_GOT_IP, (uint8_t)(gen + 1U), false);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "another join's end answered it");
    wifi_state_to_console((uint8_t)KDRV_WIFI_GOT_IP, (uint8_t)(gen + 1U), true);
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "another join's drop answered it");

    wifi_state_to_console((uint8_t)KDRV_WIFI_DISCONNECTED, gen, false);
    TEST_ASSERT_NOT_NULL(line_with(">|wifi=disconnected"));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    fake_klog_reset();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE_MESSAGE(posted(WIFI_SCAN_REQ_SIG), "the console was not free");
    end_listing(WIFI_SCAN_END_SIG);
}

TEST_CASE("a wifi_join whose join never ran says where Wi-Fi stands, then dropped",
          "[console_scan][join_gen]")
{
    uint8_t gen;

    console_up();
    type_line("wifi_join ssid=\"net\" pass=\"pw12345678\"");
    gen = posted_join_gen();
    TEST_ASSERT_NOT_EQUAL((int)KQP_SCAN_GEN_NONE, (int)gen);
    fake_klog_reset();

    wifi_state_to_console((uint8_t)KDRV_WIFI_IDLE, gen, true);
    TEST_ASSERT_NOT_NULL(line_with(">|wifi=idle"));
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=dropped"));
    TEST_ASSERT_NULL(line_with(">ok"));

    fake_klog_reset();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE_MESSAGE(posted(WIFI_SCAN_REQ_SIG), "the console was not free");
    end_listing(WIFI_SCAN_END_SIG);
}

/* A line takes a scan's place only from the port that asked for the scan:
 * the Kode channel's and the scheduler's wait their turn behind the cable's
 * command, and another port's is answered busy. */
TEST_CASE("a wifi_scan from another port ends no scan: it waits its turn or is answered busy",
          "[console_scan][newest]")
{
    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    fake_klog_reset();

    type_line_from((uint8_t)KODE_CONSOLE_SRC_NET, "wifi_scan");
    TEST_ASSERT_NULL_MESSAGE(line_with("msg=stopped"), "the channel's line ended the cable's scan");
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "the channel's line was refused, not handed back");
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_REQ_SIG));

    type_line_from((uint8_t)KODE_CONSOLE_SRC_SCHED, "wifi_scan");
    TEST_ASSERT_NULL_MESSAGE(line_with("msg=stopped"), "the scheduler's line ended the cable's scan");
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_REQ_SIG));

    type_line_from((uint8_t)KODE_CONSOLE_SRC_BLE, "wifi_scan");
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));
    TEST_ASSERT_NULL(line_with("msg=stopped"));
    TEST_ASSERT_FALSE(posted(WIFI_SCAN_REQ_SIG));

    /* The cable's scan is still the command in flight. */
    fake_klog_reset();
    end_listing(WIFI_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

/* A listing somebody else's newer wifi_scan started again goes on: the
 * networks printed so far are marked as the cut sweep's, once. */
TEST_CASE("a listing that starts again says so behind the networks already printed",
          "[console_scan][newest]")
{
    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    list_network("one");
    fake_klog_reset();

    listing_again();
    TEST_ASSERT_NOT_NULL(line_with(">|scan=again"));
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "the restart closed the command");
    TEST_ASSERT_NULL(line_with(">ok"));

    fake_klog_reset();
    listing_again();
    TEST_ASSERT_NULL_MESSAGE(line_with(">|scan=again"), "said twice with nothing printed between");
    list_network("two");
    end_listing(WIFI_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">|ssid=\"two\""));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("a ble_scan a newer request ended says stopped, code 5",
          "[console_scan][newest]")
{
    WifiScanEndEvt end;

    console_up();
    type_line("ble_scan ms=3000");
    TEST_ASSERT_EQUAL_UINT32(3000U, posted_ble_ms());
    fake_klog_reset();

    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(BLE_SCAN_END_SIG);
    end.cut   = true;
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stopped"));
    TEST_ASSERT_NULL(line_with("msg=ble_scan"));
}

/* A line that will not run replaces nothing: the scan gives way only once the
 * line has been validated. */
TEST_CASE("a line that is refused ends no scan: the scan goes on and the line gets its own error",
          "[console_scan][newest]")
{
    console_up();
    type_line("wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    fake_klog_reset();

    type_line("wifi_join ssid=net");          /* a string typed bare */
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ssid"));
    TEST_ASSERT_NULL_MESSAGE(line_with("msg=stopped"), "a refused line ended the scan");
    TEST_ASSERT_FALSE(posted(WIFI_JOIN_REQ_SIG));

    fake_klog_reset();
    type_line("wifi_join");                   /* its required key is missing */
    TEST_ASSERT_NOT_NULL(line_with(">err"));
    TEST_ASSERT_NULL(line_with("msg=stopped"));
    TEST_ASSERT_FALSE(posted(WIFI_JOIN_REQ_SIG));

    fake_klog_reset();
    list_network("one");
    end_listing(WIFI_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">|ssid=\"one\""), "the scan lost the console");
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    /* The same for a listen whose length is out of range. */
    type_line("ble_scan ms=3000");
    TEST_ASSERT_EQUAL_UINT32(3000U, posted_ble_ms());
    fake_klog_reset();
    type_line("ble_scan ms=1");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ms"));
    TEST_ASSERT_NULL(line_with("msg=stopped"));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, posted_ble_ms());
    fake_klog_reset();
    end_listing(BLE_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

/** @brief Lines of one burst: more than AO_Radio's queue holds. */
#define BURST_LINES  (KQP_RADIO_QUEUE_LEN + 8U)

/* A block of lines read in one go, as one BLE write or a paste brings them:
 * AO_Radio runs below the console and reads none of them meanwhile. */
TEST_CASE("a burst of wifi_scan lines is answered busy past the margin and never fills the radio's queue",
          "[console_scan][newest]")
{
    static const QEvt filler = QEVT_INITIALIZER(WIFI_STATE_REQ_SIG);
    unsigned const    panics = fake_panic_count();

    console_up();
    /* The harness gives AO_Radio a deeper queue than the board's: fill it
     * down to the board's depth, so the margin is met where it would be. */
    for ( uint32_t guard = 0U;
         (64U > guard) && (uxQueueSpacesAvailable(AO_Radio->eQueue) > KQP_RADIO_QUEUE_LEN);
         ++guard )
    {
        QACTIVE_POST(AO_Radio, &filler, NULL);
    }
    fake_klog_reset();
    for ( uint32_t i = 0U; BURST_LINES > i; ++i )
    {
        type_line("wifi_scan");
        TEST_ASSERT_TRUE_MESSAGE(uxQueueSpacesAvailable(AO_Radio->eQueue) >= KQP_RADIO_POST_MARGIN,
                                 "a scan request took a slot kept for the worker and the link");
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(panics, fake_panic_count(), "the burst filled AO_Radio's queue");
    TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err code=4 msg=busy"),
                                 "a request with no room was not reported");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));   /* also empties the queue */

    /* A refused request holds nothing: the console takes the next command. */
    fake_klog_reset();
    type_line("wifi_scan");
    (void)posted(WIFI_SCAN_REQ_SIG);
    end_listing(WIFI_SCAN_END_SIG);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

/** @brief The generation of the scan request the console posted; empties AO_Radio's queue. */
static uint8_t posted_scan_gen(void)
{
    const QEvt *e   = NULL;
    uint8_t     gen = (uint8_t)KQP_SCAN_GEN_NONE;

    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)WIFI_SCAN_REQ_SIG == e->sig )
        {
            gen = ((const WifiScanReqEvt *)(const void *)e)->gen;
        }
        if ( (QSignal)BLE_SCAN_REQ_SIG == e->sig )
        {
            gen = ((const BleScanReqEvt *)(const void *)e)->gen;
        }
        QF_gc(e);
    }
    return gen;
}

/* What AO_Radio published before it read the request is still in the
 * console's queue when the command begins: the end of the listing before
 * it, and its networks. Their generation is older, and they are not the
 * command's. */
TEST_CASE("the end and the networks of the listing before a wifi_scan are not taken for its own",
          "[console_scan][newest]")
{
    WifiScanEntryEvt n;
    WifiScanEndEvt   end;
    uint8_t          gen;

    console_up();
    type_line("wifi_scan");
    gen = posted_scan_gen();
    TEST_ASSERT_NOT_EQUAL((uint8_t)KQP_SCAN_GEN_NONE, gen);
    fake_klog_reset();

    (void)memset(&n, 0, sizeof(n));
    n.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_ENTRY_SIG);
    (void)snprintf(n.ssid, sizeof(n.ssid), "%s", "stale");
    n.gen = (uint8_t)(gen - 1U);
    QASM_DISPATCH(AO_Console, &n.super, 0U);
    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_END_SIG);
    end.ok    = true;
    end.gen   = (uint8_t)(gen - 1U);
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_NULL_MESSAGE(line_with("stale"), "a network of the listing before was printed");
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the end of the listing before closed the command");

    /* Its own, and a newer one's for a listing that started again, are taken. */
    (void)snprintf(n.ssid, sizeof(n.ssid), "%s", "fresh");
    n.gen = gen;
    QASM_DISPATCH(AO_Console, &n.super, 0U);
    end.gen = (uint8_t)(gen + 1U);
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">|ssid=\"fresh\""));
    TEST_ASSERT_NOT_NULL(line_with(">ok"));

    /* A refusal that names another request is not this command's either:
     * its own listing is in flight while somebody else's scan is turned away. */
    type_line("wifi_scan");
    gen = posted_scan_gen();
    fake_klog_reset();
    (void)memset(&end, 0, sizeof(end));
    end.super = (QEvt)QEVT_INITIALIZER(WIFI_SCAN_END_SIG);
    end.busy  = true;
    end.gen   = (uint8_t)(gen + 1U);
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "somebody else's refusal closed the command");
    end.gen = gen;
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=busy"));

    /* The same for a listen. */
    (void)memset(&end, 0, sizeof(end));
    end.ok = true;
    type_line("ble_scan ms=3000");
    gen = posted_scan_gen();
    fake_klog_reset();
    end.super = (QEvt)QEVT_INITIALIZER(BLE_SCAN_END_SIG);
    end.gen   = (uint8_t)(gen - 1U);
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), "the end of the listen before closed the command");
    end.gen = gen;
    QASM_DISPATCH(AO_Console, &end.super, 0U);
    TEST_ASSERT_NOT_NULL(line_with(">ok"));
}

TEST_CASE("a scan refused while an app rests the radio says so, code 4, and only its own closes it",
          "[console_scan]")
{
    uint8_t gen;

    console_up();
    type_line("wifi_scan");
    gen = posted_scan_gen();
    fake_klog_reset();
    end_rest(WIFI_SCAN_END_SIG, (uint8_t)(gen + 1U));
    TEST_ASSERT_NULL_MESSAGE(line_with(">err"), "somebody else's refusal closed the command");
    end_rest(WIFI_SCAN_END_SIG, gen);
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=\"radio: at rest\""));

    type_line("ble_scan");
    gen = posted_scan_gen();
    fake_klog_reset();
    end_rest(BLE_SCAN_END_SIG, gen);
    TEST_ASSERT_NOT_NULL(line_with(">err code=4 msg=\"radio: at rest\""));
}

/** @brief The sequence of the last line end AO_Net was told; set by net_ends(). */
static uint8_t s_done_seq;

/** @brief How many line ends AO_Net's mailbox holds; empties it. */
static unsigned net_ends(void)
{
    const QEvt *e = NULL;
    unsigned    n = 0U;

    qf_support_give_queue(AO_Net);
    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)NET_LINE_DONE_SIG == e->sig )
        {
            s_done_seq = ((const NetLineDoneEvt *)(const void *)e)->seq;
            ++n;
        }
        QF_gc(e);
    }
    return n;
}

/* A scan outlasts the time AO_Net gives a line, so the channel's next line
 * can find the channel's own scan in flight. Each of the two lines ends under
 * its own sequence: the line's error is the line's, and the scan's end the
 * scan's. */
TEST_CASE("a channel line behind the channel's scan ends under its own sequence, and the scan under its own",
          "[console_scan][newest]")
{
    uint8_t gen;

    console_up();
    (void)net_ends();

    s_line_seq = 31U;
    type_line_from((uint8_t)KODE_CONSOLE_SRC_NET, "wifi_scan");
    TEST_ASSERT_TRUE(posted(WIFI_SCAN_REQ_SIG));
    TEST_ASSERT_EQUAL_UINT(0U, net_ends());

    /* Refused: the line ends, as itself, and the scan is still out. */
    fake_klog_reset();
    s_line_seq = 32U;
    type_line_from((uint8_t)KODE_CONSOLE_SRC_NET, "wifi_join ssid=net");
    TEST_ASSERT_NOT_NULL(line_with(">err code=3 msg=ssid"));
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_ends(), "the refused line was not ended");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(32U, s_done_seq, "the refused line ended as the scan");

    /* Valid: the scan ends under its own sequence, and the join is out. */
    fake_klog_reset();
    s_line_seq = 33U;
    type_line_from((uint8_t)KODE_CONSOLE_SRC_NET, "wifi_join ssid=\"net\"");
    TEST_ASSERT_NOT_NULL(line_with(">err code=5 msg=stopped"));
    gen = posted_join_gen();
    TEST_ASSERT_NOT_EQUAL((int)KQP_SCAN_GEN_NONE, (int)gen);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, net_ends(), "the scan's line was not ended, or the join's was");
    TEST_ASSERT_EQUAL_UINT8_MESSAGE(31U, s_done_seq, "the scan ended as the line that took its place");

    /* The join's answer ends the join's line. */
    wifi_state_to_console((uint8_t)KDRV_WIFI_IDLE, gen, false);
    TEST_ASSERT_EQUAL_UINT(1U, net_ends());
    TEST_ASSERT_EQUAL_UINT8(33U, s_done_seq);
    s_line_seq = 0U;
}

