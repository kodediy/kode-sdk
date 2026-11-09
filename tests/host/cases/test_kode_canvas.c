/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_canvas.c
 * @brief The canvas rasterizer alone: exact pixels, clipping and colours.
 */
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

#include "kode.h"
#include "kode_screen_priv.h"

/** @brief Canary words on each side of the grid. */
#define GUARD_WORDS 32U

/** @brief The most pixels a fixture holds. */
#define GRID_MAX 256U

/** @brief What a guard word reads while untouched. */
#define CANARY 0xA5A5U

/** @brief The ink the cases draw with (pure red in RGB565). */
#define INK 0xF800U

/** @brief Characters of a failure message. */
#define MSG_BYTES 96U

/** @brief The grid and its guards. */
static uint16_t s_store[GUARD_WORDS + GRID_MAX + GUARD_WORDS];

/** @brief The grid inside s_store. */
static kode_raster_t s_grid;

/** @brief Make a blank grid of @p w by @p h with fresh guards. */
static void grid_make(uint32_t w, uint32_t h)
{
    TEST_ASSERT_TRUE((w * h) <= GRID_MAX);
    for ( uint32_t i = 0U; (uint32_t)(sizeof(s_store) / sizeof(s_store[0])) > i; i++ )
    {
        s_store[i] = CANARY;
    }
    s_grid.px = &s_store[GUARD_WORDS];
    s_grid.w  = w;
    s_grid.h  = h;
    for ( uint32_t i = 0U; (w * h) > i; i++ )
    {
        s_grid.px[i] = 0U;
    }
}

/** @brief Fail unless both guard bands are untouched. */
static void guards_hold(void)
{
    for ( uint32_t i = 0U; GUARD_WORDS > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(CANARY, s_store[i]);
        TEST_ASSERT_EQUAL_HEX16(CANARY, s_store[GUARD_WORDS + GRID_MAX + i]);
    }
    for ( uint32_t i = s_grid.w * s_grid.h; GRID_MAX > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(CANARY, s_grid.px[i]);
    }
}

/** @brief Fail unless the grid reads as the rows: '#' is INK, '.' is black. */
static void picture(const char *const *rows, uint32_t n)
{
    TEST_ASSERT_EQUAL_UINT32(s_grid.h, n);
    for ( uint32_t y = 0U; y < n; y++ )
    {
        TEST_ASSERT_EQUAL_UINT32(s_grid.w, (uint32_t)strlen(rows[y]));
        for ( uint32_t x = 0U; x < s_grid.w; x++ )
        {
            char msg[MSG_BYTES];

            (void)snprintf(msg, sizeof(msg), "pixel (%" PRIu32 ", %" PRIu32 ")", x, y);
            if ( '#' == rows[y][x] )
            {
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(INK, s_grid.px[(y * s_grid.w) + x], msg);
            }
            else
            {
                TEST_ASSERT_EQUAL_HEX16_MESSAGE(0U, s_grid.px[(y * s_grid.w) + x], msg);
            }
        }
    }
    guards_hold();
}

/** @brief The number of rows in a picture. */
#define ROWS(a) ((uint32_t)(sizeof(a) / sizeof((a)[0])))

/* ---- Colours --------------------------------------------------------------- */

TEST_CASE("a colour becomes RGB565, five, six and five bits", "[kode_canvas_t]")
{
    TEST_ASSERT_EQUAL_HEX16(0xF800U, kode_raster_rgb565(KODE_RED));
    TEST_ASSERT_EQUAL_HEX16(0x07E0U, kode_raster_rgb565(KODE_PURE_GREEN));
    TEST_ASSERT_EQUAL_HEX16(0x001FU, kode_raster_rgb565(KODE_PURE_BLUE));
    TEST_ASSERT_EQUAL_HEX16(0xFFFFU, kode_raster_rgb565(KODE_WHITE));
    TEST_ASSERT_EQUAL_HEX16(0x0000U, kode_raster_rgb565(KODE_BLACK));
    /* Kody blue 0x028AC2: red 0, green 0x8A >> 2 = 34, blue 0xC2 >> 3 = 24. */
    TEST_ASSERT_EQUAL_HEX16(0x0458U, kode_raster_rgb565(KODE_BLUE));
    /* Bold yellow 0xFFAC00: 31, 0xAC >> 2 = 43, 0. */
    TEST_ASSERT_EQUAL_HEX16(0xFD60U, kode_raster_rgb565(KODE_YELLOW));
    /* The top byte is not a colour. */
    TEST_ASSERT_EQUAL_HEX16(0x07E0U, kode_raster_rgb565(0xAB00FF00U));
}

/* ---- Clear and point ---------------------------------------------------------- */

TEST_CASE("clear fills every pixel and nothing else", "[kode_canvas_t]")
{
    grid_make(7U, 5U);
    kode_raster_clear(&s_grid, INK);
    for ( uint32_t i = 0U; (7U * 5U) > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(INK, s_grid.px[i]);
    }
    guards_hold();
}

TEST_CASE("a point sets one pixel, and one off the canvas is skipped at every edge", "[kode_canvas_t]")
{
    grid_make(6U, 4U);
    kode_raster_point(&s_grid, 0, 0, INK);
    kode_raster_point(&s_grid, 5, 3, INK);
    kode_raster_point(&s_grid, 2, 1, INK);
    static const char *const rows[] = {
        "#.....",
        "..#...",
        "......",
        ".....#",
    };
    picture(rows, ROWS(rows));

    kode_raster_point(&s_grid, -1, 0, INK);
    kode_raster_point(&s_grid, 6, 0, INK);
    kode_raster_point(&s_grid, 0, -1, INK);
    kode_raster_point(&s_grid, 0, 4, INK);
    kode_raster_point(&s_grid, -1, -1, INK);
    kode_raster_point(&s_grid, 6, 4, INK);
    kode_raster_point(&s_grid, KODE_DRAW_RANGE, -KODE_DRAW_RANGE, INK);
    picture(rows, ROWS(rows));
}

/* ---- Lines ----------------------------------------------------------------------- */

TEST_CASE("a diagonal line takes one pixel per column", "[kode_canvas_t]")
{
    grid_make(8U, 8U);
    kode_raster_line(&s_grid, 0, 0, 4, 4, INK);
    static const char *const rows[] = {
        "#.......",
        ".#......",
        "..#.....",
        "...#....",
        "....#...",
        "........",
        "........",
        "........",
    };
    picture(rows, ROWS(rows));

    grid_make(8U, 8U);
    kode_raster_line(&s_grid, 6, 1, 2, 5, INK);
    static const char *const back[] = {
        "........",
        "......#.",
        ".....#..",
        "....#...",
        "...#....",
        "..#.....",
        "........",
        "........",
    };
    picture(back, ROWS(back));
}

TEST_CASE("a shallow line and a steep one round to the nearest pixel", "[kode_canvas_t]")
{
    grid_make(8U, 8U);
    kode_raster_line(&s_grid, 0, 0, 5, 2, INK);
    static const char *const shallow[] = {
        "##......",
        "..##....",
        "....##..",
        "........",
        "........",
        "........",
        "........",
        "........",
    };
    picture(shallow, ROWS(shallow));

    grid_make(8U, 8U);
    kode_raster_line(&s_grid, 0, 0, 2, 5, INK);
    static const char *const steep[] = {
        "#.......",
        "#.......",
        ".#......",
        ".#......",
        "..#.....",
        "..#.....",
        "........",
        "........",
    };
    picture(steep, ROWS(steep));
}

TEST_CASE("a line half way between two pixels takes the one further along", "[kode_canvas_t]")
{
    grid_make(8U, 4U);
    kode_raster_line(&s_grid, 0, 0, 4, 2, INK);
    static const char *const forward[] = {
        "#.......",
        ".##.....",
        "...##...",
        "........",
    };
    picture(forward, ROWS(forward));

    grid_make(8U, 4U);
    kode_raster_line(&s_grid, 4, 2, 0, 0, INK);
    static const char *const backward[] = {
        "##......",
        "..##....",
        "....#...",
        "........",
    };
    picture(backward, ROWS(backward));
}

TEST_CASE("horizontal, vertical and zero-length lines", "[kode_canvas_t]")
{
    grid_make(6U, 5U);
    kode_raster_line(&s_grid, 1, 1, 4, 1, INK);
    kode_raster_line(&s_grid, 2, 4, 2, 2, INK);
    kode_raster_line(&s_grid, 5, 0, 5, 0, INK);
    static const char *const rows[] = {
        ".....#",
        ".####.",
        "..#...",
        "..#...",
        "..#...",
    };
    picture(rows, ROWS(rows));
}

/**
 * @brief The textbook Bresenham loop, plotting into @p seen (w by h bytes).
 *
 * Written out here as the reference the rasterizer is held to: error term,
 * two compares, no closed form.
 */
static void ref_line(uint8_t *seen, int32_t w, int32_t h,
                     int32_t x0, int32_t y0, int32_t x1, int32_t y1)
{
    int32_t dx = x1 - x0;
    int32_t dy = y1 - y0;
    int32_t sx = 1;
    int32_t sy = 1;

    if ( 0 > dx )
    {
        dx = -dx;
        sx = -1;
    }
    if ( 0 < dy )
    {
        dy = -dy;      /* the textbook keeps the vertical run negative */
    }
    if ( y1 < y0 )
    {
        sy = -1;
    }
    int32_t err = dx + dy;
    int32_t x = x0;
    int32_t y = y0;
    bool done = false;

    while ( !done )
    {
        if ( (0 <= x) && (x < w) && (0 <= y) && (y < h) )
        {
            seen[(y * w) + x] = 1U;
        }
        if ( (x == x1) && (y == y1) )
        {
            done = true;
        }
        else
        {
            const int32_t e2 = 2 * err;

            if ( e2 >= dy )
            {
                err += dy;
                x += sx;
            }
            if ( e2 <= dx )
            {
                err += dx;
                y += sy;
            }
        }
    }
}

/** @brief Width of the grid the sweep draws on. */
#define SWEEP_W 7U

/** @brief Height of the grid the sweep draws on. */
#define SWEEP_H 6U

/** @brief The lowest endpoint coordinate the sweep tries: off the grid. */
#define SWEEP_LO ((int32_t)-3)

/** @brief The highest endpoint coordinate the sweep tries: off the grid. */
#define SWEEP_HI ((int32_t)10)

/** @brief Fewest lines the sweep must have compared. */
#define SWEEP_LINES_MIN 5000U

TEST_CASE("every line of a small grid is the textbook Bresenham line", "[kode_canvas_t]")
{
    uint8_t  seen[SWEEP_W * SWEEP_H];
    uint32_t lines = 0U;

    grid_make(SWEEP_W, SWEEP_H);
    for ( int32_t y0 = SWEEP_LO; SWEEP_HI >= y0; y0 += 2 )
    {
        for ( int32_t x0 = SWEEP_LO; SWEEP_HI >= x0; x0++ )
        {
            for ( int32_t y1 = SWEEP_LO; SWEEP_HI >= y1; y1 += 3 )
            {
                for ( int32_t x1 = SWEEP_LO; SWEEP_HI >= x1; x1++ )
                {
                    (void)memset(seen, 0, sizeof(seen));
                    ref_line(seen, (int32_t)SWEEP_W, (int32_t)SWEEP_H, x0, y0, x1, y1);
                    for ( uint32_t i = 0U; (SWEEP_W * SWEEP_H) > i; i++ )
                    {
                        s_grid.px[i] = 0U;
                    }
                    kode_raster_line(&s_grid, x0, y0, x1, y1, INK);
                    for ( uint32_t i = 0U; (SWEEP_W * SWEEP_H) > i; i++ )
                    {
                        if ( (0U != seen[i]) != (INK == s_grid.px[i]) )
                        {
                            char msg[MSG_BYTES];

                            (void)snprintf(msg, sizeof(msg),
                                           "line (%" PRId32 ", %" PRId32 ") to (%" PRId32 ", %"
                                           PRId32 "), pixel %" PRIu32, x0, y0, x1, y1, i);
                            TEST_FAIL_MESSAGE(msg);
                        }
                    }
                    lines++;
                }
            }
        }
    }
    guards_hold();
    TEST_ASSERT_TRUE(SWEEP_LINES_MIN < lines);
}

TEST_CASE("a line that starts and ends far off the canvas draws only what crosses it", "[kode_canvas_t]")
{
    grid_make(8U, 6U);
    kode_raster_line(&s_grid, -1000, 2, 1000, 2, INK);     /* both side edges */
    kode_raster_line(&s_grid, 5, -1000, 5, 1000, INK);     /* top and bottom  */
    static const char *const cross[] = {
        ".....#..",
        ".....#..",
        "########",
        ".....#..",
        ".....#..",
        ".....#..",
    };
    picture(cross, ROWS(cross));

    grid_make(6U, 6U);
    kode_raster_line(&s_grid, -20, -20, 30, 30, INK);      /* the diagonal */
    static const char *const diagonal[] = {
        "#.....",
        ".#....",
        "..#...",
        "...#..",
        "....#.",
        ".....#",
    };
    picture(diagonal, ROWS(diagonal));

    grid_make(6U, 6U);
    kode_raster_line(&s_grid, -1, 6, 6, -1, INK);          /* the other diagonal, a pixel off each end */
    static const char *const anti[] = {
        ".....#",
        "....#.",
        "...#..",
        "..#...",
        ".#....",
        "#.....",
    };
    picture(anti, ROWS(anti));

    grid_make(6U, 6U);
    kode_raster_line(&s_grid, KODE_DRAW_RANGE, KODE_DRAW_RANGE, -KODE_DRAW_RANGE,
                     -KODE_DRAW_RANGE, INK);               /* the longest there is */
    picture(diagonal, ROWS(diagonal));

    grid_make(6U, 6U);
    kode_raster_line(&s_grid, -50, -50, -40, -40, INK);    /* wholly off */
    kode_raster_line(&s_grid, 100, 0, 100, 5, INK);
    kode_raster_line(&s_grid, 0, 100, 5, 100, INK);
    static const char *const empty[] = {
        "......", "......", "......", "......", "......", "......",
    };
    picture(empty, ROWS(empty));
}

/* ---- Rectangles ------------------------------------------------------------------- */

TEST_CASE("a filled rectangle and its outline", "[kode_canvas_t]")
{
    grid_make(8U, 6U);
    kode_raster_rect(&s_grid, 2, 1, 3, 2, INK, true);
    static const char *const filled[] = {
        "........",
        "..###...",
        "..###...",
        "........",
        "........",
        "........",
    };
    picture(filled, ROWS(filled));

    grid_make(8U, 6U);
    kode_raster_rect(&s_grid, 1, 1, 5, 4, INK, false);
    static const char *const outline[] = {
        "........",
        ".#####..",
        ".#...#..",
        ".#...#..",
        ".#####..",
        "........",
    };
    picture(outline, ROWS(outline));

    grid_make(8U, 6U);
    kode_raster_rect(&s_grid, 1, 1, 1, 3, INK, false);     /* a bar: both edges are one column */
    kode_raster_rect(&s_grid, 4, 4, 3, 1, INK, false);     /* and a row */
    static const char *const thin[] = {
        "........",
        ".#......",
        ".#......",
        ".#......",
        "....###.",
        "........",
    };
    picture(thin, ROWS(thin));
}

TEST_CASE("a rectangle is clipped at every edge", "[kode_canvas_t]")
{
    grid_make(6U, 5U);
    kode_raster_rect(&s_grid, -2, -1, 4, 3, INK, true);    /* past the left and the top */
    kode_raster_rect(&s_grid, 4, 3, 5, 5, INK, true);      /* past the right and the bottom */
    static const char *const filled[] = {
        "##....",
        "##....",
        "......",
        "....##",
        "....##",
    };
    picture(filled, ROWS(filled));

    grid_make(6U, 5U);
    kode_raster_rect(&s_grid, -1, -1, 4, 4, INK, false);   /* its left and top edges are off the canvas */
    static const char *const outline[] = {
        "..#...",
        "..#...",
        "###...",
        "......",
        "......",
    };
    picture(outline, ROWS(outline));

    grid_make(6U, 5U);
    kode_raster_rect(&s_grid, -100, -100, 300, 300, INK, true);
    for ( uint32_t i = 0U; (6U * 5U) > i; i++ )
    {
        TEST_ASSERT_EQUAL_HEX16(INK, s_grid.px[i]);
    }
    guards_hold();

    grid_make(6U, 5U);
    kode_raster_rect(&s_grid, 6, 0, 3, 3, INK, true);      /* wholly off */
    kode_raster_rect(&s_grid, -3, 0, 3, 3, INK, true);
    kode_raster_rect(&s_grid, 0, 5, 3, 3, INK, false);
    kode_raster_rect(&s_grid, 0, -3, 3, 3, INK, false);
    kode_raster_rect(&s_grid, 2, 2, 0, 3, INK, true);      /* no width */
    kode_raster_rect(&s_grid, 2, 2, 3, 0, INK, false);     /* no height */
    static const char *const empty[] = {
        "......", "......", "......", "......", "......",
    };
    picture(empty, ROWS(empty));
}

/* ---- Circles ---------------------------------------------------------------------- */

TEST_CASE("a circle of radius 3, outlined and filled", "[kode_canvas_t]")
{
    grid_make(9U, 9U);
    kode_raster_circle(&s_grid, 4, 4, 3, INK, false);
    static const char *const outline[] = {
        ".........",
        "...###...",
        "..#...#..",
        ".#.....#.",
        ".#.....#.",
        ".#.....#.",
        "..#...#..",
        "...###...",
        ".........",
    };
    picture(outline, ROWS(outline));

    grid_make(9U, 9U);
    kode_raster_circle(&s_grid, 4, 4, 3, INK, true);
    static const char *const disc[] = {
        ".........",
        "...###...",
        "..#####..",
        ".#######.",
        ".#######.",
        ".#######.",
        "..#####..",
        "...###...",
        ".........",
    };
    picture(disc, ROWS(disc));
}

TEST_CASE("the smallest circles", "[kode_canvas_t]")
{
    grid_make(5U, 5U);
    kode_raster_circle(&s_grid, 2, 2, 0, INK, false);
    kode_raster_circle(&s_grid, 0, 4, 0, INK, true);
    static const char *const dots[] = {
        ".....",
        ".....",
        "..#..",
        ".....",
        "#....",
    };
    picture(dots, ROWS(dots));

    grid_make(5U, 5U);
    kode_raster_circle(&s_grid, 2, 2, 1, INK, false);
    static const char *const ring[] = {
        ".....",
        "..#..",
        ".#.#.",
        "..#..",
        ".....",
    };
    picture(ring, ROWS(ring));

    grid_make(5U, 5U);
    kode_raster_circle(&s_grid, 2, 2, 1, INK, true);
    static const char *const plus[] = {
        ".....",
        "..#..",
        ".###.",
        "..#..",
        ".....",
    };
    picture(plus, ROWS(plus));
}

TEST_CASE("a circle is clipped at every edge", "[kode_canvas_t]")
{
    grid_make(7U, 7U);
    kode_raster_circle(&s_grid, 0, 0, 3, INK, false);      /* a quarter, in the top-left corner */
    static const char *const quarter[] = {
        "...#...",
        "...#...",
        "..#....",
        "##.....",
        ".......",
        ".......",
        ".......",
    };
    picture(quarter, ROWS(quarter));

    grid_make(7U, 7U);
    kode_raster_circle(&s_grid, 6, 6, 3, INK, true);       /* a quarter disc, bottom-right */
    static const char *const disc[] = {
        ".......",
        ".......",
        ".......",
        ".....##",
        "....###",
        "...####",
        "...####",
    };
    picture(disc, ROWS(disc));

    grid_make(7U, 7U);
    kode_raster_circle(&s_grid, -9, 3, 5, INK, true);      /* wholly off, to the left */
    kode_raster_circle(&s_grid, 3, 30, 5, INK, false);     /* below */
    kode_raster_circle(&s_grid, 3, 3, KODE_DRAW_RANGE, INK, false);  /* the canvas deep inside it */
    kode_raster_circle(&s_grid, KODE_DRAW_RANGE, -KODE_DRAW_RANGE, KODE_DRAW_RANGE, INK, true);
    static const char *const empty[] = {
        ".......", ".......", ".......", ".......", ".......", ".......", ".......",
    };
    picture(empty, ROWS(empty));
}

/** @brief Side of the grid the disc sweep draws on. */
#define DISC_N 15U

/** @brief Its centre, and so the largest radius that fits. */
#define DISC_C 7U

TEST_CASE("a disc holds its whole outline, for every radius, and is whole and symmetric", "[kode_canvas_t]")
{
    uint16_t outline[DISC_N * DISC_N];

    for ( uint32_t r = 0U; DISC_C >= r; r++ )
    {
        grid_make(DISC_N, DISC_N);
        kode_raster_circle(&s_grid, (int32_t)DISC_C, (int32_t)DISC_C, (int32_t)r, INK, false);
        (void)memcpy(outline, s_grid.px, sizeof(outline));
        kode_raster_circle(&s_grid, (int32_t)DISC_C, (int32_t)DISC_C, (int32_t)r, INK, true);
        for ( uint32_t y = 0U; DISC_N > y; y++ )
        {
            uint32_t first = DISC_N;
            uint32_t last  = 0U;

            for ( uint32_t x = 0U; DISC_N > x; x++ )
            {
                const uint16_t here = s_grid.px[(y * DISC_N) + x];

                if ( INK == outline[(y * DISC_N) + x] )
                {
                    TEST_ASSERT_EQUAL_HEX16(INK, here);
                }
                TEST_ASSERT_EQUAL_HEX16(here, s_grid.px[(y * DISC_N) + (DISC_N - 1U - x)]);
                TEST_ASSERT_EQUAL_HEX16(here, s_grid.px[((DISC_N - 1U - y) * DISC_N) + x]);
                if ( INK == here )
                {
                    if ( DISC_N == first )
                    {
                        first = x;
                    }
                    last = x;
                }
            }
            for ( uint32_t x = first; (DISC_N > first) && (x <= last); x++ )
            {
                TEST_ASSERT_EQUAL_HEX16(INK, s_grid.px[(y * DISC_N) + x]);
            }
        }
        guards_hold();
    }
}
