/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file kdrv_clocks.h
 * @brief The Kode Dot's two crystals: their real rates, and whether the board uses the one fitted.
 */
#ifndef KDRV_CLOCKS_H_
#define KDRV_CLOCKS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Cycles to count for a quick reading: about 31 ms, under 1 ppm. */
#define KDRV_CLOCKS_CYCLES_FAST   1024U

/** @brief Cycles for a bench reading: about 92 ms, about 0.27 ppm. */
#define KDRV_CLOCKS_CYCLES_FINE   3000U

/** @brief Which source the RTC slow clock is actually running on. */
typedef enum {
    KDRV_CLOCKS_SLOW_RC = 0,    /**< The internal RC: the fallback.     */
    KDRV_CLOCKS_SLOW_XTAL32K,   /**< X3, which is what this board wants. */
    KDRV_CLOCKS_SLOW_RC32K,     /**< The internal 32 kHz RC.             */
    KDRV_CLOCKS_SLOW_UNKNOWN,   /**< The mux reads something else.       */
} kdrv_clocks_slow_t;

/**
 * @brief One pass over the clock block.
 *
 * Frequencies counted against X1 are in milli-hertz, which is the unit the
 * calibration counter's fixed-point period converts into exactly; the
 * derived clocks, which are read rather than counted, are in whole hertz.
 * A `*_ok` flag that is false means the counter timed out - the source
 * produced no edges - and the matching frequency field is meaningless.
 *
 * X1's own error is not measurable here: every count is against X1, so main_from_x32k_hz
 * is the X3 reading inverted, not a measurement of X1.
 */
typedef struct {
    uint32_t           main_hz;        /**< X1's nominal frequency.         */
    kdrv_clocks_slow_t slow_src;       /**< What the mux physically picked. */

    bool     x32k_ok;                  /**< X3 produced edges.              */
    uint32_t x32k_mhz;                 /**< X3 in milli-hertz.              */
    int32_t  x32k_ppm;                 /**< X3 against nominal, in ppm.     */
    uint32_t main_from_x32k_hz;        /**< X1 implied by X3, if X3 were
                                        *   exact.                          */

    bool     rc_slow_ok;               /**< The slow RC produced edges.     */
    uint32_t rc_slow_mhz;              /**< The slow RC in milli-hertz. This
                                        *   is the clock the board falls back
                                        *   to, so it is worth reading even
                                        *   when the crystal is fine.       */

    uint32_t cpu_hz;                   /**< 0 when the chip did not answer. */
    uint32_t apb_hz;                   /**< 0 when the chip did not answer. */
    uint32_t rtc_slow_hz;              /**< 0 when the chip did not answer. */
    uint32_t rtc_fast_hz;              /**< 0 when the chip did not answer. */
} kdrv_clocks_t;

/**
 * @brief Take one reading of every clock.
 *
 * Counts two slow sources against X1 and reads four derived clocks. Cost is
 * dominated by the counting: roughly @p cycles times 31 us for X3, plus the
 * slow RC, all of it a busy wait on the calling thread. The fast clocks are
 * read rather than counted and are free.
 *
 * X3 is counted whether or not it is the selected slow clock, but note what
 * a zero reading can and cannot tell you: when the mux is not on X3 the
 * crystal's oscillator is powered down, so "no edges" then means "not
 * running", not "not fitted". With the mux on X3 - which is how this board
 * is built - the two cases coincide and the reading is conclusive.
 *
 * @param[out] out    Filled on success. Untouched on a bad argument.
 * @param[in]  cycles Cycles of each source to count. Use
 *                    KDRV_CLOCKS_CYCLES_FAST or KDRV_CLOCKS_CYCLES_FINE.
 * @return KODE_OK, or KODE_DENIED on a bad argument. A source that is not
 *         oscillating is a reading, not an error.
 */
kode_status_t kdrv_clocks_read(kdrv_clocks_t *out, uint32_t cycles);

/**
 * @brief The one-word name of a slow source, for a wire record.
 *
 * Single token, no spaces: this is what goes on the console after `src=`.
 * The prose version an operator reads stays inside the driver, where the log
 * lines are written.
 *
 * @param[in] s The source.
 * @return A static string, never NULL.
 */
const char *kdrv_clocks_slow_token(kdrv_clocks_slow_t s);

/**
 * @brief Log one reading as `!` lines, in the order an operator reads them.
 *
 * @param[in] r A reading from kdrv_clocks_read(). Does nothing when NULL.
 */
void kdrv_clocks_log(const kdrv_clocks_t *r);

/**
 * @brief The production-test row: read, log, and judge.
 *
 * Two checks, and both are about X3 because X1 cannot be judged from here:
 * the mux must actually be on the crystal, and the crystal must measure
 * 32768 Hz within KBSP_XTAL_32K_TOL_MHZ. That window is +/-1 %, which is
 * presence and sanity rather than the part's +/-20 ppm tolerance - see
 * kbsp_board.h for why the board cannot judge the latter.
 *
 * @return true when both checks pass.
 */
bool kdrv_clocks_test(void);

/* ---- The wall clock -----------------------------------------------------
 * khal holds the value; what lives here is what the board means by it. The
 * split matters on the host, where the value is a fake and this is the half
 * with something to get wrong.
 */

/** @brief Where the wall clock's current value came from. */
typedef enum {
    KDRV_CLOCKS_WALL_RTC = 0,   /**< It was already there at startup.     */
    KDRV_CLOCKS_WALL_SET,       /**< An operator typed it.                */
    KDRV_CLOCKS_WALL_SNTP,      /**< A time server answered.              */
    KDRV_CLOCKS_WALL_SAVED,     /**< Put back from NVS after a power cut:
                                 *   late by however long it was off.     */
} kdrv_clocks_wall_src_t;

/** @brief How often a running clock is written to NVS, in seconds: how
 *         late a time put back after a power cut can be, at most, beyond
 *         the time the board was off. */
#define KDRV_CLOCKS_WALL_KEEP_S  60U

/** @brief A jump this large against the monotonic time (a `time set`, a
 *         time server) is written at once, in seconds. */
#define KDRV_CLOCKS_WALL_JUMP_S  60

/**
 * @brief Whether the board has been told the time.
 *
 * @return true when the wall clock holds a value somebody set.
 */
bool kdrv_clocks_wall_known(void);

/** @brief The last year a timestamp may name: a FAT date ends in 2107 and a 32-bit unsigned time_t on 2106-02-07, so the
 *         last whole year every reader of the clock holds is 2105. */
#define KDRV_CLOCKS_WALL_YEAR_MAX  2105U

/**
 * @brief Set the wall clock from an ISO-8601 UTC timestamp.
 *
 * The only accepted shape is `YYYY-MM-DDThh:mm:ssZ`, exactly twenty
 * characters. Nothing else is guessed at: no offsets, no missing zone, no
 * fractional seconds, no day a month does not have (a 29th of February needs
 * a leap year) and no year past KDRV_CLOCKS_WALL_YEAR_MAX. A timestamp earlier
 * than the board's own threshold is refused as a typo rather than stored as a
 * date that would then read as never set.
 *
 * @param[in] iso The timestamp. NULL is refused.
 * @return KODE_OK, or KODE_DENIED with the clock untouched.
 */
kode_status_t kdrv_clocks_wall_set_iso(const char *iso);

/**
 * @brief Format the wall clock as `YYYY-MM-DDThh:mm:ssZ`.
 *
 * @param[out] out Buffer for the timestamp and its terminator.
 * @param[in]  n   Its size; 21 or more.
 * @return KODE_OK, or KODE_DENIED when the clock is not known, when @p out
 *         is NULL, or when @p n cannot hold a whole timestamp. A truncated
 *         date is worse than none.
 */
kode_status_t kdrv_clocks_wall_iso(char *out, size_t n);

/**
 * @brief Where the current value came from.
 *
 * @return The source.
 */
kdrv_clocks_wall_src_t kdrv_clocks_wall_source(void);

/**
 * @brief The one-word name of a source, for a wire record.
 *
 * @param[in] s The source.
 * @return A static string, never NULL.
 */
const char *kdrv_clocks_wall_source_token(kdrv_clocks_wall_src_t s);

/** @brief A function the clock calls once its value was written. */
typedef void (*kdrv_clocks_wall_hook_t)(void);

/**
 * @brief Register the one function told after every write of the wall clock.
 *
 * A time server or a person writing the clock ends in kdrv_clocks_wall_note_sntp()
 * or kdrv_clocks_wall_note_set(), which call it. It runs in the thread that
 * wrote the clock, after the write: it may post an event and nothing that
 * blocks. The restore at boot does not call it. A driver cannot publish
 * events, so whoever owns work that depends on the hour registers here.
 *
 * @param[in] hook The function; NULL to register none.
 */
void kdrv_clocks_wall_on_set(kdrv_clocks_wall_hook_t hook);

/** @brief Record that the current value arrived from a time server, and tell the hook. */
void kdrv_clocks_wall_note_sntp(void);

/** @brief Record that a person just set the current value, and tell the hook. Either note has the next keep write it. */
void kdrv_clocks_wall_note_set(void);

/**
 * @brief Put back a clock the chip lost with its supply. Once, at boot,
 *        before anything reads the hour.
 *
 * A clock that came up unknown gets the last value kept in NVS, source
 * SAVED. A clock that came up known kept running through a reset; it is
 * still SAVED when NVS says the value it holds was put back. Nothing else
 * is guessed.
 *
 * @return KODE_OK when the clock holds a value, put back or kept; KODE_DENIED
 *         when it is unknown and nothing was kept.
 */
kode_status_t kdrv_clocks_wall_restore(void);

/**
 * @brief Write the clock to NVS when it is due, and the approximate mark
 *        when it changed.
 *
 * Due on the first keep of the boot, every KDRV_CLOCKS_WALL_KEEP_S, on a
 * jump of KDRV_CLOCKS_WALL_JUMP_S, or when @p force. An unknown clock is
 * never written. A write that fails waits for the next period.
 *
 * @param force Write now, whatever the schedule: the supply is going.
 * @return true when the clock was written.
 */
bool kdrv_clocks_wall_keep(bool force);

/**
 * @brief Whether the clock holds a value put back after a power cut, which
 *        nothing has confirmed since.
 *
 * @return true while the source is KDRV_CLOCKS_WALL_SAVED.
 */
bool kdrv_clocks_wall_approx(void);

#ifdef __cplusplus
}
#endif

#endif /* KDRV_CLOCKS_H_ */
