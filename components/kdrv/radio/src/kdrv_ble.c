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
#include "kdrv_ble_app.h"     /* the app's own service: its rules, values and ring */
#include "kdrv_ble_find.h"    /* kdrv_ble_scan_find(): which advert is the one */
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

#define TAG "kdrv.ble"

/** How long to wait for the host and the controller to agree on reality. */
#define BLE_SYNC_MS         5000U

#define BLE_BIT_SYNCED      (1U << 0)
#define BLE_BIT_SCAN_DONE   (1U << 1)

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
#elif CONFIG_KODEOS_BLE_CONSOLE_PAIRING
/* No screen to show a code on, so the pairing is Just Works: encrypted,
 * bonded, and nobody types anything. That cannot produce an authenticated
 * link, so asking for _AUTHEN here would fail every access with
 * "insufficient authentication". What survives is confidentiality against a
 * listener; what does not is proof of who is on the other end. */
#define CHR_WRITE_ENC   BLE_GATT_CHR_F_WRITE_ENC
#define CHR_NOTIFY_ENC  BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC
#define CHR_READ_ENC    BLE_GATT_CHR_F_READ_ENC
#else
/* Pairing is off. These two masks are the whole of what an unpaired peer is
 * refused, so emptying them opens the service - which is the point, and the
 * paragraph above is what has to be honoured when it comes back. */
#define CHR_WRITE_ENC   0U
#define CHR_NOTIFY_ENC  0U
#define CHR_READ_ENC    0U
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

/** @brief The most client configuration descriptors one bonded peer can
 *         subscribe: GATT's, the console's, the mirror's pipe and every
 *         characteristic of the app's, each of which may notify. */
#define BLE_PEER_CCCDS_MAX     (BLE_GATT_SVC_CCCDS + BLE_CONSOLE_TX_CCCDS \
                                + (unsigned)KDRV_BLE_MIRROR_PIPE + KDRV_BLE_APP_CHARS_MAX)

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
    uint16_t           adv_itvl;    /**< The next advert's interval (0.625 ms units); 0 until planned. */
    uint32_t           adv_ms;      /**< How long it stays on the air, ms; 0 for no end. */
    bool               adv_hidden;  /**< The next advert hides the board and no
                                      *   new device may pair; false (the
                                      *   start) is visible.                */
    bool               adv_said;    /**< The log has told an advert.        */
    bool               adv_said_hidden; /**< Which one it told.             */
#if CONFIG_KODEOS_BLE_CONSOLE
    bool               console_on;  /**< A central is subscribed to TX.     */
    uint16_t           tx_drop;     /**< Lines the console ring refused, PLUS
                                      *   notify failures on either pipe -
                                      *   console and mirror share this
                                      *   counter rather than each keeping
                                      *   one, because both report the same
                                      *   fact: the link, not either producer,
                                      *   is what failed.                    */
    uint16_t           tx_retry;    /**< Times either drain found no
                                      *   buffers - shared for the same
                                      *   reason as tx_drop above.           */
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

const kdrv_ble_app_service_t *kdrv_ble_app_service(void)
{
    return &kode_ble_app_service;
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

#if !CONFIG_KODEOS_BOOT_ROLE_APP   /* an app shows no code */
/** @brief Pick the six digits the peer will be asked to type. */
static uint32_t ble_passkey_pick(void)
{
    /* Reducing the hardware RNG's 32-bit uniform into a range of a million
     * favours the low values by about one part in ten thousand. That is far
     * below anything that matters for a number which lives for the security
     * manager's thirty seconds and is never reused. */
    return (uint32_t)(esp_random() % 1000000U);
}
#endif

/**
 * @brief Record where a pairing is and nudge whoever asked to be told.
 *
 * @param phase   SHOW or DONE.
 * @param outcome How it ended; ignored in SHOW.
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
    /* Clear the flag BEFORE hanging up. The disconnect this provokes runs on
     * the host task, and with the flag still set it would report GONE - the
     * peer walking away - for something this board did on purpose. */
    s_pair_live = false;
    KLOG_N(TAG, "pairing refused here");
    pair_note(KDRV_BLE_PAIR_DONE, KDRV_BLE_PAIR_REJECTED, 0U);

    if ( BLE_HS_CONN_HANDLE_NONE != s_ble.conn )
    {
        (void)ble_gap_terminate(s_ble.conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    return KODE_OK;
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

/** @brief Services in a table with the app's: the console's, the app's and the terminator. */
#define BLE_APP_TABLE_SVCS  3U

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
} app_host_t;

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
 * @param conn_handle Unused.
 * @param attr_handle The value's handle.
 * @param ctxt        NimBLE's access context.
 * @param arg         Unused.
 * @return 0, or the ATT error the central gets.
 */
static int app_access(uint16_t conn_handle, uint16_t attr_handle,
                      struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
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
        uint16_t const n = kdrv_ble_app_read(i, buf, (uint16_t)KDRV_BLE_APP_VALUE_MAX);

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
 * @brief The table to register: the console's alone, or the console's and the
 *        app's after it when the app's is served.
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
    bool const    served = kdrv_ble_app_build(kdrv_ble_app_service(), &perm, &t);
    app_host_t *const a  = app_host();

    if ( NULL != a )
    {
        a->count = 0U;
        a->live  = false;
        (void)memset(a->val_handle, 0, sizeof(a->val_handle));
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
        a->svc[0]       = s_svc_defs[0];
        a->svc[1]       = (struct ble_gatt_svc_def){
            .type            = BLE_GATT_SVC_TYPE_PRIMARY,
            .uuid            = &a->uuid[0].u,
            .characteristics = a->chr,
        };
        a->svc[2] = (struct ble_gatt_svc_def){ 0 };
        a->count  = t.count;
        defs      = a->svc;
    }
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

                    if ( 0 != rc )
                    {
                        /* Freed by the callee on every path but this one,
                         * as console_drain() says. */
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

void kdrv_ble_app_kick(void)
{
    app_host_t *const a = app_host();

    /* Posted to the host's own queue, as the console's wake-up is. */
    if ( (NULL != a) && a->live && kdrv_ble_app_queued() )
    {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &a->ev);
    }
}

/** @brief The host is going: no kick reaches its queue until the next start sets the drain up. */
static void app_quiet(void)
{
    app_host_t *const a = app_host();

    if ( NULL != a )
    {
        a->live = false;
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

#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
/* ---- The bond store keeps tokens; the keys live on the C5 -----------------
 * kdrv_ble_tokens.h says what a token is. Every write of a bond record moves
 * its LTK (and the peer's IRK) to the C5 first and stores tokens in their
 * place; a write the C5 does not take fails, so the pairing fails and a key
 * never lands in this chip's NVS. Every delete tells the C5 to forget them.
 * Both run on NimBLE's host task, at pairing and unpairing only, outside
 * ble_hs_lock, and block there for one KODE round trip per key.
 *
 * NimBLE drops what the write returns (ble_sm_persist_keys()), so a refused
 * key does not fail the pairing by itself: the write records the peer, and
 * the encryption change that follows undoes the bond and ends the link. */

/** @brief How long the C5 has to take a key. */
#define BLE_KEY_PUT_MS  3000U

static ble_store_write_fn  *s_store_write;    /**< NimBLE's own, wrapped. */
static bool                 s_key_refused;    /**< A key was not kept...  */
static ble_addr_t           s_key_refused_peer; /**< ...for this peer.    */
static ble_store_delete_fn *s_store_delete;   /**< NimBLE's own, wrapped. */

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
        }
        (void)memset(&v, 0, sizeof(v));
    }
    return s_store_delete(obj_type, key);
}

/** @brief Install the two wrappers, once per stock set of callbacks. */
static void ble_store_hook(void)
{
    /* Not at ble_store_config_init(): ble_hs_pvcy re-runs it before sync and
     * puts the stock callbacks back (ble_hs_pvcy.c:337, found in spike B). */
    if ( ble_hs_cfg.store_write_cb != &ble_store_write_c5 )
    {
        s_store_write  = ble_hs_cfg.store_write_cb;
        s_store_delete = ble_hs_cfg.store_delete_cb;
        ble_hs_cfg.store_write_cb  = &ble_store_write_c5;
        ble_hs_cfg.store_delete_cb = &ble_store_delete_c5;
    }
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
#if CONFIG_KODEOS_BLE_CONSOLE_PAIRING
    ble_store_hook();
    ble_migrate_old_keys();
#endif
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
 * Never in an app: a new phone pairs in kodeOS, where the code is shown on
 * the glass, and the bonds made there reach the app through the C5's keys
 * and the P4's tokens, both of which survive the jump. In kodeOS only while
 * the board is visible: a board that serves its bonded devices and hides
 * from everyone else does not take a new one (kdrv_ble_adv_plan()).
 *
 * @return true when a pairing may go on.
 */
static bool ble_pair_open(void)
{
#if CONFIG_KODEOS_BOOT_ROLE_APP
    return false;
#else
    return !s_ble.adv_hidden;
#endif
}

/**
 * @brief Turn a pairing away: log it, optionally forget the peer, hang up.
 *
 * @param conn   The connection that asked.
 * @param forget Delete the peer's bond too: for a pairing that completed
 *               without a code, which has no business holding a slot.
 */
static void ble_pair_refuse(uint16_t conn, bool forget)
{
    struct ble_gap_conn_desc desc;

#if CONFIG_KODEOS_BOOT_ROLE_APP
    KLOG_N(TAG, "pairing refused: a new phone pairs in kodeOS; this app "
                "serves bonded phones");
#else
    KLOG_N(TAG, "pairing refused: the board is hidden; Pair new device "
                "makes it visible");
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
    if ( (BLE_STORE_EVENT_FULL == event->event_code) && !ble_pair_open() )
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
#endif /* CONFIG_KODEOS_BLE_CONSOLE_PAIRING */

#if CONFIG_KODE_LOWPOWER
/** GAP events seen since boot, for the radio's sleep sentinel (kdrv_ble_activity()). */
static atomic_uint s_gap_activity;

uint32_t kdrv_ble_activity(void)
{
    return (uint32_t)atomic_load(&s_gap_activity);
}
#endif

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
#if CONFIG_KODE_LOWPOWER
    (void)atomic_fetch_add(&s_gap_activity, 1U);
#endif
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
            else
            {
                ble_record(&event->disc);
            }
            break;
        case BLE_GAP_EVENT_DISC_COMPLETE:
            (void)xEventGroupSetBits(s_ble.bits, BLE_BIT_SCAN_DONE);
            break;
        case BLE_GAP_EVENT_CONNECT:
            if ( 0 == event->connect.status )
            {
                KLOG_N(TAG, "connected");
                s_ble.conn = event->connect.conn_handle;
                ++s_ble.links;
                s_ble.secure = false;
#if CONFIG_KODEOS_BLE_CONSOLE
                kdrv_ble_app_link_reset();
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
                 * history for why. That same timeout still turns up
                 * occasionally with only this change in place; it is not
                 * fully explained and is not new here - AO_Log and the
                 * security manager were already sharing this link before
                 * this call existed. */
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
                 * nothing. */
                (void)ble_gap_security_initiate(event->connect.conn_handle);
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
#endif
#if CONFIG_KODEOS_BLE_CONSOLE
            kdrv_ble_app_link_reset();
            s_ble.console_on = false;
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
#if CONFIG_KODEOS_BOOT_ROLE_APP
            /* No code is picked and nothing is shown: an app has no card for
             * it, and a number nobody sees would be a pairing nobody can
             * finish. Whoever asked is hung up on. */
            ble_pair_refuse(event->passkey.conn_handle, false);
#else
            /* Hidden, the board serves the devices it knows: a new one
             * pairs after Pair new device, not by asking.
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
                    s_pair_live = true;
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
#endif /* CONFIG_KODEOS_BOOT_ROLE_APP */
            break;
        }
#endif /* CONFIG_KODEOS_BLE_PAIRING_PASSKEY */
        case BLE_GAP_EVENT_ENC_CHANGE: {
            bool const refused = ble_key_refused_for(event->enc_change.conn_handle);
            s_ble.secure = (0 == event->enc_change.status) && !refused;
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
            if ( ble_refuse_unproven(event->enc_change.conn_handle,
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
                uint8_t const i = app_index(event->subscribe.attr_handle);

                if ( app_count() > i )
                {
                    kdrv_ble_app_subscribed(i, 0U != event->subscribe.cur_notify);
                }
            }
            break;
#endif /* CONFIG_KODEOS_BLE_CONSOLE */
        case BLE_GAP_EVENT_ADV_COMPLETE:
            /* The advert's time ran out (kdrv_ble_adv_plan()), or a controller
             * ended it for its own reasons. Publishing IDLE is what asks
             * AO_Radio what comes next; the same path a disconnect takes. */
            ble_set_state(KDRV_BLE_IDLE);
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
     * An app is the exception: no bond is made there, so a full store
     * refuses the request and the oldest bond, made in kodeOS, stays. */
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
    s_pair.phase = KDRV_BLE_PAIR_NONE;
    s_pair_live  = false;
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
    ble_npl_event_init(&s_tx_ev, &console_drain, NULL);
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

    (void)ble_gap_adv_stop();
    (void)ble_gap_disc_cancel();

    if ( BLE_HS_CONN_HANDLE_NONE != s_ble.conn )
    {
        /* Hang up rather than leave a session alive on a service the owner
         * has just switched off. The DISCONNECT event does the bookkeeping,
         * on the host task, exactly as it does when the peer leaves first. */
        (void)ble_gap_terminate(s_ble.conn, BLE_ERR_REM_USER_CONN_TERM);
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

    (void)ble_gap_adv_stop();
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
    s_ble.up     = false;
    s_ble.synced = false;
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
#endif
}

uint8_t kdrv_ble_bonds(void)
{
    /* The store is NimBLE's, and asking it anything before nimble_port_init()
     * has run reaches structures that do not exist yet. AO_Radio publishes
     * the BLE state with the link down as a matter of course - that is how
     * every ble_* verb is answered when the C5 is off - so this has to be
     * safe to call at any time, not merely when the stack happens to be up. */
    int n = 0;
    if ( s_ble.up )
    {
        if ( ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) != 0 )
        {
            n = 0;
        }
    }
    return (uint8_t)n;
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

void kdrv_ble_adv_plan(uint16_t itvl, uint32_t ms, bool hidden)
{
    s_ble.adv_itvl   = itvl;
    s_ble.adv_ms     = ms;
    s_ble.adv_hidden = hidden;
}

kode_status_t kdrv_ble_advertise(bool on)
{
    if ( !on )
    {
        if ( !s_ble.up )
        {
            return KODE_OK;
        }
        (void)ble_gap_adv_stop();
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
    (void)memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_BREDR_UNSUP;
    if ( !hidden )
    {
        fields.flags |= BLE_HS_ADV_F_DISC_GEN;
    }
    fields.tx_pwr_lvl_is_present = 1;
    fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
#if CONFIG_KODEOS_BLE_CONSOLE
    /* What clients filter on: requestDevice({filters:[{services}]}) in Web
     * Bluetooth, startDeviceScan([uuid]) in ble-plx. Without it a phone app
     * has to inspect every advertiser in the room. */
    fields.uuids128 = &s_svc_uuid;
    fields.num_uuids128 = 1U;
    fields.uuids128_is_complete = 1;
#else
    if ( !hidden )
    {
        fields.name = (uint8_t *)ble_svc_gap_device_name();
        fields.name_len = (uint8_t)strlen((const char *)fields.name);
        fields.name_is_complete = 1;
    }
#endif

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

    /* The time is NimBLE's host timer, an esp_timer: legacy advertising has no
     * duration in the controller. When it runs out NimBLE stops the advert and
     * ADV_COMPLETE publishes IDLE, which is what asks AO_Radio what comes
     * next. A connection ends the advert before that, and the timer with it. */
    rc_n = ble_gap_adv_start(s_ble.own_addr_type, NULL,
                             (0U != s_ble.adv_ms) ? (int32_t)s_ble.adv_ms : BLE_HS_FOREVER,
                             &params, &ble_gap_event, NULL);
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
    /* A timed advert is started again every time it runs out: only a change
     * of kind is news. */
    if ( s_ble.adv_said && (s_ble.adv_said_hidden == hidden) )
    {
        return KODE_OK;
    }
    s_ble.adv_said        = true;
    s_ble.adv_said_hidden = hidden;
    if ( hidden )
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
        && (0U != s_ble.adv_itvl) && ((uint16_t)KDRV_BLE_ITVL_SLOW > s_ble.adv_itvl) )
    {
        (void)ble_gap_adv_stop();
        kdrv_ble_adv_plan((uint16_t)KDRV_BLE_ITVL_SLOW, ms, s_ble.adv_hidden);
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

kode_status_t kdrv_ble_scan(uint32_t ms)
{
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
    EventBits_t const bits = xEventGroupWaitBits(s_ble.bits, BLE_BIT_SCAN_DONE,
                                                 pdTRUE, pdFALSE,
                                                 pdMS_TO_TICKS(ms + BLE_SCAN_MARGIN_MS));
    if ( (bits & BLE_BIT_SCAN_DONE) == 0U )
    {
        (void)ble_gap_disc_cancel();
        KLOG_W(TAG, "the scan did not report its own end");
    }

    KLOG_N(TAG, "scan: %u device(s)", (unsigned)s_ble.count);
    return KODE_OK;
}

kode_status_t kdrv_ble_scan_find(const char *name, uint32_t max_ms,
                                 kdrv_ble_dev_t *dev, bool *found)
{
    if ( (NULL == dev) || (NULL == found) )
    {
        return KODE_DENIED;
    }
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
