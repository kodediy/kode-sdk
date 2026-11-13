/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_wifi.c
 * @brief The remote WiFi stack: esp_wifi_remote, over the C5's RPC channel.
 */
#include "kdrv_wifi.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "esp_event.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"

#define TAG "kdrv.wifi"

/** Set by kdrv_wifi_scan_abort(): the sweep in flight ends before its next step. */
static volatile bool s_scan_abort;

_Static_assert(KDRV_WIFI_AUTH_OPEN == (unsigned)WIFI_AUTH_OPEN, "an open network is esp_wifi's");

/** Bits of the group the connect path waits on. */
#define WIFI_BIT_GOT_IP        (1U << 0)
#define WIFI_BIT_DISCONNECTED  (1U << 1)
#define WIFI_BIT_SCAN_DONE     (1U << 2)

/** The scan tables' alignment: kdrv_wifi_ap_t is bytes only. */
#define WIFI_TABLE_ALIGN       1U

static struct {
    bool               started;      /**< esp_wifi_init + start done.       */
    bool               netif;        /**< The STA netif exists (connect).   */
    bool               handlers;     /**< Event handlers registered once.   */
    EventGroupHandle_t bits;
    kdrv_wifi_state_t  state;
    uint32_t           ip4;
    char               ssid[33];
    int8_t             rssi;         /**< kdrv_wifi_rssi()'s cache; read back
                                       *   by kdrv_wifi_rssi_last().          */
    uint8_t            channel;      /**< The same read's primary channel;
                                       *   kdrv_wifi_channel_last().          */
    kdrv_wifi_fail_t   fail;         /**< kdrv_wifi_fail_last().              */
    uint16_t           reason;       /**< The stack's own reason behind `fail`,
                                       *   for the log; 0 when the disconnect
                                       *   carried none.                      */
    uint8_t            count;
    bool               band_warned;  /**< The first refusal of the band call
                                       *   is logged; the rest are not. Kept
                                       *   across stop() and forget(): it is
                                       *   once per boot.                     */
    /* Both tables are one PSRAM block, taken at the first scan
     * (scan_tables()): KDRV_WIFI_SCAN_MAX entries of `aps`, then
     * KDRV_WIFI_LIST_MAX of `ring`. NULL until then, while `count` is 0 and
     * the ring is empty, so no reader reaches them. */
    kdrv_wifi_ap_t    *aps;

    /* The hand-over ring: everything the sweep found, in the order it was
     * heard, waiting for the owner to take it. One producer (the worker,
     * inside a sweep) and one consumer (the owner, on its own thread), which
     * is what makes a lock unnecessary here: a slot is filled before `head`
     * admits it exists, and `tail` is only ever moved by the reader. */
    kdrv_wifi_ap_t    *ring;
    volatile uint8_t   head;
    volatile uint8_t   tail;
    uint16_t           dropped;

    /* Survives stop() and forget(): it belongs to whoever owns the radio,
     * not to any one sweep, and is installed once at bring-up. */
    kdrv_wifi_progress_cb_t on_more;
    kdrv_wifi_lost_cb_t     on_lost;   /**< kdrv_wifi_on_lost(); the same.    */
} s_wifi;

/** @brief Guards `ssid`: the one field of the struct wider than a word, written
 *         by the radio worker and the event task and read from any core. */
/* cppcheck-suppress misra-c2012-7.2 ; IDF's initialiser macro, no suffix to give it */
static portMUX_TYPE s_ssid_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * @brief Replace the network name, whole, under the lock.
 * @param name The new name, truncated to the field; NULL or "" for none.
 */
static void ssid_set(const char *name)
{
    char fresh[sizeof(s_wifi.ssid)];

    (void)memset(fresh, 0, sizeof(fresh));
    if ( NULL != name )
    {
        (void)snprintf(fresh, sizeof(fresh), "%s", name);
    }
    taskENTER_CRITICAL(&s_ssid_mux);
    (void)memcpy(s_wifi.ssid, fresh, sizeof(s_wifi.ssid));
    taskEXIT_CRITICAL(&s_ssid_mux);
}

/**
 * @brief Hold the radio to the band of an access point, or give both bands
 *        back.
 *
 * The C5 joins the first access point of a name that its own scan finds,
 * which is the 2.4 GHz one when the network is on both bands, however weak
 * that one is. A coprocessor that refuses the call (an older firmware) leaves
 * the radio as it is: the caller goes on, and the first refusal of a boot is
 * the only one logged. The call is made whether or not the station is
 * associated, and its answer is never the caller's failure.
 *
 * @param channel The channel of the access point: the lower band up to
 *                KDRV_WIFI_LAST_24_CHANNEL, the upper band above it, both
 *                bands for KDRV_WIFI_CHANNEL_ANY.
 */
static void wifi_band_aim(uint8_t channel)
{
    wifi_band_mode_t mode = WIFI_BAND_MODE_AUTO;

    if ( KDRV_WIFI_LAST_24_CHANNEL < channel )
    {
        mode = WIFI_BAND_MODE_5G_ONLY;
    }
    else if ( KDRV_WIFI_CHANNEL_ANY != channel )
    {
        mode = WIFI_BAND_MODE_2G_ONLY;
    }
    else
    {
        /* Not known: both bands. */
    }

    esp_err_t const err = esp_wifi_set_band_mode(mode);

    if ( (ESP_OK != err) && !s_wifi.band_warned )
    {
        s_wifi.band_warned = true;
        KLOG_W(TAG, "band mode: %s; a join goes to the band the C5 finds "
                    "the network on", esp_err_to_name(err));
    }
}

bool kdrv_wifi_scan_next(kdrv_wifi_ap_t *out)
{
    uint8_t const tail = s_wifi.tail;

    if ( (tail == s_wifi.head) || (NULL == out) )
    {
        return false;
    }
    *out = s_wifi.ring[tail];
    s_wifi.tail = (uint8_t)((tail + 1U) % KDRV_WIFI_LIST_MAX);
    return true;
}

uint16_t kdrv_wifi_scan_dropped(void)
{
    return s_wifi.dropped;
}

void kdrv_wifi_scan_progress(kdrv_wifi_progress_cb_t cb)
{
    s_wifi.on_more = cb;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by AO_Radio, which is a file this check does not join to this one */
void kdrv_wifi_on_lost(kdrv_wifi_lost_cb_t cb)
{
    s_wifi.on_lost = cb;
}

uint8_t kdrv_wifi_scan_count(void)
{
    return s_wifi.count;
}

bool kdrv_wifi_scan_get(uint8_t idx, kdrv_wifi_ap_t *out)
{
    if ( (idx >= s_wifi.count) || (NULL == out) )
    {
        return false;
    }
    *out = s_wifi.aps[idx];
    return true;
}

kdrv_wifi_state_t kdrv_wifi_state(uint32_t *ip4)
{
    if ( NULL != ip4 )
    {
        *ip4 = s_wifi.ip4;
    }
    return s_wifi.state;
}

void kdrv_wifi_ssid_copy(char *out, size_t cap)
{
    if ( (NULL == out) || (0U == cap) )
    {
        return;
    }
    size_t n = 0U;

    taskENTER_CRITICAL(&s_ssid_mux);
    while ( ((cap - 1U) > n) && (sizeof(s_wifi.ssid) > n) && ('\0' != s_wifi.ssid[n]) )
    {
        out[n] = s_wifi.ssid[n];
        ++n;
    }
    taskEXIT_CRITICAL(&s_ssid_mux);
    out[n] = '\0';
}

/* Local check first: the RPC below is a round trip to the coprocessor, and
 * wifi_info asked with nothing joined must not wait on it to say so. Runs on
 * the kdrv_radio worker only - the caller is a job, never an AO step. */
int8_t kdrv_wifi_rssi(void)
{
    if ( kdrv_wifi_state(NULL) != KDRV_WIFI_GOT_IP )
    {
        return 0;
    }

    wifi_ap_record_t ap;
    if ( esp_wifi_sta_get_ap_info(&ap) != ESP_OK )
    {
        return 0;
    }
    s_wifi.rssi    = (int8_t)ap.rssi;
    s_wifi.channel = ap.primary;
    return s_wifi.rssi;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by AO_Radio, which is a file this check does not join to this one */
uint8_t kdrv_wifi_channel_last(void)
{
    return s_wifi.channel;
}

/* No RPC, so no worker-only rule: this is what wifi_publish_state() reads
 * from AO_Radio's own task, between the RPCs that keep it fresh. */
int8_t kdrv_wifi_rssi_last(void)
{
    return s_wifi.rssi;
}

/* The same kind of read, for the question the state enum cannot answer:
 * IDLE covers a station that was never started and one that is up on no
 * network, and only the second is worth a stop job. */
bool kdrv_wifi_started(void)
{
    return s_wifi.started;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by the kdrv_radio worker, which is a file this check does not join to this one */
kode_status_t kdrv_wifi_power_save(bool on)
{
    wifi_ps_type_t mode = WIFI_PS_NONE;
    esp_err_t      err;

    if ( !s_wifi.started )
    {
        return KODE_ERROR;
    }
    if ( on )
    {
        mode = WIFI_PS_MIN_MODEM;
    }
    err = esp_wifi_set_ps(mode);
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "set_ps: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by the host suite too, which is a file this check does not join to this one */
kdrv_wifi_fail_t kdrv_wifi_fail_of(uint16_t reason)
{
    kdrv_wifi_fail_t fail = KDRV_WIFI_FAIL_OTHER;

    switch ( reason )
    {
        case (uint16_t)WIFI_REASON_AUTH_FAIL:
        case (uint16_t)WIFI_REASON_HANDSHAKE_TIMEOUT:
        case (uint16_t)WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case (uint16_t)WIFI_REASON_MIC_FAILURE: {
            fail = KDRV_WIFI_FAIL_PASSWORD;
            break;
        }
        case (uint16_t)WIFI_REASON_NO_AP_FOUND:
        case (uint16_t)WIFI_REASON_NO_AP_FOUND_IN_RSSI_THRESHOLD:
        case (uint16_t)WIFI_REASON_BEACON_TIMEOUT: {
            fail = KDRV_WIFI_FAIL_NOT_FOUND;
            break;
        }
        default: {
            break;
        }
    }
    return fail;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by AO_Radio, which is a file this check does not join to this one */
kdrv_wifi_fail_t kdrv_wifi_fail_last(void)
{
    return s_wifi.fail;
}

/**
 * @brief Keep the reason of a disconnect, if none is kept yet.
 * @param data The event's payload, as the stack posted it; NULL says nothing.
 */
static void wifi_fail_note(void const *data)
{
    if ( KDRV_WIFI_FAIL_NONE == s_wifi.fail )
    {
        s_wifi.fail   = KDRV_WIFI_FAIL_OTHER;
        s_wifi.reason = 0U;
        if ( NULL != data )
        {
            /* cppcheck-suppress misra-c2012-11.5 ; IDF posts the disconnect's payload untyped */
            wifi_event_sta_disconnected_t const * const ev = (wifi_event_sta_disconnected_t const *)data;

            s_wifi.reason = (uint16_t)ev->reason;
            s_wifi.fail   = kdrv_wifi_fail_of(s_wifi.reason);
        }
    }
}

/** @brief The log's word for kdrv_wifi_fail_last(). */
static const char *wifi_fail_word(void)
{
    const char *word = "no reason given";

    switch ( s_wifi.fail )
    {
        case KDRV_WIFI_FAIL_PASSWORD:  { word = "wrong password"; break; }
        case KDRV_WIFI_FAIL_NOT_FOUND: { word = "not found, or out of range"; break; }
        case KDRV_WIFI_FAIL_OTHER:     { word = "other failure"; break; }
        default:                       { break; }
    }
    return word;
}

/** Runs on the system event task: record and release the waiter. */
static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if ( WIFI_EVENT == base )
    {
        if ( WIFI_EVENT_STA_DISCONNECTED == id )
        {
            /* Every step off a network asked for moves the state first, so an
             * address still held here is one nobody asked to give up. */
            bool const lost = (KDRV_WIFI_GOT_IP == s_wifi.state);

            wifi_fail_note(data);
            s_wifi.state = KDRV_WIFI_DISCONNECTED;
            s_wifi.ip4   = 0U;
            s_wifi.rssi  = 0;   /* stale strength for a network we just left */
            s_wifi.channel = 0U;
            if ( NULL != s_wifi.bits )
            {
                (void)xEventGroupSetBits(s_wifi.bits, WIFI_BIT_DISCONNECTED);
            }
            /* No reconnect here: whether anyone still wants the network is AO_Radio's call. */
            if ( lost )
            {
                /* The stack's own reason: a beacon timeout is range or the
                 * access point, a deauthentication is the network's doing. */
                KLOG_W(TAG, "the network dropped (reason %u)", (unsigned)s_wifi.reason);
                if ( NULL != s_wifi.on_lost )
                {
                    s_wifi.on_lost();
                }
            }
        }
        else if ( WIFI_EVENT_SCAN_DONE == id )
        {
            if ( NULL != s_wifi.bits )
            {
                (void)xEventGroupSetBits(s_wifi.bits, WIFI_BIT_SCAN_DONE);
            }
        }
        else
        {
            /* Not one this driver waits on. */
        }
    }
    else if ( IP_EVENT == base )
    {
        if ( IP_EVENT_STA_GOT_IP == id )
        {
            ip_event_got_ip_t const * const ev = (ip_event_got_ip_t *)data;
            s_wifi.ip4   = ev->ip_info.ip.addr;
            s_wifi.state = KDRV_WIFI_GOT_IP;
            s_wifi.fail  = KDRV_WIFI_FAIL_NONE;
            if ( NULL != s_wifi.bits )
            {
                (void)xEventGroupSetBits(s_wifi.bits, WIFI_BIT_GOT_IP);
            }
        }
    }
    else
    {
        /* Not ours. */
    }
}

/**
 * @brief Bring the remote station up: event group, netif layer, handlers,
 *        init, mode, start.
 *
 * @param with_netif Create the default STA netif between set_mode and start
 *                   (once; an existing one is kept). A netif made after
 *                   esp_wifi_start() never hears STA_START, and making it
 *                   with the stack stopped and started around it is a stop
 *                   on the C5 - which drops an association it holds. An
 *                   adopt passes true; the scan path passes false and keeps
 *                   its netif for the first join.
 * @return KODE_OK; KODE_NO_RESOURCES for the event group; KODE_ERROR when a
 *         stack call refused.
 */
static kode_status_t wifi_bring_up(bool with_netif)
{
    if ( NULL == s_wifi.bits )
    {
        s_wifi.bits = xEventGroupCreate();
        if ( NULL == s_wifi.bits )
        {
            return KODE_NO_RESOURCES;
        }
    }

    /* esp_netif_init() is safe without any netif being created: it only
     * brings the layer up, and the STA netif itself waits for a connect. */
    esp_err_t err = esp_netif_init();
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }

    if ( !s_wifi.handlers )
    {
        (void)esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                  &wifi_event, NULL, NULL);
        (void)esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                  &wifi_event, NULL, NULL);
        s_wifi.handlers = true;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if ( ESP_OK != err )
    {
        /* On this board that means the C5 did not answer the RPC. */
        KLOG_E(TAG, "esp_wifi_init: %s (the C5 did not answer)",
               esp_err_to_name(err));
        return KODE_ERROR;
    }
    if ( esp_wifi_set_mode(WIFI_MODE_STA) != ESP_OK )
    {
        KLOG_E(TAG, "the remote stack refused to start");
        (void)esp_wifi_deinit();
        return KODE_ERROR;
    }
    if ( with_netif && !s_wifi.netif )
    {
        if ( esp_netif_create_default_wifi_sta() == NULL )
        {
            KLOG_E(TAG, "no STA netif");
            (void)esp_wifi_deinit();
            return KODE_ERROR;
        }
        s_wifi.netif = true;
    }
    if ( esp_wifi_start() != ESP_OK )
    {
        KLOG_E(TAG, "the remote stack refused to start");
        (void)esp_wifi_deinit();
        return KODE_ERROR;
    }

    s_wifi.started = true;
    s_wifi.state   = KDRV_WIFI_IDLE;
    KLOG_I(TAG, "remote WiFi up (STA, on the C5's radio)");
    return KODE_OK;
}

kode_status_t kdrv_wifi_start(void)
{
    if ( s_wifi.started )
    {
        return KODE_OK;
    }
    return wifi_bring_up(false);
}

kode_status_t kdrv_wifi_stop(void)
{
    if ( !s_wifi.started )
    {
        return KODE_OK;
    }

    /* Step by step, and each step says so before it runs. Tearing the remote
     * stack down while a station is associated is the longest chain of RPCs
     * in the system - disconnect, stop, deinit, each one a round trip to a
     * coprocessor that is also handling traffic - and when it went wrong the
     * board stopped answering with no clue as to which call it died in.
     * These lines are the clue. */
    KLOG_I(TAG, "stopping the remote stack: disconnect");
    s_wifi.state = KDRV_WIFI_IDLE;   /* asked for: not a network lost */
    (void)esp_wifi_disconnect();
    /* Let the disconnect reach the netif before the driver under it goes:
     * that is the handler which unhooks lwip from the receive path. */
    khal_delay_ms(200U);

    KLOG_I(TAG, "stopping the remote stack: stop");
    (void)esp_wifi_stop();
    khal_delay_ms(100U);

    KLOG_I(TAG, "stopping the remote stack: deinit");
    (void)esp_wifi_deinit();

    KLOG_I(TAG, "remote stack stopped");
    s_wifi.started  = false;
    s_wifi.state    = KDRV_WIFI_IDLE;
    s_wifi.ip4      = 0U;
    ssid_set(NULL);
    s_wifi.rssi     = 0;
    s_wifi.channel  = 0U;
    s_wifi.count    = 0U;
    s_wifi.head     = 0U;
    s_wifi.tail     = 0U;
    /* The netif is deliberately left alive: destroying and recreating it
     * costs another round of RPCs for nothing, and it is inert while the
     * stack is stopped. Being already there is also what makes the next
     * start correct, since it will be in place when STA_START fires. */
    return KODE_OK;
}

void kdrv_wifi_forget(void)
{
    s_wifi.started = false;
    s_wifi.state   = KDRV_WIFI_IDLE;
    s_wifi.ip4     = 0U;
    ssid_set(NULL);
    s_wifi.rssi    = 0;
    s_wifi.channel = 0U;
    s_wifi.count   = 0U;
    s_wifi.head    = 0U;
    s_wifi.tail    = 0U;
}

/**
 * @brief Hand @p ap to the owner, or count it lost.
 *
 * @param ap The network.
 */
static void list_offer(const kdrv_wifi_ap_t *ap)
{
    uint8_t const next = (uint8_t)((s_wifi.head + 1U) % KDRV_WIFI_LIST_MAX);

    if ( next == s_wifi.tail )
    {
        ++s_wifi.dropped;
        return;
    }
    s_wifi.ring[s_wifi.head] = *ap;
    s_wifi.head = next;
}

/**
 * @brief Keep @p ap if the table has room, or if it beats the weakest entry.
 *
 * The table is what autojoin and the production row read after the sweep, and
 * for both of them the question is which networks are actually reachable - so
 * when it is full the strongest wins, rather than whichever channel happened
 * to be swept first.
 *
 * @param ap The network.
 */
static void table_offer(const kdrv_wifi_ap_t *ap)
{
    if ( KDRV_WIFI_SCAN_MAX > s_wifi.count )
    {
        s_wifi.aps[s_wifi.count] = *ap;
        ++s_wifi.count;
        return;
    }

    uint8_t weakest = 0U;
    for ( uint8_t i = 1U; i < s_wifi.count; ++i )
    {
        if ( s_wifi.aps[i].rssi < s_wifi.aps[weakest].rssi )
        {
            weakest = i;
        }
    }
    if ( ap->rssi > s_wifi.aps[weakest].rssi )
    {
        s_wifi.aps[weakest] = *ap;
    }
}

/**
 * @brief The 2.4 GHz channels the slave's country opens.
 *
 * @param first Where the range starts.
 * @param count How many channels it holds.
 */
static void scan_band24_range(uint8_t *first, uint8_t *count)
{
    wifi_country_t country;
    if ( esp_wifi_get_country(&country) == ESP_OK )
    {
        uint16_t const last = (uint16_t)country.schan + (uint16_t)country.nchan;
        if ( (1U <= country.schan) && (1U <= country.nchan) && (15U >= last) )
        {
            *first = country.schan;
            *count = country.nchan;
            return;
        }
    }
    /* The RPC failed, or answered something that is not a channel range.
     * 1..13 is the widest the lower band goes, and a channel the slave does
     * not allow costs one refused start rather than a broken sweep. */
    *first = 1U;
    *count = 13U;
}

/** @brief Upper-band bitmap slots per step: 28 in four steps. */
#define SCAN_5G_PER_STEP  (KDRV_WIFI_SCAN_5G_BITS / KDRV_WIFI_SCAN_5G_STEPS)

/**
 * @brief Channels per lower-band step: the country's range split three
 *        ways, rounded up, so 13 channels go 5-5-3 and 11 go 4-4-3.
 * @param n24 How many channels the lower band holds.
 * @return Channels per step, at least one.
 */
static uint8_t scan_band24_per_step(uint8_t n24)
{
    return (uint8_t)((n24 + (KDRV_WIFI_SCAN_24_STEPS - 1U))
                     / KDRV_WIFI_SCAN_24_STEPS);
}

/**
 * @brief How many lower-band steps the range actually needs: three, or fewer
 *        when the range is too small to split that far.
 * @param n24 How many channels the lower band holds.
 * @return Steps, at least one.
 */
static uint16_t scan_band24_steps(uint8_t n24)
{
    uint8_t const per = scan_band24_per_step(n24);
    return (uint16_t)((n24 + per - 1U) / per);
}

/**
 * @brief Configure sweep step @p step: a group of channels, active, long dwell.
 *
 * Both bands go through the bitmap. Bit 0 of each band's field is that band's
 * bypass, so the band not being asked for is switched off whole. In the lower
 * band bit N stands for channel N, and the country's range is split into
 * KDRV_WIFI_SCAN_24_STEPS groups; in the upper band the 28 slots go seven at a
 * time, and slots the country closes are dropped by the stack, which is why
 * this can cover all 28 without knowing where the board is.
 *
 * @param step   Which step, from 0.
 * @param first  The lower band's first channel.
 * @param n24    How many channels the lower band holds.
 * @param cfg    Filled in.
 */
static void scan_step_config(uint16_t step, uint8_t first, uint8_t n24,
                             wifi_scan_config_t *cfg)
{
    (void)memset(cfg, 0, sizeof(*cfg));
    /* Active, a beacon interval and a half at most: 200-500 ms measured the
     * same networks by name in 36.7 s that this does in 20.6 s, and a sweep
     * takes the C5 off its network's channel. channel stays 0, which is what
     * lets the bitmap apply at all. */
    cfg->show_hidden          = true;
    cfg->scan_type            = WIFI_SCAN_TYPE_ACTIVE;
    cfg->scan_time.active.min = 60;
    cfg->scan_time.active.max = 150;

    uint16_t const steps24 = scan_band24_steps(n24);
    if ( step < steps24 )
    {
        uint8_t const per = scan_band24_per_step(n24);
        uint8_t const lo  = (uint8_t)(first + ((uint8_t)step * per));
        uint8_t const end = (uint8_t)(first + n24);
        uint8_t       hi  = (uint8_t)(lo + per);
        uint16_t      mask = 0U;
        if ( hi > end )
        {
            hi = end;
        }
        for ( uint8_t ch = lo; ch < hi; ++ch )
        {
            mask |= (uint16_t)((uint16_t)1U << ch);
        }
        cfg->channel_bitmap.ghz_2_channels = mask;
        cfg->channel_bitmap.ghz_5_channels = 1U;
        return;
    }

    uint16_t const group = (uint16_t)(step - steps24);
    uint16_t const lo    = (uint16_t)((group * SCAN_5G_PER_STEP) + 1U);
    uint32_t       mask  = 0U;
    for ( uint16_t bit = lo; (lo + SCAN_5G_PER_STEP) > bit; ++bit )
    {
        mask |= (uint32_t)1U << bit;
    }
    cfg->channel_bitmap.ghz_2_channels = 1U;
    cfg->channel_bitmap.ghz_5_channels = mask;
}

/**
 * @brief Run one step: start it, wait for it, keep what it heard.
 *
 * @param cfg   The step's configuration.
 * @param heard How many networks the stack reported.
 * @param kept  How many of them were actually read off it.
 * @return KODE_OK when the step answered; KODE_TIMEOUT when its SCAN_DONE
 *         never arrived; KODE_ERROR when the stack refused the step or would
 *         not say how many networks it found.
 */
static kode_status_t scan_one_step(const wifi_scan_config_t *cfg,
                                   uint16_t *heard, uint16_t *kept)
{
    *heard = 0U;
    *kept  = 0U;

    (void)xEventGroupClearBits(s_wifi.bits, WIFI_BIT_SCAN_DONE);
    /* Waits on SCAN_DONE, not the blocking RPC form, whose 30 s timeout a long dwell outlasts. */
    esp_err_t const err = esp_wifi_scan_start(cfg, false);
    if ( ESP_OK != err )
    {
        /* Quiet on purpose: a step the country closes whole is refused,
         * and error lines for that would say nothing the sweep's own
         * summary does not. */
        KLOG_D(TAG, "scan: step refused: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }

    EventBits_t const got = xEventGroupWaitBits(
        s_wifi.bits, WIFI_BIT_SCAN_DONE, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(KDRV_WIFI_SCAN_STEP_TIMEOUT_MS));
    if ( (got & WIFI_BIT_SCAN_DONE) == 0U )
    {
        /* A warning: the sweep goes on to its summary with what the steps
         * before this one heard, and only a sweep that ran no step at all
         * is an error (kdrv_wifi_scan()). */
        KLOG_W(TAG, "scan: no SCAN_DONE in %u ms; stopping it",
               (unsigned)KDRV_WIFI_SCAN_STEP_TIMEOUT_MS);
        (void)esp_wifi_scan_stop();
        return KODE_TIMEOUT;
    }

    uint16_t found = 0U;
    if ( esp_wifi_scan_get_ap_num(&found) != ESP_OK )
    {
        KLOG_E(TAG, "the C5 did not report how many networks it found");
        return KODE_ERROR;
    }
    *heard = found;

    /* One record at a time into a stack local: the full table is ~2.5 KB per
     * record and this runs on a worker task, not a heap budget.
     * esp_wifi_scan_get_ap_record() pops them in order. */
    for ( uint16_t i = 0U; i < found; ++i )
    {
        wifi_ap_record_t rec;
        if ( esp_wifi_scan_get_ap_record(&rec) != ESP_OK )
        {
            break;
        }
        kdrv_wifi_ap_t ap;
        (void)snprintf(ap.ssid, sizeof(ap.ssid), "%s", (char *)rec.ssid);
        ap.rssi     = rec.rssi;
        ap.channel  = rec.primary;
        ap.authmode = (uint8_t)rec.authmode;
        (void)memcpy(ap.bssid, rec.bssid, sizeof(ap.bssid));
        *kept += 1U;
        list_offer(&ap);
        table_offer(&ap);
    }

    /* Whatever was not read still occupies the C5's buffer; clearing it is
     * what lets the next step start from nothing. */
    esp_wifi_clear_ap_list();
    return KODE_OK;
}

/**
 * @brief Take the two scan tables from PSRAM, once.
 *
 * About 5 KB, which internal RAM has no reason to hold: only the radio worker
 * writes them and only the owner's thread reads them, never an interrupt.
 *
 * @return true when the tables exist.
 */
static bool scan_tables(void)
{
    if ( NULL == s_wifi.aps )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        kdrv_wifi_ap_t * const blk = (kdrv_wifi_ap_t *)khal_alloc_psram(
            WIFI_TABLE_ALIGN,
            ((size_t)KDRV_WIFI_SCAN_MAX + (size_t)KDRV_WIFI_LIST_MAX)
                * sizeof(kdrv_wifi_ap_t));
        if ( NULL != blk )
        {
            s_wifi.ring = &blk[KDRV_WIFI_SCAN_MAX];
            s_wifi.aps  = blk;
        }
    }
    return NULL != s_wifi.aps;
}

kode_status_t kdrv_wifi_scan(void)
{
    kode_status_t const rc = kdrv_wifi_start();
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( !scan_tables() )
    {
        KLOG_E(TAG, "scan: PSRAM refused the scan tables");
        return KODE_NO_RESOURCES;
    }

    /* A join readied for one band and never awaited, because the C5's vault
     * refused it, leaves the radio on that band. A sweep covers both. */
    wifi_band_aim(KDRV_WIFI_CHANNEL_ANY);

    /* Before the radio is even asked, so a failed sweep cannot leave the last
     * room's networks readable as if they were this one's - the ring
     * included, or a listing would open with somebody else's leftovers. */
    s_wifi.count   = 0U;
    s_wifi.head    = 0U;
    s_wifi.tail    = 0U;
    s_wifi.dropped = 0U;

    uint8_t first24 = 1U;
    uint8_t n24     = 13U;
    scan_band24_range(&first24, &n24);

    uint16_t const steps   = (uint16_t)(scan_band24_steps(n24)
                                        + KDRV_WIFI_SCAN_5G_STEPS);
    uint32_t const t0      = khal_time_ms();
    uint16_t       heard   = 0U;   /* everything the sweep was told about  */
    uint16_t       total   = 0U;   /* everything it actually read off      */
    uint16_t       refused = 0U;   /* steps the stack would not even start */
    uint16_t       ran     = 0U;
    kode_status_t  out     = KODE_OK;
    bool           cut     = false;   /* ended by kdrv_wifi_scan_abort()      */

    for ( uint16_t step = 0U; step < steps; ++step )
    {
        if ( s_scan_abort )
        {
            KLOG_N(TAG, "scan: ended early at step %u of %u, as its owner asked",
                   (unsigned)step, (unsigned)steps);
            cut = true;
            out = KODE_DENIED;
            break;
        }
        if ( (khal_time_ms() - t0) >= KDRV_WIFI_SCAN_TIMEOUT_MS )
        {
            KLOG_W(TAG, "scan: the sweep's %u s are up at step %u of %u",
                   (unsigned)(KDRV_WIFI_SCAN_TIMEOUT_MS / 1000U),
                   (unsigned)step, (unsigned)steps);
            out = KODE_TIMEOUT;
            break;
        }

        wifi_scan_config_t cfg;
        scan_step_config(step, first24, n24, &cfg);

        uint16_t            this_step = 0U;
        uint16_t            this_kept = 0U;
        kode_status_t const step_rc = scan_one_step(&cfg, &this_step,
                                                    &this_kept);
        if ( KODE_TIMEOUT == step_rc )
        {
            /* A radio that stopped answering will not answer the next
             * step either, and the results so far are still results. */
            out = KODE_TIMEOUT;
            break;
        }
        if ( KODE_OK != step_rc )
        {
            /* A group this board may not use at all. Counted, not fatal:
             * the upper band is covered without knowing which slots are
             * open. */
            ++refused;
            continue;
        }

        ++ran;
        heard += this_step;
        total += this_kept;

        /* Knock only when there is something new to hand over. The sweep
         * does NOT stop at a full table: the table keeps the strongest and
         * the listing keeps everything, and a crowded lower band must not be
         * able to hide the upper one. */
        if ( (0U < this_kept) && (NULL != s_wifi.on_more) )
        {
            s_wifi.on_more();
        }
    }

    if ( (0U == ran) && cut )
    {
        return KODE_DENIED;     /* asked to stop before it began: cut, not failed */
    }
    if ( 0U == ran )
    {
        KLOG_E(TAG, "scan: the radio would not take a single step");
        return KODE_ERROR;
    }
    if ( 0U < s_wifi.dropped )
    {
        KLOG_W(TAG, "scan: %u networks never reached the listing; nobody "
                    "emptied the ring in time",
               (unsigned)s_wifi.dropped);
    }

    KLOG_N(TAG, "scan: %u networks (%u listed, %u kept) over %u steps "
                "(%u refused) in %lu ms",
           (unsigned)heard, (unsigned)total, (unsigned)s_wifi.count,
           (unsigned)ran, (unsigned)refused,
           (unsigned long)(khal_time_ms() - t0));
    return out;
}

void kdrv_wifi_scan_abort(bool on)
{
    s_scan_abort = on;
}

/** @brief A block of 5 GHz channels where the radio must hear the channel
 *         before it may transmit (radar avoidance, DFS). */
typedef struct {
    uint8_t first;   /**< First channel of the block. */
    uint8_t last;    /**< Last channel of the block.  */
} wifi_radar_block_t;

/* The two DFS blocks of the 5 GHz band: U-NII-2A (5.25-5.35 GHz) and
 * U-NII-2C (5.47-5.725 GHz). Regulators require a listen-before-talk there,
 * so an active probe on them gets nothing; only a passive dwell hears the
 * beacons. */
static const wifi_radar_block_t WIFI_RADAR_BLOCKS[] = {
    { 52U, 64U },
    { 100U, 144U },
};

/** @brief Entries of WIFI_RADAR_BLOCKS. */
#define WIFI_RADAR_BLOCK_COUNT \
    (sizeof(WIFI_RADAR_BLOCKS) / sizeof(WIFI_RADAR_BLOCKS[0]))

/** @brief Active probes spend at least this long on a channel before they
 *         split it into a wait for answers and a stay, ms. */
#define WIFI_PROBE_SPLIT_MIN_MS   20U

/** @brief The wait for the first probe answer is this part of the dwell. */
#define WIFI_PROBE_MIN_DIVISOR    2U

/**
 * @brief Whether @p ch is a channel the radio may not transmit on before
 *        listening.
 * @param ch A channel number.
 * @return true on a radar channel.
 */
static bool probe_is_radar(uint8_t ch)
{
    for ( size_t i = 0U; WIFI_RADAR_BLOCK_COUNT > i; ++i )
    {
        if ( (ch >= WIFI_RADAR_BLOCKS[i].first) && (ch <= WIFI_RADAR_BLOCKS[i].last) )
        {
            return true;
        }
    }
    return false;
}

/**
 * @brief Fill the scan configuration of one probed channel.
 * @param cfg      Filled in.
 * @param channel  The channel.
 * @param dwell_ms What the caller asked for.
 * @param passive  Whether the channel is listened to passively.
 */
static void probe_config(wifi_scan_config_t *cfg, uint8_t channel,
                         uint16_t dwell_ms, bool passive)
{
    (void)memset(cfg, 0, sizeof(*cfg));
    cfg->channel     = channel;      /* nonzero: the bitmap is not used */
    cfg->show_hidden = true;
    if ( passive )
    {
        cfg->scan_type = WIFI_SCAN_TYPE_PASSIVE;
        if ( KDRV_WIFI_RADAR_DWELL_MS > dwell_ms )
        {
            cfg->scan_time.passive = KDRV_WIFI_RADAR_DWELL_MS;
        }
        else
        {
            cfg->scan_time.passive = dwell_ms;
        }
    }
    else
    {
        cfg->scan_type            = WIFI_SCAN_TYPE_ACTIVE;
        cfg->scan_time.active.max = dwell_ms;
        if ( WIFI_PROBE_SPLIT_MIN_MS < dwell_ms )
        {
            cfg->scan_time.active.min = dwell_ms / WIFI_PROBE_MIN_DIVISOR;
        }
        else
        {
            cfg->scan_time.active.min = dwell_ms;
        }
    }
}

kode_status_t kdrv_wifi_probe(uint8_t channel, uint16_t dwell_ms,
                              kdrv_wifi_probe_t *want, uint8_t n,
                              bool *passive)
{
    if ( NULL == want )
    {
        return KODE_DENIED;
    }
    kode_status_t const rc = kdrv_wifi_start();
    if ( KODE_OK != rc )
    {
        return rc;
    }
    wifi_band_aim(KDRV_WIFI_CHANNEL_ANY);   /* as the sweep does */
    bool const pas = probe_is_radar(channel);
    if ( NULL != passive )
    {
        *passive = pas;
    }
    wifi_scan_config_t cfg;
    probe_config(&cfg, channel, dwell_ms, pas);

    (void)xEventGroupClearBits(s_wifi.bits, WIFI_BIT_SCAN_DONE);
    if ( esp_wifi_scan_start(&cfg, false) != ESP_OK )
    {
        return KODE_DENIED;
    }
    EventBits_t const got = xEventGroupWaitBits(
        s_wifi.bits, WIFI_BIT_SCAN_DONE, pdTRUE, pdFALSE,
        pdMS_TO_TICKS(KDRV_WIFI_SCAN_STEP_TIMEOUT_MS));
    if ( (got & WIFI_BIT_SCAN_DONE) == 0U )
    {
        KLOG_E(TAG, "probe: no SCAN_DONE in %u ms; stopping it",
               (unsigned)KDRV_WIFI_SCAN_STEP_TIMEOUT_MS);
        (void)esp_wifi_scan_stop();
        return KODE_TIMEOUT;
    }

    uint16_t found = 0U;
    if ( esp_wifi_scan_get_ap_num(&found) != ESP_OK )
    {
        esp_wifi_clear_ap_list();
        return KODE_ERROR;
    }
    for ( uint16_t i = 0U; i < found; ++i )
    {
        wifi_ap_record_t rec;

        if ( esp_wifi_scan_get_ap_record(&rec) != ESP_OK )
        {
            break;
        }
        for ( uint8_t k = 0U; k < n; ++k )
        {
            if ( (NULL != want[k].ssid)
                && (strncmp((const char *)rec.ssid, want[k].ssid,
                            KDRV_WIFI_SSID_LEN) == 0)
                && (!want[k].seen || (rec.rssi > want[k].rssi)) )
            {
                want[k].seen    = true;
                want[k].rssi    = (int8_t)rec.rssi;
                want[k].channel = rec.primary;
            }
        }
    }
    esp_wifi_clear_ap_list();
    return KODE_OK;
}

/**
 * @brief The default STA netif, created on the first connect and kept.
 *
 * It has to exist *before* the station starts, and that is not a style
 * preference: the netif does its own setting-up from WIFI_EVENT_STA_START,
 * so one created after esp_wifi_start() is never started at all. lwip then
 * has no input path, DHCP refuses to run, and the first frame the radio
 * receives is delivered through a NULL function pointer - measured as
 * "dhcp client start failed" followed by an instruction access fault at
 * address 0x0.
 *
 * Creating it up front in kdrv_wifi_start() would fix that and break
 * something else: a scan-only firmware would carry the netif whose start-up
 * RPCs are what collided with the blocking scan in the validation repo (the
 * "0 networks" that took three sessions to find). So the netif appears when
 * something actually wants a network, and the stack is stopped and started
 * around its creation - one extra round trip, on the first join only, and
 * the scan path keeps running in the configuration it was validated in.
 */
static kode_status_t wifi_ensure_netif(void)
{
    if ( s_wifi.netif )
    {
        return KODE_OK;
    }

    bool const was_started = s_wifi.started;
    if ( was_started )
    {
        (void)esp_wifi_stop();
    }

    if ( esp_netif_create_default_wifi_sta() == NULL )
    {
        KLOG_E(TAG, "no STA netif");
        if ( was_started )
        {
            (void)esp_wifi_start();   /* leave the stack as we found it */
        }
        return KODE_ERROR;
    }
    s_wifi.netif = true;

    if ( was_started )
    {
        esp_err_t const err = esp_wifi_start();
        if ( ESP_OK != err )
        {
            KLOG_E(TAG, "restart after creating the netif: %s",
                   esp_err_to_name(err));
            s_wifi.started = false;
            return KODE_ERROR;
        }
    }
    return KODE_OK;
}

kode_status_t kdrv_wifi_prepare(const char *ssid, uint8_t channel)
{
    if ( (NULL == ssid) || ('\0' == ssid[0]) )
    {
        return KODE_ERROR;
    }

    kode_status_t rc = kdrv_wifi_start();
    if ( KODE_OK != rc )
    {
        return rc;
    }
    rc = wifi_ensure_netif();
    if ( KODE_OK != rc )
    {
        return rc;
    }

    /* Step off the current network first, if there is one. The stack would
     * do it anyway on the way to the new one - and that disconnect sets the
     * very bit the wait watches, so a perfectly good join came back reported
     * as a refusal (and was not remembered). Leaving deliberately, and
     * letting it settle before the bits are cleared, means the only
     * disconnect the wait can see is one that belongs to the new attempt. */
    if ( (KDRV_WIFI_GOT_IP == s_wifi.state)
        || (KDRV_WIFI_CONNECTING == s_wifi.state) )
    {
        s_wifi.state = KDRV_WIFI_IDLE;   /* asked for: not a network lost */
        (void)esp_wifi_disconnect();
        khal_delay_ms(300U);
    }

    /* Aimed after the step off, so no association is held on the band being
     * left, and before the bits are cleared, so an event the change raises is
     * not taken for the new attempt's. */
    wifi_band_aim(channel);
    (void)xEventGroupClearBits(s_wifi.bits,
                               WIFI_BIT_GOT_IP | WIFI_BIT_DISCONNECTED);
    /* Readied, not connecting: whoever makes the join may still refuse it
     * (the C5's vault), and then no await follows to undo a CONNECTING.
     * kdrv_wifi_await() is where the attempt begins. */
    s_wifi.state   = KDRV_WIFI_IDLE;
    s_wifi.fail    = KDRV_WIFI_FAIL_NONE;
    s_wifi.ip4     = 0U;
    ssid_set(NULL);
    return KODE_OK;
}

kode_status_t kdrv_wifi_await(const char *ssid, uint32_t timeout_ms)
{
    const char *name = "";

    if ( NULL != ssid )
    {
        name = ssid;
    }
    /* The attempt is under way from here. An address the event handler
     * already reported (a join that finished before this wait began) is
     * not overwritten. */
    if ( (xEventGroupGetBits(s_wifi.bits) & (WIFI_BIT_GOT_IP | WIFI_BIT_DISCONNECTED)) == 0U )
    {
        s_wifi.state = KDRV_WIFI_CONNECTING;
    }
    ssid_set(name);

    /* Wait for the lease, not just the association: an IP is what "joined"
     * means to anything that will use this. A disconnect on the way there
     * ends the wait early rather than burning the whole timeout - the usual
     * cause is a wrong password, and the answer should be quick. Bits set
     * before the wait began count: whoever made the join may have finished
     * it already, and prepare() cleared the old ones. */
    EventBits_t const bits =
        xEventGroupWaitBits(s_wifi.bits, WIFI_BIT_GOT_IP | WIFI_BIT_DISCONNECTED,
                            pdFALSE, pdFALSE, pdMS_TO_TICKS(timeout_ms));
    kode_status_t rc = KODE_OK;

    if ( (bits & WIFI_BIT_GOT_IP) != 0U )
    {
        /* The moment you join is the moment your signal is knowable, and
         * this is already the worker with the channel to itself - so take
         * the one sample here. Nothing else fills the cache until somebody
         * asks for a refresh, and a `wifi_info` right after a join was
         * reporting no signal at all for a board demonstrably on a network. */
        (void)kdrv_wifi_rssi();
    }
    else
    {
        (void)esp_wifi_disconnect();
        ssid_set(NULL);
        if ( (bits & WIFI_BIT_DISCONNECTED) != 0U )
        {
            /* The word is what a person can act on; the number is the
             * stack's own reason, which tells a handshake that timed out on a
             * weak signal from one the network turned down. */
            KLOG_W(TAG, "'%s' refused us (%s, reason %u)", name, wifi_fail_word(),
                   (unsigned)s_wifi.reason);
            rc = KODE_DENIED;
        }
        else
        {
            s_wifi.state = KDRV_WIFI_IDLE;   /* no event will say so: nothing happened */
            KLOG_W(TAG, "'%s': no address within %ums", name, (unsigned)timeout_ms);
            rc = KODE_TIMEOUT;
        }
    }

    /* The attempt is over, whatever it came to: the radio has both bands
     * again, so the network may change band and a scan hears both. */
    wifi_band_aim(KDRV_WIFI_CHANNEL_ANY);
    return rc;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by the kdrv_radio worker, which is a file this check does not join to this one */
kode_status_t kdrv_wifi_adopt(uint32_t timeout_ms)
{
    kode_status_t rc;

    if ( s_wifi.started )
    {
        /* Up already (a scan, say): the netif still has to exist, and
         * wifi_ensure_netif() is the way that is done with a stack running.
         * When it has to make one that is a stop and a start, which an
         * association the C5 holds does not survive. */
        if ( !s_wifi.netif )
        {
            KLOG_W(TAG, "adopt: the stack was already up; the station restarts");
        }
        rc = wifi_ensure_netif();
    }
    else
    {
        rc = wifi_bring_up(true);
    }
    if ( KODE_OK != rc )
    {
        return rc;
    }

    /* Nothing is disconnected first - the association is the thing being
     * kept. Only the bits and the fields a previous attempt left are cleared,
     * before the connect, so a lease that answers at once still counts. */
    (void)xEventGroupClearBits(s_wifi.bits,
                               WIFI_BIT_GOT_IP | WIFI_BIT_DISCONNECTED);
    s_wifi.state   = KDRV_WIFI_IDLE;
    s_wifi.ip4     = 0U;
    ssid_set(NULL);

    /* A C5 that is still associated skips the real connect and replays its
     * STA_CONNECTED, which is what starts DHCP on the netif made above. */
    if ( esp_wifi_connect() != ESP_OK )
    {
        KLOG_W(TAG, "adopt: the remote stack refused the connect");
        return KODE_ERROR;
    }

    rc = kdrv_wifi_await("", timeout_ms);
    if ( KODE_OK == rc )
    {
        /* The SSID was never ours to know; the station says what it holds. */
        wifi_ap_record_t ap;
        if ( esp_wifi_sta_get_ap_info(&ap) == ESP_OK )
        {
            ssid_set((const char *)ap.ssid);
        }
    }
    return rc;
}

kode_status_t kdrv_wifi_connect(const char *ssid, const char *pass,
                                uint32_t timeout_ms)
{
    kode_status_t const rc = kdrv_wifi_prepare(ssid, KDRV_WIFI_CHANNEL_ANY);
    if ( KODE_OK != rc )
    {
        return rc;
    }

    wifi_config_t cfg;
    (void)memset(&cfg, 0, sizeof(cfg));

    /* memcpy over the measured length, not snprintf. 802.11 allows a 32-byte
     * SSID, and IDF's field is 32 bytes with no room for a terminator for
     * exactly that reason - so snprintf, which insists on writing one, dropped
     * the last character of a maximum-length name. The board then spent
     * forever failing to join a network that does not exist, and the symptom
     * was "the WiFi does not work" rather than anything about a name. The same
     * holds for a 64-character PSK in a 64-byte field. cfg was zeroed just
     * above, so anything shorter than the field is still terminated. */
    (void)memcpy(cfg.sta.ssid, ssid, strnlen(ssid, sizeof(cfg.sta.ssid)));
    if ( NULL != pass )
    {
        (void)memcpy(cfg.sta.password, pass,
                     strnlen(pass, sizeof(cfg.sta.password)));
    }

    if ( esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK )
    {
        s_wifi.state = KDRV_WIFI_IDLE;
        return KODE_ERROR;
    }

    esp_err_t const err = esp_wifi_connect();
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "connect: %s", esp_err_to_name(err));
        s_wifi.state = KDRV_WIFI_IDLE;
        return KODE_ERROR;
    }
    return kdrv_wifi_await(ssid, timeout_ms);
}

kode_status_t kdrv_wifi_disconnect(void)
{
    if ( !s_wifi.started )
    {
        return KODE_OK;
    }
    s_wifi.state   = KDRV_WIFI_IDLE;   /* asked for: not a network lost */
    (void)esp_wifi_disconnect();
    s_wifi.state   = KDRV_WIFI_IDLE;
    s_wifi.ip4     = 0U;
    ssid_set(NULL);
    s_wifi.rssi    = 0;
    s_wifi.channel = 0U;
    return KODE_OK;
}

/* cppcheck-suppress misra-c2012-8.7 ; called by AO_Radio, which is a file this check does not join to this one */
void kdrv_wifi_keep_station_on_restart(void)
{
    /* The handler is the one wifi_default.c registered, by the same cast IDF
     * registers it with: the list compares pointers, and the signature
     * difference is IDF's, not ours. Absent, the call answers
     * ESP_ERR_INVALID_STATE and changes nothing. */
    /* cppcheck-suppress misra-c2012-11.1 ; the pointer IDF registered, cast the way it casts it */
    (void)esp_unregister_shutdown_handler((shutdown_handler_t)esp_wifi_stop);
}
