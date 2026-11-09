/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file tdan.h
 * @brief The end-of-line screen analysis: camera photos of the glass in, verdicts out.
 */
#ifndef TDAN_H_
#define TDAN_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- The panel and the grids ---- */

#define TDAN_PW            410   /**< Panel width, px. */
#define TDAN_PH            502   /**< Panel height, px. */
#define TDAN_B0            125   /**< First row of the protector's free band, the only part judged. */
#define TDAN_B1            380   /**< Row that ends the free band. */
#define TDAN_MX            6     /**< Columns not judged at each side (the glass edge). */
#define TDAN_S             2     /**< Warped grid: grid px per panel px. */
#define TDAN_WW            (TDAN_PW * TDAN_S)              /**< Warped grid width. */
#define TDAN_WH            ((TDAN_B1 - TDAN_B0) * TDAN_S)  /**< Warped grid height. */
#define TDAN_MAXP          24    /**< Stuck-on pixels kept. */
#define TDAN_ZC            41    /**< Zone cells across the band: 10 x 10 panel px each. */
#define TDAN_ZR            25    /**< Zone cells down the band. */
#define TDAN_ZONE_PX       10    /**< Side of a zone cell, panel px. */
#define TDAN_ZONES_MAX     4     /**< Zones kept per photo and per verdict. */
#define TDAN_CHANNELS      3     /**< Colour channels: red, green, blue. */
#define TDAN_MARK_COUNT    4     /**< Marks in the black photo, one at each corner of the band. */
#define TDAN_MARK_COORDS   (2 * TDAN_MARK_COUNT)   /**< Their (x, y) pairs. */
#define TDAN_MARK_LEFT_X   42    /**< Panel x of the left marks' centres. */
#define TDAN_MARK_RIGHT_X  368   /**< Panel x of the right marks' centres. */
#define TDAN_MARK_TOP_Y    167   /**< Panel y of the top marks' centres. */
#define TDAN_MARK_BOTTOM_Y 338   /**< Panel y of the bottom marks' centres. */
#define TDAN_PLANE_FLOATS  ((size_t)TDAN_WW * (size_t)TDAN_WH)              /**< Floats in one work plane. */
#define TDAN_WORK_BYTES    (4U * TDAN_PLANE_FLOATS * sizeof(float))      /**< The four planes the caller allocates. */

/* ---- The pixel method's limits (local contrast on the screen grid) ---- */

#define TDAN_PIX_EXT        3        /**< Context rows kept above and below the band. */
#define TDAN_PIX_ROWS       (TDAN_B1 - TDAN_B0 + (2 * TDAN_PIX_EXT))   /**< Rows of the screen grid. */
#define TDAN_PIX_T_C        0.08f    /**< A dead candidate: contrast against the ring median at or below -T_C. */
#define TDAN_PIX_T_D        0.63f    /**< Dead: the 3x3 deficit reaches this in white AND grey. */
#define TDAN_PIX_T_WARN     0.42f    /**< A weaker spot (dust, weak pixel): logged, does not fail. */
#define TDAN_PIX_SIG_MAX    0.05f    /**< Contrast noise of any block over this is no verdict (raw RGB565 clean: 0.019 to 0.031). */
#define TDAN_PIX_SAT_FRAC   0.005f   /**< More than this fraction of saturated cells in the judged zone: no verdict, retake. */
#define TDAN_PIX_ST_ABS     8.0f     /**< Stuck-on: grey levels above the ring median ... */
#define TDAN_PIX_ST_K       8.0f     /**< ... and this many sigma of the block. */
#define TDAN_PIX_ST_MERGE   20.0f    /**< Weak stuck maxima (strongest under this) within 4 px are one pixel; a 2x2 block is 45 and up. */
#define TDAN_PIX_ZONE_CELLS 14       /**< More cells than this around a dead candidate: a small zone, not a pixel. */
#define TDAN_PIX_MAXCAND    96       /**< Dead candidates kept from the white photo. */
#define TDAN_PIX_MAXDEAD    96       /**< Dead pixels kept. */
#define TDAN_PIX_MAXWARN    12       /**< Weak spots kept. */

#define TDAN_PIX_OK         0        /**< The photo can be judged. */
#define TDAN_PIX_SATURATED  1        /**< Too many saturated cells: retake with less light. */
#define TDAN_PIX_NOISY      2        /**< The contrast is too noisy to judge. */
#define TDAN_PIX_NOREF      3        /**< The grey came before any white. */

/* ---- The judgements' limits ---- */

#define TDAN_K_UNI          0.86f    /**< A zone: 4 or more joined cells under this ratio to the golden photo. */
#define TDAN_K_BRIGHT       1.30f    /**< A bright patch: a 35 % brighter grey patch gives 1.8, 20 % gives 1.5; clean grey 1.06 to 1.08. */
#define TDAN_LINE_C         (-0.20f) /**< A dead 1 px column: -0.41 to -0.94; normal -0.016 to -0.07 (only the catastrophic). */
#define TDAN_WLINE_COL_W    (-0.08f) /**< Wide line, white: a dead 1 px column -0.12; clean -0.016 to -0.035. */
#define TDAN_WLINE_COL_G    (-0.12f) /**< Wide line, grey: a dead 1 px column -0.24; clean -0.041 to -0.062. */
#define TDAN_WLINE_ROW      (-0.15f) /**< Wide row, white and grey: a dead 1 px row -0.30; clean -0.04 to -0.09 (refresh bands). */
#define TDAN_CHROMA_TOL     0.25     /**< L1 distance of the white-relative chromaticity: a dead channel 0.44 to 1.9, the OLED's spread up to 0.15. */
#define TDAN_WHITE_CHROMA_TOL 0.14   /**< The white against 1/3 each: a channel 14 % down 0.14 to 0.28; the real white 0.007 to 0.016. */
#define TDAN_RIPPLE_MAX     0.12     /**< Peak ripple of the white's row means: flicker m = 0.25 gives 0.21, normal 0.07. */
#define TDAN_EXP_RATIO_MAX  3.0      /**< The white's exposure against its nominal one: outside 1/3 .. 3 the brightness is anomalous. */
#define TDAN_PIX_ALLOW_DEAD  2       /**< Isolated dead pixels allowed: a third fails. */
#define TDAN_PIX_ALLOW_STUCK 1       /**< Isolated stuck-on pixels allowed: a second fails. */
#define TDAN_PIX_GROUP_PX    12      /**< Two defects closer than this in x and y are a group, which fails. */
#define TDAN_EDGE_WARN_X     40      /**< A dead pixel left of this, in a pass with retakes, is a warning (light on the glass edge). */
#define TDAN_MAX_ANGLE_DEG   7.0     /**< The turn the framing accepts, degrees. */
#define TDAN_MAX_PERSP       0.08    /**< The top/bottom edge ratio the framing accepts, off 1. */
#define TDAN_INSIDE_MARGIN_PX 40     /**< The band's corners must sit this far inside the photo. */

/* ---- Types ---- */

/** @brief A homography, panel -> photo, row-major 3x3. */
typedef struct {
    double h[9];      /**< The matrix, h[8] = 1. */
} tdan_h_t;

/** @brief A panel pixel and its strength. */
typedef struct {
    int32_t x;        /**< Panel column. */
    int32_t y;        /**< Panel row. */
    float   v;        /**< Strength (stuck: levels over the ring; dead: minus the deficit). */
} tdan_pt_t;

/** @brief The strongest dark column or row of a photo. */
typedef struct {
    int32_t pos;      /**< x of a column, y of a row. */
    int32_t a0;       /**< First px of its extent. */
    int32_t a1;       /**< Last px of its extent. */
    float   level;    /**< Its relative contrast (negative is dark). */
} tdan_line_t;

/** @brief A zone of joined cells off the golden photo. */
typedef struct {
    int32_t x0;        /**< Left panel px. */
    int32_t y0;        /**< Top panel px. */
    int32_t x1;        /**< Right panel px. */
    int32_t y1;        /**< Bottom panel px. */
    int32_t cells;     /**< Cells joined. */
    float   min_ratio; /**< Lowest ratio (dark zones) or highest (bright patches). */
} tdan_zone_t;

/** @brief The work memory: four float planes of TDAN_PLANE_FLOATS, allocated by the caller. */
typedef struct {
    float *a;          /**< Plane a (the warped grid, or scratch). */
    float *b;          /**< Plane b. */
    float *c;          /**< Plane c (the screen grid P after tdan_pix_photo). */
    float *d;          /**< Plane d (the contrast plane after tdan_pix_photo). */
    bool   warped;     /**< Plane a holds this photo's warp; cleared by tdan_work_new_photo(). */
} tdan_work_t;

/** @brief A dead pixel or a weak spot. */
typedef struct {
    int16_t x;         /**< Panel column. */
    int16_t y;         /**< Panel row. */
    float   dw;        /**< 3x3 deficit in white. */
    float   dg;        /**< 3x3 deficit in grey. */
    int32_t zone;      /**< Cells darker than 5 % within 3 cells (a pixel gives ~6-9). */
} tdan_dead_t;

/** @brief A dead candidate of the white photo. */
typedef struct {
    int16_t x;         /**< Panel column. */
    int16_t y;         /**< Panel row. */
    float   dw;        /**< 3x3 deficit in white. */
    int32_t zone;      /**< Cells darker than 5 % within 3 cells. */
} tdan_cand_t;

/** @brief The pixel method's state across the white and the grey photo. */
typedef struct {
    uint8_t     okcol[TDAN_PW];               /**< Columns that are valid references (not the glass edge). */
    bool        have_ref;                     /**< The white has been analysed. */
    tdan_cand_t cand[TDAN_PIX_MAXCAND];       /**< Dead candidates of the white. */
    int32_t     ncand;                        /**< How many. */
    tdan_dead_t dead[TDAN_PIX_MAXDEAD];       /**< Dead pixels, after the grey. */
    int32_t     ndead;                        /**< How many. */
    tdan_dead_t warn[TDAN_PIX_MAXWARN];       /**< Weak spots, after the grey. */
    int32_t     nwarn;                        /**< How many. */
    float       sat[2];                       /**< Saturated fraction, white and grey. */
    float       sigmax[2];                    /**< Largest block noise, white and grey. */
} tdan_pix_t;

/** @brief One photo's analysis (white or grey): the worker's job. */
typedef struct {
    int32_t     status;                       /**< TDAN_PIX_*. */
    tdan_line_t vl;                           /**< Narrow dead column. */
    tdan_line_t hl;                           /**< Narrow dead row. */
    tdan_line_t wvl;                          /**< Wide dead column. */
    tdan_line_t whl;                          /**< Wide dead row. */
    tdan_zone_t zones[TDAN_ZONES_MAX];        /**< Dark zones. */
    int32_t     nzones;                       /**< How many. */
    tdan_zone_t bz[TDAN_ZONES_MAX];           /**< Bright patches (grey only). */
    int32_t     nbz;                          /**< How many. */
} tdan_photo_t;

/** @brief A verdict. */
typedef enum {
    TDAN_PASS = 0,     /**< Judged good. */
    TDAN_FAIL,         /**< Judged bad. */
    TDAN_NO_VERDICT    /**< Not judged (marks, light, noise). */
} tdan_verdict_t;

/** @brief The framing and the turn of the Kode. */
typedef struct {
    tdan_verdict_t framing;    /**< The screen located and its band inside the photo. */
    tdan_verdict_t straight;   /**< The turn and the perspective within limits. */
    bool           inside;     /**< The band's corners are TDAN_INSIDE_MARGIN_PX inside the photo. */
    bool           partial;    /**< Marks seen but fewer than four: position known, no verdict. */
    double         angle_deg;  /**< Turn, degrees. */
    double         scale;      /**< Photo px per panel px. */
    double         persp;      /**< Top/bottom edge ratio, off 1. */
} tdan_frame_t;

/** @brief What the pixel, line and zone judgement reads. */
typedef struct {
    const tdan_pix_t   *pix;            /**< The pixel state after white and grey. */
    const tdan_photo_t *white;          /**< The white's analysis; NULL when it was not taken. */
    const tdan_photo_t *grey;           /**< The grey's analysis; NULL when it was not taken. */
    const tdan_pt_t    *stuck;          /**< Stuck-on pixels of the black photo. */
    int32_t             nstuck;         /**< How many. */
    int32_t             retakes;        /**< Colour photos retaken with a scaled exposure (tdan_colour_t). */
    bool                confirming;     /**< The second pass: only what comes back near the first counts. */
    const tdan_pt_t    *first_dead;     /**< Confirming: every dead pixel of the first pass's pix (pix->dead). */
    int32_t             nfirst_dead;    /**< How many. */
    const tdan_pt_t    *first_stuck;    /**< Confirming: the stuck-on pixels the first pass counted. */
    int32_t             nfirst_stuck;   /**< How many. */
} tdan_screen_in_t;

/** @brief The pixel, line and zone verdicts. */
typedef struct {
    tdan_verdict_t pixels;                      /**< Dead and stuck-on pixels. */
    tdan_verdict_t lines;                       /**< Dead columns and rows. */
    tdan_verdict_t zones;                       /**< Dark zones and bright patches. */
    int32_t        pix_status;                  /**< Why pixels has no verdict: TDAN_PIX_*. */
    bool           confirm;                     /**< First pass about to fail pixels: photograph again and confirm. */
    tdan_pt_t      dead[TDAN_PIX_MAXDEAD];      /**< Dead pixels that count. */
    int32_t        ndead;                       /**< How many. */
    tdan_pt_t      stuck[TDAN_MAXP];            /**< Stuck-on pixels that count. */
    int32_t        nstuck;                      /**< How many. */
    tdan_pt_t      edge[TDAN_PIX_MAXDEAD];      /**< Dead pixels left of TDAN_EDGE_WARN_X made warnings. */
    int32_t        nedge;                       /**< How many. */
    int32_t        nsmall;                      /**< Small zones born from dead candidates. */
    bool           group;                       /**< Two defects closer than TDAN_PIX_GROUP_PX. */
    bool           streak;                      /**< Many chained stuck-on pixels: a reflection or a lamp. */
    bool           vline;                       /**< A dead column counts. */
    bool           hline;                       /**< A dead row counts. */
    tdan_line_t    vl;                          /**< The column (narrow, else wide). */
    tdan_line_t    hl;                          /**< The row (narrow, else wide). */
    tdan_zone_t    zone[TDAN_ZONES_MAX];        /**< Zones that count. */
    int32_t        nzones;                      /**< How many. */
} tdan_screen_t;

#define TDAN_WHITE   0   /**< Colour index: the white photo. */
#define TDAN_GREY    1   /**< Colour index: the grey photo. */
#define TDAN_RED     2   /**< Colour index: the red photo. */
#define TDAN_GREEN   3   /**< Colour index: the green photo. */
#define TDAN_BLUE    4   /**< Colour index: the blue photo. */
#define TDAN_COLOURS 5   /**< Colour photos. */

/** @brief What the colour judgement reads. */
typedef struct {
    bool   ok[TDAN_COLOURS];           /**< The photo was taken. */
    double rgb[TDAN_COLOURS][TDAN_CHANNELS];       /**< Mean RGB of the band, net of the dark (tdan_colour). */
    bool   retake[TDAN_COLOURS];       /**< Retaken with a scaled exposure. */
    double exp_ratio;                  /**< The white's exposure over its nominal one. */
    double ripple;                     /**< The white's ripple (tdan_ripple). */
} tdan_colour_in_t;

/** @brief The colour verdict. */
typedef struct {
    tdan_verdict_t colour;             /**< The verdict. */
    double         dist[TDAN_COLOURS]; /**< L1 distance of grey, red, green, blue to their references. */
    double         rel[TDAN_COLOURS][TDAN_CHANNELS]; /**< White-relative chromaticity of each. */
    double         wchroma;            /**< The white's distance to 1/3 each. */
    bool           white_dead;         /**< A channel of the white under 8 levels. */
    bool           white_clipped;      /**< The white is clipped: nothing relative to it is judged. */
    int32_t        retakes;            /**< Grey, red, green and blue photos retaken. */
} tdan_colour_t;

/** @brief A hook called now and then from the long loops (NULL on the host). */
typedef void (*tdan_yield_fn_t)(void);

/* ---- Geometry (tdan_geom.c) ---- */

/**
 * @brief Set the hook the long loops call to let the idle task run.
 * @param f The hook, or NULL.
 */
void tdan_set_yield(tdan_yield_fn_t f);

/**
 * @brief Forget the warp of the last photo: call before every new photo.
 * @param wk The work memory.
 */
void tdan_work_new_photo(tdan_work_t *wk);

/**
 * @brief Map a panel point into the photo.
 * @param hm The homography.
 * @param x  Panel x.
 * @param y  Panel y.
 * @param u  Photo x out.
 * @param v  Photo y out.
 */
void tdan_h_apply(const tdan_h_t *hm, double x, double y, double *u, double *v);

/**
 * @brief The homography through four point pairs.
 * @param p   Four panel points (x, y).
 * @param q   The four photo points.
 * @param out The homography.
 * @return false when the points are degenerate.
 */
bool tdan_h_from_points(const double p[TDAN_MARK_COORDS], const double q[TDAN_MARK_COORDS], tdan_h_t *out);

/**
 * @brief The panel centres of the four marks of the black photo.
 * @param out Four (x, y): top-left, top-right, bottom-right, bottom-left.
 */
void tdan_mark_centres(double out[TDAN_MARK_COORDS]);

/**
 * @brief Find the four marks near where @p h0 predicts them, else in the whole photo, and refine the homography.
 * @param luma    The black photo's luma.
 * @param w       Width.
 * @param h       Height.
 * @param h0      The nominal homography.
 * @param hm      The refined homography out.
 * @param found   The marks' photo centres out.
 * @param missing The first mark not found (4: the pose failed), or -1.
 * @param nmarks  Marks seen: 4, or 3 with the fourth completed, 0 when not found.
 * @param rescued Found only by the whole-photo search.
 * @return false when not found.
 */
bool tdan_locate_marks(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *h0, tdan_h_t *hm,
                       double found[TDAN_MARK_COORDS], int32_t *missing, int32_t *nmarks, bool *rescued);

/**
 * @brief Turn, scale and perspective of the panel under @p hm.
 * @param hm        The homography.
 * @param angle_deg Turn, degrees.
 * @param scale     Photo px per panel px.
 * @param persp     Top/bottom edge ratio, off 1.
 */
void tdan_pose(const tdan_h_t *hm, double *angle_deg, double *scale, double *persp);

/**
 * @brief Luma of an RGB565 photo.
 * @param rgb565 The photo.
 * @param w      Width.
 * @param h      Height.
 * @param out    w x h bytes.
 */
void tdan_luma(const uint16_t *rgb565, int32_t w, int32_t h, uint8_t *out);

/* ---- Pixels (tdan_pix.c) ---- */

/**
 * @brief One photo of the white (grey = 0, first) or the grey (grey = 1) screen.
 * @param luma The photo's luma.
 * @param w    Width.
 * @param h    Height.
 * @param hm   The homography.
 * @param wk   The work memory.
 * @param grey 0 white, 1 grey.
 * @param st   The pixel state: dead pixels in st->dead after the grey.
 * @return TDAN_PIX_*.
 */
int32_t tdan_pix_photo(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                       int32_t grey, tdan_pix_t *st);

/**
 * @brief Zone cells (percent of the median) from the screen grid the last tdan_pix_photo left.
 * @param wk    The work memory.
 * @param cells The cells out.
 */
void tdan_pix_zone_cells(tdan_work_t *wk, uint8_t cells[TDAN_ZR][TDAN_ZC]);

/**
 * @brief Narrow dead column and row from the contrast plane the last tdan_pix_photo left.
 * @param wk The work memory.
 * @param vl The column out.
 * @param hl The row out.
 */
void tdan_pix_lines(const tdan_work_t *wk, tdan_line_t *vl, tdan_line_t *hl);

/**
 * @brief Wide dead column and row (the glass spreads a 1 px dead column into a 12 px dip) from the screen grid.
 * @param wk The work memory (plane a is scratch).
 * @param vl The column out.
 * @param hl The row out.
 */
void tdan_pix_wide_lines(tdan_work_t *wk, tdan_line_t *vl, tdan_line_t *hl);

/**
 * @brief Stuck-on pixels of the black photo; needs the white done first (its glass-edge columns).
 * @param luma The black photo's luma (net of the panel-off photo).
 * @param w    Width.
 * @param h    Height.
 * @param hm   The homography.
 * @param wk   The work memory.
 * @param st   The pixel state.
 * @param out  The pixels out.
 * @param maxn Room in @p out.
 * @return How many.
 */
int32_t tdan_pix_stuck(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                       const tdan_pix_t *st, tdan_pt_t *out, int32_t maxn);

/**
 * @brief Zone cells of the warped, blurred band: what tdan_gold.h is made from.
 * @param luma  The photo's luma.
 * @param w     Width.
 * @param h     Height.
 * @param hm    The homography.
 * @param wk    The work memory.
 * @param cells The cells out.
 */
void tdan_zone_cells(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                     uint8_t cells[TDAN_ZR][TDAN_ZC]);

/**
 * @brief The worker's job for the white or the grey: pixels, lines, wide lines, zones and bright patches.
 * @param luma The photo's luma.
 * @param w    Width.
 * @param h    Height.
 * @param hm   The homography.
 * @param wk   The work memory.
 * @param grey 0 white, 1 grey.
 * @param st   The pixel state.
 * @param out  The photo's analysis.
 */
void tdan_analyse_photo(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm, tdan_work_t *wk,
                        int32_t grey, tdan_pix_t *st, tdan_photo_t *out);

/* ---- Judgements (tdan_judge.c) ---- */

/**
 * @brief Dark zones against the golden photo.
 * @param cells   The photo's cells.
 * @param pattern 0 white, 1 grey.
 * @param wk      The work memory (plane a is scratch).
 * @param out     The zones out.
 * @param maxn    Room in @p out.
 * @return How many.
 */
int32_t tdan_zones_from_cells(const uint8_t cells[TDAN_ZR][TDAN_ZC], int32_t pattern, tdan_work_t *wk,
                              tdan_zone_t *out, int32_t maxn);

/**
 * @brief Bright patches of the grey photo: 4 or more joined cells over TDAN_K_BRIGHT times the golden grey.
 * @param cells The grey's cells.
 * @param wk    The work memory (plane a is scratch).
 * @param out   The patches out; min_ratio holds the highest ratio.
 * @param maxn  Room in @p out.
 * @return How many.
 */
int32_t tdan_zones_bright_from_cells(const uint8_t cells[TDAN_ZR][TDAN_ZC], tdan_work_t *wk, tdan_zone_t *out,
                                     int32_t maxn);

/**
 * @brief Mean RGB of the central band, 0..255.
 * @param rgb565 The photo.
 * @param w      Width.
 * @param h      Height.
 * @param hm     The homography.
 * @param rgb    The mean out.
 */
void tdan_colour(const uint16_t *rgb565, int32_t w, int32_t h, const tdan_h_t *hm, double rgb[TDAN_CHANNELS]);

/**
 * @brief Peak ripple of the white's row means along the band (quadratic trend removed): sqrt(2) * std / mean.
 * @param luma The white's luma.
 * @param w    Width.
 * @param h    Height.
 * @param hm   The homography.
 * @return The ripple; 0 when the band is too short.
 */
float tdan_ripple(const uint8_t *luma, int32_t w, int32_t h, const tdan_h_t *hm);

/**
 * @brief Judge the framing and the turn.
 * @param hm      The homography used (the nominal one when not located).
 * @param located tdan_locate_marks() found the marks.
 * @param nmarks  Marks it saw.
 * @param w       Photo width.
 * @param h       Photo height.
 * @param out     The verdicts.
 */
void tdan_judge_frame(const tdan_h_t *hm, bool located, int32_t nmarks, int32_t w, int32_t h, tdan_frame_t *out);

/**
 * @brief Judge pixels, lines and zones from the analyses of white, grey and black.
 * @param in  What was measured.
 * @param out The verdicts.
 */
void tdan_judge_screen(const tdan_screen_in_t *in, tdan_screen_t *out);

/**
 * @brief Judge the colour against the white of the same pass.
 * @param in  What was measured.
 * @param out The verdict.
 */
void tdan_judge_colour(const tdan_colour_in_t *in, tdan_colour_t *out);

#ifdef __cplusplus
}
#endif

#endif /* TDAN_H_ */
