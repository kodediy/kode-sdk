/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file radio_verbs.c
 * @brief AO_Radio's console verbs: the link, the C5's table, the identity, Wi-Fi and BLE, and `info`'s `c5=` record, registered by kaos_radio_start().
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kaos_wire.h"
#include "kdrv_c5.h"
#include "kdrv_c5svc.h"
#include "kdrv_radio.h"
#include "khal_identity.h"
#include "khal_time.h"
#include "kode_wire.h"
#if CONFIG_KODEOS_BLE
#include "kdrv_ble_heard.h"
#include "kdrv_ble.h"
#include "kdrv_ble_hid.h"
#endif
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"
#include "radio_hid.h"

/** @brief Which radio verb holds the console. */
typedef enum {
    RADIO_V_NONE = 0,    /**< None of them, or a field verb.               */
    RADIO_V_LINK,        /**< radio_info: closed by RADIO_STATE_SIG.        */
    RADIO_V_ID_SIGN,     /**< id_sign: closed by ID_SIGN_RSP_SIG.           */
    RADIO_V_ID,          /**< id_cert, ident_csr, ident_cert, ident_lock:
                          *   ID_PART_SIG records until ID_END_SIG.         */
    RADIO_V_WIPE,        /**< c5_wipe: closed by C5_WIPE_RSP_SIG.           */
    RADIO_V_WIFI,        /**< wifi_up/down/leave/info/auto: closed by
                          *   WIFI_STATE_SIG. Several hand the watchdog
                          *   over: association plus DHCP outlasts it, and
                          *   AO_Radio answers either way.                  */
    RADIO_V_WIFI_JOIN,   /**< wifi_join: closed by the WIFI_STATE_SIG that
                          *   carries its join's generation, and by no
                          *   other; the watchdog is handed over.           */
    RADIO_V_WIFI_SCAN,   /**< wifi_scan: entries until WIFI_SCAN_END_SIG.   */
    RADIO_V_WIFI_SAVED,  /**< wifi_list: entries until WIFI_SAVED_END_SIG.  */
    RADIO_V_WIFI_FORGET, /**< wifi_forget: closed by WIFI_FORGET_SIG.       */
    RADIO_V_FRAMES,      /**< wifi_frames: its window, then its record; the
                          *   watchdog is the window's end.                 */
    RADIO_V_BLE,         /**< ble_*: closed by BLE_STATE_SIG.               */
    RADIO_V_BLE_SCAN,    /**< ble_scan: entries until BLE_SCAN_END_SIG.     */
    RADIO_V_BLE_HEARD   /**< ble_heard: its window, then its record; the
                          *   watchdog is the window's end.                 */
} radio_verb_kind_t;

static radio_verb_kind_t s_radio_kind;

#if CONFIG_KODEOS_WIFI
/** @brief Networks `wifi_scan` has printed since it began, or since its listing started again. */
static uint16_t s_scan_rows;

/** @brief The generation of the join that holds the console (wifi_join_request_gen()). */
static uint8_t s_join_gen;

/** @brief The network `wifi_forget` asked for: only its answer closes the command. */
static char s_forget_ssid[KODE_SSID_MAX + 1U];
#endif

#if CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE
/** @brief The generation of the scan request that holds the console
 *         (wifi_scan_request(), ble_scan_request()). */
static uint8_t s_scan_gen;

/** @brief Generations this far ahead of the console's, or further, are behind
 *         it: half of what a byte counts. */
#define RADIO_V_GEN_HALF  128U

/**
 * @brief Whether a refusal is another request's, not the console's.
 *
 * AO_Radio answers a scan it has no time for, or one an app's rest of the
 * radio turns away, with an end that says busy or rest and carries that
 * request's generation: with the console's own listing in flight, somebody
 * else's refusal passes on the bus.
 *
 * @param gen The end's generation.
 * @return true for a refusal that names a request other than the console's.
 */
static bool scan_busy_foreign(uint8_t gen)
{
    return ((uint8_t)KQP_SCAN_GEN_NONE != gen)
           && ((uint8_t)KQP_SCAN_GEN_NONE != s_scan_gen)
           && (gen != s_scan_gen);
}

/**
 * @brief Whether an entry or an end belongs to a listing before the console's request.
 *
 * Such a one was published before AO_Radio read the request and is still in
 * the console's queue: the end of the scan a line has just replaced, or of
 * somebody else's that ended a moment ago.
 *
 * @param gen The event's generation.
 * @return true for one older than the request; false for the request's own,
 *         for a newer one (the listing started again for somebody else) and
 *         for one that carries none.
 */
static bool scan_gen_stale(uint8_t gen)
{
    bool stale = false;

    if ( ((uint8_t)KQP_SCAN_GEN_NONE != gen) && ((uint8_t)KQP_SCAN_GEN_NONE != s_scan_gen) )
    {
        uint8_t const ahead = (uint8_t)(gen - s_scan_gen);

        stale = (RADIO_V_GEN_HALF <= ahead);
    }
    return stale;
}
#endif

#define RADIO_V_C5_TEXT_MAX   32U  /**< Room for the C5 image's project, version or IDF name. */
#define RADIO_V_SVC_TEXT_MAX  64U  /**< Room for `info`'s KODE-services words. */
#define RADIO_V_B64_IN    3U  /**< Bytes base64 takes at a time... */
#define RADIO_V_B64_OUT   4U  /**< ...and the characters it writes for them. */
/** @brief Room for @p n bytes as base64, padding and terminator included. */
#define RADIO_V_B64_TEXT(n)  ((((((size_t)(n) + RADIO_V_B64_IN) - 1U) / RADIO_V_B64_IN) * RADIO_V_B64_OUT) + 1U)
#define RADIO_V_WIPE_MS     12000U /**< c5_wipe: three steps, each with AO_Radio's own 3 s, and margin. */
#define RADIO_V_FRAMES_MS_MIN   100U    /**< wifi_frames: the shortest window, one period of the C5's batches. */
#define RADIO_V_FRAMES_MS_MAX   10000U  /**< wifi_frames: the longest window.                                 */
#define RADIO_V_HEARD_MS_MIN      100U    /**< ble_heard: the shortest window.                                 */
#define RADIO_V_HEARD_MS_MAX      10000U  /**< ble_heard: the longest window.                                  */
#define RADIO_V_AT_REST  "\"radio: at rest\""  /**< wifi_scan, ble_scan: the refusal while an app rests the radio, quoted. */

/** @brief RADIO_V_ID: the records' word, `id` or `csr`. */
static const char *s_radio_id_rec;

/* ---- Handlers: the unit's identity, held by the C5 ---------------------- */

/**
 * @brief Close an identity command that did not succeed, naming why.
 * @param st  KODE_ST_* from the C5, or from AO_Radio on its behalf.
 * @param key What a bad request was about: `nonce`, `part`.
 */
static void id_fail(uint8_t st, const char *key)
{
    if ( KODE_ST_BAD_REQUEST == st )
    {
        console_reply_err(KCLI_ERR_VALUE, key);
    }
    else if ( KODE_ST_NOT_FOUND == st )
    {
        console_reply_err(KCLI_ERR_REJECT, "identity");
    }
    else if ( KODE_ST_LOCKED == st )
    {
        console_reply_err(KCLI_ERR_REJECT, "locked");
    }
    else if ( KODE_ST_STORAGE == st )
    {
        console_reply_err(KCLI_ERR_REJECT, "storage");
    }
    else
    {
        /* busy, the link down, or no answer: AO_Radio says which in the log */
        console_reply_err(KCLI_ERR_REJECT, "refused");
    }
    console_end();
}

/** @brief Start an identity command whose records are called @p rec. */
static void id_begin(const kverb_t *c, const char *rec, uint8_t op, uint8_t n,
                     uint8_t of, const uint8_t *der, size_t len)
{
    console_begin(c->verb);
    s_radio_kind   = RADIO_V_ID;
    s_radio_id_rec = rec;
    if ( !id_request(AO_Console, op, n, of, der, len) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/**
 * @brief id_sign nonce="<n>": the unit's key signs "kode-auth/v1\n" + n on
 *        the C5. `>|sig der="<base64>"` then `>ok`.
 */
static void h_id_sign(const kverb_t *c, const karg_val_t *v)
{
    if ( !kode_nonce_ok(v[0].s, strlen(v[0].s)) )
    {
        console_reply_err(KCLI_ERR_VALUE, "nonce");
        return;
    }
    console_begin(c->verb);
    s_radio_kind = RADIO_V_ID_SIGN;
    if ( !id_sign_request(AO_Console, v[0].s) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/**
 * @brief id_cert: `>|id serial=`, the pair as `>|id part= of= der=` records,
 *        `>|id unit_len= len=`, then `>ok`.
 */
static void h_id_cert(const kverb_t *c, const karg_val_t *v)
{
    char        q[CONSOLE_QUOTED_MAX(KHAL_ID_LEN)];
    const char *serial = NULL;

    (void)v;
    if ( khal_identity_valid() )
    {
        serial = khal_identity_serial();
    }
    console_reply(">|id serial=%s", console_kv_text(q, sizeof(q), serial));
    id_begin(c, "id", (uint8_t)ID_OP_CERT, 0U, 0U, NULL, 0U);
}

/** @brief ident_csr: the CSR as `>|csr part= of= der=` records. Not over BLE. */
static void h_ident_csr(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    if ( console_src_allows(CONSOLE_NEED_NOT_BLE) )
    {
        id_begin(c, "csr", (uint8_t)ID_OP_CSR, 0U, 0U, NULL, 0U);
    }
}

/**
 * @brief ident_cert part=<n> of=<N> der="<base64>": one part of the pair the
 *        factory's CA signed, in order; the last one stores it. Not over BLE.
 */
static void h_ident_cert(const kverb_t *c, const karg_val_t *v)
{
    uint8_t der[KODE_ID_PART_BYTES];
    size_t  len = 0U;

    if ( !console_src_allows(CONSOLE_NEED_NOT_BLE) )
    {
        return;
    }
    if ( v[0].u > v[1].u )
    {
        console_reply_err(KCLI_ERR_VALUE, "part");
        return;
    }
    if ( !kaos_wire_b64_decode(v[2].s, der, sizeof(der), &len) || (0U == len) )
    {
        console_reply_err(KCLI_ERR_VALUE, "der");
        return;
    }
    id_begin(c, "id", (uint8_t)ID_OP_CERT_PUT, (uint8_t)v[0].u,
             (uint8_t)v[1].u, der, len);
}

/**
 * @brief c5_wipe confirm=on: forget the user's secrets - the C5's networks
 *        and BLE keys, and this chip's bonds. The identity stays. Not over BLE.
 */
static void h_c5_wipe(const kverb_t *c, const karg_val_t *v)
{
    if ( !console_src_allows(CONSOLE_NEED_NOT_BLE) )
    {
        return;
    }
    if ( !v[0].b )
    {
        console_reply_err(KCLI_ERR_VALUE, "confirm");
        return;
    }
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIPE;
    console_extend(RADIO_V_WIPE_MS);
    if ( !c5_wipe_request(AO_Console) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief ident_lock confirm=on: close the identity for good. Not over BLE. */
static void h_ident_lock(const kverb_t *c, const karg_val_t *v)
{
    if ( !console_src_allows(CONSOLE_NEED_NOT_BLE) )
    {
        return;
    }
    if ( !v[0].b )
    {
        console_reply_err(KCLI_ERR_VALUE, "confirm");
        return;
    }
    id_begin(c, "id", (uint8_t)ID_OP_LOCK, 0U, 0U, NULL, 0U);
}

/** @brief Resolve a coprocessor field name. */
static kode_status_t c5_field_by_name(const char *name, uint16_t *field)
{
    kdrv_c5_field_t f = KDRV_C5_FIELD_MAX;
    kode_status_t const rc = kdrv_c5_field_by_name(name, &f);

    *field = (uint16_t)f;
    return rc;
}

/** @brief Describe a coprocessor field. */
static bool c5_field_row(uint16_t field, console_field_t *out)
{
    /* The coprocessor's rows: what each one says about the chip on the other end
     * of the SDIO link. In enum order, and the assertion below keeps it so. */
    static const char *const C5_FIELD_DESC[] = {
        [KDRV_C5_VALID]           = "A snapshot has been taken since the last link-up.",
        [KDRV_C5_CHIP_ID]         = "The slave's esp_chip_model_t; 23 is an ESP32-C5.",
        [KDRV_C5_FW_MAJOR]        = "esp-hosted version the slave runs: major.",
        [KDRV_C5_FW_MINOR]        = "esp-hosted version the slave runs: minor.",
        [KDRV_C5_FW_PATCH]        = "esp-hosted version the slave runs: patch.",
        [KDRV_C5_HOST_MAJOR]      = "esp-hosted version this firmware was built with: major.",
        [KDRV_C5_HOST_MINOR]      = "esp-hosted version this firmware was built with: minor.",
        [KDRV_C5_HOST_PATCH]      = "esp-hosted version this firmware was built with: patch.",
        [KDRV_C5_APP_DESC]        = "The slave answered the image-descriptor RPC.",
        [KDRV_C5_PROJECT_KODE]    = "The slave image's project name is kode_c5.",
        [KDRV_C5_MAC_HI]          = "Station MAC, bytes 0 and 1.",
        [KDRV_C5_MAC_MID]         = "Station MAC, bytes 2 and 3.",
        [KDRV_C5_MAC_LO]          = "Station MAC, bytes 4 and 5.",
        [KDRV_C5_RESET_REASON]    = "Why the C5 last booted (esp_reset_reason_t), from CP_INIT.",
        [KDRV_C5_LINK_MS]         = "Connect to CP_INIT at the last bring-up.",
        [KDRV_C5_HEAP_FREE_KB]    = "Free internal heap on the slave.",
        [KDRV_C5_HEAP_LARGEST_KB] = "The largest free block of it.",
        [KDRV_C5_SDIO_KHZ]        = "The clock the SDIO transport negotiated.",
        [KDRV_C5_SDIO_WIDTH]      = "Data lines the SDIO transport negotiated: 1 or 4.",
        [KDRV_C5_SD_CD]           = "C5 IO6 as read back: the slot's card detect, polarity unverified.",
        [KDRV_C5_PWR_EXT]         = "C5 IO5 as read back: J3's +3.3V_EXT enable, 1 = on.",
    };
    _Static_assert((sizeof(C5_FIELD_DESC) / sizeof(C5_FIELD_DESC[0]))
                   == (size_t)KDRV_C5_FIELD_MAX,
                   "C5_FIELD_DESC does not cover kdrv_c5's table");

    const kdrv_c5_field_info_t * const i = kdrv_c5_field_info((kdrv_c5_field_t)field);
    bool const ok = (NULL != i);

    if ( ok )
    {
        out->name = i->name; out->unit = i->unit;
        out->min = i->min;   out->max = i->max;
        out->writable = i->writable;
        out->desc = C5_FIELD_DESC[field];
    }
    return ok;
}

/**
 * @brief Close a command AO_Radio had no room for: `>err code=4 msg=busy`.
 *
 * For a verb that began its command; the answer would never come, and the
 * ones that disarm the watchdog would hold the console for ever.
 */
static void radio_v_busy_end(void)
{
    console_reply_err(KCLI_ERR_BUSY, "busy");
    console_end();
}

/** @brief Ask the owner about a coprocessor field. */
static void c5_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    if ( !c5_field_request(AO_Console, op, field, value) )
    {
        radio_v_busy_end();
    }
}

static const console_chip_t C5_CHIP = {
    .chip = "c5", .count = (uint16_t)KDRV_C5_FIELD_MAX,
    .by_name = c5_field_by_name, .row = c5_field_row, .ask = c5_field_ask,
};

/** @brief c5: the coprocessor's table, answered from AO_Radio's snapshot. */
static void h_c5(const kverb_t *c, const karg_val_t *v)
{
    s_radio_kind = RADIO_V_NONE;
    console_field_cmd(&C5_CHIP, c->verb, v);
}

/** @brief c5_help: the coprocessor field table, in slices. */
static void h_c5_help(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    s_radio_kind = RADIO_V_NONE;
    console_field_help(&C5_CHIP, c->verb);
}

/* ---- Handlers: radio -----------------------------------------------------
 * The mutations answer `>ok` as an accept; the outcome comes later, in '!'
 * lines and in the state `radio_info` reads back. Bring-up outlasts any
 * console deadline (a C5 reset plus up to 10 s waiting for CP_INIT).
 */

/** @brief radio_up: bring the C5 link up. */
static void h_radio_up(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    radio_up_request();
    console_reply(">ok");
}

/** @brief radio_down: tear the link down (the C5 keeps running). */
static void h_radio_down(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    if ( radio_down_request() )
    {
        console_reply(">ok");
    }
    else
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
    }
}

/** @brief exp_power state=on|off: switch J3's +3.3V_EXT. The pin is the
 *         C5's, so the outcome is AO_Radio's; `c5 field="pwr_ext"` reads
 *         it back while the link is up. */
static void h_exp_power(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    if ( exp_power_request(v[0].b) )
    {
        console_reply(">ok");
    }
    else
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
    }
}

/** @brief radio_info: closed by RADIO_STATE_SIG - always answerable, every
 *         state publishes on request. */
static void h_radio_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_LINK;
    if ( !radio_state_request() )
    {
        radio_v_busy_end();
    }
}

#if defined(CONFIG_KODEOS_WIFI) || defined(CONFIG_KODEOS_BLE)
/* ---- A window of listening: wifi_frames and ble_heard ------------------
 * Both ask AO_Radio for a stream with the console as the asker, let a window
 * of the console's pacer pass, ask for the stop and print one record. What
 * they share is here: the run's phases, the slice pacer that ends the window
 * and asks again for a stop that had no room, the answer of the start and of
 * the stop, the refusal words and the watchdog's net. What differs (the
 * request, the counts and the record) is each verb's, handed in as a
 * window_ops_t.
 */

/** @brief Where a window run stands. */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps the run small */
typedef enum {
    WINDOW_IDLE = 0,   /**< No run.                                         */
    WINDOW_STARTING,   /**< The start is out; AO_Radio's answer is awaited. */
    WINDOW_OPEN,       /**< The radio listens; the window runs.             */
    WINDOW_STOPPING    /**< The stop is wanted or out; its answer is awaited. */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps the run small */
} window_phase_t;

#define RADIO_V_WINDOW_SLICE_MS 10U     /**< The console pacer's slice, which the stop's retries count. */
#define RADIO_V_WINDOW_STOP_MS  1000U   /**< How long a stop with no room in AO_Radio's queue is asked again. */
#define RADIO_V_WINDOW_WAIT_MS  5000U   /**< The longest wait for AO_Radio's answer to a start or a stop. For `wifi_frames` it is
                                         *   above AO_Radio's own 3 s deadline for a request of the C5's service, so that answer
                                         *   wins and this is only the net. A `ble_heard` job is a worker job under the worker's
                                         *   much longer guard, so for it this wait is the deadline itself. */
#define RADIO_V_WINDOW_LATE_MS  (KODE_RADIO_SVC_SEND_GUARD_MS + RADIO_V_WINDOW_WAIT_MS) /**< How long after the net caught a run the answer of
                                         *   ONE `wifi_frames` request may still arrive: AO_Radio's send guard, then its deadline.
                                         *   A start and the stop held behind it take longer, and a `ble_heard` job may answer
                                         *   later still; while any of them waits AO_Radio answers the next run's start busy, so
                                         *   that run ends `busy` and is not misled, and an answer that comes once the console is
                                         *   idle is dropped. */
#define RADIO_V_WINDOW_OK       0U      /**< Both verbs' reasons say "listening, or stopped as asked" with 0. */

#define RADIO_V_LATE_FRAMES  0U   /**< The late note of `wifi_frames`. */
#define RADIO_V_LATE_BLE     1U   /**< The late note of `ble_heard`.  */
#define RADIO_V_LATE_N       2U   /**< How many notes: one per verb, their answers are different events. */

/** @brief When the answers of a run the watchdog ended may still come. */
static struct {
    bool     on;   /**< A run was ended by the watchdog.                  */
    uint32_t ms;   /**< When, on khal_time_ms()'s clock.                  */
} s_window_late[RADIO_V_LATE_N];

/** @brief What differs between the verbs that run a window. */
typedef struct {
    bool                 (*stop)(void);        /**< Ask AO_Radio to stop, as the console; false when it had no room. */
    void                 (*record)(void);      /**< Print the window's record.                                       */
    const char *const     *words;              /**< One word per reason, in the reason's order; `busy` is the busy one. */
    size_t                 n_words;            /**< How many.                                                        */
    uint8_t                busy;               /**< The reason that is `>err code=4`.                                */
    radio_verb_kind_t      kind;               /**< The verb's kind, which holds the console while it runs.          */
    size_t                 late;               /**< Its slot in s_window_late.                                       */
} window_ops_t;

/** @brief The run in hand: the console holds one verb at a time, so one is enough for both. */
static struct {
    uint8_t  phase;      /**< window_phase_t.                                  */
    uint32_t ms;         /**< The window.                                      */
    uint32_t start_ms;   /**< When it began, on khal_time_ms()'s clock.        */
    uint32_t tries;      /**< Slices spent asking for a stop that had no room. */
    bool     stop_out;   /**< The stop was posted.                             */
} s_window;

/**
 * @brief Take the console for a window run, unless the last one's answers may still be on their way.
 *
 * A run the watchdog ended may still have an answer coming, and nothing
 * tells it from the answer of this one's start: it is refused busy until
 * RADIO_V_WINDOW_LATE_MS have passed.
 *
 * @param c   The verb.
 * @param ops What differs for it.
 * @param ms  The window.
 * @return false when the run was refused and answered.
 */
static bool window_begin(const kverb_t *c, const window_ops_t *ops, uint32_t ms)
{
    if ( s_window_late[ops->late].on
        && (RADIO_V_WINDOW_LATE_MS > (khal_time_ms() - s_window_late[ops->late].ms)) )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
        return false;
    }
    s_window_late[ops->late].on = false;
    console_begin(c->verb);
    s_radio_kind = ops->kind;
    (void)memset(&s_window, 0, sizeof(s_window));
    s_window.ms    = ms;
    s_window.phase = (uint8_t)WINDOW_STARTING;
    console_extend(RADIO_V_WINDOW_WAIT_MS);
    return true;
}

/**
 * @brief End a run whose start could not be posted: nothing started, so nothing is left to stop.
 * @param c The verb.
 */
static void window_start_refused(const kverb_t *c)
{
    s_window.phase = (uint8_t)WINDOW_IDLE;
    console_reply_err(KCLI_ERR_REJECT, c->verb);
    console_end();
}

/** @brief Whether a run holds the console, in any phase. */
static bool window_active(void)
{
    return (uint8_t)WINDOW_IDLE != s_window.phase;
}

/** @brief Whether a run has the radio listening, or a stop on its way: records count, slices run. */
static bool window_counting(void)
{
    return ((uint8_t)WINDOW_OPEN == s_window.phase)
        || ((uint8_t)WINDOW_STOPPING == s_window.phase);
}

/**
 * @brief End the command with the reason AO_Radio gave.
 * @param ops The verb.
 * @param why Its reason.
 */
static void window_fail(const window_ops_t *ops, uint8_t why)
{
    s_window.phase = (uint8_t)WINDOW_IDLE;
    if ( ops->busy == why )
    {
        console_reply_err(KCLI_ERR_BUSY, ops->words[why]);
    }
    else if ( (RADIO_V_WINDOW_OK < why) && (ops->n_words > (size_t)why) )
    {
        console_reply_err(KCLI_ERR_REJECT, ops->words[why]);
    }
    else
    {
        console_reply_err(KCLI_ERR_REJECT, console_verb());
    }
    console_end();
}

/**
 * @brief AO_Radio's answer to the start or the stop.
 *
 * @param ops The verb.
 * @param on  Whether the radio listens now.
 * @param why The reason it gave.
 * @return true when this answer made the run listen: the verb takes what
 *         the answer says of the stream.
 */
static bool window_state(const window_ops_t *ops, bool on, uint8_t why)
{
    bool began = false;

    if ( (uint8_t)WINDOW_STARTING == s_window.phase )
    {
        if ( on )
        {
            s_window.phase    = (uint8_t)WINDOW_OPEN;
            s_window.start_ms = khal_time_ms();   /* the window runs from here */
            began = true;
            console_pace();
        }
        else if ( RADIO_V_WINDOW_OK != why )
        {
            window_fail(ops, why);
        }
        else
        {
            /* A refusal is never OK: this is the answer of an earlier run's
             * stop, which came late. The start's own answer is still to come. */
        }
    }
    else if ( (uint8_t)WINDOW_OPEN == s_window.phase )
    {
        /* The link went in the middle of the window, or the radio left
         * the stream: AO_Radio has freed its door, nothing is left on. */
        if ( !on )
        {
            window_fail(ops, why);
        }
    }
    else if ( ((uint8_t)WINDOW_STOPPING == s_window.phase) && s_window.stop_out && !on )
    {
        /* The stop's answer, or its expiry. */
        s_window.phase = (uint8_t)WINDOW_IDLE;
        ops->record();
        if ( RADIO_V_WINDOW_OK == why )
        {
            console_reply(">ok");
            console_end();
        }
        else
        {
            /* The stop expired: the radio did not confirm, and a record it
             * still sends with nobody asking will quiet it. */
            window_fail(ops, why);
        }
    }
    else
    {
        /* Nothing waits for it. */
    }
    return began;
}

/**
 * @brief The stop did not go in, or the worker could not take it: ask again at the next slice.
 *
 * It is asked again every slice for RADIO_V_WINDOW_STOP_MS; then the command
 * ends with its record and `>err code=5 msg=stop`: the radio may still be
 * listening and AO_Radio still counts the console as the asker, so its next
 * record reaches the console with no run to read it, and console_own() asks
 * for the stop then.
 *
 * @param ops The verb.
 */
static void window_stop_again(const window_ops_t *ops)
{
    s_window.stop_out = false;
    if ( (s_window.tries * RADIO_V_WINDOW_SLICE_MS) < RADIO_V_WINDOW_STOP_MS )
    {
        s_window.tries += 1U;
        console_pace();
    }
    else
    {
        s_window.phase = (uint8_t)WINDOW_IDLE;
        ops->record();
        console_reply_err(KCLI_ERR_REJECT, "stop");
        console_end();
    }
}

/**
 * @brief A slice of the console's pacer: the window may have run out.
 *
 * At its end the stop is asked for; with no room in AO_Radio's queue it is
 * asked again by window_stop_again().
 *
 * @param ops The verb.
 */
static void window_slice(const window_ops_t *ops)
{
    bool const over = (khal_time_ms() - s_window.start_ms) >= s_window.ms;

    if ( !over && ((uint8_t)WINDOW_OPEN == s_window.phase) )
    {
        console_pace();
    }
    else if ( (uint8_t)WINDOW_OPEN == s_window.phase )
    {
        s_window.phase = (uint8_t)WINDOW_STOPPING;
    }
    else
    {
        /* Already stopping: only a stop that had no room comes back here. */
    }
    if ( ((uint8_t)WINDOW_STOPPING == s_window.phase) && !s_window.stop_out )
    {
        if ( ops->stop() )
        {
            s_window.stop_out = true;
            console_extend(RADIO_V_WINDOW_WAIT_MS);   /* its answer, or the net */
        }
        else
        {
            window_stop_again(ops);
        }
    }
}

/**
 * @brief The watchdog ran out on a window run: the net under an answer of AO_Radio that never came, or a pacer that stopped.
 *
 * The stop is asked for unless one is out already (behind a start still
 * waiting, AO_Radio holds it until the start is answered); a stop AO_Radio's
 * queue had no room for is asked again by console_own() at the stream's next
 * record. The record is printed when there is one, and the console's own
 * timeout error is the one terminal line. The answers of the requests left
 * out are not told from the next run's, so window_begin() refuses the next
 * run until they can no longer come.
 *
 * @param ops The verb.
 */
static void window_timeout(const window_ops_t *ops)
{
    if ( window_active() )
    {
        if ( !s_window.stop_out )
        {
            (void)ops->stop();
        }
        if ( (uint8_t)WINDOW_STARTING != s_window.phase )
        {
            ops->record();
        }
        s_window.phase = (uint8_t)WINDOW_IDLE;
        s_window_late[ops->late].on = true;
        s_window_late[ops->late].ms = khal_time_ms();
    }
}
#endif /* CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE */

#if CONFIG_KODEOS_WIFI
/* ---- Handlers: wifi ------------------------------------------------------ */

/** @brief wifi_scan: entries arrive as WIFI_SCAN_ENTRY_SIG, closed by END. */
static void h_wifi_scan(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    /* A scan outlasts the console's 5 s deadline: the watchdog is disarmed,
     * and AO_Radio's answer, which always comes, closes the command. */
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI_SCAN;
    s_scan_rows  = 0U;
    s_scan_gen   = (uint8_t)KQP_SCAN_GEN_NONE;
    console_unwatched();
    /* With the watchdog disarmed, only the end marker closes the command,
     * and a request AO_Radio's queue had no room for will never have one. */
    if ( !wifi_scan_request(&s_scan_gen) )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
        console_end();
    }
}

/**
 * @brief wifi_scan_stop: ask AO_Radio to end the listing in flight.
 *
 * The table's verb that does not wait (kverbs_t.nowait): it runs while
 * `wifi_scan` holds the console, so it begins no command and leaves
 * @c s_radio_kind alone. `>ok` is an accept, with a listing in flight or
 * without one; the cut `wifi_scan` closes itself, `>err code=5 msg=stopped`.
 */
static void h_wifi_scan_stop(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    if ( wifi_scan_stop_request() )
    {
        console_reply(">ok");
    }
    else
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
    }
}

/** @brief wifi_join ssid="<s>" [pass="<p>"] [remember=on|off]: closed by
 *         the WIFI_STATE_SIG that ends its own join. remember=off is the
 *         factory's own network, which must not travel to the customer in
 *         the vault. A name of more than KODE_SSID_MAX bytes is refused
 *         `msg=ssid`, a password of 1 to KODE_WIFI_PASS_MIN - 1 or more than
 *         KODE_PASS_MAX bytes `msg=pass`, both code 3.
 * @param c The verb's row.
 * @param v Its arguments: ssid, pass, remember. */
static void h_wifi_join(const kverb_t *c, const karg_val_t *v)
{
    const char *pass     = "";
    bool        remember = true;

    if ( v[1].present )
    {
        pass = v[1].s;
    }
    if ( v[2].present )
    {
        remember = v[2].b;
    }
    /* The grammar refuses an empty string; a longer one than the join event
     * holds is refused here, never cut. */
    size_t const ssid_len = strlen(v[0].s);
    size_t const pass_len = strlen(pass);
    if ( KODE_SSID_MAX < ssid_len )
    {
        console_reply_err(KCLI_ERR_VALUE, "ssid");
        return;
    }
    if ( (0U != pass_len)
        && ((KODE_WIFI_PASS_MIN > pass_len) || (KODE_PASS_MAX < pass_len)) )
    {
        console_reply_err(KCLI_ERR_VALUE, "pass");
        return;
    }
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI_JOIN;
    s_join_gen   = (uint8_t)KQP_SCAN_GEN_NONE;
    console_unwatched();   /* association + DHCP outlast 5 s */
    /* With the watchdog disarmed, only the join's own end closes the
     * command, and a request the pool dropped will never have one. */
    if ( !wifi_join_request_gen(v[0].s, pass, remember, &s_join_gen) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief wifi_leave: disconnect and forget. Closed by WIFI_STATE_SIG. */
static void h_wifi_leave(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    if ( !wifi_leave_request() )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief wifi_up: enable the WiFi service. Closed by WIFI_STATE_SIG. */
static void h_wifi_up(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    console_unwatched();   /* rides autojoin's scan, like wifi_auto */
    if ( !wifi_start_request() )
    {
        radio_v_busy_end();
    }
}

/** @brief wifi_down: disable the WiFi service; stops the station outright.
 *  Closed by WIFI_STATE_SIG. */
static void h_wifi_down(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    console_unwatched();   /* the disconnect/stop/deinit chain */
    if ( !wifi_stop_request() )
    {
        radio_v_busy_end();
    }
}

/**
 * @brief wifi_auto: get onto a remembered network without naming one.
 *
 * A scan plus one or more joins, so the watchdog is disarmed as for `wifi_scan`.
 * AO_Radio always answers: with an address, or the state that says it could not.
 */
static void h_wifi_auto(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    console_unwatched();
    if ( !wifi_auto_request() )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/** @brief wifi_list: the remembered networks, names only. */
static void h_wifi_list(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI_SAVED;
    if ( !wifi_list_request() )
    {
        radio_v_busy_end();
    }
}

/**
 * @brief wifi_forget ssid="<s>": drop one from the store. Does not disconnect.
 *        Closed by WIFI_FORGET_SIG: `>ok` once the C5's vault forgot it.
 */
static void h_wifi_forget(const kverb_t *c, const karg_val_t *v)
{
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI_FORGET;
    (void)snprintf(s_forget_ssid, sizeof(s_forget_ssid), "%s", v[0].s);
    if ( !wifi_forget_request(v[0].s) )
    {
        radio_v_busy_end();
    }
}

/** @brief wifi_info: closed by WIFI_STATE_SIG. */
static void h_wifi_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    if ( !wifi_state_request() )
    {
        radio_v_busy_end();
    }
}

/** @brief What `wifi_frames` has counted in its window. */
static struct {
    uint8_t  channel;    /**< The radio's channel, as the last answer said. */
    bool     joined;     /**< The station holds an association.             */
    uint32_t heard;      /**< Frames heard since the start: the last tick's. */
    uint32_t handed;     /**< Frames AO_Radio handed over.                  */
    uint32_t dropped;    /**< Frames lost: the last tick's.                 */
    uint32_t mgmt;       /**< Management frames, summed over the ticks.     */
    uint32_t data;       /**< Data frames, summed over the ticks.           */
    int8_t   rssi_max;   /**< The strongest tick's level, dBm.              */
    bool     rssi_seen;  /**< A tick had a level.                           */
} s_frames_run;

/** @brief Ask AO_Radio to stop the frames, as the console. */
static bool frames_ask_stop(void)
{
    return wifi_frames_request(AO_Console, false, 0U);
}

/** @brief Print the window's record: counts and levels, no address, no payload. */
static void frames_record(void)
{
    if ( s_frames_run.rssi_seen )
    {
        console_reply(">|frames ch=%" PRIu32 " joined=%" PRIu32 " heard=%" PRIu32 " handed=%" PRIu32
                      " dropped=%" PRIu32 " mgmt=%" PRIu32 " data=%" PRIu32 " rssi_max=%" PRId32,
                      (uint32_t)s_frames_run.channel, (uint32_t)s_frames_run.joined, s_frames_run.heard,
                      s_frames_run.handed, s_frames_run.dropped, s_frames_run.mgmt, s_frames_run.data,
                      (int32_t)s_frames_run.rssi_max);
    }
    else
    {
        console_reply(">|frames ch=%" PRIu32 " joined=%" PRIu32 " heard=%" PRIu32 " handed=%" PRIu32
                      " dropped=%" PRIu32 " mgmt=%" PRIu32 " data=%" PRIu32 " rssi_max=-",
                      (uint32_t)s_frames_run.channel, (uint32_t)s_frames_run.joined, s_frames_run.heard,
                      s_frames_run.handed, s_frames_run.dropped, s_frames_run.mgmt, s_frames_run.data);
    }
}

/** @brief `wifi_frames`' refusal words, one per kwifi_frames_why_t. */
static const char *const FRAMES_WORDS[] = {
    "ok", "no_link", "no_service", "wifi_off", "joined", "channel", "busy", "memory", "resting",
};                                           /* kwifi_frames_why_t order */
_Static_assert((sizeof(FRAMES_WORDS) / sizeof(FRAMES_WORDS[0])) == ((size_t)KWIFI_FRAMES_RESTING + 1U),
               "one word per reason");
_Static_assert((uint32_t)KWIFI_FRAMES_OK == RADIO_V_WINDOW_OK, "the window's OK is the frames' OK");

/** @brief What `wifi_frames` hands the shared run: its stop, its record, its words. */
static const window_ops_t FRAMES_OPS = {
    .stop    = frames_ask_stop,
    .record  = frames_record,
    .words   = FRAMES_WORDS,
    .n_words = sizeof(FRAMES_WORDS) / sizeof(FRAMES_WORDS[0]),
    .busy    = (uint8_t)KWIFI_FRAMES_BUSY,
    .kind    = RADIO_V_FRAMES,
    .late    = RADIO_V_LATE_FRAMES,
};

/**
 * @brief wifi_frames ch=<0..177> ms=<100..10000>: Wi-Fi frames heard on a
 *        channel: counts.
 *
 * Starts listening on @p ch (0 stays where the radio is), lets the window
 * pass, stops, and prints one record of counts and levels, then `>ok`. An
 * address or a payload is never printed.
 *
 * The command holds the console until AO_Radio has answered the stop, so an
 * answer of a run that ended normally cannot reach the next one: AO_Radio
 * answers a start and a stop, its own deadline standing behind each, and the
 * watchdog is the net under that (RADIO_V_WINDOW_WAIT_MS); the window is
 * counted in the console's pacer slices (console_pace()). A run the net had to
 * end leaves answers that may still arrive. The next run waits
 * RADIO_V_WINDOW_LATE_MS, which covers one request; a start and a stop that
 * both still wait are covered by AO_Radio itself, which answers a start busy
 * while a request of the service waits. A stream left with the console as
 * its asker is stopped at its next tick (console_own()).
 */
static void h_wifi_frames(const kverb_t *c, const karg_val_t *v)
{
    if ( !window_begin(c, &FRAMES_OPS, v[1].u) )
    {
        return;
    }
    (void)memset(&s_frames_run, 0, sizeof(s_frames_run));
    if ( !wifi_frames_request(AO_Console, true, (uint8_t)v[0].u) )
    {
        window_start_refused(c);
    }
}

/**
 * @brief AO_Radio's answer to the start or the stop of `wifi_frames`.
 * @param a The answer.
 */
static void frames_state(WifiFramesStateEvt const * const a)
{
    if ( window_state(&FRAMES_OPS, a->on, a->why) )
    {
        s_frames_run.channel = a->channel;
        s_frames_run.joined  = a->joined;
    }
}

/**
 * @brief One period's counts: the window's record sums them.
 * @param t The tick.
 */
static void frames_tick(WifiFramesTickEvt const * const t)
{
    s_frames_run.channel = t->channel;
    s_frames_run.joined  = t->joined;
    s_frames_run.heard   = t->heard;
    s_frames_run.dropped = t->dropped;
    s_frames_run.mgmt   += t->mgmt;
    s_frames_run.data   += t->data;
    if ( (KODE_SNIFF_RSSI_NONE != t->rssi_max)
        && ((!s_frames_run.rssi_seen) || (t->rssi_max > s_frames_run.rssi_max)) )
    {
        s_frames_run.rssi_max  = t->rssi_max;
        s_frames_run.rssi_seen = true;
    }
}
#endif /* CONFIG_KODEOS_WIFI */

#if CONFIG_KODEOS_BLE
/* ---- Handlers: ble -------------------------------------------------------
 * There is no `ble_advertise`: `ble_up` means on the air, and AO_Radio keeps it
 * so for five minutes with nobody on the board (radio_ble_adv_reconcile()).
 * `ble_scan` brings the stack up for itself without implying any of that.
 */

/** @brief ble_up: switch BLE on, which is to say go on the air. */
static void h_ble_up(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_BLE;
    console_unwatched();
    if ( !ble_start_request() )
    {
        radio_v_busy_end();
    }
}

/** @brief Two hex digits a maker byte. */
#define RADIO_BLE_MFG_DIGITS 2U
/** @brief `mfg=`'s text: the digits of every byte, and its terminator. */
#define RADIO_BLE_MFG_TEXT  ((KQP_BLE_MFG_MAX * RADIO_BLE_MFG_DIGITS) + 1U)
/** @brief Four hex digits a service. */
#define RADIO_BLE_SVC_DIGITS 4U
/** @brief `svc=`'s text: each service, a comma between, the terminator. */
#define RADIO_BLE_SVC_TEXT  (KQP_BLE_SVC_MAX * (RADIO_BLE_SVC_DIGITS + 1U))

/**
 * @brief A device's maker data as `ble_scan` prints it: bare hex, or `-`.
 *
 * @param out RADIO_BLE_MFG_TEXT bytes.
 * @param d   The device.
 * @return @p out.
 */
static const char *ble_mfg_text(char *out, BleScanEvt const *d)
{
    uint8_t n = d->mfg_len;

    if ( KQP_BLE_MFG_MAX < n )
    {
        n = (uint8_t)KQP_BLE_MFG_MAX;
    }
    out[0] = '-';
    out[1] = '\0';
    for ( uint8_t i = 0U; i < n; ++i )
    {
        (void)snprintf(&out[i * RADIO_BLE_MFG_DIGITS],
                       RADIO_BLE_MFG_DIGITS + 1U, "%02" PRIx32, (uint32_t)d->mfg[i]);
    }
    return out;
}

/**
 * @brief A device's services as `ble_scan` prints them: hex, comma between,
 *        or `-`.
 *
 * @param out RADIO_BLE_SVC_TEXT bytes.
 * @param d   The device.
 * @return @p out.
 */
static const char *ble_svc_text(char *out, BleScanEvt const *d)
{
    uint8_t n   = d->svc_n;
    size_t  at  = 0U;

    if ( KQP_BLE_SVC_MAX < n )
    {
        n = (uint8_t)KQP_BLE_SVC_MAX;
    }
    out[0] = '-';
    out[1] = '\0';
    for ( uint8_t i = 0U; i < n; ++i )
    {
        if ( 0U < i )
        {
            out[at] = ',';
            ++at;
        }
        (void)snprintf(&out[at], RADIO_BLE_SVC_DIGITS + 1U, "%04" PRIx32,
                       (uint32_t)d->svc16[i]);
        at += RADIO_BLE_SVC_DIGITS;
    }
    return out;
}

/** @brief ble_scan [ms=]: one record per device, closed by the end marker.
 *         Without `ms` AO_Radio listens its default. */
static void h_ble_scan(const kverb_t *c, const karg_val_t *v)
{
    uint32_t ms = 0U;

    if ( v[0].present )
    {
        ms = v[0].u;
    }
    console_begin(c->verb);
    s_radio_kind = RADIO_V_BLE_SCAN;
    s_scan_gen   = (uint8_t)KQP_SCAN_GEN_NONE;
    console_unwatched();
    /* With the watchdog disarmed, only the end marker closes the command,
     * and a request the pool or AO_Radio's queue had no room for will never
     * have one. */
    if ( !ble_scan_request(ms, &s_scan_gen) )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
        console_end();
    }
}

/** @brief ble_down: take the stack down; the C5 keeps the link. */
static void h_ble_down(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_BLE;
    console_unwatched();
    if ( !ble_stop_request() )
    {
        radio_v_busy_end();
    }
}

/**
 * @brief The word after `pair=`: the board shows itself to a new device,
 *        will once the connected one leaves, or was not asked to.
 *
 * `on` only with the link free: with a device on it nothing is on the air,
 * whether the time to pair has started or waits. A board switched off shows
 * itself to nobody.
 *
 * @param b The state.
 * @return `on`, `wait` or `off`.
 */
static const char *ble_pair_word(BleStateEvt const * const b)
{
    bool const  linked = ((uint8_t)KDRV_BLE_CONNECTED == b->state);
    const char *word   = "off";

    if ( (uint8_t)KDRV_BLE_OFF == b->state )
    {
        /* off, whatever was asked before the switch */
    }
    else if ( b->pair_wait || (b->pair_open && linked) )
    {
        word = "wait";
    }
    else if ( b->pair_open )
    {
        word = "on";
    }
    else
    {
        /* off */
    }
    return word;
}

/** @brief ble_info: closed by BLE_STATE_SIG. */
static void h_ble_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_BLE;
    ble_state_request();
}

/**
 * @brief Answer a BLE verb AO_Radio refused: the state it published says why.
 * @param why KQP_BLE_REFUSED_*, from the state.
 * @return true when the verb was refused and has been answered.
 */
static bool ble_refused_reply(uint8_t why)
{
    bool refused = true;

    if ( (uint8_t)KQP_BLE_REFUSED_BUSY == why )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
    }
    else if ( (uint8_t)KQP_BLE_REFUSED_OFF == why )
    {
        console_reply_err(KCLI_ERR_REJECT, "off");
    }
    else if ( (uint8_t)KQP_BLE_REFUSED_ATTACHED == why )
    {
        console_reply_err(KCLI_ERR_REJECT, "attached");
    }
    else
    {
        refused = false;
    }
    return refused;
}

/** @brief ble_unpair: forget every bonded peer. */
static void h_ble_unpair(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_BLE;
    /* A worker job that crosses the link, like ble_down: this AO's five
     * seconds are not the right deadline for it. */
    console_unwatched();
    if ( !ble_unpair_request() )
    {
        radio_v_busy_end();
    }
}

/** @brief ble_pair: show the board for five minutes, so a new device can pair. */
static void h_ble_pair(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    if ( ble_pair_open_request() )
    {
        s_radio_kind = RADIO_V_BLE;
    }
    else
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
        console_end();
    }
}

/** @brief What `ble_heard` has counted in its window. */
static struct {
    uint32_t heard;       /**< Advertisements heard: the stack's total, as the last answer said. */
    bool     heard_seen;  /**< An answer carried the totals.                                     */
    uint32_t handed;      /**< Advertisements AO_Radio handed over.                              */
    uint32_t dropped;     /**< Advertisements not handed over: the last record's or answer's total. */
    uint32_t conn;        /**< Handed over as connectable (ADV_IND).                             */
    uint32_t direct;      /**< Handed over as directed (ADV_DIRECT_IND).                         */
    uint32_t scan;        /**< Handed over as scannable only (ADV_SCAN_IND).                     */
    uint32_t nonconn;     /**< Handed over as non-connectable (ADV_NONCONN_IND).                 */
    int8_t   rssi_max;    /**< The strongest level handed over, dBm.                             */
    bool     rssi_seen;   /**< One had a level.                                                  */
} s_ble_run;

/** @brief Characters of a level in dBm: a sign, three digits and the terminator. */
#define RADIO_V_LEVEL_TEXT  5U

/** @brief Ask AO_Radio to stop the advertisements, as the console. */
static bool ble_heard_ask_stop(void)
{
    return ble_heard_request(AO_Console, false);
}

/** @brief Print the window's record: counts and levels, no address, no name, no payload. */
static void ble_heard_record(void)
{
    char     level[RADIO_V_LEVEL_TEXT];
    uint32_t heard = s_ble_run.heard;

    level[0] = '-';
    level[1] = '\0';

    if ( !s_ble_run.heard_seen )
    {
        /* No answer carried the totals: what was handed over and what was lost is what is known. */
        heard = s_ble_run.handed + s_ble_run.dropped;
    }
    if ( s_ble_run.rssi_seen )
    {
        (void)snprintf(level, sizeof(level), "%" PRId32, (int32_t)s_ble_run.rssi_max);
    }
    console_reply(">|ble_heard heard=%" PRIu32 " handed=%" PRIu32 " dropped=%" PRIu32 " conn=%" PRIu32
                  " direct=%" PRIu32 " scan=%" PRIu32 " nonconn=%" PRIu32 " rssi_max=%s",
                  heard, s_ble_run.handed, s_ble_run.dropped, s_ble_run.conn, s_ble_run.direct,
                  s_ble_run.scan, s_ble_run.nonconn, level);
}

/** @brief `ble_heard`'s refusal words, one per kble_heard_why_t. */
static const char *const BLE_HEARD_WORDS[] = {
    "ok", "no_link", "ble_off", "busy", "memory", "refused", "resting", "cancelled",
};                                           /* kble_heard_why_t order */
_Static_assert((sizeof(BLE_HEARD_WORDS) / sizeof(BLE_HEARD_WORDS[0])) == ((size_t)KBLE_HEARD_CANCELLED + 1U),
               "one word per reason");
_Static_assert((uint32_t)KBLE_HEARD_OK == RADIO_V_WINDOW_OK, "the window's OK is the advertisements' OK");

/** @brief What `ble_heard` hands the shared run: its stop, its record, its words. */
static const window_ops_t BLE_HEARD_OPS = {
    .stop    = ble_heard_ask_stop,
    .record  = ble_heard_record,
    .words   = BLE_HEARD_WORDS,
    .n_words = sizeof(BLE_HEARD_WORDS) / sizeof(BLE_HEARD_WORDS[0]),
    .busy    = (uint8_t)KBLE_HEARD_BUSY,
    .kind    = RADIO_V_BLE_HEARD,
    .late    = RADIO_V_LATE_BLE,
};

/**
 * @brief ble_heard ms=<100..10000>: BLE advertisements heard: counts.
 *
 * Starts the stream with the console as the asker, lets the window
 * pass, stops, and prints one record of counts and levels, then `>ok`. An
 * address, a name or a payload is never printed. The run is the one
 * `wifi_frames` has, and h_wifi_frames() says how its stop and its watchdog
 * are held.
 */
static void h_ble_heard(const kverb_t *c, const karg_val_t *v)
{
    if ( !window_begin(c, &BLE_HEARD_OPS, v[0].u) )
    {
        return;
    }
    (void)memset(&s_ble_run, 0, sizeof(s_ble_run));
    if ( !ble_heard_request(AO_Console, true) )
    {
        window_start_refused(c);
    }
}

/**
 * @brief AO_Radio's answer to the start or the stop of `ble_heard`.
 *
 * A stop the radio's worker could not take comes back as still listening
 * with `busy`: the run asks again, as it does for a stop with no room in the
 * queue. Every other answer that says listening and is not a start's is
 * nobody's here.
 *
 * @param a The answer.
 */
static void ble_heard_state(BleHeardStateEvt const * const a)
{
    bool const still = a->on && ((uint8_t)KBLE_HEARD_OK != a->why);   /* a stop the worker could not take, or a stray */

    if ( !still )
    {
        if ( window_counting() )
        {
            s_ble_run.heard      = a->heard;
            s_ble_run.dropped    = a->dropped;
            s_ble_run.heard_seen = true;
        }
        (void)window_state(&BLE_HEARD_OPS, a->on, a->why);
    }
    else if ( ((uint8_t)WINDOW_STOPPING == s_window.phase) && s_window.stop_out
             && ((uint8_t)KBLE_HEARD_BUSY == a->why) )
    {
        window_stop_again(&BLE_HEARD_OPS);
    }
    else
    {
        /* Nothing waits for it. */
    }
}

/**
 * @brief One advertisement handed over: the window's record counts it.
 * @param a The advertisement; only its kind, its level and the stream's total of lost ones are read.
 */
static void ble_heard_one(BleHeardEvt const * const a)
{
    s_ble_run.handed  += 1U;
    s_ble_run.dropped  = a->dropped;
    if ( KBLE_HEARD_PDU_CONN == a->pdu )
    {
        s_ble_run.conn += 1U;
    }
    else if ( KBLE_HEARD_PDU_DIRECT == a->pdu )
    {
        s_ble_run.direct += 1U;
    }
    else if ( KBLE_HEARD_PDU_SCAN == a->pdu )
    {
        s_ble_run.scan += 1U;
    }
    else if ( KBLE_HEARD_PDU_NONCONN == a->pdu )
    {
        s_ble_run.nonconn += 1U;
    }
    else
    {
        /* A scan response or anything else: handed over, in no kind of the record. */
    }
    if ( (KBLE_HEARD_RSSI_NONE != a->rssi)
        && ((!s_ble_run.rssi_seen) || (a->rssi > s_ble_run.rssi_max)) )
    {
        s_ble_run.rssi_max  = a->rssi;
        s_ble_run.rssi_seen = true;
    }
}

/* ---- Handlers: the HID service --------------------------------------------
 * Each verb answers at once: its report is queued for the host or refused,
 * and nothing later says more. The three that send a report run from the
 * cable alone: what they type and click lands on somebody's computer, and a
 * line from the network or from the scheduler has no business there. */

/** @brief The most a console line moves the mouse or turns its wheel in one report. */
#define RADIO_V_HID_STEP_MAX  127U

_Static_assert(RADIO_V_HID_STEP_MAX == (uint32_t)KDRV_BLE_HID_STEP_MAX,
               "a line's step is one the report carries");

/**
 * @brief Close a HID verb with what the door answered.
 * @param kind The kind the report was of.
 * @param rc   ble_hid_report_post()'s status.
 */
static void hid_reply(uint8_t kind, kode_status_t rc)
{
    if ( KODE_OK == rc )
    {
        console_reply(">ok");
    }
    else if ( KODE_NO_RESOURCES == rc )
    {
        console_reply_err(KCLI_ERR_BUSY, "busy");
    }
    else if ( !kdrv_ble_hid_served() )
    {
        console_reply_err(KCLI_ERR_REJECT, "off");
    }
    else if ( 0U == (kdrv_ble_hid_kinds() & kind) )
    {
        console_reply_err(KCLI_ERR_REJECT, "kind");
    }
    else
    {
        console_reply_err(KCLI_ERR_REJECT, "no_host");
    }
}

/**
 * @brief One step of the mouse from the two keys that name its directions.
 * @param plus  The key that moves it one way.
 * @param minus The key that moves it the other.
 * @return The step; both keys given cancel out.
 */
static int8_t hid_step(const karg_val_t *plus, const karg_val_t *minus)
{
    int32_t step = 0;

    if ( plus->present )
    {
        step += (int32_t)plus->u;
    }
    if ( minus->present )
    {
        step -= (int32_t)minus->u;
    }
    return (int8_t)step;
}

/** @brief ble_hid_info: the kinds served, whether a host takes the reports, and the counts. */
static void h_ble_hid_info(const kverb_t *c, const karg_val_t *v)
{
    (void)c; (void)v;
    uint32_t sent    = 0U;
    uint32_t dropped = 0U;
    uint8_t  kinds   = 0U;

    if ( kdrv_ble_hid_served() )
    {
        kinds = kdrv_ble_hid_kinds();
    }
    kdrv_ble_hid_counts(&sent, &dropped);
    console_reply(">|ble_hid kinds=%s ready=%s sent=%" PRIu32 " dropped=%" PRIu32,
                  RADIO_HID_WORDS[kinds], console_on_off(kdrv_ble_hid_ready()), sent, dropped);
    console_reply(">ok");
}

/** @brief ble_hid_mouse [buttons=] [right=] [left=] [down=] [up=] [hold=]: one
 *         mouse report. Buttons are pressed and let go unless `hold=on`. */
static void h_ble_hid_mouse(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    uint8_t rep[KDRV_BLE_HID_MOUSE_LEN];
    uint8_t buttons = 0U;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    if ( v[0].present )
    {
        buttons = (uint8_t)v[0].u;
    }
    bool const hold = v[5].present && v[5].b;

    kdrv_ble_hid_mouse_pack(buttons, hid_step(&v[1], &v[2]), hid_step(&v[3], &v[4]), 0, rep);
    hid_reply((uint8_t)KDRV_BLE_HID_MOUSE,
              ble_hid_report_post((uint8_t)KDRV_BLE_HID_MOUSE, rep, sizeof(rep),
                                  (0U != buttons) && !hold));
}

/** @brief ble_hid_wheel [up=] [down=]: turn the mouse's wheel; every button is let go. */
static void h_ble_hid_wheel(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    uint8_t rep[KDRV_BLE_HID_MOUSE_LEN];

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    kdrv_ble_hid_mouse_pack(0U, 0, 0, hid_step(&v[0], &v[1]), rep);
    hid_reply((uint8_t)KDRV_BLE_HID_MOUSE,
              ble_hid_report_post((uint8_t)KDRV_BLE_HID_MOUSE, rep, sizeof(rep), false));
}

/** @brief ble_hid_key code= [mods=]: press one key, with its modifiers, and let it go. */
static void h_ble_hid_key(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    uint8_t rep[KDRV_BLE_HID_KEYS_LEN];
    uint8_t keys[KDRV_BLE_HID_KEYS_MAX];
    uint8_t mods = 0U;

    if ( !console_src_allows(CONSOLE_NEED_CABLE) )
    {
        return;
    }
    (void)memset(keys, 0, sizeof(keys));
    keys[0] = (uint8_t)v[0].u;
    if ( v[1].present )
    {
        mods = (uint8_t)v[1].u;
    }
    kdrv_ble_hid_keys_pack(mods, keys, rep);
    hid_reply((uint8_t)KDRV_BLE_HID_KEYBOARD,
              ble_hid_report_post((uint8_t)KDRV_BLE_HID_KEYBOARD, rep, sizeof(rep), true));
}
#endif /* CONFIG_KODEOS_BLE */

#if defined(CONFIG_KODEOS_WIFI) || defined(CONFIG_KODEOS_BLE)
/**
 * @brief The window a verb of @p kind runs.
 * @param kind The verb holding the console.
 * @return Its ops, or NULL when it runs no window.
 */
static const window_ops_t *window_ops_of(radio_verb_kind_t kind)
{
    const window_ops_t *ops = NULL;

#ifdef CONFIG_KODEOS_WIFI
    if ( RADIO_V_FRAMES == kind )
    {
        ops = &FRAMES_OPS;
    }
#endif
#ifdef CONFIG_KODEOS_BLE
    if ( RADIO_V_BLE_HEARD == kind )
    {
        ops = &BLE_HEARD_OPS;
    }
#endif
    return ops;
}
#endif /* CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE */

/** @brief `info`'s `>|c5=` record, from the snapshot the last bring-up left. */
static void radio_info(void)
{
    /* No link is crossed, so this answers with the radio off too. `recog` is
     * what the P4 makes of the snapshot: a Kode build, a stock slave, an older
     * one with no descriptor RPC, or no snapshot this boot. Only the running
     * image is described. */
    int32_t v = 0;
    bool const valid = (kdrv_c5_get(KDRV_C5_VALID, &v) == KODE_OK)
                       && (0 != v);
    bool const desc  = (kdrv_c5_get(KDRV_C5_APP_DESC, &v) == KODE_OK)
                       && (0 != v);
    bool const kode  = desc
                       && (kdrv_c5_get(KDRV_C5_PROJECT_KODE, &v) == KODE_OK)
                       && (0 != v);
    /* Empty on a failed read, which console_kv_text() prints as `-`. */
    char project[RADIO_V_C5_TEXT_MAX] = "";
    char version[RADIO_V_C5_TEXT_MAX] = "";
    char idf[RADIO_V_C5_TEXT_MAX]     = "";
    char qp[CONSOLE_QUOTED_MAX(sizeof(project))];
    char qv[CONSOLE_QUOTED_MAX(sizeof(version))];
    char qi[CONSOLE_QUOTED_MAX(sizeof(idf))];
    if ( kdrv_c5_project_name(project, sizeof(project)) != KODE_OK )
    {
        project[0] = '\0';
    }
    if ( kdrv_c5_app_version(version, sizeof(version)) != KODE_OK )
    {
        version[0] = '\0';
    }
    if ( kdrv_c5_idf_ver(idf, sizeof(idf)) != KODE_OK )
    {
        idf[0] = '\0';
    }
    /* What the P4 makes of what it saw. */
    const char *recog = "stock";

    if ( !valid )
    {
        recog = "nolink";
    }
    else if ( !desc )
    {
        recog = "old";
    }
    else if ( kode )
    {
        recog = "kode";
    }
    else
    {
        /* A stock esp-hosted slave. */
    }
    /* `power` is the C5's own reset line, not the link: held, the chip
     * is off and nothing above describes a running image. */
    const char *power = "running";

    if ( kdrv_radio_c5_held() )
    {
        power = "held";
    }
    /* What the KODE services on the C5 hold, from its last INFO: counts
     * only, never a secret. `-` until a link has brought an INFO. */
    char svc[RADIO_V_SVC_TEXT_MAX];
    kode_info_t ki;
    if ( kdrv_c5svc_info(&ki) )
    {
        const char *id = "none";
        if ( (ki.id_flags & KODE_ID_F_LOCKED) != 0U )
        {
            id = "locked";
        }
        else if ( (ki.id_flags & KODE_ID_F_CERT) != 0U )
        {
            id = "cert";
        }
        else if ( (ki.id_flags & KODE_ID_F_KEY) != 0U )
        {
            id = "key";
        }
        else
        {
            /* none */
        }
        (void)snprintf(svc, sizeof(svc), "wire=%" PRIu32 " vault=%" PRIu32 " btkeys=%" PRIu32 " id=%s",
                       (uint32_t)ki.wire_version, (uint32_t)ki.vault_count,
                       (uint32_t)ki.btkeys_count, id);
    }
    else
    {
        (void)snprintf(svc, sizeof(svc), "%s",
                       "wire=- vault=- btkeys=- id=-");
    }
    console_reply(">|c5=running project=%s version=%s idf=%s recog=%s power=%s %s",
          console_kv_text(qp, sizeof(qp), project),
          console_kv_text(qv, sizeof(qv), version),
          console_kv_text(qi, sizeof(qi), idf),
          recog, power, svc);
}

/** @brief AO_Radio's answers to the verb in flight. */
static bool radio_answer(QEvt const *e)
{
    bool handled = true;

    switch ( e->sig )
    {
        case ID_SIGN_RSP_SIG: {
            if ( RADIO_V_ID_SIGN == s_radio_kind )
            {
                IdSignRspEvt const * const a = Q_EVT_CAST(IdSignRspEvt);
                if ( KODE_ST_OK == a->status )
                {
                    char b64[RADIO_V_B64_TEXT(sizeof(a->der))];
                    (void)kaos_wire_b64_encode(a->der, a->der_len, b64);
                    console_reply(">|sig der=\"%s\"", b64);
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    id_fail(a->status, "nonce");
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
        case ID_PART_SIG: {
            if ( RADIO_V_ID == s_radio_kind )
            {
                IdPartEvt const * const a = Q_EVT_CAST(IdPartEvt);
                char b64[RADIO_V_B64_TEXT(KODE_ID_PART_BYTES)];
                (void)kaos_wire_b64_encode(a->der, a->len, b64);
                console_reply(">|%s part=%" PRIu32 " of=%" PRIu32 " der=\"%s\"", s_radio_id_rec,
                      (uint32_t)a->n, (uint32_t)a->of, b64);
            }
            else
            {
                handled = false;
            }
            break;
        }
        case C5_WIPE_RSP_SIG: {
            if ( RADIO_V_WIPE == s_radio_kind )
            {
                C5WipeRspEvt const * const a = Q_EVT_CAST(C5WipeRspEvt);
                static const char *const STEP[] = { "vault", "btkeys", "ble" };
                if ( (uint8_t)C5_WIPE_DONE == a->step )
                {
                    console_reply(">ok");
                }
                else if ( (uint8_t)C5_WIPE_DONE > a->step )
                {
                    console_reply_err(KCLI_ERR_REJECT, STEP[a->step]);
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, console_verb());
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case ID_END_SIG: {
            if ( RADIO_V_ID == s_radio_kind )
            {
                IdEndEvt const * const a = Q_EVT_CAST(IdEndEvt);
                if ( KODE_ST_OK == a->status )
                {
                    if ( 0U < a->unit_len )
                    {
                        console_reply(">|id unit_len=%" PRIu32 " len=%" PRIu32, (uint32_t)a->unit_len,
                              (uint32_t)a->len);
                    }
                    console_reply(">ok");
                    console_end();
                }
                else
                {
                    id_fail(a->status, "part");
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
        case RADIO_FIELD_RSP_SIG: {
            /* The answer comes from the snapshot the last bring-up left, so a
             * refusal is a row it did not fill (no link yet, an older slave)
             * or a write, which the table has no room for. */
            RadioFieldEvt const * const a = Q_EVT_CAST(RadioFieldEvt);

            console_field_answer(&C5_CHIP, a->op, a->field, a->rc, a->value);
            break;
        }
        case RADIO_STATE_SIG: {
            if ( RADIO_V_LINK == s_radio_kind )
            {
                RadioStateEvt const * const r = Q_EVT_CAST(RadioStateEvt);
                static const char *const LINKS[] = {
                    "off", "starting", "up", "stopping", "failed",
                };                                    /* kradio_link_t order */
                const char * const link =
                    console_word(LINKS, sizeof(LINKS) / sizeof(LINKS[0]), r->link);
                const char *sd = "direct";

                if ( r->sd_hosted )
                {
                    sd = "hosted";
                }
                if ( r->fw_known )
                {
                    console_reply(">|link=%s fw=%" PRIu32 ".%" PRIu32 ".%" PRIu32 " sd=%s",
                          link,
                          (uint32_t)r->fw_major,
                          (uint32_t)r->fw_minor,
                          (uint32_t)r->fw_patch, sd);
                }
                else
                {
                    console_reply(">|link=%s fw=unknown sd=%s", link, sd);
                }
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
#if CONFIG_KODEOS_WIFI
        case WIFI_SCAN_ENTRY_SIG: {
            if ( (RADIO_V_WIFI_SCAN == s_radio_kind)
                && scan_gen_stale(Q_EVT_CAST(WifiScanEntryEvt)->gen) )
            {
                /* A network of the listing before this command's: not printed. */
            }
            else if ( RADIO_V_WIFI_SCAN == s_radio_kind )
            {
                WifiScanEntryEvt const * const n = Q_EVT_CAST(WifiScanEntryEvt);
                /* Quoted: an SSID may legitimately contain a space, a quote
                 * or any other byte, and comes back exactly as it is. */
                char safe[CONSOLE_QUOTED_MAX(sizeof(n->ssid))];
                if ( (uint16_t)UINT16_MAX > s_scan_rows )
                {
                    ++s_scan_rows;
                }
                console_reply(">|ssid=%s rssi=%" PRId32 " ch=%" PRIu32 " auth=%" PRIu32 " "
                      "bssid=%02x:%02x:%02x:%02x:%02x:%02x",
                      console_quote(safe, sizeof(safe), n->ssid), (int32_t)n->rssi,
                      (uint32_t)n->channel, (uint32_t)n->authmode,
                      n->bssid[0], n->bssid[1], n->bssid[2],
                      n->bssid[3], n->bssid[4], n->bssid[5]);
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_SCAN_END_SIG: {
            if ( (RADIO_V_WIFI_SCAN == s_radio_kind)
                && (scan_gen_stale(Q_EVT_CAST(WifiScanEndEvt)->gen)
                    || ((Q_EVT_CAST(WifiScanEndEvt)->busy || Q_EVT_CAST(WifiScanEndEvt)->rest)
                        && scan_busy_foreign(Q_EVT_CAST(WifiScanEndEvt)->gen))) )
            {
                /* The end of the listing before this command's, or somebody
                 * else's refusal: it closes nothing. */
            }
            else if ( RADIO_V_WIFI_SCAN == s_radio_kind )
            {
                WifiScanEndEvt const * const end = Q_EVT_CAST(WifiScanEndEvt);
                if ( end->ok )
                {
                    console_reply(">ok");
                }
                else if ( end->rest )
                {
                    console_reply_err(KCLI_ERR_BUSY, RADIO_V_AT_REST);
                }
                else if ( end->busy )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
                }
                else if ( end->cut )
                {
                    /* The networks above were heard; the rest of the air
                     * was never swept. */
                    console_reply_err(KCLI_ERR_REJECT, "stopped");
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, console_verb());
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_SCAN_AGAIN_SIG: {
            if ( (RADIO_V_WIFI_SCAN == s_radio_kind)
                && scan_gen_stale(Q_EVT_CAST(WifiScanEndEvt)->gen) )
            {
                /* A restart of the listing before this command's. */
            }
            else if ( RADIO_V_WIFI_SCAN == s_radio_kind )
            {
                /* A newer `wifi_scan` from elsewhere: the listing goes on
                 * with a new sweep. The networks above, if any, were heard
                 * by the one it ended. */
                if ( 0U < s_scan_rows )
                {
                    console_reply(">|scan=again");
                }
                s_scan_rows = 0U;
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_FRAMES_STATE_SIG: {
            if ( RADIO_V_FRAMES == s_radio_kind )
            {
                frames_state(Q_EVT_CAST(WifiFramesStateEvt));
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_FRAMES_TICK_SIG: {
            /* A run's own, whatever its phase: one still starting keeps it
             * from the console, which would stop a stream that has an asker
             * (see console_own()). Counted only while the C5 listens. */
            if ( (RADIO_V_FRAMES == s_radio_kind) && window_active() )
            {
                if ( window_counting() )
                {
                    frames_tick(Q_EVT_CAST(WifiFramesTickEvt));
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_FRAME_SIG: {
            if ( (RADIO_V_FRAMES == s_radio_kind) && window_active() )
            {
                if ( window_counting() )
                {
                    s_frames_run.handed += 1U;
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_SAVED_ENTRY_SIG: {
            if ( RADIO_V_WIFI_SAVED == s_radio_kind )
            {
                WifiSavedEvt const * const n = Q_EVT_CAST(WifiSavedEvt);
                char safe[CONSOLE_QUOTED_MAX(sizeof(n->ssid))];
                console_reply(">|ssid=%s", console_quote(safe, sizeof(safe), n->ssid));
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_SAVED_END_SIG: {
            if ( RADIO_V_WIFI_SAVED == s_radio_kind )
            {
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_FORGET_SIG: {
            WifiForgetEvt const * const f = Q_EVT_CAST(WifiForgetEvt);
            if ( (RADIO_V_WIFI_FORGET == s_radio_kind)
                && (0 == strcmp(f->ssid, s_forget_ssid)) )
            {
                if ( (uint8_t)WIFI_FORGET_RES_DONE == f->result )
                {
                    console_reply(">ok");
                    console_end();
                }
                else if ( (uint8_t)WIFI_FORGET_RES_BUSY == f->result )
                {
                    radio_v_busy_end();
                }
                else if ( (uint8_t)WIFI_FORGET_RES_NO_LINK == f->result )
                {
                    console_reply_err(KCLI_ERR_REJECT, "no_link");
                    console_end();
                }
                else
                {
                    /* the vault said no, or did not answer: the log says which */
                    console_reply_err(KCLI_ERR_REJECT, "refused");
                    console_end();
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
#endif
#if CONFIG_KODEOS_BLE
        case BLE_SCAN_ENTRY_SIG: {
            if ( (RADIO_V_BLE_SCAN == s_radio_kind)
                && scan_gen_stale(Q_EVT_CAST(BleScanEvt)->gen) )
            {
                /* A device of the listing before this command's: not printed. */
            }
            else if ( RADIO_V_BLE_SCAN == s_radio_kind )
            {
                BleScanEvt const * const d = Q_EVT_CAST(BleScanEvt);
                char safe[CONSOLE_QUOTED_MAX(sizeof(d->name))];
                char mfg[RADIO_BLE_MFG_TEXT];
                char svc[RADIO_BLE_SVC_TEXT];
                const char *type = "public";
                if ( (uint8_t)KDRV_BLE_ADDR_PUBLIC != d->addr_type )
                {
                    type = "random";
                }
                console_reply(">|addr=%02x:%02x:%02x:%02x:%02x:%02x rssi=%" PRId32 " name=%s "
                      "type=%s mfg=%s svc=%s",
                      d->addr[0], d->addr[1], d->addr[2],
                      d->addr[3], d->addr[4], d->addr[5], (int32_t)d->rssi,
                      console_kv_text(safe, sizeof(safe), d->name), type,
                      ble_mfg_text(mfg, d), ble_svc_text(svc, d));
            }
            else
            {
                handled = false;
            }
            break;
        }
        case BLE_SCAN_END_SIG: {
            if ( (RADIO_V_BLE_SCAN == s_radio_kind)
                && (scan_gen_stale(Q_EVT_CAST(WifiScanEndEvt)->gen)
                    || ((Q_EVT_CAST(WifiScanEndEvt)->busy || Q_EVT_CAST(WifiScanEndEvt)->rest)
                        && scan_busy_foreign(Q_EVT_CAST(WifiScanEndEvt)->gen))) )
            {
                /* The end of the listing before this command's, or somebody
                 * else's refusal: it closes nothing. */
            }
            else if ( RADIO_V_BLE_SCAN == s_radio_kind )
            {
                WifiScanEndEvt const * const end = Q_EVT_CAST(WifiScanEndEvt);
                if ( end->ok )
                {
                    console_reply(">ok");
                }
                else if ( end->rest )
                {
                    console_reply_err(KCLI_ERR_BUSY, RADIO_V_AT_REST);
                }
                else if ( end->busy )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
                }
                else if ( end->cut )
                {
                    /* A newer Bluetooth request ended the listen. */
                    console_reply_err(KCLI_ERR_REJECT, "stopped");
                }
                else
                {
                    console_reply_err(KCLI_ERR_REJECT, console_verb());
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case BLE_STATE_SIG: {
            bool refused = false;

            if ( RADIO_V_BLE == s_radio_kind )
            {
                refused = ble_refused_reply(Q_EVT_CAST(BleStateEvt)->refused);
            }
            if ( refused )
            {
                console_end();
            }
            else if ( RADIO_V_BLE == s_radio_kind )
            {
                BleStateEvt const * const b = Q_EVT_CAST(BleStateEvt);
                static const char *const STATES[] = {
                    "off", "idle", "advertising", "connected",
                };                                  /* kdrv_ble_state_t order */
                const char * const state =
                    console_word(STATES, sizeof(STATES) / sizeof(STATES[0]), b->state);
                char name[CONSOLE_QUOTED_MAX(KQP_BLE_NAME_MAX)];
                /* `idle` is a gap, not a resting place: BLE on means
                 * advertising, and this reports the instant between the
                 * stack coming up and the advert reaching the worker. */
                if ( b->addr_known )
                {
                    console_reply(">|ble=%s addr=%02x:%02x:%02x:%02x:%02x:%02x "
                          "console=%s tx_drop=%" PRIu32 " bonds=%" PRIu32 " backlog=%" PRIu32 " retry=%" PRIu32
                          " hidden=%s pair=%s name=%s",
                          state,
                          b->addr[0], b->addr[1], b->addr[2],
                          b->addr[3], b->addr[4], b->addr[5],
                          console_on_off(b->console),
                          (uint32_t)b->tx_drop, (uint32_t)b->bonds,
                          (uint32_t)b->backlog, (uint32_t)b->retry,
                          console_on_off(b->hidden), ble_pair_word(b),
                          console_kv_text(name, sizeof(name), b->name));
                }
                else
                {
                    console_reply(">|ble=%s addr=unknown console=%s tx_drop=%" PRIu32 " "
                          "bonds=%" PRIu32 " backlog=%" PRIu32 " retry=%" PRIu32 " hidden=%s pair=%s name=%s",
                          state, console_on_off(b->console),
                          (uint32_t)b->tx_drop, (uint32_t)b->bonds,
                          (uint32_t)b->backlog, (uint32_t)b->retry,
                          console_on_off(b->hidden), ble_pair_word(b),
                          console_kv_text(name, sizeof(name), b->name));
                }
                console_reply(">ok");
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
        case BLE_HEARD_STATE_SIG: {
            if ( RADIO_V_BLE_HEARD == s_radio_kind )
            {
                ble_heard_state(Q_EVT_CAST(BleHeardStateEvt));
            }
            else
            {
                handled = false;
            }
            break;
        }
        case BLE_HEARD_SIG: {
            /* A run's own, whatever its phase: one still starting keeps it
             * from the console, which would stop a stream that has an asker
             * (see console_own()). Counted only while the radio listens. */
            if ( (RADIO_V_BLE_HEARD == s_radio_kind) && window_active() )
            {
                if ( window_counting() )
                {
                    ble_heard_one(Q_EVT_CAST(BleHeardEvt));
                }
            }
            else
            {
                handled = false;
            }
            break;
        }
#endif
#if defined(CONFIG_KODEOS_WIFI) || defined(CONFIG_KODEOS_BLE)
        case CONSOLE_SLICE_SIG: {
            const window_ops_t * const ops = window_ops_of(s_radio_kind);

            if ( (NULL != ops) && window_counting() )
            {
                window_slice(ops);
            }
            else
            {
                handled = false;
            }
            break;
        }
#endif
#if CONFIG_KODEOS_WIFI
        case WIFI_STATE_SIG: {
            if ( (RADIO_V_WIFI_JOIN == s_radio_kind)
                && (Q_EVT_CAST(WifiStateEvt)->join_gen != s_join_gen) )
            {
                /* Where Wi-Fi stands while the join waits or runs, or the end
                 * of another join: it closes nothing. */
            }
            else if ( (RADIO_V_WIFI == s_radio_kind) || (RADIO_V_WIFI_JOIN == s_radio_kind) )
            {
                WifiStateEvt const * const w = Q_EVT_CAST(WifiStateEvt);
                static const char *const STATES[] = {
                    "idle", "connecting", "got_ip", "disconnected",
                };                                 /* kdrv_wifi_state_t order */
                char     safe[CONSOLE_QUOTED_MAX(sizeof(w->ssid))];
                char     ip[CONSOLE_IP4_TEXT_MAX];
                uint32_t en = 0U;

                if ( w->enabled )
                {
                    en = 1U;
                }
                /* `en` is the service flag, not the association: it says
                 * whether this board wants WiFi at all, which survives a
                 * reboot and is what a settings toggle reads back. A board
                 * enabled but on no network reports en=1 wifi=idle. */
                console_reply(">|wifi=%s ssid=%s ip=%s rssi=%" PRId32 " en=%" PRIu32,
                      console_word(STATES, sizeof(STATES) / sizeof(STATES[0]), w->state),
                      console_kv_text(safe, sizeof(safe), w->ssid),
                      console_ip4(ip, sizeof(ip), w->ip4),
                      (int32_t)w->rssi, en);
                if ( (RADIO_V_WIFI_JOIN == s_radio_kind) && w->join_dropped )
                {
                    /* The join never ran: the record is where Wi-Fi stands. */
                    console_reply_err(KCLI_ERR_REJECT, "dropped");
                }
                else
                {
                    console_reply(">ok");
                }
                console_end();
            }
            else
            {
                handled = false;
            }
            break;
        }
#endif
        default: {
            handled = false;
            break;
        }
    }
    return handled;
}

/**
 * @brief The watchdog ran out.
 *
 * For a window run (`wifi_frames`, `ble_heard`) it is the net under an
 * answer of AO_Radio that never came, or a pacer that stopped; window_timeout()
 * says what is done.
 *
 * @return false: the console says the command timed out.
 */
static bool radio_timeout(void)
{
#if defined(CONFIG_KODEOS_WIFI) || defined(CONFIG_KODEOS_BLE)
    const window_ops_t * const ops = window_ops_of(s_radio_kind);

    if ( NULL != ops )
    {
        window_timeout(ops);
    }
#endif
    return false;
}

#if CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE
/**
 * @brief Whether @p verb is one of @p verbs.
 * @param verb  The word.
 * @param verbs The list.
 * @param n     How many it holds.
 * @return true when it is.
 */
static bool radio_verb_in(const char *verb, const char *const *verbs, size_t n)
{
    bool in = false;

    for ( size_t i = 0U; n > i; ++i )
    {
        if ( strcmp(verb, verbs[i]) == 0 )
        {
            in = true;
        }
    }
    return in;
}

/**
 * @brief kverbs_t.replaces: whether a radio line takes the place of the scan
 *        that holds the console.
 *
 * AO_Radio's rule, as the console's lines name it: a newer request of a radio
 * ends the scan of that radio. `wifi_scan` gives way to a Wi-Fi line and
 * `ble_scan` to a Bluetooth one; the line's request is what ends the sweep.
 * `wifi_up` and `ble_up` are not in the lists: AO_Radio ends a scan for one
 * only when it switches the service on from off, which the console cannot
 * tell. Every other pair is left alone, and the console answers the line busy.
 *
 * @param row The new line's verb.
 * @return true when the command in flight is a scan the line ends.
 */
static bool radio_replaces(const kverb_t *row)
{
    bool replaces = false;

#if CONFIG_KODEOS_WIFI
    if ( (RADIO_V_WIFI_SCAN == s_radio_kind) && (strcmp(console_verb(), "wifi_scan") == 0) )
    {
        static const char *const WIFI[] = { "wifi_scan", "wifi_join", "wifi_down" };

        replaces = radio_verb_in(row->verb, WIFI, sizeof(WIFI) / sizeof(WIFI[0]));
    }
#endif
#if CONFIG_KODEOS_BLE
    if ( (RADIO_V_BLE_SCAN == s_radio_kind) && (strcmp(console_verb(), "ble_scan") == 0) )
    {
        static const char *const BLE[] = { "ble_scan", "ble_down" };

        replaces = radio_verb_in(row->verb, BLE, sizeof(BLE) / sizeof(BLE[0]));
    }
#endif
    return replaces;
}
#endif

#if CONFIG_KODEOS_BLE
static const karg_spec_t A_BLE_SCAN[] = {
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = KODE_BLE_SCAN_MIN_MS, .max = KODE_BLE_SCAN_MAX_MS },
};
static const karg_spec_t A_BLE_HEARD[] = {
    { .key = "ms", .required = true, .type = KARG_UINT,
      .min = RADIO_V_HEARD_MS_MIN, .max = RADIO_V_HEARD_MS_MAX },
};
/* The console has no signed number, so each direction is a key of its own. */
static const karg_spec_t A_HID_MOUSE[] = {
    { .key = "buttons", .required = false, .type = KARG_UINT,
      .min = 0U, .max = KDRV_BLE_HID_BUTTONS },
    { .key = "right",   .required = false, .type = KARG_UINT,
      .min = 0U, .max = RADIO_V_HID_STEP_MAX },
    { .key = "left",    .required = false, .type = KARG_UINT,
      .min = 0U, .max = RADIO_V_HID_STEP_MAX },
    { .key = "down",    .required = false, .type = KARG_UINT,
      .min = 0U, .max = RADIO_V_HID_STEP_MAX },
    { .key = "up",      .required = false, .type = KARG_UINT,
      .min = 0U, .max = RADIO_V_HID_STEP_MAX },
    { .key = "hold",    .required = false, .type = KARG_BOOL },
};
static const karg_spec_t A_HID_WHEEL[] = {
    { .key = "up",   .required = false, .type = KARG_UINT,
      .min = 0U, .max = RADIO_V_HID_STEP_MAX },
    { .key = "down", .required = false, .type = KARG_UINT,
      .min = 0U, .max = RADIO_V_HID_STEP_MAX },
};
static const karg_spec_t A_HID_KEY[] = {
    { .key = "code", .required = true,  .type = KARG_UINT,
      .min = 1U, .max = KDRV_BLE_HID_KEY_MAX },
    { .key = "mods", .required = false, .type = KARG_UINT,
      .min = 0U, .max = UINT8_MAX },
};
#endif

/* The nonce is checked against the identity's own rule by the handler, so a
 * bad one never reaches AO_Radio; `part` and `of` are bounded by the most
 * parts a certificate pair can take. `ident_lock` shares sd_verify's
 * console_confirm_args. */
static const karg_spec_t A_NONCE[] = {
    { .key = "nonce", .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_IDENT_CERT[] = {
    { .key = "part", .required = true, .type = KARG_UINT,
      .min = 1U, .max = KODE_ID_PARTS_MAX },
    { .key = "of",   .required = true, .type = KARG_UINT,
      .min = 1U, .max = KODE_ID_PARTS_MAX },
    { .key = "der",  .required = true, .type = KARG_STRING },
};
#if CONFIG_KODEOS_WIFI
/* `pass` is optional because open networks exist, not as a convenience. Both
 * keys are strings, so an SSID or a passphrase with spaces or symbols in it is
 * typed whole between the quotes: `wifi_join ssid="Casa 5G" pass="a b#c"`. */
static const karg_spec_t A_SSID[] = {
    { .key = "ssid", .required = true, .type = KARG_STRING },
};
static const karg_spec_t A_WIFI_FRAMES[] = {
    { .key = "ch", .required = true, .type = KARG_UINT,
      .min = 0U, .max = KWIFI_FRAMES_CHANNEL_MAX },
    { .key = "ms", .required = true, .type = KARG_UINT,
      .min = RADIO_V_FRAMES_MS_MIN, .max = RADIO_V_FRAMES_MS_MAX },
};
static const karg_spec_t A_WIFI_JOIN[] = {
    { .key = "ssid",     .required = true,  .type = KARG_STRING },
    { .key = "pass",     .required = false, .type = KARG_STRING },
    { .key = "remember", .required = false, .type = KARG_BOOL },
};
#endif

static const kverb_t RADIO_VERBS[] = {
    { "radio_up",    NULL,                 0U,                       h_radio_up    },
    { "radio_down",  NULL,                 0U,                       h_radio_down  },
    { "radio_info",  NULL,                 0U,                       h_radio_info  },
    { "c5",          console_field_args,   CONSOLE_FIELD_ARGS_N,     h_c5          },
    { "c5_help",     NULL,                 0U,                       h_c5_help     },
    { "id_sign",     A_NONCE,              KVERB_KEYS(A_NONCE),      h_id_sign     },
    { "id_cert",     NULL,                 0U,                       h_id_cert     },
    { "ident_csr",   NULL,                 0U,                       h_ident_csr   },
    { "ident_cert",  A_IDENT_CERT,         KVERB_KEYS(A_IDENT_CERT), h_ident_cert  },
    { "ident_lock",  console_confirm_args, CONSOLE_CONFIRM_ARGS_N,   h_ident_lock  },
    { "c5_wipe",     console_confirm_args, CONSOLE_CONFIRM_ARGS_N,   h_c5_wipe     },
    { "exp_power",   console_state_args,   CONSOLE_STATE_ARGS_N,     h_exp_power   },
#if CONFIG_KODEOS_WIFI
    { "wifi_up",     NULL,                 0U,                       h_wifi_up     },
    { "wifi_down",   NULL,                 0U,                       h_wifi_down   },
    { "wifi_scan",   NULL,                 0U,                       h_wifi_scan   },
    { "wifi_scan_stop", NULL,              0U,                       h_wifi_scan_stop },
    { "wifi_join",   A_WIFI_JOIN,          KVERB_KEYS(A_WIFI_JOIN),  h_wifi_join   },
    { "wifi_auto",   NULL,                 0U,                       h_wifi_auto   },
    { "wifi_list",   NULL,                 0U,                       h_wifi_list   },
    { "wifi_forget", A_SSID,               KVERB_KEYS(A_SSID),       h_wifi_forget },
    { "wifi_leave",  NULL,                 0U,                       h_wifi_leave  },
    { "wifi_info",   NULL,                 0U,                       h_wifi_info   },
    { "wifi_frames", A_WIFI_FRAMES,        KVERB_KEYS(A_WIFI_FRAMES), h_wifi_frames },
#endif
#if CONFIG_KODEOS_BLE
    { "ble_up",      NULL,                 0U,                       h_ble_up      },
    { "ble_down",    NULL,                 0U,                       h_ble_down    },
    { "ble_scan",    A_BLE_SCAN,           KVERB_KEYS(A_BLE_SCAN),   h_ble_scan    },
    { "ble_heard",   A_BLE_HEARD,          KVERB_KEYS(A_BLE_HEARD),  h_ble_heard   },
    { "ble_info",    NULL,                 0U,                       h_ble_info    },
    { "ble_unpair",  NULL,                 0U,                       h_ble_unpair  },
    { "ble_pair",    NULL,                 0U,                       h_ble_pair    },
    { "ble_hid_info",  NULL,               0U,                       h_ble_hid_info  },
    { "ble_hid_mouse", A_HID_MOUSE,        KVERB_KEYS(A_HID_MOUSE),  h_ble_hid_mouse },
    { "ble_hid_wheel", A_HID_WHEEL,        KVERB_KEYS(A_HID_WHEEL),  h_ble_hid_wheel },
    { "ble_hid_key",   A_HID_KEY,          KVERB_KEYS(A_HID_KEY),    h_ble_hid_key   },
#endif
};

const kverbs_t radio_verbs = {
    .verbs  = RADIO_VERBS, .n = sizeof(RADIO_VERBS) / sizeof(RADIO_VERBS[0]),
    .answer  = radio_answer,
    .timeout = radio_timeout,
    .info    = radio_info,
#if CONFIG_KODEOS_WIFI
    .nowait  = "wifi_scan_stop",
#endif
#if CONFIG_KODEOS_WIFI || CONFIG_KODEOS_BLE
    .replaces = radio_replaces,
#endif
};
