/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_spring.h
 * @brief A damped spring in closed form: where a number is and how fast it moves, at any time.
 */
#ifndef KUI_SPRING_H
#define KUI_SPRING_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KUI_SPRING_CRITICAL 1.0F /**< The damping ratio of a critically damped spring, and the most a spring is given. */

/** One spring, from where it started to its target. Read it, never write it. */
typedef struct {
    float target;  /**< Where the spring is going, in pixels. */
    float x0;      /**< The start: the position less the target, in pixels. */
    float v0;      /**< The speed at the start, in pixels per millisecond. */
    float w;       /**< The natural frequency, in radians per millisecond. */
    uint32_t t0;   /**< The clock when it started. */
    float zeta;    /**< The damping ratio, from 0 to KUI_SPRING_CRITICAL. */
} kui_spring_t;

/**
 * @brief How long ago a stamp was taken, on a clock that wraps.
 *
 * The one rule for the age of a stamp on the free-running millisecond counter.
 * The difference is unsigned, so it survives the wrap of the 32 bits. One with
 * its top bit set is a stamp ahead of now_ms, taken on a clock that has not
 * got there yet, and counts as taken now.
 *
 * @param stamp_ms A time on the same clock as now_ms.
 * @param now_ms   The clock now.
 * @return Milliseconds since the stamp, 0 for a stamp ahead of now_ms.
 */
uint32_t kui_age_ms(uint32_t stamp_ms, uint32_t now_ms);

/**
 * @brief Starts a spring at a position and a speed, going to a target.
 *
 * A zeta below 0, or not a number, is 0, and one above KUI_SPRING_CRITICAL is
 * KUI_SPRING_CRITICAL. A period of 0 is a spring with no time to land: it is
 * at its target from the start, and has no speed.
 *
 * @param s             The spring; NULL is ignored.
 * @param pos           Where it starts, in pixels.
 * @param vel_px_per_ms The speed it starts with, in pixels per millisecond.
 * @param target        Where it is going, in pixels.
 * @param period_ms     The period of its natural frequency, in milliseconds.
 * @param zeta          The damping ratio: KUI_SPRING_CRITICAL, or less for a spring that crosses its target.
 * @param t_ms          The clock now.
 */
void kui_spring_start(kui_spring_t *s, float pos, float vel_px_per_ms, float target, uint32_t period_ms, float zeta,
                      uint32_t t_ms);

/**
 * @brief Where the spring is at t_ms, and how fast it moves.
 *
 * @param s             The spring; NULL gives 0 and 0.
 * @param t_ms          The clock now; a time before the spring's start counts as its start.
 * @param pos           Receives the position in pixels; NULL is not written.
 * @param vel_px_per_ms Receives the speed in pixels per millisecond; NULL is not written.
 */
void kui_spring_at(const kui_spring_t *s, uint32_t t_ms, float *pos, float *vel_px_per_ms);

/**
 * @brief Whether the spring is at rest at t_ms: close to its target and moving slowly.
 *
 * Both limits are inclusive, and both have to hold: a spring crossing its
 * target is close to it, but fast.
 *
 * @param s                 The spring; NULL answers false.
 * @param t_ms              The clock now.
 * @param pos_eps_px        The farthest from the target it may be, in pixels.
 * @param vel_eps_px_per_ms The fastest it may move, in pixels per millisecond.
 * @return true when it is within both limits.
 */
bool kui_spring_rested(const kui_spring_t *s, uint32_t t_ms, float pos_eps_px, float vel_eps_px_per_ms);

/**
 * @brief Sends the spring to another target from where it is at t_ms.
 *
 * The position and the speed the spring has at t_ms are kept, to float
 * precision (the position is held as its distance from the new target), and it
 * goes on from them to the new target, with the period and the damping it had.
 * A spring with no period (see kui_spring_start()) has no position to keep: it
 * is at the new target at once.
 *
 * @param s      The spring; NULL is ignored.
 * @param target Where it is going now, in pixels.
 * @param t_ms   The clock now.
 */
void kui_spring_retarget(kui_spring_t *s, float target, uint32_t t_ms);

#ifdef __cplusplus
}
#endif

#endif /* KUI_SPRING_H */
