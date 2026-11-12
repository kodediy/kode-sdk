/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_ble.c
 * @brief The standard layer's Bluetooth: scan response, listen, the app's service, event names.
 */
#include "kode.h"

#include <inttypes.h>
#include <stddef.h>
#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kqp_config.h"

_Static_assert(KODE_BLE_NAME_MAX == KODE_SYS_BLE_NAME_MAX, "a name is the system's");
_Static_assert(KODE_BLE_ADDR_MAX == KODE_SYS_BLE_ADDR_MAX, "an address is the system's");
_Static_assert(KODE_BLE_SCAN_RSP_MAX == KODE_SYS_BLE_SCAN_RSP_MAX, "the scan response is the system's");
_Static_assert(KODE_BLE_DATA_MIN == KODE_SYS_BLE_DATA_MIN, "the least data is the system's");
_Static_assert(KODE_BLE_SCAN_MS_MIN == KODE_SYS_BLE_SCAN_MS_MIN, "the shortest listen is the system's");
_Static_assert(KODE_BLE_SCAN_MS_MAX == KODE_SYS_BLE_SCAN_MS_MAX, "the longest listen is the system's");
_Static_assert(sizeof(kode_ble_device_t) == sizeof(kode_sys_ble_device_t),
               "a device heard and kode_sys's have drifted apart");
_Static_assert(KODE_BLE_CHARACTERISTICS_MAX == KODE_SYS_BLE_CHARS_MAX, "the characteristics are the system's");
_Static_assert(KODE_BLE_VALUE_MAX == KODE_SYS_BLE_VALUE_MAX, "a value is the system's");
_Static_assert(KODE_BLE_CHARACTERISTIC_NAME_MAX == KODE_SYS_BLE_CHAR_NAME_MAX, "a name is the system's");
_Static_assert(KODE_BLE_READ == KODE_SYS_BLE_READ, "READ is the system's");
_Static_assert(KODE_BLE_WRITE == KODE_SYS_BLE_WRITE, "WRITE is the system's");
_Static_assert(KODE_BLE_NOTIFY == KODE_SYS_BLE_NOTIFY, "NOTIFY is the system's");
_Static_assert(KODE_BLE_QUEUED == KODE_SYS_BLE_QUEUED, "QUEUED is the system's");
_Static_assert(KODE_BLE_QUEUE_SLOTS == KODE_SYS_BLE_QUEUE_SLOTS, "a queue is the system's");

/* The radio reads the constant KODE_BLE_SERVICE() defines as kode_sys's type
 * (kode_sys_ble.c holds that one to the driver's), so the layouts are held
 * equal, field by field. */
_Static_assert(sizeof(kode_ble_characteristic_t) == sizeof(kode_sys_ble_char_t), "a characteristic and the system's");
_Static_assert(offsetof(kode_ble_characteristic_t, name) == offsetof(kode_sys_ble_char_t, name), "name");
_Static_assert(offsetof(kode_ble_characteristic_t, uuid) == offsetof(kode_sys_ble_char_t, uuid), "uuid");
_Static_assert(offsetof(kode_ble_characteristic_t, props) == offsetof(kode_sys_ble_char_t, props), "props");
_Static_assert(sizeof(kode_ble_service_t) == sizeof(kode_sys_ble_service_t), "a service and the system's");
_Static_assert(offsetof(kode_ble_service_t, uuid) == offsetof(kode_sys_ble_service_t, uuid), "uuid");
_Static_assert(offsetof(kode_ble_service_t, chars) == offsetof(kode_sys_ble_service_t, chars), "chars");
_Static_assert(offsetof(kode_ble_service_t, count) == offsetof(kode_sys_ble_service_t, count), "count");

_Static_assert(KODE_SYS_BLE_VALUE_WAIT_MS == 1000U, "kode.h says a value waits a second at most");

/** @brief How long a name or data waits for AO_Radio's queue to have room, in ms. */
#define BLE_DOOR_MS 2000U

/** @brief How long a listen waits past its own length: the link's bring-up, then
 *         the job's own guard. */
#define BLE_SCAN_SLACK_MS 60000U

_Static_assert(BLE_SCAN_SLACK_MS > KODE_RADIO_BLE_SCAN_SLACK_MS,
               "the call outwaits the job's guard, so an answer is never left behind");

/** @brief The lock a listen holds, so tasks take turns; made on the first call. */
static khal_mutex_t s_ble_lock;

/** @brief Make the lock. @return true when it exists. */
static bool ble_lock_make(void)
{
    s_ble_lock = khal_mutex_create();
    return NULL != s_ble_lock;
}

/**
 * @brief kode_enter(), then the user's switch.
 * @param name The public name of the call.
 * @return true when the call may go on; false with the error set.
 */
static bool ble_enter(const char *name)
{
    bool ok = kode_enter(name);

    if ( ok && !kode_sys_ble_enabled() )
    {
#if CONFIG_KODEOS_BLE
        kode_err_set("%s: Bluetooth is off in Settings", name);
#else
        kode_err_set("%s: this firmware has no Bluetooth", name);
#endif
        ok = false;
    }
    return ok;
}

/** @brief One try of a name or data: what kode_sys answered. */
typedef struct {
    const char    *api;     /**< The public name of the call.           */
    bool           is_name; /**< true: the name; false: the data.       */
    const char    *name;    /**< The name.                              */
    const uint8_t *data;    /**< The data; NULL when @c len is 0.       */
    size_t         len;     /**< Its bytes.                             */
    kode_status_t  rc;      /**< The last answer.                       */
} ble_set_t;

/**
 * @brief One try: true once kode_sys answered anything but "no room".
 * @param ctx The ble_set_t.
 * @return true to stop trying.
 */
static bool ble_set_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the ble_set_t ble_set() passed */
    ble_set_t *const s = (ble_set_t *)ctx;

    if ( s->is_name )
    {
        s->rc = kode_sys_ble_set_name(s->name);
    }
    else
    {
        s->rc = kode_sys_ble_set_data(s->data, s->len);
    }
    return KODE_NO_RESOURCES != s->rc;
}

/**
 * @brief Hand a name or data to AO_Radio, waiting for room, and say why it failed.
 * @param s    What to set.
 * @param what "name" or "data", for the error.
 * @return true when AO_Radio has it.
 */
static bool ble_set(ble_set_t *s, const char *what)
{
    bool ok = false;

    s->rc = KODE_NO_RESOURCES;
    (void)kode_retry(&ble_set_attempt, s, BLE_DOOR_MS);
    if ( KODE_OK == s->rc )
    {
        ok = true;
    }
    else if ( KODE_DENIED == s->rc )
    {
        kode_err_set("%s: the %s does not fit: the name and the data share %u bytes, "
                     "2 of them per part", s->api, what, (unsigned)KODE_BLE_SCAN_RSP_MAX);
    }
    else
    {
        kode_err_set("%s: the radio did not take the %s", s->api, what);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ble_set_name(const char *name)
{
    bool ok = false;

    if ( ble_enter("kode_ble_set_name") )
    {
        ble_set_t s = { "kode_ble_set_name", true, name, NULL, 0U, KODE_NO_RESOURCES };

        ok = ble_set(&s, "name");
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ble_set_manufacturer_data(const uint8_t *data, size_t len)
{
    bool ok = false;

    if ( ble_enter("kode_ble_set_manufacturer_data") )
    {
        if ( (0U < len) && (NULL == data) )
        {
            kode_err_set("kode_ble_set_manufacturer_data: no data for %u bytes", (unsigned)len);
        }
        else if ( (0U < len) && (KODE_BLE_DATA_MIN > len) )
        {
            kode_err_set("kode_ble_set_manufacturer_data: the data starts with a %u-byte company id",
                         (unsigned)KODE_BLE_DATA_MIN);
        }
        else
        {
            ble_set_t s = { "kode_ble_set_manufacturer_data", false, NULL, data, len, KODE_NO_RESOURCES };

            ok = ble_set(&s, "data");
        }
    }
    return ok;
}

/** @brief Where kode_ble_scan() keeps the strongest devices while they arrive. */
typedef struct {
    kode_ble_device_t *out;  /**< The caller's array.         */
    size_t             cap;  /**< How many it holds.          */
    size_t             n;    /**< How many it holds so far.   */
} ble_scan_keep_t;

/**
 * @brief Keep @p dev if it is among the strongest @c cap so far, in order.
 *
 * Strongest first; a device as strong as one kept goes after it.
 *
 * @param dev One device.
 * @param ctx The ble_scan_keep_t.
 * @return true: every device is looked at.
 */
static bool ble_scan_keep(const kode_sys_ble_device_t *dev, void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the ble_scan_keep_t kode_ble_scan() passed */
    ble_scan_keep_t *const k  = (ble_scan_keep_t *)ctx;
    size_t                 at = k->n;

    while ( (0U < at) && (k->out[at - 1U].rssi < dev->rssi) )
    {
        at--;
    }
    if ( at < k->cap )
    {
        size_t last = k->n;

        if ( last == k->cap )
        {
            last--;   /* the weakest kept falls off the end */
        }
        else
        {
            k->n++;
        }
        while ( last > at )
        {
            k->out[last] = k->out[last - 1U];
            last--;
        }
        (void)memcpy(k->out[at].name, dev->name, sizeof(k->out[at].name));
        (void)memcpy(k->out[at].address, dev->address, sizeof(k->out[at].address));
        k->out[at].rssi = dev->rssi;
    }
    return true;
}

/**
 * @brief Keep why the listen failed as the task's error.
 * @param rc What the facade answered.
 * @param ms How long it was asked to listen.
 */
static void ble_scan_fail(kode_status_t rc, uint32_t ms)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("kode_ble_scan: Bluetooth is off in Settings");
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("kode_ble_scan: the radio is busy with a scan, a self-test or an install; "
                     "try again");
        break;
    case KODE_TIMEOUT:
        kode_err_set("kode_ble_scan: no answer to the listen within %" PRIu32 " s",
                     (ms + BLE_SCAN_SLACK_MS) / KODE_MS_PER_S);
        break;
    default:
        kode_err_set("kode_ble_scan: the Bluetooth listen failed");
        break;
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
int kode_ble_scan(kode_ble_device_t *out, size_t cap, uint32_t ms)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    int                found = -1;

    if ( !ble_enter("kode_ble_scan") )
    {
        return found;
    }
    if ( (NULL == out) || (0U == cap) )
    {
        kode_err_set("kode_ble_scan: no room for the devices");
    }
    else if ( (KODE_BLE_SCAN_MS_MIN > ms) || (KODE_BLE_SCAN_MS_MAX < ms) )
    {
        kode_err_set("kode_ble_scan: listen for %u to %u ms",
                     (unsigned)KODE_BLE_SCAN_MS_MIN, (unsigned)KODE_BLE_SCAN_MS_MAX);
    }
    else if ( !khal_once_try(&lock_once, &ble_lock_make) )
    {
        kode_err_set("kode_ble_scan: no memory for the Bluetooth lock");
    }
    else
    {
        ble_scan_keep_t keep = { out, cap, 0U };

        khal_mutex_lock(s_ble_lock);
        kode_status_t const rc = kode_block_ble_scan(&ble_scan_keep, &keep, ms,
                                                     ms + BLE_SCAN_SLACK_MS);
        khal_mutex_unlock(s_ble_lock);
        if ( KODE_OK == rc )
        {
            found = (int)keep.n;
        }
        else
        {
            ble_scan_fail(rc, ms);
        }
    }
    return found;
}

/* ---- The app's own service ------------------------------------------------- */

/**
 * @brief Check a set or a notify before the system is asked.
 * @param api  The public name of the call.
 * @param chr  The characteristic's name.
 * @param data The bytes.
 * @param len  Their count.
 * @return true when the system may be asked; false with the error set.
 */
static bool ble_value_enter(const char *api, const char *chr, const void *data, size_t len)
{
    char why[KODE_SYS_BLE_FAULT_MAX];
    bool ok = ble_enter(api);

    if ( !ok )
    {
        /* the error is set */
    }
    else if ( kode_sys_ble_service_fault(why, sizeof(why)) )
    {
        kode_err_set("%s: the service is left out: %s", api, why);
        ok = false;
    }
    else if ( NULL == chr )
    {
        kode_err_set("%s: no characteristic named", api);
        ok = false;
    }
    else if ( KODE_BLE_VALUE_MAX < len )
    {
        kode_err_set("%s: %u bytes; a value holds at most %u", api, (unsigned)len,
                     (unsigned)KODE_BLE_VALUE_MAX);
        ok = false;
    }
    else if ( (0U < len) && (NULL == data) )
    {
        kode_err_set("%s: no data for %u bytes", api, (unsigned)len);
        ok = false;
    }
    else
    {
        /* the system decides the rest */
    }
    return ok;
}

/**
 * @brief Keep why the system refused a set, a notify, a push or a clear as the task's error.
 * @param api The public name of the call.
 * @param chr The characteristic's name.
 * @param rc  What the system answered.
 * @param op  KQP_BLE_APP_OP_SET, _NOTIFY, _PUSH or _CLEAR.
 */
static void ble_value_fail(const char *api, const char *chr, kode_status_t rc, uint8_t op)
{
    bool const queued = (KQP_BLE_APP_OP_PUSH == op) || (KQP_BLE_APP_OP_CLEAR == op);

    if ( (KODE_DENIED == rc) && (KQP_BLE_APP_OP_NOTIFY == op) )
    {
        kode_err_set("%s: the service has no characteristic '%s' that notifies", api, chr);
    }
    else if ( (KODE_DENIED == rc) && queued )
    {
        kode_err_set("%s: the service has no characteristic '%s' with KODE_BLE_QUEUED", api, chr);
    }
    else if ( (KODE_NO_RESOURCES == rc) && queued )
    {
        kode_err_set("%s: the queue of '%s' holds %u values already; push again on ble_taken",
                     api, chr, (unsigned)KODE_BLE_QUEUE_SLOTS);
    }
    else if ( KODE_DENIED == rc )
    {
        kode_err_set("%s: the service has no characteristic '%s', or none is declared "
                     "(KODE_BLE_SERVICE)", api, chr);
    }
    else if ( KODE_NO_RESOURCES == rc )
    {
        kode_err_set("%s: the notifications already fill their ring; the value is set, "
                     "try again", api);
    }
    else if ( KODE_TIMEOUT == rc )
    {
        kode_err_set("%s: the radio did not take the value within %u ms", api,
                     (unsigned)KODE_SYS_BLE_VALUE_WAIT_MS);
    }
    else
    {
        kode_err_set("%s: no memory for the service's values", api);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ble_service_select(const kode_ble_service_t *service)
{
    bool ok = false;

    /* kode_enter() alone: Bluetooth off in Settings keeps the choice too. */
    if ( kode_enter("kode_ble_service_select") )
    {
        /* cppcheck-suppress misra-c2012-11.3 ; layout-compatible types, asserted above */
        ok = (KODE_OK == kode_sys_ble_service_select((const kode_sys_ble_service_t *)service));
        if ( !ok )
        {
            kode_err_set("kode_ble_service_select: Bluetooth has registered its table "
                         "already; the choice is made before it starts");
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ble_set_value(const char *chr, const void *data, size_t len)
{
    static const char api[] = "kode_ble_set_value";
    bool              ok     = false;

    if ( ble_value_enter(api, chr, data, len) )
    {
        kode_status_t const rc = kode_block_ble_value(chr, KQP_BLE_APP_OP_SET, data, len);

        ok = (KODE_OK == rc);
        if ( !ok )
        {
            ble_value_fail(api, chr, rc, KQP_BLE_APP_OP_SET);
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ble_notify(const char *chr, const void *data, size_t len)
{
    static const char api[] = "kode_ble_notify";
    bool              ok     = false;

    if ( ble_value_enter(api, chr, data, len) )
    {
        kode_status_t const rc = kode_block_ble_value(chr, KQP_BLE_APP_OP_NOTIFY, data, len);

        ok = (KODE_OK == rc);
        if ( !ok )
        {
            ble_value_fail(api, chr, rc, KQP_BLE_APP_OP_NOTIFY);
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ble_queue_push(const char *chr, const void *data, size_t len)
{
    static const char api[] = "kode_ble_queue_push";
    bool              ok     = false;

    if ( !ble_value_enter(api, chr, data, len) )
    {
        /* the error is set */
    }
    else if ( 0U == len )
    {
        kode_err_set("%s: no bytes; a read of an empty queue already gets none", api);
    }
    else
    {
        kode_status_t const rc = kode_block_ble_value(chr, KQP_BLE_APP_OP_PUSH, data, len);

        ok = (KODE_OK == rc);
        if ( !ok )
        {
            ble_value_fail(api, chr, rc, KQP_BLE_APP_OP_PUSH);
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
size_t kode_ble_queue_room(const char *chr)
{
    static const char api[] = "kode_ble_queue_room";
    size_t            room   = 0U;

    if ( ble_value_enter(api, chr, NULL, 0U)
        && (kode_sys_ble_queue_room(chr, &room) != KODE_OK) )
    {
        room = 0U;
        ble_value_fail(api, chr, KODE_DENIED, KQP_BLE_APP_OP_CLEAR);
    }
    return room;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_ble_queue_clear(const char *chr)
{
    static const char api[] = "kode_ble_queue_clear";
    bool              ok     = false;

    if ( ble_value_enter(api, chr, NULL, 0U) )
    {
        kode_status_t const rc = kode_block_ble_value(chr, KQP_BLE_APP_OP_CLEAR, NULL, 0U);

        ok = (KODE_OK == rc);
        if ( !ok )
        {
            ble_value_fail(api, chr, rc, KQP_BLE_APP_OP_CLEAR);
        }
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_ble_connected(void)
{
    return KODE_EVENT_BLE_CONNECTED_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_ble_disconnected(void)
{
    return KODE_EVENT_BLE_DISCONNECTED_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_ble_written(void)
{
    return KODE_EVENT_BLE_WRITTEN_NAME;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
const char *kode_event_ble_taken(void)
{
    return KODE_EVENT_BLE_TAKEN_NAME;
}
