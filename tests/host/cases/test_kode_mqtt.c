/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_mqtt.c
 * @brief kode_mqtt_*() over a scripted copy of the module and over the real facade, and the mqtt_message event.
 */
#include <inttypes.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "Mockkdrv_net.h"

#include "fakes.h"
#include "kaos.h"
#include "kdrv_net.h"
#include "khal_sync.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_standard_ao.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "kqp.h"
#include "kqp_config.h"
#include "qf_support.h"
#include "standard_ao_support.h"

#define URI_A           "mqtt://broker.example:1883"
#define URI_B           "mqtts://other.example:8883"
#define TOPIC           "some/topic"
#define CHANNEL_TEXT    "the Kode channel is using MQTT (set CONFIG_KODEOS_CHANNEL=n in the app)"
#define NO_NET_TEXT     "no network: call kode_wifi_connect() first"
#define NO_FIRMWARE     "this firmware has no network"
#define NOT_UP_TEXT     "not connected: call kode_mqtt_connect() first"
#define BUSY_TEXT       "MQTT is busy"
#define FILL_BYTE       'x'              /**< The byte a long string is made of.            */
#define LONG_MAX_BYTES  (KODE_MQTT_TEXT_LIMIT + 8U) /**< A buffer longer than any limit.    */
#define FILL_MAX        64U              /**< Posts that surely fill AO_Net's queue.        */
#define LOOP_PUBLISHES  40U              /**< Publishes of the loop case.                   */
#define POOLS           2U               /**< Pools whose free blocks the cases count.      */

/* ---- The facade's seam: what the scripted copy of the module calls ------------ */

/** @brief Steps a script can hold. */
#define SEAM_MAX 8U

/** @brief One read of the scripted status. */
typedef struct {
    kdrv_mqtt_state_t state;    /**< Where the client is.                       */
    kdrv_mqtt_err_t   err;      /**< The last answer's reason.                  */
    uint32_t          answers;  /**< Answers published so far.                  */
    const char       *uri;      /**< The broker it names.                       */
    bool              channel;  /**< The Kode channel holds the client.         */
} seam_step_t;

static seam_step_t   s_seam_step[SEAM_MAX];  /**< The reads status serves, in order.        */
static uint32_t      s_seam_len;             /**< How many are scripted.                    */
static uint32_t      s_seam_reads;           /**< How many reads status has served.         */
static kode_status_t s_seam_status_rc;       /**< What status answers.                      */
static kode_status_t s_seam_door_rc;         /**< What every door answers.                  */
static uint32_t      s_seam_busy_first;      /**< Connect tries refused as busy before one is taken. */
static uint32_t      s_seam_ups;             /**< Connect requests taken.                   */
static uint32_t      s_seam_downs;           /**< Disconnect requests taken.                */
static uint32_t      s_seam_pubs;            /**< Publish requests taken.                   */
static uint32_t      s_seam_subs;            /**< Subscribe requests taken.                 */
static uint32_t      s_seam_unsubs;          /**< Unsubscribe requests taken.               */
static char          s_seam_uri[KODE_MQTT_URI_MAX];      /**< The last broker asked for.    */
static char          s_seam_topic[KODE_MQTT_TOPIC_MAX];  /**< The last topic.               */
static char          s_seam_text[KODE_MQTT_PAYLOAD_MAX + 1U];  /**< The last message.       */
static uint16_t      s_seam_len_sent;        /**< Its length.                               */
static uint32_t      s_seam_ensures;         /**< Times the layer's Active Object was asked for. */
static bool          s_seam_ensure_ok;       /**< What that answers.                        */

static kode_status_t seam_status(kode_sys_mqtt_status_t *out);
static kode_status_t seam_up(const char *uri, const char *user, const char *pass);
static kode_status_t seam_down(void);
static kode_status_t seam_pub(const char *topic, const void *data, uint16_t len);
static kode_status_t seam_sub(const char *topic);
static kode_status_t seam_unsub(const char *topic);
static bool          seam_ensure(void);
bool seam_mqtt_connect(const char *uri);
bool seam_mqtt_publish(const char *topic, const char *text);
bool seam_mqtt_subscribe(const char *topic);
bool seam_mqtt_unsubscribe(const char *topic);
bool seam_mqtt_is_connected(void);
bool seam_mqtt_disconnect(void);

#define kode_sys_mqtt_status     seam_status
#define kode_sys_mqtt_connect    seam_up
#define kode_sys_mqtt_disconnect seam_down
#define kode_sys_mqtt_publish    seam_pub
#define kode_sys_mqtt_subscribe  seam_sub
#define kode_sys_mqtt_unsubscribe seam_unsub
#define kode_standard_ao_ensure    seam_ensure
#define kode_mqtt_connect        seam_mqtt_connect
#define kode_mqtt_publish        seam_mqtt_publish
#define kode_mqtt_subscribe      seam_mqtt_subscribe
#define kode_mqtt_unsubscribe    seam_mqtt_unsubscribe
#define kode_mqtt_is_connected   seam_mqtt_is_connected
#define kode_mqtt_disconnect     seam_mqtt_disconnect
#include "kode_mqtt.c"
#undef kode_sys_mqtt_status
#undef kode_sys_mqtt_connect
#undef kode_sys_mqtt_disconnect
#undef kode_sys_mqtt_publish
#undef kode_sys_mqtt_subscribe
#undef kode_sys_mqtt_unsubscribe
#undef kode_standard_ao_ensure
#undef kode_mqtt_connect
#undef kode_mqtt_publish
#undef kode_mqtt_subscribe
#undef kode_mqtt_unsubscribe
#undef kode_mqtt_is_connected
#undef kode_mqtt_disconnect

static kode_status_t seam_status(kode_sys_mqtt_status_t *out)
{
    if ( KODE_OK == s_seam_status_rc )
    {
        uint32_t at = s_seam_reads;

        if ( at >= s_seam_len )
        {
            at = s_seam_len - 1U;
        }
        (void)memset(out, 0, sizeof(*out));
        out->state   = (uint8_t)s_seam_step[at].state;
        out->err     = (uint8_t)s_seam_step[at].err;
        out->answers = s_seam_step[at].answers;
        out->channel = s_seam_step[at].channel;
        (void)snprintf(out->uri, sizeof(out->uri), "%s", s_seam_step[at].uri);
        s_seam_reads++;
    }
    return s_seam_status_rc;
}

static kode_status_t seam_up(const char *uri, const char *user, const char *pass)
{
    (void)user;
    (void)pass;
    if ( 0U < s_seam_busy_first )
    {
        s_seam_busy_first--;
        return KODE_NO_RESOURCES;
    }
    s_seam_ups++;
    (void)snprintf(s_seam_uri, sizeof(s_seam_uri), "%s", uri);
    return s_seam_door_rc;
}

static kode_status_t seam_down(void)
{
    s_seam_downs++;
    return s_seam_door_rc;
}

static kode_status_t seam_pub(const char *topic, const void *data, uint16_t len)
{
    s_seam_pubs++;
    (void)snprintf(s_seam_topic, sizeof(s_seam_topic), "%s", topic);
    (void)memset(s_seam_text, 0, sizeof(s_seam_text));
    (void)memcpy(s_seam_text, data, len);
    s_seam_len_sent = len;
    return s_seam_door_rc;
}

static kode_status_t seam_sub(const char *topic)
{
    s_seam_subs++;
    (void)snprintf(s_seam_topic, sizeof(s_seam_topic), "%s", topic);
    return s_seam_door_rc;
}

static kode_status_t seam_unsub(const char *topic)
{
    s_seam_unsubs++;
    (void)snprintf(s_seam_topic, sizeof(s_seam_topic), "%s", topic);
    return s_seam_door_rc;
}

static bool seam_ensure(void)
{
    s_seam_ensures++;
    if ( !s_seam_ensure_ok )
    {
        kode_err_set("the layer would not start");
    }
    return s_seam_ensure_ok;
}

/** @brief Script the reads status will serve and the answer of the doors. */
static void seam_script(const seam_step_t *steps, uint32_t n, kode_status_t status_rc,
                        kode_status_t door_rc)
{
    for ( uint32_t i = 0U; i < n; i++ )
    {
        s_seam_step[i] = steps[i];
    }
    s_seam_len        = n;
    s_seam_reads      = 0U;
    s_seam_status_rc  = status_rc;
    s_seam_door_rc    = door_rc;
    s_seam_busy_first = 0U;
    s_seam_ups        = 0U;
    s_seam_downs      = 0U;
    s_seam_pubs       = 0U;
    s_seam_subs       = 0U;
    s_seam_unsubs     = 0U;
    s_seam_ensures    = 0U;
    s_seam_ensure_ok  = true;
    s_seam_len_sent   = 0U;
    s_seam_topic[0]   = '\0';
    s_seam_text[0]    = '\0';
}

/** @brief Locks and clock ready; the system marked up. */
static void fresh(void)
{
    /* The module's lock is made on its first call in the binary: a real one. */
    fake_mutex_real(true);
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    qf_support_init();
    fake_sem_reset();
    fake_time_reset();
}

/** @brief A string of @p n filler bytes. */
static void filler(char *out, uint32_t n)
{
    (void)memset(out, FILL_BYTE, n);
    out[n] = '\0';
}

/* ---- connect ------------------------------------------------------------------- */

TEST_CASE("connect returns true once the client is up and posts the broker once", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 1U, URI_A, false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 1U, URI_A, false },
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 2U, URI_A, false },
    };

    fresh();
    seam_script(script, 5U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_ups);
    TEST_ASSERT_EQUAL_STRING(URI_A, s_seam_uri);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("connect to the broker it is already on answers true and posts nothing", "[kode_mqtt]")
{
    static const seam_step_t script[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 5U, URI_A, false } };

    fresh();
    seam_script(script, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_ups);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

TEST_CASE("connect while up on another broker is refused and posts nothing", "[kode_mqtt]")
{
    static const seam_step_t script[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 5U, URI_A, false } };

    fresh();
    seam_script(script, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_B));
    TEST_ASSERT_EQUAL_STRING("already connected to " URI_A ": call kode_mqtt_disconnect() first",
                             kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_ups);
}

TEST_CASE("connect names the channel when its busy answer comes, at once", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 3U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 3U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_BUSY, 4U, "", false },
    };

    fresh();
    seam_script(script, 3U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(CHANNEL_TEXT, kode_error());
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
}

TEST_CASE("connect says there is no network when AO_Net answers so", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NO_NET, 1U, "", false },
    };

    fresh();
    seam_script(script, 3U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(NO_NET_TEXT, kode_error());
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
}

TEST_CASE("connect names the broker that refused", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 1U, URI_A, false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_REFUSED, 2U, URI_A, false },
    };

    fresh();
    seam_script(script, 4U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(URI_A ": the broker refused", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("connect names the broker that did not answer, by AO_Net's answer or by the bound", "[kode_mqtt]")
{
    static const seam_step_t answered[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_TIMEOUT, 1U, URI_A, false },
    };
    static const seam_step_t silent[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 1U, URI_A, false },
    };

    fresh();
    seam_script(answered, 3U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(URI_A ": the broker did not answer", kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());

    fake_time_reset();
    seam_script(silent, 3U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(URI_A ": the broker did not answer", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("connect does not take an earlier answer for its own", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_REFUSED, 7U, URI_B, false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_REFUSED, 7U, URI_B, false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_REFUSED, 7U, URI_A, false },
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_REFUSED, 8U, URI_A, false },
    };

    fresh();
    seam_script(script, 4U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("connect counts the answers when it posts, not before it waited for room", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_REFUSED, 5U, URI_B, false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_REFUSED, 5U, URI_A, false },
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 6U, URI_A, false },
    };

    fresh();
    seam_script(script, 5U, KODE_OK, KODE_OK);
    s_seam_busy_first = 1U;
    TEST_ASSERT_TRUE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_ups);
}

TEST_CASE("connect fails at once when AO_Net answers that another client is running", "[kode_mqtt]")
{
    static const seam_step_t up[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_B, false },
    };
    static const seam_step_t dialling[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 1U, URI_B, false },
    };

    fresh();
    seam_script(up, 3U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING("another MQTT connection is running: call kode_mqtt_disconnect() first",
                             kode_error());
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());

    fake_time_reset();
    seam_script(dialling, 3U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING("another MQTT connection is running: call kode_mqtt_disconnect() first",
                             kode_error());
    TEST_ASSERT_LESS_OR_EQUAL_UINT32(KODE_RETRY_STEP_MS, kode_sys_time_ms());
}

TEST_CASE("connect does not take its own dialling, or a late stop of an old client, for another client",
          "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 1U, URI_B, false },
        { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 2U, URI_A, false },
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 3U, URI_A, false },
    };

    fresh();
    seam_script(script, 5U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
}

TEST_CASE("every call names the Kode channel when it holds the client, and posts nothing", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 3U, URI_B, true },
    };

    fresh();
    seam_script(script, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(CHANNEL_TEXT, kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_publish(TOPIC, "x"));
    TEST_ASSERT_EQUAL_STRING(CHANNEL_TEXT, kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(CHANNEL_TEXT, kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_disconnect());
    TEST_ASSERT_EQUAL_STRING(CHANNEL_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_ups);
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_pubs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_subs);
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_downs);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

TEST_CASE("connect waits for a full queue, then gives up as busy after the bound", "[kode_mqtt]")
{
    static const seam_step_t script[] = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };

    fresh();
    seam_script(script, 1U, KODE_OK, KODE_NO_RESOURCES);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(BUSY_TEXT, kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MQTT_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("connect refuses an address that is not one before it asks anything", "[kode_mqtt]")
{
    static const seam_step_t script[] = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };
    char                     longest[LONG_MAX_BYTES];
    char                     over[LONG_MAX_BYTES];

    fresh();
    seam_script(script, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_connect(NULL));
    TEST_ASSERT_FALSE(seam_mqtt_connect(""));
    TEST_ASSERT_FALSE(seam_mqtt_connect("http://broker.example"));
    TEST_ASSERT_EQUAL_STRING("bad broker address 'http://broker.example': use mqtt:// or mqtts://",
                             kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_connect("broker.example"));
    TEST_ASSERT_FALSE(seam_mqtt_connect("mqtt://"));
    TEST_ASSERT_FALSE(seam_mqtt_connect("mqtts://"));

    (void)snprintf(over, sizeof(over), "mqtt://");
    (void)memset(&over[7], FILL_BYTE, KODE_MQTT_URI_LIMIT - 7U + 1U);
    over[KODE_MQTT_URI_LIMIT + 1U] = '\0';
    TEST_ASSERT_FALSE(seam_mqtt_connect(over));
    TEST_ASSERT_EQUAL_STRING("broker address over 95 bytes", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_ups);

    /* The longest that fits is asked for. */
    (void)snprintf(longest, sizeof(longest), "mqtt://");
    (void)memset(&longest[7], FILL_BYTE, KODE_MQTT_URI_LIMIT - 7U);
    longest[KODE_MQTT_URI_LIMIT] = '\0';
    (void)seam_mqtt_connect(longest);
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_ups);
    TEST_ASSERT_EQUAL_STRING(longest, s_seam_uri);
}

/* ---- publish and subscribe ------------------------------------------------------ */

TEST_CASE("publish while the client is not up is refused and posts nothing", "[kode_mqtt]")
{
    static const seam_step_t down[]       = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };
    static const seam_step_t connecting[] = { { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };

    fresh();
    seam_script(down, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_publish(TOPIC, "hello"));
    TEST_ASSERT_EQUAL_STRING(NOT_UP_TEXT, kode_error());
    seam_script(connecting, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_publish(TOPIC, "hello"));
    TEST_ASSERT_EQUAL_STRING(NOT_UP_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_pubs);
}

TEST_CASE("publish hands the topic and the text to the door", "[kode_mqtt]")
{
    static const seam_step_t up[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_publish(TOPIC, "hello"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_pubs);
    TEST_ASSERT_EQUAL_STRING(TOPIC, s_seam_topic);
    TEST_ASSERT_EQUAL_STRING("hello", s_seam_text);
    TEST_ASSERT_EQUAL_UINT16(5U, s_seam_len_sent);

    TEST_ASSERT_TRUE(seam_mqtt_publish(TOPIC, ""));
    TEST_ASSERT_EQUAL_UINT16(0U, s_seam_len_sent);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

TEST_CASE("publish refuses a topic or text over its limit, never cutting it", "[kode_mqtt]")
{
    static const seam_step_t up[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };
    char                     topic[LONG_MAX_BYTES];
    char                     text[LONG_MAX_BYTES];

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_OK);

    filler(topic, KODE_MQTT_TOPIC_LIMIT + 1U);
    TEST_ASSERT_FALSE(seam_mqtt_publish(topic, "x"));
    TEST_ASSERT_EQUAL_STRING("topic over 79 bytes", kode_error());
    filler(text, KODE_MQTT_TEXT_LIMIT + 1U);
    TEST_ASSERT_FALSE(seam_mqtt_publish(TOPIC, text));
    TEST_ASSERT_EQUAL_STRING("text over 240 bytes", kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_publish("", "x"));
    TEST_ASSERT_EQUAL_STRING("no topic", kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_publish(NULL, "x"));
    TEST_ASSERT_FALSE(seam_mqtt_publish(TOPIC, NULL));
    TEST_ASSERT_EQUAL_STRING("no text to publish", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_pubs);

    /* The longest of each goes through whole. */
    filler(topic, KODE_MQTT_TOPIC_LIMIT);
    filler(text, KODE_MQTT_TEXT_LIMIT);
    TEST_ASSERT_TRUE(seam_mqtt_publish(topic, text));
    TEST_ASSERT_EQUAL_STRING(topic, s_seam_topic);
    TEST_ASSERT_EQUAL_STRING(text, s_seam_text);
    TEST_ASSERT_EQUAL_UINT16(KODE_MQTT_TEXT_LIMIT, s_seam_len_sent);
}

TEST_CASE("publish and subscribe wait for room, then say the queue is busy", "[kode_mqtt]")
{
    static const seam_step_t up[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_NO_RESOURCES);
    TEST_ASSERT_FALSE(seam_mqtt_publish(TOPIC, "hello"));
    TEST_ASSERT_EQUAL_STRING(BUSY_TEXT, kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MQTT_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());

    fake_time_reset();
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(BUSY_TEXT, kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MQTT_BUSY_MS, kode_sys_time_ms());
}

TEST_CASE("subscribe starts the layer's Active Object before it asks the broker", "[kode_mqtt]")
{
    static const seam_step_t up[]   = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };
    static const seam_step_t down[] = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };
    char                     topic[LONG_MAX_BYTES];

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_subscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_ensures);
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_subs);
    TEST_ASSERT_EQUAL_STRING(TOPIC, s_seam_topic);

    /* A layer that will not start is why it fails, and the broker is not asked. */
    seam_script(up, 1U, KODE_OK, KODE_OK);
    s_seam_ensure_ok = false;
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING("the layer would not start", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_subs);

    /* Not connected: refused, with the listener path made. */
    seam_script(down, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(NOT_UP_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_ensures);
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_subs);

    /* A topic that is no topic changes nothing. */
    seam_script(up, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(""));
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(NULL));
    filler(topic, KODE_MQTT_TOPIC_LIMIT + 1U);
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(topic));
    TEST_ASSERT_EQUAL_STRING("topic over 79 bytes", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_subs);
}

TEST_CASE("unsubscribe hands the topic to the door once the client is up", "[kode_mqtt]")
{
    static const seam_step_t up[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_unsubscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_unsubs);
    TEST_ASSERT_EQUAL_STRING(TOPIC, s_seam_topic);
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_subs);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

TEST_CASE("unsubscribe refuses no topic and a topic over its limit, and posts nothing", "[kode_mqtt]")
{
    static const seam_step_t up[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };
    char                     topic[LONG_MAX_BYTES];

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_unsubscribe(NULL));
    TEST_ASSERT_EQUAL_STRING("no topic", kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_unsubscribe(""));
    TEST_ASSERT_EQUAL_STRING("no topic", kode_error());
    filler(topic, KODE_MQTT_TOPIC_LIMIT + 1U);
    TEST_ASSERT_FALSE(seam_mqtt_unsubscribe(topic));
    TEST_ASSERT_EQUAL_STRING("topic over 79 bytes", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_unsubs);
}

TEST_CASE("unsubscribe while the client is not up, or the channel holds it, posts nothing", "[kode_mqtt]")
{
    static const seam_step_t down[]    = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };
    static const seam_step_t channel[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, true } };

    fresh();
    seam_script(down, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_unsubscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(NOT_UP_TEXT, kode_error());
    seam_script(channel, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_unsubscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(CHANNEL_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_unsubs);
}

TEST_CASE("unsubscribe waits for room, then says the queue is busy", "[kode_mqtt]")
{
    static const seam_step_t up[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_NO_RESOURCES);
    TEST_ASSERT_FALSE(seam_mqtt_unsubscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(BUSY_TEXT, kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MQTT_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("is_connected is true only for an up client the app can use, and never waits", "[kode_mqtt]")
{
    static const seam_step_t down[]       = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };
    static const seam_step_t connecting[] = { { KDRV_MQTT_CONNECTING, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };
    static const seam_step_t up[]         = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };
    static const seam_step_t channel[]    = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, true } };

    fresh();
    seam_script(down, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_is_connected());
    seam_script(connecting, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_is_connected());
    seam_script(up, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_is_connected());
    seam_script(channel, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_is_connected());
    seam_script(up, 1U, KODE_DENIED, KODE_DENIED);
    TEST_ASSERT_FALSE(seam_mqtt_is_connected());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
}

/* ---- disconnect ------------------------------------------------------------------ */

TEST_CASE("disconnect when the client is down posts nothing", "[kode_mqtt]")
{
    static const seam_step_t down[] = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };

    fresh();
    seam_script(down, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_disconnect());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, s_seam_downs);
}

TEST_CASE("disconnect posts a stop and returns true once the client is down", "[kode_mqtt]")
{
    static const seam_step_t script[] = {
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false },
        { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false },
        { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 2U, URI_A, false },
    };

    fresh();
    seam_script(script, 3U, KODE_OK, KODE_OK);
    TEST_ASSERT_TRUE(seam_mqtt_disconnect());
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_downs);
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("disconnect that the client does not carry out fails after the bound", "[kode_mqtt]")
{
    static const seam_step_t up[] = { { KDRV_MQTT_UP, KDRV_MQTT_ERR_NONE, 1U, URI_A, false } };

    fresh();
    seam_script(up, 1U, KODE_OK, KODE_OK);
    TEST_ASSERT_FALSE(seam_mqtt_disconnect());
    TEST_ASSERT_EQUAL_STRING("could not leave " URI_A " within 20 s", kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());

    fake_time_reset();
    seam_script(up, 1U, KODE_OK, KODE_NO_RESOURCES);
    TEST_ASSERT_FALSE(seam_mqtt_disconnect());
    TEST_ASSERT_EQUAL_STRING(BUSY_TEXT, kode_error());
    TEST_ASSERT_LESS_THAN_UINT32(KODE_MQTT_WAIT_MS, kode_sys_time_ms());
}

TEST_CASE("a firmware without a network says so from every call", "[kode_mqtt]")
{
    static const seam_step_t down[] = { { KDRV_MQTT_DOWN, KDRV_MQTT_ERR_NONE, 0U, "", false } };

    fresh();
    seam_script(down, 1U, KODE_DENIED, KODE_DENIED);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(NO_FIRMWARE, kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_publish(TOPIC, "x"));
    TEST_ASSERT_EQUAL_STRING(NO_FIRMWARE, kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_subscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(NO_FIRMWARE, kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_unsubscribe(TOPIC));
    TEST_ASSERT_EQUAL_STRING(NO_FIRMWARE, kode_error());
    TEST_ASSERT_FALSE(seam_mqtt_disconnect());
    TEST_ASSERT_EQUAL_STRING(NO_FIRMWARE, kode_error());

    /* A status that answers and a door that refuses is the same firmware. */
    seam_script(down, 1U, KODE_OK, KODE_DENIED);
    TEST_ASSERT_FALSE(seam_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_STRING(NO_FIRMWARE, kode_error());
    TEST_ASSERT_EQUAL_UINT32(1U, s_seam_ups);
}

/* ---- The real facade: AO_Net's queue is the record of what was asked ---------------- */

/** @brief The driver's copy of the broker, as a CMock callback. */
static void stub_uri_copy(char *out, size_t cap, int n)
{
    (void)n;
    (void)snprintf(out, cap, "%s", URI_A);
}

/** @brief Empty AO_Net's queue, recycling what it holds. */
static void net_flush(void)
{
    const QEvt *e = NULL;

    while ( xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE )
    {
        QF_gc(e);
    }
}

/** @brief The world of a real-facade case: the client up, AO_Net's queue empty and readable. */
static void world_up(void)
{
    fresh();
    Mockkdrv_net_Init();
    kdrv_net_mqtt_state_IgnoreAndReturn(KDRV_MQTT_UP);
    kdrv_net_mqtt_uri_copy_Stub(&stub_uri_copy);
    qf_support_give_queue(AO_Net);
    net_flush();
}

static const kdrv_mqtt_state_t *s_state_seq;  /**< The states the driver reports, in order.   */
static uint32_t                 s_state_len;  /**< How many are scripted.                     */
static uint32_t                 s_state_at;   /**< How many reads the driver has served.      */

/** @brief The driver's state, as a CMock callback: the script, then its last entry for ever. */
static kdrv_mqtt_state_t stub_state(int n)
{
    uint32_t at = s_state_at;

    (void)n;
    if ( at >= s_state_len )
    {
        at = s_state_len - 1U;
    }
    s_state_at++;
    return s_state_seq[at];
}

/** @brief Give the world back and leave AO_Net's queue empty. */
static void world_down(void)
{
    net_flush();
    Mockkdrv_net_Destroy();
}

/** @brief Fill AO_Net's queue until only its margin is free. */
static void net_fill_to_margin(void)
{
    static const QEvt fill = QEVT_INITIALIZER(MQTT_STATE_REQ_SIG);

    for ( uint32_t guard = 0U;
         (FILL_MAX > guard) && (uxQueueSpacesAvailable(AO_Net->eQueue) > KQP_NET_CMD_MARGIN);
         ++guard )
    {
        QACTIVE_POST(AO_Net, &fill, NULL);
    }
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
}

/** @brief Blocks free in every pool. */
static uint32_t net_pools_free(void)
{
    uint32_t total = 0U;

    for ( uint8_t id = 1U; POOLS >= id; ++id )
    {
        total += (uint32_t)QF_getPoolFree(id);
    }
    return total;
}

TEST_CASE("publish queues a request for AO_Net while it has room", "[kode_mqtt]")
{
    const QEvt *e = NULL;

    world_up();
    TEST_ASSERT_TRUE(kode_mqtt_publish(TOPIC, "hello"));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT(1U, uxQueueMessagesWaiting(AO_Net->eQueue));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)MQTT_PUB_REQ_SIG, (uint32_t)e->sig);
    TEST_ASSERT_EQUAL_STRING(TOPIC, ((const MqttPubEvt *)e)->topic);
    TEST_ASSERT_EQUAL_UINT16(5U, ((const MqttPubEvt *)e)->len);
    TEST_ASSERT_EQUAL_MEMORY("hello", ((const MqttPubEvt *)e)->data, 5U);
    QF_gc(e);
    TEST_ASSERT_EQUAL_UINT32(0U, kode_sys_time_ms());
    world_down();
}

TEST_CASE("publish with AO_Net's queue within its margin is busy after the bound", "[kode_mqtt]")
{
    world_up();
    net_fill_to_margin();
    const uint32_t pools = net_pools_free();

    TEST_ASSERT_FALSE(kode_mqtt_publish(TOPIC, "hello"));
    TEST_ASSERT_EQUAL_STRING(BUSY_TEXT, kode_error());
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(KODE_MQTT_BUSY_MS, kode_sys_time_ms());
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
    TEST_ASSERT_EQUAL_UINT32(pools, net_pools_free());
    world_down();
}

TEST_CASE("a loop of publishes with AO_Net not draining never posts past the margin", "[kode_mqtt]")
{
    uint32_t sent = 0U;

    world_up();
    const UBaseType_t room = uxQueueSpacesAvailable(AO_Net->eQueue);

    for ( uint32_t i = 0U; LOOP_PUBLISHES > i; i++ )
    {
        if ( kode_mqtt_publish(TOPIC, "tick") )
        {
            sent++;
        }
        TEST_ASSERT_GREATER_OR_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
    }
    TEST_ASSERT_EQUAL_UINT32((uint32_t)(room - KQP_NET_CMD_MARGIN), sent);
    TEST_ASSERT_LESS_THAN_UINT32(LOOP_PUBLISHES, sent);
    TEST_ASSERT_EQUAL_STRING(BUSY_TEXT, kode_error());
    TEST_ASSERT_EQUAL_UINT(KQP_NET_CMD_MARGIN, uxQueueSpacesAvailable(AO_Net->eQueue));
    world_down();
}

TEST_CASE("connect, subscribe and disconnect post their requests to AO_Net", "[kode_mqtt]")
{
    static const kdrv_mqtt_state_t coming[] = { KDRV_MQTT_DOWN, KDRV_MQTT_DOWN, KDRV_MQTT_UP };
    static const kdrv_mqtt_state_t going[]  = { KDRV_MQTT_UP, KDRV_MQTT_DOWN };
    const QEvt                    *e        = NULL;

    world_up();
    kdrv_net_mqtt_state_Stub(&stub_state);

    s_state_seq = coming;
    s_state_len = 3U;
    s_state_at  = 0U;
    TEST_ASSERT_TRUE(kode_mqtt_connect(URI_A));
    TEST_ASSERT_EQUAL_UINT(1U, uxQueueMessagesWaiting(AO_Net->eQueue));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)MQTT_UP_REQ_SIG, (uint32_t)e->sig);
    TEST_ASSERT_EQUAL_STRING(URI_A, ((const MqttUpEvt *)e)->uri);
    TEST_ASSERT_EQUAL_STRING("", ((const MqttUpEvt *)e)->user);
    QF_gc(e);

    s_state_seq = going;
    s_state_len = 2U;
    s_state_at  = 0U;
    TEST_ASSERT_TRUE(kode_mqtt_disconnect());
    TEST_ASSERT_TRUE(xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)MQTT_DOWN_REQ_SIG, (uint32_t)e->sig);
    QF_gc(e);

    s_state_seq = going;
    s_state_len = 1U;
    s_state_at  = 0U;
    TEST_ASSERT_TRUE(kode_mqtt_subscribe(TOPIC));
    TEST_ASSERT_TRUE(xQueueReceive(AO_Net->eQueue, &e, 0U) == pdTRUE);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)MQTT_SUB_REQ_SIG, (uint32_t)e->sig);
    TEST_ASSERT_EQUAL_STRING(TOPIC, ((const MqttTopicEvt *)e)->topic);
    QF_gc(e);
    world_down();
}

/* ---- The event ------------------------------------------------------------------------ */

/** @brief The longest the case waits for the dispatch task, in ms. */
#define TEST_WAIT_MS 4000U

/** @brief Records a listener can keep. */
#define REC_MAX 24U

/** @brief Bytes of a burst message's text. */
#define LABEL_MAX 16U

/** @brief Messages of the burst case. */
#define BURST 20U

/** @brief Records the burst keeps: the ring's depth, the one under delivery included. */
#define BURST_KEPT KODE_EVENT_QUEUE_DEPTH

/** @brief What the listener saw of one event. */
typedef struct {
    char    name[KODE_EVENT_NAME_MAX];       /**< The event's name.   */
    char    source[KODE_EVENT_SOURCE_MAX];   /**< Its source.         */
    char    text[KODE_EVENT_TEXT_MAX];       /**< Its text.           */
    int32_t value;                           /**< Its number.         */
} rec_t;

static rec_t             s_rec[REC_MAX];  /**< What the listener saw, in order. */
static volatile uint32_t s_rec_n;         /**< How many records.                */
static SemaphoreHandle_t s_given;         /**< Counts the calls.                */
static SemaphoreHandle_t s_gate_in;       /**< Given by the listener inside its gate. */
static SemaphoreHandle_t s_gate_open;     /**< Given by the case to let the listener go. */
static volatile bool     s_gated;         /**< The next call waits at the gate.  */

/** @brief Record one call; the first of a gated case waits for the case. */
static void on_mqtt(const kode_event_t *e)
{
    const uint32_t i = s_rec_n;

    if ( REC_MAX > i )
    {
        (void)snprintf(s_rec[i].name, sizeof(s_rec[i].name), "%s", e->name);
        (void)snprintf(s_rec[i].source, sizeof(s_rec[i].source), "%s", e->source);
        (void)snprintf(s_rec[i].text, sizeof(s_rec[i].text), "%s", e->text);
        s_rec[i].value = e->value;
    }
    s_rec_n = i + 1U;
    if ( s_gated )
    {
        s_gated = false;
        (void)xSemaphoreGive(s_gate_in);
        (void)xSemaphoreTake(s_gate_open, pdMS_TO_TICKS(TEST_WAIT_MS));
    }
    (void)xSemaphoreGive(s_given);
}

/** @brief Wait for @p n more calls; false if they do not all come. */
static bool rec_wait(uint32_t n)
{
    bool ok = true;

    for ( uint32_t i = 0U; (i < n) && ok; i++ )
    {
        ok = (xSemaphoreTake(s_given, pdMS_TO_TICKS(TEST_WAIT_MS)) == pdTRUE);
    }
    return ok;
}

/** @brief Publish MQTT_MSG_SIG the way AO_Net does. */
static void publish_msg(const char *topic, const void *data, uint16_t len)
{
    MqttMsgEvt *const e = Q_NEW(MqttMsgEvt, MQTT_MSG_SIG);

    (void)snprintf(e->topic, sizeof(e->topic), "%s", topic);
    e->len       = len;
    e->topic_cut = false;
    e->data_cut  = false;
    e->retained  = false;
    (void)memcpy(e->data, data, len);
    QACTIVE_PUBLISH(&e->super, (void *)0);
}

/** @brief The layer up and hearing the broker, a listener on mqtt_message. */
static void events_up(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());
    QActive_subscribe(AO_KodeStandard, MQTT_MSG_SIG);

    if ( NULL == s_given )
    {
        s_given     = xSemaphoreCreateCounting(REC_MAX, 0U);
        s_gate_in   = xSemaphoreCreateCounting(REC_MAX, 0U);
        s_gate_open = xSemaphoreCreateCounting(REC_MAX, 0U);
    }
    while ( xSemaphoreTake(s_given, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_in, 0U) == pdTRUE )
    {
    }
    while ( xSemaphoreTake(s_gate_open, 0U) == pdTRUE )
    {
    }
    s_rec_n = 0U;
    s_gated = false;
    TEST_ASSERT_TRUE(kode_event_add_listener("mqtt_message", &on_mqtt));
}

/** @brief Off the bus again, so the cases of other files never reach the layer through it. */
static void events_down(void)
{
    TEST_ASSERT_TRUE(kode_event_remove_listener("mqtt_message", &on_mqtt));
    QActive_unsubscribe(AO_KodeStandard, MQTT_MSG_SIG);
}

TEST_CASE("a message becomes mqtt_message with the topic, the payload and its length", "[kode_mqtt]")
{
    events_up();

    publish_msg("home/lamp", "on", 2U);
    TEST_ASSERT_TRUE(rec_wait(1U));
    standard_ao_support_drained();

    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_n);
    TEST_ASSERT_EQUAL_STRING("mqtt_message", s_rec[0].name);
    TEST_ASSERT_EQUAL_STRING("home/lamp", s_rec[0].source);
    TEST_ASSERT_EQUAL_STRING("on", s_rec[0].text);
    TEST_ASSERT_EQUAL_INT32(2, s_rec[0].value);
    events_down();
}

TEST_CASE("a topic of the longest length arrives whole in source, and the longest payload whole", "[kode_mqtt]")
{
    char topic[KODE_MQTT_TOPIC_LIMIT + 1U];
    char text[KODE_MQTT_TEXT_LIMIT + 1U];

    events_up();
    filler(topic, KODE_MQTT_TOPIC_LIMIT);
    filler(text, KODE_MQTT_TEXT_LIMIT);

    publish_msg(topic, text, (uint16_t)KODE_MQTT_TEXT_LIMIT);
    TEST_ASSERT_TRUE(rec_wait(1U));
    standard_ao_support_drained();

    TEST_ASSERT_EQUAL_UINT32(1U, s_rec_n);
    TEST_ASSERT_EQUAL_UINT(KODE_MQTT_TOPIC_LIMIT, strlen(s_rec[0].source));
    TEST_ASSERT_EQUAL_STRING(topic, s_rec[0].source);
    TEST_ASSERT_EQUAL_STRING(text, s_rec[0].text);
    TEST_ASSERT_EQUAL_INT32((int32_t)KODE_MQTT_TEXT_LIMIT, s_rec[0].value);
    events_down();
}

TEST_CASE("a payload with a zero byte reads up to it and still counts its length", "[kode_mqtt]")
{
    static const uint8_t data[] = { 'a', 'b', 0U, 'c', 'd' };

    events_up();
    publish_msg(TOPIC, data, (uint16_t)sizeof(data));
    TEST_ASSERT_TRUE(rec_wait(1U));
    standard_ao_support_drained();

    TEST_ASSERT_EQUAL_STRING("ab", s_rec[0].text);
    TEST_ASSERT_EQUAL_INT32((int32_t)sizeof(data), s_rec[0].value);
    events_down();
}

TEST_CASE("20 messages back to back with the listener blocked lose none from the AO's queue", "[kode_mqtt]")
{
    char text[LABEL_MAX];

    events_up();
    s_gated = true;
    (void)snprintf(text, sizeof(text), "msg-%02" PRIu32, (uint32_t)0U);
    publish_msg(TOPIC, text, (uint16_t)strlen(text));
    TEST_ASSERT_EQUAL_INT(pdTRUE, xSemaphoreTake(s_gate_in, pdMS_TO_TICKS(TEST_WAIT_MS)));

    const uint32_t dropped0 = kode_event_dropped();
    const uint32_t pools    = net_pools_free();

    for ( uint32_t i = 1U; BURST > i; i++ )
    {
        (void)snprintf(text, sizeof(text), "msg-%02" PRIu32, i);
        publish_msg(TOPIC, text, (uint16_t)strlen(text));
    }
    standard_ao_support_drained();

    /* Every message reached the ring's door: the Active Object's queue is empty and holds no block. */
    TEST_ASSERT_EQUAL_UINT(0U, uxQueueMessagesWaiting(AO_KodeStandard->eQueue));
    TEST_ASSERT_EQUAL_UINT32(pools, net_pools_free());

    /* The ring kept what fits, the one under delivery included, and counted the rest. */
    TEST_ASSERT_EQUAL_UINT32(BURST - BURST_KEPT, kode_event_dropped() - dropped0);

    (void)xSemaphoreGive(s_gate_open);
    TEST_ASSERT_TRUE(rec_wait(BURST_KEPT));
    TEST_ASSERT_EQUAL_UINT32(BURST_KEPT, s_rec_n);
    for ( uint32_t i = 0U; BURST_KEPT > i; i++ )
    {
        (void)snprintf(text, sizeof(text), "msg-%02" PRIu32, i);
        TEST_ASSERT_EQUAL_STRING(text, s_rec[i].text);
        TEST_ASSERT_EQUAL_INT32((int32_t)strlen(text), s_rec[i].value);
    }
    events_down();
}

/** @brief A listener that does nothing, for the names a case only registers. */
static void on_nothing(const kode_event_t *e)
{
    (void)e;
}

TEST_CASE("mqtt_message is a known event in any case", "[kode_mqtt]")
{
    events_up();
    TEST_ASSERT_TRUE(kode_event_add_listener("MQTT_Message", &on_nothing));
    TEST_ASSERT_TRUE(kode_event_remove_listener("MQTT_Message", &on_nothing));
    TEST_ASSERT_FALSE(kode_event_add_listener("mqtt_messages", &on_nothing));
    TEST_ASSERT_EQUAL_STRING("unknown event 'mqtt_messages'", kode_error());
    events_down();
}
