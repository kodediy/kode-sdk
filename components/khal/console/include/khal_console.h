/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_console.h
 * @brief The console's bytes: the reader task, raw mode for payloads, and writes.
 */
#ifndef KHAL_CONSOLE_H_
#define KHAL_CONSOLE_H_

#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Install the transport's driver, input verbatim. Call once. */
void khal_console_init(void);

/**
 * @brief Tell the reader the board is asleep, or that it is not any more.
 *
 * The reader wakes about 200 times a second to look at the dock, and about 20 to
 * look for a raw-mode request, which is a wake of the P4 every time. Asleep there
 * is no host on either port (a cable never sleeps) and no verb to ask for raw
 * mode, so it waits a second in one go. Bytes on the cable still end the wait at
 * once; the dock and a raw-mode request are heard within that second.
 *
 * @param rest true while the board sleeps. Takes effect from the reader's next wait.
 */
void khal_console_rest(bool rest);

/**
 * @brief Start the task that blocks on the console and reports each byte.
 * @param on_byte Called with each byte; it posts and returns.
 * @param prio    Scheduler priority for the task.
 * @return KODE_OK; KODE_ERROR for a NULL @p on_byte; KODE_NO_RESOURCES when
 *         the task cannot be created.
 */
kode_status_t khal_console_start_reader(void (*on_byte)(uint8_t byte),
                                        uint32_t prio);

/**
 * @brief Ask the reader task to call @p pump instead of reporting bytes.
 *
 * At its next pass the reader discards pending input, enters raw mode and
 * calls @p on_armed. @p pump then runs on the reader task until it calls
 * khal_console_raw_end().
 *
 * @param pump     Reads the payload through khal_console_read_raw().
 * @param on_armed Called once, from the reader task, when raw mode is live.
 * @return KODE_OK; KODE_DENIED for a NULL argument or when raw mode is
 *         already armed or requested.
 */
kode_status_t khal_console_raw_begin(void (*pump)(void), void (*on_armed)(void));

/** @brief Leave raw mode: the reader task goes back to reporting bytes. */
void khal_console_raw_end(void);

/**
 * @brief Read up to @p want bytes; a short read is normal.
 * @param buf        Where the bytes go.
 * @param want       At most this many.
 * @param timeout_ms How long to wait for the first of them.
 * @return Bytes read, 0 on timeout, -1 for bad arguments or no transport.
 */
int32_t khal_console_read_raw(void *buf, size_t want, uint32_t timeout_ms);

/**
 * @brief Write bytes to the console, on every port it has.
 * @param buf The bytes.
 * @param len How many.
 */
void khal_console_write(const char *buf, size_t len);

/**
 * @brief Let the dock console's pins go: TinyUSB off the pads and the pads
 *        handed to the GPIO matrix, for a test that drives them itself.
 *
 * The cable console is untouched. Without the dock built in
 * (CONFIG_KHAL_CONSOLE_DOCK off) nothing holds the pins and this does nothing.
 *
 * @return KODE_OK.
 */
kode_status_t khal_console_dock_release(void);

/**
 * @brief Put the dock console back on its pins after khal_console_dock_release().
 *
 * The pins must be inputs again, their UART freed, before this is called.
 * A dock already up, or none built in, is left as it is.
 *
 * @return KODE_OK; KODE_ERROR when TinyUSB would not install again, the
 *         cable console intact.
 */
kode_status_t khal_console_dock_take(void);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_CONSOLE_H_ */
