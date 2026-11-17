/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_compose_check.h
 * @brief Whether a pass fits its pictures: the arithmetic, with no accelerator.
 */
#ifndef KDRV_COMPOSE_CHECK_H_
#define KDRV_COMPOSE_CHECK_H_

#include "kdrv_compose.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief How many pixels @p pixels become at a scale, as the accelerator counts them.
 * @param pixels     A width or a height.
 * @param sixteenths The scale; KDRV_COMPOSE_SCALE_ONE is the same size.
 * @return @p pixels * @p sixteenths / 16, rounded down.
 */
uint32_t kdrv_compose_scaled(uint32_t pixels, uint16_t sixteenths);

/**
 * @brief The size a block lands with after a scale and a turn.
 * @param from  The block.
 * @param sx    Scale along its width, in sixteenths.
 * @param sy    Scale along its height.
 * @param turns Quarter turns; an odd one swaps the two sides.
 * @param w     Receives the columns written.
 * @param h     Receives the rows written.
 */
void kdrv_compose_landed(const kdrv_compose_rect_t *from, uint16_t sx, uint16_t sy, uint8_t turns,
                         uint32_t *w, uint32_t *h);

/**
 * @brief Whether kdrv_compose_fill() may run.
 * @param dst The picture written.
 * @param at  The block.
 * @return KODE_OK, or KODE_DENIED.
 */
kode_status_t kdrv_compose_check_fill(const kdrv_compose_pic_t *dst, const kdrv_compose_rect_t *at);

/**
 * @brief Whether kdrv_compose_scale() may run.
 * @param dst  The picture written.
 * @param x    Column the block lands on.
 * @param y    Row the block lands on.
 * @param src  The picture read.
 * @param from The block of @p src.
 * @param sx    Scale along the block's width, in sixteenths.
 * @param sy    Scale along its height.
 * @param turns Quarter turns, 0 to 3.
 * @return KODE_OK, or KODE_DENIED.
 */
kode_status_t kdrv_compose_check_scale(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                       const kdrv_compose_pic_t *src,
                                       const kdrv_compose_rect_t *from, uint16_t sx, uint16_t sy,
                                       uint8_t turns);

/**
 * @brief Whether kdrv_compose_blend() may run.
 * @param dst  The picture written in place.
 * @param x    Column the block lands on.
 * @param y    Row the block lands on.
 * @param fg   The picture laid over.
 * @param from The block of @p fg.
 * @return KODE_OK, or KODE_DENIED.
 */
kode_status_t kdrv_compose_check_blend(const kdrv_compose_pic_t *dst, uint16_t x, uint16_t y,
                                       const kdrv_compose_pic_t *fg,
                                       const kdrv_compose_rect_t *from);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_COMPOSE_CHECK_H_ */
