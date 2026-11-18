/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_ao.c
 * @brief AO_Display: the Active Object that owns the panel.
 */
#include "kaos.h"
#include "kaos_start.h"

#include <stdio.h>
#include <string.h>
/* cppcheck-suppress misra-c2012-21.10 ; localtime_r() only: the board keeps UTC and the face's zone is a TZ string */
#include <time.h>

#include "sdkconfig.h"    /* CONFIG_KODEOS_MIRROR and CONFIG_KODEOS_BLE_
                           * CONSOLE - both tested directly below, not only
                           * dragged in transitively. */

#include "kqp.h"
#include "kaos_claim.h"
#include "kbsp_board.h"   /* how big the glass is: a board fact, not a QP one */
#include "kdrv_buttons.h"  /* KDRV_BTN_TOP and KDRV_BTN_BOT: the row's answers */
#include "kdrv_display.h"
#include "kdrv_tca6408a.h"  /* the supply pin's field names, asked of AO_IoExp */
#include "khal_nvs.h"
#include "khal_sync.h"
#include "khal_system.h"  /* khal_alloc_psram/khal_free: the mirror's frame
                           * buffer and the `display` row's band. */
#include "kode_log.h"
#include "kset.h"

#include "khal_time.h"    /* the panel's look: delays and uptime; asleep, the minute */

#include "display_sleep.h"   /* the two words a task reads the glass's rest from */

#if CONFIG_KODE_LOWPOWER
#include "display_clock.h"
#endif
#include "display_compose.h"

#if CONFIG_KODEOS_MIRROR
/* Everything below is the mirror's and is compiled only with KODEOS_MIRROR:
 * its codec and PPA buffers cost about 530 KB of PSRAM, and its claims would
 * name AO_Display the owner of a codec nothing can reach. */
#include "kaos_wire.h"    /* the base64 encoder shared with console_ao.c */
#include "kdrv_ble.h"     /* the mirror's second transport - see mirror_send_slice() */
#include "kdrv_jpeg.h"
#include "kdrv_ppa.h"
#include "kmirror_proto.h"
#endif

#include "kverbs.h"
#include "krows.h"

#define TAG "kaos.display"

#if CONFIG_KODEOS_MIRROR
/* ---- Mirror: pacing and wire shape ---------------------------------------
 * Two pacers, kept distinct (see mirror_pass()): the credit window in
 * kmirror_proto between frames, and KMIRROR_SLICE records per tick within one,
 * since AO_Display is the highest-priority AO and a whole frame in one step
 * would starve the rest. Records:
 *   `>|m=<base64>`  a slice of the current frame (mirror_send_slice()).
 *   `>|mabort`      the frame in flight will not finish (mirror_abort()).
 *   `>|mpause`      the session is asleep; `mirror_start` revives it.
 * The two control records go on the console line on both transports.
 */
#define KMIRROR_TICK_MS      10U   /**< The console's own pacing tick.      */
#define KMIRROR_SLICE        KAOS_WIRE_SLICE  /**< Records per tick: KCLI_SLICE,
                                    *   since both ride AO_Log.              */
#define KMIRROR_WINDOW        4U   /**< Frames that may be outstanding: how
                                    *   far the sequence may run ahead of the
                                    *   host's acks. Only one frame is ever
                                    *   mid-emission; 4 leaves slack for an
                                    *   ack queued behind other console lines. */

/** @brief Sent when a stop or restart cuts a frame short. No `=`, so a parser
 *         tells it from a data record without decoding. Always on the console
 *         line: on the BLE binary pipe it could not be told from body bytes. */
#define KMIRROR_ABORT_LINE   ">|mabort"

/** @brief Sent when the session goes to sleep (mirror_pause()). The host has
 *         nothing left to ack, so this record is how it learns it must send
 *         `mirror_start`. No `=`, as KMIRROR_ABORT_LINE. */
#define KMIRROR_PAUSE_LINE   ">|mpause"

/**
 * @brief What one mirror_try_start() pass actually did.
 *
 * Idle and blocked are kept apart: an idle screen sends nothing by design,
 * while rows that were owed and could not be encoded mean the pipeline is
 * stuck, and count toward a stall.
 */
typedef enum {
    MIRROR_IDLE = 0,   /**< Nothing was dirty. Costs nothing, by design.    */
    MIRROR_BEGUN,      /**< A frame is encoded and now mid-emission.        */
    MIRROR_BLOCKED     /**< Rows were owed and something refused them.      */
} mirror_step_t;

/** @brief 5 s without a credit pauses the mirror, so a host that stopped
 *         acking does not keep the JPEG codec claimed. Pause, not end: a host
 *         whose acks queue behind other console traffic is slow, not gone. */
#define KMIRROR_STALL_MS      5000U
#define KMIRROR_STALL_TICKS   (KMIRROR_STALL_MS / KMIRROR_TICK_MS)

/* The payload record: 216 base64 characters (162 bytes) plus the `>|m=` prefix
 * fit kode_log_reply()'s line buffer; shared with console_ao.c (kaos_wire.h). */
#define KMIRROR_PAY_B64      KAOS_WIRE_PAY_B64
#define KMIRROR_PAY_BYTES    KAOS_WIRE_PAY_BYTES

/** @brief The BLE binary characteristic's slice, in bytes: what one 256-byte
 *         msys_1 mbuf holds after its headers and the ATT/L2CAP/HCI leading
 *         space. A larger slice chains a second mbuf and drains the pool. Not
 *         KMIRROR_PAY_BYTES, which is base64 arithmetic. */
#define KMIRROR_BLE_PAY_BYTES 214U

/** @brief The highest quality mirror_start() hands the codec.
 *
 * The verb accepts q=1..100, but a full panel measured 19.7 KB at q70; much
 * higher qualities would overflow KMIRROR_JPEG_CAP and drop every frame.
 */
#define KMIRROR_QUALITY_MAX   70U

/* The copied-out frame's ceiling: margin over the 19.7 KB a full panel measured
 * at quality 70 (KMIRROR_QUALITY_MAX), for screens busier than that one. A
 * frame that does not fit is dropped and logged (mirror_try_start()). */
#define KMIRROR_JPEG_CAP     (32U * 1024U)
#define KMIRROR_FRAME_CAP    (KMIRROR_HDR_LEN + KMIRROR_JPEG_CAP)

#endif /* CONFIG_KODEOS_MIRROR */

static DisplayStateEvt s_state_evt;

/* The frozen image, between a capture's response and its release. Written by
 * this AO before it posts the response and read by the borrower afterwards,
 * so the post is what orders the two - no lock needed, and none would help. */
static const uint8_t *s_capture;

/* The shot's own buffer: KSHOT_JPEG_CAP bytes of PSRAM taken the first time a
 * shot is asked for and kept for the life of the board, so a stream pays no
 * allocation per frame and cannot be ended by a refusal in its middle. A
 * board that never uploads a screen never takes it. NULL until then. */
static uint8_t *s_shot_buf;

/* Whether the buffer above is lent, from a KODE_OK response to the release.
 * Ordered by the post, like s_capture: no lock. */
static bool s_shot_lent;

void display_state_request(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_STATE_REQ_SIG);
    QACTIVE_POST(AO_Display, &req, (void *)0);
}

#if CONFIG_KODE_LOWPOWER
void display_minute_request(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_MINUTE_REQ_SIG);
    QACTIVE_POST(AO_Display, &req, NULL);
}
#endif

void display_brightness_request(uint8_t pct, bool persist)
{
    BrightnessEvt *ev = Q_NEW_X(BrightnessEvt, 0U, DISPLAY_BRIGHTNESS_SIG);
    if ( NULL != ev )
    {
        ev->percent = pct;
        ev->persist = persist;
        QACTIVE_POST(AO_Display, &ev->super, NULL);
    }
}

void display_capture_request(QActive *who)
{
    DisplayCaptureReqEvt *e = Q_NEW_X(DisplayCaptureReqEvt, 0U,
                                      DISPLAY_CAPTURE_REQ_SIG);
    if ( NULL != e )
    {
        e->who = who;
        QACTIVE_POST(AO_Display, &e->super, (void *)0);
    }
}

const uint8_t *display_capture_buf(void)
{
    return s_capture;
}

void display_capture_release(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_CAPTURE_END_SIG);
    QACTIVE_POST(AO_Display, &req, (void *)0);
}

/* cppcheck-suppress misra-c2012-8.7 ; kaos.h's display interface: the shot's callers are other AOs, outside this unit */
void display_shot_request(QActive *who, uint8_t quality)
{
    DisplayShotReqEvt *e = Q_NEW_X(DisplayShotReqEvt, 0U,
                                   DISPLAY_SHOT_REQ_SIG);
    if ( NULL != e )
    {
        e->who     = who;
        e->quality = quality;
        QACTIVE_POST(AO_Display, &e->super, (void *)0);
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; kaos.h's display interface: the shot's callers are other AOs, outside this unit */
const uint8_t *display_shot_buf(void)
{
    const uint8_t *lent = NULL;

    if ( s_shot_lent )
    {
        lent = s_shot_buf;
    }
    return lent;
}

/* cppcheck-suppress misra-c2012-8.7 ; kaos.h's display interface: the shot's callers are other AOs, outside this unit */
void display_shot_release(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_SHOT_END_SIG);
    QACTIVE_POST(AO_Display, &req, (void *)0);
}

/* cppcheck-suppress misra-c2012-8.7 ; kaos.h's display interface: the encode's callers are workers, outside this unit */
void display_jpeg_request(display_jpeg_t const *job, uint32_t *len, uint32_t *rc, khal_sem_t done,
                          uint32_t gen, uint32_t const *cur, uint32_t *ack)
{
    DisplayJpegReqEvt *e = Q_NEW_X(DisplayJpegReqEvt, 0U, DISPLAY_JPEG_REQ_SIG);

    if ( NULL == e )
    {
        /* A dry pool must not leave the caller asleep on a request that was
         * never made: answered here. */
        *len = 0U;
        *rc  = (uint32_t)KODE_NO_RESOURCES;
        __atomic_store_n(ack, gen, __ATOMIC_RELEASE);
        khal_sem_signal(done);
        return;
    }
    e->job  = *job;
    e->len  = len;
    e->rc   = rc;
    e->done = done;
    e->gen  = gen;
    e->cur  = cur;
    e->ack  = ack;
    QACTIVE_POST(AO_Display, &e->super, (void *)0);
}

#if CONFIG_KODEOS_MIRROR
void display_mirror_start(uint8_t scale, uint8_t quality)
{
    MirrorStartEvt *e = Q_NEW_X(MirrorStartEvt, 0U, DISPLAY_MIRROR_START_SIG);
    if ( NULL != e )
    {
        e->scale   = scale;
        e->quality = quality;
        QACTIVE_POST(AO_Display, &e->super, (void *)0);
    }
}

void display_mirror_stop(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_MIRROR_STOP_SIG);
    QACTIVE_POST(AO_Display, &req, (void *)0);
}

void display_mirror_ack(uint16_t seq)
{
    MirrorAckEvt *e = Q_NEW_X(MirrorAckEvt, 0U, DISPLAY_MIRROR_ACK_SIG);
    if ( NULL != e )
    {
        e->seq = seq;
        QACTIVE_POST(AO_Display, &e->super, (void *)0);
    }
}

void display_mirror_info_request(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_MIRROR_INFO_REQ_SIG);
    QACTIVE_POST(AO_Display, &req, (void *)0);
}
#endif /* CONFIG_KODEOS_MIRROR */

/* cppcheck-suppress misra-c2012-8.7 ; kaos.h's display interface: the renderer that hands the glyphs is in another unit */
void display_glyphs_hand(const DisplayGlyphsEvt *e)
{
    DisplayGlyphsEvt *ev = Q_NEW_X(DisplayGlyphsEvt, 0U, DISPLAY_GLYPHS_SIG);

    if ( NULL == ev )
    {
        /* The glass then waits out KODE_LOWPOWER_GLYPHS_WAIT_MS and refuses
         * the round or the app's sleep, naming the renderer. */
        KLOG_E(TAG, "out of events; the glyphs were not handed over");
    }
    else
    {
        ev->strip    = NULL;
        ev->glyph_w  = 0U;
        ev->glyph_h  = 0U;
        ev->x        = 0U;
        ev->y        = 0U;
        ev->colon_w  = 0U;
        ev->rest_pct = 0U;
        ev->h12      = false;
        (void)memset(ev->adv, 0, sizeof(ev->adv));
        /* A round's hand-over: a park's carries its number (kode_sys_screen_parked()). */
        ev->park     = 0U;
        if ( NULL != e )
        {
            ev->strip    = e->strip;
            ev->glyph_w  = e->glyph_w;
            ev->glyph_h  = e->glyph_h;
            ev->x        = e->x;
            ev->y        = e->y;
            ev->colon_w  = e->colon_w;
            ev->rest_pct = e->rest_pct;
            ev->h12      = e->h12;
            (void)memcpy(ev->adv, e->adv, sizeof(ev->adv));
        }
        QACTIVE_POST(AO_Display, &ev->super, (void *)0);
    }
}

void display_test_request(void)
{
    static const QEvt req = QEVT_INITIALIZER(DISPLAY_TEST_REQ_SIG);
    QACTIVE_POST(AO_Display, &req, (void *)0);
}

/** @brief Where the glass is in its rest: the board's, or an app's sleep. */
typedef enum {
    REST_AWAKE = 0,   /**< Lit and driven; nothing below applies.            */
    REST_PARKING,     /**< A round or an app's sleep is open and the renderer
                       *   has not yet handed over: flushes still reach the
                       *   glass (the renderer draws its last page), the link
                       *   is still up.                                       */
    REST_REFUSED,     /**< The round was answered busy, or an app's sleep
                       *   failed half way: flushes stay refused until the
                       *   glass is put back and repaints.                   */
    REST_ASLEEP,      /**< Dimmed, the link down, the panel showing its
                       *   memory. The board's rest only.                    */
    REST_OFF          /**< The panel in sleep-in and the link down.          */
} rest_t;

#ifdef CONFIG_KODE_LOWPOWER
/** @brief What the expander's owner has been asked for the panel's supply, and not yet answered. */
enum {
    SUPPLY_ASK_NONE = 0,   /**< Nothing.                                     */
    SUPPLY_ASK_OFF,        /**< P7 low: the supply taken for an app's rest.  */
    SUPPLY_ASK_ON          /**< P7 high: the supply on its way back.         */
};
#endif

typedef struct {
    QActive super;

    /* The panel's supply is P7 of the expander, asked of its owner by event
     * (the level, then the direction); the panel is touched only when both
     * answers are back. A state request meanwhile is deferred here. */
    QEQueue     deferq;
    QEvtPtr     defer_sto[4];
    uint8_t     supply_step;   /**< Which of the two answers is awaited. */
#if CONFIG_KODEOS_MIRROR
    QTimeEvt          tick;      /**< One-shot mirror pacer; re-armed after
                                  *   every pass - see mirror_pass().       */
    kmirror_credit_t  credit;    /**< The host's room, in frames.           */
    bool              on;        /**< A mirror_start is in effect, ticking or
                                  *   paused. Cleared only by mirror_stop and
                                  *   a transport that is gone.              */
    bool              paused;    /**< A stall suspended the stream: no tick,
                                  *   no frame buffer, @c on still true. Any
                                  *   DISPLAY_MIRROR_ACK_SIG clears it.      */
    bool              codec;     /**< kdrv_jpeg_init() succeeded at boot. A
                                  *   codec that is not up answers every
                                  *   encode KODE_DENIED, which a shot must
                                  *   not pass on as "busy".                */
    uint8_t           scale;     /**< 1 or 2.                               */
    uint8_t           quality;   /**< 1..100.                               */
    bool              first;     /**< The next frame must be the whole panel:
                                  *   the host has just attached or revived
                                  *   a pause, and has no canvas to delta.   */
    uint16_t          stall_ticks; /**< Consecutive ticks waiting on a credit
                                    *   or sending a frame that made no
                                    *   progress; see KMIRROR_STALL_TICKS.  */
    uint32_t          sent;        /**< Frames started this session, for
                                    *   mirror_info; reset by mirror_start.  */
    uint32_t          stalls;      /**< Times this session has paused - the
                                    *   same reset as @c sent.               */

    /* One frame in flight at a time, copied whole out of the codec's own
     * borrowed buffer before a single record leaves - see
     * mirror_try_start() for why. */
    bool     sending;              /**< A frame is mid-emission.            */
    bool     ble;                  /**< This frame goes over the BLE binary
                                    *   characteristic, not base64 records.
                                    *   Decided once when the frame starts,
                                    *   so one frame never spans two
                                    *   transports.                          */
    uint32_t frame_len;            /**< Header + JPEG bytes, this frame.    */
    uint32_t frame_pos;            /**< How far mirror_send_slice() has
                                    *   reached.                            */
    uint8_t *frame;                /**< The header, then the JPEG:
                                    *   KMIRROR_FRAME_CAP bytes of PSRAM from
                                    *   DISPLAY_MIRROR_START_SIG to
                                    *   mirror_free_frame(). NULL while the
                                    *   mirror is off or paused.             */

    /* A dirty band taken from the driver but not yet encoded (the codec or
     * the PPA was busy). kdrv_display_dirty_take() resets its tracker on each
     * call, so the band is kept here for a retry. y1 >= y2: nothing pending. */
    int32_t  pend_y1;
    int32_t  pend_y2;
#endif /* CONFIG_KODEOS_MIRROR */

    /* The `display` row: a substate of active that answers every GUI flush
     * with flush_abort() and paints the panel itself, one band per
     * self-posted step, then asks the bench about each field. */
    uint8_t   test_field;      /**< DISPLAY_FIELD_* being painted or asked
                                *   about.                                   */
    uint8_t   test_stage;      /**< TEST_STAGE_*: the opening screen, the
                                *   renderer's last frame, or the fields.    */
    bool      test_asking;     /**< The field is on the glass and its
                                *   question is out: a press is an answer.   */
    bool      flush_owed;      /**< The renderer is owed one completion: one
                                *   it had not collected when a field started
                                *   going up, or a flush it asked for since.
                                *   Paid when the last band has landed.      */
    QTimeEvt  test_wait;       /**< DISPLAY_TEST_WAIT_SIG: the end of the
                                *   opening screen or of the renderer's last
                                *   frame, then the operator's deadline for
                                *   each field.                              */
    bool      test_wait_live;  /**< An arming is outstanding.                */
    uint32_t  test_wait_stale; /**< Expiries queued for a field that was
                                *   already answered.                        */
    uint16_t *band;            /**< KBSP_LCD_H_RES by KODE_DISPLAY_TEST_BAND_
                                *   ROWS pixels of one colour, PSRAM, held
                                *   for the row and no longer.               */
    int32_t   test_y;          /**< The next band's first row.               */

    /* The look at the glass (display_health()), with or without the rest. */
    QTimeEvt  health;          /**< DISPLAY_HEALTH_SIG: the glass, once a second. */
    uint32_t  health_frames;   /**< Frames at the last look. */
    uint32_t  health_dpi;      /**< Pixel-path errors at the last look. */
    uint8_t   health_bad;      /**< Looks in a row that went wrong. */
    uint16_t  retries;         /**< Looks since the first bring-up failed. */
    uint8_t   cycle;           /**< Where the supply's off and on is. */

    /* The rest. The framebuffers are freed while the link is down, so the
     * glass is parked before it goes: a renderer that has asked for the
     * panel's state or flushed is assumed to exist, and the link stays up
     * until it has handed over its glyphs (or none), which is how it says it
     * is no longer drawing. */
    rest_t    rest;            /**< Where the glass is.                       */
    bool      app_sleep;       /**< The rest in hand is an app's, asked with
                                *   DISPLAY_SLEEP_REQ_SIG, not a round's.     */
    bool      wake_open;       /**< The app's wake is not answered yet: the
                                *   glass is still being put back.            */
    uint8_t   ask_who;         /**< The DISPLAY_SLEEP_WHO_* of the request in
                                *   hand: what its answer carries.            */
    uint8_t   open_who;        /**< The same for the request that is open: a
                                *   sleep parking its renderer, or a wake not
                                *   answered yet.                             */
    bool      renderer_seen;   /**< Somebody asked for the panel's state or
                                *   flushed: a renderer exists.              */
    bool      panel_asleep;    /**< kdrv_display_panel_sleep(true) is in effect. */
    QTimeEvt  glyphs_tmo;      /**< DISPLAY_GLYPHS_TMO_SIG: the renderer's
                                *   time to hand over.                        */
    bool      glyphs_tmo_live; /**< An arming is outstanding.                  */
    uint32_t  park_seq;        /**< The last DISPLAY_PARK_SIG's number: only
                                *   a hand-over that carries it settles the
                                *   app's sleep. Never 0 once one is sent.   */
    bool      repaint_owed;    /**< A round reached a renderer: whatever
                                *   became of it, the AWAKE that ends it
                                *   publishes DISPLAY_REPAINT_SIG once.       */
    uint32_t  glyphs_tmo_stale; /**< Expiries queued for a wait already over. */
    QTimeEvt  restore_tmo;     /**< DISPLAY_RESTORE_SIG: the glass is put back
                                *   again after a wake that failed.           */
    bool      restore_owed;    /**< The last wake could not put the glass back;
                                *   the retry timer is running.               */
#if CONFIG_KODE_LOWPOWER
    /* The board's rounds. */
    uint8_t   lp_who;          /**< The id lowpower_join() gave.              */
    uint32_t  lp_round;        /**< The round being answered.                 */
    bool      renderer_late;   /**< That round was refused for a renderer that
                                *   handed nothing over: the glass never moved,
                                *   and asked again it waits for the renderer
                                *   again (rest_round()).                     */
    uint8_t   rest_target;     /**< The kode_lp_state_t of the open round.    */
    bool      supply_off;      /**< An app's rest took the panel's supply: the
                                *   way back powers it and starts it cold.    */
    bool      supply_rising;   /**< P7 is high and the rail is rising.        */
    uint8_t   supply_ask;      /**< A SUPPLY_ASK_*: what the expander's owner
                                *   has been asked and not yet answered.      */
    uint32_t  supply_round;    /**< The round a supply taken away answers.    */
    bool      have_glyphs;     /**< @c glyphs holds a usable hand-over.        */
    DisplayGlyphsEvt glyphs;   /**< The last hand-over; its strip is the
                                *   sender's, valid until the next AWAKE.     */
#endif
} Display;

static Display l_display;
QActive * const AO_Display = &l_display.super;

static QState Display_initial(Display * const me, void const * const par);
static QState Display_powering(Display * const me, QEvt const * const e);
static QState Display_active(Display * const me, QEvt const * const e);
static QState Display_testing(Display * const me, QEvt const * const e);

void Display_ctor(void)
{
    QActive_ctor(&l_display.super, Q_STATE_CAST(&Display_initial));
    QEQueue_init(&l_display.deferq, l_display.defer_sto, Q_DIM(l_display.defer_sto));
    l_display.supply_step = 0U;
    l_display.retries = 0U;
    l_display.cycle = 0U;
    l_display.health_frames = 0U;
    l_display.health_dpi = 0U;
    l_display.health_bad = 0U;
    QTimeEvt_ctorX(&l_display.health, &l_display.super, DISPLAY_HEALTH_SIG, 0U);
#if CONFIG_KODEOS_MIRROR
    QTimeEvt_ctorX(&l_display.tick, &l_display.super,
                   DISPLAY_MIRROR_TICK_SIG, 0U);
    l_display.codec = false;
#endif
    l_display.test_field      = (uint8_t)DISPLAY_FIELD_WHITE;
    l_display.test_asking     = false;
    l_display.flush_owed      = false;
    QTimeEvt_ctorX(&l_display.test_wait, &l_display.super,
                   DISPLAY_TEST_WAIT_SIG, 0U);
    l_display.test_wait_live  = false;
    l_display.test_wait_stale = 0U;
    l_display.band            = NULL;
    l_display.test_y          = 0;
    QTimeEvt_ctorX(&l_display.glyphs_tmo, &l_display.super,
                   DISPLAY_GLYPHS_TMO_SIG, 0U);
    l_display.rest             = REST_AWAKE;
    l_display.app_sleep        = false;
    l_display.wake_open        = false;
    l_display.ask_who          = DISPLAY_SLEEP_WHO_APP;
    l_display.open_who         = DISPLAY_SLEEP_WHO_APP;
    l_display.renderer_seen    = false;
    l_display.panel_asleep     = false;
    l_display.glyphs_tmo_live  = false;
    l_display.park_seq         = 0U;
    l_display.repaint_owed     = false;
    l_display.glyphs_tmo_stale = 0U;
    QTimeEvt_ctorX(&l_display.restore_tmo, &l_display.super,
                   DISPLAY_RESTORE_SIG, 0U);
    l_display.restore_owed     = false;
#if CONFIG_KODE_LOWPOWER
    l_display.lp_who           = KODE_LOWPOWER_WHO_NONE;
    l_display.lp_round         = 0U;
    l_display.renderer_late    = false;
    l_display.rest_target      = (uint8_t)KODE_LP_AWAKE;
    l_display.supply_off       = false;
    l_display.supply_rising    = false;
    l_display.supply_ask       = SUPPLY_ASK_NONE;
    l_display.supply_round     = 0U;
    l_display.have_glyphs      = false;
#endif
}

AO_STORAGE(display, KQP_AO_QUEUE_LEN, KQP_AO_STACK_LARGE);

#if CONFIG_KODEOS_AUTOTEST
/* ---- The self-test rows: registered by kaos_display_register() ---------------- */

/** @brief The display's row: AO_Display runs it and answers TEST_RESULT_SIG. */
static bool display_row_display(void)
{
    display_test_request();
    return false;
}

/**
 * @brief The capture buffer and its header, judged by the driver that wrote it.
 *
 * The body lives in kdrv_display.c: the layout it judges is described nowhere
 * above kdrv.
 */
static bool display_row_screenshot(void)
{
    return (kdrv_display_capture_selftest() == KODE_OK);
}

#if CONFIG_KODEOS_MIRROR
/**
 * @brief The encoder round-trip, judged by the driver that ran it.
 *
 * The body lives in kdrv_jpeg.c: the SOI/EOI framing it checks is that
 * driver's business.
 */
static bool display_row_jpeg(void)
{
    kode_status_t const st = kdrv_jpeg_selftest();
    if ( KODE_DENIED == st )
    {
        /* Contention, not a broken codec: AO_Display was mid-encode. The row
         * reports only pass or fail, so the log says which. */
        KLOG_W(TAG, "test jpeg: the codec is busy, not broken - try again");
        return false;
    }
    return (KODE_OK == st);
}
#endif /* CONFIG_KODEOS_MIRROR */
#endif

void kaos_display_register(void)
{
#if CONFIG_KODEOS_AUTOTEST
    static const krow_t DISPLAY_ROWS[] = {
        { "screenshot", AUTOTEST_ID_SCREENSHOT, display_row_screenshot, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Screen capture",
          "Capture not working",
          AUTOTEST_ORDER_SCREENSHOT },
#if CONFIG_KODEOS_MIRROR
        { "jpeg", AUTOTEST_ID_JPEG, display_row_jpeg, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Image encoder",
          "Encoder not working",
          AUTOTEST_ORDER_JPEG },
#endif
        { "display", AUTOTEST_ID_DISPLAY, display_row_display, KROW_ASYNC, AUTOTEST_G_NONE, KODE_DISPLAY_TEST_BUDGET_MS,
          NULL, 0U, "Screen",
          "Screen problem",
          AUTOTEST_ORDER_DISPLAY },
#if defined(CONFIG_KODE_COMPOSE)
        { "compose", AUTOTEST_ID_COMPOSE, display_compose_row, KROW_SYNC, AUTOTEST_G_NONE, 0U,
          NULL, 0U, "Pixel accelerator",
          "Accelerator problem",
          AUTOTEST_ORDER_COMPOSE },
#endif
    };

    kaos_require(krows_add(DISPLAY_ROWS, sizeof(DISPLAY_ROWS) / sizeof(DISPLAY_ROWS[0])),
                 "display rows");
#endif
#if CONFIG_KODEOS_CONSOLE
    kaos_require(kverbs_add(&display_verbs), "display verbs");
#endif
}

void kaos_display_start(void)
{
    Display_ctor();
    kaos_display_register();
    AO_START(AO_Display, display, KODE_PRIO_DISPLAY);
#if CONFIG_KODEOS_CONSOLE
    /* The settings this AO answers, registered once it has the queue their
     * requests are posted to: an app that does not link it has no such keys. */
    static const kode_config_t DISPLAY_CONFIG[] = {
        { .name = KSET_KEY_BRIGHT, .type = KODE_TYPE_INT,
          .min = (int32_t)KSET_BRIGHT_MIN, .max = (int32_t)KSET_PCT_MAX },
    };

    kaos_require(KODE_CONFIG_ADD(AO_Display, KQP_DISPLAY_CMD_MARGIN, DISPLAY_CONFIG),
                 "display settings");
#endif
}

/**
 * @brief Disarm a one-shot and say whether its expiry is still queued.
 *
 * A timer that already fired has an event queued that belongs to nothing;
 * acting on it would end the window that replaced the one it was timing.
 *
 * @param te   The one-shot.
 * @param live Whether the caller considers a previous arming outstanding.
 * @return true when an expired event for @p te is still queued.
 */
static bool timer_stale(QTimeEvt * const te, bool live)
{
    bool stale = false;

    if ( !QTimeEvt_disarm(te) )
    {
        stale = live;
    }
    return stale;
}

/* ---- The glass at rest -----------------------------------------------------
 * Two askers rest the glass. An app, with kode_sys_screen_sleep(): the panel
 * in sleep-in and the link down (REST_OFF) until it asks for the glass awake.
 * And, in a build with AO_LowPower, the board's rounds: ASLEEP (the glass
 * dimmed, the link down, the panel showing its memory, where the time is
 * written each minute, rest_minute()) or OFF.
 * The link takes the framebuffers with it, so a renderer that has flushed is
 * parked first: the link stays up until DISPLAY_GLYPHS_SIG says it stopped (a
 * NULL strip: nothing to give). On the way back, DISPLAY_REPAINT_SIG makes it
 * repaint the whole glass.
 * The board's rest has the glass first: a round takes it as an app left it,
 * and the AWAKE that ends the rest wakes it.
 */

/**
 * @brief Why the glass cannot rest now.
 * @param me The AO.
 * @return A busy reason (at most KODE_LOWPOWER_WHY_MAX - 1 characters), or
 *         NULL when nothing holds the glass.
 */
static const char *rest_busy_why(const Display * const me)
{
    const char *why = NULL;

    if ( (NULL != s_capture) || s_shot_lent )
    {
        why = "glass lent";
    }
#if CONFIG_KODEOS_MIRROR
    if ( me->on )
    {
        why = "mirror running";
    }
#else
    (void)me;
#endif
    return why;
}

/** @brief Stop the renderer's wait, counting an expiry that is already queued. */
static void glyphs_wait_stop(Display * const me)
{
    if ( timer_stale(&me->glyphs_tmo, me->glyphs_tmo_live) )
    {
        me->glyphs_tmo_stale += 1U;
    }
    me->glyphs_tmo_live = false;
}

/**
 * @brief Note where the glass is, for this AO and for display_asleep()'s readers.
 * @param me   The AO.
 * @param rest Where the glass is from now on.
 */
static void rest_set(Display * const me, rest_t rest)
{
    me->rest = rest;
    display_sleep_glass_set((REST_ASLEEP == rest) || (REST_OFF == rest));
}

/**
 * @brief Say how one DISPLAY_SLEEP_REQ_SIG ended: the word a task reads, then
 *        DISPLAY_SLEEP_SIG on the bus.
 *
 * The answer carries the door its request came through. The word is the
 * app's door's alone: a task waiting there never counts an answer to
 * AO_LowPower's request, and AO_LowPower reads only its own on the bus.
 *
 * @param me  The AO.
 * @param who The DISPLAY_SLEEP_WHO_* of the request answered.
 * @param rc  KODE_OK when the glass is as the request asked.
 * @param why Why not, when @p rc is not KODE_OK.
 */
/* cppcheck-suppress misra-c2012-2.7 ; @p me is the publish's sender, which only a QS build reads */
static void sleep_answer(Display * const me, uint8_t who, kode_status_t rc,
                         display_sleep_why_t why)
{
    DisplaySleepEvt *ev;

    if ( KODE_OK != rc )
    {
        KLOG_N(TAG, "the glass stays as it is: %s", display_sleep_why_text((uint8_t)why));
    }
    if ( DISPLAY_SLEEP_WHO_APP == who )
    {
        display_sleep_answer_set(rc, why);
    }
    ev = Q_NEW_X(DisplaySleepEvt, 0U, DISPLAY_SLEEP_SIG);
    if ( NULL == ev )
    {
        /* A task still reads the word; an AO asks again when it hears nothing. */
        KLOG_E(TAG, "out of events; a request's answer was not published");
    }
    else
    {
        ev->asleep = display_asleep();
        ev->rc     = (uint8_t)rc;
        ev->why    = (uint8_t)why;
        ev->who    = who;
        QACTIVE_PUBLISH(&ev->super, &me->super);
    }
}

/**
 * @brief The panel into sleep-in, then its link down; the renderer is parked.
 *
 * The sleep-in rides the link, so it goes first; a panel already asleep is
 * left alone.
 *
 * @param me The AO, with the link up.
 * @return NULL when both are done, or which of the two refused.
 */
static const char *glass_off(Display * const me)
{
    const char *failed = NULL;

    if ( !me->panel_asleep )
    {
        me->panel_asleep = (kdrv_display_panel_sleep(true) == KODE_OK);
        if ( !me->panel_asleep )
        {
            failed = "display panel";
        }
    }
    if ( (NULL == failed) && (kdrv_display_link_set(false) != KODE_OK) )
    {
        failed = "display link";
    }
    return failed;
}

#ifdef CONFIG_KODE_LOWPOWER
/**
 * @brief Bring back the supply an app's rest took from the panel.
 *
 * As at the boot, the panel is not touched until the expander's owner has
 * said P7 is high (supply_answer()). Then the rail rises for
 * KODE_DISPLAY_SUPPLY_SETTLE_MS, and then the panel starts cold, which leaves
 * it in sleep-in with its registers loaded, where rest_restore() finds a
 * panel that only slept. Each step is taken on the restore timer, which also
 * asks again when no answer came and tries a cold start again that failed.
 *
 * The reset line is the touch controller's too. In an app's rest AO_LowPower
 * tells the touch's owner to restore only after this wake is answered. On the
 * board's AWAKE both owners hear it at once: the touch's writes find the chip
 * in reset and its owner tries again every KODE_TOUCH_RESTORE_RETRY_MS, which
 * is after the cold start here.
 *
 * @param me The AO, the glass at rest with its supply off.
 * @return true when the panel is powered and loaded; false while a step is
 *         still to come, the restore timer armed for it.
 */
static bool supply_back(Display * const me)
{
    bool back = false;

    if ( !me->supply_rising )
    {
        ioexp_field_request(&me->super, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 1);
        me->supply_ask = SUPPLY_ASK_ON;
        KLOG_N(TAG, "the panel's supply is asked back (P7 high)");
    }
    else if ( kdrv_display_panel_cold() == KODE_OK )
    {
        me->supply_rising = false;
        me->supply_off    = false;
        back              = true;
    }
    else
    {
        KLOG_E(TAG, "the panel did not start cold; trying again in %u ms",
               (unsigned)KODE_DISPLAY_RESTORE_RETRY_MS);
    }
    if ( !back )
    {
        me->restore_owed = true;
        (void)QTimeEvt_disarm(&me->restore_tmo);
        QTimeEvt_armX(&me->restore_tmo, KQP_TICKS_PER_MS(KODE_DISPLAY_RESTORE_RETRY_MS), 0U);
    }
    return back;
}

/**
 * @brief IOEXP_FIELD_RSP_SIG: the expander's owner answered for the panel's supply.
 *
 * Taken away: the app's rest is answered only now, with what the expander
 * did. A supply that would not go leaves the panel in reset all the same, so
 * the way back still starts it cold. Asked back: the rail's rise begins here,
 * and a refusal is asked again on the restore timer.
 *
 * @param me The AO.
 * @param a  The answer.
 */
static void supply_answer(Display * const me, const IoexpFieldEvt * const a)
{
    bool const    ok  = ((uint8_t)KODE_OK == a->rc);
    uint8_t const ask = me->supply_ask;

    me->supply_ask = SUPPLY_ASK_NONE;
    if ( (uint16_t)KDRV_TCA_O7 != a->field )
    {
        me->supply_ask = ask;   /* another field's answer: still waiting */
    }
    else if ( SUPPLY_ASK_OFF == ask )
    {
        me->supply_off    = true;
        me->supply_rising = false;
        if ( ok )
        {
            KLOG_N(TAG, "the panel's supply is gone for the app (P7 low)");
        }
        else
        {
            KLOG_W(TAG, "the expander would not take the panel's supply away (rc %u)",
                   (unsigned)a->rc);
        }
        lowpower_ready(me->lp_who, me->supply_round, !ok, "panel supply");
    }
    else if ( (SUPPLY_ASK_ON == ask) && ok )
    {
        me->supply_rising = true;
        me->restore_owed  = true;
        (void)QTimeEvt_disarm(&me->restore_tmo);
        QTimeEvt_armX(&me->restore_tmo, KQP_TICKS_PER_MS(KODE_DISPLAY_SUPPLY_SETTLE_MS), 0U);
    }
    else if ( SUPPLY_ASK_ON == ask )
    {
        /* The restore timer asks again. */
        KLOG_E(TAG, "the expander would not give the panel its supply back (rc %u)",
               (unsigned)a->rc);
    }
    else
    {
        /* Nothing was asked: an answer that outlived its question. */
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief Put the glass back, whatever part of the rest it got to: for
 *        STATE(AWAKE), for an app's wake, and to undo a sleep that failed.
 *
 * Idempotent: with the glass awake and no repaint owed it does nothing. A
 * failed step is retried on a short timer (rest_restore_retry()), as no
 * further AWAKE is coming. Every round a renderer parked for ends in one
 * DISPLAY_REPAINT_SIG, even one refused before the glass moved. An app's wake
 * is answered here, after the repaint, once the glass is back.
 *
 * @param me The AO.
 */
static void rest_restore(Display * const me)
{
    static const QEvt repaint = QEVT_INITIALIZER(DISPLAY_REPAINT_SIG);

    if ( REST_AWAKE == me->rest )
    {
        if ( me->repaint_owed )
        {
            /* The round was refused before the glass moved: the renderer
             * parked all the same and is waiting to be told it may draw. */
            me->repaint_owed = false;
            QACTIVE_PUBLISH(&repaint, &me->super);
        }
    }
    else
    {
        bool ok = true;

        glyphs_wait_stop(me);
#ifdef CONFIG_KODE_LOWPOWER
        if ( SUPPLY_ASK_OFF == me->supply_ask )
        {
            /* The expander's owner has not said the supply is gone, and may
             * never say it: the reset line is low and P7 was asked low, so
             * the panel starts cold all the same. */
            me->supply_ask = SUPPLY_ASK_NONE;
            me->supply_off = true;
        }
        if ( me->supply_off && !supply_back(me) )
        {
            /* The restore timer comes back here once the rail has risen. */
            return;
        }
#endif
        /* Always asked for, not only after a link this AO saw go down: a
         * link_set(false) that answers an error leaves the link down anyway,
         * and link_set(true) is a no-op where the link is already up. */
        ok = (kdrv_display_link_set(true) == KODE_OK);
        if ( ok && me->panel_asleep )
        {
            ok = (kdrv_display_panel_sleep(false) == KODE_OK);
            me->panel_asleep = !ok;
        }
        if ( ok )
        {
            /* Over a panel that stayed awake the driver writes it at once,
             * onto the picture the glass kept; over one that slept it keeps it
             * until the first frame lights the glass. */
            (void)kdrv_display_brightness(s_state_evt.bright);
#if CONFIG_KODE_LOWPOWER
            me->have_glyphs  = false;
#endif
            rest_set(me, REST_AWAKE);
            me->repaint_owed = false;
            me->restore_owed = false;
            me->app_sleep    = false;
            QACTIVE_PUBLISH(&repaint, &me->super);
            if ( me->wake_open )
            {
                me->wake_open = false;
                KLOG_N(TAG, "glass awake again for the app");
                sleep_answer(me, me->open_who, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
            }
        }
        else
        {
            /* No later AWAKE or wake is coming for a board that stays awake,
             * so the glass is asked again on a short timer, while it stays dark. */
            KLOG_E(TAG, "the glass did not come back; trying again in %u ms",
                   (unsigned)KODE_DISPLAY_RESTORE_RETRY_MS);
            me->restore_owed = true;
            (void)QTimeEvt_disarm(&me->restore_tmo);
            QTimeEvt_armX(&me->restore_tmo,
                          KQP_TICKS_PER_MS(KODE_DISPLAY_RESTORE_RETRY_MS), 0U);
        }
    }
}

/**
 * @brief DISPLAY_RESTORE_SIG: put the glass back again.
 *
 * An expiry that outlived the retry it belonged to finds nothing owed and
 * does nothing.
 *
 * @param me The AO.
 */
static void rest_restore_retry(Display * const me)
{
    if ( me->restore_owed )
    {
        me->restore_owed = false;
        rest_restore(me);
    }
}

/**
 * @brief The renderer is parked (or none exists): sleep the glass for the app
 *        and answer its request.
 *
 * A step that fails leaves the glass awake: what was done is undone, and if
 * that fails too the restore timer goes on trying while the asker hears no.
 *
 * @param me The AO, parking for an app's sleep.
 */
static void sleep_settle(Display * const me)
{
    const char * const failed = glass_off(me);

    if ( NULL == failed )
    {
        rest_set(me, REST_OFF);
        KLOG_N(TAG, "glass asleep for the app: panel asleep, link down");
        sleep_answer(me, me->open_who, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    }
    else
    {
        KLOG_W(TAG, "the glass could not sleep: %s; it is put back", failed);
        me->app_sleep = false;
        rest_set(me, REST_REFUSED);
        rest_restore(me);
        sleep_answer(me, me->open_who, KODE_IO_ERROR, DISPLAY_SLEEP_WHY_PANEL);
    }
}

/**
 * @brief Ask the renderer to stop drawing for an app's sleep, and wait for it.
 *
 * Each park has its own number, so only the answer to this one settles the
 * sleep: an answer to an earlier park, late or never sent, settles nothing.
 * With no event free to ask, the sleep is refused and the glass stays lit.
 *
 * @param me The AO, awake, with a renderer seen.
 */
static void sleep_park(Display * const me)
{
    DisplayParkEvt *park = Q_NEW_X(DisplayParkEvt, 0U, DISPLAY_PARK_SIG);

    if ( NULL == park )
    {
        sleep_answer(me, me->open_who, KODE_NO_RESOURCES, DISPLAY_SLEEP_WHY_EVENTS);
    }
    else
    {
        me->park_seq += 1U;
        if ( 0U == me->park_seq )
        {
            /* 0 is a round's hand-over. */
            me->park_seq = 1U;
        }
        park->seq     = me->park_seq;
        me->app_sleep = true;
        rest_set(me, REST_PARKING);
        QACTIVE_PUBLISH(&park->super, &me->super);
        QTimeEvt_armX(&me->glyphs_tmo, KQP_TICKS_PER_MS(KODE_LOWPOWER_GLYPHS_WAIT_MS), 0U);
        me->glyphs_tmo_live = true;
    }
}

/**
 * @brief An app asks for the glass asleep.
 *
 * From awake the renderer is parked first: DISPLAY_PARK_SIG, then up to
 * KODE_LOWPOWER_GLYPHS_WAIT_MS for its DISPLAY_GLYPHS_SIG, with the glass lit
 * and the link up meanwhile. With no renderer seen there is nothing to wait
 * for. A glass already at rest, by an app or by the board, is as asked.
 *
 * @param me The AO, with the panel up.
 */
static void sleep_down(Display * const me)
{
    if ( me->restore_owed )
    {
        /* The glass is still being put back. */
        sleep_answer(me, me->ask_who, KODE_DENIED, DISPLAY_SLEEP_WHY_UNDERWAY);
    }
    else if ( REST_AWAKE == me->rest )
    {
        if ( NULL != rest_busy_why(me) )
        {
            sleep_answer(me, me->ask_who, KODE_DENIED, DISPLAY_SLEEP_WHY_LENT);
        }
        else if ( !me->renderer_seen )
        {
            me->open_who  = me->ask_who;
            me->app_sleep = true;
            rest_set(me, REST_PARKING);
            sleep_settle(me);
        }
        else
        {
            me->open_who = me->ask_who;
            sleep_park(me);
        }
    }
    else if ( (REST_ASLEEP == me->rest) || (REST_OFF == me->rest) )
    {
        sleep_answer(me, me->ask_who, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    }
    else if ( me->app_sleep )
    {
        /* The renderer is being parked for an earlier request. */
        sleep_answer(me, me->ask_who, KODE_DENIED, DISPLAY_SLEEP_WHY_UNDERWAY);
    }
    else
    {
        /* A round is open, or was refused and its AWAKE has not come. */
        sleep_answer(me, me->ask_who, KODE_DENIED, DISPLAY_SLEEP_WHY_BOARD);
    }
}

/**
 * @brief An app asks for the glass awake.
 *
 * A sleep still parking the renderer ends there, its asker told first. A glass
 * asleep for an app is put back, and the answer waits for it: at once, or at
 * the retry that works. A glass the board rests is not the app's to wake.
 *
 * @param me The AO, with the panel up.
 */
static void sleep_up(Display * const me)
{
    if ( REST_AWAKE == me->rest )
    {
        sleep_answer(me, me->ask_who, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    }
    else if ( me->restore_owed )
    {
        /* The glass is being put back already. */
        sleep_answer(me, me->ask_who, KODE_DENIED, DISPLAY_SLEEP_WHY_UNDERWAY);
    }
    else if ( !me->app_sleep )
    {
        sleep_answer(me, me->ask_who, KODE_DENIED, DISPLAY_SLEEP_WHY_BOARD);
    }
    else if ( REST_PARKING == me->rest )
    {
        static const QEvt repaint = QEVT_INITIALIZER(DISPLAY_REPAINT_SIG);

        glyphs_wait_stop(me);
        me->app_sleep = false;
        rest_set(me, REST_AWAKE);
        /* The renderer may have parked already. */
        QACTIVE_PUBLISH(&repaint, &me->super);
        sleep_answer(me, me->open_who, KODE_DENIED, DISPLAY_SLEEP_WHY_WOKEN);
        sleep_answer(me, me->ask_who, KODE_OK, DISPLAY_SLEEP_WHY_NONE);
    }
    else
    {
        me->open_who  = me->ask_who;
        me->wake_open = true;
        rest_restore(me);
    }
}

/**
 * @brief DISPLAY_SLEEP_REQ_SIG: sleep the glass or wake it, and answer once.
 *
 * @param me The AO.
 * @param r  The request.
 */
static void sleep_request(Display * const me, const DisplaySleepReqEvt * const r)
{
    me->ask_who = r->who;
    if ( !s_state_evt.up )
    {
        sleep_answer(me, me->ask_who, KODE_DENIED, DISPLAY_SLEEP_WHY_NO_PANEL);
    }
    else if ( r->sleep )
    {
        sleep_down(me);
    }
    else
    {
        sleep_up(me);
    }
}

/**
 * @brief DISPLAY_GLYPHS_SIG while an app's sleep has the glass: the renderer
 *        has stopped drawing.
 *
 * @param me The AO, with app_sleep set.
 */
static void sleep_glyphs(Display * const me)
{
    if ( REST_PARKING == me->rest )
    {
        glyphs_wait_stop(me);
        sleep_settle(me);
    }
    else
    {
        KLOG_D(TAG, "a second hand-over for a glass already asleep; ignored");
    }
}

#if CONFIG_KODE_LOWPOWER
/* ---- The board's rounds ----------------------------------------------------
 * The dimming level of ASLEEP comes with the glyphs, so the glass is dimmed
 * just before the link goes; a renderer that never hands over leaves it lit.
 */

/** @brief Microseconds in a second: the wall clock counts in them. */
#define REST_US_PER_S      1000000ULL

/** @brief Alignment of the minute's buffer: a cache line, for the DMA. */
#define REST_BUF_ALIGN     64U

/** @brief Full brightness: the top of the scale. */
#define REST_PCT_MAX       100U

/** @brief Answer the open round. */
static void rest_answer(const Display * const me, bool busy, const char *why)
{
    lowpower_ready(me->lp_who, me->lp_round, busy, why);
}

/**
 * @brief Write the time into the panel's memory, from the strip.
 *
 * Local time through the zone the face keeps in TZ. Nothing is written when no
 * glyphs were handed or the wall clock is not set: the memory then holds what
 * the renderer last drew.
 *
 * @param me The AO, resting ASLEEP with the link down.
 * @return true when the time reached the panel's memory, or there was nothing
 *         to write; false when the driver refused or timed out, or no buffer
 *         could be had (a retry may do better).
 */
static bool rest_minute(Display * const me)
{
    time_t now;
    struct tm local;
    bool written = true;

    if ( me->have_glyphs && khal_time_wall_known() )
    {
        uint64_t const secs = khal_time_wall_us() / REST_US_PER_S;

        now = (time_t)secs;
        /* cppcheck-suppress misra-c2012-17.3 ; localtime_r is POSIX, which cppcheck's C library model leaves undeclared; kgui calls it the same way */
        if ( localtime_r(&now, &local) != NULL )
        {
            uint16_t const w = display_clock_width(&me->glyphs);
            size_t const bytes = (size_t)w * me->glyphs.glyph_h * sizeof(uint16_t);
            /* cppcheck-suppress misra-c2012-11.5 ; the allocator hands back raw memory, as it does for the mirror's frame and the row's band */
            uint16_t * const buf = (uint16_t *)khal_alloc_psram(REST_BUF_ALIGN, bytes);

            if ( NULL == buf )
            {
                KLOG_W(TAG, "no memory for the minute's rectangle (%u bytes)",
                       (unsigned)bytes);
                written = false;
            }
            else
            {
                display_clock_compose(&me->glyphs, (uint8_t)local.tm_hour,
                                      (uint8_t)local.tm_min, buf);
                kode_status_t const st = kdrv_display_gram_write(
                    me->glyphs.x, me->glyphs.y, w, me->glyphs.glyph_h, buf);
                if ( KODE_OK != st )
                {
                    KLOG_W(TAG, "the minute did not reach the panel (rc %d)", (int)st);
                    written = false;
                }
                khal_free(buf);
            }
        }
    }
    return written;
}

/**
 * @brief The minute's request: write the time, and once more if it failed.
 *
 * A failed write was most likely cut short by a flash erase stopping the cache.
 * The retry waits KODE_LOWPOWER_MINUTE_RETRY_MS in this step, since the QP tick
 * is stopped while the board sleeps. A second failure waits for the next
 * minute; the driver has left the panel's window whole.
 *
 * @param me The AO, resting ASLEEP with the link down.
 */
static void rest_minute_request(Display * const me)
{
    if ( !rest_minute(me) )
    {
        khal_delay_ms(KODE_LOWPOWER_MINUTE_RETRY_MS);
        if ( rest_minute(me) )
        {
            KLOG_N(TAG, "the minute reached the panel at the second try");
        }
        else
        {
            KLOG_W(TAG, "the minute did not reach the panel at the second try");
        }
    }
}

/**
 * @brief The renderer is parked (or none exists): take the glass to rest and
 *        answer the round.
 *
 * ASLEEP: dim, link down, then the time at once, so the glass does not show
 * the last page until the next minute. OFF: sleep-in (bringing the link up for
 * it from ASLEEP), then link down. A failed step answers the round busy, and
 * STATE(AWAKE) undoes what was done.
 *
 * @param me The AO, with rest_target set.
 */
static void rest_settle(Display * const me)
{
    bool ok = true;
    const char *why = "display link";

    if ( (uint8_t)KODE_LP_OFF == me->rest_target )
    {
        if ( REST_ASLEEP == me->rest )
        {
            /* The link is down at the ASLEEP rest, and the sleep-in rides it. */
            ok = (kdrv_display_link_set(true) == KODE_OK);
        }
        if ( ok )
        {
            const char * const failed = glass_off(me);

            if ( NULL != failed )
            {
                ok  = false;
                why = failed;
            }
        }
    }
    else
    {
        uint8_t pct = (uint8_t)KODE_LOWPOWER_DIM_PCT;

        if ( me->have_glyphs )
        {
            pct = me->glyphs.rest_pct;
        }
        if ( kdrv_display_brightness(pct) != KODE_OK )
        {
            KLOG_W(TAG, "the rest brightness (%u%%) did not reach the panel", (unsigned)pct);
        }
        ok = (kdrv_display_link_set(false) == KODE_OK);
    }

    if ( ok )
    {
        if ( (uint8_t)KODE_LP_OFF == me->rest_target )
        {
            rest_set(me, REST_OFF);
            KLOG_N(TAG, "glass at rest: off, panel asleep, link down");
        }
        else
        {
            rest_set(me, REST_ASLEEP);
            KLOG_N(TAG, "glass at rest: asleep, link down");
            (void)rest_minute(me);
        }
        rest_answer(me, false, NULL);
    }
    else
    {
        KLOG_W(TAG, "the glass could not rest: %s", why);
        if ( REST_PARKING == me->rest )
        {
            rest_set(me, REST_REFUSED);
        }
        rest_answer(me, true, why);
    }
}

/**
 * @brief A round opens from AWAKE: park the renderer, then rest.
 *
 * With no renderer seen there is nothing to wait for. With one, the glass
 * stays lit and the link up until DISPLAY_GLYPHS_SIG, and a renderer that does
 * not answer in KODE_LOWPOWER_GLYPHS_WAIT_MS refuses the round.
 *
 * @param me The AO, AWAKE, with rest_target and lp_round set.
 */
static void rest_begin(Display * const me)
{
    const char * const why = rest_busy_why(me);

    if ( NULL != why )
    {
        rest_answer(me, true, why);
    }
    else
    {
        rest_set(me, REST_PARKING);
        if ( me->renderer_seen )
        {
            QTimeEvt_armX(&me->glyphs_tmo,
                          KQP_TICKS_PER_MS(KODE_LOWPOWER_GLYPHS_WAIT_MS), 0U);
            me->glyphs_tmo_live = true;
        }
        else
        {
            rest_settle(me);
        }
    }
}

/**
 * @brief The board's rest takes the glass from an app's request.
 *
 * A sleep still parking the renderer ends refused, and the renderer is owed
 * the repaint that ends the round; a wake still being tried ends refused, and
 * the AWAKE that ends the rest is what wakes the glass; a glass asleep for the
 * app stays as it is, the board's from here on.
 *
 * @param me The AO.
 */
static void sleep_yield(Display * const me)
{
    if ( me->app_sleep )
    {
        me->app_sleep = false;
        if ( REST_PARKING == me->rest )
        {
            glyphs_wait_stop(me);
            rest_set(me, REST_AWAKE);
            me->repaint_owed = true;
            sleep_answer(me, me->open_who, KODE_DENIED, DISPLAY_SLEEP_WHY_BOARD);
        }
        else if ( me->wake_open )
        {
            me->wake_open = false;
            sleep_answer(me, me->open_who, KODE_DENIED, DISPLAY_SLEEP_WHY_BOARD);
        }
        else
        {
            /* Asleep for the app: the round finds the glass at rest. */
        }
    }
}

/**
 * @brief DISPLAY_SUPPLY_REQ_SIG: take the supply of a panel asleep for an app.
 *
 * The panel's power-off order, as at OFF: the reset line low for its time,
 * then the supply (P7). The line is the touch controller's too, so the app's
 * rest has put the touch away first. Only a glass an app put to sleep is
 * taken. The request is answered when the expander's owner says the supply
 * is gone (supply_answer()), and the wake that follows, an app's or the
 * board's AWAKE, brings it back before anything else (supply_back()). A
 * request that finds an earlier one still unanswered asks again: AO_LowPower
 * asks one at a time, so the answer of the earlier one was lost.
 *
 * @param me The AO.
 * @param r  The request; its round is the answer's.
 */
static void supply_request(Display * const me, const DisplaySupplyReqEvt * const r)
{
    if ( me->supply_off )
    {
        lowpower_ready(me->lp_who, r->round, false, NULL);
    }
    else if ( (!me->app_sleep) || (REST_OFF != me->rest) || (!me->panel_asleep) )
    {
        lowpower_ready(me->lp_who, r->round, true, "glass not asleep");
    }
    else if ( SUPPLY_ASK_ON == me->supply_ask )
    {
        lowpower_ready(me->lp_who, r->round, true, "panel supply busy");
    }
    else if ( kdrv_display_reset_low() != KODE_OK )
    {
        lowpower_ready(me->lp_who, r->round, true, "reset line");
    }
    else
    {
        ioexp_field_request(&me->super, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 0);
        me->supply_ask   = SUPPLY_ASK_OFF;
        me->supply_round = r->round;
    }
}

/**
 * @brief LOWPOWER_ROUND_SIG: rest for ASLEEP or OFF, and answer once.
 *
 * AO_LowPower asks a busy owner again in the round it refused, with no AWAKE
 * between the asks. A glass that refused for a renderer that was late had not
 * moved: asked again it is put back as that AWAKE would put it, which gives
 * the renderer its flushes and its repaint, and the wait for the glyphs
 * starts over. The renderer is another owner, asked again in the same way.
 *
 * @param me The AO.
 * @param r  The round.
 */
static void rest_round(Display * const me, const LowPowerRoundEvt * const r)
{
    bool const late_again = me->renderer_late && (r->round == me->lp_round)
                            && (REST_REFUSED == me->rest);

    /* The board's rest has the glass before an app's request. */
    sleep_yield(me);
    me->lp_round      = r->round;
    me->rest_target   = r->target;
    me->renderer_late = false;
    if ( late_again )
    {
        rest_restore(me);
    }
    /* A glass still being put back is not the round's to find half done: the
     * AWAKE that ends this round restores it. */
    me->restore_owed = false;
    (void)QTimeEvt_disarm(&me->restore_tmo);
    if ( me->supply_off && (me->supply_rising || (SUPPLY_ASK_ON == me->supply_ask)) )
    {
        /* The supply was on its way back to a panel still in reset: the board
         * rests without it, and the AWAKE that ends the rest asks again. */
        ioexp_field_request(NULL, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 0);
        me->supply_rising = false;
        me->supply_ask    = SUPPLY_ASK_NONE;
    }
    /* A renderer parks on every round it hears, whether or not the glass
     * goes on to rest, and waits for the repaint to start drawing again. */
    if ( me->renderer_seen )
    {
        me->repaint_owed = true;
    }

    if ( !s_state_evt.up )
    {
        /* A board with no glass has nothing to rest. */
        rest_answer(me, false, NULL);
    }
    else if ( REST_AWAKE == me->rest )
    {
        rest_begin(me);
    }
    else if ( REST_ASLEEP == me->rest )
    {
        /* ASLEEP to OFF: the glass goes from one rest to the other without
         * lighting; ASLEEP again is already true. */
        if ( (uint8_t)KODE_LP_OFF == r->target )
        {
            rest_settle(me);
        }
        else
        {
            rest_answer(me, false, NULL);
        }
    }
    else if ( REST_OFF == me->rest )
    {
        rest_answer(me, false, NULL);
    }
    else
    {
        rest_answer(me, true, "glass not restored");
    }
}

/**
 * @brief DISPLAY_GLYPHS_SIG: the renderer has stopped drawing.
 *
 * @param me The AO.
 * @param g  The hand-over; its strip is NULL when the renderer has no glyphs.
 */
static void rest_glyphs(Display * const me, const DisplayGlyphsEvt * const g)
{
    if ( REST_PARKING != me->rest )
    {
        KLOG_D(TAG, "glyphs handed over outside a round; ignored");
    }
    else
    {
        glyphs_wait_stop(me);
        me->glyphs      = *g;
        me->have_glyphs = display_clock_usable(g);
        if ( (NULL != g->strip) && !me->have_glyphs )
        {
            KLOG_W(TAG, "the glyphs cannot be written to the panel (%ux%u at %u,%u); dark glass",
                   (unsigned)g->glyph_w, (unsigned)g->glyph_h,
                   (unsigned)g->x, (unsigned)g->y);
        }
        if ( (uint8_t)REST_PCT_MAX < me->glyphs.rest_pct )
        {
            me->glyphs.rest_pct = (uint8_t)REST_PCT_MAX;
        }
        rest_settle(me);
    }
}

/**
 * @brief LOWPOWER_STATE_SIG: restore on AWAKE; ASLEEP and OFF were rested for
 *        on the round, and OFF also takes the panel's supply away.
 *
 * STATE(OFF) is the point of no return: deep sleep follows. Only there is
 * VCI_EN (P7) let go, and only with the panel in sleep-in and the link down;
 * a panel that refused sleep-in keeps its supply rather than lose it lit. The
 * panel's power-off order has the reset line low, for at least 1 ms, before
 * the supply: the driver drives the line low and waits that time before this
 * asks the expander's owner for P7. A panel that did not sleep gets neither
 * the line nor the supply from this AO; there the hold alone drives the line
 * low, at the deep sleep (khal_power_off_enter()), and it stays low through
 * it. The expander holds P7 low through deep sleep, so the next boot powers
 * the panel up cold. No answer is asked: if the request misses the grace, the
 * panel stays powered and asleep.
 *
 * @param me The AO.
 * @param s  The change.
 */
static void rest_state(Display * const me, const LowPowerStateEvt * const s)
{
    if ( (uint8_t)KODE_LP_AWAKE == s->state )
    {
        if ( me->app_sleep && (REST_PARKING == me->rest) )
        {
            /* An AWAKE no round came before: the app's sleep ends here, and
             * the renderer it parked is told it may draw. */
            sleep_yield(me);
        }
        rest_restore(me);
    }
    else if ( ((uint8_t)KODE_LP_OFF == s->state) && (REST_OFF == me->rest)
             && me->panel_asleep )
    {
        kode_status_t const st = kdrv_display_reset_low();

        if ( KODE_OK != st )
        {
            KLOG_W(TAG, "OFF: the reset line would not go low (rc %d); the supply goes all the same",
                   (int)st);
        }
        ioexp_field_request(NULL, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 0);
        KLOG_N(TAG, "OFF: the panel's supply goes (P7 low)");
    }
    else
    {
        /* ASLEEP and DARK: the round did it all. */
    }
}
#endif /* CONFIG_KODE_LOWPOWER */

/**
 * @brief DISPLAY_GLYPHS_TMO_SIG: the renderer did not hand over in time.
 *
 * A round is refused and the glass waits, refused, for the AWAKE that follows.
 * An app's sleep ends at once: the glass stays lit and the renderer is told it
 * may draw, in case it stopped late.
 *
 * @param me The AO.
 */
static void rest_glyphs_late(Display * const me)
{
    if ( 0U < me->glyphs_tmo_stale )
    {
        /* The wait of a round or a request already decided. */
        me->glyphs_tmo_stale -= 1U;
    }
    else
    {
        me->glyphs_tmo_live = false;
        if ( REST_PARKING != me->rest )
        {
            /* Nothing is waiting for a renderer. */
        }
        else if ( me->app_sleep )
        {
            static const QEvt repaint = QEVT_INITIALIZER(DISPLAY_REPAINT_SIG);

            me->app_sleep = false;
            rest_set(me, REST_AWAKE);
            KLOG_W(TAG, "the renderer did not park in %u ms; the glass stays lit",
                   (unsigned)KODE_LOWPOWER_GLYPHS_WAIT_MS);
            QACTIVE_PUBLISH(&repaint, &me->super);
            sleep_answer(me, me->open_who, KODE_TIMEOUT, DISPLAY_SLEEP_WHY_RENDERER);
        }
        else
        {
#if CONFIG_KODE_LOWPOWER
            rest_set(me, REST_REFUSED);
            me->renderer_late = true;
            KLOG_W(TAG, "the renderer handed over nothing in %u ms; the glass stays lit",
                   (unsigned)KODE_LOWPOWER_GLYPHS_WAIT_MS);
            rest_answer(me, true, "renderer not parked");
#endif
        }
    }
}

/**
 * @brief Claim the panel, subscribe, and ask the expander's owner for the panel's supply.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to Display_powering.
 */
static QState Display_initial(Display * const me, void const * const par)
{
    (void)par;

    kaos_claim(KBSP_RES_PANEL, me, "AO_Display");
    QActive_subscribe(&me->super, BOOT_KEEP_SIG);
#if CONFIG_KODE_LOWPOWER
    me->lp_who = lowpower_join(&me->super, "display");
    if ( KODE_LOWPOWER_WHO_NONE != me->lp_who )
    {
        QActive_subscribe(&me->super, LOWPOWER_ROUND_SIG);
        QActive_subscribe(&me->super, LOWPOWER_STATE_SIG);
    }
#endif
#if defined(CONFIG_KODEOS_MIRROR) || defined(CONFIG_KODE_COMPOSE)
    /* The accelerator scales the mirror and composes for a renderer. */
    kaos_claim(KBSP_RES_PPA, me, "AO_Display");
#endif
#if CONFIG_KODEOS_MIRROR
    /* The codec is claimed only with the mirror: nothing else touches it. */
    kaos_claim(KBSP_RES_JPEG, me, "AO_Display");
#endif

    /* The panel's supply first, and from its owner: VCI_EN is P7 of the
     * expander. The output level is asked for before the direction so the
     * pin never drives low on its way to high; the chip's reset value is
     * already 1, so on a cold board the first answer changes nothing. */
    me->supply_step = 0U;
    ioexp_field_request(&me->super, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 1);
    return Q_TRAN(&Display_powering);
}

/* The bring-up has had its say. Written by AO_Display, read by kode_init(). */
static volatile bool s_bringup_done;

bool kaos_display_settled(void)
{
    return s_bringup_done;
}

/**
 * @brief The panel, brought up - or not - once its supply has been asked for.
 *
 * Bringing the panel up takes ~100ms of mandated settling delays. Doing it
 * here, from the AO's own thread rather than from kode_init(), keeps it off
 * everyone else's critical path: the other AOs are already running by now.
 *
 * @param me     The AO.
 * @param supply Whether the expander's owner reports P7 driving high.
 */
static void panel_up(Display * const me, bool supply)
{
    s_state_evt.super = (QEvt)QEVT_INITIALIZER(DISPLAY_STATE_SIG);
    s_state_evt.up     = false;
    if ( !supply )
    {
        KLOG_E(TAG, "the expander's owner could not power the panel");
    }
    else
    {
        s_state_evt.up = (kdrv_display_init() == KODE_OK);
    }
    s_bringup_done = true;

#if CONFIG_KODEOS_MIRROR
    /* Never fatal: the board still boots and shows a picture without the
     * encoder. mirror_try_start() then answers MIRROR_BLOCKED, which
     * mirror_pass() turns into one warning and a pause. */
    me->codec = (kdrv_jpeg_init() == KODE_OK);
    if ( !me->codec )
    {
        KLOG_E(TAG, "JPEG codec unavailable; continuing without it");
    }

    /* Never fatal either: without the PPA only the half-scale mirror is lost. */
    if ( kdrv_ppa_init() != KODE_OK )
    {
        KLOG_E(TAG, "PPA unavailable; continuing without it");
    }
#endif

    if ( !s_state_evt.up )
    {
        KLOG_E(TAG, "panel unavailable; kodeOS stays console-only");
    }
    else
    {
        /* The stored brightness goes to the driver before the announcement;
         * the driver writes it with the first frame, so the panel lights once,
         * at its level. No stored value: the default. */
        uint8_t pct = (uint8_t)KSET_BRIGHT_DEF;
        (void)khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, &pct);
        if ( (uint8_t)KSET_BRIGHT_MIN > pct )
        {
            /* An old store never blacks the glass: the control for turning it
             * back up is ON the glass, so a stored 0 would be a board nobody
             * can recover from the settings screen. */
            pct = (uint8_t)KSET_BRIGHT_MIN;
        }
        if ( 100U < pct )
        {
            pct = 100U;
        }
        (void)kdrv_display_brightness(pct);
        /* Announced with the panel: the store is this AO's, and the settings
         * slider must show the level the glass is at. */
        s_state_evt.bright = pct;
        KLOG_I(TAG, "up at %u%%; announcing the panel", (unsigned)pct);
    }

    /* Announced either way. A renderer that hears nothing cannot tell a panel
     * that failed from one that has not answered yet, and would wait for the
     * second forever. */
    QACTIVE_PUBLISH(&s_state_evt.super, &me->super);
    /* The look starts here: it also retries a panel that did not come up. */
    (void)QTimeEvt_rearm(&me->health, KQP_TICKS_PER_MS(100U));
}

#define HEALTH_RESTARTS_MAX 3U
#define HEALTH_BAD_LOOKS    3U
#define HEALTH_DPI_PER_S    20U
#define HEALTH_RETRY_S      10U
#define HEALTH_RETRY_MAX    30U

/**
 * @brief Once a second: is the glass being driven, and is anybody receiving it?
 *
 * A live panel never shows pixel-path errors on most frames (a panel that lost
 * its init or reset) or a scan-out that stopped counting (a wedged host).
 * Either, three looks in a row, restarts the board with the panel's supply cut,
 * at most HEALTH_RESTARTS_MAX times in a row. A panel that never came up is
 * tried again every HEALTH_RETRY_S seconds.
 */
static void display_health(Display * const me)
{
    if ( !s_state_evt.up )
    {
        /* A brownout can latch the module's supply stage, and only its enable
         * clears it (50 ms is enough): P7 down, P7 up, and the bring-up again. */
        uint32_t ms = 1000U;
        ++me->retries;
        if ( 1U == me->cycle )
        {
            ioexp_field_request(&me->super, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 1);
            me->cycle = 2U;
            ms = 150U;
        }
        else if ( 2U == me->cycle )
        {
            me->cycle = 0U;
            panel_up(me, true);
            return;
        }
        else if ( (1U == me->retries) || (0U == (me->retries % HEALTH_RETRY_S)) )
        {
            if ( (HEALTH_RETRY_MAX * HEALTH_RETRY_S) > me->retries )
            {
                KLOG_W(TAG, "no panel: its supply off and on, and again");
                ioexp_field_request(&me->super, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 0);
                me->cycle = 1U;
                ms = 100U;
            }
            else
            {
                /* Nothing will ever show on this glass, and a board awake with
                 * a dark one empties its cell in hours. A cable keeps it up. */
                power_reading_t pw;
                if ( power_reading(&pw) && !pw.vbus )
                {
                    KLOG_E(TAG, "no panel after %u looks, on battery: ship mode", (unsigned)me->retries);
                    (void)power_ship_request();
                }
            }
        }
        else
        {
            /* A look between two tries: nothing to do but wait. */
        }
        (void)QTimeEvt_rearm(&me->health, KQP_TICKS_PER_MS(ms));
        return;
    }
    kdrv_display_link_t l = { 0 };
    kdrv_display_link(&l);
    bool expected = kdrv_display_video_expected();
    /* A glass at rest, the board's or an app's, sends no frames. */
    expected = expected && (REST_AWAKE == me->rest) && !me->panel_asleep;
    bool bad = false;
    if ( expected && (0U != me->health_frames) )
    {
        bad = (l.frames == me->health_frames) || (HEALTH_DPI_PER_S <= (l.dpi - me->health_dpi));
    }
    me->health_frames = expected ? l.frames : 0U;
    me->health_dpi    = l.dpi;
    me->health_bad    = bad ? (uint8_t)(me->health_bad + 1U) : 0U;
    if ( HEALTH_BAD_LOOKS <= me->health_bad )
    {
        uint32_t const restarts = kdrv_display_recoveries();

        if ( HEALTH_RESTARTS_MAX > restarts )
        {
            kdrv_display_recoveries_set(restarts + 1U);
            KLOG_E(TAG, "the glass is lost (frames %lu, dpi %lu): restart %lu of %u with its reset",
                   (unsigned long)l.frames, (unsigned long)l.dpi,
                   (unsigned long)(restarts + 1U), (unsigned)HEALTH_RESTARTS_MAX);
            ioexp_field_request(&me->super, FIELD_OP_SET, (uint16_t)KDRV_TCA_O7, 0);
            khal_delay_ms(100U);
            khal_restart();
        }
        me->health_bad = 0U;
        KLOG_E(TAG, "the glass stays lost after %u restarts; no more", (unsigned)HEALTH_RESTARTS_MAX);
    }
    else if ( (0U == me->health_bad) && expected && (120U < (khal_time_ms() / 1000U)) )
    {
        kdrv_display_recoveries_set(0U);   /* two good minutes: the count starts again */
    }
    else
    {
        /* A bad look not yet repeated, or a glass not expected to scan. */
    }
    (void)QTimeEvt_rearm(&me->health, KQP_TICKS_PER_MS(1000U));
}

/**
 * @brief Waiting for the expander's owner to power the panel.
 *
 * Two answers: the output level, then the direction. A state request that
 * arrives meanwhile is deferred and recalled on entry to Display_active.
 */
static QState Display_powering(Display * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case IOEXP_FIELD_RSP_SIG: {
            IoexpFieldEvt const * const a = Q_EVT_CAST(IoexpFieldEvt);
            bool const ok = ((uint8_t)KODE_OK == a->rc);
            if ( !ok )
            {
                KLOG_E(TAG, "expander refused P7 (field %u, rc %u); no panel",
                       (unsigned)a->field, (unsigned)a->rc);
                panel_up(me, false);
                status = Q_TRAN(&Display_active);
            }
            else if ( 0U == me->supply_step )
            {
                /* The level is set; now make the pin drive it. */
                me->supply_step = 1U;
                ioexp_field_request(&me->super, FIELD_OP_SET, (uint16_t)KDRV_TCA_C7, 0);
                status = Q_HANDLED();
            }
            else
            {
                KLOG_I(TAG, "panel power on (P7 via AO_IoExp)");
                panel_up(me, true);
                status = Q_TRAN(&Display_active);
            }
            break;
        }
        case DISPLAY_STATE_REQ_SIG: {
            /* Kept, not refused: the asker cannot tell "not yet" from "no". */
            (void)QActive_defer(&me->super, &me->deferq, e);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_SLEEP_REQ_SIG: {
            /* Asked before the panel is up: answered, as every request is. */
            sleep_answer(me, Q_EVT_CAST(DisplaySleepReqEvt)->who, KODE_DENIED,
                         DISPLAY_SLEEP_WHY_NO_PANEL);
            status = Q_HANDLED();
            break;
        }
#if defined(CONFIG_KODE_COMPOSE)
        case DISPLAY_COMPOSE_REQ_SIG: {
            /* The accelerator does not wait for the panel: answered now, and
             * a pass that reads the glass is told there is no frame yet. */
            display_compose_answer(Q_EVT_CAST(DisplayComposeReqEvt));
            status = Q_HANDLED();
            break;
        }
#endif
#if CONFIG_KODE_LOWPOWER
        case DISPLAY_SUPPLY_REQ_SIG: {
            lowpower_ready(me->lp_who, Q_EVT_CAST(DisplaySupplyReqEvt)->round, true,
                           "glass not asleep");
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_ROUND_SIG: {
            /* Asked before the panel is up: say so rather than let the round
             * time out on an owner that never answered. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "panel starting");
            status = Q_HANDLED();
            break;
        }
#endif
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}

#if CONFIG_KODEOS_MIRROR
/* ---- Mirror: the frame producer -------------------------------------------
 * The base64 encoder is kaos_wire.h's, shared with console_ao.c.
 */

/**
 * @brief Whether a BLE central is subscribed to the mirror characteristic.
 *
 * kdrv_ble.c is not in every build, so only this and mirror_ble_write() test
 * CONFIG_KODEOS_BLE_CONSOLE; the rest of the mirror calls them.
 */
static bool mirror_ble_attached(void)
{
#if CONFIG_KODEOS_BLE_CONSOLE
    return kdrv_ble_mirror_attached();
#else
    return false;
#endif
}

/**
 * @brief mirror_ble_attached()'s twin for the write itself.
 * @return kdrv_ble_mirror_write()'s result, or KODE_DENIED when the
 *         characteristic is not in this build, as when nobody is subscribed.
 */
static kode_status_t mirror_ble_write(const uint8_t *data, size_t len)
{
#if CONFIG_KODEOS_BLE_CONSOLE
    return kdrv_ble_mirror_write(data, len);
#else
    (void)data;
    (void)len;
    return KODE_DENIED;
#endif
}

/* Forward declarations: mirror_send_slice() ends a session on a KODE_DENIED
 * transport. */
static void mirror_free_frame(Display * const me);
static void mirror_abort(Display * const me);

/**
 * @brief Push up to KMIRROR_SLICE records of the frame already in me->frame,
 *        and notice when it has all gone out.
 *
 * The transport is chosen once per frame (me->ble). On the text path every
 * record is a whole number of base64 groups except the frame's last, which may
 * be padded: the frame is one buffer of known length, so no part-group needs
 * carrying. The BLE binary characteristic takes raw bytes.
 */
static void mirror_send_slice(Display * const me)
{
    char b64[KMIRROR_PAY_B64 + 1U];
    char line[KMIRROR_PAY_B64 + 8U];

    for ( uint8_t n = 0U;
         (KMIRROR_SLICE > n) && (me->frame_pos < me->frame_len);
         ++n )
    {
        uint32_t const left = me->frame_len - me->frame_pos;
        /* The transports do not share a slice size (KMIRROR_BLE_PAY_BYTES). */
        uint32_t const cap  = me->ble ? KMIRROR_BLE_PAY_BYTES
                                       : KMIRROR_PAY_BYTES;
        uint32_t const take = (left < cap) ? left : cap;
        if ( me->ble )
        {
            /* KODE_NO_RESOURCES: the ring is full and the bytes untouched, so
             * break without moving frame_pos and retry the slice next tick.
             * KODE_DENIED: the central is gone; end the session now rather
             * than hold the codec, the PPA and the frame buffer until the
             * stall timer. */
            kode_status_t const wst =
                mirror_ble_write(&me->frame[me->frame_pos], (size_t)take);
            if ( KODE_DENIED == wst )
            {
                KLOG_W(TAG, "mirror: BLE transport denied mid-frame; ending session");
                mirror_abort(me);
                me->on = false;
                mirror_free_frame(me);
                return;
            }
            if ( KODE_OK != wst )
            {
                break;
            }
        }
        else
        {
            /* Checked per record: a check per slice could find room for one
             * record and then post twelve into a full ring. */
            if ( console_congested() )
            {
                break;
            }
            (void)kaos_wire_b64_encode(&me->frame[me->frame_pos], (size_t)take,
                                       b64);
            (void)snprintf(line, sizeof(line), ">|m=%s", b64);
            kode_log_reply(line);
        }
        me->frame_pos += take;
    }

    if ( me->frame_pos >= me->frame_len )
    {
        me->sending = false;   /* the next pass may start another frame */
    }
}

/**
 * @brief Encode one frame, copy it whole out of the borrowed driver buffers,
 *        and hand it to mirror_send_slice() to start leaving in slices.
 *
 * Every early return costs nothing: an unchanged screen produces no bytes. The
 * frame is copied into me->frame in this step because the encoder's and the
 * PPA's buffers are borrowed only until their next call; holding the codec for
 * the whole emission would refuse `test component=jpeg` for seconds.
 *
 * @return MIRROR_IDLE when nothing was dirty, MIRROR_BEGUN when a frame is
 *         now mid-emission, MIRROR_BLOCKED when rows were owed and could not
 *         be had. Only the last is a stall (mirror_pass() counts it).
 */
static mirror_step_t mirror_try_start(Display * const me)
{
    /* The credit is checked by mirror_pass(), which counts the stall. */
    int32_t y1;
    int32_t y2;
    if ( me->pend_y2 > me->pend_y1 )
    {
        /* Rows taken from the driver on a pass that could not finish them:
         * the driver would now answer "nothing changed". */
        y1 = me->pend_y1;
        y2 = me->pend_y2;
    }
    else
    {
        kdrv_display_dirty_take(&y1, &y2);
    }

    if ( me->first )
    {
        /* A host that has just attached has nothing to apply a delta to:
         * the whole panel, whatever changed. */
        y1 = 0;
        y2 = (int32_t)KBSP_LCD_V_RES;
    }

    uint32_t y = 0U;
    uint32_t h = 0U;
    kmirror_band(y1, y2, me->scale, (uint32_t)KBSP_LCD_V_RES, &y, &h);
    if ( 0U == h )
    {
        me->pend_y1 = 0;
        me->pend_y2 = 0;             /* nothing to retry either */
        return MIRROR_IDLE;          /* nothing moved, and nothing is wrong */
    }

    /* The band is read at full size and scaled after: it is the panel's own
     * framebuffer, and halving it is this pass's business, not the flush
     * path's. */
    const uint8_t *rows = NULL;
    size_t         len  = 0U;
    if ( kdrv_display_band(y * me->scale, h * me->scale, &rows, &len)
        != KODE_OK )
    {
        me->pend_y1 = y1;
        me->pend_y2 = y2;            /* remembered; try again next tick */
        /* Permanent in practice (a panel with no framebuffer), so it counts
         * as a stall and the session stops with a reason (mirror_pass()). */
        return MIRROR_BLOCKED;
    }

    /* At scale 1 the band is encoded where it lies, in the framebuffer: the
     * encoder takes any alignment, and the framebuffer cannot change before
     * this step ends, since only this AO flips it. At scale 2 the PPA halves
     * it first, into a buffer of its own. */
    uint32_t const w      = (uint32_t)KBSP_LCD_H_RES;
    uint32_t const rh     = h * me->scale;
    const uint8_t *pixels = rows;
    uint8_t       *halved = NULL;
    uint32_t       fw     = w;
    uint32_t       fh     = rh;
    if ( (1U != me->scale)
        && (kdrv_ppa_prepare(rows, w, rh, (uint32_t)me->scale, &halved,
                             &fw, &fh) != KODE_OK) )
    {
        me->pend_y1 = y1;
        me->pend_y2 = y2;
        /* Transient when another caller holds the PPA, permanent when it
         * never registered; the stall counter tells the two apart. */
        return MIRROR_BLOCKED;
    }

    uint8_t  *jpg  = NULL;
    uint32_t  jlen = 0U;
    if ( NULL != halved )
    {
        pixels = halved;
    }
    if ( kdrv_jpeg_encode(pixels, fw, fh, me->quality, &jpg, &jlen)
        != KODE_OK )
    {
        /* KODE_DENIED is the codec busy with another caller, not a fault.
         * The band is kept for a retry, and given up on after 5 s. */
        me->pend_y1 = y1;
        me->pend_y2 = y2;
        return MIRROR_BLOCKED;
    }

    if ( (KMIRROR_HDR_LEN + jlen) > KMIRROR_FRAME_CAP )
    {
        /* No retry would fit it, so it is dropped and logged: silent drops
         * would look like an idle mirror. me->first is cleared so the next
         * tick encodes only what is dirty instead of the same oversized
         * whole panel forever. */
        KLOG_W(TAG, "mirror frame %u B would not fit the %u B buffer; dropped",
               (unsigned)jlen, (unsigned)KMIRROR_FRAME_CAP);
        me->first   = false;
        me->pend_y1 = 0;
        me->pend_y2 = 0;
        /* BLOCKED, not IDLE: rows were owed and nothing went out, so a
         * screen of nothing but oversized frames pauses after 5 s. */
        return MIRROR_BLOCKED;
    }

    /* Copied out now, before a single record leaves. */
    kmirror_hdr_t const hdr = {
        .seq = kmirror_sent(&me->credit),
        .x   = 0U,
        .y   = (uint16_t)y,
        .w   = (uint16_t)fw,
        .h   = (uint16_t)fh,
        .len = jlen,
    };
    kmirror_hdr_put(me->frame, &hdr);
    (void)memcpy(&me->frame[KMIRROR_HDR_LEN], jpg, (size_t)jlen);

    me->frame_len = KMIRROR_HDR_LEN + jlen;
    me->frame_pos = 0U;
    me->sending   = true;
    ++me->sent;                      /* mirror_info's own counter */
    /* Decided once per frame (me->ble). */
    me->ble       = mirror_ble_attached();
    me->first     = false;
    me->pend_y1   = 0;
    me->pend_y2   = 0;               /* consumed successfully */

    /* The first slice waits for the next tick: this step has already paid
     * for an encode (about 3 ms) and a copy. */
    return MIRROR_BEGUN;
}

/**
 * @brief Release the frame buffer, if this AO is holding one.
 *
 * The one place that frees it: a stop, a pause and a lost transport all end
 * here, so an unwatched mirror holds no PSRAM.
 */
static void mirror_free_frame(Display * const me)
{
    if ( NULL != me->frame )
    {
        khal_free(me->frame);
        me->frame = NULL;
    }
}

/**
 * @brief Make sure a frame buffer is held, allocating one if it is not.
 *
 * Used by a start or restart and by an ack that revives a pause. A no-op when
 * a buffer is already held.
 *
 * @return true when me->frame is usable afterward.
 */
static bool mirror_frame_alloc(Display * const me)
{
    if ( NULL == me->frame )
    {
        me->frame = khal_alloc_psram(sizeof(uint32_t), KMIRROR_FRAME_CAP);
    }
    return NULL != me->frame;
}

/**
 * @brief Suspend the mirror after a stall, without ending the session.
 *
 * Leaves me->on true; DISPLAY_MIRROR_ACK_SIG wakes it. A caller with a frame
 * mid-emission calls mirror_abort() first.
 */
static void mirror_pause(Display * const me)
{
    me->paused = true;
    /* On the wire, before anything is released: the host has nothing left to
     * ack and must be told to start again (KMIRROR_PAUSE_LINE). */
    kode_log_reply(KMIRROR_PAUSE_LINE);
    mirror_free_frame(me);
    (void)QTimeEvt_disarm(&me->tick);   /* the tick case does not re-arm
                                         *   either */
    ++me->stalls;
}

/**
 * @brief Tell the host to discard whatever of the in-flight frame it has.
 *
 * Always leaves sending/frame_pos/frame_len clean, so callers need not check
 * me->sending first. The record is written only when frame_pos > 0: before the
 * first slice no header has promised a `len`. A stop stays immediate: one line
 * rather than the rest of the frame.
 */
static void mirror_abort(Display * const me)
{
    if ( me->sending && (0U < me->frame_pos) )
    {
        /* The console line, on both transports. On the binary characteristic
         * a mid-frame marker would be read as body bytes by a host framing by
         * length; the console line is framed by newlines, and a BLE viewer is
         * subscribed to it to send its verbs. */
        kode_log_reply(KMIRROR_ABORT_LINE);
    }
    me->sending   = false;
    me->frame_pos = 0U;
    me->frame_len = 0U;
}

/**
 * @brief One pass: continue a frame already in flight, or try to start one.
 *
 * A frame in flight moves one slice per pass, and a new one starts only when
 * the last is gone, so no AO step runs long. Three things feed the stall
 * counter: no credit, a frame that made no progress, and MIRROR_BLOCKED. Each
 * pauses the mirror after the same 5 s, with its own KLOG_W.
 */
static void mirror_pass(Display * const me)
{
    if ( me->sending )
    {
        uint32_t const before = me->frame_pos;
        mirror_send_slice(me);
        if ( !me->on )
        {
            /* mirror_send_slice() ended the session (KODE_DENIED); the frame
             * buffer may be gone. */
            return;
        }
        if ( me->frame_pos != before )
        {
            me->stall_ticks = 0U;    /* a frame is moving; nothing is stuck */
        }
        else
        {
            /* No record left this tick: a congested cable or a full BLE ring.
             * Same counter and pause as starved credit: slow, not gone (gone
             * is KODE_DENIED, handled above). */
            ++me->stall_ticks;
            if ( (uint16_t)KMIRROR_STALL_TICKS <= me->stall_ticks )
            {
                KLOG_W(TAG, "mirror: no progress for %u s; pausing",
                       (unsigned)(KMIRROR_STALL_MS / 1000U));
                mirror_abort(me);
                mirror_pause(me);
            }
        }
        return;
    }

    if ( !kmirror_may_send(&me->credit) )
    {
        ++me->stall_ticks;
        if ( (uint16_t)KMIRROR_STALL_TICKS <= me->stall_ticks )
        {
            KLOG_W(TAG, "mirror: no credit for %u s; pausing",
                   (unsigned)(KMIRROR_STALL_MS / 1000U));
            mirror_pause(me);            /* the tick case sees me->paused
                                          *   and does not re-arm */
        }
        return;
    }

    if ( mirror_try_start(me) == MIRROR_BLOCKED )
    {
        /* Rows were owed and the framebuffer, the PPA or the codec refused
         * them; a PPA that never registered does so on every pass. Same
         * counter, ceiling and pause; an ack or a restart revives it. */
        ++me->stall_ticks;
        if ( (uint16_t)KMIRROR_STALL_TICKS <= me->stall_ticks )
        {
            KLOG_W(TAG, "mirror: nothing could be encoded for %u s; pausing",
                   (unsigned)(KMIRROR_STALL_MS / 1000U));
            mirror_pause(me);
        }
    }
    else
    {
        /* IDLE as much as BEGUN. An idle screen is the feature working, not
         * a stall, and must never be counted as one: a still launcher would
         * otherwise pause itself every five seconds. */
        me->stall_ticks = 0U;
    }
}

#endif /* CONFIG_KODEOS_MIRROR */

/* ---- The `display` row ----------------------------------------------------
 * Nothing on the board can see the glass, so the row asks the bench: five
 * fields of one colour (white, red, green, blue, black), each held until the
 * pad answers. BUTTON_TOP: the whole glass is that colour, nothing dark, stuck
 * or tinted; BUTTON_BOT: it is not; no answer in KODE_DISPLAY_TEST_STEP_MS:
 * nobody there. A press answers; a press while a field is painting is dropped.
 *
 * The fields carry no words, so the row opens with the glass still the
 * renderer's (DISPLAY_TEST_INTRO): whoever draws it says what the pad will
 * mean, until BUTTON_TOP or KODE_DISPLAY_TEST_INTRO_MS. Then the renderer's
 * flushes are held for KODE_DISPLAY_TEST_SETTLE_MS before the first band, so
 * a frame it started on the opening screen lands before the field, not on it.
 *
 * A substate of active: everything else still works, but a renderer flush is
 * completed at once and dropped, so the launcher cannot paint over the field.
 * The renderer's completion shares a binary semaphore with the bands' waits,
 * and LVGL collects it only at its next flush, so one completion is owed to
 * the renderer and paid when the last band has landed. A field goes up one
 * 13 KB band per self-posted step, so no step holds this AO long.
 */

/** @brief The five fields in the order shown, as native RGB565 (what
 *         kdrv_display_flush() takes). Indexed by DISPLAY_FIELD_*: keep the order. */
static const uint16_t TEST_FIELD_RGB[DISPLAY_FIELD_COUNT] = {
    0xFFFFU,   /* DISPLAY_FIELD_WHITE: all three channels.                  */
    0xF800U,   /* DISPLAY_FIELD_RED                                          */
    0x07E0U,   /* DISPLAY_FIELD_GREEN                                        */
    0x001FU,   /* DISPLAY_FIELD_BLUE                                         */
    0x0000U    /* DISPLAY_FIELD_BLACK: nothing.                              */
};

/** @brief Pixels in one band, and what they cost in PSRAM. */
#define TEST_BAND_PIXELS  ((size_t)KBSP_LCD_H_RES \
                           * (size_t)KODE_DISPLAY_TEST_BAND_ROWS)
#define TEST_BAND_BYTES   (TEST_BAND_PIXELS * sizeof(uint16_t))

/** @brief How long one band's transfer is given. The DMA itself is about a
 *         quarter of a millisecond; this is a ceiling on a panel that has
 *         stopped answering, not a figure anything waits out. */
#define TEST_FLUSH_MS     20U

/* Where the row stands (Display.test_stage). */
#define TEST_STAGE_INTRO   0U  /**< The opening screen is up.                */
#define TEST_STAGE_SETTLE  1U  /**< The renderer finishes its last frame.    */
#define TEST_STAGE_FIELDS  2U  /**< The fields are painted and asked about.  */

/** @brief What a field is called in the log. */
static const char *field_name(uint8_t field)
{
    static const char * const NAMES[DISPLAY_FIELD_COUNT] = {
        "white", "red", "green", "blue", "black"
    };

    if ( (uint8_t)DISPLAY_FIELD_COUNT <= field )
    {
        return "?";
    }
    return NAMES[field];
}

/**
 * @brief Publish one record of the row, about the field being asked.
 *
 * A question and its outcome carry the same step number, which is the
 * field's place in the row.
 *
 * @param me    The AO.
 * @param phase DISPLAY_TEST_*
 */
static void test_record(Display * const me, uint8_t phase)
{
    DisplayTestEvt *e = Q_NEW_X(DisplayTestEvt, 0U, DISPLAY_TEST_SIG);

    if ( NULL == e )
    {
        /* Bounded, never fatal: a record nobody sees costs the operator a
         * line, and asserting here would cost them the board. */
        return;
    }
    e->step  = (uint8_t)(me->test_field + 1U);
    e->total = (uint8_t)DISPLAY_FIELD_COUNT;
    e->phase = phase;
    e->field = me->test_field;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief End the row: the closing record, then the verdict.
 *
 * The transition out is the caller's (only a state handler takes one), and so
 * is logging the reason first: the console prints only `result=fail`.
 *
 * @param me The AO.
 * @param ok Whether the panel passed.
 */
static void test_finish(Display * const me, bool ok)
{
    TestResultEvt *e;

    test_record(me, (uint8_t)DISPLAY_TEST_DONE);

    e = Q_NEW_X(TestResultEvt, 0U, TEST_RESULT_SIG);
    if ( NULL == e )
    {
        KLOG_E(TAG, "out of events; the display verdict is lost");
        return;
    }
    (void)snprintf(e->name, sizeof(e->name), "%s", "display");
    e->ok = ok;
    e->origin = TEST_ORIGIN_AUTOTEST;
    QACTIVE_PUBLISH(&e->super, &me->super);
}

/**
 * @brief The row on a board with no panel: one record, a failed verdict.
 *
 * A main PCB is often tested before its glass: say so at once rather than
 * paint into a silent DSI: `>|step=1 panel=absent result=absent`.
 *
 * @param me The AO.
 */
static void test_absent(Display * const me)
{
    me->test_field = (uint8_t)DISPLAY_FIELD_WHITE;
    test_record(me, (uint8_t)DISPLAY_TEST_ABSENT);
    KLOG_E(TAG, "test display: ABSENT - no panel on this board");
    test_finish(me, false);
}

/**
 * @brief The row while the glass rests: a failed verdict, said why.
 *
 * Asleep, for an app or for the board, or on its way there, the link is down
 * or going and the framebuffers with it: there is nothing to paint, and the
 * row's end, which has the renderer paint the glass whole, would wake it over
 * memory the link let go.
 *
 * @param me The AO.
 */
static void test_resting(Display * const me)
{
    KLOG_E(TAG, "test display: refused - the glass is asleep or going to sleep; wake it first");
    test_finish(me, false);
}

/** @brief Stop the row's one-shot, counting an expiry that is already queued. */
static void wait_stop(Display * const me)
{
    if ( timer_stale(&me->test_wait, me->test_wait_live) )
    {
        me->test_wait_stale += 1U;
    }
    me->test_wait_live = false;
}

/**
 * @brief Arm the row's one-shot for @p ms.
 *
 * @param me The AO.
 * @param ms How long from now.
 */
static void wait_arm(Display * const me, uint32_t ms)
{
    wait_stop(me);
    QTimeEvt_armX(&me->test_wait, KQP_TICKS_PER_MS(ms), 0U);
    me->test_wait_live = true;
}

/** @brief Ask this AO for the next band of the field being painted. */
static void paint_step_post(Display * const me)
{
    static const QEvt step = QEVT_INITIALIZER(DISPLAY_TEST_STEP_SIG);

    QACTIVE_POST(&me->super, &step, &me->super);
}

/**
 * @brief Fill the band with @p me->test_field's colour and start putting it
 *        on the glass.
 *
 * The fill happens once per field rather than once per band: every band of a
 * field is the same colour, so the buffer is written here and handed to the
 * driver unchanged thirty-two times.
 *
 * @param me The AO.
 */
static void paint_start(Display * const me)
{
    uint16_t const colour = TEST_FIELD_RGB[me->test_field];
    size_t i;

    for ( i = 0U; TEST_BAND_PIXELS > i; ++i )
    {
        me->band[i] = colour;
    }
    if ( kdrv_display_flush_wait(0U) )
    {
        /* A completion the renderer has not come for yet. Kept for it
         * rather than left where the first band's wait would take it. */
        me->flush_owed = true;
    }
    me->test_asking = false;
    me->test_y      = 0;
    paint_step_post(me);
}

/**
 * @brief The field is on the glass. Ask about it.
 *
 * The question goes out after the last band, so whoever reads it is already
 * looking at the colour it names.
 *
 * @param me The AO.
 */
static void paint_done(Display * const me)
{
    if ( me->flush_owed )
    {
        /* The renderer's completion, kept out of the bands' way while the
         * field went up: see the block comment above. */
        me->flush_owed = false;
        kdrv_display_flush_abort();
    }
    KLOG_N(TAG, "test display: step %u of %u - is the whole glass %s? "
                "TOP = yes, BOT = no",
           (unsigned)(me->test_field + 1U), (unsigned)DISPLAY_FIELD_COUNT,
           field_name(me->test_field));
    test_record(me, (uint8_t)DISPLAY_TEST_ASK);
    me->test_asking = true;
    wait_arm(me, KODE_DISPLAY_TEST_STEP_MS);
}

/**
 * @brief The opening screen is done with: hold the renderer's flushes, and
 *        give the frame it may be drawing the time to land.
 *
 * @param me The AO.
 */
static void intro_end(Display * const me)
{
    me->test_stage = (uint8_t)TEST_STAGE_SETTLE;
    wait_arm(me, KODE_DISPLAY_TEST_SETTLE_MS);
}

/**
 * @brief The renderer's last frame has landed: the first field goes up.
 *
 * @param me The AO.
 */
static void fields_start(Display * const me)
{
    me->test_stage = (uint8_t)TEST_STAGE_FIELDS;
    (void)kdrv_display_brightness((uint8_t)KODE_DISPLAY_TEST_BRIGHT);
    paint_start(me);
}

/**
 * @brief `test component=display`: the panel, judged by whoever is looking
 *        at it.
 *
 * A substate of Display_active (see the row's block comment). Every way out
 * goes through Q_EXIT_SIG, which gives the band back, restores the user's
 * brightness and has the renderer repaint the whole screen.
 */
static QState Display_testing(Display * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            me->test_field      = (uint8_t)DISPLAY_FIELD_WHITE;
            me->test_stage      = (uint8_t)TEST_STAGE_INTRO;
            me->test_asking     = false;
            me->flush_owed      = false;
            /* Zeroed per row: an expiry left by the previous row was dropped
             * in active, and counting it here would swallow a real deadline. */
            me->test_wait_stale = 0U;
            /* The row reads the operator's answer on the pad, and nothing
             * else in this AO listens to buttons. */
            QActive_subscribe(&me->super, BUTTON_EVT_SIG);

            me->band = (uint16_t *)khal_alloc_psram(64U, TEST_BAND_BYTES);
            if ( NULL == me->band )
            {
                /* A row that cannot paint cannot ask. Q_ENTRY_SIG is no
                 * place to take a transition from, so the step below is what
                 * ends it - see the DISPLAY_TEST_STEP_SIG case. */
                paint_step_post(me);
            }
            else
            {
                KLOG_N(TAG, "test display: five fields follow; TOP starts "
                            "them, or they start in %u ms",
                       (unsigned)KODE_DISPLAY_TEST_INTRO_MS);
                test_record(me, (uint8_t)DISPLAY_TEST_INTRO);
                wait_arm(me, KODE_DISPLAY_TEST_INTRO_MS);
            }
            status = Q_HANDLED();
            break;
        }
        case Q_EXIT_SIG: {
            static const QEvt repaint = QEVT_INITIALIZER(DISPLAY_REPAINT_SIG);

            wait_stop(me);
            if ( me->flush_owed )
            {
                /* A row that ended before its field was up still owes the
                 * renderer its release. */
                me->flush_owed = false;
                kdrv_display_flush_abort();
            }
            QActive_unsubscribe(&me->super, BUTTON_EVT_SIG);
            khal_free(me->band);
            me->band = NULL;
            /* Back to the announced brightness, which holds the user's choice
             * throughout the row, including one made mid-row. */
            (void)kdrv_display_brightness(s_state_evt.bright);
            /* Every flush during the row was acknowledged and never painted,
             * so the renderer must repaint the whole glass. */
            QACTIVE_PUBLISH(&repaint, &me->super);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_FLUSH_SIG: {
            /* The opening screen is the renderer's, so its frames go to the
             * panel as ever. After it the rectangle is dropped so the
             * launcher does not paint over the field. The renderer is
             * released at once while a question is out; until then it is
             * owed, since a release between two bands is a token the next
             * band takes. */
            if ( (uint8_t)TEST_STAGE_INTRO == me->test_stage )
            {
                status = Q_SUPER(&Display_active);
                break;
            }
            me->renderer_seen = true;
            if ( me->test_asking )
            {
                kdrv_display_flush_abort();
            }
            else
            {
                me->flush_owed = true;
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_TEST_STEP_SIG: {
            int32_t y2;
            bool    last;

            if ( NULL == me->band )
            {
                KLOG_E(TAG, "test display: FAIL - there is no memory for a "
                            "band to paint");
                test_finish(me, false);
                status = Q_TRAN(&Display_active);
                break;
            }

            y2 = me->test_y + (int32_t)KODE_DISPLAY_TEST_BAND_ROWS;
            if ( (int32_t)KBSP_LCD_V_RES < y2 )
            {
                /* 502 rows do not divide by 16, so the last band is short.
                 * Without this the last six rows of the glass keep whatever
                 * the launcher left there, on every field. */
                y2 = (int32_t)KBSP_LCD_V_RES;
            }
            last = ((int32_t)KBSP_LCD_V_RES <= y2);

            if ( kdrv_display_flush(0, me->test_y, (int32_t)KBSP_LCD_H_RES, y2,
                                   me->band, last) == KODE_OK )
            {
                /* Waited here: the transfer is about a quarter of a millisecond
                 * and its completion is a semaphore, not an event. A refused
                 * band is NOT aborted: flush_abort() only signals the binary
                 * semaphore, and the next band's wait would take that token
                 * before its own DMA has finished. */
                (void)kdrv_display_flush_wait(TEST_FLUSH_MS);
            }

            me->test_y = y2;
            if ( (int32_t)KBSP_LCD_V_RES > me->test_y )
            {
                paint_step_post(me);
            }
            else
            {
                paint_done(me);
            }
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const b = Q_EVT_CAST(ButtonEvt);

            if ( b->pressed && ((uint8_t)KDRV_BTN_TOP == b->id)
                && ((uint8_t)TEST_STAGE_INTRO == me->test_stage) )
            {
                /* Read, and the operator is ready. */
                intro_end(me);
                status = Q_HANDLED();
                break;
            }
            if ( !b->pressed || !me->test_asking )
            {
                /* A release, the opening screen's other buttons, or a press
                 * that landed while the field was still going up: nothing
                 * has been asked yet. */
                status = Q_HANDLED();
                break;
            }
            if ( (uint8_t)KDRV_BTN_TOP == b->id )
            {
                wait_stop(me);
                KLOG_N(TAG, "test display: %s confirmed",
                       field_name(me->test_field));
                test_record(me, (uint8_t)DISPLAY_TEST_PASS);
                if ( (uint8_t)(me->test_field + 1U) >= (uint8_t)DISPLAY_FIELD_COUNT )
                {
                    KLOG_N(TAG, "test display: PASS - five fields, each "
                                "confirmed at the bench");
                    test_finish(me, true);
                    status = Q_TRAN(&Display_active);
                }
                else
                {
                    me->test_field += 1U;
                    paint_start(me);
                    status = Q_HANDLED();
                }
            }
            else if ( (uint8_t)KDRV_BTN_BOT == b->id )
            {
                /* The row stops on the first field that was refused: the
                 * fields after it would be shown on the same panel, and an
                 * operator needs one sentence, not five. */
                test_record(me, (uint8_t)DISPLAY_TEST_FAIL);
                KLOG_E(TAG, "test display: FAIL on %s - the operator says the "
                            "glass is not showing it", field_name(me->test_field));
                test_finish(me, false);
                status = Q_TRAN(&Display_active);
            }
            else
            {
                KLOG_D(TAG, "test display: button %u is not an answer; "
                            "TOP = yes, BOT = no", (unsigned)b->id);
                status = Q_HANDLED();
            }
            break;
        }
        case DISPLAY_TEST_WAIT_SIG: {
            if ( 0U < me->test_wait_stale )
            {
                /* A deadline of a field already answered. The live flag
                 * belongs to the arming that replaced it: do not clear it. */
                me->test_wait_stale -= 1U;
                status = Q_HANDLED();
                break;
            }
            me->test_wait_live = false;   /* this event IS its expiry */
            if ( (uint8_t)TEST_STAGE_INTRO == me->test_stage )
            {
                intro_end(me);
                status = Q_HANDLED();
                break;
            }
            if ( (uint8_t)TEST_STAGE_SETTLE == me->test_stage )
            {
                fields_start(me);
                status = Q_HANDLED();
                break;
            }
            /* Nobody is there, and nobody will be there for the next field
             * either: the row ends here rather than waiting out the rest. */
            test_record(me, (uint8_t)DISPLAY_TEST_TIMEOUT);
            KLOG_E(TAG, "test display: FAIL on %s - nobody answered in %u ms",
                   field_name(me->test_field),
                   (unsigned)KODE_DISPLAY_TEST_STEP_MS);
            test_finish(me, false);
            status = Q_TRAN(&Display_active);
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            /* The row owns the glass and an operator is answering on it. */
            lowpower_ready(me->lp_who, Q_EVT_CAST(LowPowerRoundEvt)->round,
                           true, "display test");
            status = Q_HANDLED();
            break;
        }
#endif
        case DISPLAY_SLEEP_REQ_SIG: {
            /* The same for an app: the row has the glass until it ends. */
            sleep_answer(me, Q_EVT_CAST(DisplaySleepReqEvt)->who, KODE_DENIED,
                         DISPLAY_SLEEP_WHY_TEST);
            status = Q_HANDLED();
            break;
        }
#ifdef CONFIG_KODE_LOWPOWER
        case DISPLAY_SUPPLY_REQ_SIG: {
            lowpower_ready(me->lp_who, Q_EVT_CAST(DisplaySupplyReqEvt)->round, true,
                           "display test");
            status = Q_HANDLED();
            break;
        }
#endif
        case DISPLAY_TEST_REQ_SIG: {
            /* Not deferred: restarting the row would leave the first
             * request's verdict unpublished. */
            KLOG_W(TAG, "test display: already running; the request is ignored");
            status = Q_HANDLED();
            break;
        }
        default: {
            status = Q_SUPER(&Display_active);
            break;
        }
    }
    return status;
}

/**
 * @brief Drive the glass at @p percent and, with @p persist, store it.
 *
 * @param percent The brightness.
 * @param persist A chosen brightness: held to KSET_BRIGHT_MIN..100 and
 *                stored. false: a transient level, 0 included, not stored.
 */
static void bright_apply(uint8_t percent, bool persist)
{
    uint8_t pct = percent;

    if ( persist )
    {
        /* A chosen brightness has the boot read's floor, applied here so
         * the glass and the store hold the same number. A blank
         * (persist=false) has none and is never stored. */
        if ( (uint8_t)KSET_BRIGHT_MIN > pct )
        {
            pct = (uint8_t)KSET_BRIGHT_MIN;
        }
        if ( 100U < pct )
        {
            pct = 100U;
        }
    }
    (void)kdrv_display_brightness(pct);
    /* After the clamp, so a re-announcement reports what the panel is
     * driven at (0 for a blank). The `display` row drives the glass
     * through the driver, not this signal, so this field keeps the
     * user's choice while it runs. */
    s_state_evt.bright = pct;
    if ( persist )
    {
        /* One write per gesture, not one per event: the slider sends
         * its travel with persist=false and only the release lands
         * here. This AO is the key's one writer. */
        (void)khal_nvs_set_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, pct);
        KLOG_N(TAG, "brightness %u%% (stored)", (unsigned)pct);
    }
    else
    {
        KLOG_D(TAG, "brightness %u%%", (unsigned)pct);
    }
}

/** @brief Answer `config key="bright"`: set it first when asked. */
static void bright_config(Display * const me, KodeConfigReqEvt const * const r)
{
    uint8_t pct = (uint8_t)KSET_BRIGHT_DEF;
    char word[KODE_CONFIG_VALUE_MAX + 1U];

    if ( r->set )
    {
        bright_apply((uint8_t)r->i, true);
    }
    bool const stored = (khal_nvs_get_u8(KSET_NAMESPACE, KSET_KEY_BRIGHT, &pct) == KODE_OK);
    (void)snprintf(word, sizeof(word), "%u", (unsigned)pct);
    (void)kode_config_answer(&me->super, r, KODE_OK, stored, word);
}

/**
 * @brief Encode the whole front framebuffer as one JPEG and lend it.
 *
 * The glass is read in one band and encoded where it lies, as the mirror does
 * at scale 1, then copied into the shot buffer in this step (the codec's output
 * is borrowed). The mirror's state is not touched. The buffer is taken on the
 * first shot and kept.
 *
 * @param me      The AO.
 * @param quality JPEG quality, 1..100.
 * @param len     Receives the JPEG's length when KODE_OK is returned.
 * @return KODE_OK with the buffer lent; KODE_DENIED when the codec is busy or
 *         a shot is already lent; KODE_NO_RESOURCES with no codec, no
 *         framebuffer or no PSRAM; KODE_ERROR when the frame does not fit
 *         KSHOT_JPEG_CAP. Nothing is lent on any failure.
 */
static kode_status_t shot_take(const Display * const me, uint8_t quality,
                               uint32_t * const len)
{
#if CONFIG_KODEOS_MIRROR
    kode_status_t  st   = KODE_OK;
    const uint8_t *rows = NULL;
    size_t         blen = 0U;
    uint8_t       *jpg  = NULL;
    uint32_t       jlen = 0U;

    if ( s_shot_lent )
    {
        st = KODE_DENIED;
    }
    else if ( !me->codec )
    {
        st = KODE_NO_RESOURCES;
    }
    else if ( NULL == s_shot_buf )
    {
        /* cppcheck-suppress misra-c2012-11.5 ; khal_alloc_psram() answers void *: PSRAM is taken by name (rule 8) */
        s_shot_buf = (uint8_t *)khal_alloc_psram(sizeof(uint32_t),
                                                 KSHOT_JPEG_CAP);
        if ( NULL == s_shot_buf )
        {
            st = KODE_NO_RESOURCES;
        }
    }
    else
    {
        /* Taken already: nothing to allocate. */
    }

    if ( (KODE_OK == st)
        && (kdrv_display_band(0U, (uint32_t)KBSP_LCD_V_RES, &rows, &blen)
            != KODE_OK) )
    {
        st = KODE_NO_RESOURCES;
    }

    if ( KODE_OK == st )
    {
        st = kdrv_jpeg_encode(rows, (uint32_t)KBSP_LCD_H_RES,
                              (uint32_t)KBSP_LCD_V_RES, (uint32_t)quality,
                              &jpg, &jlen);
        if ( KODE_NO_RESOURCES == st )
        {
            st = KODE_ERROR;
        }
        else if ( (KODE_OK != st) && (KODE_DENIED != st) )
        {
            st = KODE_NO_RESOURCES;
        }
        else
        {
            /* Encoded, or the ordinary busy codec: st says which. */
        }
    }

    if ( (KODE_OK == st) && (KSHOT_JPEG_CAP < jlen) )
    {
        st = KODE_ERROR;
    }

    if ( KODE_OK == st )
    {
        (void)memcpy(s_shot_buf, jpg, (size_t)jlen);
        s_shot_lent = true;
        *len        = jlen;
    }
    return st;
#else
    (void)me;
    (void)quality;
    (void)len;
    return KODE_NO_RESOURCES;
#endif
}

/**
 * @brief Encode a worker's RGB565 into its own buffer: display_jpeg_request().
 *
 * In this step, so nothing else reaches the engine between the encode and the
 * copy. A request whose caller stopped waiting (its generation moved on) is
 * dropped unanswered: the answer would land in a newer request's storage. The
 * generation is stamped before the semaphore, whose release orders the copy
 * before the caller's read on the other core.
 *
 * @param me The AO.
 * @param q  The request.
 */
static void jpeg_answer(const Display * const me, DisplayJpegReqEvt const * const q)
{
    kode_status_t st      = KODE_NO_RESOURCES;
    uint32_t      out_len = 0U;
#if CONFIG_KODEOS_MIRROR
    uint8_t      *jpg     = NULL;
    uint32_t      jlen    = 0U;
#endif

    if ( __atomic_load_n(q->cur, __ATOMIC_ACQUIRE) != q->gen )
    {
        KLOG_W(TAG, "an encode its caller stopped waiting for is dropped");
        return;
    }
#if CONFIG_KODEOS_MIRROR
    if ( !me->codec )
    {
        st = KODE_NO_RESOURCES;
    }
    else if ( (NULL == q->job.src) || (NULL == q->job.dst) )
    {
        st = KODE_DENIED;
    }
    else
    {
        st = kdrv_jpeg_encode(q->job.src, (uint32_t)q->job.w, (uint32_t)q->job.h, (uint32_t)q->job.quality, &jpg,
                              &jlen);
        if ( (KODE_OK == st) && (jlen > q->job.cap) )
        {
            st = KODE_NO_RESOURCES;
        }
        if ( KODE_OK == st )
        {
            (void)memcpy(q->job.dst, jpg, (size_t)jlen);
            out_len = jlen;
        }
    }
#else
    (void)me;
#endif
    *q->len = out_len;
    *q->rc  = (uint32_t)st;
    __atomic_store_n(q->ack, q->gen, __ATOMIC_RELEASE);
    khal_sem_signal(q->done);
}

/**
 * @brief The panel served, up or not: flushes, brightness, captures, the mirror and the tests.
 *
 * @param me The AO.
 * @param e  The event.
 * @return The QP status.
 */
static QState Display_active(Display * const me, QEvt const * const e)
{
    QState status;
    switch ( e->sig )
    {
        case BOOT_KEEP_SIG: {
            /* AO_Boot resets in a moment; a refusal only means the old way. */
            kode_status_t const st = kdrv_display_keep_for_restart();

            KLOG_I(TAG, "the restart: the glass %s",
                   (KODE_OK == st) ? "stays lit" : "goes dark");
            status = Q_HANDLED();
            break;
        }
        case Q_ENTRY_SIG: {
            /* Whoever asked while the supply was being arranged is answered
             * now, in the order they asked. */
            while ( QActive_recall(&me->super, &me->deferq) )
            {
                /* one more recalled */
            }
            status = Q_HANDLED();
            break;
        }
        /* A flush completes only on kdrv_display's semaphore, once, never by an event: the
         * renderer waits for it inside its render call, not on its queue. */
        case DISPLAY_FLUSH_SIG: {
            FlushEvt const * const f = Q_EVT_CAST(FlushEvt);
            kode_status_t st = KODE_DENIED;
            /* A flush proves a renderer exists. While a round or an app's
             * sleep waits for it to park (REST_PARKING) the renderer draws the
             * page the glass will rest on, so its flushes are sent; after the
             * hand-over or a refusal the framebuffers are going, and a flush
             * is only completed. */
            me->renderer_seen = true;
            bool const parked = (REST_AWAKE != me->rest)
                                && (REST_PARKING != me->rest);
            if ( !parked )
            {
                st = kdrv_display_flush(f->x1, f->y1, f->x2, f->y2,
                                        f->pixels, f->last);
            }
            if ( KODE_OK != st )
            {
                /* No DMA was queued, so no completion is coming: release the
                 * renderer by hand, or one bad rectangle hangs the GUI. */
                if ( !parked )
                {
                    KLOG_W(TAG, "flush failed");
                }
                kdrv_display_flush_abort();
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_BRIGHTNESS_SIG: {
            BrightnessEvt const * const b = Q_EVT_CAST(BrightnessEvt);
            bright_apply(b->percent, b->persist);
            status = Q_HANDLED();
            break;
        }
        case CONFIG_REQ_SIG: {
            KodeConfigReqEvt const * const r = Q_EVT_CAST(KodeConfigReqEvt);
            if ( strcmp(r->key, KSET_KEY_BRIGHT) == 0 )
            {
                bright_config(me, r);
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_CAPTURE_REQ_SIG: {
            DisplayCaptureReqEvt const * const req =
                Q_EVT_CAST(DisplayCaptureReqEvt);
            size_t              len = 0U;
            const uint8_t      *img = NULL;
            kode_status_t const st  = kdrv_display_capture_begin(&img, &len);

            if ( KODE_OK == st )
            {
                s_capture = img;
            }

            DisplayCaptureRspEvt *rsp = Q_NEW_X(DisplayCaptureRspEvt, 0U,
                                                DISPLAY_CAPTURE_RSP_SIG);
            if ( NULL != rsp )
            {
                rsp->status  = (uint8_t)st;
                rsp->len     = (uint32_t)len;
                rsp->w       = (uint16_t)KBSP_LCD_H_RES;
                rsp->h       = (uint16_t)KBSP_LCD_V_RES;
                rsp->painted = kdrv_display_capture_painted();
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            else if ( KODE_OK == st )
            {
                /* Nobody is coming to release it, so let it go here. A
                 * capture never released holds its 402 KB and refuses every
                 * capture after it. */
                kdrv_display_capture_end();
                s_capture = NULL;
                KLOG_E(TAG, "out of events; the capture was dropped");
            }
            else
            {
                /* Refused and unreportable: nothing to undo. */
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_CAPTURE_END_SIG: {
            kdrv_display_capture_end();
            s_capture = NULL;
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_SHOT_REQ_SIG: {
            DisplayShotReqEvt const * const req = Q_EVT_CAST(DisplayShotReqEvt);
            uint32_t              len = 0U;
            kode_status_t const   st  = shot_take(me, req->quality, &len);

            DisplayShotRspEvt *rsp = Q_NEW_X(DisplayShotRspEvt, 0U,
                                             DISPLAY_SHOT_RSP_SIG);
            if ( NULL != rsp )
            {
                rsp->status = (uint8_t)st;
                rsp->len    = len;
                QACTIVE_POST(req->who, &rsp->super, &me->super);
            }
            else if ( KODE_OK == st )
            {
                /* Nobody is coming to release it, so let it go here: a shot
                 * never released refuses every shot after it. */
                s_shot_lent = false;
                KLOG_E(TAG, "out of events; the shot was dropped");
            }
            else
            {
                /* Refused and unreportable: nothing to undo. */
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_SHOT_END_SIG: {
            s_shot_lent = false;
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_JPEG_REQ_SIG: {
            jpeg_answer(me, Q_EVT_CAST(DisplayJpegReqEvt));
            status = Q_HANDLED();
            break;
        }
#if defined(CONFIG_KODE_COMPOSE)
        case DISPLAY_COMPOSE_REQ_SIG: {
            display_compose_answer(Q_EVT_CAST(DisplayComposeReqEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case DISPLAY_HEALTH_SIG: {
            /* In every build: the look and the retry of a panel that did not
             * come up are not about rest. */
            display_health(me);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_SLEEP_REQ_SIG: {
            sleep_request(me, Q_EVT_CAST(DisplaySleepReqEvt));
            status = Q_HANDLED();
            break;
        }
#ifdef CONFIG_KODE_LOWPOWER
        case DISPLAY_SUPPLY_REQ_SIG: {
            supply_request(me, Q_EVT_CAST(DisplaySupplyReqEvt));
            status = Q_HANDLED();
            break;
        }
        case IOEXP_FIELD_RSP_SIG: {
            supply_answer(me, Q_EVT_CAST(IoexpFieldEvt));
            status = Q_HANDLED();
            break;
        }
#endif
        case DISPLAY_GLYPHS_SIG: {
            const DisplayGlyphsEvt *const g = Q_EVT_CAST(DisplayGlyphsEvt);

            if ( 0U != g->park )
            {
                if ( me->app_sleep && (me->park_seq == g->park) )
                {
                    sleep_glyphs(me);
                }
                else
                {
                    /* The sleep it answered ended: woken, timed out or taken
                     * by a round. The renderer draws again at the repaint. */
                    KLOG_D(TAG, "a hand-over for an earlier park; ignored");
                }
            }
            else if ( me->app_sleep )
            {
                KLOG_D(TAG, "a round's hand-over while an app's sleep has the glass; ignored");
            }
            else
            {
#if CONFIG_KODE_LOWPOWER
                rest_glyphs(me, g);
#else
                KLOG_D(TAG, "a hand-over nobody asked for; ignored");
#endif
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_GLYPHS_TMO_SIG: {
            rest_glyphs_late(me);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_RESTORE_SIG: {
            rest_restore_retry(me);
            status = Q_HANDLED();
            break;
        }
#if CONFIG_KODE_LOWPOWER
        case LOWPOWER_ROUND_SIG: {
            rest_round(me, Q_EVT_CAST(LowPowerRoundEvt));
            status = Q_HANDLED();
            break;
        }
        case LOWPOWER_STATE_SIG: {
            rest_state(me, Q_EVT_CAST(LowPowerStateEvt));
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_MINUTE_REQ_SIG: {
            if ( REST_ASLEEP == me->rest )
            {
                rest_minute_request(me);
            }
            status = Q_HANDLED();
            break;
        }
#endif
        case DISPLAY_STATE_REQ_SIG: {
            /* AO_Gui and kscreen_start() ask before their first frame, and
             * AO_TestDot asks for the level. Whoever asks is counted as
             * drawing from here, flushed or not, so a sleep parks it before
             * the link goes: a park nobody answers only refuses the sleep,
             * while a link taken from a renderer frees what it draws into. */
            me->renderer_seen = true;
            /* A late asker: re-publishing answers whether the panel is up and
             * the current brightness, which s_state_evt tracks. */
            QACTIVE_PUBLISH(&s_state_evt.super, &me->super);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_TEST_REQ_SIG: {
            if ( !s_state_evt.up )
            {
                /* No glass: nothing to paint and nobody to ask about it. */
                test_absent(me);
                status = Q_HANDLED();
            }
            else if ( REST_AWAKE != me->rest )
            {
                test_resting(me);
                status = Q_HANDLED();
            }
            else
            {
                /* `test component=display`: a substate, so this state's work
                 * goes on while it runs. */
                status = Q_TRAN(&Display_testing);
            }
            break;
        }
#if CONFIG_KODEOS_MIRROR
        case DISPLAY_MIRROR_START_SIG: {
            MirrorStartEvt const * const req = Q_EVT_CAST(MirrorStartEvt);

            /* A restart must not leave the old frame half-sent on the wire
             * (mirror_abort()); a no-op when nothing was sending. */
            mirror_abort(me);

            /* Allocated on start, not in Display_ctor(), so an unasked mirror
             * holds no PSRAM. A restart, or a start on a paused session, reuses
             * a buffer already held; the size never changes. */
            if ( !mirror_frame_alloc(me) )
            {
                KLOG_E(TAG, "no PSRAM for a mirror frame; mirror_start refused");
                status = Q_HANDLED();
                break;
            }

            /* kdrv_ppa_prepare() refuses any scale but 1 or 2, so anything
             * else becomes 1 here, once, not on every pass. */
            me->scale   = (2U == req->scale) ? 2U : 1U;
            uint8_t const q = ((1U <= req->quality) && (100U >= req->quality))
                             ? req->quality : 70U;   /* the spec's own default */
            /* Clamped as well as range-checked: a legal q=100 would drop every
             * frame (KMIRROR_QUALITY_MAX). */
            me->quality   = ((uint8_t)KMIRROR_QUALITY_MAX < q)
                           ? (uint8_t)KMIRROR_QUALITY_MAX : q;
            me->first       = true;
            me->sending     = false;
            me->frame_pos   = 0U;
            me->frame_len   = 0U;
            me->pend_y1     = 0;
            me->pend_y2     = 0;
            me->stall_ticks = 0U;
            me->sent        = 0U;
            me->stalls      = 0U;
            kmirror_credit_init(&me->credit, KMIRROR_WINDOW);
            me->on     = true;
            me->paused = false;      /* on a paused session: a restart, not a
                                      *   resume */

            /* Disarmed first: arming an armed QTimeEvt asserts, and a host may
             * start again without stopping. */
            (void)QTimeEvt_disarm(&me->tick);
            QTimeEvt_armX(&me->tick, KQP_TICKS_PER_MS(KMIRROR_TICK_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_MIRROR_STOP_SIG: {
            /* Mid-frame, the host has records promising bytes that will not
             * follow (mirror_abort()). A paused session is never mid-frame. */
            mirror_abort(me);
            me->on     = false;
            me->paused = false;
            (void)QTimeEvt_disarm(&me->tick);
            mirror_free_frame(me);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_MIRROR_ACK_SIG: {
            MirrorAckEvt const * const ack = Q_EVT_CAST(MirrorAckEvt);
            if ( me->on && me->paused )
            {
                /* Any ack revives a paused session: the host was only slow.
                 * The credit window starts over, since the host missed every
                 * frame meanwhile, and me->first makes the next frame whole. */
                if ( mirror_frame_alloc(me) )
                {
                    me->paused      = false;
                    me->first       = true;
                    me->stall_ticks = 0U;
                    kmirror_credit_init(&me->credit, KMIRROR_WINDOW);
                    QTimeEvt_armX(&me->tick,
                                  KQP_TICKS_PER_MS(KMIRROR_TICK_MS), 0U);
                }
                else
                {
                    /* Still asleep - nothing this AO can do without PSRAM,
                     * and the next ack gets another chance. */
                    KLOG_E(TAG, "no PSRAM to revive a paused mirror");
                }
            }
            else
            {
                /* kmirror_ack() refuses a stale or invented sequence itself. */
                (void)kmirror_ack(&me->credit, ack->seq);
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_MIRROR_TICK_SIG: {
            if ( me->on && !me->paused )
            {
                mirror_pass(me);
                /* Read again: mirror_pass() may have paused or ended the
                 * session. A one-shot re-armed per pass, so a link that falls
                 * behind slows the stream instead of piling up ticks. */
                if ( me->on && !me->paused )
                {
                    QTimeEvt_armX(&me->tick,
                                  KQP_TICKS_PER_MS(KMIRROR_TICK_MS), 0U);
                }
            }
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_MIRROR_INFO_REQ_SIG: {
            /* Q_NEW_X, as for any request/answer: an empty pool means no
             * answer, and the console's watchdog reports that. */
            MirrorInfoEvt *info = Q_NEW_X(MirrorInfoEvt, 0U,
                                          DISPLAY_MIRROR_INFO_SIG);
            if ( NULL != info )
            {
                info->on        = me->on;
                info->paused    = me->paused;
                info->scale     = me->scale;
                info->quality   = me->quality;
                info->window    = me->credit.window;
                info->in_flight = kmirror_in_flight(&me->credit);
                info->sent      = me->sent;
                info->stalls    = me->stalls;
                QACTIVE_PUBLISH(&info->super, &me->super);
            }
            status = Q_HANDLED();
            break;
        }
#endif /* CONFIG_KODEOS_MIRROR */
        default: {
            status = Q_SUPER(&QHsm_top);
            break;
        }
    }
    return status;
}
