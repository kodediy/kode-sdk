/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file autotest_psram.h
 * @brief The PSRAM row's patterns: write a span of words, or check it.
 */
#ifndef AUTOTEST_PSRAM_H
#define AUTOTEST_PSRAM_H

#include <stddef.h>
#include <stdint.h>

#define PSRAM_PASS_WRITE_ADDR  0U  /**< Every word gets its own address.   */
#define PSRAM_PASS_CHECK_ADDR  1U  /**< Every word must still hold it.     */
#define PSRAM_PASS_WRITE_INV   2U  /**< Every word gets the complement.    */
#define PSRAM_PASS_CHECK_INV   3U  /**< Every word must still hold that.   */
#define PSRAM_PASSES           4U  /**< How many passes make one run.      */

/** @brief What the check passes found, accumulated over a run. */
typedef struct {
    uint32_t  errors; /**< Words that read wrong, up to KODE_PSRAM_TEST_ERRORS_MAX. */
    uintptr_t first;  /**< The first of them; 0 while there is none.            */
    uint32_t  wrote;  /**< What it should have held.                             */
    uint32_t  read;   /**< What it held.                                         */
} psram_fault_t;

/**
 * @brief Run one pass over one span of words.
 *
 * A write pass stores the pass's pattern in every word. A check pass reads
 * every word back and adds whatever does not match to @p fault, which
 * carries over from call to call: the caller zeroes it once per run.
 *
 * @param pass  PSRAM_PASS_*.
 * @param words The span.
 * @param count How many words it holds.
 * @param fault What the check passes found so far.
 */
void psram_pass_span(uint8_t pass, volatile uint32_t *words, size_t count,
                     psram_fault_t *fault);

#endif /* AUTOTEST_PSRAM_H */
