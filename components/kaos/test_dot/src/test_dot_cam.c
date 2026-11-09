/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_dot_cam.c
 * @brief The E5 test's camera part: the photos, what each measures, the verdicts and their records.
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "kdrv_ov5640.h"
#include "khal_system.h"
#include "khal_time.h"
#include "kode_log.h"
#include "kqp.h"

#include "tdan.h"
#include "test_dot_priv.h"

/** @brief This file's log tag. */
#define TAG "kaos.test_dot"

/** @brief Bytes of one window photo, RGB565. */
#define TD_WIN_BYTES     ((size_t)TD_WIN_W * (size_t)TD_WIN_H * 2U)
/** @brief Pixels of one window photo. */
#define TD_WIN_PX        ((size_t)TD_WIN_W * (size_t)TD_WIN_H)
/** @brief Bytes of one field photo, RGB565. */
#define TD_FIELD_BYTES   ((size_t)TD_FIELD_W * (size_t)TD_FIELD_H * 2U)
/** @brief Pixels of one field photo. */
#define TD_FIELD_PX      ((size_t)TD_FIELD_W * (size_t)TD_FIELD_H)
/** @brief Alignment of every PSRAM block. */
#define TD_ALIGN         16U
/** @brief The window's origin must be even: an odd one shifts the Bayer pattern and swaps red and blue. */
#define TD_EVEN_MASK     0xFFFEU
/** @brief The analysis lets the idle task in this often, ms. */
#define TD_YIELD_MS      100U
/** @brief Panel-off frames averaged for the dark, the first included. */
#define TD_DARK_FRAMES   4U
/** @brief The longest exposure, lines. */
#define TD_EXP_MAX       1900U
/** @brief The shortest exposure a retake may ask, lines. */
#define TD_EXP_MIN       2U
/** @brief Percent. */
#define TD_PCT           100U
/** @brief The exposures the marks are looked for at when the nominal one finds none, percent. */
#define TD_MARK_TRIES    3U
/** @brief A colour's level against its reference: under this it is retaken. */
#define TD_RATIO_LOW     0.85
/** @brief Over this it is retaken. */
#define TD_RATIO_HIGH    1.18
/** @brief Under this (or over the next) a level is first a frame caught in the pattern change. */
#define TD_RATIO_SAME_LOW  0.35
/** @brief Over this. */
#define TD_RATIO_SAME_HIGH 2.5
/** @brief Retakes of a colour. */
#define TD_COLOUR_TRIES  4U
/** @brief A clipped level reads low: the sensor's top reads about 220 here. */
#define TD_CLIP_LEVEL    215.0
/** @brief What a clipped level is taken to be worth. */
#define TD_CLIP_FACTOR   2.2
/** @brief The smallest ratio a retake divides by. */
#define TD_RATIO_FLOOR   0.05
/** @brief Frames let go by after a transient level, on top of the pattern's. */
#define TD_SKIP_TRANSIENT 2U
/** @brief A luma at or over this is saturated. */
#define TD_SAT_LUMA      250U
/** @brief Saturated samples (every second pixel) over which a white or grey is taken again with less light. */
#define TD_SAT_SAMPLES   900U
/** @brief The light each saturation retake keeps. */
#define TD_SAT_SCALE     0.7
/** @brief Saturation retakes at most. */
#define TD_SAT_TRIES     2U
/** @brief The rounding of an exposure. */
#define TD_HALF          0.5
/* The colour photos' references: the level each reads at its nominal exposure. */
#define TD_REF_WHITE     152.35  /**< White. */
#define TD_REF_GREY      127.45  /**< Grey. */
#define TD_REF_RED       133.2  /**< Red. */
#define TD_REF_GREEN     115.05  /**< Green. */
#define TD_REF_BLUE      129.5  /**< Blue. */
/* RGB565: the bits of each channel and where they sit. */
#define TD_R_SHIFT       11U  /**< Red sits here. */
#define TD_G_SHIFT       5U  /**< Green. */
#define TD_R_MASK        0x1FU  /**< Red has five bits. */
#define TD_G_MASK        0x3FU  /**< Green six. */
#define TD_B_MASK        0x1FU  /**< Blue five. */
/* The weights that bring each channel to 0..255. */
#define TD_R_W           8  /**< Red to 0..255. */
#define TD_G_W           4  /**< Green. */
#define TD_B_W           8  /**< Blue. */
/** @brief The LED's saturation is counted on every fourth pixel. */
#define TD_LED_SAT_STEP  4U
/** @brief The LED's nominal exposure, the flux's reference. */
#define TD_LED_NOMINAL   6.0
/** @brief Channels. */
#define TD_CH            3U
/** @brief The LED's light is summed over a square of this many blocks a side, centred on one. */
#define TD_LED_NEIGH     3U
/** @brief Room of a list of points in a record. */
#define TD_LIST_MAX      96U
/** @brief A list in a record stops taking entries with less room than this left. */
#define TD_LIST_ROOM     16U
/** @brief A defect's crop is centred: half its side. */
#define TD_CROP_HALF     ((int32_t)TD_CROP_PX / 2)

/** @brief The yield the analysis calls: the idle task's tick every TD_YIELD_MS. */
static void td_yield(void)
{
    static uint32_t s_last;
    uint32_t const now = khal_time_ms();

    if ( (now - s_last) > TD_YIELD_MS )
    {
        khal_delay_ms(1U);
        s_last = khal_time_ms();
    }
}

/* ---- Memory ------------------------------------------------------------------ */

/** @brief One block of PSRAM, counted. */
static void *td_take(size_t bytes, bool *ok)
{
    void *p = khal_alloc_psram(TD_ALIGN, bytes);

    if ( NULL == p )
    {
        *ok = false;
    }
    return p;
}

td_cam_t *td_cam_alloc(void)
{
    td_cam_t *c = (td_cam_t *)khal_alloc_psram(TD_ALIGN, sizeof(td_cam_t));
    bool ok = true;
    size_t const plane = TDAN_PLANE_FLOATS * sizeof(float);

    if ( NULL == c )
    {
        return NULL;
    }
    (void)memset(c, 0, sizeof(*c));
    c->wk.a       = (float *)td_take(plane, &ok);
    c->wk.b       = (float *)td_take(plane, &ok);
    c->wk.c       = (float *)td_take(plane, &ok);
    c->wk.d       = (float *)td_take(plane, &ok);
    c->luma       = (uint8_t *)td_take(TD_WIN_PX, &ok);
    c->lj[0]      = (uint8_t *)td_take(TD_WIN_PX, &ok);
    c->lj[1]      = (uint8_t *)td_take(TD_WIN_PX, &ok);
    c->lj[2]      = (uint8_t *)td_take(TD_WIN_PX, &ok);
    c->lb         = (uint8_t *)td_take(TD_WIN_PX, &ok);
    c->keep_w     = (uint16_t *)td_take(TD_WIN_BYTES, &ok);
    c->keep_b     = (uint16_t *)td_take(TD_WIN_BYTES, &ok);
    c->strip      = (uint16_t *)td_take((size_t)TD_STRIP_SLOTS * TD_STRIP_W * TD_STRIP_H * 2U, &ok);
    c->photo_pool = (uint8_t *)td_take((size_t)TD_PHOTOS_MAX * TD_PHOTO_SLOT, &ok);
    c->blob_pool  = (uint8_t *)td_take(TD_BLOB_POOL, &ok);
    c->jout       = (uint8_t *)td_take(TD_JOUT_MAX, &ok);
    c->crec       = (td_crec_t *)td_take(sizeof(td_crec_t) * TD_CREC_MAX, &ok);
    if ( !ok )
    {
        KLOG_E(TAG, "camera: no PSRAM for the photos (%u free, %u largest)",
               (unsigned)khal_psram_free(), (unsigned)khal_psram_largest());
        td_cam_free(c);
        c = NULL;
    }
    else
    {
        c->win_x = TD_WIN_X;
        c->win_y = TD_WIN_Y;
        td_cam_reset(c, false);
    }
    return c;
}

void td_cam_free(td_cam_t *c)
{
    if ( NULL != c )
    {
        khal_free(c->wk.a);
        khal_free(c->wk.b);
        khal_free(c->wk.c);
        khal_free(c->wk.d);
        khal_free(c->luma);
        khal_free(c->lj[0]);
        khal_free(c->lj[1]);
        khal_free(c->lj[2]);
        khal_free(c->lb);
        khal_free(c->keep_w);
        khal_free(c->keep_b);
        khal_free(c->strip);
        khal_free(c->photo_pool);
        khal_free(c->blob_pool);
        khal_free(c->jout);
        khal_free(c->crec);
        khal_free(c);
    }
}

void td_cam_reset(td_cam_t *c, bool keep_place)
{
    uint8_t keep = 0U;

    if ( keep_place && c->placed && (0U < c->nphoto) )
    {
        keep = 1U;
    }
    for ( uint8_t i = keep; TD_PHOTOS_MAX > i; i++ )
    {
        c->photo[i].px = NULL;
    }
    c->nphoto    = keep;
    c->placed    = (1U == keep);
    c->nblob     = 0U;
    c->blob_used = 0U;
    c->located   = false;
    c->partial   = false;
    c->rescued   = false;
    c->nmarks    = 0;
    c->missing   = -1;
    c->nstuck    = 0;
    c->eoff      = 0.0;
    c->ripple    = 0.0f;
    c->colscale  = 1.0;
    c->auto_pct  = TD_PCT;
    c->led_verdict = (uint8_t)TDAN_NO_VERDICT;
    c->led_why   = "no_frames";
    c->photo_ms  = 0U;
    c->an_ms     = 0U;
    c->led_ms    = 0U;
    (void)memset(&c->pix, 0, sizeof(c->pix));
    (void)memset(c->ph, 0, sizeof(c->ph));
    (void)memset(c->shot, 0, sizeof(c->shot));
    (void)memset(c->retake, 0, sizeof(c->retake));
    (void)memset(c->rgb, 0, sizeof(c->rgb));
    (void)memset(c->eexp, 0, sizeof(c->eexp));
    (void)memset(c->doff, 0, sizeof(c->doff));
    (void)memset(&c->frame, 0, sizeof(c->frame));
    (void)memset(&c->screen, 0, sizeof(c->screen));
    (void)memset(&c->colour, 0, sizeof(c->colour));
    (void)memset(c->led, 0, sizeof(c->led));
    (void)memset(c->strip, 0, (size_t)TD_STRIP_SLOTS * TD_STRIP_W * TD_STRIP_H * 2U);
}

uint8_t td_cam_blob_find(const td_cam_t *c, const char *name)
{
    uint8_t found = (uint8_t)TD_BLOBS_MAX;

    for ( uint8_t i = 0U; i < c->nblob; i++ )
    {
        if ( ((uint8_t)TD_BLOBS_MAX == found) && (strcmp(c->blob[i].name, name) == 0) )
        {
            found = i;
        }
    }
    return found;
}

/* ---- The camera ---------------------------------------------------------------- */

/** @brief The test's own settings, with the exposure @p exp. */
static kode_status_t td_apply(uint32_t exp)
{
    kdrv_ov5640_cfg_t cfg;

    (void)memset(&cfg, 0, sizeof(cfg));
    cfg.exp_set   = true;
    cfg.gain_set  = true;
    cfg.awb_set   = true;
    cfg.isp_off   = true;
    cfg.vcm_set   = true;
    cfg.exp_lines = exp;
    cfg.gain      = (uint16_t)TD_GAIN;
    cfg.awb[0]    = (uint16_t)TD_AWB_R;
    cfg.awb[1]    = (uint16_t)TD_AWB_G;
    cfg.awb[2]    = (uint16_t)TD_AWB_B;
    cfg.vcm       = (uint16_t)TD_VCM;
    cfg.vflip     = true;
    cfg.mirror    = false;
    return kdrv_ov5640_apply(&cfg);
}

/** @brief The camera capturing in the window at c->win_x, c->win_y (sensor px, unflipped). */
static kode_status_t td_open_window(td_cam_t *c)
{
    kdrv_ov5640_mode_t m;
    uint32_t ax = c->win_x;
    uint32_t ay = c->win_y;
    kode_status_t rc = KODE_OK;

    if ( (KDRV_OV5640_SENSOR_W - TD_WIN_W) < ax )
    {
        ax = KDRV_OV5640_SENSOR_W - TD_WIN_W;
    }
    if ( (KDRV_OV5640_SENSOR_H - TD_WIN_H) < ay )
    {
        ay = KDRV_OV5640_SENSOR_H - TD_WIN_H;
    }
    ax &= TD_EVEN_MASK;
    ay &= TD_EVEN_MASK;
    if ( !c->open || c->field )
    {
        (void)memset(&m, 0, sizeof(m));
        m.window = true;
        m.x      = (uint16_t)ax;
        /* The sensor is flipped vertically: the window's row is counted from the array's far edge. */
        m.y      = (uint16_t)(KDRV_OV5640_SENSOR_H - ay - TD_WIN_H);
        m.w      = (uint16_t)TD_WIN_W;
        m.h      = (uint16_t)TD_WIN_H;
        m.fmt    = KDRV_OV5640_RGB565;
        rc = td_apply(TD_EXP_BLACK);
        if ( KODE_OK == rc )
        {
            rc = kdrv_ov5640_open(&m);
        }
        c->open  = (KODE_OK == rc);
        c->field = false;
    }
    return rc;
}

/** @brief The camera capturing the whole field. */
static kode_status_t td_open_field(td_cam_t *c)
{
    kdrv_ov5640_mode_t m;
    kode_status_t rc = KODE_OK;

    if ( !c->open || !c->field )
    {
        (void)memset(&m, 0, sizeof(m));
        m.window = false;
        m.fmt    = KDRV_OV5640_RGB565;
        rc = td_apply(TD_EXP_LED);
        if ( KODE_OK == rc )
        {
            rc = kdrv_ov5640_open(&m);
        }
        c->open  = (KODE_OK == rc);
        c->field = (KODE_OK == rc);
    }
    return rc;
}

/**
 * @brief A photo at exposure @p exp after @p skip frames: held until td_drop().
 * @return The frame, or NULL.
 */
static const uint16_t *td_take_photo(td_cam_t *c, uint32_t exp, uint32_t skip)
{
    const uint8_t *f = NULL;
    size_t len = 0U;
    uint32_t e = (exp * c->auto_pct) / TD_PCT;
    uint32_t const t0 = khal_time_ms();

    if ( 1U > e )
    {
        e = 1U;
    }
    if ( TD_EXP_MAX < e )
    {
        e = TD_EXP_MAX;
    }
    c->last_exp = e;
    if ( (td_apply(e) != KODE_OK) || (kdrv_ov5640_grab(skip, TD_GRAB_MS, &f, &len) != KODE_OK) )
    {
        f = NULL;
    }
    c->photo_ms += khal_time_ms() - t0;
    return (const uint16_t *)(const void *)f;
}

/** @brief Give the held frame back. */
static void td_drop(void)
{
    kdrv_ov5640_release();
}

/** @brief Keep @p f small under @p name, in the order taken; the same name again replaces it. */
static void td_keep(td_cam_t *c, const char *name, const uint16_t *f, uint32_t w, uint32_t h)
{
    uint32_t shrink = TD_WINDOW_SHRINK;
    uint8_t slot = c->nphoto;

    if ( TD_FIELD_W == w )
    {
        shrink = TD_FIELD_SHRINK;
    }
    for ( uint8_t i = 0U; i < c->nphoto; i++ )
    {
        if ( strcmp(c->photo[i].name, name) == 0 )
        {
            slot = i;
        }
    }
    if ( (NULL != f) && (TD_PHOTOS_MAX > slot) )
    {
        uint32_t const kw = w / shrink;
        uint32_t const kh = h / shrink;
        uint16_t *d = (uint16_t *)(void *)&c->photo_pool[(size_t)slot * TD_PHOTO_SLOT];

        /* One sample from the middle of each block: a few ms, not the 100 ms of averaging the whole frame. */
        for ( uint32_t y = 0U; y < kh; y++ )
        {
            const uint16_t *row = &f[(((size_t)y * shrink) + (shrink / 2U)) * w];
            for ( uint32_t x = 0U; x < kw; x++ )
            {
                d[((size_t)y * kw) + x] = row[((size_t)x * shrink) + (shrink / 2U)];
            }
        }
        (void)snprintf(c->photo[slot].name, sizeof(c->photo[slot].name), "%s", name);
        c->photo[slot].px = d;
        c->photo[slot].w  = (uint16_t)kw;
        c->photo[slot].h  = (uint16_t)kh;
        if ( slot == c->nphoto )
        {
            c->nphoto++;
        }
    }
}

/** @brief A JPEG of @p src, kept as blob @p name; false with no room or no encode. */
static bool td_blob_jpeg(td_cam_t *c, const char *name, const uint16_t *src, uint32_t w, uint32_t h, uint32_t q)
{
    uint32_t len = 0U;
    bool ok = false;

    /* Encoded straight into the pool: AO_Display copies the JPEG there. */
    if ( (TD_BLOBS_MAX > c->nblob)
        && (td_jpeg_encode(src, w, h, q, &c->blob_pool[c->blob_used], TD_BLOB_POOL - c->blob_used, &len)
            == KODE_OK) )
    {
        td_blob_t *b = &c->blob[c->nblob];

        b->data = &c->blob_pool[c->blob_used];
        b->len  = len;
        (void)snprintf(b->name, sizeof(b->name), "%s", name);
        c->blob_used += len;
        c->nblob++;
        ok = true;
    }
    return ok;
}

/**
 * @brief A crop of the window photo @p src, scaled by nearest neighbour to tw x th, as blob @p name.
 *
 * The crop is built in the luma plane's room (free by then), and an edge
 * outside the photo repeats the edge.
 */
static void td_blob_crop(td_cam_t *c, const char *name, const uint16_t *src, int32_t cx, int32_t cy,
                         int32_t cw, int32_t ch, uint32_t tw, uint32_t th, uint32_t q)
{
    uint16_t *t = (uint16_t *)(void *)c->wk.d;

    for ( uint32_t y = 0U; y < th; y++ )
    {
        int32_t sy = cy + (((int32_t)y * ch) / (int32_t)th);
        if ( 0 > sy )
        {
            sy = 0;
        }
        if ( (int32_t)TD_WIN_H <= sy )
        {
            sy = (int32_t)TD_WIN_H - 1;
        }
        for ( uint32_t x = 0U; x < tw; x++ )
        {
            int32_t sx = cx + (((int32_t)x * cw) / (int32_t)tw);
            if ( 0 > sx )
            {
                sx = 0;
            }
            if ( (int32_t)TD_WIN_W <= sx )
            {
                sx = (int32_t)TD_WIN_W - 1;
            }
            t[((size_t)y * tw) + x] = src[((size_t)sy * TD_WIN_W) + (size_t)sx];
        }
    }
    (void)td_blob_jpeg(c, name, t, tw, th, q);
}

/** @brief The free band of photo @p f into the colours strip's slot @p slot. */
static void td_strip_fill(td_cam_t *c, const uint16_t *f, uint32_t slot)
{
    for ( uint32_t y = 0U; TD_STRIP_H > y; y++ )
    {
        for ( uint32_t x = 0U; TD_STRIP_W > x; x++ )
        {
            double u;
            double v;
            int32_t iu;
            int32_t iv;

            tdan_h_apply(&c->h, ((double)x + TD_HALF) * (double)TDAN_PW / (double)TD_STRIP_W,
                         (double)TDAN_B0 + ((((double)y + TD_HALF) * (double)(TDAN_B1 - TDAN_B0)) / (double)TD_STRIP_H),
                         &u, &v);
            iu = (int32_t)u;
            iv = (int32_t)v;
            if ( 0 > iu )
            {
                iu = 0;
            }
            if ( 0 > iv )
            {
                iv = 0;
            }
            if ( (int32_t)TD_WIN_W <= iu )
            {
                iu = (int32_t)TD_WIN_W - 1;
            }
            if ( (int32_t)TD_WIN_H <= iv )
            {
                iv = (int32_t)TD_WIN_H - 1;
            }
            c->strip[((size_t)y * TD_STRIP_SLOTS * TD_STRIP_W) + ((size_t)slot * TD_STRIP_W) + x]
                = f[((size_t)iv * TD_WIN_W) + (size_t)iu];
        }
    }
}

/* ---- The steps ----------------------------------------------------------------- */

/** @brief Panel to window, as the golden photo of 28 October 2026 found it. */
static const tdan_h_t H_NOMINAL = { { 2.13662459, 0.0277076, 90.13618311, -0.00851681, 2.09238792,
                                      -147.4912306, 6.212e-05, -1.692e-05, 1.0 } };

/** @brief The exposures the marks are looked for at, percent of the nominal one. */
static const uint32_t MARK_TRY_PCT[TD_MARK_TRIES] = { 50U, 25U, 200U };

/** @brief Look for the marks in the luma of @p f. */
static void td_locate(td_cam_t *c, const uint16_t *f)
{
    double found[TDAN_MARK_COORDS];
    uint32_t const t0 = khal_time_ms();

    tdan_luma(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, c->luma);
    c->located = tdan_locate_marks(c->luma, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &H_NOMINAL, &c->h, found,
                                   &c->missing, &c->nmarks, &c->rescued);
    c->an_ms += khal_time_ms() - t0;
}

/** @brief Where the screen sits, from the located marks: the offset of its centre from the array's, mm. */
static void td_offset(td_cam_t *c, double *ang)
{
    double scale = 0.0;
    double persp = 0.0;
    double cu = 0.0;
    double cv = 0.0;

    tdan_pose(&c->h, ang, &scale, &persp);
    tdan_h_apply(&c->h, TD_PANEL_CX, TD_PANEL_CY, &cu, &cv);
    c->dx_mm = 0.0;
    c->dy_mm = 0.0;
    if ( 0.0 < scale )
    {
        c->dx_mm = ((((double)c->win_x + cu) - TD_SENSOR_CX) / scale) * TD_MM_PER_PANEL_PX;
        c->dy_mm = ((((double)c->win_y + cv) - TD_SENSOR_CY) / scale) * TD_MM_PER_PANEL_PX;
    }
}

/** @brief The marks: located, at the nominal exposure first, then at the others. */
static kode_status_t td_step_marks(td_cam_t *c)
{
    const uint16_t *f;
    uint8_t t = 0U;

    c->auto_pct = TD_PCT;
    f = td_take_photo(c, TD_EXP_BLACK, TD_SKIP_PATTERN);
    if ( NULL == f )
    {
        return KODE_TIMEOUT;
    }
    td_locate(c, f);
    while ( !c->located && (TD_MARK_TRIES > t) )
    {
        td_drop();
        c->auto_pct = MARK_TRY_PCT[t];
        f = td_take_photo(c, TD_EXP_BLACK, TD_SKIP_PATTERN);
        if ( NULL == f )
        {
            return KODE_TIMEOUT;
        }
        td_locate(c, f);
        t++;
    }
    if ( !c->located )
    {
        /* None found anything: the photo kept is the nominal one. */
        td_drop();
        c->auto_pct = TD_PCT;
        f = td_take_photo(c, TD_EXP_BLACK, TD_SKIP_PATTERN);
        if ( NULL == f )
        {
            return KODE_TIMEOUT;
        }
        tdan_luma(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, c->luma);
        c->h = H_NOMINAL;
    }
    c->partial = c->located && (TDAN_MARK_COUNT > c->nmarks);
    (void)memcpy(c->keep_b, f, TD_WIN_BYTES);
    (void)memcpy(c->lj[0], c->luma, TD_WIN_PX);
    td_keep(c, "black_marks", f, TD_WIN_W, TD_WIN_H);
    td_drop();
    /* The location: the kept photo is already the window at a third, as the station shows it. */
    if ( 0U < c->nphoto )
    {
        const td_photo_t *p = &c->photo[c->nphoto - 1U];
        (void)td_blob_jpeg(c, "location", p->px, p->w, p->h, TD_LOC_QUALITY);
    }
    return KODE_OK;
}

/** @brief The black with the panel off: the dark, and the black net of it for the stuck-on pixels. */
static kode_status_t td_step_off(td_cam_t *c)
{
    const uint16_t *f = td_take_photo(c, TD_EXP_BLACK, TD_SKIP_PATTERN);
    uint32_t got = 1U;
    double c0[TD_CH];

    if ( NULL == f )
    {
        return KODE_TIMEOUT;
    }
    c->eoff = (double)c->last_exp;
    tdan_luma(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, c->lb);
    tdan_colour(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h, c0);
    td_keep(c, "panel_off", f, TD_WIN_W, TD_WIN_H);
    for ( size_t q = 0U; TD_WIN_PX > q; q++ )
    {
        int32_t const d = (int32_t)c->lj[0][q] - (int32_t)c->lb[q];
        uint8_t v = 0U;
        if ( 0 < d )
        {
            v = (uint8_t)d;
        }
        c->lj[0][q] = v;
    }
    for ( uint32_t k = 0U; TD_CH > k; k++ )
    {
        c->doff[k] = c0[k];
    }
    td_drop();
    /* One frame is noisy: the dark is the mean of a few. */
    for ( uint32_t n = 1U; TD_DARK_FRAMES > n; n++ )
    {
        const uint8_t *g = NULL;
        size_t len = 0U;
        if ( kdrv_ov5640_grab(0U, TD_GRAB_MS, &g, &len) == KODE_OK )
        {
            tdan_colour((const uint16_t *)(const void *)g, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h, c0);
            for ( uint32_t k = 0U; TD_CH > k; k++ )
            {
                c->doff[k] += c0[k];
            }
            got++;
            td_drop();
        }
    }
    for ( uint32_t k = 0U; TD_CH > k; k++ )
    {
        c->doff[k] /= (double)got;
    }
    return KODE_OK;
}

/** @brief The brightest channel. */
static double td_level(const double rgb[TD_CH])
{
    return fmax(rgb[0], fmax(rgb[1], rgb[2]));
}

/** @brief @p raw less the dark of exposure @p e. */
static void td_dark_net(const td_cam_t *c, const double raw[TD_CH], double e, double out[TD_CH])
{
    for ( uint32_t k = 0U; TD_CH > k; k++ )
    {
        double d = 0.0;
        if ( 0.0 < c->eoff )
        {
            d = (c->doff[k] * e) / c->eoff;
        }
        out[k] = fmax(0.0, raw[k] - d);
    }
}

/** @brief A colour step's fixed facts. */
typedef struct {
    const char *name;     /**< The kept photo's name.          */
    uint32_t    exp;      /**< Its nominal exposure, lines.    */
    double      ref;      /**< Its reference level.            */
    uint32_t    slot;     /**< Its slot in the colours strip.  */
    uint8_t     ci;       /**< Its TDAN_* colour index.        */
} td_colour_step_t;

/** @brief White, grey, red, green, blue, by td_cam_step_t from TD_CS_WHITE. */
static const td_colour_step_t COLOUR_STEP[TDAN_COLOURS] = {
    { "white", TD_EXP_WHITE,  TD_REF_WHITE, 3U, (uint8_t)TDAN_WHITE },
    { "grey",  TD_EXP_GREY,   TD_REF_GREY,  4U, (uint8_t)TDAN_GREY  },
    { "red",   TD_EXP_COLOUR, TD_REF_RED,   0U, (uint8_t)TDAN_RED   },
    { "green", TD_EXP_COLOUR, TD_REF_GREEN, 1U, (uint8_t)TDAN_GREEN },
    { "blue",  TD_EXP_COLOUR, TD_REF_BLUE,  2U, (uint8_t)TDAN_BLUE  },
};

/** @brief The colour photo, rescued by retakes when its level is far from the reference. */
static const uint16_t *td_colour_photo(td_cam_t *c, const td_colour_step_t *s)
{
    uint32_t base = s->exp;
    uint32_t skip = TD_SKIP_PATTERN;
    const uint16_t *f;
    double net[TD_CH];
    double ratio;
    uint32_t exp2;
    bool again_same = true;

    if ( ((uint8_t)TDAN_GREEN == s->ci) || ((uint8_t)TDAN_BLUE == s->ci) )
    {
        base = (uint32_t)(((double)s->exp * c->colscale) + TD_HALF);
        if ( 1.0 == c->colscale )
        {
            skip = TD_SKIP_COLOUR;
        }
    }
    f = td_take_photo(c, base, skip);
    if ( NULL == f )
    {
        return NULL;
    }
    tdan_colour(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h, c->rgb[s->ci]);
    c->eexp[s->ci] = (double)c->last_exp;
    td_dark_net(c, c->rgb[s->ci], c->eexp[s->ci], net);
    ratio = td_level(net) / s->ref;
    exp2 = base;
    for ( uint32_t it = 0U; (TD_COLOUR_TRIES > it) && ((TD_RATIO_LOW > ratio) || (TD_RATIO_HIGH < ratio)); it++ )
    {
        /* Far from the pattern's own level, it is first a frame caught in the pattern change: the same exposure again. */
        bool const transient = again_same && ((TD_RATIO_SAME_LOW > ratio) || (TD_RATIO_SAME_HIGH < ratio));
        uint32_t sk = TD_SKIP_PATTERN;

        td_drop();
        again_same = false;
        if ( transient )
        {
            sk = TD_SKIP_PATTERN + TD_SKIP_TRANSIENT;
        }
        else
        {
            double r2 = ratio;
            if ( td_level(c->rgb[s->ci]) >= TD_CLIP_LEVEL )
            {
                r2 = ratio * TD_CLIP_FACTOR;
            }
            if ( TD_RATIO_FLOOR > r2 )
            {
                r2 = TD_RATIO_FLOOR;
            }
            exp2 = (uint32_t)((double)exp2 / r2);
            if ( TD_EXP_MIN > exp2 )
            {
                exp2 = TD_EXP_MIN;
            }
            if ( TD_EXP_MAX < exp2 )
            {
                exp2 = TD_EXP_MAX;
            }
        }
        f = td_take_photo(c, exp2, sk);
        if ( NULL == f )
        {
            return NULL;
        }
        tdan_colour(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h, c->rgb[s->ci]);
        c->eexp[s->ci] = (double)c->last_exp;
        td_dark_net(c, c->rgb[s->ci], c->eexp[s->ci], net);
        ratio = td_level(net) / s->ref;
        c->retake[s->ci] = true;
    }
    if ( c->retake[s->ci] && ((uint8_t)TDAN_RED == s->ci) )
    {
        c->colscale = (double)exp2 / (double)s->exp;
    }
    return f;
}

/** @brief One colour: white, grey, red, green or blue. White and grey are analysed for pixels, lines and zones. */
static kode_status_t td_step_colour(td_cam_t *c, uint8_t step)
{
    const td_colour_step_t *s = &COLOUR_STEP[step - (uint8_t)TD_CS_WHITE];
    const uint16_t *f;

    if ( !c->located )
    {
        /* No place known: the photo is kept for the station and nothing is measured. */
        f = td_take_photo(c, s->exp, TD_SKIP_PATTERN);
        if ( NULL == f )
        {
            return KODE_TIMEOUT;
        }
        td_keep(c, s->name, f, TD_WIN_W, TD_WIN_H);
        td_drop();
        return KODE_OK;
    }
    f = td_colour_photo(c, s);
    if ( NULL == f )
    {
        return KODE_TIMEOUT;
    }
    c->shot[s->ci] = true;
    td_strip_fill(c, f, s->slot);
    if ( ((uint8_t)TDAN_WHITE == s->ci) || ((uint8_t)TDAN_GREY == s->ci) )
    {
        uint8_t * const lj = c->lj[1U + s->ci];
        double sat_scale = 1.0;
        uint32_t t0;

        tdan_luma(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, lj);
        for ( uint32_t rt = 0U; TD_SAT_TRIES > rt; rt++ )
        {
            uint32_t nsat = 0U;
            for ( size_t q = 0U; TD_WIN_PX > q; q += 2U )
            {
                if ( TD_SAT_LUMA <= lj[q] )
                {
                    nsat++;
                }
            }
            if ( TD_SAT_SAMPLES <= nsat )
            {
                /* Too bright to judge pixels: again with less light. */
                const uint16_t *f5;
                sat_scale *= TD_SAT_SCALE;
                td_drop();
                f5 = td_take_photo(c, (uint32_t)(((double)s->exp * sat_scale) + TD_HALF), TD_SKIP_PATTERN);
                if ( NULL == f5 )
                {
                    return KODE_TIMEOUT;
                }
                f = f5;
                tdan_luma(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, lj);
                tdan_colour(f, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h, c->rgb[s->ci]);
                c->eexp[s->ci] = (double)c->last_exp;
            }
        }
        if ( (uint8_t)TDAN_WHITE == s->ci )
        {
            (void)memcpy(c->keep_w, f, TD_WIN_BYTES);
        }
        td_keep(c, s->name, f, TD_WIN_W, TD_WIN_H);
        td_drop();
        t0 = khal_time_ms();
        tdan_work_new_photo(&c->wk);
        tdan_analyse_photo(lj, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h, &c->wk, (int32_t)s->ci, &c->pix,
                           &c->ph[s->ci]);
        if ( ((uint8_t)TDAN_GREY == s->ci) && c->located && !c->partial && (0.0 < c->eoff) )
        {
            tdan_work_new_photo(&c->wk);
            c->nstuck = tdan_pix_stuck(c->lj[0], (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h, &c->wk, &c->pix,
                                       c->stuck, (int32_t)TDAN_MAXP);
        }
        c->an_ms += khal_time_ms() - t0;
    }
    else
    {
        td_keep(c, s->name, f, TD_WIN_W, TD_WIN_H);
        td_drop();
    }
    return KODE_OK;
}

/** @brief The verdicts of the screen: framing, pixels, lines, zones, colours; and the blobs that show them. */
static kode_status_t td_step_judge(td_cam_t *c)
{
    tdan_screen_in_t si;
    tdan_colour_in_t ci;
    double ang = 0.0;
    uint32_t const t0 = khal_time_ms();
    char name[KODE_TEST_DOT_NAME_MAX];

    tdan_judge_frame(&c->h, c->located, c->nmarks, (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->frame);
    td_offset(c, &ang);
    (void)memset(&c->screen, 0, sizeof(c->screen));
    (void)memset(&c->colour, 0, sizeof(c->colour));
    c->colour.colour = TDAN_NO_VERDICT;
    c->screen.pixels = TDAN_NO_VERDICT;
    c->screen.lines  = TDAN_NO_VERDICT;
    c->screen.zones  = TDAN_NO_VERDICT;
    if ( c->located && !c->partial )
    {
        (void)memset(&si, 0, sizeof(si));
        si.pix = &c->pix;
        if ( c->shot[TDAN_WHITE] )
        {
            si.white = &c->ph[TDAN_WHITE];
        }
        if ( c->shot[TDAN_GREY] )
        {
            si.grey = &c->ph[TDAN_GREY];
        }
        si.stuck  = c->stuck;
        si.nstuck = c->nstuck;
        (void)memset(&ci, 0, sizeof(ci));
        for ( uint32_t i = 0U; TDAN_COLOURS > i; i++ )
        {
            ci.ok[i]     = c->shot[i];
            ci.retake[i] = c->retake[i];
            td_dark_net(c, c->rgb[i], c->eexp[i], ci.rgb[i]);
            if ( c->retake[i] && ((uint32_t)TDAN_WHITE != i) )
            {
                si.retakes++;
            }
        }
        ci.exp_ratio = 1.0;
        {
            double const nominal = ((double)TD_EXP_WHITE * (double)c->auto_pct) / (double)TD_PCT;
            if ( (0.0 < nominal) && (0.0 < c->eexp[TDAN_WHITE]) )
            {
                ci.exp_ratio = c->eexp[TDAN_WHITE] / nominal;
            }
        }
        c->ripple = tdan_ripple(c->lj[1], (int32_t)TD_WIN_W, (int32_t)TD_WIN_H, &c->h);
        ci.ripple = (double)c->ripple;
        tdan_judge_screen(&si, &c->screen);
        tdan_judge_colour(&ci, &c->colour);
        for ( uint32_t i = 0U; TDAN_COLOURS > i; i++ )
        {
            (void)memcpy(c->rgb[i], ci.rgb[i], sizeof(c->rgb[i]));
        }
        (void)td_blob_jpeg(c, "colours", c->strip, TD_STRIP_SLOTS * TD_STRIP_W, TD_STRIP_H, TD_STRIP_QUALITY);
        for ( int32_t i = 0; (i < c->screen.ndead) && ((int32_t)TD_CROPS_MAX > i); i++ )
        {
            double u;
            double v;
            tdan_h_apply(&c->h, (double)c->screen.dead[i].x + TD_HALF, (double)c->screen.dead[i].y + TD_HALF, &u, &v);
            (void)snprintf(name, sizeof(name), "dead_%d_%d", (int)c->screen.dead[i].x, (int)c->screen.dead[i].y);
            td_blob_crop(c, name, c->keep_w, (int32_t)u - TD_CROP_HALF, (int32_t)v - TD_CROP_HALF,
                         (int32_t)TD_CROP_PX, (int32_t)TD_CROP_PX, TD_CROP_PX, TD_CROP_PX, TD_CROP_QUALITY);
        }
        for ( int32_t i = 0; (i < c->screen.nstuck) && ((int32_t)TD_CROPS_MAX > i); i++ )
        {
            double u;
            double v;
            tdan_h_apply(&c->h, (double)c->screen.stuck[i].x + TD_HALF, (double)c->screen.stuck[i].y + TD_HALF, &u, &v);
            (void)snprintf(name, sizeof(name), "on_%d_%d", (int)c->screen.stuck[i].x, (int)c->screen.stuck[i].y);
            td_blob_crop(c, name, c->keep_b, (int32_t)u - TD_CROP_HALF, (int32_t)v - TD_CROP_HALF,
                         (int32_t)TD_CROP_PX, (int32_t)TD_CROP_PX, TD_CROP_PX, TD_CROP_PX, TD_CROP_QUALITY);
        }
        for ( int32_t z = 0; (z < c->screen.nzones) && ((int32_t)TD_ZONE_CROPS > z); z++ )
        {
            const tdan_zone_t *zn = &c->screen.zone[z];
            double u0;
            double v0;
            double u1;
            double v1;
            tdan_h_apply(&c->h, (double)zn->x0, (double)zn->y0, &u0, &v0);
            tdan_h_apply(&c->h, (double)zn->x1, (double)zn->y1, &u1, &v1);
            (void)snprintf(name, sizeof(name), "zone_%d_%d", (int)zn->x0, (int)zn->y0);
            td_blob_crop(c, name, c->keep_w, (int32_t)u0 - TD_ZONE_MARGIN, (int32_t)v0 - TD_ZONE_MARGIN,
                         (int32_t)(u1 - u0) + (2 * TD_ZONE_MARGIN), (int32_t)(v1 - v0) + (2 * TD_ZONE_MARGIN),
                         TD_ZONE_CROP_W, TD_ZONE_CROP_H, TD_ZONE_QUALITY);
        }
    }
    c->an_ms += khal_time_ms() - t0;
    return KODE_OK;
}

/** @brief The LED photos' names, off, red, green, blue. */
static const char *const LED_PHOTO[TD_LED_PHOTOS] = { "led_off", "led_red", "led_green", "led_blue" };

/** @brief One LED photo, in the whole field, into a buffer that is free by then. */
static kode_status_t td_step_led(td_cam_t *c, uint8_t k)
{
    uint16_t * const room[TD_LED_PHOTOS] = { c->keep_w, c->keep_b, (uint16_t *)(void *)c->wk.a,
                                             (uint16_t *)(void *)c->wk.b };
    const uint16_t *f;
    uint32_t const t0 = khal_time_ms();
    kode_status_t rc = td_open_field(c);

    if ( KODE_OK != rc )
    {
        return rc;
    }
    c->auto_pct = TD_PCT;
    f = td_take_photo(c, TD_EXP_LED, TD_SKIP_LED);
    if ( NULL == f )
    {
        c->led[k] = NULL;
        return KODE_TIMEOUT;
    }
    (void)memcpy(room[k], f, TD_FIELD_BYTES);
    c->led[k] = room[k];
    td_keep(c, LED_PHOTO[k], f, TD_FIELD_W, TD_FIELD_H);
    td_drop();
    c->led_ms += khal_time_ms() - t0;
    return KODE_OK;
}

/** @brief A pixel's three channels, each on 0..255. */
static void td_rgb(uint16_t a, int32_t out[TD_CH])
{
    out[0] = (int32_t)(((uint32_t)a >> TD_R_SHIFT) & TD_R_MASK) * TD_R_W;
    out[1] = (int32_t)(((uint32_t)a >> TD_G_SHIFT) & TD_G_MASK) * TD_G_W;
    out[2] = (int32_t)((uint32_t)a & TD_B_MASK) * TD_B_W;
}

/** @brief The LED's verdict: where it is, then each colour's light and how pure it is. */
static kode_status_t td_step_led_judge(td_cam_t *c)
{
    uint32_t const t0 = khal_time_ms();
    uint32_t const bw = TD_FIELD_W / TD_LED_BLOCK;
    uint32_t const bh = TD_FIELD_H / TD_LED_BLOCK;
    uint32_t *blk = (uint32_t *)(void *)c->wk.c;
    uint32_t best = 0U;
    int32_t bx = 0;
    int32_t by = 0;
    bool sat = false;
    bool allok = true;

    c->led_verdict = (uint8_t)TDAN_NO_VERDICT;
    c->led_why = "no_frames";
    for ( uint32_t k = 0U; TD_LED_PHOTOS > k; k++ )
    {
        if ( NULL == c->led[k] )
        {
            return KODE_OK;
        }
    }
    for ( uint32_t k = 1U; (TD_LED_PHOTOS > k) && !sat; k++ )
    {
        uint32_t nsat = 0U;
        for ( size_t q = 0U; TD_FIELD_PX > q; q += TD_LED_SAT_STEP )
        {
            uint32_t const a = c->led[k][q];
            if ( (((a >> TD_R_SHIFT) & TD_R_MASK) >= TD_R_MASK) || (((a >> TD_G_SHIFT) & TD_G_MASK) >= TD_G_MASK)
                || ((a & TD_B_MASK) >= TD_B_MASK) )
            {
                nsat++;
            }
        }
        if ( TD_LED_SAT_MAX < nsat )
        {
            sat = true;
        }
    }
    if ( sat )
    {
        c->led_why = "saturated";
        c->led_ms += khal_time_ms() - t0;
        return KODE_OK;
    }
    /* Where the LED is: the block with the largest brightening over the three colours, and its neighbours. */
    (void)memset(blk, 0, (size_t)bw * bh * sizeof(uint32_t));
    for ( uint32_t k = 1U; TD_LED_PHOTOS > k; k++ )
    {
        for ( uint32_t y = 0U; TD_FIELD_H > y; y++ )
        {
            for ( uint32_t x = 0U; TD_FIELD_W > x; x++ )
            {
                int32_t a[TD_CH];
                int32_t b[TD_CH];
                int32_t d;
                td_rgb(c->led[k][((size_t)y * TD_FIELD_W) + x], a);
                td_rgb(c->led[0][((size_t)y * TD_FIELD_W) + x], b);
                d = (a[0] + a[1] + a[2]) - (b[0] + b[1] + b[2]);
                if ( 0 < d )
                {
                    blk[((y / TD_LED_BLOCK) * bw) + (x / TD_LED_BLOCK)] += (uint32_t)d;
                }
            }
        }
        td_yield();
    }
    for ( uint32_t y = 1U; (bh - 1U) > y; y++ )
    {
        for ( uint32_t x = 1U; (bw - 1U) > x; x++ )
        {
            uint32_t s3 = 0U;
            for ( uint32_t dy = 0U; TD_LED_NEIGH > dy; dy++ )
            {
                for ( uint32_t dx = 0U; TD_LED_NEIGH > dx; dx++ )
                {
                    s3 += blk[(((y + dy) - 1U) * bw) + ((x + dx) - 1U)];
                }
            }
            if ( s3 > best )
            {
                best = s3;
                bx = (int32_t)((x * TD_LED_BLOCK) + (TD_LED_BLOCK / 2U));
                by = (int32_t)((y * TD_LED_BLOCK) + (TD_LED_BLOCK / 2U));
            }
        }
    }
    if ( TD_LED_R > bx )
    {
        bx = TD_LED_R;
    }
    if ( TD_LED_R > by )
    {
        by = TD_LED_R;
    }
    if ( ((int32_t)TD_FIELD_W - TD_LED_R) < bx )
    {
        bx = (int32_t)TD_FIELD_W - TD_LED_R;
    }
    if ( ((int32_t)TD_FIELD_H - TD_LED_R) < by )
    {
        by = (int32_t)TD_FIELD_H - TD_LED_R;
    }
    c->led_x = bx;
    c->led_y = by;
    for ( uint32_t k = 1U; TD_LED_PHOTOS > k; k++ )
    {
        double acc[TD_CH] = { 0.0, 0.0, 0.0 };
        double sum;
        double dist = 0.0;

        for ( int32_t y = by - TD_LED_R; (by + TD_LED_R) > y; y++ )
        {
            for ( int32_t x = bx - TD_LED_R; (bx + TD_LED_R) > x; x++ )
            {
                int32_t a[TD_CH];
                int32_t b[TD_CH];
                td_rgb(c->led[k][((size_t)y * TD_FIELD_W) + (size_t)x], a);
                td_rgb(c->led[0][((size_t)y * TD_FIELD_W) + (size_t)x], b);
                for ( uint32_t ch = 0U; TD_CH > ch; ch++ )
                {
                    int32_t const d = a[ch] - b[ch];
                    if ( 0 < d )
                    {
                        acc[ch] += (double)d;
                    }
                }
            }
        }
        sum = acc[0] + acc[1] + acc[2];
        for ( uint32_t ch = 0U; TD_CH > ch; ch++ )
        {
            double share = 0.0;
            double want = 0.0;
            if ( 0.0 < sum )
            {
                share = acc[ch] / sum;
            }
            if ( (k - 1U) == ch )
            {
                want = 1.0;
            }
            dist += fabs(share - want);
        }
        /* As if taken at the nominal exposure. */
        c->led_flux[k] = ((sum / (double)TD_CH) * TD_LED_NOMINAL) / (double)TD_EXP_LED;
        c->led_dist[k] = dist;
        if ( !((TD_LED_MIN_FLUX < c->led_flux[k]) && (TD_LED_MAX_DIST > dist)) )
        {
            allok = false;
        }
    }
    c->led_verdict = (uint8_t)TDAN_FAIL;
    if ( allok )
    {
        c->led_verdict = (uint8_t)TDAN_PASS;
    }
    c->led_why = NULL;
    c->led_ms += khal_time_ms() - t0;
    return KODE_OK;
}

static void td_judge_records(td_cam_t *c);
static void td_led_record(td_cam_t *c);

kode_status_t td_cam_step(td_cam_t *c, uint8_t step)
{
    kode_status_t rc = KODE_OK;

    c->ncrec = 0U;
    tdan_set_yield(&td_yield);
    /* The verdicts take no photo: they run on a camera that would not open. */
    if ( (uint8_t)TD_CS_JUDGE > step )
    {
        rc = td_open_window(c);
    }
    if ( KODE_OK != rc )
    {
        return KODE_ERROR;
    }
    switch ( step )
    {
        case (uint8_t)TD_CS_MARKS:
            rc = td_step_marks(c);
            break;
        case (uint8_t)TD_CS_OFF:
            rc = KODE_OK;
            if ( c->located )
            {
                rc = td_step_off(c);
            }
            break;
        case (uint8_t)TD_CS_WHITE:
        case (uint8_t)TD_CS_GREY:
        case (uint8_t)TD_CS_RED:
        case (uint8_t)TD_CS_GREEN:
        case (uint8_t)TD_CS_BLUE:
            rc = td_step_colour(c, step);
            break;
        case (uint8_t)TD_CS_JUDGE:
            rc = td_step_judge(c);
            td_judge_records(c);
            break;
        case (uint8_t)TD_CS_LED_OFF:
        case (uint8_t)TD_CS_LED_RED:
        case (uint8_t)TD_CS_LED_GREEN:
        case (uint8_t)TD_CS_LED_BLUE:
            rc = td_step_led(c, (uint8_t)(step - (uint8_t)TD_CS_LED_OFF));
            break;
        case (uint8_t)TD_CS_LED_JUDGE:
            rc = td_step_led_judge(c);
            td_led_record(c);
            break;
        default:
            rc = KODE_DENIED;
            break;
    }
    return rc;
}

kode_status_t td_cam_place(td_cam_t *c)
{
    double ang = 0.0;
    kode_status_t rc;

    tdan_set_yield(&td_yield);
    rc = td_open_window(c);
    if ( KODE_OK != rc )
    {
        return KODE_ERROR;
    }
    c->auto_pct = TD_PCT;
    {
        const uint16_t *f = td_take_photo(c, TD_EXP_BLACK, TD_SKIP_PATTERN);
        if ( NULL == f )
        {
            return KODE_TIMEOUT;
        }
        td_locate(c, f);
        c->place_ok = c->located && (TDAN_MARK_COUNT <= c->nmarks);
        c->place_ang = 0.0;
        c->place_dx = 0.0;
        c->place_dy = 0.0;
        if ( c->located )
        {
            td_offset(c, &ang);
            c->place_ang = ang;
            c->place_dx = c->dx_mm;
            c->place_dy = c->dy_mm;
        }
        if ( c->place_ok )
        {
            /* The placement's photo is the first of the pass's, and its location the station's first picture. */
            c->nphoto = 0U;
            c->nblob = 0U;
            c->blob_used = 0U;
            td_keep(c, "place", f, TD_WIN_W, TD_WIN_H);
            c->placed = true;
            (void)td_blob_jpeg(c, "location", c->photo[0].px, c->photo[0].w, c->photo[0].h, TD_LOC_QUALITY);
        }
        td_drop();
    }
    return KODE_OK;
}

kode_status_t td_cam_photo_out(td_cam_t *c, uint8_t i)
{
    uint32_t len = 0U;
    kode_status_t rc;

    c->jout_len = 0U;
    if ( (i >= c->nphoto) || (NULL == c->photo[i].px) )
    {
        return KODE_DENIED;
    }
    rc = td_jpeg_encode(c->photo[i].px, c->photo[i].w, c->photo[i].h, TD_PHOTO_QUALITY, c->jout, TD_JOUT_MAX, &len);
    if ( KODE_OK == rc )
    {
        c->jout_len = len;
    }
    return rc;
}

/* ---- The records ----------------------------------------------------------------- */

/** @brief Keep record @p text of part @p part for the AO; past TD_CREC_MAX it is dropped, logged. */
static void td_crec_put(td_cam_t *c, uint8_t part, const char *text)
{
    if ( (uint8_t)TD_CREC_MAX > c->ncrec )
    {
        td_crec_t * const r = &c->crec[c->ncrec];
        size_t n = strlen(text);

        if ( sizeof(r->text) <= n )
        {
            n = sizeof(r->text) - 1U;
        }
        (void)memcpy(r->text, text, n);
        r->text[n] = '\0';
        r->part    = part;
        c->ncrec++;
    }
    else
    {
        KLOG_E(TAG, "camera: no room for a record; dropped: %s", text);
    }
}

/** @brief A verdict's word. */
static const char *td_word(tdan_verdict_t v)
{
    const char *w = "noverdict";

    if ( TDAN_PASS == v )
    {
        w = "pass";
    }
    else if ( TDAN_FAIL == v )
    {
        w = "fail";
    }
    else
    {
        /* no verdict */
    }
    return w;
}

/** @brief 1 or 0. */
static unsigned td_bit(bool b)
{
    unsigned v = 0U;

    if ( b )
    {
        v = 1U;
    }
    return v;
}

/**
 * @brief Append ` at="<list>"` to record @p r, the list cut after its last
 *        whole entry (`;`) when the record has no room for all of it.
 * @param r    The record so far.
 * @param cap  Its room.
 * @param list The entries, each ending in `;`.
 */
static void td_rec_at(char *r, size_t cap, const char *list)
{
    static const char head[] = " at=\"";
    size_t const used = strlen(r);
    size_t n = strlen(list);

    if ( (used + (sizeof(head) - 1U) + 2U) > cap )
    {
        return;
    }
    if ( (used + (sizeof(head) - 1U) + n + 2U) > cap )
    {
        n = cap - used - (sizeof(head) - 1U) - 2U;
        while ( (0U < n) && (';' != list[n - 1U]) )
        {
            n--;
        }
    }
    (void)memcpy(&r[used], head, sizeof(head) - 1U);
    (void)memcpy(&r[used + (sizeof(head) - 1U)], list, n);
    r[used + (sizeof(head) - 1U) + n]      = '"';
    r[used + (sizeof(head) - 1U) + n + 1U] = '\0';
}

/** @brief The camera part's verdicts as records, in the order the station reads them, after TD_CS_JUDGE. */
static void td_judge_records(td_cam_t *c)
{
    char r[KODE_FUNCTION_REC_MAX];
    char list[TD_LIST_MAX];
    const char *why = "";

    if ( !c->located )
    {
        why = " why=marks_not_found_in_field";
    }
    else if ( c->partial )
    {
        why = " why=mark_cut_by_window";
    }
    else
    {
        /* located */
    }
    (void)snprintf(r, sizeof(r),
                   "test=cam_framing result=%s marks=%d inside=%u dx_mm=%.2f dy_mm=%.2f scale=%.3f nominal=%u"
                   " min_marks=%d rescued=%u recentred=0 win=\"%u,%u\"%s",
                   td_word(c->frame.framing), (int)c->nmarks, td_bit(c->frame.inside), c->dx_mm, c->dy_mm,
                   c->frame.scale, td_bit(!c->rescued), TDAN_MARK_COUNT, td_bit(c->rescued),
                   (unsigned)c->win_x, (unsigned)c->win_y, why);
    td_crec_put(c, (uint8_t)TEST_DOT_CAM_FRAMING, r);
    (void)snprintf(r, sizeof(r),
                   "test=cam_straight result=%s angle_deg=%.2f persp_pct=%.1f max_angle=%d max_persp_pct=%d",
                   td_word(c->frame.straight), c->frame.angle_deg, c->frame.persp * (double)TD_PCT,
                   (int)TDAN_MAX_ANGLE_DEG, (int)(TDAN_MAX_PERSP * (double)TD_PCT));
    td_crec_put(c, (uint8_t)TEST_DOT_CAM_FRAMING, r);
    if ( !c->located || c->partial )
    {
        td_crec_put(c, (uint8_t)TEST_DOT_CAM_PIXELS, "test=cam_pixels result=noverdict why=not_located");
        td_crec_put(c, (uint8_t)TEST_DOT_CAM_LINES, "test=cam_lines result=noverdict why=not_located");
        td_crec_put(c, (uint8_t)TEST_DOT_CAM_ZONES, "test=cam_zones result=noverdict why=not_located");
        td_crec_put(c, (uint8_t)TEST_DOT_CAM_COLOUR, "test=cam_colour result=noverdict why=not_located");
        return;
    }
    /* Pixels. */
    if ( TDAN_PIX_OK != c->screen.pix_status )
    {
        const char *w = "no_reference";
        if ( TDAN_PIX_SATURATED == c->screen.pix_status )
        {
            w = "saturated";
        }
        else if ( TDAN_PIX_NOISY == c->screen.pix_status )
        {
            w = "noise";
        }
        else
        {
            /* no reference */
        }
        (void)snprintf(r, sizeof(r), "test=cam_pixels result=noverdict why=%s sat=%.3f,%.3f sigma=%.3f,%.3f", w,
                       (double)c->pix.sat[0], (double)c->pix.sat[1], (double)c->pix.sigmax[0],
                       (double)c->pix.sigmax[1]);
    }
    else
    {
        size_t lp = 0U;
        list[0] = '\0';
        for ( int32_t i = 0; (i < c->screen.ndead) && ((sizeof(list) - TD_LIST_ROOM) > lp); i++ )
        {
            int const n = snprintf(&list[lp], sizeof(list) - lp, "%d,%d;", (int)c->screen.dead[i].x,
                                   (int)c->screen.dead[i].y);
            if ( (0 < n) && ((size_t)n < (sizeof(list) - lp)) )
            {
                lp += (size_t)n;
            }
        }
        for ( int32_t i = 0; (i < c->screen.nstuck) && ((sizeof(list) - TD_LIST_ROOM) > lp); i++ )
        {
            int const n = snprintf(&list[lp], sizeof(list) - lp, "on:%d,%d;", (int)c->screen.stuck[i].x,
                                   (int)c->screen.stuck[i].y);
            if ( (0 < n) && ((size_t)n < (sizeof(list) - lp)) )
            {
                lp += (size_t)n;
            }
        }
        why = "";
        if ( c->screen.streak )
        {
            why = " why=reflection_or_light";
        }
        (void)snprintf(r, sizeof(r), "test=cam_pixels result=%s dead=%d stuck=%d warn=%d max_dead=%d max_stuck=%d%s",
                       td_word(c->screen.pixels), (int)c->screen.ndead, (int)c->screen.nstuck, (int)c->pix.nwarn,
                       TDAN_PIX_ALLOW_DEAD, TDAN_PIX_ALLOW_STUCK, why);
        td_rec_at(r, sizeof(r), list);
    }
    td_crec_put(c, (uint8_t)TEST_DOT_CAM_PIXELS, r);
    for ( int32_t i = 0; (i < c->pix.nwarn) && ((int32_t)TD_WARN_RECORDS > i); i++ )
    {
        (void)snprintf(r, sizeof(r), "pixwarn x=%d y=%d dw=%.2f dg=%.2f", (int)c->pix.warn[i].x,
                       (int)c->pix.warn[i].y, (double)c->pix.warn[i].dw, (double)c->pix.warn[i].dg);
        td_crec_put(c, (uint8_t)TEST_DOT_CAM_PIXELS, r);
    }
    /* Lines. */
    {
        int vx = 0;
        int y0 = 0;
        int y1 = 0;
        int hy = 0;
        unsigned n = 0U;
        if ( c->screen.vline )
        {
            vx = (int)c->screen.vl.pos;
            y0 = (int)c->screen.vl.a0;
            y1 = (int)c->screen.vl.a1;
            n++;
        }
        if ( c->screen.hline )
        {
            hy = (int)c->screen.hl.pos;
            n++;
        }
        (void)snprintf(r, sizeof(r),
                       "test=cam_lines result=%s lines=%u x=%d y0=%d y1=%d row_y=%d level=%.3f hlevel=%.3f max_level=%.2f"
                       " wlevel=%.3f,%.3f whlevel=%.3f,%.3f",
                       td_word(c->screen.lines), n, vx, y0, y1, hy, (double)c->ph[TDAN_WHITE].vl.level,
                       (double)c->ph[TDAN_WHITE].hl.level, (double)TDAN_LINE_C, (double)c->ph[TDAN_WHITE].wvl.level,
                       (double)c->ph[TDAN_GREY].wvl.level, (double)c->ph[TDAN_WHITE].whl.level,
                       (double)c->ph[TDAN_GREY].whl.level);
        td_crec_put(c, (uint8_t)TEST_DOT_CAM_LINES, r);
    }
    /* Zones. */
    {
        size_t zp = 0U;
        double minr = 1.0;
        list[0] = '\0';
        for ( int32_t z = 0; (z < c->screen.nzones) && ((sizeof(list) - TD_LIST_ROOM) > zp); z++ )
        {
            int const n = snprintf(&list[zp], sizeof(list) - zp, "%d,%d-%d,%d;", (int)c->screen.zone[z].x0,
                                   (int)c->screen.zone[z].y0, (int)c->screen.zone[z].x1, (int)c->screen.zone[z].y1);
            if ( (0 < n) && ((size_t)n < (sizeof(list) - zp)) )
            {
                zp += (size_t)n;
            }
        }
        if ( 0 < c->screen.nzones )
        {
            minr = (double)c->screen.zone[0].min_ratio;
        }
        (void)snprintf(r, sizeof(r), "test=cam_zones result=%s zones=%d min_ratio=%.2f limit_ratio=%.2f bright=%d",
                       td_word(c->screen.zones), (int)c->screen.nzones, minr, (double)TDAN_K_UNI,
                       (int)c->ph[TDAN_GREY].nbz);
        td_rec_at(r, sizeof(r), list);
        td_crec_put(c, (uint8_t)TEST_DOT_CAM_ZONES, r);
    }
    /* Colours. */
    (void)snprintf(r, sizeof(r),
                   "colour_info dark=%.1f,%.1f,%.1f white=%.1f,%.1f,%.1f grey=%.4f,%.4f,%.4f red=%.4f,%.4f,%.4f"
                   " green=%.4f,%.4f,%.4f blue=%.4f,%.4f,%.4f",
                   c->doff[0], c->doff[1], c->doff[2], c->rgb[TDAN_WHITE][0], c->rgb[TDAN_WHITE][1],
                   c->rgb[TDAN_WHITE][2], c->colour.rel[TDAN_GREY][0], c->colour.rel[TDAN_GREY][1],
                   c->colour.rel[TDAN_GREY][2], c->colour.rel[TDAN_RED][0], c->colour.rel[TDAN_RED][1],
                   c->colour.rel[TDAN_RED][2], c->colour.rel[TDAN_GREEN][0], c->colour.rel[TDAN_GREEN][1],
                   c->colour.rel[TDAN_GREEN][2], c->colour.rel[TDAN_BLUE][0], c->colour.rel[TDAN_BLUE][1],
                   c->colour.rel[TDAN_BLUE][2]);
    td_crec_put(c, (uint8_t)TEST_DOT_CAM_COLOUR, r);
    why = "";
    if ( c->colour.white_dead )
    {
        why = " white_dead=1";
    }
    (void)snprintf(r, sizeof(r),
                   "test=cam_colour result=%s dist_grey=%.4f dist_red=%.4f dist_green=%.4f dist_blue=%.4f"
                   " wchroma=%.3f ripple=%.3f expratio=%.2f tol=%.3f retakes=%d%s",
                   td_word(c->colour.colour), c->colour.dist[TDAN_GREY], c->colour.dist[TDAN_RED],
                   c->colour.dist[TDAN_GREEN], c->colour.dist[TDAN_BLUE], c->colour.wchroma, (double)c->ripple,
                   c->eexp[TDAN_WHITE] / ((double)TD_EXP_WHITE), TDAN_CHROMA_TOL, (int)c->colour.retakes, why);
    td_crec_put(c, (uint8_t)TEST_DOT_CAM_COLOUR, r);
}

/** @brief The LED's record, after TD_CS_LED_JUDGE. */
static void td_led_record(td_cam_t *c)
{
    char r[KODE_FUNCTION_REC_MAX];

    if ( (uint8_t)TDAN_NO_VERDICT == c->led_verdict )
    {
        const char *why = c->led_why;
        if ( NULL == why )
        {
            why = "no_frames";
        }
        (void)snprintf(r, sizeof(r), "test=led result=noverdict why=%s", why);
    }
    else
    {
        (void)snprintf(r, sizeof(r),
                       "test=led result=%s flux_red=%.0f dist_red=%.2f flux_green=%.0f dist_green=%.2f flux_blue=%.0f"
                       " dist_blue=%.2f at=%d,%d min_flux=%.0f max_dist=%.1f",
                       td_word((tdan_verdict_t)c->led_verdict), c->led_flux[1], c->led_dist[1], c->led_flux[2],
                       c->led_dist[2], c->led_flux[3], c->led_dist[3], (int)c->led_x, (int)c->led_y, TD_LED_MIN_FLUX,
                       TD_LED_MAX_DIST);
    }
    td_crec_put(c, (uint8_t)TEST_DOT_CAM_LED, r);
}
