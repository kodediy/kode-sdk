/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_event.c
 * @brief The standard layer's events: the ring, the listeners, the waiting tasks, the dispatcher.
 */
#include "kode.h"

#include <stdatomic.h>
#include <string.h>

#include "kdrv_buttons.h"
#include "kode_button.h"
#include "kode_entry.h"
#include "kode_log.h"
#include "kode_pin_lock.h"
#include "kode_standard_ao.h"
#include "kode_task.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "kqp.h"
#include "kaos.h"

#define TAG "kode.event"

/** @brief The prefix of every button event. */
static const char s_button_prefix[] = "button_";

/** @brief The suffix of a button event for a press. */
static const char s_what_pressed[] = "pressed";

/** @brief The suffix of a button event for a release. */
static const char s_what_released[] = "released";

/** @brief The suffix of a button event for a hold (top and bottom only). */
static const char s_what_held[] = "held";

/** @brief A pin's number in its event is written in decimal. */
#define PIN_NUMBER_BASE 10U

/** @brief Digits of the highest pin's number, KODE_PIN_LAST. */
#define PIN_NUMBER_DIGITS_MAX 2U

/* The owners an event comes from, and AO_Ir's capture door, named weakly
 * (KAOS_WEAK, kaos.h): an event is refused when its owner is not in the image,
 * and asking must not link it. */
KAOS_WEAK void kaos_ir_start(void);
KAOS_WEAK void kaos_radio_start(void);
KAOS_WEAK void kaos_touch_start(void);
KAOS_WEAK void touch_subscribe(QActive const *who);
KAOS_WEAK void ble_app_subscribe(QActive const *who);
KAOS_WEAK void ir_capture_request(IrSymbol *out, size_t cap, size_t *n, khal_sem_t done,
                                  uint32_t ms, uint32_t gen, uint32_t *ack);

/**
 * @brief Have AO_Ir listen for ir_received: a capture with no buffer, no
 *        waiter and no deadline, which arms the receiver and displaces no
 *        capture. With the C5 link down AO_Ir asks AO_Radio for it and the
 *        next link-up arms the receiver ("The link", ir_ao.c).
 */
static bool ir_arm_listener(void)
{
    ir_capture_request(NULL, 0U, NULL, NULL, 0U, 0U, NULL);
    return true;   /* AO_Ir reports a dry pool itself; nothing comes back here */
}

/**
 * @brief Have the standard layer's Active Object subscribe to the finger.
 *
 * Reached only for a touch event that can arrive, so AO_Touch is linked and
 * touch_subscribe(), which lives beside its start (tools/check_starts.sh),
 * with it.
 *
 * @return true once the request is posted, by this call or an earlier one;
 *         false when it could not be, and the next listener, wait or read
 *         asks again.
 */
static bool touch_listen(void)
{
    return kode_standard_ao_touch_listen(&touch_subscribe);
}

/**
 * @brief Have the standard layer's Active Object subscribe to the app's
 *        Bluetooth service: the link's edges and the writes.
 *
 * Reached only for a Bluetooth event that can arrive, so AO_Radio is linked
 * and ble_app_subscribe(), which lives beside its start
 * (tools/check_starts.sh), with it.
 *
 * @return true once subscribed, by this call or an earlier one; false in the
 *         kodeOS image.
 */
static bool ble_listen(void)
{
    return kode_standard_ao_subscribe(&ble_app_subscribe);
}

/**
 * @brief What a listener or a waiter of an event asks of its owner.
 *
 * The contract every row's hook keeps: it runs in the caller's task, once the
 * layer is up and the owner is linked, before the listener is registered or
 * the wait begins. It returns true when what the owner needs was asked for, or
 * had been already; false when it could not be. On false the call that asked
 * fails, registers nothing and names the event in kode_error(), so the hook
 * sets no error of its own. A hook may run many times for one event: asking
 * again must be harmless.
 */
typedef bool (*kode_listen_fn_t)(void);

/** @brief An event that is not a button: its exact name, its owner's start and what listening asks of it. */
typedef struct {
    const char     *name;    /**< The event, in lower case.                               */
    kaos_start_fn_t owner;   /**< The owner no other call need link, weakly; NULL: none.   */
    const char     *link;    /**< The kode.h name that links that owner; NULL with it.    */
    kode_listen_fn_t listen; /**< What a listener or a waiter asks of the owner (kode_listen_fn_t); NULL: nothing. */
} kode_known_t;

/**
 * @brief The events that are not buttons; a NULL name ends the list.
 *
 * Each module that sends an event adds its names here, and an event whose
 * owner no other call need link names that owner's start and the constant
 * that links it.
 */
static const kode_known_t s_known[] = {
    { KODE_EVENT_WIFI_UP_NAME,   &kaos_radio_start, "KODE_EVENT_WIFI_CONNECTED", NULL             },
    { KODE_EVENT_WIFI_LOST_NAME, &kaos_radio_start, "KODE_EVENT_WIFI_LOST",      NULL             },
    { "mqtt_message",            NULL,              NULL,                        NULL             },
    { "battery_low",             NULL,              NULL,                        NULL             },
    { KODE_EVENT_IR_NAME,        &kaos_ir_start,    "KODE_EVENT_IR_RECEIVED",    &ir_arm_listener },
    { KODE_EVENT_NFC_TAG,        NULL,              NULL,                        NULL             },
    { KODE_EVENT_TOUCH_PRESSED_NAME,  &kaos_touch_start, "KODE_EVENT_TOUCH_PRESSED",  &touch_listen },
    { KODE_EVENT_TOUCH_MOVED_NAME,    &kaos_touch_start, "KODE_EVENT_TOUCH_MOVED",    &touch_listen },
    { KODE_EVENT_TOUCH_RELEASED_NAME, &kaos_touch_start, "KODE_EVENT_TOUCH_RELEASED", &touch_listen },
    { KODE_EVENT_BLE_CONNECTED_NAME,    &kaos_radio_start, "KODE_EVENT_BLE_CONNECTED",    &ble_listen },
    { KODE_EVENT_BLE_DISCONNECTED_NAME, &kaos_radio_start, "KODE_EVENT_BLE_DISCONNECTED", &ble_listen },
    { KODE_EVENT_BLE_WRITTEN_NAME,      &kaos_radio_start, "KODE_EVENT_BLE_WRITTEN",      &ble_listen },
    { NULL,                      NULL,              NULL,                        NULL             }
};

/** @brief One registered listener. */
typedef struct {
    char            name[KODE_EVENT_NAME_MAX];  /**< The event, in lower case. */
    kode_listener_t fn;                         /**< What to call.             */
} kode_listener_slot_t;

/** @brief Everything the events share, one block in PSRAM. */
typedef struct {
    khal_mutex_t         lock;                       /**< Guards the fields below.                 */
    khal_sem_t           wake;                       /**< Signalled for every entry pushed.        */
    _Atomic(void *)      dispatcher;                 /**< The dispatch task, once it runs.         */
    uint32_t             seq;                        /**< Number of the last entry pushed; wraps.  */
    uint32_t             head;                       /**< Index of the oldest entry.               */
    uint32_t             count;                      /**< Entries in the ring.                     */
    uint32_t             dropped;                    /**< Events dropped for want of room.         */
    bool                 overflowing;                /**< A drop was reported and the ring has not emptied since. */
    uint32_t             listeners;                  /**< Used slots of the table.                 */
    kode_task_t         *waiting;                    /**< Tasks waiting for an event, newest first. */
    bool                 touch_down;                 /**< A finger was down at the last touch sample. */
    int16_t              touch_x;                    /**< The last touch sample's x.               */
    int16_t              touch_y;                    /**< The last touch sample's y.               */
    kode_event_copy_t    ring[KODE_EVENT_QUEUE_DEPTH];   /**< The queue.                           */
    kode_listener_slot_t table[KODE_LISTENERS_MAX];  /**< The listeners, in the order added.       */
} kode_events_t;

/** @brief The block once the dispatch task runs; NULL before. Events go live with it. */
static _Atomic(kode_events_t *) s_events;

/**
 * @brief The block a start made, kept across a failed try so a retry reuses it.
 *
 * Only the start touches it, and the starts are one at a time (khal_once_try()).
 */
static kode_events_t *s_block;

/** @brief The events block, or NULL while the layer is not up. */
static kode_events_t *events_get(void)
{
    return atomic_load(&s_events);
}

/**
 * @brief Copy @p from into @p to in lower case, cut to @p size bytes with a NUL.
 *
 * @param to   The destination.
 * @param size Bytes of @p to.
 * @param from The text; NULL copies "".
 */
static void lower_copy(char *to, size_t size, const char *from)
{
    size_t i = 0U;

    if ( NULL != from )
    {
        while ( ((size - 1U) > i) && ('\0' != from[i]) )
        {
            to[i] = kode_ascii_lower(from[i]);
            i++;
        }
    }
    to[i] = '\0';
}

/**
 * @brief Copy @p from into @p to, cut to @p size bytes with a NUL.
 *
 * @param to   The destination.
 * @param size Bytes of @p to.
 * @param from The text; NULL copies "".
 */
static void text_copy(char *to, size_t size, const char *from)
{
    size_t i = 0U;

    if ( NULL != from )
    {
        while ( ((size - 1U) > i) && ('\0' != from[i]) )
        {
            to[i] = from[i];
            i++;
        }
    }
    to[i] = '\0';
}

/**
 * @brief Whether @p name (lower case) is "<prefix><rest>"; @p rest receives
 *        where the rest begins.
 */
static bool has_prefix(const char *name, const char *prefix, const char **rest)
{
    size_t i = 0U;

    while ( ('\0' != prefix[i]) && (name[i] == prefix[i]) )
    {
        i++;
    }
    if ( '\0' == prefix[i] )
    {
        *rest = &name[i];
    }
    return '\0' == prefix[i];
}

/**
 * @brief Whether @p name (lower case) is a J3 pin's event, "pin_<n>_changed",
 *        and which pin.
 *
 * The number is written as kode.h prints it: 1 to 14 in decimal, with no
 * leading zero.
 *
 * @param name The event, in lower case.
 * @param pin  Receives the pin when the answer is true; untouched otherwise.
 * @return true for pin_1_changed to pin_14_changed.
 */
static bool pin_event_number(const char *name, uint8_t *pin)
{
    const char *rest   = NULL;
    uint32_t    n      = 0U;
    size_t      digits = 0U;
    bool        ok     = has_prefix(name, KODE_EVENT_PIN_PREFIX, &rest);

    if ( ok && ('0' == rest[0]) )
    {
        ok = false;   /* a leading zero, or pin 0 */
    }
    while ( ok && (PIN_NUMBER_DIGITS_MAX > digits) && ('0' <= rest[digits]) && ('9' >= rest[digits]) )
    {
        n = (n * PIN_NUMBER_BASE) + ((uint32_t)rest[digits] - (uint32_t)'0');
        digits++;
    }
    if ( ok )
    {
        ok = (0U < digits) && (strcmp(&rest[digits], KODE_EVENT_PIN_SUFFIX) == 0)
             && (KODE_PIN_FIRST <= n) && (KODE_PIN_LAST >= n);
    }
    if ( ok )
    {
        *pin = (uint8_t)n;
    }
    return ok;
}

/**
 * @brief Whether @p name (lower case) is an event that exists.
 *
 * "button_<name>_<what>": a button the kode.h calls know, then pressed or
 * released, or held for the top and bottom buttons alone; "pin_<n>_changed"
 * for a J3 pin; or one of the exact names in s_known.
 *
 * @param name The event, in lower case.
 * @return true when something can send it.
 */
static bool event_is_known(const char *name)
{
    bool known = false;
    const char *rest = NULL;

    if ( has_prefix(name, s_button_prefix, &rest) )
    {
        for ( uint8_t id = 0U; ((uint8_t)KDRV_BTN_COUNT > id) && !known; id++ )
        {
            const char *what = NULL;
            const char *const button = kode_button_name(id);

            if ( has_prefix(rest, button, &what) && ('_' == what[0]) )
            {
                what = &what[1];
                known = (strcmp(what, s_what_pressed) == 0) || (strcmp(what, s_what_released) == 0);
                if ( ((uint8_t)KDRV_BTN_TOP == id) || ((uint8_t)KDRV_BTN_BOT == id) )
                {
                    known = known || (strcmp(what, s_what_held) == 0);
                }
            }
        }
    }
    if ( !known )
    {
        uint8_t pin = 0U;

        known = pin_event_number(name, &pin);
    }
    for ( size_t i = 0U; (NULL != s_known[i].name) && !known; i++ )
    {
        known = (strcmp(name, s_known[i].name) == 0);
    }
    return known;
}

/**
 * @brief Whether @p name (lower case, known) can arrive: its owner is in the image.
 *
 * @param name The event, in lower case.
 * @return true unless its owner was not linked; false with the task's error set.
 */
static bool event_can_arrive(const char *name)
{
    bool can = true;

    for ( size_t i = 0U; NULL != s_known[i].name; i++ )
    {
        if ( (strcmp(name, s_known[i].name) == 0) && (NULL != s_known[i].owner)
            && !kaos_linked(s_known[i].owner) )
        {
            kode_err_set("'%s' needs its owner in this app: name it %s", name, s_known[i].link);
            can = false;
        }
    }
    return can;
}

/**
 * @brief Ask @p name's owner for what a listener or a waiter needs of it, if anything.
 *
 * Called once the layer is up, so the owner has started. A pin's event holds
 * its pin (kode_pin_listen()): each true answer for one is given back with
 * event_unlisten() once that listener or wait is over.
 *
 * @param name The event, in lower case, known and able to arrive.
 * @return true when nothing is needed or the hook asked for it; false with
 *         the task's error naming the event when the hook could not ("'<name>'
 *         cannot be heard: " and why, for a pin).
 */
static bool event_listen(const char *name)
{
    bool    asked = true;
    uint8_t pin   = 0U;

    if ( pin_event_number(name, &pin) )
    {
        asked = kode_pin_listen(pin, &kode_standard_ao_pin_arm);
        if ( !asked )
        {
            char why[KODE_ERROR_MAX];

            text_copy(why, sizeof(why), kode_error());
            kode_err_set("'%s' cannot be heard: %s", name, why);
        }
    }
    else
    {
        for ( size_t i = 0U; NULL != s_known[i].name; i++ )
        {
            if ( (strcmp(name, s_known[i].name) == 0) && (NULL != s_known[i].listen) )
            {
                asked = s_known[i].listen();
            }
        }
        if ( !asked )
        {
            kode_err_set("'%s' cannot be heard now: the standard layer is busy, try again", name);
        }
    }
    return asked;
}

/**
 * @brief Give back what event_listen() asked for @p name, when it counts:
 *        a pin's event lets go of its pin's hold, and the last one the pin.
 *
 * @param name The event, in lower case.
 */
static void event_unlisten(const char *name)
{
    uint8_t pin = 0U;

    if ( pin_event_number(name, &pin) )
    {
        kode_pin_unlisten(pin);
    }
}

/**
 * @brief Check an event name's shape and keep it in lower case.
 *
 * @param event The name the caller typed.
 * @param out   Receives it, KODE_EVENT_NAME_MAX bytes.
 * @return true when @p out holds the name; false with the task's error set.
 */
static bool name_prepare(const char *event, char *out)
{
    bool ok = false;

    if ( NULL == event )
    {
        kode_err_set("event name is NULL");
    }
    else if ( '\0' == event[0] )
    {
        kode_err_set("event name is empty");
    }
    else if ( strlen(event) >= KODE_EVENT_NAME_MAX )
    {
        kode_err_set("event name '%s' is too long (at most %u characters)",
                     event, (unsigned)(KODE_EVENT_NAME_MAX - 1U));
    }
    else
    {
        lower_copy(out, KODE_EVENT_NAME_MAX, event);
        ok = true;
    }
    return ok;
}

/**
 * @brief name_prepare(), and the name must be an event that exists.
 *
 * @param event The name the caller typed.
 * @param out   Receives it in lower case, KODE_EVENT_NAME_MAX bytes.
 * @return true when @p out holds the name; false with the task's error set
 *         ("unknown event '<as typed>'" for a well-formed name no event has,
 *         and event_can_arrive()'s words for one whose owner is not linked).
 */
static bool name_prepare_known(const char *event, char *out)
{
    bool ok = name_prepare(event, out);

    if ( ok && !event_is_known(out) )
    {
        kode_err_set("unknown event '%s'", event);
        ok = false;
    }
    if ( ok )
    {
        ok = event_can_arrive(out);
    }
    return ok;
}

/** @brief The index of the listener (@p name, @p fn) in the table; s->listeners if none. Lock held. */
static uint32_t table_find(const kode_events_t *s, const char *name, kode_listener_t fn)
{
    uint32_t found = s->listeners;

    for ( uint32_t i = 0U; (i < s->listeners) && (found == s->listeners); i++ )
    {
        if ( (s->table[i].fn == fn) && (strcmp(s->table[i].name, name) == 0) )
        {
            found = i;
        }
    }
    return found;
}

/* ---- Dispatch -------------------------------------------------------------- */

bool kode_event_is_after(uint32_t entry_seq, uint32_t start)
{
    return ((int32_t)(entry_seq - start)) > 0;
}

/**
 * @brief Hand the entry to every task waiting for its name that began
 *        waiting before the entry was pushed. Lock held.
 *
 * A task that began after the push is not served by it: the entry may have
 * sat in the ring, behind a slow listener, while the task began its wait,
 * and a wait only receives what happens after it began. Each task served
 * leaves the list; the copy goes into the task's own state.
 *
 * @param s  The block.
 * @param en The entry at the head of the ring.
 */
static void waiters_serve(kode_events_t *s, const kode_event_copy_t *en)
{
    kode_task_t **link = &s->waiting;

    while ( NULL != *link )
    {
        kode_task_t *const t = *link;

        if ( kode_name_is(en->name, t->want) && kode_event_is_after(en->seq, t->start) )
        {
            t->got     = *en;
            t->hit     = true;
            t->waiting = false;
            *link      = t->next;
            t->next    = NULL;
            khal_sem_signal(t->wait_sem);
        }
        else
        {
            link = &t->next;
        }
    }
}

/**
 * @brief Copy the functions to call for @p name, in the order they were added.
 *
 * @param s    The block.
 * @param name The event, in lower case.
 * @param fns  Receives them; KODE_LISTENERS_MAX places.
 * @return How many were copied.
 */
static size_t listeners_match(const kode_events_t *s, const char *name, kode_listener_t *fns)
{
    size_t n = 0U;

    for ( uint32_t i = 0U; i < s->listeners; i++ )
    {
        if ( strcmp(s->table[i].name, name) == 0 )
        {
            fns[n] = s->table[i].fn;
            n++;
        }
    }
    return n;
}

/** @brief Whether (@p name, @p fn) is still in the table. Takes the lock. */
static bool listener_is_registered(kode_events_t *s, const char *name, kode_listener_t fn)
{
    khal_mutex_lock(s->lock);
    const bool found = table_find(s, name, fn) < s->listeners;
    khal_mutex_unlock(s->lock);
    return found;
}

/**
 * @brief Take every entry out of the ring: serve the waiters, call the
 *        listeners, free the place.
 *
 * @param s The block.
 */
static void drain(kode_events_t *s)
{
    for ( ;; )
    {
        kode_listener_t fns[KODE_LISTENERS_MAX];

        khal_mutex_lock(s->lock);
        if ( 0U == s->count )
        {
            khal_mutex_unlock(s->lock);
            break;
        }

        /* Producers never write the head (a push writes past head + count,
         * a coalesced move is never the head), so this entry holds still
         * while its listeners run without the lock. */
        const kode_event_copy_t *const en = &s->ring[s->head];
        waiters_serve(s, en);
        const size_t n = listeners_match(s, en->name, fns);
        khal_mutex_unlock(s->lock);

        const kode_event_t ev = { en->name, en->source, en->value, en->text, en->x, en->y };

        /* Without the lock: a listener may add or remove listeners and make any kode.h call. */
        for ( size_t i = 0U; i < n; i++ )
        {
            /* A listener removed by an earlier one in this very delivery is
             * not called. */
            if ( listener_is_registered(s, en->name, fns[i]) )
            {
                fns[i](&ev);
            }
        }

        khal_mutex_lock(s->lock);
        s->head = (s->head + 1U) % KODE_EVENT_QUEUE_DEPTH;
        s->count--;
        if ( 0U == s->count )
        {
            /* The burst is over: the next drop is news again. */
            s->overflowing = false;
        }
        khal_mutex_unlock(s->lock);
    }
}

/** @brief The dispatch task: sleep until an entry is pushed, then drain. */
static void dispatch_task(void *arg)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the task parameter is the block kode_event_start() passed */
    kode_events_t *const s = (kode_events_t *)arg;

    atomic_store(&s->dispatcher, khal_task_self());
    for ( ;; )
    {
        (void)khal_sem_wait_timeout(s->wake, KODE_FOREVER);
        drain(s);
    }
}

bool kode_event_start(void)
{
    bool ok = false;

    if ( events_get() != NULL )
    {
        return true;   /* an earlier try got as far as the task */
    }
    if ( NULL == s_block )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the block is a kode_events_t */
        s_block = (kode_events_t *)khal_alloc_psram(sizeof(void *), sizeof(*s_block));
        if ( NULL != s_block )
        {
            (void)memset(s_block, 0, sizeof(*s_block));
        }
    }
    if ( NULL == s_block )
    {
        kode_err_set("no memory for the event queue (%u bytes)", (unsigned)sizeof(kode_events_t));
    }
    else
    {
        /* A mutex or a semaphore cannot be deleted, so a try that fails
         * keeps what it made and the next one makes only what is missing. */
        if ( NULL == s_block->lock )
        {
            s_block->lock = khal_mutex_create();
        }
        if ( NULL == s_block->wake )
        {
            s_block->wake = khal_sem_create();
        }
        if ( (NULL == s_block->lock) || (NULL == s_block->wake) )
        {
            kode_err_set("no memory for the event queue's locks");
        }
        else if ( khal_task_start(&dispatch_task, "kode_events", KODE_EVENT_STACK_BYTES,
                                 KODE_PRIO_STANDARD_EVENTS, s_block) != KODE_OK )
        {
            kode_err_set("no memory for the event task");
        }
        else
        {
            atomic_store(&s_events, s_block);
            ok = true;
        }
    }
    return ok;
}

/* ---- Pushing --------------------------------------------------------------- */

/**
 * @brief Put an entry at the ring's tail, numbered, with no point. Lock held.
 *
 * @param s      The block.
 * @param name   The event; kept in lower case.
 * @param source Who; NULL for none.
 * @param value  Its number.
 * @param text   Its payload; NULL for none.
 * @param lost   KODE_EVENT_NAME_MAX bytes; receives the name when this is the
 *               first drop of a burst, and is left alone otherwise.
 * @return The entry; NULL when the ring was full and the event is counted dropped.
 */
static kode_event_copy_t *entry_put(kode_events_t *s, const char *name, const char *source,
                                   int32_t value, const char *text, char *lost)
{
    kode_event_copy_t *en = NULL;

    if ( KODE_EVENT_QUEUE_DEPTH <= s->count )
    {
        s->dropped++;
        if ( !s->overflowing )
        {
            s->overflowing = true;
            lower_copy(lost, KODE_EVENT_NAME_MAX, name);
        }
    }
    else
    {
        en = &s->ring[(s->head + s->count) % KODE_EVENT_QUEUE_DEPTH];
        s->seq++;
        en->seq = s->seq;
        lower_copy(en->name, sizeof(en->name), name);
        text_copy(en->source, sizeof(en->source), source);
        text_copy(en->text, sizeof(en->text), text);
        en->value = value;
        en->x     = 0;
        en->y     = 0;
        s->count++;
    }
    return en;
}

/**
 * @brief After the lock: wake the dispatch task for an entry put, or warn of
 *        the first drop of a burst.
 *
 * @param s    The block.
 * @param put  Whether an entry went in.
 * @param lost The name entry_put() kept for the warning; "" when none is due.
 */
static void entry_after(kode_events_t *s, bool put, const char *lost)
{
    if ( put )
    {
        khal_sem_signal(s->wake);
    }
    else if ( '\0' != lost[0] )
    {
        KLOG_W(TAG, "event queue full (%u waiting): '%s' dropped, and the others "
                    "until the listeners catch up", (unsigned)KODE_EVENT_QUEUE_DEPTH, lost);
    }
    else
    {
        /* dropped in a burst already reported */
    }
}

void kode_event_push(const char *name, const char *source, int32_t value, const char *text)
{
    kode_events_t *const s = events_get();

    if ( NULL != s )
    {
        char lost[KODE_EVENT_NAME_MAX];

        lost[0] = '\0';
        khal_mutex_lock(s->lock);
        const bool put = entry_put(s, name, source, value, text, lost) != NULL;
        khal_mutex_unlock(s->lock);
        entry_after(s, put, lost);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; kode_standard_ao.c calls it */
void kode_event_push_bytes(const char *name, const char *source, const uint8_t *data, size_t len)
{
    kode_events_t *const s = events_get();

    if ( NULL != s )
    {
        char   lost[KODE_EVENT_NAME_MAX];
        size_t n = len;

        if ( (KODE_EVENT_TEXT_MAX - 1U) < n )
        {
            n = KODE_EVENT_TEXT_MAX - 1U;
        }
        if ( NULL == data )
        {
            n = 0U;
        }
        lost[0] = '\0';
        khal_mutex_lock(s->lock);
        kode_event_copy_t *const en = entry_put(s, name, source, (int32_t)n, NULL, lost);
        if ( NULL != en )
        {
            if ( 0U < n )
            {
                (void)memcpy(en->text, data, n);
            }
            en->text[n] = '\0';
        }
        khal_mutex_unlock(s->lock);
        entry_after(s, NULL != en, lost);
    }
}

/**
 * @brief Push the touch event @p name with its point.
 *
 * @param s    The block.
 * @param name One of the three touch events.
 * @param x    The point's x.
 * @param y    The point's y.
 */
static void touch_push(kode_events_t *s, const char *name, int16_t x, int16_t y)
{
    char lost[KODE_EVENT_NAME_MAX];

    lost[0] = '\0';
    khal_mutex_lock(s->lock);
    kode_event_copy_t *const en = entry_put(s, name, NULL, 0, NULL, lost);

    if ( NULL != en )
    {
        en->x = x;
        en->y = y;
    }
    khal_mutex_unlock(s->lock);
    entry_after(s, NULL != en, lost);
}

/**
 * @brief The newest touch event waiting in the ring, the head left out. Lock held.
 *
 * @param s The block.
 * @return The entry; NULL when none waits behind the head.
 */
static kode_event_copy_t *touch_newest(kode_events_t *s)
{
    /* The prefix of every touch event. */
    static const char touch_prefix[] = "touch_";
    kode_event_copy_t *found = NULL;

    for ( uint32_t k = s->count; (1U < k) && (NULL == found); k-- )
    {
        kode_event_copy_t *const en = &s->ring[(s->head + k - 1U) % KODE_EVENT_QUEUE_DEPTH];
        const char *rest = NULL;

        if ( has_prefix(en->name, touch_prefix, &rest) )
        {
            found = en;
        }
    }
    return found;
}

/**
 * @brief Push a move of the finger, or give its point to the move still waiting.
 *
 * When the newest touch event waiting behind the head is a move, that move
 * takes the point and a new number in place and nothing is pushed, so a drag
 * leaves at most one move waiting per stroke however long the listeners take.
 * When it is a press or a release, or no touch event waits, the move is pushed
 * at the tail like any other event. The head is never written: its listeners
 * may be reading it.
 *
 * @param s The block.
 * @param x The point's x.
 * @param y The point's y.
 */
static void kode_event_ring_replace_move(kode_events_t *s, int16_t x, int16_t y)
{
    khal_mutex_lock(s->lock);
    kode_event_copy_t *const en = touch_newest(s);
    const bool replace = (NULL != en) && (strcmp(en->name, KODE_EVENT_TOUCH_MOVED_NAME) == 0);

    if ( replace )
    {
        s->seq++;
        en->seq = s->seq;
        en->x   = x;
        en->y   = y;
    }
    khal_mutex_unlock(s->lock);
    if ( !replace )
    {
        touch_push(s, KODE_EVENT_TOUCH_MOVED_NAME, x, y);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; the app build's Active Object calls it, and the kodeOS image builds only its refusal */
void kode_event_push_touch(int16_t x, int16_t y, bool pressed)
{
    kode_events_t *const s = events_get();

    if ( NULL != s )
    {
        khal_mutex_lock(s->lock);
        const bool was = s->touch_down;

        s->touch_down = pressed;
        s->touch_x    = x;
        s->touch_y    = y;
        khal_mutex_unlock(s->lock);

        if ( pressed && !was )
        {
            touch_push(s, KODE_EVENT_TOUCH_PRESSED_NAME, x, y);
        }
        else if ( pressed )
        {
            kode_event_ring_replace_move(s, x, y);
        }
        else if ( was )
        {
            touch_push(s, KODE_EVENT_TOUCH_RELEASED_NAME, x, y);
        }
        else
        {
            /* a lift with no finger down: nothing to say */
        }
    }
}

bool kode_event_touch_read(int16_t *x, int16_t *y)
{
    bool down = false;
    kode_events_t *const s = events_get();

    if ( NULL != s )
    {
        khal_mutex_lock(s->lock);
        down = s->touch_down;
        if ( down && (NULL != x) )
        {
            *x = s->touch_x;
        }
        if ( down && (NULL != y) )
        {
            *y = s->touch_y;
        }
        khal_mutex_unlock(s->lock);
    }
    return down;
}

uint32_t kode_event_dropped(void)
{
    uint32_t dropped = 0U;
    kode_events_t *const s = events_get();

    if ( NULL != s )
    {
        khal_mutex_lock(s->lock);
        dropped = s->dropped;
        khal_mutex_unlock(s->lock);
    }
    return dropped;
}

/* ---- The public calls ------------------------------------------------------ */

bool kode_event_add_listener(const char *event, kode_listener_t fn)
{
    char want[KODE_EVENT_NAME_MAX];
    bool ok = false;

    if ( kode_enter("kode_event_add_listener") && name_prepare_known(event, want) )
    {
        if ( NULL == fn )
        {
            kode_err_set("listener is NULL");
        }
        else if ( !kode_standard_ao_ensure() )
        {
            /* kode_standard_ao_ensure() said why */
        }
        else if ( !event_listen(want) )
        {
            /* event_listen() said why, and nothing is registered */
        }
        else
        {
            kode_events_t *const s     = events_get();
            bool                 added = false;   /* this call registered it, and keeps the ask */

            khal_mutex_lock(s->lock);
            if ( table_find(s, want, fn) < s->listeners )
            {
                ok = true;   /* already there: once is enough */
            }
            else if ( KODE_LISTENERS_MAX > s->listeners )
            {
                kode_listener_slot_t *const slot = &s->table[s->listeners];

                text_copy(slot->name, sizeof(slot->name), want);
                slot->fn = fn;
                s->listeners++;
                ok    = true;
                added = true;
            }
            else
            {
                kode_err_set("too many listeners (at most %u)", (unsigned)KODE_LISTENERS_MAX);
            }
            khal_mutex_unlock(s->lock);
            if ( !added )
            {
                event_unlisten(want);
            }
        }
    }
    return ok;
}

bool kode_event_remove_listener(const char *event, kode_listener_t fn)
{
    char want[KODE_EVENT_NAME_MAX];
    bool ok = false;

    if ( kode_enter("kode_event_remove_listener") && name_prepare(event, want) )
    {
        kode_events_t *const s = events_get();

        if ( NULL == fn )
        {
            kode_err_set("listener is NULL");
        }
        else
        {
            if ( NULL != s )
            {
                khal_mutex_lock(s->lock);
                const uint32_t at = table_find(s, want, fn);

                if ( at < s->listeners )
                {
                    /* Close the gap: the order the others were added in stays. */
                    for ( uint32_t i = at; (i + 1U) < s->listeners; i++ )
                    {
                        s->table[i] = s->table[i + 1U];
                    }
                    s->listeners--;
                    ok = true;
                }
                khal_mutex_unlock(s->lock);
            }
            if ( ok )
            {
                event_unlisten(want);
            }
            else
            {
                kode_err_set("no listener of that function is registered for '%s'", want);
            }
        }
    }
    return ok;
}

/**
 * @brief Take the calling task off the list of waiting tasks. Lock held.
 *
 * @param s The block.
 * @param t The task; it may already be off the list.
 */
static void waiting_unlink(kode_events_t *s, kode_task_t *t)
{
    kode_task_t **link = &s->waiting;

    while ( NULL != *link )
    {
        if ( *link == t )
        {
            *link = t->next;
        }
        else
        {
            link = &(*link)->next;
        }
    }
    t->next    = NULL;
    t->waiting = false;
}

/**
 * @brief Put the task on the list and wait on its semaphore for its event.
 *
 * @param s    The block.
 * @param t    The calling task's state, with its semaphore.
 * @param want The event, in lower case.
 * @param ms   How long to wait, or KODE_FOREVER.
 * @return true when the event arrived (its copy is in @p t->got).
 */
static bool task_wait(kode_events_t *s, kode_task_t *t, const char *want, uint32_t ms)
{
    /* A token left by an event that arrived just as an earlier wait gave up. */
    (void)khal_sem_wait_timeout(t->wait_sem, 0U);

    khal_mutex_lock(s->lock);
    text_copy(t->want, sizeof(t->want), want);
    t->start   = s->seq;
    t->hit     = false;
    t->waiting = true;
    t->next    = s->waiting;
    s->waiting = t;
    khal_mutex_unlock(s->lock);

    bool signalled = khal_sem_wait_timeout(t->wait_sem, ms);

    while ( !signalled && (KODE_FOREVER == ms) )
    {
        signalled = khal_sem_wait_timeout(t->wait_sem, ms);
    }

    /* The event may have been handed over in the instant the wait gave up:
     * the flag, judged under the lock, says. */
    khal_mutex_lock(s->lock);
    const bool hit = t->hit;

    if ( t->waiting )
    {
        waiting_unlink(s, t);
    }
    khal_mutex_unlock(s->lock);
    return hit;
}

bool kode_event_wait(const char *event, uint32_t ms, kode_event_t *out)
{
    char want[KODE_EVENT_NAME_MAX];
    bool ok = false;

    if ( kode_enter("kode_event_wait") && name_prepare_known(event, want) )
    {
        kode_events_t *const s = events_get();

        if ( (NULL != s) && (atomic_load(&s->dispatcher) == khal_task_self()) )
        {
            kode_err_set("cannot wait for an event inside a listener");
        }
        else if ( !kode_standard_ao_ensure() )
        {
            /* kode_standard_ao_ensure() said why */
        }
        else if ( !event_listen(want) )
        {
            /* event_listen() said why */
        }
        else
        {
            kode_task_t *const t = kode_task_ensure();

            if ( (NULL != t) && (NULL == t->wait_sem) )
            {
                t->wait_sem = khal_sem_create();
            }
            if ( (NULL == t) || (NULL == t->wait_sem) )
            {
                kode_err_set("no memory for waiting");
            }
            else
            {
                ok = task_wait(events_get(), t, want, ms);
                if ( !ok )
                {
                    kode_err_set("no '%s' event within %u ms", want, (unsigned)ms);
                }
                else if ( NULL != out )
                {
                    out->name   = t->got.name;
                    out->source = t->got.source;
                    out->value  = t->got.value;
                    out->text   = t->got.text;
                    out->x      = t->got.x;
                    out->y      = t->got.y;
                }
                else
                {
                    /* only the fact was asked for */
                }
            }
            event_unlisten(want);
        }
    }
    return ok;
}
