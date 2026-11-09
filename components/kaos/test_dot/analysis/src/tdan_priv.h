/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file tdan_priv.h
 * @brief What the analysis' three files share and nobody else sees.
 */
#ifndef TDAN_PRIV_H_
#define TDAN_PRIV_H_

#include <stdint.h>

#include "tdan.h"

#define TDAN_YIELD_EVERY_8   7U    /**< Yield mask: every 8 rows. */
#define TDAN_YIELD_EVERY_16  15U   /**< Yield mask: every 16 rows. */
#define TDAN_YIELD_EVERY_32  31U   /**< Yield mask: every 32 rows. */
#define TDAN_YIELD_EVERY_4   3U    /**< Yield mask: every 4 windows. */
#define TDAN_SAMPLE_EDGE     0.001f   /**< A bilinear sample stays this far inside the last row and column. */
#define TDAN_BILINEAR_TAPS   4        /**< Pixels a bilinear sample weighs. */
#define TDAN_RGB565_R_SHIFT  11U      /**< Red's place in RGB565. */
#define TDAN_RGB565_G_SHIFT  5U       /**< Green's place. */
#define TDAN_RGB565_5BIT     31U      /**< A 5-bit channel. */
#define TDAN_RGB565_6BIT     63U      /**< The 6-bit green. */
#define TDAN_EXPAND5_UP      3U       /**< A 5-bit channel to 8 bits: shifted up this far ... */
#define TDAN_EXPAND5_DOWN    2U       /**< ... and its top bits repeated below, shifted down this far. */
#define TDAN_EXPAND6_UP      2U       /**< The 6-bit green to 8 bits: shifted up this far ... */
#define TDAN_EXPAND6_DOWN    4U       /**< ... and its top bits repeated below, shifted down this far. */
#define TDAN_CH_R            0        /**< Channel index: red. */
#define TDAN_CH_G            1        /**< Channel index: green. */
#define TDAN_CH_B            2        /**< Channel index: blue. */

/**
 * @brief Call the yield hook when the low bits of @p i are all set in @p mask.
 * @param i    The loop counter.
 * @param mask How often: 2^n - 1.
 */
void tdan_yield_point(uint32_t i, uint32_t mask);

/**
 * @brief Absolute value.
 * @param v The value.
 * @return |v|.
 */
int32_t tdan_iabs(int32_t v);

/**
 * @brief Bilinear sample of a luma photo, clamped to its edges.
 * @param l The luma.
 * @param w Width.
 * @param h Height.
 * @param u Photo x.
 * @param v Photo y.
 * @return The sample.
 */
float tdan_bilinear(const uint8_t *l, int32_t w, int32_t h, float u, float v);

/**
 * @brief One RGB565 pixel as three 8-bit channels, the top bits repeated in the low ones (31 -> 255, 63 -> 255).
 * @param px  The pixel.
 * @param rgb Red, green and blue out, 0..255.
 */
void tdan_rgb565_expand(uint16_t px, uint32_t rgb[TDAN_CHANNELS]);

#endif /* TDAN_PRIV_H_ */
