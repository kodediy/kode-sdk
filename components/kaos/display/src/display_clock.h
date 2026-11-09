/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file display_clock.h
 * @brief The time on the glass while the board sleeps, composed from the renderer's glyphs.
 */
#ifndef DISPLAY_CLOCK_H_
#define DISPLAY_CLOCK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kaos.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Whether a hand-over can be written into the panel's memory.
 *
 * The panel takes a window that starts on an even column and an even row and
 * is an even number of columns and rows wide, and the rectangle of the time -
 * four digits and the colon, side by side - must lie on the glass. A hand-over
 * that fails any of that is as good as no glyphs.
 *
 * @param g The glyphs and where they go.
 * @return true when display_clock_compose() output can be written.
 */
bool display_clock_usable(const DisplayGlyphsEvt *g);

/**
 * @brief Width, in pixels, of the rectangle of the time.
 * @param g Glyphs that display_clock_usable() accepted.
 * @return Four digits and the colon.
 */
uint16_t display_clock_width(const DisplayGlyphsEvt *g);

/**
 * @brief Compose hh:mm from the strip, row after row with no padding.
 *
 * @param g      Glyphs that display_clock_usable() accepted.
 * @param hour   0..23; a larger value is taken modulo 24. On the 12 h dial
 *               (DisplayGlyphsEvt::h12) it is read 1..12, and an hour of one
 *               figure leaves its tens cell black.
 * @param minute 0..59; a larger value is taken modulo 60.
 * @param out    display_clock_width(g) * g->glyph_h pixels, RGB565.
 */
void display_clock_compose(const DisplayGlyphsEvt *g, uint8_t hour,
                           uint8_t minute, uint16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_CLOCK_H_ */
