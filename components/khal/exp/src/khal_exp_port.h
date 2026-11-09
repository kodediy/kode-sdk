/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_exp_port.h
 * @brief khal_exp's private seam to the ADC, mocked on the host.
 */
#ifndef KHAL_EXP_PORT_H_
#define KHAL_EXP_PORT_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

/**
 * @brief Open ADC1 channel @p chan: oneshot, 12 bits, 12 dB attenuation.
 * @param chan The ADC1 channel, from kbsp's table.
 * @return KODE_OK; KODE_DENIED for a channel the unit lacks; KODE_ERROR when
 *         the unit or the channel could not be set up.
 */
kode_status_t khal_exp_port_adc_open(uint8_t chan);

/**
 * @brief One conversion. @p mv through the calibration scheme when there is one.
 * @param chan       The ADC1 channel, already open.
 * @param raw        Always filled: the 12-bit count.
 * @param mv         Filled: millivolts, an estimate without calibration.
 * @param calibrated Whether @p mv came through the chip's scheme.
 * @return KODE_OK; KODE_DENIED for a channel that is not open; KODE_IO_ERROR
 *         when the conversion itself failed.
 */
kode_status_t khal_exp_port_adc_read(uint8_t chan, uint16_t *raw, int32_t *mv,
                                     bool *calibrated);

/**
 * @brief Give the channel back.
 * @param chan The ADC1 channel to close.
 */
void khal_exp_port_adc_close(uint8_t chan);

#endif /* KHAL_EXP_PORT_H_ */
