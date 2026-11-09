/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file log_ao.c
 * @brief AO_Log: the Active Object that owns the console device.
 */
#include "kaos.h"
#include "kaos_start.h"

#include <string.h>

#include "kqp.h"
#include "kaos_claim.h"
#include "khal_console.h"
#include "khal_nvs.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kset.h"
#if CONFIG_KODEOS_CHANNEL
#include "net_channel.h"
#endif
#if CONFIG_KODEOS_BLE_CONSOLE
#include "kdrv_ble.h"     /* the console's second port */
#endif

#define TAG        "kaos.log"
#define PROMPT     "kode> "
/* Carriage return + erase to end of line: wipes whatever the prompt had on
 * screen so a log line starts in column zero. */
#define ERASE_LINE "\r\033[K"
/* How long one raw read waits for bytes before giving the pump a chance to
 * notice the world changed. Short enough that abandoning raw mode is prompt,
 * long enough that a streaming host never pays for the round trip. */
#define KHAL_RAW_READ_MS   200U

typedef struct {
    QActive super;
    char    line[KODE_CONSOLE_LINE_MAX];
    size_t  len;
    bool    prompt_drawn;   /**< No prompt to erase until the first draw. */
    bool    overflow;       /**< This line ran past the buffer: refuse it. */
} Log;

static Log l_log;
QActive * const AO_Log = &l_log.super;

/** @brief The console's word for each level, as `log_level` takes it. */
static const char *const LEVEL_WORDS[] = KODE_LOG_LEVEL_WORDS;
_Static_assert((sizeof(LEVEL_WORDS) / sizeof(LEVEL_WORDS[0]))
                   == ((size_t)KODE_LOG_OFF + 1U),
               "one word per kode_log_lvl_t value");

static QState Log_initial(Log * const me, void const * const par);
static QState Log_active(Log * const me, QEvt const * const e);

/* ---- The sink klog posts through ---------------------------------------- */

static uint32_t s_dropped;   /**< Lines the pool could not carry. */

uint32_t log_dropped(void)
{
    return s_dropped;
}

/**
 * @brief Bytes the input side lost outright, past the retry below.
 *
 * Unlike @c s_dropped (log lines this AO could not carry), these are bytes off
 * the wire that never became an event. Both only grow; `ps` prints both.
 */
static uint32_t s_rx_dropped;

uint32_t console_rx_dropped(void)
{
    return s_rx_dropped;
}

/* The reader task's retry on an empty pool: 200 tries of 1 ms. Bounded so a
 * pool that stays empty gives the byte up rather than blocking the reader,
 * the wire's only consumer. */
#define KODE_CONSOLE_RX_RETRY_MS     1U
#define KODE_CONSOLE_RX_RETRY_TRIES  200U

/* The room a byte and a line leave in AO_Log's queue: the pools alone do not
 * bound it, and a post with no margin asserts. A line leaves more than a
 * byte, so diagnostics never starve the console's input. */
#define LOG_BYTE_MARGIN  8U   /**< Slots a console byte leaves free in AO_Log's queue. */
#define LOG_LINE_MARGIN  16U  /**< Slots a log line leaves free in AO_Log's queue.     */

/* How much backlog counts as "wait". A quarter of the driver's ring: enough
 * that the drain always has work in hand, little enough that a slice does not
 * refill it the moment it clears. */
#define LOG_BLE_BACKLOG_HIGH  512U

/* Lines handed to this AO that it has not written yet. When the writer is
 * behind, producers wait (console_congested()), so a big transfer settles at
 * what the port can carry. Half the pool on the cable, so others can still log.
 * On BLE, AO_Log runs below the producers and the driver's ring still reads
 * empty, so the queue itself is bounded: 2048 bytes of ring / 256-byte lines is
 * eight, and half of that leaves the drain room to stay ahead. */
static uint32_t s_pending;
#define LOG_PENDING_HIGH      (KQP_LARGE_POOL_COUNT / 2U)
#define LOG_PENDING_HIGH_BLE  4U

bool console_congested(void)
{
    QF_CRIT_STAT
    QF_CRIT_ENTRY();
    uint32_t const pending = s_pending;
    QF_CRIT_EXIT();

    uint32_t limit = LOG_PENDING_HIGH;
#if CONFIG_KODEOS_BLE_CONSOLE
    if ( kdrv_ble_console_attached() )
    {
        limit = LOG_PENDING_HIGH_BLE;
    }
#endif
    bool congested = (pending > limit);
#if CONFIG_KODEOS_BLE_CONSOLE
    /* And the ring itself, which is the measure that works once AO_Log HAS
     * run - between two of its own writes, and for anyone producing from a
     * priority below it. Either one is a reason to wait. */
    if ( !congested )
    {
        congested = (kdrv_ble_console_backlog() > LOG_BLE_BACKLOG_HIGH);
    }
#endif
    return congested;
}

/**
 * @brief Count a line the front end could not carry.
 *
 * Dropped rather than asserted, but never invisibly: `ps` reports the count.
 *
 * @param src The line's source (a KodeConsoleSrc).
 */
static void log_line_lost(uint8_t src)
{
    ++s_dropped;
#if CONFIG_KODEOS_CHANNEL
    if ( (uint8_t)KODE_CONSOLE_SRC_NET == src )
    {
        /* A reply the owner is waiting for on the channel: `channel_info`
         * must say a line went missing, not only `ps`. */
        kchan_out_lose(1U);
    }
#else
    (void)src;
#endif
}

/**
 * @brief Called by klog, on whichever thread did the logging.
 *
 * Bounded: a full pool drops the line rather than asserting.
 *
 * @param line The line.
 * @param len  Its length in bytes; cut to what a LogEvt holds.
 * @param src  The line's source (a KodeConsoleSrc).
 */
static void log_sink(const char *line, size_t len, uint8_t src)
{
    LogEvt *e = Q_NEW_X(LogEvt, 0U, LOG_LINE_SIG);
    if ( NULL == e )
    {
        log_line_lost(src);
        return;
    }
    if ( sizeof(e->line) <= len )
    {
        len = sizeof(e->line) - 1U;
    }
    (void)memcpy(e->line, line, len);
    e->line[len] = '\0';
    e->len       = (uint16_t)len;
    e->src       = src;

    /* Counted under a critical section, unlike s_dropped above: that one only
     * ever grows, so a lost increment costs a number. This one comes back
     * down, and a lost decrement would leave the console permanently "behind"
     * and stall every producer after it. */
    QF_CRIT_STAT
    QF_CRIT_ENTRY();
    ++s_pending;
    QF_CRIT_EXIT();

    /* A refused post gives the event back to its pool. */
    if ( !QACTIVE_POST_X(&l_log.super, &e->super, LOG_LINE_MARGIN, NULL) )
    {
        QF_CRIT_ENTRY();
        --s_pending;
        QF_CRIT_EXIT();
        log_line_lost(src);
    }
}

/**
 * @brief The reader task's only job: hand the byte over and go back to
 *        blocking.
 *
 * Runs on the reader task, which may block. A burst wider than the small pool
 * can find it empty, so a failed allocation is retried for a bounded time
 * before the byte is given up and counted; hanging here would stop reading
 * the wire at all.
 */
static void log_on_byte(uint8_t byte)
{
    bool     sent  = false;
    uint32_t tries = 0U;

    /* A queue within LOG_BYTE_MARGIN of full, or a pool down to
     * KQP_BYTE_POOL_MARGIN blocks, is waited out the same way: AO_Log drains
     * it as fast as the pool refills. A flood that outlasts the wait is
     * counted in rx_dropped and never takes a block another AO needs. */
    while ( !sent && (KODE_CONSOLE_RX_RETRY_TRIES >= tries) )
    {
        if ( 0U < tries )
        {
            khal_delay_ms(KODE_CONSOLE_RX_RETRY_MS);
        }
        ConsoleByteEvt * const e = Q_NEW_X(ConsoleByteEvt, KQP_BYTE_POOL_MARGIN, CONSOLE_BYTE_SIG);
        if ( NULL != e )
        {
            e->byte = byte;
            /* A refused post gives the event back to its pool. */
            sent = QACTIVE_POST_X(&l_log.super, &e->super, LOG_BYTE_MARGIN, NULL);
        }
        ++tries;
    }
    if ( !sent )
    {
        /* Still nothing after the retry window: counted, not silent. */
        ++s_rx_dropped;
    }
}

#if CONFIG_KODEOS_BLE_CONSOLE
/**
 * @brief A line arrived on the other port. Same event, different tag.
 *
 * Runs on NimBLE's host task. No line editor and no echo on this side.
 *
 * @param line NUL-terminated, terminator already stripped.
 */
static void log_on_ble_line(const char *line)
{
    size_t const n = strlen(line);
    if ( KODE_CONSOLE_LINE_MAX <= n )
    {
        KLOG_W(TAG, "ble line too long (%u), dropped", (unsigned)n);
        return;
    }
    ConsoleLineEvt *e = Q_NEW_X(ConsoleLineEvt, 0U, CONSOLE_LINE_SIG);
    if ( NULL != e )
    {
        e->src = (uint8_t)KODE_CONSOLE_SRC_BLE;
        (void)memcpy(e->line, line, n + 1U);
        /* Published, like the cable's: this AO owns the ports, not the
         * policy of what a line means. */
        QACTIVE_PUBLISH(&e->super, &l_log.super);
    }
}
#endif

/* ---- Raw mode: the input stream, handed to somebody else ------------------
 * For one transfer the reader task runs raw_pump() instead of the line path,
 * and image bytes go to the requesting AO as whole chunks. AO_Log stays the
 * console's only writer.
 *
 * AO_Log writes these before arming (s_pump is volatile and written last);
 * the reader task owns them from then until it disarms.
 */
static QActive *s_raw_who;
static uint32_t s_raw_left;
#if CONFIG_KODEOS_BLE_CONSOLE
/* The BLE side of raw mode: bytes are pushed on NimBLE's host task and
 * gathered here until a chunk is whole. Owned by that task; AO_Log touches it
 * only in a BLE raw request, to collect a chunk an abort left behind. */
static RecvDataEvt *s_ble_chunk;
#endif

/**
 * @brief Called by the reader task the moment raw mode is live.
 *
 * Only the reader task knows when the diversion has happened, so the
 * requester hears yes from here. If the answer cannot be allocated, the
 * requester's own watchdog closes the path.
 */
static void raw_armed(void)
{
    ConsoleRawRspEvt *a = Q_NEW_X(ConsoleRawRspEvt, 0U, CONSOLE_RAW_RSP_SIG);
    if ( NULL != a )
    {
        a->status = (uint8_t)KODE_OK;
        QACTIVE_POST(s_raw_who, &a->super, NULL);
    }
    else
    {
        khal_console_raw_end();
    }
}

/**
 * @brief One pass of the reader task while an image is arriving.
 *
 * Allocate first, read second: an empty pool then costs only a delay. The
 * flow control is the `>|ack` protocol.
 */
static void raw_pump(void)
{
    RecvDataEvt *e = Q_NEW_X(RecvDataEvt, 0U, RECV_DATA_SIG);
    if ( NULL == e )
    {
        khal_delay_ms(10U);      /* the consumer is behind; let it work */
        return;
    }

    size_t const want = ((uint32_t)sizeof(e->data) > s_raw_left)
                        ? (size_t)s_raw_left : sizeof(e->data);
    int32_t const n = khal_console_read_raw(e->data, want, KHAL_RAW_READ_MS);

    if ( 0 >= n )
    {
        QF_gc(&e->super);             /* timeout: the watchdog is AO_AppMgr's */
        return;
    }

    e->len      = (uint16_t)n;
    s_raw_left -= (uint32_t)n;
    QACTIVE_POST(s_raw_who, &e->super, NULL);

    if ( 0U == s_raw_left )
    {
        /* The count is the boundary, and the board holds it: a host that
         * keeps talking past its own `size` finds the line editor again, and
         * cannot run its overrun into the next command's payload. */
        khal_console_raw_end();
    }
}

#if CONFIG_KODEOS_BLE_CONSOLE
/**
 * @brief Gather raw bytes off the air into chunks and hand them over.
 *
 * Runs on NimBLE's host task. One chunk is filled across many GATT writes and
 * posted when full or when the payload's count runs out.
 *
 * @param data Bytes exactly as they arrived.
 * @param len  How many.
 */
static void log_on_ble_raw(const uint8_t *data, uint16_t len)
{
    uint16_t off = 0U;
    while ( (off < len) && (0U < s_raw_left) )
    {
        if ( NULL == s_ble_chunk )
        {
            s_ble_chunk = Q_NEW_X(RecvDataEvt, 0U, RECV_DATA_SIG);
            if ( NULL == s_ble_chunk )
            {
                /* The consumer is behind. Dropping here would corrupt the
                 * image silently, so say so and let the transfer fail on the
                 * CRC rather than install something wrong. */
                KLOG_E(TAG, "out of events; the image is now short");
                return;
            }
            s_ble_chunk->len = 0U;
        }
        uint16_t room = (uint16_t)(sizeof(s_ble_chunk->data)
                                   - s_ble_chunk->len);
        uint16_t take = (uint16_t)(len - off);
        if ( take > room )
        {
            take = room;
        }
        if ( (uint32_t)take > s_raw_left )
        {
            take = (uint16_t)s_raw_left;
        }
        (void)memcpy(&s_ble_chunk->data[s_ble_chunk->len], &data[off], take);
        s_ble_chunk->len = (uint16_t)(s_ble_chunk->len + take);
        off              = (uint16_t)(off + take);
        s_raw_left      -= (uint32_t)take;

        if ( (sizeof(s_ble_chunk->data) == s_ble_chunk->len)
            || (0U == s_raw_left) )
        {
            QACTIVE_POST(s_raw_who, &s_ble_chunk->super, NULL);
            s_ble_chunk = NULL;
        }
    }
    if ( 0U == s_raw_left )
    {
        /* The count is the boundary and the board holds it, exactly as it
         * does on the cable: a host that keeps talking past its own `size`
         * finds the line assembler again. */
        kdrv_ble_console_raw(false);
    }
}
#endif

/* ---- Drawing ------------------------------------------------------------- */

/**
 * @brief Write the prompt and, after it, whatever of the line has been typed.
 *
 * @param me The AO.
 */
static void prompt_draw(Log * const me)
{
    khal_console_write(PROMPT, sizeof(PROMPT) - 1U);
    if ( 0U < me->len )
    {
        khal_console_write(me->line, me->len);
    }
    me->prompt_drawn = true;
}

/**
 * @brief Wipe the prompt line, if a prompt has ever been drawn.
 *
 * @param me The AO.
 */
static void prompt_erase(Log * const me)
{
    if ( me->prompt_drawn )
    {
        khal_console_write(ERASE_LINE, sizeof(ERASE_LINE) - 1U);
    }
}

/* ---- The console's log level --------------------------------------------- */

void log_level_request(const char *tag, kode_log_lvl_t level)
{
    LogLevelEvt *ev = Q_NEW_X(LogLevelEvt, 0U, LOG_LEVEL_SET_SIG);
    if ( NULL != ev )
    {
        ev->tag[0] = '\0';
        if ( NULL != tag )
        {
            (void)strncpy(ev->tag, tag, sizeof(ev->tag) - 1U);
            ev->tag[sizeof(ev->tag) - 1U] = '\0';
        }
        ev->level = (uint8_t)level;
        /* A queue with no room for a request refuses it; the event goes back
         * to its pool. */
        (void)QACTIVE_POST_X(AO_Log, &ev->super, KQP_LOG_CMD_MARGIN, NULL);
    }
}

bool log_dock_lend_request(QActive *who, bool lend)
{
    LogDockReqEvt *ev;

    if ( NULL == who )
    {
        return false;
    }
    ev = Q_NEW_X(LogDockReqEvt, 0U, LOG_DOCK_REQ_SIG);
    if ( NULL == ev )
    {
        return false;
    }
    ev->who  = who;
    ev->lend = lend;
    return QACTIVE_POST_X(AO_Log, &ev->super, KQP_CMD_MARGIN, NULL);
}

/** @brief The word a dock request's log line uses. @param lend The request. @return The word. */
static const char *console_dock_word(bool lend)
{
    const char *w = "take back";

    if ( lend )
    {
        w = "lend";
    }
    return w;
}

/**
 * @brief Lend the dock console's pins, or take them back, and say so to the asker.
 *
 * The dock's pads are GPIO26/27, which the station's data loop drives with a
 * UART of its own. The cable console is never touched. Without the dock built
 * in the pins are already free and khal answers KODE_OK at once.
 *
 * @param r The request.
 */
static void dock_lend(LogDockReqEvt const * const r)
{
    LogDockEvt * const ev = Q_NEW_X(LogDockEvt, 0U, LOG_DOCK_SIG);
    kode_status_t      rc;

    bool               lent;

    if ( r->lend )
    {
        rc   = khal_console_dock_release();
        lent = (KODE_OK == rc);
    }
    else
    {
        rc   = khal_console_dock_take();
        lent = (KODE_OK != rc);   /* a dock that would not come back leaves the pins free */
    }
    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "dock pins: the %s was refused (%d)", console_dock_word(r->lend), (int)rc);
    }
    if ( NULL != ev )
    {
        ev->lent   = lent;
        ev->status = (uint8_t)rc;
        QACTIVE_POST(r->who, &ev->super, AO_Log);
    }
}

/**
 * @brief Apply the level stored under the `log` key, if there is one.
 *
 * Called first in the initial transition, before any other AO starts, so every
 * later line is filtered at the stored level. The NOTICE line says a stored
 * level applies. A byte past KODE_LOG_OFF is ignored and the build's level stands.
 */
static void level_load(void)
{
    uint8_t stored = 0U;

    if ( khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_LOG, &stored) != KODE_OK )
    {
        return;
    }
    if ( (uint8_t)KODE_LOG_OFF < stored )
    {
        KLOG_W(TAG, "log: stored level %u is not one this build has; "
                    "keeping '%s'", (unsigned)stored,
               LEVEL_WORDS[kode_log_build_level()]);
        return;
    }
    kode_log_set_level(NULL, (kode_log_lvl_t)stored);
    KLOG_N(TAG, "log: level %s (stored); config key=\"log\" value=%s resets it",
           LEVEL_WORDS[stored], LEVEL_WORDS[kode_log_build_level()]);
}

/**
 * @brief Apply a level, and store it when it is the global default.
 *
 * The key holds only what differs from the build: a level set back to
 * CONFIG_KLOG_DEFAULT_LEVEL is erased rather than written, so an absent key
 * and the build's level are one state, not two.
 *
 * @param l The request, as log_level_request() posted it.
 */
static void level_apply(LogLevelEvt const * const l)
{
    if ( (uint8_t)KODE_LOG_OFF < l->level )
    {
        return;
    }
    kode_log_lvl_t const lvl = (kode_log_lvl_t)l->level;

    if ( '\0' != l->tag[0] )
    {
        kode_log_set_level(l->tag, lvl);
        return;
    }

    kode_log_set_level(NULL, lvl);
    kode_status_t st;
    if ( kode_log_build_level() == lvl )
    {
        st = khal_nvs_erase(KSET_NAMESPACE, KSET_KEY_LOG);
    }
    else
    {
        st = khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_LOG, (uint8_t)lvl);
    }
    if ( KODE_OK != st )
    {
        KLOG_W(TAG, "log: level %s applied but not stored; the next boot "
                    "starts at %s", LEVEL_WORDS[lvl],
               LEVEL_WORDS[kode_log_build_level()]);
    }
}

/**
 * @brief Answer `config key="log"`: set the global level first when asked.
 *
 * @param me The AO, the answer's sender.
 * @param r  The request; its value is an index into KODE_LOG_LEVEL_WORDS.
 */
static void level_config(Log * const me, KodeConfigReqEvt const * const r)
{
    uint8_t stored = 0U;

    if ( r->set )
    {
        LogLevelEvt l;
        (void)memset(&l, 0, sizeof(l));
        l.level = r->e;
        level_apply(&l);
    }
    bool const is_stored = (khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_LOG, &stored) == KODE_OK);
    (void)kode_config_answer(&me->super, r, KODE_OK, is_stored,
                             LEVEL_WORDS[kode_log_get_level()]);
}

/* ---- The AO -------------------------------------------------------------- */

void Log_ctor(void)
{
    Log * const me = &l_log;
    QActive_ctor(&me->super, Q_STATE_CAST(&Log_initial));
    me->len          = 0U;
    me->prompt_drawn = false;
    me->overflow     = false;
}

#define KODE_QUEUE_LOG_LEN  64U  /**< AO_Log's queue; lines, bytes and requests leave margins. */

AO_STORAGE(log, KODE_QUEUE_LOG_LEN, KQP_AO_STACK_MEDIUM);
_Static_assert((KQP_LOG_CMD_MARGIN > 0U) && (KQP_LOG_CMD_MARGIN < LOG_BYTE_MARGIN)
               && (LOG_BYTE_MARGIN < LOG_LINE_MARGIN) && (LOG_LINE_MARGIN < KODE_QUEUE_LOG_LEN),
               "a line leaves a byte its room, a byte a request's, and a request the no-margin posts'");

void kaos_log_start(void)
{
    Log_ctor();
    AO_START(AO_Log, log, KODE_PRIO_LOG);
#if CONFIG_KODEOS_CONSOLE
    /* The global level's words, in kode_log_lvl_t order. */
    static const char *const LEVELS[] = KODE_LOG_LEVEL_WORDS;
    /* `config key="log"`, registered once this AO has the queue its requests
     * are posted to. */
    static const kode_config_t LOG_CONFIG[] = {
        { .name = KSET_KEY_LOG, .type = KODE_TYPE_ENUM, .enums = LEVELS,
          .n_enums = (uint8_t)(sizeof(LEVELS) / sizeof(LEVELS[0])) },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Log, KQP_LOG_CMD_MARGIN, LOG_CONFIG), "log settings");
#endif
}

/**
 * @brief Claim the console, load the stored level, open the transport and take klog's sink.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Log_active.
 */
static QState Log_initial(Log * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_CONSOLE, me, "AO_Log");

    /* The stored level first, so the transport's bring-up and this AO's own
     * lines below are filtered at it too. */
    level_load();

    /* Open the transport before claiming the log stream, so the first line to
     * arrive through the sink has somewhere to go. */
    khal_console_init();
    khal_console_write("\r\n", 2);

    /* From here on every KLOG_* line becomes an event on this queue. Until
     * now they went out synchronously through klog's rescue path, which is
     * how the boot lines above this point got printed at all. */
    kode_log_set_sink(&log_sink);

#if CONFIG_KODEOS_BLE_CONSOLE
    /* The second port: the driver decides when it has bytes, this AO that
     * they are console lines. Installed here so the radio is never up with
     * nobody listening. */
    (void)kdrv_ble_console_set_rx_cb(&log_on_ble_line);
    (void)kdrv_ble_console_set_raw_cb(&log_on_ble_raw);
#endif

    KLOG_I(TAG, "console owned by AO_Log; logging is an event from here on");
    return Q_TRAN(&Log_active);
}

/** Handle one input byte: echo, edit, and put finished lines on the bus. */
static void handle_byte(Log * const me, uint8_t c)
{
    if ( ('\r' == c) || ('\n' == c) )
    {
        khal_console_write("\r\n", 2);
        me->prompt_drawn = false;
        if ( me->overflow )
        {
            /* A line cut in half must not run: refuse it. kode_log_reply(), not
             * khal_console_write(), so the reply keeps its place behind the
             * replies already queued. */
            kode_log_reply(">err code=3 msg=line_too_long");
            me->overflow = false;
            me->len      = 0U;
        }
        else if ( 0U < me->len )
        {
            me->line[me->len] = '\0';
            ConsoleLineEvt *e = Q_NEW_X(ConsoleLineEvt, 0U, CONSOLE_LINE_SIG);
            if ( NULL != e )
            {
                /* Explicit: Q_NEW_X does not zero the block, and an
                 * uninitialised src from a recycled one would refuse
                 * a terminal install over the cable at random. */
                e->src = (uint8_t)KODE_CONSOLE_SRC_SERIAL;
                (void)memcpy(e->line, me->line, me->len + 1U);
                /* Published, not posted: this AO owns the device, not the
                 * policy, and a firmware built without a command line still
                 * has a console it can log to. */
                QACTIVE_PUBLISH(&e->super, &me->super);
            }
            else
            {
                /* AO_Log's own step must never block (rule 5), so there is no retry
                 * here: the lost line is counted. */
                ++s_rx_dropped;
            }
            me->len = 0U;
        }
        /* No prompt here: it would sit in front of the response's '>' on the
         * wire. The next keystroke brings it back. */
    }
    else if ( (0x7fU == c) || (0x08U == c) )
    {   /* DEL / backspace */
        if ( 0U < me->len )
        {
            --me->len;
            khal_console_write("\b \b", 3);
        }
        /* Deleting brings the line back under the limit, so somebody who
         * overshot, backed up and retyped is not refused a line that fits. */
        me->overflow = false;
    }
    else if ( (0x20U <= c) && (0x7fU > c) )
    {
        if ( (sizeof(me->line) - 1U) > me->len )
        {
            if ( !me->prompt_drawn )
            {
                /* The prompt is drawn on demand: at the first byte of a
                 * line, not after every output line. That is what keeps '>'
                 * and '!' in column zero while a host is driving. */
                prompt_draw(me);
            }
            char const ch = (char)c;
            me->line[me->len++] = ch;
            khal_console_write(&ch, 1);            /* echo */
        }
        else
        {
            me->overflow = true;
        }
    }
    else
    {
        /* a control byte nobody asked about */
    }
}

/**
 * @brief Lines out and bytes in: log lines, console input, raw transfers, the dock and the level.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Log_active(Log * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case LOG_LINE_SIG: {
            LogEvt const * const l = Q_EVT_CAST(LogEvt);
            /* Wipe the prompt and print on a clean line; it comes back only to
             * restore a half-typed command, so the next record starts with '>' or
             * '!'. No lock: this AO is the only writer. */
            prompt_erase(me);
            khal_console_write(l->line, l->len);
#if CONFIG_KODEOS_BLE_CONSOLE
            /* The console's second port gets every complete line, if anyone is
             * listening. The prompt, echo and erase are for a terminal and are
             * not mirrored. */
            (void)kdrv_ble_console_write(l->line, l->len);
#endif
#if CONFIG_KODEOS_CHANNEL
            /* A reply to a command that came in on the Kode channel also goes
             * to the ring AO_Net drains onto cmd/out, one line per message,
             * without its newline. Nothing else does: not the replies to the
             * cable or BLE, and not a '!' line. */
            if ( (uint8_t)KODE_CONSOLE_SRC_NET == l->src )
            {
                size_t n = l->len;
                while ( (0U < n) && (('\n' == l->line[n - 1U])
                                    || ('\r' == l->line[n - 1U])) )
                {
                    --n;
                }
                (void)kchan_out_push(l->line, n);
            }
#endif
            if ( 0U < me->len )
            {
                prompt_draw(me);
            }
            else
            {
                me->prompt_drawn = false;
            }

            /* On the wire now, so whoever is waiting on this AO has room. */
            QF_CRIT_STAT
            QF_CRIT_ENTRY();
            if ( 0U < s_pending )
            {
                --s_pending;
            }
            QF_CRIT_EXIT();

            status = Q_HANDLED();
            break;
        }
        case CONSOLE_RAW_REQ_SIG: {
            ConsoleRawReqEvt const * const r = Q_EVT_CAST(ConsoleRawReqEvt);
            kode_status_t st = KODE_ERROR;

            if ( (NULL != r->who) && (0U < r->size) )
            {
                /* Any half-typed line is abandoned rather than carried over:
                 * what follows on this wire is not text, and whatever was in
                 * the editor was never terminated. */
                me->len          = 0U;
                me->prompt_drawn = false;
                s_raw_who        = r->who;
                s_raw_left       = r->size;
#if CONFIG_KODEOS_BLE_CONSOLE
                /* Two ports have a wire to hand over, the cable and BLE; any
                 * source that is not BLE is the cable here. The Kode channel
                 * (KODE_CONSOLE_SRC_NET) has none, and the console refuses
                 * every verb that would ask it for one (src_allows()). */
                if ( (uint8_t)KODE_CONSOLE_SRC_BLE == r->src )
                {
                    /* No reader task to divert on this port: the driver stops
                     * assembling lines, so the mode is live at once (the `>|ready`
                     * gate keeps a central from streaming early).
                     *
                     * A chunk an aborted transfer stranded is collected here: the
                     * host-task callback that filled it has returned, and nothing
                     * can reach it again until the mode is turned back on below. */
                    if ( NULL != s_ble_chunk )
                    {
                        QF_gc(&s_ble_chunk->super);
                        s_ble_chunk = NULL;
                    }
                    kdrv_ble_console_raw(true);
                    raw_armed();
                    st = KODE_OK;
                }
                else
#endif
                {
                    /* A request, not a switch: the reader task drains the residue,
                     * enters the mode and answers through raw_armed(). This AO never
                     * touches the transport's input side. */
                    st = khal_console_raw_begin(&raw_pump, &raw_armed);
                }
            }

            if ( (KODE_OK != st) && (NULL != r->who) )
            {
                /* Refused outright (bad request, or raw mode already on): nothing
                 * is pending, so this AO answers no. Keep the NULL check: a NULL
                 * requester is a bad request, and posting to it would crash. */
                ConsoleRawRspEvt *a = Q_NEW_X(ConsoleRawRspEvt, 0U, CONSOLE_RAW_RSP_SIG);
                if ( NULL != a )
                {
                    a->status = (uint8_t)st;
                    QACTIVE_POST(r->who, &a->super, me);
                }
                /* else: nothing armed, nobody to tell - the requester's own
                 * watchdog is what closes this. */
            }
            status = Q_HANDLED();
            break;
        }
        case CONSOLE_RAW_END_SIG: {
            /* Only ever an abort: both ports leave raw mode themselves when the
             * count runs out. Ending both is cheaper than remembering which. */
#if CONFIG_KODEOS_BLE_CONSOLE
            /* Stopped, not collected: a GATT write may still be filling
             * s_ble_chunk on the other core. The next BLE request collects a
             * stranded chunk (see CONSOLE_RAW_REQ_SIG). */
            kdrv_ble_console_raw(false);
#endif
            khal_console_raw_end();
            status = Q_HANDLED();
            break;
        }
        case CONSOLE_BYTE_SIG: {
            handle_byte(me, Q_EVT_CAST(ConsoleByteEvt)->byte);
            status = Q_HANDLED();
            break;
        }
        case LOG_DOCK_REQ_SIG: {
            dock_lend(Q_EVT_CAST(LogDockReqEvt));
            status = Q_HANDLED();
            break;
        }
        case LOG_LEVEL_SET_SIG: {
            level_apply(Q_EVT_CAST(LogLevelEvt));
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            KodeConfigReqEvt const * const r = Q_EVT_CAST(KodeConfigReqEvt);
            if ( strcmp(r->key, KSET_KEY_LOG) == 0 )
            {
                level_config(me, r);
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

void log_start_reader(void)
{
    /* Last, and from outside the AO: until the queue exists there is nowhere
     * for a keystroke to go. */
    prompt_draw(&l_log);
    (void)khal_console_start_reader(&log_on_byte, KODE_PRIO_READER);
}
