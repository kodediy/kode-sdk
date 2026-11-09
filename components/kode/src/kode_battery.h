/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kode_battery.h
 * @brief The battery_low edge the standard layer's Active Object asks about.
 *        Private to this component; never installed.
 */
#ifndef KODE_BATTERY_H_
#define KODE_BATTERY_H_

#include <stdbool.h>

#include "kode_sys.h"

/**
 * @brief Whether a reading is a fall below the low mark.
 * @param low In and out: the level fell below the mark and has not recovered.
 * @param r   The reading.
 * @return true when battery_low is due.
 */
bool kode_battery_low_edge(bool *low, const kode_sys_battery_t *r);

#endif /* KODE_BATTERY_H_ */
