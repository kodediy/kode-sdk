/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file mqtt_ao.c
 * @brief An MQTT session that AO_Net owns and this AO drives with posts.
 */
#include "mqtt_ao.h"

#include <stdio.h>
#include <string.h>

#define TAG "mqtt"

/** The broker: HiveMQ's public one, reachable from the bench. Plain TCP, so
 *  the port is 1883; mqtts:// and 8883 is the encrypted pair. */
#define MD_BROKER_URI     "mqtt://broker.hivemq.com:1883"

/** The front of both topics. The board's serial follows and then the
 *  direction, and the serial is what makes a topic on a shared broker this
 *  board's own. */
#define MD_TOPIC_ROOT     "kode/example/"

/** The level of the colour showing. Dim on purpose - this LED is bright. */
#define MD_LEVEL          ((uint8_t)32U)

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive super;
    char    topic_in[KODE_SYS_MQTT_TOPIC_MAX];   /**< This board listens on. */
    char    topic_out[KODE_SYS_MQTT_TOPIC_MAX];  /**< Its buttons publish to. */
} MqttDemo;

/* The two arrays kodeOS declares for every AO of its own; buttons_led_ao.c
 * says what each number decides. The queue's depth follows kode_sys.h's one
 * queue rule ("Queue depth"): here no request, a button's press and release,
 * and the Wi-Fi, MQTT state and message signals. The defaults are enough
 * because this AO outranks AO_Net and never blocks, so a burst from the
 * broker is drained message by message. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static MqttDemo l_ao;
QActive * const AO_MqttDemo = &l_ao.super;

/** What each button publishes, by KODE_SYS_BUTTON_x. A press of a button that
 *  has no name here (there is none today) would publish nothing. */
static char const * const MD_BUTTON_NAMES[KODE_SYS_BUTTON_COUNT] = {
    [KODE_SYS_BUTTON_UP]    = "up",
    [KODE_SYS_BUTTON_DOWN]  = "down",
    [KODE_SYS_BUTTON_LEFT]  = "left",
    [KODE_SYS_BUTTON_RIGHT] = "right",
    [KODE_SYS_BUTTON_TOP]   = "top",
    [KODE_SYS_BUTTON_BOT]   = "bottom",
};

/** The words for MqttStateEvt.state, by KODE_SYS_MQTT_STATE_x. */
static char const * const MD_STATES[] = {
    [KODE_SYS_MQTT_STATE_DOWN]       = "down",
    [KODE_SYS_MQTT_STATE_CONNECTING] = "connecting",
    [KODE_SYS_MQTT_STATE_UP]         = "up",
};

/** The words for MqttStateEvt.err, by KODE_SYS_MQTT_ERR_x. */
static char const * const MD_WHY[] = {
    [KODE_SYS_MQTT_ERR_NONE]    = "no error",
    [KODE_SYS_MQTT_ERR_NO_NET]  = "no network",
    [KODE_SYS_MQTT_ERR_REFUSED] = "refused",
    [KODE_SYS_MQTT_ERR_TIMEOUT] = "timeout",
    [KODE_SYS_MQTT_ERR_BUSY]    = "the Kode channel holds it",
};

/* No step here waits: kode.h's kode_mqtt_*() wait for the broker, so they
 * belong to a task. */
static QState MqttDemo_initial(MqttDemo * const me, void const * const par);
static QState MqttDemo_offline(MqttDemo * const me, QEvt const * const e);
static QState MqttDemo_connected(MqttDemo * const me, QEvt const * const e);

/**
 * @brief Copy bytes into a string that is safe to log.
 *
 * A payload comes from whoever can reach the broker, and a log line is what
 * the console protocol is made of: a payload with a newline in it could write
 * lines of its own. Everything that is not printable ASCII becomes a dot, and
 * the copy is cut to fit and always terminated.
 *
 * @param out Where the string goes.
 * @param cap The size of @p out, terminator included.
 * @param in  The bytes, not necessarily terminated.
 * @param len How many of them there are.
 */
static void printable(char * const out, size_t const cap,
                      char const * const in, size_t const len)
{
    size_t n = len;
    size_t i;

    if ( (cap - 1U) < n )
    {
        n = cap - 1U;
    }
    for ( i = 0U; i < n; i++ )
    {
        if ( (' ' <= in[i]) && ('~' >= in[i]) )
        {
            out[i] = in[i];
        }
        else
        {
            out[i] = '.';
        }
    }
    out[n] = '\0';
}

/**
 * @brief Whether the payload is exactly @p word.
 *
 * The length is compared, not a terminator looked for: the payload of an
 * MqttMsgEvt is a byte count and a buffer, and one that fills the buffer has
 * no terminator to find.
 *
 * @param m    The message.
 * @param word The text to compare with.
 * @return true when @p m carries those bytes and no others.
 */
static bool payload_is(MqttMsgEvt const * const m, char const * const word)
{
    size_t const n = strlen(word);
    return ((size_t)m->len == n) && (memcmp(m->data, word, n) == 0);
}

/**
 * @brief Set the LED from what a message said.
 *
 * kode_sys_led_set_rgb() posts to AO_LedMgr and returns: AO_LedMgr owns the
 * chip. A word this does not know leaves the LED as it was, and so does a
 * colour AO_LedMgr's queue had no room for.
 *
 * @param m The message.
 */
static void apply_colour(MqttMsgEvt const * const m)
{
    kode_status_t rc = KODE_OK;

    if ( payload_is(m, "red") )
    {
        rc = kode_sys_led_set_rgb(MD_LEVEL, 0U, 0U);
    }
    else if ( payload_is(m, "green") )
    {
        rc = kode_sys_led_set_rgb(0U, MD_LEVEL, 0U);
    }
    else if ( payload_is(m, "blue") )
    {
        rc = kode_sys_led_set_rgb(0U, 0U, MD_LEVEL);
    }
    else if ( payload_is(m, "off") )
    {
        rc = kode_sys_led_set_rgb(0U, 0U, 0U);
    }
    else
    {
        KLOG_I(TAG, "not a colour this LED knows; it stays as it is");
    }
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "the LED refused the colour (status %d)", (int)rc);
    }
}

/**
 * @brief Say where the broker connection stands, and why it is not up.
 * @param m The state AO_Net published.
 */
static void log_state(MqttStateEvt const * const m)
{
    char const *state = "?";
    char const *why   = "?";

    if ( (size_t)m->state < Q_DIM(MD_STATES) )
    {
        state = MD_STATES[m->state];
    }
    if ( (size_t)m->err < Q_DIM(MD_WHY) )
    {
        why = MD_WHY[m->err];
    }
    KLOG_I(TAG, "broker %s (%s)", state, why);
}

/**
 * @brief Ask AO_Net to connect to the broker.
 *
 * Safe to ask more than once: a client that is already running answers with
 * its state and a warning, and one that has no network answers "no network".
 * Either way the outcome is an MQTT_STATE_SIG, which is what this AO acts on.
 *
 * @param me The AO.
 */
static void connect_to_broker(MqttDemo const * const me)
{
    if ( '\0' == me->topic_in[0] )
    {
        /* kode_sys_device_id() is empty only when the board could not read
         * its own factory MAC, and a topic without it would be shared with
         * every other board in that state on a public broker. Better silent
         * than that. */
        KLOG_E(TAG, "no device id, so no topic that is this board's own");
    }
    else
    {
        kode_status_t const rc =
            kode_sys_mqtt_connect(MD_BROKER_URI, (char const *)0,
                                  (char const *)0);

        if ( KODE_OK == rc )
        {
            KLOG_I(TAG, "connecting to %s", MD_BROKER_URI);
        }
        else
        {
            KLOG_W(TAG, "connect not taken (status %d)", (int)rc);
        }
    }
}

void MqttDemo_ctor(void)
{
    MqttDemo * const me = &l_ao;
    char const * const id = kode_sys_device_id();

    QActive_ctor(&me->super, Q_STATE_CAST(&MqttDemo_initial));
    me->topic_in[0]  = '\0';
    me->topic_out[0] = '\0';
    if ( '\0' != id[0] )
    {
        (void)snprintf(me->topic_in,  sizeof(me->topic_in),
                       "%s%s/in",  MD_TOPIC_ROOT, id);
        (void)snprintf(me->topic_out, sizeof(me->topic_out),
                       "%s%s/out", MD_TOPIC_ROOT, id);
    }
}

void MqttDemo_start(void)
{
    /* Name the FreeRTOS task after the AO. Without this the port calls every
     * one of them "AO", which is fine until something asserts: a full queue
     * names the victim, never the culprit. */
    QActive_setAttr(AO_MqttDemo, TASK_NAME_ATTR, "mqtt");
    /* Above AO_Net: a broker sets MQTT_MSG_SIG's rate, and only a subscriber
     * above AO_Net drains each one before a full queue asserts. */
    QACTIVE_START(AO_MqttDemo, KODE_PRIO_USER_HIGH,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe, then read where Wi-Fi stands.
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to MqttDemo_offline.
 */
static QState MqttDemo_initial(MqttDemo * const me, void const * const par)
{
    kode_sys_wifi_status_t w;

    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    QActive_subscribe(&me->super, WIFI_STATE_SIG);
    QActive_subscribe(&me->super, MQTT_STATE_SIG);
    QActive_subscribe(&me->super, MQTT_MSG_SIG);

    /* Read rather than wait: AO_Radio may have joined a network before this
     * AO existed, and the announcement of that is long gone. Subscribed first,
     * so a join between the two lines is not missed either. */
    if ( (KODE_OK == kode_sys_wifi_status(&w))
         && (KODE_SYS_WIFI_STATE_GOT_IP == (uint32_t)w.state) && (0U != w.ip4) )
    {
        connect_to_broker(me);
    }
    else
    {
        KLOG_I(TAG, "up; waiting for an address (`wifi_up`, then `wifi_join` "
                    "once, if this board has no network yet)");
    }
    return Q_TRAN(&MqttDemo_offline);
}

/**
 * @brief No session: waiting for an address, or for the broker to answer.
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState MqttDemo_offline(MqttDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case WIFI_STATE_SIG: {
            WifiStateEvt const * const w = (WifiStateEvt const *)e;

            /* state is a KODE_SYS_WIFI_STATE_x, and the address is
             * meaningful only in that one state - the same test AO_Net
             * applies before it will take a request. Everything else this
             * signal carries (joining, a failure) changes nothing here. */
            if ( (KODE_SYS_WIFI_STATE_GOT_IP == (uint32_t)w->state)
                 && (0U != w->ip4) )
            {
                connect_to_broker(me);
            }
            status = Q_HANDLED();
            break;
        }
        case MQTT_STATE_SIG: {
            MqttStateEvt const * const m = (MqttStateEvt const *)e;

            log_state(m);
            if ( KODE_SYS_MQTT_STATE_UP == (uint32_t)m->state )
            {
                status = Q_TRAN(&MqttDemo_connected);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* Any press retries. It is the recovery for a broker that timed
             * out, and the answer is in the log either way. */
            if ( be->pressed )
            {
                KLOG_I(TAG, "not connected; trying again");
                connect_to_broker(me);
            }
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
 * @brief The broker is up: buttons publish and messages arrive.
 *
 * Entering this state is what subscribes, so it happens again after every
 * reconnect: a client that dropped and came back has forgotten what it was
 * listening to.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState MqttDemo_connected(MqttDemo * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            /* Only once UP: AO_Net refuses a subscribe or publish before that,
             * with a log line and no event. */
            if ( KODE_OK != kode_sys_mqtt_subscribe(me->topic_in) )
            {
                KLOG_W(TAG, "the subscription was not taken");
            }
            KLOG_I(TAG, "listening on %s; the buttons publish to %s",
                   me->topic_in, me->topic_out);
            status = Q_HANDLED();
            break;
        }
        case MQTT_STATE_SIG: {
            MqttStateEvt const * const m = (MqttStateEvt const *)e;

            /* Not up any more: the link dropped, or the network went and
             * AO_Net stopped the client. The client reconnects by itself and
             * says UP again, which brings this AO back through the entry
             * above. */
            if ( KODE_SYS_MQTT_STATE_UP != (uint32_t)m->state )
            {
                log_state(m);
                status = Q_TRAN(&MqttDemo_offline);
            }
            else
            {
                status = Q_HANDLED();
            }
            break;
        }
        /* The event is only good for this step: anything kept is copied out. */
        case MQTT_MSG_SIG: {
            MqttMsgEvt const * const m = (MqttMsgEvt const *)e;
            char topic[KODE_SYS_MQTT_TOPIC_MAX];
            char text[KODE_MQTT_PAYLOAD_MAX + 1U];
            char const *cut = "";

            printable(topic, sizeof(topic), m->topic, strlen(m->topic));
            printable(text, sizeof(text), (char const *)m->data, (size_t)m->len);
            if ( m->data_cut )
            {
                cut = " (cut)";
            }
            KLOG_I(TAG, "message on %s: \"%s\"%s", topic, text, cut);
            apply_colour(m);
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_COUNT > (uint32_t)be->id)
                && ((char const *)0 != MD_BUTTON_NAMES[be->id]) )
            {
                char const * const name = MD_BUTTON_NAMES[be->id];

                if ( KODE_OK == kode_sys_mqtt_publish(me->topic_out, name,
                                                      (uint16_t)strlen(name)) )
                {
                    KLOG_I(TAG, "published \"%s\"", name);
                }
                else
                {
                    KLOG_W(TAG, "\"%s\" was not taken", name);
                }
            }
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
