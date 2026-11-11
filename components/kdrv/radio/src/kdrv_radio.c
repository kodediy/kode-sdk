/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_radio.c
 * @brief The esp-hosted link to the ESP32-C5, and the jobs that wait on it.
 */
#include "kdrv_radio.h"

#include <string.h>

#include "kdrv_radio_note.h"
#include "kdrv_radio_note_reg.h"

#include "kbsp.h"
#include "kdrv_c5.h"
#include "khal_gpio.h"
#include "khal_time.h"
#if CONFIG_KODEOS_WIFI
#include "kdrv_wifi.h"
#endif
#if CONFIG_KODEOS_BLE
#include "kdrv_ble.h"
#endif
#include "khal_nvs.h"
#include "sdkconfig.h"
#include "kode_log.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_system.h"
#include "soc/lp_system_reg.h"
#include "soc/soc.h"
#include "esp_image_format.h"
#include <stddef.h>          /* offsetof, for the header pins below */
#include "kapp_desc.h"        /* kapp_image_chip(): the header read by offset */

#define TAG                 "kdrv.radio"

/** What one esp_hosted_slave_ota_write() carries. The vendor's own example
 * uses 1500; the API sleeps 10 ms per call, so this is also the pacing knob:
 * a 32 KB storage chunk becomes 22 writes and about a fifth of a second of
 * deliberate breathing room for everything else. */
#define RADIO_OTA_CHUNK     1500U

/** How long CONNECT waits for the C5's RPC layer after the transport is up.
 * The slave reboots on every bring-up (ESP_HOSTED_SLAVE_RESET_ON_EVERY_
 * HOST_BOOTUP) and its whole boot fits comfortably inside this; the value is
 * the one the validation runs waited. */
#define RADIO_CP_INIT_MS    10000U
#define RADIO_GATE_POLL_MS  5U   /**< How often the connect job looks at the gate (kdrv_radio_gate()). */

static struct {
    SemaphoreHandle_t   cp_ready;   /**< Given by the CP_INIT event; made by
                                     *   the first CONNECT.                    */
    kdrv_radio_lost_cb_t lost_cb;   /**< Told that the link is gone.            */
    kdrv_radio_fwver_t  fw;
    volatile bool       fw_known;
    volatile bool       link_up;
    volatile bool       expect_down; /**< A deinit is ours; mask LINK_LOST.     */
    bool                platform_up; /**< NVS + event loop + handler, once.     */
    bool                hosted_live; /**< Between esp_hosted_init() and its
                                      *   deinit: the reset line is not ours.   */
    bool                c5_held;     /**< C5_EN is driven low by this driver.   */
    bool                c5_en_ours;  /**< C5_EN is an output this driver set.   */
    bool                adopted;     /**< The last CONNECT kept a parked C5.    */
    bool                exp_switched; /**< The last CONNECT drove J3's supply. */
    bool                exp_level;   /**< The level it drove, or tried to.      */
} s_radio;


/* The settle after the pulse is esp-hosted's own wait, said once more here
 * so that a change to the vendor value cannot slip past. */
_Static_assert(KDRV_RADIO_C5_BOOT_MS == CONFIG_ESP_HOSTED_SDIO_RESET_DELAY_MS,
               "the C5's boot wait is esp-hosted's SDIO reset delay");

/** The gate in front of the link's first words: see kdrv_radio_gate(). */
static volatile bool s_gate_closed;

void kdrv_radio_gate(bool closed)
{
    s_gate_closed = closed;
}

/** The wish for J3's supply: set by AO_Radio's thread alone, read once by the
 *  CONNECT worker, one byte. */
static volatile bool s_exp_wish;

void kdrv_radio_exp_wish(bool on)
{
    s_exp_wish = on;
}

void kdrv_radio_note_leave(kdrv_radio_note_t target)
{
    kdrv_radio_note_reg_write(kradio_note_pack((uint32_t)target));
}

bool kdrv_radio_adopted(void)
{
    return s_radio.adopted;
}

bool kdrv_radio_exp_switched(void)
{
    return s_radio.exp_switched;
}

bool kdrv_radio_exp_level(void)
{
    return s_radio.exp_level;
}

bool kdrv_radio_link_up(void)
{
    return s_radio.link_up;
}

void kdrv_radio_on_link_lost(kdrv_radio_lost_cb_t cb)
{
    s_radio.lost_cb = cb;
}

/** @brief Tell the owner the link is gone, if it asked to hear. */
static void radio_lost(kode_status_t why)
{
    kdrv_radio_lost_cb_t const cb = s_radio.lost_cb;

    if ( NULL != cb )
    {
        cb(why);
    }
}

kode_status_t kdrv_radio_c5_hold(bool hold)
{
    if ( s_radio.hosted_live )
    {
        return KODE_DENIED;
    }
    /* Configured once per boot and written after that: a second configuration
     * of a pin this driver already drives is one the GPIO driver reports as a
     * conflict, and it would be right to. */
    if ( s_radio.c5_en_ours )
    {
        if ( khal_gpio_write((uint8_t)KBSP_PIN_C5_EN, !hold) != KODE_OK )
        {
            KLOG_E(TAG, "C5_EN would not change");
            return KODE_ERROR;
        }
    }
    else if ( khal_gpio_output((uint8_t)KBSP_PIN_C5_EN, !hold) != KODE_OK )
    {
        KLOG_E(TAG, "C5_EN would not configure");
        return KODE_ERROR;
    }
    else
    {
        s_radio.c5_en_ours = true;
    }
    s_radio.c5_held = hold;
    if ( hold )
    {
        /* A held C5 is off, not parked: whatever note was left is no longer
         * true. */
        kdrv_radio_note_reg_write(0U);
    }
    return KODE_OK;
}

bool kdrv_radio_c5_held(void)
{
    return s_radio.c5_held;
}

/* The C5's enable line is esp-hosted's to drive, from its own Kconfig, and the
 * board says where that line is. Two numbers for one wire: this is what stops
 * them drifting apart, which they once did. */
_Static_assert(CONFIG_ESP_HOSTED_SDIO_GPIO_RESET_SLAVE == (int)KBSP_PIN_C5_EN,
               "esp-hosted resets the C5 on the pin kbsp says is C5_EN");

/* esp-hosted's two transport pools are 88 KB, taken when the link comes up
 * and held until the next reset. By its own default they come out of
 * internal RAM, which leaves under 10 KB of it for everything else on this
 * board. kbsp's sdkconfig.board sends them to PSRAM; a project that does not
 * read it stops here. */
#if !CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM
#error "esp-hosted's transport pools must come from PSRAM on the Kode Dot: read sdk/components/kbsp/sdkconfig.board before your own sdkconfig defaults (CONFIG_ESP_HOSTED_MEMPOOL_PREFER_SPIRAM=y)"
#endif

/* ---- Peer data: the seam to a service on the C5 --------------------------- */

/** @brief Listeners this boot may hold: the infrared block, the KODE
 *         services (kdrv_c5svc) and one spare. */
#define RADIO_PEER_SLOTS 3U

/** @brief One listener, and whether esp-hosted has been told about it. */
typedef struct {
    uint32_t             msg_id;
    kdrv_radio_peer_cb_t cb;
    kdrv_radio_peer_up_t up;
    void                *ctx;
    bool                 registered;
} radio_peer_t;

static radio_peer_t s_peer[RADIO_PEER_SLOTS];

/** @brief esp-hosted's callback shape, turned into this driver's. */
static void radio_peer_trampoline(uint32_t msg_id, const uint8_t *data,
                                  size_t len, void *ctx)
{
    radio_peer_t const * const p = (radio_peer_t const *)ctx;

    (void)msg_id;
    if ( (NULL != p) && (NULL != p->cb) )
    {
        p->cb(data, len, p->ctx);
    }
}

/**
 * @brief The link is up: tell every listener, and hand those not yet
 *        registered to esp-hosted.
 *
 * Only once the link is up: esp-hosted's table takes a mutex from a function
 * table that does not exist before its own init.
 */
static void radio_peer_register_all(void)
{
    for ( size_t i = 0U; RADIO_PEER_SLOTS > i; ++i )
    {
        radio_peer_t * const p = &s_peer[i];

        if ( NULL == p->cb )
        {
            continue;
        }
        if ( NULL != p->up )
        {
            p->up(p->ctx);
        }
        if ( p->registered )
        {
            continue;
        }
        if ( esp_hosted_register_custom_callback(p->msg_id, radio_peer_trampoline,
                                                p) == ESP_OK )
        {
            p->registered = true;
        }
        else
        {
            KLOG_E(TAG, "peer data 0x%08lX could not be listened for",
                   (unsigned long)p->msg_id);
        }
    }
}

kode_status_t kdrv_radio_peer_listen(uint32_t msg_id, kdrv_radio_peer_cb_t cb,
                                     kdrv_radio_peer_up_t up, void *ctx)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    for ( size_t i = 0U; RADIO_PEER_SLOTS > i; ++i )
    {
        if ( NULL == s_peer[i].cb )
        {
            s_peer[i].msg_id     = msg_id;
            s_peer[i].ctx        = ctx;
            s_peer[i].up         = up;
            s_peer[i].registered = false;
            s_peer[i].cb         = cb;
            if ( s_radio.link_up )
            {
                radio_peer_register_all();
            }
            return KODE_OK;
        }
    }
    return KODE_NO_RESOURCES;
}

kode_status_t kdrv_radio_peer_send(uint32_t msg_id, const uint8_t *data,
                                   size_t len)
{
    if ( (NULL == data) || (0U == len) || !s_radio.link_up )
    {
        return KODE_DENIED;
    }
    if ( esp_hosted_send_custom_data(msg_id, data, len) != ESP_OK )
    {
        return KODE_IO_ERROR;
    }
    return KODE_OK;
}

kode_status_t kdrv_radio_fwversion(kdrv_radio_fwver_t *ver)
{
    if ( !s_radio.fw_known || (NULL == ver) )
    {
        return KODE_DENIED;
    }
    *ver = s_radio.fw;
    return KODE_OK;
}

/** Runs on esp-hosted's event task: report, and post nothing heavier. */
static void radio_hosted_event(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    (void)arg; (void)base;
    switch ( id )
    {
        case ESP_HOSTED_EVENT_TRANSPORT_UP:
            KLOG_D(TAG, "SDIO transport up");
            break;
        case ESP_HOSTED_EVENT_CP_INIT:
            /* The slave says why it booted. A fresh unit says power-on, a
             * unit that just took an `install dst=c5` says software; the
             * production row prints it, so it is kept where the rest of
             * the chip's facts live. */
            if ( NULL != data )
            {
                kdrv_c5_note_reset(
                    (int32_t)((const esp_hosted_event_init_t *)data)->reason);
            }
            (void)xSemaphoreGive(s_radio.cp_ready);
            break;
        case ESP_HOSTED_EVENT_TRANSPORT_DOWN:
        case ESP_HOSTED_EVENT_TRANSPORT_FAILURE:
            if ( s_radio.link_up && !s_radio.expect_down )
            {
                s_radio.link_up = false;
                kdrv_c5_forget();
                KLOG_E(TAG, "link to the C5 dropped");
                radio_lost(KODE_ERROR);
            }
            break;
        default:
            break;
    }
}

/* ---- esp-hosted's pools, given back ----------------------------------------
 *
 * esp-hosted 2.12.9 does not give back everything it takes. Each bring-up
 * makes two memory pools - one for the SDIO buffers, one shared by the
 * transport's channels - and each pool makes a mutex. The teardown destroys
 * the SDIO pool and stops there:
 *
 *   - transport_drv_common_mempool_destroy() counts its references down to
 *     zero and then does nothing, so the channels' pool is never freed and
 *     the next bring-up makes another. 40,064 bytes a cycle, in PSRAM on
 *     this board (ESP_HOSTED_MEMPOOL_PREFER_SPIRAM).
 *   - os_mempool_init() stores a new mutex in the one global,
 *     hosted_port_mutex, every time it runs, and nothing ever deletes one.
 *     Two a cycle, 92 bytes each, in internal RAM.
 *
 * Neither is reachable from outside the component, so the two pool calls are
 * wrapped at link time (kdrv's CMakeLists.txt) and this file keeps the list
 * the component does not: every pool made and not yet destroyed, and every
 * mutex the global has held. Once esp_hosted_deinit() has returned nothing
 * of esp-hosted's is running, so whatever is still on the list is given
 * back there, in radio_hosted_deinit() - pools first, because destroying one
 * takes the mutex. Measured with heap tracing around one bring-up and
 * teardown: 40,064 + 44 + 28 bytes of pool and two mutexes left behind
 * before, and nothing of esp-hosted's after.
 *
 * If a later esp-hosted frees its own, the lists are simply found empty.
 */

/** @brief How many of each one bring-up can make, and room to spare. */
#define RADIO_POOLS_MAX  4U

void *__real_hosted_mempool_create(void *config);
void  __real_hosted_mempool_destroy(void *pool);
void *__wrap_hosted_mempool_create(void *config);
void  __wrap_hosted_mempool_destroy(void *pool);

/** @brief esp-hosted's pool mutex: one global, overwritten by every pool. */
extern SemaphoreHandle_t hosted_port_mutex;

static void              *s_pools[RADIO_POOLS_MAX];
static SemaphoreHandle_t  s_pool_mutexes[RADIO_POOLS_MAX];

/**
 * @brief esp-hosted makes a pool: make it, and remember it and its mutex.
 *
 * @param config esp-hosted's hosted_mempool_config_t, passed through.
 * @return The pool, or NULL exactly when the component's own call fails.
 */
void *__wrap_hosted_mempool_create(void *config)
{
    void * const pool = __real_hosted_mempool_create(config);

    for ( size_t i = 0U; (NULL != pool) && (RADIO_POOLS_MAX > i); ++i )
    {
        if ( NULL == s_pools[i] )
        {
            s_pools[i] = pool;
            break;
        }
    }
    for ( size_t i = 0U; RADIO_POOLS_MAX > i; ++i )
    {
        if ( s_pool_mutexes[i] == hosted_port_mutex )
        {
            break;                       /* already on the list */
        }
        if ( NULL == s_pool_mutexes[i] )
        {
            s_pool_mutexes[i] = hosted_port_mutex;
            break;
        }
    }
    return pool;
}

/**
 * @brief esp-hosted destroys a pool: forget it, then let it.
 *
 * @param pool The pool.
 */
void __wrap_hosted_mempool_destroy(void *pool)
{
    for ( size_t i = 0U; RADIO_POOLS_MAX > i; ++i )
    {
        if ( s_pools[i] == pool )
        {
            s_pools[i] = NULL;
        }
    }
    __real_hosted_mempool_destroy(pool);
}

/**
 * @brief esp_hosted_deinit(), and then what it left behind.
 *
 * @return What esp_hosted_deinit() returned.
 */
static esp_err_t radio_hosted_deinit(void)
{
    esp_err_t const err = esp_hosted_deinit();

    for ( size_t i = 0U; RADIO_POOLS_MAX > i; ++i )
    {
        if ( NULL != s_pools[i] )
        {
            __real_hosted_mempool_destroy(s_pools[i]);
            s_pools[i] = NULL;
        }
    }
    for ( size_t i = 0U; RADIO_POOLS_MAX > i; ++i )
    {
        if ( NULL != s_pool_mutexes[i] )
        {
            vSemaphoreDelete(s_pool_mutexes[i]);
            s_pool_mutexes[i] = NULL;
        }
    }
    hosted_port_mutex = NULL;
    return err;
}

/**
 * @brief What esp-hosted needs from the platform, brought up once.
 *
 * NVS because the WiFi stack behind the RPC keeps its calibration there -
 * through khal_nvs, which is the same store the credentials use, so the
 * partition has exactly one piece of code that decides what to do with a
 * damaged one. The default event loop because that is where esp-hosted
 * announces the transport; it may already exist, and "already there" is
 * success.
 */
static kode_status_t radio_platform_init(void)
{
    if ( s_radio.platform_up )
    {
        return KODE_OK;
    }

    /* Made here, before the handler that gives it is registered and before
     * the first CONNECT takes it. */
    if ( NULL == s_radio.cp_ready )
    {
        s_radio.cp_ready = xSemaphoreCreateBinary();
        if ( NULL == s_radio.cp_ready )
        {
            return KODE_NO_RESOURCES;
        }
    }

    if ( khal_nvs_init() != KODE_OK )
    {
        return KODE_ERROR;
    }

    esp_err_t err = esp_event_loop_create_default();
    if ( (ESP_OK != err) && (ESP_ERR_INVALID_STATE != err) )
    {
        KLOG_E(TAG, "event loop: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }

    err = esp_event_handler_instance_register(ESP_HOSTED_EVENT,
                                              ESP_EVENT_ANY_ID,
                                              &radio_hosted_event, NULL, NULL);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "event handler: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }

    s_radio.platform_up = true;
    return KODE_OK;
}

/**
 * Read and erase the handover note, and decide about the C5's reset line.
 *
 * Only the first CONNECT of a boot can find a note: reading erases it, so
 * every later one resets the C5. Runs before esp-hosted takes the line, which
 * is the only time kdrv_radio_c5_hold() is allowed.
 *
 * @param wants_wifi This boot wants Wi-Fi.
 * @return true when the C5 was left running, parked by the last firmware.
 */
static bool radio_c5_decide(bool wants_wifi)
{
    kradio_note_t const note = kradio_note_unpack(kdrv_radio_note_reg_read());
    bool const reset_was_sw  = (esp_reset_reason() == ESP_RST_SW);
    /* A C5 this driver holds in reset is off, whatever the note says. */
    bool const held          = s_radio.c5_held;
#if CONFIG_KODEOS_BOOT_ROLE_OS
    bool const role_is_os    = true;
    uint32_t const mine      = (uint32_t)KDRV_RADIO_NOTE_OS;
#else
    bool const role_is_os    = false;
    uint32_t const mine      = (uint32_t)KDRV_RADIO_NOTE_APP;
#endif

    kdrv_radio_note_reg_write(0U);

    if ( !held &&
        kradio_note_keeps_c5(&note, reset_was_sw, role_is_os, wants_wifi) )
    {
        KLOG_N(TAG, "C5 kept from the last firmware (parked)");
        return true;
    }

    char const *why = "Wi-Fi not wanted";
    if ( held )
    {
        why = "held in reset";
    }
    else if ( KRADIO_NOTE_MAGIC != note.magic )
    {
        why = "no note";
    }
    else if ( !reset_was_sw )
    {
        why = "not a software reset";
    }
    else if ( note.target != mine )
    {
        why = "another role";
    }
    else
    {
        /* Every other reason is spent: the boot does not want Wi-Fi. */
    }
    KLOG_D(TAG, "C5 reset (%s)", why);

    /* esp-hosted's own pulse, run by us while the line is still ours: low to
     * reset, high to let it boot, then the time it needs to answer. */
    bool pulsed = (kdrv_radio_c5_hold(true) == KODE_OK);
    if ( pulsed )
    {
        khal_delay_ms(KDRV_RADIO_C5_PULSE_MS);
        pulsed = (kdrv_radio_c5_hold(false) == KODE_OK);
    }
    if ( pulsed )
    {
        khal_delay_ms(KDRV_RADIO_C5_BOOT_MS);
    }
    else
    {
        KLOG_W(TAG, "the C5 would not take the reset pulse; "
                    "esp-hosted resets it if the card does not answer");
    }
    return false;
}

/**
 * The CONNECT job, start to finish, on the worker.
 *
 * Once the link is up it drives the expansion header's supply enable to the
 * wish as it is then (kdrv_radio_exp_wish()), read once, and keeps the level
 * it drove for kdrv_radio_exp_level(). A reset C5 has let go of the pin and a
 * parked one is not reset, so it is driven either way.
 *
 * @param wants_wifi This boot wants Wi-Fi, so a C5 parked by the last
 *                   firmware may stay as it is.
 */
static kode_status_t radio_do_connect(bool wants_wifi)
{
    s_radio.adopted      = false;
    s_radio.exp_switched = false;

    /* Before anything can ask the C5 anything. */
    kdrv_radio_rpc_init();

    if ( radio_platform_init() != KODE_OK )
    {
        return KODE_ERROR;
    }

    /* Drain a CP_INIT left over from a link that dropped after handshaking. */
    (void)xSemaphoreTake(s_radio.cp_ready, 0U);

    bool const kept = radio_c5_decide(wants_wifi);

    /* From here until the matching esp_hosted_deinit() the reset line is
     * esp-hosted's, and kdrv_radio_c5_hold() is refused. */
    s_radio.hosted_live = true;
    /* And the reset line is esp-hosted's from here. The decision above has
     * already reset the C5 or kept it, and nothing is held any more. The pin
     * stays an output this driver configured: esp-hosted drives it in the
     * same mode and only when the SDIO card does not answer, and the next
     * hold writes it, as a second configuration of a pin already configured
     * is a conflict the GPIO driver reports. */
    s_radio.c5_held    = false;

    esp_err_t err = esp_hosted_init();
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "esp_hosted_init: %s", esp_err_to_name(err));
        s_radio.hosted_live = false;
        return KODE_ERROR;
    }

    /* This is the call that starts the SDIO transport. A C5 that was just
     * reset by the decision above boots, re-asserts its own pins and brings
     * its RPC layer up - which is the CP_INIT we wait for below; a kept C5
     * answers at once. */
    {
        uint32_t const g0 = khal_time_ms();

        while ( s_gate_closed && ((khal_time_ms() - g0) < KDRV_RADIO_GATE_MS) )
        {
            khal_delay_ms(RADIO_GATE_POLL_MS);
        }
    }
    KLOG_D(TAG, "boot: connect_to_slave at t=%lums",
           (unsigned long)khal_time_ms());
    err = esp_hosted_connect_to_slave();
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "connect_to_slave: %s", esp_err_to_name(err));
        (void)radio_hosted_deinit();
        s_radio.hosted_live = false;
        return KODE_ERROR;
    }

    uint32_t const t0 = khal_time_ms();
    if ( xSemaphoreTake(s_radio.cp_ready,
                       pdMS_TO_TICKS(RADIO_CP_INIT_MS)) != pdTRUE )
    {
        KLOG_E(TAG, "no CP_INIT from the C5 in %us - is this board's C5 "
                    "flashed with the slave firmware?",
               (unsigned)(RADIO_CP_INIT_MS / 1000U));
        s_radio.expect_down = true;
        (void)radio_hosted_deinit();
        s_radio.expect_down = false;
        s_radio.hosted_live = false;
        return KODE_TIMEOUT;
    }
    /* How long the C5 took to boot and answer: a reading the production row
     * prints, and the first thing to look at when a unit's bring-up is slow. */
    kdrv_c5_note_link_ms(khal_time_ms() - t0);

    esp_hosted_coprocessor_fwver_t fw;
    if ( esp_hosted_get_coprocessor_fwversion(&fw) == ESP_OK )
    {
        s_radio.fw.major = fw.major1;
        s_radio.fw.minor = fw.minor1;
        s_radio.fw.patch = fw.patch1;
        s_radio.fw_known = true;
        KLOG_N(TAG, "link up; C5 slave firmware v%lu.%lu.%lu",
               (unsigned long)s_radio.fw.major,
               (unsigned long)s_radio.fw.minor,
               (unsigned long)s_radio.fw.patch);
    }
    else
    {
        /* RPC answered CP_INIT but not this? Say so and carry on - the link
         * works, and the version is diagnosis, not machinery. */
        KLOG_W(TAG, "link up, but the version RPC went unanswered");
    }

    /* Never fatal: the link works, and a slave that answers CP_INIT but not
     * its own description is the row's to judge, not a failed bring-up. */
    if ( kdrv_c5_refresh() != KODE_OK )
    {
        KLOG_W(TAG, "link up, but the coprocessor would not describe itself");
    }
    /* Not fatal either: `c5 field="pwr_ext"` reads what the pin holds. The
     * wish is read here, once, so an "on" asked while the link came up is
     * the one driven. */
    s_radio.exp_level = s_exp_wish;
    if ( kdrv_c5_exp_power(s_radio.exp_level) != KODE_OK )
    {
        KLOG_W(TAG, "link up, but J3's supply was not switched");
    }
    else
    {
        s_radio.exp_switched = true;
    }

    s_radio.adopted = kept;
    s_radio.link_up = true;
    radio_peer_register_all();
    return KODE_OK;
}

/** The DISCONNECT job. */
static kode_status_t radio_do_disconnect(void)
{
#if CONFIG_KODEOS_WIFI
    /* The remote stack lives on this link, so it has to be told to stop
     * while there is still a channel to tell it on. Skipped when the link
     * is already gone: those calls would be RPCs into the void, and the
     * forget below covers that case honestly instead. */
    if ( s_radio.link_up )
    {
        (void)kdrv_wifi_stop();
    }
#endif
#if CONFIG_KODEOS_BLE
    /* The controller is the C5's, so it has to be told while the link can
     * still carry the telling. */
    if ( s_radio.link_up )
    {
        (void)kdrv_ble_stop();
    }
#endif

    s_radio.expect_down = true;
    esp_err_t const err = radio_hosted_deinit();
    s_radio.expect_down = false;
    s_radio.hosted_live = false;
    s_radio.link_up  = false;
    s_radio.fw_known = false;
    /* The chip's table goes with the conversation: `c5` after a teardown
     * must say it has no snapshot rather than describe a link that is gone. */
    kdrv_c5_forget();
#if CONFIG_KODEOS_WIFI
    /* Whatever the teardown managed, nothing is connected now. Saying
     * otherwise - a `wifi_info` still reporting an address after the radio
     * was torn down - is the kind of lie that costs someone an afternoon. */
    kdrv_wifi_forget();
#endif
#if CONFIG_KODEOS_BLE
    kdrv_ble_forget();
#endif
    if ( ESP_OK != err )
    {
        KLOG_W(TAG, "esp_hosted_deinit: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    KLOG_I(TAG, "link down");
    return KODE_OK;
}

/**
 * @brief Push one storage chunk into the C5's open OTA session.
 *
 * Broken into RADIO_OTA_CHUNK pieces because that is what the vendor's API
 * expects and because its built-in 10 ms sleep per write is the transfer's
 * pacing: a 32 KB chunk spends about 220 ms here, and the C5 - which is
 * erasing flash on the other end - gets to keep up.
 */
static kode_status_t radio_do_ota_write(const uint8_t *data, uint32_t len)
{
    if ( (NULL == data) || (0U == len) )
    {
        return KODE_ERROR;
    }
    for ( uint32_t off = 0U; off < len; off += RADIO_OTA_CHUNK )
    {
        uint32_t const piece = ((len - off) > RADIO_OTA_CHUNK)
                             ? RADIO_OTA_CHUNK : (len - off);
        esp_err_t const err =
            esp_hosted_slave_ota_write((uint8_t *)&data[off], piece);
        if ( ESP_OK != err )
        {
            KLOG_E(TAG, "slave OTA write: %s", esp_err_to_name(err));
            return KODE_ERROR;
        }
    }
    return KODE_OK;
}

/** Close the session and hand the C5 over to the image just written. */
static kode_status_t radio_do_ota_finish(void)
{
    esp_err_t err = esp_hosted_slave_ota_end();
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "slave OTA end: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    /* Activate is the point of no return on the C5's side: it sets its own
     * boot partition and reboots. Everything after this is a link that is
     * about to drop, which AO_Radio is expecting. */
    err = esp_hosted_slave_ota_activate();
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "slave OTA activate: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }
    /* The C5 is rebooting as of this call, so the link is gone whether or
     * not the transport has noticed yet. Saying so here is what keeps the
     * teardown that follows from trying to hold a conversation with it. */
    s_radio.link_up = false;
    KLOG_I(TAG, "C5 image activated; the coprocessor is rebooting");
    return KODE_OK;
}

/**
 * @brief Judge a service job's outcome - and, when the job failed because
 *        the C5 has stopped answering, tell the owner the link is lost.
 *
 * A C5 that hangs keeps its transport up: esp-hosted sees no drop, every RPC
 * times out, and the link goes on reading "up" over a chip that will not
 * answer again until it is reset. So a failed job asks the C5 one thing it
 * always answers, its firmware version. An answer means the failure was the
 * job's own - a wrong password, a network out of range - and it is returned
 * as it is. Silence means the link is gone in everything but name: it is
 * marked down here, before anything else can send an RPC into it, and the
 * owner is told with KODE_TIMEOUT, which is how it tells a mute C5 from a
 * transport that dropped by itself. The job's own status is returned either
 * way; the owner drops it, knowing the link went.
 *
 * @param st What the job returned.
 * @return @p st.
 */
static kode_status_t radio_answer(kode_status_t st)
{
    if ( (KODE_OK != st) && s_radio.link_up )
    {
        esp_hosted_coprocessor_fwver_t fw;

        if ( esp_hosted_get_coprocessor_fwversion(&fw) != ESP_OK )
        {
            s_radio.link_up = false;
            kdrv_c5_forget();
            KLOG_E(TAG, "the C5 has stopped answering; the link is lost");
            radio_lost(KODE_TIMEOUT);
        }
    }
    return st;
}

/** What the last WIFI_PROBE or BLE_FIND job heard: written by the worker that
 *  ran it, read by AO_Radio through kdrv_radio_probe_result() after the done
 *  event, while the worker is idle. */
static kdrv_radio_probe_result_t s_probe;

void kdrv_radio_probe_result(kdrv_radio_probe_result_t *out)
{
    if ( NULL != out )
    {
        *out = s_probe;
    }
}

#if CONFIG_KODEOS_WIFI
/**
 * @brief The WIFI_PROBE job: one channel, up to two wanted networks.
 * @param req The job.
 * @return What the probe came to.
 */
static kode_status_t radio_do_wifi_probe(const kdrv_radio_req_t *req)
{
    kdrv_wifi_probe_t want[KDRV_RADIO_PROBE_NETS];
    bool              passive = false;

    (void)memset(&s_probe, 0, sizeof(s_probe));
    (void)memset(want, 0, sizeof(want));
    if ( '\0' != req->ssid[0] )
    {
        want[0].ssid = req->ssid;
    }
    if ( '\0' != req->ssid2[0] )
    {
        want[1].ssid = req->ssid2;
    }
    kode_status_t const rc = radio_answer(kdrv_wifi_probe(req->channel, req->dwell_ms,
                                                          want, KDRV_RADIO_PROBE_NETS,
                                                          &passive));
    s_probe.passive = passive;
    for ( uint8_t i = 0U; KDRV_RADIO_PROBE_NETS > i; ++i )
    {
        s_probe.net[i].seen    = want[i].seen;
        s_probe.net[i].rssi    = want[i].rssi;
        s_probe.net[i].channel = want[i].channel;
    }
    return rc;
}

/**
 * @brief The WiFi half of kdrv_radio_run().
 * @param req The job.
 * @param rc  Receives the job's status when it is one of these.
 * @return true when @p req was a WiFi job.
 */
static bool radio_run_wifi(const kdrv_radio_req_t *req, kode_status_t *rc)
{
    bool mine = true;

    switch ( req->type )
    {
        case KDRV_RADIO_JOB_WIFI_SCAN:
            *rc = radio_answer(kdrv_wifi_scan());
            break;
        case KDRV_RADIO_JOB_WIFI_JOIN:
            *rc = radio_answer(kdrv_wifi_connect(req->ssid, req->pass,
                                                 KDRV_RADIO_JOIN_MS));
            break;
        case KDRV_RADIO_JOB_WIFI_PREPARE:
            *rc = radio_answer(kdrv_wifi_prepare(req->ssid));
            break;
        case KDRV_RADIO_JOB_WIFI_AWAIT:
            *rc = radio_answer(kdrv_wifi_await(req->ssid, KDRV_RADIO_JOIN_MS));
            break;
        case KDRV_RADIO_JOB_WIFI_ADOPT:
            *rc = radio_answer(kdrv_wifi_adopt(KDRV_RADIO_JOIN_MS));
            break;
        case KDRV_RADIO_JOB_WIFI_LEAVE:
            *rc = radio_answer(kdrv_wifi_disconnect());
            break;
        case KDRV_RADIO_JOB_WIFI_STOP:
            *rc = kdrv_wifi_stop();
            break;
        case KDRV_RADIO_JOB_WIFI_RSSI:
            /* The measurement lives in kdrv_wifi's cache, not in this
             * outcome: an int8_t dBm value has no honest place inside a
             * kode_status_t, so the job just reports that it ran. */
            (void)kdrv_wifi_rssi();
            *rc = KODE_OK;
            break;
        case KDRV_RADIO_JOB_WIFI_PS:
            *rc = radio_answer(kdrv_wifi_power_save(req->on));
            break;
        case KDRV_RADIO_JOB_WIFI_PROBE:
            *rc = radio_do_wifi_probe(req);
            break;
        default:
            mine = false;
            break;
    }
    return mine;
}
#endif /* CONFIG_KODEOS_WIFI */

#if CONFIG_KODEOS_BLE
/**
 * @brief The BLE_FIND job: one name, up to req.len ms.
 * @param req The job.
 * @return What the listen came to.
 */
static kode_status_t radio_do_ble_find(const kdrv_radio_req_t *req)
{
    kdrv_ble_dev_t dev;
    bool           found = false;

    (void)memset(&s_probe, 0, sizeof(s_probe));
    (void)memset(&dev, 0, sizeof(dev));
    kode_status_t const rc = radio_answer(kdrv_ble_scan_find(req->ssid, req->len,
                                                             &dev, &found));
    if ( found )
    {
        s_probe.found    = true;
        s_probe.ble_rssi = dev.rssi;
        (void)memcpy(s_probe.ble_addr, dev.addr, sizeof(s_probe.ble_addr));
    }
    return rc;
}

/**
 * @brief The BLE half of kdrv_radio_run().
 * @param req The job.
 * @param rc  Receives the job's status when it is one of these.
 * @return true when @p req was a BLE job.
 */
static bool radio_run_ble(const kdrv_radio_req_t *req, kode_status_t *rc)
{
    bool mine = true;

    switch ( req->type )
    {
        case KDRV_RADIO_JOB_BLE_START:
            *rc = radio_answer(kdrv_ble_start());
            break;
        case KDRV_RADIO_JOB_BLE_ADV:
            if ( req->on )
            {
                kdrv_ble_adv_plan(req->itvl, req->len, req->hidden);
            }
            *rc = radio_answer(kdrv_ble_advertise(req->on));
            break;
        case KDRV_RADIO_JOB_BLE_SCAN:
            *rc = radio_answer(kdrv_ble_scan(req->len));
            break;
        case KDRV_RADIO_JOB_BLE_UNPAIR:
            *rc = radio_answer(kdrv_ble_unpair());
            break;
        case KDRV_RADIO_JOB_BLE_FIND:
            *rc = radio_do_ble_find(req);
            break;
        case KDRV_RADIO_JOB_BLE_QUIET:
            *rc = kdrv_ble_quiet();
            break;
        case KDRV_RADIO_JOB_BLE_STOP:
            *rc = kdrv_ble_stop();
            break;
        case KDRV_RADIO_JOB_BLE_RSP: {
            const char *name = NULL;   /* the serial */

            if ( '\0' != req->ssid[0] )
            {
                name = req->ssid;
            }
            *rc = radio_answer(kdrv_ble_set_scan_rsp(name, req->data, (size_t)req->len));
            break;
        }
        default:
            mine = false;
            break;
    }
    return mine;
}
#endif /* CONFIG_KODEOS_BLE */

kode_status_t kdrv_radio_run(const kdrv_radio_req_t *req)
{
    kode_status_t rc = KODE_DENIED;

    if ( NULL == req )
    {
        return KODE_DENIED;
    }
    switch ( req->type )
    {
        case KDRV_RADIO_JOB_CONNECT:
            rc = radio_do_connect(req->wants_wifi);
            break;
        case KDRV_RADIO_JOB_DISCONNECT:
            rc = radio_do_disconnect();
            break;
        case KDRV_RADIO_JOB_EXP_POWER:
            rc = radio_answer(kdrv_c5_exp_power(req->on));
            break;
        case KDRV_RADIO_JOB_AMP:
            rc = radio_answer(kdrv_c5_amp(req->on));
            break;
        case KDRV_RADIO_JOB_C5_BEGIN: {
            esp_err_t const err = esp_hosted_slave_ota_begin();

            if ( ESP_OK != err )
            {
                KLOG_E(TAG, "slave OTA begin: %s", esp_err_to_name(err));
                rc = KODE_ERROR;
            }
            else
            {
                rc = KODE_OK;
            }
            break;
        }
        case KDRV_RADIO_JOB_C5_WRITE:
            rc = radio_do_ota_write(req->data, req->len);
            break;
        case KDRV_RADIO_JOB_C5_FINISH:
            rc = radio_do_ota_finish();
            break;
        case KDRV_RADIO_JOB_C5_ABORT:
            (void)esp_hosted_slave_ota_end();   /* no activate: dropped */
            rc = KODE_OK;
            break;
        case KDRV_RADIO_JOB_C5_INFO:
            rc = radio_answer(kdrv_c5_refresh());
            break;
        default: {
            bool mine = false;

            /* Compiled out with the driver that serves them: a firmware can
             * carry the link (for the coprocessor's own firmware, or BLE)
             * without a network stack, and the jobs must go with the driver
             * that serves them. */
#if CONFIG_KODEOS_WIFI
            mine = radio_run_wifi(req, &rc);
#endif
#if CONFIG_KODEOS_BLE
            if ( !mine )
            {
                mine = radio_run_ble(req, &rc);
            }
#endif
            if ( !mine )
            {
                rc = KODE_DENIED;
            }
            break;
        }
    }
    return rc;
}

/* kapp_image_chip() reads the header by offset so its own header can stay
 * free of ESP-IDF's; this is where the layout it assumes is pinned. */
_Static_assert(KAPP_IMAGE_HDR_LEN == sizeof(esp_image_header_t),
               "kapp_image_chip() reads a header of another size");
_Static_assert(KAPP_IMAGE_CHIP_OFF == offsetof(esp_image_header_t, chip_id),
               "kapp_image_chip() reads the chip id from the wrong offset");
_Static_assert(KAPP_IMAGE_MAGIC == ESP_IMAGE_HEADER_MAGIC,
               "kapp_image_chip() looks for the wrong magic");

bool kdrv_radio_image_header_ok(const uint8_t *head, uint32_t len)
{
    uint16_t chip = 0U;

    if ( kapp_image_chip(head, (size_t)len, &chip) != KODE_OK )
    {
        return false;
    }
    if ( (uint16_t)ESP_CHIP_ID_ESP32C5 != chip )
    {
        KLOG_E(TAG, "that image was built for chip id 0x%04x, not the "
                    "ESP32-C5 (0x%04x)", (unsigned)chip,
               (unsigned)ESP_CHIP_ID_ESP32C5);
        return false;
    }
    return true;
}
