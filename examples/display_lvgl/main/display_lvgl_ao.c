/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_lvgl_ao.c
 * @brief LVGL on the glass, from an Active Object of your own.
 */
#include "display_lvgl_ao.h"

#include "kscreen.h"

#define TAG "display_lvgl"

/** Where the lines sit, in pixels from the middle of the glass. Well inside
 *  the corners, and far enough apart not to touch at these sizes. */
#define DL_TIME_DY          (-170)
#define DL_DATE_DY          (-135)
#define DL_TITLE_DY         (-70)
#define DL_COUNT_DY         10
#define DL_TOUCH_DY         70
#define DL_BRIGHT_DY        95
#define DL_HINT_DY          135
#define DL_HINT2_DY         160

/** How often the time on the glass is read again, ms. */
#define DL_CLOCK_MS         1000U

/** How far one press of up or down moves the brightness, per cent. */
#define DL_BRIGHT_STEP      20U

/** Days in a week: kode_sys_time_t's weekday runs 0 (Sunday) to 6. */
#define DL_WEEK_DAYS        7U

/** @brief This AO's private signals. Posted to itself, never published. */
enum DisplayLvglSignals {
    DL_TICK_SIG = MAX_SIG,  /**< The render pass: kscreen's one-shot.  */
    DL_CLOCK_SIG            /**< The time on the glass: a one-shot too. */
};

/** @brief The AO's own state, private to this file. */
typedef struct {
    QActive    super;
    kscreen_t  screen;    /**< LVGL on the glass, driven from this AO.   */
    QTimeEvt   clock;     /**< Reads the local time once a second.        */
    lv_obj_t * count;     /**< The counter's label; LVGL owns the object. */
    lv_obj_t * time;      /**< The local time.                            */
    lv_obj_t * date;      /**< The local date, or why there is none.      */
    lv_obj_t * touch;     /**< The finger's point.                        */
    lv_obj_t * bright;    /**< The brightness this AO set.                */
    uint32_t   presses;   /**< Presses and taps since the panel came up.  */
    uint8_t    bright_pct; /**< The panel's level, per cent.              */
} DisplayLvgl;

/* The queue and the stack, written out as the reference example does. The
 * queue's depth follows kode_sys.h's one queue rule ("Queue depth"): no
 * request, the two one-shot timers, a button's press and release, and
 * TOUCH_EVT_SIG, which AO_Touch publishes at most every KODE_TOUCH_POLL_MS
 * (10 ms) while a finger moves. A render pass holds this AO while it waits
 * for the flush, about one frame (kscreen.h: about 59 a second for a
 * whole-screen change), so a sample or two waits behind it and
 * KQP_AO_QUEUE_LEN has room to spare. The stack is the other interesting
 * number: LVGL renders on it, and 8 KB is what the launcher's renderer runs
 * on, with a far heavier tree than this one. */
static QEvtPtr     l_queue[KQP_AO_QUEUE_LEN];
static StackType_t l_stack[KQP_AO_STACK_BYTES / sizeof(StackType_t)];

static DisplayLvgl l_ao;
QActive * const AO_DisplayLvgl = &l_ao.super;

static QState DisplayLvgl_initial(DisplayLvgl * const me, void const * const par);
static QState DisplayLvgl_running(DisplayLvgl * const me, QEvt const * const e);
static QState DisplayLvgl_waiting(DisplayLvgl * const me, QEvt const * const e);
static QState DisplayLvgl_drawing(DisplayLvgl * const me, QEvt const * const e);

/* ---- What is on the glass ------------------------------------------------ */
/* Only this AO's thread calls lv_*, which is why LVGL needs no lock. */

/**
 * @brief One centred line of text on the active screen.
 *
 * @param text   What it says.
 * @param font   Which of the fonts sdkconfig.defaults compiled in.
 * @param colour Its colour; the screen behind it is black.
 * @param dy     Pixels from the middle of the glass, positive is down.
 * @return The label, which LVGL owns.
 */
static lv_obj_t * line_new(const char * text, const lv_font_t * font,
                           lv_color_t colour, int32_t dy)
{
    lv_obj_t * const label = lv_label_create(lv_screen_active());

    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, font, 0);
    lv_obj_set_style_text_color(label, colour, 0);
    lv_obj_align(label, LV_ALIGN_CENTER, 0, dy);
    return label;
}

/**
 * @brief Add one to the counter and show it.
 *
 * @param me The AO.
 */
static void count_bump(DisplayLvgl * const me)
{
    me->presses++;
    lv_label_set_text_fmt(me->count, "%u", (unsigned)me->presses);
    KLOG_I(TAG, "count %u", (unsigned)me->presses);
}

/**
 * @brief Show the local time and date, or that the clock is not set.
 *
 * @param me The AO.
 */
static void clock_show(DisplayLvgl const * const me)
{
    /* The weekday's short name, indexed by kode_sys_time_t's weekday. */
    static const char * const DL_DAYS[DL_WEEK_DAYS] = {
        "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
    };
    kode_sys_time_t now;

    if ( (kode_sys_clock_local(&now) == KODE_OK) && (now.weekday < DL_WEEK_DAYS) )
    {
        lv_label_set_text_fmt(me->time, "%02u:%02u:%02u", (unsigned)now.hour,
                              (unsigned)now.minute, (unsigned)now.second);
        lv_label_set_text_fmt(me->date, "%s %04u-%02u-%02u", DL_DAYS[now.weekday],
                              (unsigned)now.year, (unsigned)now.month,
                              (unsigned)now.day);
    }
    else
    {
        lv_label_set_text(me->time, "--:--:--");
        lv_label_set_text(me->date, "time not set");
    }
}

/**
 * @brief Show where the finger is, from AO_Touch's report.
 *
 * @param me The AO.
 * @param te The report: the point in glass pixels, and whether it is down.
 */
static void touch_show(DisplayLvgl const * const me, TouchEvt const * const te)
{
    if ( te->pressed )
    {
        lv_label_set_text_fmt(me->touch, "finger at %d, %d", (int)te->x, (int)te->y);
    }
    else
    {
        lv_label_set_text_fmt(me->touch, "finger lifted at %d, %d", (int)te->x,
                              (int)te->y);
    }
}

/**
 * @brief Move the brightness one step up or down, and show it.
 *
 * kode_sys_screen_brightness() posts to AO_Display and returns; a refusal
 * (KODE_NO_RESOURCES: its queue is full) leaves the level where it was.
 *
 * @param me The AO.
 * @param up true for brighter.
 */
static void bright_step(DisplayLvgl * const me, bool up)
{
    uint8_t pct = me->bright_pct;

    if ( up )
    {
        if ( pct < (KODE_SYS_SCREEN_BRIGHT_MAX - DL_BRIGHT_STEP) )
        {
            pct += (uint8_t)DL_BRIGHT_STEP;
        }
        else
        {
            pct = (uint8_t)KODE_SYS_SCREEN_BRIGHT_MAX;
        }
    }
    else
    {
        if ( pct > (KODE_SYS_SCREEN_BRIGHT_MIN + DL_BRIGHT_STEP) )
        {
            pct -= (uint8_t)DL_BRIGHT_STEP;
        }
        else
        {
            pct = (uint8_t)KODE_SYS_SCREEN_BRIGHT_MIN;
        }
    }

    if ( kode_sys_screen_brightness(pct) == KODE_OK )
    {
        me->bright_pct = pct;
        lv_label_set_text_fmt(me->bright, "brightness %u%%", (unsigned)pct);
        KLOG_I(TAG, "brightness %u%%", (unsigned)pct);
    }
    else
    {
        KLOG_W(TAG, "AO_Display is busy; press again");
    }
}

/**
 * @brief A tap on the glass: LVGL's click, on this AO's thread.
 *
 * The labels are not clickable, so a tap anywhere lands on the screen itself.
 *
 * @param e The click; its user data is the AO.
 */
static void tap_cb(lv_event_t * e)
{
    count_bump((DisplayLvgl *)lv_event_get_user_data(e));
}

/**
 * @brief Build the screen: the time, a title, the counter, the finger, the
 *        brightness and two hints.
 *
 * Nothing here draws. LVGL marks what changed and the next pass renders it, so
 * this can run in an entry action and cost the step almost nothing.
 *
 * @param me The AO.
 */
static void ui_build(DisplayLvgl * const me)
{
    lv_obj_t * const scr = lv_screen_active();

    /* Black, opaque: LVGL's theme would otherwise paint the screen itself. */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_add_event_cb(scr, &tap_cb, LV_EVENT_CLICKED, me);

    me->time = line_new("--:--:--", &lv_font_montserrat_28, lv_color_white(),
                        DL_TIME_DY);
    me->date = line_new("", &lv_font_montserrat_14,
                        lv_palette_main(LV_PALETTE_GREY), DL_DATE_DY);
    (void)line_new("LVGL on the glass", &lv_font_montserrat_28,
                   lv_color_white(), DL_TITLE_DY);
    me->count = line_new("0", &lv_font_montserrat_48,
                         lv_palette_main(LV_PALETTE_LIGHT_BLUE), DL_COUNT_DY);
    me->touch = line_new("no finger yet", &lv_font_montserrat_14,
                         lv_color_white(), DL_TOUCH_DY);
    me->bright = line_new("", &lv_font_montserrat_14, lv_color_white(),
                          DL_BRIGHT_DY);
    lv_label_set_text_fmt(me->bright, "brightness %u%%", (unsigned)me->bright_pct);
    (void)line_new("TOP or tap: count    BOTTOM: reset", &lv_font_montserrat_14,
                   lv_palette_main(LV_PALETTE_GREY), DL_HINT_DY);
    (void)line_new("UP / DOWN: brightness", &lv_font_montserrat_14,
                   lv_palette_main(LV_PALETTE_GREY), DL_HINT2_DY);
}

/* ---- The AO -------------------------------------------------------------- */

void DisplayLvgl_ctor(void)
{
    DisplayLvgl * const me = &l_ao;

    QActive_ctor(&me->super, Q_STATE_CAST(&DisplayLvgl_initial));
    kscreen_ctor(&me->screen, &me->super, (enum_t)DL_TICK_SIG);
    QTimeEvt_ctorX(&me->clock, &me->super, (enum_t)DL_CLOCK_SIG, 0U);
    me->count      = NULL;
    me->time       = NULL;
    me->date       = NULL;
    me->touch      = NULL;
    me->bright     = NULL;
    me->presses    = 0U;
    me->bright_pct = (uint8_t)KODE_SYS_SCREEN_BRIGHT_MAX;
}

void DisplayLvgl_start(void)
{
    QActive_setAttr(AO_DisplayLvgl, TASK_NAME_ATTR, "display_lvgl");

    /* KODE_PRIO_USER_GUI, not KODE_PRIO_USER_LOW: it is the level the SDK keeps
     * for a renderer, just above AO_Net and below every owner. Rendering is the
     * heaviest work in the system and the least urgent, and here a slow frame
     * can only ever delay itself. */
    QACTIVE_START(AO_DisplayLvgl, KODE_PRIO_USER_GUI,
                  l_queue, Q_DIM(l_queue),
                  l_stack, sizeof(l_stack),
                  (void *)0);
}

/**
 * @brief The initial transition: subscribe, then ask.
 *
 * It runs in the thread that calls DisplayLvgl_start(), before the AO's own
 * task exists, and that is after kode_init() has returned - the condition
 * kscreen_start()'s question needs. kscreen_start() subscribes before it asks:
 * the answer can come the instant the question is posted, and an AO that asked
 * first would miss it.
 *
 * @param me  The AO.
 * @param par Unused.
 * @return The transition to DisplayLvgl_waiting.
 */
static QState DisplayLvgl_initial(DisplayLvgl * const me, void const * const par)
{
    (void)par;

    QActive_subscribe(&me->super, BUTTON_EVT_SIG);
    kscreen_start(&me->screen);

    KLOG_I(TAG, "up; asked AO_Display whether the panel is there");
    return Q_TRAN(&DisplayLvgl_waiting);
}

/**
 * @brief The parent of both states: what is true whether or not there is a
 *        picture. The bottom button resets the board.
 *
 * @param me The AO; unused, nothing here needs it.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState DisplayLvgl_running(DisplayLvgl * const me, QEvt const * const e)
{
    QState status;

    (void)me;
    switch ( e->sig )
    {
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            /* id is a button's number: KODE_SYS_BUTTON_x in kode_sys.h. */
            if ( be->pressed && (KODE_SYS_BUTTON_BOT == (uint32_t)be->id) )
            {
                /* Posts to AO_Boot, which resets the board a moment later:
                 * into kodeOS after a launcher start, into this app again
                 * after a mailbox install. */
                KLOG_I(TAG, "resetting");
                if ( KODE_OK != kode_sys_exit_to_os() )
                {
                    KLOG_W(TAG, "AO_Boot is busy; press again");
                }
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

/**
 * @brief Waiting for AO_Display's answer: LVGL is not started yet.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED(), a transition to DisplayLvgl_drawing, or Q_SUPER().
 */
static QState DisplayLvgl_waiting(DisplayLvgl * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case DISPLAY_STATE_SIG: {
            DisplayStateEvt const * const st = (DisplayStateEvt const *)e;

            if ( kscreen_open(&me->screen, st) == KODE_OK )
            {
                KLOG_I(TAG, "panel up at %u%%, LVGL running",
                       (unsigned)st->bright);
                /* The steps start from the level the panel is driven at:
                 * the user's own, from kodeOS's settings. */
                if ( st->bright >= KODE_SYS_SCREEN_BRIGHT_MIN )
                {
                    me->bright_pct = st->bright;
                }
                status = Q_TRAN(&DisplayLvgl_drawing);
            }
            else
            {
                /* kscreen has said why. Stay here: nothing will be drawn,
                 * and the bottom button still resets the board. */
                status = Q_HANDLED();
            }
            break;
        }
        default: {
            status = Q_SUPER(&DisplayLvgl_running);
            break;
        }
    }

    return status;
}

/**
 * @brief Drawing: the screen is built and the render pass ticks.
 *
 * @param me The AO.
 * @param e  The event.
 * @return Q_HANDLED() for what it handles, Q_SUPER() otherwise.
 */
static QState DisplayLvgl_drawing(DisplayLvgl * const me, QEvt const * const e)
{
    QState status;

    switch ( e->sig )
    {
        case Q_ENTRY_SIG: {
            ui_build(me);
            clock_show(me);
            QTimeEvt_armX(&me->clock, KQP_TICKS_PER_MS(DL_CLOCK_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        case DL_CLOCK_SIG: {
            clock_show(me);
            QTimeEvt_armX(&me->clock, KQP_TICKS_PER_MS(DL_CLOCK_MS), 0U);
            status = Q_HANDLED();
            break;
        }
        /* Always to kscreen_tick(), which re-arms the one-shot: a periodic tick
         * would fill the queue behind a slow frame, and a full queue asserts. */
        case DL_TICK_SIG: {
            kscreen_tick(&me->screen);
            status = Q_HANDLED();
            break;
        }
        case TOUCH_EVT_SIG: {
            kscreen_touch(&me->screen, (TouchEvt const *)e);
            touch_show(me, (TouchEvt const *)e);
            status = Q_HANDLED();
            break;
        }
        case DISPLAY_REPAINT_SIG: {
            kscreen_repaint(&me->screen);
            status = Q_HANDLED();
            break;
        }
        case BUTTON_EVT_SIG: {
            ButtonEvt const * const be = (ButtonEvt const *)e;

            if ( be->pressed && (KODE_SYS_BUTTON_TOP == (uint32_t)be->id) )
            {
                count_bump(me);
                status = Q_HANDLED();
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_UP == (uint32_t)be->id) )
            {
                bright_step(me, true);
                status = Q_HANDLED();
            }
            else if ( be->pressed && (KODE_SYS_BUTTON_DOWN == (uint32_t)be->id) )
            {
                bright_step(me, false);
                status = Q_HANDLED();
            }
            else
            {
                /* The bottom button is the parent's. */
                status = Q_SUPER(&DisplayLvgl_running);
            }
            break;
        }
        default: {
            status = Q_SUPER(&DisplayLvgl_running);
            break;
        }
    }

    return status;
}
