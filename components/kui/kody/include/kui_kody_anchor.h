/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_kody_anchor.h
 * @brief Where Kody may change what he does: anchors and crossings, as plain integers.
 */
#ifndef KUI_KODY_ANCHOR_H
#define KUI_KODY_ANCHOR_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The first anchor after a frame.
 *
 * Strictly after: a playhead standing on an anchor waits for the next one.
 * Past the last anchor of the pass the answer is 0, the first frame of the
 * next pass. With no anchors the answer is the end of the pass, the one
 * frame every animation is sure to reach.
 *
 * @param frame  The frame on the glass, 0 .. frames - 1.
 * @param anchor Frames between two anchors; 0 for an animation that has none.
 * @param frames Frames in one pass.
 * @return The anchor, 0 .. frames - 1; frames - 1 when @p anchor is 0; 0 when
 *         @p frames is 0.
 */
uint16_t kui_kody_next_anchor(uint16_t frame, uint16_t anchor, uint16_t frames);

/**
 * @brief Whether one step of the playhead passed a frame or landed on it.
 *
 * A step that ends below where it started went through the end of the pass,
 * so it crossed everything after @p prev and everything up to @p now.
 *
 * @param prev   The frame before the step.
 * @param now    The frame after it.
 * @param target The frame asked about.
 * @param frames Frames in one pass.
 * @return true when @p target lies in (prev, now], read across the end of
 *         the pass when @p now is below @p prev; false when the playhead did
 *         not move, or when @p target is not a frame of the pass.
 */
bool kui_kody_crossed(uint16_t prev, uint16_t now, uint16_t target, uint16_t frames);

#ifdef __cplusplus
}
#endif

#endif /* KUI_KODY_ANCHOR_H */
