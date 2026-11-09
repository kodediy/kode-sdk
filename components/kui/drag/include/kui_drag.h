/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_drag.h
 * @brief A page that follows a finger along one axis, starts with resistance and lands on a spring.
 */
#ifndef KUI_DRAG_H
#define KUI_DRAG_H

#include <stdbool.h>
#include <stdint.h>

#include "kui_spring.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KUI_DRAG_SAMPLES 16U /**< Finger samples kept: the speed window at one sample per 10 ms, with room to spare. */

#define KUI_DRAG_RESIST_SHARE_PCT 35  /**< Percent of the finger's distance the page moves at the start of a drag. */
#define KUI_DRAG_RESIST_FROM_PX   48  /**< The share holds up to this distance from the grab, in pixels. */
#define KUI_DRAG_RESIST_TO_PX     160 /**< By this distance from the grab the page is back under the finger, in pixels. */

#define KUI_DRAG_FLING_PX_S 800 /**< A speed above this, toward an end, decides the release by itself, in pixels per second. */

#define KUI_DRAG_VEL_WINDOW_MS     80U /**< The finger's speed is measured over its last samples this old at most. */
#define KUI_DRAG_SPEED_MIN_SAMPLES 2U  /**< A speed needs this many samples inside the window: one to start from, one to compare. */

#define KUI_DRAG_SPRING_MS   250U /**< The period of the spring: one turn of its natural frequency. */
#define KUI_DRAG_LAND_MAX_MS 320U /**< The spring lands on its end no later than this after it started. */

#define KUI_DRAG_REST_PX          0.5F  /**< The spring is at rest when it is this close to its end, in pixels... */
#define KUI_DRAG_REST_SPEED_PX_MS 0.05F /**< ...and moves slower than this, in pixels per millisecond. */

/** Where the page stands. */
typedef enum {
    KUI_DRAG_IDLE = 0, /**< At rest at one end. */
    KUI_DRAG_HELD,     /**< Caught in flight: frozen, not claimed by a finger yet. */
    KUI_DRAG_FOLLOW,   /**< Following a finger. */
    KUI_DRAG_SETTLE    /**< The spring is landing it. */
} kui_drag_phase_t;

/** One finger sample. */
typedef struct {
    int32_t pos;    /**< The finger's position on the axis, signed toward open. */
    uint32_t t_ms;  /**< The clock when it was sampled. */
} kui_drag_sample_t;

/** One page, from closed to open along an axis. Read it, never write it. */
typedef struct {
    int32_t span;             /**< Pixels from closed (0) to open. */
    kui_drag_phase_t phase;   /**< Where the page stands. */
    bool resist;              /**< The drag in progress starts with resistance. */
    bool target_open;         /**< The end the page is at or going to. */
    int32_t grab_pos;         /**< The finger at the grab. */
    float grab_off;           /**< The page at the grab. */
    float off;                /**< The page now, from 0 to span. */
    kui_spring_t spring;      /**< The spring that lands the page, aimed at the end it is going to. */
    kui_drag_sample_t samples[KUI_DRAG_SAMPLES]; /**< The ring of finger samples, oldest overwritten first. */
    uint8_t n_samples;        /**< How many samples the ring holds. */
    uint8_t head;             /**< The slot the next sample goes to. */
} kui_drag_t;

/**
 * @brief Makes a page closed, at rest, with no samples.
 *
 * @param d       The page; NULL is ignored.
 * @param span_px Pixels from closed to open; a negative span is stored as 0.
 */
void kui_drag_init(kui_drag_t *d, int32_t span_px);

/**
 * @brief Puts the page at rest at one end, dropping whatever it was doing.
 *
 * @param d    The page; NULL is ignored.
 * @param open true for the open end, false for the closed one.
 */
void kui_drag_set(kui_drag_t *d, bool open);

/**
 * @brief A finger lands on the page and starts to follow it.
 *
 * The samples start over with this one. The drag has resistance only when the
 * page was at rest (IDLE): a page caught in flight, HELD or still settling,
 * follows the finger one to one. A page that is still settling is caught
 * where the spring has it at t_ms first.
 *
 * @param d    The page; NULL is ignored.
 * @param pos  The finger's position on the axis.
 * @param t_ms The samples' clock: the same for every call, and a stamp ahead
 *             of it counts as now.
 */
void kui_drag_grab(kui_drag_t *d, int32_t pos, uint32_t t_ms);

/**
 * @brief Freezes a settling page where the spring has it at t_ms.
 *
 * The phase becomes HELD, and stays so until the catch is answered. Ignored
 * in any other phase.
 * A catch is answered by kui_drag_release(), _grab(), _finish(), _aim() or _set(): a HELD
 * page is not stepped, so an unanswered catch leaves it frozen.
 *
 * @param d    The page; NULL is ignored.
 * @param t_ms The samples' clock: the same for every call, and a stamp ahead
 *             of it counts as now.
 */
void kui_drag_catch(kui_drag_t *d, uint32_t t_ms);

/**
 * @brief Feeds the finger's position while it follows.
 *
 * The offset becomes the grab's plus the finger's distance from the grab,
 * with resistance at the start, kept between 0 and the span. Ignored unless
 * the phase is FOLLOW.
 *
 * @param d    The page; NULL is ignored.
 * @param pos  The finger's position on the axis.
 * @param t_ms The samples' clock: the same for every call, and a stamp ahead
 *             of it counts as now. It stamps this sample.
 */
void kui_drag_follow(kui_drag_t *d, int32_t pos, uint32_t t_ms);

/**
 * @brief Lifts the finger and starts the spring.
 *
 * From FOLLOW the page opens or goes back by the rule below, and the spring
 * starts at the finger's speed. From HELD it goes on to the end it was going
 * to, from rest. Either way the phase becomes
 * SETTLE. In any other phase nothing changes. It takes no position: the point
 * where the finger lifted is fed through kui_drag_follow() before it.
 * The rule: a fling faster than KUI_DRAG_FLING_PX_S goes its way; otherwise the page opens
 * from halfway.
 *
 * @param d           The page; NULL answers false.
 * @param t_ms        The samples' clock: the same for every call, and a stamp
 *                    ahead of it counts as now.
 * @param by_position true to decide by the page's position alone, as if the
 *                    finger had been still.
 * @return true when the page is going to the open end.
 */
bool kui_drag_release(kui_drag_t *d, uint32_t t_ms, bool by_position);

/**
 * @brief Sends the page to an end by the spring, from where it is.
 *
 * A settling page keeps the speed the spring has at t_ms; any other starts
 * from rest. The phase becomes SETTLE.
 *
 * @param d    The page; NULL is ignored.
 * @param open true for the open end, false for the closed one.
 * @param t_ms The samples' clock: the same for every call, and a stamp ahead
 *             of it counts as now.
 */
void kui_drag_aim(kui_drag_t *d, bool open, uint32_t t_ms);

/**
 * @brief Moves a settling page to where the spring has it at t_ms.
 *
 * The page lands, exactly on its end and IDLE, when the spring has run
 * KUI_DRAG_LAND_MAX_MS, or is at rest within KUI_DRAG_REST_PX and
 * KUI_DRAG_REST_SPEED_PX_MS, or has left the span (it only can past its end). A
 * time before the spring's start counts as its start. Ignored unless the
 * phase is SETTLE.
 *
 * @param d    The page; NULL answers false.
 * @param t_ms The samples' clock: the same for every call, and a stamp ahead
 *             of it counts as now.
 * @return true when this step landed the page.
 */
bool kui_drag_step(kui_drag_t *d, uint32_t t_ms);

/**
 * @brief Lands a settling or held page at once on the end it is going to.
 *
 * Ignored in any other phase.
 *
 * @param d The page; NULL is ignored.
 */
void kui_drag_finish(kui_drag_t *d);

/**
 * @brief The page's offset, rounded to the nearest pixel.
 *
 * @param d The page; NULL answers 0.
 * @return Pixels from closed, 0 to the span.
 */
int32_t kui_drag_offset(const kui_drag_t *d);

/**
 * @brief The finger's speed over its last KUI_DRAG_VEL_WINDOW_MS, signed toward open.
 *
 * The samples inside the window give the newest position less the oldest,
 * over the time between them; a sample stamped ahead of t_ms is inside it.
 * Fewer than two samples inside the window, or no time between them, is 0.
 *
 * @param d    The page; NULL answers 0.
 * @param t_ms The samples' clock: the same for every call, and a stamp ahead
 *             of it counts as now.
 * @return Pixels per second, rounded.
 */
int32_t kui_drag_speed_px_s(const kui_drag_t *d, uint32_t t_ms);

/**
 * @brief What a release at t_ms would decide.
 *
 * While a finger follows, the answer of kui_drag_release() without doing it.
 * In any other phase it is the end the page is at or going to.
 *
 * @param d           The page; NULL answers false.
 * @param t_ms        The would-be release's time on the samples' clock: the
 *                    same for every call, and a stamp ahead of it counts as
 *                    now.
 * @param by_position As for kui_drag_release().
 * @return true when the page would go to the open end.
 */
bool kui_drag_would_open(const kui_drag_t *d, uint32_t t_ms, bool by_position);

/**
 * @brief Where the page stands.
 *
 * @param d The page; NULL answers IDLE.
 * @return The phase.
 */
kui_drag_phase_t kui_drag_phase(const kui_drag_t *d);

/**
 * @brief Whether the page is at rest at the open end.
 *
 * @param d The page; NULL answers false.
 * @return true only in IDLE, and only at the open end.
 */
bool kui_drag_is_open(const kui_drag_t *d);

#ifdef __cplusplus
}
#endif

#endif /* KUI_DRAG_H */
