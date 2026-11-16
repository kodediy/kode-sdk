/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file lowpower_part.h
 * @brief The rules of an app's rest: which parts may rest together, and in what order.
 *
 * Plain arithmetic on the KODE_LP_PART_* masks, with no event and no device:
 * AO_LowPower asks here what a request may have and which part comes next,
 * and does the asking of the owners itself.
 */
#ifndef LOWPOWER_PART_H_
#define LOWPOWER_PART_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOWPOWER_PART_WHY_CABLE "cable: external power"  /**< Why the CPU part is refused while a USB cable is in. */

/** @brief What a request is judged against. */
typedef struct {
    uint8_t at_rest;     /**< The KODE_LP_PART_* held at rest already.         */
    bool    glass;       /**< The build has a display owner.                   */
    bool    cable;       /**< A USB cable is in.                               */
    bool    wake_touch;  /**< The request wants a touch to end the CPU part.   */
    bool    wake_pin;    /**< The request names a line of the header.          */
    bool    pin_known;   /**< That line is a position the header has.          */
    bool    pin_free;    /**< Nothing drives that position and nobody has it on
                          *   loan: it is free, or a plain input.              */
} lowpower_part_ask_t;

/**
 * @brief Judge a request, and complete its mask.
 *
 * KODE_LP_PART_PANEL brings KODE_LP_PART_SCREEN and KODE_LP_PART_TOUCH with
 * it: the reset line that goes low before the supply is the touch
 * controller's too. Nothing else is added. The radio wants the sound in the
 * same request, whatever rested before: the sound comes back by itself, so
 * only its owner can say it rests now.
 *
 * @param parts The mask asked for; on a request that stands, the mask to rest.
 * @param ask   What it is judged against.
 * @return NULL when the request stands; otherwise why not, "part: why", a
 *         string that outlives the call.
 */
const char *lowpower_part_check(uint8_t *parts, const lowpower_part_ask_t *ask);

/**
 * @brief The next part to put away.
 *
 * The order is fixed: sound, card, touch, screen, panel, radio, CPU. The sound
 * goes before the radio, whose link switches its amplifier off; the screen
 * before the panel's supply; the CPU last, once nothing else is left to ask.
 *
 * @param want The mask to rest.
 * @param done The parts held at rest already, and the ones this request has
 *             put away.
 * @return One KODE_LP_PART_* bit; 0 when every part wanted is done.
 */
uint8_t lowpower_part_next_down(uint8_t want, uint8_t done);

/**
 * @brief Whether a part stays at rest once it is put away.
 *
 * The sound and the card do not: they are put away once and come back by
 * themselves at their next use, so nobody can say they still rest. They are
 * never among the parts held, and asking for them again asks their owner
 * again.
 *
 * @param part One KODE_LP_PART_* bit.
 * @return false for the sound and the card.
 */
bool lowpower_part_stays(uint8_t part);

/**
 * @brief The next part to bring back.
 *
 * The CPU first, then the radio, the glass and the touch.
 *
 * @param at_rest The parts held at rest.
 * @return One KODE_LP_PART_* bit; 0 when none is left.
 */
uint8_t lowpower_part_next_up(uint8_t at_rest);

/**
 * @brief A part's name, for the log and a refusal.
 * @param part One KODE_LP_PART_* bit.
 * @return The name; "part" for a value that is no single part.
 */
const char *lowpower_part_name(uint8_t part);

/**
 * @brief The name its owner joined the rounds under.
 * @param part One KODE_LP_PART_* bit.
 * @return The participant's name; "" for a value that is no single part.
 */
const char *lowpower_part_owner(uint8_t part);

#ifdef __cplusplus
}
#endif

#endif /* LOWPOWER_PART_H_ */
