/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_exp.h
 * @brief The expansion header by position (1..14): each one's mode, and who holds it.
 */
#ifndef KHAL_EXP_H_
#define KHAL_EXP_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbsp_exp.h"
#include "khal_gpio.h"
#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KHAL_EXP_POS_MIN        1U       /**< First position, as printed on the connector. */
#define KHAL_EXP_POS_MAX        14U      /**< Last position, as printed on the connector. */
#define KHAL_EXP_PWM_HZ_MIN     1U       /**< Lowest PWM frequency, Hz. */
#define KHAL_EXP_PWM_HZ_MAX     40000U   /**< Highest PWM frequency, Hz. */
#define KHAL_EXP_PWM_DUTY_MAX   1000U    /**< Tenths of a percent: 100.0 %. */

/** @brief What a position is doing right now. */
typedef enum {
    KHAL_EXP_FREE = 0,  /**< High-Z, nobody's.                          */
    KHAL_EXP_IN,        /**< Digital input, with or without a pull.     */
    KHAL_EXP_OUT,       /**< Digital output.                            */
    KHAL_EXP_ADC,       /**< Analog input on ADC1.                      */
    KHAL_EXP_PWM,       /**< LEDC output.                               */
    KHAL_EXP_LENT,      /**< Handed to a peripheral driver, by tag.     */
} khal_exp_mode_t;

/** @brief Everything `exp`, `ps` and the host suite ask about a position. */
typedef struct {
    uint8_t         gpio;     /**< The P4 GPIO behind the position.      */
    bool            adc;      /**< It has an ADC1 channel.               */
    bool            lp;       /**< It is an LP pin.                      */
    khal_exp_mode_t mode;     /**< What it is doing.                     */
    const char     *tag;      /**< LENT: who holds it. NULL otherwise.   */
} khal_exp_info_t;

/**
 * @brief Put every position FREE and high-Z, closing its mode and voiding its loan.
 *
 * Idempotent but not harmless on a table in use: the owner calls it at its own start only.
 *
 * @return KODE_OK.
 */
kode_status_t khal_exp_init(void);

/**
 * @brief Make @p pos a digital input.
 * @param pos  The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @param pull The pull to apply.
 * @return KODE_OK; KODE_DENIED when out of range or lent; KODE_ERROR when the pin refused.
 */
kode_status_t khal_exp_input(uint8_t pos, khal_gpio_pull_t pull);

/**
 * @brief Make @p pos an output driven to @p level.
 * @param pos   The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @param level The level to drive.
 * @return KODE_OK; KODE_DENIED when out of range or lent; KODE_ERROR when the pin refused.
 */
kode_status_t khal_exp_output(uint8_t pos, bool level);

/**
 * @brief Drive @p pos, which must be an output.
 * @param pos   The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @param level The level to drive.
 * @return KODE_OK; KODE_DENIED when out of range or not an output; what the pin answered otherwise.
 */
kode_status_t khal_exp_write(uint8_t pos, bool level);

/**
 * @brief The level of an IN or OUT position.
 * @param pos The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @return The level; false out of range or in any other mode.
 */
bool khal_exp_level(uint8_t pos);

/**
 * @brief Make @p pos an analog input.
 * @param pos The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @return KODE_OK, also when it already is one; KODE_DENIED when out of range,
 *         lent or with no ADC channel; KODE_ERROR when the channel would not open.
 */
kode_status_t khal_exp_adc(uint8_t pos);

/**
 * @brief Read an ADC position. All three outputs are written on every path, refusals included.
 * @param pos        The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @param raw        The 12-bit count.
 * @param mv         Millivolts, calibrated when @p calibrated says so.
 * @param calibrated Whether @p mv came through the chip's calibration scheme.
 * @return KODE_DENIED when @p pos is not an ADC position or a pointer is
 *         NULL, otherwise whatever the conversion answered.
 */
kode_status_t khal_exp_adc_read(uint8_t pos, uint16_t *raw, int32_t *mv,
                                bool *calibrated);

/**
 * @brief Make @p pos a PWM output, or retune one that already is.
 *
 * Each position gets a channel from khal_pwm, on a timer shared only with
 * positions at the same frequency; no position's frequency moves another's.
 *
 * @param pos      The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @param hz       KHAL_EXP_PWM_HZ_MIN..KHAL_EXP_PWM_HZ_MAX.
 * @param duty_x10 0..KHAL_EXP_PWM_DUTY_MAX, tenths of a percent.
 * @return KODE_OK; KODE_DENIED when out of range or lent; KODE_NO_RESOURCES
 *         when khal_pwm has no channel or no timer for it, the position as it
 *         was; KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_exp_pwm(uint8_t pos, uint32_t hz, uint16_t duty_x10);

/**
 * @brief Attach an interrupt to an IN position; khal_gpio's contract.
 * @param pos  The position, which must already be an input.
 * @param kind What the interrupt reacts to.
 * @param cb   The callback, which runs in interrupt context.
 * @param arg  Handed back to @p cb untouched.
 * @return KODE_DENIED when @p pos is not IN or @p cb is NULL.
 */
kode_status_t khal_exp_irq_attach(uint8_t pos, khal_gpio_irq_t kind,
                                  khal_gpio_cb_t cb, void *arg);

/**
 * @brief Re-enable @p pos's interrupt after it has been served.
 * @param pos The position, which must be an input.
 */
void khal_exp_irq_enable(uint8_t pos);

/**
 * @brief Put @p pos back to FREE and high-Z.
 * @param pos The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @return KODE_OK; KODE_DENIED when out of range or lent.
 */
kode_status_t khal_exp_release(uint8_t pos);

/**
 * @brief Lend @p n positions to a peripheral driver, all or none.
 *
 * Every position must be FREE; one refusal refuses the set and the table is
 * untouched.
 * @param pos  The positions, in the caller's order.
 * @param n    How many; at most KHAL_EXP_POS_MAX.
 * @param tag  Who holds them; a string that outlives the loan.
 * @param gpio Where the GPIO numbers go, one per position, same order.
 * @return KODE_DENIED when an argument is bad or a position is not free.
 */
kode_status_t khal_exp_lend(const uint8_t *pos, size_t n, const char *tag,
                            uint8_t *gpio);

/**
 * @brief Free every position lent under @p tag and put its pins to high-Z,
 *        and every controller taken under it (khal_exp_ctrl()).
 * @param tag The loan's name, as it was given to khal_exp_lend().
 * @return KODE_DENIED for a NULL tag, KODE_OK otherwise - a tag that holds
 *         nothing frees nothing and is not an error.
 */
kode_status_t khal_exp_return(const char *tag);

/** @brief One controller an app may take, and who holds it. */
typedef struct {
    kbsp_exp_bus_t bus;   /**< Its kind.                                  */
    uint8_t        id;    /**< The number in its name: I2C1, SPI3, UART2. */
    const char    *tag;   /**< Who took it, or NULL while it is free.     */
} khal_exp_ctrl_info_t;

/**
 * @brief Take a controller for J3 under @p tag: the lowest of its kind in
 *        KBSP_EXP_CTRL that nobody holds.
 *
 * The books only: the caller installs IDF's driver itself with the number it
 * gets. The controller goes back with the positions, by khal_exp_return(),
 * and every one goes back at khal_exp_init().
 *
 * @param bus KBSP_EXP_I2C, KBSP_EXP_SPI or KBSP_EXP_UART.
 * @param tag Who holds it; a string that outlives the loan, as for
 *            khal_exp_lend().
 * @param id  Where the number goes: the one in its name (I2C1 is 1).
 * @return KODE_OK; KODE_NO_RESOURCES when every one of that kind is held;
 *         KODE_DENIED for a NULL @p tag or @p id, or a bus out of range.
 */
kode_status_t khal_exp_ctrl(kbsp_exp_bus_t bus, const char *tag, uint8_t *id);

/**
 * @brief The controller at @p idx of KBSP_EXP_CTRL, and who holds it.
 * @param idx 0..KBSP_EXP_CTRL_COUNT-1; @p out untouched past it.
 * @param out Filled with its kind, number and tag.
 */
void khal_exp_ctrl_info(size_t idx, khal_exp_ctrl_info_t *out);

/**
 * @brief What @p pos is; @p out untouched when @p pos is out of range.
 * @param pos The position, KHAL_EXP_POS_MIN..KHAL_EXP_POS_MAX.
 * @param out Filled with the position's GPIO, capabilities, mode and tag.
 */
void khal_exp_info(uint8_t pos, khal_exp_info_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_EXP_H_ */
