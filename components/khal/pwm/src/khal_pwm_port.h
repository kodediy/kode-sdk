/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_pwm_port.h
 * @brief khal_pwm's private seam to the LEDC, mocked on the host.
 */
#ifndef KHAL_PWM_PORT_H_
#define KHAL_PWM_PORT_H_

#include <stdint.h>

#include "kode_defs.h"

/* ---- The chip ---- */

#define KHAL_PWM_PORT_CLK_HZ    80000000ULL   /**< PLL_F80M: the one clock of every timer. */
#define KHAL_PWM_PORT_BITS_MAX  20U           /**< The widest duty resolution a timer has. */

/* ---- Timers ---- */

/**
 * @brief Configure or retune a timer.
 * @param timer 0..KHAL_PWM_TIMERS-1.
 * @param hz    The frequency.
 * @param bits  The duty resolution, already checked against the clock.
 * @return KODE_OK, or KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_port_timer_set(uint8_t timer, uint32_t hz, uint8_t bits);

/**
 * @brief Stop a timer whose last channel left.
 * @param timer 0..KHAL_PWM_TIMERS-1.
 */
void khal_pwm_port_timer_stop(uint8_t timer);

/**
 * @brief Hold a timer where it is.
 * @param timer 0..KHAL_PWM_TIMERS-1.
 * @return KODE_OK, or KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_port_timer_pause(uint8_t timer);

/**
 * @brief Let a held timer run again.
 * @param timer 0..KHAL_PWM_TIMERS-1.
 * @return KODE_OK, or KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_port_timer_resume(uint8_t timer);

/* ---- Channels ---- */

/**
 * @brief Route a channel to @p gpio on @p timer and start it.
 * @param chan  0..KHAL_PWM_CHANNELS-1.
 * @param gpio  The pin.
 * @param timer The timer, already set.
 * @param duty  The duty as a count at the timer's resolution.
 * @return KODE_OK, or KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_port_chan_start(uint8_t chan, uint8_t gpio, uint8_t timer,
                                       uint32_t duty);

/**
 * @brief Move a running channel to another timer and set its duty.
 * @param chan  0..KHAL_PWM_CHANNELS-1.
 * @param timer The timer, already set.
 * @param duty  The duty as a count at that timer's resolution.
 * @return KODE_OK, or KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_port_chan_bind(uint8_t chan, uint8_t timer, uint32_t duty);

/**
 * @brief Change a running channel's duty.
 * @param chan 0..KHAL_PWM_CHANNELS-1.
 * @param duty The duty as a count at its timer's resolution.
 * @return KODE_OK, or KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_port_chan_duty(uint8_t chan, uint32_t duty);

/**
 * @brief Stop a channel and take it off its pin, which is left a plain GPIO driven low.
 * @param chan 0..KHAL_PWM_CHANNELS-1.
 * @param gpio The pin khal_pwm_port_chan_start() routed it to.
 */
void khal_pwm_port_chan_stop(uint8_t chan, uint8_t gpio);

#endif /* KHAL_PWM_PORT_H_ */
