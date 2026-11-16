/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble_hid.c
 * @brief The HID service without NimBLE: the choice of kinds, the report map, the ring and the table's print.
 */
#include "kdrv_ble_hid_table.h"

#include <stdatomic.h>
#include <string.h>

#include "khal_nvs.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kode_log.h"
#include "sdkconfig.h"

#define TAG "kdrv.ble"

/** @brief s_hid_pick's bit for "the table is built": the kinds beside it are served. */
#define HID_PICK_BUILT   0x80U

/** @brief The kinds, counted: the size of what is kept per kind. */
#define HID_KIND_COUNT   2U

/** @brief FNV-1a's prime for 32 bits. */
#define HID_PRINT_PRIME  16777619U

/** @brief What a table with HID is kept under when its print adds up to KDRV_BLE_HID_PRINT_NONE. */
#define HID_PRINT_MOVED  1U

_Static_assert((KDRV_BLE_HID_KINDS & HID_PICK_BUILT) == 0U, "the built mark is no kind");
_Static_assert(KDRV_BLE_HID_MOUSE_LEN <= KDRV_BLE_HID_REPORT_MAX, "a mouse report fits a slot");
_Static_assert(KDRV_BLE_HID_KEYS_LEN <= KDRV_BLE_HID_REPORT_MAX, "a keyboard report fits a slot");
_Static_assert((KDRV_BLE_HID_KEYS_AT + KDRV_BLE_HID_KEYS_MAX) == KDRV_BLE_HID_KEYS_LEN,
               "the keys fill a keyboard report from their place to its end");
_Static_assert(KDRV_BLE_HID_RING_SLOTS <= (uint32_t)UINT8_MAX, "the free slots fit their counter");

/** @brief One report waiting for the host task. */
typedef struct {
    uint8_t kind;                           /**< Its kind.         */
    uint8_t len;                            /**< Bytes of @c data. */
    uint8_t data[KDRV_BLE_HID_REPORT_MAX];  /**< The report.       */
} hid_note_t;

/** @brief Everything the service changes at run time, one block in PSRAM, guarded by s_hid_lock. */
typedef struct {
    uint32_t   head;                                          /**< The oldest report.            */
    uint32_t   count;                                         /**< Reports waiting.              */
    hid_note_t ring[KDRV_BLE_HID_RING_SLOTS];                 /**< The ring.                     */
    uint8_t    last[HID_KIND_COUNT][KDRV_BLE_HID_REPORT_MAX]; /**< The last report sent, per kind. */
    uint8_t    free;                 /**< Slots neither queued nor claimed.                        */
    uint8_t    out[HID_KIND_COUNT];  /**< Reports of each kind claimed and neither sent nor dropped. */
    uint8_t    owed;                 /**< The kinds whose report of zeroes waits for theirs to go. */
    uint8_t    peeked;               /**< The kind whose owed report the last peek gave; 0 for the
                                      *   ring's head. The host task's alone.                    */
} hid_mem_t;

/** @brief The block, once taken; NULL before. */
static hid_mem_t *s_hid_mem;

/** @brief Guards the block; made with it, and kept. */
static khal_mutex_t s_hid_lock;

/** @brief The choice: the kinds, with HID_PICK_BUILT once the table is built. */
static _Atomic uint8_t s_hid_pick;

/** @brief The kinds whose input report a host is subscribed to. */
static _Atomic uint8_t s_hid_sub;

/** @brief The keyboard LEDs the host wrote. */
static _Atomic uint8_t s_hid_leds;

/** @brief The time to pair a new device runs. */
static atomic_bool s_hid_window;

/** @brief Reports the link took since boot. */
static _Atomic uint32_t s_hid_sent;

/** @brief Reports queued or claimed and never sent, since boot. */
static _Atomic uint32_t s_hid_dropped;

/** @brief The refusal has been logged once. */
static atomic_bool s_hid_refusal_said;

/**
 * @brief Where a kind's last report is kept.
 * @param kind A kind.
 * @return Its place; HID_KIND_COUNT for anything that is not one kind.
 */
static uint8_t kind_index(uint8_t kind)
{
    uint8_t i = (uint8_t)HID_KIND_COUNT;

    if ( KDRV_BLE_HID_MOUSE == kind )
    {
        i = 0U;
    }
    else if ( KDRV_BLE_HID_KEYBOARD == kind )
    {
        i = 1U;
    }
    else
    {
        /* not one kind */
    }
    return i;
}

/**
 * @brief Bytes of a report of @p kind.
 * @param kind A kind.
 * @return Its length; 0 for anything that is not one kind.
 */
static uint8_t kind_len(uint8_t kind)
{
    uint8_t len = 0U;

    if ( KDRV_BLE_HID_MOUSE == kind )
    {
        len = (uint8_t)KDRV_BLE_HID_MOUSE_LEN;
    }
    else if ( KDRV_BLE_HID_KEYBOARD == kind )
    {
        len = (uint8_t)KDRV_BLE_HID_KEYS_LEN;
    }
    else
    {
        /* not one kind */
    }
    return len;
}

/**
 * @brief Give back the slots of reports of one kind that are neither queued nor claimed any more.
 * @note Called with s_hid_lock held.
 * @param i     The kind's place, under HID_KIND_COUNT.
 * @param slots How many.
 */
static void slots_back(uint8_t i, uint8_t slots)
{
    s_hid_mem->free = (uint8_t)(s_hid_mem->free + slots);
    if ( s_hid_mem->out[i] >= slots )
    {
        s_hid_mem->out[i] = (uint8_t)(s_hid_mem->out[i] - slots);
    }
    else
    {
        s_hid_mem->out[i] = 0U;
    }
}

/**
 * @brief Count one report in @p count.
 * @param count s_hid_sent or s_hid_dropped.
 */
static void count_one(_Atomic uint32_t *count)
{
    /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
    (void)atomic_fetch_add(count, 1U);
}

kode_status_t kdrv_ble_hid_select(uint8_t kinds)
{
    kode_status_t rc = KODE_DENIED;

#ifdef CONFIG_KODEOS_BLE_CONSOLE_PAIRING
    if ( 0U == (kinds & (uint8_t)~KDRV_BLE_HID_KINDS) )
    {
        uint8_t was  = atomic_load(&s_hid_pick);
        bool    took = false;

        while ( (0U == (was & HID_PICK_BUILT)) && !took )
        {
            took = atomic_compare_exchange_weak(&s_hid_pick, &was, kinds);
        }
        if ( took )
        {
            rc = KODE_OK;
        }
        else if ( !atomic_exchange(&s_hid_refusal_said, true) )
        {
            KLOG_W(TAG, "hid: the GATT table is registered already; the choice takes "
                        "effect at the next boot that chooses before Bluetooth starts");
        }
        else
        {
            /* refused, and said once */
        }
    }
#else
    (void)kinds;
#endif
    return rc;
}

uint8_t kdrv_ble_hid_kinds(void)
{
    return atomic_load(&s_hid_pick) & (uint8_t)KDRV_BLE_HID_KINDS;
}

bool kdrv_ble_hid_served(void)
{
    uint8_t const pick = atomic_load(&s_hid_pick);

    return (0U != (pick & HID_PICK_BUILT)) && (0U != (pick & (uint8_t)KDRV_BLE_HID_KINDS));
}

/** @brief Take the ring and its lock. @return true when both exist. */
static bool hid_mem_make(void)
{
    if ( NULL == s_hid_lock )
    {
        s_hid_lock = khal_mutex_create();
    }
    if ( (NULL != s_hid_lock) && (NULL == s_hid_mem) )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned as asked */
        hid_mem_t *const m = (hid_mem_t *)khal_alloc_psram(sizeof(uint32_t), sizeof(hid_mem_t));

        if ( NULL != m )
        {
            (void)memset(m, 0, sizeof(*m));
            m->free   = (uint8_t)KDRV_BLE_HID_RING_SLOTS;
            s_hid_mem = m;
        }
    }
    return (NULL != s_hid_lock) && (NULL != s_hid_mem);
}

/* cppcheck-suppress misra-c2012-8.7 ; kdrv_ble.c builds the GATT table from it */
uint8_t kdrv_ble_hid_fix(void)
{
    uint8_t was = atomic_load(&s_hid_pick);

    while ( 0U == (was & HID_PICK_BUILT) )
    {
        uint8_t kinds = was;

        if ( (0U != kinds) && !hid_mem_make() )
        {
            KLOG_E(TAG, "hid: no PSRAM for the reports; the service is left out");
            kinds = 0U;
        }
        /* A swap that fails reloads @c was with the choice that beat it. */
        if ( atomic_compare_exchange_weak(&s_hid_pick, &was, (uint8_t)(kinds | HID_PICK_BUILT)) )
        {
            was = (uint8_t)(kinds | HID_PICK_BUILT);
        }
    }
    return was & (uint8_t)KDRV_BLE_HID_KINDS;
}

/* cppcheck-suppress misra-c2012-8.7 ; the host suite starts each case from no choice with it */
void kdrv_ble_hid_select_forget(void)
{
    kdrv_ble_hid_link_reset();
    if ( NULL != s_hid_mem )
    {
        khal_free(s_hid_mem);
        s_hid_mem = NULL;
    }
    atomic_store(&s_hid_pick, 0U);
    atomic_store(&s_hid_window, false);
    atomic_store(&s_hid_sent, 0U);
    atomic_store(&s_hid_dropped, 0U);
    atomic_store(&s_hid_refusal_said, false);
}

/* cppcheck-suppress misra-c2012-8.7 ; kdrv_ble.c answers a read of Report Map with it */
size_t kdrv_ble_hid_report_map(uint8_t kinds, uint8_t *out, size_t cap)
{
    /* The mouse's collection: report KDRV_BLE_HID_ID_MOUSE, four bytes. Every
     * item is one prefix byte and one byte of data. */
    static const uint8_t map_mouse[] = {
        0x05U, 0x01U,                   /* Usage Page (Generic Desktop)      */
        0x09U, 0x02U,                   /* Usage (Mouse)                     */
        0xA1U, 0x01U,                   /* Collection (Application)          */
        0x85U, KDRV_BLE_HID_ID_MOUSE,   /*   Report ID                       */
        0x09U, 0x01U,                   /*   Usage (Pointer)                 */
        0xA1U, 0x00U,                   /*   Collection (Physical)           */
        0x05U, 0x09U,                   /*     Usage Page (Buttons)          */
        0x19U, 0x01U,                   /*     Usage Minimum (1)             */
        0x29U, 0x03U,                   /*     Usage Maximum (3)             */
        0x15U, 0x00U,                   /*     Logical Minimum (0)           */
        0x25U, 0x01U,                   /*     Logical Maximum (1)           */
        0x95U, 0x03U,                   /*     Report Count (3)              */
        0x75U, 0x01U,                   /*     Report Size (1)               */
        0x81U, 0x02U,                   /*     Input (Data, Variable, Absolute): the buttons */
        0x95U, 0x01U,                   /*     Report Count (1)              */
        0x75U, 0x05U,                   /*     Report Size (5)               */
        0x81U, 0x03U,                   /*     Input (Constant): the byte's other bits */
        0x05U, 0x01U,                   /*     Usage Page (Generic Desktop)  */
        0x09U, 0x30U,                   /*     Usage (X)                     */
        0x09U, 0x31U,                   /*     Usage (Y)                     */
        0x09U, 0x38U,                   /*     Usage (Wheel)                 */
        0x15U, 0x81U,                   /*     Logical Minimum (-127)        */
        0x25U, 0x7FU,                   /*     Logical Maximum (127)         */
        0x75U, 0x08U,                   /*     Report Size (8)               */
        0x95U, 0x03U,                   /*     Report Count (3)              */
        0x81U, 0x06U,                   /*     Input (Data, Variable, Relative): x, y, wheel */
        0xC0U,                          /*   End Collection                  */
        0xC0U,                          /* End Collection                    */
    };

    /* The keyboard's collection: report KDRV_BLE_HID_ID_KEYBOARD, the boot
     * keyboard's eight bytes in, and one byte of LEDs out. */
    static const uint8_t map_keys[] = {
        0x05U, 0x01U,                     /* Usage Page (Generic Desktop)    */
        0x09U, 0x06U,                     /* Usage (Keyboard)                */
        0xA1U, 0x01U,                     /* Collection (Application)        */
        0x85U, KDRV_BLE_HID_ID_KEYBOARD,  /*   Report ID                     */
        0x05U, 0x07U,                     /*   Usage Page (Keyboard)         */
        0x19U, 0xE0U,                     /*   Usage Minimum (Left Control)  */
        0x29U, 0xE7U,                     /*   Usage Maximum (Right GUI)     */
        0x15U, 0x00U,                     /*   Logical Minimum (0)           */
        0x25U, 0x01U,                     /*   Logical Maximum (1)           */
        0x75U, 0x01U,                     /*   Report Size (1)               */
        0x95U, 0x08U,                     /*   Report Count (8)              */
        0x81U, 0x02U,                     /*   Input (Data, Variable, Absolute): the modifiers */
        0x95U, 0x01U,                     /*   Report Count (1)              */
        0x75U, 0x08U,                     /*   Report Size (8)               */
        0x81U, 0x01U,                     /*   Input (Constant): the reserved byte */
        0x95U, 0x05U,                     /*   Report Count (5)              */
        0x75U, 0x01U,                     /*   Report Size (1)               */
        0x05U, 0x08U,                     /*   Usage Page (LEDs)             */
        0x19U, 0x01U,                     /*   Usage Minimum (Num Lock)      */
        0x29U, 0x05U,                     /*   Usage Maximum (Kana)          */
        0x91U, 0x02U,                     /*   Output (Data, Variable, Absolute): the LEDs */
        0x95U, 0x01U,                     /*   Report Count (1)              */
        0x75U, 0x03U,                     /*   Report Size (3)               */
        0x91U, 0x01U,                     /*   Output (Constant): the byte's other bits */
        0x95U, KDRV_BLE_HID_KEYS_MAX,     /*   Report Count: the keys        */
        0x75U, 0x08U,                     /*   Report Size (8)               */
        0x15U, 0x00U,                     /*   Logical Minimum (0)           */
        0x25U, KDRV_BLE_HID_KEY_MAX,      /*   Logical Maximum: the last key */
        0x05U, 0x07U,                     /*   Usage Page (Keyboard)         */
        0x19U, 0x00U,                     /*   Usage Minimum (0)             */
        0x29U, KDRV_BLE_HID_KEY_MAX,      /*   Usage Maximum: the last key   */
        0x81U, 0x00U,                     /*   Input (Data, Array): the keys */
        0xC0U,                            /* End Collection                  */
    };

    _Static_assert((sizeof(map_mouse) + sizeof(map_keys)) <= KDRV_BLE_HID_MAP_MAX,
                   "the longest map fits KDRV_BLE_HID_MAP_MAX");
    size_t need = 0U;
    size_t n    = 0U;

    if ( 0U != (kinds & KDRV_BLE_HID_MOUSE) )
    {
        need += sizeof(map_mouse);
    }
    if ( 0U != (kinds & KDRV_BLE_HID_KEYBOARD) )
    {
        need += sizeof(map_keys);
    }
    if ( (NULL != out) && (need <= cap) )
    {
        if ( 0U != (kinds & KDRV_BLE_HID_MOUSE) )
        {
            (void)memcpy(out, map_mouse, sizeof(map_mouse));
            n = sizeof(map_mouse);
        }
        if ( 0U != (kinds & KDRV_BLE_HID_KEYBOARD) )
        {
            (void)memcpy(&out[n], map_keys, sizeof(map_keys));
            n += sizeof(map_keys);
        }
    }
    return n;
}

/* cppcheck-suppress misra-c2012-8.7 ; kdrv_ble.c puts it in the advert and in the GAP service */
uint16_t kdrv_ble_hid_appearance(uint8_t kinds)
{
    uint16_t appearance = 0U;

    if ( KDRV_BLE_HID_KINDS == (kinds & KDRV_BLE_HID_KINDS) )
    {
        appearance = (uint16_t)KDRV_BLE_HID_APPEAR_HID;
    }
    else if ( 0U != (kinds & KDRV_BLE_HID_KEYBOARD) )
    {
        appearance = (uint16_t)KDRV_BLE_HID_APPEAR_KEYS;
    }
    else if ( 0U != (kinds & KDRV_BLE_HID_MOUSE) )
    {
        appearance = (uint16_t)KDRV_BLE_HID_APPEAR_MOUSE;
    }
    else
    {
        /* no kind: no appearance */
    }
    return appearance;
}

bool kdrv_ble_hid_ready(void)
{
    uint8_t const pick  = atomic_load(&s_hid_pick);
    uint8_t const kinds = pick & (uint8_t)KDRV_BLE_HID_KINDS;

    return (0U != (pick & HID_PICK_BUILT)) && (0U != kinds)
           && ((atomic_load(&s_hid_sub) & kinds) == kinds);
}

/**
 * @brief One step of a mouse report as its byte.
 * @param step The move or the turn.
 * @return Its two's complement, with -128 written as -127.
 */
static uint8_t step_byte(int8_t step)
{
    int8_t s = step;

    if ( (-KDRV_BLE_HID_STEP_MAX) > s )
    {
        s = (int8_t)(-KDRV_BLE_HID_STEP_MAX);
    }
    return (uint8_t)s;
}

void kdrv_ble_hid_mouse_pack(uint8_t buttons, int8_t dx, int8_t dy, int8_t wheel,
                             uint8_t out[KDRV_BLE_HID_MOUSE_LEN])
{
    if ( NULL != out )
    {
        out[0] = buttons & (uint8_t)KDRV_BLE_HID_BUTTONS;
        out[1] = step_byte(dx);
        out[2] = step_byte(dy);
        out[3] = step_byte(wheel);
    }
}

void kdrv_ble_hid_keys_pack(uint8_t modifiers, const uint8_t *keys,
                            uint8_t out[KDRV_BLE_HID_KEYS_LEN])
{
    if ( NULL != out )
    {
        (void)memset(out, 0, KDRV_BLE_HID_KEYS_LEN);
        out[0] = modifiers;
        if ( NULL != keys )
        {
            (void)memcpy(&out[KDRV_BLE_HID_KEYS_AT], keys, KDRV_BLE_HID_KEYS_MAX);
        }
    }
}

/**
 * @brief Whether reports of @p kind can go: the kind is served and a host is subscribed to it.
 * @param kind A kind.
 * @return true when they can.
 */
static bool kind_hosted(uint8_t kind)
{
    uint8_t const pick = atomic_load(&s_hid_pick);

    return ((uint8_t)HID_KIND_COUNT > kind_index(kind)) && (0U != (pick & HID_PICK_BUILT))
           && (0U != (pick & kind)) && (0U != (atomic_load(&s_hid_sub) & kind))
           && (NULL != s_hid_mem);
}

kode_status_t kdrv_ble_hid_claim(uint8_t kind, uint8_t slots)
{
    kode_status_t rc = KODE_DENIED;

    if ( kind_hosted(kind) && (0U < slots) && ((uint8_t)KDRV_BLE_HID_RING_SLOTS >= slots) )
    {
        uint8_t const i = kind_index(kind);

        rc = KODE_NO_RESOURCES;
        khal_mutex_lock(s_hid_lock);
        /* A kind that owes its report of zeroes takes nothing behind it: that
         * report goes first, when the ones before it have gone. */
        if ( (0U == (s_hid_mem->owed & kind)) && (s_hid_mem->free >= slots) )
        {
            s_hid_mem->free   = (uint8_t)(s_hid_mem->free - slots);
            s_hid_mem->out[i] = (uint8_t)(s_hid_mem->out[i] + slots);
            rc                = KODE_OK;
        }
        khal_mutex_unlock(s_hid_lock);
    }
    return rc;
}

void kdrv_ble_hid_unclaim(uint8_t kind, uint8_t slots)
{
    uint8_t const i = kind_index(kind);

    if ( (NULL != s_hid_mem) && ((uint8_t)HID_KIND_COUNT > i) )
    {
        khal_mutex_lock(s_hid_lock);
        slots_back(i, slots);
        khal_mutex_unlock(s_hid_lock);
    }
}

kode_status_t kdrv_ble_hid_owe(uint8_t kind)
{
    kode_status_t rc = KODE_DENIED;

    if ( kind_hosted(kind) )
    {
        khal_mutex_lock(s_hid_lock);
        s_hid_mem->owed |= kind;
        khal_mutex_unlock(s_hid_lock);
        rc = KODE_OK;
    }
    return rc;
}

void kdrv_ble_hid_push(uint8_t kind, const uint8_t *data, size_t len)
{
    uint8_t const i      = kind_index(kind);
    bool          queued = false;

    if ( (NULL != s_hid_mem) && ((uint8_t)HID_KIND_COUNT > i) )
    {
        khal_mutex_lock(s_hid_lock);
        /* The subscription is read under the lock a link's end takes, so a
         * report never lands in a ring that end has emptied. */
        if ( (NULL != data) && ((size_t)kind_len(kind) == len)
            && (0U != (atomic_load(&s_hid_sub) & kind))
            && (KDRV_BLE_HID_RING_SLOTS > s_hid_mem->count) )
        {
            hid_note_t *const n = &s_hid_mem->ring[(s_hid_mem->head + s_hid_mem->count) % KDRV_BLE_HID_RING_SLOTS];

            n->kind = kind;
            n->len  = (uint8_t)len;
            (void)memcpy(n->data, data, len);
            ++s_hid_mem->count;
            queued = true;
        }
        else
        {
            slots_back(i, 1U);
        }
        khal_mutex_unlock(s_hid_lock);
    }
    if ( !queued )
    {
        count_one(&s_hid_dropped);
    }
}

void kdrv_ble_hid_counts(uint32_t *sent, uint32_t *dropped)
{
    if ( NULL != sent )
    {
        *sent = atomic_load(&s_hid_sent);
    }
    if ( NULL != dropped )
    {
        *dropped = atomic_load(&s_hid_dropped);
    }
}

void kdrv_ble_hid_pair_window(bool open)
{
    atomic_store(&s_hid_window, open);
}

/* cppcheck-suppress misra-c2012-8.7 ; kdrv_ble.c asks it for every pairing in an app */
bool kdrv_ble_hid_pair_open(void)
{
    return atomic_load(&s_hid_window) && kdrv_ble_hid_served();
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, kept apart from this one so the host suite compiles these rules without NimBLE */
void kdrv_ble_hid_link_reset(void)
{
    atomic_store(&s_hid_sub, 0U);
    atomic_store(&s_hid_leds, 0U);
    if ( NULL != s_hid_mem )
    {
        khal_mutex_lock(s_hid_lock);
        for ( uint32_t k = 0U; k < s_hid_mem->count; ++k )
        {
            const hid_note_t *const n = &s_hid_mem->ring[(s_hid_mem->head + k) % KDRV_BLE_HID_RING_SLOTS];
            uint8_t const           i = kind_index(n->kind);

            count_one(&s_hid_dropped);
            if ( (uint8_t)HID_KIND_COUNT > i )
            {
                slots_back(i, 1U);
            }
        }
        s_hid_mem->head   = 0U;
        s_hid_mem->count  = 0U;
        s_hid_mem->owed   = 0U;
        s_hid_mem->peeked = 0U;
        (void)memset(s_hid_mem->last, 0, sizeof(s_hid_mem->last));
        khal_mutex_unlock(s_hid_lock);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c calls it from its own translation unit, as kdrv_ble_hid_link_reset() */
void kdrv_ble_hid_subscribed(uint8_t kind, bool on)
{
    if ( (uint8_t)HID_KIND_COUNT > kind_index(kind) )
    {
        if ( on )
        {
            /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
            (void)atomic_fetch_or(&s_hid_sub, kind);
        }
        else
        {
            /* cppcheck-suppress misra-c2012-17.3 ; GCC's __atomic builtins are the compiler's own, never declared in a header */
            (void)atomic_fetch_and(&s_hid_sub, (uint8_t)~kind);
        }
    }
}

/**
 * @brief The kind whose owed report of zeroes can go now: nothing of its own is ahead of it.
 * @note Called with s_hid_lock held.
 * @return The kind; 0 when no kind owes one, or its reports have not all gone.
 */
static uint8_t owed_due(void)
{
    uint8_t kind = 0U;

    if ( (0U != (s_hid_mem->owed & KDRV_BLE_HID_MOUSE)) && (0U == s_hid_mem->out[0]) )
    {
        kind = (uint8_t)KDRV_BLE_HID_MOUSE;
    }
    else if ( (0U != (s_hid_mem->owed & KDRV_BLE_HID_KEYBOARD)) && (0U == s_hid_mem->out[1]) )
    {
        kind = (uint8_t)KDRV_BLE_HID_KEYBOARD;
    }
    else
    {
        /* none owed, or its reports are still on their way */
    }
    return kind;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c wakes its drain by it */
bool kdrv_ble_hid_queued(void)
{
    bool queued = false;

    if ( NULL != s_hid_mem )
    {
        khal_mutex_lock(s_hid_lock);
        queued = (0U < s_hid_mem->count) || (0U != owed_due());
        khal_mutex_unlock(s_hid_lock);
    }
    return queued;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c drains the ring with it */
bool kdrv_ble_hid_peek(uint8_t *kind, uint8_t *out, uint16_t *len)
{
    bool got = false;

    if ( (NULL != s_hid_mem) && (NULL != kind) && (NULL != out) && (NULL != len) )
    {
        khal_mutex_lock(s_hid_lock);
        s_hid_mem->peeked = owed_due();
        if ( 0U != s_hid_mem->peeked )
        {
            *kind = s_hid_mem->peeked;
            *len  = kind_len(s_hid_mem->peeked);
            (void)memset(out, 0, *len);
            got = true;
        }
        else if ( 0U < s_hid_mem->count )
        {
            const hid_note_t *const n = &s_hid_mem->ring[s_hid_mem->head];

            *kind = n->kind;
            *len  = n->len;
            (void)memcpy(out, n->data, n->len);
            got = true;
        }
        else
        {
            /* nothing to send */
        }
        khal_mutex_unlock(s_hid_lock);
    }
    return got;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c drains the ring with it */
void kdrv_ble_hid_pop(bool sent)
{
    bool took = false;

    if ( NULL != s_hid_mem )
    {
        khal_mutex_lock(s_hid_lock);
        if ( 0U != s_hid_mem->peeked )
        {
            uint8_t const i = kind_index(s_hid_mem->peeked);

            /* The owed report of zeroes: no slot was ever its own. */
            s_hid_mem->owed &= (uint8_t)~s_hid_mem->peeked;
            if ( sent && ((uint8_t)HID_KIND_COUNT > i) )
            {
                (void)memset(s_hid_mem->last[i], 0, sizeof(s_hid_mem->last[i]));
            }
            s_hid_mem->peeked = 0U;
            took              = true;
        }
        else if ( 0U < s_hid_mem->count )
        {
            const hid_note_t *const n = &s_hid_mem->ring[s_hid_mem->head];
            uint8_t const           i = kind_index(n->kind);

            if ( (uint8_t)HID_KIND_COUNT > i )
            {
                if ( sent )
                {
                    (void)memcpy(s_hid_mem->last[i], n->data, n->len);
                }
                slots_back(i, 1U);
            }
            s_hid_mem->head = (s_hid_mem->head + 1U) % KDRV_BLE_HID_RING_SLOTS;
            --s_hid_mem->count;
            took = true;
        }
        else
        {
            /* nothing was there */
        }
        khal_mutex_unlock(s_hid_lock);
    }
    if ( took )
    {
        if ( sent )
        {
            count_one(&s_hid_sent);
        }
        else
        {
            count_one(&s_hid_dropped);
        }
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c answers a read of an input report with it */
uint16_t kdrv_ble_hid_last(uint8_t kind, uint8_t *out)
{
    uint8_t const i   = kind_index(kind);
    uint16_t      len = 0U;

    if ( ((uint8_t)HID_KIND_COUNT > i) && (NULL != out) )
    {
        len = kind_len(kind);
        (void)memset(out, 0, len);
        if ( NULL != s_hid_mem )
        {
            khal_mutex_lock(s_hid_lock);
            (void)memcpy(out, s_hid_mem->last[i], len);
            khal_mutex_unlock(s_hid_lock);
        }
    }
    return len;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c keeps a write of the output report with it */
void kdrv_ble_hid_leds_set(uint8_t leds)
{
    atomic_store(&s_hid_leds, leds);
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c answers a read of the output report with it */
uint8_t kdrv_ble_hid_leds(void)
{
    return atomic_load(&s_hid_leds);
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c prints the table it registers with it */
uint32_t kdrv_ble_hid_print_add(uint32_t print, const void *data, size_t len)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the bytes of an object, read as bytes */
    const uint8_t *const bytes = (const uint8_t *)data;
    uint32_t             h     = print;

    if ( NULL != bytes )
    {
        for ( size_t i = 0U; i < len; ++i )
        {
            h ^= (uint32_t)bytes[i];
            h *= HID_PRINT_PRIME;
        }
    }
    return h;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c prints the table it registers with it */
uint32_t kdrv_ble_hid_print_end(uint32_t print, bool served)
{
    uint32_t out = KDRV_BLE_HID_PRINT_NONE;

    if ( served )
    {
        out = print;
        if ( KDRV_BLE_HID_PRINT_NONE == out )
        {
            out = HID_PRINT_MOVED;
        }
    }
    return out;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c decides the forgetting with it */
bool kdrv_ble_hid_prune_due(uint32_t kept, uint32_t now)
{
    return kept != now;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c decides the forgetting with it */
bool kdrv_ble_hid_prune_takes(uint16_t handle, uint16_t changed)
{
    return handle != changed;
}

/* ---- The bonds made for the HID service ---------------------------------------
 * The mark of a bond: kdrv_ble_hid_table.h says what it decides. Kept between
 * boots, so a HID bond is one after a reboot, and a boot that serves no HID
 * knows the hosts that come back by themselves. */

/**
 * @brief Where @p addr is in @p hosts.
 * @param hosts The hosts.
 * @param addr  A device's identity address: its type, then its six bytes.
 * @return Its place; hosts->count when it is not there.
 */
static uint8_t host_find(const kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr)
{
    uint8_t found = hosts->count;

    for ( uint8_t i = 0U; (i < hosts->count) && (found == hosts->count); ++i )
    {
        if ( memcmp(hosts->addr[i], addr, KDRV_BLE_HID_ADDR_LEN) == 0 )
        {
            found = i;
        }
    }
    return found;
}

/**
 * @brief Take the host at @p at out of @p hosts, keeping the others in order.
 * @param hosts The hosts.
 * @param at    Its place, under hosts->count.
 */
static void host_take(kdrv_ble_hid_hosts_t *hosts, uint8_t at)
{
    for ( uint8_t i = at; (i + 1U) < hosts->count; ++i )
    {
        (void)memcpy(hosts->addr[i], hosts->addr[i + 1U], KDRV_BLE_HID_ADDR_LEN);
    }
    --hosts->count;
    (void)memset(hosts->addr[hosts->count], 0, KDRV_BLE_HID_ADDR_LEN);
}

/* cppcheck-suppress misra-c2012-8.7 ; the contract the host tests hold a stored record to, called from there */
bool kdrv_ble_hid_hosts_ok(const kdrv_ble_hid_hosts_t *hosts, size_t len)
{
    return (NULL != hosts) && (sizeof(*hosts) == len)
           && ((uint8_t)KDRV_BLE_HID_HOSTS_MAX >= hosts->count);
}

/* cppcheck-suppress misra-c2012-8.7 ; the contract the host tests hold the list to, called from there */
bool kdrv_ble_hid_hosts_has(const kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr)
{
    return (NULL != hosts) && (NULL != addr) && (host_find(hosts, addr) < hosts->count);
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c keeps a host that subscribed with it */
bool kdrv_ble_hid_hosts_add(kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr)
{
    bool changed = false;

    if ( (NULL != hosts) && (NULL != addr) && !kdrv_ble_hid_hosts_has(hosts, addr) )
    {
        if ( (uint8_t)KDRV_BLE_HID_HOSTS_MAX <= hosts->count )
        {
            /* No more hosts than bonds: the oldest gives its place. */
            host_take(hosts, 0U);
        }
        (void)memcpy(hosts->addr[hosts->count], addr, KDRV_BLE_HID_ADDR_LEN);
        ++hosts->count;
        changed = true;
    }
    return changed;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c forgets a host whose bond went with it */
bool kdrv_ble_hid_hosts_drop(kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr)
{
    bool changed = false;

    if ( kdrv_ble_hid_hosts_has(hosts, addr) )
    {
        host_take(hosts, host_find(hosts, addr));
        changed = true;
    }
    return changed;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c marks each bond NimBLE writes with it */
bool kdrv_ble_hid_bond_note(kdrv_ble_hid_hosts_t *hosts, const uint8_t *addr, bool served)
{
    bool changed;

    if ( served )
    {
        changed = kdrv_ble_hid_hosts_add(hosts, addr);
    }
    else
    {
        changed = kdrv_ble_hid_hosts_drop(hosts, addr);
    }
    return changed;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c checks the hosts against the bond store with it */
bool kdrv_ble_hid_hosts_sift(kdrv_ble_hid_hosts_t *hosts, kdrv_ble_hid_bonded_t bonded, void *ctx)
{
    bool changed = false;

    if ( (NULL != hosts) && (NULL != bonded) )
    {
        /* From the last: taking one out moves the ones after it. */
        for ( uint8_t i = hosts->count; 0U < i; --i )
        {
            if ( !bonded(hosts->addr[i - 1U], ctx) )
            {
                host_take(hosts, (uint8_t)(i - 1U));
                changed = true;
            }
        }
    }
    return changed;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c reads the hosts at each start with it */
void kdrv_ble_hid_hosts_load(kdrv_ble_hid_hosts_t *hosts)
{
    kdrv_ble_hid_hosts_t kept;
    size_t               len = sizeof(kept);

    if ( NULL != hosts )
    {
        (void)memset(hosts, 0, sizeof(*hosts));
        if ( (khal_nvs_get_blob(KDRV_BLE_HID_NVS_NS, KDRV_BLE_HID_NVS_HOSTS, &kept, &len) == KODE_OK)
            && kdrv_ble_hid_hosts_ok(&kept, len) )
        {
            *hosts = kept;
        }
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c keeps the hosts as they change with it */
void kdrv_ble_hid_hosts_store(const kdrv_ble_hid_hosts_t *hosts)
{
    if ( NULL == hosts )
    {
        /* nothing to keep */
    }
    else if ( 0U == hosts->count )
    {
        (void)khal_nvs_erase(KDRV_BLE_HID_NVS_NS, KDRV_BLE_HID_NVS_HOSTS);
    }
    else
    {
        (void)khal_nvs_set_blob(KDRV_BLE_HID_NVS_NS, KDRV_BLE_HID_NVS_HOSTS, hosts, sizeof(*hosts));
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c answers each authorization with it */
bool kdrv_ble_hid_access_ok(bool hid_attr, bool hid_bond)
{
    return hid_attr == hid_bond;
}

/* cppcheck-suppress misra-c2012-8.7 ; external, not static: kdrv_ble.c judges each encrypted link with it */
kdrv_ble_hid_host_do_t kdrv_ble_hid_host_judge(const kdrv_ble_hid_hosts_t *hosts,
                                               const uint8_t *addr, bool served)
{
    kdrv_ble_hid_host_do_t act = KDRV_BLE_HID_HOST_STAY;

    if ( !served && kdrv_ble_hid_hosts_has(hosts, addr) )
    {
        act = KDRV_BLE_HID_HOST_HANG_UP;
    }
    return act;
}
