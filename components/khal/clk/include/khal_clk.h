/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file khal_clk.h
 * @brief The clock block: which slow source the mux picked, and what a source really runs at.
 */
#ifndef KHAL_CLK_H_
#define KHAL_CLK_H_

#include <stdint.h>

#include "kode_defs.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief What the RTC slow-clock mux is actually switched to. */
typedef enum {
    KHAL_CLK_SLOW_RC = 0,     /**< The internal RC, about 136-150 kHz.     */
    KHAL_CLK_SLOW_XTAL32K,    /**< The external 32.768 kHz crystal.        */
    KHAL_CLK_SLOW_RC32K,      /**< The internal 32 kHz RC. Not used here.  */
    KHAL_CLK_SLOW_UNKNOWN,    /**< Something this enum does not name.      */
} khal_clk_slow_t;

/** @brief A slow source that can be counted; milli-hertz in 32 bits tops out at 4.29 MHz. */
typedef enum {
    KHAL_CLK_SRC_XTAL32K = 0, /**< The 32.768 kHz crystal, whether or not it
                               *   is the selected slow clock.             */
    KHAL_CLK_SRC_RC_SLOW,     /**< The internal slow RC: the fallback.     */
} khal_clk_src_t;

/** @brief A derived clock, read rather than counted. */
typedef enum {
    KHAL_CLK_TREE_CPU = 0,    /**< The CPU clock.                          */
    KHAL_CLK_TREE_APB,        /**< The peripheral bus clock.               */
    KHAL_CLK_TREE_RTC_SLOW,   /**< Whatever the slow mux is switched to.   */
    KHAL_CLK_TREE_RTC_FAST,   /**< The LP fast clock.                      */
} khal_clk_tree_t;

/**
 * @brief The main crystal's nominal frequency: what every measurement is counted against.
 * @return Frequency in Hz, or 0 if the chip reports none.
 */
uint32_t khal_clk_main_hz(void);

/**
 * @brief Which source the RTC slow-clock mux selected, read from the mux itself.
 *
 * The only way to see the start-up fallback to the RC when the 32 kHz crystal
 * does not start: ESP-IDF sets no flag for it.
 *
 * @return The selected source.
 */
khal_clk_slow_t khal_clk_slow_src(void);

/**
 * @brief Count a source against the main crystal.
 *
 * @note A busy wait: the caller's core is held for @p cycles periods of the
 *       source, about 31 us each at 32.768 kHz.
 *
 * @param[in]  src      Which source to count.
 * @param[in]  cycles   Cycles of @p src to count; more is finer and slower.
 * @param[out] milli_hz The frequency in milli-hertz, set only on KODE_OK.
 * @return KODE_OK; KODE_TIMEOUT when the source does not oscillate (an absent
 *         or dead crystal); KODE_DENIED on a bad argument.
 */
kode_status_t khal_clk_measure(khal_clk_src_t src, uint32_t cycles,
                               uint32_t *milli_hz);

/**
 * @brief Read a derived clock's real frequency.
 *
 * @param[in]  clk Which clock.
 * @param[out] hz  Its frequency in Hz. Untouched unless KODE_OK is returned.
 * @return KODE_OK, or KODE_DENIED on a bad argument, or KODE_ERROR when the
 *         chip cannot answer for that clock.
 */
kode_status_t khal_clk_tree_hz(khal_clk_tree_t clk, uint32_t *hz);

#ifdef __cplusplus
}
#endif

#endif /* KHAL_CLK_H_ */
