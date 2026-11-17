/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_radio_doors.c
 * @brief AO_Radio's request doors: each leaves the queue's margin to the AO's
 *        own traffic, and whoever asked hears the refusal.
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
#include "krows.h"
#include "owners_support.h"
#include "qf_support.h"
#include "radio_knocks.h"

/* The command watchdog, in QP ticks, repeated rather than imported for the
 * reason test_console_flow.c repeats it. */
#define CMD_TIMEOUT_TICKS  ((5000U * 100U) / 1000U)

/** @brief A static event AO_Radio's mailbox holds, to take its room with. */
static const QEvt s_room = QEVT_INITIALIZER(RADIO_STATE_REQ_SIG);

/** @brief Give AO_Radio a mailbox, and empty it. Nobody dispatches it here. */
static void radio_mailbox(void)
{
    const QEvt *e = NULL;

    qf_support_give_queue(AO_Radio);
    while ( xQueueReceive(AO_Radio->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    /* A knock whose event went that way would fold every later post. */
    radio_knocks_reset();
}

/** @brief Take AO_Radio's room down to the doors' margin. */
static void radio_fill(void)
{
    while ( uxQueueSpacesAvailable(AO_Radio->eQueue) > (UBaseType_t)KQP_RADIO_POST_MARGIN )
    {
        TEST_ASSERT_TRUE(QACTIVE_POST_X(AO_Radio, &s_room, 0U, NULL));
    }
}

/** @brief AO_Radio's free slots. */
static unsigned radio_free(void)
{
    return (unsigned)uxQueueSpacesAvailable(AO_Radio->eQueue);
}

/* ---- The doors ------------------------------------------------------------ */

static bool door_radio_down(void)     { return radio_down_request(); }
static bool door_radio_state(void)    { return radio_state_request(); }
static bool door_id_sign(void)        { return id_sign_request(ao_spy(), "abc"); }
static bool door_c5_wipe(void)        { return c5_wipe_request(ao_spy()); }
static bool door_id(void)             { return id_request(ao_spy(), (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U); }
static bool door_c5_flash(void)       { return c5_flash_request((uint8_t)KFW_SRC_SD, "c5.bin"); }
static bool door_c5_recv(void)        { return c5_recv_request(1024U, 0x1234U, (uint8_t)KODE_CONSOLE_SRC_SERIAL); }
static bool door_c5_test(void)        { return c5_test_request(); }
static bool door_c5_test_user(void)   { return c5_test_request_user(); }
static bool door_c5_field(void)       { return c5_field_request(ao_spy(), FIELD_OP_GET, 0U, 0); }
static bool door_exp_power(void)      { return exp_power_request(true); }
#if CONFIG_KODEOS_WIFI
static bool door_wifi_start(void)     { return wifi_start_request(); }
static bool door_wifi_stop(void)      { return wifi_stop_request(); }
static bool door_wifi_state(void)     { return wifi_state_request(); }
static bool door_wifi_fast(void)      { return wifi_fast_request(true); }
static bool door_wifi_rssi(void)      { return wifi_rssi_request(); }
static bool door_wifi_list(void)      { return wifi_list_request(); }
static bool door_wifi_forget(void)    { return wifi_forget_request("home"); }
#endif
#if CONFIG_KODEOS_BLE
static bool door_ble_start(void)      { return ble_start_request(); }
static bool door_ble_stop(void)       { return ble_stop_request(); }
static bool door_ble_unpair(void)     { return ble_unpair_request(); }
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
static bool door_ble_pair_reject(void) { return ble_pair_reject_request(); }
#endif
#endif

/** @brief A door, by the name a failure prints. */
typedef struct {
    const char *name;
    bool (*ask)(void);
} door_t;

static const door_t DOORS[] = {
    { "radio_down_request",      door_radio_down     },
    { "radio_state_request",     door_radio_state    },
    { "id_sign_request",         door_id_sign        },
    { "c5_wipe_request",         door_c5_wipe        },
    { "id_request",              door_id             },
    { "c5_flash_request",        door_c5_flash       },
    { "c5_recv_request",         door_c5_recv        },
    { "c5_test_request",         door_c5_test        },
    { "c5_test_request_user",    door_c5_test_user   },
    { "c5_field_request",        door_c5_field       },
    { "exp_power_request",       door_exp_power      },
#if CONFIG_KODEOS_WIFI
    { "wifi_start_request",      door_wifi_start     },
    { "wifi_stop_request",       door_wifi_stop      },
    { "wifi_state_request",      door_wifi_state     },
    { "wifi_fast_request",       door_wifi_fast      },
    { "wifi_rssi_request",       door_wifi_rssi      },
    { "wifi_list_request",       door_wifi_list      },
    { "wifi_forget_request",     door_wifi_forget    },
#endif
#if CONFIG_KODEOS_BLE
    { "ble_start_request",       door_ble_start      },
    { "ble_stop_request",        door_ble_stop       },
    { "ble_unpair_request",      door_ble_unpair     },
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    { "ble_pair_reject_request", door_ble_pair_reject },
#endif
#endif
};

TEST_CASE("every request door leaves AO_Radio's margin to its own traffic", "[radio_ao][doors]")
{
    radio_mailbox();
    radio_fill();
    for ( size_t i = 0U; (sizeof(DOORS) / sizeof(DOORS[0])) > i; ++i )
    {
        TEST_ASSERT_FALSE_MESSAGE(DOORS[i].ask(), DOORS[i].name);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(KQP_RADIO_POST_MARGIN, radio_free(), DOORS[i].name);
    }
    radio_mailbox();
}

TEST_CASE("a park AO_Radio has no room for is answered as one the C5 refused", "[radio_ao][doors]")
{
    const QEvt *e      = NULL;
    unsigned    parked = 0U;

    qf_support_give_queue(AO_Boot);
    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
    radio_mailbox();
    radio_fill();
    radio_park_request(KDRV_RADIO_NOTE_APP);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(KQP_RADIO_POST_MARGIN, radio_free(), "the park took a slot past the margin");
    while ( xQueueReceive(AO_Boot->eQueue, &e, 0U) == pdTRUE )
    {
        if ( (QSignal)BOOT_RADIO_PARKED_SIG == e->sig )
        {
            ++parked;
        }
        QF_gc(e);
    }
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, parked, "the jump was left waiting for an answer");
    radio_mailbox();
}

/* ---- The self-test rows --------------------------------------------------- */

/** @brief The registered row called @p name, or NULL. */
static const krow_t *row_named(const char *name)
{
    const krow_t *r = NULL;

    for ( size_t k = 0U; krows_count() > k; ++k )
    {
        if ( strcmp(krows_at(k)->name, name) == 0 )
        {
            r = krows_at(k);
        }
    }
    return r;
}

/** @brief The verdicts of @p name the spy holds, and whether each failed; empties it. */
static unsigned failed_verdicts(const char *name)
{
    const QEvt *e;
    unsigned    n = 0U;

    while ( NULL != (e = ao_spy_take()) )
    {
        if ( (QSignal)TEST_RESULT_SIG == e->sig )
        {
            TestResultEvt const * const r = (TestResultEvt const *)e;

            if ( (strcmp(r->name, name) == 0) && !r->ok )
            {
                ++n;
            }
        }
    }
    ao_spy_drain();
    return n;
}

#if CONFIG_KODEOS_AUTOTEST
TEST_CASE("the radio's rows fail at once when AO_Radio's queue has no room", "[radio_ao][doors]")
{
    static const char *const ROWS[] = { "c5", "c5_user" };

    owners_up();
    (void)ao_spy();
    ao_spy_subscribe(TEST_RESULT_SIG);
    for ( size_t i = 0U; (sizeof(ROWS) / sizeof(ROWS[0])) > i; ++i )
    {
        const krow_t * const r = row_named(ROWS[i]);

        TEST_ASSERT_NOT_NULL_MESSAGE(r, ROWS[i]);
        ao_spy_drain();
        radio_mailbox();
        radio_fill();
        TEST_ASSERT_FALSE(r->fn());
        TEST_ASSERT_EQUAL_UINT_MESSAGE(KQP_RADIO_POST_MARGIN, radio_free(), ROWS[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, failed_verdicts("c5"), "the row was left to its budget");
    }
    radio_mailbox();
}
#endif

/* ---- The console ---------------------------------------------------------- */

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
    radio_mailbox();
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

/* Every verb that asks AO_Radio through a door with no room for it answers
 * busy and closes: the ones that disarm the watchdog would otherwise hold the
 * console for ever, and the ones that accept at once would say `>ok` for a
 * request nobody took. */
TEST_CASE("a radio verb with no room in AO_Radio's queue answers busy and leaves the console free",
          "[console][doors]")
{
    static const char *const LINES[] = {
        "radio_down", "radio_info", "exp_power state=on", "c5 field=\"chip_id\"",
        "install src=\"sd:c5.bin\" dst=c5",
#if CONFIG_KODEOS_WIFI
        "wifi_up", "wifi_down", "wifi_list", "wifi_forget ssid=\"home\"", "wifi_info",
#endif
#if CONFIG_KODEOS_BLE
        "ble_up", "ble_down", "ble_unpair",
#endif
    };

    console_up();
    for ( size_t i = 0U; (sizeof(LINES) / sizeof(LINES[0])) > i; ++i )
    {
        radio_mailbox();
        radio_fill();
        fake_klog_reset();
        type_line(LINES[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">err code=4 msg=busy"), LINES[i]);
        TEST_ASSERT_NULL_MESSAGE(line_with(">ok"), LINES[i]);
        TEST_ASSERT_EQUAL_UINT_MESSAGE(KQP_RADIO_POST_MARGIN, radio_free(), LINES[i]);

        /* Free for the next line: `ping` answers at once. */
        fake_klog_reset();
        type_line("ping");
        TEST_ASSERT_NOT_NULL_MESSAGE(line_with(">ok"), LINES[i]);
    }
    radio_mailbox();
}
