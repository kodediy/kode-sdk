/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_kody.h
 * @brief Kody, the Dot's character: a Lottie player with anchors, chaining and rest.
 */
#ifndef KUI_KODY_H
#define KUI_KODY_H

#include <stdbool.h>
#include <stdint.h>

#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One animation: the Lottie and what the player needs to know of it.
 *
 * The Lottie is its JSON, minified and deflated into one zlib stream (RFC
 * 1950: what Python's `zlib.compress()` writes). tools/kodygen.py makes the
 * kit's own descriptors; one made any other way follows the same form.
 */
typedef struct {
    const uint8_t *zlib;     /**< The Lottie JSON, minified, as one zlib stream. */
    uint32_t       zlib_len; /**< Bytes in zlib. */
    uint32_t       json_len; /**< Bytes in the JSON the stream inflates to. */
    uint16_t       w;        /**< Canvas width, px. */
    uint16_t       h;        /**< Canvas height, px. */
    uint16_t       frames;   /**< Frames in one pass (op - ip). */
    uint8_t        fps;      /**< Design rate (30). */
    uint8_t        anchor;   /**< Frames between anchor poses; 0 = none. */
    bool           deflated; /**< true: zlib holds a zlib stream; false: it holds the JSON itself, json_len bytes. */
} kui_kody_anim_t;

/**
 * @brief Create a player for animations of one canvas size.
 *
 * Nothing is drawn and no buffer is taken until the first play.
 *
 * @param parent The parent object.
 * @param w      Canvas width, px; not 0.
 * @param h      Canvas height, px; not 0.
 * @return The player, named "kody_#", or NULL when a size is 0 or its state
 *         could not be allocated (logged).
 */
lv_obj_t *kui_kody_create(lv_obj_t *parent, uint16_t w, uint16_t h);

/**
 * @brief Play an animation from frame 0, in place of whatever plays.
 *
 * Whatever was queued, pending or stopped is dropped. The JSON is inflated
 * and parsed, and the first frame rasterised, before this returns: that is
 * the cost of a play, paid in the caller's step.
 *
 * @param k    The player.
 * @param a    The animation; drawn for the player's canvas.
 * @param loop true to loop; false to play one pass and hold the last frame.
 * @return true when it plays; false when @p k or @p a is NULL, the sizes
 *         differ, the buffer, the JSON's block or the widget could not be
 *         allocated, or the data does not inflate (logged). After a failed
 *         allocation or inflate the player is empty.
 */
bool kui_kody_play(lv_obj_t *k, const kui_kody_anim_t *a, bool loop);

/**
 * @brief Play an animation when the current pass ends.
 *
 * The hand-over is on the current pass's last frame: a one-shot hands over
 * instead of holding, a loop at the end of its pass. The next animation's
 * frame 0 takes that frame's place, so a pair made to link, whose last and
 * first frames are one pose, shows that pose once and goes on. With nothing
 * playing, or a one-shot already holding its end, it starts at once. A second
 * call replaces the first. A reaction comes first: while one waits or plays,
 * the loop comes back after it, and the hand-over is at the end of that
 * loop's pass.
 *
 * @param k    The player.
 * @param next The animation; drawn for the player's canvas, or it is refused
 *             with a log line and nothing is queued.
 * @param loop true for the next one to loop, false to play it once.
 */
void kui_kody_then(lv_obj_t *k, const kui_kody_anim_t *next, bool loop);

/**
 * @brief Play a reaction at the loop's next anchor, then pick the loop up there.
 *
 * The reaction is a one-shot that starts and ends on the anchor pose, and
 * each switch shows that pose once, as kui_kody_then() does. On an anchor
 * already, or stopped on one, it starts at once. A reaction also wakes
 * a stopped player: a freeze that was pending is dropped, a pause is lifted,
 * and after the reaction the loop plays on.
 *
 * @param k The player; it must be playing a loop.
 * @param r The reaction; drawn for the player's canvas.
 * @return true when it is playing or waiting for its anchor; false when @p k
 *         or @p r is NULL, the sizes differ (logged), nothing loops, a
 *         reaction is already waiting or playing, or, on an anchor, the
 *         reaction could not be allocated or inflated (logged), which leaves
 *         the player empty.
 */
bool kui_kody_react(lv_obj_t *k, const kui_kody_anim_t *r);

/**
 * @brief Stop on the next anchor, or now when there is none.
 *
 * A loop stops on its next anchor, at once when it is on one or has none; a
 * reaction that waits for that anchor plays first, and the loop stops where
 * it comes back. A loop with no anchors stops at once even then, and the
 * reaction that waits plays after kui_kody_resume(). A one-shot that hands
 * over to a loop (a reaction, or a then() that loops) plays to its end and
 * the loop stops on the frame it starts at; any other one-shot stops at
 * once. Nothing happens to a player that is still.
 *
 * @param k The player.
 */
void kui_kody_freeze(lv_obj_t *k);

/**
 * @brief Stop now, on the frame on the glass.
 *
 * What was queued, and a reaction that waits or plays, stay as they are:
 * kui_kody_resume() goes on with them. A freeze still on its way to its
 * anchor does not: kui_kody_resume() drops it.
 *
 * @param k The player.
 */
void kui_kody_pause(lv_obj_t *k);

/**
 * @brief Go on from where the player stopped.
 *
 * Lifts a pause or a freeze, and drops a freeze still on its way to its
 * anchor. A one-shot that holds its last frame stays as it is.
 *
 * @param k The player.
 */
void kui_kody_resume(lv_obj_t *k);

/**
 * @brief Let go of every animation the player was given.
 *
 * Drops what plays, what is queued, a reaction waiting for its anchor and
 * the loop a reaction returns to, and deletes the widget: nothing plays
 * until the next kui_kody_play(). After it the caller may free the data of
 * every animation the player was given - which is what it is for: a pack
 * retired while its animations are on a player.
 *
 * @param k The player; NULL does nothing.
 */
void kui_kody_stop(lv_obj_t *k);

/**
 * @brief Whether the player is still: frozen, paused, holding a one-shot's end, or empty.
 * @param k The player.
 * @return true when its clock is stopped or nothing plays, so nothing
 *         rasterises; true for NULL. A player that is only out of sight is
 *         not still: it goes on once it is seen.
 */
bool kui_kody_is_still(lv_obj_t *k);

/**
 * @brief Whether a reaction is under way: waiting for its anchor, or playing.
 *
 * From the kui_kody_react() that took it until the loop is picked up again
 * at its anchor, a pause in between included. What a caller asks before it
 * plays something else in the loop's place, when the reaction is to end
 * first: kui_kody_play() cuts a reaction short.
 *
 * @param k The player.
 * @return true while a reaction waits or plays; false otherwise, and for
 *         NULL.
 */
bool kui_kody_is_reacting(lv_obj_t *k);

/**
 * @brief Whether a reaction has begun to play: past its wait for the anchor.
 *
 * @param k The player.
 * @return true from the frame the reaction starts on until the loop comes
 *         back; false while it waits, and for NULL.
 */
bool kui_kody_reaction_plays(lv_obj_t *k);

#ifdef __cplusplus
}
#endif

#endif /* KUI_KODY_H */
