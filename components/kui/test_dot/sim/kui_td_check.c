/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_td_check.c
 * @brief Headless checks of the E5 screens and patterns: building, leaks, and the pixels analysed.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "kui.h"
#include "kui_test_dot.h"
#include "kui_tokens.h"
#include "lvgl.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define CHECK_UNDER_ASAN 1   /**< AddressSanitizer owns the heap: its quarantine keeps freed blocks counted. */
#endif
#endif
#if !defined(CHECK_UNDER_ASAN) && defined(__SANITIZE_ADDRESS__)
#define CHECK_UNDER_ASAN 1   /**< The same, as GCC says it. */
#endif

#if defined(__APPLE__) && !defined(CHECK_UNDER_ASAN)
#include <malloc/malloc.h>
#define CHECK_HEAP_STATS 1   /**< This C library counts the live blocks. */
#else
#define CHECK_HEAP_STATS 0   /**< No exact count of live blocks here. */
#endif

#define CHECK_W            410U   /**< The panel's width, as Luismi's kgui_e5pat.c draws it. */
#define CHECK_H            502U   /**< The panel's height. */
#define CHECK_BUF_ROWS     50U    /**< Rows of the render buffer. */
#define CHECK_BPP          2U     /**< Bytes per RGB565 pixel. */
#define CHECK_MARK_HALF    12     /**< Half the side of a mark in kgui_e5pat.c. */
#define CHECK_BAND_X       205    /**< A column in the middle of the band. */
#define CHECK_BAND_Y       252    /**< A row in the middle of the band. */
#define CHECK_BAND_TOP_Y   130    /**< A row near the top of the band, clear of every glyph. */
#define CHECK_RIM_DX       17     /**< From a witness' centre to its black rim. */
#define CHECK_GRID_CELL    5U     /**< The cell the grid checks change. */
#define CHECK_ROUNDS       3U     /**< Builds of each view in the leak check: the first warms LVGL's caches. */

/** Luismi's mark centres (kgui_e5pat.c, K_MARKS; e5cam_an.c's an_mark_centres()). */
static const int32_t MARKS[4][2] = { { 42, 167 }, { 368, 167 }, { 368, 338 }, { 42, 338 } };
/** Luismi's witness centres (kgui_e5pat.c, apply_frame()). */
static const int32_t WITNESSES[4][2] = { { 50, 72 }, { 360, 72 }, { 55, 405 }, { 355, 405 } };

/** How many checks failed. */
static uint32_t s_failed;

/** The render buffer of the headless display. */
static uint8_t s_render[CHECK_W * CHECK_BUF_ROWS * CHECK_BPP];

/**
 * @brief The headless display's flush: nothing to send.
 * @param disp The display.
 * @param area Unused.
 * @param px   Unused.
 */
static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

/**
 * @brief Report one check.
 * @param ok   Whether it held.
 * @param what What it says.
 */
static void expect(bool ok, const char *what)
{
    if ( ok )
    {
        (void)printf("PASS  %s\n", what);
    }
    else
    {
        (void)printf("FAIL  %s\n", what);
        s_failed++;
    }
}

/**
 * @brief RGB565 of a 0xRRGGBB colour, as kgui_e5pat.c's c565().
 * @param rgb The colour.
 * @return Its RGB565 value.
 */
static uint16_t c565(uint32_t rgb)
{
    uint32_t const r = (rgb >> 16) & 0xF8U;
    uint32_t const g = (rgb >> 8) & 0xFCU;
    uint32_t const b = (rgb & 0xFFU) >> 3;
    return (uint16_t)((r << 8) | (g << 3) | b);
}

/**
 * @brief How many timers LVGL holds.
 * @return The count.
 */
static uint32_t timer_count(void)
{
    uint32_t n = 0U;
    lv_timer_t *t = lv_timer_get_next(NULL);
    while ( NULL != t )
    {
        n++;
        t = lv_timer_get_next(t);
    }
    return n;
}

/** The objects a builder must leave alone, counted. */
typedef struct {
    uint32_t other;   /**< Children of another screen. */
    uint32_t active;  /**< Children of the active screen. */
    uint32_t top;     /**< Children of the top layer. */
    uint32_t sys;     /**< Children of the system layer. */
    uint32_t timers;  /**< LVGL's timers. */
} world_t;

/**
 * @brief Count what a builder must not touch.
 * @param other Another screen.
 * @return The counts.
 */
static world_t world(lv_obj_t *other)
{
    world_t w;
    w.other = lv_obj_get_child_count(other);
    w.active = lv_obj_get_child_count(lv_screen_active());
    w.top = lv_obj_get_child_count(lv_layer_top());
    w.sys = lv_obj_get_child_count(lv_layer_sys());
    w.timers = timer_count();
    return w;
}

/**
 * @brief Whether nothing but the timers changed.
 * @param a Before.
 * @param b After.
 * @return true when the objects are the same.
 */
static bool same_objects(const world_t *a, const world_t *b)
{
    return (a->other == b->other) && (a->active == b->active) && (a->top == b->top) && (a->sys == b->sys);
}

/** A rendered parent: its RGB565 pixels. */
typedef struct {
    lv_draw_buf_t *buf; /**< The snapshot. */
} shot_t;

/**
 * @brief Render a parent the way the panel shows it.
 * @param obj The parent.
 * @return The snapshot; buf is NULL when it failed.
 */
static shot_t shoot(lv_obj_t *obj)
{
    shot_t s;
    s.buf = lv_snapshot_take(obj, LV_COLOR_FORMAT_RGB565);
    return s;
}

/**
 * @brief One pixel of a snapshot.
 * @param s The snapshot.
 * @param x Column.
 * @param y Row.
 * @return Its RGB565 value.
 */
static uint16_t px_at(const shot_t *s, int32_t x, int32_t y)
{
    const uint8_t *row = (const uint8_t *)s->buf->data + ((size_t)y * s->buf->header.stride);
    uint16_t v = 0U;
    (void)memcpy(&v, row + ((size_t)x * CHECK_BPP), sizeof(v));
    return v;
}

/**
 * @brief Whether two snapshots hold the same pixels.
 * @param a One.
 * @param b The other.
 * @return true when every pixel matches.
 */
static bool same_pixels(const shot_t *a, const shot_t *b)
{
    bool same = (NULL != a->buf) && (NULL != b->buf);
    for ( uint32_t y = 0U; same && (y < CHECK_H); y++ )
    {
        const uint8_t *ra = (const uint8_t *)a->buf->data + ((size_t)y * a->buf->header.stride);
        const uint8_t *rb = (const uint8_t *)b->buf->data + ((size_t)y * b->buf->header.stride);
        same = (0 == memcmp(ra, rb, (size_t)CHECK_W * CHECK_BPP));
    }
    return same;
}

/** The C heap at one moment: live blocks and their bytes. */
typedef struct {
    size_t blocks; /**< Live blocks. */
    size_t bytes;  /**< Their bytes. */
} heap_t;

/**
 * @brief What the C heap holds now.
 * @return The live blocks and bytes; zeros where the library cannot say.
 */
static heap_t heap_now(void)
{
    heap_t h = { 0U, 0U };
#if CHECK_HEAP_STATS
    malloc_statistics_t st;
    malloc_zone_statistics(NULL, &st);
    h.blocks = st.blocks_in_use;
    h.bytes = st.size_in_use;
#endif
    return h;
}

/**
 * @brief Build one view on a fresh parent, draw it, delete the parent, and say what the heap kept.
 * @param screen true for a screen, false for a pattern.
 * @param id     The screen or the pattern.
 * @param arg    The screen's states, or NULL.
 * @param len    Their length.
 * @param parg   The pattern's arg.
 * @return Live blocks after, less live blocks before.
 */
static long build_and_free(bool screen, uint32_t id, const uint8_t *arg, size_t len, uint8_t parg)
{
    heap_t const before = heap_now();
    lv_obj_t *parent = lv_obj_create(NULL);
    if ( screen )
    {
        kui_td_screen_build(parent, (kui_td_screen_t)id, arg, len);
    }
    else
    {
        kui_td_pattern_build(parent, (kui_td_pattern_t)id, parg);
    }
    shot_t const s = shoot(parent);
    if ( NULL != s.buf )
    {
        lv_draw_buf_destroy(s.buf);
    }
    lv_obj_delete(parent);
    heap_t const after = heap_now();
    long kept = (long)after.blocks - (long)before.blocks;
    if ( after.bytes != before.bytes )
    {
        kept += 1L;
    }
    return kept;
}

/** @brief Every screen and every pattern gives back all it took once its parent is deleted. */
static void check_nothing_leaks(void)
{
    static const uint8_t BUSY[KUI_TD_GRID_CELLS] = { 'o', 'o', 'x', 'r', 'p', 'p', 'p', 'p', 'p',
                                                     'p', 'p', 'p', 'p', 'p', 'p', 'p', 'p' };
    if ( 0 == CHECK_HEAP_STATS )
    {
        (void)printf("SKIP  the leak check: no exact count of live blocks in this build\n");
        return;
    }
    for ( uint32_t v = 0U; v < ((uint32_t)KUI_TD_SCREEN_COUNT + (uint32_t)KUI_TD_PAT_COUNT); v++ )
    {
        char what[96];
        bool const screen = (v < (uint32_t)KUI_TD_SCREEN_COUNT);
        uint32_t id = v;
        long kept = 0L;
        if ( !screen )
        {
            id = v - (uint32_t)KUI_TD_SCREEN_COUNT;
        }
        for ( uint32_t r = 0U; r < CHECK_ROUNDS; r++ )
        {
            kept = build_and_free(screen, id, BUSY, sizeof(BUSY), (uint8_t)KUI_TD_COLOUR_WHITE);
        }
        if ( screen )
        {
            (void)snprintf(what, sizeof(what), "screen %u gives back every block once its parent is deleted", (unsigned)id);
        }
        else
        {
            (void)snprintf(what, sizeof(what), "pattern %u gives back every block once its parent is deleted", (unsigned)id);
        }
        expect(0L == kept, what);
    }
}

/** @brief Every screen builds one canvas on its parent, touches nothing else and leaves no timer behind. */
static void check_screens_stay_on_their_parent(void)
{
    lv_obj_t *other = lv_obj_create(NULL);
    for ( uint32_t s = 0U; s < (uint32_t)KUI_TD_SCREEN_COUNT; s++ )
    {
        char what[96];
        lv_obj_t *parent = lv_obj_create(NULL);
        world_t const before = world(other);
        kui_td_screen_build(parent, (kui_td_screen_t)s, NULL, 0U);
        world_t const after = world(other);
        (void)snprintf(what, sizeof(what), "screen %u builds one canvas on its parent", (unsigned)s);
        expect(1U == lv_obj_get_child_count(parent), what);
        (void)snprintf(what, sizeof(what), "screen %u leaves every other parent alone", (unsigned)s);
        expect(same_objects(&before, &after), what);
        lv_obj_delete(parent);
        (void)snprintf(what, sizeof(what), "screen %u leaves no timer behind", (unsigned)s);
        expect(before.timers == timer_count(), what);
    }
    lv_obj_delete(other);
}

/** @brief Every pattern but OFF builds one canvas on its parent; OFF builds nothing; none touches anything else. */
static void check_patterns_stay_on_their_parent(void)
{
    lv_obj_t *other = lv_obj_create(NULL);
    for ( uint32_t p = 0U; p < (uint32_t)KUI_TD_PAT_COUNT; p++ )
    {
        char what[96];
        uint32_t want = 1U;
        lv_obj_t *parent = lv_obj_create(NULL);
        world_t const before = world(other);
        kui_td_pattern_build(parent, (kui_td_pattern_t)p, (uint8_t)KUI_TD_COLOUR_WHITE);
        world_t const after = world(other);
        if ( (uint32_t)KUI_TD_PAT_OFF == p )
        {
            want = 0U;
        }
        (void)snprintf(what, sizeof(what), "pattern %u builds %u canvas on its parent", (unsigned)p, (unsigned)want);
        expect(want == lv_obj_get_child_count(parent), what);
        (void)snprintf(what, sizeof(what), "pattern %u leaves every other parent and the timers alone", (unsigned)p);
        expect(same_objects(&before, &after) && (before.timers == after.timers), what);
        lv_obj_delete(parent);
    }
    lv_obj_delete(other);
}

/** @brief Out-of-range requests build nothing. */
static void check_refusals_build_nothing(void)
{
    lv_obj_t *parent = lv_obj_create(NULL);
    kui_td_screen_build(NULL, KUI_TD_SCREEN_AUTO, NULL, 0U);
    kui_td_pattern_build(NULL, KUI_TD_PAT_MARKS, 0U);
    kui_td_screen_build(parent, KUI_TD_SCREEN_COUNT, NULL, 0U);
    kui_td_pattern_build(parent, KUI_TD_PAT_COUNT, 0U);
    kui_td_pattern_build(parent, KUI_TD_PAT_COLOUR, (uint8_t)KUI_TD_COLOUR_COUNT);
    expect(0U == lv_obj_get_child_count(parent), "an unknown screen, pattern or colour builds nothing");
    lv_obj_delete(parent);
}

/** @brief The marks sit where Luismi's code and the analysis put them, 24 px white on black, with no witness. */
static void check_marks_geometry(void)
{
    lv_obj_t *parent = lv_obj_create(NULL);
    kui_td_pattern_build(parent, KUI_TD_PAT_MARKS, 0U);
    shot_t s = shoot(parent);
    bool ok = (NULL != s.buf);
    for ( uint32_t k = 0U; ok && (k < 4U); k++ )
    {
        int32_t const x = MARKS[k][0];
        int32_t const y = MARKS[k][1];
        ok = (0xFFFFU == px_at(&s, x, y))
             && (0xFFFFU == px_at(&s, x - CHECK_MARK_HALF, y - CHECK_MARK_HALF))
             && (0xFFFFU == px_at(&s, (x + CHECK_MARK_HALF) - 1, (y + CHECK_MARK_HALF) - 1))
             && (0x0000U == px_at(&s, (x - CHECK_MARK_HALF) - 1, y))
             && (0x0000U == px_at(&s, x + CHECK_MARK_HALF, y))
             && (0x0000U == px_at(&s, x, (y - CHECK_MARK_HALF) - 1))
             && (0x0000U == px_at(&s, x, y + CHECK_MARK_HALF));
    }
    expect(ok, "the four marks are 24 px white squares at Luismi's centres");
    expect((NULL != s.buf) && (0x0000U == px_at(&s, CHECK_BAND_X, CHECK_BAND_Y)), "the marks' band is black");
    expect((NULL != s.buf) && (0x0000U == px_at(&s, WITNESSES[0][0], WITNESSES[0][1])), "the marks carry no witness");
    if ( NULL != s.buf )
    {
        lv_draw_buf_destroy(s.buf);
    }
    lv_obj_delete(parent);
}

/** @brief The colours and the grey are exact, and the witnesses sit where Luismi's code puts them, on all but black. */
static void check_colours_and_witnesses(void)
{
    static const uint32_t RGB[KUI_TD_COLOUR_COUNT] = { 0x000000U, 0xFFFFFFU, 0xFF0000U, 0x00FF00U, 0x0000FFU };
    for ( uint32_t c = 0U; c < KUI_TD_COLOUR_COUNT; c++ )
    {
        char what[96];
        lv_obj_t *parent = lv_obj_create(NULL);
        kui_td_pattern_build(parent, KUI_TD_PAT_COLOUR, (uint8_t)c);
        shot_t s = shoot(parent);
        bool wit = (NULL != s.buf);
        uint16_t want_dot = c565(0xE12D2DU);
        uint16_t want_rim = 0x0000U;
        if ( 0U == c )
        {
            want_dot = 0x0000U;
        }
        for ( uint32_t k = 0U; wit && (k < 4U); k++ )
        {
            wit = (want_dot == px_at(&s, WITNESSES[k][0], WITNESSES[k][1]))
                  && (want_rim == px_at(&s, WITNESSES[k][0] + CHECK_RIM_DX, WITNESSES[k][1]));
        }
        (void)snprintf(what, sizeof(what), "colour %u fills the band with its exact value", (unsigned)c);
        expect((NULL != s.buf) && (c565(RGB[c]) == px_at(&s, CHECK_BAND_X, CHECK_BAND_Y)), what);
        (void)snprintf(what, sizeof(what), "colour %u carries Luismi's witnesses (none on black)", (unsigned)c);
        expect(wit, what);
        if ( NULL != s.buf )
        {
            lv_draw_buf_destroy(s.buf);
        }
        lv_obj_delete(parent);
    }
    for ( uint32_t p = (uint32_t)KUI_TD_PAT_OK; p <= (uint32_t)KUI_TD_PAT_FAIL; p++ )
    {
        char what[96];
        lv_obj_t *icon = lv_obj_create(NULL);
        kui_td_pattern_build(icon, (kui_td_pattern_t)p, 0U);
        shot_t si = shoot(icon);
        bool wit = (NULL != si.buf);
        for ( uint32_t k = 0U; wit && (k < 4U); k++ )
        {
            wit = (c565(0xE12D2DU) == px_at(&si, WITNESSES[k][0], WITNESSES[k][1]));
        }
        (void)snprintf(what, sizeof(what), "pattern %u carries Luismi's witnesses", (unsigned)p);
        expect(wit, what);
        if ( NULL != si.buf )
        {
            lv_draw_buf_destroy(si.buf);
        }
        lv_obj_delete(icon);
    }
    lv_obj_t *parent = lv_obj_create(NULL);
    kui_td_pattern_build(parent, KUI_TD_PAT_GREY, (uint8_t)KUI_TD_GREY_RUN);
    shot_t s = shoot(parent);
    expect((NULL != s.buf) && (c565(0x808080U) == px_at(&s, CHECK_BAND_X, CHECK_BAND_Y)), "grey 128 is (16, 32, 16) in RGB565");
    if ( NULL != s.buf )
    {
        lv_draw_buf_destroy(s.buf);
    }
    lv_obj_delete(parent);
}

/** @brief A grid cell set in place draws the same pixels as a screen built with that state, and a refused set changes nothing. */
static void check_grid_set_matches_a_rebuild(void)
{
    static const uint8_t STATES[4] = { KUI_TD_ST_PENDING, KUI_TD_ST_RUNNING, KUI_TD_ST_OK, KUI_TD_ST_FAILED };
    uint8_t arg[KUI_TD_GRID_CELLS];
    bool same = true;
    lv_obj_t *live = lv_obj_create(NULL);
    (void)memset(arg, KUI_TD_ST_PENDING, sizeof(arg));
    kui_td_screen_build(live, KUI_TD_SCREEN_AUTO, arg, sizeof(arg));
    for ( uint32_t i = 0U; same && (i < KUI_TD_GRID_CELLS); i++ )
    {
        uint8_t const st = STATES[i % 4U];
        lv_obj_t *fresh = lv_obj_create(NULL);
        arg[i] = st;
        kui_td_grid_set(live, (uint8_t)i, st);
        kui_td_screen_build(fresh, KUI_TD_SCREEN_AUTO, arg, sizeof(arg));
        shot_t a = shoot(live);
        shot_t b = shoot(fresh);
        same = same_pixels(&a, &b);
        if ( NULL != a.buf )
        {
            lv_draw_buf_destroy(a.buf);
        }
        if ( NULL != b.buf )
        {
            lv_draw_buf_destroy(b.buf);
        }
        lv_obj_delete(fresh);
    }
    expect(same, "every cell set in place matches a screen built with it");
    expect(1U == lv_obj_get_child_count(live), "a cell set in place adds no object");

    shot_t before = shoot(live);
    kui_td_grid_set(live, (uint8_t)KUI_TD_GRID_CELLS, KUI_TD_ST_OK);
    kui_td_grid_set(live, (uint8_t)CHECK_GRID_CELL, (uint8_t)'z');
    shot_t after = shoot(live);
    expect(same_pixels(&before, &after), "a cell past the grid or an unknown state changes nothing");
    lv_draw_buf_destroy(before.buf);
    lv_draw_buf_destroy(after.buf);
    lv_obj_delete(live);

    lv_obj_t *pat = lv_obj_create(NULL);
    kui_td_pattern_build(pat, KUI_TD_PAT_MARKS, 0U);
    before = shoot(pat);
    kui_td_grid_set(pat, (uint8_t)CHECK_GRID_CELL, KUI_TD_ST_OK);
    kui_td_grid_set(NULL, (uint8_t)CHECK_GRID_CELL, KUI_TD_ST_OK);
    after = shoot(pat);
    expect(same_pixels(&before, &after), "a cell set on anything but an auto screen changes nothing");
    lv_draw_buf_destroy(before.buf);
    lv_draw_buf_destroy(after.buf);
    lv_obj_delete(pat);
}

/** @brief The result is a green band for PASS and a red one for anything else. */
static void check_result_verdict(void)
{
    static const uint8_t VERDICTS[3] = { KUI_TD_ST_OK, KUI_TD_ST_FAILED, (uint8_t)'?' };
    static const uint32_t WANT[3] = { KUI_PALETTE_OK_GREEN, KUI_PALETTE_DANGER_RED, KUI_PALETTE_DANGER_RED };
    for ( uint32_t i = 0U; i < 3U; i++ )
    {
        lv_obj_t *parent = lv_obj_create(NULL);
        kui_td_screen_build(parent, KUI_TD_SCREEN_RESULT, &VERDICTS[i], 1U);
        shot_t s = shoot(parent);
        expect((NULL != s.buf) && (c565(WANT[i]) == px_at(&s, CHECK_BAND_X, CHECK_BAND_TOP_Y))
               && (0x0000U == px_at(&s, CHECK_BAND_X, KUI_TD_BAND_Y0_PX - 1)),
               "the result's band is green for PASS, red otherwise, and nothing above it");
        if ( NULL != s.buf )
        {
            lv_draw_buf_destroy(s.buf);
        }
        lv_obj_delete(parent);
    }
}

/** @brief Only a screen that moves owns a timer: the connectors, and an amber key or target. */
static void check_moving_screens_own_a_timer(void)
{
    static const uint8_t PAD[KUI_TD_PAD_KEYS] = { 'c', 'p', 'p', 'p', 'p', 'p' };
    uint32_t const base = timer_count();
    lv_obj_t *a = lv_obj_create(NULL);
    kui_td_screen_build(a, KUI_TD_SCREEN_CONNECTING, NULL, 0U);
    expect((base + 1U) == timer_count(), "the connecting screen owns one timer");
    lv_obj_delete(a);
    a = lv_obj_create(NULL);
    kui_td_screen_build(a, KUI_TD_SCREEN_BUTTONS, PAD, sizeof(PAD));
    expect((base + 1U) == timer_count(), "a key asked for blinks on a timer");
    lv_obj_delete(a);
    a = lv_obj_create(NULL);
    kui_td_screen_build(a, KUI_TD_SCREEN_BUTTONS, NULL, 0U);
    expect(base == timer_count(), "a pad with nothing asked owns no timer");
    lv_obj_delete(a);
    expect(base == timer_count(), "every timer went with its screen");
}

/**
 * @brief Run every check.
 * @return 0 when all passed, 1 otherwise.
 */
int main(void)
{
    lv_init();
    lv_display_t *disp = lv_display_create((int32_t)CHECK_W, (int32_t)CHECK_H);
    if ( NULL == disp )
    {
        return 1;
    }
    lv_display_set_buffers(disp, s_render, NULL, sizeof(s_render), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flush);
    kui_init(disp);

    check_screens_stay_on_their_parent();
    check_patterns_stay_on_their_parent();
    check_refusals_build_nothing();
    check_marks_geometry();
    check_colours_and_witnesses();
    check_grid_set_matches_a_rebuild();
    check_result_verdict();
    check_moving_screens_own_a_timer();
    check_nothing_leaks();

    if ( 0U != s_failed )
    {
        (void)printf("%u check(s) failed\n", (unsigned)s_failed);
        return 1;
    }
    (void)printf("every check passed\n");
    return 0;
}
