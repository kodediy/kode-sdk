/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_gpio.h
 * @brief An input pin that can interrupt, an output pin, and the way back to high-Z.
 */
#ifndef KHAL_GPIO_H_
#define KHAL_GPIO_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the P4 pulls an input to, if anything. */
typedef enum {
    KHAL_GPIO_PULL_NONE = 0,  /**< Leave the net to what the board put on it. */
    KHAL_GPIO_PULL_UP,        /**< The P4's internal pull-up.                 */
    KHAL_GPIO_PULL_DOWN,      /**< The P4's internal pull-down.               */
} khal_gpio_pull_t;

/** @brief What the interrupt reacts to. */
typedef enum {
    KHAL_GPIO_IRQ_LOW_LEVEL = 0,  /**< While low: a request line held until served. */
    KHAL_GPIO_IRQ_ANY_EDGE,       /**< On every change: a contact.               */
    KHAL_GPIO_IRQ_HIGH_LEVEL,     /**< While high: a request line held until served. */
} khal_gpio_irq_t;

/** @brief The interrupt callback. Runs in interrupt context. */
typedef void (*khal_gpio_cb_t)(void *arg);

/**
 * @brief Make @p pin an input.
 * @param pin  The P4 GPIO number, from kbsp.
 * @param pull KHAL_GPIO_PULL_NONE, _UP or _DOWN.
 * @return KODE_DENIED when @p pin is not one the P4 has, KODE_ERROR when the
 *         driver refused the configuration, KODE_OK otherwise.
 */
kode_status_t khal_gpio_input(uint8_t pin, khal_gpio_pull_t pull);

/**
 * @brief Make @p pin a push-pull output, driven to @p level at once and readable.
 * @param pin   The P4 GPIO number, from kbsp.
 * @param level true is high.
 * @return KODE_DENIED when @p pin is not one the P4 has, KODE_ERROR when the
 *         driver refused the configuration, KODE_OK otherwise.
 */
kode_status_t khal_gpio_output(uint8_t pin, bool level);

/**
 * @brief Drive an output pin. Nothing happens on a pin that is not one.
 * @param pin   The P4 GPIO number, from kbsp.
 * @param level true is high.
 * @return KODE_OK, or KODE_DENIED when the pin number is not one the P4 has.
 */
kode_status_t khal_gpio_write(uint8_t pin, bool level);

/**
 * @brief Detach any interrupt and put @p pin back to high impedance.
 *
 * The state a header position must be in when nobody holds it: no pull, no
 * drive, no handler. Safe on a pin that was never configured.
 *
 * @param pin The P4 GPIO number, from kbsp.
 */
void khal_gpio_release(uint8_t pin);

/**
 * @brief The pin's level right now.
 * @param pin The P4 GPIO number, from kbsp.
 * @return true when high.
 */
bool khal_gpio_level(uint8_t pin);

/**
 * @brief Whether a pin is an output driven low already, whoever drove it.
 *
 * A software reset leaves the GPIO block as it was, so this is how a boot tells
 * that the last firmware has been driving a supply's enable all along.
 *
 * @param pin The pin.
 * @return true when the pin's output is enabled and its level is low.
 */
bool khal_gpio_driven_low(uint8_t pin);

/**
 * @brief Install ESP-IDF's GPIO interrupt service, once for all of khal.
 *
 * One installer: a second gpio_install_isr_service() is refused, and ESP-IDF
 * logs that refusal as an error although nothing failed.
 *
 * @return KODE_OK once it is installed, by this call or an earlier one;
 *         KODE_ERROR when it cannot be.
 */
kode_status_t khal_gpio_service(void);

/**
 * @brief Attach @p cb to @p pin's interrupt and enable it.
 *
 * The pin must already be an input. Every trigger disables the interrupt and
 * calls @p cb once, in the ISR, which may only post a static event with
 * kqp_post_from_isr(); khal_gpio_irq_enable() arms it again.
 *
 * @param pin  The P4 GPIO number, from kbsp.
 * @param kind What triggers it.
 * @param cb   The callback.
 * @param arg  Passed to @p cb.
 * @return KODE_OK; KODE_DENIED for a NULL callback or a pin the P4 lacks;
 *         KODE_ERROR when the interrupt could not be installed.
 */
kode_status_t khal_gpio_irq_attach(uint8_t pin, khal_gpio_irq_t kind,
                                   khal_gpio_cb_t cb, void *arg);

/**
 * @brief Arm @p pin's interrupt again, once the trigger has been served.
 * @param pin The P4 GPIO number, from kbsp.
 */
void khal_gpio_irq_enable(uint8_t pin);

/**
 * @brief Let @p pin's level wake the P4 from light sleep, or stop it doing so.
 *
 * The pin keeps the interrupt khal_gpio_irq_attach() gave it; this adds the
 * wake on top of it, at the level that interrupt is armed for. While it is on,
 * a level that stays asserted wakes the chip again and again, so the owner
 * turns it off from the interrupt's callback and on again once the cause was
 * served. Turning it off puts the pin's interrupt type back as attach left it
 * (the driver clears the type when a wake is withdrawn). Safe to call from
 * the interrupt's callback.
 *
 * @param pin  The P4 GPIO number, from kbsp; an interrupt must be attached.
 * @param on   true to let the pin wake the chip, false to withdraw it.
 * @return KODE_OK; KODE_DENIED when @p pin is not one the P4 has or has no
 *         interrupt attached; KODE_ERROR when the driver refused.
 */
kode_status_t khal_gpio_wake(uint8_t pin, bool on);

/**
 * @brief Arm a pin attached on both edges as a light-sleep wake, on its low level.
 *
 * A wake takes a level and the owner's type may be an edge. khal_gpio_wake()
 * with false takes it away and puts the owner's type back.
 *
 * @param pin The pin, attached.
 * @return KODE_OK when armed.
 */
kode_status_t khal_gpio_wake_low(uint8_t pin);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_GPIO_H_ */
