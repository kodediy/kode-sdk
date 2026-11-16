/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file appmgr_outcome.c
 * @brief The record of the last app install and the last kodeOS update: kept and told.
 *
 * The block lives in PSRAM, not in AO_AppMgr, whose static object is held
 * under 2 KB of internal RAM. It is AO_AppMgr's alone: every function runs in
 * its thread.
 *
 * AO_AppMgr hears of a port only while a record waits on it: it subscribes
 * then, asks the port's owner where it stands (except at its own start, when
 * the owners have no queue yet: they say so when the port changes), and lets
 * go once nothing waits. The rest of the time those signals never reach its
 * queue.
 *
 * `told` means delivered. On the cable it is set as the line is written. On
 * the Kode channel and the BLE console the final record goes as a confirmed
 * line (kode_log_reply_confirm()), one at a time per port, and `told` waits
 * for CONSOLE_SENT_SIG: the broker's PUBACK, or the BLE stack's answer to a
 * request sent after the notification. A record not delivered stays owed and
 * goes out again when its port comes up.
 *
 * A record that ends in a jump (`run=on`, or a kodeOS switch) cannot wait for
 * that answer: nothing delays the jump. Its line is handed to the port as a
 * plain one and the record stays owed; AO_Boot's leave lets AO_Log write it
 * into the port's ring before the radios go down. Back in kodeOS, an owed
 * `installed` goes out again when its port comes up, and a `rebooting` gives
 * way to the verdict.
 */
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"
#include "kaos.h"
#include "kdrv_apps.h"
#include "kdrv_kfw.h"
#include "kdrv_slot.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "kode_log.h"
#include "kqp.h"

#if CONFIG_KODEOS_CONSOLE
#include "console_grammar.h"
#include "console_verbs.h"
#endif

#include "appmgr_outcome.h"

#define TAG "kaos.appmgr"  /**< AO_AppMgr's tag. */

/** @brief The block both records and the install in hand live in. */
typedef struct {
    appmgr_inst_rec_t rec[APPMGR_INST_KINDS];  /**< The last of each kind.            */
    uint8_t           pending;                 /**< Kind a fresh request noted, or
                                                *   APPMGR_INST_KINDS for none.      */
    uint8_t           pending_src;             /**< Its asker's port.                 */
    uint8_t           open;                    /**< Kind taken and not ended, or
                                                *   APPMGR_INST_KINDS for none.      */
    uint8_t           why;                     /**< The cause noted for the open one. */
    bool              live;                    /**< The open one's command is still in
                                                *   flight: its end answers it.      */
    bool              watch_net;               /**< Subscribed to the channel.        */
    bool              watch_ble;               /**< Subscribed to the BLE half.       */
    bool              watch_sent;              /**< Subscribed to CONSOLE_SENT_SIG.   */
    bool              chan_up;                 /**< The Kode channel is up.           */
    bool              ble_up;                  /**< A BLE console is subscribed.      */
    bool              sent[APPMGR_INST_KINDS]; /**< Its confirmed line is out and its
                                                *   answer awaited.                 */
    QActive          *ao;                      /**< AO_AppMgr, the subscriber.        */
} appmgr_inst_t;

static appmgr_inst_t *s_inst;   /**< The block; NULL when PSRAM refused it. */

/** @brief The `state=` word of each state. */
static const char * const INST_STATE_WORD[APPMGR_INST_STATES] = {
    "none", "running", "installed", "failed", "rebooting", "confirmed",
    "rolled_back",
};

/** @brief The NVS key of each kind. */
static const char * const INST_KEY[APPMGR_INST_KINDS] = {
    APPMGR_INST_KEY_APP, APPMGR_INST_KEY_OS,
};

/**
 * @brief Keep one record in NVS. A refusal is logged; the record in RAM stands.
 * @param kind APPMGR_INST_APP or APPMGR_INST_OS.
 */
static void inst_store(uint8_t kind)
{
    if ( khal_nvs_set_blob(APPMGR_UNDO_NS, INST_KEY[kind], &s_inst->rec[kind],
                           sizeof(appmgr_inst_rec_t)) != KODE_OK )
    {
        KLOG_W(TAG, "cannot keep the record of install %lu",
               (unsigned long)s_inst->rec[kind].id);
    }
}

#if CONFIG_KODEOS_CONSOLE
/**
 * @brief Write one record as its `>|install` line.
 * @param buf  Where.
 * @param cap  Its size.
 * @param kind APPMGR_INST_APP or APPMGR_INST_OS.
 * @param pct  The percent to show.
 */
static void inst_line(char *buf, size_t cap, uint8_t kind, uint32_t pct)
{
    /* The `dst=` word of each kind. */
    static const char * const KIND_WORD[APPMGR_INST_KINDS] = { "app", "os" };
    /* The `why=` word of each cause; none is `-`. */
    static const char * const WHY_WORD[APPMGR_WHY_COUNT] = {
        "-", "missing", "cut", "corrupt", "write", "no_room", "wrong_board",
        "busy", "keep", "cancelled", "stalled", "reset", "switch", "unconfirmed",
        "crashed", "other",
    };
    appmgr_inst_rec_t const * const r = &s_inst->rec[kind];
    char name[CONSOLE_QUOTED_MAX(APPMGR_INST_NAME_MAX)];
    char ver[CONSOLE_QUOTED_MAX(APPMGR_INST_VER_MAX)];
    uint8_t state = r->state;
    uint8_t why   = r->why;

    if ( (uint8_t)APPMGR_INST_STATES <= state )
    {
        state = (uint8_t)APPMGR_INST_NONE;
    }
    if ( (uint8_t)APPMGR_WHY_COUNT <= why )
    {
        why = (uint8_t)APPMGR_WHY_OTHER;
    }
    (void)snprintf(buf, cap,
                   ">|install id=%lu dst=%s state=%s pct=%lu name=%s "
                   "version=%s why=%s",
                   (unsigned long)r->id, KIND_WORD[kind],
                   INST_STATE_WORD[state], (unsigned long)pct,
                   console_kv_text(name, sizeof(name), r->name),
                   console_kv_text(ver, sizeof(ver), r->version),
                   WHY_WORD[why]);
}

/**
 * @brief Put one record on a port, the way the console answers a command from it.
 * @param kind    APPMGR_INST_APP or APPMGR_INST_OS.
 * @param src     KODE_CONSOLE_SRC_*, or KODE_LOG_SRC_NONE for nobody.
 * @param pct     The percent to show.
 * @param confirm Ask the port to answer CONSOLE_SENT_SIG once it is delivered.
 */
static void inst_say(uint8_t kind, uint8_t src, uint32_t pct, bool confirm)
{
    if ( (uint8_t)KODE_LOG_SRC_NONE != src )
    {
        char line[KODE_LOG_LINE_MAX];

        inst_line(line, sizeof(line), kind, pct);
        if ( (uint8_t)KODE_CONSOLE_SRC_SCHED == src )
        {
            KLOG_N(TAG, "sched: %s", line);
        }
        else if ( confirm )
        {
            kode_log_reply_confirm(line, src);
        }
        else
        {
            kode_log_reply_src(line, src);
        }
    }
}
#endif

/**
 * @brief Whether a record's port delivers only when it is up and says so.
 * @param src KODE_CONSOLE_SRC_*, or KODE_LOG_SRC_NONE.
 * @return true for the Kode channel and the BLE console.
 */
static bool inst_remote(uint8_t src)
{
    return ((uint8_t)KODE_CONSOLE_SRC_NET == src)
           || ((uint8_t)KODE_CONSOLE_SRC_BLE == src);
}

/**
 * @brief Whether a confirmed line is out on @p src and its answer awaited.
 * @param src KODE_CONSOLE_SRC_*.
 * @return true while one is.
 */
static bool inst_port_busy(uint8_t src)
{
    bool busy = false;

    for ( uint8_t k = 0U; (uint8_t)APPMGR_INST_KINDS > k; ++k )
    {
        if ( s_inst->sent[k] && (src == s_inst->rec[k].src) )
        {
            busy = true;
        }
    }
    return busy;
}

/**
 * @brief Whether a record's port can take it now.
 * @param src KODE_CONSOLE_SRC_*, or KODE_LOG_SRC_NONE.
 * @return false while the Kode channel or the BLE console it came on is down,
 *         or not yet known to be up.
 */
static bool inst_port_up(uint8_t src)
{
    bool up = true;

    if ( (uint8_t)KODE_CONSOLE_SRC_NET == src )
    {
        up = s_inst->chan_up;
    }
    else if ( (uint8_t)KODE_CONSOLE_SRC_BLE == src )
    {
        up = s_inst->ble_up;
    }
    else
    {
        /* The cable, the scheduler and nobody are always there. */
    }
    return up;
}

/**
 * @brief Subscribe to what the records wait on, and to nothing else.
 *
 * A port while an install from it is open or a record for it is owed, and
 * CONSOLE_SENT_SIG while any port is. A port newly watched is asked where it
 * stands, since it says so only when it changes.
 *
 * @param ask Ask the owners; false at AO_AppMgr's start, which comes before
 *            AO_Radio's and AO_Net's.
 */
static void inst_watch(bool ask)
{
    bool want_net = false;
    bool want_ble = false;

    for ( uint8_t k = 0U; (uint8_t)APPMGR_INST_KINDS > k; ++k )
    {
        appmgr_inst_rec_t const * const r = &s_inst->rec[k];
        bool const owed = (k == s_inst->open)
                          || ((0U == r->told) && ((uint8_t)APPMGR_INST_NONE != r->state));

        if ( owed && ((uint8_t)KODE_CONSOLE_SRC_NET == r->src) )
        {
            want_net = true;
        }
        if ( owed && ((uint8_t)KODE_CONSOLE_SRC_BLE == r->src) )
        {
            want_ble = true;
        }
    }

#if CONFIG_KODEOS_CHANNEL
    if ( want_net != s_inst->watch_net )
    {
        s_inst->watch_net = want_net;
        s_inst->chan_up   = false;
        if ( want_net )
        {
            QActive_subscribe(s_inst->ao, CHANNEL_STATE_SIG);
            if ( ask )
            {
                (void)channel_state_request();
            }
        }
        else
        {
            QActive_unsubscribe(s_inst->ao, CHANNEL_STATE_SIG);
        }
    }
#else
    (void)want_net;
#endif
#if CONFIG_KODEOS_BLE_CONSOLE
    if ( want_ble != s_inst->watch_ble )
    {
        s_inst->watch_ble = want_ble;
        s_inst->ble_up    = false;
        if ( want_ble )
        {
            QActive_subscribe(s_inst->ao, BLE_STATE_SIG);
            if ( ask )
            {
                ble_state_request();
            }
        }
        else
        {
            QActive_unsubscribe(s_inst->ao, BLE_STATE_SIG);
        }
    }
#else
    (void)want_ble;
#endif
    if ( (want_net || want_ble) != s_inst->watch_sent )
    {
        s_inst->watch_sent = want_net || want_ble;
        if ( s_inst->watch_sent )
        {
            QActive_subscribe(s_inst->ao, CONSOLE_SENT_SIG);
        }
        else
        {
            QActive_unsubscribe(s_inst->ao, CONSOLE_SENT_SIG);
        }
    }
#if !CONFIG_KODEOS_CHANNEL && !CONFIG_KODEOS_BLE_CONSOLE
    (void)ask;
#endif
}

/**
 * @brief Tell a record's port its state, keep it, and watch what it still
 *        waits on.
 *
 * The cable and the scheduler are told as the line is written. On the
 * channel or BLE, up: a running record goes plain, a final one confirmed,
 * unless another confirmed line is out there, in which case it waits for that
 * one's answer. Not known to be up: the command in flight still gets its line,
 * and the record stays owed. Before a jump a final one goes plain and stays
 * owed: no answer could arrive before the reset.
 *
 * @param kind  APPMGR_INST_APP or APPMGR_INST_OS.
 * @param live  The command it answers is in flight.
 * @param jumps The board resets once the line is handed over.
 */
static void inst_tell(uint8_t kind, bool live, bool jumps)
{
    appmgr_inst_rec_t * const r = &s_inst->rec[kind];
    bool const final = ((uint8_t)APPMGR_INST_RUNNING != r->state);
    bool       say   = false;
    bool       conf  = false;

    r->told = 0U;
    s_inst->sent[kind] = false;
    if ( !inst_remote(r->src) )
    {
        say     = true;
        r->told = 1U;
    }
    else if ( jumps )
    {
        say = live || inst_port_up(r->src);
    }
    else if ( inst_port_up(r->src) )
    {
        if ( !final )
        {
            say = true;
        }
        else if ( !inst_port_busy(r->src) )
        {
            say  = true;
            conf = true;
            s_inst->sent[kind] = true;
        }
        else
        {
            /* The line out answers first; this one follows it. */
        }
    }
    else
    {
        say = live;
    }
#if CONFIG_KODEOS_CONSOLE
    if ( say )
    {
        inst_say(kind, r->src, r->pct, conf);
    }
#else
    (void)say;
    (void)conf;
#endif
    inst_store(kind);
    inst_watch(true);
}

/**
 * @brief Tell every record still owed to @p src.
 * @param src KODE_CONSOLE_SRC_* of a port that came up.
 */
static void inst_flush(uint8_t src)
{
    for ( uint8_t k = 0U; (uint8_t)APPMGR_INST_KINDS > k; ++k )
    {
        appmgr_inst_rec_t const * const r = &s_inst->rec[k];

        if ( (src == r->src) && (0U == r->told) && (k != s_inst->open)
            && (!s_inst->sent[k]) && ((uint8_t)APPMGR_INST_NONE != r->state) )
        {
            inst_tell(k, false, false);
        }
    }
}

/**
 * @brief A port answered its confirmed line, or went down before it could.
 *
 * Delivered: the records out on it are told and kept, and the next one owed
 * there goes. Not delivered: they stay owed until the port comes up again.
 *
 * @param src KODE_CONSOLE_SRC_* of the port.
 * @param ok  Whether the line was delivered.
 */
static void inst_answered(uint8_t src, bool ok)
{
    bool told = false;

    for ( uint8_t k = 0U; (uint8_t)APPMGR_INST_KINDS > k; ++k )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[k];

        if ( s_inst->sent[k] && (src == r->src) )
        {
            s_inst->sent[k] = false;
            if ( ok )
            {
                r->told = 1U;
                inst_store(k);
                told = true;
            }
        }
    }
    if ( told && inst_port_up(src) )
    {
        inst_flush(src);
    }
    inst_watch(true);
}

/**
 * @brief Close the install in hand with @p state, then tell and keep it.
 * @param state appmgr_inst_state_t.
 * @param jumps The board resets once the line is handed over.
 */
static void inst_finish(appmgr_inst_state_t state, bool jumps)
{
    uint8_t const kind = s_inst->open;
    bool const    live = s_inst->live;

    s_inst->open = (uint8_t)APPMGR_INST_KINDS;
    s_inst->live = false;
    s_inst->rec[kind].state = (uint8_t)state;
    inst_tell(kind, live, jumps);
}

void appmgr_inst_init(QActive *ao)
{
    if ( NULL == s_inst )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() hands back an untyped block aligned for any type */
        s_inst = (appmgr_inst_t *)khal_alloc_psram(sizeof(uint32_t),
                                                   sizeof(appmgr_inst_t));
    }
    if ( NULL == s_inst )
    {
        KLOG_W(TAG, "no PSRAM for the install records; installs are not "
                    "numbered or told");
        return;
    }
    (void)memset(s_inst, 0, sizeof(appmgr_inst_t));
    s_inst->ao      = ao;
    s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
    s_inst->open    = (uint8_t)APPMGR_INST_KINDS;
    /* Started over: whatever an earlier start subscribed to is let go, and
     * inst_watch() below takes what these records wait on. */
#if CONFIG_KODEOS_CHANNEL
    QActive_unsubscribe(ao, CHANNEL_STATE_SIG);
#endif
#if CONFIG_KODEOS_BLE_CONSOLE
    QActive_unsubscribe(ao, BLE_STATE_SIG);
#endif
    QActive_unsubscribe(ao, CONSOLE_SENT_SIG);

    for ( uint8_t k = 0U; (uint8_t)APPMGR_INST_KINDS > k; ++k )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[k];
        size_t len = sizeof(appmgr_inst_rec_t);

        if ( (khal_nvs_get_blob(APPMGR_UNDO_NS, INST_KEY[k], r, &len) != KODE_OK)
            || (sizeof(appmgr_inst_rec_t) != len)
            || ((uint8_t)APPMGR_INST_LAYOUT != r->layout) )
        {
            (void)memset(r, 0, sizeof(appmgr_inst_rec_t));
        }
        r->name[APPMGR_INST_NAME_MAX - 1U]   = '\0';
        r->version[APPMGR_INST_VER_MAX - 1U] = '\0';
        if ( (uint8_t)APPMGR_INST_RUNNING == r->state )
        {
            /* Its port is told when it comes up; the cable, by install_info. */
            r->state = (uint8_t)APPMGR_INST_FAILED;
            r->why   = (uint8_t)APPMGR_WHY_RESET;
            r->told  = 0U;
            inst_store(k);
        }
    }
    inst_watch(false);
}

void appmgr_inst_prepare(uint8_t src_kind, uint8_t dst_kind, uint8_t asker)
{
    if ( NULL != s_inst )
    {
        s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
        if ( (uint8_t)KFW_SRC_PENDING == src_kind )
        {
            /* The mailbox's install is the host's bench path: no record. */
        }
        else if ( (uint8_t)KFW_TARGET_APP == dst_kind )
        {
            s_inst->pending = (uint8_t)APPMGR_INST_APP;
        }
        else if ( (uint8_t)KFW_TARGET_OS == dst_kind )
        {
            s_inst->pending = (uint8_t)APPMGR_INST_OS;
        }
        else
        {
            /* The mailbox as a destination, the coprocessor: no record. */
        }
        s_inst->pending_src = asker;
        /* A stream answers its command when it ends; a file or a URL was
         * answered when it was taken. */
        s_inst->live = ((uint8_t)KFW_SRC_SERIAL == src_kind);
    }
}

void appmgr_inst_taken(void)
{
    if ( (NULL == s_inst) || ((uint8_t)APPMGR_INST_KINDS == s_inst->pending) )
    {
        return;
    }
    uint8_t const kind = s_inst->pending;
    uint32_t      id   = s_inst->rec[APPMGR_INST_APP].id;

    if ( s_inst->rec[APPMGR_INST_OS].id > id )
    {
        id = s_inst->rec[APPMGR_INST_OS].id;
    }
    ++id;
    if ( 0U == id )
    {
        id = 1U;   /* 0 is "none" */
    }
    appmgr_inst_rec_t * const r = &s_inst->rec[kind];
    (void)memset(r, 0, sizeof(appmgr_inst_rec_t));
    r->id     = id;
    r->layout = (uint8_t)APPMGR_INST_LAYOUT;
    r->state  = (uint8_t)APPMGR_INST_RUNNING;
    r->src    = s_inst->pending_src;
    s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
    s_inst->open    = kind;
    s_inst->why     = (uint8_t)APPMGR_WHY_NONE;
    inst_tell(kind, true, false);   /* the answer to the command that asked */
}

void appmgr_inst_named(const char *name)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_KINDS != s_inst->open)
        && (NULL != name) && ('\0' != name[0]) )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[s_inst->open];

        (void)snprintf(r->name, sizeof(r->name), "%s", name);
    }
}

void appmgr_inst_why(appmgr_inst_why_t why)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_KINDS != s_inst->open)
        && ((uint8_t)APPMGR_WHY_NONE == s_inst->why) )
    {
        s_inst->why = (uint8_t)why;
    }
}

void appmgr_inst_failed(uint32_t pct)
{
    if ( NULL == s_inst )
    {
        return;
    }
    s_inst->pending = (uint8_t)APPMGR_INST_KINDS;
    if ( (uint8_t)APPMGR_INST_KINDS != s_inst->open )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[s_inst->open];

        r->why = s_inst->why;
        if ( (uint8_t)APPMGR_WHY_NONE == r->why )
        {
            r->why = (uint8_t)APPMGR_WHY_OTHER;
        }
        r->pct = (uint8_t)pct;
        inst_finish(APPMGR_INST_FAILED, false);
    }
}

void appmgr_inst_installed(const char *name, const char *version, bool jumps)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_APP == s_inst->open) )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[APPMGR_INST_APP];

        (void)snprintf(r->name, sizeof(r->name), "%s", name);
        (void)snprintf(r->version, sizeof(r->version), "%s", version);
        r->pct = (uint8_t)APPMGR_INST_PCT_DONE;
        inst_finish(APPMGR_INST_INSTALLED, jumps);
    }
}

void appmgr_inst_rebooting(void)
{
    if ( (NULL != s_inst) && ((uint8_t)APPMGR_INST_OS == s_inst->open) )
    {
        appmgr_inst_rec_t * const r = &s_inst->rec[APPMGR_INST_OS];
        kdrv_slot_desc_t desc;

        if ( kdrv_slot_desc(&desc) == KODE_OK )
        {
            (void)snprintf(r->name, sizeof(r->name), "%s", desc.project);
            (void)snprintf(r->version, sizeof(r->version), "%s", desc.version);
        }
        r->slot = kdrv_slot_address();
        r->pct  = (uint8_t)APPMGR_INST_PCT_DONE;
        inst_finish(APPMGR_INST_REBOOTING, true);
    }
}

bool appmgr_inst_on(QEvt const *e)
{
    bool handled = true;

    if ( NULL == s_inst )
    {
        return false;   /* the top state ignores them */
    }
    switch ( e->sig )
    {
        case CHANNEL_STATE_SIG: {
            s_inst->chan_up = ((uint8_t)CHAN_UP == Q_EVT_CAST(ChannelStateEvt)->state);
            if ( s_inst->chan_up )
            {
                inst_flush((uint8_t)KODE_CONSOLE_SRC_NET);
            }
            else
            {
                inst_answered((uint8_t)KODE_CONSOLE_SRC_NET, false);
            }
            break;
        }
        case BLE_STATE_SIG: {
            s_inst->ble_up = Q_EVT_CAST(BleStateEvt)->console;
            if ( s_inst->ble_up )
            {
                inst_flush((uint8_t)KODE_CONSOLE_SRC_BLE);
            }
            else
            {
                inst_answered((uint8_t)KODE_CONSOLE_SRC_BLE, false);
            }
            break;
        }
        case CONSOLE_SENT_SIG: {
            ConsoleSentEvt const * const c = Q_EVT_CAST(ConsoleSentEvt);
            inst_answered(c->src, c->ok);
            break;
        }
        default: {
            handled = false;
            break;
        }
    }
    return handled;
}
