/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_ble.c
 * @brief NimBLE on the P4, driving the controller that lives on the C5.
 */
#include "kdrv_ble.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_hosted.h"
#include "esp_random.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#if CONFIG_KODEOS_BLE_CONSOLE
#include "host/ble_gatt.h"
#include "host/ble_hs_mbuf.h"
#include "host/ble_uuid.h"
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
#include "host/ble_sm.h"
#endif
#include "host/ble_store.h"   /* ble_store_util_status_rr: the full-store rule */
#include "store/config/ble_store_config.h"
/* NimBLE does not declare this in any public header: its own examples
 * (bleprph, ble_uart) declare it at their call site, so we do too. */
extern void ble_store_config_init(void);
#endif

#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
#include "kdrv_ble_tokens.h"   /* the bond store keeps tokens; keys on the C5 */
#include "kdrv_c5svc.h"
#include "kode_wire.h"
#endif
#include "kdrv_ble_heard.h"   /* the ring kdrv_ble_hear()'s scan feeds */
#include "kdrv_ble_app.h"     /* the app's own service: its rules, values and ring */
#include "kdrv_ble_bonds.h"   /* the count of bonded peers, read with no lock */
#include "kdrv_ble_find.h"    /* kdrv_ble_scan_find(): which advert is the one */
#include "kdrv_ble_hid_table.h" /* the HID service: its kinds, report map and ring */
#include "khal_nvs.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "sdkconfig.h"

/* NimBLE's host holds 32 KB in pools and tables, and by its own default it
 * takes them from internal RAM. The controller is the C5's, so none of it
 * is read by a radio interrupt and all of it can live in PSRAM. kbsp's
 * sdkconfig.board says so; a project that does not read it stops here. */
#if !CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL
#error "NimBLE's host memory must come from PSRAM on the Kode Dot: read sdk/components/kbsp/sdkconfig.board before your own sdkconfig defaults (CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL=y)"
#endif

/* A scan and kdrv_ble_hear() read legacy reports (BLE_GAP_EVENT_DISC, 31
 * bytes of payload). With extended advertising NimBLE reports through
 * another event, and both would hear nothing and say nothing. */
#ifdef CONFIG_BT_NIMBLE_EXT_ADV
#error "kdrv_ble reads legacy advertising reports: build with CONFIG_BT_NIMBLE_EXT_ADV=n"
#endif

#define TAG "kdrv.ble"

/** How long to wait for the host and the controller to agree on reality. */
#define BLE_SYNC_MS         5000U

#define BLE_BIT_SYNCED      (1U << 0)
#define BLE_BIT_SCAN_DONE   (1U << 1)
/** @brief The owner asked the listen in flight to end (kdrv_ble_scan_abort()). */
#define BLE_BIT_SCAN_ABORT  (1U << 2)
/** @brief The host task has run kdrv_ble_quiet()'s hang-up. */
#define BLE_BIT_HUNG_UP     (1U << 3)
/** @brief How long kdrv_ble_quiet() gives the host task to run its hang-up
 *         before it hangs up from its own thread. */
#define BLE_HANGUP_MS       50U

/** How long past a scan's duration its completion is waited for, ms. */
#define BLE_SCAN_MARGIN_MS  2000U

#if CONFIG_KODEOS_BLE_CONSOLE
/* ---- The console service's identity --------------------------------------
 * Kode's own UUIDs rather than the Nordic UART Service's, for one reason:
 * clients filter by service UUID when scanning, and "every Kode Dot in range
 * and nothing else" is what a Kode app wants. The characteristic semantics
 * are NUS's exactly - a write pipe and a notify pipe carrying a byte stream -
 * so a generic BLE terminal can still drive it by hand.
 *
 * The first field spells KODE in ASCII; the second is the index.
 * ble_uuid128_t stores wire order, which is the reverse of how a UUID reads:
 *   4b4f4445-0001-4f53-a2d1-7c0e9b5f3a61
 *   -> 61 3a 5f 9b 0e 7c d1 a2 53 4f 01 00 45 44 4f 4b
 */
static const ble_uuid128_t s_svc_uuid = BLE_UUID128_INIT(
    0x61, 0x3a, 0x5f, 0x9b, 0x0e, 0x7c, 0xd1, 0xa2,
    0x53, 0x4f, 0x01, 0x00, 0x45, 0x44, 0x4f, 0x4b);
static const ble_uuid128_t s_rx_uuid = BLE_UUID128_INIT(
    0x61, 0x3a, 0x5f, 0x9b, 0x0e, 0x7c, 0xd1, 0xa2,
    0x53, 0x4f, 0x02, 0x00, 0x45, 0x44, 0x4f, 0x4b);
static const ble_uuid128_t s_tx_uuid = BLE_UUID128_INIT(
    0x61, 0x3a, 0x5f, 0x9b, 0x0e, 0x7c, 0xd1, 0xa2,
    0x53, 0x4f, 0x03, 0x00, 0x45, 0x44, 0x4f, 0x4b);
/* The mirror's pipe exists only where the mirror does. It needs the console
 * service to hang off AND KODEOS_MIRROR to have anything to carry: with the
 * feature out of the build nothing ever calls kdrv_ble_mirror_write(), so
 * the characteristic, its 8 KB ring and its retry callout would all be
 * carried for a producer that does not exist. */
#if CONFIG_KODEOS_BLE_CONSOLE && CONFIG_KODEOS_MIRROR
#define KDRV_BLE_MIRROR_PIPE 1
#else
#define KDRV_BLE_MIRROR_PIPE 0
#endif

#if KDRV_BLE_MIRROR_PIPE
/** Index 0x0004: the mirror's own notify pipe, in the same service. */
static const ble_uuid128_t s_mirror_uuid = BLE_UUID128_INIT(
    0x61, 0x3a, 0x5f, 0x9b, 0x0e, 0x7c, 0xd1, 0xa2,
    0x53, 0x4f, 0x04, 0x00, 0x45, 0x44, 0x4f, 0x4b);
#endif

/* The CCCD does NOT inherit READ_ENC/WRITE_ENC. NimBLE builds a notify
 * characteristic's descriptor permissions from the NOTIFY_INDICATE_* family
 * alone (ble_gatts_chr_clt_cfg_flags_from_chr_flags). Flag the TX
 * characteristic the obvious way and an unpaired stranger subscribes
 * successfully and reads every console line - passwords included - in the
 * clear. These two masks are not interchangeable. */
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/* _ENC and _AUTHEN, both, and not one standing in for the other: NimBLE
 * checks encryption and authentication separately, and a link that is
 * encrypted but unauthenticated satisfies the first while failing the
 * second. _AUTHEN demands a secret a human carried between the two ends,
 * which is what the six digits on the glass are. */
#define CHR_WRITE_ENC   (BLE_GATT_CHR_F_WRITE_ENC \
                       | BLE_GATT_CHR_F_WRITE_AUTHEN)
#define CHR_NOTIFY_ENC  (BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC \
                       | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN)
#define CHR_READ_ENC    (BLE_GATT_CHR_F_READ_ENC \
                       | BLE_GATT_CHR_F_READ_AUTHEN)
#define DSC_READ_ENC    (BLE_ATT_F_READ_ENC | BLE_ATT_F_READ_AUTHEN)
#elif CONFIG_KODEOS_BLE_CONSOLE_PAIRING
/* No screen to show a code on, so the pairing is Just Works: encrypted,
 * bonded, and nobody types anything. That cannot produce an authenticated
 * link, so asking for _AUTHEN here would fail every access with
 * "insufficient authentication". What survives is confidentiality against a
 * listener; what does not is proof of who is on the other end. */
#define CHR_WRITE_ENC   BLE_GATT_CHR_F_WRITE_ENC
#define CHR_NOTIFY_ENC  BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC
#define CHR_READ_ENC    BLE_GATT_CHR_F_READ_ENC
#define DSC_READ_ENC    BLE_ATT_F_READ_ENC
#else
/* Pairing is off. These two masks are the whole of what an unpaired peer is
 * refused, so emptying them opens the service - which is the point, and the
 * paragraph above is what has to be honoured when it comes back. */
#define CHR_WRITE_ENC   0U
#define CHR_NOTIFY_ENC  0U
#define CHR_READ_ENC    0U
#define DSC_READ_ENC    0U
#endif

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/* The app's service takes these same masks (kdrv_ble.h): with a screen to
 * show a code on, every one of its accesses needs the bonded, authenticated
 * link the console's does. */
_Static_assert(((CHR_READ_ENC & BLE_GATT_CHR_F_READ_ENC) != 0U)
               && ((CHR_READ_ENC & BLE_GATT_CHR_F_READ_AUTHEN) != 0U),
               "a read of the app's service needs an encrypted, authenticated link");
_Static_assert(((CHR_WRITE_ENC & BLE_GATT_CHR_F_WRITE_ENC) != 0U)
               && ((CHR_WRITE_ENC & BLE_GATT_CHR_F_WRITE_AUTHEN) != 0U),
               "a write needs an encrypted, authenticated link");
_Static_assert(((CHR_NOTIFY_ENC & BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC) != 0U)
               && ((CHR_NOTIFY_ENC & BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHEN) != 0U),
               "a subscription needs an encrypted, authenticated link");
#endif

/** @brief Client configuration descriptors of GATT's own service: Service Changed. */
#define BLE_GATT_SVC_CCCDS     1U

/** @brief The console's notified characteristics besides the mirror's: TX
 *         (s_chr_defs[1]). */
#define BLE_CONSOLE_TX_CCCDS   1U

/** @brief Client configuration descriptors of the Battery service: Battery Level. */
#define BLE_BATT_CCCDS         1U

/** @brief The most client configuration descriptors one bonded peer can
 *         subscribe: GATT's, the console's, the mirror's pipe, every
 *         characteristic of the app's, each of which may notify, Battery
 *         Level and the HID service's input reports. */
#define BLE_PEER_CCCDS_MAX     (BLE_GATT_SVC_CCCDS + BLE_CONSOLE_TX_CCCDS \
                                + (unsigned)KDRV_BLE_MIRROR_PIPE + KDRV_BLE_APP_CHARS_MAX \
                                + BLE_BATT_CCCDS + KDRV_BLE_HID_CCCDS)

/* NimBLE keeps CONFIG_BT_NIMBLE_MAX_CCCDS subscriptions for all its bonds
 * together (BLE_STORE_MAX_CCCDS), and one that does not fit makes the store
 * unpair the oldest other peer. So the store holds every bond's worst case. */
_Static_assert(((unsigned)CONFIG_BT_NIMBLE_MAX_BONDS * BLE_PEER_CCCDS_MAX)
               <= (unsigned)CONFIG_BT_NIMBLE_MAX_CCCDS,
               "every bond's subscriptions must fit NimBLE's store (sdkconfig.board)");

/* Whoever reads the log should not have to know how the firmware was
 * configured to know who is on the port. */
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
#define CONSOLE_OPEN_NOTE   ""
#else
#define CONSOLE_OPEN_NOTE   " (no pairing)"
#endif

/** One GATT write is copied out through here; the host task is its only user. */
#define BLE_RX_SCRATCH      256U
#endif /* CONFIG_KODEOS_BLE_CONSOLE */

static struct {
    bool               up;          /**< NimBLE started.                    */
    bool               synced;      /**< sync_cb has run: GAP is usable.    */
    uint8_t            own_addr_type;
    uint8_t            addr[6];
    kdrv_ble_state_t   state;
    kdrv_ble_cb_t      cb;
    EventGroupHandle_t bits;
    uint8_t            count;
    kdrv_ble_dev_t    *dev;         /**< KDRV_BLE_SCAN_MAX devices in PSRAM,
                                      *   taken at the first scan. NULL until
                                      *   then, while @c count is 0.        */
    uint8_t            rsp[KDRV_BLE_ADV_MAX]; /**< The scan response, packed. */
    uint8_t            rsp_len;     /**< Its bytes; 0 for none.             */
    bool               rsp_set;     /**< kdrv_ble_set_scan_rsp() has set it;
                                      *   until then the console's default.  */
    char               name[KDRV_BLE_ADV_MAX]; /**< The name it set; "" for the serial. */
    /* Outside the console guard, unlike the three fields below it: hanging
     * up on a central is kdrv_ble_quiet()'s job and that exists whether or
     * not this firmware carries a console. */
    uint16_t           conn;        /**< BLE_HS_CONN_HANDLE_NONE when idle. */
    uint32_t           links;       /**< Connections opened since boot; wraps. */
    bool               secure;      /**< The last link got encrypted, by a bond or a pairing. */
    bool               hearing;     /**< kdrv_ble_hear() has a passive scan
                                     *   running; written by the worker, read
                                     *   by the host task.                   */
    uint16_t           adv_itvl;    /**< The next advert's interval (0.625 ms units); 0 until planned. */
    uint32_t           adv_ms;      /**< How long it stays on the air, ms; 0 for no end. */
    bool               adv_hidden;  /**< The next advert hides the board and no
                                      *   new device may pair; false (the
                                      *   start) is visible.                */
    bool               adv_pulsed;  /**< The next advert goes on the air in
                                      *   pulses (kdrv_ble_adv_plan()).      */
    bool               adv_said;    /**< The log has told an advert.        */
    bool               adv_said_hidden; /**< Which one it told.             */
    bool               adv_said_pulsed; /**< And whether it was pulsed.     */
#if CONFIG_KODEOS_BLE_CONSOLE
    bool               console_on;  /**< A central is subscribed to TX.     */
    uint16_t           tx_drop;     /**< Lines the console ring refused, PLUS
                                      *   notify failures on either pipe -
                                      *   console and mirror share this
                                      *   counter rather than each keeping
                                      *   one, because both report the same
                                      *   fact: the link, not either producer,
                                      *   is what failed.                    */
    uint16_t           tx_retry;    /**< Times a drain found no buffers,
                                      *   its own or the stack's for the
                                      *   notification's header - shared for
                                      *   the same reason as tx_drop above.  */
#if KDRV_BLE_MIRROR_PIPE
    bool               mirror_on;   /**< A central is subscribed to the
                                      *   mirror characteristic.             */
#endif
#endif
} s_ble;

uint8_t kdrv_ble_scan_count(void)
{
    return s_ble.count;
}

bool kdrv_ble_scan_get(uint8_t idx, kdrv_ble_dev_t *out)
{
    if ( (idx >= s_ble.count) || (NULL == out) )
    {
        return false;
    }
    *out = s_ble.dev[idx];
    return true;
}

kdrv_ble_state_t kdrv_ble_state(void)
{
    return s_ble.state;
}

bool kdrv_ble_addr(uint8_t out[6])
{
    if ( !s_ble.synced || (NULL == out) )
    {
        return false;
    }
    (void)memcpy(out, s_ble.addr, sizeof(s_ble.addr));
    return true;
}

uint32_t kdrv_ble_links(void)
{
    return s_ble.links;
}

/* The app's service, found by name: a weak reference, under the deviation
 * kaos.h writes for KAOS_WEAK (kdrv may not include kaos.h, so the attribute
 * is spelled out). An app without KODE_BLE_SERVICE leaves it 0, and nothing
 * here dereferences it without that test.
 *
 * A second deviation, of Rule 8.3 and C11 6.2.7: the declaration and the
 * definition name different types. The app defines it with
 * kode_sys_ble_service_t (or kode.h's kode_ble_service_t), types of a layer
 * above this one that kdrv may not include; this declaration names kdrv's
 * mirror, a distinct struct tag. They are layout-compatible, not compatible
 * types: kode_sys_ble.c and kode_ble.c assert the size and every offset
 * equal, field by field, which is what the reads below rely on. */
extern const kdrv_ble_app_service_t kode_ble_app_service __attribute__((weak));

const kdrv_ble_app_service_t *kdrv_ble_app_declared(void)
{
    return &kode_ble_app_service;
}

/* The Battery service, asked for by KODE_BLE_BATTERY_SERVICE(): a weak
 * reference too, for the same deviation, and one byte whose value nothing
 * reads - its address is the answer. */
extern const uint8_t kode_ble_battery_service __attribute__((weak));

bool kdrv_ble_battery_declared(void)
{
    /* A HID device serves its battery whether or not the app declares it. */
    return (NULL != &kode_ble_battery_service) || (0U != kdrv_ble_hid_kinds());
}

kode_status_t kdrv_ble_set_cb(kdrv_ble_cb_t cb)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    s_ble.cb = cb;
    return KODE_OK;
}

/* The hear hook: written once, by kdrv_ble_set_hear_cb() before the
 * first start, and read from NimBLE's host task like s_ble.cb is. */
static kdrv_ble_hear_cb_t s_hear_cb;

kode_status_t kdrv_ble_set_hear_cb(kdrv_ble_hear_cb_t cb)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    s_hear_cb = cb;
    return KODE_OK;
}

/** Move to @p state and tell whoever asked to be told. */
static void ble_set_state(kdrv_ble_state_t state)
{
    if ( s_ble.state == state )
    {
        return;
    }
    s_ble.state = state;
    if ( NULL != s_ble.cb )
    {
        s_ble.cb(state);
    }
}

/* ---- The pulses of a pulsed advert -----------------------------------------
 * With the advert enabled, the C5's station on a 5 GHz channel does not fetch
 * the frames its access point holds for it; with the advert enabled only for
 * the event, it does. A pulsed plan (kdrv_ble_adv_plan()) therefore goes on the
 * air as a train: the advert is enabled for KDRV_BLE_PULSE_ON_MS, which holds
 * one event, disabled until the interval is over, and enabled again by a
 * callout on NimBLE's host task. The air sees one event per interval, as it
 * does from a continuous advert, and the published state is ADVERTISING from
 * the first pulse to the end of the train.
 *
 * Two threads own the train: the radio worker starts and ends it, the host
 * task ends each pulse and starts the next. The fields below, and the NimBLE
 * call that goes with each change of them, are under s_pulse_lock: a restart
 * that has begun cannot outlive the stop that follows it, and one that comes
 * after the stop finds the train over. The lock is never taken inside a NimBLE
 * lock, and no callback is called with it held.
 */

/** @brief Half the range of khal_time_ms(): the distance past which a time is behind. */
#define BLE_TIME_HALF  0x80000000U

/* The GAP callback every advert starts with; defined with the GAP handlers below. */
static int ble_gap_event(struct ble_gap_event *event, void *arg);

static struct {
    bool                      live;     /**< A train is on: a pulse is enabled, or the next is due.  */
    bool                      on_air;   /**< A pulse is enabled now.                                  */
    struct ble_gap_adv_params params;   /**< What every pulse starts with.                            */
    uint32_t                  leg_ms;   /**< The plan's time on the air, ms; 0 for no end.            */
    uint32_t                  leg_t0;   /**< khal_time_ms() when the first pulse began.               */
    uint32_t                  pulse_t0; /**< khal_time_ms() when the last pulse, or its try, began.   */
    uint32_t                  due;      /**< khal_time_ms() when the next pulse is due.               */
    uint8_t                   refused;  /**< Restarts NimBLE refused in a row.                        */
} s_pulse;

static khal_mutex_t           s_pulse_lock;   /**< Guards s_pulse, and the NimBLE call of each change. */
static struct ble_npl_callout s_pulse_co;     /**< The pause: expires when the next pulse is due.      */

/**
 * @brief The ms from @p now to @p due, on a clock that wraps.
 * @param due The time reached.
 * @param now The present.
 * @return The ms left; 0 once @p due has passed.
 */
static uint32_t pulse_ms_left(uint32_t due, uint32_t now)
{
    uint32_t const left = due - now;
    uint32_t       ms   = 0U;

    if ( BLE_TIME_HALF > left )
    {
        ms = left;
    }
    return ms;
}

/**
 * @brief What kdrv_ble_pulse_next() judges, from the train as it stands.
 * @param now       khal_time_ms().
 * @param timed_out Whether the pulse ended on its own time.
 * @return The input. Called with s_pulse_lock held.
 */
static kdrv_ble_pulse_t pulse_input(uint32_t now, bool timed_out)
{
    kdrv_ble_pulse_t in;

    in.live         = s_pulse.live;
    in.timed_out    = timed_out;
    in.itvl         = s_pulse.params.itvl_min;
    in.leg_ms       = s_pulse.leg_ms;
    in.leg_run_ms   = now - s_pulse.leg_t0;
    in.pulse_run_ms = now - s_pulse.pulse_t0;
    in.refused      = s_pulse.refused;
    return in;
}

/**
 * @brief Arm the pause: its callout expires in @p wait_ms.
 * @param wait_ms The wait, ms.
 * @return true when the callout is armed. Called with s_pulse_lock held.
 */
static bool pulse_arm(uint32_t wait_ms)
{
    s_pulse.due = khal_time_ms() + wait_ms;
    return 0 == ble_npl_callout_reset(&s_pulse_co, ble_npl_time_ms_to_ticks32(wait_ms));
}

/**
 * @brief End the train, if one is on: no pulse starts after this returns.
 *
 * A callout that already fired and is running on the host task waits for the
 * lock, and finds the train over. Any thread may call it.
 */
static void pulse_train_end(void)
{
    if ( NULL != s_pulse_lock )
    {
        khal_mutex_lock(s_pulse_lock);
        s_pulse.live   = false;
        s_pulse.on_air = false;
        if ( s_ble.up )
        {
            /* The callout belongs to the host: after nimble_port_deinit() it
             * is gone, and kdrv_ble_stop() ends the train before that. */
            ble_npl_callout_stop(&s_pulse_co);
        }
        khal_mutex_unlock(s_pulse_lock);
    }
}

/**
 * @brief Take the advert off the air: end the train, then disable the advert.
 *
 * The order is the point: a pulse the train started before it ended is on the
 * air when the advert is disabled, and no other starts after.
 */
static void ble_adv_halt(void)
{
    pulse_train_end();
    (void)ble_gap_adv_stop();
}

/**
 * @brief Start the first pulse of a train. The radio worker's.
 * @param params The advert's parameters, kept for every pulse after it.
 * @return NimBLE's answer: 0, its refusal, or BLE_HS_EALREADY when a train is
 *         on already, which goes on as it is.
 */
static int pulse_begin(const struct ble_gap_adv_params *params)
{
    int rc = BLE_HS_EALREADY;

    khal_mutex_lock(s_pulse_lock);
    if ( !s_pulse.live )
    {
        rc = ble_gap_adv_start(s_ble.own_addr_type, NULL, (int32_t)KDRV_BLE_PULSE_ON_MS,
                               params, &ble_gap_event, NULL);
        if ( 0 == rc )
        {
            uint32_t const now = khal_time_ms();

            s_pulse.params   = *params;
            s_pulse.live     = true;
            s_pulse.on_air   = true;
            s_pulse.leg_ms   = s_ble.adv_ms;
            s_pulse.leg_t0   = now;
            s_pulse.pulse_t0 = now;
            s_pulse.refused  = 0U;
        }
    }
    khal_mutex_unlock(s_pulse_lock);
    return rc;
}

/**
 * @brief A pulse ended (BLE_GAP_EVENT_ADV_COMPLETE): arm the pause for the next,
 *        or say the state is to go IDLE. Runs on NimBLE's host task.
 * @param reason NimBLE's: BLE_HS_ETIMEOUT for a pulse that ran its time.
 * @return true when the state is to go IDLE: no train is on (the advert is a
 *         continuous one, or was stopped), or the train is over. false while
 *         the train goes on.
 */
static bool pulse_ended(int reason)
{
    bool idle = true;

    khal_mutex_lock(s_pulse_lock);
    if ( s_pulse.live )
    {
        uint32_t                  wait = 0U;
        kdrv_ble_pulse_t const    in   = pulse_input(khal_time_ms(), BLE_HS_ETIMEOUT == reason);
        kdrv_ble_pulse_do_t const act  = kdrv_ble_pulse_next(&in, &wait);

        s_pulse.on_air = false;
        if ( (KDRV_BLE_PULSE_NEXT == act) && pulse_arm(wait) )
        {
            idle = false;
        }
        else
        {
            s_pulse.live = false;
        }
    }
    else
    {
        /* no train: the advert was a continuous one, or was stopped */
    }
    khal_mutex_unlock(s_pulse_lock);
    return idle;
}

/**
 * @brief The pause is over: start the next pulse, wait again, or end the train.
 *        The callout's callback; runs on NimBLE's host task.
 * @param ev Unused.
 */
static void pulse_timer(struct ble_npl_event *ev)
{
    bool     idle    = false;
    bool     began   = false;
    bool     first   = false;
    int      refusal = 0;
    uint32_t wait    = 0U;

    (void)ev;
    khal_mutex_lock(s_pulse_lock);
    if ( s_pulse.live && (!s_pulse.on_air) )
    {
        uint32_t const now  = khal_time_ms();
        uint32_t const left = pulse_ms_left(s_pulse.due, now);

        if ( 0U != left )
        {
            /* Fired a tick early, or left over from a train that began
             * again: the pulse is due when it is due. */
            idle = !pulse_arm(left);
        }
        else
        {
            kdrv_ble_pulse_t    in  = pulse_input(now, true);
            kdrv_ble_pulse_do_t act = kdrv_ble_pulse_next(&in, &wait);

            if ( KDRV_BLE_PULSE_NEXT == act )
            {
                /* EALREADY is an advert on the air, which is what this wants. */
                refusal = ble_gap_adv_start(s_ble.own_addr_type, NULL, (int32_t)KDRV_BLE_PULSE_ON_MS,
                                            &s_pulse.params, &ble_gap_event, NULL);
                if ( (0 == refusal) || (BLE_HS_EALREADY == refusal) )
                {
                    refusal           = 0;
                    s_pulse.on_air    = true;
                    s_pulse.pulse_t0  = now;
                    s_pulse.refused   = 0U;
                    began             = true;
                }
                else
                {
                    /* Tried again at the next period. */
                    ++s_pulse.refused;
                    first            = (1U == s_pulse.refused);
                    s_pulse.pulse_t0 = now;
                    in               = pulse_input(now, true);
                    act              = kdrv_ble_pulse_next(&in, &wait);
                    idle             = !((KDRV_BLE_PULSE_NEXT == act) && pulse_arm(wait));
                }
            }
            else
            {
                idle = true;
            }
        }
        if ( idle )
        {
            s_pulse.live   = false;
            s_pulse.on_air = false;
        }
    }
    khal_mutex_unlock(s_pulse_lock);

    if ( began )
    {
        KLOG_D(TAG, "advert pulse");
    }
    if ( first )
    {
        KLOG_W(TAG, "an advert pulse was refused (%d); the next is tried at the next period", refusal);
    }
    if ( idle )
    {
        ble_set_state(KDRV_BLE_IDLE);
    }
}

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
/* ---- Pairing: the code that goes on the glass ---------------------------- */

/* Two threads reach this record. NimBLE's host task writes every phase from
 * the GAP handler; the reader that empties it is somebody else's thread, and
 * that same thread writes REJECTED when a human closes the card. Three words
 * written from two places is exactly the shape that tears, so it is guarded -
 * the way the console ring below already is. */
static khal_mutex_t       s_pair_lock;
static kdrv_ble_pair_t    s_pair;
static kdrv_ble_pair_cb_t s_pair_cb;

/**
 * @brief An exchange is in flight: a code has been shown and nothing has
 *        ended it yet.
 *
 * It is what lets a disconnect tell "the peer walked away in the middle of
 * pairing" from an ordinary hang-up, and those are two different sentences on
 * the card. Written from the host task and from the rejecting thread; a
 * single bool, and the only race either could lose is a duplicate DONE, which
 * the reader drops.
 */
static bool s_pair_live;

/**
 * @brief The pairing on this link was refused, and the peer is not hung up
 *        on yet.
 *
 * kdrv_ble_pair_reject() sets it at once, and the hang-up waits for the
 * radio's worker (kdrv_ble_pair_hang_up()). A peer that finishes typing the
 * code in between is undone by the host task (ble_pair_refused_undo()). A new
 * code on the glass and the link's end clear it: the next pairing is not the
 * one refused. A single bool, written as s_pair_live is.
 */
static bool s_pair_refused;

/** @brief Pick the six digits the peer will be asked to type. */
static uint32_t ble_passkey_pick(void)
{
    /* Reducing the hardware RNG's 32-bit uniform into a range of a million
     * favours the low values by about one part in ten thousand. That is far
     * below anything that matters for a number which lives for the security
     * manager's thirty seconds and is never reused. */
    return (uint32_t)(esp_random() % 1000000U);
}

/**
 * @brief Record where a pairing is and nudge whoever asked to be told.
 *
 * @param phase   SHOW, DONE or REFUSED.
 * @param outcome How it ended, or why it was refused; ignored in SHOW.
 * @param passkey The six digits; ignored in DONE.
 */
static void pair_note(kdrv_ble_pair_phase_t phase,
                      kdrv_ble_pair_outcome_t outcome,
                      uint32_t passkey)
{
    if ( NULL == s_pair_lock )
    {
        return;
    }
    khal_mutex_lock(s_pair_lock);
    s_pair.phase   = phase;
    s_pair.outcome = outcome;
    s_pair.passkey = passkey;
    khal_mutex_unlock(s_pair_lock);

    if ( NULL != s_pair_cb )
    {
        s_pair_cb();
    }
}

kode_status_t kdrv_ble_set_pair_cb(kdrv_ble_pair_cb_t cb)
{
    if ( NULL == cb )
    {
        return KODE_DENIED;
    }
    s_pair_cb = cb;
    return KODE_OK;
}

bool kdrv_ble_pair_take(kdrv_ble_pair_t *out)
{
    bool taken = false;

    if ( (NULL == out) || (NULL == s_pair_lock) )
    {
        return false;
    }
    khal_mutex_lock(s_pair_lock);
    if ( KDRV_BLE_PAIR_NONE != s_pair.phase )
    {
        *out         = s_pair;
        s_pair.phase = KDRV_BLE_PAIR_NONE;
        taken        = true;
    }
    khal_mutex_unlock(s_pair_lock);
    return taken;
}

kode_status_t kdrv_ble_pair_reject(void)
{
    if ( !s_pair_live )
    {
        return KODE_DENIED;
    }
    /* Refused BEFORE the flag clears: an encryption change the host task
     * handles in between finds one of the two set. And the flag is clear
     * before the hang-up: with it still set, the disconnect would report
     * GONE - the peer walking away - for something this board did on
     * purpose. */
    s_pair_refused = true;
    s_pair_live    = false;
    KLOG_N(TAG, "pairing refused here");
    pair_note(KDRV_BLE_PAIR_DONE, KDRV_BLE_PAIR_REJECTED, 0U);
    return KODE_OK;
}

kode_status_t kdrv_ble_pair_hang_up(void)
{
    kode_status_t rc = KODE_DENIED;

    /* Nothing to hang up on once the link ended, a new code replaced the
     * refused one, or the host task undid the pairing itself. */
    if ( s_pair_refused )
    {
        s_pair_refused = false;
        if ( BLE_HS_CONN_HANDLE_NONE != s_ble.conn )
        {
            (void)ble_gap_terminate(s_ble.conn, BLE_ERR_REM_USER_CONN_TERM);
        }
        rc = KODE_OK;
    }
    return rc;
}
#endif /* CONFIG_KODEOS_BLE_PAIRING_PASSKEY */

#if CONFIG_KODEOS_BLE_CONSOLE
/* ---- The console service (host task) ------------------------------------- */

static uint16_t s_tx_val_handle;
#if KDRV_BLE_MIRROR_PIPE
static uint16_t s_mirror_val_handle;
#endif
/* Up to three characteristics (RX, TX, and the mirror's when KODEOS_MIRROR
 * is in the build) plus the {0} terminator - growing this without growing
 * the loop that fills it is exactly the kind of off-by-one that writes past
 * a static array and makes the terminator disappear, so the count is spelled
 * out here rather than left implicit. Sized for the largest case and
 * terminated one slot earlier when the mirror is out: a table entry a client
 * can subscribe to, on a pipe nothing will ever write, is a GATT table that
 * lies about what the board can do. */
static struct ble_gatt_chr_def s_chr_defs[4];
/* The console's service and the terminator. A table with the app's service
 * in it is the app's block's own copy (app_host_t.svc). */
static struct ble_gatt_svc_def s_svc_defs[2];

static int console_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg);

/**
 * @brief Fill the service table.
 *
 * Rebuilt on every start rather than declared once: nimble_port_deinit()
 * frees what was registered (ble_gatts_stop -> ble_gatts_free_svc_defs), so
 * a table registered once would leave the second bring-up of a session with
 * no console service at all.
 */
static void console_build_table(void)
{
    /* ble_gatt_chr_flags is 32 bits wide, and the NOTIFY_INDICATE_* bits
     * live at 0x8000 and above - a 16-bit intermediate would truncate them
     * and silently unprotect the subscription. */
    s_chr_defs[0] = (struct ble_gatt_chr_def){
        .uuid      = &s_rx_uuid.u,
        .access_cb = &console_access,
        .flags     = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP
                   | CHR_WRITE_ENC,
    };
    s_chr_defs[1] = (struct ble_gatt_chr_def){
        .uuid       = &s_tx_uuid.u,
        .access_cb  = &console_access,
        .flags      = BLE_GATT_CHR_F_NOTIFY | CHR_NOTIFY_ENC,
        .val_handle = &s_tx_val_handle,
    };
#if KDRV_BLE_MIRROR_PIPE
    /* Notify-only, same as TX, and CHR_NOTIFY_ENC for the same reason: this
     * mask - not READ_ENC/WRITE_ENC - is what NimBLE actually builds the
     * CCCD's own permissions from (ble_gatts_chr_clt_cfg_flags_from_chr_flags).
     * Getting this wrong is not a smaller failure than getting it wrong on
     * TX - an unpaired stranger would subscribe to the panel instead of the
     * log, which is worse. */
    s_chr_defs[2] = (struct ble_gatt_chr_def){
        .uuid       = &s_mirror_uuid.u,
        .access_cb  = &console_access,
        .flags      = BLE_GATT_CHR_F_NOTIFY | CHR_NOTIFY_ENC,
        .val_handle = &s_mirror_val_handle,
    };
    s_chr_defs[3] = (struct ble_gatt_chr_def){ 0 };
#else
    s_chr_defs[2] = (struct ble_gatt_chr_def){ 0 };
#endif

    s_svc_defs[0] = (struct ble_gatt_svc_def){
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_svc_uuid.u,
        .characteristics = s_chr_defs,
    };
    s_svc_defs[1] = (struct ble_gatt_svc_def){ 0 };
}

/* The assembler's capacity mirrors KODE_CONSOLE_LINE_MAX. It is not that
 * constant because kqp's configuration is console policy and this layer knows
 * about chips; the bound that actually protects the event is the one in
 * AO_Log's callback, where the copy into it happens. */
#define BLE_LINE_MAX        240U

static char     s_line[BLE_LINE_MAX];
static uint16_t s_line_len;
static bool     s_line_over;
static kdrv_ble_console_rx_t s_rx_cb;
static kdrv_ble_console_raw_t s_raw_cb;
static bool s_raw_on;

/**
 * @brief Feed received bytes to the line assembler.
 *
 * The line editor's rules minus the editing: a client on this port is a
 * machine, and echo, prompt and backspace are what a terminal needs. CR or LF
 * ends a line; an empty line is ignored; a line that runs past the buffer is
 * dropped WHOLE and said so, because a command silently cut down and then
 * executed is a lie about what was asked - and this port has no echo for
 * anyone to watch stop.
 *
 * @param data Bytes as they arrived. One write may carry a partial line, a
 *             whole one, or several.
 * @param len  How many.
 */
static void console_feed(const uint8_t *data, uint16_t len)
{
    for ( uint16_t i = 0U; i < len; ++i )
    {
        char const c = (char)data[i];
        if ( ('\r' == c) || ('\n' == c) )
        {
            if ( s_line_over )
            {
                KLOG_W(TAG, "console line over %u bytes, dropped",
                       (unsigned)(BLE_LINE_MAX - 1U));
            }
            else if ( (0U < s_line_len) && (NULL != s_rx_cb) )
            {
                s_line[s_line_len] = '\0';
                s_rx_cb(s_line);
            }
            else
            {
                /* an empty line is not a command */
            }
            s_line_len  = 0U;
            s_line_over = false;
        }
        else if ( (BLE_LINE_MAX - 1U) > s_line_len )
        {
            s_line[s_line_len] = c;
            ++s_line_len;
        }
        else
        {
            s_line_over = true;
        }
    }
}

kode_status_t kdrv_ble_console_set_rx_cb(kdrv_ble_console_rx_t cb)
{
    s_rx_cb = cb;
    return KODE_OK;
}

kode_status_t kdrv_ble_console_set_raw_cb(kdrv_ble_console_raw_t cb)
{
    s_raw_cb = cb;
    return KODE_OK;
}

void kdrv_ble_console_raw(bool on)
{
    /* Whatever sat half-typed in the assembler is abandoned: what follows is
     * an image, and a line that was never terminated is not a command. The
     * cable's raw mode discards its residue for the same reason. */
    s_line_len  = 0U;
    s_line_over = false;
    s_raw_on    = on;
}

/**
 * @brief The console characteristics' access callback.
 *
 * Runs on NimBLE's host task. @c ctxt->om belongs to the stack and is freed
 * after this returns, so the payload is copied out rather than kept. A long
 * write arrives here already reassembled.
 *
 * @param conn_handle Unused: this slice supports one central at a time.
 * @param attr_handle Unused: both characteristics share this callback and
 *                    only one of them is writable.
 * @param ctxt        The operation and its buffer.
 * @param arg         Unused.
 * @return 0, or an ATT error code.
 */
static int console_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    int rc;
    switch ( ctxt->op )
    {
        case BLE_GATT_ACCESS_OP_WRITE_CHR: {
            static uint8_t buf[BLE_RX_SCRATCH];
            uint16_t const total = OS_MBUF_PKTLEN(ctxt->om);
            if ( (uint16_t)sizeof(buf) < total )
            {
                rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
            }
            else
            {
                uint16_t copied = 0U;
                if ( ble_hs_mbuf_to_flat(ctxt->om, buf, total, &copied) != 0 )
                {
                    rc = BLE_ATT_ERR_UNLIKELY;
                }
                else
                {
                    if ( s_raw_on && (NULL != s_raw_cb) )
                    {
                        s_raw_cb(buf, copied);
                    }
                    else
                    {
                        console_feed(buf, copied);
                    }
                    rc = 0;
                }
            }
            break;
        }
        case BLE_GATT_ACCESS_OP_READ_CHR:
            /* Neither characteristic is readable: one is a pipe in, the
             * other a pipe out. A read is a client that misunderstood. */
            rc = BLE_ATT_ERR_READ_NOT_PERMITTED;
            break;
        default:
            rc = BLE_ATT_ERR_UNLIKELY;
            break;
    }
    return rc;
}

/* ---- The output ring -----------------------------------------------------
 * Length-prefixed records: two bytes of length, then the bytes. A record goes
 * in whole or not at all - a '>|' line cut in half cannot be recognised as
 * cut by whatever is parsing it, so a partial record is worse than a missing
 * one. What is refused is counted and reported by `ble_info`.
 */
typedef struct {
    uint8_t *buf;
    size_t   cap;
    size_t   head;   /**< next write */
    size_t   tail;   /**< next read  */
    size_t   used;
} ble_ring_t;

/** @brief Point a ring at its storage and empty it. */
static void ring_init(ble_ring_t *r, uint8_t *storage, size_t cap)
{
    r->buf  = storage;
    r->cap  = cap;
    r->head = 0U;
    r->tail = 0U;
    r->used = 0U;
}

/** @brief Copy @p n bytes in, wrapping. The caller has checked the room. */
static void ring_write(ble_ring_t *r, const uint8_t *src, size_t n)
{
    for ( size_t i = 0U; i < n; ++i )
    {
        r->buf[r->head] = src[i];
        r->head = (r->head + 1U) % r->cap;
    }
    r->used += n;
}

/**
 * @brief Put one record in, or none of it.
 * @return KODE_OK, KODE_NO_RESOURCES when it would not fit, KODE_DENIED for
 *         a length this ring cannot describe.
 */
static kode_status_t ring_put(ble_ring_t *r, const char *buf, size_t len)
{
    if ( (0U == len) || (0xFFFFU < len) )
    {
        return KODE_DENIED;
    }
    if ( (r->used + len + 2U) > r->cap )
    {
        return KODE_NO_RESOURCES;
    }
    uint8_t const hdr[2] = { (uint8_t)(len & 0xFFU), (uint8_t)(len >> 8) };
    ring_write(r, hdr, 2U);
    ring_write(r, (const uint8_t *)buf, len);
    return KODE_OK;
}

/**
 * @brief Take one record out.
 * @return false when the ring is empty, or when @p max cannot hold the
 *         record - in which case nothing is consumed.
 */
static bool ring_get(ble_ring_t *r, uint8_t *out, size_t max, size_t *n)
{
    if ( 2U > r->used )
    {
        return false;
    }
    size_t const len = (size_t)r->buf[r->tail]
                     | ((size_t)r->buf[(r->tail + 1U) % r->cap] << 8);
    if ( len > max )
    {
        return false;
    }
    r->tail = (r->tail + 2U) % r->cap;
    for ( size_t i = 0U; i < len; ++i )
    {
        out[i] = r->buf[r->tail];
        r->tail = (r->tail + 1U) % r->cap;
    }
    r->used -= (len + 2U);
    *n = len;
    return true;
}

#define KDRV_BLE_RING_BYTES  2048U
/** Notifications per drain pass. The mbuf pool these allocate from is the
 *  same one RECEPTION allocates from (12 blocks), so a burst that empties it
 *  makes esp-hosted drop incoming events. Whatever is left goes out on the
 *  next post. */
#define KDRV_BLE_DRAIN_MAX     12U
#define KDRV_BLE_REC_MAX      256U

/* The record being sent, kept across passes. A record leaves the ring only
 * when there is somewhere for it to go; otherwise the ring stays full, which
 * is what lets console_congested() see the link falling behind. */
static uint8_t     s_pend[KDRV_BLE_REC_MAX];
static uint16_t    s_pend_len;
static uint16_t    s_pend_off;

/* The ring's bytes live in PSRAM (design rule 8): taken once, with the lock,
 * and kept for the boot. Only tasks touch them - the console's writers and
 * NimBLE's host task - never an ISR, so the cache being off does not reach
 * them. */
static uint8_t    *s_ring_store;
static ble_ring_t  s_ring;
static khal_mutex_t s_ring_lock;
static struct ble_npl_event s_tx_ev;
/* The retry when the stack is out of buffers. It has to be a TIMED callback,
 * not another event: re-posting the event from inside the drain spins the
 * host task on its own queue, so the very completions that free the buffers
 * never get to run - the console stops dead with a record in hand. Found
 * exactly that way. */
static struct ble_npl_callout s_tx_co;
#define KDRV_BLE_RETRY_TICKS  2U
/* kdrv_ble_quiet()'s hang-up, run on the host task behind the drain. */
static struct ble_npl_event s_hangup_ev;

/* ---- Confirmed lines ------------------------------------------------------
 * A line written with kdrv_ble_console_write_confirmed() is confirmed once
 * NimBLE has taken every byte the ring held, that line's included, AND the
 * central has answered a request sent after them. The link layer delivers in
 * order, so an answer to the later request means the notifications before it
 * reached the central. The request is a discovery of the GAP service by its
 * UUID: every GATT server holds that service and answers it without security,
 * and an error answer counts as well, since it too comes from the central.
 * One mark is kept: one answer confirms every line before it. A line the
 * ring refused or gave up on since the mark, a link that went, or a request
 * nobody answered is reported as not sent. */

#define KDRV_BLE_GAP_SVC_UUID16  0x1800U  /**< The GAP service, on every GATT server. */
#define KDRV_BLE_ATT_ERR_SPAN    0x100    /**< An ATT error is one byte over BLE_HS_ERR_ATT_BASE. */

static kdrv_ble_console_sent_t s_sent_cb;   /**< Told how a confirmed line went.     */
static bool     s_confirm_want;   /**< A confirmed line waits; under the ring's lock. */
static uint16_t s_confirm_drops;  /**< tx_drop when it was marked; under the lock.   */
static bool     s_confirm_busy;   /**< The request is out; the host task alone.      */

/**
 * @brief Say how the confirmed lines went, and forget the mark.
 * @param ok true when the central has them.
 */
static void confirm_report(bool ok)
{
    s_confirm_busy = false;
    if ( NULL != s_sent_cb )
    {
        s_sent_cb(ok);
    }
}

/**
 * @brief A marked line waits and the link is gone: it is not sent.
 *
 * Runs on the host task, the way every caller of confirm_report() does.
 */
static void confirm_lost(void)
{
    bool want = false;

    if ( NULL != s_ring_lock )
    {
        khal_mutex_lock(s_ring_lock);
        want           = s_confirm_want;
        s_confirm_want = false;
        khal_mutex_unlock(s_ring_lock);
    }
    if ( want || s_confirm_busy )
    {
        confirm_report(false);
    }
}

/**
 * @brief The central's answer to the request sent after the marked line.
 *
 * Any answer from the central confirms: a service found, the end of the
 * procedure, or an ATT error. A host error (no link, no answer in time) does
 * not. A line marked after the request went is not covered by its answer: the
 * drain sends another request, whose answer speaks for both. Returning
 * nonzero ends the procedure here, so it calls back once.
 */
static int confirm_answered(uint16_t conn_handle, const struct ble_gatt_error *error,
                            const struct ble_gatt_svc *service, void *arg)
{
    (void)conn_handle;
    (void)service;
    (void)arg;
    if ( s_confirm_busy )
    {
        int const st = (int)error->status;
        bool const ok = (0 == st) || (BLE_HS_EDONE == st)
                        || ((BLE_HS_ERR_ATT_BASE <= st)
                            && ((BLE_HS_ERR_ATT_BASE + KDRV_BLE_ATT_ERR_SPAN) > st));
        bool again = false;

        if ( ok )
        {
            khal_mutex_lock(s_ring_lock);
            again = s_confirm_want;
            khal_mutex_unlock(s_ring_lock);
        }
        if ( again )
        {
            s_confirm_busy = false;
            ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
        }
        else
        {
            confirm_report(ok);
        }
    }
    return 1;
}

/**
 * @brief The ring and the record in hand are empty: send the request a marked
 *        line waits for.
 * @param conn The console's connection.
 */
static void confirm_send(uint16_t conn)
{
    bool     want  = false;
    uint16_t drops = 0U;

    khal_mutex_lock(s_ring_lock);
    if ( s_confirm_want && (!s_confirm_busy) && (0U == s_ring.used) )
    {
        want           = true;
        drops          = s_confirm_drops;
        s_confirm_want = false;
    }
    khal_mutex_unlock(s_ring_lock);
    if ( want )
    {
        s_confirm_busy = true;
        if ( drops != s_ble.tx_drop )
        {
            confirm_report(false);   /* a line since the mark never went */
        }
        else if ( ble_gattc_disc_svc_by_uuid(conn,
                                             BLE_UUID16_DECLARE(KDRV_BLE_GAP_SVC_UUID16),
                                             &confirm_answered, NULL) != 0 )
        {
            confirm_report(false);
        }
        else
        {
            /* confirm_answered() reports. */
        }
    }
}

/**
 * @brief Push what the ring holds to the central. Runs on NimBLE's host task.
 *
 * Nothing here logs. NimBLE and esp-hosted log their own failures through
 * esp_log, klog captures that stream, and the line comes back to AO_Log as
 * one more thing to mirror - which fails, and logs, and comes back again.
 * Failures are counted and `ble_info` reports them.
 *
 * @param ev Unused; the ring is the payload.
 */
static void console_drain(struct ble_npl_event *ev)
{
    (void)ev;

    /* conn and console_on are written by the GAP event handler, which runs on
     * this same host task - so reading them here needs no lock. The ring is
     * the only thing AO_Log's thread touches. */
    uint16_t const conn = s_ble.conn;
    if ( (!s_ble.console_on) || (BLE_HS_CONN_HANDLE_NONE == conn) )
    {
        s_pend_len = 0U;
        s_pend_off = 0U;
        confirm_lost();
        return;
    }

    uint16_t const mtu   = ble_att_mtu(conn);
    size_t   const chunk = (3U < mtu) ? (size_t)(mtu - 3U) : 20U;

    for ( uint8_t i = 0U; KDRV_BLE_DRAIN_MAX > i; ++i )
    {
        if ( 0U == s_pend_len )
        {
            size_t n = 0U;
            khal_mutex_lock(s_ring_lock);
            bool const got = ring_get(&s_ring, s_pend, sizeof(s_pend), &n);
            khal_mutex_unlock(s_ring_lock);
            if ( !got )
            {
                break;
            }
            s_pend_len = (uint16_t)n;
            s_pend_off = 0U;
        }

        while ( s_pend_off < s_pend_len )
        {
            size_t const left = (size_t)(s_pend_len - s_pend_off);
            size_t const take = (left > chunk) ? chunk : left;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(&s_pend[s_pend_off],
                                                       (uint16_t)take);
            if ( NULL == om )
            {
                /* The mbuf pool is empty: the link is behind, not broken.
                 * The record STAYS pending - taking it off the ring and then
                 * dropping it is what made the back-pressure blind, because
                 * the ring emptied itself while nothing reached the air and
                 * the producer was never told to wait.
                 *
                 * Come back on a timer rather than immediately: this runs ON
                 * the host task, so an immediate re-post is a spin that
                 * starves the completions that free the buffers. */
                ++s_ble.tx_retry;
                (void)ble_npl_callout_reset(&s_tx_co, KDRV_BLE_RETRY_TICKS);
                return;
            }
            int const rc = ble_gatts_notify_custom(conn, s_tx_val_handle, om);
            if ( BLE_HS_ENOMEM == rc )
            {
                /* Out of buffers inside the stack: the payload had its blocks
                 * and the header found none. The same fact as no mbuf above,
                 * and the same answer: the record stays pending and a timer
                 * comes back for it. The callee has freed the mbuf. */
                ++s_ble.tx_retry;
                (void)ble_npl_callout_reset(&s_tx_co, KDRV_BLE_RETRY_TICKS);
                return;
            }
            if ( 0 != rc )
            {
                /* The callee frees the mbuf on every failure path EXCEPT the
                 * notify-disabled early return, where it would leak. Freeing
                 * on any other rc would be a double free. */
                if ( BLE_HS_ENOTSUP == rc )
                {
                    (void)os_mbuf_free_chain(om);
                }
                /* A refusal that is not "out of buffers" is not worth
                 * retrying forever: give this record up and move on. */
                ++s_ble.tx_drop;
                s_pend_len = 0U;
                s_pend_off = 0U;
                return;
            }
            s_pend_off += (uint16_t)take;
        }
        s_pend_len = 0U;
        s_pend_off = 0U;
    }

    /* Still work in hand after the cap: come back for it. */
    khal_mutex_lock(s_ring_lock);
    bool const more = (0U < s_ring.used);
    khal_mutex_unlock(s_ring_lock);
    if ( more || (0U < s_pend_len) )
    {
        (void)ble_npl_callout_reset(&s_tx_co, KDRV_BLE_RETRY_TICKS);
    }
    else
    {
        confirm_send(conn);   /* everything written is NimBLE's now */
    }
}

/**
 * @brief Hand the console's ring to the stack, then hang up (host task).
 *
 * kdrv_ble_quiet()'s disconnect, posted to the host task's queue rather than
 * asked from the worker: a line AO_Log wrote before it (an install's last
 * record, before a jump) is given to the stack first. Only what the stack
 * takes at once goes: nothing here waits for the air.
 *
 * @param ev Unused.
 */
static void console_hangup(struct ble_npl_event *ev)
{
    (void)ev;
    console_drain(NULL);
    if ( BLE_HS_CONN_HANDLE_NONE != s_ble.conn )
    {
        (void)ble_gap_terminate(s_ble.conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    (void)xEventGroupSetBits(s_ble.bits, BLE_BIT_HUNG_UP);
}

kode_status_t kdrv_ble_console_write(const char *buf, size_t len)
{
    if ( (NULL == buf) || (0U == len) || (!s_ble.console_on)
        || (NULL == s_ring_lock) )
    {
        return KODE_DENIED;
    }

    khal_mutex_lock(s_ring_lock);
    kode_status_t const st = ring_put(&s_ring, buf, len);
    khal_mutex_unlock(s_ring_lock);

    if ( KODE_OK != st )
    {
        ++s_ble.tx_drop;
        /* Fall through to the post rather than returning: a FULL ring is
         * precisely when the drain is most needed, and returning here left
         * nothing to empty it - the next write found it full too, and the
         * console wedged itself shut one refusal at a time. */
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
        return st;
    }

    /* Re-posting an event that is already queued is a no-op in NimBLE's NPL
     * (npl_freertos_eventq_put returns early on event->queued), so this
     * driver contributes at most one entry to that queue. That is both "one
     * drain in flight" and the reason this post cannot block. */
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
    return KODE_OK;
}

kode_status_t kdrv_ble_console_write_confirmed(const char *buf, size_t len)
{
    /* Read before the line is in the ring: the drain may take it, and lose
     * it, before this call marks it. */
    uint16_t const drops = s_ble.tx_drop;
    kode_status_t const st = kdrv_ble_console_write(buf, len);

    if ( KODE_OK == st )
    {
        khal_mutex_lock(s_ring_lock);
        if ( !s_confirm_want )
        {
            s_confirm_drops = drops;   /* a drop after this fails the mark */
        }
        s_confirm_want = true;
        khal_mutex_unlock(s_ring_lock);
        /* The drain may have taken the line before the mark: once more, so
         * it finds the ring empty and sends the request. */
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_ev);
    }
    return st;
}

kode_status_t kdrv_ble_console_set_sent_cb(kdrv_ble_console_sent_t cb)
{
    s_sent_cb = cb;
    return KODE_OK;
}

/**
 * @brief The console ring, judged against a ring of its own.
 *
 * Non-mutating: it never touches the live ring, so it can run with a central
 * attached and change nothing - the way test_radio judges a handshake it did
 * not perform.
 */
kode_status_t kdrv_ble_console_selftest(void)
{
    static uint8_t    store[64];
    static ble_ring_t r;
    uint8_t out[32];
    size_t  n = 0U;

    ring_init(&r, store, sizeof(store));

    /* 1. An empty ring has nothing to give. */
    if ( ring_get(&r, out, sizeof(out), &n) )
    {
        return KODE_ERROR;
    }
    /* 2. A record comes back exactly as it went in. */
    if ( ring_put(&r, "ab\n", 3U) != KODE_OK )
    {
        return KODE_ERROR;
    }
    if ( !ring_get(&r, out, sizeof(out), &n) || (3U != n)
        || (memcmp(out, "ab\n", 3U) != 0) )
    {
        return KODE_ERROR;
    }
    /* 3. Order survives. */
    if ( (ring_put(&r, "1\n", 2U) != KODE_OK)
        || (ring_put(&r, "2\n", 2U) != KODE_OK) )
    {
        return KODE_ERROR;
    }
    if ( !ring_get(&r, out, sizeof(out), &n) || ('1' != out[0]) )
    {
        return KODE_ERROR;
    }
    if ( !ring_get(&r, out, sizeof(out), &n) || ('2' != out[0]) )
    {
        return KODE_ERROR;
    }
    /* 4. A record that does not fit is refused WHOLE. The ring never holds
     *    half a line, because a '>|' cut in half cannot be recognised as cut
     *    by whatever is parsing it. */
    static const char big[80] = { 0 };
    if ( ring_put(&r, big, sizeof(big)) != KODE_NO_RESOURCES )
    {
        return KODE_ERROR;
    }
    if ( ring_get(&r, out, sizeof(out), &n) )
    {   /* nothing left behind */
        return KODE_ERROR;
    }
    /* 5. The seam: fill it, drain it, and write across the wrap. */
    for ( uint8_t i = 0U; 8U > i; ++i )
    {
        if ( ring_put(&r, "xxxx\n", 5U) != KODE_OK )
        {
            break;
        }
    }
    while ( ring_get(&r, out, sizeof(out), &n) )
    {
        if ( (5U != n) || ('\n' != out[4]) )
        {
            return KODE_ERROR;
        }
    }
    if ( ring_put(&r, "after\n", 6U) != KODE_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

bool kdrv_ble_console_attached(void)
{
    return s_ble.console_on;
}

uint16_t kdrv_ble_console_tx_drop(void)
{
    return s_ble.tx_drop;
}

uint16_t kdrv_ble_console_retry(void)
{
    return s_ble.tx_retry;
}

uint16_t kdrv_ble_console_backlog(void)
{
    if ( (!s_ble.console_on) || (NULL == s_ring_lock) )
    {
        return 0U;
    }
    khal_mutex_lock(s_ring_lock);
    size_t const used = s_ring.used;
    khal_mutex_unlock(s_ring_lock);
    /* Guarded subtraction: an underflow here reads as a permanently
     * congested console, which silently throttles every producer to a stop.
     * That is exactly what a missed `s_pend_off = 0` did - backlog came back
     * as 65532 and nothing could tell why. */
    size_t const pend = (s_pend_len > s_pend_off)
                      ? (size_t)(s_pend_len - s_pend_off) : 0U;
    return (uint16_t)(used + pend);
}

#if KDRV_BLE_MIRROR_PIPE
/* ---- The mirror ring ------------------------------------------------------
 * A second ring, a second pending-record pair and a second callout - all of
 * it a copy of the console's shape above, on purpose. What
 * is NOT copied is the record size: a console line is capped at
 * KDRV_BLE_REC_MAX (256) because a console line is short; a mirror record is
 * one wire slice out of display_ao.c's own KMIRROR_BLE_PAY_BYTES (214 - the
 * largest slice that still fits ONE msys_1 block, which is a different
 * figure from the cable's base64 arithmetic and deliberately so), which fits
 * inside that same cap, so the two rings can share ring_init/ring_put/
 * ring_get - which take the ring as a parameter and know nothing about which
 * pipe it belongs to - without sharing storage. */
#define KDRV_BLE_MIRROR_RING_BYTES  8192U
#define KDRV_BLE_MIRROR_DRAIN_MAX     12U
#define KDRV_BLE_MIRROR_REC_MAX      256U
#define KDRV_BLE_MIRROR_RETRY_TICKS    2U

static uint8_t      s_mirror_pend[KDRV_BLE_MIRROR_REC_MAX];
static uint16_t     s_mirror_pend_len;
static uint16_t     s_mirror_pend_off;

/* In PSRAM, taken once with its lock, for the console ring's reason. */
static uint8_t     *s_mirror_ring_store;
static ble_ring_t   s_mirror_ring;
static khal_mutex_t s_mirror_ring_lock;
static struct ble_npl_event    s_mirror_tx_ev;
static struct ble_npl_callout  s_mirror_tx_co;

/**
 * @brief Push what the mirror ring holds to the central. Runs on NimBLE's
 *        host task - console_drain()'s twin, one ring over.
 *
 * The two hard-won rules there apply here unchanged: a record that meets an
 * empty mbuf pool STAYS pending rather than being dropped, because dropping
 * it is what makes back-pressure invisible to display_ao.c; and the retry on
 * that path is a TIMED callout, never a re-post, because a re-post spins
 * this task on its own queue and starves the very completions that would
 * have freed a buffer.
 *
 * @param ev Unused; the ring is the payload.
 */
static void mirror_drain(struct ble_npl_event *ev)
{
    (void)ev;

    uint16_t const conn = s_ble.conn;
    if ( (!s_ble.mirror_on) || (BLE_HS_CONN_HANDLE_NONE == conn) )
    {
        s_mirror_pend_len = 0U;
        s_mirror_pend_off = 0U;
        return;
    }

    uint16_t const mtu   = ble_att_mtu(conn);
    size_t   const chunk = (3U < mtu) ? (size_t)(mtu - 3U) : 20U;

    for ( uint8_t i = 0U; KDRV_BLE_MIRROR_DRAIN_MAX > i; ++i )
    {
        if ( 0U == s_mirror_pend_len )
        {
            size_t n = 0U;
            khal_mutex_lock(s_mirror_ring_lock);
            bool const got = ring_get(&s_mirror_ring, s_mirror_pend,
                                      sizeof(s_mirror_pend), &n);
            khal_mutex_unlock(s_mirror_ring_lock);
            if ( !got )
            {
                break;
            }
            s_mirror_pend_len = (uint16_t)n;
            s_mirror_pend_off = 0U;
        }

        while ( s_mirror_pend_off < s_mirror_pend_len )
        {
            size_t const left = (size_t)(s_mirror_pend_len - s_mirror_pend_off);
            size_t const take = (left > chunk) ? chunk : left;
            struct os_mbuf *om =
                ble_hs_mbuf_from_flat(&s_mirror_pend[s_mirror_pend_off],
                                      (uint16_t)take);
            if ( NULL == om )
            {
                ++s_ble.tx_retry;
                (void)ble_npl_callout_reset(&s_mirror_tx_co,
                                            KDRV_BLE_MIRROR_RETRY_TICKS);
                return;
            }
            int const rc = ble_gatts_notify_custom(conn, s_mirror_val_handle,
                                                    om);
            if ( BLE_HS_ENOMEM == rc )
            {
                /* Out of buffers inside the stack: the slice stays pending,
                 * as console_drain() says. */
                ++s_ble.tx_retry;
                (void)ble_npl_callout_reset(&s_mirror_tx_co,
                                            KDRV_BLE_MIRROR_RETRY_TICKS);
                return;
            }
            if ( 0 != rc )
            {
                if ( BLE_HS_ENOTSUP == rc )
                {
                    (void)os_mbuf_free_chain(om);
                }
                ++s_ble.tx_drop;
                s_mirror_pend_len = 0U;
                s_mirror_pend_off = 0U;
                return;
            }
            s_mirror_pend_off += (uint16_t)take;
        }
        s_mirror_pend_len = 0U;
        s_mirror_pend_off = 0U;
    }

    khal_mutex_lock(s_mirror_ring_lock);
    bool const more = (0U < s_mirror_ring.used);
    khal_mutex_unlock(s_mirror_ring_lock);
    if ( more || (0U < s_mirror_pend_len) )
    {
        (void)ble_npl_callout_reset(&s_mirror_tx_co, KDRV_BLE_MIRROR_RETRY_TICKS);
    }
}

kode_status_t kdrv_ble_mirror_write(const uint8_t *data, size_t len)
{
    if ( (NULL == data) || (0U == len) || (!s_ble.mirror_on)
        || (NULL == s_mirror_ring_lock) )
    {
        return KODE_DENIED;
    }

    khal_mutex_lock(s_mirror_ring_lock);
    kode_status_t const st = ring_put(&s_mirror_ring, (const char *)data, len);
    khal_mutex_unlock(s_mirror_ring_lock);

    if ( KODE_OK != st )
    {
        /* No counter bumped here, and that is a deliberate difference from
         * kdrv_ble_console_write(): a console line that finds the ring full
         * is gone for good, so tx_drop is the truth about it. A mirror slice
         * that finds ITS ring full is not gone - display_ao.c's contract
         * with this return value is to hold frame_pos where it is and offer
         * the same bytes again next tick, so counting it as
         * dropped would report data loss that never happens. Posting anyway
         * is still right, for the same reason it is on the line above: a
         * full ring is exactly when the drain is most needed. */
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_mirror_tx_ev);
        return st;
    }

    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_mirror_tx_ev);
    return KODE_OK;
}

bool kdrv_ble_mirror_attached(void)
{
    return s_ble.mirror_on;
}
#else  /* !KDRV_BLE_MIRROR_PIPE, with the console still in the build */
/* The same two answers the no-console stubs below give, and for the same
 * reason: display_ao.c's callers are gated on KODEOS_MIRROR themselves, so
 * nothing in kodeOS reaches these - but kdrv_ble.h declares them
 * unconditionally, and a header that promises a symbol has to be able to
 * produce one. */
bool kdrv_ble_mirror_attached(void)
{
    return false;
}

kode_status_t kdrv_ble_mirror_write(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return KODE_DENIED;
}
#endif /* KDRV_BLE_MIRROR_PIPE */

/* ---- The app's own service (host task) -----------------------------------
 * Served after the console's when the app declares one and it passes the
 * rules (kdrv_ble_app.c). The descriptors below are rebuilt at every start,
 * for the console table's reason; the values, the subscriptions and the ring
 * are kdrv_ble_app.c's, in PSRAM. */

/** @brief Bytes a notification spends on its ATT header: an opcode and a handle. */
#define BLE_ATT_NOTIFY_HDR  3U

/** @brief Bytes a read's answer spends on its ATT header: an opcode. */
#define BLE_ATT_READ_HDR    1U

/** @brief The largest MTU the host asks for: an ATT PDU of 451 bytes. A PDU
 *         of 515 never reaches the air over the link to the C5, while one of
 *         451 does, so a longer value travels as a long read or a long write
 *         and a notification carries at most 448 of its bytes. */
#define BLE_APP_MTU_MAX     451U

/** @brief Services in a table with the app's: the console's, the app's, Battery, HID,
 *         Device Information and the terminator. */
#define BLE_APP_TABLE_SVCS  6U

#define BLE_UUID_BATTERY_SVC    0x180FU  /**< The Bluetooth SIG's Battery service.            */
#define BLE_UUID_BATTERY_LEVEL  0x2A19U  /**< Its Battery Level: one byte, 0 to 100 percent. */

#define BLE_UUID_HID_INFO       0x2A4AU  /**< Its HID Information.                            */
#define BLE_UUID_HID_MAP        0x2A4BU  /**< Its Report Map.                                 */
#define BLE_UUID_HID_CTRL       0x2A4CU  /**< Its HID Control Point: suspend, exit suspend.   */
#define BLE_UUID_HID_REPORT     0x2A4DU  /**< A Report, in or out.                            */
#define BLE_UUID_HID_REPORT_REF 0x2908U  /**< A Report's Report Reference descriptor.         */
#define BLE_UUID_DIS_SVC        0x180AU  /**< The Bluetooth SIG's Device Information service. */
#define BLE_UUID_DIS_MAKER      0x2A29U  /**< Its Manufacturer Name String.                   */
#define BLE_UUID_DIS_PNP        0x2A50U  /**< Its PnP ID.                                     */

#define BLE_HID_CHRS_MAX     6U   /**< HID Information, Report Map, HID Control Point, the mouse's
                                   *   report and the keyboard's two.                          */
#define BLE_HID_REPORTS      3U   /**< Reports, each with a Report Reference.                  */
#define BLE_HID_REF_MOUSE    0U   /**< The mouse's input report's place among the reports.     */
#define BLE_HID_REF_KEYS     1U   /**< The keyboard's input report's place.                    */
#define BLE_HID_REF_LEDS     2U   /**< The keyboard's output report's place.                   */
#define BLE_HID_REF_LEN      2U   /**< Bytes of a Report Reference: the report's ID, its type. */
#define BLE_HID_TYPE_INPUT   1U   /**< Report Reference's type for an input report.            */
#define BLE_HID_TYPE_OUTPUT  2U   /**< Report Reference's type for an output report.           */
#define BLE_HID_LEDS_LEN     1U   /**< Bytes of the keyboard's output report.                  */
#define BLE_HID_CTRL_LEN     1U   /**< Bytes of a write to HID Control Point.                  */
#define BLE_DIS_CHRS         2U   /**< Device Information's characteristics: PnP ID, the maker. */

#define BLE_HID_BCD_LO       0x11U  /**< HID Information: the HID specification's release 1.11, low byte. */
#define BLE_HID_BCD_HI       0x01U  /**< And its high byte.                                     */
#define BLE_HID_COUNTRY      0x00U  /**< HID Information: no country's hardware.                */
#define BLE_HID_INFO_FLAGS   0x03U  /**< HID Information: wakes its host, and advertises when not connected. */
#define BLE_PNP_SOURCE_USB   0x02U  /**< PnP ID: the vendor's ID is one the USB-IF assigned.    */
#define BLE_PNP_VENDOR_LO    0x3AU  /**< PnP ID: the vendor the board's USB ports name, low byte. */
#define BLE_PNP_VENDOR_HI    0x30U  /**< And its high byte.                                     */
#define BLE_PNP_PRODUCT_LO   0x01U  /**< PnP ID: the Kode Dot's product ID, low byte.           */
#define BLE_PNP_PRODUCT_HI   0x40U  /**< And its high byte.                                     */
#define BLE_PNP_VERSION_LO   0x00U  /**< PnP ID: the product's version 1.0, low byte.           */
#define BLE_PNP_VERSION_HI   0x01U  /**< And its high byte.                                     */

/** @brief The name Device Information gives as the maker. */
#define BLE_DIS_MAKER_NAME   "Kode"

#define BLE_PRINT_NS         KDRV_BLE_HID_NVS_NS  /**< The NVS namespace of what the HID service keeps between boots. */
#define BLE_PRINT_KEY        "table"              /**< Its key for the print of the table the last boot registered. */

#define BLE_UUID_GATT_SVC      0x1801U  /**< The Generic Attribute service.                   */
#define BLE_UUID_GATT_CHANGED  0x2A05U  /**< Its Service Changed characteristic.              */

/**
 * @brief kdrv_ble.c's NimBLE structures for the HID service and Device
 *        Information, part of app_host_t.
 */
typedef struct {
    struct ble_gatt_chr_def chr[BLE_HID_CHRS_MAX + 1U];   /**< The characteristics and the {0} terminator. */
    struct ble_gatt_dsc_def dsc[BLE_HID_REPORTS][2];      /**< Each report's Report Reference and its terminator. */
    struct ble_gatt_chr_def dis_chr[BLE_DIS_CHRS + 1U];   /**< Device Information's and the {0} terminator. */
    uint16_t                handle[BLE_HID_REPORTS];      /**< Each report's value handle; 0 when not served. */
    bool                    live;   /**< The host runs with the drain's event set up. */
    struct ble_npl_event    ev;     /**< The drain's wake-up.                         */
    struct ble_npl_callout  co;     /**< Its retry, timed for console_drain()'s reason. */
} hid_host_t;

/**
 * @brief kdrv_ble.c's NimBLE structures for the app's service, in the block
 *        kdrv_ble_app.c takes from PSRAM (kdrv_ble_app_host()).
 *
 * NimBLE keeps pointers into the UUIDs, the characteristics and the table
 * after ble_gatts_add_svcs(), and the block is never freed. Nothing here is
 * touched from an interrupt: the event and the callout hold blocks of the
 * porting layer's own pools, which its timer reaches.
 */
typedef struct {
    ble_uuid128_t            uuid[1U + KDRV_BLE_APP_CHARS_MAX];   /**< The service's, then each characteristic's. */
    struct ble_gatt_chr_def  chr[KDRV_BLE_APP_CHARS_MAX + 1U];    /**< The characteristics and the {0} terminator. */
    struct ble_gatt_svc_def  svc[BLE_APP_TABLE_SVCS];             /**< The table registered with the app's.      */
    uint16_t                 val_handle[KDRV_BLE_APP_CHARS_MAX];  /**< Each value's handle, as NimBLE assigned it. */
    uint8_t                  count;   /**< Characteristics served; 0 when the service is not. */
    bool                     live;    /**< The host runs with the drain's event set up.        */
    struct ble_npl_event     ev;      /**< The drain's wake-up.                                */
    struct ble_npl_callout   co;      /**< Its retry, timed for console_drain()'s reason.     */
    struct ble_gatt_chr_def  batt_chr[2];  /**< Battery Level and the {0} terminator.         */
    uint16_t                 batt_handle;  /**< Battery Level's value handle; 0 when not served. */
    bool                     batt_live;    /**< The host runs with batt_ev set up.             */
    struct ble_npl_event     batt_ev;      /**< A new level to notify.                         */
    hid_host_t               hid;          /**< The HID service's, when a kind is served.      */
} app_host_t;

/** @brief The Battery service's UUID. */
static const ble_uuid16_t s_batt_svc_uuid = BLE_UUID16_INIT(BLE_UUID_BATTERY_SVC);

/** @brief Battery Level's UUID. */
static const ble_uuid16_t s_batt_level_uuid = BLE_UUID16_INIT(BLE_UUID_BATTERY_LEVEL);

/** @brief The last charge kdrv_ble_battery_set() was given, in percent; 0 before the first. */
static _Atomic uint8_t s_batt_pct;

/** @brief kdrv_ble_battery_set() has given a charge: Battery Level has a value to read. */
static atomic_bool s_batt_known;

static const ble_uuid16_t s_hid_svc_uuid    = BLE_UUID16_INIT(KDRV_BLE_HID_SVC_UUID);    /**< The HID service's UUID. */
static const ble_uuid16_t s_hid_info_uuid   = BLE_UUID16_INIT(BLE_UUID_HID_INFO);       /**< HID Information's.      */
static const ble_uuid16_t s_hid_map_uuid    = BLE_UUID16_INIT(BLE_UUID_HID_MAP);        /**< Report Map's.           */
static const ble_uuid16_t s_hid_ctrl_uuid   = BLE_UUID16_INIT(BLE_UUID_HID_CTRL);       /**< HID Control Point's.    */
static const ble_uuid16_t s_hid_report_uuid = BLE_UUID16_INIT(BLE_UUID_HID_REPORT);     /**< A Report's.             */
static const ble_uuid16_t s_hid_ref_uuid    = BLE_UUID16_INIT(BLE_UUID_HID_REPORT_REF); /**< Report Reference's.     */
static const ble_uuid16_t s_dis_svc_uuid    = BLE_UUID16_INIT(BLE_UUID_DIS_SVC);        /**< Device Information's.   */
static const ble_uuid16_t s_dis_maker_uuid  = BLE_UUID16_INIT(BLE_UUID_DIS_MAKER);      /**< Manufacturer Name's.    */
static const ble_uuid16_t s_dis_pnp_uuid    = BLE_UUID16_INIT(BLE_UUID_DIS_PNP);        /**< PnP ID's.               */

/** @brief What a read of HID Information gets. */
static const uint8_t s_hid_info[] = {
    BLE_HID_BCD_LO, BLE_HID_BCD_HI, BLE_HID_COUNTRY, BLE_HID_INFO_FLAGS,
};

/** @brief What a read of PnP ID gets. */
static const uint8_t s_dis_pnp[] = {
    BLE_PNP_SOURCE_USB, BLE_PNP_VENDOR_LO, BLE_PNP_VENDOR_HI, BLE_PNP_PRODUCT_LO,
    BLE_PNP_PRODUCT_HI, BLE_PNP_VERSION_LO, BLE_PNP_VERSION_HI,
};

/** @brief What each report's Report Reference says: the report's ID, its type. */
static const uint8_t s_hid_ref[BLE_HID_REPORTS][BLE_HID_REF_LEN] = {
    [BLE_HID_REF_MOUSE] = { KDRV_BLE_HID_ID_MOUSE, BLE_HID_TYPE_INPUT },
    [BLE_HID_REF_KEYS]  = { KDRV_BLE_HID_ID_KEYBOARD, BLE_HID_TYPE_INPUT },
    [BLE_HID_REF_LEDS]  = { KDRV_BLE_HID_ID_KEYBOARD, BLE_HID_TYPE_OUTPUT },
};

/** @brief The print of the table this boot registers; KDRV_BLE_HID_PRINT_NONE without HID. */
static uint32_t s_table_print;

static const ble_uuid16_t s_gatt_svc_uuid     = BLE_UUID16_INIT(BLE_UUID_GATT_SVC);     /**< The GATT service's UUID. */
static const ble_uuid16_t s_gatt_changed_uuid = BLE_UUID16_INIT(BLE_UUID_GATT_CHANGED); /**< Service Changed's.       */

/** @brief One pass of the search for a stored subscription to forget. */
typedef struct {
    uint16_t                  keep;   /**< Service Changed's value handle: its subscriptions stay. */
    bool                      found;  /**< The pass found one.                                     */
    struct ble_store_key_cccd key;    /**< Its key.                                                */
} cccd_prune_t;

size_t kdrv_ble_app_host_bytes(void)
{
    return sizeof(app_host_t);
}

/**
 * @brief kdrv_ble.c's part of the app's block.
 * @return It; NULL before the block is taken, so before any service or value.
 */
static app_host_t *app_host(void)
{
    /* cppcheck-suppress misra-c2012-11.5 ; kdrv_ble_app_host() hands back the part sized by kdrv_ble_app_host_bytes(), aligned for any type */
    return (app_host_t *)kdrv_ble_app_host();
}

/** @brief Characteristics of the app's served now; 0 without a block or a service. */
static uint8_t app_count(void)
{
    const app_host_t *const a = app_host();
    uint8_t                 n = 0U;

    if ( NULL != a )
    {
        n = a->count;
    }
    return n;
}

/**
 * @brief The place in the table of the characteristic whose value is @p attr.
 * @param attr An attribute handle.
 * @return Its place; KDRV_BLE_APP_CHARS_MAX when it is none of the app's.
 */
static uint8_t app_index(uint16_t attr)
{
    const app_host_t *const a     = app_host();
    uint8_t const           n     = app_count();
    uint8_t                 found = (uint8_t)KDRV_BLE_APP_CHARS_MAX;

    for ( uint8_t i = 0U; (i < n) && ((uint8_t)KDRV_BLE_APP_CHARS_MAX == found); ++i )
    {
        if ( a->val_handle[i] == attr )
        {
            found = i;
        }
    }
    return found;
}

/**
 * @brief A read or a write of one of the app's characteristics. Host task.
 *
 * A read appends the whole value and NimBLE sends the part from the read's
 * offset that fits the answer; a queued value is taken once the part sent
 * reaches its end (kdrv_ble_app_read()).
 *
 * @param conn_handle The link, for its MTU.
 * @param attr_handle The value's handle.
 * @param ctxt        NimBLE's access context.
 * @param arg         Unused.
 * @return 0, or the ATT error the central gets.
 */
static int app_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)arg;

    uint8_t const  i   = app_index(attr_handle);
    uint8_t *const buf = kdrv_ble_app_scratch();
    int            rc  = BLE_ATT_ERR_UNLIKELY;

    if ( (app_count() <= i) || (NULL == buf) )
    {
        /* not one of the app's: rc stays UNLIKELY */
    }
    else if ( BLE_GATT_ACCESS_OP_READ_CHR == ctxt->op )
    {
        uint16_t const mtu  = ble_att_mtu(conn_handle);
        uint16_t       span = 0U;

        if ( BLE_ATT_READ_HDR < mtu )
        {
            span = (uint16_t)(mtu - BLE_ATT_READ_HDR);
        }
        uint16_t const n = kdrv_ble_app_read(i, ctxt->offset, span, buf,
                                             (uint16_t)KDRV_BLE_APP_VALUE_MAX);

        rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        if ( os_mbuf_append(ctxt->om, buf, n) == 0 )
        {
            rc = 0;
        }
    }
    else if ( BLE_GATT_ACCESS_OP_WRITE_CHR == ctxt->op )
    {
        uint16_t const total  = OS_MBUF_PKTLEN(ctxt->om);
        uint16_t       copied = 0U;

        if ( (uint16_t)KDRV_BLE_APP_VALUE_MAX < total )
        {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        else if ( ble_hs_mbuf_to_flat(ctxt->om, buf, total, &copied) != 0 )
        {
            /* rc stays UNLIKELY */
        }
        else if ( kdrv_ble_app_written(i, buf, copied) )
        {
            rc = 0;
        }
        else
        {
            rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    else
    {
        /* no descriptors of its own: rc stays UNLIKELY */
    }
    return rc;
}

/**
 * @brief A read of Battery Level. Host task.
 * @param conn_handle Unused.
 * @param attr_handle Unused: the service has one value.
 * @param ctxt        NimBLE's access context.
 * @param arg         Unused.
 * @return 0, or the ATT error the central gets.
 */
static int batt_access(uint16_t conn_handle, uint16_t attr_handle,
                       struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    int rc = BLE_ATT_ERR_UNLIKELY;

    /* Before the first reading there is no level to give: an error reads as
     * no battery, where a 0 would read as an empty one. */
    if ( (BLE_GATT_ACCESS_OP_READ_CHR == ctxt->op) && atomic_load(&s_batt_known) )
    {
        uint8_t const pct = atomic_load(&s_batt_pct);

        rc = 0;
        if ( os_mbuf_append(ctxt->om, &pct, sizeof(pct)) != 0 )
        {
            rc = BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    return rc;
}

/**
 * @brief Tell the subscribed central the new Battery Level. Host task.
 * @param ev Unused; the level is s_batt_pct.
 */
static void batt_notify(struct ble_npl_event *ev)
{
    (void)ev;

    const app_host_t *const a = app_host();

    if ( (NULL != a) && (0U != a->batt_handle) )
    {
        /* NimBLE reads the value through batt_access() for each subscriber. */
        ble_gatts_chr_updated(a->batt_handle);
    }
}

/* ---- The HID service (host task) -------------------------------------------
 * Served last in the table when a kind was chosen (kdrv_ble_hid_select()), so
 * every other handle is the same with it and without it. The kinds, the
 * report map, the reports' ring and the subscriptions are kdrv_ble_hid.c's. */

/** @brief The HID service's part of the block; NULL before the block is taken. */
static hid_host_t *hid_host(void)
{
    app_host_t *const a = app_host();
    hid_host_t       *h = NULL;

    if ( NULL != a )
    {
        h = &a->hid;
    }
    return h;
}

/**
 * @brief Which report an access is about.
 * @param h    The HID service's part of the block.
 * @param attr The attribute's handle: a report's value for a characteristic.
 * @param ctxt NimBLE's access context: the descriptor for a Report Reference.
 * @return The report's place; BLE_HID_REPORTS when it is none.
 */
static uint8_t hid_ref_of(const hid_host_t *h, uint16_t attr,
                          const struct ble_gatt_access_ctxt *ctxt)
{
    bool const dsc   = (BLE_GATT_ACCESS_OP_READ_DSC == ctxt->op)
                       || (BLE_GATT_ACCESS_OP_WRITE_DSC == ctxt->op);
    uint8_t    found = (uint8_t)BLE_HID_REPORTS;

    for ( uint8_t i = 0U; (i < (uint8_t)BLE_HID_REPORTS) && ((uint8_t)BLE_HID_REPORTS == found); ++i )
    {
        if ( dsc )
        {
            if ( ctxt->dsc == &h->dsc[i][0] )
            {
                found = i;
            }
        }
        else if ( (0U != h->handle[i]) && (h->handle[i] == attr) )
        {
            found = i;
        }
        else
        {
            /* not this report */
        }
    }
    return found;
}

/**
 * @brief The kind whose input report sits at @p ref.
 * @param ref A report's place.
 * @return KDRV_BLE_HID_MOUSE or KDRV_BLE_HID_KEYBOARD; 0 for the output report or none.
 */
static uint8_t hid_kind_of(uint8_t ref)
{
    uint8_t kind = 0U;

    if ( (uint8_t)BLE_HID_REF_MOUSE == ref )
    {
        kind = (uint8_t)KDRV_BLE_HID_MOUSE;
    }
    else if ( (uint8_t)BLE_HID_REF_KEYS == ref )
    {
        kind = (uint8_t)KDRV_BLE_HID_KEYBOARD;
    }
    else
    {
        /* the output report, or none */
    }
    return kind;
}

/**
 * @brief The word the log names @p kinds by.
 * @param kinds One kind, or both.
 * @return "mouse", "keyboard", or "mouse and keyboard".
 */
static const char *hid_kind_word(uint8_t kinds)
{
    const char *word = "mouse and keyboard";

    if ( (uint8_t)KDRV_BLE_HID_MOUSE == kinds )
    {
        word = "mouse";
    }
    else if ( (uint8_t)KDRV_BLE_HID_KEYBOARD == kinds )
    {
        word = "keyboard";
    }
    else
    {
        /* both */
    }
    return word;
}

/**
 * @brief The word the log says a subscription's state with.
 * @param on Whether the host is subscribed.
 * @return "on" or "off".
 */
static const char *hid_on_word(bool on)
{
    const char *word = "off";

    if ( on )
    {
        word = "on";
    }
    return word;
}

/**
 * @brief The kind whose input report's value is @p attr.
 * @param attr An attribute handle.
 * @return The kind; 0 when it is no input report's.
 */
static uint8_t hid_kind_at(uint16_t attr)
{
    const hid_host_t *const h    = hid_host();
    uint8_t                 kind = 0U;

    if ( (NULL != h) && (0U != attr) )
    {
        if ( h->handle[BLE_HID_REF_MOUSE] == attr )
        {
            kind = (uint8_t)KDRV_BLE_HID_MOUSE;
        }
        else if ( h->handle[BLE_HID_REF_KEYS] == attr )
        {
            kind = (uint8_t)KDRV_BLE_HID_KEYBOARD;
        }
        else
        {
            /* not an input report */
        }
    }
    return kind;
}

/**
 * @brief Append @p len bytes to a read's answer.
 * @param ctxt NimBLE's access context.
 * @param src  The bytes.
 * @param len  Their count.
 * @return 0, or the ATT error the central gets.
 */
static int hid_answer(struct ble_gatt_access_ctxt *ctxt, const void *src, size_t len)
{
    int rc = BLE_ATT_ERR_INSUFFICIENT_RES;

    if ( os_mbuf_append(ctxt->om, src, (uint16_t)len) == 0 )
    {
        rc = 0;
    }
    return rc;
}

/**
 * @brief A read of a value that never changes: HID Information, Report Map,
 *        PnP ID or the maker's name. Host task.
 * @param conn_handle Unused.
 * @param attr_handle Unused: the characteristic's UUID says which.
 * @param ctxt        NimBLE's access context.
 * @param arg         Unused.
 * @return 0, or the ATT error the central gets.
 */
static int hid_const_access(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    int rc = BLE_ATT_ERR_UNLIKELY;

    if ( BLE_GATT_ACCESS_OP_READ_CHR != ctxt->op )
    {
        /* read only: rc stays UNLIKELY */
    }
    else if ( ble_uuid_cmp(ctxt->chr->uuid, &s_hid_info_uuid.u) == 0 )
    {
        rc = hid_answer(ctxt, s_hid_info, sizeof(s_hid_info));
    }
    else if ( ble_uuid_cmp(ctxt->chr->uuid, &s_hid_map_uuid.u) == 0 )
    {
        uint8_t      map[KDRV_BLE_HID_MAP_MAX];
        size_t const n = kdrv_ble_hid_report_map(kdrv_ble_hid_kinds(), map, sizeof(map));

        /* The whole map: NimBLE sends the part from the read's offset. */
        rc = hid_answer(ctxt, map, n);
    }
    else if ( ble_uuid_cmp(ctxt->chr->uuid, &s_dis_pnp_uuid.u) == 0 )
    {
        rc = hid_answer(ctxt, s_dis_pnp, sizeof(s_dis_pnp));
    }
    else if ( ble_uuid_cmp(ctxt->chr->uuid, &s_dis_maker_uuid.u) == 0 )
    {
        rc = hid_answer(ctxt, BLE_DIS_MAKER_NAME, sizeof(BLE_DIS_MAKER_NAME) - 1U);
    }
    else
    {
        /* none of the four: rc stays UNLIKELY */
    }
    return rc;
}

/**
 * @brief A write to HID Control Point: the host suspends or wakes. Host task.
 *
 * Taken and not acted on: the board's rest is AO_LowPower's.
 *
 * @param conn_handle Unused.
 * @param attr_handle Unused.
 * @param ctxt        NimBLE's access context.
 * @param arg         Unused.
 * @return 0, or the ATT error the central gets.
 */
static int hid_ctrl_access(uint16_t conn_handle, uint16_t attr_handle,
                           struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;

    int rc = BLE_ATT_ERR_UNLIKELY;

    if ( BLE_GATT_ACCESS_OP_WRITE_CHR == ctxt->op )
    {
        rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if ( (uint16_t)BLE_HID_CTRL_LEN == OS_MBUF_PKTLEN(ctxt->om) )
        {
            rc = 0;
        }
    }
    return rc;
}

/**
 * @brief A read or a write of a report, or a read of its Report Reference. Host task.
 *
 * An input report reads as the last one sent on this link. The output report
 * is the keyboard's LEDs, kept as the host wrote them.
 *
 * @param conn_handle Unused.
 * @param attr_handle The attribute's handle.
 * @param ctxt        NimBLE's access context.
 * @param arg         Unused.
 * @return 0, or the ATT error the central gets.
 */
static int hid_report_access(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)arg;

    const hid_host_t *const h   = hid_host();
    uint8_t                 ref = (uint8_t)BLE_HID_REPORTS;
    int                     rc  = BLE_ATT_ERR_UNLIKELY;

    if ( NULL != h )
    {
        ref = hid_ref_of(h, attr_handle, ctxt);
    }
    if ( (uint8_t)BLE_HID_REPORTS <= ref )
    {
        /* not one of the reports: rc stays UNLIKELY */
    }
    else if ( BLE_GATT_ACCESS_OP_READ_DSC == ctxt->op )
    {
        rc = hid_answer(ctxt, s_hid_ref[ref], sizeof(s_hid_ref[ref]));
    }
    else if ( BLE_GATT_ACCESS_OP_READ_CHR == ctxt->op )
    {
        uint8_t  buf[KDRV_BLE_HID_REPORT_MAX];
        uint16_t n = (uint16_t)BLE_HID_LEDS_LEN;

        if ( (uint8_t)BLE_HID_REF_LEDS == ref )
        {
            buf[0] = kdrv_ble_hid_leds();
        }
        else
        {
            n = kdrv_ble_hid_last(hid_kind_of(ref), buf);
        }
        rc = hid_answer(ctxt, buf, n);
    }
    else if ( (BLE_GATT_ACCESS_OP_WRITE_CHR == ctxt->op) && ((uint8_t)BLE_HID_REF_LEDS == ref) )
    {
        uint8_t  leds   = 0U;
        uint16_t copied = 0U;

        if ( (uint16_t)BLE_HID_LEDS_LEN != OS_MBUF_PKTLEN(ctxt->om) )
        {
            rc = BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        else if ( ble_hs_mbuf_to_flat(ctxt->om, &leds, (uint16_t)BLE_HID_LEDS_LEN, &copied) == 0 )
        {
            kdrv_ble_hid_leds_set(leds);
            rc = 0;
        }
        else
        {
            /* rc stays UNLIKELY */
        }
    }
    else
    {
        /* an input report takes no write: rc stays UNLIKELY */
    }
    return rc;
}

/**
 * @brief Fill one report's characteristic and its Report Reference.
 * @param h     The HID service's part of the block.
 * @param at    The characteristic's place in the service.
 * @param ref   The report's place among the reports.
 * @param flags The characteristic's flags.
 */
static void hid_report_def(hid_host_t *h, uint8_t at, uint8_t ref, ble_gatt_chr_flags flags)
{
    h->dsc[ref][0] = (struct ble_gatt_dsc_def){
        .uuid      = &s_hid_ref_uuid.u,
        .att_flags = (uint8_t)(BLE_ATT_F_READ | DSC_READ_ENC | BLE_ATT_F_READ_AUTHOR),
        .access_cb = &hid_report_access,
    };
    h->dsc[ref][1] = (struct ble_gatt_dsc_def){ 0 };
    h->chr[at] = (struct ble_gatt_chr_def){
        .uuid        = &s_hid_report_uuid.u,
        .access_cb   = &hid_report_access,
        .flags       = flags,
        .val_handle  = &h->handle[ref],
        .descriptors = h->dsc[ref],
    };
}

/**
 * @brief Add the HID service and Device Information to the table.
 * @param a     The block.
 * @param n     Services the table holds already.
 * @param kinds The kinds served, at least one.
 * @param perm  The flags each property earns: the console's.
 * @return Services the table holds now.
 */
static uint8_t hid_build_table(app_host_t *a, uint8_t n, uint8_t kinds,
                               const kdrv_ble_app_perm_t *perm)
{
    hid_host_t *const h    = &a->hid;
    uint8_t           at   = 0U;
    uint8_t           svcs = n;
    /* Every access of the service asks whose bond is on the link: a HID
     * bond's alone gets in (BLE_GAP_EVENT_AUTHORIZE). */
    ble_gatt_chr_flags const rd = (ble_gatt_chr_flags)(perm->read | BLE_GATT_CHR_F_READ_AUTHOR);
    ble_gatt_chr_flags const wr = (ble_gatt_chr_flags)(perm->write | BLE_GATT_CHR_F_WRITE_AUTHOR);
    ble_gatt_chr_flags const nt = (ble_gatt_chr_flags)(perm->notify
                                                       | BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR);

    h->chr[at] = (struct ble_gatt_chr_def){
        .uuid      = &s_hid_info_uuid.u,
        .access_cb = &hid_const_access,
        .flags     = rd,
    };
    ++at;
    h->chr[at] = (struct ble_gatt_chr_def){
        .uuid      = &s_hid_map_uuid.u,
        .access_cb = &hid_const_access,
        .flags     = rd,
    };
    ++at;
    h->chr[at] = (struct ble_gatt_chr_def){
        .uuid      = &s_hid_ctrl_uuid.u,
        .access_cb = &hid_ctrl_access,
        .flags     = (ble_gatt_chr_flags)(BLE_GATT_CHR_F_WRITE_NO_RSP | CHR_WRITE_ENC
                                          | BLE_GATT_CHR_F_WRITE_AUTHOR),
    };
    ++at;
    if ( 0U != (kinds & KDRV_BLE_HID_MOUSE) )
    {
        hid_report_def(h, at, (uint8_t)BLE_HID_REF_MOUSE, (ble_gatt_chr_flags)(rd | nt));
        ++at;
    }
    if ( 0U != (kinds & KDRV_BLE_HID_KEYBOARD) )
    {
        hid_report_def(h, at, (uint8_t)BLE_HID_REF_KEYS, (ble_gatt_chr_flags)(rd | nt));
        ++at;
        hid_report_def(h, at, (uint8_t)BLE_HID_REF_LEDS,
                       (ble_gatt_chr_flags)(rd | wr | BLE_GATT_CHR_F_WRITE_NO_RSP));
        ++at;
    }
    h->chr[at] = (struct ble_gatt_chr_def){ 0 };
    a->svc[svcs] = (struct ble_gatt_svc_def){
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_hid_svc_uuid.u,
        .characteristics = h->chr,
    };
    ++svcs;

    /* What the board is, for the host that lists it: read without a bond. */
    h->dis_chr[0] = (struct ble_gatt_chr_def){
        .uuid      = &s_dis_pnp_uuid.u,
        .access_cb = &hid_const_access,
        .flags     = BLE_GATT_CHR_F_READ,
    };
    h->dis_chr[1] = (struct ble_gatt_chr_def){
        .uuid      = &s_dis_maker_uuid.u,
        .access_cb = &hid_const_access,
        .flags     = BLE_GATT_CHR_F_READ,
    };
    h->dis_chr[BLE_DIS_CHRS] = (struct ble_gatt_chr_def){ 0 };
    a->svc[svcs] = (struct ble_gatt_svc_def){
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &s_dis_svc_uuid.u,
        .characteristics = h->dis_chr,
    };
    ++svcs;
    return svcs;
}

/**
 * @brief Add a UUID's bytes to a table's print.
 * @param print The print so far.
 * @param uuid  The UUID, of 16 or of 128 bits.
 * @return The print with it in.
 */
static uint32_t print_uuid(uint32_t print, const ble_uuid_t *uuid)
{
    uint32_t out = print;

    if ( BLE_UUID_TYPE_16 == uuid->type )
    {
        /* cppcheck-suppress misra-c2012-11.3 ; NimBLE's own reading of a UUID by its type */
        uint16_t const v = BLE_UUID16(uuid)->value;

        out = kdrv_ble_hid_print_add(out, &v, sizeof(v));
    }
    else if ( BLE_UUID_TYPE_128 == uuid->type )
    {
        /* cppcheck-suppress misra-c2012-11.3 ; NimBLE's own reading of a UUID by its type */
        out = kdrv_ble_hid_print_add(out, BLE_UUID128(uuid)->value, KDRV_BLE_UUID128_BYTES);
    }
    else
    {
        /* no table of this file's has a UUID of 32 bits */
    }
    return out;
}

/**
 * @brief The print of the table @p defs: every service, characteristic and
 *        descriptor in order, and each characteristic's flags.
 *
 * Two tables with one print give every attribute the same handle.
 *
 * @param defs The table, ended by a service of no type.
 * @return The print.
 */
static uint32_t table_print(const struct ble_gatt_svc_def *defs)
{
    uint32_t print = KDRV_BLE_HID_PRINT_SEED;

    for ( size_t s = 0U; BLE_GATT_SVC_TYPE_END != defs[s].type; ++s )
    {
        const struct ble_gatt_chr_def *const chrs = defs[s].characteristics;

        print = print_uuid(print, defs[s].uuid);
        for ( size_t c = 0U; (NULL != chrs) && (NULL != chrs[c].uuid); ++c )
        {
            const struct ble_gatt_dsc_def *const dscs = chrs[c].descriptors;

            print = print_uuid(print, chrs[c].uuid);
            print = kdrv_ble_hid_print_add(print, &chrs[c].flags, sizeof(chrs[c].flags));
            for ( size_t d = 0U; (NULL != dscs) && (NULL != dscs[d].uuid); ++d )
            {
                print = print_uuid(print, dscs[d].uuid);
            }
        }
    }
    return print;
}

/**
 * @brief The table to register: the console's alone, or the console's with
 *        the app's after it when the app's is served, Battery after those
 *        when KODE_BLE_BATTERY_SERVICE() or the HID service asks for it, and
 *        the HID service and Device Information last when a kind was chosen.
 *
 * Called after console_build_table(). The table with the app's service is
 * the block's own copy, so the console's static table stays two entries.
 *
 * @return The table.
 */
static const struct ble_gatt_svc_def *app_build_table(void)
{
    const struct ble_gatt_svc_def *defs = s_svc_defs;
    kdrv_ble_app_table_t           t;
    kdrv_ble_app_perm_t const      perm = {
        .read   = BLE_GATT_CHR_F_READ | CHR_READ_ENC,
        .write  = BLE_GATT_CHR_F_WRITE | CHR_WRITE_ENC,
        .notify = BLE_GATT_CHR_F_NOTIFY | CHR_NOTIFY_ENC,
    };
    bool const    served = kdrv_ble_app_build(kdrv_ble_app_fix(), &perm, &t);

    /* The HID service's structures live in the block: a board that asks for
     * it takes the block, and one the block is refused to serves no HID. */
    if ( (0U != kdrv_ble_hid_kinds()) && !kdrv_ble_app_block() )
    {
        KLOG_E(TAG, "hid: no PSRAM for the service; it is left out");
        (void)kdrv_ble_hid_select(0U);
    }
    uint8_t const     hid = kdrv_ble_hid_fix();
    app_host_t *const a   = app_host();

    if ( NULL != a )
    {
        a->count       = 0U;
        a->live        = false;
        a->batt_live   = false;
        a->batt_handle = 0U;
        a->hid.live    = false;
        (void)memset(a->val_handle, 0, sizeof(a->val_handle));
        (void)memset(a->hid.handle, 0, sizeof(a->hid.handle));
    }
    if ( served && (NULL != a) )
    {
        a->uuid[0].u.type = BLE_UUID_TYPE_128;
        (void)memcpy(a->uuid[0].value, t.svc_uuid, sizeof(a->uuid[0].value));
        for ( uint8_t i = 0U; i < t.count; ++i )
        {
            ble_uuid128_t *const u = &a->uuid[1U + i];

            u->u.type = BLE_UUID_TYPE_128;
            (void)memcpy(u->value, t.uuid[i], sizeof(u->value));
            a->chr[i] = (struct ble_gatt_chr_def){
                .uuid       = &u->u,
                .access_cb  = &app_access,
                .flags      = (ble_gatt_chr_flags)t.flags[i],
                .val_handle = &a->val_handle[i],
            };
        }
        a->chr[t.count] = (struct ble_gatt_chr_def){ 0 };
        a->count        = t.count;
    }
    if ( 0U != hid )
    {
        /* The console's service is a console bond's alone, and the HID
         * service a HID bond's: both ask who is on the link before each
         * access. A table without HID asks nobody, and is as it was. */
        s_chr_defs[0].flags |= BLE_GATT_CHR_F_WRITE_AUTHOR;
        s_chr_defs[1].flags |= BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR;
#if KDRV_BLE_MIRROR_PIPE
        s_chr_defs[2].flags |= BLE_GATT_CHR_F_NOTIFY_INDICATE_AUTHOR;
#endif
    }
    if ( (NULL != a) && (served || kdrv_ble_battery_declared()) )
    {
        uint8_t n = 0U;

        a->svc[n] = s_svc_defs[0];
        ++n;
        if ( served )
        {
            a->svc[n] = (struct ble_gatt_svc_def){
                .type            = BLE_GATT_SVC_TYPE_PRIMARY,
                .uuid            = &a->uuid[0].u,
                .characteristics = a->chr,
            };
            ++n;
        }
        if ( kdrv_ble_battery_declared() )
        {
            /* Read and notify, behind the same permissions as the app's own
             * characteristics: a bonded phone reads the charge. */
            a->batt_chr[0] = (struct ble_gatt_chr_def){
                .uuid       = &s_batt_level_uuid.u,
                .access_cb  = &batt_access,
                .flags      = (ble_gatt_chr_flags)(perm.read | perm.notify),
                .val_handle = &a->batt_handle,
            };
            a->batt_chr[1] = (struct ble_gatt_chr_def){ 0 };
            a->svc[n] = (struct ble_gatt_svc_def){
                .type            = BLE_GATT_SVC_TYPE_PRIMARY,
                .uuid            = &s_batt_svc_uuid.u,
                .characteristics = a->batt_chr,
            };
            ++n;
        }
        if ( 0U != hid )
        {
            n = hid_build_table(a, n, hid, &perm);
        }
        a->svc[n] = (struct ble_gatt_svc_def){ 0 };
        defs      = a->svc;
    }
    s_table_print = kdrv_ble_hid_print_end(table_print(defs), 0U != hid);
    return defs;
}

/**
 * @brief Send what the ring holds to the central. Runs on NimBLE's host task.
 *
 * Logs nothing, for console_drain()'s reason. A notification longer than the
 * link's MTU less its header is dropped and counted in tx_drop; the value
 * stays for a read.
 *
 * @param ev Unused; the ring is the payload.
 */
static void app_drain(struct ble_npl_event *ev)
{
    (void)ev;

    app_host_t *const a    = app_host();
    uint16_t const    conn = s_ble.conn;
    uint8_t *const    buf  = kdrv_ble_app_scratch();
    uint8_t           chr  = 0U;
    uint16_t          len  = 0U;
    bool              more = (NULL != a) && (BLE_HS_CONN_HANDLE_NONE != conn) && (NULL != buf);

    for ( uint32_t k = 0U; more && (KDRV_BLE_APP_RING_SLOTS > k); ++k )
    {
        more = kdrv_ble_app_peek(&chr, buf, &len);
        if ( more )
        {
            uint16_t const mtu = ble_att_mtu(conn);

            if ( (chr >= a->count) || (BLE_ATT_NOTIFY_HDR >= mtu)
                || ((uint16_t)(mtu - BLE_ATT_NOTIFY_HDR) < len) )
            {
                ++s_ble.tx_drop;
                kdrv_ble_app_pop();
            }
            else
            {
                struct os_mbuf *const om = ble_hs_mbuf_from_flat(buf, len);

                if ( NULL == om )
                {
                    /* Out of buffers: the notification stays, and a timer
                     * comes back for it. */
                    ++s_ble.tx_retry;
                    (void)ble_npl_callout_reset(&a->co, KDRV_BLE_RETRY_TICKS);
                    more = false;
                }
                else
                {
                    int const rc = ble_gatts_notify_custom(conn, a->val_handle[chr], om);

                    if ( BLE_HS_ENOMEM == rc )
                    {
                        /* Out of buffers inside the stack: the notification
                         * stays, as with no mbuf above. */
                        ++s_ble.tx_retry;
                        (void)ble_npl_callout_reset(&a->co, KDRV_BLE_RETRY_TICKS);
                        more = false;
                    }
                    else
                    {
                        if ( 0 != rc )
                        {
                            /* Freed by the callee on every path but this
                             * one, as console_drain() says. */
                            if ( BLE_HS_ENOTSUP == rc )
                            {
                                (void)os_mbuf_free_chain(om);
                            }
                            ++s_ble.tx_drop;
                        }
                        kdrv_ble_app_pop();
                    }
                }
            }
        }
    }
}

void kdrv_ble_app_kick(void)
{
    app_host_t *const a = app_host();

    /* Posted to the host's own queue, as the console's wake-up is. */
    if ( (NULL != a) && a->live && kdrv_ble_app_queued() )
    {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &a->ev);
    }
}

/**
 * @brief Notify the reports the ring holds to the host. Runs on NimBLE's host task.
 *
 * Logs nothing, for console_drain()'s reason. A report the link refuses is
 * counted dropped; one the stack has no buffer for stays, and a timer comes
 * back for it.
 *
 * @param ev Unused; the ring is the payload.
 */
static void hid_drain(struct ble_npl_event *ev)
{
    (void)ev;

    hid_host_t *const h    = hid_host();
    uint16_t const    conn = s_ble.conn;
    uint8_t           buf[KDRV_BLE_HID_REPORT_MAX];
    uint8_t           kind = 0U;
    uint16_t          len  = 0U;
    bool              more = (NULL != h) && (BLE_HS_CONN_HANDLE_NONE != conn);

    for ( uint32_t k = 0U; more && (KDRV_BLE_HID_RING_SLOTS > k); ++k )
    {
        more = kdrv_ble_hid_peek(&kind, buf, &len);
        if ( more )
        {
            uint16_t        handle = h->handle[BLE_HID_REF_KEYS];
            struct os_mbuf *om     = NULL;

            if ( (uint8_t)KDRV_BLE_HID_MOUSE == kind )
            {
                handle = h->handle[BLE_HID_REF_MOUSE];
            }
            if ( 0U != handle )
            {
                om = ble_hs_mbuf_from_flat(buf, len);
            }
            if ( 0U == handle )
            {
                kdrv_ble_hid_pop(false);
            }
            else if ( NULL == om )
            {
                ++s_ble.tx_retry;
                (void)ble_npl_callout_reset(&h->co, KDRV_BLE_RETRY_TICKS);
                more = false;
            }
            else
            {
                int const rc = ble_gatts_notify_custom(conn, handle, om);

                if ( BLE_HS_ENOMEM == rc )
                {
                    ++s_ble.tx_retry;
                    (void)ble_npl_callout_reset(&h->co, KDRV_BLE_RETRY_TICKS);
                    more = false;
                }
                else
                {
                    /* Freed by the callee on every path but this one, as
                     * console_drain() says. */
                    if ( BLE_HS_ENOTSUP == rc )
                    {
                        (void)os_mbuf_free_chain(om);
                    }
                    kdrv_ble_hid_pop(0 == rc);
                }
            }
        }
    }
}

void kdrv_ble_hid_kick(void)
{
    hid_host_t *const h = hid_host();

    /* Posted to the host's own queue, as the console's wake-up is. */
    if ( (NULL != h) && h->live && kdrv_ble_hid_queued() )
    {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &h->ev);
    }
}

void kdrv_ble_battery_set(uint8_t pct)
{
    app_host_t *const a = app_host();

    /* Posted to the host's own queue, as the drain's wake-up is, and only
     * for a level that moved: AO_Power publishes every reading. */
    bool const moved = (atomic_exchange(&s_batt_pct, pct) != pct);
    bool const first = !atomic_exchange(&s_batt_known, true);

    if ( (moved || first) && (NULL != a) && a->batt_live )
    {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &a->batt_ev);
    }
}

/** @brief The host is going: no kick reaches its queue until the next start sets the drain up. */
static void app_quiet(void)
{
    app_host_t *const a = app_host();

    if ( NULL != a )
    {
        a->live      = false;
        a->batt_live = false;
        a->hid.live  = false;
    }
}

#else  /* !CONFIG_KODEOS_BLE_CONSOLE */
/* No console's service for the app's to ride beside, so no structures of
 * its own in the block and nothing to wake. */
size_t kdrv_ble_app_host_bytes(void)
{
    return 0U;
}

void kdrv_ble_app_kick(void)
{
}

void kdrv_ble_hid_kick(void)
{
}

void kdrv_ble_battery_set(uint8_t pct)
{
    (void)pct;
}

bool kdrv_ble_console_attached(void)
{
    return false;
}

uint16_t kdrv_ble_console_tx_drop(void)
{
    return 0U;
}

uint16_t kdrv_ble_console_backlog(void)
{
    return 0U;
}

uint16_t kdrv_ble_console_retry(void)
{
    return 0U;
}

kode_status_t kdrv_ble_console_write_confirmed(const char *buf, size_t len)
{
    (void)buf;
    (void)len;
    return KODE_DENIED;
}

kode_status_t kdrv_ble_console_set_sent_cb(kdrv_ble_console_sent_t cb)
{
    (void)cb;
    return KODE_OK;
}

bool kdrv_ble_mirror_attached(void)
{
    return false;
}

kode_status_t kdrv_ble_mirror_write(const uint8_t *data, size_t len)
{
    (void)data;
    (void)len;
    return KODE_DENIED;
}
#endif /* CONFIG_KODEOS_BLE_CONSOLE */

/* ---- NimBLE callbacks (host task) --------------------------------------- */

#if CONFIG_KODEOS_BLE_CONSOLE
static ble_store_write_fn  *s_store_write;    /**< NimBLE's own, wrapped. */
static ble_store_delete_fn *s_store_delete;   /**< NimBLE's own, wrapped. */

/* ---- What each bond was made for --------------------------------------------
 * A bond is the console's or the HID service's, never both
 * (kdrv_ble_hid_table.h): the mark is set when NimBLE writes the bond, by
 * what the board serves then, and goes when the bond is deleted. It is kept
 * by the device's identity address, the one the bond is stored under. In a
 * table that serves HID the console's characteristics and the HID service's
 * ask who is on the link before each access (BLE_GAP_EVENT_AUTHORIZE); a
 * table without HID asks nobody. A device with a HID bond comes back by
 * itself whenever it sees the board, and a boot that serves no HID hangs up
 * on it. The host task's alone, but for kdrv_ble_unpair(), which runs with
 * no link. */

/** @brief The devices with a HID bond, as KDRV_BLE_HID_NVS_HOSTS keeps them. */
static kdrv_ble_hid_hosts_t s_hid_hosts;

/** @brief This link has been told, once, which service its bond does not open. */
static bool s_author_said;

/**
 * @brief Write a device's identity address the way the hosts keep it.
 * @param addr NimBLE's address.
 * @param out  Receives KDRV_BLE_HID_ADDR_LEN bytes: the type, then the six bytes.
 */
static void hid_host_addr(const ble_addr_t *addr, uint8_t *out)
{
    out[0] = addr->type;
    (void)memcpy(&out[1], addr->val, sizeof(addr->val));
}

_Static_assert(KDRV_BLE_HID_ADDR_LEN == (1U + sizeof(((ble_addr_t *)NULL)->val)),
               "a host is kept as its address type and its six bytes");
_Static_assert(KDRV_BLE_HID_HOSTS_MAX == (unsigned)CONFIG_BT_NIMBLE_MAX_BONDS,
               "one host can be remembered for each bond");

/**
 * @brief Whether the device of @p conn has a HID bond.
 * @param conn The connection.
 * @return true for a HID bond; false for a console bond, or no such link.
 */
static bool hid_bond_of(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    uint8_t                  addr[KDRV_BLE_HID_ADDR_LEN];
    bool                     hid = false;

    if ( ble_gap_conn_find(conn, &desc) == 0 )
    {
        hid_host_addr(&desc.peer_id_addr, addr);
        hid = kdrv_ble_hid_hosts_has(&s_hid_hosts, addr);
    }
    return hid;
}

#ifdef CONFIG_KODEOS_BLE_CONSOLE_PAIRING
/**
 * @brief NimBLE wrote the bond of @p peer: mark it by what the board serves now.
 * @param peer Its identity address.
 */
static void hid_bond_written(const ble_addr_t *peer)
{
    uint8_t addr[KDRV_BLE_HID_ADDR_LEN];

    hid_host_addr(peer, addr);
    if ( kdrv_ble_hid_bond_note(&s_hid_hosts, addr, kdrv_ble_hid_served()) )
    {
        kdrv_ble_hid_hosts_store(&s_hid_hosts);
    }
}

/**
 * @brief The bond of @p peer is being deleted: its mark goes with it.
 * @param peer Its identity address.
 */
static void hid_bond_gone(const ble_addr_t *peer)
{
    uint8_t addr[KDRV_BLE_HID_ADDR_LEN];

    hid_host_addr(peer, addr);
    if ( kdrv_ble_hid_hosts_drop(&s_hid_hosts, addr) )
    {
        kdrv_ble_hid_hosts_store(&s_hid_hosts);
    }
}

/**
 * @brief kdrv_ble_hid_bonded_t: whether the bond store holds a bond for @p addr.
 * @param addr The device's identity address.
 * @param ctx  Unused.
 * @return true when it does.
 */
static bool hid_host_bonded(const uint8_t *addr, void *ctx)
{
    (void)ctx;

    struct ble_store_key_sec   key;
    struct ble_store_value_sec sec;

    (void)memset(&key, 0, sizeof(key));
    key.peer_addr.type = addr[0];
    (void)memcpy(key.peer_addr.val, &addr[1], sizeof(key.peer_addr.val));
    return ble_store_read_peer_sec(&key, &sec) == 0;
}

/** @brief Read the marks the last boot kept, less the ones whose bond is gone. Host task. */
static void hid_hosts_load(void)
{
    kdrv_ble_hid_hosts_load(&s_hid_hosts);
    if ( kdrv_ble_hid_hosts_sift(&s_hid_hosts, &hid_host_bonded, NULL) )
    {
        kdrv_ble_hid_hosts_store(&s_hid_hosts);
    }
}

/**
 * @brief Judge the device of @p conn, whose link has just been encrypted.
 *
 * A device with a HID bond that comes back while no HID is served is hung up
 * on, so the one link stays free for the console and the phone.
 *
 * @param conn The connection.
 */
static void hid_host_check(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    uint8_t                  addr[KDRV_BLE_HID_ADDR_LEN];

    if ( ble_gap_conn_find(conn, &desc) == 0 )
    {
        hid_host_addr(&desc.peer_id_addr, addr);
        if ( KDRV_BLE_HID_HOST_HANG_UP
             == kdrv_ble_hid_host_judge(&s_hid_hosts, addr, kdrv_ble_hid_served()) )
        {
            KLOG_N(TAG, "hid: a host of the board's mouse or keyboard came back and none "
                        "is served; hung up, the link is free");
            (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        }
    }
}
#endif /* CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
#endif /* CONFIG_KODEOS_BLE_CONSOLE */

#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
/* ---- The bond store keeps tokens; the keys live on the C5 -----------------
 * kdrv_ble_tokens.h says what a token is. Every write of a bond record moves
 * its LTK (and the peer's IRK) to the C5 first and stores tokens in their
 * place; a write the C5 does not take fails, so the pairing fails and a key
 * never lands in this chip's NVS. Every delete tells the C5 to forget them.
 * Both run on the task that changes the store (NimBLE's host task at a
 * pairing, the radio worker at an unpairing), inside ble_hs_lock, which
 * NimBLE takes around its store callbacks, and block there for one KODE
 * round trip per key. Whoever needs the store meanwhile waits for them, which
 * is why the count of bonds is kept apart (kdrv_ble_bonds.h).
 *
 * NimBLE drops what the write returns (ble_sm_persist_keys()), so a refused
 * key does not fail the pairing by itself: the write records the peer, and
 * the encryption change that follows undoes the bond and ends the link. */

/** @brief How long the C5 has to take a key. */
#define BLE_KEY_PUT_MS  3000U

static bool                 s_key_refused;    /**< A key was not kept...  */
static ble_addr_t           s_key_refused_peer; /**< ...for this peer.    */

/** @brief kbt_put_t: one key to the C5's BTKEYS service. */
static kode_status_t ble_key_put(const uint8_t tok[16], const uint8_t key[16])
{
    kode_btkey_t  k;
    uint8_t       st  = KODE_ST_REFUSED;
    size_t        n   = 0U;
    kode_status_t rc;

    (void)memcpy(k.token, tok, sizeof(k.token));
    (void)memcpy(k.key, key, sizeof(k.key));
    rc = kdrv_c5svc_call(KODE_SVC_BTKEYS, KODE_BTKEYS_PUT, &k, sizeof(k), &st,
                         NULL, 0U, &n, BLE_KEY_PUT_MS);
    (void)memset(&k, 0, sizeof(k));
    if ( (KODE_OK == rc) && (KODE_ST_OK != st) )
    {
        rc = KODE_DENIED;
    }
    return rc;
}

/** @brief The C5 forgets one token; best effort, logged. */
static void ble_key_del(const uint8_t tok[16])
{
    uint8_t st = KODE_ST_REFUSED;
    size_t  n  = 0U;

    if ( !kbt_is_token(tok) )
    {
        return;
    }
    if ( (kdrv_c5svc_call(KODE_SVC_BTKEYS, KODE_BTKEYS_DEL, tok, 16U, &st,
                         NULL, 0U, &n, BLE_KEY_PUT_MS) != KODE_OK)
        || (KODE_ST_OK != st) )
    {
        KLOG_W(TAG, "the C5 did not forget a bond's key (status %u)", (unsigned)st);
    }
}

/** @brief ble_hs_cfg.store_write_cb: keys to the C5, tokens to the store. */
static int ble_store_write_c5(int obj_type, const union ble_store_value *val)
{
    union ble_store_value v = *val;

    if ( (BLE_STORE_OBJ_TYPE_OUR_SEC == obj_type)
        || (BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type) )
    {
        uint8_t role = KBT_ROLE_OUR;
        uint8_t *ltk = NULL;
        uint8_t *irk = NULL;
        if ( BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type )
        {
            role = KBT_ROLE_PEER;
        }
        if ( 0U != v.sec.ltk_present )
        {
            ltk = v.sec.ltk;
        }
        /* Only the peer's IRK is a secret to move: ours is the local IRK,
         * which the controller needs as it is. */
        if ( (BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type) && (0U != v.sec.irk_present) )
        {
            irk = v.sec.irk;
        }
        kode_status_t const rc = kbt_tokenize(ltk, irk, role, v.sec.peer_addr.val,
                                              v.sec.peer_addr.type, &ble_key_put);
        if ( KODE_OK != rc )
        {
            KLOG_W(TAG, "the C5 did not take a bond's key (%d): the bond is "
                        "refused", (int)rc);
            s_key_refused      = true;
            s_key_refused_peer = v.sec.peer_addr;
            (void)memset(&v, 0, sizeof(v));
            return BLE_HS_EUNKNOWN;
        }
    }
    int const err = s_store_write(obj_type, &v);
    (void)memset(&v, 0, sizeof(v));
    if ( (0 == err) && (BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type) )
    {
        /* A bond is written: it is the console's or the HID service's from here. */
        hid_bond_written(&val->sec.peer_addr);
    }
    return err;
}

/** @brief ble_hs_cfg.store_delete_cb: the C5 forgets, then the store does. */
static int ble_store_delete_c5(int obj_type, const union ble_store_key *key)
{
    if ( (BLE_STORE_OBJ_TYPE_OUR_SEC == obj_type)
        || (BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type) )
    {
        union ble_store_value v;
        if ( ble_store_read(obj_type, key, &v) == 0 )
        {
            if ( 0U != v.sec.ltk_present )
            {
                ble_key_del(v.sec.ltk);
            }
            if ( 0U != v.sec.irk_present )
            {
                ble_key_del(v.sec.irk);
            }
            if ( BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type )
            {
                hid_bond_gone(&v.sec.peer_addr);
            }
        }
        (void)memset(&v, 0, sizeof(v));
    }
    return s_store_delete(obj_type, key);
}

/** @brief Bonds one start can move to tokens: as many as the store keeps,
 *         ours and the peers'. */
#define BLE_MIGRATE_MAX  (2U * (uint32_t)CONFIG_BT_NIMBLE_MAX_BONDS)

/** @brief The bonds found holding a real key, gathered before any is moved. */
typedef struct {
    int        obj[BLE_MIGRATE_MAX];    /**< BLE_STORE_OBJ_TYPE_*.         */
    ble_addr_t addr[BLE_MIGRATE_MAX];   /**< Its peer.                     */
    uint32_t   n;                       /**< How many.                     */
} ble_migrate_t;

/** @brief ble_store_iterate()'s callback: note a record with a real key. */
static int ble_migrate_find(int obj_type, union ble_store_value *val, void *cookie)
{
    /* cppcheck-suppress misra-c2012-11.5 ; NimBLE hands the cookie back as void * */
    ble_migrate_t *const m = (ble_migrate_t *)cookie;
    bool raw = (0U != val->sec.ltk_present) && !kbt_is_token(val->sec.ltk);

    if ( (BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type) && (0U != val->sec.irk_present)
        && !kbt_is_token(val->sec.irk) )
    {
        raw = true;
    }
    if ( raw && (BLE_MIGRATE_MAX > m->n) )
    {
        m->obj[m->n]  = obj_type;
        m->addr[m->n] = val->sec.peer_addr;
        m->n++;
    }
    return 0;
}

/**
 * @brief Move the keys of bonds made before the tokens to the C5.
 *
 * A bond written by an older build holds its real LTK and IRK in this chip's
 * NVS. Each one goes to the C5 now and the store keeps its token, as a new
 * pairing would. A key the C5 refuses takes its bond with it, so no real key
 * stays here; a C5 that does not answer leaves the bond for the next start.
 */
static void ble_migrate_old_keys(void)
{
    static ble_migrate_t m;
    uint32_t moved   = 0U;
    uint32_t dropped = 0U;

    (void)memset(&m, 0, sizeof(m));
    (void)ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, &ble_migrate_find, &m);
    (void)ble_store_iterate(BLE_STORE_OBJ_TYPE_OUR_SEC, &ble_migrate_find, &m);
    for ( uint32_t i = 0U; i < m.n; ++i )
    {
        union ble_store_key   key;
        union ble_store_value v;
        uint8_t               role = KBT_ROLE_OUR;
        uint8_t              *ltk  = NULL;
        uint8_t              *irk  = NULL;

        (void)memset(&key, 0, sizeof(key));
        key.sec.peer_addr = m.addr[i];
        if ( ble_store_read(m.obj[i], &key, &v) != 0 )
        {
            continue;
        }
        if ( BLE_STORE_OBJ_TYPE_PEER_SEC == m.obj[i] )
        {
            role = KBT_ROLE_PEER;
            if ( 0U != v.sec.irk_present )
            {
                irk = v.sec.irk;
            }
        }
        if ( 0U != v.sec.ltk_present )
        {
            ltk = v.sec.ltk;
        }
        kode_status_t const rc = kbt_tokenize(ltk, irk, role, v.sec.peer_addr.val,
                                              v.sec.peer_addr.type, &ble_key_put);
        if ( KODE_OK == rc )
        {
            (void)s_store_write(m.obj[i], &v);
            moved++;
        }
        else if ( KODE_DENIED == rc )
        {
            (void)ble_store_util_delete_peer(&m.addr[i]);
            dropped++;
        }
        else
        {
            /* The C5 did not answer: kept for the next start. */
        }
        (void)memset(&v, 0, sizeof(v));
    }
    (void)memset(&m, 0, sizeof(m));
    if ( (moved + dropped) > 0U )
    {
        KLOG_N(TAG, "old bonds: %u keys moved to the C5, %u bonds dropped",
               (unsigned)moved, (unsigned)dropped);
    }
}

/**
 * @brief Whether the pairing that just encrypted @p conn had a key the C5
 *        refused (ble_store_write_c5()). Clears the record either way.
 */
static bool ble_key_refused_for(uint16_t conn)
{
    struct ble_gap_conn_desc desc;
    bool refused = false;

    if ( s_key_refused && (ble_gap_conn_find(conn, &desc) == 0) )
    {
        refused = (ble_addr_cmp(&desc.peer_id_addr, &s_key_refused_peer) == 0);
    }
    s_key_refused = false;
    return refused;
}

/**
 * @brief A bonded peer's link ended because its key failed: the C5 does not
 *        know the bond's token (a wipe, a C5 swapped in repair), or the peer
 *        lost its half. Forget the bond so the next attempt pairs again.
 * @param peer   The peer's identity address.
 * @param reason The disconnect reason (a ble_hs code).
 */
static void ble_forget_lost_key(const ble_addr_t *peer, int reason)
{
    struct ble_store_key_sec   k;
    struct ble_store_value_sec v;
    uint8_t                    token[KODE_TOKEN_LEN];
    bool                       has_token = false;

    (void)memset(&k, 0, sizeof(k));
    k.peer_addr = *peer;
    bool const bonded = (ble_store_read_peer_sec(&k, &v) == 0);
    if ( bonded && (0U != v.ltk_present) && kbt_is_token(v.ltk) )
    {
        (void)memcpy(token, v.ltk, sizeof(token));
        has_token = true;
    }
    (void)memset(&v, 0, sizeof(v));
    if ( !kbt_forget_on_enc_fail(reason, bonded) || !has_token )
    {
        return;
    }
    /* Only the C5 can tell a key it lost from a device spoofing the peer's
     * address: ask it before anything is forgotten. */
    uint8_t             st = KODE_ST_REFUSED;
    size_t              n  = 0U;
    kode_status_t const rc = kdrv_c5svc_call(KODE_SVC_BTKEYS, KODE_BTKEYS_HAS,
                                             token, sizeof(token), &st,
                                             NULL, 0U, &n, BLE_KEY_PUT_MS);
    if ( kbt_c5_lost_key(rc, st) )
    {
        KLOG_W(TAG, "bond %02x:%02x:%02x:%02x:%02x:%02x lost its key; it is "
                    "forgotten and the next connection pairs again",
               peer->val[5], peer->val[4], peer->val[3],
               peer->val[2], peer->val[1], peer->val[0]);
        (void)ble_store_util_delete_peer(peer);
    }
    else
    {
        KLOG_W(TAG, "bond %02x:%02x:%02x:%02x:%02x:%02x failed to encrypt but "
                    "its key is still here (%d, status %u); it is kept",
               peer->val[5], peer->val[4], peer->val[3],
               peer->val[2], peer->val[1], peer->val[0], (int)rc, (unsigned)st);
    }
}
#endif /* CONFIG_KODEOS_BLE_CONSOLE_PAIRING */

#if CONFIG_KODEOS_BLE_CONSOLE
/* ---- The count of bonded peers ---------------------------------------------
 * NimBLE calls the store's write and delete callbacks inside its host lock,
 * and a write may hold it for a round trip to the C5 per key. Whoever asks
 * how many peers are bonded must not wait behind that, so both callbacks are
 * wrapped: each counts the peers after its change, inside the same hold of
 * the lock, and kdrv_ble_bonds.h keeps the count for readers. */

/** @brief The count of bonds moved: whoever watches the BLE state reads it again. */
static void ble_bonds_tell(void)
{
    if ( NULL != s_ble.cb )
    {
        s_ble.cb(s_ble.state);
    }
}

/* The count below reads the store through ble_store_read(), which takes the
 * host lock again under the task that holds it. The lock is recursive, but a
 * host built with its debug checks asserts on the second take. */
#if CONFIG_BT_NIMBLE_DEBUG
#error "the bond count is made inside NimBLE's host lock, which its debug build asserts on: leave CONFIG_BT_NIMBLE_DEBUG off"
#endif

/**
 * @brief Count the peers after a change of the store, inside the hold of the
 *        host lock that made the change.
 *
 * The count takes the lock again for each record it reads, which the task
 * that holds it may do.
 */
static void ble_bonds_recount(void)
{
    int n = 0;

    if ( ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) != 0 )
    {
        KLOG_W(TAG, "the bond store could not be counted; the last count stands");
    }
    else if ( kdrv_ble_bonds_changed((uint8_t)n) )
    {
        ble_bonds_tell();
    }
    else
    {
        /* A key written over the same peer's: the count stands. */
    }
}

/**
 * @brief Count the peers outside the host lock: at sync, when no change of
 *        the store is this task's own.
 *
 * A change that comes while the peers are counted makes this count an old
 * one, and the keeper drops it for the change's own.
 */
static void ble_bonds_count(void)
{
    uint32_t const ticket = kdrv_ble_bonds_begin();
    int            n      = 0;

    if ( ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) != 0 )
    {
        KLOG_W(TAG, "the bond store could not be counted; the last count stands");
    }
    else if ( kdrv_ble_bonds_settle(ticket, (uint8_t)n) )
    {
        ble_bonds_tell();
    }
    else
    {
        /* The count kept already was this one, or a change left a newer. */
    }
}

/** @brief ble_hs_cfg.store_write_cb: the write, then the count it leaves. */
static int ble_store_write_counted(int obj_type, const union ble_store_value *val)
{
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
    int const err = ble_store_write_c5(obj_type, val);
#else
    int const err = s_store_write(obj_type, val);
#endif

    if ( BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type )
    {
        ble_bonds_recount();
    }
    return err;
}

/** @brief ble_hs_cfg.store_delete_cb: the delete, then the count it leaves. */
static int ble_store_delete_counted(int obj_type, const union ble_store_key *key)
{
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
    int const err = ble_store_delete_c5(obj_type, key);
#else
    int const err = s_store_delete(obj_type, key);
#endif

    if ( BLE_STORE_OBJ_TYPE_PEER_SEC == obj_type )
    {
        ble_bonds_recount();
    }
    return err;
}

/** @brief Install the two wrappers, once per stock set of callbacks. */
static void ble_store_hook(void)
{
    /* Not at ble_store_config_init(): ble_hs_pvcy re-runs it before sync and
     * puts the stock callbacks back (ble_hs_pvcy.c:337, found in spike B). */
    if ( ble_hs_cfg.store_write_cb != &ble_store_write_counted )
    {
        s_store_write  = ble_hs_cfg.store_write_cb;
        s_store_delete = ble_hs_cfg.store_delete_cb;
        ble_hs_cfg.store_write_cb  = &ble_store_write_counted;
        ble_hs_cfg.store_delete_cb = &ble_store_delete_counted;
    }
}
#endif /* CONFIG_KODEOS_BLE_CONSOLE */

/** @brief The first and the last handle an attribute can have: Service
 *         Changed over this range says the whole table may be new. */
#define BLE_ATT_HANDLE_FIRST  0x0001U
#define BLE_ATT_HANDLE_LAST   0xFFFFU

#ifdef CONFIG_KODEOS_BLE_CONSOLE
/**
 * @brief ble_store_iterate()'s callback: stop at the first stored
 *        subscription that is not Service Changed's, and hand back its key.
 * @param obj_type The object's type: a subscription.
 * @param val      The stored subscription.
 * @param cookie   A cccd_prune_t: the handle to keep, and where the key goes.
 * @return 0 to go on; 1 once one was found.
 */
static int cccd_prune_find(int obj_type, union ble_store_value *val, void *cookie)
{
    /* cppcheck-suppress misra-c2012-11.5 ; the cookie is the cccd_prune_t this file passed */
    cccd_prune_t *const p = (cccd_prune_t *)cookie;
    int                 rc = 0;

    if ( (BLE_STORE_OBJ_TYPE_CCCD == obj_type)
        && kdrv_ble_hid_prune_takes(val->cccd.chr_val_handle, p->keep) )
    {
        ble_store_key_from_value_cccd(&p->key, &val->cccd);
        p->found = true;
        rc       = 1;
    }
    return rc;
}

/**
 * @brief Forget the subscriptions the bonds stored on another table.
 *
 * NimBLE keeps a subscription by its characteristic's handle and applies it,
 * when its bond comes back, to whichever characteristic has that handle now.
 * A table with the HID service has a print, one without has none, and both
 * are compared with the print the last boot left: when they differ, every
 * stored subscription but Service Changed's goes, so that one still tells
 * each bond to read the table again. A board that never serves HID compares
 * none with none, and neither forgets nor writes anything. Host task.
 */
static void ble_cccd_prune(void)
{
    uint32_t was = KDRV_BLE_HID_PRINT_NONE;
    size_t   len = sizeof(was);

    if ( (khal_nvs_get_blob(BLE_PRINT_NS, BLE_PRINT_KEY, &was, &len) != KODE_OK)
        || (sizeof(was) != len) )
    {
        was = KDRV_BLE_HID_PRINT_NONE;
    }
    if ( kdrv_ble_hid_prune_due(was, s_table_print) )
    {
        cccd_prune_t p      = { .keep = 0U, .found = true };
        uint32_t     pruned = 0U;

        (void)ble_gatts_find_chr(&s_gatt_svc_uuid.u, &s_gatt_changed_uuid.u, NULL, &p.keep);
        for ( uint32_t i = 0U; p.found && ((uint32_t)CONFIG_BT_NIMBLE_MAX_CCCDS > i); ++i )
        {
            p.found = false;
            (void)ble_store_iterate(BLE_STORE_OBJ_TYPE_CCCD, &cccd_prune_find, &p);
            if ( p.found && (ble_store_delete_cccd(&p.key) == 0) )
            {
                ++pruned;
            }
        }
        if ( KDRV_BLE_HID_PRINT_NONE == s_table_print )
        {
            (void)khal_nvs_erase(BLE_PRINT_NS, BLE_PRINT_KEY);
        }
        else
        {
            (void)khal_nvs_set_blob(BLE_PRINT_NS, BLE_PRINT_KEY, &s_table_print,
                                    sizeof(s_table_print));
        }
        KLOG_N(TAG, "hid: the GATT table changed; %u stored subscription(s) forgotten",
               (unsigned)pruned);
    }
}
#endif /* CONFIG_KODEOS_BLE_CONSOLE */

/** @brief The controller answered: from here the stack knows its own address. */
static void ble_on_sync(void)
{
    (void)ble_hs_util_ensure_addr(0);
    if ( ble_hs_id_infer_auto(0, &s_ble.own_addr_type) != 0 )
    {
        s_ble.own_addr_type = BLE_OWN_ADDR_PUBLIC;
    }

    uint8_t a[6];
    if ( ble_hs_id_copy_addr(BLE_ADDR_PUBLIC, a, NULL) == 0 )
    {
        /* NimBLE keeps addresses little-endian; everyone who reads one
         * expects the other order, so it is turned here rather than at each
         * of the places that print it. */
        for ( uint8_t i = 0U; 6U > i; ++i )
        {
            s_ble.addr[i] = a[5U - i];
        }
    }
#if CONFIG_KODEOS_BLE_CONSOLE
    ble_store_hook();
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
    ble_migrate_old_keys();
#endif
    /* What the store holds at this start, after the old keys have moved. */
    ble_bonds_count();
    ble_cccd_prune();
#ifdef CONFIG_KODEOS_BLE_CONSOLE_PAIRING
    hid_hosts_load();
#endif
#endif
    /* A bonded peer keeps the attribute table it found on its last visit,
     * and the table is not the same from one firmware to the next: kodeOS
     * and each app build their own. Without Service Changed, iOS reuses the
     * table it kept and never finds an app's service. NimBLE marks every
     * bond that subscribed to it, and indicates once on that bond's next
     * encrypted connection, until the peer acknowledges. */
    ble_svc_gatt_changed((uint16_t)BLE_ATT_HANDLE_FIRST,
                         (uint16_t)BLE_ATT_HANDLE_LAST);
    s_ble.synced = true;
    (void)xEventGroupSetBits(s_ble.bits, BLE_BIT_SYNCED);
    KLOG_D(TAG, "controller in sync: %02x:%02x:%02x:%02x:%02x:%02x",
           s_ble.addr[0], s_ble.addr[1], s_ble.addr[2],
           s_ble.addr[3], s_ble.addr[4], s_ble.addr[5]);
}

/**
 * @brief NimBLE's reset callback: the controller went away; stop claiming sync and go IDLE.
 *
 * Runs on NimBLE's host task.
 *
 * @param reason The reason NimBLE gives; only logged.
 */
static void ble_on_reset(int reason)
{
    /* The controller went away under us - the C5 was reset, most likely.
     * Say so and stop claiming to be synchronised; AO_Radio's link handling
     * is what actually recovers. */
    KLOG_W(TAG, "controller reset (reason %d)", reason);
    s_ble.synced = false;
    /* The stack reported the scan's end before this callback, and the hook
     * was told then; the flag is cleared again in case it did not. */
    s_ble.hearing = false;
    kdrv_ble_hid_link_reset();
    pulse_train_end();
    ble_set_state(KDRV_BLE_IDLE);
}

/**
 * @brief NimBLE's host task: run the stack until nimble_port_stop(), then end the task.
 * @param param Unused.
 */
static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();              /* returns on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/** @brief The device table's alignment: its widest member, svc16[]. */
#define BLE_DEV_ALIGN        sizeof(uint16_t)
/** @brief The bytes of a 16-bit UUID in service data. */
#define BLE_UUID16_BYTES     2U
/** @brief Where the high byte of a little-endian 16-bit UUID goes. */
#define BLE_UUID16_HI_SHIFT  8U

/**
 * @brief Keep what an advert says about the device beyond its name.
 *
 * A field the device already holds is left alone: the advert and its scan
 * response each carry part of it, and the first one that has a field fills it.
 *
 * @param dev    The device.
 * @param d      The packet, for the address type.
 * @param f      Its parsed fields.
 * @param parsed Whether NimBLE could parse it; the address type is kept
 *               either way.
 */
static void ble_take_fields(kdrv_ble_dev_t * const dev,
                            const struct ble_gap_disc_desc * const d,
                            const struct ble_hs_adv_fields * const f,
                            bool parsed)
{
    if ( (BLE_ADDR_PUBLIC == d->addr.type) || (BLE_ADDR_PUBLIC_ID == d->addr.type) )
    {
        dev->addr_type = (uint8_t)KDRV_BLE_ADDR_PUBLIC;
    }
    else
    {
        dev->addr_type = (uint8_t)KDRV_BLE_ADDR_RANDOM;
    }
    if ( !parsed )
    {
        return;
    }
    if ( (0U == dev->mfg_len) && (NULL != f->mfg_data) && (0U < f->mfg_data_len) )
    {
        uint8_t n = f->mfg_data_len;
        if ( KDRV_BLE_MFG_MAX < n )
        {
            n = (uint8_t)KDRV_BLE_MFG_MAX;
        }
        (void)memcpy(dev->mfg, f->mfg_data, n);
        dev->mfg_len = n;
    }
    if ( 0U == dev->svc_n )
    {
        if ( NULL != f->uuids16 )
        {
            for ( uint8_t i = 0U;
                 (i < f->num_uuids16) && (KDRV_BLE_SVC_MAX > dev->svc_n); ++i )
            {
                dev->svc16[dev->svc_n] = f->uuids16[i].value;
                ++dev->svc_n;
            }
        }
        if ( (NULL != f->svc_data_uuid16)
            && (BLE_UUID16_BYTES <= f->svc_data_uuid16_len)
            && (KDRV_BLE_SVC_MAX > dev->svc_n) )
        {
            dev->svc16[dev->svc_n] =
                (uint16_t)((uint16_t)f->svc_data_uuid16[0]
                           | (uint16_t)((uint16_t)f->svc_data_uuid16[1]
                                        << BLE_UUID16_HI_SHIFT));
            ++dev->svc_n;
        }
    }
}

/* kdrv_ble_scan_find()'s target and verdict: begun by the worker before the
 * scan starts, offered to by the host task while it runs. */
static kdrv_ble_find_t s_find;

/** @brief Set by kdrv_ble_scan_abort(): the listen in flight ends now, and one
 *         that starts ends before it listens. */
static volatile bool s_scan_abort;

/** Is this advertisement (or scan response) the one kdrv_ble_scan_find() wants? */
static void ble_find_check(const struct ble_gap_disc_desc *d)
{
    struct ble_hs_adv_fields f;

    if ( ble_hs_adv_parse_fields(&f, d->data, d->length_data) != 0 )
    {
        return;
    }

    uint8_t addr_type = KDRV_BLE_ADDR_RANDOM;
    if ( (BLE_ADDR_PUBLIC == d->addr.type) || (BLE_ADDR_PUBLIC_ID == d->addr.type) )
    {
        addr_type = KDRV_BLE_ADDR_PUBLIC;
    }
    if ( kdrv_ble_find_offer(&s_find, d->addr.val, addr_type, d->rssi, f.name,
                            f.name_len) )
    {
        (void)xEventGroupSetBits(s_ble.bits, BLE_BIT_SCAN_DONE);
    }
}

/** Keep one heard advertisement for the stream, and tell the hook. */
static void ble_hear_record(const struct ble_gap_disc_desc *d)
{
    kdrv_ble_heard_t rec;

    kdrv_ble_heard_make(d->addr.val, d->addr.type, d->event_type, d->rssi, d->data,
                        d->length_data, &rec);
    /* Taken or counted as a drop: the owner reads both in the ring's counts,
     * so either way it is told that something was heard. */
    (void)kdrv_ble_heard_feed(&rec);
    if ( NULL != s_hear_cb )
    {
        s_hear_cb(KDRV_BLE_HEAR_RECORD);
    }
}

/** Remember one advertising device, or refresh it if we have seen it. */
static void ble_record(const struct ble_gap_disc_desc *d)
{
    if ( NULL == s_ble.dev )
    {
        return;   /* no scan has taken the table: nothing asked for this */
    }

    uint8_t addr[6];
    for ( uint8_t i = 0U; 6U > i; ++i )
    {
        addr[i] = d->addr.val[5U - i];
    }

    struct ble_hs_adv_fields f;
    bool const parsed =
        (ble_hs_adv_parse_fields(&f, d->data, d->length_data) == 0);

    for ( uint8_t i = 0U; i < s_ble.count; ++i )
    {
        if ( memcmp(s_ble.dev[i].addr, addr, 6U) == 0 )
        {
            /* Same device again: keep the strongest reading rather than the
             * last, which is the one that says how close it actually is. */
            if ( d->rssi > s_ble.dev[i].rssi )
            {
                s_ble.dev[i].rssi = d->rssi;
            }
            /* And take the name from whichever packet carries one - it is
             * usually the scan response, which arrives separately from the
             * advert that got us here. */
            if ( ('\0' == s_ble.dev[i].name[0]) && parsed
                && (NULL != f.name) && (0U < f.name_len) )
            {
                size_t const n = ((sizeof(s_ble.dev[i].name) - 1U) > f.name_len)
                               ? f.name_len : (sizeof(s_ble.dev[i].name) - 1U);
                (void)memcpy(s_ble.dev[i].name, f.name, n);
                s_ble.dev[i].name[n] = '\0';
            }
            ble_take_fields(&s_ble.dev[i], d, &f, parsed);
            return;
        }
    }
    if ( KDRV_BLE_SCAN_MAX <= s_ble.count )
    {
        return;
    }

    kdrv_ble_dev_t * const dev = &s_ble.dev[s_ble.count];
    (void)memset(dev, 0, sizeof(*dev));
    (void)memcpy(dev->addr, addr, sizeof(dev->addr));
    dev->rssi = d->rssi;

    if ( parsed && (NULL != f.name) && (0U < f.name_len) )
    {
        size_t const n = ((sizeof(dev->name) - 1U) > f.name_len)
                       ? f.name_len : (sizeof(dev->name) - 1U);
        (void)memcpy(dev->name, f.name, n);
        dev->name[n] = '\0';
    }
    ble_take_fields(dev, d, &f, parsed);
    ++s_ble.count;
}

#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
/**
 * @brief Whether a new device may pair now.
 *
 * In an app only while it serves the HID service and its time to pair runs
 * (kdrv_ble_hid_pair_window()): the app shows the code. Any other app pairs
 * nobody: a new phone pairs in kodeOS, where the code is shown on the glass,
 * and the bonds made there reach the app through the C5's keys and the P4's
 * tokens, both of which survive the jump. In kodeOS only while the board is
 * visible: a board that serves its bonded devices and hides from everyone
 * else does not take a new one (kdrv_ble_adv_plan()).
 *
 * @return true when a pairing may go on.
 */
static bool ble_pair_open(void)
{
#if CONFIG_KODEOS_BOOT_ROLE_APP
    return kdrv_ble_hid_pair_open();
#else
    return !s_ble.adv_hidden;
#endif
}

/**
 * @brief Turn a pairing away: log it, note it with its cause, optionally
 *        forget the peer, hang up.
 *
 * The note (KDRV_BLE_PAIR_REFUSED) only tells: nothing here opens the door,
 * and a phone that retries is noted at each try.
 *
 * @param conn   The connection that asked.
 * @param forget Delete the peer's bond too: for a pairing that completed
 *               without a code, which has no business holding a slot.
 */
static void ble_pair_refuse(uint16_t conn, bool forget)
{
    struct ble_gap_conn_desc desc;

#if CONFIG_KODEOS_BOOT_ROLE_APP
    if ( kdrv_ble_hid_served() )
    {
        KLOG_N(TAG, "pairing refused: the app's time to pair is not running");
    }
    else
    {
        KLOG_N(TAG, "pairing refused: a new phone pairs in kodeOS; this app "
                    "serves bonded phones");
    }
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    pair_note(KDRV_BLE_PAIR_REFUSED, KDRV_BLE_PAIR_APP_CLOSED, 0U);
#endif
#else
    KLOG_N(TAG, "pairing refused: the board is hidden; Pair new device "
                "makes it visible");
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    pair_note(KDRV_BLE_PAIR_REFUSED, KDRV_BLE_PAIR_HIDDEN, 0U);
#endif
#endif
    if ( forget && (ble_gap_conn_find(conn, &desc) == 0) )
    {
        (void)ble_store_util_delete_peer(&desc.peer_id_addr);
    }
    (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

/**
 * @brief ble_hs_cfg.store_status_cb: a full store makes room only for a
 *        pairing that may go on, and refuses any other.
 *
 * The round-robin rule drops the OLDEST bond when a new one arrives and the
 * store is full. While no new bond may be made (ble_pair_open()), a
 * stranger's request must not cost a bonded phone its place.
 *
 * @param event NimBLE's store event.
 * @param arg   Unused.
 * @return 0 to go on, a ble_hs code to refuse.
 */
static int ble_store_status(struct ble_store_status_event *event, void *arg)
{
    bool make_room = ble_pair_open();

#ifdef CONFIG_KODEOS_BOOT_ROLE_APP
    /* Never in an app, whose time to pair shows no card on the glass: the
     * oldest bond is the system's, and kodeOS is where one is given up. */
    if ( make_room && (BLE_STORE_EVENT_FULL == event->event_code) )
    {
        KLOG_N(TAG, "pairing: the bond store is full and an app makes no room; "
                    "forget a device in kodeOS");
    }
    make_room = false;
#endif
    if ( (BLE_STORE_EVENT_FULL == event->event_code) && !make_room )
    {
        return BLE_HS_ENOMEM;
    }
    return ble_store_util_status_rr(event, arg);
}
#endif /* CONFIG_KODEOS_BLE_CONSOLE_PAIRING */

#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
/**
 * @brief Undo a pairing that ended without a code while no pairing may go on.
 *
 * A pairing can finish with no PASSKEY_ACTION: a central that declares no
 * input and no display negotiates Just Works with a DisplayOnly board whatever
 * MITM flag it sets, because the table of IO capabilities picks the method.
 * It ends as an encrypted link that is not authenticated. A bond made with a
 * code is authenticated, because its passkey was typed, so an unauthenticated
 * link while the door is shut (ble_pair_open()) is a pairing that must not
 * stay. While a board is visible its characteristics refuse such a link by
 * themselves.
 *
 * @param conn   The connection whose encryption changed.
 * @param status The status of the change; only 0 is a link.
 * @return true when the link was refused and its peer forgotten.
 */
static bool ble_refuse_unproven(uint16_t conn, int status)
{
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    struct ble_gap_conn_desc link;

    if ( (0 == status) && !ble_pair_open() && (ble_gap_conn_find(conn, &link) == 0)
        && (0U == link.sec_state.authenticated) )
    {
        ble_pair_refuse(conn, true);
        return true;
    }
#else
    (void)conn;
    (void)status;
#endif
    return false;
}

/**
 * @brief Undo a pairing that finished after it was refused on the glass.
 *
 * kdrv_ble_pair_reject() says the refusal at once and leaves the hang-up to
 * the radio's worker, which may be busy for seconds: a peer that types the
 * code in between would end bonded. Here, on the host task, its bond is
 * forgotten and the link closed.
 *
 * @param conn   The connection whose encryption changed.
 * @param status The status of the change; only 0 is a link.
 * @return true when the link was refused and its peer forgotten.
 */
static bool ble_pair_refused_undo(uint16_t conn, int status)
{
    bool undone = false;
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    struct ble_gap_conn_desc link;

    if ( s_pair_refused && (0 == status) )
    {
        s_pair_refused = false;
        if ( ble_gap_conn_find(conn, &link) == 0 )
        {
            (void)ble_store_util_delete_peer(&link.peer_id_addr);
        }
        (void)ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
        undone = true;
    }
#else
    (void)conn;
    (void)status;
#endif
    return undone;
}
#endif /* CONFIG_KODEOS_BLE_CONSOLE_PAIRING */

#if CONFIG_KODE_LOWPOWER
/** GAP events seen since boot, for the radio's sleep sentinel (kdrv_ble_activity()). */
static atomic_uint s_gap_activity;

uint32_t kdrv_ble_activity(void)
{
    return (uint32_t)atomic_load(&s_gap_activity);
}
#endif

/** @brief Count a GAP event as the link moving, for the radio's sleep sentinel. */
static void ble_gap_moved(void)
{
#if CONFIG_KODE_LOWPOWER
    (void)atomic_fetch_add(&s_gap_activity, 1U);
#endif
}

/**
 * @brief Whether the link @p conn is encrypted already.
 *
 * NimBLE hands a peripheral its connect event only after it has read the
 * central's version and features. A bonded central that encrypts at once is
 * encrypted by then, and its ENC_CHANGE has been handled.
 *
 * @param conn The connection.
 * @return true when the link exists and is encrypted.
 */
static bool ble_link_encrypted(uint16_t conn)
{
    struct ble_gap_conn_desc link;
    bool encrypted = false;

    if ( ble_gap_conn_find(conn, &link) == 0 )
    {
        encrypted = (0U != link.sec_state.encrypted);
    }
    return encrypted;
}

/**
 * @brief Ask the central of @p conn for the awake set or the rest set.
 * @param conn The connection.
 * @param rest true for the rest set (kdrv_ble_conn_params()).
 * @return What ble_gap_update_params() answered; 0 when the request went out.
 */
static int ble_conn_ask(uint16_t conn, bool rest)
{
    kdrv_ble_conn_params_t p;
    struct ble_gap_upd_params up;

    kdrv_ble_conn_params(rest, &p);
    (void)memset(&up, 0, sizeof(up));
    up.itvl_min            = p.itvl_min;
    up.itvl_max            = p.itvl_max;
    up.latency             = p.latency;
    up.supervision_timeout = p.timeout;
    return ble_gap_update_params(conn, &up);
}

/**
 * @brief The GAP callback of every scan, advert and connection. Runs on NimBLE's host task.
 * @param event What happened.
 * @param arg   Unused.
 * @return 0; for a repeat-pairing event, BLE_GAP_REPEAT_PAIRING_IGNORE or _RETRY.
 */
static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    /* The end of an advert is counted below: the end of a pulse is not the link
     * moving, and one every interval would keep the sentinel awake. */
    if ( BLE_GAP_EVENT_ADV_COMPLETE != event->type )
    {
        ble_gap_moved();
    }
    /* Zero unless a case says otherwise: only the repeat-pairing event -
     * which exists only when pairing is built in - asks the caller to do
     * something with the answer. */
    int status = 0;
    switch ( event->type )
    {
        case BLE_GAP_EVENT_DISC:
            if ( s_find.on )
            {
                ble_find_check(&event->disc);
            }
            else if ( s_ble.hearing )
            {
                ble_hear_record(&event->disc);
            }
            else
            {
                ble_record(&event->disc);
            }
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            if ( s_ble.hearing )
            {
                /* The stack ended a scan that was asked to have no end:
                 * nobody waits on the scan's bit, the owner is told. */
                s_ble.hearing = false;
                if ( NULL != s_hear_cb )
                {
                    s_hear_cb(KDRV_BLE_HEAR_ENDED);
                }
            }
            else
            {
                (void)xEventGroupSetBits(s_ble.bits, BLE_BIT_SCAN_DONE);
            }
            break;
        case BLE_GAP_EVENT_CONNECT:
            /* The connection ends the advert, a pulse or a pause of its train,
             * and nothing restarts it. */
            pulse_train_end();
            if ( 0 == event->connect.status )
            {
                bool const encrypted = ble_link_encrypted(event->connect.conn_handle);

                KLOG_N(TAG, "connected");
                s_ble.conn = event->connect.conn_handle;
                ++s_ble.links;
                /* An encrypted link had its ENC_CHANGE already, and that
                 * said whether it is secure. */
                if ( !encrypted )
                {
                    s_ble.secure = false;
                }
#if CONFIG_KODEOS_BLE_CONSOLE
                kdrv_ble_app_link_reset();
                /* The HID service's subscriptions are not forgotten here: a
                 * bonded host that encrypts at once has them restored before
                 * this event, and writes them once in its life. The link's
                 * end forgot the last host's. A report queued before the
                 * handle above was known goes now. */
                kdrv_ble_hid_kick();
#endif
                ble_set_state(KDRV_BLE_CONNECTED);
                /* What a central offers by default is 30 ms, and this link is
                 * rationed by packets per connection event rather than by
                 * air time - so a shorter interval SHOULD buy throughput on
                 * a link rationed this way, for one call made once per
                 * connect. On the console path it did not measurably; see
                 * below. The call stays: it is still the correct thing to
                 * ask for on a packet-rationed link, and costs nothing extra
                 * to make once at connect time even where it does not move
                 * the number.
                 *
                 * 12 units is 1.25 ms units, i.e. 15 ms, and 15 ms is the
                 * floor: macOS silently refuses anything shorter and stays
                 * where it was, so asking for less buys a negotiation and no
                 * bandwidth.
                 *
                 * Measured 2026-08-28 on the console path, over a bonded
                 * link with the security manager and AO_Log both live: the
                 * mean is a few percent faster than baseline, but the spread
                 * runs from 20 s to 38.7 s on a screenshot that took 32-33.5 s
                 * before this call existed, and that slowest trial landed
                 * slower than either baseline run - so this is not a
                 * guaranteed per-run improvement, it is a mean that moved
                 * inside its own noise. Two changes tried alongside
                 * this one, a larger preferred MTU and a matching notify
                 * chunk size, made that transfer fail outright three trials
                 * in four - a security-manager timeout on this already
                 * bonded peer, the link stalling mid-transfer rather than
                 * finishing slow - and are not in this file; see the commit
                 * history for why. That timeout is most likely the security
                 * request below asked of a link the bonded central had
                 * encrypted already, a request the board does not make
                 * (ble_link_encrypted()). */
                if ( ble_conn_ask(event->connect.conn_handle, false) != 0 )
                {
                    /* Not fatal, and not worth a warning either: a central
                     * that declines simply keeps its own interval, which is
                     * what happened before this call existed. */
                    KLOG_D(TAG, "the central kept its own interval");
                }
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
                /* Ask for security now rather than letting the first
                 * protected access provoke it, and the reason is not
                 * cosmetic: a write-without-response that fails the
                 * permission check is dropped in SILENCE - NimBLE computes
                 * the ATT error and discards it, sending no error PDU. A
                 * client whose send() is write-without-response would wait
                 * forever for a pairing nobody started. With Just Works this
                 * completes in a few hundred milliseconds and asks the human
                 * nothing. A link that is encrypted already is not asked: the
                 * central has no need to answer, and the request would end
                 * 30 s later as a failed encryption that hangs up a link in
                 * use. */
                if ( !encrypted )
                {
                    (void)ble_gap_security_initiate(event->connect.conn_handle);
                }
#endif
            }
            else
            {
                /* The advert was consumed by the attempt either way, so the
                 * board is no longer on the air until someone says so. */
                ble_set_state(KDRV_BLE_IDLE);
            }
            break;
        case BLE_GAP_EVENT_DISCONNECT:
            /* IDLE, not a fresh advert. Re-arming here would mean calling
             * kdrv_ble_advertise() on NimBLE's own host task, and that call
             * begins with kdrv_ble_start() - a chain of RPCs to the C5 that
             * belongs to the kdrv_radio worker and to nobody else. The state
             * this publishes reaches AO_Radio as an event, which submits the
             * advert as a job, on the thread that may block for it. */
            KLOG_N(TAG, "disconnected (reason %d)",
                   event->disconnect.reason);
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
            /* A key that failed shows here, as the reason the link ended:
             * the encryption change itself only says the link is gone. */
            ble_forget_lost_key(&event->disconnect.conn.peer_id_addr,
                                event->disconnect.reason);
#endif
            s_ble.conn       = BLE_HS_CONN_HANDLE_NONE;
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
            /* Still in flight means the peer left with a code on the glass:
             * nobody here ended this, so the card has to say the device went
             * away rather than that the pairing failed. A rejection got here
             * first and cleared the flag, which is why that path does not
             * land in this branch. */
            if ( s_pair_live )
            {
                s_pair_live = false;
                pair_note(KDRV_BLE_PAIR_DONE, KDRV_BLE_PAIR_GONE, 0U);
            }
            s_pair_refused = false;   /* nobody left to hang up on */
#endif
#if CONFIG_KODEOS_BLE_CONSOLE
            kdrv_ble_app_link_reset();
            kdrv_ble_hid_link_reset();
            s_author_said    = false;
            s_ble.console_on = false;
            confirm_lost();   /* before tx_drop restarts under the mark */
            s_ble.tx_drop    = 0U;
            s_line_len       = 0U;
            s_line_over      = false;
            s_raw_on         = false;
            s_pend_len       = 0U;
            s_pend_off       = 0U;
            if ( NULL != s_ring_lock )
            {
                khal_mutex_lock(s_ring_lock);
                ring_init(&s_ring, s_ring_store, KDRV_BLE_RING_BYTES);
                khal_mutex_unlock(s_ring_lock);
            }
#if KDRV_BLE_MIRROR_PIPE
            s_ble.mirror_on   = false;
            s_mirror_pend_len = 0U;
            s_mirror_pend_off = 0U;
            if ( NULL != s_mirror_ring_lock )
            {
                khal_mutex_lock(s_mirror_ring_lock);
                ring_init(&s_mirror_ring, s_mirror_ring_store,
                         KDRV_BLE_MIRROR_RING_BYTES);
                khal_mutex_unlock(s_mirror_ring_lock);
            }
#endif
#endif
            ble_set_state(KDRV_BLE_IDLE);
            break;
#if CONFIG_KODEOS_BLE_CONSOLE
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            /* Hidden, the board serves the devices it knows: a new one
             * pairs after Pair new device, not by asking. An app picks no
             * code unless it serves the HID service and opened its time to
             * pair: a number nobody shows would be a pairing nobody can
             * finish, so whoever asked is hung up on.
             *
             * This side declares DisplayOnly, so the only action it has any
             * business answering is "show a number". A comparison or an
             * input request means the negotiated method does not match what
             * was declared, and answering one would be this stack lying
             * about what it is. Let the security manager's own timeout end
             * it. */
            if ( !ble_pair_open() )
            {
                ble_pair_refuse(event->passkey.conn_handle, false);
            }
            else if ( BLE_SM_IOACT_DISP == event->passkey.params.action )
            {
                struct ble_sm_io io;
                (void)memset(&io, 0, sizeof(io));
                io.action  = BLE_SM_IOACT_DISP;
                io.passkey = ble_passkey_pick();

                if ( ble_sm_inject_io(event->passkey.conn_handle, &io) == 0 )
                {
                    /* A new code is the user's to judge again: the one
                     * refused is gone, and its hang-up with it. */
                    s_pair_refused = false;
                    s_pair_live    = true;
                    /* The digits are NOT logged, here or anywhere. This line
                     * travels the BLE console itself, and a peer that is
                     * already bonded would read the next peer's code out of
                     * it. The screen is the only place they exist. */
                    KLOG_N(TAG, "pairing: a code is on the screen");
                    pair_note(KDRV_BLE_PAIR_SHOW, KDRV_BLE_PAIR_OK,
                              io.passkey);
                }
                else
                {
                    KLOG_W(TAG, "the code could not be handed to the "
                                "security manager");
                }
            }
            else
            {
                KLOG_W(TAG, "pairing asked for action %d, which a display-"
                            "only board cannot answer",
                       (int)event->passkey.params.action);
            }
            break;
        }
#endif /* CONFIG_KODEOS_BLE_PAIRING_PASSKEY */
        case BLE_GAP_EVENT_ENC_CHANGE: {
            bool const refused = ble_key_refused_for(event->enc_change.conn_handle);
            bool const undone  = ble_pair_refused_undo(event->enc_change.conn_handle,
                                                       event->enc_change.status);
            s_ble.secure = (0 == event->enc_change.status) && !refused && !undone;
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
            if ( s_pair_live )
            {
                kdrv_ble_pair_outcome_t how = KDRV_BLE_PAIR_FAILED;
                if ( (0 == event->enc_change.status) && !refused )
                {
                    how = KDRV_BLE_PAIR_OK;
                }
                s_pair_live = false;
                pair_note(KDRV_BLE_PAIR_DONE, how, 0U);
            }
#endif
            if ( undone )
            {
                KLOG_N(TAG, "a refused pairing finished anyway; it is undone");
            }
            else if ( ble_refuse_unproven(event->enc_change.conn_handle,
                                         event->enc_change.status) )
            {
                KLOG_D(TAG, "a link without a code was undone");
            }
            else if ( refused )
            {
                /* Half a bond would be worse than none: the peer keeps a
                 * bond this board cannot encrypt with. */
                struct ble_gap_conn_desc desc;
                KLOG_W(TAG, "the C5 did not keep this bond's keys; the "
                            "pairing is undone");
                if ( ble_gap_conn_find(event->enc_change.conn_handle, &desc) == 0 )
                {
                    (void)ble_store_util_delete_peer(&desc.peer_id_addr);
                }
                (void)ble_gap_terminate(event->enc_change.conn_handle,
                                        BLE_ERR_REM_USER_CONN_TERM);
            }
            else if ( 0 == event->enc_change.status )
            {
                KLOG_N(TAG, "bonded");
                hid_host_check(event->enc_change.conn_handle);
            }
            else
            {
                /* ble_hs return codes, and two are worth knowing by sight
                 * because both were paid for on the bench: 13 is ETIMEOUT,
                 * the security manager's own 30 s giving up - what a pairing
                 * that waited on a human used to end in; 12 is ECONTROLLER,
                 * which is usually a peer offering a key this side can no
                 * longer match (`ble_unpair` here, forget the board there). */
                KLOG_W(TAG, "encryption failed (status %d)",
                       event->enc_change.status);
                /* Whoever made it fail, the link goes: a peer that refuses
                 * the pairing and stays connected would hold the one link
                 * there is, and the board would be off the air for as long
                 * as it liked. Most failures end the link by themselves, and
                 * hanging up on one that is gone costs nothing. */
                (void)ble_gap_terminate(event->enc_change.conn_handle,
                                        BLE_ERR_AUTH_FAIL);
            }
            break;
        }
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            /* The peer lost its half of the bond - reinstalled, or reset.
             * Drop the stale one and let it try again. Retrying is no longer
             * free: with a passkey it puts a card on the glass and asks for
             * six digits. It is still the right answer, because the
             * alternative is a peer permanently unable to reconnect, and
             * because a re-pair is exactly the moment a human should be
             * shown what is happening.
             *
             * The event carries a connection handle rather than an address,
             * so the address is looked up - the shape bleprph uses.
             *
             * While no pairing may go on (ble_pair_open()) the bond stays:
             * nobody gets to erase a bond by asking to pair. In an app the
             * phone pairs again in kodeOS; in kodeOS, after Pair new device.
             * The request is ignored and the link dropped. */
            if ( !ble_pair_open() )
            {
                ble_pair_refuse(event->repeat_pairing.conn_handle, false);
                status = BLE_GAP_REPEAT_PAIRING_IGNORE;
            }
            else
            {
                struct ble_gap_conn_desc desc;
                if ( ble_gap_conn_find(event->repeat_pairing.conn_handle,
                                      &desc) == 0 )
                {
                    (void)ble_store_util_delete_peer(&desc.peer_id_addr);
                }
                status = BLE_GAP_REPEAT_PAIRING_RETRY;
            }
            break;
        }
#endif /* CONFIG_KODEOS_BLE_CONSOLE_PAIRING */
        case BLE_GAP_EVENT_AUTHORIZE: {
            /* Asked only in a table that serves HID, for an attribute of the
             * console's service or of the HID service: each is its own kind
             * of bond's alone. The device is told why in the log, once a link. */
            uint16_t   first    = 0U;
            bool const hid_attr = (ble_gatts_find_svc(&s_hid_svc_uuid.u, &first) == 0)
                                  && (first <= event->authorize.attr_handle);
            bool const hid_bond = hid_bond_of(event->authorize.conn_handle);

            event->authorize.out_response = BLE_GAP_AUTHORIZE_REJECT;
            if ( kdrv_ble_hid_access_ok(hid_attr, hid_bond) )
            {
                event->authorize.out_response = BLE_GAP_AUTHORIZE_ACCEPT;
            }
            else if ( s_author_said )
            {
                /* refused, and said */
            }
            else if ( hid_bond )
            {
                s_author_said = true;
                KLOG_N(TAG, "console refused: this device's bond was made for the mouse or "
                            "keyboard; for the console, forget the board on it and pair "
                            "again while no HID is served");
            }
            else
            {
                s_author_said = true;
                KLOG_N(TAG, "hid: refused: this device's bond was made for the console; "
                            "for the mouse or keyboard, forget the board on it and pair "
                            "again while HID is served");
            }
            break;
        }
        case BLE_GAP_EVENT_SUBSCRIBE:
            if ( event->subscribe.attr_handle == s_tx_val_handle )
            {
                s_ble.console_on = (0 != event->subscribe.cur_notify);
                KLOG_N(TAG, "console %s%s",
                       s_ble.console_on ? "attached" : "detached",
                       CONSOLE_OPEN_NOTE);
            }
#if KDRV_BLE_MIRROR_PIPE
            else if ( event->subscribe.attr_handle == s_mirror_val_handle )
            {
                s_ble.mirror_on = (0 != event->subscribe.cur_notify);
                KLOG_N(TAG, "mirror %s%s",
                       s_ble.mirror_on ? "attached" : "detached",
                       CONSOLE_OPEN_NOTE);
            }
#endif
            else
            {
                uint8_t const i   = app_index(event->subscribe.attr_handle);
                uint8_t const hid = hid_kind_at(event->subscribe.attr_handle);

                if ( app_count() > i )
                {
                    kdrv_ble_app_subscribed(i, 0U != event->subscribe.cur_notify);
                }
                else if ( 0U != hid )
                {
                    kdrv_ble_hid_subscribed(hid, 0U != event->subscribe.cur_notify);
                    KLOG_N(TAG, "hid: the host's subscription to the %s's reports is %s",
                           hid_kind_word(hid), hid_on_word(0U != event->subscribe.cur_notify));
                }
                else
                {
                    /* no characteristic of this file's */
                }
            }
            break;
#endif /* CONFIG_KODEOS_BLE_CONSOLE */
        case BLE_GAP_EVENT_ADV_COMPLETE:
            /* The advert's time ran out (kdrv_ble_adv_plan()), or a controller
             * ended it for its own reasons. Publishing IDLE is what asks
             * AO_Radio what comes next; the same path a disconnect takes.
             * The end of a pulse is not the end of the advert: the pause
             * follows, and the state stays ADVERTISING until the train is over. */
            if ( pulse_ended(event->adv_complete.reason) )
            {
                ble_gap_moved();
                ble_set_state(KDRV_BLE_IDLE);
            }
            break;
        default:
            break;
    }
    return status;
}

/* ---- Lifecycle ----------------------------------------------------------- */

kode_status_t kdrv_ble_start(void)
{
    if ( s_ble.up )
    {
        return KODE_OK;
    }

    /* Before anything can connect. Zero is a VALID connection handle, so a
     * statically zeroed field would have kdrv_ble_quiet() hang up on a peer
     * that was never there. */
    s_ble.conn = BLE_HS_CONN_HANDLE_NONE;

    if ( NULL == s_ble.bits )
    {
        s_ble.bits = xEventGroupCreate();
        if ( NULL == s_ble.bits )
        {
            return KODE_NO_RESOURCES;
        }
    }
    /* Here rather than at the first pulse: the first user of the lock may be
     * NimBLE's host task, which must not be the one that creates it. */
    if ( NULL == s_pulse_lock )
    {
        s_pulse_lock = khal_mutex_create();
        if ( NULL == s_pulse_lock )
        {
            KLOG_E(TAG, "no mutex for the advert's pulses");
            return KODE_NO_RESOURCES;
        }
    }
    (void)xEventGroupClearBits(s_ble.bits, BLE_BIT_SYNCED | BLE_BIT_SCAN_DONE);

    /* The radio end first: this is the C5's controller, reached by RPC. */
    if ( esp_hosted_bt_controller_init() != ESP_OK )
    {
        KLOG_E(TAG, "the C5 refused to bring its BT controller up");
        return KODE_ERROR;
    }
    if ( esp_hosted_bt_controller_enable() != ESP_OK )
    {
        KLOG_E(TAG, "the C5 refused to enable its BT controller");
        return KODE_ERROR;
    }

    /* Then the host end, whose HCI rides the same SDIO link through
     * esp-hosted's virtual transport. A log line reading "hci_stub_drv:
     * Host BT Support: Disabled" here means the NimBLE options did not make
     * it into the build - the stack is there, but nothing is carrying HCI. */
    esp_err_t const err = nimble_port_init();
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "nimble_port_init: %s", esp_err_to_name(err));
        return KODE_ERROR;
    }

    /* Everything below configures the host, and it all has to happen HERE:
     * ble_hs_init() queues its own start event on the default event queue,
     * which only nimble_port_run() services - so the window is between
     * nimble_port_init() and nimble_port_freertos_init(), and it closes when
     * the host task starts. It also all re-runs on every start cycle,
     * because deinit frees what was registered. */
    ble_hs_cfg.sync_cb  = &ble_on_sync;
    ble_hs_cfg.reset_cb = &ble_on_reset;

#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
    /* Pairing policy: LE Secure Connections, bonded, and authenticated when
     * there is a screen to authenticate with.
     *
     * DisplayOnly is the literal truth once the code goes on the glass. It
     * was NO_IO for as long as the only place the board could print a
     * passkey was the console - the very thing being protected, reachable
     * only over the cable this port exists to replace. A card on the AMOLED
     * removes that, so the IO capability can describe what the board
     * actually has. Against a central that declares KeyboardDisplay, which
     * is what macOS, Windows and Android all do, DisplayOnly selects Passkey
     * Entry with this side displaying.
     *
     * sm_mitm follows the same fact rather than a preference: with the
     * digits carried between the two ends by a human, MITM protection is
     * real and claiming it is honest. Without a screen it goes back to 0,
     * because a stack that claims what Just Works cannot deliver only fails
     * later, at the permission check.
     *
     * sm_sc is 1 either way: the ECDH exchange is what defeats a passive
     * listener and it costs nothing here. See CHR_WRITE_ENC above, which
     * spells the same trade-off in the permission bits. */
#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_DISP_ONLY;
    ble_hs_cfg.sm_mitm           = 1;
#else
    ble_hs_cfg.sm_io_cap         = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_mitm           = 0;
#endif
    ble_hs_cfg.sm_sc             = 1;
    ble_hs_cfg.sm_bonding        = 1;
    /* ENC only: the board advertises its public address, so a peer has
     * nothing to resolve with our IRK, and the local IRK stops being a
     * secret worth moving to the C5. */
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC
                                 | BLE_SM_PAIR_KEY_DIST_ID;

    /* What to do when the bond store is full: drop the oldest and take the
     * new one. Without this the store overflows into ble_store_status(),
     * which answers ENOTSUP when no callback is installed - so the pairing
     * fails and NOTHING says why. Three bonds is the ceiling
     * (CONFIG_BT_NIMBLE_MAX_BONDS), and asking every machine to re-pair is
     * exactly what turning the passkey on did, so the fourth one arrives a
     * great deal sooner than it used to. Round-robin is the vendor's own
     * answer; both esp-hosted NimBLE examples install it.
     *
     * An app is the exception: it never makes room, so a full store refuses
     * the request and the oldest bond, made in kodeOS, stays. */
    ble_hs_cfg.store_status_cb = &ble_store_status;
#endif /* CONFIG_KODEOS_BLE_CONSOLE_PAIRING */

    /* GAP (0x1800) and GATT (0x1801) were never registered here. Advertising
     * worked without them because the name lives in a heap buffer under
     * BLE_STATIC_TO_DYNAMIC, but a central reading Device Name over GATT
     * found no such characteristic. */
    ble_svc_gap_init();
    ble_svc_gatt_init();
    (void)ble_svc_gap_device_name_set(kdrv_ble_name());

#if CONFIG_KODEOS_BLE_CONSOLE
    console_build_table();
    const struct ble_gatt_svc_def *const defs = app_build_table();

    if ( ble_gatts_count_cfg(defs) != 0 )
    {
        KLOG_E(TAG, "the console service would not fit the GATT table");
        return KODE_ERROR;
    }
    if ( ble_gatts_add_svcs(defs) != 0 )
    {
        KLOG_E(TAG, "the console service was refused");
        return KODE_ERROR;
    }
    s_ble.conn       = BLE_HS_CONN_HANDLE_NONE;
    s_ble.console_on = false;
#if KDRV_BLE_MIRROR_PIPE
    s_ble.mirror_on  = false;
#endif
    s_ble.tx_drop    = 0U;

    /* Before the lock: a lock that exists means the ring has its bytes. */
    if ( NULL == s_ring_store )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_ring_store = (uint8_t *)khal_alloc_psram(sizeof(uint32_t),
                                                   KDRV_BLE_RING_BYTES);
        if ( NULL == s_ring_store )
        {
            KLOG_E(TAG, "no PSRAM for the console ring");
            return KODE_NO_RESOURCES;
        }
    }
    if ( NULL == s_ring_lock )
    {
        s_ring_lock = khal_mutex_create();
        if ( NULL == s_ring_lock )
        {
            KLOG_E(TAG, "no mutex for the console ring");
            return KODE_NO_RESOURCES;
        }
    }

#if CONFIG_KODEOS_BLE_PAIRING_PASSKEY
    /* Here rather than on first use: the first use is the GAP handler, on
     * NimBLE's host task, and creating a mutex lazily from the thread that is
     * racing for it is how a lock ends up not being one. This runs before
     * that task exists. */
    if ( NULL == s_pair_lock )
    {
        s_pair_lock = khal_mutex_create();
        if ( NULL == s_pair_lock )
        {
            KLOG_E(TAG, "no mutex for the pairing record");
            return KODE_NO_RESOURCES;
        }
    }
    s_pair.phase   = KDRV_BLE_PAIR_NONE;
    s_pair_live    = false;
    s_pair_refused = false;
#endif
    ring_init(&s_ring, s_ring_store, KDRV_BLE_RING_BYTES);
    s_pend_len = 0U;
    s_pend_off = 0U;
    /* Before the host task starts: the queue it drains is the one this
     * event goes on.
     *
     * Both objects are cleared first. Each holds a block from the porting
     * layer's pools, and nimble_port_deinit() frees those pools without
     * knowing who still points into them - while the two init calls below
     * keep whatever block they find already there. So on a second start in
     * one boot they would be handed back a block of a pool that no longer
     * exists, and the host would run on freed memory: a load fault inside
     * os_memblock_get() on some starts and not on others. */
    (void)memset(&s_tx_ev, 0, sizeof(s_tx_ev));
    (void)memset(&s_tx_co, 0, sizeof(s_tx_co));
    (void)memset(&s_hangup_ev, 0, sizeof(s_hangup_ev));
    ble_npl_event_init(&s_tx_ev, &console_drain, NULL);
    ble_npl_event_init(&s_hangup_ev, &console_hangup, NULL);
    (void)ble_npl_callout_init(&s_tx_co, nimble_port_get_dflt_eventq(),
                               &console_drain, NULL);

    /* The app's drain, set up the same way and for the same reason, when
     * its service is served; its subscriptions and ring start empty. */
    kdrv_ble_app_link_reset();
    app_host_t *const app = app_host();

    if ( (NULL != app) && (0U < app->count) )
    {
        (void)memset(&app->ev, 0, sizeof(app->ev));
        (void)memset(&app->co, 0, sizeof(app->co));
        ble_npl_event_init(&app->ev, &app_drain, NULL);
        (void)ble_npl_callout_init(&app->co, nimble_port_get_dflt_eventq(),
                                   &app_drain, NULL);
        app->live = true;
    }
    if ( (NULL != app) && (0U != app->batt_handle) )
    {
        (void)memset(&app->batt_ev, 0, sizeof(app->batt_ev));
        ble_npl_event_init(&app->batt_ev, &batt_notify, NULL);
        app->batt_live = true;
    }

    /* The HID service's drain, set up the same way, and what the board says
     * it is in the GAP service: the appearance its advert carries. */
    kdrv_ble_hid_link_reset();
    if ( (NULL != app) && kdrv_ble_hid_served() )
    {
        (void)memset(&app->hid.ev, 0, sizeof(app->hid.ev));
        (void)memset(&app->hid.co, 0, sizeof(app->hid.co));
        ble_npl_event_init(&app->hid.ev, &hid_drain, NULL);
        (void)ble_npl_callout_init(&app->hid.co, nimble_port_get_dflt_eventq(),
                                   &hid_drain, NULL);
        app->hid.live = true;
        (void)ble_svc_gap_device_appearance_set(kdrv_ble_hid_appearance(kdrv_ble_hid_kinds()));
        KLOG_N(TAG, "hid: the board is a %s to its hosts", hid_kind_word(kdrv_ble_hid_kinds()));
    }

    /* A value longer than a notification at the preferred MTU makes the host
     * ask for more, up to BLE_APP_MTU_MAX, so one notification carries one
     * whole value where the link allows. Only an app that asked for long
     * values (CONFIG_KODE_BLE_APP_VALUE_MAX) moves it; the central decides
     * the MTU in the end, and a value it cannot take in one notification is
     * dropped and counted (app_drain()). */
    uint16_t want_mtu = (uint16_t)(KDRV_BLE_APP_VALUE_MAX + BLE_ATT_NOTIFY_HDR);

    if ( (uint16_t)BLE_APP_MTU_MAX < want_mtu )
    {
        want_mtu = (uint16_t)BLE_APP_MTU_MAX;
    }

    if ( (ble_att_preferred_mtu() < want_mtu) && (ble_att_set_preferred_mtu(want_mtu) != 0) )
    {
        KLOG_W(TAG, "the host refused a preferred MTU of %u", (unsigned)want_mtu);
    }

#if KDRV_BLE_MIRROR_PIPE
    /* The mirror ring, set up the same way and for the same reason - see the
     * struct comment on why it is not the console's ring reused - its bytes
     * taken before its lock, as the console's are. */
    if ( NULL == s_mirror_ring_store )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_mirror_ring_store = (uint8_t *)khal_alloc_psram(
            sizeof(uint32_t), KDRV_BLE_MIRROR_RING_BYTES);
        if ( NULL == s_mirror_ring_store )
        {
            KLOG_E(TAG, "no PSRAM for the mirror ring");
            return KODE_NO_RESOURCES;
        }
    }
    if ( NULL == s_mirror_ring_lock )
    {
        s_mirror_ring_lock = khal_mutex_create();
        if ( NULL == s_mirror_ring_lock )
        {
            KLOG_E(TAG, "no mutex for the mirror ring");
            return KODE_NO_RESOURCES;
        }
    }
    ring_init(&s_mirror_ring, s_mirror_ring_store, KDRV_BLE_MIRROR_RING_BYTES);
    s_mirror_pend_len = 0U;
    s_mirror_pend_off = 0U;
    /* Cleared for the console pair's reason, just above. */
    (void)memset(&s_mirror_tx_ev, 0, sizeof(s_mirror_tx_ev));
    (void)memset(&s_mirror_tx_co, 0, sizeof(s_mirror_tx_co));
    ble_npl_event_init(&s_mirror_tx_ev, &mirror_drain, NULL);
    (void)ble_npl_callout_init(&s_mirror_tx_co, nimble_port_get_dflt_eventq(),
                               &mirror_drain, NULL);
#endif

    /* The bond store, NVS-backed. Guarded against re-allocation internally,
     * so calling it once per start cycle is safe. */
    ble_store_config_init();
#endif

    /* The pulse's callout, cleared for the console pair's reason: the block
     * it holds comes from a pool that nimble_port_deinit() frees. */
    s_pulse.live   = false;
    s_pulse.on_air = false;
    (void)memset(&s_pulse_co, 0, sizeof(s_pulse_co));
    (void)ble_npl_callout_init(&s_pulse_co, nimble_port_get_dflt_eventq(),
                               &pulse_timer, NULL);

    nimble_port_freertos_init(&ble_host_task);
    s_ble.up = true;

    /* Nothing may touch GAP until the controller has answered. Waiting here
     * is what lets every call above this one assume a usable stack. */
    EventBits_t const bits = xEventGroupWaitBits(s_ble.bits, BLE_BIT_SYNCED,
                                                 pdFALSE, pdFALSE,
                                                 pdMS_TO_TICKS(BLE_SYNC_MS));
    if ( (bits & BLE_BIT_SYNCED) == 0U )
    {
        KLOG_E(TAG, "the controller never synchronised");
        return KODE_TIMEOUT;
    }

    ble_set_state(KDRV_BLE_IDLE);
    KLOG_N(TAG, "BLE up (NimBLE here, the radio on the C5), as '%s'",
           kdrv_ble_name());
    return KODE_OK;
}

kode_status_t kdrv_ble_quiet(void)
{
    if ( !s_ble.up )
    {
        return KODE_OK;
    }

    ble_adv_halt();
    /* Cleared first: the cancel may complete the scan on this very call, and
     * a passive scan that is ended on purpose says nothing to its hook. */
    s_ble.hearing = false;
    (void)ble_gap_disc_cancel();

    if ( BLE_HS_CONN_HANDLE_NONE != s_ble.conn )
    {
        /* Hang up rather than leave a session alive on a service the owner
         * has just switched off. The DISCONNECT event does the bookkeeping,
         * on the host task, exactly as it does when the peer leaves first. */
#if CONFIG_KODEOS_BLE_CONSOLE
        /* On the host task, behind the console's drain: what is in the ring
         * reaches the stack first (console_hangup()). The bound only covers a
         * host task that does not run. */
        (void)xEventGroupClearBits(s_ble.bits, BLE_BIT_HUNG_UP);
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_hangup_ev);
        EventBits_t const bits = xEventGroupWaitBits(s_ble.bits, BLE_BIT_HUNG_UP,
                                                     pdFALSE, pdFALSE,
                                                     pdMS_TO_TICKS(BLE_HANGUP_MS));
        if ( (bits & BLE_BIT_HUNG_UP) == 0U )
        {
            KLOG_W(TAG, "the host task did not hang up in %u ms; hanging up here",
                   (unsigned)BLE_HANGUP_MS);
            (void)ble_gap_terminate(s_ble.conn, BLE_ERR_REM_USER_CONN_TERM);
        }
#else
        (void)ble_gap_terminate(s_ble.conn, BLE_ERR_REM_USER_CONN_TERM);
#endif
    }

    /* IDLE, not OFF: the stack IS up, and this function's whole reason for
     * existing is that it stays up. What the user switched off is the
     * service, and AO_Radio's flag is where that lives - see
     * ble_publish_state(), which reports OFF from the flag and never from
     * here. */
    ble_set_state(KDRV_BLE_IDLE);
    KLOG_N(TAG, "BLE quiet (the host stays up; see kdrv_ble_stop)");
    return KODE_OK;
}

kode_status_t kdrv_ble_stop(void)
{
    if ( !s_ble.up )
    {
        return KODE_OK;
    }

    ble_adv_halt();
    s_ble.hearing = false;   /* first, as in kdrv_ble_quiet() */
    (void)ble_gap_disc_cancel();
#if CONFIG_KODEOS_BLE_CONSOLE
    app_quiet();
#endif

    /* nimble_port_stop() ends the host task's run loop; deinit then releases
     * what it held. Order matters and there is no shortcut: deinit under a
     * running host is the kind of teardown that takes the system with it. */
    if ( nimble_port_stop() == 0 )
    {
        (void)nimble_port_deinit();
    }
    else
    {
        KLOG_W(TAG, "the NimBLE host would not stop");
    }

    (void)esp_hosted_bt_controller_disable();
    (void)esp_hosted_bt_controller_deinit(false);
#if CONFIG_KODEOS_BLE_CONSOLE
    kdrv_ble_app_link_reset();
    kdrv_ble_hid_link_reset();
#endif

    s_ble.up     = false;
    s_ble.synced = false;
    s_ble.count  = 0U;
    ble_set_state(KDRV_BLE_OFF);
    KLOG_N(TAG, "BLE down");
    return KODE_OK;
}

void kdrv_ble_forget(void)
{
    pulse_train_end();
    s_ble.up     = false;
    s_ble.synced = false;
    s_ble.hearing = false;   /* the link is gone with its scan; no hook, as above */
    s_ble.count  = 0U;
    s_ble.state  = KDRV_BLE_OFF;
    s_ble.conn   = BLE_HS_CONN_HANDLE_NONE;
#if CONFIG_KODEOS_BLE_CONSOLE
    s_ble.console_on = false;
#if KDRV_BLE_MIRROR_PIPE
    s_ble.mirror_on  = false;
#endif
    s_ble.tx_drop    = 0U;
    app_quiet();
    kdrv_ble_app_link_reset();
    kdrv_ble_hid_link_reset();
#endif
}

uint8_t kdrv_ble_bonds(void)
{
    /* The store is NimBLE's, and asking it anything before nimble_port_init()
     * has run reaches structures that do not exist yet. AO_Radio publishes
     * the BLE state with the link down as a matter of course - that is how
     * every ble_* verb is answered when the C5 is off - so this has to be
     * safe to call at any time, not merely when the stack happens to be up.
     * It reads the count the last change of the store left and takes no
     * lock: a store write holds NimBLE's for a round trip to the C5. */
    uint8_t n = 0U;

    if ( s_ble.up )
    {
        n = kdrv_ble_bonds_kept();
    }
    return n;
}

kode_status_t kdrv_ble_unpair(void)
{
    /* The bonds live in NVS, but the store that reaches them is NimBLE's, so
     * the stack has to be up to clear them - the same reason kdrv_ble_bonds()
     * answers 0 rather than asking when it is down. Starting it here rather
     * than refusing is what advertise and scan already do: the verb should
     * work from any state, and the start is idempotent. */
    kode_status_t const rc = kdrv_ble_start();
    if ( KODE_OK != rc )
    {
        return rc;
    }
    /* ble_store_clear() forgets every bond without disconnecting anybody,
     * which is why the caller refuses this while a central is attached: the
     * phone would keep a link whose key no longer exists. */
#ifdef CONFIG_KODEOS_BLE_CONSOLE
    /* No bond, no host: nobody is connected, so the host task reads none. */
    (void)memset(&s_hid_hosts, 0, sizeof(s_hid_hosts));
    kdrv_ble_hid_hosts_store(&s_hid_hosts);
#endif
    return (ble_store_clear() == 0) ? KODE_OK : KODE_ERROR;
}

/**
 * @brief Pack @p name and @p data into the scan response this driver keeps.
 * @param name The name; NULL for the serial.
 * @param data The manufacturer data.
 * @param len  Its bytes.
 * @return What kdrv_ble_adv_pack() answers; the kept one changes only on KODE_OK.
 */
static kode_status_t ble_rsp_pack(const char *name, const uint8_t *data, size_t len)
{
    uint8_t       buf[KDRV_BLE_ADV_MAX];
    size_t        n    = 0U;
    const char   *who  = name;
    kode_status_t rc;

    if ( NULL == who )
    {
        who = kdrv_ble_serial_name();
    }
    rc = kdrv_ble_adv_pack(who, data, len, buf, sizeof(buf), &n);
    if ( KODE_OK == rc )
    {
        (void)memcpy(s_ble.rsp, buf, n);
        s_ble.rsp_len = (uint8_t)n;
    }
    return rc;
}

kode_status_t kdrv_ble_set_scan_rsp(const char *name, const uint8_t *data, size_t len)
{
    kode_status_t const rc = ble_rsp_pack(name, data, len);

    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "scan response refused: the name and %u byte(s) of data do not "
                    "fit its %u", (unsigned)len, (unsigned)KDRV_BLE_ADV_MAX);
        return rc;
    }
    s_ble.rsp_set = true;
    s_ble.name[0] = '\0';
    if ( NULL != name )
    {
        (void)snprintf(s_ble.name, sizeof(s_ble.name), "%s", name);
    }
    if ( s_ble.up )
    {
        /* What a central reads once it has connected, and lists the board
         * under from then on: the same name the scan response shows. */
        (void)ble_svc_gap_device_name_set(kdrv_ble_name());
    }
    if ( s_ble.synced && !s_ble.adv_hidden )
    {
        /* Legal while advertising: the next scan request is answered with it.
         * A hidden board keeps it for its next visible advert. */
        int const rc_n = ble_gap_adv_rsp_set_data(s_ble.rsp, (int)s_ble.rsp_len);
        if ( 0 != rc_n )
        {
            KLOG_E(TAG, "scan response refused by the host (%d)", rc_n);
            return KODE_ERROR;
        }
    }
    KLOG_N(TAG, "scan response: %u of %u bytes", (unsigned)s_ble.rsp_len,
           (unsigned)KDRV_BLE_ADV_MAX);
    return KODE_OK;
}

const char *kdrv_ble_name(void)
{
    const char *name = s_ble.name;

    if ( '\0' == s_ble.name[0] )
    {
        name = kdrv_ble_serial_name();
    }
    return name;
}

bool kdrv_ble_link_secure(void)
{
    return s_ble.secure;
}

void kdrv_ble_adv_plan(uint16_t itvl, uint32_t ms, bool hidden, bool pulsed)
{
    s_ble.adv_itvl   = itvl;
    s_ble.adv_ms     = ms;
    s_ble.adv_hidden = hidden;
    s_ble.adv_pulsed = pulsed;
}

kode_status_t kdrv_ble_advertise(bool on)
{
    if ( !on )
    {
        if ( !s_ble.up )
        {
            return KODE_OK;
        }
        ble_adv_halt();
        ble_set_state(KDRV_BLE_IDLE);
        return KODE_OK;
    }

    kode_status_t const rc = kdrv_ble_start();
    if ( KODE_OK != rc )
    {
        return rc;
    }
    if ( KDRV_BLE_CONNECTED == s_ble.state )
    {
        /* A connection already occupies the one link this slice supports;
         * advertising again would only invite a second. */
        KLOG_W(TAG, "already connected; not advertising");
        return KODE_DENIED;
    }

    /* The 31 bytes, counted: each AD structure costs 2 bytes of header plus
     * its payload, so flags 3 + a 128-bit UUID 18 + TX power 3 = 24, and the
     * name would need 10 more. NimBLE emits flags, UUIDs, name, TX power in
     * that order, so keeping the name here is what pushes TX POWER off the
     * end - the refusal names the wrong field if you go looking. Dropping TX
     * power instead would also fit, at exactly 31 with no headroom. So the
     * name moves to the scan response, which has a whole packet to itself:
     * 29 characters against the 15 a serial needs. Without the console
     * service the name rides the advert instead, and that still fits:
     * flags 3 + name 17 + TX power 3 = 23 of 31. */
    /* Hidden, the advert still reaches the devices that know this board -
     * they connect to its address - but it carries no discoverable flag, so
     * a phone lists it with nothing new to pair, and no name: no scan
     * response, and none in the advert. The console's UUID stays, which is
     * what a bonded client filters on. */
    bool const hidden = s_ble.adv_hidden;
    struct ble_hs_adv_fields fields;
    kdrv_ble_adv_ask_t       ask;
    kdrv_ble_adv_shape_t     shape;

    /* Which fields go is kdrv_ble_adv_shape()'s: a board that serves HID adds
     * 0x1812 and its appearance and gives up the transmit power for them, so
     * the 128-bit UUID stays for the clients that filter on it. */
    (void)memset(&ask, 0, sizeof(ask));
    ask.hidden = hidden;
#ifdef CONFIG_KODEOS_BLE_CONSOLE
    ask.console = true;
    if ( kdrv_ble_hid_served() )
    {
        ask.hid = kdrv_ble_hid_kinds();
    }
#ifdef CONFIG_KODE_BLE_ADVERTISE_APP_SERVICE
    app_host_t *const app_adv = app_host();

    ask.app_uuid = (NULL != app_adv) && (0U < app_adv->count);
#endif
#endif
    kdrv_ble_adv_shape(&ask, &shape);

    (void)memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_BREDR_UNSUP;
    if ( shape.discoverable )
    {
        fields.flags |= BLE_HS_ADV_F_DISC_GEN;
    }
    if ( shape.tx_power )
    {
        fields.tx_pwr_lvl_is_present = 1;
        fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    }
#if CONFIG_KODEOS_BLE_CONSOLE
    /* What clients filter on: requestDevice({filters:[{services}]}) in Web
     * Bluetooth, startDeviceScan([uuid]) in ble-plx. Without it a phone app
     * has to inspect every advertiser in the room. */
    if ( (uint8_t)KDRV_BLE_ADV_UUID128_NONE != shape.uuid128 )
    {
        fields.uuids128 = &s_svc_uuid;
#ifdef CONFIG_KODE_BLE_ADVERTISE_APP_SERVICE
        /* The app's UUID in its place, for the phones that filter on the
         * app's protocol; only one 128-bit UUID fits. */
        if ( (uint8_t)KDRV_BLE_ADV_UUID128_APP == shape.uuid128 )
        {
            fields.uuids128 = &app_adv->uuid[0];
        }
#endif
        fields.num_uuids128 = 1U;
        fields.uuids128_is_complete = 1;
    }
    if ( 0U != shape.uuid16 )
    {
        fields.uuids16             = &s_hid_svc_uuid;
        fields.num_uuids16         = 1U;
        fields.uuids16_is_complete = 1;
    }
    if ( 0U != shape.appearance )
    {
        fields.appearance            = shape.appearance;
        fields.appearance_is_present = 1;
    }
#endif
    if ( shape.name )
    {
        fields.name = (uint8_t *)ble_svc_gap_device_name();
        fields.name_len = (uint8_t)strlen((const char *)fields.name);
        fields.name_is_complete = 1;
    }

    int rc_n = ble_gap_adv_set_fields(&fields);
    if ( 0 != rc_n )
    {
        KLOG_E(TAG, "advert fields refused (%d) - too long for one packet?",
               rc_n);
        return KODE_ERROR;
    }

#if CONFIG_KODEOS_BLE_CONSOLE
    if ( !s_ble.rsp_set )
    {
        /* The serial alone until an app names the board: the name the advert
         * has no room for. */
        (void)ble_rsp_pack(NULL, NULL, 0U);
    }
#endif
    /* No flags field here: a scan response carrying one is malformed, and
     * the packing writes only the name and the data. Hidden, the controller
     * is told to answer with an empty one: it keeps the last it was given. */
    if ( hidden )
    {
        rc_n = ble_gap_adv_rsp_set_data(NULL, 0);
        if ( 0 != rc_n )
        {
            KLOG_E(TAG, "scan response refused (%d)", rc_n);
            return KODE_ERROR;
        }
    }
    else if ( 0U < s_ble.rsp_len )
    {
        rc_n = ble_gap_adv_rsp_set_data(s_ble.rsp, (int)s_ble.rsp_len);
        if ( 0 != rc_n )
        {
            KLOG_E(TAG, "scan response refused (%d)", rc_n);
            return KODE_ERROR;
        }
    }

    struct ble_gap_adv_params params;
    (void)memset(&params, 0, sizeof(params));
    params.conn_mode = BLE_GAP_CONN_MODE_UND;   /* connectable */
    params.disc_mode = BLE_GAP_DISC_MODE_GEN;   /* discoverable */
    if ( hidden )
    {
        params.disc_mode = BLE_GAP_DISC_MODE_NON;
    }
    /* The interval and the time on the air are AO_Radio's (kdrv_ble_adv_plan());
     * an advert nobody planned is the slow one, with no end. NimBLE's own
     * default of ~30 ms would have the radio waking for it all the time. */
    uint16_t const itvl = (0U != s_ble.adv_itvl) ? s_ble.adv_itvl
                                                 : (uint16_t)KDRV_BLE_ITVL_SLOW;

    params.itvl_min = itvl;
    params.itvl_max = itvl;

    bool const pulsed = s_ble.adv_pulsed;

    if ( pulsed )
    {
        /* A train: the first pulse here, the rest from the pause's callout. */
        rc_n = pulse_begin(&params);
    }
    else
    {
        int32_t duration = BLE_HS_FOREVER;

        if ( 0U != s_ble.adv_ms )
        {
            duration = (int32_t)s_ble.adv_ms;
        }
        /* The time is NimBLE's host timer, an esp_timer: legacy advertising has
         * no duration in the controller. When it runs out NimBLE stops the
         * advert and ADV_COMPLETE publishes IDLE, which is what asks AO_Radio
         * what comes next. A connection ends the advert before that, and the
         * timer with it. A train still on gives way to a continuous plan. */
        pulse_train_end();
        rc_n = ble_gap_adv_start(s_ble.own_addr_type, NULL, duration,
                                 &params, &ble_gap_event, NULL);
    }
    /* EALREADY is the answer this call wants: the advert is on the air. A
     * state published late by the host task (a disconnect that waited on
     * the C5 to forget a bond's keys) can ask again for an advert that is
     * already running; called a failure, it was asked again, forever. */
    if ( (0 != rc_n) && (BLE_HS_EALREADY != rc_n) )
    {
        KLOG_E(TAG, "ble_gap_adv_start: %d", rc_n);
        return KODE_ERROR;
    }

    ble_set_state(KDRV_BLE_ADVERTISING);
    if ( pulsed && (0 == rc_n) )
    {
        KLOG_D(TAG, "advert pulse");
    }
    /* A timed advert is started again every time it runs out: only a change
     * of kind is news. */
    if ( s_ble.adv_said && (s_ble.adv_said_hidden == hidden)
        && (s_ble.adv_said_pulsed == pulsed) )
    {
        return KODE_OK;
    }
    s_ble.adv_said        = true;
    s_ble.adv_said_hidden = hidden;
    s_ble.adv_said_pulsed = pulsed;
    if ( pulsed )
    {
        uint32_t const every = kdrv_ble_pulse_period_ms(itvl);

        if ( hidden )
        {
            KLOG_I(TAG, "advertising hidden, to the devices that know it, one event every %u ms",
                   (unsigned)every);
        }
        else
        {
            KLOG_I(TAG, "advertising as '%s', one event every %u ms", kdrv_ble_name(),
                   (unsigned)every);
        }
    }
    else if ( hidden )
    {
        KLOG_I(TAG, "advertising hidden, to the devices that know it");
    }
    else
    {
        KLOG_I(TAG, "advertising as '%s'", kdrv_ble_name());
    }
    return KODE_OK;
}

kode_status_t kdrv_ble_adv_calm(uint32_t ms)
{
    kode_status_t rc = KODE_OK;

    if ( s_ble.up && (KDRV_BLE_ADVERTISING == s_ble.state)
        && (s_ble.adv_pulsed
            || ((0U != s_ble.adv_itvl) && ((uint16_t)KDRV_BLE_ITVL_SLOW > s_ble.adv_itvl))) )
    {
        ble_adv_halt();
        kdrv_ble_adv_plan((uint16_t)KDRV_BLE_ITVL_SLOW, ms, s_ble.adv_hidden, false);
        rc = kdrv_ble_advertise(true);
    }
    return rc;
}

/* The rest set is asked for on the way into the board's rest, while the bus
 * still runs, and the awake set once the rest is over. A central that is
 * asked while a previous update is still pending answers EALREADY; the set
 * that is pending then stands, which is not worth an error. */
kode_status_t kdrv_ble_conn_calm(bool rest)
{
    uint16_t const conn = s_ble.conn;
    kode_status_t rc = KODE_OK;

    if ( s_ble.up && (BLE_HS_CONN_HANDLE_NONE != conn) )
    {
        int const rc_n = ble_conn_ask(conn, rest);

        if ( (0 != rc_n) && (BLE_HS_EALREADY != rc_n) )
        {
            KLOG_D(TAG, "connection update refused (%d)", rc_n);
            rc = KODE_ERROR;
        }
    }
    return rc;
}

/**
 * @brief Give the host's one scan to the scan that is about to start.
 *
 * The owner ended the stream before it sent the job, so the hook is told
 * nothing: the flag is cleared before the cancel, as kdrv_ble_hear(false)
 * does. A scan, a find and a new start of the stream call it first.
 */
static void ble_hear_yield(void)
{
    if ( s_ble.hearing )
    {
        s_ble.hearing = false;
        (void)ble_gap_disc_cancel();
    }
}

kode_status_t kdrv_ble_scan(uint32_t ms)
{
    ble_hear_yield();
    kode_status_t const rc = kdrv_ble_start();
    if ( KODE_OK != rc )
    {
        return rc;
    }

    if ( NULL == s_ble.dev )
    {
        /* About 2 KB that internal RAM has no reason to hold: NimBLE's host
         * task writes it and the owner's thread reads it, never an
         * interrupt. Kept for the life of the boot. */
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_ble.dev = (kdrv_ble_dev_t *)khal_alloc_psram(
            BLE_DEV_ALIGN,
            (size_t)KDRV_BLE_SCAN_MAX * sizeof(kdrv_ble_dev_t));
        if ( NULL == s_ble.dev )
        {
            KLOG_E(TAG, "scan: PSRAM refused the device table");
            return KODE_NO_RESOURCES;
        }
    }
    s_ble.count = 0U;
    (void)xEventGroupClearBits(s_ble.bits, BLE_BIT_SCAN_DONE);
    /* The stop's own bit is not cleared here: one asked from now on is
     * still set when the wait below begins, and ends it. */
    if ( s_scan_abort )
    {
        KLOG_N(TAG, "scan: ended before it listened, as its owner asked");
        return KODE_DENIED;
    }

    struct ble_gap_disc_params params;
    (void)memset(&params, 0, sizeof(params));
    /* Active, not passive. Most devices - phones especially - put their name
     * in the scan response rather than the advert, and a passive scan never
     * asks for one: the first run of this code found ten devices and could
     * not name a single one, which is a listing nobody can use. The cost is
     * that the board transmits while scanning. */
    params.passive = 0;
    /* Duplicate filtering off on purpose. The controller would report each
     * device once, and the advert it happens to pick may be the one without
     * the name; de-duplicating here instead lets every packet contribute,
     * and keeps the strongest reading rather than the first. */
    params.filter_duplicates = 0;

    int const rc_n = ble_gap_disc(s_ble.own_addr_type, (int32_t)ms, &params,
                                  &ble_gap_event, NULL);
    if ( 0 != rc_n )
    {
        KLOG_E(TAG, "ble_gap_disc: %d", rc_n);
        return KODE_ERROR;
    }

    /* The duration plus a margin: the completion arrives on the host task,
     * and waiting for it rather than for the clock is what makes the job's
     * answer mean "the scan is over". */
    EventBits_t const bits = xEventGroupWaitBits(s_ble.bits,
                                                 BLE_BIT_SCAN_DONE | BLE_BIT_SCAN_ABORT,
                                                 pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(ms + BLE_SCAN_MARGIN_MS));
    if ( (bits & BLE_BIT_SCAN_ABORT) != 0U )
    {
        /* Judged by the bit the wait woke on, not by the flag: the owner may
         * take the request back before this line runs, and the discovery
         * would then go on with nobody waiting for it. A cancelled discovery
         * reports no completion, and the next listen clears the completion's
         * bit before it starts: nothing of this one reaches it. */
        (void)ble_gap_disc_cancel();
        KLOG_N(TAG, "scan: ended early with %u device(s), as its owner asked",
               (unsigned)s_ble.count);
        return KODE_DENIED;
    }
    if ( (bits & BLE_BIT_SCAN_DONE) == 0U )
    {
        (void)ble_gap_disc_cancel();
        KLOG_W(TAG, "the scan did not report its own end");
    }

    KLOG_N(TAG, "scan: %u device(s)", (unsigned)s_ble.count);
    return KODE_OK;
}

void kdrv_ble_scan_abort(bool on)
{
    s_scan_abort = on;
    if ( NULL == s_ble.bits )
    {
        /* No stack yet, so no listen to knock on: the flag is enough. */
    }
    else if ( on )
    {
        /* A bit of its own: the completion's would end a wait it was never
         * meant for. */
        (void)xEventGroupSetBits(s_ble.bits, BLE_BIT_SCAN_ABORT);
    }
    else
    {
        /* Taken back: a knock nobody read must not end the next listen. */
        (void)xEventGroupClearBits(s_ble.bits, BLE_BIT_SCAN_ABORT);
    }
}

kode_status_t kdrv_ble_scan_find(const char *name, uint32_t max_ms,
                                 kdrv_ble_dev_t *dev, bool *found)
{
    if ( (NULL == dev) || (NULL == found) )
    {
        return KODE_DENIED;
    }
    ble_hear_yield();
    *found = false;
    if ( !kdrv_ble_find_begin(&s_find, name) )
    {
        return KODE_DENIED;
    }
    kode_status_t const rc = kdrv_ble_start();
    if ( KODE_OK != rc )
    {
        kdrv_ble_find_end(&s_find);
        return rc;
    }
    (void)xEventGroupClearBits(s_ble.bits, BLE_BIT_SCAN_DONE);

    struct ble_gap_disc_params params;
    (void)memset(&params, 0, sizeof(params));
    params.passive           = 0;   /* the name may be in the scan response */
    params.filter_duplicates = 0;
    int const rc_n = ble_gap_disc(s_ble.own_addr_type, (int32_t)max_ms, &params,
                                  &ble_gap_event, NULL);
    if ( 0 != rc_n )
    {
        kdrv_ble_find_end(&s_find);
        KLOG_E(TAG, "ble_gap_disc (find): %d", rc_n);
        return KODE_ERROR;
    }

    /* Woken by the hit, or by the scan's own end. */
    EventBits_t const bits = xEventGroupWaitBits(s_ble.bits, BLE_BIT_SCAN_DONE,
                                                 pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(max_ms + BLE_SCAN_MARGIN_MS));
    kdrv_ble_find_end(&s_find);
    if ( s_find.hit || ((bits & BLE_BIT_SCAN_DONE) == 0U) )
    {
        (void)ble_gap_disc_cancel();     /* heard it, or it never ended */
    }
    if ( s_find.hit )
    {
        *dev   = s_find.dev;
        *found = true;
    }
    return KODE_OK;
}

/** @brief Scan interval of kdrv_ble_hear(), 0.625 ms units: 10 ms. */
#define BLE_HEAR_ITVL         16U

/** @brief A window of the whole interval: the radio listens all the time. */
#define BLE_HEAR_WINDOW_FULL  BLE_HEAR_ITVL

/** @brief A window of half the interval: the other half is left to Wi-Fi and to a BLE link. */
/* cppcheck-suppress misra-c2012-2.5 ; the other value BLE_HEAR_WINDOW takes, kept by name */
#define BLE_HEAR_WINDOW_HALF  (BLE_HEAR_ITVL / 2U)

/** @brief Scan window of kdrv_ble_hear(): BLE_HEAR_WINDOW_FULL or BLE_HEAR_WINDOW_HALF. */
#define BLE_HEAR_WINDOW       BLE_HEAR_WINDOW_FULL

kode_status_t kdrv_ble_hear(bool on)
{
    if ( !on )
    {
        if ( s_ble.hearing )
        {
            /* Cleared before the cancel, so the completion it provokes is
             * not taken for the stack ending the passive scan by itself. */
            s_ble.hearing = false;
            (void)ble_gap_disc_cancel();
        }
        return KODE_OK;
    }

    /* A passive scan still running is one the owner already let go of (a
     * listing ended its stream while the worker was busy, or a start that
     * timed out): the new start takes its place, so a start asked twice
     * runs once. */
    ble_hear_yield();

    /* Never brings the stack up: this scan is the owner's, asked once the
     * radio is already in the state it wants. A scan or a find that runs
     * holds the one scan the controller has. */
    if ( !s_ble.up || !s_ble.synced || (0 != ble_gap_disc_active()) )
    {
        return KODE_DENIED;
    }

    struct ble_gap_disc_params params;
    (void)memset(&params, 0, sizeof(params));
    /* Passive, the opposite of kdrv_ble_scan() and kdrv_ble_scan_find(). Those
     * are active because a name hides in the scan response and the listing is
     * useless without it. This scan only receives: a scan request is a
     * packet the board would send, and a stream of what is on the air has no
     * business transmitting. */
    params.passive           = 1;
    params.filter_duplicates = 0;   /* every packet is a record: the app counts them */
    params.itvl              = BLE_HEAR_ITVL;
    params.window            = BLE_HEAR_WINDOW;

    /* Set before the scan starts: the first report can reach the host task
     * before ble_gap_disc() returns. */
    s_ble.hearing = true;
    int const rc_n = ble_gap_disc(s_ble.own_addr_type, BLE_HS_FOREVER, &params,
                                  &ble_gap_event, NULL);
    if ( 0 != rc_n )
    {
        s_ble.hearing = false;
        KLOG_E(TAG, "ble_gap_disc (hear): %d", rc_n);
        return KODE_ERROR;
    }
    return KODE_OK;
}
