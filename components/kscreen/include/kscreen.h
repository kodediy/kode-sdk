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
    bool          parked;   /**< DISPLAY_PARK_SIG stopped the render pass
                             *   until DISPLAY_REPAINT_SIG. kscreen's own. */
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
 * Subscribes to DISPLAY_STATE_SIG, DISPLAY_REPAINT_SIG, DISPLAY_PARK_SIG and
 * TOUCH_EVT_SIG, in that order, before display_state_request(): the answer can
 * come the instant the request is posted. The owner hands each to its call:
 * kscreen_open(), kscreen_repaint(), kscreen_park() and kscreen_touch(). Call from the owner's initial transition, which runs
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
 * While kscreen_park() holds the render pass, a press reaches LVGL as a
 * contact to wait out: it clicks nothing, also when the finger stays down
 * past DISPLAY_REPAINT_SIG. The owner still hears TOUCH_EVT_SIG itself, so
 * the touch can be what wakes the glass.
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
 * is up to date. The next pass paints everything back. A renderer parked by
 * kscreen_park() takes the panel's framebuffers again, which the link brought
 * back new, and starts its render pass.
 *
 * @param s The renderer.
 */
void kscreen_repaint(kscreen_t *s);

/**
 * @brief Answer DISPLAY_PARK_SIG: stop drawing, and tell AO_Display so.
 *
 * AO_Display is about to put the glass to sleep (kode_sys_screen_sleep()),
 * and the link it takes down frees the framebuffers LVGL draws in. The render
 * pass stops here and draws nothing until DISPLAY_REPAINT_SIG; a touch
 * presses nothing meanwhile (kscreen_touch()). Then
 * kode_sys_screen_parked(@p seq) lets the glass go to sleep. Before
 * kscreen_open() has succeeded nothing answers: the sleep is refused, and the
 * glass stays lit.
 *
 * @param s   The renderer.
 * @param seq The DisplayParkEvt's @c seq.
 */
void kscreen_park(kscreen_t *s, uint32_t seq);

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

/* ---- Composing with the pixel accelerator -----------------------------------
 * Whole pictures moved by the P4's accelerator instead of drawn by LVGL: the
 * frame on the glass copied, a picture scaled, dimmed or laid over another,
 * and the result shown by an LVGL object. AO_Display owns the accelerator;
 * each call below asks it for one pass and returns when the pixels are there,
 * as the render pass waits for a flush. Call them from the owner's thread
 * only, after kscreen_open() has succeeded. The app's sdkconfig.defaults says
 * CONFIG_KODE_COMPOSE=y; without it every pass is refused. So is a pass asked
 * while the glass is asleep: the renderer is parked then, and composes again
 * once it is told to repaint.
 *
 * A call returns only when no pass of its own can still write a picture.
 * KODE_TIMEOUT says the pass never started and never will: the pictures may
 * be deleted. A pass that had started when the time ran out is waited for,
 * two seconds more at most, and its own answer returned. The one exception
 * is KODE_IO_ERROR, an accelerator that stopped answering in the middle of a
 * pass: the pictures of that call are not deleted.
 */

#define KSCREEN_SCALE_ONE  16U   /**< The same size: a scale is in sixteenths. */
#define KSCREEN_SCALE_MIN  1U    /**< The smallest scale, one sixteenth. */
#define KSCREEN_SCALE_MAX  256U  /**< The largest scale, sixteen times. */

/**
 * @brief A picture the accelerator reads and writes: RGB565 in PSRAM.
 *
 * @c buf is LVGL's own view of it, for the owner to use as any draw buffer -
 * an image's source, a canvas, a snapshot's target. Its rows follow one
 * another with no padding; a picture reshaped so that they no longer do is
 * refused by every call below.
 */
typedef struct {
    lv_draw_buf_t buf;  /**< The pixels and their size; kscreen's to set. */
} kscreen_pic_t;

/**
 * @brief Take a picture of @p w by @p h pixels, black.
 *
 * No side is longer than the glass's longer one, 502 pixels: a pass on the
 * largest picture is about one pass over a full frame.
 *
 * @param w Width in pixels, 1 to 502.
 * @param h Height in pixels, 1 to 502.
 * @return The picture, or NULL, logged, for a size out of range or when PSRAM
 *         could not give it.
 */
kscreen_pic_t *kscreen_pic_create(uint16_t w, uint16_t h);

/**
 * @brief Give a picture back. No object may still show it.
 * @param pic The picture; NULL does nothing.
 */
void kscreen_pic_delete(kscreen_pic_t *pic);

/**
 * @brief Copy the frame on the glass into @p dst, as the glass shows it.
 *
 * On a turned glass the copy is turned with it, in the same pass.
 *
 * @param s   The renderer, open.
 * @param dst A picture the size of the glass as it is turned: 410 by 502
 *            upright or at 180 degrees, 502 by 410 at 90 or 270.
 * @return KODE_OK; KODE_DENIED before kscreen_open() has succeeded, for a
 *         picture of another size, with the glass asleep or in a build
 *         without CONFIG_KODE_COMPOSE;
 *         KODE_NO_RESOURCES on a panel with a single framebuffer, with the
 *         link down, with no accelerator or with no room in AO_Display's
 *         queue; KODE_TIMEOUT when AO_Display did not take the request in
 *         500 ms; KODE_ERROR when the accelerator refused; KODE_IO_ERROR
 *         when a pass it started never ended. The same for every call below,
 *         each failure logged.
 */
kode_status_t kscreen_pic_grab(const kscreen_t *s, kscreen_pic_t *dst);

/**
 * @brief Paint an area of @p dst one colour.
 * @param dst    The picture.
 * @param area   The area, inside the picture; NULL is all of it.
 * @param colour The colour.
 * @return As kscreen_pic_grab(); KODE_DENIED for an area outside the picture.
 */
kode_status_t kscreen_pic_fill(kscreen_pic_t *dst, const lv_area_t *area, lv_color_t colour);

/**
 * @brief Copy an area of @p src into @p dst, scaled. At KSCREEN_SCALE_ONE it is a copy.
 *
 * The accelerator scales in sixteenths and rounds a size down: 410 pixels at
 * 15 are 384. Nothing is clipped: the scaled area must fit @p dst.
 *
 * @param dst        The picture written; never @p src.
 * @param x          Column of @p dst the area's left edge lands on.
 * @param y          Row of @p dst the area's top edge lands on.
 * @param src        The picture read.
 * @param from       The area of @p src; NULL is all of it.
 * @param sixteenths The scale, KSCREEN_SCALE_MIN to KSCREEN_SCALE_MAX.
 * @return As kscreen_pic_grab(); KODE_DENIED for a scale out of range or a
 *         result that is empty or does not fit.
 */
kode_status_t kscreen_pic_scale(kscreen_pic_t *dst, int32_t x, int32_t y, const kscreen_pic_t *src,
                                const lv_area_t *from, uint16_t sixteenths);

/**
 * @brief Lay an area of @p fg over @p dst, at an opacity.
 *
 * @param dst  The picture under it, changed in place; never @p fg.
 * @param x    Column of @p dst the area's left edge lands on.
 * @param y    Row of @p dst the area's top edge lands on.
 * @param fg   The picture laid over.
 * @param from The area of @p fg; NULL is all of it.
 * @param opa  LV_OPA_TRANSP leaves @p dst as it is, LV_OPA_COVER replaces it.
 * @return As kscreen_pic_grab(); KODE_DENIED for an area that does not fit.
 */
kode_status_t kscreen_pic_blend(kscreen_pic_t *dst, int32_t x, int32_t y, const kscreen_pic_t *fg,
                                const lv_area_t *from, lv_opa_t opa);

/**
 * @brief Write @p src into @p dst, dimmed towards black. Two passes.
 *
 * @param dst   The picture written; never @p src.
 * @param src   The picture read, the same size.
 * @param level LV_OPA_COVER is @p src as it is, LV_OPA_TRANSP is black.
 * @return As kscreen_pic_grab(); KODE_DENIED, with nothing painted, for
 *         pictures of two sizes or for one picture given twice.
 */
kode_status_t kscreen_pic_dim(kscreen_pic_t *dst, const kscreen_pic_t *src, lv_opa_t level);

/**
 * @brief An LVGL object that shows @p pic: LVGL copies its rows into the frame as it draws.
 *
 * An image object the size of the picture, placed and layered as any other.
 * After composing into @p pic, lv_obj_invalidate() it. Delete it before the
 * picture.
 *
 * @param parent The object it goes on.
 * @param pic    The picture shown.
 * @return The object, or NULL when LVGL had no memory for it.
 */
lv_obj_t *kscreen_frame_create(lv_obj_t *parent, const kscreen_pic_t *pic);

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
