/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_display.h
 * @brief The AMOLED panel as a device: bring-up, pixels, brightness.
 */
#ifndef KDRV_DISPLAY_H_
#define KDRV_DISPLAY_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbsp_board.h"
#include "kode_defs.h"
#include "khal_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* cppcheck-suppress misra-c2012-2.5 ; read by kdrv_display.c, which cppcheck cannot parse */
#define KDRV_DISPLAY_FRAME_US 16829U /**< One refresh of the panel, in us,
                                      *   from the DPI timing in
                                      *   kdrv_display.c.                  */

#define KDRV_DISPLAY_CACHE_LINE 64U /**< The PSRAM cache line of the board's
                                     *   cache setup (kdrv_display.c asserts
                                     *   it matches the build's).          */

#define KDRV_DISPLAY_BYTES_PER_PIXEL 2U /**< RGB565. */

/**
 * The size of each framebuffer in bytes: the one ESP-IDF's panel driver
 * allocates for it, the picture rounded up to whole cache lines. It is what
 * kdrv_display_back_buffer() and kdrv_display_front_buffer() point into, so a
 * consumer that tells another driver how big the buffer is - a PPA output -
 * says this, not the picture's own 410 x 502 x 2.
 */
#define KDRV_DISPLAY_FB_BYTES \
    ((((size_t)KBSP_LCD_H_RES * KBSP_LCD_V_RES * KDRV_DISPLAY_BYTES_PER_PIXEL) \
      + (KDRV_DISPLAY_CACHE_LINE - 1U)) & ~((size_t)KDRV_DISPLAY_CACHE_LINE - 1U))

_Static_assert(0U == (KDRV_DISPLAY_FB_BYTES % KDRV_DISPLAY_CACHE_LINE),
               "a framebuffer is whole cache lines");
_Static_assert(KDRV_DISPLAY_FB_BYTES >=
                   ((size_t)KBSP_LCD_H_RES * KBSP_LCD_V_RES * KDRV_DISPLAY_BYTES_PER_PIXEL),
               "a framebuffer holds the picture");
_Static_assert(KDRV_DISPLAY_FB_BYTES <
                   (((size_t)KBSP_LCD_H_RES * KBSP_LCD_V_RES * KDRV_DISPLAY_BYTES_PER_PIXEL)
                    + KDRV_DISPLAY_CACHE_LINE),
               "a framebuffer is the picture plus less than one cache line");

/**
 * @brief Leave the frame on the glass through the restart that comes next.
 *
 * The video stops in a vertical blanking, so the panel's memory holds a whole
 * frame; the reset line is latched high with the pad hold, which outlasts a
 * software reset and the bootloader's pulse; and a note in an LP store register
 * tells the next boot that the panel is awake, lit and showing it. No flush or
 * brightness reaches the panel after this. AO_Display's thread.
 *
 * @return KODE_OK; KODE_DENIED when the panel is not lit on a frame of ours;
 *         KODE_ERROR or KODE_TIMEOUT when it could not be arranged, and the
 *         restart resets the panel as it always did.
 */
kode_status_t kdrv_display_keep_for_restart(void);

/** Whether the scan-out should be running now: up, link up, first frame out. */
bool kdrv_display_video_expected(void);

/**
 * @brief Restarts asked in a row because the glass was lost.
 *
 * Kept in an LP store register, so it survives the software restart it counts,
 * and a power-on starts it at zero.
 */
uint32_t kdrv_display_recoveries(void);

/** @brief Set what kdrv_display_recoveries() answers. */
void kdrv_display_recoveries_set(uint32_t n);


/**
 * @brief Wait for the flush in flight to complete.
 *
 * The completion is signalled from the DMA ISR on a semaphore rather than
 * through a callback, because that ISR runs with the flash cache disabled and
 * everything it touches must be in IRAM - a caller cannot register a
 * semaphore in the wrong place the way it could register a function.
 *
 * The completion is this file's alone to decide - the DMA's interrupt, a
 * chunk that needed no DMA, an empty frame, a flush that never started - and
 * that is why the semaphore behind this call is private here and no longer
 * travels to the renderer as a handle. It had three holders once, any of
 * which could signal it, and nothing could check that exactly one of them
 * did: a completion signalled twice is silently swallowed by a binary
 * semaphore, and the wait after it hangs forever.
 *
 * **Bounded on purpose.** A completion that never arrives used to stop the
 * renderer for good, and the renderer sits near the bottom of the priority
 * map, so the whole interface froze with the board still healthy
 * underneath and nothing on the console to say why. Returning false is
 * worse than a clean frame and far better than that: the caller loses a
 * frame, says so, and carries on.
 *
 * @param ms How long to wait before giving up.
 * @return true when the flush completed, false when the time ran out.
 */
bool kdrv_display_flush_wait(uint32_t ms);

/**
 * @brief Complete the flush in flight without a transfer.
 *
 * For the callers that discover there is nothing to send after the renderer
 * is already committed to waiting: no event to carry the chunk, or a flush
 * the panel refused. Exactly one completion per flush is the contract, and
 * routing these through here rather than through the semaphore is what keeps
 * that countable in one place.
 */
void kdrv_display_flush_abort(void);

/**
 * @brief Bring the panel up: DPHY rail, panel power, MIPI-DSI, CO5300 - on,
 *        and black until the first frame.
 *
 * Long and blocking (~100 ms of mandated settling delays). Called once, from
 * AO_Display's panel_up(), on Display_powering's second answer.
 *
 * The panel comes up at brightness 0 and stays there until the first frame
 * reaches the glass through kdrv_display_flush(), which lights it at the
 * brightness asked for meanwhile (see kdrv_display_brightness()). Until then
 * a warm panel would show whatever its own RAM kept across the reset; a
 * firmware that never flushes leaves the glass black.
 *
 * A panel that comes up also gets, once, a shutdown handler that drives its
 * reset line low and leaves it low, so esp_restart() takes the panel and the
 * touch controller (same line) down instead of leaving the last frame on the
 * glass into the next boot. It covers esp_restart() alone: a panic, a
 * watchdog, a brownout and every reset that is not a software restart never
 * run shutdown handlers, and the bootloader's pulse on the same line at
 * every boot covers those. A board with no glass registers nothing.
 *
 * @return KODE_DENIED if no completion semaphore has been handed over yet.
 *         Refusing is deliberate: a panel with no way to report a finished
 *         transfer draws exactly one band and then hangs the renderer, which
 *         is a much harder thing to diagnose than a message at boot.
 */
kode_status_t kdrv_display_init(void);

/**
 * @brief Push one rectangle of RGB565 pixels. Returns as soon as the DMA is
 *        queued; completion arrives on the semaphore.
 *
 * When @p pixels is one of the panel's framebuffers - what
 * kdrv_display_back_buffer() or kdrv_display_front_buffer() returned - the
 * frame was drawn in place: nothing is copied and the panel flips to that
 * buffer, the other one becoming the back. The whole frame must already be in
 * that buffer; the rectangle is whole rows (@p x1 0, @p x2 the width), the
 * ones the caller changed, sent alone with @p last set - the whole panel when
 * it changed everything.
 *
 * The first frame to reach the glass - a flip, a frame drawn in place, or the
 * last chunk on a single framebuffer - also lights the panel, once, at the
 * brightness kept until then (see kdrv_display_init()).
 *
 * @note The x2/y2 bounds are exclusive, matching esp_lcd.
 */
kode_status_t kdrv_display_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                 const void *pixels, bool last);

/**
 * @brief The framebuffer the next frame is assembled in, for a caller that
 *        draws whole frames itself.
 *
 * A renderer that produces the whole panel at once - a PPA scaling a game's
 * frame, a video decoder - writes straight into this buffer and then flushes
 * the whole panel with this pointer as its pixels, and the frame reaches the
 * glass with no copy at all. The buffer changes at every flip, so ask for it
 * each frame, after kdrv_display_flush_wait() says the last one is done; until
 * then the engine may still be reading it.
 *
 * The PPA refuses an output whose address or size is not a multiple of the
 * cache line, and a 410 x 502 RGB565 frame is 411640 bytes, 56 past one. The
 * buffer itself is KDRV_DISPLAY_FB_BYTES long, whole lines, so that is the
 * size to name as the output's. A caller that names the picture's own size
 * instead gets its tallest output that fits, 410 x 496 (a row is 820 bytes,
 * so only a multiple of 16 rows lands on a line), and fills the last six rows
 * itself.
 *
 * @return The back buffer, 410 x 502 RGB565 with a stride of 820 bytes in a
 *         KDRV_DISPLAY_FB_BYTES allocation, or NULL when the panel is not up
 *         or runs on a single framebuffer.
 */
void *kdrv_display_back_buffer(void);

/**
 * @brief The framebuffer the panel shows now, for a renderer that keeps both.
 *
 * LVGL in direct mode draws in two screen-sized buffers and alternates
 * between them; handing it this one and kdrv_display_back_buffer() lets it
 * draw every frame in the buffer the panel is not reading, and a frame then
 * reaches the glass through kdrv_display_flush() with no copy. Never write it
 * while it is on the glass: the engine reads it at every refresh.
 *
 * @return The front buffer, laid out as kdrv_display_back_buffer()'s, or NULL
 *         when the panel is not up or runs on a single framebuffer.
 */
void *kdrv_display_front_buffer(void);

/**
 * @brief Set panel brightness, 0..100. On this AMOLED 0 is black even when on.
 *
 * One DCS command through the DSI host's command FIFO, which drains a
 * command per frame. So writes are paced - one that follows the last by less
 * than two frames waits out the gap, so a ramp can call this in a loop - and
 * a FIFO that stays full is reported, never waited on for ever.
 *
 * Before the first frame has reached the glass nothing is written: the value
 * is kept, the last one asked for wins, and the first frame lights the panel
 * at it - at 100 when nothing was asked (see kdrv_display_init()).
 *
 * @return KODE_OK, also for a value kept until the first frame; KODE_DENIED
 *         before init; KODE_TIMEOUT when the command FIFO did not drain, and
 *         nothing was written; KODE_ERROR when the panel refused the write.
 */
kode_status_t kdrv_display_brightness(uint8_t percent);

/* ---- The link and the panel's memory --------------------------------------
 * The CO5300 keeps the picture in a memory of its own and shows it without
 * anyone driving it. So the DSI link can go - the video scan-out with its two
 * framebuffers, the bus whose PM lock keeps the P4 out of light sleep, the
 * DPHY rail - while the glass stays lit, and a small change (the minute of a
 * clock) is written into that memory alone. None of the three calls below
 * resets the panel or sends its init table. AO_Display's thread only, and
 * only while the renderer is parked: nothing may draw or flush between a link
 * going down and coming back. See kdrv_display_link_set() for how long.
 */

/**
 * @brief Take the DSI link down (false) or bring it back (true); the panel
 *        keeps its memory and its state through both.
 *
 * Down stops the video scan-out, frees the framebuffers, deletes the control
 * IO and the bus (and with it the lock that blocks light sleep) and powers
 * the DPHY rail off. The panel is not touched: whatever its memory holds stays
 * on the glass at the brightness it was left at, so a caller that wants the
 * glass dark darkens it first (kdrv_display_brightness(), or
 * kdrv_display_panel_sleep()). Calls that need the link answer KODE_DENIED
 * meanwhile, and kdrv_display_back_buffer() and kdrv_display_front_buffer()
 * answer NULL.
 *
 * Up raises the rail, the bus and the control IO again and restarts the
 * scan-out with no panel reset and no init sequence: the panel is told a
 * brightness of 0 and nothing else. The scan-out replaces what the panel's
 * memory held with black from the first frame it sends. The link then behaves
 * as after kdrv_display_init(): dark until the first frame reaches the glass,
 * which lights the panel at the last level kdrv_display_brightness() was
 * given. A panel put to sleep before the link went down is still asleep;
 * kdrv_display_panel_sleep(false) wakes it.
 *
 * **The framebuffers are new.** Down frees both, and up allocates a pair that
 * is black and not necessarily at the addresses the old one had; nothing in
 * this driver stops a renderer that kept the old pointers from drawing into
 * freed memory (LVGL in direct mode renders into its buffers before any
 * flush, so a flush that is refused protects nothing). So the renderer must
 * be parked from before the call with @p up false until after the call with
 * @p up true **and** its buffers have been fetched again from
 * kdrv_display_back_buffer() and kdrv_display_front_buffer() - on the
 * consumers' side that is the repaint path (DISPLAY_REPAINT_SIG, which ends in
 * kscreen_repaint() and its buffer re-alignment) - and only then painted whole
 * and flushed. kdrv_display_buffers_epoch() lets a consumer check, from
 * whatever thread it runs on, that the pointers it holds are the current ones.
 * Both directions log the two addresses at NOTICE.
 *
 * Down waits up to three frames for a flip already handed to the engine.
 * Either direction is a no-op, KODE_OK, when the link is already there.
 *
 * @param up true to bring the link up, false to take it down.
 * @return KODE_OK; KODE_DENIED before kdrv_display_init(), or on a down with
 *         a frame open (chunks assembled and not yet flipped); KODE_TIMEOUT
 *         when a flip never completed (nothing was taken down); KODE_ERROR
 *         when IDF refused a step (a failed up leaves the link down and
 *         nothing standing; a failed down leaves it down).
 */
kode_status_t kdrv_display_link_set(bool up);

/**
 * @brief A number that changes every time the framebuffers are replaced.
 *
 * Incremented by every successful kdrv_display_link_set(true), which is the
 * only thing that replaces the pair after the bring-up. A consumer that holds
 * the pointers from kdrv_display_back_buffer() and kdrv_display_front_buffer()
 * keeps the epoch it read them under; when this returns another value its
 * pointers are stale and must not be drawn into or handed to a flush until it
 * has fetched them again. Zero until the first link comes back. A plain read
 * of one word, safe from any thread.
 *
 * @return The current epoch.
 */
uint32_t kdrv_display_buffers_epoch(void);

/**
 * @brief Put the panel to sleep (true) or wake it (false): brightness 0 and
 *        display off, then sleep-in; or sleep-out, then display on.
 *
 * Memory keeps its content in sleep-in, but the glass is dark. Both directions
 * wait 120 ms for the panel to settle, so the call blocks about 160 ms to
 * sleep and 120 ms to wake. Going to sleep writes a brightness of 0 first; a
 * wake writes none, so the glass is lit again only when the caller gives a
 * level with kdrv_display_brightness() - which takes effect at once, unless
 * the link has just come back and no frame has reached the glass yet, in which
 * case it is kept and lights that first frame (see kdrv_display_link_set()).
 *
 * Needs the link up (the commands ride on it): a caller that wants the link
 * down and the panel asleep sleeps the panel first. A panel put to sleep stays
 * asleep across kdrv_display_link_set(false) and (true).
 *
 * @param sleep true to sleep, false to wake.
 * @return KODE_OK, also when the panel is already in that state; KODE_DENIED
 *         with the link down; KODE_TIMEOUT when the command FIFO did not
 *         drain; KODE_ERROR when a command was refused. After a failure the
 *         state is unchanged for this driver and the call may be repeated.
 */
kode_status_t kdrv_display_panel_sleep(bool sleep);

/**
 * @brief Drive the reset line the panel shares with the touch controller low,
 *        and wait out the least time the panel wants it low.
 *
 * The first step of the panel's power-off: after the sleep-in and with the
 * link down, the line goes low, stays there for at least 1 ms, and only then
 * may the panel's supply go. Both parts are in reset from here; the line is
 * left low. Nothing brings it back but the next boot's bring-up, so it is for
 * the board going OFF only. Blocks for the wait, as the sleep-in does.
 * AO_Display's thread only.
 *
 * @return KODE_OK with the line low; KODE_DENIED before kdrv_display_init(),
 *         or with the link up (the video panel owns the line then, and the
 *         line is left as it is); the pin's own refusal when it would not be
 *         driven, with no wait.
 */
kode_status_t kdrv_display_reset_low(void);

/**
 * @brief Write a rectangle of RGB565 straight into the panel's memory, with
 *        the link down.
 *
 * Builds a command-only link for the write - the DPHY rail, the bus and the
 * control IO, no video, no framebuffer, no panel object, so no reset and no
 * init - sends CASET, RASET and RAMWR, waits for the packets to leave and
 * takes the link down again. Meant for the glass that shows its memory alone:
 * a clock's minute, written while the P4 sleeps between writes. It is the
 * link-down counterpart of kdrv_display_flush(); with the link up (video
 * scanning) the framebuffer is the picture, so the call is refused.
 *
 * The CO5300 takes a window that starts on an even column and an even row and
 * is an even number of columns and rows wide: @p x, @p y, @p w and @p h are
 * all even, and anything else is refused rather than rounded. A pixel is sent
 * low byte first, the order LVGL keeps RGB565 in.
 *
 * After the pixels the panel's window is set back to the whole glass, because
 * the video scan-out streams a full frame through it: a window left on the
 * rectangle would fold the next frames into it.
 *
 * About 8 000 pixels take about 10 ms, in low-power mode.
 *
 * @param x      First column, 0..KBSP_LCD_H_RES - 2, even.
 * @param y      First row, 0..KBSP_LCD_V_RES - 2, even.
 * @param w      Columns, even, at least 2, with @p x + @p w within the glass.
 * @param h      Rows, even, at least 2, with @p y + @p h within the glass.
 * @param rgb565 @p w * @p h pixels, row after row with no padding.
 * @return KODE_OK once the packets left the host; KODE_DENIED for a NULL
 *         pointer, a rectangle off the glass, empty or not even (kdefs has
 *         no invalid-argument status; this is its closest), before
 *         kdrv_display_init(), or with the link up; KODE_TIMEOUT when a FIFO of
 *         the host did not drain; KODE_ERROR when the link could not be built.
 */
kode_status_t kdrv_display_gram_write(uint16_t x, uint16_t y, uint16_t w,
                                      uint16_t h, const uint16_t *rgb565);

/* ---- Capture: the frame on the glass, as a file --------------------------
 * With KODEOS_SCREENSHOT on, a capture copies the front framebuffer - the
 * frame the panel shows - into a buffer in PSRAM, once, when it is asked
 * for. The framebuffer is what the panel scans, so the copy is bit-exact -
 * and it is as faithful to a firmware that draws by hand as to one that
 * draws through LVGL, which LVGL's own snapshot cannot be.
 *
 * The buffer carries its BMP header in front of the pixels and stores rows
 * bottom-up, which is the order BMP wants. The consequence is the point:
 * there is no assembly step anywhere, because the allocation *is* the file.
 */

/**
 * @brief Copy the frame on the glass and hand it over as a complete BMP.
 *
 * Call from the panel's owner: nothing flips while it runs, so the copy is
 * one frame, not a smear. The renderer goes on drawing; the capture is the
 * moment it was taken, for as long as the caller takes to send it.
 *
 * @param image Receives the buffer: header first, then bottom-up RGB565.
 *              Borrowed, not given - valid until kdrv_display_capture_end().
 * @param len   Receives its total length in bytes.
 * @return KODE_NO_RESOURCES when built without KODEOS_SCREENSHOT, on a panel
 *         with a single framebuffer, or when PSRAM will not hold the copy;
 *         KODE_DENIED when a capture is already open.
 */
kode_status_t kdrv_display_capture_begin(const uint8_t **image, size_t *len);

/** @brief Release the capture and its buffer. */
void kdrv_display_capture_end(void);

/** @brief Whether any frame has reached the glass since boot. */
bool kdrv_display_capture_painted(void);

/**
 * @brief The console's `test screenshot`: is there a frame to capture, and
 *        would the file be a well-formed BMP?
 *
 * Here rather than in the console for the reason kdrv_storage_selftest()'s
 * body is here: what it checks is the BMP header this file writes, and
 * nothing above kdrv should have to know that layout to have an opinion on it.
 *
 * Deliberately does not take a capture. Copying from a thread that is not
 * AO_Display would race the owner's flips, and it would prove nothing the
 * header cannot: the pixels are the framebuffer's, copied as they are.
 */
kode_status_t kdrv_display_capture_selftest(void);

/* ---- Mirror: a live band of the glass ------------------------------------
 * A capture (above) is a still moment, copied whole. The mirror wants the
 * opposite: a small slice, read while the panel keeps changing, every frame.
 * These two functions are that: a read into a handful of rows of the front
 * framebuffer, and the row range that changed since the last read.
 */

/**
 * @brief Read-only access to @p h rows of the glass, starting at panel row
 *        @p y, top-down.
 *
 * @param y    First panel row of the band (0 = top of the glass).
 * @param h    Row count; must be at least 1 and @p y + @p h at most
 *             KBSP_LCD_V_RES.
 * @param rows Receives a pointer to the band's bytes: h rows of the panel's
 *             stride, contiguous, top-down. Borrowed - valid only until the
 *             panel's owner takes the next frame, so read it in the same
 *             step that asked.
 * @param len  Receives the band's length in bytes.
 * @return KODE_NO_RESOURCES when built without KODEOS_SCREENSHOT or on a
 *         panel with a single framebuffer; KODE_DENIED for a NULL pointer,
 *         when @p h is zero or the band reaches past the bottom of the panel.
 */
kode_status_t kdrv_display_band(uint32_t y, uint32_t h, const uint8_t **rows,
                                size_t *len);

/**
 * @brief Hand back the panel-row range that has changed on the glass since
 *        the last call, and reset it.
 *
 * The reset is what lets two readers each trust the range they got: without
 * it, a second caller would see rows the first one already sent and encode
 * them again, or a slow caller would see nothing new because a faster one
 * already claimed it. Rows accumulate here on every frame that reaches the
 * panel.
 *
 * @param y1 Receives the first changed row (0 = top of the glass), or
 *           KBSP_LCD_V_RES if nothing changed since the last call.
 * @param y2 Receives one past the last changed row, or 0 if nothing
 *           changed. Something changed iff @p y1 < @p y2.
 */
void kdrv_display_dirty_take(int32_t *y1, int32_t *y2);

/**
 * @brief What the DSI link has been through since the panel came up, counted
 *        by the frame interrupt.
 *
 * A screenshot cannot see any of it: it reads the display's shadow, which is
 * right whatever reached the glass. Each count is of frames, not of events:
 * a frame with three underruns counts one.
 */
typedef struct {
    uint32_t frames;     /**< Frames scanned out.                                */
    uint32_t underruns;  /**< Frames the bridge's FIFO ran dry in: stripes and
                           *   black dots on the glass, PSRAM too slow.          */
    uint32_t dpi;        /**< Frames with a DSI host pixel-path error
                           *   (dpi_pld_wr_err, dpi_buff_pld_under).            */
    uint32_t ack;        /**< Frames with an error the panel acknowledged.      */
    uint32_t phy;        /**< Frames with a D-PHY error.                        */
    uint32_t other;      /**< Frames with any other DSI host error bit, the
                           *   command path's included (gen_*: a brightness
                           *   command the FIFO refused).                      */
    uint32_t st0;        /**< The last non-zero DSI host int_st0, to decode.    */
    uint32_t st1;        /**< The last non-zero DSI host int_st1, to decode.    */
    uint32_t missed;     /**< Frames the scan-out was restarted too late for:
                           *   two frame ends more than a refresh and a half
                           *   apart. The bridge sends such a frame as its
                           *   underflow word, the glass flat black for a
                           *   refresh, and `underruns` does not count it.   */
} kdrv_display_link_t;

/**
 * @brief Copy the link's counters.
 *
 * Each field is one 32-bit read of a counter only the frame interrupt writes,
 * so the copy is not one instant across fields - enough for a bench counter.
 * All zeros before the panel is up.
 *
 * @param out Filled with the counters; nothing happens for NULL.
 */
void kdrv_display_link(kdrv_display_link_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_DISPLAY_H_ */
