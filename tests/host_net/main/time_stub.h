/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file time_stub.h
 * @brief The clock kdrv_net reads in this binary, and the hand that moves it.
 */
#ifndef TIME_STUB_H_
#define TIME_STUB_H_

#include <stdint.h>

/**
 * @brief Set the clock.
 * @param ms Milliseconds since boot, as khal_time_ms() will report.
 */
void time_stub_set_ms(uint32_t ms);

/**
 * @brief Move the clock on.
 * @param ms Milliseconds to add.
 */
void time_stub_advance_ms(uint32_t ms);

#endif /* TIME_STUB_H_ */
