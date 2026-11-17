/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_compose.h
 * @brief The ESP32-P4's pixel accelerator (PPA): fill, scale and blend on the caller's pictures.
 */
#ifndef KDRV_COMPOSE_H_
#define KDRV_COMPOSE_H_

#include <stdbool.h>
#include <stdint.h>

#include "kbsp_board.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KDRV_COMPOSE_ALIGN      64U   /**< An output's address and size are multiples of this: a cache line. */
#define KDRV_COMPOSE_PX_BYTES   2U    /**< RGB565. */
#define KDRV_COMPOSE_SCALE_ONE  16U   /**< The same size, in the accelerator's sixteenths. */
#define KDRV_COMPOSE_SCALE_MIN  1U    /**< The smallest scale: one sixteenth. */
#define KDRV_COMPOSE_SCALE_MAX  256U  /**< The largest scale: sixteen times. */
/* cppcheck-suppress misra-c2012-2.5 ; read by the blend, built with CONFIG_KODE_COMPOSE */
#define KDRV_COMPOSE_OPA_MAX    255U  /**< A foreground that covers what is under it. */
#define KDRV_COMPOSE_TURNS      4U    /**< Quarter turns in a whole one: a turn is 0 to 3. */

/**
 * The longest a picture's side may be, in pixels: the glass's longer side.
 * A picture is then at most 1.23 times the glass, so the largest pass - one
 * AO_Display runs whole inside a step - is about one pass over a full frame.
 */
#define KDRV_COMPOSE_SIDE_MAX   KBSP_LCD_V_RES

/** @brief A picture: RGB565, row after row from the top, @c w * 2 bytes a row. */
typedef struct {
    uint8_t *px;     /**< The pixels. */
    uint32_t bytes;  /**< The allocation's size, at least @c w * @c h * 2. */
    uint16_t w;      /**< Width in pixels. */
    uint16_t h;      /**< Height in pixels. */
} kdrv_compose_pic_t;

/** @brief A block of a picture, in pixels from its top left. */
typedef struct {
    uint16_t x;  /**< First column. */
    uint16_t y;  /**< First row. */
    uint16_t w;  /**< Columns. */
    uint16_t h;  /**< Rows. */
} kdrv_compose_rect_t;

/**
 * @brief Register with the accelerator, for scaling. Idempotent; takes no picture memory.
 *
 * A fill and a blend register their own clients at their first pass. Those
 * two are built with CONFIG_KODE_COMPOSE; the mirror needs the scale alone.
 *
 * @return KODE_OK; KODE_NO_RESOURCES when the accelerator gave no client.
 */
kode_status_t kdrv_compose_init(void);

/**
 * @brief Paint a block of @p dst one colour.
 *
 * The PPA's owner's thread only, as every pass here. An output picture's
 * address and @c bytes are multiples of KDRV_COMPOSE_ALIGN, and no picture
 * has a side longer than KDRV_COMPOSE_SIDE_MAX.
 *
 * @param dst      The picture written.
 * @param at       The block.
 * @param argb8888 The colour, 0xAARRGGBB; the alpha is not used.
 * @return KODE_OK; KODE_DENIED for a NULL, malformed or oversized picture or
 *         a block that does not fit; KODE_NO_RESOURCES with no accelerator;
 *         KODE_ERROR when the accelerator refused the pass.
 */
kode_status_t kdrv_compose_fill(const kdrv_compose_pic_t *dst, const kdrv_compose_rect_t *at,
                                uint32_t argb8888);

/**
 * @brief Copy a block of @p src into @p dst, scaled and turned.
 *
 * @param dst  The picture written; never @p src.
 * @param x    Column of @p dst the block's left edge lands on.
 * @param y    Row of @p dst the block's top edge lands on.
 * @param src  The picture read.
 * @param from The block of @p src.
 * @param sx    Scale along the block's own width, in sixteenths,
 *              KDRV_COMPOSE_SCALE_MIN to _MAX.
 * @param sy    Scale along its height, likewise.
 * @param turns Quarter turns counter-clockwise the block is turned on its
 *              way, 0 to 3; an odd one lands it as tall as it was wide.
 * @return As kdrv_compose_fill(); KODE_DENIED also for a scale or a turn out
 *         of range, or a result that is empty or does not fit @p dst.
 */
kode_status_t kdrv_compose_scale(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                 const kdrv_compose_pic_t *src, const kdrv_compose_rect_t *from,
                                 uint16_t sx, uint16_t sy, uint8_t turns);

/**
 * @brief Lay a block of @p fg over @p dst: each pixel is @p fg at @p opa over what @p dst held.
 *
 * @param dst  The picture under it, written in place; never @p fg.
 * @param x    Column of @p dst the block's left edge lands on.
 * @param y    Row of @p dst the block's top edge lands on.
 * @param fg   The picture laid over.
 * @param from The block of @p fg.
 * @param opa  0 leaves @p dst as it is, KDRV_COMPOSE_OPA_MAX replaces it.
 * @return As kdrv_compose_fill().
 */
kode_status_t kdrv_compose_blend(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                 const kdrv_compose_pic_t *fg, const kdrv_compose_rect_t *from,
                                 uint8_t opa);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_COMPOSE_H_ */
