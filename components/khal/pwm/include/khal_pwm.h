/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_pwm.h
 * @brief The P4's LEDC, handed out by channel: the only owner of its channels and timers.
 */
#ifndef KHAL_PWM_H_
#define KHAL_PWM_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits ---- */

#define KHAL_PWM_CHANNELS     8U      /**< Low-speed LEDC channels: the most PWM outputs at once. */
#define KHAL_PWM_TIMERS       4U      /**< LEDC timers: the most distinct frequencies at once. */
#define KHAL_PWM_DUTY_MAX     1000U   /**< Duty in tenths of a percent: 100.0 % is full on. */
#define KHAL_PWM_BITS_WIDEST  0U      /**< As a resolution: the widest the clock allows at the frequency. */
#define KHAL_PWM_NONE         0xFFU   /**< The handle of no channel. */

/* ---- Types ---- */

/** @brief A channel handle, from khal_pwm_open(); KHAL_PWM_NONE is none. */
typedef uint8_t khal_pwm_t;

/** @brief What khal_pwm_open() is asked for. */
typedef struct {
    uint8_t  gpio;        /**< The pin the channel drives. */
    uint32_t hz;          /**< The frequency, Hz. */
    uint8_t  bits;        /**< Duty resolution, or KHAL_PWM_BITS_WIDEST. */
    uint16_t duty_x10;    /**< 0..KHAL_PWM_DUTY_MAX, tenths of a percent. */
    bool     own_timer;   /**< A timer nobody else shares; required to pause it. */
} khal_pwm_cfg_t;

/* ---- Calls ---- */

/**
 * @brief Create the lock; every channel and timer starts free. A second call is a no-op.
 * @return KODE_OK; KODE_NO_RESOURCES when the lock could not be created.
 */
kode_status_t khal_pwm_init(void);

/**
 * @brief Start a channel on a timer that runs at @p cfg's frequency and resolution.
 *
 * A shared request joins a shared timer already at the same frequency and
 * resolution, or takes a free one; an own_timer request always takes a free
 * one. On any refusal nothing changes and @p out is KHAL_PWM_NONE.
 *
 * @param cfg What to start.
 * @param out The handle.
 * @return KODE_OK; KODE_DENIED for a NULL pointer, a zero frequency, a duty
 *         over KHAL_PWM_DUTY_MAX or a resolution the clock cannot reach;
 *         KODE_NO_RESOURCES when no channel is free or no timer fits;
 *         KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_open(const khal_pwm_cfg_t *cfg, khal_pwm_t *out);

/**
 * @brief Change a channel's frequency, duty, or both, never another channel's.
 *
 * The same frequency changes the duty alone. A new one joins a shared timer
 * already there, retunes the channel's own timer when it is its only user,
 * or moves to a free timer; the resolution is recomputed only when the
 * channel was opened with KHAL_PWM_BITS_WIDEST.
 *
 * @param ch       The handle.
 * @param hz       The frequency, Hz.
 * @param duty_x10 0..KHAL_PWM_DUTY_MAX, tenths of a percent.
 * @return KODE_OK; KODE_DENIED for a handle that is not open or an argument
 *         out of range; KODE_NO_RESOURCES when no timer fits, the channel
 *         running as it was; KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_set(khal_pwm_t ch, uint32_t hz, uint16_t duty_x10);

/**
 * @brief Stop the channel's timer where it is. Takes no lock: it touches only the caller's own timer.
 * @param ch The handle, opened with own_timer.
 * @return KODE_OK; KODE_DENIED for a handle not open or not own_timer; KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_pause(khal_pwm_t ch);

/**
 * @brief Let a paused timer run again. Takes no lock, as khal_pwm_pause().
 * @param ch The handle, opened with own_timer.
 * @return KODE_OK; KODE_DENIED for a handle not open or not own_timer; KODE_ERROR when the LEDC refused.
 */
kode_status_t khal_pwm_resume(khal_pwm_t ch);

/**
 * @brief Free the channel, and its timer with its last user; the pin is left a plain GPIO driven low.
 * @param ch The handle; one that is not open is ignored.
 */
void khal_pwm_close(khal_pwm_t ch);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_PWM_H_ */
