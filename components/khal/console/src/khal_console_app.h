/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_console_app.h
 * @brief The console's writes to a cable port the app provides, private to khal's console.
 */
#ifndef KHAL_CONSOLE_APP_H_
#define KHAL_CONSOLE_APP_H_

#include <stddef.h>
#include <stdint.h>

/**
 * @brief Hand one write to the app's port, in one call to khal_console_app_write().
 *
 * The call waits up to KHAL_CONSOLE_TX_WAIT_MS for room. A write the port
 * takes only part of counts the rest in khal_console_app_dropped(), and every
 * later call waits 0 until one is taken whole: nobody is reading, and a writer
 * that waited each time would stall AO_Log.
 *
 * @param buf The bytes.
 * @param len How many; 0 reaches no port.
 */
void khal_console_app_put(const char *buf, size_t len);

/**
 * @brief Bytes the app's port did not take.
 * @return Bytes dropped since boot. Only ever grows.
 */
uint32_t khal_console_app_dropped(void);

#endif /* KHAL_CONSOLE_APP_H_ */
