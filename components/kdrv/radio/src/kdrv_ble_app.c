/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_app.c
 * @brief The app's GATT service without NimBLE: table rules, UUIDs, values, subscriptions, ring.
 */
#include "kdrv_ble_app.h"

#include <stdatomic.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "khal_once.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_log.h"
#include "sdkconfig.h"

#define TAG "kdrv.ble"

/** @brief Characters of a UUID as text: 32 hex digits and 4 hyphens. */
#define UUID_TEXT_LEN  36U

/** @brief Where the hyphens of a UUID's text are. */
#define UUID_DASH_1    8U
#define UUID_DASH_2    13U   /**< The second hyphen. */
#define UUID_DASH_3    18U   /**< The third hyphen.  */
#define UUID_DASH_4    23U   /**< The fourth hyphen. */

/** @brief Bits of one hex digit. */
#define HEX_DIGIT_BITS 4U

/** @brief The value of the hex digits 'a' to 'f' past their letter. */
#define HEX_LETTER_BASE 10U

/** @brief Every property a characteristic may carry. */
#define APP_PROPS_ALL  (KDRV_BLE_APP_READ | KDRV_BLE_APP_WRITE | KDRV_BLE_APP_NOTIFY | KDRV_BLE_APP_QUEUED)

/** @brief The properties a central reads by. */
#define APP_PROPS_READ (KDRV_BLE_APP_READ | KDRV_BLE_APP_QUEUED)

_Static_assert(KDRV_BLE_APP_CHARS_MAX == 4U, "the reasons say at most 4 characteristics");
_Static_assert(KDRV_BLE_APP_NAME_MAX == 16U, "the reasons say names of 1 to 15 bytes");
_Static_assert(KDRV_BLE_APP_VALUE_MAX <= (size_t)UINT16_MAX, "a value's length fits its counter");
_Static_assert(KDRV_BLE_APP_QUEUE_SLOTS <= (size_t)UINT8_MAX, "what is left in a queue fits a byte");

/** @brief One queued characteristic's values, a PSRAM block of its own. */
typedef struct {
    uint32_t head;                                                  /**< The oldest value.  */
    uint32_t count;                                                 /**< Values waiting.    */
    uint16_t len[KDRV_BLE_APP_QUEUE_SLOTS];                         /**< Bytes of each.     */
    uint8_t  data[KDRV_BLE_APP_QUEUE_SLOTS][KDRV_BLE_APP_VALUE_MAX]; /**< The values.        */
} app_queue_t;

/** @brief One notification waiting for the host task. */
typedef struct {
    uint8_t  chr;                            /**< Its characteristic.  */
    uint16_t len;                            /**< Bytes of @c data.    */
    uint8_t  data[KDRV_BLE_APP_VALUE_MAX];   /**< The bytes.           */
} app_note_t;

/** @brief Everything the service changes at run time, one block in PSRAM. */
typedef struct {
    khal_mutex_t lock;                                             /**< Guards all below but @c scratch. */
    uint16_t     vlen[KDRV_BLE_APP_CHARS_MAX];                     /**< Bytes of each value.            */
    uint8_t      value[KDRV_BLE_APP_CHARS_MAX][KDRV_BLE_APP_VALUE_MAX]; /**< What a read gets.         */
    bool         sub[KDRV_BLE_APP_CHARS_MAX];                      /**< A central is subscribed.        */
    uint32_t     head;                                             /**< The oldest notification.        */
    uint32_t     count;                                            /**< Notifications waiting.          */
    app_note_t   ring[KDRV_BLE_APP_RING_SLOTS];                    /**< The ring.                       */
    app_queue_t *queue[KDRV_BLE_APP_CHARS_MAX];                    /**< Each queued one's; NULL until taken. */
    uint8_t      scratch[KDRV_BLE_APP_VALUE_MAX];                  /**< The host task's alone.          */
} app_mem_t;

/** @brief The alignment the block and kdrv_ble.c's part of it get: 16, any
 *         scalar's on the P4 and on the host, so NimBLE's structures too. */
#define APP_ALIGN        16U

/** @brief Where kdrv_ble.c's part starts in the block: past the values, aligned. */
#define APP_HOST_OFFSET  ((((sizeof(app_mem_t) + APP_ALIGN) - 1U) / APP_ALIGN) * APP_ALIGN)

/** @brief The block, once taken; NULL before. */
static app_mem_t *s_mem;

/** @brief What a write is handed to; NULL takes every write. */
static kdrv_ble_app_write_cb_t s_write_cb;

/** @brief What a take from a queue is told to; NULL tells nobody. */
static kdrv_ble_app_taken_cb_t s_taken_cb;

/* ---- The app's choice of service -------------------------------------------
 *
 * One pointer, s_pick, shared by the app's task that chooses and the radio's
 * worker that builds the table, with no lock: each side changes it only by a
 * compare-and-swap, so a choice either lands before the build reads it (and
 * the table is built from it) or finds the build's mark and is refused. Two
 * marks stand in for the two answers that are not a service the app gave:
 * the declared one, still unchosen, and the table already built. */

/** @brief s_pick's mark for "nothing chosen": the declared service serves. */
static const kdrv_ble_app_service_t s_mark_declared = { NULL, NULL, 0U };

/** @brief s_pick's mark for "the table is built": s_served is what it holds. */
static const kdrv_ble_app_service_t s_mark_built = { NULL, NULL, 0U };

/** @brief The choice: a service, NULL for none, or one of the two marks. */
static _Atomic(const kdrv_ble_app_service_t *) s_pick = &s_mark_declared;

/** @brief The service the table was built from; written before s_pick takes s_mark_built. */
static _Atomic(const kdrv_ble_app_service_t *) s_served;

/** @brief The refusal has been logged once. */
static atomic_bool s_refusal_said;

/**
 * @brief What a choice means: the declared service for s_mark_declared.
 * @param pick A choice, never s_mark_built.
 * @return The service; NULL for none.
 */
static const kdrv_ble_app_service_t *pick_service(const kdrv_ble_app_service_t *pick)
{
    const kdrv_ble_app_service_t *svc = pick;

    if ( &s_mark_declared == pick )
    {
        svc = kdrv_ble_app_declared();
    }
    return svc;
}

const kdrv_ble_app_service_t *kdrv_ble_app_service(void)
{
    const kdrv_ble_app_service_t *pick = atomic_load(&s_pick);

    if ( &s_mark_built == pick )
    {
        pick = atomic_load(&s_served);
    }
    return pick_service(pick);
}

bool kdrv_ble_app_select(const kdrv_ble_app_service_t *svc)
{
    const kdrv_ble_app_service_t *was  = atomic_load(&s_pick);
    bool                          took = false;

    while ( (&s_mark_built != was) && !took )
    {
        took = atomic_compare_exchange_weak(&s_pick, &was, svc);
    }
    if ( !took && !atomic_exchange(&s_refusal_said, true) )
    {
        KLOG_W(TAG, "app service: the GATT table is registered already; the choice "
                    "takes effect at the next boot that chooses before Bluetooth starts");
    }
    return took;
}

/* cppcheck-suppress misra-c2012-8.7 ; kdrv_ble.c builds the GATT table from it */
const kdrv_ble_app_service_t *kdrv_ble_app_fix(void)
{
    const kdrv_ble_app_service_t *was = atomic_load(&s_pick);

    while ( &s_mark_built != was )
    {
        /* s_served first: a reader that sees the mark finds it filled. A
         * swap that fails reloads @c was with the choice that beat it. */
        atomic_store(&s_served, pick_service(was));
        if ( atomic_compare_exchange_weak(&s_pick, &was, &s_mark_built) )
        {
            was = &s_mark_built;
        }
    }
    return atomic_load(&s_served);
}

/* cppcheck-suppress misra-c2012-8.7 ; the host suite starts each case from no choice with it */
void kdrv_ble_app_select_forget(void)
{
    atomic_store(&s_served, NULL);
    atomic_store(&s_pick, &s_mark_declared);
    atomic_store(&s_refusal_said, false);
}

/** @brief Take the block and its lock. @return true when both exist. */
static bool app_mem_make(void)
{
    bool         ok    = false;
    size_t const total = APP_HOST_OFFSET + kdrv_ble_app_host_bytes();
    /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned as asked */
    app_mem_t *const m = (app_mem_t *)khal_alloc_psram(APP_ALIGN, total);

    if ( NULL != m )
    {
        (void)memset(m, 0, total);
        m->lock = khal_mutex_create();
        if ( NULL != m->lock )
        {
            s_mem = m;
            ok    = true;
        }
        else
        {
            khal_free(m);
        }
    }
    return ok;
}

/** @brief Take the block the first time it is needed. @return true when it exists. */
static bool app_mem_ensure(void)
{
    static khal_once_t once;   /* the one-shot the block is taken by */

    return khal_once_try(&once, &app_mem_make);
}

/**
 * @brief Bytes of @p text before its NUL, reading at most @p cap of them.
 * @param text The text.
 * @param cap  The most bytes read.
 * @return Its length; @p cap when no NUL came first.
 */
static size_t text_len(const char *text, size_t cap)
{
    size_t n = 0U;

    while ( (n < cap) && ('\0' != text[n]) )
    {
        n++;
    }
    return n;
}

/**
 * @brief The value of one hex digit.
 * @param c   The character.
 * @param out Receives its value.
 * @return false when @p c is not a hex digit.
 */
static bool hex_digit(char c, uint8_t *out)
{
    bool ok = true;

    if ( ('0' <= c) && ('9' >= c) )
    {
        *out = (uint8_t)c - (uint8_t)'0';
    }
    else if ( ('a' <= c) && ('f' >= c) )
    {
        *out = (uint8_t)(((uint8_t)c - (uint8_t)'a') + HEX_LETTER_BASE);
    }
    else if ( ('A' <= c) && ('F' >= c) )
    {
        *out = (uint8_t)(((uint8_t)c - (uint8_t)'A') + HEX_LETTER_BASE);
    }
    else
    {
        ok = false;
    }
    return ok;
}

/**
 * @brief Parse a 128-bit UUID's text into wire order, least significant byte first.
 * @param text "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx", either case; NULL fails.
 * @param out  Receives the 16 bytes; may be NULL to check only.
 * @return false when @p text is not one.
 */
static bool uuid_parse(const char *text, uint8_t *out)
{
    uint8_t bytes[KDRV_BLE_UUID128_BYTES];
    size_t  pos = 0U;
    bool    ok  = (NULL != text) && (text_len(text, UUID_TEXT_LEN + 1U) == UUID_TEXT_LEN);

    for ( size_t n = 0U; ok && (KDRV_BLE_UUID128_BYTES > n); n++ )
    {
        uint8_t hi = 0U;
        uint8_t lo = 0U;

        if ( (UUID_DASH_1 == pos) || (UUID_DASH_2 == pos) || (UUID_DASH_3 == pos) ||
            (UUID_DASH_4 == pos) )
        {
            ok = ('-' == text[pos]);
            pos++;
        }
        ok = ok && hex_digit(text[pos], &hi) && hex_digit(text[pos + 1U], &lo);
        pos += 2U;   /* two digits a byte */
        if ( ok )
        {
            bytes[n] = (uint8_t)((uint8_t)(hi << HEX_DIGIT_BITS) | lo);
        }
    }
    if ( ok && (NULL != out) )
    {
        for ( size_t k = 0U; KDRV_BLE_UUID128_BYTES > k; k++ )
        {
            out[k] = bytes[KDRV_BLE_UUID128_BYTES - 1U - k];
        }
    }
    return ok;
}

/**
 * @brief Whether @p name is 1 to KDRV_BLE_APP_NAME_MAX - 1 bytes.
 * @param name The name; NULL is not one.
 * @return true when it is.
 */
static bool app_name_ok(const char *name)
{
    bool ok = false;

    if ( NULL != name )
    {
        size_t const len = text_len(name, KDRV_BLE_APP_NAME_MAX);

        ok = (0U < len) && (KDRV_BLE_APP_NAME_MAX > len);
    }
    return ok;
}

/**
 * @brief Keep a reason that carries nothing, when the caller wants one.
 * @param why  Where; NULL for nowhere.
 * @param cap  Bytes @p why holds.
 * @param text The reason.
 */
static void why_put(char *why, size_t cap, const char *text)
{
    if ( (NULL != why) && (0U < cap) )
    {
        (void)snprintf(why, cap, "%s", text);
    }
}

/**
 * @brief Keep a reason about the characteristic @p name, when the caller wants one.
 * @param why  Where; NULL for nowhere.
 * @param cap  Bytes @p why holds.
 * @param name The characteristic's name.
 * @param text The reason, after the name.
 */
static void why_name(char *why, size_t cap, const char *name, const char *text)
{
    if ( (NULL != why) && (0U < cap) )
    {
        (void)snprintf(why, cap, "'%s'%s", name, text);
    }
}

/**
 * @brief Keep a reason that carries a number, when the caller wants one.
 * @param why  Where; NULL for nowhere.
 * @param cap  Bytes @p why holds.
 * @param head The reason, before the number.
 * @param n    The number.
 * @param tail The reason, after the number.
 */
static void why_num(char *why, size_t cap, const char *head, unsigned n, const char *tail)
{
    if ( (NULL != why) && (0U < cap) )
    {
        (void)snprintf(why, cap, "%s%u%s", head, n, tail);
    }
}

/**
 * @brief Whether this build lacks the console's service, whose pairing the app's needs.
 * @param why As kdrv_ble_app_fault().
 * @param cap As kdrv_ble_app_fault().
 * @return true without CONFIG_KODEOS_BLE_CONSOLE.
 */
static bool console_fault(char *why, size_t cap)
{
#if CONFIG_KODEOS_BLE_CONSOLE
    (void)why;
    (void)cap;
    return false;
#else
    why_put(why, cap, "this firmware has no Bluetooth console, whose pairing it needs");
    return true;
#endif
}

/**
 * @brief Judge one characteristic.
 * @param svc The service.
 * @param i   Its place in the table.
 * @param why As kdrv_ble_app_fault().
 * @param cap As kdrv_ble_app_fault().
 * @return true when it cannot be served.
 */
static bool char_fault(const kdrv_ble_app_service_t *svc, size_t i, char *why, size_t cap)
{
    const kdrv_ble_app_char_t *const c     = &svc->chars[i];
    bool                             fault = true;

    if ( !app_name_ok(c->name) )
    {
        why_num(why, cap, "the characteristic at ", (unsigned)i,
                " has no name of 1 to 15 bytes");
    }
    else if ( !uuid_parse(c->uuid, NULL) )
    {
        why_name(why, cap, c->name, ": the UUID is not 128 bits as 8-4-4-4-12 hex");
    }
    else if ( (0U == c->props) || ((c->props & (uint8_t)~APP_PROPS_ALL) != 0U) )
    {
        why_name(why, cap, c->name, ": the properties are not READ, WRITE, NOTIFY or QUEUED");
    }
    else
    {
        fault = false;
        for ( size_t k = 0U; (k < i) && !fault; k++ )
        {
            if ( strcmp(svc->chars[k].name, c->name) == 0 )
            {
                why_name(why, cap, c->name, " names two characteristics");
                fault = true;
            }
        }
    }
    return fault;
}

bool kdrv_ble_app_fault(const kdrv_ble_app_service_t *svc, char *why, size_t cap)
{
    bool fault = true;

    if ( NULL == svc )
    {
        fault = false;
    }
    /* cppcheck-suppress knownConditionTrueFalse ; constant in each build: whether it has the Bluetooth console */
    else if ( console_fault(why, cap) )
    {
        /* the reason is kept */
    }
    else if ( !uuid_parse(svc->uuid, NULL) )
    {
        why_put(why, cap, "the service's UUID is not 128 bits as 8-4-4-4-12 hex");
    }
    else if ( (NULL == svc->chars) || (0U == svc->count) )
    {
        why_put(why, cap, "the service has no characteristics");
    }
    else if ( KDRV_BLE_APP_CHARS_MAX < svc->count )
    {
        why_num(why, cap, "the service has ", (unsigned)svc->count,
                " characteristics; at most 4");
    }
    else
    {
        fault = false;
        for ( size_t i = 0U; (i < svc->count) && !fault; i++ )
        {
            fault = char_fault(svc, i, why, cap);
        }
    }
    return fault;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
bool kdrv_ble_app_build(const kdrv_ble_app_service_t *svc, const kdrv_ble_app_perm_t *perm,
                        kdrv_ble_app_table_t *out)
{
    char why[KDRV_BLE_APP_WHY_MAX];
    bool served = false;

    (void)memset(out, 0, sizeof(*out));
    if ( NULL == svc )
    {
        /* the app declares none: the table is the console's alone */
    }
    else if ( kdrv_ble_app_fault(svc, why, sizeof(why)) )
    {
        KLOG_W(TAG, "the app's service is left out: %s", why);
    }
    else if ( !app_mem_ensure() )
    {
        KLOG_W(TAG, "the app's service is left out: no PSRAM for its values");
    }
    else
    {
        (void)uuid_parse(svc->uuid, out->svc_uuid);
        for ( size_t i = 0U; i < svc->count; i++ )
        {
            uint8_t const props = svc->chars[i].props;
            uint32_t      flags = 0U;

            (void)uuid_parse(svc->chars[i].uuid, out->uuid[i]);
            if ( (props & APP_PROPS_READ) != 0U )
            {
                flags |= perm->read;
            }
            if ( (props & KDRV_BLE_APP_WRITE) != 0U )
            {
                flags |= perm->write;
            }
            if ( (props & KDRV_BLE_APP_NOTIFY) != 0U )
            {
                flags |= perm->notify;
            }
            out->flags[i] = flags;
        }
        out->count = (uint8_t)svc->count;
        served     = true;
    }
    return served;
}

/**
 * @brief Check a set or a notify against the declared constant and find its characteristic.
 * @param chr   The characteristic's name.
 * @param data  The bytes.
 * @param len   Their count.
 * @param idx   Receives its place in the table.
 * @param props Receives its properties.
 * @return KODE_OK; otherwise KODE_DENIED, as kdrv_ble_app_check().
 */
static kode_status_t app_find(const char *chr, const void *data, size_t len, uint8_t *idx,
                              uint8_t *props)
{
    const kdrv_ble_app_service_t *const svc   = kdrv_ble_app_service();
    kode_status_t                       rc    = KODE_DENIED;
    bool                                found = false;

    if ( (NULL != svc) && (NULL != chr) && !kdrv_ble_app_fault(svc, NULL, 0U) )
    {
        for ( size_t i = 0U; (i < svc->count) && !found; i++ )
        {
            if ( strcmp(svc->chars[i].name, chr) == 0 )
            {
                *idx   = (uint8_t)i;
                *props = svc->chars[i].props;
                found  = true;
            }
        }
    }
    if ( found && (KDRV_BLE_APP_VALUE_MAX >= len) && ((0U == len) || (NULL != data)) )
    {
        rc = KODE_OK;
    }
    return rc;
}

/**
 * @brief app_find(), then the block the values live in.
 * @param chr   As app_find().
 * @param data  As app_find().
 * @param len   As app_find().
 * @param idx   As app_find().
 * @param props As app_find().
 * @return KODE_OK; otherwise as kdrv_ble_app_set_value().
 */
static kode_status_t app_enter(const char *chr, const void *data, size_t len, uint8_t *idx,
                               uint8_t *props)
{
    kode_status_t rc = app_find(chr, data, len, idx, props);

    if ( (KODE_OK == rc) && !app_mem_ensure() )
    {
        rc = KODE_ERROR;
    }
    return rc;
}

/**
 * @brief Whether characteristic properties @p props and @p len suit the call @p op names.
 * @param op    KDRV_BLE_APP_OP_SET, _NOTIFY, _PUSH or _CLEAR.
 * @param props The characteristic's properties.
 * @param len   The bytes' count.
 * @return true when they do.
 */
static bool op_fits(uint8_t op, uint8_t props, size_t len)
{
    bool fits = false;

    switch ( op )
    {
    case KDRV_BLE_APP_OP_SET:
        fits = true;
        break;
    case KDRV_BLE_APP_OP_NOTIFY:
        fits = (props & KDRV_BLE_APP_NOTIFY) != 0U;
        break;
    case KDRV_BLE_APP_OP_PUSH:
        fits = ((props & KDRV_BLE_APP_QUEUED) != 0U) && (0U < len);
        break;
    case KDRV_BLE_APP_OP_CLEAR:
        fits = ((props & KDRV_BLE_APP_QUEUED) != 0U) && (0U == len);
        break;
    default:
        /* no such call */
        break;
    }
    return fits;
}

kode_status_t kdrv_ble_app_check(const char *chr, uint8_t op, const void *data, size_t len)
{
    uint8_t       i     = 0U;
    uint8_t       props = 0U;
    kode_status_t rc    = app_find(chr, data, len, &i, &props);

    if ( (KODE_OK == rc) && !op_fits(op, props, len) )
    {
        rc = KODE_DENIED;
    }
    return rc;
}

/**
 * @brief Keep @p len bytes as characteristic @p i's value.
 * @note Called with the lock held.
 * @param i    Its place in the table.
 * @param data The bytes; NULL when @p len is 0.
 * @param len  Their count, at most KDRV_BLE_APP_VALUE_MAX.
 */
static void value_put(uint8_t i, const void *data, size_t len)
{
    if ( 0U < len )
    {
        (void)memcpy(s_mem->value[i], data, len);
    }
    s_mem->vlen[i] = (uint16_t)len;
}

kode_status_t kdrv_ble_app_set_value(const char *chr, const void *data, size_t len)
{
    uint8_t       i     = 0U;
    uint8_t       props = 0U;
    kode_status_t rc    = app_enter(chr, data, len, &i, &props);

    if ( KODE_OK == rc )
    {
        khal_mutex_lock(s_mem->lock);
        value_put(i, data, len);
        khal_mutex_unlock(s_mem->lock);
    }
    return rc;
}

kode_status_t kdrv_ble_app_notify(const char *chr, const void *data, size_t len)
{
    uint8_t       i     = 0U;
    uint8_t       props = 0U;
    kode_status_t rc    = app_enter(chr, data, len, &i, &props);

    if ( (KODE_OK == rc) && ((props & KDRV_BLE_APP_NOTIFY) == 0U) )
    {
        rc = KODE_DENIED;
    }
    if ( KODE_OK == rc )
    {
        khal_mutex_lock(s_mem->lock);
        value_put(i, data, len);
        if ( !s_mem->sub[i] )
        {
            /* nobody to tell: the value waits for a read */
        }
        else if ( KDRV_BLE_APP_RING_SLOTS <= s_mem->count )
        {
            rc = KODE_NO_RESOURCES;
        }
        else
        {
            app_note_t *const n =
                &s_mem->ring[(s_mem->head + s_mem->count) % KDRV_BLE_APP_RING_SLOTS];

            n->chr = i;
            n->len = (uint16_t)len;
            if ( 0U < len )
            {
                (void)memcpy(n->data, data, len);
            }
            s_mem->count++;
        }
        khal_mutex_unlock(s_mem->lock);
    }
    return rc;
}

kode_status_t kdrv_ble_app_set_write_cb(kdrv_ble_app_write_cb_t cb)
{
    kode_status_t rc = KODE_DENIED;

    if ( NULL != cb )
    {
        s_write_cb = cb;
        rc         = KODE_OK;
    }
    return rc;
}

kode_status_t kdrv_ble_app_set_taken_cb(kdrv_ble_app_taken_cb_t cb)
{
    kode_status_t rc = KODE_DENIED;

    if ( NULL != cb )
    {
        s_taken_cb = cb;
        rc         = KODE_OK;
    }
    return rc;
}

/**
 * @brief Characteristic @p i's queue, taken from PSRAM the first time when @p make.
 * @note Called with the lock held.
 * @param i    Its place in the table.
 * @param make true to take it when it does not exist yet.
 * @return It; NULL when it does not exist, or PSRAM refused it.
 */
static app_queue_t *queue_of(uint8_t i, bool make)
{
    app_queue_t *q = s_mem->queue[i];

    if ( (NULL == q) && make )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned as asked */
        q = (app_queue_t *)khal_alloc_psram(APP_ALIGN, sizeof(app_queue_t));
        if ( NULL != q )
        {
            (void)memset(q, 0, sizeof(*q));
            s_mem->queue[i] = q;
        }
    }
    return q;
}

kode_status_t kdrv_ble_app_queue_push(const char *chr, const void *data, size_t len)
{
    uint8_t       i     = 0U;
    uint8_t       props = 0U;
    kode_status_t rc    = app_enter(chr, data, len, &i, &props);

    if ( (KODE_OK == rc) && !op_fits(KDRV_BLE_APP_OP_PUSH, props, len) )
    {
        rc = KODE_DENIED;
    }
    if ( KODE_OK == rc )
    {
        khal_mutex_lock(s_mem->lock);
        app_queue_t *const q = queue_of(i, true);

        if ( NULL == q )
        {
            rc = KODE_ERROR;
        }
        else if ( KDRV_BLE_APP_QUEUE_SLOTS <= q->count )
        {
            rc = KODE_NO_RESOURCES;
        }
        else
        {
            uint32_t const at = (q->head + q->count) % KDRV_BLE_APP_QUEUE_SLOTS;

            (void)memcpy(q->data[at], data, len);
            q->len[at] = (uint16_t)len;
            q->count++;
        }
        khal_mutex_unlock(s_mem->lock);
    }
    return rc;
}

kode_status_t kdrv_ble_app_queue_clear(const char *chr)
{
    uint8_t       i     = 0U;
    uint8_t       props = 0U;
    kode_status_t rc    = app_find(chr, NULL, 0U, &i, &props);

    if ( (KODE_OK == rc) && !op_fits(KDRV_BLE_APP_OP_CLEAR, props, 0U) )
    {
        rc = KODE_DENIED;
    }
    if ( (KODE_OK == rc) && (NULL != s_mem) )
    {
        khal_mutex_lock(s_mem->lock);
        app_queue_t *const q = queue_of(i, false);

        if ( NULL != q )
        {
            q->head  = 0U;
            q->count = 0U;
        }
        khal_mutex_unlock(s_mem->lock);
    }
    return rc;
}

kode_status_t kdrv_ble_app_queue_room(const char *chr, size_t *room)
{
    uint8_t       i     = 0U;
    uint8_t       props = 0U;
    kode_status_t rc    = app_find(chr, NULL, 0U, &i, &props);

    if ( (KODE_OK == rc) && (!op_fits(KDRV_BLE_APP_OP_CLEAR, props, 0U) || (NULL == room)) )
    {
        rc = KODE_DENIED;
    }
    if ( KODE_OK == rc )
    {
        size_t used = 0U;

        if ( NULL != s_mem )
        {
            khal_mutex_lock(s_mem->lock);
            const app_queue_t *const q = queue_of(i, false);

            if ( NULL != q )
            {
                used = q->count;
            }
            khal_mutex_unlock(s_mem->lock);
        }
        *room = KDRV_BLE_APP_QUEUE_SLOTS - used;
    }
    return rc;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
void *kdrv_ble_app_host(void)
{
    void *p = NULL;

    if ( NULL != s_mem )
    {
        p = (void *)&((uint8_t *)s_mem)[APP_HOST_OFFSET];
    }
    return p;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
bool kdrv_ble_app_queued(void)
{
    bool queued = false;

    if ( NULL != s_mem )
    {
        khal_mutex_lock(s_mem->lock);
        queued = (0U < s_mem->count);
        khal_mutex_unlock(s_mem->lock);
    }
    return queued;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
void kdrv_ble_app_link_reset(void)
{
    if ( NULL != s_mem )
    {
        khal_mutex_lock(s_mem->lock);
        (void)memset(s_mem->sub, 0, sizeof(s_mem->sub));
        s_mem->head  = 0U;
        s_mem->count = 0U;
        for ( uint8_t i = 0U; KDRV_BLE_APP_CHARS_MAX > i; i++ )
        {
            app_queue_t *const q = queue_of(i, false);

            if ( NULL != q )
            {
                q->head  = 0U;
                q->count = 0U;
            }
        }
        khal_mutex_unlock(s_mem->lock);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
void kdrv_ble_app_subscribed(uint8_t chr, bool on)
{
    if ( (NULL != s_mem) && (KDRV_BLE_APP_CHARS_MAX > chr) )
    {
        khal_mutex_lock(s_mem->lock);
        s_mem->sub[chr] = on;
        khal_mutex_unlock(s_mem->lock);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
bool kdrv_ble_app_written(uint8_t chr, const uint8_t *data, uint16_t len)
{
    const kdrv_ble_app_service_t *const svc  = kdrv_ble_app_service();
    bool                                took = false;

    if ( (NULL != s_mem) && (NULL != svc) && (chr < svc->count) && (KDRV_BLE_APP_CHARS_MAX > chr)
        && (KDRV_BLE_APP_VALUE_MAX >= len) && ((0U == len) || (NULL != data)) )
    {
        took = true;
        if ( NULL != s_write_cb )
        {
            took = s_write_cb(chr, svc->chars[chr].name, data, len);
        }
        if ( took )
        {
            khal_mutex_lock(s_mem->lock);
            value_put(chr, data, len);
            khal_mutex_unlock(s_mem->lock);
        }
    }
    return took;
}

/**
 * @brief Copy queue @p q's oldest value out, and take it once the read reaches its end.
 * @note Called with the lock held.
 * @param q      The queue.
 * @param offset As kdrv_ble_app_read().
 * @param span   As kdrv_ble_app_read().
 * @param out    As kdrv_ble_app_read().
 * @param cap    As kdrv_ble_app_read().
 * @param taken  Set true when the value was taken out.
 * @return Its bytes; 0 for an empty queue.
 */
static uint16_t queue_read(app_queue_t *q, uint16_t offset, uint16_t span, uint8_t *out,
                           uint16_t cap, bool *taken)
{
    uint16_t n = 0U;

    if ( 0U < q->count )
    {
        uint16_t const whole = q->len[q->head];

        n = whole;
        if ( n > cap )
        {
            n = cap;
        }
        (void)memcpy(out, q->data[q->head], n);
        if ( ((uint32_t)offset + (uint32_t)span) >= (uint32_t)whole )
        {
            q->head = (q->head + 1U) % KDRV_BLE_APP_QUEUE_SLOTS;
            q->count--;
            *taken = true;
        }
    }
    return n;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
uint16_t kdrv_ble_app_read(uint8_t chr, uint16_t offset, uint16_t span, uint8_t *out,
                           uint16_t cap)
{
    const kdrv_ble_app_service_t *const svc   = kdrv_ble_app_service();
    uint16_t                            n     = 0U;
    bool                                taken = false;
    uint8_t                             left  = 0U;

    if ( (NULL != s_mem) && (NULL != svc) && (chr < svc->count)
        && (KDRV_BLE_APP_CHARS_MAX > chr) && (NULL != out) )
    {
        khal_mutex_lock(s_mem->lock);
        if ( (svc->chars[chr].props & KDRV_BLE_APP_QUEUED) != 0U )
        {
            app_queue_t *const q = queue_of(chr, false);

            if ( NULL != q )
            {
                n    = queue_read(q, offset, span, out, cap, &taken);
                left = (uint8_t)q->count;
            }
        }
        else
        {
            n = s_mem->vlen[chr];
            if ( n > cap )
            {
                n = cap;
            }
            if ( 0U < n )
            {
                (void)memcpy(out, s_mem->value[chr], n);
            }
        }
        khal_mutex_unlock(s_mem->lock);
    }
    if ( taken && (NULL != s_taken_cb) )
    {
        s_taken_cb(chr, svc->chars[chr].name, left);
    }
    return n;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
uint8_t *kdrv_ble_app_scratch(void)
{
    uint8_t *p = NULL;

    if ( NULL != s_mem )
    {
        p = s_mem->scratch;
    }
    return p;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
bool kdrv_ble_app_peek(uint8_t *chr, uint8_t *out, uint16_t *len)
{
    bool got = false;

    if ( NULL != s_mem )
    {
        khal_mutex_lock(s_mem->lock);
        if ( 0U < s_mem->count )
        {
            const app_note_t *const n = &s_mem->ring[s_mem->head];

            *chr = n->chr;
            *len = n->len;
            if ( 0U < n->len )
            {
                (void)memcpy(out, n->data, n->len);
            }
            got = true;
        }
        khal_mutex_unlock(s_mem->lock);
    }
    return got;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
void kdrv_ble_app_pop(void)
{
    if ( NULL != s_mem )
    {
        khal_mutex_lock(s_mem->lock);
        if ( 0U < s_mem->count )
        {
            s_mem->head = (s_mem->head + 1U) % KDRV_BLE_APP_RING_SLOTS;
            s_mem->count--;
        }
        khal_mutex_unlock(s_mem->lock);
    }
}
