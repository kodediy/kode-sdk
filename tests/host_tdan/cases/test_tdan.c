/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_tdan.c
 * @brief The screen analysis on panel photos: a good one, one with dead pixels, one with no marks.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity.h"

#include "frame_png.h"
#include "tdan.h"

#define FRAME_PATH_MAX 256   /**< Room for a frame's path. */

/** @brief One frame, as luma and RGB565. */
typedef struct {
    uint16_t *px;     /**< RGB565. */
    uint8_t  *luma;   /**< Its luma. */
    int32_t   w;      /**< Width. */
    int32_t   h;      /**< Height. */
} frame_t;

/** @brief What a whole run of the station's photos gave. */
typedef struct {
    tdan_frame_t  frame;    /**< Framing and turn. */
    tdan_screen_t screen;   /**< Pixels, lines, zones. */
    tdan_colour_t colour;   /**< Colour. */
} run_t;

static tdan_work_t s_wk;
static tdan_pix_t s_pix;
static tdan_photo_t s_photo[2];
static run_t s_run;

static frame_t load(const char *name)
{
    char path[FRAME_PATH_MAX];
    frame_t f = { 0 };
    (void)snprintf(path, sizeof(path), "%s/%s.png", TDAN_FRAMES_DIR, name);
    f.px = frame_png_load(path, &f.w, &f.h);
    TEST_ASSERT_NOT_NULL_MESSAGE(f.px, path);
    f.luma = malloc((size_t)f.w * (size_t)f.h);
    TEST_ASSERT_NOT_NULL(f.luma);
    tdan_luma(f.px, f.w, f.h, f.luma);
    return f;
}

static void drop(frame_t *f)
{
    free(f->px);
    free(f->luma);
}

static void work_alloc(void)
{
    if ( NULL == s_wk.a )
    {
        s_wk.a = malloc(TDAN_PLANE_FLOATS * sizeof(float));
        s_wk.b = malloc(TDAN_PLANE_FLOATS * sizeof(float));
        s_wk.c = malloc(TDAN_PLANE_FLOATS * sizeof(float));
        s_wk.d = malloc(TDAN_PLANE_FLOATS * sizeof(float));
    }
    TEST_ASSERT_NOT_NULL(s_wk.d);
}

/**
 * @brief Run the station's analysis on a set of frames.
 * @param black The black with marks.
 * @param white The white.
 * @param grey  The grey.
 */
static void run(const char *black, const char *white, const char *grey)
{
    static const char *const COLOURS[TDAN_COLOURS] = { NULL, NULL, "red", "green", "blue" };
    tdan_h_t h0;
    tdan_h_t hm;
    double found[8];
    int32_t missing = 0;
    int32_t nmarks = 0;
    bool rescued = false;

    work_alloc();
    (void)memset(&s_pix, 0, sizeof(s_pix));
    (void)memset(s_photo, 0, sizeof(s_photo));
    (void)memset(&s_run, 0, sizeof(s_run));

    /* The nominal pose is the one the frames were drawn with. */
    char path[FRAME_PATH_MAX];
    (void)snprintf(path, sizeof(path), "%s/H.txt", TDAN_FRAMES_DIR);
    FILE *fh = fopen(path, "r");
    TEST_ASSERT_NOT_NULL(fh);
    for ( int32_t i = 0; 9 > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT(1, fscanf(fh, "%lf", &h0.h[i]));
    }
    (void)fclose(fh);

    frame_t fb = load(black);
    const bool located = tdan_locate_marks(fb.luma, fb.w, fb.h, &h0, &hm, found, &missing, &nmarks, &rescued);
    if ( !located )
    {
        hm = h0;
    }
    tdan_judge_frame(&hm, located, nmarks, fb.w, fb.h, &s_run.frame);
    if ( !located )
    {
        drop(&fb);
        return;
    }

    /* The panel-off photo: the dark, also what the black is taken net of. */
    frame_t off = load("black_nomarks");
    double dark[3];
    tdan_colour(off.px, off.w, off.h, &hm, dark);
    for ( size_t q = 0U; ((size_t)fb.w * (size_t)fb.h) > q; q++ )
    {
        const int32_t d = (int32_t)fb.luma[q] - (int32_t)off.luma[q];
        fb.luma[q] = 0U;
        if ( 0 < d )
        {
            fb.luma[q] = (uint8_t)d;
        }
    }
    drop(&off);

    tdan_colour_in_t cin = { .exp_ratio = 1.0 };
    const char *const names[2] = { white, grey };
    for ( int32_t g = 0; 2 > g; g++ )
    {
        frame_t f = load(names[g]);
        tdan_work_new_photo(&s_wk);
        tdan_analyse_photo(f.luma, f.w, f.h, &hm, &s_wk, g, &s_pix, &s_photo[g]);
        tdan_colour(f.px, f.w, f.h, &hm, cin.rgb[g]);
        cin.ok[g] = true;
        if ( 0 == g )
        {
            cin.ripple = (double)tdan_ripple(f.luma, f.w, f.h, &hm);
        }
        drop(&f);
    }
    for ( int32_t c = TDAN_RED; TDAN_COLOURS > c; c++ )
    {
        frame_t f = load(COLOURS[c]);
        tdan_colour(f.px, f.w, f.h, &hm, cin.rgb[c]);
        cin.ok[c] = true;
        drop(&f);
    }
    for ( int32_t c = 0; TDAN_COLOURS > c; c++ )
    {
        for ( int32_t k = 0; 3 > k; k++ )
        {
            cin.rgb[c][k] -= dark[k];
        }
    }

    static tdan_pt_t stuck[TDAN_MAXP];
    const int32_t nstuck = tdan_pix_stuck(fb.luma, fb.w, fb.h, &hm, &s_wk, &s_pix, stuck, TDAN_MAXP);
    drop(&fb);

    const tdan_screen_in_t sin = {
        .pix = &s_pix, .white = &s_photo[0], .grey = &s_photo[1], .stuck = stuck, .nstuck = nstuck,
    };
    tdan_judge_screen(&sin, &s_run.screen);
    tdan_judge_colour(&cin, &s_run.colour);
}

TEST_CASE("a good panel passes every judgement", "[tdan]")
{
    run("black", "white", "grey");
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.frame.framing);
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.frame.straight);
    TEST_ASSERT_EQUAL_INT(TDAN_PIX_OK, s_run.screen.pix_status);
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.screen.pixels);
    TEST_ASSERT_EQUAL_INT(0, s_run.screen.ndead);
    TEST_ASSERT_EQUAL_INT(0, s_run.screen.nstuck);
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.screen.lines);
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.screen.zones);
    TEST_ASSERT_FALSE(s_run.screen.confirm);
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.colour.colour);
}

TEST_CASE("a cluster of dead pixels fails the pixels and asks to confirm", "[tdan]")
{
    run("black", "white_dead", "grey_dead");
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.frame.framing);
    TEST_ASSERT_EQUAL_INT(TDAN_PIX_OK, s_run.screen.pix_status);
    TEST_ASSERT_EQUAL(TDAN_FAIL, s_run.screen.pixels);
    TEST_ASSERT_EQUAL_INT(3, s_run.screen.ndead);
    TEST_ASSERT_TRUE(s_run.screen.group);
    TEST_ASSERT_TRUE(s_run.screen.confirm);
    /* Three dead pixels are no line and no zone. */
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.screen.lines);
    TEST_ASSERT_EQUAL(TDAN_PASS, s_run.screen.zones);
}

TEST_CASE("a black with no marks gives no framing verdict", "[tdan]")
{
    run("black_nomarks", "white", "grey");
    TEST_ASSERT_EQUAL(TDAN_NO_VERDICT, s_run.frame.framing);
    TEST_ASSERT_EQUAL(TDAN_NO_VERDICT, s_run.frame.straight);
}
