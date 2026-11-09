/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kscreen.h
 * @brief LVGL on the glass, driven from one Active Object of your own.
 */
#ifndef KSCREEN_H
#define KSCREEN_H

#include <stdbool.h>
#include <stdint.h>

#include "sdkconfig.h"

#include "kode_sys.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One renderer: LVGL on the glass, from its owner's thread.
 *
 * Lives inside the owner's struct. The fields are the owner's to READ -
 * @c disp and @c indev for settings of its own, @c pressed, @c x and @c y for
 * gestures - and kscreen's alone to write.
 */
typedef struct {
    QTimeEvt      tick;     /**< The render pass: one-shot, re-armed by
                             *   kscreen_tick().                          */
    QActive      *owner;    /**< The AO this lives in; the sender of every
                             *   FlushEvt.                                */
    lv_display_t *disp;     /**< NULL until kscreen_open() succeeds.      */
    lv_indev_t   *indev;    /**< The pointer, in event mode.              */
    int16_t       x;        /**< The last touch sample's x.               */
    int16_t       y;        /**< The last touch sample's y.               */
    bool          pressed;  /**< Whether a finger is down, as of the last
                             *   sample.                                  */
    bool          direct;   /**< LVGL draws in the panel's framebuffers.
                             *   kscreen's own.                           */
    bool          posted;   /**< A flush is with AO_Display and not yet
                             *   waited for. kscreen's own.               */
    int32_t       band_y1;  /**< First and last row the frame being drawn
                             *   has changed so far, in direct mode;
                             *   empty when y1 > y2. kscreen's own.       */
    int32_t       band_y2;
    lv_display_rotation_t rot; /**< How the glass is turned; set only by
                             *   kscreen_set_rotation().                  */
    void         *part_buf[2]; /**< The two partial-mode draw buffers, PSRAM,
                             *   taken once and kept. kscreen's own.      */
    uint8_t      *rot_buf;  /**< Where a band is turned before it goes to
                             *   the panel, PSRAM, taken once and kept.
                             *   kscreen's own.                           */
} kscreen_t;

/**
 * @brief Bind the renderer to its owner. Call from the owner's constructor.
 *
 * One per image: LVGL has one tick and one display, and the owner runs at
 * KODE_PRIO_USER_GUI, a level with a single tenant.
 *
 * @param s        The renderer, inside the owner's struct.
 * @param owner    The Active Object that will drive it.
 * @param tick_sig The owner's private signal for the render pass.
 */
void kscreen_ctor(kscreen_t *s, QActive *owner, enum_t tick_sig);

/**
 * @brief Subscribe the owner to the panel and the touch feed, then ask for
 *        the panel's announcement.
 *
 * Subscribes to DISPLAY_STATE_SIG, DISPLAY_REPAINT_SIG and TOUCH_EVT_SIG, in
 * that order, before display_state_request(): the answer can come the instant
 * the request is posted. Call from the owner's initial transition, which runs
 * after kode_init() has returned - the condition the request needs.
 *
 * @param s The renderer.
 */
void kscreen_start(kscreen_t *s);

/**
 * @brief Bring LVGL up on the panel @p d announces, and start the render pass.
 *
 * lv_init(), a display of the glass's size in RGB565 drawn straight into the
 * panel's two framebuffers (or, on a panel with one, into two draw buffers in
 * PSRAM), the flush handed to AO_Display, the touch feed in event mode, then
 * kui_init() on that display. The owner builds its screens after this
 * returns KODE_OK.
 * From then on `lv_*` is called from the owner's thread only; other tasks reach it by event.
 *
 * @param s The renderer.
 * @param d The DISPLAY_STATE_SIG announcement.
 * @return KODE_OK when LVGL is up and the tick armed; KODE_DENIED when @p d
 *         says the panel is not up; KODE_EXISTS when it is already open;
 *         KODE_NO_RESOURCES when the draw buffers, the display or the input
 *         could not be had. Every failure is logged, leaves nothing
 *         allocated and does not arm the tick.
 */
kode_status_t kscreen_open(kscreen_t *s, DisplayStateEvt const *d);

/**
 * @brief The render pass, for the owner's tick signal.
 *
 * One extra input read while a finger rests, one lv_timer_handler(), then the
 * one-shot is re-armed for when LVGL's next timer is due, and never later
 * than KODE_GUI_TICK_MS. Does nothing before kscreen_open() has succeeded.
 * Every tick signal must reach this call: a state that takes it and skips this stops the screen.
 *
 * @param s The renderer.
 */
void kscreen_tick(kscreen_t *s);

/**
 * @brief Feed one TOUCH_EVT_SIG sample to LVGL.
 *
 * @param s The renderer.
 * @param t The sample.
 */
void kscreen_touch(kscreen_t *s, TouchEvt const *t);

/**
 * @brief Answer DISPLAY_REPAINT_SIG: invalidate the screen and the top layer.
 *
 * AO_Display completed every flush issued while it ran the panel row without
 * painting it, so the glass holds the row's last field while LVGL believes it
 * is up to date. The next pass paints everything back.
 *
 * @param s The renderer.
 */
void kscreen_repaint(kscreen_t *s);

/**
 * @brief Turn the glass: 0, 90, 180 or 270 degrees, now or at any later time.
 *
 * Unturned, LVGL draws straight into the panel's framebuffers. Turned, it
 * draws in bands into two PSRAM buffers, and each band is turned into a third
 * before it goes to AO_Display, a band at a time: a change of the whole
 * screen redraws at about 20 frames a second (59 unturned), a smaller one
 * faster. The three buffers are
 * taken on the first turn and kept. The screen is redrawn whole after each
 * change. Touch follows by itself: LVGL turns every point. The pad follows
 * too when CONFIG_KSCREEN_ROTATE_PAD is on (buttons_rotation_request()); TOP
 * and BOT never turn.
 *
 * Call from the owner's thread, outside kscreen_tick().
 * The only way to turn it: lv_display_set_rotation() on @c disp, or a flush callback of one's
 * own, draws a sheared or striped screen.
 *
 * @param s   The renderer.
 * @param rot The turn.
 * @return KODE_OK when the glass is turned that way (already, or now);
 *         KODE_DENIED before kscreen_open() has succeeded or for a value
 *         that is not one of the four; KODE_NO_RESOURCES when the buffers
 *         could not be had, the glass left as it was. Every failure is
 *         logged.
 */
kode_status_t kscreen_set_rotation(kscreen_t *s, lv_display_rotation_t rot);

#if CONFIG_KODE_TEST_DOT
/**
 * @brief Show what AO_TestDot asks for: an E5 screen or a pattern, over
 *        everything else on the glass.
 *
 * For the owner's TEST_DOT_SCREEN_REQ_SIG, to which kscreen_start()
 * subscribes it in a build with the E5 test. The canvas the last request
 * built is deleted first, then the new one is built on LVGL's top layer from
 * kui_test_dot.h, the frame is drawn and flushed at once, and
 * test_dot_screen_done() answers with the request's kind and number. An auto
 * screen over an auto screen only
 * changes the cells that changed. TEST_DOT_SCR_END deletes the canvas and
 * answers nothing. Before kscreen_open() has succeeded nothing is shown and
 * nothing answers: AO_TestDot's wait runs out.
 *
 * @param s The renderer.
 * @param e The request, a TestDotScreenEvt.
 */
void kscreen_test_dot_event(kscreen_t *s, QEvt const *e);
#endif

#ifdef __cplusplus
}
#endif

#endif /* KSCREEN_H */
