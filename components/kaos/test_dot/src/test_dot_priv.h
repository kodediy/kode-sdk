/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_priv.h
 * @brief AO_TestDot's private half: the worker's jobs and the camera memory its files share.
 */
#ifndef TEST_DOT_PRIV_H_
#define TEST_DOT_PRIV_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kdrv_dock_loop.h"
#include "kode_defs.h"
#include "kqp_config.h"
#include "tdan.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The camera's fixed settings ---- */

#define TD_WIN_X          774U    /**< The window the test looks in first: column of the array.  */
#define TD_WIN_Y          600U    /**< Its first row.                                            */
#define TD_WIN_W          1056U   /**< Its width: the screen and a margin.                       */
#define TD_WIN_H          768U    /**< Its height.                                               */
#define TD_FIELD_W        800U    /**< The whole field the LED photos are taken in: width.       */
#define TD_FIELD_H        600U    /**< Its height.                                               */
#define TD_AWB_R          1518U   /**< White-balance gain, red (x1 = 0x400).                     */
#define TD_AWB_G          1024U   /**< Green.                                                    */
#define TD_AWB_B          1176U   /**< Blue.                                                     */
#define TD_VCM            712U    /**< The lens motor's code, focused on the glass.              */
#define TD_GAIN           16U     /**< Sensor gain: x1.                                          */
#define TD_GRAB_MS        3000U   /**< The longest wait for one frame.                           */
#define TD_SKIP_PATTERN   2U      /**< Frames let go after a pattern or exposure change.         */
#define TD_SKIP_COLOUR    1U      /**< After a colour's exposure: the change shows a frame later. */
#define TD_SKIP_LED       1U      /**< After the LED's colour.                                   */
#define TD_EXP_BLACK      150U    /**< Exposure, lines: the black with its marks, and panel off. */
#define TD_EXP_WHITE      40U     /**< The white.                                                */
#define TD_EXP_GREY       144U    /**< The grey.                                                 */
#define TD_EXP_COLOUR     30U     /**< Red, green and blue.                                      */
#define TD_EXP_LED        6U      /**< The LED photos.                                           */
#define TD_SENSOR_CX      1296.0  /**< The array's centre column.                                */
#define TD_SENSOR_CY      972.0   /**< Its centre row.                                           */
#define TD_PANEL_CX       205.0   /**< The panel's centre column.                                */
#define TD_PANEL_CY       251.0   /**< Its centre row.                                           */
#define TD_MM_PER_PANEL_PX 0.0807 /**< The panel's pixel pitch, mm.                              */
#define TD_H_TERMS        9U      /**< Terms of a homography.                                    */

/* ---- The photos kept for `test_dot_photos`, and the blobs ---- */

#define TD_PHOTOS_MAX     14U     /**< Photos a pass keeps: placement, eleven of the camera part, spare. */
#define TD_PHOTO_SLOT     (400U * 300U * 2U) /**< The largest kept photo, bytes: a field halved.   */
#define TD_PHOTO_NAME     24U     /**< Bytes of a kept photo's name.                              */
#define TD_PHOTO_QUALITY  70U     /**< Their JPEG quality.                                        */
#define TD_JOUT_MAX       (128U * 1024U) /**< The largest kept photo as a JPEG, bytes.               */
#define TD_BLOBS_MAX      8U      /**< Blobs a pass keeps: location, colours, crops of defects.  */
#define TD_BLOB_POOL      (512U * 1024U) /**< PSRAM for every blob's bytes.                       */
#define TD_LOC_W          352U    /**< The location photo, small: width.                          */
#define TD_LOC_H          256U    /**< Its height.                                                */
#define TD_LOC_QUALITY    70U     /**< Its JPEG quality.                                          */
#define TD_STRIP_W        96U     /**< One colour's crop in the colours strip: width.             */
#define TD_STRIP_H        64U     /**< Its height.                                                */
#define TD_STRIP_QUALITY  75U     /**< The strip's JPEG quality.                                  */
#define TD_CROP_PX        64U     /**< A defect's crop: side.                                     */
#define TD_CROP_QUALITY   85U     /**< Its JPEG quality.                                          */
#define TD_CROPS_MAX      4U      /**< Defects cropped at most, dead and stuck-on each.           */
#define TD_ZONE_CROPS     2U      /**< Zones cropped at most.                                     */
#define TD_ZONE_CROP_W    128U    /**< A zone's crop: width.                                      */
#define TD_ZONE_CROP_H    96U     /**< Its height.                                                */
#define TD_ZONE_QUALITY   80U     /**< Its JPEG quality.                                          */
#define TD_ZONE_MARGIN    20      /**< What a zone's crop adds around it, photo px.               */
#define TD_STRIP_SLOTS    5U      /**< Crops in the colours strip: red, green, blue, white, grey. */
#define TD_WINDOW_SHRINK  3U      /**< A window photo is kept at a third of its side.             */
#define TD_FIELD_SHRINK   2U      /**< A field photo is kept at half of its side.                 */

/* ---- The LED judged by the camera ---- */

#define TD_LED_PHOTOS     4U      /**< Off, red, green, blue.                                     */
#define TD_LED_BLOCK      8U      /**< The block the LED is searched in, px.                      */
#define TD_LED_R          22      /**< Half the side of the box the LED's light is summed in, px. */
#define TD_LED_SAT_MAX    160U    /**< Saturated samples a colour photo may hold.                 */
#define TD_LED_MIN_FLUX   4000.0  /**< The least light a colour gives, at the nominal exposure.   */
#define TD_LED_MAX_DIST   0.8     /**< The furthest its colour may be from the pure one.          */

/** @brief The camera part's steps, in the order a run takes them. */
typedef enum {
    TD_CS_MARKS = 0,  /**< The black with its marks: where the screen is.          */
    TD_CS_OFF,        /**< The same, panel off: what the room adds.                */
    TD_CS_WHITE,      /**< White: pixels, lines, zones, the white's colour.        */
    TD_CS_GREY,       /**< Grey: the same, and the stuck-on pixels after it.       */
    TD_CS_RED,        /**< Red's colour.                                           */
    TD_CS_GREEN,      /**< Green's.                                                */
    TD_CS_BLUE,       /**< Blue's.                                                 */
    TD_CS_JUDGE,      /**< No photo: the screen's and the colours' verdicts.       */
    TD_CS_LED_OFF,    /**< The black screen, the LED off.                          */
    TD_CS_LED_RED,    /**< The LED red.                                            */
    TD_CS_LED_GREEN,  /**< Green.                                                  */
    TD_CS_LED_BLUE,   /**< Blue.                                                   */
    TD_CS_LED_JUDGE,  /**< No photo: the LED's verdict.                            */
    TD_CS_DONE        /**< The camera part is over.                                */
} td_cam_step_t;

/** @brief A photo kept small for the PC. */
typedef struct {
    char      name[TD_PHOTO_NAME];  /**< `photo_NN_<name>` is made of it.  */
    uint16_t *px;                   /**< RGB565, in the photo pool.        */
    uint16_t  w;                    /**< Width.                            */
    uint16_t  h;                    /**< Height.                           */
} td_photo_t;

/** @brief A blob kept for the wire. */
typedef struct {
    char     name[KODE_TEST_DOT_NAME_MAX]; /**< Its name.            */
    uint8_t *data;                         /**< Its bytes, in the pool. */
    uint32_t len;                          /**< How many.            */
} td_blob_t;

/* ---- The records a verdict step leaves for the AO ---- */

#define TD_WARN_RECORDS   4U      /**< Pixel warnings recorded at most.                          */
#define TD_CREC_FIXED     8U      /**< The judge's records besides the warnings: framing, turn,
                                   *   pixels, lines, zones, the colours' two, one spare.       */
#define TD_CREC_MAX       (TD_CREC_FIXED + TD_WARN_RECORDS) /**< Records one step leaves.         */

/** @brief One record a verdict step formatted on the worker, for the AO to send. */
typedef struct {
    uint8_t part;                          /**< test_dot_cam_t: which part it belongs to. */
    char    text[KODE_FUNCTION_REC_MAX];   /**< The record.                               */
} td_crec_t;

/** @brief The camera part's memory: PSRAM, taken when a camera verb first needs it and given back at test_dot_end. */
typedef struct {
    tdan_work_t    wk;                         /**< The analysis' four planes.               */
    uint8_t       *luma;                       /**< Scratch luma of the window.              */
    uint8_t       *lj[3];                      /**< Black (net of the panel off), white, grey. */
    uint8_t       *lb;                         /**< The panel-off photo's luma.              */
    uint16_t      *keep_w;                     /**< The white photo, for the crops.          */
    uint16_t      *keep_b;                     /**< The black photo.                         */
    uint16_t      *strip;                      /**< The colours strip.                       */
    uint8_t       *photo_pool;                 /**< TD_PHOTOS_MAX slots.                     */
    uint8_t       *blob_pool;                  /**< TD_BLOB_POOL bytes.                      */
    uint8_t       *jout;                       /**< TD_JOUT_MAX bytes: a kept photo as a JPEG. */
    uint32_t       jout_len;                   /**< Its length.                              */
    td_crec_t     *crec;                       /**< TD_CREC_MAX records the last step left.  */
    uint8_t        ncrec;                      /**< How many.                                */
    td_photo_t     photo[TD_PHOTOS_MAX];       /**< Kept photos.                             */
    uint8_t        nphoto;                     /**< How many.                                */
    td_blob_t      blob[TD_BLOBS_MAX];         /**< Kept blobs.                              */
    uint8_t        nblob;                      /**< How many.                                */
    uint32_t       blob_used;                  /**< Bytes of the pool taken.                 */
    bool           placed;                     /**< The placement's photo is the first kept. */
    bool           place_ok;                   /**< The last look found four marks.          */
    double         place_dx;                   /**< Its offset across, mm.                   */
    double         place_dy;                   /**< Down, mm.                                */
    bool           open;                       /**< The camera is capturing.                 */
    bool           field;                      /**< In the whole field, not the window.      */
    uint16_t       win_x;                      /**< The window in use: column.               */
    uint16_t       win_y;                      /**< Row.                                     */
    uint32_t       last_exp;                   /**< The exposure the last photo had.         */
    /* What the photos measured. */
    tdan_h_t       h;                          /**< Panel to window.                         */
    bool           located;                    /**< Four marks found.                        */
    bool           partial;                    /**< Fewer: the place known, no verdict.      */
    bool           rescued;                    /**< Found by the whole-photo search.         */
    int32_t        nmarks;                     /**< Marks seen.                              */
    int32_t        missing;                    /**< The first mark not found, or -1.         */
    tdan_pix_t     pix;                        /**< The pixel method's state.                */
    tdan_photo_t   ph[2];                      /**< White and grey's analysis.               */
    tdan_pt_t      stuck[TDAN_MAXP];           /**< Stuck-on pixels of the black.            */
    int32_t        nstuck;                     /**< How many.                                */
    bool           shot[TDAN_COLOURS];         /**< The colour photo was taken.              */
    double         rgb[TDAN_COLOURS][3];       /**< Its band's mean RGB.                     */
    double         eexp[TDAN_COLOURS];         /**< The exposure it had.                     */
    bool           retake[TDAN_COLOURS];       /**< Retaken with a scaled exposure.          */
    double         colscale;                   /**< Red's exposure scale, kept for green and blue. */
    uint32_t       auto_pct;                   /**< The exposure the marks needed, percent.  */
    double         doff[3];                    /**< The panel-off photo's colour.            */
    double         eoff;                       /**< Its exposure.                            */
    float          ripple;                     /**< The white's ripple.                      */
    /* The verdicts. */
    tdan_frame_t   frame;                      /**< Framing and turn.                        */
    tdan_screen_t  screen;                     /**< Pixels, lines, zones.                    */
    tdan_colour_t  colour;                     /**< The colours.                             */
    double         dx_mm;                      /**< Where the screen sits: across.           */
    double         dy_mm;                      /**< Down.                                    */
    uint16_t      *led[TD_LED_PHOTOS];         /**< The LED photos, in buffers free by then. */
    uint8_t        led_verdict;                /**< tdan_verdict_t.                          */
    const char    *led_why;                    /**< Why no verdict.                          */
    double         led_flux[TD_LED_PHOTOS];    /**< Red, green, blue at 1..3.                */
    double         led_dist[TD_LED_PHOTOS];    /**< Their distance to the pure colour.       */
    int32_t        led_x;                      /**< Where the LED is, field px.              */
    int32_t        led_y;                      /**< Its row.                                 */
    uint32_t       photo_ms;                   /**< Time spent photographing.                */
    uint32_t       an_ms;                      /**< Time spent analysing.                    */
    uint32_t       led_ms;                     /**< Time spent on the LED.                   */
    /* The placement. */
    double         place_ang;                  /**< The turn it read.                        */
} td_cam_t;

/** @brief The worker's jobs. */
typedef enum {
    TD_JOB_PROBE = 0,  /**< Find the camera on J3.                                  */
    TD_JOB_STATION,    /**< The station's two chips answer; the relays at rest.      */
    TD_JOB_CLOSE,      /**< The relays at rest (if touched), the camera closed.     */
    TD_JOB_RELAYS,     /**< Write the relay byte.                                   */
    TD_JOB_ADC,        /**< Read one input.                                         */
    TD_JOB_LOOP,       /**< The pads loop.                                          */
    TD_JOB_CAM,        /**< One step of the camera part.                            */
    TD_JOB_PLACE,      /**< One look for the marks.                                 */
    TD_JOB_PHOTO_OUT   /**< One kept photo as a JPEG blob.                          */
} td_job_kind_t;

/** @brief One job and what it found: AO_TestDot's, read only after its done event. */
typedef struct {
    uint8_t          kind;       /**< td_job_kind_t.                                  */
    uint8_t          arg;        /**< The relay byte, the input, the frames, the step, the photo. */
    bool             rest;       /**< CLOSE: write the relays' rest first.            */
    uint8_t          map;        /**< PROBE: the camera's map.                        */
    int32_t          mv;         /**< ADC: the reading.                               */
    kdrv_dock_loop_t loop;       /**< LOOP: what came back.                           */
    uint32_t         ms;         /**< How long it took.                               */
    td_cam_t        *cam;        /**< CAM, PLACE, PHOTO_OUT: the camera's memory.     */
} td_job_t;

/**
 * @brief Run one job on the worker's thread.
 * @param arg   The td_job_t.
 * @param value Unused.
 * @return The job's status.
 */
kode_status_t td_job_run(void *arg, uint32_t *value);

/**
 * @brief Take the camera part's memory from PSRAM.
 * @return It, every block taken, or NULL with nothing taken.
 */
td_cam_t *td_cam_alloc(void);

/**
 * @brief Give the camera part's memory back.
 * @param c It, or NULL.
 */
void td_cam_free(td_cam_t *c);

/**
 * @brief Forget a pass's measurements and kept photos, keep the memory.
 * @param c The memory.
 * @param keep_place Keep the placement's photo, the first kept.
 */
void td_cam_reset(td_cam_t *c, bool keep_place);

/**
 * @brief One step of the camera part: a photo and what it measures, or a verdict. Worker thread.
 *
 * The verdict steps (TD_CS_JUDGE, TD_CS_LED_JUDGE) also format their records
 * into @c crec, in the order the station reads them; the AO only copies them.
 *
 * @param c    The memory.
 * @param step td_cam_step_t.
 * @return KODE_OK; KODE_TIMEOUT when the camera gave no frame; KODE_ERROR when
 *         it would not open.
 */
kode_status_t td_cam_step(td_cam_t *c, uint8_t step);

/**
 * @brief One look for the marks, for `test_dot_place`. Worker thread.
 * @param c The memory; its window is where the look goes.
 * @return KODE_OK with the look's result in @p c; KODE_TIMEOUT or KODE_ERROR as td_cam_step().
 */
kode_status_t td_cam_place(td_cam_t *c);

/**
 * @brief Kept photo @p i as a JPEG in the memory's out buffer. Worker thread.
 * @param c The memory.
 * @param i The photo, from 0.
 * @return KODE_OK; KODE_DENIED past the last; the encoder's refusal otherwise.
 */
kode_status_t td_cam_photo_out(td_cam_t *c, uint8_t i);

/**
 * @brief Encode @p w x @p h of RGB565 at @p src into @p dst. Worker thread.
 *
 * Asked of AO_Display, which owns the JPEG engine (display_jpeg_request()),
 * waiting at most KODE_TEST_DOT_JPEG_MS. In a file of its own,
 * test_dot_jpeg.c, so the host suite can stand a fake in its place.
 *
 * @param src     The pixels.
 * @param w       Width.
 * @param h       Height.
 * @param quality 1..100.
 * @param dst     The JPEG's room.
 * @param cap     Its bytes.
 * @param len     Receives the JPEG's length; 0 on any refusal.
 * @return KODE_OK; KODE_TIMEOUT when AO_Display did not answer; its refusal otherwise.
 */
kode_status_t td_jpeg_encode(const uint16_t *src, uint32_t w, uint32_t h, uint32_t quality, uint8_t *dst,
                             uint32_t cap, uint32_t *len);

/**
 * @brief Find a kept blob by its name.
 * @param c    The memory.
 * @param name The name.
 * @return Its index, or TD_BLOBS_MAX.
 */
uint8_t td_cam_blob_find(const td_cam_t *c, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* TEST_DOT_PRIV_H_ */
