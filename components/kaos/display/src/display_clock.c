/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_clock.c
 * @brief Composes the time's rectangle from the renderer's glyph strip.
 */
#include "display_clock.h"

#include <string.h>

#include "kbsp_board.h"
#include "kqp.h"

/** @brief Cells of the time: h, h, colon, m, m. */
#define CLOCK_CELLS   5U

/** @brief The cell that holds the colon. */
#define CELL_COLON    2U

/** @brief Radix of a clock digit. */
#define DECIMAL       10U

/** @brief Hours in a day. */
#define HOURS_DAY     24U

/** @brief Hours on a 12 h dial. */
#define HOURS_DIAL    12U

/** @brief Minutes in an hour. */
#define MINUTES_HOUR  60U

/** @brief The panel's window is even in every direction. */
#define EVEN_MASK     1U

uint16_t display_clock_width(const DisplayGlyphsEvt *g)
{
    uint32_t const w = (KODE_LOWPOWER_CLOCK_DIGITS * (uint32_t)g->glyph_w) + (uint32_t)g->colon_w;

    return (uint16_t)w;
}

bool display_clock_usable(const DisplayGlyphsEvt *g)
{
    bool usable = false;

    if ( (NULL != g) && (NULL != g->strip) && (0U < g->glyph_w)
        && (0U < g->glyph_h) && (0U < g->colon_w) )
    {
        uint32_t const w = (KODE_LOWPOWER_CLOCK_DIGITS * (uint32_t)g->glyph_w)
                         + (uint32_t)g->colon_w;
        uint32_t const h = (uint32_t)g->glyph_h;
        uint32_t const x = (uint32_t)g->x;
        uint32_t const y = (uint32_t)g->y;
        bool const even = (((w | h | x | y) & EVEN_MASK) == 0U);

        usable = even
              && ((x + w) <= (uint32_t)KBSP_LCD_H_RES)
              && ((y + h) <= (uint32_t)KBSP_LCD_V_RES);
    }
    return usable;
}

/**
 * @brief The strip column a cell is copied from, and its width.
 *
 * @param g     The glyphs.
 * @param slot  0..CLOCK_CELLS - 1.
 * @param digit What the cell shows, when it is not the colon.
 * @param width Receives the cell's width in pixels.
 * @return The first column of the glyph in the strip.
 */
static size_t cell_source(const DisplayGlyphsEvt *g, uint8_t slot, uint8_t digit,
                          size_t *width)
{
    size_t col = (size_t)KODE_LOWPOWER_GLYPH_DIGITS * g->glyph_w;

    *width = g->colon_w;
    if ( (uint8_t)CELL_COLON != slot )
    {
        col    = (size_t)digit * g->glyph_w;
        *width = g->glyph_w;
    }
    return col;
}

/**
 * @brief The hour as the renderer's clock reads it.
 *
 * @param g    The glyphs; h12 picks the dial.
 * @param hour 0..23.
 * @return The hour modulo 24, or on a 12 h dial 1..12 (midnight and noon read
 *         12) with no AM or PM.
 */
static uint8_t clock_hour(const DisplayGlyphsEvt *g, uint8_t hour)
{
    uint8_t hh = (uint8_t)(hour % HOURS_DAY);

    if ( g->h12 )
    {
        hh = (uint8_t)(hh % HOURS_DIAL);
        if ( 0U == hh )
        {
            hh = (uint8_t)HOURS_DIAL;
        }
    }
    return hh;
}

void display_clock_compose(const DisplayGlyphsEvt *g, uint8_t hour,
                           uint8_t minute, uint16_t *out)
{
    uint8_t const hh = clock_hour(g, hour);
    uint8_t const mm = (uint8_t)(minute % MINUTES_HOUR);
    uint8_t const digits[CLOCK_CELLS] = {
        (uint8_t)(hh / DECIMAL), (uint8_t)(hh % DECIMAL), 0U,
        (uint8_t)(mm / DECIMAL), (uint8_t)(mm % DECIMAL)
    };
    /* The tens cell of a one-figure hour on the 12 h dial says nothing. */
    bool const blank_tens = g->h12 && (DECIMAL > hh);
    size_t const strip_w = ((size_t)KODE_LOWPOWER_GLYPH_DIGITS * g->glyph_w) + g->colon_w;
    size_t const out_w   = display_clock_width(g);

    if ( 0U != g->adv[0] )
    {
        /* The renderer's own spacing: the figures, each from where the one
         * before it says, over black. Only what they draw is copied, as their
         * cells overlap. */
        (void)memset(out, 0, out_w * g->glyph_h * sizeof(uint16_t));
        size_t at = 0U;

        for ( uint8_t slot = 0U; (uint8_t)CLOCK_CELLS > slot; slot++ )
        {
            if ( (0U != slot) || !blank_tens )
            {
                size_t width = 0U;
                size_t const col = cell_source(g, slot, digits[slot], &width);
                uint8_t const which = ((uint8_t)CELL_COLON == slot)
                                      ? (uint8_t)KODE_LOWPOWER_GLYPH_DIGITS : digits[slot];

                for ( size_t row = 0U; row < g->glyph_h; row++ )
                {
                    uint16_t const * const from = &g->strip[(row * strip_w) + col];
                    uint16_t * const to = &out[row * out_w];

                    for ( size_t x = 0U; (x < width) && ((at + x) < out_w); x++ )
                    {
                        if ( 0U != from[x] )
                        {
                            to[at + x] = from[x];
                        }
                    }
                }
                at += g->adv[which];
            }
        }
        return;
    }

    for ( size_t row = 0U; row < g->glyph_h; row++ )
    {
        size_t at = row * out_w;

        for ( uint8_t slot = 0U; (uint8_t)CLOCK_CELLS > slot; slot++ )
        {
            size_t width = 0U;
            size_t const col = cell_source(g, slot, digits[slot], &width);

            if ( (0U == slot) && blank_tens )
            {
                /* The page's black, which is what the strip's cells stand on. */
                (void)memset(&out[at], 0, width * sizeof(uint16_t));
            }
            else
            {
                (void)memcpy(&out[at], &g->strip[(row * strip_w) + col],
                             width * sizeof(uint16_t));
            }
            at += width;
        }
    }
}
