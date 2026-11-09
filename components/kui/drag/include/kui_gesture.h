/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kui_gesture.h
 * @brief Turns a finger's first pixels into a tap, a claimed drag or a named rejection.
 */
#ifndef KUI_GESTURE_H
#define KUI_GESTURE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define KUI_GESTURE_DIR_UP    0x01U /**< A drag toward y 0 (the finger moves up). */
#define KUI_GESTURE_DIR_DOWN  0x02U /**< A drag away from y 0 (the finger moves down). */
#define KUI_GESTURE_DIR_LEFT  0x04U /**< A drag toward x 0 (the finger moves left). */
#define KUI_GESTURE_DIR_RIGHT 0x08U /**< A drag away from x 0 (the finger moves right). */
#define KUI_GESTURE_DIR_VERT  (KUI_GESTURE_DIR_UP | KUI_GESTURE_DIR_DOWN)    /**< Up and down. */
#define KUI_GESTURE_DIR_HORZ  (KUI_GESTURE_DIR_LEFT | KUI_GESTURE_DIR_RIGHT) /**< Left and right. */

#define KUI_GESTURE_CLAIM_PX 12 /**< Pixels from the landing, on the farther axis, at which a drag is decided. */
#define KUI_GESTURE_TAP_PX   16 /**< A press that never went farther than this from the landing, on either axis, is a tap. */
#define KUI_GESTURE_AXIS_NUM 3  /**< Numerator of the axis ratio: the farther axis needs at least NUM/DEN times the other. */
#define KUI_GESTURE_AXIS_DEN 2  /**< Denominator of the axis ratio (3/2 is 1.5:1). */

/** Where a gesture stands. */
typedef enum {
    KUI_GESTURE_IDLE = 0, /**< No press yet. Also the value of "no rejection noted". */
    KUI_GESTURE_PENDING,  /**< Finger down, undecided. */
    KUI_GESTURE_CLAIMED,  /**< A drag along dir, from (cx, cy). */
    KUI_GESTURE_TAP,      /**< Released, never claimed, within KUI_GESTURE_TAP_PX of the landing. */
    KUI_GESTURE_DIAGONAL, /**< Rejected: the axes were under 1.5:1. */
    KUI_GESTURE_AXIS,     /**< Rejected: a direction its zone does not take. */
    KUI_GESTURE_ZONE,     /**< Rejected: landed where no drag is born. */
    KUI_GESTURE_BLOCKED   /**< Rejected: landed while input was shut. */
} kui_gesture_state_t;

/** One press, from the landing to the release. Read it, never write it. */
typedef struct {
    int32_t x0;                 /**< Column where the finger landed. */
    int32_t y0;                 /**< Row where the finger landed. */
    int32_t cx;                 /**< Column of the sample that claimed the drag; meaningful once CLAIMED. */
    int32_t cy;                 /**< Row of the sample that claimed the drag; meaningful once CLAIMED. */
    int32_t travel;             /**< The farthest the finger has been from the landing, on either axis, in pixels. */
    uint8_t allowed;            /**< The KUI_GESTURE_DIR_* bits the press's zone takes; 0 when no drag is born there. */
    uint8_t dir;                /**< The claimed direction, one KUI_GESTURE_DIR_* bit; 0 until CLAIMED. */
    kui_gesture_state_t reason; /**< A rejection noted and not yet final (DIAGONAL, AXIS or ZONE), else IDLE. */
    kui_gesture_state_t state;  /**< Where the gesture stands. */
} kui_gesture_t;

/**
 * @brief Starts a gesture at a landing, discarding whatever the structure held.
 *
 * The state is PENDING, or BLOCKED at once when input was shut: a blocked
 * press never claims, whatever the finger does.
 *
 * @param g       The gesture; NULL is ignored.
 * @param x       Column where the finger landed.
 * @param y       Row where the finger landed.
 * @param allowed The KUI_GESTURE_DIR_* bits the landing zone takes, 0 for a zone where no drag is born.
 * @param blocked true when input is shut and this press must not act.
 */
void kui_gesture_press(kui_gesture_t *g, int32_t x, int32_t y, uint8_t allowed, bool blocked);

/**
 * @brief Feeds the finger's position while it is down.
 *
 * Decides once, at the first sample KUI_GESTURE_CLAIM_PX or more from the
 * landing on its farther axis, while the gesture is PENDING. A claim holds
 * its axis whatever the finger does afterwards; a rejection and BLOCKED
 * never change. Only an IDLE gesture and a TAP ignore a sample; a settled
 * one, CLAIMED, rejected or BLOCKED, still updates `travel` and nothing else.
 *
 * @param g The gesture; NULL answers IDLE.
 * @param x Column of the finger now.
 * @param y Row of the finger now.
 * @return The state after the sample.
 */
kui_gesture_state_t kui_gesture_move(kui_gesture_t *g, int32_t x, int32_t y);

/**
 * @brief Lifts the finger.
 *
 * The point where it lifted is measured first, exactly as a move would: it
 * can complete a claim or put a rejection past the tap radius, so a release
 * far from the last move (a lost sample, an injected jump) is never a tap.
 * A gesture still PENDING after that, with or without a rejection noted,
 * becomes TAP. Every other state stays.
 *
 * @param g The gesture; NULL answers IDLE.
 * @param x Column where the finger lifted.
 * @param y Row where the finger lifted.
 * @return The final state.
 */
kui_gesture_state_t kui_gesture_release(kui_gesture_t *g, int32_t x, int32_t y);

/**
 * @brief The name of a state, for logs and the console.
 *
 * @param s The state.
 * @return "idle", "pending", "claim", "tap", "diagonal", "axis", "zone" or "blocked"; "?" for any other value.
 */
const char *kui_gesture_name(kui_gesture_state_t s);

#ifdef __cplusplus
}
#endif

#endif /* KUI_GESTURE_H */
