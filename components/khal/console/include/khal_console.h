/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_console.h
 * @brief The console's bytes: the reader task, raw mode for payloads, and writes.
 */
#ifndef KHAL_CONSOLE_H_
#define KHAL_CONSOLE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

#define KHAL_CONSOLE_FRAME_WAIT_MS  100U   /**< How long the cable port's owner waits for a frame from the host:
                                            *   a host that keeps the port in use sends one every millisecond. */
#define KHAL_CONSOLE_OFF_BUS_MS     300U   /**< How long the cable port's pad stays off the bus for a host to see
                                            *   the port leave; back on, the host finds a new device. */
#define KHAL_CONSOLE_FRAME_SPAN_MS  2048U  /**< The span of the frame count khal_console_frame() reads: eleven
                                            *   bits, one frame a millisecond, then it wraps. */
/* cppcheck-suppress misra-c2012-2.5 ; used by khal_console_esp.c's reader, which cppcheck does not analyse: esp_vfs.h stops it with an #error on FD_SETSIZE */
#define KHAL_CONSOLE_REST_MS        1000U  /**< The reader's one wait while the console rests (khal_console_rest()):
                                            *   the longest a P4 in light sleep goes without a wake for it. */
/* cppcheck-suppress misra-c2012-2.5 ; used by khal_console_esp.c's writer, which cppcheck does not analyse: esp_vfs.h stops it with an #error on FD_SETSIZE */
#define KHAL_CONSOLE_TX_WAIT_MS     500U   /**< Longest a write to the cable waits for the driver's lock, and then as
                                            *   long again for room in its TX ring. Past it nobody is reading, and
                                            *   later writes are dropped until one fits. */
#define KHAL_CONSOLE_TX_HOLD_MS     (2U * KHAL_CONSOLE_TX_WAIT_MS)  /**< Longest one chunk written to the cable
                                            *   holds its writer: the lock's wait, then the ring's. */

_Static_assert(KHAL_CONSOLE_FRAME_WAIT_MS < KHAL_CONSOLE_FRAME_SPAN_MS,
               "a wait shorter than the count's span: a frame in it always moves the count");

/**
 * @brief Install the transport's driver, input verbatim. Call once.
 *
 * With CONFIG_KHAL_CONSOLE_APP_PORT the cable port's driver is the app's:
 * this calls khal_console_app_init() instead.
 */
void khal_console_init(void);

/**
 * @brief Tell the reader the board is asleep, or that it is not any more.
 *
 * The reader wakes about 200 times a second to look at the dock, and about 20 to
 * look for a raw-mode request, which is a wake of the P4 every time. While the
 * board sleeps there is no host on either port (a cable never sleeps) and no
 * verb to ask for raw mode, so it waits KHAL_CONSOLE_REST_MS in one go. Bytes on
 * the cable still end the wait at once; the dock and a raw-mode request are
 * heard within that wait. A timer that does not wake the P4 by itself runs at
 * the reader's wake after it is due, so it runs at least that late.
 *
 * @param rest true while the board sleeps. Takes effect from the reader's next wait.
 */
void khal_console_rest(bool rest);

/**
 * @brief Read the cable port's frame count: the index of the last
 *        start-of-frame the host sent it.
 *
 * A host that keeps the port in use sends a frame every millisecond, so two
 * reads KHAL_CONSOLE_FRAME_WAIT_MS apart that agree mean no frame came between
 * them. A frame says the host keeps the port in use, not that it talks to it:
 * a port the host suspended gets none either.
 *
 * @param frame Receives the count, which wraps every KHAL_CONSOLE_FRAME_SPAN_MS.
 * @return KODE_OK; KODE_DENIED for a NULL @p frame, a console with no cable
 *         port (a UART), or a cable port the app provides.
 */
kode_status_t khal_console_frame(uint32_t *frame);

/**
 * @brief Put the cable port's pad on the bus, or take it off.
 *
 * Off, a host sees the port leave; back on, it sees a new device and
 * enumerates it. Each light sleep of the P4 takes the pad off and puts it
 * back by itself, so this is for a P4 held awake.
 *
 * @param on true to put the pad on the bus, false to take it off.
 * @return KODE_OK; KODE_DENIED for a console with no cable port (a UART), or a
 *         cable port the app provides.
 */
kode_status_t khal_console_pad(bool on);

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
 *
 * With CONFIG_KHAL_CONSOLE_APP_PORT the cable's share is one call to
 * khal_console_app_write().
 *
 * @param buf The bytes.
 * @param len How many.
 */
void khal_console_write(const char *buf, size_t len);

/**
 * @brief Bytes of output the cable dropped because its host was not reading.
 *
 * A write waits a bounded time for the host to read; past it, output is
 * dropped until the host reads again, so a host that stops reading never
 * stalls the writer. The cable's input drops nothing: when it is full the
 * cable takes no more from the host, and the host waits. Opening the cable
 * port resets the board, so on the cable this counts from the session's start.
 *
 * @return Bytes dropped since boot. Only ever grows; always 0 on a console
 *         with no cable port (a UART). For a cable port the app provides, the
 *         bytes khal_console_app_write() did not take.
 */
uint32_t khal_console_tx_dropped(void);

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

/* ---- The cable port the app provides (CONFIG_KHAL_CONSOLE_APP_PORT) ---- */

/**
 * @brief Bring the app's port up. Called once, from khal_console_init().
 *
 * The app defines it when CONFIG_KHAL_CONSOLE_APP_PORT is on, and the console
 * then installs no driver on the USB-Serial/JTAG: the port's driver is the
 * app's. The console reads nothing from that port, khal_console_frame() and
 * khal_console_pad() answer KODE_DENIED, and khal_console_tx_dropped() counts
 * what khal_console_app_write() did not take. The dock, when built, stays the
 * console's. Only khal calls the two functions, and it comes after the app's
 * component in the link: defined beside app_main they arrive with that object,
 * but in an object nothing else pulls in they are undefined at link, so an
 * ESP-IDF app that keeps them there registers its component with WHOLE_ARCHIVE.
 */
void khal_console_app_init(void);

/**
 * @brief Write to the app's port.
 *
 * The app defines it when CONFIG_KHAL_CONSOLE_APP_PORT is on. Each console
 * write is one call, with no line endings turned. After a call that took less
 * than @p len, the next ones get a @p wait_ms of 0 until one takes all.
 *
 * @param buf     The bytes.
 * @param len     How many.
 * @param wait_ms The longest this call may wait for room.
 * @return Bytes taken, 0 to @p len; @p len or more is the whole write.
 */
size_t khal_console_app_write(const char *buf, size_t len, uint32_t wait_ms);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_CONSOLE_H_ */
