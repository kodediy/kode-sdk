/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_power.h
 * @brief The P4's sleep: the awake lock, the light-sleep wake lines, and deep sleep.
 */
#ifndef KHAL_POWER_H_
#define KHAL_POWER_H_

#include <stdbool.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- Limits ---- */

/* cppcheck-suppress misra-c2012-2.5 ; used by khal_power_esp.c, in a region cppcheck reads under CONFIG_KODE_LOWPOWER, whose macro uses it does not record */
#define KHAL_POWER_LOCKS       4U      /**< Locks khal_power_lock_new() hands out, besides the awake lock. */
#define KHAL_POWER_LOCK_NONE   0xFFU   /**< The handle of no lock. */
/* cppcheck-suppress misra-c2012-2.5 ; used by khal_power_esp.c, in a region cppcheck reads under CONFIG_KODE_LOWPOWER, whose macro uses it does not record */
#define KHAL_POWER_WAKE_LINES  4U      /**< Wake lines armed at once. */

/* ---- Types ---- */

/** @brief A lock handle, from khal_power_lock_new(); KHAL_POWER_LOCK_NONE is none. */
typedef uint8_t khal_power_lock_t;

/** @brief What the light sleeps have added up to since khal_power_init(). */
typedef struct {
    uint64_t slept_us;   /**< Microseconds spent in light sleep. */
    uint64_t since_us;   /**< Microseconds since khal_power_init(). */
    uint32_t sleeps;     /**< Light sleeps entered and left. */
} khal_power_stats_t;

/** @brief A wake line's callback. Runs in interrupt context, once per arming. */
typedef void (*khal_power_wake_cb_t)(void *arg);

/* ---- Light sleep ---- */

/**
 * @brief Configure power management and take the awake lock.
 *
 * The clock is fixed at 400 MHz, and light sleep is allowed whenever no lock
 * is held. The awake lock starts held: the board is AWAKE until
 * khal_power_hold(false). A second call is a no-op.
 *
 * @return KODE_OK; KODE_NO_RESOURCES when the lock could not be created;
 *         KODE_ERROR when power management would not configure.
 */
kode_status_t khal_power_init(void);

/**
 * @brief Take or give back the awake lock. Held, the P4 never light-sleeps.
 *
 * Idempotent: holding twice holds once. One caller owns it (AO_LowPower).
 *
 * @param hold true to hold, false to let go.
 */
void khal_power_hold(bool hold);

/**
 * @brief Create a lock of its own, for a short window that must not be slept through.
 * @param name Shown in the power-management dump; must outlive the lock.
 * @param out  Receives the handle.
 * @return KODE_OK; KODE_DENIED when @p name or @p out is NULL;
 *         KODE_NO_RESOURCES when KHAL_POWER_LOCKS are taken or the driver
 *         refused.
 */
kode_status_t khal_power_lock_new(const char *name, khal_power_lock_t *out);

/**
 * @brief Take or give back one of the locks from khal_power_lock_new(). Idempotent.
 * @param l    The handle; KHAL_POWER_LOCK_NONE and unknown handles are ignored.
 * @param hold true to hold, false to let go.
 */
void khal_power_lock(khal_power_lock_t l, bool hold);

/**
 * @brief Arm a level wake line.
 *
 * The line wakes the P4 from light sleep while it sits at the active level.
 * The first thing its interrupt does is disarm it (both the interrupt and
 * the wake source), then call @p cb: a level that stays asserted would
 * otherwise wake the chip again at once, without end. khal_power_wake_rearm()
 * arms it again once its cause has been served. The pin is made an input with
 * its pull toward the inactive level.
 *
 * @param gpio       The P4 GPIO number, from kbsp.
 * @param level_high true to wake while the line is high, false while low.
 * @param cb         Called from the interrupt; must only post an event.
 * @param arg        Passed through to @p cb.
 * @return KODE_OK; KODE_DENIED when @p gpio is not a pin the P4 has or @p cb
 *         is NULL; KODE_NO_RESOURCES when KHAL_POWER_WAKE_LINES are taken;
 *         KODE_ERROR when the driver refused.
 */
kode_status_t khal_power_wake_arm(uint32_t gpio, bool level_high,
                                  khal_power_wake_cb_t cb, void *arg);

/**
 * @brief Arm a line again after its callback ran. Nothing happens on a pin that was never armed.
 * @param gpio The P4 GPIO number given to khal_power_wake_arm().
 */
void khal_power_wake_rearm(uint32_t gpio);

/**
 * @brief The light-sleep accounting so far.
 * @param out Receives the numbers; zeros before khal_power_init().
 */
void khal_power_stats(khal_power_stats_t *out);

/* ---- Deep sleep (the OFF state) ---- */

/**
 * @brief Whether this boot is a wake from deep sleep by the expander's interrupt line.
 * @return true after a wake from OFF; false on a cold boot or any other wake.
 */
bool khal_power_woke_from_off(void);

/**
 * @brief Let go of the pins deep sleep held.
 *
 * Call it before anything configures a pin. After a deep sleep each held pin
 * is first set to the level and direction it was held at, while the pad is
 * still held, and only then released: releasing first, with the pad left as
 * an input with a pull-up, pulses the panel's reset line, which has no board
 * pull-up. On any other boot it does nothing, so a cold boot never resets
 * the C5 or powers the card down.
 */
void khal_power_holds_release(void);

/**
 * @brief Enter deep sleep. Does not return.
 *
 * Holds the card's supply, the NFC chip's select, the LF front end and the
 * panel's reset at their rest levels, arms the expander's interrupt line
 * (low) as the one wake source, and only then holds the C5's enable low and
 * starts deep sleep: the C5 goes into reset as the last write before the
 * chip stops, so the esp-hosted tasks that still run never see it vanish.
 * The caller has rested every device and cleared any pending expander edge.
 */
void khal_power_off_enter(void);

/**
 * @brief Leave a word for the next wake from OFF.
 *
 * Kept in the memory deep sleep keeps, read back by khal_power_off_note(). A
 * cold boot starts from 0, and another image in between may leave anything, so
 * a caller compares with a value of its own rather than asking for non-zero.
 *
 * @param note The word; 0 to clear it.
 */
void khal_power_off_note_set(uint32_t note);

/**
 * @brief The word khal_power_off_note_set() left.
 * @return The word; 0 when there is none.
 */
uint32_t khal_power_off_note(void);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_POWER_H_ */
