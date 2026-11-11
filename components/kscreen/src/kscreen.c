/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kscreen.c
 * @brief LVGL on the glass for one Active Object: display, flush, touch feed and render pass.
 */
#include "kscreen.h"

#include <stddef.h>
#include <string.h>

#include "sdkconfig.h"

#include "kaos.h"
#include "kbsp_board.h"
#include "kdrv_display.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kui.h"
#if CONFIG_KODE_TEST_DOT
#include "kui_test_dot.h"
#endif

#define TAG "kscreen"

#if LV_USE_OS != LV_OS_NONE
/* LVGL's FreeRTOS layer runs its draw threads at tskIDLE_PRIORITY plus this,
 * and the renderer's level is the one they belong at: above them, they would
 * stall everything the renderer yields to while a frame is drawn. */
_Static_assert((unsigned)LV_DRAW_THREAD_PRIO == KODE_PRIO_USER_GUI,
               "LVGL's draw threads must run at KODE_PRIO_USER_GUI");
#endif

/**
 * @brief Rows per draw buffer: a tenth of the glass. Only for a panel with a
 *        single framebuffer; with two, LVGL draws in those.
 *
 * Measured on the launcher's full redraw, with the buffers in PSRAM:
 *     2x40KB -> 54 ms        (internal RAM would be 37 ms)
 * Bigger buffers mean fewer flush cycles, which sounds like a win and is
 * not once they cost internal RAM: free internal sits between 80 and 98 KB,
 * the radio needs a few tens of bytes of it constantly and can use nothing
 * else, and a board left at zero internal free had a BLE stack that never
 * synchronised. So the buffers are PSRAM, always, and 17 ms a redraw is the
 * price of a radio.
 */
#define KSCREEN_BUF_LINES        (KBSP_LCD_V_RES / 10U)

/** @brief Bytes of the rows one draw buffer holds: RGB565, two bytes a pixel. */
#define KSCREEN_BUF_ROW_BYTES    ((size_t)KBSP_LCD_H_RES * KSCREEN_BUF_LINES * 2U)

/**
 * @brief Bytes per draw buffer, as allocated and as told to LVGL: the rows
 *        rounded up to whole cache lines, which the PPA demands of a buffer.
 *
 * The few bytes past the last row are less than one row, so LVGL still draws
 * KSCREEN_BUF_LINES rows into a partial buffer.
 */
#define KSCREEN_BUF_BYTES \
    ((KSCREEN_BUF_ROW_BYTES + (KDRV_DISPLAY_CACHE_LINE - 1U)) \
     & ~((size_t)KDRV_DISPLAY_CACHE_LINE - 1U))

_Static_assert(0U == (KSCREEN_BUF_BYTES % KDRV_DISPLAY_CACHE_LINE),
               "a draw buffer is whole cache lines");
_Static_assert((KSCREEN_BUF_BYTES - KSCREEN_BUF_ROW_BYTES) < ((size_t)KBSP_LCD_H_RES * 2U),
               "the rounding never adds a row to a draw buffer");

/** @brief Degrees in one quarter turn of the glass, for the log. */
#define KSCREEN_DEGREES_PER_TURN 90U

/** @brief Draw-buffer alignment, comfortably above LV_DRAW_BUF_ALIGN. */
#define KSCREEN_BUF_ALIGN        KDRV_DISPLAY_CACHE_LINE

/**
 * @brief How long LVGL may wait for the panel to finish one flush.
 *
 * Bounded on purpose: a completion that never comes would otherwise stop the
 * renderer for good, and the renderer sits near the bottom of the priority
 * map, so the whole interface would freeze with the board healthy underneath
 * and nothing logged.
 */
#define KSCREEN_FLUSH_TIMEOUT_MS 500U

/**
 * @brief LVGL's clock.
 *
 * Reading the system time rather than calling lv_tick_inc() from a timer means
 * a pass that overruns its period costs nothing: the next pass simply sees
 * more time gone, and animations do not drift.
 *
 * @return Milliseconds since boot.
 */
static uint32_t tick_cb(void)
{
    return khal_time_ms();
}

/**
 * @brief How LVGL waits for a flush, instead of how it would by default.
 *
 * Left to itself LVGL spins - `while(disp->flushing);` in lv_refr.c - which
 * burns a whole core doing nothing for the length of every DMA. A wait
 * callback turns that into a blocking take, so the renderer sleeps until the
 * panel is done and the CPU goes to whoever else wants it. LVGL serialises
 * flushes, so this pairs 1:1 with the interrupt. Returning from here is the
 * "done" signal, which is why nothing calls lv_display_flush_ready().
 * Not an event: LVGL waits inside its render call, so the owner is not reading its queue.
 *
 * @param disp The display; unused.
 */
static void flush_wait_cb(lv_display_t *disp)
{
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands the user data back as void * */
    kscreen_t * const s = (kscreen_t *)lv_display_get_user_data(disp);

    /* In direct mode only a frame's last area is posted; the others are
     * already in the framebuffer and there is nothing to wait for. */
    if ( !s->posted )
    {
        return;
    }
    s->posted = false;
    if ( !kdrv_display_flush_wait(KSCREEN_FLUSH_TIMEOUT_MS) )
    {
        /* Returning says "done" anyway, and LVGL may reuse the draw buffer
         * under a transfer that is somehow still live: a torn frame at worst.
         * The alternative is not a cleaner frame, it is this thread never
         * running again. */
        KLOG_E(TAG, "the panel never reported a flush done in %u ms; "
                    "dropping the frame", (unsigned)KSCREEN_FLUSH_TIMEOUT_MS);
    }
}

/**
 * @brief Hand one rendered rectangle to the panel's owner.
 *
 * @p px_map is not copied. It points into one of our draw buffers and stays
 * valid because LVGL does not render into that buffer again until
 * flush_wait_cb() has returned - with two buffers it draws into the other
 * one meanwhile.
 *
 * @param disp   The display; its user data is the renderer.
 * @param area   The rectangle, in LVGL's inclusive coordinates.
 * @param px_map The pixels, RGB565.
 */
static void flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map)
{
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands the user data back as void * */
    kscreen_t * const s      = (kscreen_t *)lv_display_get_user_data(disp);
    bool const        last   = lv_display_flush_is_last(disp);
    lv_area_t         rows   = *area;
    const lv_area_t  *a      = area;
    uint8_t          *pixels = px_map;

    if ( s->direct )
    {
        /* Every area of the frame is already where the panel reads it, so
         * nothing is handed on but the rows they span, once, with the last
         * one: whole rows, the framebuffer as the pixels. kdrv_display shows
         * that buffer at the next refresh and copies nothing; the rows only
         * tell it what changed, which is all it writes back and all the
         * mirror is told about. */
        if ( area->y1 < s->band_y1 )
        {
            s->band_y1 = area->y1;
        }
        if ( area->y2 > s->band_y2 )
        {
            s->band_y2 = area->y2;
        }
        if ( !last )
        {
            return;
        }
        rows.x1    = 0;
        rows.x2    = (int32_t)KBSP_LCD_H_RES - 1;
        rows.y1    = s->band_y1;
        rows.y2    = s->band_y2;
        a          = &rows;
        s->band_y1 = (int32_t)KBSP_LCD_V_RES;
        s->band_y2 = -1;
    }
    else if ( LV_DISPLAY_ROTATION_0 != s->rot )
    {
        /* LVGL drew the band as the turned glass reads it; the panel takes
         * it as wired. Turned into rot_buf, packed at the turned band's own
         * width - never the panel's, which would shear it - and handed on as
         * the rectangle it covers on the panel. One buffer is enough: LVGL
         * waits for this flush before it hands over the next band. */
        int32_t const w = lv_area_get_width(area);
        int32_t const h = lv_area_get_height(area);

        lv_display_rotate_area(disp, &rows);
        lv_draw_rotate(px_map, s->rot_buf, w, h,
                       (int32_t)lv_draw_buf_width_to_stride((uint32_t)w,
                                                            LV_COLOR_FORMAT_RGB565),
                       (int32_t)lv_draw_buf_width_to_stride(
                           (uint32_t)lv_area_get_width(&rows), LV_COLOR_FORMAT_RGB565),
                       s->rot, LV_COLOR_FORMAT_RGB565);
        a      = &rows;
        pixels = s->rot_buf;
    }
    else
    {
        /* unturned bands go as LVGL drew them */
    }

    FlushEvt * const ev = Q_NEW_X(FlushEvt, 0U, DISPLAY_FLUSH_SIG);

    s->posted = true;
    if ( NULL == ev )
    {
        /* No event to carry the chunk. The flush must still be completed, or
         * LVGL would wait for one nobody was asked to do; kdrv_display decides
         * every completion, so ask it to. */
        kdrv_display_flush_abort();
    }
    else
    {
        ev->x1     = a->x1;
        ev->y1     = a->y1;
        ev->x2     = a->x2 + 1;   /* esp_lcd's bounds are exclusive */
        ev->y2     = a->y2 + 1;
        ev->pixels = pixels;
        /* The last chunk is the one that makes the frame real: the panel's
         * driver assembles chunks in the framebuffer it is not scanning and
         * flips on this one, at a frame boundary, which is what keeps
         * scrolling from tearing. A producer that never sets it never gets a
         * frame on the glass. */
        ev->last   = last;
        QACTIVE_POST(AO_Display, &ev->super, s->owner);
    }
}

/**
 * @brief LVGL's input: report what the last TOUCH_EVT_SIG said.
 *
 * @param indev The pointer; its user data is the renderer.
 * @param data  Filled with the last sample.
 */
static void indev_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    /* cppcheck-suppress misra-c2012-11.5 ; LVGL hands the user data back as void * */
    kscreen_t const * const s = (kscreen_t const *)lv_indev_get_user_data(indev);

    data->point.x = s->x;
    data->point.y = s->y;
    if ( s->pressed )
    {
        data->state = LV_INDEV_STATE_PRESSED;
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

/**
 * @brief The pointer, against @p disp, fed by events rather than polled.
 *
 * LVGL's default is a 33 ms read timer, and AO_Touch samples every 30 ms: the
 * beat between the two makes LVGL read some samples twice (velocity zero) and
 * skip others (velocity doubled), so a drag's measured speed jumps between
 * 0x, 1x and 2x of reality - which is what an uncontrollable flick feels like.
 * In event mode each sample triggers one read: every sample counted, once.
 *
 * @param s    The renderer.
 * @param disp The display it reads into.
 * @return The input, or NULL when LVGL had no memory for it.
 */
static lv_indev_t *indev_open(kscreen_t *s, lv_display_t *disp)
{
    lv_indev_t * const indev = lv_indev_create();

    if ( NULL != indev )
    {
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_user_data(indev, s);
        lv_indev_set_read_cb(indev, &indev_cb);
        lv_indev_set_display(indev, disp);
        lv_indev_set_mode(indev, LV_INDEV_MODE_EVENT);
    }
    return indev;
}

/**
 * @brief Give LVGL the panel's two framebuffers, the one being assembled
 *        first.
 *
 * LVGL alternates between its two buffers and kdrv_display between its two
 * framebuffers, so starting them on the same one keeps them in step: LVGL
 * always draws in the buffer the panel is not showing. Anything else that
 * flips the panel - AO_Display's own test row - breaks that, which is why
 * kscreen_repaint() calls this again.
 *
 * The size told to LVGL is the framebuffers' real one, whole cache lines
 * (KDRV_DISPLAY_FB_BYTES): LVGL's PPA draw unit names it to the PPA as the
 * output's size, and the PPA refuses one that is not. LVGL draws the picture
 * and nothing past it.
 *
 * @param disp The renderer's display, in direct mode.
 */
static void buffers_align(lv_display_t *disp)
{
    lv_display_set_buffers(disp, kdrv_display_back_buffer(),
                           kdrv_display_front_buffer(),
                           (uint32_t)KDRV_DISPLAY_FB_BYTES,
                           LV_DISPLAY_RENDER_MODE_DIRECT);
}

/**
 * @brief Whether the panel offers two framebuffers to draw in directly.
 * @return true when both are there.
 */
static bool panel_direct(void)
{
    return (kdrv_display_back_buffer() != NULL)
        && (kdrv_display_front_buffer() != NULL);
}

/**
 * @brief Give the partial-mode draw buffers back; either may be NULL.
 * @param s The renderer.
 */
static void partial_buffers_put(kscreen_t *s)
{
    for ( uint8_t i = 0U; 2U > i; ++i )
    {
        khal_free(s->part_buf[i]);   /* takes NULL */
        s->part_buf[i] = NULL;
    }
}

/**
 * @brief Take the two partial-mode draw buffers, once; later calls find them.
 *
 * @param s The renderer.
 * @return KODE_OK when both are there; KODE_NO_RESOURCES, logged, when PSRAM
 *         could not give them, with neither kept.
 */
static kode_status_t partial_buffers_get(kscreen_t *s)
{
    kode_status_t rc = KODE_OK;

    for ( uint8_t i = 0U; 2U > i; ++i )
    {
        if ( NULL == s->part_buf[i] )
        {
            s->part_buf[i] = khal_alloc_psram(KSCREEN_BUF_ALIGN, KSCREEN_BUF_BYTES);
        }
    }
    if ( (NULL == s->part_buf[0]) || (NULL == s->part_buf[1]) )
    {
        KLOG_E(TAG, "no PSRAM for the draw buffers (%u bytes each)",
               (unsigned)KSCREEN_BUF_BYTES);
        partial_buffers_put(s);
        rc = KODE_NO_RESOURCES;
    }
    return rc;
}

void kscreen_ctor(kscreen_t *s, QActive *owner, enum_t tick_sig)
{
    QTimeEvt_ctorX(&s->tick, owner, tick_sig, 0U);
    s->owner   = owner;
    s->disp    = NULL;
    s->indev   = NULL;
    s->x       = 0;
    s->y       = 0;
    s->pressed = false;
    s->direct  = false;
    s->posted  = false;
    s->band_y1 = (int32_t)KBSP_LCD_V_RES;
    s->band_y2 = -1;
    s->rot     = LV_DISPLAY_ROTATION_0;
    s->part_buf[0] = NULL;
    s->part_buf[1] = NULL;
    s->rot_buf = NULL;
}

void kscreen_start(kscreen_t *s)
{
    QActive_subscribe(s->owner, DISPLAY_STATE_SIG);
    QActive_subscribe(s->owner, DISPLAY_REPAINT_SIG);
    touch_subscribe(s->owner);
#if CONFIG_KODE_TEST_DOT
    QActive_subscribe(s->owner, TEST_DOT_SCREEN_REQ_SIG);
#endif
    /* The panel was announced from inside kode_init(), before the owner
     * existed, and the bus keeps no copy. Ask for it again rather than wait
     * for one that already happened - which is what lets a renderer start
     * whenever it likes. */
    display_state_request();
}

kode_status_t kscreen_open(kscreen_t *s, DisplayStateEvt const *d)
{
    kode_status_t rc     = KODE_OK;
    bool          took   = false;   /* this call took the partial buffers */
    lv_display_t *disp   = NULL;
    lv_indev_t *  indev  = NULL;
    bool          direct = false;

    if ( NULL != s->disp )
    {
        rc = KODE_EXISTS;
    }
    else if ( !d->up )
    {
        /* Said out loud rather than waited out: the panel is not coming, and
         * a renderer that stayed silent here would look exactly like one that
         * started too early. */
        KLOG_E(TAG, "no panel; nothing to draw on");
        rc = KODE_DENIED;
    }
    else if ( panel_direct() )
    {
        direct = true;
    }
    else
    {
        rc   = partial_buffers_get(s);
        took = true;
    }

    if ( KODE_OK == rc )
    {
        /* Idempotent: a second open after a failed one finds LVGL already
         * initialised and leaves it as it is. */
        lv_init();
        lv_tick_set_cb(&tick_cb);
        disp = lv_display_create((int32_t)KBSP_LCD_H_RES, (int32_t)KBSP_LCD_V_RES);
        if ( NULL == disp )
        {
            KLOG_E(TAG, "lv_display_create failed");
            rc = KODE_NO_RESOURCES;
        }
    }

    if ( KODE_OK == rc )
    {
        lv_display_set_user_data(disp, s);
        /* The panel takes RGB565 and cannot take anything else at 410x502. */
        lv_display_set_color_format(disp, LV_COLOR_FORMAT_RGB565);
        /* Before the flush callbacks can run: they read it. */
        s->direct = direct;
        if ( direct )
        {
            buffers_align(disp);
        }
        else
        {
            lv_display_set_buffers(disp, s->part_buf[0], s->part_buf[1],
                                   (uint32_t)KSCREEN_BUF_BYTES,
                                   LV_DISPLAY_RENDER_MODE_PARTIAL);
        }
        lv_display_set_flush_cb(disp, &flush_cb);
        lv_display_set_flush_wait_cb(disp, &flush_wait_cb);
        indev = indev_open(s, disp);
        if ( NULL == indev )
        {
            KLOG_E(TAG, "lv_indev_create failed");
            lv_display_delete(disp);
            rc = KODE_NO_RESOURCES;
        }
    }

    if ( KODE_OK == rc )
    {
        s->disp  = disp;
        s->indev = indev;
        kui_init(disp);
        if ( direct )
        {
            KLOG_I(TAG, "LVGL up (%ux%u RGB565, drawing in the panel's "
                        "framebuffers)",
                   (unsigned)KBSP_LCD_H_RES, (unsigned)KBSP_LCD_V_RES);
        }
        else
        {
            KLOG_I(TAG, "LVGL up (%ux%u RGB565, 2x%uKB draw buffers)",
                   (unsigned)KBSP_LCD_H_RES, (unsigned)KBSP_LCD_V_RES,
                   (unsigned)(KSCREEN_BUF_BYTES / 1024U));
        }
        QTimeEvt_armX(&s->tick, KQP_TICKS_PER_MS(KODE_GUI_TICK_MS), 0U);
    }
    else if ( took )
    {
        /* Only what this call took: on KODE_EXISTS the buffers are LVGL's. */
        partial_buffers_put(s);
    }
    else
    {
        /* nothing of this call's to give back */
    }

    return rc;
}

void kscreen_tick(kscreen_t *s)
{
    if ( NULL != s->disp )
    {
        /* A resting finger publishes nothing - AO_Touch only speaks on change
         * - so feed LVGL the unchanged position each pass while pressed:
         * repeated same-coordinate reads are what decay its velocity to zero,
         * and what stop a drag-stop-lift from throwing at the speed the drag
         * used to have. */
        if ( s->pressed )
        {
            lv_indev_read(s->indev);
        }
        /* One pass: LVGL's timers, the refresh, and the flushes it triggers,
         * which is where flush_wait_cb() may block. */
        uint32_t wait = lv_timer_handler();

        /* The next pass goes when LVGL says its next timer is due, so a frame
         * starts on time rather than on a 10 ms grid - a 16 ms refresh that
         * waited for the grid ran every 20 ms. Never later than
         * KODE_GUI_TICK_MS, which is how soon a touch or a repaint is seen
         * while LVGL has nothing due, and one tick more than asked, because
         * a one-shot of N ticks can fire up to a tick early.
         *
         * One-shot, re-armed only now - NOT a periodic timer. A periodic
         * QTimeEvt posts on every expiry whether or not the owner got to run,
         * and a renderer sits near the bottom of the priority map: one slow
         * step elsewhere would bury it under a queue of stale ticks, and a
         * full queue is a QP assert, not a dropped event. Re-arming here keeps
         * at most one tick outstanding, so the screen degrades to a lower
         * frame rate instead. */
        if ( KODE_GUI_TICK_MS < wait )
        {
            wait = KODE_GUI_TICK_MS;
        }
        QTimeEvt_armX(&s->tick, KQP_TICKS_PER_MS(wait) + 1U, 0U);
    }
}

void kscreen_touch(kscreen_t *s, TouchEvt const *t)
{
    if ( t->stale && (NULL != s->indev) )
    {
        /* A contact that stopped counting lifts with no click: nothing was
         * tapped where it rested. */
        lv_indev_wait_release(s->indev);
    }
    s->x       = t->x;
    s->y       = t->y;
    s->pressed = t->pressed;
    /* One sample, one read - the whole point of event mode. */
    if ( NULL != s->indev )
    {
        lv_indev_read(s->indev);
    }
}

void kscreen_repaint(kscreen_t *s)
{
    if ( NULL != s->disp )
    {
        if ( s->direct )
        {
            /* The row flipped the panel on its own, so LVGL's idea of which
             * framebuffer is free may be the one on the glass. */
            buffers_align(s->disp);
        }
        lv_obj_invalidate(lv_screen_active());
        lv_obj_invalidate(lv_layer_top());
    }
}

/* cppcheck-suppress misra-c2012-8.7 ; the API an app turns its glass with; no app is in this build */
kode_status_t kscreen_set_rotation(kscreen_t *s, lv_display_rotation_t rot)
{
    kode_status_t rc = KODE_OK;

    if ( (NULL == s->disp) || (LV_DISPLAY_ROTATION_270 < rot) )
    {
        KLOG_E(TAG, "no turn of %d for a glass that is not open, or is not one "
                    "of the four", (int)rot);
        rc = KODE_DENIED;
    }
    else if ( rot == s->rot )
    {
        /* already so */
    }
    else
    {
        if ( LV_DISPLAY_ROTATION_0 == s->rot )
        {
            rc = partial_buffers_get(s);
            if ( (KODE_OK == rc) && (NULL == s->rot_buf) )
            {
                /* cppcheck-suppress misra-c2012-11.5 ; khal hands memory back as void * */
                s->rot_buf = (uint8_t *)khal_alloc_psram(KSCREEN_BUF_ALIGN,
                                                         KSCREEN_BUF_BYTES);
                if ( NULL == s->rot_buf )
                {
                    KLOG_E(TAG, "no PSRAM to turn the glass (%u bytes); left "
                                "as it is", (unsigned)KSCREEN_BUF_BYTES);
                    rc = KODE_NO_RESOURCES;
                }
            }
        }
        if ( KODE_OK == rc )
        {
            if ( LV_DISPLAY_ROTATION_0 == rot )
            {
                lv_display_set_rotation(s->disp, rot);
                if ( panel_direct() )
                {
                    /* Back into the framebuffers, LVGL in step with whichever
                     * one the panel is showing. */
                    buffers_align(s->disp);
                    s->direct = true;
                }
            }
            else
            {
                if ( LV_DISPLAY_ROTATION_0 == s->rot )
                {
                    /* LVGL cannot turn what it draws straight into the
                     * panel's framebuffers: bands it is, turned on the way. */
                    lv_display_set_buffers(s->disp, s->part_buf[0],
                                           s->part_buf[1],
                                           (uint32_t)KSCREEN_BUF_BYTES,
                                           LV_DISPLAY_RENDER_MODE_PARTIAL);
                    s->direct = false;
                }
                lv_display_set_rotation(s->disp, rot);
            }
            s->rot = rot;
            (void)lv_obj_invalidate(lv_screen_active());
            (void)lv_obj_invalidate(lv_layer_top());
            KLOG_I(TAG, "glass turned to %u degrees",
                   (unsigned)rot * KSCREEN_DEGREES_PER_TURN);
#if defined(CONFIG_KSCREEN_ROTATE_PAD)
            buttons_rotation_request((uint8_t)rot);
#endif
        }
    }
    return rc;
}

#if CONFIG_KODE_TEST_DOT
/* cppcheck-suppress misra-c2012-8.7 ; a call for an app's AO, never used inside the SDK: kodeOS draws the E5 screens through kgui */
void kscreen_test_dot_event(kscreen_t *s, QEvt const *e)
{
    /* The E5 test's canvas: one per image, like the renderer, so kept here. */
    static lv_obj_t *s_td_obj;                       /**< What the last request built, or NULL. */
    static uint8_t   s_td_kind;                      /**< Its kind.                             */
    static uint8_t   s_td_grid[KUI_TD_GRID_CELLS];   /**< An auto screen's cells as shown.      */
    TestDotScreenEvt const * const r = Q_EVT_CAST(TestDotScreenEvt);
    bool const auto_again = (NULL != s_td_obj) && (TEST_DOT_SCR_AUTO == r->kind)
                            && (TEST_DOT_SCR_AUTO == s_td_kind);

    if ( NULL == s->disp )
    {
        KLOG_W(TAG, "E5 screen 0x%02x asked before the glass is open", (unsigned)r->kind);
        return;
    }
    if ( auto_again )
    {
        for ( uint8_t i = 0U; ((uint8_t)KUI_TD_GRID_CELLS > i) && (i < r->len); i++ )
        {
            if ( s_td_grid[i] != r->arg[i] )
            {
                kui_td_grid_set(s_td_obj, i, r->arg[i]);
                s_td_grid[i] = r->arg[i];
            }
        }
    }
    else
    {
        /* The last canvas goes before the next is built: two whole-glass
         * canvases at once would double the PSRAM they take. */
        if ( NULL != s_td_obj )
        {
            lv_obj_delete(s_td_obj);
            s_td_obj = NULL;
        }
        if ( TEST_DOT_SCR_END == r->kind )
        {
            return;
        }
        s_td_obj = lv_obj_create(lv_layer_top());
        if ( NULL == s_td_obj )
        {
            KLOG_E(TAG, "no memory for E5 screen 0x%02x", (unsigned)r->kind);
            return;
        }
        lv_obj_remove_style_all(s_td_obj);
        lv_obj_set_size(s_td_obj, KUI_TD_W_PX, KUI_TD_H_PX);
        lv_obj_set_scrollable(s_td_obj, false);
        if ( 0U != (r->kind & TEST_DOT_PAT_FLAG) )
        {
            uint8_t const pattern = r->kind & (uint8_t)~TEST_DOT_PAT_FLAG;
            uint8_t arg = 0U;
            if ( 0U < r->len )
            {
                arg = r->arg[0];
            }
            kui_td_pattern_build(s_td_obj, (kui_td_pattern_t)pattern, arg);
        }
        else
        {
            kui_td_screen_build(s_td_obj, (kui_td_screen_t)r->kind, r->arg, r->len);
        }
        s_td_kind = r->kind;
        (void)memset(s_td_grid, (int)KUI_TD_ST_PENDING, sizeof(s_td_grid));
        if ( TEST_DOT_SCR_AUTO == r->kind )
        {
            for ( uint8_t i = 0U; ((uint8_t)KUI_TD_GRID_CELLS > i) && (i < r->len); i++ )
            {
                s_td_grid[i] = r->arg[i];
            }
        }
    }
    /* Drawn and flushed now, so the answer means the glass shows it. */
    lv_refr_now(s->disp);
    test_dot_screen_done(r->kind, r->seq);
}
#endif
