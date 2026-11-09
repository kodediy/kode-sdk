/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_mqtt.c
 * @brief The standard layer's MQTT: connect, publish, subscribe, unsubscribe, leave.
 */
#include "kode.h"

#include <inttypes.h>
#include <string.h>

#include "kdrv_net.h"
#include "khal_once.h"
#include "khal_sync.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"

_Static_assert(KODE_MQTT_URI_LIMIT == (KODE_MQTT_URI_MAX - 1U),
               "the address limit is what the request event carries, less its NUL");
_Static_assert(KODE_MQTT_TOPIC_LIMIT == (KODE_MQTT_TOPIC_MAX - 1U),
               "the topic limit is what the request events carry, less their NUL");
_Static_assert(KODE_MQTT_TEXT_LIMIT == KODE_MQTT_PAYLOAD_MAX,
               "the text limit is what a request event carries");

_Static_assert(KODE_MQTT_WAIT_MS > KODE_MQTT_CONNECT_MS,
               "AO_Net's own timeout answer arrives inside the call that asked");

/** @brief The scheme of a plain broker address. */
#define MQTT_SCHEME "mqtt://"

/** @brief The scheme of a verified broker address. */
#define MQTT_SCHEME_TLS "mqtts://"

/** @brief What the error text shows for an address that is NULL. */
#define MQTT_NULL_SHOWN "(null)"

/** @brief Why a call is refused while the Kode channel holds the client. */
#define MQTT_CHANNEL_USING "the Kode channel is using MQTT (set CONFIG_KODEOS_CHANNEL=n in the app)"

/** @brief The lock every public MQTT call holds; made on the first call. */
static khal_mutex_t s_mqtt_lock;

/** @brief Make the MQTT lock. @return true when it exists. */
static bool mqtt_lock_make(void)
{
    s_mqtt_lock = khal_mutex_create();
    return NULL != s_mqtt_lock;
}

/**
 * @brief kode_enter(), then the MQTT turn; mqtt_exit() gives it back.
 * @param name The public name of the call.
 * @return true when the call has its turn; false with the error set.
 */
static bool mqtt_enter(const char *name)
{
    static khal_once_t lock_once;  /* the one-shot the lock is made by */
    bool               ok = kode_enter(name);

    if ( ok )
    {
        ok = khal_once_try(&lock_once, &mqtt_lock_make);
        if ( ok )
        {
            khal_mutex_lock(s_mqtt_lock);
        }
        else
        {
            kode_err_set("%s: no memory for the MQTT lock", name);
        }
    }
    return ok;
}

/** @brief Give the turn back, at the end of a call mqtt_enter() let in. */
static void mqtt_exit(void)
{
    khal_mutex_unlock(s_mqtt_lock);
}

/**
 * @brief Keep why the facade refused as the task's error.
 * @param rc The facade's answer.
 */
static void mqtt_fail(kode_status_t rc)
{
    switch ( rc )
    {
    case KODE_DENIED:
        kode_err_set("this firmware has no network");
        break;
    case KODE_NO_RESOURCES:
        kode_err_set("MQTT is busy");
        break;
    default:
        kode_err_set("MQTT did not take the request");
        break;
    }
}

/**
 * @brief Whether @p uri starts with mqtt:// or mqtts:// and a host follows.
 * @param uri The address.
 * @return true when it does.
 */
static bool mqtt_has_scheme(const char *uri)
{
    size_t start = 0U;

    if ( kode_has_prefix(uri, MQTT_SCHEME) )
    {
        start = strlen(MQTT_SCHEME);
    }
    else if ( kode_has_prefix(uri, MQTT_SCHEME_TLS) )
    {
        start = strlen(MQTT_SCHEME_TLS);
    }
    else
    {
        /* no scheme */
    }
    return (0U < start) && ('\0' != uri[start]);
}

/**
 * @brief Check a broker address before anything is posted.
 * @param uri What the caller typed; may be NULL.
 * @return true when it is mqtt:// or mqtts:// and fits; false with the error set.
 */
static bool mqtt_uri_ok(const char *uri)
{
    const uint32_t limit = KODE_MQTT_URI_LIMIT;
    const char    *shown = MQTT_NULL_SHOWN;
    bool           ok    = false;

    if ( NULL != uri )
    {
        shown = uri;
    }
    if ( (NULL == uri) || (strlen(uri) == 0U)
        || !mqtt_has_scheme(uri) )
    {
        kode_err_set("bad broker address '%s': use mqtt:// or mqtts://", shown);
    }
    else if ( strlen(uri) > KODE_MQTT_URI_LIMIT )
    {
        kode_err_set("broker address over %" PRIu32 " bytes", limit);
    }
    else
    {
        ok = true;
    }
    return ok;
}

/**
 * @brief Check a topic before anything is posted.
 * @param topic What the caller typed; may be NULL.
 * @return true when it is 1 to KODE_MQTT_TOPIC_LIMIT bytes; false with the error set.
 */
static bool mqtt_topic_ok(const char *topic)
{
    const uint32_t limit = KODE_MQTT_TOPIC_LIMIT;
    bool           ok    = false;

    if ( (NULL == topic) || ('\0' == topic[0]) )
    {
        kode_err_set("no topic");
    }
    else if ( strlen(topic) > KODE_MQTT_TOPIC_LIMIT )
    {
        kode_err_set("topic over %" PRIu32 " bytes", limit);
    }
    else
    {
        ok = true;
    }
    return ok;
}

/**
 * @brief Check a message before anything is posted.
 * @param text What the caller typed; may be NULL.
 * @return true when it is at most KODE_MQTT_TEXT_LIMIT bytes; false with the error set.
 */
static bool mqtt_text_ok(const char *text)
{
    const uint32_t limit = KODE_MQTT_TEXT_LIMIT;
    bool           ok    = false;

    if ( NULL == text )
    {
        kode_err_set("no text to publish");
    }
    else if ( strlen(text) > KODE_MQTT_TEXT_LIMIT )
    {
        kode_err_set("text over %" PRIu32 " bytes", limit);
    }
    else
    {
        ok = true;
    }
    return ok;
}

/**
 * @brief Read where the client stands; the error is set on failure.
 * @param st The status.
 * @return true when it was read.
 */
static bool mqtt_status_read(kode_sys_mqtt_status_t *st)
{
    const kode_status_t rc = kode_sys_mqtt_status(st);

    if ( KODE_OK != rc )
    {
        mqtt_fail(rc);
    }
    return KODE_OK == rc;
}

/**
 * @brief Whether the Kode channel leaves the client to the app.
 * @param st The status.
 * @return true when it does; false with the error set.
 */
static bool mqtt_channel_free(const kode_sys_mqtt_status_t *st)
{
    if ( st->channel )
    {
        kode_err_set(MQTT_CHANNEL_USING);
    }
    return !st->channel;
}

/**
 * @brief Read where the client stands and require it to be connected.
 * @return true when it is; false with the error set.
 */
static bool mqtt_require_up(void)
{
    kode_sys_mqtt_status_t st = { 0U, 0U, 0U, "", false };
    bool                   ok = mqtt_status_read(&st) && mqtt_channel_free(&st);

    if ( ok && (KODE_SYS_MQTT_STATE_UP != st.state) )
    {
        kode_err_set("not connected: call kode_mqtt_connect() first");
        ok = false;
    }
    return ok;
}

/** @brief What a request asks of AO_Net. */
typedef enum {
    MQTT_OP_CONNECT = 0,  /**< Connect to a broker.   */
    MQTT_OP_LEAVE,        /**< Stop the client.       */
    MQTT_OP_PUBLISH,      /**< Send a message.        */
    MQTT_OP_SUBSCRIBE,    /**< Ask for a topic.       */
    MQTT_OP_UNSUBSCRIBE   /**< Drop a topic.          */
} mqtt_op_t;

/** @brief A request to AO_Net and its last answer. */
typedef struct {
    mqtt_op_t     op;       /**< What to ask.                                   */
    const char   *first;    /**< The broker or the topic.                       */
    const char   *text;     /**< The message, for a publish.                    */
    uint16_t      len;      /**< Its length.                                    */
    kode_status_t rc;       /**< The last answer.                               */
    uint32_t      answers;  /**< AO_Net's answers counted just before a connect. */
} mqtt_post_t;

/** @brief One try of a request: true unless AO_Net's queue had no room. */
static bool mqtt_post_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the mqtt_post_t mqtt_post() passed */
    mqtt_post_t *const p = (mqtt_post_t *)ctx;

    switch ( p->op )
    {
    case MQTT_OP_CONNECT: {
        kode_sys_mqtt_status_t st = { 0U, 0U, 0U, "", false };

        /* counted here, so an answer to an earlier try is not this connect's */
        if ( kode_sys_mqtt_status(&st) == KODE_OK )
        {
            p->answers = st.answers;
        }
        p->rc = kode_sys_mqtt_connect(p->first, NULL, NULL);
        break;
    }
    case MQTT_OP_LEAVE:
        p->rc = kode_sys_mqtt_disconnect();
        break;
    case MQTT_OP_PUBLISH:
        p->rc = kode_sys_mqtt_publish(p->first, p->text, p->len);
        break;
    case MQTT_OP_SUBSCRIBE:
        p->rc = kode_sys_mqtt_subscribe(p->first);
        break;
    default:
        p->rc = kode_sys_mqtt_unsubscribe(p->first);
        break;
    }
    return KODE_NO_RESOURCES != p->rc;
}

/**
 * @brief Ask AO_Net, waiting for room in its queue; the error is set on failure.
 * @param p The request.
 * @return true when it was posted.
 */
static bool mqtt_post(mqtt_post_t *p)
{
    (void)kode_retry(&mqtt_post_attempt, p, KODE_MQTT_BUSY_MS);
    if ( KODE_OK != p->rc )
    {
        mqtt_fail(p->rc);
    }
    return KODE_OK == p->rc;
}

/** @brief Where a connect stands. */
typedef enum {
    MQTT_WAITING = 0,  /**< Not there yet.                       */
    MQTT_JOINED,       /**< The client is up.                    */
    MQTT_ANSWERED,     /**< AO_Net answered with a reason.       */
    MQTT_OTHER,        /**< A client of another broker answered. */
    MQTT_UNAVAILABLE   /**< The facade has no network to ask.    */
} mqtt_outcome_t;

/** @brief The watch over one connect. */
typedef struct {
    const char    *uri;      /**< The broker asked.                                */
    uint32_t       answers;  /**< The answers counted when the request was posted. */
    uint8_t        err;      /**< The reason of the answer that ended the wait.    */
    mqtt_outcome_t outcome;  /**< Where the connect stands.                        */
    kode_status_t  rc;       /**< The facade's answer if it refused.               */
} mqtt_watch_t;

/** @brief One look at the connect: true once it has an answer. */
static bool mqtt_watch_attempt(void *ctx)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the context is the mqtt_watch_t mqtt_wait_up() passed */
    mqtt_watch_t *const    w  = (mqtt_watch_t *)ctx;
    kode_sys_mqtt_status_t st = { 0U, 0U, 0U, "", false };

    w->rc = kode_sys_mqtt_status(&st);
    if ( KODE_OK != w->rc )
    {
        w->outcome = MQTT_UNAVAILABLE;
    }
    else if ( (KODE_SYS_MQTT_STATE_UP == st.state) && (strcmp(st.uri, w->uri) == 0) )
    {
        w->outcome = MQTT_JOINED;
    }
    else if ( (st.answers != w->answers) && (KODE_SYS_MQTT_ERR_NONE != st.err) )
    {
        /* an answer after the post, and not a good one */
        w->err     = st.err;
        w->outcome = MQTT_ANSWERED;
    }
    else if ( (st.answers != w->answers) && (KODE_SYS_MQTT_STATE_DOWN != st.state)
             && (strcmp(st.uri, w->uri) != 0) )
    {
        /* an answer after the post, from a client on another broker */
        w->outcome = MQTT_OTHER;
    }
    else
    {
        /* still connecting, or an answer from before the post */
    }
    return MQTT_WAITING != w->outcome;
}

/**
 * @brief Keep why AO_Net refused the connect as the task's error.
 * @param uri The broker asked.
 * @param err A KODE_SYS_MQTT_ERR_x.
 */
static void mqtt_answer_fail(const char *uri, uint8_t err)
{
    switch ( err )
    {
    case KODE_SYS_MQTT_ERR_BUSY:
        kode_err_set(MQTT_CHANNEL_USING);
        break;
    case KODE_SYS_MQTT_ERR_NO_NET:
        kode_err_set("no network: call kode_wifi_connect() first");
        break;
    case KODE_SYS_MQTT_ERR_REFUSED:
        kode_err_set("%s: the broker refused", uri);
        break;
    default:
        kode_err_set("%s: the broker did not answer", uri);
        break;
    }
}

/**
 * @brief Wait for the client to be up on @p uri; the error is set on failure.
 * @param uri     The broker asked.
 * @param answers The answers counted when the request was posted.
 * @return true when the client is up.
 */
static bool mqtt_wait_up(const char *uri, uint32_t answers)
{
    mqtt_watch_t w = { uri, answers, KODE_SYS_MQTT_ERR_NONE, MQTT_WAITING, KODE_OK };

    (void)kode_retry(&mqtt_watch_attempt, &w, KODE_MQTT_WAIT_MS);
    switch ( w.outcome )
    {
    case MQTT_JOINED:
        break;
    case MQTT_UNAVAILABLE:
        mqtt_fail(w.rc);
        break;
    case MQTT_ANSWERED:
        mqtt_answer_fail(uri, w.err);
        break;
    case MQTT_OTHER:
        kode_err_set("another MQTT connection is running: call kode_mqtt_disconnect() first");
        break;
    default:
        mqtt_answer_fail(uri, KODE_SYS_MQTT_ERR_TIMEOUT);
        break;
    }
    return MQTT_JOINED == w.outcome;
}

/**
 * @brief The connect itself, under the MQTT lock.
 * @param uri The broker.
 * @return true when the client is up on it.
 */
static bool mqtt_open(const char *uri)
{
    kode_sys_mqtt_status_t st = { 0U, 0U, 0U, "", false };
    bool                   ok = mqtt_uri_ok(uri) && mqtt_status_read(&st) && mqtt_channel_free(&st);

    if ( ok && (KODE_SYS_MQTT_STATE_UP == st.state) )
    {
        if ( strcmp(st.uri, uri) != 0 )
        {
            kode_err_set("already connected to %s: call kode_mqtt_disconnect() first", st.uri);
            ok = false;
        }
    }
    else if ( ok )
    {
        mqtt_post_t p = { MQTT_OP_CONNECT, uri, NULL, 0U, KODE_ERROR, st.answers };

        ok = mqtt_post(&p) && mqtt_wait_up(uri, p.answers);
    }
    else
    {
        /* the error is set */
    }
    return ok;
}

bool kode_mqtt_connect(const char *uri)
{
    bool ok = mqtt_enter("kode_mqtt_connect");

    if ( ok )
    {
        ok = mqtt_open(uri);
        mqtt_exit();
    }
    return ok;
}

/**
 * @brief The publish itself, under the MQTT lock.
 * @param topic The topic.
 * @param text  The message.
 * @return true when it was queued.
 */
static bool mqtt_send(const char *topic, const char *text)
{
    bool ok = mqtt_topic_ok(topic) && mqtt_text_ok(text) && mqtt_require_up();

    if ( ok )
    {
        mqtt_post_t p = { MQTT_OP_PUBLISH, topic, text, (uint16_t)strlen(text), KODE_ERROR, 0U };

        ok = mqtt_post(&p);
    }
    return ok;
}

bool kode_mqtt_publish(const char *topic, const char *text)
{
    bool ok = mqtt_enter("kode_mqtt_publish");

    if ( ok )
    {
        ok = mqtt_send(topic, text);
        mqtt_exit();
    }
    return ok;
}

/**
 * @brief The subscribe itself, under the MQTT lock.
 * @param topic The topic.
 * @return true when it was queued.
 */
static bool mqtt_listen(const char *topic)
{
    bool ok = mqtt_topic_ok(topic) && kode_standard_ao_ensure() && mqtt_require_up();

    if ( ok )
    {
        mqtt_post_t p = { MQTT_OP_SUBSCRIBE, topic, NULL, 0U, KODE_ERROR, 0U };

        ok = mqtt_post(&p);
    }
    return ok;
}

bool kode_mqtt_subscribe(const char *topic)
{
    bool ok = mqtt_enter("kode_mqtt_subscribe");

    if ( ok )
    {
        ok = mqtt_listen(topic);
        mqtt_exit();
    }
    return ok;
}

/**
 * @brief The unsubscribe itself, under the MQTT lock.
 * @param topic The topic.
 * @return true when it was queued.
 */
static bool mqtt_forget(const char *topic)
{
    bool ok = mqtt_topic_ok(topic) && mqtt_require_up();

    if ( ok )
    {
        mqtt_post_t p = { MQTT_OP_UNSUBSCRIBE, topic, NULL, 0U, KODE_ERROR, 0U };

        ok = mqtt_post(&p);
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_mqtt_unsubscribe(const char *topic)
{
    bool ok = mqtt_enter("kode_mqtt_unsubscribe");

    if ( ok )
    {
        ok = mqtt_forget(topic);
        mqtt_exit();
    }
    return ok;
}

/* cppcheck-suppress misra-c2012-8.7 ; a facade call: the apps outside this tree call it */
bool kode_mqtt_is_connected(void)
{
    kode_sys_mqtt_status_t st = { 0U, 0U, 0U, "", false };
    bool                   up = false;

    if ( kode_enter("kode_mqtt_is_connected") )
    {
        up = (kode_sys_mqtt_status(&st) == KODE_OK)
             && (KODE_SYS_MQTT_STATE_UP == st.state) && !st.channel;
    }
    return up;
}

/** @brief One look at the client: true once it is down. */
static bool mqtt_gone_attempt(void *ctx)
{
    kode_sys_mqtt_status_t st = { 0U, 0U, 0U, "", false };

    (void)ctx;
    return (kode_sys_mqtt_status(&st) != KODE_OK) || (KODE_SYS_MQTT_STATE_DOWN == st.state);
}

/**
 * @brief The leave itself, under the MQTT lock.
 * @return true when the client is down.
 */
static bool mqtt_drop(void)
{
    kode_sys_mqtt_status_t st = { 0U, 0U, 0U, "", false };
    bool                   ok = mqtt_status_read(&st) && mqtt_channel_free(&st);

    if ( ok && (KODE_SYS_MQTT_STATE_DOWN != st.state) )
    {
        mqtt_post_t    p       = { MQTT_OP_LEAVE, NULL, NULL, 0U, KODE_ERROR, 0U };
        const uint32_t seconds = KODE_MQTT_WAIT_MS / KODE_MS_PER_S;

        ok = mqtt_post(&p);
        if ( ok && !kode_retry(&mqtt_gone_attempt, NULL, KODE_MQTT_WAIT_MS) )
        {
            kode_err_set("could not leave %s within %" PRIu32 " s", st.uri, seconds);
            ok = false;
        }
    }
    return ok;
}

bool kode_mqtt_disconnect(void)
{
    bool ok = mqtt_enter("kode_mqtt_disconnect");

    if ( ok )
    {
        ok = mqtt_drop();
        mqtt_exit();
    }
    return ok;
}
