/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_display.c
 * @brief Chipone CO5300 AMOLED (410x502, MIPI-DSI 1 lane) bring-up.
 */
#include "kdrv_display.h"

#include <string.h>

#include "sdkconfig.h"

#include "kbsp.h"
#include "khal_gpio.h"
#include "khal_i2c.h"
#include "khal_sync.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"

#include "driver/gpio.h"       /* gpio_reset_pin() - see panel_present() below */
#include "esp_attr.h"          /* IRAM_ATTR - see on_trans_done below */
#include "esp_cpu.h"           /* esp_cpu_get_cycle_count() - see on_frame_done() */
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_ldo_regulator.h"
#include "esp_lcd_co5300.h"
#include "esp_system.h"        /* esp_register_shutdown_handler() - see panel_reset_on_restart() */
#include "hal/mipi_dsi_brg_ll.h"    /* the bridge's underrun flag and underflow word */
#include "hal/mipi_dsi_host_ll.h"   /* the command FIFO's flag - see kdrv_display_brightness() */
#include "hal/mipi_dsi_types.h"     /* the DCS packet types - see gram_send() and gram_settle() */
#include "esp_cache.h"              /* video_begin(): the whole of both framebuffers to PSRAM */
#include "soc/lp_system_reg.h"      /* the note a kept panel leaves - see panel_note_take() */
#include "soc/lp_system_struct.h"   /* the pad hold, read at boot - see panel_note_take() */
#include "soc/soc.h"
#include "kdrv_tca6408a.h"          /* whether the supply was left on: see panel_power_on() */

#define TAG "kdrv.display"

/* MIPI-DSI / panel timings: GVO 2.13 MIPI, column offset 22 (CASET starts at
 * 0x16). Changing any of these without the panel datasheet is a good way to
 * get a blank screen. */
#define DSI_DPI_CLK_MHZ      13
#define DSI_LANE_NUM         1
/* 360 and not 480. At 480 the single lane has too little margin: on some
 * content the panel misreads lines, and the About sheet's lower half shook a
 * few pixels from one row down - with the scan-out frozen on one unchanging
 * framebuffer, so the pixels sent were identical every refresh and only the
 * link could move them. 360 is still about 1.7x the ~210 Mbps the 13 MHz
 * RGB565 stream needs in burst mode. */
#define DSI_LANE_BITRATE     360
#define DSI_HSYNC            2
#define DSI_HBP              5
#define DSI_HFP              7
#define DSI_VSYNC            2
#define DSI_VBP              5
#define DSI_VFP              7
/* A refresh is the visible lines plus the blanking ones, each as long as the
 * visible width plus its own blanking, at the pixel clock. The period the
 * header names is that, floored to the microsecond. */
_Static_assert(KDRV_DISPLAY_FRAME_US ==
                   (((KBSP_LCD_H_RES + DSI_HSYNC + DSI_HBP + DSI_HFP) *
                     (KBSP_LCD_V_RES + DSI_VSYNC + DSI_VBP + DSI_VFP)) /
                    DSI_DPI_CLK_MHZ),
               "KDRV_DISPLAY_FRAME_US must follow the DPI timing");
/* What the bridge sends for a pixel the DMA did not deliver: black, in place
 * of its reset blue. Zero in every channel, whatever the RGB coding - see
 * panel_bring_up(). */
#define DSI_UNDERFLOW_BLACK  0U
/* The DPHY rail and the panel's power pin are board facts, not panel facts:
 * which LDO channel and which expander pin is a question about the Kode Dot,
 * so kbsp answers it. */
#define DPHY_LDO_CHAN        KBSP_LCD_DPHY_LDO_CHAN
#define DPHY_LDO_MV          KBSP_LCD_DPHY_LDO_MV

/* How the glass is asked whether it is there, before the DSI is touched: one
 * probe of each touch-controller address, repeated at PANEL_PROBE_GAP_MS for
 * up to PANEL_PROBE_BUDGET_MS. A warm reset answers the first probe; a cold
 * power-on answers once the controller on the assembly has started; a main
 * with no glass pays the whole budget, once, and goes console-only. */
#define PANEL_PROBE_MS         20U
#define PANEL_PROBE_GAP_MS     20U
#define PANEL_PROBE_BUDGET_MS 300U
#define PANEL_PROBE_PULSE_MS  100U   /* Silent this long: the reset pulse (panel_present()). */

/* A brightness write is one DCS command through the DSI host's generic
 * command FIFO, which drains one low-power command per frame (~11 ms here).
 * IDF's write path spins on "FIFO full" with no bound, so the two numbers
 * below are what stand between a burst of writes and a parked thread: writes
 * are spaced at least BRIGHT_GAP_MS apart - two frames, the pace MeshKode's
 * dim ramp settled on after the same FIFO filled under it - and a FIFO still
 * full after BRIGHT_FIFO_WAIT_MS is reported rather than entered. */
#define BRIGHT_GAP_MS          20U
#define BRIGHT_FIFO_WAIT_MS    50U

/* DCS commands the panel takes outside the init table. The names are IDF's
 * (esp_lcd_panel_commands.h); the casts are the driver's, which wants bytes. */
#define DCS_NOP            ((uint8_t)LCD_CMD_NOP)      /**< No operation: the packet a write may lose. */
#define DCS_SLPIN          ((uint8_t)LCD_CMD_SLPIN)    /**< Sleep in: memory keeps its content. */
#define DCS_SLPOUT         ((uint8_t)LCD_CMD_SLPOUT)   /**< Sleep out.                          */
#define DCS_DISPOFF        ((uint8_t)LCD_CMD_DISPOFF)  /**< Display off.                        */
#define DCS_DISPON         ((uint8_t)LCD_CMD_DISPON)   /**< Display on.                         */
#define DCS_CASET          ((uint8_t)LCD_CMD_CASET)    /**< Column address window.              */
#define DCS_RASET          ((uint8_t)LCD_CMD_RASET)    /**< Row address window.                 */
#define DCS_RAMWR          ((uint8_t)LCD_CMD_RAMWR)    /**< Memory write, from the window's top left. */
#define DCS_RAMWRC         ((uint8_t)LCD_CMD_RAMWRC)   /**< Memory write, continuing where RAMWR stopped. */
#define DCS_BRIGHTNESS     ((uint8_t)LCD_CMD_WRDISBV)  /**< Display brightness, 0..255.         */
#define BRIGHT_HW_MAX      255U                        /**< The brightness register's full scale. */
#define BRIGHT_PCT_MAX     100U                        /**< The API's full scale.               */

/* Sleep and wake. The panel needs 120 ms between SLPIN or SLPOUT and the next
 * command (the vendor driver's CO5300_SLEEP_SETTLE_TIME_MS); commands in a
 * row are spaced PANEL_CMD_GAP_MS apart, two frames, for the same reason
 * brightness writes are (BRIGHT_GAP_MS). */
#define PANEL_SLEEP_SETTLE_MS 120U
#define PANEL_CMD_GAP_MS       20U

/* A panel powered this boot (panel_power_on(), panel_reset_cold()). */
#define PANEL_SUPPLY_PIN         7U   /* VCI_EN, the expander's P7 */
#define PANEL_RAIL_SETTLE_MS    20U   /* T2: 10 ms, from VDDI to the reset line high */
#define PANEL_COLD_RESET_LOW_MS  5U
#define PANEL_COLD_FIRST_CMD_MS 20U   /* T3: 10 ms, from the line high to the first command */

/* The panel going OFF (kdrv_display_reset_low()): the reset line low for at
 * least 1 ms before the supply goes. vTaskDelay() of n ticks lasts between n-1
 * and n, the tick being 1 ms, so 2 is the least that always covers it. */
#define PANEL_OFF_RESET_LOW_MS   2U

/* The glass kept lit across a restart (kdrv_display_keep_for_restart()). The
 * note lives in an LP store register for the reason the radio's does: an image's
 * RTC sections are its own, a register is the same address for all of them. It
 * says the panel is awake on a whole frame, at which level, under which init
 * table: bump PANEL_NOTE_MAGIC when s_init_cmds changes what a panel keeps. */
#define PANEL_NOTE_REG    LP_SYSTEM_REG_LP_STORE14_REG
#define PANEL_NOTE_MAGIC  0x4B4C01UL     /* "KL", table 1: 24 bits over the level */
#define PANEL_NOTE_SHIFT  8U
#define PANEL_NOTE_PCT    0xFFU
#define PANEL_READ_TRIES  4U             /* RDDPM asked this many times. */
#define PANEL_READ_GAP_MS 5U
#define RDDPM_CMD         0x0AU          /* Read display power mode. */
#define RDDPM_SLEEP_OUT   0x10U
#define RDDPM_DISPLAY_ON  0x04U

/* Taking the link down waits this long for a flip handed to the engine to be
 * taken: three frames, since one flip completes at the end of the frame in
 * progress and the next frame time covers the rest. */
#define LINK_FLIP_WAIT_MS      60U

/* Taking the link down waits this long for the vertical blanking where the
 * video stops (on_vsync()): two frames, one of which may be in progress. */
#define LINK_STOP_WAIT_MS      40U

/* Where the panel's memory starts. The glass is 410 columns wide in a 416
 * column memory, centred: CASET starts at column 22 (0x16 in s_init_cmds). */
#define PANEL_COL_OFFSET       22U

/* A GRAM write goes through the DSI host's generic FIFOs by hand: IDF's own
 * write spins with no bound on a full FIFO. Pixels travel GRAM_CHUNK_PX to a
 * packet - the command byte and 128 bytes of pixels, 33 words of the payload
 * FIFO - and every wait is bounded by GRAM_WAIT_US, a fault long past the
 * ~0.15 ms a packet takes in low-power mode. GRAM_TAIL_MS lets the last
 * packet's low-power tail leave the lane before the link is taken down, and
 * GRAM_SETTLE_MS lets the NOP that opens every write leave it before the
 * window follows (gram_settle()). */
#define GRAM_CHUNK_PX          64U
#define GRAM_BYTES_PER_PX      2U
#define GRAM_WORD_BYTES        4U
#define GRAM_PKT_BYTES         (1U + (GRAM_CHUNK_PX * GRAM_BYTES_PER_PX))
#define GRAM_PKT_WORDS         ((GRAM_PKT_BYTES + GRAM_WORD_BYTES - 1U) / GRAM_WORD_BYTES)
#define GRAM_WAIT_US           20000U
#define GRAM_TAIL_MS           1U
#define GRAM_SETTLE_MS         1U
#define GRAM_WINDOW_BYTES      4U    /**< CASET and RASET: start and end, 16 bits each. */
#define GRAM_BYTE_MASK         0xFFU /**< One byte of a 16-bit value.                    */
#define GRAM_BYTE_BITS         8U    /**< Bits in a byte.                                */


static const co5300_lcd_init_cmd_t s_init_cmds[] = {
    {0xFE, (uint8_t []){0x00}, 1, 0},
    {0x3A, (uint8_t []){0x55}, 1, 0},   /* COLMOD: RGB565 */
    {0x35, (uint8_t []){0x00}, 1, 0},   /* TE on */
    {0x53, (uint8_t []){0x20}, 1, 0},
    {0x51, (uint8_t []){0x00}, 1, 0},   /* brightness 0: dark until the first frame */
    {0x63, (uint8_t []){0xFF}, 1, 0},
    {0x2A, (uint8_t []){0x00, 0x16, 0x01, 0xAF}, 4, 0},   /* CASET: cols 22..431 */
    {0x2B, (uint8_t []){0x00, 0x00, 0x01, 0xF5}, 4, 0},   /* RASET: rows 0..501  */
    {0x11, NULL, 0, 60},
    {0x29, NULL, 0, 0},
};

static esp_lcd_panel_handle_t   s_panel;
static khal_sem_t               s_flush_done;

/* The command path to the panel: the DPHY rail, the DSI bus and the DBI
 * control IO. It is what every DCS command rides on, and what the link keeps
 * apart from the video (the DPI panel and its framebuffers) so that
 * kdrv_display_link_set() and kdrv_display_gram_write() can build and drop it. */
static esp_ldo_channel_handle_t  s_ldo;
static esp_lcd_dsi_bus_handle_t  s_bus;
static esp_lcd_panel_io_handle_t s_io;

/* s_brought_up: panel_bring_up() has run once - the panel's registers hold the
 * init table, and kdrv_display_init() must never run it again. s_ready: the
 * link is up as well, video is scanning and DCS commands can go. They differ
 * only while kdrv_display_link_set(false) has the link down. s_asleep: the
 * panel is in sleep-in, which survives a link down and up. */
static bool                     s_brought_up;
/* s_keep_lit: this firmware is leaving the frame on the glass, and the restart
 * must not reset the panel; written here, read by the restart's handler.
 * s_video_owed: a resume is waiting for its first frame to start the scan-out. */
static volatile bool            s_keep_lit;
static bool                     s_rail_was_off;  /* This boot powered the panel: see panel_power_on(). */
static bool                     s_video_owed;
/* Counts the framebuffer pairs link_up() has created: see
 * kdrv_display_buffers_epoch(). Written by AO_Display's thread, read by any. */
static volatile uint32_t        s_fb_epoch;
static bool                     s_asleep;

static esp_err_t     panel_resume(uint8_t percent);
static bool          gram_wait(bool (*flag)(dsi_host_dev_t *), bool want);
static bool          cmd_fifo_room(uint32_t wait_ms);
static kode_status_t brightness_write(uint8_t percent);
static void          hold_release(void);
static kode_status_t panel_command(uint8_t cmd);
static kode_status_t panel_read1(uint8_t cmd, uint8_t *val);

/* The two DPI framebuffers and the assembly state. `back` is the one being
 * assembled - the engine scans the other. A band is rows-only: chunks are
 * full frame width in practice, and a row range of a full-width frame is
 * contiguous memory, which is what draw_bitmap needs handed to it. */
static uint8_t *s_fb[2];
static uint8_t  s_back;          /* index into s_fb of the assembly target  */
static bool     s_frame_open;    /* chunks landed since the last flip       */
static int32_t  s_dirty_y1, s_dirty_y2;   /* this frame's band              */
static int32_t  s_prev_y1,  s_prev_y2;    /* the band the last flip carried */
/* A flip handed to the engine and not yet taken: set by the task once
 * draw_bitmap has returned, cleared by on_frame_done() when the scan moves. */
static volatile bool s_flip_pending;
/* True only across the flip's own draw_bitmap call, whose synchronous
 * completion must not release the renderer. Task context only. */
static volatile bool s_flipping;
/* The video's end, asked by link_down() and done by on_vsync() in the vertical
 * blanking, where no row is on the lane. */
static volatile bool s_video_stop_ask;
static volatile bool s_video_stopped;
static bool                     s_ready;          /**< Brought up and the link is up. */

/* What the DSI link went through, counted once per frame by on_frame_done():
 * written there alone, read by kdrv_display_link(). DRAM statics, so the
 * interrupt reaches them with the flash cache off. The masks follow IDF
 * 6.1's dsi_host_int_st0_reg_t / dsi_host_int_st1_reg_t. */
#define KDRV_DSI_ST0_ACK_MASK  0x0000FFFFU /**< int_st0 ack_with_err_0..15.       */
#define KDRV_DSI_ST0_PHY_MASK  0x001F0000U /**< int_st0 dphy_errors_0..4.         */
#define KDRV_DSI_ST1_DPI_MASK  0x00080080U /**< int_st1 dpi_pld_wr_err (bit 7) and
                                             *   dpi_buff_pld_under (bit 19).      */
#define KDRV_DSI_ST1_ALL_MASK  0x00081FFFU /**< int_st1's defined bits: 0..12, 19.  */
#define KDRV_DSI_MISSED_SKIP   2U         /**< Frame ends not judged after the bring-up:
                                             *   the first has no end before it.     */
#define KDRV_DSI_MISSED_GAP_US ((KDRV_DISPLAY_FRAME_US * 3U) / 2U) /**< A refresh and a half:
                                             *   a longer gap between two frame ends
                                             *   is a missed frame.                  */
static volatile uint32_t s_lk_frames;
static volatile uint32_t s_lk_underruns;
static volatile uint32_t s_lk_dpi;
static volatile uint32_t s_lk_ack;
static volatile uint32_t s_lk_phy;
static volatile uint32_t s_lk_other;
static volatile uint32_t s_lk_st0;
static volatile uint32_t s_lk_st1;
static volatile uint32_t s_lk_missed;
static volatile uint32_t s_lk_end_cyc;   /* the cycle count at the last frame end */

/* Dark until the first frame: a warm reset never reaches the panel's supply
 * (P7), so its RAM still holds the last picture. s_held stands from the
 * end of the bring-up until the first frame reaches the glass; meanwhile
 * kdrv_display_brightness() keeps its value in s_held_pct instead of writing
 * it. s_up_ms is when the bring-up ended, which the line that first frame
 * logs counts from. Task context, AO_Display's thread, like the rest. */
static bool     s_held;
static uint8_t  s_held_pct;
static uint8_t  s_last_pct = BRIGHT_PCT_MAX;   /* the last level asked for */
static uint32_t s_up_ms;

/* ---- The capture ---------------------------------------------------------
 * A screenshot is the frame on the glass - the front framebuffer - copied
 * once, when it is asked for, into an allocation that is already a BMP file:
 * a 122-byte header, then the pixels bottom-up. Nothing assembles an image
 * later because there is nothing left to assemble - a capture hands out this
 * pointer and this length, and that is the file. It is copied in AO_Display's
 * own step, and only AO_Display asks for a flip, so the front buffer cannot
 * change under the copy; and it is freed when the caller is done sending it.
 * Nothing is copied on the way to the panel, so screenshots cost the frame
 * rate nothing.
 *
 * The header is BITMAPFILEHEADER + BITMAPINFOHEADER followed by the three
 * channel masks, which is where BI_BITFIELDS has always put them - in the
 * space a palette would occupy. A BITMAPV4HEADER carries the same masks in
 * named fields and looks tidier, and it was the first thing tried: macOS
 * ImageIO reads 16bpp V4 only when bV4CSType is zero, and rejects the file
 * outright when it says LCS_sRGB, which is the one value that is actually
 * true. Measured 2026-08-24 with sips, which is Preview's decoder. The old
 * form has no field to get wrong.
 *
 * Height is positive, which in BMP means bottom-up - hence the row flip in
 * kdrv_display_capture_begin()'s copy, which costs one subtraction and saves
 * the whole format from needing a translation pass.
 *
 * The pixels start at 128 rather than at 66, where the masks end. BMP reads
 * the pixel array from the offset the header names, so padding to a round
 * number is legal rather than clever - and it buys a 64-byte-aligned pixel
 * area with a 4-byte-aligned stride for the copy.
 */
#if CONFIG_KODEOS_SCREENSHOT
#define SHOT_BPP        2U
#define SHOT_HDR_LEN    128U
#define SHOT_STRIDE     ((size_t)KBSP_LCD_H_RES * SHOT_BPP)
#define SHOT_PIXELS     (SHOT_STRIDE * (size_t)KBSP_LCD_V_RES)
#define SHOT_LEN        ((size_t)SHOT_HDR_LEN + SHOT_PIXELS)

static uint8_t *s_shot;           /**< The capture while one is open.        */
static bool     s_painted;        /**< A frame has reached the glass.        */

/* The mirror's own accumulation: rows of the glass that changed since the
 * mirror last asked. This is deliberately NOT s_dirty_y1/s_dirty_y2 (declared
 * above, by the framebuffer state) - those are reset by kdrv_display_flush()'s
 * flip every frame, on a schedule the mirror does not control, so a reader
 * sharing them would race the panel for the same two variables and miss
 * whole frames whenever it fell behind. Widened by shown_rows() at every
 * frame that reaches the glass, taken and reset only by
 * kdrv_display_dirty_take(). */
static int32_t  s_mirror_y1 = (int32_t)KBSP_LCD_V_RES;
static int32_t  s_mirror_y2 = 0;

/** @brief Store @p v little-endian, the only byte order BMP has. */
static void put_u16le(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
}

/** @brief Store @p v little-endian. */
static void put_u32le(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v & 0xFFU);
    p[1] = (uint8_t)((v >> 8) & 0xFFU);
    p[2] = (uint8_t)((v >> 16) & 0xFFU);
    p[3] = (uint8_t)((v >> 24) & 0xFFU);
}

/**
 * @brief Write the BMP header into the front of a capture.
 *
 * Every field not set here is zero and means zero: no palette, nothing
 * important about the colours, and the run from the masks to the pixel offset
 * is padding. The memset is what says so, and it is why this reads as short
 * as it does.
 *
 * @param h SHOT_HDR_LEN bytes.
 */
static void bmp_header(uint8_t *h)
{
    (void)memset(h, 0, (size_t)SHOT_HDR_LEN);

    h[0] = (uint8_t)'B';
    h[1] = (uint8_t)'M';
    put_u32le(&h[2],  (uint32_t)SHOT_LEN);
    put_u32le(&h[10], (uint32_t)SHOT_HDR_LEN);      /* pixels start here    */

    put_u32le(&h[14], 40U);                         /* BITMAPINFOHEADER     */
    put_u32le(&h[18], (uint32_t)KBSP_LCD_H_RES);
    put_u32le(&h[22], (uint32_t)KBSP_LCD_V_RES);    /* positive: bottom-up  */
    put_u16le(&h[26], 1U);                          /* planes               */
    put_u16le(&h[28], 16U);                         /* bits per pixel       */
    put_u32le(&h[30], 3U);                          /* BI_BITFIELDS         */
    put_u32le(&h[34], (uint32_t)SHOT_PIXELS);
    put_u32le(&h[38], 2835U);                       /* ~72 dpi across       */
    put_u32le(&h[42], 2835U);                       /* ~72 dpi down         */
    /* The masks go where a palette would, which is what BI_BITFIELDS means. */
    put_u32le(&h[54], 0xF800U);                     /* red   mask           */
    put_u32le(&h[58], 0x07E0U);                     /* green mask           */
    put_u32le(&h[62], 0x001FU);                     /* blue  mask           */
}

/**
 * @brief Note that rows @p y1 .. @p y2 of the glass have just changed.
 *
 * Called wherever a frame reaches the panel, after it did.
 */
static void shown_rows(int32_t y1, int32_t y2)
{
    s_painted = true;
    if ( y1 < s_mirror_y1 ) { s_mirror_y1 = y1; }
    if ( y2 > s_mirror_y2 ) { s_mirror_y2 = y2; }
}
#endif

/**
 * @brief Panel DMA completion, in ISR context.
 *
 * It signals this file's own semaphore rather than calling back into the
 * renderer, and that is not a style choice. With CONFIG_LCD_DSI_ISR_CACHE_SAFE
 * this ISR runs with the flash cache disabled - which is precisely when it
 * matters, since writing an app image is what turns the cache off - so every
 * function on this path has to be in IRAM. A callback pointer would let a
 * caller register something that is not, and the fault would only appear
 * during a flash write. Signalling the semaphore directly keeps the whole path
 * inside kdrv and khal, which are the two layers that can spell IRAM_ATTR.
 */
static bool IRAM_ATTR on_trans_done(esp_lcd_panel_handle_t panel,
                                    esp_lcd_dpi_panel_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    /* A flip's own draw_bitmap calls this synchronously, before the engine
     * has moved; that completion is on_frame_done()'s to give. */
    if ( !s_flipping )
    {
        khal_sem_signal(s_flush_done);
    }
    return false;
}

/**
 * @brief End of a scanned frame, in ISR context: where a flip completes.
 *
 * A flip only asks the engine to move. draw_bitmap retargets the scan-out,
 * but the frame in progress finishes on the buffer it started on, and the
 * move happens when the DMA is restarted at its end - which IDF does just
 * before calling this. Released any earlier, the next frame started writing
 * its debt and its first chunk into the buffer still being scanned - 3 to 7
 * frames in 100 while a sheet was dragged - and whatever of that reached PSRAM
 * ahead of the scan went on the glass a refresh early. Waiting here costs the
 * renderer at most one refresh (16.8 ms at 59.4 Hz).
 *
 * The flag is raised after draw_bitmap returns, so a frame that ended while
 * the flip was being written back cannot count as the move. What is left is
 * the few instructions between the driver's store of the new index and ours;
 * an end of frame landing exactly there completes one flip a refresh early.
 * The same IRAM rules as on_trans_done() apply, for the same reason.
 */
static bool IRAM_ATTR on_frame_done(esp_lcd_panel_handle_t panel,
                                    esp_lcd_dpi_panel_event_data_t *edata,
                                    void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    if ( s_flip_pending )
    {
        s_flip_pending = false;
        khal_sem_signal(s_flush_done);
    }

    /* The link's health, one count per frame per kind. The bridge's underrun
     * flag latches whether its interrupt is enabled or not, and IDF's is
     * masked at bring-up so its handler does not clear the flag first. The
     * host's two status registers clear when read. Inline register reads and
     * DRAM statics only: this runs with the flash cache off. */
    s_lk_frames = s_lk_frames + 1U;
    /* A missed frame. The scan-out is restarted here, and a restart held
     * back past the vertical blanking and the bridge's FIFO - by anything
     * that keeps this interrupt off this core for about a millisecond - is
     * one the bridge does not wait for: it sends that whole frame as its
     * underflow word, dpi_rsv_data - black, set so in panel_bring_up(), in
     * place of its reset blue - and raises no underrun for it. What is left
     * of it is the gap: this frame end comes two refreshes after the last
     * one, not one. The interrupt stays on the core it was allocated on, so
     * one core's cycle counter times both ends. */
    {
        uint32_t const now = esp_cpu_get_cycle_count();
        uint32_t const gap_us = (now - s_lk_end_cyc)
                                / (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;

        if ( (KDRV_DSI_MISSED_SKIP < s_lk_frames)
            && (KDRV_DSI_MISSED_GAP_US < gap_us) )
        {
            s_lk_missed = s_lk_missed + 1U;
        }
        s_lk_end_cyc = now;
    }
    if ( 0U != MIPI_DSI_BRIDGE.int_raw.underrun_int_raw )
    {
        s_lk_underruns = s_lk_underruns + 1U;
        mipi_dsi_brg_ll_clear_interrupt_status(&MIPI_DSI_BRIDGE,
                                               MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
    }
    uint32_t const st0 = MIPI_DSI_HOST.int_st0.val;
    uint32_t const st1 = MIPI_DSI_HOST.int_st1.val;
    if ( (st0 & KDRV_DSI_ST0_ACK_MASK) != 0U )
    {
        s_lk_ack = s_lk_ack + 1U;
    }
    if ( (st0 & KDRV_DSI_ST0_PHY_MASK) != 0U )
    {
        s_lk_phy = s_lk_phy + 1U;
    }
    if ( (st1 & KDRV_DSI_ST1_DPI_MASK) != 0U )
    {
        s_lk_dpi = s_lk_dpi + 1U;
    }
    if ( (st1 & ~KDRV_DSI_ST1_DPI_MASK) != 0U )
    {
        s_lk_other = s_lk_other + 1U;
    }
    if ( 0U != st0 )
    {
        s_lk_st0 = st0;
    }
    if ( 0U != st1 )
    {
        s_lk_st1 = st1;
    }
    return false;
}

/**
 * @brief Start of the vertical sync, in ISR context: where the video stops.
 *
 * The panel writes the video into its own memory as it arrives, and keeps
 * what it has when the video ends. Ended in the middle of a frame, the row on
 * the lane at that moment stays half written: a dark line across the resting
 * glass, wherever the scan happened to be. Here only blanking is on the lane,
 * so the host is put in command mode now and every row the panel keeps is
 * whole. One register bit, written inline: the same IRAM rules as
 * on_trans_done() apply.
 */
static bool IRAM_ATTR on_vsync(esp_lcd_panel_handle_t panel,
                               esp_lcd_dpi_panel_event_data_t *edata,
                               void *user_ctx)
{
    (void)panel; (void)edata; (void)user_ctx;
    if ( s_video_stop_ask )
    {
        s_video_stop_ask = false;
        MIPI_DSI_HOST.mode_cfg.cmd_video_mode = 1U;
        s_video_stopped = true;
    }
    return false;
}

/**
 * @brief Let the panel rails settle before the glass is asked anything.
 *
 * VCI_EN is P7 of the shared expander, and the expander has an owner: by the
 * time AO_Display calls kdrv_display_init() it has asked AO_IoExp for that pin
 * and been told it is driving high. This driver never touches the expander -
 * a panel driver that created its own handle on a chip four other things use
 * is how the screen once died when the buttons came up - so what is left of
 * powering the panel here is the settling time the module needs. It runs
 * before panel_present() rather than inside panel_bring_up(), because the
 * probe is the first thing on the rail that is asked to answer.
 */
static void panel_power_on(void)
{
    if ( kdrv_tca6408a_boot_drove_high(PANEL_SUPPLY_PIN) )
    {
        return;   /* up since the last firmware: there is nothing to settle */
    }
    s_rail_was_off = true;
    khal_delay_ms(PANEL_RAIL_SETTLE_MS);
    KLOG_I(TAG, "panel power settled (VCI_EN is AO_IoExp's P7)");
}

/**
 * @brief Reset a panel whose rail has just come up, at its own pace.
 *
 * esp_lcd's reset waits 120 ms for any panel, which is the most a reset can take:
 * the hoja's "reset applied during Sleep Out". A panel that has just been powered
 * is in Sleep In, and takes 5 ms; the power-on sequence asks for 10 ms before the
 * first command (T3), 20 ms here. A panel that may be lit goes through esp_lcd.
 */
static void panel_reset_cold(void)
{
    (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 1U);
    khal_delay_ms(PANEL_COLD_RESET_LOW_MS);
    (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 0U);
    khal_delay_ms(PANEL_COLD_RESET_LOW_MS);
    (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 1U);
    khal_delay_ms(PANEL_COLD_FIRST_CMD_MS);
}

/**
 * @brief Take the reset line back, high, from a panel that is gone.
 *
 * esp_lcd's co5300 driver configured the line as its reset and keeps ESP-IDF's
 * claim on it after esp_lcd_panel_del(): taken again as it is, the pin logs
 * "conflict found for GPIO[7]" on every rest of the glass. gpio_reset_pin()
 * revokes the claim and leaves a pull-up, so the line reads high through the
 * gap, as in panel_present().
 */
static void reset_line_reclaim(void)
{
    (void)gpio_reset_pin((gpio_num_t)KBSP_PIN_DSP_TP_RST);
    (void)khal_gpio_output(KBSP_PIN_DSP_TP_RST, true);
}

/**
 * @brief Is there a glass at all?
 *
 * The panel assembly carries its touch controller, a CST820 at 0x15, and
 * it answering its address is the assembly being there. (Supplier B's
 * panel carries a CST916E at 0x5A instead, the haptic driver's address on
 * this board, so it cannot be told from the antenna PCB and is not driven.) This is asked BEFORE the DSI is
 * touched, because the panel driver's first act is to read the panel's ID
 * over DSI, and a read from a panel that is not there never returns: the HAL
 * waits for a FIFO nothing fills, this task spins at its priority for the
 * life of the boot, and everything below it on that core starves (seen
 * 2026-09-14 on a main PCB tested before its panel). A main without its
 * glass is a board the line tests; it boots console-only, as designed.
 *
 * It is asked more than once, and with the reset line released first,
 * because a cold power-on is not the bench's warm reset. On a warm reset the
 * expander kept P7 high through it, the assembly has been powered since the
 * last boot, the bootloader's pulse on GPIO7 ended long ago, and the
 * controller answers the first probe. On a cold power-on
 * - the battery connected, a wake from ship mode, every customer's first
 * boot - P7 was pulled down (R48) until AO_IoExp drove it a moment
 * ago, and the controller's reset is GPIO7, shared with the panel, which the
 * bootloader leaves pulled up on every boot: the controller starts as P7
 * powers it, so at the first probe it has had power only for as long as
 * panel_power_on() waited. A board whose bootloader has no pulse comes
 * back from esp_restart() with the line still held low by
 * panel_reset_on_restart(), and its controller starts here too. So the line
 * is driven high before the first probe, and the probe is repeated at
 * PANEL_PROBE_GAP_MS for up to PANEL_PROBE_BUDGET_MS. A healthy unit answers
 * as soon as its controller has started; only a main with no glass pays the
 * whole budget. The bench cannot see the cold case at all - opening the port
 * is a warm reset - which is why the budget is a number and not a
 * measurement.
 *
 * The reset line stays driven high on a glass that answered: esp_lcd takes
 * the pin over in panel_bring_up() and pulses it properly, and a live
 * assembly is not left floating in between. On a main with no glass the pin
 * goes back to nobody's.
 *
 * esp_lcd's own co5300 driver configures the same pin again, as its reset
 * line, the moment panel_bring_up() reaches it - and ESP-IDF's GPIO driver
 * remembers khal_gpio_output()'s claim on it until something revokes it,
 * so that second gpio_config() logs "conflict found for GPIO[7]" on every
 * boot with a panel, a warning that reads like a fault and is not one.
 * gpio_reset_pin() is what revokes it, but khal_gpio_release() (which calls
 * it) also forces the pin floating afterwards on purpose - the right
 * default for a header position handed back to whoever wired it, and the
 * wrong one here: this pin is not going back to anybody, esp_lcd is about
 * to reconfigure it as an output within the next few instructions, and
 * gpio_reset_pin() itself already leaves an internal pull-up standing while
 * it disables the driver, so the line reads high, never floats, through
 * the gap. Called here, raw, once the pin's job as a probe is done and
 * before bring_up() reaches it.
 *
 * @return true when the touch controller answered at 0x15.
 */
static bool panel_present(void)
{
    bool     found  = false;
    bool     pulsed = false;
    uint32_t waited = 0U;

    (void)khal_gpio_output(KBSP_PIN_DSP_TP_RST, true);
    if ( esp_reset_reason() == ESP_RST_DEEPSLEEP )
    {
        /* OFF held the line low and cut the rail. A controller that missed its
         * start answers nothing until a reset: reset first, and do not lose
         * 100 ms finding out. */
        pulsed = true;
        (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 0U);
        khal_delay_ms(PANEL_COLD_RESET_LOW_MS);
        (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 1U);
    }
    for ( ;; )
    {
        found = khal_i2c_probe(KBSP_I2C_ADDR_TOUCH, PANEL_PROBE_MS);
        if ( found || (PANEL_PROBE_BUDGET_MS <= waited) )
        {
            break;
        }
        if ( (PANEL_PROBE_PULSE_MS <= waited) && !pulsed )
        {
            /* A CST820 in its low-power scan NAKs everything until a finger or
             * a reset. A controller that is simply slow loses nothing by this. */
            pulsed = true;
            KLOG_W(TAG, "the touch controller is silent: a reset pulse before the verdict");
            (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 0U);
            khal_delay_ms(PANEL_COLD_RESET_LOW_MS);
            (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 1U);
        }
        khal_delay_ms(PANEL_PROBE_GAP_MS);
        waited += PANEL_PROBE_GAP_MS;
    }
    if ( !found )
    {
        khal_gpio_release(KBSP_PIN_DSP_TP_RST);
        return found;
    }
    (void)gpio_reset_pin((gpio_num_t)KBSP_PIN_DSP_TP_RST);
    if ( 0U < waited )
    {
        /* The line a cold power-on leaves in the log; a warm reset never
         * prints it. */
        KLOG_I(TAG, "panel assembly answered after %lu ms",
               (unsigned long)waited);
    }
    return found;
}

/**
 * @brief Hold the panel and its touch controller in reset through a restart.
 *
 * A shutdown handler, registered by kdrv_display_init() once the panel is up
 * and run by esp_restart() in the task that asked for the restart, before the
 * scheduler stops. The line is an output already - esp_lcd's since
 * panel_bring_up() - so this only drives it. Low, and left low: esp_restart()
 * resets neither the GPIOs nor the expander, so the panel keeps its supply
 * and the line keeps its level through the restart, which holds both parts in
 * reset until the bootloader releases the line or panel_present() drives it
 * high. No pulse is timed here; the restart holds the line far longer than
 * either part needs.
 *
 * It acts on the panel outside AO_Display's step, which makes it a documented
 * exception to "everything is an event" (design rule 3). No event could do
 * it: AO_Boot is not esp_restart()'s only caller - esp_hosted restarts from
 * its own SDIO task when the link to the C5 fails, and asks nobody first -
 * and esp_restart() waits on no queue before the scheduler stops. Hence the
 * limit: one pin level, written once, no lock, no allocation, and nothing
 * AO_Display's step reads; AO_Touch reads a touch controller held in reset
 * as a release.
 */
static void panel_reset_on_restart(void)
{
    if ( s_keep_lit )
    {
        return;   /* kdrv_display_keep_for_restart(): the line is held high */
    }
    /* A hold nobody asked for would make the write below a no-op. */
    (void)gpio_hold_dis((gpio_num_t)KBSP_PIN_DSP_TP_RST);
    (void)khal_gpio_write(KBSP_PIN_DSP_TP_RST, false);
}

/**
 * @brief Whether the firmware before this boot left the glass lit for us, read
 *        once: the note is erased, and the reset line is back in our hands.
 *
 * All three must stand: the note, a software reset (a panic or a watchdog says
 * nothing about the panel) and the pad hold on the line. A hold with no note is
 * an image that stops a different way, or a deep sleep: the panel's memory is
 * not ours to show, and the bring-up resets it.
 *
 * @param percent Receives the level the panel is lit at.
 * @return true when the panel may be taken up without its reset.
 */
static bool panel_note_take(uint8_t *percent)
{
    uint32_t const word = REG_READ(PANEL_NOTE_REG);
    uint32_t const bit  = 1UL << (uint32_t)KBSP_PIN_DSP_TP_RST;
    bool const     held = (LP_SYS.pad_rtc_hold_ctrl0.pad_rtc_hold_ctrl0 & bit) != 0U;

    REG_WRITE(PANEL_NOTE_REG, 0U);
    if ( held )
    {
        /* Driven high first: letting the hold go changes nothing on the wire. */
        (void)khal_gpio_output(KBSP_PIN_DSP_TP_RST, true);
        (void)gpio_hold_dis((gpio_num_t)KBSP_PIN_DSP_TP_RST);
    }
    *percent = (uint8_t)(word & PANEL_NOTE_PCT);
    return held && ((word >> PANEL_NOTE_SHIFT) == PANEL_NOTE_MAGIC)
           && (esp_reset_reason() == ESP_RST_SW) && (BRIGHT_PCT_MAX >= *percent);
}

/**
 * @brief Take down the command path, whatever part of it stands.
 *
 * The reverse of cmd_path_up(), in the order that frees each part after what
 * used it: the DBI IO, the bus, then the DPHY rail. The bus holds the PM lock
 * that keeps the P4 out of light sleep, and the rail is the DSI's own current,
 * so a link that stays down is what lets the board rest.
 *
 * @return The first failure IDF reported, ESP_OK when every part went.
 */
static esp_err_t cmd_path_down(void)
{
    esp_err_t first = ESP_OK;
    esp_err_t err;

    if ( NULL != s_io )
    {
        err = esp_lcd_panel_io_del(s_io);
        s_io = NULL;
        if ( ESP_OK == first )
        {
            first = err;
        }
    }
    if ( NULL != s_bus )
    {
        err = esp_lcd_del_dsi_bus(s_bus);
        s_bus = NULL;
        if ( ESP_OK == first )
        {
            first = err;
        }
    }
    if ( NULL != s_ldo )
    {
        err = esp_ldo_release_channel(s_ldo);
        s_ldo = NULL;
        if ( ESP_OK == first )
        {
            first = err;
        }
    }
    return first;
}

/**
 * @brief Bring up the command path: the DPHY rail, the DSI bus, the DBI IO.
 *
 * Everything a DCS command needs and nothing that scans video: no DPI panel,
 * no framebuffer, no panel object, so the panel's reset line is never touched
 * and no init sequence is sent. The bus leaves the host in command mode.
 * Every failure is logged under its own name and leaves nothing standing.
 *
 * @return ESP_OK when commands can go.
 */
static esp_err_t cmd_path_up(void)
{
    const esp_ldo_channel_config_t ldo_cfg = {
        .chan_id    = DPHY_LDO_CHAN,
        .voltage_mv = DPHY_LDO_MV,
    };
    const esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id             = 0,
        .num_data_lanes     = DSI_LANE_NUM,
        .lane_bit_rate_mbps = DSI_LANE_BITRATE,
    };
    const esp_lcd_dbi_io_config_t dbi_cfg = {
        .virtual_channel = 0,
        .lcd_cmd_bits    = 8,
        .lcd_param_bits  = 8,
    };

    /* The DPHY rail, then the bus, then the control IO. */
    esp_err_t err = esp_ldo_acquire_channel(&ldo_cfg, &s_ldo);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "DPHY LDO ch%d: %s", DPHY_LDO_CHAN, esp_err_to_name(err));
        return err;
    }
    err = esp_lcd_new_dsi_bus(&bus_cfg, &s_bus);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "dsi bus: %s", esp_err_to_name(err));
        (void)cmd_path_down();
        return err;
    }
    err = esp_lcd_new_panel_io_dbi(s_bus, &dbi_cfg, &s_io);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "dbi io: %s", esp_err_to_name(err));
        (void)cmd_path_down();
        return err;
    }
    return ESP_OK;
}

/**
 * @brief The DPI timing of the glass, as one configuration both the bring-up
 *        and a link coming back build their video panel from.
 *
 * @param cfg Receives the configuration.
 */
static void dpi_config_fill(esp_lcd_dpi_panel_config_t *cfg)
{
    const esp_lcd_dpi_panel_config_t dpi_cfg = {
        .virtual_channel   = 0,
        /* Two framebuffers, and they are the whole tear-free story. The
         * panel runs in DPI video mode: the P4 scans a framebuffer
         * continuously, so writing into the one being scanned races the
         * scan and shears whatever is moving. With two, chunks are
         * assembled into the one the engine is NOT reading and the flip
         * happens in the vsync ISR, at a frame boundary - the driver's
         * draw_bitmap flips instead of copying whenever the pointer it is
         * handed lies inside a framebuffer, which kdrv_display_flush()
         * arranges. */
        .num_fbs           = 2,
        .dpi_clk_src       = MIPI_DSI_DPI_CLK_SRC_DEFAULT,
        .dpi_clock_freq_mhz = DSI_DPI_CLK_MHZ,
        .in_color_format   = LCD_COLOR_FMT_RGB565,
        .video_timing = {
            .h_size            = KBSP_LCD_H_RES,
            .v_size            = KBSP_LCD_V_RES,
            .hsync_back_porch  = DSI_HBP,
            .hsync_pulse_width = DSI_HSYNC,
            .hsync_front_porch = DSI_HFP,
            .vsync_back_porch  = DSI_VBP,
            .vsync_pulse_width = DSI_VSYNC,
            .vsync_front_porch = DSI_VFP,
        },
    };
    *cfg = dpi_cfg;
}

/**
 * @brief Keep the link's health flags where on_frame_done() counts them.
 *
 * IDF enables the bridge's underrun interrupt inside the panel init, and its
 * handler clears the flag and prints through the ROM, which never reaches the
 * console. Masked, the flag still latches and on_frame_done() counts it. The
 * DSI host latches an error into int_st0/int_st1 only where int_msk0/int_msk1
 * let it (Synopsys's DW DSI host: 1 = on), and both reset to 0 - on every
 * new bus, which is why a link coming back sets them again. Nobody routes the
 * host's own interrupt line - IDF allocates the bridge's alone - so turning
 * them on raises no interrupt anywhere: it only lets on_frame_done() see them.
 */
static void link_watch(void)
{
    mipi_dsi_brg_ll_enable_interrupt(&MIPI_DSI_BRIDGE,
                                     MIPI_DSI_BRG_LL_EVENT_UNDERRUN, false);
    mipi_dsi_brg_ll_clear_interrupt_status(&MIPI_DSI_BRIDGE,
                                           MIPI_DSI_BRG_LL_EVENT_UNDERRUN);
    MIPI_DSI_HOST.int_msk0.val = KDRV_DSI_ST0_ACK_MASK | KDRV_DSI_ST0_PHY_MASK;
    MIPI_DSI_HOST.int_msk1.val = KDRV_DSI_ST1_ALL_MASK;
}

/**
 * @brief Hand the video panel's frame events to this file, and start the
 *        link's counters clean.
 *
 * @return ESP_OK, or what IDF said when it refused the callbacks.
 */
static esp_err_t link_events_on(void)
{
    const esp_lcd_dpi_panel_event_callbacks_t cbs = {
        .on_color_trans_done   = on_trans_done,
        .on_frame_buf_complete = on_frame_done,
        .on_vsync              = on_vsync,
    };
    esp_err_t const err = esp_lcd_dpi_panel_register_event_callbacks(s_panel,
                                                                    &cbs, NULL);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "event cbs: %s", esp_err_to_name(err));
        return err;
    }
    /* What the bring-up's own command traffic left in the host's status is
     * not the link's: read once (they clear on read) so the first frame
     * on_frame_done() counts starts clean. */
    (void)MIPI_DSI_HOST.int_st0.val;
    (void)MIPI_DSI_HOST.int_st1.val;
    return ESP_OK;
}

/**
 * @brief Pick up the video panel's two framebuffers and start the flip
 *        bookkeeping from a clean slate: the engine scans buffer 0, the
 *        other is the back, and the back owes the whole screen.
 */
static void link_frames_reset(void)
{
    if ( esp_lcd_dpi_panel_get_frame_buffer(s_panel, 2,
                                           (void **)&s_fb[0],
                                           (void **)&s_fb[1]) != ESP_OK )
    {
        /* No pair, no flip: flushes fall back to racing the scan, which is
         * a worse picture but still a picture. */
        s_fb[0] = NULL;
        s_fb[1] = NULL;
        KLOG_W(TAG, "single framebuffer: flushes will tear");
    }
    s_back         = 1U;   /* the engine starts on fb 0 */
    s_frame_open   = false;
    s_flip_pending = false;
    s_dirty_y1     = KBSP_LCD_V_RES;
    s_dirty_y2     = 0;
    s_prev_y1      = 0;
    s_prev_y2      = KBSP_LCD_V_RES;
}

/** @brief The panel answers a DCS read within a deadline: IDF's own has none. */
static bool panel_answers(void)
{
    uint8_t id = 0U;
    for ( uint32_t i = 0U; 3U > i; ++i )
    {
        if ( i > 0U ) { khal_delay_ms(20U); }
        if ( panel_read1(0x04U, &id) == KODE_OK ) { return true; }
    }
    return false;
}

/** The bring-up proper. Kept in esp_err_t so every failure keeps its name in
 *  the log; the public entry point below is what the rest of kodeOS sees. */
static esp_err_t panel_bring_up(void)
{
    /* 1. DPHY rail, 2. DSI bus + DBI control IO. The panel's rail settled
     *    before the glass was probed, in kdrv_display_init(). */
    esp_err_t err = cmd_path_up();
    if ( ESP_OK != err )
    {
        return err;
    }

    /* 3. The CO5300 itself. */
    esp_lcd_dpi_panel_config_t dpi_cfg;
    dpi_config_fill(&dpi_cfg);
    co5300_vendor_config_t vendor_cfg = {
        .init_cmds      = s_init_cmds,
        .init_cmds_size = sizeof(s_init_cmds) / sizeof(s_init_cmds[0]),
        .flags.use_mipi_interface = 1,
        .mipi_config = { .dsi_bus = s_bus, .dpi_config = &dpi_cfg },
    };
    const esp_lcd_panel_dev_config_t dev_cfg = {
        .reset_gpio_num = KBSP_PIN_DSP_TP_RST,  /* shared with the touch */
        .rgb_ele_order  = LCD_RGB_ELEMENT_ORDER_RGB,
        /* 16, not 24: at 24 bpp the CO5300 driver wants h * v % 8 == 0, and 410 x 502 is not. */
        .bits_per_pixel = 16,
        .vendor_config  = &vendor_cfg,
    };
    /* Our init table sets 3Ah, the pixel format, on purpose, and the vendor's
     * driver warns that it will be used: by design, not a fault. */
    kode_log_set_level("co5300_mipi", KODE_LOG_ERROR);
    err = esp_lcd_new_panel_co5300(s_io, &dev_cfg, &s_panel);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "co5300: %s", esp_err_to_name(err));
        return err;
    }
    /* The bridge's underflow word, black. For every pixel the DMA has not
     * delivered, the bridge sends dpi_rsv_data instead, and a scan-out
     * restart held off about a millisecond past a frame's end delivers none
     * of the next frame: the whole refresh goes out as this word (see
     * on_frame_done()). Its reset value, 0x3FFF, is blue - a flash across
     * the glass on a dark interface. Black is all channels zero in every RGB
     * coding the bridge can send, and it sends RGB565 here (out_color_format
     * is left to follow in_color_format), so zero is black however the
     * word's bits map to the channels. A YUV output would need another word.
     * The register is one of the bridge's DPI_* set, which take effect on
     * dpi_config_update: written here, between the panel's creation and its
     * init, it is latched by the update that starts the scan-out inside
     * esp_lcd_panel_init(). Only the field is written - on chip revision 3
     * the same register holds dpi_dbg_en. IDF has no LL call for it. */
    MIPI_DSI_BRIDGE.dpi_rsv_dpi_data.dpi_rsv_data = DSI_UNDERFLOW_BLACK;

    if ( s_rail_was_off )
    {
        panel_reset_cold();                /* pulses GPIO7: resets touch too */
    }
    else
    {
        err = esp_lcd_panel_reset(s_panel);   /* pulses GPIO7: resets touch too */
    }
    if ( (ESP_OK == err) && !panel_answers() )
    {
        /* IDF's init reads the ID with no deadline and a panel that does not
         * answer would hold this task until the watchdog reboots the board,
         * for ever. Asked here first, with one. */
        KLOG_E(TAG, "the panel does not answer over DSI (ID read timed out)");
        (void)esp_lcd_panel_del(s_panel);
        s_panel = NULL;
        (void)cmd_path_down();
        return ESP_ERR_TIMEOUT;
    }
    if ( ESP_OK == err )
    {
        err = esp_lcd_panel_init(s_panel);
    }
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "panel init: %s", esp_err_to_name(err));
        return err;
    }
    link_watch();

    /* 4. On, and dark. Display-on goes over the DBI control IO like every
     * other command after the init table: the panel object IDF builds here
     * has no display-on/off of its own, and asking it prints an error line
     * on every boot. Brightness is what actually lights the pixels, so
     * neither result is fatal on its own. The brightness is 0, the table's
     * own value said once more after display-on, and it stays 0 until the
     * first frame reaches the glass (hold_release()). Both are DCS commands
     * through the host's command FIFO, so both take the bounded wait every
     * later brightness write takes - and neither is made inside a log line,
     * whose level must never decide what the panel shows. */
    if ( panel_command(DCS_DISPON) != KODE_OK )
    {
        KLOG_D(TAG, "display on: not sent");
    }
    kode_status_t const bright_st = brightness_write(0U);
    KLOG_D(TAG, "brightness: %s", (KODE_OK == bright_st) ? "ok" : "not written");

    err = link_events_on();
    if ( ESP_OK != err )
    {
        return err;
    }

    /* Raised before anyone may ask for a brightness, and full brightness
     * unless somebody does. */
    s_held     = true;
    s_held_pct = 100U;
    s_up_ms    = khal_time_ms();
    s_ready    = true;
    s_brought_up = true;
    link_frames_reset();

    KLOG_D(TAG, "CO5300 up: %ux%u RGB565, %d lane @%dMbps",
           (unsigned)KBSP_LCD_H_RES, (unsigned)KBSP_LCD_V_RES,
           DSI_LANE_NUM, DSI_LANE_BITRATE);
    return ESP_OK;
}

bool kdrv_display_flush_wait(uint32_t ms)
{
    return khal_sem_wait_timeout(s_flush_done, ms);
}

void kdrv_display_flush_abort(void)
{
    khal_sem_signal(s_flush_done);
}

kode_status_t kdrv_display_init(void)
{
    if ( s_brought_up )
    {
        return KODE_OK;   /* also while the link is down: see kdrv_display_link_set() */
    }
    if ( NULL == s_flush_done )
    {
        s_flush_done = khal_sem_create();
    }
    if ( NULL == s_flush_done )
    {
        /* Without this the panel would come up perfectly and the renderer
         * would wait forever for a completion nobody could deliver - one band
         * on screen, no error anywhere. Say so instead. */
        KLOG_E(TAG, "no flush semaphore; refusing to bring the panel up");
        return KODE_DENIED;
    }
    uint8_t percent = 0U;

    if ( panel_note_take(&percent) )
    {
        uint32_t const t0 = khal_time_ms();

        if ( panel_resume(percent) == ESP_OK )
        {
            KLOG_I(TAG, "the glass was left lit: up without the reset in %lu ms, "
                        "the last picture kept until our first frame",
                   (unsigned long)(khal_time_ms() - t0));
            if ( esp_register_shutdown_handler(panel_reset_on_restart) != ESP_OK )
            {
                KLOG_W(TAG, "no shutdown handler for the panel");
            }
            return KODE_OK;
        }
        KLOG_W(TAG, "the glass was left lit but would not resume; the bring-up "
                    "with the reset");
    }
    panel_power_on();
    if ( !panel_present() )
    {
        KLOG_W(TAG, "no panel assembly: no touch controller answered on the "
                    "glass's I2C (0x%02X) in %u ms; the DSI is left alone",
               (unsigned)KBSP_I2C_ADDR_TOUCH, (unsigned)PANEL_PROBE_BUDGET_MS);
        return KODE_ERROR;
    }
    esp_err_t up_err = panel_bring_up();
    for ( uint32_t attempt = 1U; (ESP_ERR_TIMEOUT == up_err) && (3U > attempt); ++attempt )
    {
        /* Pum pum: a long reset, the slow path, and again. */
        KLOG_W(TAG, "panel bring-up: attempt %lu of 3 after a long reset", (unsigned long)(attempt + 1U));
        s_rail_was_off = false;
        (void)khal_gpio_output(KBSP_PIN_DSP_TP_RST, false);
        khal_delay_ms(100U);
        (void)gpio_set_level((gpio_num_t)KBSP_PIN_DSP_TP_RST, 1U);
        khal_delay_ms(150U);
        (void)gpio_reset_pin((gpio_num_t)KBSP_PIN_DSP_TP_RST);
        up_err = panel_bring_up();
    }
    if ( ESP_OK != up_err )
    {
        return KODE_ERROR;
    }
    /* Once, and only for a panel that came up: every later call returns at
     * s_brought_up above, and a board with no glass never reaches this line. */
    esp_err_t const sd_err = esp_register_shutdown_handler(panel_reset_on_restart);
    if ( ESP_OK != sd_err )
    {
        KLOG_W(TAG, "no shutdown handler for the panel (%s): a restart leaves "
                    "the last frame on the glass until the bootloader resets it",
               esp_err_to_name(sd_err));
    }
    return KODE_OK;
}

void *kdrv_display_back_buffer(void)
{
    void *fb = NULL;

    if ( s_ready && (NULL != s_fb[0]) )
    {
        fb = s_fb[s_back];
    }
    return fb;
}

void *kdrv_display_front_buffer(void)
{
    void *fb = NULL;

    if ( s_ready && (NULL != s_fb[0]) )
    {
        fb = s_fb[s_back ^ 1U];
    }
    return fb;
}

static esp_err_t video_begin(uint8_t *shown);

/**
 * @brief Show a frame the caller drew straight into one of the framebuffers.
 *
 * Nothing is copied into the framebuffer: the caller has already drawn the
 * whole frame there, the rows it did not change included - LVGL in direct
 * mode copies them over from the other buffer itself. So only whole rows
 * qualify, sent alone, and never after chunks of a frame assembled the usual
 * way. y1..y2 are the rows the caller changed.
 *
 * Those rows are what the mirror is told changed, and those rows plus the
 * ones the last flip carried are written back from the cache before the
 * flip: the caller drew the first through the cache and copied the second
 * through it, and the engine reads PSRAM. The rest of the buffer is as the cache last left it.
 * Handing over only the band is what keeps a small animation from paying for
 * the whole glass every frame.
 *
 * Either buffer is accepted, and the one not shown becomes the back. A
 * renderer that keeps both (LVGL in direct mode) is normally drawing in the
 * back one; if something else flipped the panel in between - AO_Display's
 * own test row - it may hand over the front one, and following it is what
 * puts the two back in step.
 *
 * @param x1     Must be 0: only whole rows qualify.
 * @param y1     First row the caller changed.
 * @param x2     Must be KBSP_LCD_H_RES: only whole rows qualify.
 * @param y2     Last row the caller changed.
 * @param pixels The framebuffer the frame was drawn in.
 * @param last   Must be true: the frame comes in one piece.
 * @return KODE_OK once the flip is handed to the engine, KODE_ERROR for a
 *         frame that is not whole rows alone, or a flip that failed.
 */
static kode_status_t flush_in_place(int32_t x1, int32_t y1, int32_t x2,
                                    int32_t y2, const void *pixels, bool last)
{
    uint8_t   shown = s_back;
    esp_err_t err;

    if ( (const void *)s_fb[s_back] != pixels )
    {
        shown = (uint8_t)(s_back ^ 1U);
    }
    uint8_t * const frame = s_fb[shown];
    size_t const    row   = (size_t)KBSP_LCD_H_RES * 2U;

    if ( (0 != x1) || ((int32_t)KBSP_LCD_H_RES != x2) || !last
        || s_frame_open )
    {
        KLOG_W(TAG, "a frame drawn in place must be whole rows, alone");
        return KODE_ERROR;
    }

    int32_t sync_y1 = y1;
    int32_t sync_y2 = y2;

    if ( s_prev_y1 < sync_y1 )
    {
        sync_y1 = s_prev_y1;
    }
    if ( s_prev_y2 > sync_y2 )
    {
        sync_y2 = s_prev_y2;
    }
    s_flipping = true;
    err = esp_lcd_panel_draw_bitmap(s_panel, 0, (int)sync_y1,
                                    (int)KBSP_LCD_H_RES, (int)sync_y2,
                                    &frame[(size_t)sync_y1 * row]);
    s_flipping = false;
    if ( (ESP_OK == err) && s_video_owed )
    {
        err = video_begin(frame);
    }
    if ( ESP_OK != err )
    {
        return KODE_ERROR;
    }
    /* The two buffers now differ exactly where this frame painted. */
    s_prev_y1      = y1;
    s_prev_y2      = y2;
    s_back         = (uint8_t)(shown ^ 1U);
    s_flip_pending = true;
#if CONFIG_KODEOS_SCREENSHOT
    shown_rows(y1, y2);
#endif
    hold_release();
    return KODE_OK;
}

kode_status_t kdrv_display_flush(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                 const void *pixels, bool last)
{
    if ( !s_ready )
    {
        return KODE_DENIED;
    }
    if ( s_keep_lit )
    {
        /* The glass is being kept for the restart: the renderer is released and
         * the panel keeps what it shows. */
        khal_sem_signal(s_flush_done);
        return KODE_OK;
    }

    /* One check for every destination this rectangle reaches. "A rectangle
     * off the panel is the panel driver's business" is true of the direct
     * path and not of the double-buffered one, where the chunk is memcpy'd
     * into a 400 KB DMA-mapped framebuffer *before* the panel driver ever
     * sees the coordinates.
     *
     * Returning an error here is safe rather than a hang: AO_Display releases
     * the renderer by hand on a failed flush, precisely so one bad rectangle
     * cannot stop the GUI. */
    if ( (0 > x1) || (0 > y1) || (x2 <= x1) || (y2 <= y1)
        || ((int32_t)KBSP_LCD_H_RES < x2) || ((int32_t)KBSP_LCD_V_RES < y2)
        || (NULL == pixels) )
    {
        KLOG_W(TAG, "flush rectangle %ld,%ld..%ld,%ld is outside the panel",
               (long)x1, (long)y1, (long)x2, (long)y2);
        return KODE_ERROR;
    }

    if ( NULL == s_fb[0] )
    {
        /* Single-framebuffer fallback: draw straight at the scan. A frame is
         * whole here once its last chunk has drawn, and that is the moment
         * the first one lights the panel. */
        esp_err_t const err = esp_lcd_panel_draw_bitmap(s_panel, x1, y1,
                                                       x2, y2, pixels);
        if ( ESP_OK != err )
        {
            return KODE_ERROR;
        }
#if CONFIG_KODEOS_SCREENSHOT
        shown_rows(y1, y2);
#endif
        if ( last )
        {
            hold_release();
        }
        return KODE_OK;
    }

    uint8_t *back  = s_fb[s_back];
    uint8_t *front = s_fb[s_back ^ 1U];

    if ( ((const void *)back == pixels) || ((const void *)front == pixels) )
    {
        return flush_in_place(x1, y1, x2, y2, pixels, last);
    }

    /* First chunk since the flip: settle the back buffer's debt. It last
     * showed the frame before the one on the glass, so the rows the last
     * flip changed are stale here - copy exactly those from the front. Skip
     * it and yesterday's pixels bleed through wherever this frame does not
     * paint. */
    if ( !s_frame_open )
    {
        if ( s_prev_y2 > s_prev_y1 )
        {
            (void)memcpy(&back[(size_t)s_prev_y1 * KBSP_LCD_H_RES * 2U],
                         &front[(size_t)s_prev_y1 * KBSP_LCD_H_RES * 2U],
                         (size_t)(s_prev_y2 - s_prev_y1)
                             * KBSP_LCD_H_RES * 2U);
        }
        s_frame_open = true;
    }

    /* The chunk itself, row by row: the source is packed to the chunk's
     * width, the destination is strided to the panel's. */
    {
        const uint8_t *src = pixels;
        int32_t const  w   = x2 - x1;
        for ( int32_t y = y1; y < y2; ++y )
        {
            (void)memcpy(&back[((size_t)y * KBSP_LCD_H_RES + (size_t)x1) * 2U],
                         &src[(size_t)(y - y1) * (size_t)w * 2U],
                         (size_t)w * 2U);
        }
    }
    if ( y1 < s_dirty_y1 ) { s_dirty_y1 = y1; }
    if ( y2 > s_dirty_y2 ) { s_dirty_y2 = y2; }

    if ( !last )
    {
        /* The chunk lives in the back buffer now; the renderer may reuse
         * its buffer immediately. Same signal the DMA callback gives, from
         * task context - khal_sem_signal serves both. */
        khal_sem_signal(s_flush_done);
        return KODE_OK;
    }

    /* The flip. Handing draw_bitmap a pointer INSIDE a framebuffer makes
     * the driver write back the cache and retarget the scan-out to that
     * framebuffer at the end of the frame in progress - no copy. Its
     * completion callback fires synchronously on this path, before the move,
     * so the renderer is released by on_frame_done() instead, once the
     * engine is scanning the new front and the old one is free to write. */
    esp_err_t err = ESP_OK;
    if ( s_dirty_y2 > s_dirty_y1 )
    {
        /* The writeback covers the debt rows as well as this frame's:
         * draw_bitmap's no-copy path writes back ONLY y_start..y_end, but
         * the flip retargets the scan to the WHOLE buffer - a debt row
         * outside the band would sit in cache while the DMA reads
         * two-frames-old PSRAM under it. The rows are already copied, they
         * only have to be covered.
         *
         * The NEXT debt is this frame's own rows and nothing more. After
         * the flip the two buffers differ exactly where this frame painted -
         * the debt copied at its start made them equal everywhere else - so
         * handing the union on as well would be a debt that only ever
         * grows: from the first full frame onward, every flip copied and
         * wrote back the whole 400 KB frame whatever had changed, 17 ms of
         * copy alone. */
        int32_t sync_y1 = s_dirty_y1;
        int32_t sync_y2 = s_dirty_y2;

        if ( s_prev_y1 < sync_y1 )
        {
            sync_y1 = s_prev_y1;
        }
        if ( s_prev_y2 > sync_y2 )
        {
            sync_y2 = s_prev_y2;
        }
        s_flipping = true;
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, sync_y1,
                                        KBSP_LCD_H_RES, sync_y2,
                                        &back[(size_t)sync_y1
                                              * KBSP_LCD_H_RES * 2U]);
        s_flipping = false;
        if ( (ESP_OK == err) && s_video_owed )
        {
            err = video_begin(back);
        }
        if ( ESP_OK == err )
        {
            s_prev_y1 = s_dirty_y1;
            s_prev_y2 = s_dirty_y2;
            s_back    = (uint8_t)(s_back ^ 1U);
            s_flip_pending = true;
#if CONFIG_KODEOS_SCREENSHOT
            shown_rows(s_dirty_y1, s_dirty_y2);
#endif
            hold_release();
        }
    }
    else
    {
        /* Empty frame: nothing was shown, so nothing flips. Advancing
         * s_back here would aim the next frame's assembly at the buffer
         * the engine is scanning, with its debt copy skipped - the exact
         * two failure modes this file exists to prevent. */
        khal_sem_signal(s_flush_done);
    }
    s_dirty_y1   = KBSP_LCD_V_RES;
    s_dirty_y2   = 0;
    s_frame_open = false;
    return (ESP_OK == err) ? KODE_OK : KODE_ERROR;
}

/**
 * @brief Wait, boundedly, for room in the DSI host's generic command FIFO.
 *
 * The flag IDF's own write path spins on, read here first so that a host
 * whose drain has stalled costs this call @p wait_ms and an error, not the
 * caller's thread for the life of the boot - which is what the unbounded
 * `while (gen_cmd_full);` in mipi_dsi_hal.c would cost it.
 */
static bool cmd_fifo_room(uint32_t wait_ms)
{
    for ( uint32_t waited = 0U; mipi_dsi_host_ll_gen_is_cmd_fifo_full(&MIPI_DSI_HOST);
         ++waited )
    {
        if ( waited >= wait_ms )
        {
            return false;
        }
        khal_delay_ms(1U);
    }
    return true;
}

/**
 * @brief One brightness write, paced and behind the FIFO's bounded wait.
 *
 * The one place a 0x51 reaches the HAL after the init table: bring-up's
 * write of 0, the first frame's (hold_release()) and every
 * kdrv_display_brightness() after it, so the guard cannot be bypassed by the
 * path nobody thinks of as "a brightness write".
 */
static kode_status_t brightness_write(uint8_t percent)
{
    static bool     s_sent;
    static uint32_t s_sent_ms;

    /* Paced rather than dropped: a ramp's last value is the one that has to
     * land, so a write that comes too soon waits out the rest of the gap. */
    if ( s_sent )
    {
        uint32_t const since = khal_time_ms() - s_sent_ms;
        if ( BRIGHT_GAP_MS > since )
        {
            khal_delay_ms(BRIGHT_GAP_MS - since);
        }
    }
    if ( !cmd_fifo_room(BRIGHT_FIFO_WAIT_MS) )
    {
        KLOG_W(TAG, "DSI command FIFO still full after %u ms: brightness %u%% "
                    "not written", (unsigned)BRIGHT_FIFO_WAIT_MS, (unsigned)percent);
        return KODE_TIMEOUT;
    }
    /* The panel's register is 0..255; the vendor driver's own scaling, said
     * here because the panel object it hangs off is not always there: a link
     * that came back (kdrv_display_link_set()) has a plain video panel and
     * the control IO alone. */
    uint8_t const hw = (uint8_t)(((uint32_t)percent * BRIGHT_HW_MAX) / BRIGHT_PCT_MAX);
    esp_err_t const err = esp_lcd_panel_io_tx_param(s_io, (int)DCS_BRIGHTNESS, &hw, 1U);
    s_sent    = true;
    s_sent_ms = khal_time_ms();
    return (ESP_OK == err) ? KODE_OK : KODE_ERROR;
}

/**
 * @brief Light the panel for the first frame: the end of the hold.
 *
 * Called by every path that puts a whole frame on its way to the glass - the
 * flip, a frame drawn in place, the last chunk on a single framebuffer -
 * once the frame is handed over, and a no-op from the second frame on. With
 * two framebuffers lighting here shows nothing early: a flip moves the scan
 * at the end of the frame in progress, and until then the engine scans the
 * black framebuffer it started on, which is black at any brightness. On a
 * single one the frame has been going into the scanned buffer all along and
 * is whole once its last chunk lands.
 *
 * The value goes through brightness_write(), paced and bounded like every
 * other, once per boot, on the panel owner's thread. The hold ends whatever
 * the write answers: a value that could not be written is reported, and the
 * next kdrv_display_brightness() writes straight through, as every one after
 * the first frame does.
 */
static void hold_release(void)
{
    if ( s_held )
    {
        s_held = false;
        if ( brightness_write(s_held_pct) == KODE_OK )
        {
            KLOG_D(TAG, "panel lit at the first frame, %lu ms after bring-up, %u %%",
                   (unsigned long)(khal_time_ms() - s_up_ms),
                   (unsigned)s_held_pct);
        }
        else
        {
            KLOG_W(TAG, "the first frame is on its way but its brightness "
                        "(%u %%) was not written; the panel stays dark until "
                        "the next brightness write", (unsigned)s_held_pct);
        }
    }
}

kode_status_t kdrv_display_brightness(uint8_t percent)
{
    if ( !s_ready )
    {
        return KODE_DENIED;
    }
    if ( BRIGHT_PCT_MAX < percent )
    {
        percent = (uint8_t)BRIGHT_PCT_MAX;
    }
    if ( s_keep_lit )
    {
        return KODE_OK;   /* the note has said the level: the glass stays at it */
    }
    s_last_pct = percent;
    if ( s_held )
    {
        /* Before the first frame: kept, not written. The panel stays black
         * until there is a picture to show, then lights once, at the last
         * value asked for (hold_release()). */
        s_held_pct = percent;
        return KODE_OK;
    }
    return brightness_write(percent);
}

/* ---- The link and the panel's memory --------------------------------------
 * The CO5300 keeps the picture in a memory of its own and shows it without
 * anyone driving it, so the DSI link - the video scan-out, the bus with the
 * PM lock that keeps the P4 awake, the DPHY rail - can go while the glass
 * stays lit, and a small change (the minute) can be written into that memory
 * alone. Three operations, none of which sends the init table or pulses the
 * reset line: kdrv_display_link_set(), kdrv_display_panel_sleep() and
 * kdrv_display_gram_write(). AO_Display's thread only, and only while the
 * renderer is parked. */

/**
 * @brief Take the video and the command path down, the panel left as it is.
 *
 * The video panel goes first, with the two framebuffers it owns, then the
 * control IO, the bus and the DPHY rail. The CO5300 object of the first
 * bring-up gives GPIO7 back to nobody as it goes (the reset line the panel
 * shares with the touch controller), so the line is driven high again, which
 * is where it already was: the panel's reset is never asserted, and the
 * shutdown handler still has an output to pull low.
 *
 * @return KODE_OK; KODE_DENIED before the bring-up or with a frame open,
 *         KODE_TIMEOUT when a flip never completed (nothing was taken down),
 *         KODE_ERROR when IDF refused part of the teardown (the link is down
 *         for this file all the same: what failed to free is leaked).
 */
static kode_status_t link_down(void)
{
    if ( !s_brought_up )
    {
        return KODE_DENIED;
    }
    if ( !s_ready )
    {
        return KODE_OK;
    }
    if ( s_frame_open )
    {
        KLOG_W(TAG, "link down refused: a frame is open");
        return KODE_DENIED;
    }
    for ( uint32_t waited = 0U; s_flip_pending; ++waited )
    {
        if ( LINK_FLIP_WAIT_MS <= waited )
        {
            KLOG_W(TAG, "link down refused: a flip never completed");
            return KODE_TIMEOUT;
        }
        khal_delay_ms(1U);
    }
    /* The video ends in the vertical blanking (on_vsync()). A board that sees
     * no vsync in time goes on regardless: a dark line is less harm than a
     * glass that never rests. */
    s_video_stopped  = false;
    s_video_stop_ask = true;
    for ( uint32_t waited = 0U; !s_video_stopped; ++waited )
    {
        if ( LINK_STOP_WAIT_MS <= waited )
        {
            s_video_stop_ask = false;
            KLOG_W(TAG, "link down: no vsync in %u ms; the video stops where it is",
                   (unsigned)LINK_STOP_WAIT_MS);
            break;
        }
        khal_delay_ms(1U);
    }

    KLOG_D(TAG, "link down: framebuffers 0x%08lx and 0x%08lx freed",
           (unsigned long)(uintptr_t)s_fb[0], (unsigned long)(uintptr_t)s_fb[1]);
    s_ready = false;
    s_fb[0] = NULL;
    s_fb[1] = NULL;
    esp_err_t const panel_err = esp_lcd_panel_del(s_panel);
    s_panel = NULL;
    esp_err_t const path_err = cmd_path_down();
    reset_line_reclaim();
    if ( (ESP_OK != panel_err) || (ESP_OK != path_err) )
    {
        KLOG_W(TAG, "link down: video panel %s, command path %s",
               esp_err_to_name(panel_err), esp_err_to_name(path_err));
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Send the full CASET and RASET on the control IO, behind the FIFO's
 *        bounded wait.
 *
 * Any DCS command ends a memory write still open, and the full window is the
 * one video mode streams the frame through (the init table's, cols 22..431,
 * rows 0..501).
 *
 * @return KODE_OK; KODE_TIMEOUT when the FIFO had no room; KODE_ERROR when
 *         IDF refused a packet.
 */
static kode_status_t window_whole(void)
{
    static const uint8_t cols[GRAM_WINDOW_BYTES] = {
        (uint8_t)(PANEL_COL_OFFSET >> GRAM_BYTE_BITS),
        (uint8_t)(PANEL_COL_OFFSET & GRAM_BYTE_MASK),
        (uint8_t)((PANEL_COL_OFFSET + KBSP_LCD_H_RES - 1U) >> GRAM_BYTE_BITS),
        (uint8_t)((PANEL_COL_OFFSET + KBSP_LCD_H_RES - 1U) & GRAM_BYTE_MASK)};
    static const uint8_t rows[GRAM_WINDOW_BYTES] = {
        0U, 0U,
        (uint8_t)((KBSP_LCD_V_RES - 1U) >> GRAM_BYTE_BITS),
        (uint8_t)((KBSP_LCD_V_RES - 1U) & GRAM_BYTE_MASK)};

    if ( !cmd_fifo_room(BRIGHT_FIFO_WAIT_MS) )
    {
        return KODE_TIMEOUT;
    }
    if ( esp_lcd_panel_io_tx_param(s_io, (int)DCS_CASET, cols, sizeof(cols)) != ESP_OK )
    {
        return KODE_ERROR;
    }
    if ( !cmd_fifo_room(BRIGHT_FIFO_WAIT_MS) )
    {
        return KODE_TIMEOUT;
    }
    if ( esp_lcd_panel_io_tx_param(s_io, (int)DCS_RASET, rows, sizeof(rows)) != ESP_OK )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

/**
 * @brief Bring the video and the command path back, with no reset and no init.
 *
 * The same rail, bus and control IO as the bring-up, then a plain video panel
 * of the same timing - not the CO5300 object, whose init would read the ID and
 * send the table - started with the generic init, which only turns the scan-out
 * on. The panel is told nothing but a brightness of 0.
 *
 * The framebuffers are new: black, and possibly at other addresses than
 * before. The link comes back in the state the bring-up leaves, dark until the
 * first frame, and that first frame lights the panel at the last level asked
 * for (hold_release()).
 *
 * @return KODE_OK; KODE_DENIED before the bring-up; KODE_ERROR when a part
 *         refused (the link is left down, nothing standing).
 */
static kode_status_t link_up(void)
{
    if ( !s_brought_up )
    {
        return KODE_DENIED;
    }
    if ( s_ready )
    {
        return KODE_OK;
    }

    esp_lcd_dpi_panel_config_t dpi_cfg;
    dpi_config_fill(&dpi_cfg);
    if ( cmd_path_up() != ESP_OK )
    {
        return KODE_ERROR;
    }
    /* Before the scan-out starts: a panel the last GRAM write left in the
     * middle of a RAMWR, or on a small window, would fold the video into it
     * and show noise. */
    if ( window_whole() != KODE_OK )
    {
        KLOG_W(TAG, "link up: the full window was not written");
    }
    esp_err_t err = esp_lcd_new_panel_dpi(s_bus, &dpi_cfg, &s_panel);
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "dpi panel: %s", esp_err_to_name(err));
        s_panel = NULL;
    }
    else
    {
        err = esp_lcd_panel_init(s_panel);
        if ( ESP_OK != err )
        {
            KLOG_E(TAG, "dpi init: %s", esp_err_to_name(err));
        }
    }
    if ( ESP_OK == err )
    {
        link_watch();
        err = link_events_on();
    }
    if ( ESP_OK != err )
    {
        if ( NULL != s_panel )
        {
            (void)esp_lcd_panel_del(s_panel);
            s_panel = NULL;
        }
        (void)cmd_path_down();
        reset_line_reclaim();
        return KODE_ERROR;
    }

    link_frames_reset();
    s_fb_epoch = s_fb_epoch + 1U;
    s_ready = true;
    /* The scan-out is black until the renderer's first frame, so the panel
     * goes dark with it and lights at that frame, as after the bring-up. */
    kode_status_t const dark = brightness_write(0U);
    s_held     = true;
    s_held_pct = s_last_pct;
    s_up_ms    = khal_time_ms();
#if CONFIG_KODEOS_SCREENSHOT
    shown_rows(0, (int32_t)KBSP_LCD_V_RES);
#endif
    KLOG_D(TAG, "link up: framebuffers 0x%08lx and 0x%08lx, epoch %lu",
           (unsigned long)(uintptr_t)s_fb[0], (unsigned long)(uintptr_t)s_fb[1],
           (unsigned long)s_fb_epoch);
    if ( KODE_OK == dark )
    {
        KLOG_D(TAG, "link up: video scanning black, brightness 0");
    }
    else
    {
        KLOG_W(TAG, "link up: video scanning black, brightness 0 not written");
    }
    return KODE_OK;
}

/* ---- The glass kept lit across a restart ------------------------------------
 * In video mode the CO5300 writes what it receives into its memory and shows
 * that memory by itself (C2h = 0, via RAM), so a panel the video has stopped
 * reaching goes on showing the last whole frame. Three parts: the leaving
 * firmware stops the video in a blanking and latches the reset line
 * (kdrv_display_keep_for_restart()); the next one finds the note and takes the
 * panel up with no reset and no init (panel_resume()), asking it first whether
 * it is awake; and its scan-out waits for its first whole frame, because video
 * started on an empty framebuffer would write black into the memory the glass
 * is showing (video_begin()). */

/**
 * @brief Read one DCS byte, every wait bounded.
 *
 * IDF's read waits with no bound on a FIFO only an answering panel fills. Before
 * the video starts, with the host in command mode.
 *
 * @param cmd The command.
 * @param val Receives the first byte.
 * @return KODE_OK; KODE_TIMEOUT when the panel did not answer.
 */
static kode_status_t panel_read1(uint8_t cmd, uint8_t *val)
{
    if ( !gram_wait(mipi_dsi_host_ll_gen_is_cmd_fifo_full, false) )
    {
        return KODE_TIMEOUT;
    }
    mipi_dsi_host_ll_gen_set_packet_header(&MIPI_DSI_HOST, 0U,
                                           MIPI_DSI_DT_SET_MAXIMUM_RETURN_PKT, 0U, 1U);
    mipi_dsi_host_ll_enable_bta(&MIPI_DSI_HOST, true);
    mipi_dsi_host_ll_gen_set_rx_vcid(&MIPI_DSI_HOST, 0U);
    if ( !gram_wait(mipi_dsi_host_ll_gen_is_cmd_fifo_full, false) )
    {
        return KODE_TIMEOUT;
    }
    mipi_dsi_host_ll_gen_set_packet_header(&MIPI_DSI_HOST, 0U, MIPI_DSI_DT_DCS_READ_0, 0U, cmd);
    if ( !gram_wait(mipi_dsi_host_ll_gen_is_read_cmd_busy, false)
        || !gram_wait(mipi_dsi_host_ll_gen_is_read_fifo_empty, false) )
    {
        return KODE_TIMEOUT;
    }
    uint32_t const w = mipi_dsi_host_ll_gen_read_payload_fifo(&MIPI_DSI_HOST);

    while ( !mipi_dsi_host_ll_gen_is_read_fifo_empty(&MIPI_DSI_HOST) )
    {
        (void)mipi_dsi_host_ll_gen_read_payload_fifo(&MIPI_DSI_HOST);
    }
    *val = (uint8_t)(w & GRAM_BYTE_MASK);
    return KODE_OK;
}

/**
 * @brief Whether the panel says it is out of sleep with its display on.
 *
 * @return true when it answered so. The first read after a link comes up can be
 *         lost, hence the tries.
 */
static bool panel_awake(void)
{
    uint8_t       pm = 0U;
    kode_status_t rd = KODE_TIMEOUT;

    for ( uint32_t i = 0U; (PANEL_READ_TRIES > i) && (KODE_OK != rd); ++i )
    {
        if ( 0U < i )
        {
            khal_delay_ms(PANEL_READ_GAP_MS);
        }
        rd = panel_read1(RDDPM_CMD, &pm);
    }
    if ( KODE_OK != rd )
    {
        KLOG_W(TAG, "the panel did not answer 0Ah");
        return false;
    }
    if ( (pm & (RDDPM_SLEEP_OUT | RDDPM_DISPLAY_ON)) != (RDDPM_SLEEP_OUT | RDDPM_DISPLAY_ON) )
    {
        KLOG_W(TAG, "the panel is not awake on its memory (0Ah 0x%02X)", (unsigned)pm);
        return false;
    }
    return true;
}

/**
 * @brief Take up a panel the last firmware left lit: no reset, no init table.
 *
 * The rail, the bus and the control IO; the panel asked whether it is awake; the
 * window it streams through; a plain video panel with its framebuffers, and its
 * scan-out left for the first frame. The panel keeps its brightness: nothing
 * here writes one. Failing, it leaves nothing standing and the line in the state
 * the bring-up expects.
 *
 * @param percent The level the note says the glass is at.
 * @return ESP_OK when the first flush will start the video.
 */
static esp_err_t panel_resume(uint8_t percent)
{
    esp_lcd_dpi_panel_config_t dpi_cfg;
    esp_err_t err = cmd_path_up();

    dpi_config_fill(&dpi_cfg);
    if ( (ESP_OK == err) && !panel_awake() )
    {
        err = ESP_ERR_INVALID_STATE;
    }
    if ( ESP_OK == err )
    {
        if ( window_whole() != KODE_OK )
        {
            KLOG_W(TAG, "resume: the full window was not written");
        }
        err = esp_lcd_new_panel_dpi(s_bus, &dpi_cfg, &s_panel);
    }
    if ( ESP_OK == err )
    {
        /* Reset by the restart, back to its blue: see panel_bring_up(). Latched
         * by the update that starts the scan-out in video_begin(). */
        MIPI_DSI_BRIDGE.dpi_rsv_dpi_data.dpi_rsv_data = DSI_UNDERFLOW_BLACK;
        err = link_events_on();
    }
    if ( ESP_OK != err )
    {
        if ( NULL != s_panel )
        {
            (void)esp_lcd_panel_del(s_panel);
            s_panel = NULL;
        }
        (void)cmd_path_down();
        return err;
    }
    link_frames_reset();
    /* Nothing is scanned until the first frame, which goes to fb 0 itself: the
     * engine starts there, always. */
    s_back       = 0U;
    s_video_owed = true;
    s_held       = false;
    s_held_pct   = percent;
    s_last_pct   = percent;
    s_up_ms      = khal_time_ms();
    s_ready      = true;
    s_brought_up = true;
    return ESP_OK;
}

/**
 * @brief Start the scan-out a resume left waiting, on the first whole frame.
 *
 * Called by the flush paths once the frame has been handed to esp_lcd.
 * esp_lcd_panel_init() starts the DPI on fb 0, always, so the frame has to be
 * there by then, written back; a frame anywhere else is copied there first and
 * the scan is pointed back at it, which lands at the end of the first refresh
 * and shows the same picture.
 *
 * The PSRAM keeps its contents across a restart, and the scan-out reads PSRAM
 * and not the cache: what esp_lcd zeroed is still in the cache, over the last
 * boot's frames. Both buffers go to PSRAM whole before the video starts, or the
 * glass shows them wherever this frame did not paint.
 *
 * @param shown The framebuffer the frame is in.
 * @return ESP_OK when the video runs.
 */
static esp_err_t video_begin(uint8_t *shown)
{
    size_t const bytes = (((size_t)KBSP_LCD_H_RES * (size_t)KBSP_LCD_V_RES * 2U) + 63U)
                         & ~(size_t)63U;
    bool const   other = (shown != s_fb[0]);
    esp_err_t    err   = ESP_OK;

    s_video_owed = false;
    s_flipping   = true;   /* the writebacks' completions are not the renderer's */
    if ( other )
    {
        (void)memcpy(s_fb[0], shown, bytes);
    }
    for ( uint8_t i = 0U; 2U > i; ++i )
    {
        (void)esp_cache_msync(s_fb[i], bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
    }
    if ( other )
    {
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, (int)KBSP_LCD_H_RES,
                                        (int)KBSP_LCD_V_RES, s_fb[0]);
    }
    if ( ESP_OK == err )
    {
        err = esp_lcd_panel_init(s_panel);
    }
    if ( ESP_OK == err )
    {
        link_watch();
        (void)MIPI_DSI_HOST.int_st0.val;
        (void)MIPI_DSI_HOST.int_st1.val;
    }
    if ( (ESP_OK == err) && other )
    {
        err = esp_lcd_panel_draw_bitmap(s_panel, 0, 0, (int)KBSP_LCD_H_RES,
                                        (int)KBSP_LCD_V_RES, shown);
    }
    s_flipping = false;
    if ( ESP_OK != err )
    {
        KLOG_E(TAG, "resume: the scan-out would not start (%s)", esp_err_to_name(err));
    }
    else
    {
        KLOG_I(TAG, "resume: video from the first frame, %lu ms after the panel was up",
               (unsigned long)(khal_time_ms() - s_up_ms));
    }
    return err;
}

#define RECOVER_REG    LP_SYSTEM_REG_LP_STORE12_REG
#define RECOVER_MAGIC  0x4B520000UL   /* "KR" over the count */
#define RECOVER_MASK   0xFFFF0000UL

uint32_t kdrv_display_recoveries(void)
{
    uint32_t const v = REG_READ(RECOVER_REG);

    return ((v & RECOVER_MASK) == RECOVER_MAGIC) ? (v & ~RECOVER_MASK) : 0U;
}

void kdrv_display_recoveries_set(uint32_t n)
{
    REG_WRITE(RECOVER_REG, RECOVER_MAGIC | (n & ~RECOVER_MASK));
}

bool kdrv_display_video_expected(void)
{
    return s_ready && !s_video_owed && !s_held && !s_asleep && !s_keep_lit;
}

kode_status_t kdrv_display_keep_for_restart(void)
{
    if ( s_keep_lit )
    {
        return KODE_OK;
    }
    if ( !s_ready || s_held || s_asleep || s_video_owed )
    {
        return KODE_DENIED;   /* before its first frame the memory is not ours */
    }
    /* The hold first: it is the part that can fail, and undoes with one call. */
    (void)khal_gpio_write(KBSP_PIN_DSP_TP_RST, true);
    if ( gpio_hold_en((gpio_num_t)KBSP_PIN_DSP_TP_RST) != ESP_OK )
    {
        KLOG_W(TAG, "the reset line would not hold; the restart resets the panel");
        return KODE_ERROR;
    }
    /* No more flips from here: what is on its way lands, and nothing follows. */
    s_keep_lit = true;
    for ( uint32_t waited = 0U; s_flip_pending && (LINK_FLIP_WAIT_MS > waited); ++waited )
    {
        khal_delay_ms(1U);
    }
    s_video_stopped  = false;
    s_video_stop_ask = true;
    for ( uint32_t waited = 0U; !s_video_stopped && (LINK_STOP_WAIT_MS > waited); ++waited )
    {
        khal_delay_ms(1U);
    }
    if ( !s_video_stopped )
    {
        /* A video cut where it stands leaves a dark line: the reset is better. */
        s_video_stop_ask = false;
        s_keep_lit       = false;
        (void)gpio_hold_dis((gpio_num_t)KBSP_PIN_DSP_TP_RST);
        KLOG_W(TAG, "no vsync to stop in; the restart resets the panel");
        return KODE_TIMEOUT;
    }
    REG_WRITE(PANEL_NOTE_REG, (PANEL_NOTE_MAGIC << PANEL_NOTE_SHIFT) | (uint32_t)s_last_pct);
    return KODE_OK;
}

uint32_t kdrv_display_buffers_epoch(void)
{
    return s_fb_epoch;
}

kode_status_t kdrv_display_link_set(bool up)
{
    if ( up )
    {
        return link_up();
    }
    return link_down();
}

kode_status_t kdrv_display_reset_low(void)
{
    kode_status_t st;

    /* With the link up the video panel's object has the line: it is left alone. */
    if ( !s_brought_up || s_ready )
    {
        return KODE_DENIED;
    }
    st = khal_gpio_output(KBSP_PIN_DSP_TP_RST, false);
    if ( KODE_OK == st )
    {
        khal_delay_ms(PANEL_OFF_RESET_LOW_MS);
    }
    return st;
}

/** @brief One parameterless DCS command, behind the FIFO's bounded wait. */
static kode_status_t panel_command(uint8_t cmd)
{
    if ( !cmd_fifo_room(BRIGHT_FIFO_WAIT_MS) )
    {
        return KODE_TIMEOUT;
    }
    esp_err_t const err = esp_lcd_panel_io_tx_param(s_io, (int)cmd, NULL, 0U);
    if ( ESP_OK != err )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
}

/** @brief Sleep in: dark, display off, SLPIN, and the settling time. */
static kode_status_t panel_sleep_in(void)
{
    /* Dark first, so DISPOFF never shows the last picture at full light. */
    kode_status_t st = brightness_write(0U);

    if ( KODE_OK == st )
    {
        khal_delay_ms(PANEL_CMD_GAP_MS);
        st = panel_command(DCS_DISPOFF);
    }
    if ( KODE_OK == st )
    {
        khal_delay_ms(PANEL_CMD_GAP_MS);
        st = panel_command(DCS_SLPIN);
    }
    if ( KODE_OK == st )
    {
        khal_delay_ms(PANEL_SLEEP_SETTLE_MS);
    }
    return st;
}

/** @brief Sleep out: SLPOUT, the settling time, display on. */
static kode_status_t panel_sleep_out(void)
{
    kode_status_t st = panel_command(DCS_SLPOUT);

    if ( KODE_OK == st )
    {
        khal_delay_ms(PANEL_SLEEP_SETTLE_MS);
        st = panel_command(DCS_DISPON);
    }
    return st;
}

kode_status_t kdrv_display_panel_sleep(bool sleep)
{
    if ( !s_ready )
    {
        return KODE_DENIED;
    }
    if ( sleep == s_asleep )
    {
        return KODE_OK;
    }

    kode_status_t st;
    if ( sleep )
    {
        st = panel_sleep_in();
    }
    else
    {
        st = panel_sleep_out();
    }
    if ( KODE_OK == st )
    {
        s_asleep = sleep;
        if ( sleep )
        {
            KLOG_N(TAG, "panel asleep");
        }
        else
        {
            KLOG_N(TAG, "panel awake");
        }
    }
    else
    {
        KLOG_W(TAG, "panel sleep(%u) not completed", (unsigned)sleep);
    }
    return st;
}

/**
 * @brief Wait until @p flag(host) reads @p want, at most GRAM_WAIT_US.
 *
 * A wait that runs out reads the flag once more before it says so: the clock
 * can have run on while this task did not (a flash erase in another task
 * stops the cache on both cores for milliseconds), and the host drains its
 * FIFOs by itself in that time. A stalled task is not a stuck bus.
 */
static bool gram_wait(bool (*flag)(dsi_host_dev_t *), bool want)
{
    uint64_t const t0 = khal_time_us();

    for ( ;; )
    {
        if ( flag(&MIPI_DSI_HOST) == want )
        {
            return true;
        }
        if ( (khal_time_us() - t0) > (uint64_t)GRAM_WAIT_US )
        {
            return flag(&MIPI_DSI_HOST) == want;
        }
    }
}

/**
 * @brief Queue one DCS long write: the payload words, then the header, every
 *        wait bounded.
 *
 * @param pkt   The command byte, then its parameters, padded to whole words.
 * @param total Bytes of @p pkt that are the packet (command plus parameters).
 * @return KODE_OK; KODE_TIMEOUT when a FIFO did not make room.
 */
static kode_status_t gram_send(const uint8_t *pkt, size_t total)
{
    for ( size_t at = 0U; at < total; at += GRAM_WORD_BYTES )
    {
        uint32_t word;

        (void)memcpy(&word, &pkt[at], sizeof(word));
        if ( !gram_wait(mipi_dsi_host_ll_gen_is_write_fifo_full, false) )
        {
            return KODE_TIMEOUT;
        }
        mipi_dsi_host_ll_gen_write_payload_fifo(&MIPI_DSI_HOST, word);
    }
    /* The header goes last: it is what makes the host send the packet. */
    if ( !gram_wait(mipi_dsi_host_ll_gen_is_cmd_fifo_full, false) )
    {
        return KODE_TIMEOUT;
    }
    mipi_dsi_host_ll_gen_set_packet_header(
        &MIPI_DSI_HOST, 0U, MIPI_DSI_DT_DCS_LONG_WRITE,
        (uint8_t)((total >> GRAM_BYTE_BITS) & GRAM_BYTE_MASK),
        (uint8_t)(total & GRAM_BYTE_MASK));
    return KODE_OK;
}

/** @brief Queue a CASET or RASET for [@p first, @p last], both inclusive. */
static kode_status_t gram_window(uint8_t cmd, uint32_t first, uint32_t last)
{
    uint8_t pkt[GRAM_WORD_BYTES * 2U] = {0};

    pkt[0] = cmd;
    pkt[1] = (uint8_t)((first >> GRAM_BYTE_BITS) & GRAM_BYTE_MASK);
    pkt[2] = (uint8_t)(first & GRAM_BYTE_MASK);
    pkt[3] = (uint8_t)((last >> GRAM_BYTE_BITS) & GRAM_BYTE_MASK);
    pkt[4] = (uint8_t)(last & GRAM_BYTE_MASK);
    return gram_send(pkt, 1U + GRAM_WINDOW_BYTES);
}

/** @brief Every packet queued has left the host. */
static bool gram_drain(void)
{
    return gram_wait(mipi_dsi_host_ll_gen_is_cmd_fifo_empty, true)
           && gram_wait(mipi_dsi_host_ll_gen_is_write_fifo_empty, true);
}

/**
 * @brief Open a write with a NOP the panel may lose, and let it leave the lane.
 *
 * The first packet after the bus comes back from light sleep reaches the panel
 * with an ECC error it cannot correct, and the panel drops it. When that packet
 * was the CASET, the pixels went into the whole window and the time came out
 * sheared; when it was the RAMWR, they went nowhere to be seen. The NOP is that
 * first packet, and losing it changes nothing.
 *
 * @return KODE_OK; KODE_TIMEOUT when the FIFO did not make room or drain.
 */
static kode_status_t gram_settle(void)
{
    if ( !gram_wait(mipi_dsi_host_ll_gen_is_cmd_fifo_full, false) )
    {
        return KODE_TIMEOUT;
    }
    mipi_dsi_host_ll_gen_set_packet_header(&MIPI_DSI_HOST, 0U, MIPI_DSI_DT_DCS_SHORT_WRITE_0,
                                           DCS_NOP, 0U);
    if ( !gram_drain() )
    {
        return KODE_TIMEOUT;
    }
    khal_delay_ms(GRAM_SETTLE_MS);
    return KODE_OK;
}

/**
 * @brief Send @p count pixels as RAMWR then RAMWRC packets into the window
 *        already set.
 *
 * Low byte first, the order LVGL keeps RGB565 in and the DPI path sends it
 * in: a pixel of white or black reads the same either way, a grey does not.
 * The window walks itself, so a packet need not end on a row.
 *
 * @return KODE_OK; KODE_TIMEOUT when a FIFO did not make room.
 */
static kode_status_t gram_pixels(const uint16_t *rgb565, size_t count)
{
    uint8_t       pkt[GRAM_PKT_WORDS * GRAM_WORD_BYTES] = {0};
    uint8_t       cmd  = DCS_RAMWR;
    size_t        done = 0U;
    kode_status_t st   = KODE_OK;

    while ( (KODE_OK == st) && (done < count) )
    {
        size_t k = GRAM_CHUNK_PX;

        if ( (count - done) < k )
        {
            k = count - done;
        }
        pkt[0] = cmd;
        for ( size_t i = 0U; i < k; ++i )
        {
            uint16_t const p = rgb565[done + i];

            pkt[1U + (GRAM_BYTES_PER_PX * i)] = (uint8_t)(p & GRAM_BYTE_MASK);
            pkt[2U + (GRAM_BYTES_PER_PX * i)] = (uint8_t)(p >> GRAM_BYTE_BITS);
        }
        st   = gram_send(pkt, 1U + (GRAM_BYTES_PER_PX * k));
        cmd  = DCS_RAMWRC;
        done += k;
    }
    return st;
}

kode_status_t kdrv_display_gram_write(uint16_t x, uint16_t y, uint16_t w,
                                      uint16_t h, const uint16_t *rgb565)
{
    /* The CO5300 takes a window that starts on an even column and row and
     * covers an even count of both (its CASET and RASET restrictions); the
     * column offset is even too. Checked against the glass without adding two
     * unbounded values: w and h against what is left after x and y. */
    uint32_t const odd = ((uint32_t)x | (uint32_t)y | (uint32_t)w | (uint32_t)h) & 1U;

    if ( (NULL == rgb565) || (0U == w) || (0U == h) || (0U != odd)
        || ((uint32_t)x >= KBSP_LCD_H_RES) || ((uint32_t)y >= KBSP_LCD_V_RES)
        || ((uint32_t)w > (KBSP_LCD_H_RES - (uint32_t)x))
        || ((uint32_t)h > (KBSP_LCD_V_RES - (uint32_t)y)) )
    {
        KLOG_W(TAG, "gram write %ux%u at (%u,%u) refused: off the glass, "
                    "empty, or not even", (unsigned)w, (unsigned)h,
               (unsigned)x, (unsigned)y);
        return KODE_DENIED;
    }
    if ( !s_brought_up || s_ready )
    {
        /* With video scanning the framebuffer is the picture and it overwrites
         * the panel's memory: there is nothing to write. */
        return KODE_DENIED;
    }
    if ( cmd_path_up() != ESP_OK )
    {
        return KODE_ERROR;
    }

    uint64_t const t0  = khal_time_us();
    uint32_t const col = (uint32_t)x + PANEL_COL_OFFSET;
    kode_status_t  st  = gram_settle();

    if ( KODE_OK == st )
    {
        st = gram_window(DCS_CASET, col, col + (uint32_t)w - 1U);
    }
    if ( KODE_OK == st )
    {
        st = gram_window(DCS_RASET, (uint32_t)y, (uint32_t)y + (uint32_t)h - 1U);
    }
    if ( KODE_OK == st )
    {
        st = gram_pixels(rgb565, (size_t)w * (size_t)h);
    }
    /* Leave the panel's window whole again: video mode streams the full
     * frame through it, and a window left on this rectangle folds the next
     * frames into it (stripes where the time was). It is sent after a failed
     * step too - a RAMWR left open on the small window would take every
     * later frame into it - and the first error is the one kept. */
    kode_status_t const caset = gram_window(DCS_CASET, PANEL_COL_OFFSET,
                                            PANEL_COL_OFFSET + KBSP_LCD_H_RES - 1U);
    kode_status_t const raset = gram_window(DCS_RASET, 0U, KBSP_LCD_V_RES - 1U);

    if ( KODE_OK == st )
    {
        st = caset;
    }
    if ( KODE_OK == st )
    {
        st = raset;
    }
    if ( !gram_drain() && (KODE_OK == st) )
    {
        st = KODE_TIMEOUT;
    }
    khal_delay_ms(GRAM_TAIL_MS);
    esp_err_t const down = cmd_path_down();
    uint32_t const   us  = (uint32_t)(khal_time_us() - t0);

    if ( KODE_OK == st )
    {
        KLOG_D(TAG, "gram write %ux%u at (%u,%u): written in %lu us",
               (unsigned)w, (unsigned)h, (unsigned)x, (unsigned)y,
               (unsigned long)us);
        if ( ESP_OK != down )
        {
            st = KODE_ERROR;
        }
    }
    else
    {
        KLOG_W(TAG, "gram write %ux%u at (%u,%u): NOT written (%lu us)",
               (unsigned)w, (unsigned)h, (unsigned)x, (unsigned)y,
               (unsigned long)us);
    }
    return st;
}

/* ---- Capture ------------------------------------------------------------- */

kode_status_t kdrv_display_capture_begin(const uint8_t **image, size_t *len)
{
#if CONFIG_KODEOS_SCREENSHOT
    if ( (NULL == image) || (NULL == len) )
    {
        return KODE_DENIED;
    }
    if ( !s_ready || (NULL == s_fb[0]) )
    {
        /* A panel on one framebuffer is drawn at the scan, chunk by chunk,
         * and keeps no whole frame anywhere to copy. */
        return KODE_NO_RESOURCES;
    }
    if ( NULL != s_shot )
    {
        /* One capture at a time: the first is still being sent. */
        return KODE_DENIED;
    }
    s_shot = khal_alloc_psram(64U, SHOT_LEN);
    if ( NULL == s_shot )
    {
        KLOG_W(TAG, "no PSRAM for a capture (%u KB)",
               (unsigned)(SHOT_LEN / 1024U));
        return KODE_NO_RESOURCES;
    }
    bmp_header(s_shot);

    /* The frame on the glass, bottom-up. The front buffer is the one the
     * last flip asked for, which is what the panel shows from its next
     * refresh on - and nothing flips again until AO_Display, which is
     * running this, takes the next frame. */
    const uint8_t * const front = s_fb[s_back ^ 1U];
    for ( int32_t y = 0; (int32_t)KBSP_LCD_V_RES > y; ++y )
    {
        size_t const row = (size_t)((KBSP_LCD_V_RES - 1) - y);
        (void)memcpy(&s_shot[(size_t)SHOT_HDR_LEN + (row * SHOT_STRIDE)],
                     &front[(size_t)y * SHOT_STRIDE], SHOT_STRIDE);
    }
    *image = s_shot;
    *len   = SHOT_LEN;
    return KODE_OK;
#else
    (void)image;
    (void)len;
    return KODE_NO_RESOURCES;
#endif
}

void kdrv_display_capture_end(void)
{
#if CONFIG_KODEOS_SCREENSHOT
    /* khal_free() takes NULL: an end with no capture open frees nothing. */
    khal_free(s_shot);
    s_shot = NULL;
#endif
}

bool kdrv_display_capture_painted(void)
{
#if CONFIG_KODEOS_SCREENSHOT
    return s_painted;
#else
    return false;
#endif
}

kode_status_t kdrv_display_capture_selftest(void)
{
#if CONFIG_KODEOS_SCREENSHOT
    uint8_t h[SHOT_HDR_LEN];

    if ( !s_ready || (NULL == s_fb[0]) )
    {
        return KODE_NO_RESOURCES;
    }
    /* Read the header back out of the bytes bmp_header() writes rather than
     * trusting the constants: the question is whether what a host will
     * receive is a BMP, and the only bytes that can answer that are the
     * ones it would be sent. */
    bmp_header(h);
    uint32_t const size    = (uint32_t)h[2] | ((uint32_t)h[3] << 8)
                           | ((uint32_t)h[4] << 16) | ((uint32_t)h[5] << 24);
    uint32_t const offbits = (uint32_t)h[10] | ((uint32_t)h[11] << 8)
                           | ((uint32_t)h[12] << 16) | ((uint32_t)h[13] << 24);
    uint32_t const width   = (uint32_t)h[18] | ((uint32_t)h[19] << 8)
                           | ((uint32_t)h[20] << 16) | ((uint32_t)h[21] << 24);
    uint32_t const height  = (uint32_t)h[22] | ((uint32_t)h[23] << 8)
                           | ((uint32_t)h[24] << 16) | ((uint32_t)h[25] << 24);
    uint16_t const bpp     = (uint16_t)((uint16_t)h[28]
                                        | ((uint16_t)h[29] << 8));

    bool const ok = ((uint8_t)'B' == h[0]) && ((uint8_t)'M' == h[1])
                    && ((uint32_t)SHOT_LEN == size)
                    && ((uint32_t)SHOT_HDR_LEN == offbits)
                    && ((uint32_t)KBSP_LCD_H_RES == width)
                    && ((uint32_t)KBSP_LCD_V_RES == height)
                    && (16U == bpp);
    if ( !ok )
    {
        return KODE_ERROR;
    }
    return KODE_OK;
#else
    return KODE_NO_RESOURCES;
#endif
}

/* ---- Mirror: reading the glass without stopping it -----------------------
 * A capture is a still moment, copied whole. The mirror wants a live band
 * every frame, not one still moment every few seconds: read access to a few
 * rows of the front framebuffer at a time, and to know which rows are worth
 * reading at all.
 */

kode_status_t kdrv_display_band(uint32_t y, uint32_t h, const uint8_t **rows,
                                size_t *len)
{
#if CONFIG_KODEOS_SCREENSHOT
    if ( (NULL == rows) || (NULL == len) )
    {
        return KODE_DENIED;
    }
    if ( !s_ready || (NULL == s_fb[0]) )
    {
        return KODE_NO_RESOURCES;
    }
    /* Subtracts rather than adds, on purpose: y + h is itself unsigned and
     * can wrap (e.g. y=1, h=0xFFFFFFFF wraps the sum to 0, which then passes
     * a "> V_RES" check with room to spare). Comparing h against the room
     * actually left - V_RES - y, computed only after y is already known in
     * range - cannot overflow, because it never adds two unbounded values.
     * y >= V_RES rather than > : at y == V_RES the band is empty by
     * definition, not one row from underflowing. */
    if ( (0U == h) || ((uint32_t)KBSP_LCD_V_RES <= y)
        || (((uint32_t)KBSP_LCD_V_RES - y) < h) )
    {
        return KODE_DENIED;
    }
    *rows = &s_fb[s_back ^ 1U][(size_t)y * SHOT_STRIDE];
    *len  = (size_t)h * SHOT_STRIDE;
    return KODE_OK;
#else
    (void)y;
    (void)h;
    (void)rows;
    (void)len;
    return KODE_NO_RESOURCES;
#endif
}

void kdrv_display_dirty_take(int32_t *y1, int32_t *y2)
{
#if CONFIG_KODEOS_SCREENSHOT
    if ( (NULL == y1) || (NULL == y2) )
    {
        return;
    }
    *y1 = s_mirror_y1;
    *y2 = s_mirror_y2;
    s_mirror_y1 = (int32_t)KBSP_LCD_V_RES;
    s_mirror_y2 = 0;
#else
    if ( NULL != y1 )
    {
        *y1 = 0;
    }
    if ( NULL != y2 )
    {
        *y2 = 0;
    }
#endif
}

void kdrv_display_link(kdrv_display_link_t *out)
{
    if ( NULL == out )
    {
        return;
    }
    out->frames    = s_lk_frames;
    out->underruns = s_lk_underruns;
    out->dpi       = s_lk_dpi;
    out->ack       = s_lk_ack;
    out->phy       = s_lk_phy;
    out->other     = s_lk_other;
    out->st0       = s_lk_st0;
    out->st1       = s_lk_st1;
    out->missed    = s_lk_missed;
}
