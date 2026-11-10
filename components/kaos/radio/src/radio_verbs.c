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
#include "kdrv_ble.h"
#endif
#include "kqp.h"

#include "console_verbs.h"
#include "kverbs.h"

/** @brief Which radio verb holds the console. */
typedef enum {
    RADIO_V_NONE = 0,    /**< None of them, or a field verb.               */
    RADIO_V_LINK,        /**< radio_info: closed by RADIO_STATE_SIG.        */
    RADIO_V_ID_SIGN,     /**< id_sign: closed by ID_SIGN_RSP_SIG.           */
    RADIO_V_ID,          /**< id_cert, ident_csr, ident_cert, ident_lock:
                          *   ID_PART_SIG records until ID_END_SIG.         */
    RADIO_V_WIPE,        /**< c5_wipe: closed by C5_WIPE_RSP_SIG.           */
    RADIO_V_WIFI,        /**< wifi_up/down/join/leave/info/auto: closed by
                          *   WIFI_STATE_SIG. Several hand the watchdog
                          *   over: association plus DHCP outlasts it, and
                          *   AO_Radio answers either way.                  */
    RADIO_V_WIFI_SCAN,   /**< wifi_scan: entries until WIFI_SCAN_END_SIG.   */
    RADIO_V_WIFI_SAVED,  /**< wifi_list: entries until WIFI_SAVED_END_SIG.  */
    RADIO_V_FRAMES,      /**< wifi_frames: its window, then its record; the
                          *   watchdog is the window's end.                 */
    RADIO_V_BLE,         /**< ble_*: closed by BLE_STATE_SIG.               */
    RADIO_V_BLE_SCAN     /**< ble_scan: entries until BLE_SCAN_END_SIG.     */
} radio_verb_kind_t;

static radio_verb_kind_t s_radio_kind;

#define RADIO_V_C5_TEXT_MAX   32U  /**< Room for the C5 image's project, version or IDF name. */
#define RADIO_V_SVC_TEXT_MAX  64U  /**< Room for `info`'s KODE-services words. */
#define RADIO_V_B64_IN    3U  /**< Bytes base64 takes at a time... */
#define RADIO_V_B64_OUT   4U  /**< ...and the characters it writes for them. */
/** @brief Room for @p n bytes as base64, padding and terminator included. */
#define RADIO_V_B64_TEXT(n)  ((((((size_t)(n) + RADIO_V_B64_IN) - 1U) / RADIO_V_B64_IN) * RADIO_V_B64_OUT) + 1U)
#define RADIO_V_WIPE_MS     12000U /**< c5_wipe: three steps, each with AO_Radio's own 3 s, and margin. */
#define RADIO_V_FRAMES_MS_MIN   100U    /**< wifi_frames: the shortest window, one period of the C5's batches. */
#define RADIO_V_FRAMES_MS_MAX   10000U  /**< wifi_frames: the longest window.                                 */
#define RADIO_V_FRAMES_SLICE_MS 10U     /**< wifi_frames: the console pacer's slice, which the stop's retries count. */
#define RADIO_V_FRAMES_STOP_MS  1000U   /**< wifi_frames: how long a stop with no room in AO_Radio's queue is asked again. */
#define RADIO_V_FRAMES_WAIT_MS  5000U   /**< wifi_frames: the longest wait for AO_Radio's answer to a start or a stop. Above AO_Radio's own 3 s
                                         *   deadline for a KODE request, so its answer wins and this is only the net. */
#define RADIO_V_FRAMES_LATE_MS  (KODE_RADIO_SVC_SEND_GUARD_MS + RADIO_V_FRAMES_WAIT_MS) /**< wifi_frames: how long after the net caught a run an answer of
                                         *   its requests may still arrive: AO_Radio's send guard, then its deadline. */

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

/** @brief Ask the owner about a coprocessor field. */
static void c5_field_ask(uint8_t op, uint16_t field, int32_t value)
{
    c5_field_request(AO_Console, op, field, value);
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
    radio_down_request();
    console_reply(">ok");
}

/** @brief exp_power state=on|off: switch J3's +3.3V_EXT. The pin is the
 *         C5's, so the outcome is AO_Radio's; `c5 field="pwr_ext"` reads
 *         it back while the link is up. */
static void h_exp_power(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    exp_power_request(v[0].b);
    console_reply(">ok");
}

/** @brief radio_info: closed by RADIO_STATE_SIG - always answerable, every
 *         state publishes on request. */
static void h_radio_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_LINK;
    radio_state_request();
}

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
    console_unwatched();
    wifi_scan_request();
}

/** @brief wifi_join ssid="<s>" [pass="<p>"] [remember=on|off]: closed by
 *         WIFI_STATE_SIG. remember=off is the factory's own network, which
 *         must not travel to the customer in the vault. */
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
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    console_unwatched();   /* association + DHCP outlast 5 s */
    /* With the watchdog disarmed, only WIFI_STATE_SIG closes the command,
     * and a request the pool dropped will never have one. */
    if ( !wifi_join_request(v[0].s, pass, remember) )
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
    wifi_start_request();
}

/** @brief wifi_down: disable the WiFi service; stops the station outright.
 *  Closed by WIFI_STATE_SIG. */
static void h_wifi_down(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    console_unwatched();   /* the disconnect/stop/deinit chain */
    wifi_stop_request();
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
    wifi_list_request();
}

/** @brief wifi_forget ssid="<s>": drop one from the store. Does not disconnect. */
static void h_wifi_forget(const kverb_t *c, const karg_val_t *v)
{
    (void)c;
    wifi_forget_request(v[0].s);
    console_reply(">ok");
}

/** @brief wifi_info: closed by WIFI_STATE_SIG. */
static void h_wifi_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_WIFI;
    wifi_state_request();
}

/** @brief Where a `wifi_frames` run stands. */
/* cppcheck-suppress misra-c2012-2.4 ; the type names the values of a uint8_t field, which keeps the run small */
typedef enum {
    FRAMES_IDLE = 0,   /**< No run.                                         */
    FRAMES_STARTING,   /**< The start is out; AO_Radio's answer is awaited. */
    FRAMES_LISTENING,  /**< The C5 listens; the window runs.                */
    FRAMES_STOPPING    /**< The stop is wanted or out; its answer is awaited. */
/* cppcheck-suppress misra-c2012-2.3 ; the type names the values of a uint8_t field, which keeps the run small */
} frames_phase_t;

/** @brief What `wifi_frames` has counted in its window. */
static struct {
    uint8_t  phase;      /**< frames_phase_t.                               */
    uint32_t ms;         /**< The window.                                   */
    uint32_t start_ms;   /**< When it began, on khal_time_ms()'s clock.     */
    uint32_t tries;      /**< Slices spent asking for a stop that had no room. */
    uint8_t  channel;    /**< The radio's channel, as the last answer said. */
    bool     joined;     /**< The station holds an association.             */
    bool     stop_out;   /**< The stop was posted.                          */
    uint32_t heard;      /**< Frames heard since the start: the last tick's. */
    uint32_t handed;     /**< Frames AO_Radio handed over.                  */
    uint32_t dropped;    /**< Frames lost: the last tick's.                 */
    uint32_t mgmt;       /**< Management frames, summed over the ticks.     */
    uint32_t data;       /**< Data frames, summed over the ticks.           */
    int8_t   rssi_max;   /**< The strongest tick's level, dBm.              */
    bool     rssi_seen;  /**< A tick had a level.                           */
} s_frames_run;

static bool     s_frames_late;      /**< A run was ended by the watchdog: an answer of its requests may still come. */
static uint32_t s_frames_late_ms;   /**< When, on khal_time_ms()'s clock.                                          */

/**
 * @brief wifi_frames ch=<0..177> ms=<100..10000>: Wi-Fi frames heard on a
 *        channel: counts.
 *
 * Starts listening on @p ch (0 stays where the radio is), lets the window
 * pass, stops, and prints one record of counts and levels, then `>ok`. An
 * address or a payload is never printed.
 *
 * The command holds the console until AO_Radio has answered the stop, so no
 * answer of one run can reach the next one, and the C5 is never left
 * listening for a console that has gone: AO_Radio answers a start and a stop,
 * its own deadline standing behind each, and the watchdog is the net under
 * that (RADIO_V_FRAMES_WAIT_MS); the window is counted in the console's pacer
 * slices (console_pace()). A run the net had to end leaves answers that may
 * still arrive, so the next run waits RADIO_V_FRAMES_LATE_MS.
 */
static void h_wifi_frames(const kverb_t *c, const karg_val_t *v)
{
    if ( s_frames_late && (RADIO_V_FRAMES_LATE_MS > (khal_time_ms() - s_frames_late_ms)) )
    {
        /* A run the watchdog ended may still have an answer on its way, and
         * nothing tells it from the answer of this one's start: wait it out. */
        console_reply_err(KCLI_ERR_BUSY, "busy");
        return;
    }
    s_frames_late = false;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_FRAMES;
    (void)memset(&s_frames_run, 0, sizeof(s_frames_run));
    s_frames_run.ms    = v[1].u;
    s_frames_run.phase = (uint8_t)FRAMES_STARTING;
    console_extend(RADIO_V_FRAMES_WAIT_MS);
    if ( !wifi_frames_request(AO_Console, true, (uint8_t)v[0].u) )
    {
        /* Nothing started, so nothing is left to stop. */
        s_frames_run.phase = (uint8_t)FRAMES_IDLE;
        console_reply_err(KCLI_ERR_REJECT, c->verb);
        console_end();
    }
}

/**
 * @brief End the command with the reason AO_Radio gave.
 * @param why kwifi_frames_why_t.
 */
static void frames_fail(uint8_t why)
{
    static const char *const WORDS[] = {
        "ok", "no_link", "no_service", "wifi_off", "joined", "channel", "busy", "memory", "resting",
    };                                           /* kwifi_frames_why_t order */
    _Static_assert((sizeof(WORDS) / sizeof(WORDS[0])) == ((size_t)KWIFI_FRAMES_RESTING + 1U),
                   "one word per reason");

    s_frames_run.phase = (uint8_t)FRAMES_IDLE;
    if ( (uint8_t)KWIFI_FRAMES_BUSY == why )
    {
        console_reply_err(KCLI_ERR_BUSY, WORDS[why]);
    }
    else if ( ((uint8_t)KWIFI_FRAMES_OK < why) && ((uint8_t)KWIFI_FRAMES_RESTING >= why) )
    {
        console_reply_err(KCLI_ERR_REJECT, WORDS[why]);
    }
    else
    {
        console_reply_err(KCLI_ERR_REJECT, console_verb());
    }
    console_end();
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

/** @brief Whether a run has the C5 listening, or a stop on its way: ticks and frames count, slices run. */
static bool frames_hearing(void)
{
    return ((uint8_t)FRAMES_LISTENING == s_frames_run.phase)
        || ((uint8_t)FRAMES_STOPPING == s_frames_run.phase);
}

/**
 * @brief AO_Radio's answer to the start or the stop of `wifi_frames`.
 * @param a The answer.
 */
static void frames_state(WifiFramesStateEvt const * const a)
{
    if ( (uint8_t)FRAMES_STARTING == s_frames_run.phase )
    {
        if ( a->on )
        {
            s_frames_run.phase   = (uint8_t)FRAMES_LISTENING;
            s_frames_run.channel = a->channel;
            s_frames_run.joined  = a->joined;
            s_frames_run.start_ms = khal_time_ms();   /* the window runs from here */
            console_pace();
        }
        else if ( (uint8_t)KWIFI_FRAMES_OK != a->why )
        {
            frames_fail(a->why);
        }
        else
        {
            /* A refusal is never OK: this is the answer of an earlier run's
             * stop, which came late. The start's own answer is still to come. */
        }
    }
    else if ( (uint8_t)FRAMES_LISTENING == s_frames_run.phase )
    {
        /* The link went in the middle of the window, or the radio left
         * the stream: AO_Radio has freed its door, nothing is left on. */
        if ( !a->on )
        {
            frames_fail(a->why);
        }
    }
    else if ( ((uint8_t)FRAMES_STOPPING == s_frames_run.phase) && s_frames_run.stop_out && !a->on )
    {
        /* The stop's answer, or its expiry. */
        s_frames_run.phase = (uint8_t)FRAMES_IDLE;
        frames_record();
        if ( (uint8_t)KWIFI_FRAMES_OK == a->why )
        {
            console_reply(">ok");
            console_end();
        }
        else
        {
            /* The stop expired: the C5 did not confirm, and a batch it
             * still sends with nobody asking will quiet it. */
            frames_fail(a->why);
        }
    }
    else
    {
        /* Nothing waits for it. */
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

/**
 * @brief A slice of the console's pacer: the window may have run out.
 *
 * At its end the stop is asked for. With no room in AO_Radio's queue it is
 * asked again every slice for RADIO_V_FRAMES_STOP_MS; then the command ends
 * with its record and `>err code=5 msg=stop`: the C5 may still be listening,
 * AO_Radio still counts the console as the asker, and the next `wifi_frames`
 * (the same asker) restarts and stops it; a batch heard with nobody asking
 * would quiet it too.
 */
static void frames_slice(void)
{
    bool const over = s_frames_run.ms <= (khal_time_ms() - s_frames_run.start_ms);

    if ( !over && ((uint8_t)FRAMES_LISTENING == s_frames_run.phase) )
    {
        console_pace();
    }
    else if ( (uint8_t)FRAMES_LISTENING == s_frames_run.phase )
    {
        s_frames_run.phase = (uint8_t)FRAMES_STOPPING;
    }
    else
    {
        /* Already stopping: only a stop that had no room comes back here. */
    }
    if ( ((uint8_t)FRAMES_STOPPING == s_frames_run.phase) && !s_frames_run.stop_out )
    {
        if ( wifi_frames_request(AO_Console, false, 0U) )
        {
            s_frames_run.stop_out = true;
            console_extend(RADIO_V_FRAMES_WAIT_MS);   /* its answer, or the net */
        }
        else if ( (s_frames_run.tries * RADIO_V_FRAMES_SLICE_MS) < RADIO_V_FRAMES_STOP_MS )
        {
            s_frames_run.tries += 1U;
            console_pace();
        }
        else
        {
            s_frames_run.phase = (uint8_t)FRAMES_IDLE;
            frames_record();
            console_reply_err(KCLI_ERR_REJECT, "stop");
            console_end();
        }
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
    ble_start_request();
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
    console_unwatched();
    /* With the watchdog disarmed, only the end marker closes the command,
     * and a request the pool dropped will never have one. */
    if ( !ble_scan_request(ms) )
    {
        console_reply_err(KCLI_ERR_REJECT, c->verb);
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
    ble_stop_request();
}

/** @brief ble_info: closed by BLE_STATE_SIG. */
static void h_ble_info(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_BLE;
    ble_state_request();
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
    ble_unpair_request();
}

/** @brief ble_pair: show the board for five minutes, so a new device can pair. */
static void h_ble_pair(const kverb_t *c, const karg_val_t *v)
{
    (void)v;
    console_begin(c->verb);
    s_radio_kind = RADIO_V_BLE;
    ble_pair_open_request();
}
#endif /* CONFIG_KODEOS_BLE */

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
            if ( RADIO_V_WIFI_SCAN == s_radio_kind )
            {
                WifiScanEntryEvt const * const n = Q_EVT_CAST(WifiScanEntryEvt);
                /* Quoted: an SSID may legitimately contain a space, a quote
                 * or any other byte, and comes back exactly as it is. */
                char safe[CONSOLE_QUOTED_MAX(sizeof(n->ssid))];
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
            if ( RADIO_V_WIFI_SCAN == s_radio_kind )
            {
                WifiScanEndEvt const * const end = Q_EVT_CAST(WifiScanEndEvt);
                if ( end->ok )
                {
                    console_reply(">ok");
                }
                else if ( end->busy )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
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
            if ( (RADIO_V_FRAMES == s_radio_kind) && frames_hearing() )
            {
                frames_tick(Q_EVT_CAST(WifiFramesTickEvt));
            }
            else
            {
                handled = false;
            }
            break;
        }
        case WIFI_FRAME_SIG: {
            if ( (RADIO_V_FRAMES == s_radio_kind) && frames_hearing() )
            {
                s_frames_run.handed += 1U;
            }
            else
            {
                handled = false;
            }
            break;
        }
        case CONSOLE_SLICE_SIG: {
            if ( (RADIO_V_FRAMES == s_radio_kind) && frames_hearing() )
            {
                frames_slice();
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
#endif
#if CONFIG_KODEOS_BLE
        case BLE_SCAN_ENTRY_SIG: {
            if ( RADIO_V_BLE_SCAN == s_radio_kind )
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
            if ( RADIO_V_BLE_SCAN == s_radio_kind )
            {
                WifiScanEndEvt const * const end = Q_EVT_CAST(WifiScanEndEvt);
                if ( end->ok )
                {
                    console_reply(">ok");
                }
                else if ( end->busy )
                {
                    console_reply_err(KCLI_ERR_BUSY, "busy");
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
            if ( RADIO_V_BLE == s_radio_kind )
            {
                BleStateEvt const * const b = Q_EVT_CAST(BleStateEvt);
                static const char *const STATES[] = {
                    "off", "idle", "advertising", "connected",
                };                                  /* kdrv_ble_state_t order */
                const char * const state =
                    console_word(STATES, sizeof(STATES) / sizeof(STATES[0]), b->state);
                /* `idle` is a gap, not a resting place: BLE on means
                 * advertising, and this reports the instant between the
                 * stack coming up and the advert reaching the worker. */
                if ( b->addr_known )
                {
                    console_reply(">|ble=%s addr=%02x:%02x:%02x:%02x:%02x:%02x "
                          "console=%s tx_drop=%" PRIu32 " bonds=%" PRIu32 " backlog=%" PRIu32 " retry=%" PRIu32
                          " hidden=%s pair=%s",
                          state,
                          b->addr[0], b->addr[1], b->addr[2],
                          b->addr[3], b->addr[4], b->addr[5],
                          console_on_off(b->console),
                          (uint32_t)b->tx_drop, (uint32_t)b->bonds,
                          (uint32_t)b->backlog, (uint32_t)b->retry,
                          console_on_off(b->hidden), console_on_off(b->pair_open));
                }
                else
                {
                    console_reply(">|ble=%s addr=unknown console=%s tx_drop=%" PRIu32 " "
                          "bonds=%" PRIu32 " backlog=%" PRIu32 " retry=%" PRIu32 " hidden=%s pair=%s",
                          state, console_on_off(b->console),
                          (uint32_t)b->tx_drop, (uint32_t)b->bonds,
                          (uint32_t)b->backlog, (uint32_t)b->retry,
                          console_on_off(b->hidden), console_on_off(b->pair_open));
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
#endif
#if CONFIG_KODEOS_WIFI
        case WIFI_STATE_SIG: {
            if ( RADIO_V_WIFI == s_radio_kind )
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
                console_reply(">ok");
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
 * For `wifi_frames` it is the net under an answer of AO_Radio that never
 * came, or a pacer that stopped. The stop is asked for unless one is out
 * already (behind a start still waiting, AO_Radio holds it until the start
 * is answered), the record is printed when there is one, and the console's
 * own timeout error is the one terminal line. The answers of the requests
 * left out are not told from the next run's, so h_wifi_frames() refuses the
 * next run until they can no longer come.
 *
 * @return false: the console says the command timed out.
 */
static bool radio_timeout(void)
{
#if CONFIG_KODEOS_WIFI
    if ( (RADIO_V_FRAMES == s_radio_kind) && ((uint8_t)FRAMES_IDLE != s_frames_run.phase) )
    {
        if ( !s_frames_run.stop_out )
        {
            (void)wifi_frames_request(AO_Console, false, 0U);
        }
        if ( (uint8_t)FRAMES_STARTING != s_frames_run.phase )
        {
            frames_record();
        }
        s_frames_run.phase = (uint8_t)FRAMES_IDLE;
        s_frames_late      = true;
        s_frames_late_ms   = khal_time_ms();
    }
#endif
    return false;
}

#if CONFIG_KODEOS_BLE
static const karg_spec_t A_BLE_SCAN[] = {
    { .key = "ms", .required = false, .type = KARG_UINT,
      .min = KODE_BLE_SCAN_MIN_MS, .max = KODE_BLE_SCAN_MAX_MS },
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
    { "ble_info",    NULL,                 0U,                       h_ble_info    },
    { "ble_unpair",  NULL,                 0U,                       h_ble_unpair  },
    { "ble_pair",    NULL,                 0U,                       h_ble_pair    },
#endif
};

const kverbs_t radio_verbs = {
    .verbs  = RADIO_VERBS, .n = sizeof(RADIO_VERBS) / sizeof(RADIO_VERBS[0]),
    .answer  = radio_answer,
    .timeout = radio_timeout,
    .info    = radio_info,
};
