/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_ble.c
 * @brief The standard layer's Bluetooth: the user's switch, refusal texts and strongest devices.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"
#include "qf_support.h"

/** @brief The listen the cases ask for, in ms. */
#define SEAM_LISTEN_MS 2000U

/** @brief Devices the scripted listen offers at most. */
#define SEAM_DEV_MAX 12U

static bool          seam_enabled(void);
static kode_status_t seam_set_name(const char *name);
static kode_status_t seam_set_data(const uint8_t *data, size_t len);
static kode_status_t seam_block_scan(kode_sys_ble_device_cb cb, void *ctx, uint32_t ms,
                                     uint32_t wait_ms);
static kode_status_t seam_block_value(const char *chr, uint8_t op, const void *data,
                                      size_t len);
static bool          seam_fault(char *why, size_t cap);
static kode_status_t seam_queue_room(const char *chr, size_t *room);
bool   seam_ble_queue_push(const char *chr, const void *data, size_t len);
size_t seam_ble_queue_room(const char *chr);
bool   seam_ble_queue_clear(const char *chr);
const char *seam_event_ble_taken(void);
bool seam_ble_set_name(const char *name);
bool seam_ble_set_data(const uint8_t *data, size_t len);
int  seam_ble_scan(kode_ble_device_t *out, size_t cap, uint32_t ms);
bool seam_ble_set_value(const char *chr, const void *data, size_t len);
bool seam_ble_notify(const char *chr, const void *data, size_t len);
const char *seam_event_ble_connected(void);
const char *seam_event_ble_disconnected(void);
const char *seam_event_ble_written(void);
bool seam_ble_service_select(const kode_ble_service_t *service);

#define kode_sys_ble_enabled        seam_enabled
#define kode_sys_ble_set_name       seam_set_name
#define kode_sys_ble_set_data       seam_set_data
#define kode_block_ble_scan         seam_block_scan
#define kode_block_ble_value        seam_block_value
#define kode_sys_ble_service_fault  seam_fault
#define kode_sys_ble_queue_room     seam_queue_room
#define kode_ble_queue_push         seam_ble_queue_push
#define kode_ble_queue_room         seam_ble_queue_room
#define kode_ble_queue_clear        seam_ble_queue_clear
#define kode_event_ble_taken        seam_event_ble_taken
#define kode_ble_set_name           seam_ble_set_name
#define kode_ble_set_manufacturer_data           seam_ble_set_data
#define kode_ble_scan               seam_ble_scan
#define kode_ble_set_value          seam_ble_set_value
#define kode_ble_notify             seam_ble_notify
#define kode_event_ble_connected    seam_event_ble_connected
#define kode_event_ble_disconnected seam_event_ble_disconnected
#define kode_event_ble_written      seam_event_ble_written
#define kode_ble_service_select     seam_ble_service_select
#include "kode_ble.c"
#undef kode_sys_ble_enabled
#undef kode_sys_ble_set_name
#undef kode_sys_ble_set_data
#undef kode_block_ble_scan
#undef kode_block_ble_value
#undef kode_sys_ble_service_fault
#undef kode_sys_ble_queue_room
#undef kode_ble_queue_push
#undef kode_ble_queue_room
#undef kode_ble_queue_clear
#undef kode_ble_service_select
#undef kode_event_ble_taken
#undef kode_ble_set_name
#undef kode_ble_set_manufacturer_data
#undef kode_ble_scan
#undef kode_ble_set_value
#undef kode_ble_notify
#undef kode_event_ble_connected
#undef kode_event_ble_disconnected
#undef kode_event_ble_written

static bool                  s_seam_on;                  /**< The user's switch.                */
static kode_status_t         s_seam_set_rc;              /**< What the set doors answer.        */
static uint32_t              s_seam_sets;                /**< Sets the doors took.              */
static const char           *s_seam_name;                /**< The last name handed down.        */
static size_t                s_seam_len;                 /**< The last data's length.           */
static kode_sys_ble_device_t s_seam_devs[SEAM_DEV_MAX];  /**< What the scripted listen offers.  */
static uint32_t              s_seam_devs_n;              /**< How many it offers.               */
static kode_status_t         s_seam_scan_rc;             /**< What it answers.                  */
static uint32_t              s_seam_scans;               /**< Listens it was asked for.         */
static uint32_t              s_seam_scan_ms;             /**< The listen it was handed.         */
static uint32_t              s_seam_wait_ms;             /**< The bound it was handed.          */
static kode_status_t         s_seam_value_rc;            /**< What set_value and notify answer. */
static uint32_t              s_seam_values;              /**< Values and notifies handed down.  */
static uint32_t              s_seam_notifies;            /**< Of those, the notifies.           */
static uint8_t               s_seam_last_op;             /**< The last request's op.            */
static kode_status_t         s_seam_room_rc;             /**< What the room door answers.       */
static size_t                s_seam_room;                /**< The room it reports.              */
static const char           *s_seam_fault;               /**< The reason the service is left out; NULL: served. */

static bool seam_enabled(void)
{
    return s_seam_on;
}

static kode_status_t seam_set_name(const char *name)
{
    s_seam_sets++;
    s_seam_name = name;
    return s_seam_set_rc;
}

static kode_status_t seam_set_data(const uint8_t *data, size_t len)
{
    (void)data;
    s_seam_sets++;
    s_seam_len = len;
    return s_seam_set_rc;
}

static kode_status_t seam_block_value(const char *chr, uint8_t op, const void *data,
                                      size_t len)
{
    (void)chr; (void)data; (void)len;
    s_seam_values++;
    s_seam_last_op = op;
    if ( KQP_BLE_APP_OP_NOTIFY == op )
    {
        s_seam_notifies++;
    }
    return s_seam_value_rc;
}

static kode_status_t seam_queue_room(const char *chr, size_t *room)
{
    (void)chr;
    *room = s_seam_room;
    return s_seam_room_rc;
}

static bool seam_fault(char *why, size_t cap)
{
    if ( NULL != s_seam_fault )
    {
        (void)snprintf(why, cap, "%s", s_seam_fault);
    }
    return NULL != s_seam_fault;
}

static kode_status_t seam_block_scan(kode_sys_ble_device_cb cb, void *ctx, uint32_t ms,
                                     uint32_t wait_ms)
{
    s_seam_scans++;
    s_seam_scan_ms = ms;
    s_seam_wait_ms = wait_ms;
    if ( KODE_OK == s_seam_scan_rc )
    {
        for ( uint32_t i = 0U; i < s_seam_devs_n; i++ )
        {
            if ( !cb(&s_seam_devs[i], ctx) )
            {
                break;
            }
        }
    }
    return s_seam_scan_rc;
}

/**
 * @brief Script the listen: @p n devices whose strengths are @p rssi, named "d-<i>", and its answer.
 * @param rssi The strengths, in the order they were heard.
 * @param n    How many.
 * @param rc   The answer.
 */
static void seam_scan_script(const int8_t *rssi, uint32_t n, kode_status_t rc)
{
    (void)memset(s_seam_devs, 0, sizeof(s_seam_devs));
    for ( uint32_t i = 0U; i < n; i++ )
    {
        (void)snprintf(s_seam_devs[i].name, sizeof(s_seam_devs[i].name), "d-%" PRIu32, i);
        (void)snprintf(s_seam_devs[i].address, sizeof(s_seam_devs[i].address),
                       "c0:00:00:00:00:%02" PRIx32, i);
        s_seam_devs[i].rssi = rssi[i];
    }
    s_seam_devs_n  = n;
    s_seam_scan_rc = rc;
    s_seam_scans   = 0U;
    s_seam_scan_ms = 0U;
    s_seam_wait_ms = 0U;
}

/** @brief A fresh case: the system up, Bluetooth on, every door answering OK. */
static void fresh(void)
{
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    qf_support_init();
    fake_sem_reset();
    fake_time_reset();
    s_seam_on     = true;
    s_seam_set_rc = KODE_OK;
    s_seam_sets   = 0U;
    s_seam_name   = NULL;
    s_seam_len    = 0U;
    s_seam_value_rc = KODE_OK;
    s_seam_values   = 0U;
    s_seam_notifies = 0U;
    s_seam_last_op  = 0xFFU;
    s_seam_room_rc  = KODE_OK;
    s_seam_room     = 0U;
    s_seam_fault    = NULL;
    seam_scan_script(NULL, 0U, KODE_OK);
}

/* ---- The name and the data ---------------------------------------------------- */

TEST_CASE("with Bluetooth off in Settings, every call fails and asks the radio nothing",
          "[kode_ble]")
{
    static const uint8_t DATA[2] = { 0xFFU, 0xFFU };
    kode_ble_device_t    out[2];

    fresh();
    s_seam_on = false;
    TEST_ASSERT_FALSE(seam_ble_set_name("Dot-7"));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "Bluetooth is off in Settings"));
    TEST_ASSERT_FALSE(seam_ble_set_data(DATA, sizeof(DATA)));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "Bluetooth is off in Settings"));
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 2U, SEAM_LISTEN_MS));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "Bluetooth is off in Settings"));
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_sets);
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_scans);
}

TEST_CASE("a name the radio takes answers true, NULL included", "[kode_ble]")
{
    fresh();
    TEST_ASSERT_TRUE(seam_ble_set_name("Dot-7"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_STRING("Dot-7", s_seam_name);
    TEST_ASSERT_TRUE_MESSAGE(seam_ble_set_name(NULL), "NULL goes back to the serial");
    TEST_ASSERT_NULL(s_seam_name);
    TEST_ASSERT_EQUAL_UINT32(2U, s_seam_sets);
}

TEST_CASE("a name or data that does not fit says the two share 31 bytes", "[kode_ble]")
{
    static const uint8_t DATA[4] = { 0xFFU, 0xFFU, 0x01U, 0x02U };

    fresh();
    s_seam_set_rc = KODE_DENIED;
    TEST_ASSERT_FALSE(seam_ble_set_name("a name"));
    TEST_ASSERT_EQUAL_STRING("kode_ble_set_name: the name does not fit: the name and the data "
                             "share 31 bytes, 2 of them per part", kode_error());
    TEST_ASSERT_FALSE(seam_ble_set_data(DATA, sizeof(DATA)));
    TEST_ASSERT_EQUAL_STRING("kode_ble_set_manufacturer_data: the data does not fit: the name and the data "
                             "share 31 bytes, 2 of them per part", kode_error());

    s_seam_set_rc = KODE_ERROR;
    TEST_ASSERT_FALSE(seam_ble_set_name("a name"));
    TEST_ASSERT_EQUAL_STRING("kode_ble_set_name: the radio did not take the name", kode_error());
}

TEST_CASE("data of one byte, or bytes with no data, are refused before the radio is asked",
          "[kode_ble]")
{
    static const uint8_t ONE[1] = { 0x4CU };

    fresh();
    TEST_ASSERT_FALSE(seam_ble_set_data(ONE, 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_set_manufacturer_data: the data starts with a 2-byte company id",
                             kode_error());
    TEST_ASSERT_FALSE(seam_ble_set_data(NULL, 4U));
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(kode_error()));
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_sets);

    TEST_ASSERT_TRUE_MESSAGE(seam_ble_set_data(NULL, 0U), "no data at all clears it");
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_sets);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)s_seam_len);
}

/* ---- The listen --------------------------------------------------------------- */

TEST_CASE("a listen keeps the strongest devices that fit, strongest first", "[kode_ble]")
{
    static const int8_t rssi[8] = { -70, -40, -90, -55, -40, -81, -35, -62 };
    kode_ble_device_t   out[4];

    fresh();
    seam_scan_script(rssi, 8U, KODE_OK);
    (void)memset(out, 0xA5, sizeof(out));

    TEST_ASSERT_EQUAL_INT(4, seam_ble_scan(out, 4U, SEAM_LISTEN_MS));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_scans);
    TEST_ASSERT_EQUAL_UINT32(SEAM_LISTEN_MS, s_seam_scan_ms);
    TEST_ASSERT_EQUAL_UINT32(SEAM_LISTEN_MS + BLE_SCAN_SLACK_MS, s_seam_wait_ms);
    TEST_ASSERT_EQUAL_STRING("d-6", out[0].name);
    TEST_ASSERT_EQUAL_STRING("c0:00:00:00:00:06", out[0].address);
    TEST_ASSERT_EQUAL_INT8(-35, out[0].rssi);
    /* Two at -40: the one heard first stays first. */
    TEST_ASSERT_EQUAL_STRING("d-1", out[1].name);
    TEST_ASSERT_EQUAL_STRING("d-4", out[2].name);
    TEST_ASSERT_EQUAL_STRING("d-3", out[3].name);
}

TEST_CASE("a listen refuses no room or a length out of range before it asks", "[kode_ble]")
{
    static const int8_t rssi[1] = { -50 };
    kode_ble_device_t   out[1];

    fresh();
    seam_scan_script(rssi, 1U, KODE_OK);
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 0U, SEAM_LISTEN_MS));
    TEST_ASSERT_NOT_EQUAL(0, (int)strlen(kode_error()));
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(NULL, 1U, SEAM_LISTEN_MS));
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 1U, KODE_BLE_SCAN_MS_MIN - 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_scan: listen for 500 to 30000 ms", kode_error());
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 1U, KODE_BLE_SCAN_MS_MAX + 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_scans);
    TEST_ASSERT_EQUAL_INT(1, seam_ble_scan(out, 1U, KODE_BLE_SCAN_MS_MIN));
    TEST_ASSERT_EQUAL_INT(1, seam_ble_scan(out, 1U, KODE_BLE_SCAN_MS_MAX));
}

TEST_CASE("a listen that fails says why and returns -1", "[kode_ble]")
{
    static const int8_t rssi[2] = { -50, -60 };
    kode_ble_device_t   out[4];

    fresh();
    seam_scan_script(rssi, 2U, KODE_DENIED);
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 4U, SEAM_LISTEN_MS));
    TEST_ASSERT_EQUAL_STRING("kode_ble_scan: Bluetooth is off in Settings", kode_error());

    seam_scan_script(rssi, 2U, KODE_NO_RESOURCES);
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 4U, SEAM_LISTEN_MS));
    TEST_ASSERT_EQUAL_STRING("kode_ble_scan: the radio is busy with a scan, a self-test or an "
                             "install; try again", kode_error());

    seam_scan_script(rssi, 2U, KODE_TIMEOUT);
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 4U, SEAM_LISTEN_MS));
    TEST_ASSERT_EQUAL_STRING("kode_ble_scan: no answer to the listen within 62 s", kode_error());

    seam_scan_script(rssi, 2U, KODE_ERROR);
    TEST_ASSERT_EQUAL_INT(-1, seam_ble_scan(out, 4U, SEAM_LISTEN_MS));
    TEST_ASSERT_EQUAL_STRING("kode_ble_scan: the Bluetooth listen failed", kode_error());

    seam_scan_script(rssi, 2U, KODE_OK);
    TEST_ASSERT_EQUAL_INT(2, seam_ble_scan(out, 4U, SEAM_LISTEN_MS));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

/* ---- The app's own service ---------------------------------------------------- */

TEST_CASE("with Bluetooth off, a value and a notify fail and ask the radio nothing",
          "[kode_ble]")
{
    fresh();
    s_seam_on = false;
    TEST_ASSERT_FALSE(seam_ble_set_value("temp", "1", 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_set_value: Bluetooth is off in Settings", kode_error());
    TEST_ASSERT_FALSE(seam_ble_notify("temp", "1", 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: Bluetooth is off in Settings", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_values);
}

TEST_CASE("test_ble_service_bad_table_left_out: a service left out makes notify fail with "
          "its reason", "[kode_ble]")
{
    fresh();
    s_seam_fault = "the service has 5 characteristics; at most 4";
    TEST_ASSERT_FALSE(seam_ble_notify("temp", "1", 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: the service is left out: the service has 5 "
                             "characteristics; at most 4", kode_error());
    TEST_ASSERT_FALSE(seam_ble_set_value("temp", "1", 1U));
    TEST_ASSERT_NOT_NULL(strstr(kode_error(), "kode_ble_set_value: the service is left out"));
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_values);
}

TEST_CASE("notify of 245 bytes, of no characteristic or of one that does not notify fails",
          "[kode_ble]")
{
    static uint8_t big[KODE_BLE_VALUE_MAX + 1U];

    fresh();
    TEST_ASSERT_FALSE(seam_ble_notify("temp", big, sizeof(big)));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: 245 bytes; a value holds at most 244", kode_error());
    TEST_ASSERT_FALSE(seam_ble_notify(NULL, big, 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: no characteristic named", kode_error());
    TEST_ASSERT_FALSE(seam_ble_notify("temp", NULL, 3U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: no data for 3 bytes", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_values);

    s_seam_value_rc = KODE_DENIED;
    TEST_ASSERT_FALSE(seam_ble_notify("cmd", big, 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: the service has no characteristic 'cmd' that "
                             "notifies", kode_error());
    TEST_ASSERT_FALSE(seam_ble_set_value("nope", big, 1U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_set_value: the service has no characteristic 'nope', or "
                             "none is declared (KODE_BLE_SERVICE)", kode_error());
}

TEST_CASE("notify with the ring full fails naming it; with room or nobody subscribed it is true",
          "[kode_ble]")
{
    fresh();
    TEST_ASSERT_TRUE(seam_ble_notify("temp", "21.5", 4U));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_TRUE(seam_ble_set_value("temp", NULL, 0U));

    s_seam_value_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_FALSE(seam_ble_notify("temp", "21.5", 4U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: the notifications already fill their ring; the "
                             "value is set, try again", kode_error());

    s_seam_value_rc = KODE_ERROR;
    TEST_ASSERT_FALSE(seam_ble_notify("temp", "21.5", 4U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: no memory for the service's values", kode_error());
    TEST_ASSERT_EQUAL_UINT32(4U, s_seam_values);
    TEST_ASSERT_EQUAL_UINT32(3U, s_seam_notifies);
}

TEST_CASE("a value the radio does not take within a second fails naming the wait",
          "[kode_ble]")
{
    fresh();
    s_seam_value_rc = KODE_TIMEOUT;
    TEST_ASSERT_FALSE(seam_ble_notify("temp", "21.5", 4U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_notify: the radio did not take the value within 1000 ms",
                             kode_error());
    TEST_ASSERT_FALSE(seam_ble_set_value("temp", "21.5", 4U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_set_value: the radio did not take the value within "
                             "1000 ms", kode_error());
}

TEST_CASE("a push and a clear hand their operation down, and a push of no bytes asks nothing",
          "[kode_ble]")
{
    fresh();
    TEST_ASSERT_TRUE(seam_ble_queue_push("log", "ab", 2U));
    TEST_ASSERT_EQUAL_UINT8(KQP_BLE_APP_OP_PUSH, s_seam_last_op);
    TEST_ASSERT_TRUE(seam_ble_queue_clear("log"));
    TEST_ASSERT_EQUAL_UINT8(KQP_BLE_APP_OP_CLEAR, s_seam_last_op);
    TEST_ASSERT_EQUAL_UINT32(2U, s_seam_values);

    TEST_ASSERT_FALSE(seam_ble_queue_push("log", "ab", 0U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_queue_push: no bytes; a read of an empty queue already "
                             "gets none", kode_error());
    TEST_ASSERT_EQUAL_UINT32(2U, s_seam_values);
}

TEST_CASE("a push to a full queue or to a characteristic with no queue fails naming it",
          "[kode_ble]")
{
    fresh();
    s_seam_value_rc = KODE_NO_RESOURCES;
    TEST_ASSERT_FALSE(seam_ble_queue_push("log", "ab", 2U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_queue_push: the queue of 'log' holds 8 values already; "
                             "push again on ble_taken", kode_error());

    s_seam_value_rc = KODE_DENIED;
    TEST_ASSERT_FALSE(seam_ble_queue_push("temp", "ab", 2U));
    TEST_ASSERT_EQUAL_STRING("kode_ble_queue_push: the service has no characteristic 'temp' "
                             "with KODE_BLE_QUEUED", kode_error());
    TEST_ASSERT_FALSE(seam_ble_queue_clear("temp"));
    TEST_ASSERT_EQUAL_STRING("kode_ble_queue_clear: the service has no characteristic 'temp' "
                             "with KODE_BLE_QUEUED", kode_error());
}

TEST_CASE("the room of a queue is what the radio reports, and 0 with the reason when it "
          "refuses", "[kode_ble]")
{
    fresh();
    s_seam_room_rc = KODE_OK;
    s_seam_room    = 5U;
    TEST_ASSERT_EQUAL_UINT(5U, seam_ble_queue_room("log"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());

    s_seam_room_rc = KODE_DENIED;
    TEST_ASSERT_EQUAL_UINT(0U, seam_ble_queue_room("temp"));
    TEST_ASSERT_EQUAL_STRING("kode_ble_queue_room: the service has no characteristic 'temp' "
                             "with KODE_BLE_QUEUED", kode_error());

    s_seam_on = false;
    TEST_ASSERT_EQUAL_UINT(0U, seam_ble_queue_room("log"));
    TEST_ASSERT_EQUAL_STRING("kode_ble_queue_room: Bluetooth is off in Settings", kode_error());
}

TEST_CASE("the four Bluetooth event constants name their events", "[kode_ble]")
{
    TEST_ASSERT_EQUAL_STRING("ble_connected", seam_event_ble_connected());
    TEST_ASSERT_EQUAL_STRING("ble_disconnected", seam_event_ble_disconnected());
    TEST_ASSERT_EQUAL_STRING("ble_written", seam_event_ble_written());
    TEST_ASSERT_EQUAL_STRING("ble_taken", seam_event_ble_taken());
}
