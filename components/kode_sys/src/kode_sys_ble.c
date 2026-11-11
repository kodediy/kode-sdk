/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_sys_ble.c
 * @brief Bluetooth through AO_Radio: scan response, AO and blocking listens, the user's switch.
 */
#include "kode_sys.h"
#include "kode_sys_internal.h"
#include "kode_sys_priv.h"

#include "kaos.h"
#include "kdrv_ble.h"
#include "khal_sync.h"
#include "sdkconfig.h"

#include <stdio.h>
#include <stddef.h>
#include <string.h>

#if CONFIG_KODEOS_BLE

_Static_assert(KODE_SYS_BLE_NAME_MAX == KQP_BLE_NAME_MAX,
               "the facade's name and the request's have drifted apart");
_Static_assert(KODE_SYS_BLE_SCAN_RSP_MAX == KDRV_BLE_ADV_MAX,
               "the scan response is the driver's packet");
_Static_assert(KODE_SYS_BLE_DATA_MIN == KDRV_BLE_MFG_ID_LEN,
               "the least data is the company id");
_Static_assert(KODE_SYS_BLE_SCAN_MAX == KDRV_BLE_SCAN_MAX,
               "the facade's listen ceiling and the driver's table have drifted apart");
_Static_assert(KODE_SYS_BLE_SCAN_MAX <= (uint32_t)UINT8_MAX,
               "a waiter counts its rows in a byte");
_Static_assert(KODE_SYS_BLE_SCAN_MS_MIN == KODE_BLE_SCAN_MIN_MS,
               "the shortest listen is AO_Radio's");
_Static_assert(KODE_SYS_BLE_SCAN_MS_MAX == KODE_BLE_SCAN_MAX_MS,
               "the longest listen is AO_Radio's");
_Static_assert(offsetof(kode_sys_ble_scan_answer_t, rc) == offsetof(StatusRspEvt, rc),
               "the listen's answer carries its rc where every answer does");
_Static_assert(sizeof(((kode_sys_ble_scan_answer_t *)NULL)->rc) == sizeof(((StatusRspEvt *)NULL)->rc),
               "the listen's answer's rc is every answer's");

/** @brief Bytes of an address in kdrv_ble_dev_t. */
#define BLE_ADDR_BYTES  6U

_Static_assert(sizeof(((kdrv_ble_dev_t *)NULL)->addr) == BLE_ADDR_BYTES,
               "an address is six bytes");
_Static_assert(KODE_SYS_BLE_ADDR_MAX == ((BLE_ADDR_BYTES * 3U)),
               "an address prints as six pairs, five colons and a NUL");

/** @brief Characters of a device's name that fit kode_sys_ble_device_t::name, less its NUL. */
#define BLE_NAME_SHOWN  29

_Static_assert((size_t)BLE_NAME_SHOWN == (KODE_SYS_BLE_NAME_MAX - 1U),
               "a shown name fills the facade's field but for its NUL");

/* ---- The scan response: name and data ------------------------------------- */

/** @brief The scan response's two parts. */
typedef struct {
    char    name[KODE_SYS_BLE_NAME_MAX];  /**< "" for the serial.        */
    uint8_t len;                          /**< Bytes of @c data.         */
    uint8_t data[KQP_BLE_DATA_MAX];       /**< The maker data.           */
} rsp_t;

/** @brief What this file posted last; AO_Radio holds the same. Guarded by s_rsp_lock. */
static rsp_t s_rsp;

/** @brief Guards s_rsp across a check and its post; made on the first call. */
static khal_mutex_t s_rsp_lock;

/** @brief Make the lock. @return true when it exists. */
static bool rsp_lock_make(void)
{
    s_rsp_lock = khal_mutex_create();
    return NULL != s_rsp_lock;
}

/**
 * @brief Take the lock that keeps a check and its post together.
 * @return true when held; false when it could not be made.
 */
static bool rsp_enter(void)
{
    static khal_once_t made;  /* the one-shot the lock is made by */
    bool const         ok = khal_once_try(&made, &rsp_lock_make);

    if ( ok )
    {
        khal_mutex_lock(s_rsp_lock);
    }
    return ok;
}

/**
 * @brief Check a pair against the packet, post it whole, and keep it once posted.
 * @note Called with s_rsp_lock held.
 * @param next The pair.
 * @return As kode_sys_ble_set_name().
 */
static kode_status_t rsp_pair_post(const rsp_t *next)
{
    uint8_t       packet[KDRV_BLE_ADV_MAX];
    size_t        used = 0U;
    const char   *who  = next->name;
    kode_status_t rc;

    if ( '\0' == who[0] )
    {
        who = kdrv_ble_serial_name();
    }
    if ( kdrv_ble_adv_pack(who, next->data, next->len, packet, sizeof(packet), &used) != KODE_OK )
    {
        return KODE_DENIED;
    }
    rc = kode_sys_cmd_post(AO_Radio, ble_rsp_evt(next->name, next->data, next->len),
                           KQP_RADIO_CMD_MARGIN);
    if ( KODE_OK == rc )
    {
        s_rsp = *next;
    }
    return rc;
}

kode_status_t kode_sys_ble_set_name(const char *name)
{
    const char   *who = "";
    kode_status_t rc;

    if ( NULL != name )
    {
        who = name;
    }
    if ( memchr(who, (int)'\0', KODE_SYS_BLE_NAME_MAX) == NULL )
    {
        return KODE_DENIED;
    }
    if ( !rsp_enter() )
    {
        return KODE_NO_RESOURCES;
    }
    rsp_t next = s_rsp;

    (void)snprintf(next.name, sizeof(next.name), "%s", who);
    rc = rsp_pair_post(&next);
    khal_mutex_unlock(s_rsp_lock);
    return rc;
}

kode_status_t kode_sys_ble_set_data(const uint8_t *data, size_t len)
{
    kode_status_t rc;

    if ( ((size_t)KQP_BLE_DATA_MAX < len) || ((0U < len) && (NULL == data)) )
    {
        return KODE_DENIED;
    }
    if ( !rsp_enter() )
    {
        return KODE_NO_RESOURCES;
    }
    rsp_t next = s_rsp;

    next.len = (uint8_t)len;
    (void)memset(next.data, 0, sizeof(next.data));
    if ( 0U < len )
    {
        (void)memcpy(next.data, data, len);
    }
    rc = rsp_pair_post(&next);
    khal_mutex_unlock(s_rsp_lock);
    return rc;
}

bool kode_sys_ble_enabled(void)
{
    return ble_enabled();
}

/* ---- The app's own service ------------------------------------------------- */

_Static_assert(KODE_SYS_BLE_CHARS_MAX == KDRV_BLE_APP_CHARS_MAX, "the characteristics are the driver's");
_Static_assert(KODE_SYS_BLE_VALUE_MAX == KDRV_BLE_APP_VALUE_MAX, "a value is the driver's");
_Static_assert(KODE_SYS_BLE_CHAR_NAME_MAX == KDRV_BLE_APP_NAME_MAX, "a name is the driver's");
_Static_assert(KODE_SYS_BLE_FAULT_MAX == KDRV_BLE_APP_WHY_MAX, "the reason is the driver's");
_Static_assert(KODE_SYS_BLE_READ == KDRV_BLE_APP_READ, "READ is the driver's");
_Static_assert(KODE_SYS_BLE_WRITE == KDRV_BLE_APP_WRITE, "WRITE is the driver's");
_Static_assert(KODE_SYS_BLE_NOTIFY == KDRV_BLE_APP_NOTIFY, "NOTIFY is the driver's");
_Static_assert(KODE_SYS_BLE_QUEUED == KDRV_BLE_APP_QUEUED, "QUEUED is the driver's");
_Static_assert(KODE_SYS_BLE_QUEUE_SLOTS == KDRV_BLE_APP_QUEUE_SLOTS, "a queue is the driver's");
_Static_assert(KODE_SYS_BLE_VALUE_MAX == KQP_BLE_APP_VALUE_MAX, "a write's event carries a whole value");
_Static_assert(KODE_SYS_BLE_CHAR_NAME_MAX == KQP_BLE_APP_NAME_MAX, "a write's event carries a whole name");

/* The driver reads the constant KODE_SYS_BLE_SERVICE() defines through its own
 * mirror of the two types, so the layouts are held equal, field by field. */
_Static_assert(sizeof(kode_sys_ble_char_t) == sizeof(kdrv_ble_app_char_t), "a characteristic and the driver's");
_Static_assert(offsetof(kode_sys_ble_char_t, name) == offsetof(kdrv_ble_app_char_t, name), "name");
_Static_assert(offsetof(kode_sys_ble_char_t, uuid) == offsetof(kdrv_ble_app_char_t, uuid), "uuid");
_Static_assert(offsetof(kode_sys_ble_char_t, props) == offsetof(kdrv_ble_app_char_t, props), "props");
_Static_assert(sizeof(((kode_sys_ble_char_t *)NULL)->props) == sizeof(((kdrv_ble_app_char_t *)NULL)->props),
               "props is a byte in both");
_Static_assert(sizeof(kode_sys_ble_service_t) == sizeof(kdrv_ble_app_service_t), "a service and the driver's");
_Static_assert(offsetof(kode_sys_ble_service_t, uuid) == offsetof(kdrv_ble_app_service_t, uuid), "uuid");
_Static_assert(offsetof(kode_sys_ble_service_t, chars) == offsetof(kdrv_ble_app_service_t, chars), "chars");
_Static_assert(offsetof(kode_sys_ble_service_t, count) == offsetof(kdrv_ble_app_service_t, count), "count");

/**
 * @brief Check a request against the declared table and post it to AO_Radio as a command.
 * @param chr  The characteristic's name.
 * @param op   KQP_BLE_APP_OP_SET, _NOTIFY, _PUSH or _CLEAR.
 * @param data The bytes.
 * @param len  Their count.
 * @return As kode_sys_ble_set_value().
 */
static kode_status_t value_command(const char *chr, uint8_t op, const void *data, size_t len)
{
    kode_status_t rc = kdrv_ble_app_check(chr, op, data, len);

    if ( KODE_OK == rc )
    {
        rc = kode_sys_cmd_post(AO_Radio, ble_value_evt(chr, op, data, len, NULL, 0U),
                               KQP_RADIO_CMD_MARGIN);
    }
    return rc;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_set_value(const char *chr, const void *data, size_t len)
{
    return value_command(chr, KQP_BLE_APP_OP_SET, data, len);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_notify(const char *chr, const void *data, size_t len)
{
    return value_command(chr, KQP_BLE_APP_OP_NOTIFY, data, len);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_queue_push(const char *chr, const void *data, size_t len)
{
    return value_command(chr, KQP_BLE_APP_OP_PUSH, data, len);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_queue_clear(const char *chr)
{
    return value_command(chr, KQP_BLE_APP_OP_CLEAR, NULL, 0U);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_queue_room(const char *chr, size_t *room)
{
    return kdrv_ble_app_queue_room(chr, room);
}

/* ---- The blocking value: one call at a time, as the other blocking doors ---- */
static khal_mutex_t   s_value_lock;   /**< Guards s_value_busy and the generation.            */
static bool           s_value_busy;   /**< A call holds the claim.                            */
static BleValueWaiter s_value_wait;   /**< Where AO_Radio answers; @c done is the door's one
                                       *   semaphore.                                         */

/**
 * @brief Make the door's semaphore and lock; kode_block_ble_value()'s one-shot.
 * @return true when both exist.
 */
static bool value_make(void)
{
    s_value_wait.done = khal_sem_create();
    s_value_lock      = khal_mutex_create();
    return (NULL != s_value_wait.done) && (NULL != s_value_lock);
}

/**
 * @brief Take the claim, atomically against the other core, and draw its generation.
 * @param[out] gen This request's generation when the claim was taken; never zero.
 * @return true when this caller took it, and must release it again.
 */
static bool value_claim(uint32_t *gen)
{
    bool took = false;

    khal_mutex_lock(s_value_lock);
    if ( !s_value_busy )
    {
        static uint32_t last;   /* the last generation drawn; tags requests apart, never 0 */

        s_value_busy  = true;
        took          = true;
        last         += 1U;
        if ( 0U == last )
        {
            last = 1U;
        }
        *gen = last;
    }
    khal_mutex_unlock(s_value_lock);
    return took;
}

/** @brief Give back the claim value_claim() took. */
static void value_release(void)
{
    khal_mutex_lock(s_value_lock);
    s_value_busy = false;
    khal_mutex_unlock(s_value_lock);
}

/**
 * @brief Whether the call that began at @p start still has time.
 * @param start When the call began, kode_sys_time_ms().
 * @return true while less than KODE_SYS_BLE_VALUE_WAIT_MS has gone by.
 */
static bool value_time_left(uint32_t start)
{
    return (kode_sys_time_ms() - start) < KODE_SYS_BLE_VALUE_WAIT_MS;
}

/**
 * @brief Take the claim, waiting out another task's call while there is time.
 * @param start When the call began.
 * @param[out] gen As value_claim().
 * @return true when the claim is held.
 */
static bool value_enter(uint32_t start, uint32_t *gen)
{
    bool took = value_claim(gen);

    while ( !took && value_time_left(start) )
    {
        kode_block_delay_ms(KODE_SYS_BLE_VALUE_RETRY_MS);
        took = value_claim(gen);
    }
    return took;
}

/**
 * @brief Post one blocking value tagged @p gen, trying a full queue again
 *        while there is time.
 * @param chr    As kode_block_ble_value().
 * @param op     As kode_block_ble_value().
 * @param data   As kode_block_ble_value().
 * @param len    As kode_block_ble_value().
 * @param gen    This request's generation.
 * @param start  When the call began.
 * @return true when AO_Radio's queue took it.
 */
static bool value_post(const char *chr, uint8_t op, const void *data, size_t len,
                       uint32_t gen, uint32_t start)
{
    bool posted = false;
    bool more   = true;

    while ( more )
    {
        posted = (kode_sys_cmd_post(AO_Radio,
                                    ble_value_evt(chr, op, data, len, &s_value_wait, gen),
                                    KQP_RADIO_CMD_MARGIN) == KODE_OK);
        more = !posted && value_time_left(start);
        if ( more )
        {
            kode_block_delay_ms(KODE_SYS_BLE_VALUE_RETRY_MS);
        }
    }
    return posted;
}

/**
 * @brief Wait for AO_Radio's answer tagged @p gen, discarding a late one.
 * @param gen   This request's generation.
 * @param start When the call began.
 * @return The answer's status; KODE_TIMEOUT when it did not come in time.
 */
static kode_status_t value_await(uint32_t gen, uint32_t start)
{
    uint32_t const spent = kode_sys_time_ms() - start;
    uint32_t       left  = 0U;
    kode_status_t  rc    = KODE_TIMEOUT;

    if ( KODE_SYS_BLE_VALUE_WAIT_MS > spent )
    {
        left = KODE_SYS_BLE_VALUE_WAIT_MS - spent;
    }
    if ( kode_sys_answer_await(s_value_wait.done, &s_value_wait.ack, gen, left) )
    {
        rc = (kode_status_t)s_value_wait.rc;
    }
    return rc;
}

kode_status_t kode_block_ble_value(const char *chr, uint8_t op, const void *data, size_t len)
{
    static khal_once_t made;   /* the one-shot the semaphore and the lock are made by */
    uint32_t const     start = kode_sys_time_ms();
    kode_status_t      rc    = kdrv_ble_app_check(chr, op, data, len);
    uint32_t           gen   = 0U;

    if ( (KODE_OK == rc) && !khal_once_try(&made, &value_make) )
    {
        rc = KODE_ERROR;
    }
    if ( KODE_OK == rc )
    {
        rc = KODE_TIMEOUT;
        if ( value_enter(start, &gen) )
        {
            /* A request a timed-out call left is answered first, AO_Radio
             * taking its queue in order, and dropped by its generation. */
            if ( value_post(chr, op, data, len, gen, start) )
            {
                rc = value_await(gen, start);
            }
            value_release();
        }
    }
    return rc;
}

/* cppcheck-suppress [misra-c2012-8.7,constParameterPointer] ; a facade call the apps outside this tree call; @p me is the asking AO, non-const as every request takes it */
kode_status_t kode_sys_ble_subscribe(QActive *me)
{
    kode_status_t rc = KODE_DENIED;

    if ( kaos_asker_started(me) )
    {
        ble_app_subscribe(me);
        rc = KODE_OK;
    }
    return rc;
}

bool kode_sys_ble_service_fault(char *why, size_t cap)
{
    return kdrv_ble_app_fault(kdrv_ble_app_service(), why, cap);
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_service_select(const kode_sys_ble_service_t *svc)
{
    kode_status_t rc = KODE_DENIED;

    /* The layouts are held equal above, field by field; the driver reads
     * the app's constant through its own mirror of the type, as it reads
     * KODE_SYS_BLE_SERVICE()'s. */
    /* cppcheck-suppress misra-c2012-11.3 ; layout-compatible types, asserted above */
    if ( kdrv_ble_app_select((const kdrv_ble_app_service_t *)svc) )
    {
        rc = KODE_OK;
    }
    return rc;
}

/* ---- The listen ------------------------------------------------------------ */

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
kode_status_t kode_sys_ble_scan(QActive *me, uint32_t ms)
{
    return ble_scan_ask(me, ms);
}

/** @brief The blocking listen's claim and storage (kode_sys_priv.h says how). */
static kode_sys_scan_t s_ble_scan = { .row_size = sizeof(kdrv_ble_dev_t),
                                      .cap      = (uint8_t)KODE_SYS_BLE_SCAN_MAX };

/** @brief Make the semaphore and the lock. @return true when both exist. */
static bool ble_scan_make(void)
{
    return kode_sys_scan_make(&s_ble_scan);
}

/**
 * @brief Offer the devices AO_Radio wrote, converted, until @p cb says stop.
 * @param cb  The caller's callback.
 * @param ctx Its context.
 */
static void ble_scan_offer(kode_sys_ble_device_cb cb, void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the rows are the kdrv_ble_dev_t[] kode_sys_scan_enter() took for this listen */
    const kdrv_ble_dev_t *const rows = (const kdrv_ble_dev_t *)s_ble_scan.wait.rows;
    uint8_t                     n    = s_ble_scan.wait.got;

    if ( (uint8_t)KODE_SYS_BLE_SCAN_MAX < n )
    {
        n = (uint8_t)KODE_SYS_BLE_SCAN_MAX;
    }
    for ( uint8_t i = 0U; i < n; ++i )
    {
        kode_sys_ble_device_t dev;
        const uint8_t *const  a = rows[i].addr;

        /* An advert's name is 29 bytes at most (31 less its 2-byte header), so the
         * precision never cuts a name a device can send. */
        (void)snprintf(dev.name, sizeof(dev.name), "%.*s", BLE_NAME_SHOWN, rows[i].name);
        (void)snprintf(dev.address, sizeof(dev.address), "%02x:%02x:%02x:%02x:%02x:%02x",
                       a[0], a[1], a[2], a[3], a[4], a[5]);
        dev.rssi = rows[i].rssi;
        if ( !cb(&dev, ctx) )
        {
            break;
        }
    }
}

kode_status_t kode_block_ble_scan(kode_sys_ble_device_cb cb, void *ctx, uint32_t ms,
                                  uint32_t wait_ms)
{
    if ( NULL == cb )
    {
        return KODE_ERROR;
    }
    if ( (KODE_SYS_BLE_SCAN_MS_MIN > ms) || (KODE_SYS_BLE_SCAN_MS_MAX < ms) )
    {
        return KODE_DENIED;
    }
    kode_status_t rc = kode_sys_scan_enter(&s_ble_scan, &ble_scan_make, wait_ms);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    uint32_t const gen = kode_sys_scan_gen(&s_ble_scan);

    ble_scan_request_wait(&s_ble_scan.wait, gen, ms);
    rc = kode_sys_scan_await(&s_ble_scan, gen, wait_ms);
    if ( KODE_OK == rc )
    {
        ble_scan_offer(cb, ctx);
    }
    kode_sys_scan_release(&s_ble_scan);
    return rc;
}

#else  /* !CONFIG_KODEOS_BLE - the surface still links, and refuses */

kode_status_t kode_sys_ble_set_name(const char *name)
{
    (void)name;
    return KODE_DENIED;
}

kode_status_t kode_sys_ble_set_data(const uint8_t *data, size_t len)
{
    (void)data; (void)len;
    return KODE_DENIED;
}

bool kode_sys_ble_enabled(void)
{
    return false;
}

kode_status_t kode_sys_ble_scan(QActive *me, uint32_t ms)
{
    (void)me; (void)ms;
    return KODE_DENIED;
}

kode_status_t kode_block_ble_scan(kode_sys_ble_device_cb cb, void *ctx, uint32_t ms,
                                  uint32_t wait_ms)
{
    (void)cb; (void)ctx; (void)ms; (void)wait_ms;
    return KODE_DENIED;
}

kode_status_t kode_sys_ble_set_value(const char *chr, const void *data, size_t len)
{
    (void)chr; (void)data; (void)len;
    return KODE_DENIED;
}

kode_status_t kode_sys_ble_notify(const char *chr, const void *data, size_t len)
{
    (void)chr; (void)data; (void)len;
    return KODE_DENIED;
}

kode_status_t kode_sys_ble_queue_push(const char *chr, const void *data, size_t len)
{
    (void)chr; (void)data; (void)len;
    return KODE_DENIED;
}

kode_status_t kode_sys_ble_queue_clear(const char *chr)
{
    (void)chr;
    return KODE_DENIED;
}

kode_status_t kode_sys_ble_queue_room(const char *chr, size_t *room)
{
    (void)chr; (void)room;
    return KODE_DENIED;
}

kode_status_t kode_block_ble_value(const char *chr, uint8_t op, const void *data, size_t len)
{
    (void)chr; (void)op; (void)data; (void)len;
    return KODE_DENIED;
}

kode_status_t kode_sys_ble_subscribe(QActive *me)
{
    (void)me;
    return KODE_DENIED;
}

bool kode_sys_ble_service_fault(char *why, size_t cap)
{
    (void)why; (void)cap;
    return false;
}

#endif /* CONFIG_KODEOS_BLE */
