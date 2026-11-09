/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_pin_lock.h
 * @brief Not part of the API: the lock over J3's books, and the pins something listens to.
 */
#ifndef KODE_PIN_LOCK_H_
#define KODE_PIN_LOCK_H_

#ifndef KODE_INTERNAL_ALLOWED
#error "kode_pin_lock.h is private to kode: an app uses kode.h"
#endif

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The pins on the connector, KODE_PIN_FIRST to KODE_PIN_LAST. */
#define KODE_PIN_COUNT ((KODE_PIN_LAST - KODE_PIN_FIRST) + 1U)

/**
 * @brief The prologue of every pin and bus call: kode_enter(), then the pins
 *        lock, made on the first call; blocks while another task holds it.
 *
 * Lock order: a bus's locks first, this one last; never a bus lock while holding this one.
 * @param name The public name of the call: the name a refusal carries.
 * @return true when the caller holds the lock; false with the error set when
 *         kode_enter() refused or there is no memory to make the lock.
 */
bool kode_pin_lock_take(const char *name);

/** @brief Give the pins lock back, at the end of a call kode_pin_lock_take() let in. */
void kode_pin_lock_give(void);

/**
 * @brief Ready @p n pins for a bus: judge every one, then release the ones the app drove.
 *
 * Each pin must be on the connector, named once, on a header the app holds,
 * and not lent; all are judged before any is touched. Then every pin the app
 * made an input, an output, an analog input or a PWM output with kode_pin_*
 * goes back to free and high-Z, ready to be lent. Call it with the lock held.
 * @param pins The pins, as the caller typed them.
 * @param n    How many.
 * @return true when every pin is free; false with the error set, naming the
 *         first pin that is not, and no pin touched.
 */
bool kode_pin_ready_for_bus(const uint8_t *pins, size_t n);

/**
 * @brief What hands a listened pin's interrupt to its Active Object:
 *        kode_standard_ao_pin_arm().
 *
 * Called with the pins lock held, once the pin is an input: it attaches the
 * pin's interrupt, enables it and tells the Active Object the level the pin
 * starts at.
 * @param pin The pin, KODE_PIN_FIRST to KODE_PIN_LAST.
 * @return true when the pin is armed; false with the error set.
 */
typedef bool (*kode_pin_arm_fn_t)(uint8_t pin);

/**
 * @brief Count one more listener of @p pin: a listener of pin_<n>_changed
 *        about to be registered, or a wait about to begin.
 *
 * Takes the pins lock. The first one judges the pin as a bus's begin does
 * (on the connector, on a header the app holds, not lent), makes it an input
 * with no pull and has @p arm attach its interrupt; the others only count.
 * Each true answer is matched by one kode_pin_unlisten() once that listener
 * or wait is over. While the count is above zero, kode_pin_write(),
 * kode_pin_write_pwm(), kode_pin_read_analog() and kode_pin_ready_for_bus() refuse
 * the pin with "pin N is listened to".
 * @param pin The pin, KODE_PIN_FIRST to KODE_PIN_LAST.
 * @param arm kode_standard_ao_pin_arm(); never NULL.
 * @return true when the pin is listened to; false with the error set ("pin 5
 *         is lent to i2c", "pin 5 could not be made an input", what @p arm
 *         said), the count as it was and the pin let go when it was the first.
 */
bool kode_pin_listen(uint8_t pin, kode_pin_arm_fn_t arm);

/**
 * @brief Count one listener of @p pin less; the last one lets the pin go.
 *
 * Takes the pins lock. When the count reaches zero the pin is no longer
 * listened to and goes back to free and high-Z, its interrupt detached.
 * Does nothing for a pin with no listener.
 * @param pin The pin, KODE_PIN_FIRST to KODE_PIN_LAST.
 */
void kode_pin_unlisten(uint8_t pin);

/**
 * @brief Whether anything listens to @p pin; callable from any thread.
 *
 * Read without the pins lock, so the answer may be a moment old: the Active
 * Object asks it before it arms a settle or re-arms the interrupt, and a pin
 * let go meanwhile has its interrupt detached already.
 * @param pin The pin.
 * @return true while a listener or a wait holds it; false otherwise and for a
 *         pin off the connector.
 */
bool kode_pin_is_listened(uint8_t pin);

#ifdef __cplusplus
}
#endif

#endif /* KODE_PIN_LOCK_H_ */
