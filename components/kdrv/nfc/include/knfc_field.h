/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file knfc_field.h
 * @brief The listen window's stats: min, mean, peak and modulation of the 13.56 MHz field.
 */
#ifndef KNFC_FIELD_H_
#define KNFC_FIELD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One listen window's answer. All zero when nothing was measured. */
typedef struct {
    uint8_t  min;   /**< Lowest amplitude byte in the window.            */
    uint8_t  mean;  /**< Integer mean of every amplitude byte.           */
    uint8_t  peak;  /**< Highest amplitude byte in the window.           */
    uint8_t  mod;   /**< peak - min: how much the field moved.           */
    bool     ext;   /**< The chip's external field detector was set.     */
    uint16_t n;     /**< Samples taken.                                  */
} knfc_field_t;

/**
 * @brief The running stats of an open window. Opaque in use: reset it, fold
 *        bursts into it, close it; read the answer from knfc_field_t.
 */
typedef struct {
    uint32_t sum;   /**< Sum of every folded amplitude byte.             */
    uint8_t  min;   /**< Lowest byte so far; 0xFF before the first.      */
    uint8_t  peak;  /**< Highest byte so far.                            */
    bool     ext;   /**< Set once any fold carried the detector set.     */
    uint16_t n;     /**< Bytes folded, stopping at UINT16_MAX.           */
} knfc_field_acc_t;

/**
 * @brief Clear @p a for a new window.
 *
 * @param a The accumulator. NULL is ignored.
 */
void knfc_field_reset(knfc_field_acc_t *a);

/**
 * @brief Fold one burst into @p a.
 *
 * Every byte updates min and peak. The sum and the count take bytes only
 * while the count is below UINT16_MAX, so a window longer than that keeps a
 * true mean of its first 65535 samples instead of wrapping. @p ext is folded
 * even when @p n is 0: the detector read is its own measurement, and a burst
 * whose amplitude reads all failed still saw what it saw.
 *
 * @param a   The accumulator, reset before the window's first fold. NULL is
 *            ignored.
 * @param amp The burst's amplitude bytes. May be NULL only when @p n is 0.
 * @param n   How many bytes @p amp holds; 0 is legal.
 * @param ext The external field detector's reading for this burst.
 */
void knfc_field_fold(knfc_field_acc_t *a, const uint8_t *amp, size_t n,
                     bool ext);

/**
 * @brief Close the window: copy the stats out of @p a into @p out.
 *
 * The mean is the integer quotient sum / n and the modulation is peak - min.
 * A window with no samples closes with every amplitude field and the count
 * at zero - never with the 0xFF an untouched min holds - and with @p ext as
 * the folds left it. @p a is not modified, so a window may be read before it
 * ends.
 *
 * @param a   The accumulator. NULL writes an all-zero answer.
 * @param out Where the answer goes. NULL is ignored.
 */
void knfc_field_close(const knfc_field_acc_t *a, knfc_field_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KNFC_FIELD_H_ */
