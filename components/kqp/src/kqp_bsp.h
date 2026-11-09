/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kqp_bsp.h
 * @brief Package-private: what QP needs from the machine.
 */
#ifndef KQP_BSP_H_
#define KQP_BSP_H_

#include <stdbool.h>
#include <stdint.h>

/** @brief Start the periodic source that drives QTIMEEVT_TICK_X(). */
void kqp_tick_start(void);

/**
 * @brief Whether the QP tick is stopped by kqp_tick_rest().
 *
 * The Active Object supervisor reads it: with the tick stopped nothing
 * dispatches on purpose, which is no hang.
 *
 * @return true from just before the tick stops until just after it runs again.
 */
bool kqp_tick_resting(void);

/**
 * @brief How many times the tick has been put back after a rest.
 *
 * The supervisor compares it with the value it saw last, and forgets what it
 * remembered about every Active Object when it moved: a rest is not time any
 * of them spent stuck.
 *
 * @return The count of completed rests; wraps.
 */
uint32_t kqp_tick_rests(void);

#endif /* KQP_BSP_H_ */
