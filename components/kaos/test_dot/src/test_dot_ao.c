/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_ao.c
 * @brief AO_TestDot: the E5 final test, run by the station on the cable.
 */
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kaos_claim.h"
#include "kaos_start.h"
#include "kdrv_buttons.h"
#include "kdrv_wifi.h"
#include "khal_nvs.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"
#include "kqp_worker.h"
#include "kverbs.h"

#include "test_dot_priv.h"

_Static_assert(KODE_TEST_DOT_LINE_MS > KODE_TEST_DOT_RESET_MS,
               "the console's line outlasts the SHIP reset's bound, so the reset's own answer reaches it");
_Static_assert(KODE_TEST_DOT_LINE_MS >= (KODE_TEST_DOT_CHECK_MS + KODE_TEST_DOT_RUN_MS),
               "the console's line outlasts the fixture check and a run's watchdog");
_Static_assert(KODE_TEST_DOT_LINE_MS > KODE_TEST_DOT_DISCOVER_MS,
               "the console's line outlasts the discovery's bound, the longest radio check");
_Static_assert(KODE_TEST_DOT_DISCOVER_MS > KODE_TEST_DOT_RADIO_MS, "a discovery is given more than a radio check");
_Static_assert(KODE_TEST_DOT_SWEEP_MS >= (KDRV_WIFI_SCAN_TIMEOUT_MS + KDRV_WIFI_SCAN_STEP_TIMEOUT_MS),
               "the discovery's bound holds a whole sweep at the driver's own bound");
_Static_assert(KODE_TEST_DOT_LINE_MS > KODE_TEST_DOT_END_MS, "the console's line outlasts the end");
_Static_assert(KODE_TEST_DOT_LINE_MS > KODE_TEST_DOT_LOOP_MS, "the console's line outlasts the pads loop");

/* The option has no "depends on" (kdrv's manifest reads it, see its Kconfig
 * help), so what it needs is checked here. */
#if !defined(CONFIG_KODEOS_CONSOLE) || !defined(CONFIG_KODEOS_RADIO) || !defined(CONFIG_KODEOS_MIRROR)
#error "CONFIG_KODE_TEST_DOT needs CONFIG_KODEOS_CONSOLE, CONFIG_KODEOS_RADIO and CONFIG_KODEOS_MIRROR"
#endif

/** @brief This file's log tag. */
#define TAG "kaos.test_dot"

#define TD_LED_FULL       255U  /**< A channel of the RGB LED fully lit.                     */
#define TD_LED_DARK       0U    /**< A channel of the RGB LED off.                           */
#define TD_BRIGHT_FULL    100U  /**< The glass at full brightness, percent.                  */
#define TD_BRIGHT_OFF     0U    /**< The panel dark, for the photo of what the room adds.   */
#define TD_GRID_ID_CELL   0U    /**< The grid's first cell: the Kode identified.            */
#define TD_ROWS_TOTAL     13U   /**< Rows a run asks AO_Autotest for.                        */
#define TD_TOUCH_TARGETS  2U    /**< The touch test's targets (KBSP_TOUCH_TEST_A and _B).   */
#define TD_ERR_BUSY       4U    /**< kcli code 4: busy, or what is missing.                  */
#define TD_ERR_REJECT     5U    /**< kcli code 5: refused.                                   */
#define TD_ERR_TIMEOUT    6U    /**< kcli code 6: timed out.                                 */
#define TD_MM10_PER_MM    10.0  /**< The place's limits arrive in tenths.                    */
#define TD_NOT_GIVEN      (-1)  /**< A place window coordinate the station left out.          */
#define TD_WHY_MAX        16U   /**< Bytes of a refusal's word kept for later.               */
#define TD_ALIGN          16U   /**< Alignment of the AO's PSRAM blocks.                     */
#define TD_REC_CUT_HEAD   48U   /**< Characters of a cut record the log shows.               */

/* ---- What the AO is doing ---------------------------------------------------- */

/** @brief The wake's place in the AO's life. */
typedef enum {
    MODE_DORMANT = 0,  /**< Holds nothing.                                     */
    MODE_WAKING,       /**< Rail, probe, station: on the way up, or back down. */
    MODE_AWAKE,        /**< The station is in use.                             */
    MODE_ENDING        /**< Giving everything back.                            */
} td_mode_t;

/** @brief The foreground activity. */
typedef enum {
    ACT_NONE = 0,  /**< Nothing: a verb may start.     */
    ACT_WAKE,      /**< The wake for a pending verb.   */
    ACT_PLACE,     /**< `test_dot_place`.              */
    ACT_CAMERA,    /**< The camera part.               */
    ACT_PHOTOS,    /**< `test_dot_photos`.             */
    ACT_RELAYS,    /**< `test_dot_relays`.             */
    ACT_ADC,       /**< `test_dot_adc`.                */
    ACT_LOOP,      /**< `test_dot_loop`.               */
    ACT_RADIO,     /**< `test_dot_radio`.              */
    ACT_BOOST,     /**< `test_dot_boost`.              */
    ACT_RESULT,    /**< `test_dot_result`.             */
    ACT_SCREEN,    /**< `test_dot_screen`.             */
    ACT_END        /**< `test_dot_end`.                */
} td_act_t;

/** @brief Where a run is. */
typedef enum {
    RUN_NONE = 0,  /**< None since the wake.                   */
    RUN_GOING,     /**< From `test component=all` to its end.  */
    RUN_DONE,      /**< Every row and the operator answered.   */
    RUN_ABORTED    /**< Ended early: no SD, or the watchdog.   */
} td_run_t;

/** @brief Where `test_dot_result` is. */
typedef enum {
    RES_NONE = 0,  /**< Not asked.                                  */
    RES_PASSING,   /**< The SHIP reset is running.                  */
    RES_PASSED,    /**< The reset answered DONE: PASS on the glass. */
    RES_FAILED     /**< FAIL on the glass.                          */
} td_res_t;

/** @brief What the tmo timer bounds. */
typedef enum {
    TMO_NONE = 0,  /**< Nothing.                 */
    TMO_RAIL,      /**< J3's rail coming up.     */
    TMO_SCREEN,    /**< The host showing a screen. */
    TMO_RADIO,     /**< The radio check.         */
    TMO_END,       /**< An owner's answer to a step of the end. */
    TMO_JOB,       /**< The worker's job.        */
    TMO_ANSWER,    /**< The boost's answer, or the dock's pins'. */
    TMO_RESET      /**< The SHIP reset's answer. */
} td_tmo_t;

/** @brief What the rest timer paces. */
typedef enum {
    REST_NONE = 0, /**< Nothing.                         */
    REST_CAMERA,   /**< A settle before a photo.         */
    REST_PLACE,    /**< The pause between two looks.     */
    REST_OPERATOR  /**< The operator's screen settling.  */
} td_rest_t;

/** @brief What the drain timer waits for. */
typedef enum {
    DRAIN_NONE = 0, /**< Nothing.                             */
    DRAIN_RETRY,    /**< Room in the console's queue.         */
    DRAIN_LEND      /**< The lent blob back.                  */
} td_drain_t;

/** @brief The operator part's place. */
typedef enum {
    OP_NONE = 0,   /**< Not started.             */
    OP_BUTTONS,    /**< The guided button test.  */
    OP_TOUCH,      /**< The guided touch test.   */
    OP_DONE        /**< Both answered.           */
} td_op_t;

/** @brief The loop verb's place. */
typedef enum {
    LOOP_LENDING = 0, /**< The dock's pins asked for.   */
    LOOP_RUNNING,     /**< The frames going round.      */
    LOOP_RETURNING    /**< The dock console asked back. */
} td_loop_t;

/** @brief The end's steps, in order. */
typedef enum {
    END_START = 0, /**< Cancel the run, the LED off.            */
    END_BOOST,     /**< The boost off.                          */
    END_IDLE,      /**< A job still out answers first.          */
    END_DOCK,      /**< The dock console back.                  */
    END_CLOSE,     /**< The relays at rest, the camera closed.  */
    END_FINISH     /**< Rail, brightness, glass, memory.        */
} td_end_t;

/** @brief Whose answer the end waits for: any other is dropped. */
typedef enum {
    END_WAIT_NONE = 0, /**< Nobody's.          */
    END_WAIT_BOOST,    /**< AO_Power's.        */
    END_WAIT_DOCK,     /**< AO_Log's.          */
    END_WAIT_JOB       /**< The worker's.      */
} td_end_wait_t;

/** @brief What one out entry carries. */
typedef enum {
    OUT_REC = 0,   /**< A record.    */
    OUT_ANS,       /**< An end.      */
    OUT_LEND       /**< A blob lent. */
} td_out_kind_t;

/** @brief One record, end or loan waiting for room in the console's queue. */
typedef struct {
    uint8_t        kind;                         /**< td_out_kind_t.                  */
    uint8_t        seq;                          /**< The command's number.           */
    uint8_t        code;                         /**< OUT_ANS: 0 or kcli_err_t.       */
    QActive       *who;                          /**< The asker.                      */
    const uint8_t *data;                         /**< OUT_LEND: the bytes.            */
    uint32_t       len;                          /**< OUT_LEND: how many.             */
    char           text[KODE_FUNCTION_REC_MAX];  /**< The record, the word or the name. */
} td_out_t;

/** @brief AO_TestDot. */
typedef struct {
    QActive       super;              /**< QP's.                                           */
    QTimeEvt      tmo;                /**< TEST_DOT_TMO_SIG: what td_tmo_t says.           */
    QTimeEvt      rest;               /**< TEST_DOT_REST_SIG: what td_rest_t says.         */
    QTimeEvt      watch;              /**< TEST_DOT_WATCH_SIG: the run's watchdog.         */
    QTimeEvt      drain;              /**< TEST_DOT_DRAIN_SIG: what td_drain_t says.       */
    uint8_t       tmo_for;            /**< td_tmo_t.                                       */
    uint8_t       tmo_stale;          /**< Ticks of the tmo timer to let pass, disarmed late. */
    uint8_t       rest_for;           /**< td_rest_t.                                      */
    uint8_t       rest_stale;         /**< Same, for rest.                                 */
    uint8_t       drain_for;          /**< td_drain_t.                                     */
    uint8_t       drain_stale;        /**< Same, for drain.                                */
    uint8_t       mode;               /**< td_mode_t.                                      */
    uint8_t       act;                /**< td_act_t.                                       */
    bool          locked;             /**< Stored lock read at the start: every verb refused. */
    bool          locked_now;         /**< Locked in this session: only the end answers.   */
    /* The verb in the foreground. */
    QActive      *cur_who;            /**< Its asker.                                      */
    uint8_t       cur_seq;            /**< Its number.                                     */
    TestDotReqEvt pend;               /**< The verb that woke the AO.                      */
    /* The worker. */
    KqpWorker     worker;             /**< The task.                                       */
    bool          worker_up;          /**< Started, on the first need.                     */
    td_job_t      job;                /**< The job in flight, or the last.                 */
    bool          job_busy;           /**< A job is out.                                   */
    bool          job_lost;           /**< It ran past KODE_TEST_DOT_JOB_MS: given up.     */
    uint16_t      job_seq;            /**< Its number.                                     */
    /* What the wake took. */
    bool          rail_on;            /**< J3's rail was asked on.                         */
    bool          probed;             /**< The camera answered: J3 is lent to it.          */
    bool          station_seen;       /**< The relays may have been touched.               */
    bool          bright_wait;        /**< The next DISPLAY_STATE_SIG is the level before. */
    bool          bright_known;       /**< @c bright is what to put back.                  */
    uint8_t       bright;             /**< The level before the wake.                      */
    bool          dock_lent;          /**< The dock console's pins are lent.               */
    bool          boost_on;           /**< The boost test is on.                           */
    bool          subscribed;         /**< The wake's subscriptions are in.                */
    uint32_t      wake_t0;            /**< When the wake began.                            */
    uint32_t      wake_ms;            /**< How long it took.                               */
    char          wake_why[TD_WHY_MAX]; /**< Why the wake failed.                          */
    /* The camera. */
    td_cam_t     *cam;                /**< The camera part's memory, or NULL.              */
    uint8_t       cam_step;           /**< td_cam_step_t.                                  */
    uint8_t       cam_part;           /**< test_dot_cam_t: what the records keep.          */
    bool          cam_in_run;         /**< The run's camera part, not the verb's.          */
    bool          cam_abort;          /**< The watchdog ended the run under it.            */
    uint32_t      cam_t0;             /**< When it began.                                  */
    uint8_t       scr_wait;           /**< The kind a step waits to see on the glass.      */
    uint8_t       scr_wait_seq;       /**< The number of the request it waits on.          */
    uint8_t       scr_seq;            /**< The number of the last screen asked.            */
    bool          scr_waiting;        /**< It waits.                                       */
    uint8_t       scr_phase;          /**< `test_dot_screen`: test_dot_phase_t.            */
    /* The place. */
    uint32_t      place_t0;           /**< When the looks began.                           */
    uint32_t      place_mark_ms;      /**< How long they may go on.                        */
    double        place_mm;           /**< The most the screen may sit off, mm.            */
    double        place_deg;          /**< The most it may turn.                           */
    uint16_t      place_tries;        /**< Looks so far.                                   */
    /* The photos. */
    uint8_t       ph_idx;             /**< The photo being sent.                           */
    uint8_t       ph_last;            /**< The last to send.                               */
    uint8_t       ph_sent;            /**< Sent.                                           */
    uint32_t      ph_bytes;           /**< Their bytes.                                    */
    uint32_t      ph_t0;              /**< When the first was asked.                       */
    bool          ph_wait_back;       /**< The photo is lent.                              */
    /* The loop. */
    uint8_t       loop_phase;         /**< td_loop_t.                                      */
    uint8_t       loop_count;         /**< Frames at each speed.                           */
    /* The run. */
    uint8_t       run;                /**< td_run_t.                                       */
    QActive      *run_who;            /**< The run's asker.                                */
    uint8_t       run_seq;            /**< The run's number.                               */
    bool          line_open;          /**< The run's line is open: its records go out.     */
    uint32_t      run_t0;             /**< When it began.                                  */
    uint32_t      cam_end_ms;         /**< When its camera part ended, from its start.     */
    uint8_t       rows_list;          /**< 0 the rows before the camera, 1 after.          */
    uint8_t       rows_idx;           /**< The row asked.                                  */
    bool          rows_done;          /**< The rows after the camera are over.             */
    uint32_t      row_t0;             /**< When the row was asked.                         */
    uint32_t      row_ms[TD_ROWS_TOTAL]; /**< Each row's time, in ROWS_ALL's order.        */
    uint8_t       grid[KODE_TEST_DOT_GRID_LEN]; /**< The auto screen's cells.              */
    uint8_t       keys[KDRV_BTN_COUNT];         /**< The buttons screen's keys.            */
    uint8_t       targets[TD_TOUCH_TARGETS];    /**< The touch screen's targets.           */
    uint8_t       scr_now;            /**< The screen on the glass, TEST_DOT_SCR_*.        */
    uint8_t       op;                 /**< td_op_t.                                        */
    uint32_t      op_t0;              /**< When the operator's test began.                 */
    bool          op_timeout;         /**< A step of the operator's test timed out.        */
    bool          op_giveup;          /**< The button test ended on three wrong presses.   */
    uint8_t       res;                /**< td_res_t.                                       */
    uint8_t       res_code;           /**< The result's end: 0 or a kcli code.             */
    char          res_why[TD_WHY_MAX];/**< Its word.                                       */
    uint8_t       end_step;           /**< td_end_t.                                       */
    uint8_t       end_wait;           /**< td_end_wait_t.                                  */
    bool          end_answered;       /**< The end's line is answered: what is left runs on. */
    /* The out FIFO and the poll ring, PSRAM. */
    td_out_t     *out;                /**< KODE_TEST_DOT_OUT_MAX entries, or NULL.         */
    uint8_t       out_head;           /**< The oldest.                                     */
    uint8_t       out_n;              /**< How many.                                       */
    bool          lent;               /**< A blob is out: nothing more is posted.          */
    char        (*ring)[KODE_FUNCTION_REC_MAX]; /**< KODE_TEST_DOT_POLL_RING records.      */
    uint8_t       ring_head;          /**< The oldest.                                     */
    uint8_t       ring_n;             /**< How many.                                       */
    uint32_t      ring_dropped;       /**< Pushed out by newer ones.                       */
} TestDot;

static TestDot l_td;
QActive * const AO_TestDot = &l_td.super;

/** @brief Set while a run goes: read by the console's `factory_reset`, from its own task. */
static volatile bool s_run_going;

static QState TestDot_initial(TestDot * const me, void const * const par);
static QState TestDot_active(TestDot * const me, QEvt const * const e);
static void verb_start(TestDot *me, TestDotReqEvt const *e);
static void end_next(TestDot *me);
static void out_drain(TestDot *me);
static void cam_step_go(TestDot *me, uint8_t step);
static void row_next(TestDot *me);

/* ---- The rows of a run ------------------------------------------------------- */

/** @brief The rows before the camera: the SD first, then the motion while the Kode is still. */
static const char *const ROWS_FIRST[] = { "sd", "imu" };
/** @brief The rows after it, beside the operator: sound and buzz only once the photos are taken. */
static const char *const ROWS_AFTER[] = { "audio", "haptic", "mag", "pmic", "gauge", "ioexp", "clocks",
                                          "ir", "nfc", "rfid", "mem" };
/** @brief Every row in the timing record's order: @c row_ms follows it. */
static const char *const ROWS_ALL[TD_ROWS_TOTAL] = { "sd", "imu", "audio", "haptic", "mag", "pmic", "gauge",
                                                     "ioexp", "clocks", "ir", "nfc", "rfid", "mem" };
/** @brief The auto screen's cells after the first, in the order they show. */
static const char *const GRID_ROWS[] = { "pmic", "gauge", "ioexp", "clocks", "imu", "mag", "haptic",
                                         "audio", "sd", "nfc", "rfid", "ir" };
/** @brief The row states' words, by AUTOTEST_ROW_*. */
static const char *const ROW_WORD[AUTOTEST_ROW_STATES] = { "running", "pass", "fail", "timeout", "skipped" };
/** @brief The reset's steps' words, by kboot_reset_step_t. */
static const char *const RESET_WORD[] = { "reset_find", "reset_install", "reset_apps", "reset_link",
                                          "reset_wipe", "reset_nvs", "reset_request", "reset_done" };
/** @brief The loop's reasons, by kdrv_dock_loop_why_t. */
static const char *const LOOP_WHY[] = { "-", "no_echo", "corrupt", "uart_busy" };

/* ---- Small helpers ------------------------------------------------------------ */

/** @brief 1 or 0, for a record. */
static unsigned td_bit(bool b)
{
    unsigned v = 0U;

    if ( b )
    {
        v = 1U;
    }
    return v;
}

/** @brief Word @p i of @p words, or `?` past its @p n. */
static const char *td_word(const char *const *words, size_t n, uint32_t i)
{
    const char *w = "?";

    if ( i < n )
    {
        w = words[i];
    }
    return w;
}

/**
 * @brief Stop a one-shot timer. When it was live and had already fired, its
 *        event is on the way: counted, so it is let pass.
 */
static void timer_stop(QTimeEvt *t, uint8_t *stale, uint8_t *what)
{
    if ( !QTimeEvt_disarm(t) && (0U != *what) )
    {
        *stale += 1U;
    }
    *what = 0U;
}

/** @brief Arm a one-shot timer afresh for @p why. */
static void timer_arm(QTimeEvt *t, uint8_t *stale, uint8_t *what, uint8_t why, uint32_t ms)
{
    timer_stop(t, stale, what);
    *what = why;
    QTimeEvt_armX(t, KQP_TICKS_PER_MS(ms), 0U);
}

/** @brief Whether a timer's event is one to let pass; @return true to ignore it. */
static bool timer_stale(uint8_t *stale)
{
    bool st = false;

    if ( 0U < *stale )
    {
        *stale -= 1U;
        st = true;
    }
    return st;
}

/** @brief Copy text @p src into @p dst of @p cap bytes, cut to fit, always terminated. */
static void td_copy(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);

    if ( n >= cap )
    {
        n = cap - 1U;
    }
    (void)memcpy(dst, src, n);
    dst[n] = '\0';
}

/** @brief Arm the tmo timer for @p why. */
static void tmo_arm(TestDot *me, uint8_t why, uint32_t ms)
{
    timer_arm(&me->tmo, &me->tmo_stale, &me->tmo_for, why, ms);
}

/** @brief The tmo timer is no longer needed. */
static void tmo_stop(TestDot *me)
{
    timer_stop(&me->tmo, &me->tmo_stale, &me->tmo_for);
}

/** @brief Arm the rest timer for @p why. */
static void rest_arm(TestDot *me, uint8_t why, uint32_t ms)
{
    timer_arm(&me->rest, &me->rest_stale, &me->rest_for, why, ms);
}

/** @brief Publish a colour for the RGB LED. */
static void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    LedRgbEvt *e = Q_NEW_X(LedRgbEvt, KODE_TEST_DOT_POOL_MARGIN, LED_RGB_SIG);

    if ( NULL != e )
    {
        e->r = r;
        e->g = g;
        e->b = b;
        QACTIVE_PUBLISH(&e->super, AO_TestDot);
    }
    else
    {
        KLOG_W(TAG, "out of events; the LED keeps its colour");
    }
}

/** @brief The LED off. */
static void led_off(void)
{
    led_set(TD_LED_DARK, TD_LED_DARK, TD_LED_DARK);
}

/** @brief Ask the screen host for a screen or a pattern, under the next number. */
static void screen_ask(TestDot *me, uint8_t kind, const uint8_t *arg, uint8_t len)
{
    TestDotScreenEvt *e = Q_NEW_X(TestDotScreenEvt, KODE_TEST_DOT_POOL_MARGIN, TEST_DOT_SCREEN_REQ_SIG);

    /* Counted even when not asked: a wait on it then runs out. */
    me->scr_seq++;
    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; screen 0x%02x not asked", (unsigned)kind);
        return;
    }
    e->kind = kind;
    e->seq  = me->scr_seq;
    e->len  = 0U;
    (void)memset(e->arg, 0, sizeof(e->arg));
    if ( (NULL != arg) && (0U < len) )
    {
        uint8_t n = len;
        if ( (uint8_t)sizeof(e->arg) < n )
        {
            n = (uint8_t)sizeof(e->arg);
        }
        (void)memcpy(e->arg, arg, n);
        e->len = n;
    }
    if ( (kind & TEST_DOT_PAT_FLAG) == 0U )
    {
        me->scr_now = kind;
    }
    QACTIVE_PUBLISH(&e->super, AO_TestDot);
}

/**
 * @brief Ask for @p kind and wait for the host to show it, at most
 *        KODE_TEST_DOT_SCREEN_MS: only the answer that echoes this request's
 *        number ends the wait, so a late one for an earlier request cannot.
 */
static void screen_wait(TestDot *me, uint8_t kind, const uint8_t *arg, uint8_t len)
{
    me->scr_wait    = kind;
    me->scr_waiting = true;
    screen_ask(me, kind, arg, len);
    me->scr_wait_seq = me->scr_seq;
    tmo_arm(me, (uint8_t)TMO_SCREEN, KODE_TEST_DOT_SCREEN_MS);
}

/** @brief A pattern with its one byte, waited for. */
static void pattern_wait(TestDot *me, uint8_t kind, uint8_t arg)
{
    uint8_t a = arg;

    screen_wait(me, kind, &a, 1U);
}

/** @brief The auto screen again, when it is the one on the glass or @p force. */
static void grid_show(TestDot *me, bool force)
{
    if ( force || (TEST_DOT_SCR_AUTO == me->scr_now) )
    {
        screen_ask(me, TEST_DOT_SCR_AUTO, me->grid, (uint8_t)KODE_TEST_DOT_GRID_LEN);
    }
}

/** @brief Row @p name's grid cell, or the grid's length for none. */
static uint8_t grid_cell(const char *name)
{
    uint8_t cell = (uint8_t)KODE_TEST_DOT_GRID_LEN;

    for ( uint8_t i = 0U; (uint8_t)(sizeof(GRID_ROWS) / sizeof(GRID_ROWS[0])) > i; i++ )
    {
        if ( ((uint8_t)KODE_TEST_DOT_GRID_LEN == cell) && (strcmp(GRID_ROWS[i], name) == 0) )
        {
            cell = (uint8_t)(i + 1U);
        }
    }
    return cell;
}

/** @brief Set row @p name's cell to @p st and show it. */
static void grid_set(TestDot *me, const char *name, uint8_t st)
{
    uint8_t const cell = grid_cell(name);

    if ( (uint8_t)KODE_TEST_DOT_GRID_LEN > cell )
    {
        me->grid[cell] = st;
        grid_show(me, false);
    }
}

/* ---- The way out: records, ends and loans -------------------------------------- */

/** @brief Post one out entry now. @return false when the pool or the queue said no. */
static bool out_post(TestDot *me, td_out_t const *o)
{
    bool ok = false;

    if ( (uint8_t)OUT_REC == o->kind )
    {
        TestDotRecEvt *e = Q_NEW_X(TestDotRecEvt, KODE_TEST_DOT_POOL_MARGIN, TEST_DOT_REC_SIG);
        if ( NULL != e )
        {
            e->seq = o->seq;
            td_copy(e->text, sizeof(e->text), o->text);
            ok = QACTIVE_POST_X(o->who, &e->super, KODE_TEST_DOT_POST_MARGIN, &me->super);
        }
    }
    else if ( (uint8_t)OUT_ANS == o->kind )
    {
        TestDotAnsEvt *e = Q_NEW_X(TestDotAnsEvt, KODE_TEST_DOT_POOL_MARGIN, TEST_DOT_ANS_SIG);
        if ( NULL != e )
        {
            e->seq  = o->seq;
            e->code = o->code;
            td_copy(e->msg, sizeof(e->msg), o->text);
            ok = QACTIVE_POST_X(o->who, &e->super, KODE_TEST_DOT_POST_MARGIN, &me->super);
        }
    }
    else
    {
        TestDotLendEvt *e = Q_NEW_X(TestDotLendEvt, KODE_TEST_DOT_POOL_MARGIN, TEST_DOT_LEND_SIG);
        if ( NULL != e )
        {
            e->seq  = o->seq;
            e->data = o->data;
            e->len  = o->len;
            td_copy(e->name, sizeof(e->name), o->text);
            ok = QACTIVE_POST_X(o->who, &e->super, KODE_TEST_DOT_POST_MARGIN, &me->super);
        }
        if ( ok )
        {
            /* Back within the bound, or the line goes on without it. */
            me->lent = true;
            timer_arm(&me->drain, &me->drain_stale, &me->drain_for, (uint8_t)DRAIN_LEND, KODE_TEST_DOT_BLOB_MS);
        }
    }
    return ok;
}

/**
 * @brief Post the waiting entries in order until one is refused or a blob is
 *        lent; an empty FIFO goes back to PSRAM once the AO is dormant.
 */
static void out_drain(TestDot *me)
{
    bool go = true;

    while ( go && (0U < me->out_n) && !me->lent )
    {
        if ( out_post(me, &me->out[me->out_head]) )
        {
            me->out_head = (uint8_t)((me->out_head + 1U) % KODE_TEST_DOT_OUT_MAX);
            me->out_n--;
        }
        else
        {
            go = false;
            if ( !me->lent )
            {
                timer_arm(&me->drain, &me->drain_stale, &me->drain_for, (uint8_t)DRAIN_RETRY,
                          KODE_TEST_DOT_DRAIN_MS);
            }
        }
    }
    if ( (0U == me->out_n) && (NULL != me->out) && ((uint8_t)MODE_DORMANT == me->mode) )
    {
        khal_free(me->out);
        me->out = NULL;
    }
}

/**
 * @brief The next out entry, its kind and its asker set and the rest empty,
 *        for the caller to fill in place; out_commit() queues it.
 * @return The entry, or NULL with no room (logged): nothing to commit then.
 */
static td_out_t *out_slot(TestDot *me, uint8_t kind, QActive *who, uint8_t seq)
{
    td_out_t *o = NULL;

    if ( NULL == me->out )
    {
        me->out = (td_out_t *)khal_alloc_psram(TD_ALIGN, sizeof(td_out_t) * KODE_TEST_DOT_OUT_MAX);
        me->out_head = 0U;
        me->out_n    = 0U;
    }
    if ( (NULL == me->out) || ((uint8_t)KODE_TEST_DOT_OUT_MAX <= me->out_n) )
    {
        KLOG_E(TAG, "no room for a record to the console; dropped");
    }
    else
    {
        o = &me->out[(me->out_head + me->out_n) % KODE_TEST_DOT_OUT_MAX];
        o->kind    = kind;
        o->seq     = seq;
        o->code    = 0U;
        o->who     = who;
        o->data    = NULL;
        o->len     = 0U;
        o->text[0] = '\0';
    }
    return o;
}

/** @brief The entry out_slot() gave joins the FIFO, and what can go is posted. */
static void out_commit(TestDot *me)
{
    me->out_n++;
    out_drain(me);
}

/** @brief A record to @p who under @p seq. */
static void rec_to(TestDot *me, QActive *who, uint8_t seq, const char *text)
{
    td_out_t * const o = out_slot(me, (uint8_t)OUT_REC, who, seq);

    if ( NULL != o )
    {
        td_copy(o->text, sizeof(o->text), text);
        out_commit(me);
    }
}

/** @brief The end of command @p seq: 0 for `>ok`, else the code and its word. */
static void ans_to(TestDot *me, QActive *who, uint8_t seq, uint8_t code, const char *msg)
{
    td_out_t * const o = out_slot(me, (uint8_t)OUT_ANS, who, seq);

    if ( NULL != o )
    {
        o->code = code;
        if ( NULL != msg )
        {
            td_copy(o->text, sizeof(o->text), msg);
        }
        out_commit(me);
    }
}

/** @brief Lend @p len bytes at @p data as blob @p name. */
static void lend_to(TestDot *me, QActive *who, uint8_t seq, const char *name, const uint8_t *data, uint32_t len)
{
    td_out_t * const o = out_slot(me, (uint8_t)OUT_LEND, who, seq);

    if ( NULL != o )
    {
        o->data = data;
        o->len  = len;
        td_copy(o->text, sizeof(o->text), name);
        out_commit(me);
    }
}

/**
 * @brief Log a record that did not fit: it goes out cut, its tail lost.
 * @param n    What vsnprintf() returned.
 * @param size The room the record had.
 * @param text The record as written.
 */
static void rec_cut_check(int n, size_t size, const char *text)
{
    if ( (0 > n) || ((size_t)n >= size) )
    {
        KLOG_E(TAG, "record cut at %u bytes (%d wanted): %.*s", (unsigned)size, n, (int)TD_REC_CUT_HEAD, text);
    }
}

/** @brief A formatted record for the verb in the foreground, written in place. */
static void rec(TestDot *me, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void rec(TestDot *me, const char *fmt, ...)
{
    td_out_t * const o = out_slot(me, (uint8_t)OUT_REC, me->cur_who, me->cur_seq);

    if ( NULL != o )
    {
        va_list ap;

        va_start(ap, fmt);
        rec_cut_check(vsnprintf(o->text, sizeof(o->text), fmt, ap), sizeof(o->text), o->text);
        va_end(ap);
        out_commit(me);
    }
}

/** @brief The end of the verb in the foreground; the activity is over. */
static void ans(TestDot *me, uint8_t code, const char *msg)
{
    me->act = (uint8_t)ACT_NONE;
    ans_to(me, me->cur_who, me->cur_seq, code, msg);
}

/** @brief Room for one more record in the poll ring; the oldest goes when it is full. NULL with no ring. */
static char *ring_room(TestDot *me)
{
    char *t = NULL;

    if ( NULL == me->ring )
    {
        KLOG_W(TAG, "no poll ring; a record of the run is dropped");
    }
    else
    {
        if ( (uint8_t)KODE_TEST_DOT_POLL_RING <= me->ring_n )
        {
            me->ring_head = (uint8_t)((me->ring_head + 1U) % KODE_TEST_DOT_POLL_RING);
            me->ring_n--;
            me->ring_dropped++;
        }
        t = me->ring[(me->ring_head + me->ring_n) % KODE_TEST_DOT_POLL_RING];
        t[0] = '\0';
    }
    return t;
}

/**
 * @brief Room for the run's next record: on its line while it is open, else
 *        in the poll ring. run_put() keeps what was written there.
 * @return KODE_FUNCTION_REC_MAX bytes, or NULL with no room (logged).
 */
static char *run_room(TestDot *me)
{
    char *t = NULL;

    if ( me->line_open )
    {
        td_out_t * const o = out_slot(me, (uint8_t)OUT_REC, me->run_who, me->run_seq);
        if ( NULL != o )
        {
            t = o->text;
        }
    }
    else
    {
        t = ring_room(me);
    }
    return t;
}

/** @brief The record written at run_room() is kept: posted, or counted in the ring. */
static void run_put(TestDot *me)
{
    if ( me->line_open )
    {
        out_commit(me);
    }
    else
    {
        me->ring_n++;
    }
}

/** @brief A record of the run, written in place: on its line while it is open, else into the poll ring. */
static void run_rec(TestDot *me, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void run_rec(TestDot *me, const char *fmt, ...)
{
    char * const t = run_room(me);

    if ( NULL != t )
    {
        va_list ap;

        va_start(ap, fmt);
        rec_cut_check(vsnprintf(t, KODE_FUNCTION_REC_MAX, fmt, ap), KODE_FUNCTION_REC_MAX, t);
        va_end(ap);
        run_put(me);
    }
}

/** @brief A record of the camera part: the run's, or the verb's. */
static void cam_rec(TestDot *me, const char *text)
{
    if ( me->cam_in_run )
    {
        char * const t = run_room(me);
        if ( NULL != t )
        {
            td_copy(t, KODE_FUNCTION_REC_MAX, text);
            run_put(me);
        }
    }
    else
    {
        rec_to(me, me->cur_who, me->cur_seq, text);
    }
}

/** @brief A record of the camera part, kept when it is of the part asked. */
static void cam_emit(TestDot *me, uint8_t part, const char *text)
{
    if ( ((uint8_t)TEST_DOT_CAM_ALL == me->cam_part) || (me->cam_part == part) )
    {
        cam_rec(me, text);
    }
}

/** @brief Lend blob @p i of the camera part, on the run's line or the verb's. */
static void cam_lend(TestDot *me, uint8_t i)
{
    td_blob_t const * const b = &me->cam->blob[i];

    if ( !me->cam_in_run )
    {
        lend_to(me, me->cur_who, me->cur_seq, b->name, b->data, b->len);
    }
    else if ( me->line_open )
    {
        lend_to(me, me->run_who, me->run_seq, b->name, b->data, b->len);
    }
    else
    {
        /* The line closed under it: the station asks for it by name. */
    }
}

/* ---- The worker ------------------------------------------------------------------ */

/** @brief Start the worker the first time it is needed: it cannot be stopped, so it stays. */
static bool worker_ensure(TestDot *me)
{
    if ( !me->worker_up )
    {
        me->worker_up = (kqp_worker_start(&me->worker, &me->super, "test_dot_worker", KODE_TEST_DOT_WORKER_STACK,
                                          (uint8_t)KODE_PRIO_TEST_DOT_WORKER, (uint8_t)KODE_TEST_DOT_WORKER_DEPTH,
                                          true)
                         == KODE_OK);
        if ( !me->worker_up )
        {
            KLOG_E(TAG, "the worker would not start");
        }
    }
    return me->worker_up;
}

/**
 * @brief Hand the worker one job, bounded by KODE_TEST_DOT_JOB_MS.
 *        A job given up still holds the worker: none is taken until it answers.
 * @return false when it could not take it.
 */
static bool job_submit(TestDot *me, uint8_t kind, uint8_t arg)
{
    bool ok = false;

    if ( worker_ensure(me) && !me->job_busy )
    {
        me->job.kind = kind;
        me->job.arg  = arg;
        me->job.cam  = me->cam;
        me->job_busy = true;
        if ( kqp_worker_submit(&me->worker, &td_job_run, &me->job, (enum_t)TEST_DOT_JOB_SIG, &me->job_seq)
            == KODE_OK )
        {
            ok = true;
            tmo_arm(me, (uint8_t)TMO_JOB, KODE_TEST_DOT_JOB_MS);
        }
        else
        {
            me->job_busy = false;
        }
    }
    return ok;
}

/* ---- The wake ----------------------------------------------------------------- */

/** @brief The published signals a woken AO listens to. */
static const enum_t WAKE_SUBS[] = {
    (enum_t)EXP_POWER_SIG,    (enum_t)DISPLAY_STATE_SIG, (enum_t)AUTOTEST_BEGIN_SIG, (enum_t)AUTOTEST_ROW_SIG,
    (enum_t)AUTOTEST_END_SIG, (enum_t)BTN_TEST_SIG,      (enum_t)TOUCH_TEST_SIG,     (enum_t)TEST_RESULT_SIG,
};

/** @brief Listen to the owners' answers, or stop. */
static void subscribe(TestDot *me, bool on)
{
    if ( on != me->subscribed )
    {
        for ( size_t i = 0U; (sizeof(WAKE_SUBS) / sizeof(WAKE_SUBS[0])) > i; i++ )
        {
            if ( on )
            {
                QActive_subscribe(&me->super, WAKE_SUBS[i]);
            }
            else
            {
                QActive_unsubscribe(&me->super, WAKE_SUBS[i]);
            }
        }
        me->subscribed = on;
    }
}

/** @brief Whether verb @p v needs the station woken. */
static bool verb_wakes(uint8_t v)
{
    return ((uint8_t)TEST_DOT_VERB_PLACE == v) || ((uint8_t)TEST_DOT_VERB_RUN == v)
           || ((uint8_t)TEST_DOT_VERB_RELAYS == v) || ((uint8_t)TEST_DOT_VERB_ADC == v)
           || ((uint8_t)TEST_DOT_VERB_CAMERA == v);
}

/** @brief Whether verb @p v needs the camera's memory. */
static bool verb_needs_cam(uint8_t v)
{
    return ((uint8_t)TEST_DOT_VERB_PLACE == v) || ((uint8_t)TEST_DOT_VERB_RUN == v)
           || ((uint8_t)TEST_DOT_VERB_CAMERA == v);
}

/** @brief Start the wake for @p e: J3's rail first. */
static void wake_begin(TestDot *me, TestDotReqEvt const *e)
{
    me->pend    = *e;
    me->cur_who = e->who;
    me->cur_seq = e->seq;
    me->act     = (uint8_t)ACT_WAKE;
    me->wake_t0 = khal_time_ms();
    if ( !worker_ensure(me) )
    {
        ans(me, TD_ERR_BUSY, "no_memory");
        return;
    }
    me->mode = (uint8_t)MODE_WAKING;
    subscribe(me, true);
    me->rail_on = true;
    exp_power_request(true);
    tmo_arm(me, (uint8_t)TMO_RAIL, KODE_TEST_DOT_RAIL_MS);
    KLOG_N(TAG, "wake: J3's rail asked");
}

/** @brief The wake failed on @p why: the relays at rest and the camera closed, then the rail off. */
static void wake_fail(TestDot *me, const char *why)
{
    KLOG_W(TAG, "wake: %s; giving J3 back", why);
    tmo_stop(me);
    (void)snprintf(me->wake_why, sizeof(me->wake_why), "%s", why);
    me->job.rest = me->station_seen;
    if ( !job_submit(me, (uint8_t)TD_JOB_CLOSE, 0U) )
    {
        KLOG_E(TAG, "wake: the close could not be asked");
    }
}

/** @brief The wake's close is done: the rail off, then the refusal. */
static void wake_closed(TestDot *me)
{
    if ( me->rail_on )
    {
        exp_power_request(false);
    }
    me->rail_on      = false;
    me->probed       = false;
    me->station_seen = false;
    subscribe(me, false);
    me->mode = (uint8_t)MODE_DORMANT;
    if ( ((uint8_t)TEST_DOT_VERB_PLACE == me->pend.verb) && (strcmp(me->wake_why, "camera") == 0) )
    {
        rec(me, "place state=no_camera");
        rec(me, "place result=cam_missing");
        ans(me, 0U, NULL);
    }
    else
    {
        ans(me, TD_ERR_BUSY, me->wake_why);
    }
}

/** @brief The station answered: the glass up, then the verb that woke the AO. */
static void wake_done(TestDot *me)
{
    me->wake_ms = khal_time_ms() - me->wake_t0;
    me->mode    = (uint8_t)MODE_AWAKE;
    me->act     = (uint8_t)ACT_NONE;
    /* The level before, then full: AO_Display answers the first before it takes the second. */
    me->bright_wait = true;
    display_state_request();
    display_brightness_request(TD_BRIGHT_FULL, false);
    KLOG_N(TAG, "wake: the station answers (%lu ms)", (unsigned long)me->wake_ms);
    verb_start(me, &me->pend);
}

/* ---- The camera part ---------------------------------------------------------------- */

/** @brief Hand the worker the step in hand; a refusal goes on as a step that failed. */
static void cam_after(TestDot *me, kode_status_t rc);

/** @brief Run the step in hand on the worker. */
static void cam_submit(TestDot *me)
{
    if ( !job_submit(me, (uint8_t)TD_JOB_CAM, me->cam_step) )
    {
        KLOG_E(TAG, "camera: step %u not taken by the worker", (unsigned)me->cam_step);
        cam_after(me, KODE_NO_RESOURCES);
    }
}

/** @brief The camera part's timing record: the run's or the verb's, the same words. */
#define TD_CAM_TIMING "timing=camera photos_ms=%lu analysis_ms=%lu led_ms=%lu total_ms=%lu"

/** @brief The camera part's end: its timing, then the run's line closes or the verb answers. */
static void cam_finish(TestDot *me)
{
    td_cam_t const * const c = me->cam;
    unsigned long const total = (unsigned long)(khal_time_ms() - me->cam_t0);

    if ( me->cam_in_run )
    {
        run_rec(me, TD_CAM_TIMING, (unsigned long)c->photo_ms, (unsigned long)c->an_ms, (unsigned long)c->led_ms,
                total);
    }
    else
    {
        rec(me, TD_CAM_TIMING, (unsigned long)c->photo_ms, (unsigned long)c->an_ms, (unsigned long)c->led_ms, total);
    }
    me->act = (uint8_t)ACT_NONE;
    if ( me->cam_in_run )
    {
        me->cam_end_ms = khal_time_ms() - me->run_t0;
        ans_to(me, me->run_who, me->run_seq, 0U, NULL);
        me->line_open = false;
        /* The operator, and beside it the rest of the rows. */
        me->op = (uint8_t)OP_BUTTONS;
        (void)memset(me->keys, TEST_DOT_ST_PENDING, sizeof(me->keys));
        screen_ask(me, TEST_DOT_SCR_BUTTONS, me->keys, (uint8_t)sizeof(me->keys));
        rest_arm(me, (uint8_t)REST_OPERATOR, KODE_TEST_DOT_OP_SETTLE_MS);
        me->rows_list = 1U;
        me->rows_idx  = 0U;
        row_next(me);
    }
    else
    {
        uint8_t const none = 0U;
        screen_ask(me, TEST_DOT_PAT_OFF, &none, 1U);
        ans(me, 0U, NULL);
    }
}

/** @brief The step in hand returned @p rc: its records, then the next step. */
static void cam_after(TestDot *me, kode_status_t rc)
{
    td_cam_t * const c = me->cam;
    uint8_t next = (uint8_t)(me->cam_step + 1U);

    if ( KODE_OK != rc )
    {
        KLOG_W(TAG, "camera: step %u answered %d", (unsigned)me->cam_step, (int)rc);
    }
    /* The verdicts' records, formatted on the worker: copied out in order. */
    for ( uint8_t i = 0U; i < c->ncrec; i++ )
    {
        cam_emit(me, c->crec[i].part, c->crec[i].text);
    }
    c->ncrec = 0U;
    if ( (uint8_t)TD_CS_MARKS == me->cam_step )
    {
        uint8_t const loc = td_cam_blob_find(c, "location");
        if ( ((uint8_t)TD_BLOBS_MAX > loc)
            && (((uint8_t)TEST_DOT_CAM_ALL == me->cam_part) || ((uint8_t)TEST_DOT_CAM_FRAMING == me->cam_part)) )
        {
            cam_lend(me, loc);
        }
    }
    else if ( (uint8_t)TD_CS_OFF == me->cam_step )
    {
        display_brightness_request(TD_BRIGHT_FULL, false);
    }
    else if ( (uint8_t)TD_CS_JUDGE == me->cam_step )
    {
        if ( (uint8_t)TEST_DOT_CAM_ALL == me->cam_part )
        {
            for ( uint8_t i = 0U; i < c->nblob; i++ )
            {
                if ( strcmp(c->blob[i].name, "location") != 0 )
                {
                    cam_lend(me, i);
                }
            }
        }
    }
    else
    {
        /* A photo: what it measured waits for its verdict; a verdict's records are out. */
    }
    if ( ((uint8_t)TD_CS_LED_OFF == next) && ((uint8_t)TEST_DOT_CAM_ALL != me->cam_part)
        && ((uint8_t)TEST_DOT_CAM_LED != me->cam_part) )
    {
        next = (uint8_t)TD_CS_DONE;
    }
    cam_step_go(me, next);
}

/** @brief Start step @p step: what goes on the glass or the LED before its photo. */
static void cam_step_go(TestDot *me, uint8_t step)
{
    me->cam_step = step;
    if ( me->cam_abort )
    {
        /* The run ended under the camera part: what is left is not taken. */
        me->act = (uint8_t)ACT_NONE;
        return;
    }
    switch ( step )
    {
        case (uint8_t)TD_CS_MARKS:
            pattern_wait(me, TEST_DOT_PAT_MARKS, 0U);
            break;
        case (uint8_t)TD_CS_OFF:
            /* The panel dark under the same pattern: what the room adds. */
            display_brightness_request(TD_BRIGHT_OFF, false);
            rest_arm(me, (uint8_t)REST_CAMERA, KODE_TEST_DOT_OFF_SETTLE_MS);
            break;
        case (uint8_t)TD_CS_WHITE:
            pattern_wait(me, TEST_DOT_PAT_COLOUR, TEST_DOT_COLOUR_WHITE);
            break;
        case (uint8_t)TD_CS_GREY:
            pattern_wait(me, TEST_DOT_PAT_GREY, TEST_DOT_GREY_RUN);
            break;
        case (uint8_t)TD_CS_RED:
            pattern_wait(me, TEST_DOT_PAT_COLOUR, TEST_DOT_COLOUR_RED);
            break;
        case (uint8_t)TD_CS_GREEN:
            pattern_wait(me, TEST_DOT_PAT_COLOUR, TEST_DOT_COLOUR_GREEN);
            break;
        case (uint8_t)TD_CS_BLUE:
            pattern_wait(me, TEST_DOT_PAT_COLOUR, TEST_DOT_COLOUR_BLUE);
            break;
        case (uint8_t)TD_CS_JUDGE:
            cam_submit(me);
            break;
        case (uint8_t)TD_CS_LED_OFF:
            screen_wait(me, TEST_DOT_SCR_LED, NULL, 0U);
            break;
        case (uint8_t)TD_CS_LED_RED:
            led_set(TD_LED_FULL, TD_LED_DARK, TD_LED_DARK);
            rest_arm(me, (uint8_t)REST_CAMERA, KODE_TEST_DOT_LED_SETTLE_MS);
            break;
        case (uint8_t)TD_CS_LED_GREEN:
            led_set(TD_LED_DARK, TD_LED_FULL, TD_LED_DARK);
            rest_arm(me, (uint8_t)REST_CAMERA, KODE_TEST_DOT_LED_SETTLE_MS);
            break;
        case (uint8_t)TD_CS_LED_BLUE:
            led_set(TD_LED_DARK, TD_LED_DARK, TD_LED_FULL);
            rest_arm(me, (uint8_t)REST_CAMERA, KODE_TEST_DOT_LED_SETTLE_MS);
            break;
        case (uint8_t)TD_CS_LED_JUDGE:
            led_off();
            cam_submit(me);
            break;
        default:
            cam_finish(me);
            break;
    }
}

/** @brief Start the camera part: the whole of it for a run, or the part `test_dot_camera` asked. */
static void cam_begin(TestDot *me, bool in_run, uint8_t part)
{
    me->act        = (uint8_t)ACT_CAMERA;
    me->cam_in_run = in_run;
    me->cam_part   = part;
    me->cam_abort  = false;
    me->cam_t0     = khal_time_ms();
    if ( (uint8_t)TEST_DOT_CAM_LED == part )
    {
        cam_step_go(me, (uint8_t)TD_CS_LED_OFF);
    }
    else
    {
        cam_step_go(me, (uint8_t)TD_CS_MARKS);
    }
}

/* ---- The run --------------------------------------------------------------------------- */

/** @brief The rows of list @p l. */
static size_t rows_n(uint8_t l)
{
    size_t n = sizeof(ROWS_AFTER) / sizeof(ROWS_AFTER[0]);

    if ( 0U == l )
    {
        n = sizeof(ROWS_FIRST) / sizeof(ROWS_FIRST[0]);
    }
    return n;
}

/** @brief Row @p i of list @p l. */
static const char *rows_name(uint8_t l, uint8_t i)
{
    const char *n = ROWS_AFTER[i];

    if ( 0U == l )
    {
        n = ROWS_FIRST[i];
    }
    return n;
}

/** @brief Keep @p ms as row @p name's time. */
static void row_time(TestDot *me, const char *name, uint32_t ms)
{
    for ( uint8_t i = 0U; TD_ROWS_TOTAL > i; i++ )
    {
        if ( strcmp(ROWS_ALL[i], name) == 0 )
        {
            me->row_ms[i] = ms;
        }
    }
}

/** @brief The operator's guided test, if one is asked or running, ends with no verdict; no screen of it follows. */
static void op_cancel(TestDot *me)
{
    if ( (uint8_t)OP_BUTTONS == me->op )
    {
        buttons_test_cancel_request(TEST_ORIGIN_TEST_DOT);
    }
    else if ( (uint8_t)OP_TOUCH == me->op )
    {
        touch_test_cancel_request(TEST_ORIGIN_TEST_DOT);
    }
    else
    {
        /* None asked. */
    }
    me->op = (uint8_t)OP_NONE;
}

/**
 * @brief The run ends before its time: @p why, the line answered with @p code
 *        if still open, FAIL on the glass. Nothing of the run paints over it
 *        afterwards: the rows and the operator's steps are a running run's.
 */
static void run_abort(TestDot *me, const char *result, const char *why, uint8_t code)
{
    uint8_t const fail = TEST_DOT_ST_FAILED;

    run_rec(me, "test=all result=%s why=%s", result, why);
    if ( me->line_open )
    {
        ans_to(me, me->run_who, me->run_seq, code, "test");
        me->line_open = false;
    }
    me->run      = (uint8_t)RUN_ABORTED;
    s_run_going  = false;
    (void)QTimeEvt_disarm(&me->watch);
    autotest_cancel_request(AUTOTEST_CLIENT_TEST_DOT);
    op_cancel(me);
    if ( (uint8_t)ACT_CAMERA == me->act )
    {
        me->cam_abort = true;
    }
    led_off();
    screen_ask(me, TEST_DOT_SCR_RESULT, &fail, 1U);
}

/** @brief The run is over when the rows after the camera and the operator both are. */
static void run_check_done(TestDot *me)
{
    if ( ((uint8_t)RUN_GOING == me->run) && me->rows_done && ((uint8_t)OP_DONE == me->op) )
    {
        char * const t = run_room(me);

        if ( NULL != t )
        {
            size_t n = (size_t)snprintf(t, KODE_FUNCTION_REC_MAX, "timing=all total_ms=%lu cam_end_ms=%lu",
                                        (unsigned long)(khal_time_ms() - me->run_t0), (unsigned long)me->cam_end_ms);
            for ( uint8_t i = 0U; (TD_ROWS_TOTAL > i) && (KODE_FUNCTION_REC_MAX > n); i++ )
            {
                int const w = snprintf(&t[n], KODE_FUNCTION_REC_MAX - n, " %s_ms=%lu", ROWS_ALL[i],
                                       (unsigned long)me->row_ms[i]);
                if ( 0 < w )
                {
                    n += (size_t)w;
                }
            }
            run_put(me);
        }
        led_off();
        me->run     = (uint8_t)RUN_DONE;
        s_run_going = false;
        (void)QTimeEvt_disarm(&me->watch);
        KLOG_N(TAG, "run: done");
    }
}

/** @brief Ask AO_Autotest for the next row, or move on when the list is over. */
static void row_next(TestDot *me)
{
    if ( (uint8_t)RUN_GOING != me->run )
    {
        return;
    }
    if ( rows_n(me->rows_list) <= me->rows_idx )
    {
        if ( 0U == me->rows_list )
        {
            cam_begin(me, true, (uint8_t)TEST_DOT_CAM_ALL);
        }
        else
        {
            me->rows_done = true;
            run_check_done(me);
        }
        return;
    }
    {
        const char * const name = rows_name(me->rows_list, me->rows_idx);
        grid_set(me, name, TEST_DOT_ST_RUNNING);
        me->row_t0 = khal_time_ms();
        autotest_run_request(AUTOTEST_CLIENT_TEST_DOT, AUTOTEST_G_NONE, name);
    }
}

/** @brief AO_Autotest took the row or not. */
static void on_row_begin(TestDot *me, AutotestBeginEvt const *e)
{
    const char *name;

    if ( (AUTOTEST_CLIENT_TEST_DOT != e->client) || ((uint8_t)RUN_GOING != me->run)
        || (rows_n(me->rows_list) <= me->rows_idx) )
    {
        return;
    }
    name = rows_name(me->rows_list, me->rows_idx);
    if ( AUTOTEST_TAKEN != e->status )
    {
        const char *w = "absent";
        if ( AUTOTEST_BUSY == e->status )
        {
            w = "busy";
        }
        run_rec(me, "test=%s result=%s", name, w);
        grid_set(me, name, TEST_DOT_ST_FAILED);
        if ( strcmp(name, "sd") == 0 )
        {
            run_abort(me, "aborted", "sd_missing", TD_ERR_REJECT);
        }
        else
        {
            me->rows_idx++;
            row_next(me);
        }
    }
}

/** @brief A row ended: its record and its cell; no SD ends the run. */
static void on_row(TestDot *me, AutotestRowEvt const *e)
{
    const char *name;
    uint32_t ms;

    if ( (AUTOTEST_CLIENT_TEST_DOT != e->client) || ((uint8_t)RUN_GOING != me->run)
        || (AUTOTEST_ROW_RUNNING == e->state) || (rows_n(me->rows_list) <= me->rows_idx) )
    {
        return;
    }
    name = rows_name(me->rows_list, me->rows_idx);
    ms   = khal_time_ms() - me->row_t0;
    row_time(me, name, ms);
    run_rec(me, "test=%s result=%s ms=%lu", name, td_word(ROW_WORD, AUTOTEST_ROW_STATES, e->state),
            (unsigned long)ms);
    if ( AUTOTEST_ROW_PASS == e->state )
    {
        grid_set(me, name, TEST_DOT_ST_OK);
    }
    else
    {
        grid_set(me, name, TEST_DOT_ST_FAILED);
        if ( strcmp(name, "sd") == 0 )
        {
            /* No card, nothing written: the rest would judge a Kode that cannot keep its log. */
            run_abort(me, "aborted", "sd_missing", TD_ERR_REJECT);
        }
    }
}

/** @brief The row's run is over: the next row. */
static void on_row_end(TestDot *me, AutotestEndEvt const *e)
{
    if ( (AUTOTEST_CLIENT_TEST_DOT == e->client) && ((uint8_t)RUN_GOING == me->run) )
    {
        me->rows_idx++;
        row_next(me);
    }
}

/** @brief `test component=all`: the rows before the camera, from a fresh pass that keeps the placement. */
static void run_start(TestDot *me)
{
    if ( NULL == me->ring )
    {
        me->ring = (char (*)[KODE_FUNCTION_REC_MAX])khal_alloc_psram(
            TD_ALIGN, (size_t)KODE_TEST_DOT_POLL_RING * KODE_FUNCTION_REC_MAX);
    }
    if ( NULL == me->ring )
    {
        ans(me, TD_ERR_BUSY, "no_memory");
        return;
    }
    me->ring_head    = 0U;
    me->ring_n       = 0U;
    me->ring_dropped = 0U;
    td_cam_reset(me->cam, true);
    me->run        = (uint8_t)RUN_GOING;
    s_run_going    = true;
    me->run_who    = me->cur_who;
    me->run_seq    = me->cur_seq;
    me->line_open  = true;
    me->run_t0     = khal_time_ms();
    me->cam_end_ms = 0U;
    me->rows_done  = false;
    me->op         = (uint8_t)OP_NONE;
    (void)memset(me->row_ms, 0, sizeof(me->row_ms));
    (void)memset(me->grid, TEST_DOT_ST_PENDING, sizeof(me->grid));
    me->grid[TD_GRID_ID_CELL] = TEST_DOT_ST_OK;
    (void)QTimeEvt_disarm(&me->watch);
    QTimeEvt_armX(&me->watch, KQP_TICKS_PER_MS(KODE_TEST_DOT_RUN_MS), 0U);
    grid_show(me, true);
    me->act       = (uint8_t)ACT_NONE;
    me->rows_list = 0U;
    me->rows_idx  = 0U;
    KLOG_N(TAG, "run: started");
    row_next(me);
}

/* ---- The operator part ------------------------------------------------------------------ */

/** @brief One step of the guided button test the run asked for: its record, and the key on the glass. */
static void on_buttons(TestDot *me, ButtonsTestEvt const *e)
{
    char    r[KODE_FUNCTION_REC_MAX];
    uint8_t st = 0U;

    if ( (TEST_ORIGIN_TEST_DOT != e->origin) || ((uint8_t)OP_BUTTONS != me->op) || (KDRV_BTN_COUNT <= e->button) )
    {
        return;
    }
    switch ( e->phase )
    {
        case BTN_TEST_PROMPT:
            st = TEST_DOT_ST_CURRENT;
            break;
        case BTN_TEST_PASS:
            st = TEST_DOT_ST_OK;
            break;
        case BTN_TEST_WRONG:
        case BTN_TEST_DOUBLE:
            st = TEST_DOT_ST_AGAIN;
            break;
        case BTN_TEST_TIMEOUT:
            st             = TEST_DOT_ST_FAILED;
            me->op_timeout = true;
            break;
        case BTN_TEST_GIVEUP:
            st            = TEST_DOT_ST_FAILED;
            me->op_giveup = true;
            break;
        case BTN_TEST_STUCK:
        case BTN_TEST_DOUBLEFAIL:
        case BTN_TEST_HELD:
            st = TEST_DOT_ST_FAILED;
            break;
        default:
            /* DONE: the verdict follows. */
            break;
    }
    if ( buttons_test_record(e, r, sizeof(r)) > 0U )
    {
        run_rec(me, "%s", r);
    }
    if ( 0U != st )
    {
        me->keys[e->button] = st;
        screen_ask(me, TEST_DOT_SCR_BUTTONS, me->keys, (uint8_t)sizeof(me->keys));
    }
}

/** @brief One step of the guided touch test the run asked for: its record, and the target on the glass. */
static void on_touch(TestDot *me, TouchTestEvt const *e)
{
    uint8_t const i = (uint8_t)(e->step - 1U);
    char    r[KODE_FUNCTION_REC_MAX];
    uint8_t st = 0U;

    if ( (TEST_ORIGIN_TEST_DOT != e->origin) || ((uint8_t)OP_TOUCH != me->op) || (0U == e->step)
        || (TD_TOUCH_TARGETS <= i) )
    {
        return;
    }
    switch ( e->phase )
    {
        case TP_TEST_PROMPT:
            st = TEST_DOT_ST_CURRENT;
            break;
        case TP_TEST_PASS:
            st = TEST_DOT_ST_OK;
            break;
        case TP_TEST_FAIL:
            st = TEST_DOT_ST_FAILED;
            break;
        case TP_TEST_TIMEOUT:
            st             = TEST_DOT_ST_FAILED;
            me->op_timeout = true;
            break;
        default:
            /* MISS is forgiven and changes nothing on the glass; DONE: the verdict follows. */
            break;
    }
    if ( touch_test_record(e, r, sizeof(r)) > 0U )
    {
        run_rec(me, "%s", r);
    }
    if ( 0U != st )
    {
        me->targets[i] = st;
        screen_ask(me, TEST_DOT_SCR_TOUCH, me->targets, (uint8_t)sizeof(me->targets));
    }
}

/**
 * @brief The word an operator's test is judged with.
 *
 * pass when the test passed; fail on a give-up (the spec, section 8.3);
 * timeout when a step ran out, even beside a step that failed; fail otherwise.
 */
static const char *op_word(TestDot const *me, bool ok)
{
    const char *w = "fail";

    if ( ok )
    {
        w = "pass";
    }
    else if ( me->op_giveup )
    {
        w = "fail";
    }
    else if ( me->op_timeout )
    {
        w = "timeout";
    }
    else
    {
        /* A step failed. */
    }
    return w;
}

/** @brief A guided test's verdict: the buttons lead to the touch, the touch to the grid. */
static void on_verdict(TestDot *me, TestResultEvt const *e)
{
    const char *res;

    if ( (TEST_ORIGIN_TEST_DOT != e->origin) || ((uint8_t)RUN_GOING != me->run) )
    {
        return;
    }
    res = op_word(me, e->ok);
    if ( ((uint8_t)OP_BUTTONS == me->op) && (strcmp(e->name, "buttons") == 0) )
    {
        run_rec(me, "test=buttons result=%s ms=%lu", res, (unsigned long)(khal_time_ms() - me->op_t0));
        me->op = (uint8_t)OP_TOUCH;
        me->targets[0] = TEST_DOT_ST_CURRENT;
        me->targets[1] = TEST_DOT_ST_PENDING;
        screen_ask(me, TEST_DOT_SCR_TOUCH, me->targets, (uint8_t)sizeof(me->targets));
        rest_arm(me, (uint8_t)REST_OPERATOR, KODE_TEST_DOT_OP_SETTLE_MS);
    }
    else if ( ((uint8_t)OP_TOUCH == me->op) && (strcmp(e->name, "tp") == 0) )
    {
        run_rec(me, "test=tp result=%s ms=%lu", res, (unsigned long)(khal_time_ms() - me->op_t0));
        me->op = (uint8_t)OP_DONE;
        grid_show(me, true);
        run_check_done(me);
    }
    else
    {
        /* Another test's verdict. */
    }
}

/* ---- The place ------------------------------------------------------------------------------ */

/** @brief One look for the marks. */
static void place_look(TestDot *me)
{
    me->place_tries++;
    if ( !job_submit(me, (uint8_t)TD_JOB_PLACE, 0U) )
    {
        ans(me, TD_ERR_BUSY, "busy");
    }
}

/** @brief A look answered: placed, another look, or misplaced. */
static void place_after(TestDot *me, kode_status_t rc)
{
    td_cam_t const * const c = me->cam;
    uint32_t const ms = khal_time_ms() - me->place_t0;
    bool good = false;

    if ( KODE_ERROR == rc )
    {
        rec(me, "place check n=%u why=camera", (unsigned)me->place_tries);
        rec(me, "place result=cam_missing");
        ans(me, 0U, NULL);
        return;
    }
    if ( KODE_OK != rc )
    {
        rec(me, "place check n=%u why=no_frame", (unsigned)me->place_tries);
    }
    else
    {
        rec(me, "place check n=%u marks=%d dx=%.2f dy=%.2f ang=%.2f win=\"%u,%u\"", (unsigned)me->place_tries,
            (int)c->nmarks, c->place_dx, c->place_dy, c->place_ang, (unsigned)c->win_x, (unsigned)c->win_y);
        good = c->place_ok && (fabs(c->place_dx) <= me->place_mm) && (fabs(c->place_dy) <= me->place_mm)
               && (fabs(c->place_ang) <= me->place_deg);
    }
    if ( good )
    {
        uint8_t const loc = td_cam_blob_find(c, "location");
        rec(me, "place result=ok dx_mm=%.2f dy_mm=%.2f ang_deg=%.2f marks=%d tries=%u win=\"%u,%u\" ms=%lu",
            c->place_dx, c->place_dy, c->place_ang, (int)c->nmarks, (unsigned)me->place_tries, (unsigned)c->win_x,
            (unsigned)c->win_y, (unsigned long)ms);
        if ( (uint8_t)TD_BLOBS_MAX > loc )
        {
            lend_to(me, me->cur_who, me->cur_seq, c->blob[loc].name, c->blob[loc].data, c->blob[loc].len);
        }
        ans(me, 0U, NULL);
    }
    else if ( ms < me->place_mark_ms )
    {
        rest_arm(me, (uint8_t)REST_PLACE, KODE_TEST_DOT_PLACE_RETRY_MS);
    }
    else
    {
        rec(me, "place state=misplaced");
        rec(me, "place result=misplaced marks=%d dx_mm=%.2f dy_mm=%.2f ang_deg=%.2f limit_mm=%.1f limit_deg=%.1f",
            (int)c->nmarks, c->place_dx, c->place_dy, c->place_ang, me->place_mm, me->place_deg);
        ans(me, 0U, NULL);
    }
}

/** @brief `test_dot_place`: look for the marks until they sit within the limits or the time is up. */
static void place_start(TestDot *me, TestDotReqEvt const *e)
{
    td_cam_t * const c = me->cam;

    if ( (int16_t)TD_NOT_GIVEN != e->a.place.win_x )
    {
        c->win_x = (uint16_t)e->a.place.win_x;
    }
    if ( (int16_t)TD_NOT_GIVEN != e->a.place.win_y )
    {
        c->win_y = (uint16_t)e->a.place.win_y;
    }
    me->place_mark_ms = e->a.place.mark_ms;
    me->place_mm      = (double)e->a.place.dx_mm10 / TD_MM10_PER_MM;
    me->place_deg     = (double)e->a.place.deg10 / TD_MM10_PER_MM;
    me->place_tries   = 0U;
    me->place_t0      = khal_time_ms();
    me->act           = (uint8_t)ACT_PLACE;
    rec(me, "place mem psram_free=%lu largest=%lu", (unsigned long)khal_psram_free(),
        (unsigned long)khal_psram_largest());
    rec(me, "place init ok=1 ms=%lu why=-", (unsigned long)me->wake_ms);
    rec(me, "place state=searching");
    place_look(me);
}

/* ---- The photos ---------------------------------------------------------------------------- */

/** @brief The next photo, or the summary. */
static void photo_next(TestDot *me)
{
    if ( me->ph_idx > me->ph_last )
    {
        rec(me, "photos n=%u bytes=%lu ms=%lu", (unsigned)me->ph_sent, (unsigned long)me->ph_bytes,
            (unsigned long)(khal_time_ms() - me->ph_t0));
        ans(me, 0U, NULL);
    }
    else if ( !job_submit(me, (uint8_t)TD_JOB_PHOTO_OUT, me->ph_idx) )
    {
        ans(me, TD_ERR_BUSY, "busy");
    }
    else
    {
        /* The JPEG comes back with the job. */
    }
}

/** @brief A photo's JPEG is ready: lent, and the next waits until it is back. */
static void photo_after(TestDot *me, kode_status_t rc)
{
    td_cam_t const * const c = me->cam;

    if ( KODE_OK == rc )
    {
        td_out_t * const o = out_slot(me, (uint8_t)OUT_LEND, me->cur_who, me->cur_seq);
        me->ph_sent++;
        me->ph_bytes += c->jout_len;
        if ( NULL != o )
        {
            (void)snprintf(o->text, sizeof(o->text), "photo_%02u_%s",
                           (unsigned)me->ph_idx + TEST_DOT_PHOTO_FIRST, c->photo[me->ph_idx].name);
            o->data = c->jout;
            o->len  = c->jout_len;
            me->ph_wait_back = true;
            out_commit(me);
        }
        else
        {
            /* Not lent, so nothing comes back: the next. */
            me->ph_idx++;
            photo_next(me);
        }
    }
    else
    {
        rec(me, "photo n=%u why=encode", (unsigned)me->ph_idx + TEST_DOT_PHOTO_FIRST);
        me->ph_idx++;
        photo_next(me);
    }
}

/**
 * @brief `test_dot_photos [only=]`: the kept photos, as JPEG blobs, one at a time.
 *
 * Numbered from TEST_DOT_PHOTO_FIRST, as the station counts them. The whole
 * set says first how many to expect (`photos total=`); `only=` is the
 * station's retry of one photo, and says no total.
 */
static void photos_start(TestDot *me, TestDotReqEvt const *e)
{
    uint8_t n = 0U;

    if ( NULL != me->cam )
    {
        n = me->cam->nphoto;
    }
    me->ph_idx   = 0U;
    me->ph_sent  = 0U;
    me->ph_bytes = 0U;
    me->ph_t0    = khal_time_ms();
    if ( e->a.photos.one )
    {
        if ( (TEST_DOT_PHOTO_FIRST <= e->a.photos.only) && ((e->a.photos.only - TEST_DOT_PHOTO_FIRST) < n) )
        {
            me->ph_idx = (uint8_t)(e->a.photos.only - TEST_DOT_PHOTO_FIRST);
            n = 1U;
        }
        else
        {
            n = 0U;
        }
    }
    else
    {
        rec(me, "photos total=%u", (unsigned)n);
    }
    if ( 0U == n )
    {
        rec(me, "photos n=0 bytes=0 ms=0");
        ans(me, 0U, NULL);
        return;
    }
    me->ph_last = (uint8_t)(me->ph_idx + n - 1U);
    me->act     = (uint8_t)ACT_PHOTOS;
    photo_next(me);
}

/* ---- The pads, the radio and the boost -------------------------------------------------------- */

/** @brief The loop's record and its end, once the dock console is back. */
static void loop_report(TestDot *me)
{
    kdrv_dock_loop_t const * const l = &me->job.loop;

    rec(me, "loop ok=%u b9600=%u/%u b115200=%u/%u rev=%u/%u ms=%lu why=%s",
        td_bit(((uint8_t)KDRV_DOCK_LOOP_OK == l->why) && (l->slow == me->loop_count)),
        (unsigned)l->slow, (unsigned)me->loop_count, (unsigned)l->fast, (unsigned)me->loop_count,
        (unsigned)l->rev, (unsigned)me->loop_count, (unsigned long)l->ms,
        td_word(LOOP_WHY, sizeof(LOOP_WHY) / sizeof(LOOP_WHY[0]), l->why));
    ans(me, 0U, NULL);
}

/** @brief The loop's frames are over: the dock console asked back, its answer bounded; then the report. */
static void loop_return(TestDot *me)
{
    me->loop_phase = (uint8_t)LOOP_RETURNING;
    if ( log_dock_lend_request(&me->super, false) )
    {
        tmo_arm(me, (uint8_t)TMO_ANSWER, KODE_TEST_DOT_ANSWER_MS);
    }
    else
    {
        loop_report(me);
    }
}

/**
 * @brief AO_Log's answer about the dock's pins. The end takes it only while
 *        it waits for it; a late one, its wait run out, only says where the
 *        pins are.
 */
static void on_dock(TestDot *me, LogDockEvt const *e)
{
    me->dock_lent = e->lent;
    if ( (uint8_t)MODE_ENDING == me->mode )
    {
        if ( (uint8_t)END_WAIT_DOCK == me->end_wait )
        {
            me->end_wait = (uint8_t)END_WAIT_NONE;
            tmo_stop(me);
            end_next(me);
        }
    }
    else if ( ((uint8_t)ACT_LOOP == me->act) && ((uint8_t)LOOP_LENDING == me->loop_phase) )
    {
        tmo_stop(me);
        if ( !e->lent )
        {
            ans(me, TD_ERR_BUSY, "dock");
        }
        else
        {
            (void)memset(&me->job.loop, 0, sizeof(me->job.loop));
            me->loop_phase = (uint8_t)LOOP_RUNNING;
            if ( !job_submit(me, (uint8_t)TD_JOB_LOOP, me->loop_count) )
            {
                me->job.loop.why = (uint8_t)KDRV_DOCK_LOOP_UART_BUSY;
                loop_return(me);
            }
        }
    }
    else if ( ((uint8_t)ACT_LOOP == me->act) && ((uint8_t)LOOP_RETURNING == me->loop_phase) )
    {
        tmo_stop(me);
        loop_report(me);
    }
    else
    {
        /* Not asked now. */
    }
}

/* The radio check's records at their longest, every number at its type's
 * widest: each must fit one record, so none is ever cut (the `why=` last). */
#define TD_RADIO_CARD_WORST \
    "radio_card ok=0 chip=esp32c5 id=65535 kode=0 fw=255.255.255 host=255.255.255 mac=00:00:00:00:00:00 " \
    "sdio=255x4294967295kHz" /**< The longest `radio_card`. */
#define TD_RADIO_WORST \
    "radio ok=0 seen24=0 rssi24=-128 ch24=255 seen5=0 rssi5=-128 ch5=255 passive5=0 ble_adv=0 ble_rx=0 " \
    "rssi_rx=-128 why=timeout" /**< The longest `radio`. */
#define TD_RADIO_TIME_WORST \
    "radio_time card_ms=4294967295 w24_ms=4294967295 w5_ms=4294967295 ble_ms=4294967295 ms=4294967295" \
    /**< The longest `radio_time`. */
_Static_assert(sizeof(TD_RADIO_CARD_WORST) <= KODE_FUNCTION_REC_MAX, "the longest radio_card fits one record");
_Static_assert(sizeof(TD_RADIO_WORST) <= KODE_FUNCTION_REC_MAX, "the longest radio fits one record");
_Static_assert(sizeof(TD_RADIO_TIME_WORST) <= KODE_FUNCTION_REC_MAX, "the longest radio_time fits one record");

/**
 * @brief Whether the C5's card passes: an ESP32-C5 running Kode's slave at this image's
 *        esp-hosted version, with its data lines up.
 *
 * @param e AO_Radio's answer to the radio check.
 * @return true when the card, the chip, the slave, the version and the SDIO width all pass.
 */
static bool radio_card_ok(RadioProbeEvt const *e)
{
    return e->card && (RADIO_PROBE_CHIP_C5 == e->chip) && e->kode && (e->fw[0] == e->host[0])
           && (e->fw[1] == e->host[1]) && (e->fw[2] == e->host[2]) && (0U < e->sdio_width);
}

/**
 * @brief AO_Radio's answer to `test_dot_radio`: the card's record, what each
 *        part took, then the readings. The times have a record of their own
 *        so neither is cut.
 */
static void on_radio(TestDot *me, RadioProbeEvt const *e)
{
    const char *why  = "-";
    const char *chip = "other";
    char mac[sizeof("00:00:00:00:00:00")];

    if ( (uint8_t)ACT_RADIO != me->act )
    {
        return;
    }
    tmo_stop(me);
    if ( RADIO_PROBE_CHIP_C5 == e->chip )
    {
        chip = "esp32c5";
    }
    (void)snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x", e->mac[0], e->mac[1], e->mac[2], e->mac[3],
                   e->mac[4], e->mac[5]);
    if ( (uint8_t)KODE_DENIED == e->status )
    {
        why = "link";
    }
    else if ( (uint8_t)KODE_NO_RESOURCES == e->status )
    {
        why = "busy";
    }
    else if ( (uint8_t)KODE_TIMEOUT == e->status )
    {
        why = "timeout";
    }
    else if ( (uint8_t)KODE_OK != e->status )
    {
        why = "error";
    }
    else
    {
        /* Every part answered. */
    }
    rec(me, "radio_card ok=%u chip=%s id=%u kode=%u fw=%u.%u.%u host=%u.%u.%u mac=%s sdio=%ux%lukHz",
        td_bit(radio_card_ok(e)), chip, (unsigned)e->chip, td_bit(e->kode), (unsigned)e->fw[0], (unsigned)e->fw[1],
        (unsigned)e->fw[2], (unsigned)e->host[0], (unsigned)e->host[1], (unsigned)e->host[2], mac,
        (unsigned)e->sdio_width, (unsigned long)e->sdio_khz);
    rec(me, "radio_time card_ms=%lu w24_ms=%lu w5_ms=%lu ble_ms=%lu ms=%lu", (unsigned long)e->card_ms,
        (unsigned long)e->w24_ms, (unsigned long)e->w5_ms, (unsigned long)e->ble_ms, (unsigned long)e->ms);
    rec(me, "radio ok=%u seen24=%u rssi24=%d ch24=%u seen5=%u rssi5=%d ch5=%u passive5=%u ble_adv=%u ble_rx=%u"
            " rssi_rx=%d why=%s",
        td_bit((uint8_t)KODE_OK == e->status), td_bit(e->seen24), (int)e->rssi24, (unsigned)e->ch24,
        td_bit(e->seen5), (int)e->rssi5, (unsigned)e->ch5, td_bit(e->passive5), td_bit(e->ble_adv),
        td_bit(e->ble_rx), (int)e->rssi_rx, why);
    ans(me, 0U, NULL);
}

/**
 * @brief AO_Power's answer about the boost. The end takes it only while it
 *        waits for it: one that arrives after its wait ran out, the end
 *        already a step further, only says where the boost is.
 */
static void on_boost(TestDot *me, PowerBoostTestEvt const *e)
{
    me->boost_on = e->boost;
    if ( (uint8_t)MODE_ENDING == me->mode )
    {
        if ( (uint8_t)END_WAIT_BOOST == me->end_wait )
        {
            me->end_wait = (uint8_t)END_WAIT_NONE;
            tmo_stop(me);
            end_next(me);
        }
    }
    else if ( (uint8_t)ACT_BOOST == me->act )
    {
        tmo_stop(me);
        rec(me, "boost on=%u fault=0x%02X current_ma=%d", td_bit(e->boost), (unsigned)e->fault,
            (int)e->current_ma);
        if ( (uint8_t)KODE_OK == e->status )
        {
            ans(me, 0U, NULL);
        }
        else
        {
            ans(me, TD_ERR_BUSY, "boost");
        }
    }
    else
    {
        /* Not asked now. */
    }
}

/* ---- The result, the lock and the end ---------------------------------------------------------- */

/** @brief Show the result screen, then answer with what @c res_code says. */
static void result_show(TestDot *me, uint8_t mark)
{
    uint8_t m = mark;

    led_off();
    screen_wait(me, TEST_DOT_SCR_RESULT, &m, 1U);
}

/** @brief AO_Boot's answer to the SHIP reset: PASS on DONE, FAIL with its step otherwise. */
static void on_reset(TestDot *me, FactoryResetRspEvt const *e)
{
    if ( ((uint8_t)ACT_RESULT != me->act) || ((uint8_t)RES_PASSING != me->res) )
    {
        return;
    }
    tmo_stop(me);
    if ( ((uint8_t)KBOOT_RESET_STEP_DONE == e->step) && ((uint8_t)KODE_OK == e->status) )
    {
        me->res      = (uint8_t)RES_PASSED;
        me->res_code = 0U;
        me->res_why[0] = '\0';
        result_show(me, TEST_DOT_ST_OK);
    }
    else
    {
        me->res      = (uint8_t)RES_FAILED;
        me->res_code = TD_ERR_BUSY;
        (void)snprintf(me->res_why, sizeof(me->res_why), "%s",
                       td_word(RESET_WORD, sizeof(RESET_WORD) / sizeof(RESET_WORD[0]), e->step));
        result_show(me, TEST_DOT_ST_FAILED);
    }
}

/**
 * @brief `test_dot_result verdict=`: a pass erases the Kode (SHIP reset)
 *        first; a fail erases nothing. A fail during a run ends the run
 *        first, so FAIL stays on the glass; a pass after a fail is refused.
 */
static void result_start(TestDot *me, TestDotReqEvt const *e)
{
    bool const pass = e->a.pass;

    if ( ((uint8_t)RES_PASSING == me->res) || ((uint8_t)RES_PASSED == me->res) )
    {
        ans(me, TD_ERR_BUSY, "busy");
    }
    else if ( pass && ((uint8_t)RES_FAILED == me->res) )
    {
        ans(me, TD_ERR_REJECT, "failed");
    }
    else if ( pass && ((uint8_t)RUN_DONE != me->run) )
    {
        ans(me, TD_ERR_BUSY, "no_run");
    }
    else if ( !pass && ((uint8_t)RUN_NONE == me->run) )
    {
        ans(me, TD_ERR_BUSY, "no_run");
    }
    else if ( pass )
    {
        me->act = (uint8_t)ACT_RESULT;
        if ( boot_factory_reset_request(KBOOT_RESET_SHIP, &me->super) )
        {
            me->res = (uint8_t)RES_PASSING;
            tmo_arm(me, (uint8_t)TMO_RESET, KODE_TEST_DOT_RESET_MS);
            KLOG_N(TAG, "result: pass; the factory reset runs");
        }
        else
        {
            ans(me, TD_ERR_BUSY, "busy");
        }
    }
    else
    {
        if ( (uint8_t)RUN_GOING == me->run )
        {
            run_abort(me, "aborted", "verdict", TD_ERR_REJECT);
        }
        me->act      = (uint8_t)ACT_RESULT;
        me->res      = (uint8_t)RES_FAILED;
        me->res_code = 0U;
        me->res_why[0] = '\0';
        result_show(me, TEST_DOT_ST_FAILED);
    }
}

/** @brief The glass shows what was asked, or the wait ran out: the activity that waited goes on. */
static void screen_shown(TestDot *me, bool shown)
{
    if ( !shown )
    {
        KLOG_W(TAG, "screen 0x%02x: no answer from the host", (unsigned)me->scr_wait);
    }
    if ( (uint8_t)ACT_CAMERA == me->act )
    {
        if ( !shown )
        {
            /* No photo of a glass that may still show the last pattern: the
             * step fails, and its verdict says so. */
            cam_after(me, KODE_TIMEOUT);
        }
        else if ( (uint8_t)TD_CS_LED_OFF == me->cam_step )
        {
            led_off();
            rest_arm(me, (uint8_t)REST_CAMERA, KODE_TEST_DOT_LED_SETTLE_MS);
        }
        else
        {
            cam_submit(me);
        }
    }
    else if ( (uint8_t)ACT_SCREEN == me->act )
    {
        const char *ph = "connecting";
        if ( (uint8_t)TEST_DOT_PHASE_RESULT == me->scr_phase )
        {
            ph = "result";
        }
        rec(me, "screen phase=%s ok=%u", ph, td_bit(shown));
        ans(me, 0U, NULL);
    }
    else if ( (uint8_t)ACT_RESULT == me->act )
    {
        ans(me, me->res_code, me->res_why);
    }
    else
    {
        /* Nobody waits on it any more. */
    }
}

/**
 * @brief The end, one step at a time: a step that waits for an owner sets
 *        @c end_wait and returns; that answer, or its bound, calls it again.
 *
 * Nothing is given back under a job still out. A job past its bound answers
 * the line with a timeout and the rest runs when it answers; every verb is
 * refused `busy` until then.
 */
static void end_next(TestDot *me)
{
    bool go = true;

    while ( go )
    {
        switch ( me->end_step )
        {
            case (uint8_t)END_START:
                if ( (uint8_t)RUN_GOING == me->run )
                {
                    autotest_cancel_request(AUTOTEST_CLIENT_TEST_DOT);
                }
                op_cancel(me);
                me->run         = (uint8_t)RUN_NONE;
                s_run_going     = false;
                me->line_open   = false;
                me->scr_waiting = false;
                (void)QTimeEvt_disarm(&me->watch);
                timer_stop(&me->rest, &me->rest_stale, &me->rest_for);
                led_off();
                me->end_step = (uint8_t)END_BOOST;
                break;
            case (uint8_t)END_BOOST:
                me->end_step = (uint8_t)END_IDLE;
                if ( me->boost_on && power_boost_test_request(&me->super, false) )
                {
                    me->end_wait = (uint8_t)END_WAIT_BOOST;
                    tmo_arm(me, (uint8_t)TMO_END, KODE_TEST_DOT_ANSWER_MS);
                    go = false;
                }
                break;
            case (uint8_t)END_IDLE:
                if ( me->job_busy )
                {
                    /* A job given up still holds the worker, the camera and
                     * maybe the dock's pins: it answers first. */
                    me->end_wait = (uint8_t)END_WAIT_JOB;
                    tmo_arm(me, (uint8_t)TMO_JOB, KODE_TEST_DOT_JOB_MS);
                    go = false;
                }
                else
                {
                    me->end_step = (uint8_t)END_DOCK;
                }
                break;
            case (uint8_t)END_DOCK:
                me->end_step = (uint8_t)END_CLOSE;
                if ( me->dock_lent && log_dock_lend_request(&me->super, false) )
                {
                    me->end_wait = (uint8_t)END_WAIT_DOCK;
                    tmo_arm(me, (uint8_t)TMO_END, KODE_TEST_DOT_ANSWER_MS);
                    go = false;
                }
                break;
            case (uint8_t)END_CLOSE:
                me->end_step = (uint8_t)END_FINISH;
                if ( (me->probed || me->station_seen || me->rail_on) )
                {
                    me->job.rest = me->station_seen;
                    if ( job_submit(me, (uint8_t)TD_JOB_CLOSE, 0U) )
                    {
                        me->end_wait = (uint8_t)END_WAIT_JOB;
                        go = false;
                    }
                }
                break;
            default:
                if ( me->job_busy )
                {
                    /* The close was given up and still runs: the rail stays
                     * on and the memory stays taken until it answers. */
                    me->end_wait = (uint8_t)END_WAIT_JOB;
                    go = false;
                    break;
                }
                tmo_stop(me);
                if ( me->rail_on )
                {
                    exp_power_request(false);
                }
                me->rail_on      = false;
                me->probed       = false;
                me->station_seen = false;
                if ( me->bright_known )
                {
                    display_brightness_request(me->bright, false);
                }
                me->bright_known = false;
                me->bright_wait  = false;
                screen_ask(me, TEST_DOT_SCR_END, NULL, 0U);
                td_cam_free(me->cam);
                me->cam = NULL;
                khal_free(me->ring);
                me->ring   = NULL;
                me->ring_n = 0U;
                subscribe(me, false);
                me->res      = (uint8_t)RES_NONE;
                me->end_wait = (uint8_t)END_WAIT_NONE;
                me->mode     = (uint8_t)MODE_DORMANT;
                KLOG_N(TAG, "end: everything given back");
                if ( me->end_answered )
                {
                    me->end_answered = false;
                    me->act = (uint8_t)ACT_NONE;
                    out_drain(me);
                }
                else
                {
                    ans(me, 0U, NULL);
                }
                go = false;
                break;
        }
    }
}

/** @brief A job ran past its bound during the end: the end's line answered, the rest waits for the job. */
static void end_lost(TestDot *me)
{
    KLOG_W(TAG, "end: a job is still out; the end goes on when it answers");
    if ( !me->end_answered )
    {
        me->end_answered = true;
        ans(me, TD_ERR_TIMEOUT, "timeout");
    }
}

/** @brief Start the end; @p answered when the verb that led here is already answered. */
static void end_begin(TestDot *me, bool answered)
{
    me->act          = (uint8_t)ACT_END;
    me->mode         = (uint8_t)MODE_ENDING;
    me->end_step     = (uint8_t)END_START;
    me->end_wait     = (uint8_t)END_WAIT_NONE;
    me->end_answered = answered;
    if ( answered )
    {
        me->act = (uint8_t)ACT_NONE;
    }
    end_next(me);
}

/**
 * @brief The wake's job ran past its bound: the verb answered with a timeout,
 *        and what the wake took is given back by the end, once the job answers.
 */
static void wake_lost(TestDot *me)
{
    ans(me, TD_ERR_TIMEOUT, "timeout");
    end_begin(me, true);
}

/** @brief The job in flight ran past KODE_TEST_DOT_JOB_MS: its step fails, reported; its late answer is dropped. */
static void job_given_up(TestDot *me)
{
    me->job_lost = true;
    KLOG_W(TAG, "job %u: no answer in %u ms; given up", (unsigned)me->job.kind, (unsigned)KODE_TEST_DOT_JOB_MS);
    if ( (uint8_t)MODE_ENDING == me->mode )
    {
        end_lost(me);
    }
    else if ( (uint8_t)MODE_WAKING == me->mode )
    {
        wake_lost(me);
    }
    else if ( ((uint8_t)ACT_CAMERA == me->act) && me->cam_in_run )
    {
        if ( (uint8_t)RUN_GOING == me->run )
        {
            run_abort(me, "timeout", "camera", TD_ERR_TIMEOUT);
        }
        me->act = (uint8_t)ACT_NONE;
    }
    else if ( (uint8_t)ACT_NONE != me->act )
    {
        /* The loop's pins stay lent until the job answers; the end gives them back. */
        ans(me, TD_ERR_TIMEOUT, "timeout");
    }
    else
    {
        /* Nobody waits on it. */
    }
}

/* ---- The verbs ------------------------------------------------------------------------------- */

/** @brief `test_dot_info`. */
static void info_answer(TestDot *me)
{
    static const char *const MODE_WORD[] = { "dormant", "waking", "awake", "ending" };
    static const char *const RUN_WORD[]  = { "none", "running", "done", "aborted" };
    const char *j3 = "free";

    if ( me->probed )
    {
        j3 = "lent";
    }
    rec(me, "test_dot state=%s locked=%u run=%s psram_free=%lu j3=%s",
        td_word(MODE_WORD, sizeof(MODE_WORD) / sizeof(MODE_WORD[0]), me->mode), td_bit(me->locked_now),
        td_word(RUN_WORD, sizeof(RUN_WORD) / sizeof(RUN_WORD[0]), me->run), (unsigned long)khal_psram_free(), j3);
    ans(me, 0U, NULL);
}

/** @brief `test_dot_poll`: the ring, then where the run is. */
static void poll_answer(TestDot *me)
{
    const char *state = "done";
    const char *lane  = "idle";

    while ( 0U < me->ring_n )
    {
        rec_to(me, me->cur_who, me->cur_seq, me->ring[me->ring_head]);
        me->ring_head = (uint8_t)((me->ring_head + 1U) % KODE_TEST_DOT_POLL_RING);
        me->ring_n--;
    }
    if ( 0U < me->ring_dropped )
    {
        rec(me, "poll dropped=%lu", (unsigned long)me->ring_dropped);
        me->ring_dropped = 0U;
    }
    if ( (uint8_t)RUN_GOING == me->run )
    {
        state = "running";
    }
    if ( me->job_busy || me->lent )
    {
        lane = "busy";
    }
    rec(me, "poll state=%s lane=%s", state, lane);
    ans(me, 0U, NULL);
}

/** @brief A verb the AO can serve as it is now. */
static void verb_start(TestDot *me, TestDotReqEvt const *e)
{
    me->cur_who = e->who;
    me->cur_seq = e->seq;
    if ( verb_needs_cam(e->verb) && (NULL == me->cam) )
    {
        me->cam = td_cam_alloc();
        if ( NULL == me->cam )
        {
            ans(me, TD_ERR_BUSY, "no_memory");
            return;
        }
    }
    switch ( e->verb )
    {
        case (uint8_t)TEST_DOT_VERB_RUN:
            if ( (uint8_t)RES_NONE != me->res )
            {
                ans(me, TD_ERR_BUSY, "busy");
            }
            else
            {
                run_start(me);
            }
            break;
        case (uint8_t)TEST_DOT_VERB_PLACE:
            place_start(me, e);
            break;
        case (uint8_t)TEST_DOT_VERB_CAMERA:
            td_cam_reset(me->cam, true);
            cam_begin(me, false, e->a.cam);
            break;
        case (uint8_t)TEST_DOT_VERB_RELAYS:
            me->act          = (uint8_t)ACT_RELAYS;
            me->station_seen = true;
            if ( !job_submit(me, (uint8_t)TD_JOB_RELAYS, e->a.relays) )
            {
                ans(me, TD_ERR_BUSY, "busy");
            }
            break;
        case (uint8_t)TEST_DOT_VERB_ADC:
            me->act = (uint8_t)ACT_ADC;
            if ( !job_submit(me, (uint8_t)TD_JOB_ADC, e->a.ch) )
            {
                ans(me, TD_ERR_BUSY, "busy");
            }
            break;
        case (uint8_t)TEST_DOT_VERB_POLL:
            poll_answer(me);
            break;
        case (uint8_t)TEST_DOT_VERB_INFO:
            info_answer(me);
            break;
        case (uint8_t)TEST_DOT_VERB_LOCK:
            if ( khal_nvs_set_u8(KODE_STATION_NVS_NS, KODE_TEST_DOT_LOCK_KEY, KODE_TEST_DOT_LOCK_ON) == KODE_OK )
            {
                me->locked_now = true;
                KLOG_N(TAG, "lock: stored; the station verbs are refused from now on");
                ans(me, 0U, NULL);
            }
            else
            {
                ans(me, TD_ERR_BUSY, "nvs");
            }
            break;
        case (uint8_t)TEST_DOT_VERB_SCREEN:
            me->act       = (uint8_t)ACT_SCREEN;
            me->scr_phase = e->a.screen.phase;
            if ( (uint8_t)TEST_DOT_PHASE_RESULT == e->a.screen.phase )
            {
                uint8_t mark = TEST_DOT_ST_FAILED;
                if ( e->a.screen.pass )
                {
                    mark = TEST_DOT_ST_OK;
                }
                screen_wait(me, TEST_DOT_SCR_RESULT, &mark, 1U);
            }
            else
            {
                screen_wait(me, TEST_DOT_SCR_CONNECTING, NULL, 0U);
            }
            break;
        case (uint8_t)TEST_DOT_VERB_PHOTOS:
            photos_start(me, e);
            break;
        case (uint8_t)TEST_DOT_VERB_BLOB: {
            uint8_t i = (uint8_t)TD_BLOBS_MAX;
            const char *name = "location";
            if ( 0U != e->a.blob )
            {
                name = "colours";
            }
            if ( NULL != me->cam )
            {
                i = td_cam_blob_find(me->cam, name);
            }
            if ( (uint8_t)TD_BLOBS_MAX <= i )
            {
                ans(me, TD_ERR_BUSY, "no_blob");
            }
            else
            {
                lend_to(me, me->cur_who, me->cur_seq, me->cam->blob[i].name, me->cam->blob[i].data,
                        me->cam->blob[i].len);
                ans(me, 0U, NULL);
            }
            break;
        }
        case (uint8_t)TEST_DOT_VERB_LOOP:
            me->act        = (uint8_t)ACT_LOOP;
            me->loop_phase = (uint8_t)LOOP_LENDING;
            me->loop_count = e->a.count;
            if ( !worker_ensure(me) || !log_dock_lend_request(&me->super, true) )
            {
                ans(me, TD_ERR_BUSY, "busy");
            }
            else
            {
                /* Taken as lent until AO_Log says: an answer lost or late
                 * still leaves the end to ask for the pins back. */
                me->dock_lent = true;
                tmo_arm(me, (uint8_t)TMO_ANSWER, KODE_TEST_DOT_ANSWER_MS);
            }
            break;
        case (uint8_t)TEST_DOT_VERB_RADIO: {
            radio_probe_t probe = e->a.radio;
            probe.card  = true;
            me->act     = (uint8_t)ACT_RADIO;
            if ( radio_probe_request(&me->super, &probe) )
            {
                uint32_t bound = KODE_TEST_DOT_RADIO_MS;
                if ( probe.discover )
                {
                    bound = KODE_TEST_DOT_DISCOVER_MS;
                }
                tmo_arm(me, (uint8_t)TMO_RADIO, bound);
            }
            else
            {
                ans(me, TD_ERR_BUSY, "busy");
            }
            break;
        }
        case (uint8_t)TEST_DOT_VERB_BOOST:
            me->act = (uint8_t)ACT_BOOST;
            if ( !power_boost_test_request(&me->super, e->a.on) )
            {
                ans(me, TD_ERR_BUSY, "busy");
            }
            else
            {
                if ( e->a.on )
                {
                    /* Taken as on until AO_Power says: the end turns it off
                     * even when this answer is lost. */
                    me->boost_on = true;
                }
                tmo_arm(me, (uint8_t)TMO_ANSWER, KODE_TEST_DOT_ANSWER_MS);
            }
            break;
        case (uint8_t)TEST_DOT_VERB_RESULT:
            result_start(me, e);
            break;
        case (uint8_t)TEST_DOT_VERB_END:
            if ( (uint8_t)MODE_DORMANT == me->mode )
            {
                /* Nothing taken, but a screen may be up: `test_dot_screen` is
                 * served dormant (the PC shows "connecting" before the place),
                 * and the end gives the glass back whatever woke. With no
                 * test screen up the host takes it as nothing. */
                screen_ask(me, TEST_DOT_SCR_END, NULL, 0U);
                ans(me, 0U, NULL);
            }
            else
            {
                end_begin(me, false);
            }
            break;
        default:
            ans(me, TD_ERR_REJECT, "verb");
            break;
    }
}

/** @brief A station verb: the lock first, then whether anything is in the way, then the wake. */
static void on_req(TestDot *me, TestDotReqEvt const *e)
{
    bool const is_end = ((uint8_t)TEST_DOT_VERB_END == e->verb);
    bool const run_verb = ((uint8_t)TEST_DOT_VERB_RUN == e->verb) || ((uint8_t)TEST_DOT_VERB_PLACE == e->verb)
                          || ((uint8_t)TEST_DOT_VERB_CAMERA == e->verb);

    if ( me->locked || (me->locked_now && !is_end) )
    {
        ans_to(me, e->who, e->seq, TD_ERR_REJECT, "locked");
    }
    else if ( ((uint8_t)ACT_NONE != me->act) || ((uint8_t)MODE_ENDING == me->mode) )
    {
        ans_to(me, e->who, e->seq, TD_ERR_BUSY, "busy");
    }
    else if ( run_verb && ((uint8_t)RUN_GOING == me->run) )
    {
        ans_to(me, e->who, e->seq, TD_ERR_BUSY, "busy");
    }
    else if ( verb_wakes(e->verb) && ((uint8_t)MODE_DORMANT == me->mode) )
    {
        wake_begin(me, e);
    }
    else
    {
        verb_start(me, e);
    }
}

/** @brief The worker answered the job in flight. */
static void on_job(TestDot *me, KqpWorkerDoneEvt const *e)
{
    kode_status_t const rc = (kode_status_t)e->status;

    bool late;

    if ( !me->job_busy || (e->seq != me->job_seq) )
    {
        return;
    }
    me->job_busy = false;
    late         = me->job_lost;
    me->job_lost = false;
    if ( (uint8_t)TMO_JOB == me->tmo_for )
    {
        tmo_stop(me);
    }
    if ( late )
    {
        KLOG_W(TAG, "job %u answered after it was given up; its result is dropped", (unsigned)me->job.kind);
    }
    if ( (uint8_t)MODE_ENDING == me->mode )
    {
        if ( (uint8_t)TD_JOB_CLOSE == me->job.kind )
        {
            me->probed       = false;
            me->station_seen = false;
        }
        if ( (uint8_t)END_WAIT_JOB == me->end_wait )
        {
            me->end_wait = (uint8_t)END_WAIT_NONE;
            end_next(me);
        }
        return;
    }
    if ( late )
    {
        return;
    }
    switch ( me->job.kind )
    {
        case (uint8_t)TD_JOB_PROBE:
            if ( KODE_OK == rc )
            {
                me->probed       = true;
                me->station_seen = true;
                if ( !job_submit(me, (uint8_t)TD_JOB_STATION, 0U) )
                {
                    wake_fail(me, "station_i2c");
                }
            }
            else
            {
                wake_fail(me, "camera");
            }
            break;
        case (uint8_t)TD_JOB_STATION:
            if ( KODE_OK == rc )
            {
                wake_done(me);
            }
            else
            {
                wake_fail(me, "station_i2c");
            }
            break;
        case (uint8_t)TD_JOB_CLOSE:
            wake_closed(me);
            break;
        case (uint8_t)TD_JOB_RELAYS:
            rec(me, "relays state=0x%02X ok=%u", (unsigned)me->job.arg, td_bit(KODE_OK == rc));
            if ( KODE_OK == rc )
            {
                ans(me, 0U, NULL);
            }
            else
            {
                ans(me, TD_ERR_BUSY, "station_i2c");
            }
            break;
        case (uint8_t)TD_JOB_ADC:
            if ( KODE_OK == rc )
            {
                rec(me, "adc ch=%u mv=%ld", (unsigned)me->job.arg, (long)me->job.mv);
                ans(me, 0U, NULL);
            }
            else
            {
                ans(me, TD_ERR_BUSY, "station_i2c");
            }
            break;
        case (uint8_t)TD_JOB_LOOP:
            loop_return(me);
            break;
        case (uint8_t)TD_JOB_CAM:
            cam_after(me, rc);
            break;
        case (uint8_t)TD_JOB_PLACE:
            place_after(me, rc);
            break;
        case (uint8_t)TD_JOB_PHOTO_OUT:
            photo_after(me, rc);
            break;
        default:
            break;
    }
}

/** @brief A tmo tick: what it bounded ran out. */
static void on_tmo(TestDot *me)
{
    uint8_t const why = me->tmo_for;

    if ( timer_stale(&me->tmo_stale) )
    {
        return;
    }
    me->tmo_for = (uint8_t)TMO_NONE;
    if ( ((uint8_t)TMO_RAIL == why) && ((uint8_t)MODE_WAKING == me->mode) )
    {
        wake_fail(me, "camera");
    }
    else if ( ((uint8_t)TMO_SCREEN == why) && me->scr_waiting )
    {
        me->scr_waiting = false;
        screen_shown(me, false);
    }
    else if ( ((uint8_t)TMO_RADIO == why) && ((uint8_t)ACT_RADIO == me->act) )
    {
        rec(me, "radio ok=0 why=timeout");
        ans(me, TD_ERR_TIMEOUT, "timeout");
    }
    else if ( ((uint8_t)TMO_END == why) && ((uint8_t)MODE_ENDING == me->mode)
             && (((uint8_t)END_WAIT_BOOST == me->end_wait) || ((uint8_t)END_WAIT_DOCK == me->end_wait)) )
    {
        /* Its answer, if it comes now, is dropped: the end no longer waits for it. */
        KLOG_W(TAG, "end: an owner did not answer; going on");
        me->end_wait = (uint8_t)END_WAIT_NONE;
        end_next(me);
    }
    else if ( ((uint8_t)TMO_JOB == why) && me->job_busy )
    {
        job_given_up(me);
    }
    else if ( ((uint8_t)TMO_ANSWER == why) && ((uint8_t)ACT_BOOST == me->act) )
    {
        ans(me, TD_ERR_TIMEOUT, "timeout");
    }
    else if ( ((uint8_t)TMO_ANSWER == why) && ((uint8_t)ACT_LOOP == me->act) )
    {
        if ( (uint8_t)LOOP_RETURNING == me->loop_phase )
        {
            /* The frames are counted: reported, and the end asks for the pins again. */
            loop_report(me);
        }
        else
        {
            ans(me, TD_ERR_TIMEOUT, "timeout");
        }
    }
    else if ( ((uint8_t)TMO_RESET == why) && ((uint8_t)ACT_RESULT == me->act) && ((uint8_t)RES_PASSING == me->res) )
    {
        me->res      = (uint8_t)RES_FAILED;
        me->res_code = TD_ERR_TIMEOUT;
        td_copy(me->res_why, sizeof(me->res_why), "reset_timeout");
        result_show(me, TEST_DOT_ST_FAILED);
    }
    else
    {
        /* Nothing waits on it. */
    }
}

/** @brief A rest tick: what it paced goes on. */
static void on_rest(TestDot *me)
{
    uint8_t const why = me->rest_for;

    if ( timer_stale(&me->rest_stale) )
    {
        return;
    }
    me->rest_for = (uint8_t)REST_NONE;
    if ( ((uint8_t)REST_CAMERA == why) && ((uint8_t)ACT_CAMERA == me->act) )
    {
        cam_submit(me);
    }
    else if ( ((uint8_t)REST_PLACE == why) && ((uint8_t)ACT_PLACE == me->act) )
    {
        place_look(me);
    }
    else if ( ((uint8_t)REST_OPERATOR == why) && ((uint8_t)RUN_GOING == me->run) )
    {
        me->op_t0      = khal_time_ms();
        me->op_timeout = false;
        me->op_giveup  = false;
        if ( (uint8_t)OP_BUTTONS == me->op )
        {
            buttons_test_request_from(TEST_ORIGIN_TEST_DOT);
        }
        else if ( (uint8_t)OP_TOUCH == me->op )
        {
            touch_test_request_from(TEST_ORIGIN_TEST_DOT);
        }
        else
        {
            /* The operator part is over. */
        }
    }
    else
    {
        /* Nothing waits on it. */
    }
}

/** @brief The lent blob is back, or its bound ran out: the next out entry may go. */
static void on_back(TestDot *me)
{
    if ( !me->lent )
    {
        return;
    }
    me->lent = false;
    timer_stop(&me->drain, &me->drain_stale, &me->drain_for);
    if ( ((uint8_t)ACT_PHOTOS == me->act) && me->ph_wait_back )
    {
        me->ph_wait_back = false;
        me->ph_idx++;
        photo_next(me);
    }
    out_drain(me);
}

/* ---- The Active Object ---------------------------------------------------------------------------- */

void TestDot_ctor(void)
{
    TestDot * const me = &l_td;

    (void)memset(me, 0, sizeof(*me));
    QActive_ctor(&me->super, Q_STATE_CAST(&TestDot_initial));
    QTimeEvt_ctorX(&me->tmo, &me->super, TEST_DOT_TMO_SIG, 0U);
    QTimeEvt_ctorX(&me->rest, &me->super, TEST_DOT_REST_SIG, 0U);
    QTimeEvt_ctorX(&me->watch, &me->super, TEST_DOT_WATCH_SIG, 0U);
    QTimeEvt_ctorX(&me->drain, &me->super, TEST_DOT_DRAIN_SIG, 0U);
    s_run_going = false;
}

/** @brief Initial transition: read the lock, and nothing more - no claim, no allocation, no rail. */
static QState TestDot_initial(TestDot * const me, void const * const par)
{
    uint8_t v = 0U;

    (void)par;
    me->mode = (uint8_t)MODE_DORMANT;
    if ( (khal_nvs_get_u8(KODE_STATION_NVS_NS, KODE_TEST_DOT_LOCK_KEY, &v) == KODE_OK)
        && ((uint8_t)KODE_TEST_DOT_LOCK_ON == v) )
    {
        me->locked = true;
        KLOG_N(TAG, "locked: this Kode passed the E5 test; the station verbs are refused");
    }
    return Q_TRAN(&TestDot_active);
}

/** @brief The AO's one state: what it does now is in @c mode and @c act. */
static QState TestDot_active(TestDot * const me, QEvt const * const e)
{
    QState status = Q_HANDLED();

    switch ( e->sig )
    {
        case TEST_DOT_REQ_SIG:
            on_req(me, Q_EVT_CAST(TestDotReqEvt));
            break;
        case TEST_DOT_JOB_SIG:
            on_job(me, Q_EVT_CAST(KqpWorkerDoneEvt));
            break;
        case TEST_DOT_TMO_SIG:
            on_tmo(me);
            break;
        case TEST_DOT_REST_SIG:
            on_rest(me);
            break;
        case TEST_DOT_WATCH_SIG:
            if ( (uint8_t)RUN_GOING == me->run )
            {
                run_abort(me, "timeout", "watchdog", TD_ERR_TIMEOUT);
            }
            break;
        case TEST_DOT_DRAIN_SIG: {
            uint8_t const why = me->drain_for;
            if ( !timer_stale(&me->drain_stale) )
            {
                me->drain_for = (uint8_t)DRAIN_NONE;
                if ( ((uint8_t)DRAIN_LEND == why) && me->lent )
                {
                    KLOG_W(TAG, "a lent blob did not come back; going on without it");
                    on_back(me);
                }
                else
                {
                    out_drain(me);
                }
            }
            break;
        }
        case TEST_DOT_BACK_SIG:
            on_back(me);
            break;
        case TEST_DOT_SCREEN_DONE_SIG:
            if ( me->scr_waiting && (Q_EVT_CAST(TestDotScreenDoneEvt)->kind == me->scr_wait)
                && (Q_EVT_CAST(TestDotScreenDoneEvt)->seq == me->scr_wait_seq) )
            {
                me->scr_waiting = false;
                tmo_stop(me);
                screen_shown(me, true);
            }
            break;
        case EXP_POWER_SIG: {
            ExpPowerDoneEvt const * const p = Q_EVT_CAST(ExpPowerDoneEvt);
            if ( ((uint8_t)MODE_WAKING == me->mode) && ((uint8_t)TMO_RAIL == me->tmo_for) && p->on )
            {
                tmo_stop(me);
                if ( ((uint8_t)KODE_OK == p->rc) && job_submit(me, (uint8_t)TD_JOB_PROBE, 0U) )
                {
                    KLOG_I(TAG, "wake: J3's rail up; probing the camera");
                }
                else
                {
                    wake_fail(me, "camera");
                }
            }
            break;
        }
        case DISPLAY_STATE_SIG:
            if ( me->bright_wait )
            {
                me->bright       = Q_EVT_CAST(DisplayStateEvt)->bright;
                me->bright_known = true;
                me->bright_wait  = false;
            }
            break;
        case AUTOTEST_BEGIN_SIG:
            on_row_begin(me, Q_EVT_CAST(AutotestBeginEvt));
            break;
        case AUTOTEST_ROW_SIG:
            on_row(me, Q_EVT_CAST(AutotestRowEvt));
            break;
        case AUTOTEST_END_SIG:
            on_row_end(me, Q_EVT_CAST(AutotestEndEvt));
            break;
        case BTN_TEST_SIG:
            on_buttons(me, Q_EVT_CAST(ButtonsTestEvt));
            break;
        case TOUCH_TEST_SIG:
            on_touch(me, Q_EVT_CAST(TouchTestEvt));
            break;
        case TEST_RESULT_SIG:
            on_verdict(me, Q_EVT_CAST(TestResultEvt));
            break;
        case RADIO_PROBE_SIG:
            on_radio(me, Q_EVT_CAST(RadioProbeEvt));
            break;
        case POWER_BOOST_TEST_SIG:
            on_boost(me, Q_EVT_CAST(PowerBoostTestEvt));
            break;
        case LOG_DOCK_SIG:
            on_dock(me, Q_EVT_CAST(LogDockEvt));
            break;
        case FACTORY_RESET_RSP_SIG:
            on_reset(me, Q_EVT_CAST(FactoryResetRspEvt));
            break;
        default:
            status = Q_SUPER(&QHsm_top);
            break;
    }
    return status;
}

/* ---- The doors -------------------------------------------------------------------------------- */

bool test_dot_busy(void)
{
    return s_run_going;
}

void test_dot_screen_done(uint8_t kind, uint8_t seq)
{
    TestDotScreenDoneEvt *e = Q_NEW_X(TestDotScreenDoneEvt, KODE_TEST_DOT_POOL_MARGIN, TEST_DOT_SCREEN_DONE_SIG);

    if ( NULL == e )
    {
        KLOG_W(TAG, "out of events; the screen's answer is lost and the step waits it out");
        return;
    }
    e->kind = kind;
    e->seq  = seq;
    if ( !QACTIVE_POST_X(AO_TestDot, &e->super, KODE_TEST_DOT_REQ_MARGIN, NULL) )
    {
        KLOG_W(TAG, "queue full; the screen's answer is lost and the step waits it out");
    }
}

void test_dot_blob_back(void)
{
    static const QEvt back = QEVT_INITIALIZER(TEST_DOT_BACK_SIG);

    if ( !QACTIVE_POST_X(AO_TestDot, &back, 0U, NULL) )
    {
        KLOG_W(TAG, "queue full; the blob's return is lost and its bound ends the loan");
    }
}

AO_STORAGE(test_dot, KODE_TEST_DOT_QUEUE_LEN, KODE_TEST_DOT_STACK);

void kaos_test_dot_register(void)
{
    kaos_require(kverbs_add(&test_dot_verbs), "test_dot verbs");
}

void kaos_testdot_start(void)
{
    TestDot_ctor();
    kaos_test_dot_register();
    AO_START(AO_TestDot, test_dot, KODE_PRIO_TEST_DOT);
}
