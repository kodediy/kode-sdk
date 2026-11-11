/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_time.h
 * @brief The chip's clock: time since boot, wall clock, local time, a delay,
 *        a periodic timer and a one-shot.
 */
#ifndef KHAL_TIME_H_
#define KHAL_TIME_H_

#include <stdbool.h>
#include <stdint.h>
/* cppcheck-suppress misra-c2012-21.10 ; khal_time_local() answers in the C library's struct tm */
#include <time.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Time since boot ---- */

/**
 * @brief Microseconds since boot. Monotonic.
 * @return The microseconds.
 */
uint64_t khal_time_us(void);

/**
 * @brief Milliseconds since boot. Monotonic, wraps after ~49 days.
 * @return The milliseconds.
 */
uint32_t khal_time_ms(void);

/**
 * @brief Sleep, yielding the CPU. Never call this from an AO.
 * @param ms How long, milliseconds.
 */
void khal_delay_ms(uint32_t ms);

/* ---- Wall clock (UTC; may be unset) ---- */

/**
 * @brief Microseconds since the Unix epoch, UTC.
 * @return The reading; at or near 0 on a board never told the time
 *         (khal_time_wall_known()).
 */
uint64_t khal_time_wall_us(void);

/**
 * @brief Set the wall clock.
 * @param unix_us Microseconds since the Unix epoch, UTC.
 */
void khal_time_wall_set(uint64_t unix_us);

/**
 * @brief Whether the clock has been told the time.
 * @return true when the reading is past KODE_CLOCK_KNOWN_AFTER_S.
 */
bool khal_time_wall_known(void);

/* ---- Local time (the zone is the C library's, one for the firmware) ---- */

/**
 * @brief Make @p tz the zone every local time is read in, from now on.
 *
 * TZ in the environment, then tzset(), as one step under the environment
 * lock (khal_env_lock()): a local time read on another task meanwhile is
 * wholly the old zone's or wholly the new one's. The zone belongs to the
 * whole firmware: the FAT driver stamps its files in it too.
 *
 * @param tz A POSIX TZ string, such as "CET-1CEST,M3.5.0,M10.5.0/3".
 * @return true when @p tz applied; false for NULL, or when the C library had
 *         no memory for it, and TZ is then taken out, which is UTC.
 */
bool khal_time_zone_set(const char *tz);

/**
 * @brief @p unix_s as local time, in the zone khal_time_zone_set() applied.
 *
 * localtime_r() under the environment lock, so a change of zone never lands
 * in the middle of it. UTC when no zone was ever applied.
 *
 * @param unix_s Seconds since the Unix epoch, UTC.
 * @param[out] out The broken-down local time; written only on true.
 * @return false for NULL, or when the library would not convert it.
 */
bool khal_time_local(int64_t unix_s, struct tm *out);

/**
 * @brief The khal_time_us() reading at which the wall clock will read a whole
 *        second.
 *
 * What an owner tells AO_LowPower with lowpower_wake_at(), which counts in
 * time since boot and not in the date. A second that has already passed gives
 * the reading now (wake at once). The answer is never 0, which a wake time
 * takes to mean "nothing".
 *
 * @param wall_s Unix seconds, UTC.
 * @return The khal_time_us() reading; 0 for a negative @p wall_s (no time).
 */
uint64_t khal_time_at_wall_s(int64_t wall_s);

/* ---- Periodic timer ---- */

#define KHAL_TIMER_PERIODIC_SLOTS  4U   /**< Periodic timers that can be stopped and restarted. */

/**
 * @brief Start a periodic callback. It runs until khal_timer_periodic_run()
 *        stops it, or until the next reset (Mainly used for the QP timer).
 *
 * The callback is also the timer's name for khal_timer_periodic_run(): one
 * timer per callback, and starting the same callback again replaces the
 * earlier entry (the earlier timer keeps running, unnamed).
 *
 * A callback that is late is called once, not once for every period it
 * owes: the periods a stall or a sleep took are skipped, and the next call
 * comes one period after the late one. The timer does not count towards the
 * time a light sleep may last.
 *
 * @param cb        Runs in task context, above every AO but AO_Rfid.
 * @param arg       Passed through to @p cb.
 * @param period_us Period, microseconds.
 * @return KODE_OK; KODE_NO_RESOURCES when no timer is free, or when all
 *         KHAL_TIMER_PERIODIC_SLOTS are named by other callbacks; KODE_ERROR
 *         when it would not start.
 */
kode_status_t khal_timer_periodic(void (*cb)(void *), void *arg, uint64_t period_us);

/**
 * @brief Stop or restart a periodic timer started by khal_timer_periodic().
 *
 * Idempotent: stopping a stopped timer, or running a running one, is
 * KODE_OK and touches nothing. A restart keeps the original period and
 * phase-resets the timer (the first expiry is one period away). A callback
 * already running is not interrupted. Not safe against a concurrent call for
 * the same callback; the owner of the timer is the one caller.
 *
 * @param cb  The callback the timer was started with.
 * @param run true to run it, false to stop it.
 * @return KODE_OK; KODE_DENIED when @p cb names no periodic timer; KODE_ERROR
 *         when the timer would not stop or start.
 */
kode_status_t khal_timer_periodic_run(void (*cb)(void *), bool run);

/* ---- One-shot timer ---- */

#define KHAL_TIMER_ONCE_SLOTS  4U   /**< One-shots pending at once. */

/**
 * @brief Run a callback once, @p delay_us from now.
 *
 * The slot is free again before @p cb runs, so a callback may arm the next
 * one-shot. There is no cancel: a callback that no longer applies checks its
 * own state.
 *
 * @param cb       Runs in task context, like khal_timer_periodic()'s.
 * @param arg      Passed through to @p cb.
 * @param delay_us Delay, microseconds.
 * @return KODE_OK; KODE_DENIED when @p cb is NULL; KODE_NO_RESOURCES when
 *         KHAL_TIMER_ONCE_SLOTS are pending or no timer could be created;
 *         KODE_ERROR when it would not start.
 */
kode_status_t khal_timer_once(void (*cb)(void *), void *arg, uint64_t delay_us);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_TIME_H_ */
