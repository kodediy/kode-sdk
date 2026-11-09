/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_standard_ao.c
 * @brief The standard layer's one Active Object: owners' signals into events, and the screen.
 */
#include "kode.h"

#include <inttypes.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "kdrv_buttons.h"
#include "kdrv_wifi.h"
#include "khal_once.h"
#include "khal_system.h"
#include "kode_battery.h"
#include "kode_button.h"
#include "kode_entry.h"
#include "kode_pin_lock.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_tag.h"
#include "knfc_tag.h"
#include "kqp.h"
#include "kaos.h"

/* AO_Radio, named weakly (KAOS_WEAK, kaos.h): this AO starts in every app that
 * uses events, and reading the radio's state must not link the radio. */
KAOS_WEAK void kaos_radio_start(void);

#if CONFIG_KODEOS_BOOT_ROLE_OS

bool kode_standard_ao_ensure(void)
{
    kode_err_set("the standard layer is for apps, not kodeOS");
    return false;
}

bool kode_standard_ao_screen_attach(const kode_screen_ops_t *ops)
{
    (void)ops;
    return kode_standard_ao_ensure();
}

bool kode_standard_ao_screen_post(QEvt const *e)
{
    QF_gc(e);
    return false;
}

bool kode_standard_ao_touch_listen(kode_owner_subscribe_t subscribe)
{
    /* cppcheck-suppress misra-c2012-11.1 ; the kodeOS image subscribes nothing, so the function is unused, not converted */
    (void)subscribe;
    return kode_standard_ao_ensure();
}

bool kode_standard_ao_pin_arm(uint8_t pin)
{
    (void)pin;
    return kode_standard_ao_ensure();
}

void kode_standard_ao_power_expect(bool on, khal_sem_t done)
{
    (void)on;
    (void)done;
}

kode_status_t kode_standard_ao_power_await(uint32_t ms)
{
    (void)ms;
    return KODE_TIMEOUT;
}

bool kode_standard_ao_subscribe(kode_owner_subscribe_t subscribe)
{
    /* cppcheck-suppress misra-c2012-11.1 ; the kodeOS image subscribes nothing, so the function is unused, not converted */
    (void)subscribe;
    return kode_standard_ao_ensure();
}

#else /* an app image */

/** @brief Events the Active Object buffers: the margin, and as many screen requests. */
#define STANDARD_QUEUE_LEN (2U * KODE_SCREEN_POST_MARGIN)

_Static_assert(KODE_EVENT_SOURCE_MAX >= KODE_MQTT_TOPIC_MAX,
               "an event's source holds a whole topic");
_Static_assert(KODE_EVENT_TEXT_MAX > KODE_MQTT_PAYLOAD_MAX,
               "an event's text holds a whole message and its NUL");
_Static_assert(KODE_EVENT_TEXT_MAX > KQP_BLE_APP_VALUE_MAX,
               "an event's text holds a whole Bluetooth write and its NUL");
_Static_assert(KODE_EVENT_SOURCE_MAX >= KQP_BLE_APP_NAME_MAX,
               "an event's source holds a whole characteristic's name");

/** @brief Task name of the Active Object. */
#define STANDARD_AO_NAME "kode_standard"

/** @brief The Active Object's state. */
typedef struct {
    QActive                  super;                              /**< The Active Object. */
    bool                     wifi_up;                            /**< The radio had an address at the last WIFI_STATE_SIG. */
    char                     wifi_ssid[KODE_SYS_WIFI_SSID_MAX];  /**< The network it had, for the event that says it is lost. */
    bool                     battery_low;                        /**< The level fell below the mark and has not recovered. */
    bool                     touch_on;                           /**< Subscribed to the finger: its samples become events. */
    const kode_screen_ops_t *screen;                             /**< The screen's table; NULL until attached. */
    uint16_t                 pin_settling;                       /**< One bit per pin whose settle is armed. */
    uint16_t                 pin_level;                          /**< One bit per pin: the level last pushed, or the one it started at. */
} KodeStandard;

_Static_assert(KODE_PIN_COUNT <= (sizeof(uint16_t) * (size_t)CHAR_BIT),
               "one bit per pin fits pin_settling and pin_level");

/** @brief The screen's table on its way (KODE_SCREEN_ATTACH_SIG). */
typedef struct {
    QEvt                     super;   /**< The event.  */
    const kode_screen_ops_t *ops;     /**< The table.  */
} ScreenAttachEvt;

_Static_assert(sizeof(ScreenAttachEvt) <= sizeof(TouchEvt),
               "ScreenAttachEvt must fit the small pool");

/** @brief The request to subscribe to the finger (KODE_TOUCH_LISTEN_SIG). */
typedef struct {
    QEvt                   super;       /**< The event.                */
    kode_owner_subscribe_t subscribe;   /**< AO_Touch's touch_subscribe(). */
} TouchListenEvt;

_Static_assert(sizeof(TouchListenEvt) <= sizeof(TouchEvt),
               "TouchListenEvt must fit the small pool");

/** @brief A listened pin moved (KODE_PIN_EDGE_SIG): one static event per pin. */
typedef struct {
    /* cppcheck-suppress unusedStructMember ; QP's event header: QP reads it through the QEvt pointer the interrupt posts */
    QEvt    super;   /**< The event.                       */
    uint8_t pin;     /**< The pin, KODE_PIN_FIRST to _LAST. */
} PinEdgeEvt;

/** @brief A pin listened to from now on (KODE_PIN_LISTEN_SIG). */
typedef struct {
    QEvt    super;   /**< The event.                      */
    uint8_t pin;     /**< The pin.                        */
    bool    level;   /**< The level it starts at.         */
} PinListenEvt;

_Static_assert(sizeof(PinListenEvt) <= sizeof(TouchEvt),
               "PinListenEvt must fit the small pool");

/** @brief What one try of kode_standard_ao_pin_arm() posts. */
typedef struct {
    uint8_t pin;     /**< The pin.                        */
    bool    level;   /**< The level it starts at.         */
} pin_listen_try_t;

/** @brief Each pin's edge, the static event its interrupt posts; in flash, as the interrupt runs with the cache on. */
/* cppcheck-suppress misra-c2012-8.9 ; also sized by the _Static_assert below it, at file scope, so one function is not its only user */
static const PinEdgeEvt s_pin_edge[] = {
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 1U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 2U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 3U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 4U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 5U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 6U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 7U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 8U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 9U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 10U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 11U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 12U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 13U },
    { QEVT_INITIALIZER(KODE_PIN_EDGE_SIG), 14U },
};

_Static_assert((sizeof(s_pin_edge) / sizeof(s_pin_edge[0])) == KODE_PIN_COUNT,
               "s_pin_edge holds one event per pin");

/** @brief What one try of kode_standard_ao_screen_attach() posts. */
typedef struct {
    const kode_screen_ops_t *ops;     /**< The table.  */
} attach_try_t;

/** @brief The Active Object; constructed by ao_start(). */
static KodeStandard l_standard;

QActive * const AO_KodeStandard = &l_standard.super;

/** @brief Stack bytes, LVGL's included; PSRAM is legal: nothing on this thread writes flash. */
#define STANDARD_STACK_BYTES KQP_AO_STACK_BYTES

/** @brief Alignment of the queue, the stack and the pins' settles in PSRAM; the RISC-V stack's. */
#define STANDARD_ALLOC_ALIGN 16U

/**
 * @brief The Active Object's event queue; PSRAM, taken by the first start.
 *
 * It only copies names into the ring and logs, so it never needs the
 * internal RAM the owners' stacks keep for a flash write. A start that fails
 * keeps it and the next try reuses it.
 */
static QEvtPtr *s_queue;

/** @brief The Active Object's stack; PSRAM, kept the way s_queue is. */
static StackType_t *s_stack;

/**
 * @brief Each pin's settle, by pin less KODE_PIN_FIRST: PSRAM, taken and
 *        constructed by the first pin listen (pin_settle_get()), NULL until then.
 *
 * Written once, under the pins lock and before any pin's interrupt is
 * attached; the Active Object reads it only for a pin that is listened to,
 * so an app that never listens to a pin keeps the timers out of internal RAM.
 */
static QTimeEvt *s_pin_settle;

/** @brief The one-shot the first-use start is elected by. */
static khal_once_t s_once;

/** @brief No kode_pin_power_3v3() waits (s_power_wait). */
/* cppcheck-suppress misra-c2012-2.5 ; s_power_wait's value, used by the app build; the kodeOS image builds only the refusals */
#define POWER_WAIT_NONE 0U

/** @brief A kode_pin_power_3v3() waits for J3's supply to be off. */
/* cppcheck-suppress misra-c2012-2.5 ; s_power_wait's value, used by the app build; the kodeOS image builds only the refusals */
#define POWER_WAIT_OFF  1U

/** @brief A kode_pin_power_3v3() waits for J3's supply to be on. */
/* cppcheck-suppress misra-c2012-2.5 ; s_power_wait's value, used by the app build; the kodeOS image builds only the refusals */
#define POWER_WAIT_ON   2U

/**
 * @brief The one wait for J3's supply: POWER_WAIT_NONE, _OFF or _ON.
 *
 * Whoever takes it back to POWER_WAIT_NONE owns the end: the Active Object,
 * which then signals s_power_done, or the waiter giving up, after which the
 * Active Object touches nothing.
 */
static atomic_uint s_power_wait;

/** @brief The waiter's semaphore; set before s_power_wait is armed. */
static khal_sem_t s_power_done;

/** @brief The end's code the Active Object took (a kode_status_t); written before s_power_done is signalled. */
static atomic_uint s_power_rc;

static QState KodeStandard_initial(KodeStandard * const me, void const * const par);
static QState KodeStandard_running(KodeStandard * const me, QEvt const * const e);

/**
 * @brief Push "button_<name>_<what>" for button @p id.
 *
 * @param id   A KODE_SYS_BUTTON_x.
 * @param what "pressed", "released" or "held".
 */
static void button_event(uint8_t id, const char *what)
{
    const char *const button = kode_button_name(id);

    if ( NULL != button )
    {
        char name[KODE_EVENT_NAME_MAX];

        (void)snprintf(name, sizeof(name), "button_%s_%s", button, what);
        kode_event_push(name, button, 0, NULL);
    }
}

/** @brief Bytes of "0x" and four hex digits of a NEC address, NUL included. */
#define IR_ADDR_TEXT_BYTES 7U

/** @brief The source of a frame that did not decode as NEC. */
#define IR_SOURCE_UNKNOWN "unknown"

/** @brief The source of a NEC frame. */
#define IR_SOURCE_NEC "NEC"

/** @brief Characters a UID byte takes in the text: two digits and a colon. */
#define NFC_UID_TEXT_PER_BYTE 3U

/** @brief Bytes of the longest UID as "AA:BB:...", NUL included. */
#define NFC_UID_TEXT_BYTES (KQP_NFC_UID_MAX * NFC_UID_TEXT_PER_BYTE)

/**
 * @brief Push "wifi_connected" or "wifi_lost" when the address appears or goes.
 * @param me The Active Object.
 * @param w  The state the radio published.
 */
static void wifi_edge(KodeStandard * const me, const WifiStateEvt *w)
{
    const bool up = KODE_SYS_WIFI_STATE_GOT_IP == w->state;

    if ( up )
    {
        (void)snprintf(me->wifi_ssid, sizeof(me->wifi_ssid), "%s", w->ssid);
        if ( !me->wifi_up )
        {
            kode_event_push(KODE_EVENT_WIFI_UP_NAME, me->wifi_ssid, 0, NULL);
        }
    }
    else if ( me->wifi_up )
    {
        kode_event_push(KODE_EVENT_WIFI_LOST_NAME, me->wifi_ssid, 0, NULL);
    }
    else
    {
        /* still without an address */
    }
    me->wifi_up = up;
}

/**
 * @brief Push a broker's message as an "mqtt_message" event.
 * @param m The message AO_Net published.
 */
static void mqtt_message(const MqttMsgEvt *m)
{
    char     text[KODE_MQTT_PAYLOAD_MAX + 1U];
    uint32_t len = m->len;

    if ( KODE_MQTT_PAYLOAD_MAX < len )
    {
        len = KODE_MQTT_PAYLOAD_MAX;
    }
    (void)memcpy(text, m->data, len);
    text[len] = '\0';
    kode_event_push("mqtt_message", m->topic, (int32_t)len, text);
}

/**
 * @brief Push "battery_low" on a fall below the mark.
 * @param me The Active Object.
 */
static void battery_edge(KodeStandard * const me)
{
    kode_sys_battery_t r;

    if ( (kode_sys_battery_read(&r) == KODE_OK) && kode_battery_low_edge(&me->battery_low, &r) )
    {
        kode_event_push("battery_low", NULL, (int32_t)r.soc_pct, NULL);
    }
}

/**
 * @brief Take the radio's address as it is now, without an event.
 * @param me The Active Object.
 */
static void wifi_seed(KodeStandard * const me)
{
    kode_sys_wifi_status_t st = { 0U, 0U, "" };

    /* With no AO_Radio there is no Wi-Fi: off, and nothing to read. */
    me->wifi_up = kaos_linked(&kaos_radio_start)
                  && (kode_sys_wifi_status(&st) == KODE_OK)
                  && (KODE_SYS_WIFI_STATE_GOT_IP == st.state);
    if ( me->wifi_up )
    {
        (void)snprintf(me->wifi_ssid, sizeof(me->wifi_ssid), "%s", st.ssid);
    }
}

/**
 * @brief Push "ir_received" for a frame the receiver heard.
 *
 * @param ie The frame.
 */
static void ir_event(const IrRxEvt *ie)
{
    if ( ie->nec )
    {
        char text[IR_ADDR_TEXT_BYTES];

        (void)snprintf(text, sizeof(text), "0x%04" PRIX32, (uint32_t)ie->addr);
        kode_event_push(KODE_EVENT_IR_NAME, IR_SOURCE_NEC, (int32_t)ie->cmd, text);
    }
    else
    {
        kode_event_push(KODE_EVENT_IR_NAME, IR_SOURCE_UNKNOWN, 0, "");
    }
}

/**
 * @brief Push "nfc_tag" for a tag the poller activated.
 *
 * @param te The tag.
 */
static void nfc_event(const NfcTagEvt *te)
{
    char     text[NFC_UID_TEXT_BYTES];
    uint32_t n = te->uid_len;

    if ( KQP_NFC_UID_MAX < n )
    {
        n = KQP_NFC_UID_MAX;
    }
    if ( !kode_tag_hex_format(te->uid, n, text, (uint32_t)sizeof(text)) )
    {
        text[0] = '\0';
    }
    kode_event_push(KODE_EVENT_NFC_TAG, knfc_tag_model(te->atqa, te->sak), 0, text);
}

/** @brief The bit of @p pin in pin_settling and pin_level; @p pin is on the connector. */
static uint16_t pin_book_bit(uint8_t pin)
{
    const uint16_t pin_first_bit = 1U;

    return (uint16_t)(pin_first_bit << (uint8_t)(pin - KODE_PIN_FIRST));
}

/**
 * @brief A listened pin moved: arm its settle, unless it is armed already.
 *
 * An edge from a pin nothing listens to any more arms nothing: its interrupt
 * stays off, as the GPIO driver left it.
 *
 * @param me  The Active Object.
 * @param pin The pin.
 */
static void pin_edge(KodeStandard * const me, uint8_t pin)
{
    const uint16_t bit = pin_book_bit(pin);

    if ( kode_pin_is_listened(pin) && (NULL != s_pin_settle)
        && ((me->pin_settling & bit) == 0U) )
    {
        me->pin_settling |= bit;
        QTimeEvt_armX(&s_pin_settle[pin - KODE_PIN_FIRST],
                      KQP_TICKS_PER_MS(KODE_PIN_SETTLE_MS), 0U);
    }
}

/**
 * @brief A pin's settle is over: re-arm its interrupt, read it, and push the
 *        level when it is not the last one pushed.
 *
 * @param me The Active Object.
 * @param e  The settle that fired, one of s_pin_settle.
 */
static void pin_settled(KodeStandard * const me, QEvt const * const e)
{
    for ( uint8_t i = 0U; (NULL != s_pin_settle) && ((uint8_t)KODE_PIN_COUNT > i); i++ )
    {
        if ( e == &s_pin_settle[i].super )
        {
            const uint8_t  pin = (uint8_t)(i + KODE_PIN_FIRST);
            const uint16_t bit = pin_book_bit(pin);

            me->pin_settling &= (uint16_t)~bit;
            if ( kode_pin_is_listened(pin) )
            {
                kode_sys_exp_irq_enable(pin);   /* first: an edge after the read is seen */

                const bool level = kode_sys_exp_level(pin);
                const bool last  = (me->pin_level & bit) != 0U;

                if ( level != last )
                {
                    char    name[KODE_EVENT_NAME_MAX];
                    int32_t value = 0;

                    me->pin_level ^= bit;
                    if ( level )
                    {
                        value = 1;
                    }
                    (void)snprintf(name, sizeof(name), "%s%u%s", KODE_EVENT_PIN_PREFIX,
                                   (unsigned)pin, KODE_EVENT_PIN_SUFFIX);
                    kode_event_push(name, NULL, value, NULL);
                }
            }
        }
    }
}

/**
 * @brief A pin is listened to from now on: keep the level it starts at.
 *
 * @param me The Active Object.
 * @param pe The request.
 */
static void pin_listen(KodeStandard * const me, const PinListenEvt *pe)
{
    if ( (KODE_PIN_FIRST <= pe->pin) && (KODE_PIN_LAST >= pe->pin) )
    {
        const uint16_t bit = pin_book_bit(pe->pin);

        if ( pe->level )
        {
            me->pin_level |= bit;
        }
        else
        {
            me->pin_level &= (uint16_t)~bit;
        }
    }
}

/**
 * @brief A switch of J3's supply is over: end the wait that names it, if any.
 *
 * @param d What AO_Radio published.
 */
static void power_done(const ExpPowerDoneEvt *d)
{
    unsigned want = POWER_WAIT_OFF;

    if ( d->on )
    {
        want = POWER_WAIT_ON;
    }
    if ( atomic_compare_exchange_strong(&s_power_wait, &want, POWER_WAIT_NONE) )
    {
        atomic_store(&s_power_rc, (unsigned)d->rc);
        khal_sem_signal(s_power_done);
    }
}

/**
 * @brief Push "ble_written" for a write a phone made to the app's service.
 *
 * @param w The write's copy.
 */
static void ble_written_event(const BleAppWriteEvt *w)
{
    size_t n = w->len;

    if ( sizeof(w->data) < n )
    {
        n = sizeof(w->data);
    }
    kode_event_push_bytes(KODE_EVENT_BLE_WRITTEN_NAME, w->name, w->data, n);
}

/**
 * @brief Push "ble_connected" or "ble_disconnected" for an edge of the link.
 *
 * @param l The edge.
 */
static void ble_link_event(const BleAppLinkEvt *l)
{
    if ( l->connected )
    {
        kode_event_push(KODE_EVENT_BLE_CONNECTED_NAME, NULL, 0, NULL);
    }
    else
    {
        kode_event_push(KODE_EVENT_BLE_DISCONNECTED_NAME, NULL, 0, NULL);
    }
}

/**
 * @brief Hand one of the screen's signals to its table.
 *
 * @param me The Active Object.
 * @param e  The event.
 * @return Q_HANDLED() when the table took it, Q_SUPER() otherwise.
 */
static QState screen_forward(KodeStandard * const me, QEvt const * const e)
{
    QState status;

    if ( (NULL != me->screen) && me->screen->event(e) )
    {
        status = Q_HANDLED();
    }
    else
    {
        status = Q_SUPER(&QHsm_top);
    }
    return status;
}

/**
 * @brief The initial transition: subscribe to the buttons, the radio, the
 *        broker, the battery, the infrared receiver, the NFC poller and the
 *        end of a switch of J3's supply, seed the address, then run.
 *
 * @param me  The Active Object.
 * @param par Unused.
 * @return The transition to KodeStandard_running.
 */
static QState KodeStandard_initial(KodeStandard * const me, void const * const par)
{
    (void)par;
    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    QActive_subscribe(&me->super, BUTTON_HOLD_SIG);
    QActive_subscribe(&me->super, WIFI_STATE_SIG);
    QActive_subscribe(&me->super, MQTT_MSG_SIG);
    QActive_subscribe(&me->super, POWER_EVT_SIG);
    QActive_subscribe(&me->super, IR_RX_SIG);
    QActive_subscribe(&me->super, NFC_TAG_SIG);
    QActive_subscribe(&me->super, EXP_POWER_SIG);
    wifi_seed(me);
    return Q_TRAN(&KodeStandard_running);
}

/**
 * @brief The one state: each button edge, each hold, each change of the
 *        radio's address, each message from the broker, each fall of the
 *        battery below the mark, each infrared frame, each tag, once asked
 *        each touch sample, and each settled change of a listened J3 pin
 *        becomes an event, the end of a switch of J3's supply ends the wait
 *        for it, and the screen's signals go to its table, every touch
 *        sample included.
 *
 * The table's attach() subscribes to what it draws from (kscreen_start()),
 * so nothing reaches the screen before it is there.
 *
 * @param me The Active Object.
 * @param e  The event.
 * @return Q_HANDLED() for what it hears, Q_SUPER() otherwise.
 */
static QState KodeStandard_running(KodeStandard * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case KODE_SCREEN_ATTACH_SIG: {
            if ( NULL == me->screen )
            {
                me->screen = Q_EVT_CAST(ScreenAttachEvt)->ops;
                me->screen->attach(&me->super);
            }
            status = Q_HANDLED();
            break;
        }
        case KODE_TOUCH_LISTEN_SIG: {
            Q_EVT_CAST(TouchListenEvt)->subscribe(&me->super);
            me->touch_on = true;
            status = Q_HANDLED();
            break;
        }
        case KODE_PIN_EDGE_SIG: {
            const uint8_t pin = Q_EVT_CAST(PinEdgeEvt)->pin;

            if ( (KODE_PIN_FIRST <= pin) && (KODE_PIN_LAST >= pin) )
            {
                pin_edge(me, pin);
            }
            status = Q_HANDLED();
            break;
        }
        case KODE_PIN_SETTLE_SIG: {
            pin_settled(me, e);
            status = Q_HANDLED();
            break;
        }
        case KODE_PIN_LISTEN_SIG: {
            pin_listen(me, Q_EVT_CAST(PinListenEvt));
            status = Q_HANDLED();
            break;
        }
        case TOUCH_EVT_SIG: {
            /* The event first; the screen gets every sample. */
            if ( me->touch_on )
            {
                const TouchEvt *const t = Q_EVT_CAST(TouchEvt);

                kode_event_push_touch(t->x, t->y, t->pressed);
            }
            status = screen_forward(me, e);
            break;
        }
        case DISPLAY_STATE_SIG:
        case DISPLAY_REPAINT_SIG:
        case KODE_SCREEN_TICK_SIG:
        case KODE_SCREEN_REQ_SIG: {
            status = screen_forward(me, e);
            break;
        }
        case BUTTON_EVT_SIG: {
            const ButtonEvt *const be = Q_EVT_CAST(ButtonEvt);

            if ( be->pressed )
            {
                button_event(be->id, "pressed");
            }
            else
            {
                button_event(be->id, "released");
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTON_HOLD_SIG: {
            button_event(Q_EVT_CAST(ButtonHoldEvt)->id, "held");
            status = Q_HANDLED();
            break;
        }
        case WIFI_STATE_SIG: {
            wifi_edge(me, Q_EVT_CAST(WifiStateEvt));
            status = Q_HANDLED();
            break;
        }
        case MQTT_MSG_SIG: {
            mqtt_message(Q_EVT_CAST(MqttMsgEvt));
            status = Q_HANDLED();
            break;
        }
        case POWER_EVT_SIG: {
            battery_edge(me);
            status = Q_HANDLED();
            break;
        }
        case IR_RX_SIG: {
            ir_event(Q_EVT_CAST(IrRxEvt));
            status = Q_HANDLED();
            break;
        }
        case NFC_TAG_SIG: {
            nfc_event(Q_EVT_CAST(NfcTagEvt));
            status = Q_HANDLED();
            break;
        }
        case EXP_POWER_SIG: {
            power_done(Q_EVT_CAST(ExpPowerDoneEvt));
            status = Q_HANDLED();
            break;
        }
        case BLE_APP_WRITE_SIG: {
            ble_written_event(Q_EVT_CAST(BleAppWriteEvt));
            status = Q_HANDLED();
            break;
        }
        case BLE_APP_LINK_SIG: {
            ble_link_event(Q_EVT_CAST(BleAppLinkEvt));
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

/**
 * @brief Take the queue and the stack from PSRAM, once.
 *
 * @return true when both exist; false with the task's error set.
 */
static bool storage_get(void)
{
    if ( NULL == s_queue )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the queue is an array of QEvtPtr */
        s_queue = (QEvtPtr *)khal_alloc_psram(STANDARD_ALLOC_ALIGN,
                                              STANDARD_QUEUE_LEN * sizeof(QEvtPtr));
    }
    if ( NULL == s_stack )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the stack is an array of StackType_t */
        s_stack = (StackType_t *)khal_alloc_psram(STANDARD_ALLOC_ALIGN, STANDARD_STACK_BYTES);
    }
    if ( (NULL == s_queue) || (NULL == s_stack) )
    {
        kode_err_set("no memory for the standard layer's Active Object");
    }
    return (NULL != s_queue) && (NULL != s_stack);
}

/** @brief Construct the Active Object and start it. */
static void ao_start(void)
{
    QActive_ctor(&l_standard.super, Q_STATE_CAST(&KodeStandard_initial));
    l_standard.pin_settling = 0U;
    l_standard.pin_level    = 0U;
    l_standard.screen   = NULL;
    l_standard.touch_on = false;
    QActive_setAttr(&l_standard.super, TASK_NAME_ATTR, STANDARD_AO_NAME);
    QACTIVE_START(&l_standard.super, KODE_PRIO_USER_GUI,
                  s_queue, STANDARD_QUEUE_LEN,
                  s_stack, STANDARD_STACK_BYTES,
                  (void *)0);
}

/**
 * @brief The start itself: what khal_once_try() runs on the winner's thread.
 *
 * @return true when the dispatch task and the Active Object run; false with
 *         the task's error set, and nothing of the Active Object started.
 */
static bool standard_start(void)
{
    const char *holder = NULL;
    bool        ok     = false;

    /* Asked first: QP stops the run when a level is taken twice. */
    if ( kqp_prio_taken(KODE_PRIO_USER_GUI, &holder) )
    {
        kode_err_set("QP level %u is taken by another Active Object (%s)",
                     (unsigned)KODE_PRIO_USER_GUI, holder);
    }
    else if ( !storage_get() )
    {
        /* storage_get() said why */
    }
    else if ( kode_event_start() )
    {
        ao_start();
        ok = true;
    }
    else
    {
        /* kode_event_start() said why */
    }
    return ok;
}

bool kode_standard_ao_ensure(void)
{
    return khal_once_try(&s_once, &standard_start);
}

/** @brief One try of an attach: true when the table's event was posted. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_retry() takes */
static bool attach_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the attach_try_t kode_standard_ao_screen_attach() passed */
    const attach_try_t *const t = (const attach_try_t *)ctx;
    ScreenAttachEvt *const e = Q_NEW_X(ScreenAttachEvt, KODE_SCREEN_POOL_MARGIN,
                                       KODE_SCREEN_ATTACH_SIG);
    bool posted = false;

    if ( NULL != e )
    {
        e->ops = t->ops;
        posted = kode_standard_ao_screen_post(&e->super);
    }
    return posted;
}

bool kode_standard_ao_screen_attach(const kode_screen_ops_t *ops)
{
    bool ok = kode_standard_ao_ensure();

    if ( ok )
    {
        attach_try_t t = { ops };

        ok = kode_retry(&attach_attempt, &t, KODE_SCREEN_BUSY_MS);
        if ( !ok )
        {
            kode_err_set("the screen is busy");
        }
    }
    return ok;
}

bool kode_standard_ao_screen_post(QEvt const *e)
{
    return QACTIVE_POST_X(&l_standard.super, e, KODE_SCREEN_POST_MARGIN, NULL);
}

/** @brief One try of a touch listen: true when the request was posted. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_retry() takes */
static bool touch_listen_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the subscribe kode_standard_ao_touch_listen() passed */
    const kode_owner_subscribe_t *const subscribe = (const kode_owner_subscribe_t *)ctx;
    TouchListenEvt *const e = Q_NEW_X(TouchListenEvt, KODE_SCREEN_POOL_MARGIN,
                                      KODE_TOUCH_LISTEN_SIG);
    bool posted = false;

    if ( NULL != e )
    {
        e->subscribe = *subscribe;
        posted = kode_standard_ao_screen_post(&e->super);
    }
    return posted;
}

bool kode_standard_ao_subscribe(kode_owner_subscribe_t subscribe)
{
    subscribe(&l_standard.super);
    return true;
}

bool kode_standard_ao_touch_listen(kode_owner_subscribe_t subscribe)
{
    /* The request to subscribe to the finger is in the Active Object's queue.
     * Set only after a post went through, never before: a caller that finds
     * it clear posts a request of its own, so no caller answers true while
     * the only request is still on its way and may yet fail. Two callers at
     * once may both post; subscribing twice is harmless. */
    static atomic_bool s_touch_posted;
    bool ok = atomic_load(&s_touch_posted);

    if ( !ok )
    {
        kode_owner_subscribe_t fn = subscribe;

        ok = kode_retry(&touch_listen_attempt, &fn, KODE_TOUCH_LISTEN_BOUND_MS);
        if ( ok )
        {
            atomic_store(&s_touch_posted, true);
        }
    }
    return ok;
}

/**
 * @brief A listened pin's interrupt: post the pin's static event, nothing else.
 *
 * Runs in interrupt context, with the pin's interrupt already disabled by the
 * GPIO driver; KODE_PIN_SETTLE_SIG re-arms it.
 *
 * @param arg The pin's entry of s_pin_edge.
 */
static void pin_edge_isr(void *arg)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the argument is the pin's PinEdgeEvt kode_standard_ao_pin_arm() attached */
    kqp_post_from_isr(AO_KodeStandard, (QEvt const *)arg);
}

/** @brief One try of a pin arm: true when the level was posted. */
/* cppcheck-suppress constParameterCallback ; the signature is the one kode_retry() takes */
static bool pin_listen_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the pin_listen_try_t kode_standard_ao_pin_arm() passed */
    const pin_listen_try_t *const t = (const pin_listen_try_t *)ctx;
    PinListenEvt *const e = Q_NEW_X(PinListenEvt, KODE_SCREEN_POOL_MARGIN, KODE_PIN_LISTEN_SIG);
    bool posted = false;

    if ( NULL != e )
    {
        e->pin   = t->pin;
        e->level = t->level;
        posted   = kode_standard_ao_screen_post(&e->super);
    }
    return posted;
}

/**
 * @brief Take the pins' settles from PSRAM and construct them, once: the
 *        first pin listen's, under the pins lock.
 *
 * @return true when they exist; false with the task's error set.
 */
static bool pin_settle_get(void)
{
    if ( NULL == s_pin_settle )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; the allocator returns void *; the settles are an array of QTimeEvt */
        QTimeEvt *const t = (QTimeEvt *)khal_alloc_psram(STANDARD_ALLOC_ALIGN,
                                                         KODE_PIN_COUNT * sizeof(QTimeEvt));

        if ( NULL == t )
        {
            kode_err_set("no memory for the pins' settle timers");
        }
        else
        {
            for ( uint32_t i = 0U; KODE_PIN_COUNT > i; i++ )
            {
                QTimeEvt_ctorX(&t[i], &l_standard.super, KODE_PIN_SETTLE_SIG, 0U);
            }
            s_pin_settle = t;
        }
    }
    return NULL != s_pin_settle;
}

bool kode_standard_ao_pin_arm(uint8_t pin)
{
    bool ok   = (KODE_PIN_FIRST <= pin) && (KODE_PIN_LAST >= pin);
    bool room = true;

    if ( ok )
    {
        room = pin_settle_get();
        ok   = room;
    }
    if ( ok )
    {
        /* cppcheck-suppress misra-c2012-11.8 ; the GPIO driver's callback argument is a plain pointer, and pin_edge_isr() hands it back to kqp_post_from_isr() as const */
        void *const edge = (void *)&s_pin_edge[pin - KODE_PIN_FIRST];

        ok = kode_sys_exp_irq(pin, KODE_SYS_GPIO_IRQ_ANY_EDGE, &pin_edge_isr, edge) == KODE_OK;
    }
    if ( !room )
    {
        /* pin_settle_get() said why */
    }
    else if ( !ok )
    {
        kode_err_set("pin %u's interrupt could not be attached", (unsigned)pin);
    }
    else
    {
        kode_sys_exp_irq_enable(pin);   /* first: an edge after the read is seen */

        pin_listen_try_t t = { pin, kode_sys_exp_level(pin) };

        ok = kode_retry(&pin_listen_attempt, &t, KODE_PIN_LISTEN_BOUND_MS);
        if ( !ok )
        {
            kode_err_set("the standard layer is busy, try again");
        }
    }
    return ok;
}

void kode_standard_ao_power_expect(bool on, khal_sem_t done)
{
    unsigned wait = POWER_WAIT_OFF;

    if ( on )
    {
        wait = POWER_WAIT_ON;
    }
    s_power_done = done;
    atomic_store(&s_power_rc, (unsigned)KODE_TIMEOUT);
    atomic_store(&s_power_wait, wait);
}

kode_status_t kode_standard_ao_power_await(uint32_t ms)
{
    kode_status_t     end = KODE_TIMEOUT;
    bool              got = false;

    if ( 0U < ms )
    {
        got = khal_sem_wait_timeout(s_power_done, ms);
    }
    if ( !got )
    {
        const unsigned left = atomic_exchange(&s_power_wait, POWER_WAIT_NONE);

        /* NONE: the Active Object took the end as the time ran out, so its
         * signal is certain and comes next. */
        while ( (POWER_WAIT_NONE == left) && !got )
        {
            got = khal_sem_wait_timeout(s_power_done, KODE_FOREVER);
        }
    }
    if ( got )
    {
        const unsigned rc = atomic_load(&s_power_rc);

        /* The three codes AO_Radio says, by name: no cast makes an enum. */
        if ( (unsigned)KODE_OK == rc )
        {
            end = KODE_OK;
        }
        else if ( (unsigned)KODE_IO_ERROR == rc )
        {
            end = KODE_IO_ERROR;
        }
        else
        {
            end = KODE_DENIED;
        }
    }
    return end;
}

#endif /* CONFIG_KODEOS_BOOT_ROLE_OS */
