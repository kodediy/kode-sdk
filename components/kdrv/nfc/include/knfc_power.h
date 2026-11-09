/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_power.h
 * @brief The reader's rest/wake decision, as a state and a table.
 */
#ifndef KNFC_POWER_H_
#define KNFC_POWER_H_

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The reader's coarse power state. */
typedef enum {
    KNFC_POWER_RESTING = 0, /**< Oscillator off. A register read still works. */
    KNFC_POWER_AWAKE,       /**< Oscillator running; ready for the field.     */
} knfc_power_state_t;

/** @brief What a transition asks of the chip. */
typedef enum {
    KNFC_POWER_HOLD = 0, /**< Already there: touch nothing.                   */
    KNFC_POWER_WAKE,     /**< Leave rest: rfalLowPowerModeStop().             */
    KNFC_POWER_REST,     /**< Enter rest: rfalLowPowerModeStart(RFAL_LP_MODE_PD). */
} knfc_power_action_t;

/**
 * @brief The chip action that brings @p from to @p want. Pure: mutates nothing.
 *
 * The caller applies the action to the reader and records @p want as the new
 * state only if the chip agreed, so a wake or a rest that failed leaves the
 * believed state where it was and the next call decides afresh. A HOLD is
 * returned whenever the two already agree.
 *
 * @param from The state the reader is believed to be in.
 * @param want The state a use (awake) or the end of one (resting) needs.
 * @return KNFC_POWER_WAKE, KNFC_POWER_REST, or KNFC_POWER_HOLD.
 */
knfc_power_action_t knfc_power_action(knfc_power_state_t from,
                                      knfc_power_state_t want);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_POWER_H_ */
