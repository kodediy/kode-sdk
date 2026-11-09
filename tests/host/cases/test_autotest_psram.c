/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_autotest_psram.c
 * @brief The PSRAM row's four passes, on an ordinary buffer.
 */
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "autotest_psram.h"
#include "kqp_config.h"

/** @brief Words in the buffer the small cases use. */
#define SPAN_WORDS  256U

/** @brief A bit that a stuck cell would hold. */
#define STUCK_BIT   0x00000100U

/** @brief The address a word of the pattern carries. */
static uint32_t addr_of(const volatile uint32_t *w)
{
    return (uint32_t)(uintptr_t)w;
}

/** @brief Run passes [0, upto) over @p words. */
static void passes(volatile uint32_t *words, size_t count, uint8_t upto,
                   psram_fault_t *f)
{
    for ( uint8_t p = 0U; p < upto; p++ )
    {
        psram_pass_span(p, words, count, f);
    }
}

TEST_CASE("a clean span passes all four passes", "[autotest_psram]")
{
    static uint32_t buf[SPAN_WORDS];
    psram_fault_t f;

    (void)memset(&f, 0, sizeof(f));
    passes(buf, SPAN_WORDS, (uint8_t)PSRAM_PASSES, &f);
    TEST_ASSERT_EQUAL_UINT32(0U, f.errors);
    TEST_ASSERT_EQUAL_UINT32(0U, (uint32_t)f.first);
    TEST_ASSERT_EQUAL_UINT32(~addr_of(&buf[SPAN_WORDS - 1U]), buf[SPAN_WORDS - 1U]);
}

TEST_CASE("a stuck bit is caught with its address and both values",
          "[autotest_psram]")
{
    static uint32_t buf[SPAN_WORDS];
    psram_fault_t f;

    (void)memset(&f, 0, sizeof(f));
    psram_pass_span((uint8_t)PSRAM_PASS_WRITE_ADDR, buf, SPAN_WORDS, &f);
    buf[17] ^= STUCK_BIT;
    psram_pass_span((uint8_t)PSRAM_PASS_CHECK_ADDR, buf, SPAN_WORDS, &f);

    TEST_ASSERT_EQUAL_UINT32(1U, f.errors);
    TEST_ASSERT_TRUE(f.first == (uintptr_t)&buf[17]);
    TEST_ASSERT_EQUAL_HEX32(addr_of(&buf[17]), f.wrote);
    TEST_ASSERT_EQUAL_HEX32(f.wrote ^ STUCK_BIT, f.read);
}

TEST_CASE("an aliased word reads another word's address", "[autotest_psram]")
{
    static uint32_t buf[SPAN_WORDS];
    psram_fault_t f;

    (void)memset(&f, 0, sizeof(f));
    psram_pass_span((uint8_t)PSRAM_PASS_WRITE_ADDR, buf, SPAN_WORDS, &f);
    buf[40] = buf[8];
    psram_pass_span((uint8_t)PSRAM_PASS_CHECK_ADDR, buf, SPAN_WORDS, &f);

    TEST_ASSERT_EQUAL_UINT32(1U, f.errors);
    TEST_ASSERT_TRUE(f.first == (uintptr_t)&buf[40]);
    TEST_ASSERT_EQUAL_HEX32(addr_of(&buf[8]), f.read);
}

TEST_CASE("the complement pass checks the inverted pattern", "[autotest_psram]")
{
    static uint32_t buf[SPAN_WORDS];
    psram_fault_t f;

    (void)memset(&f, 0, sizeof(f));
    passes(buf, SPAN_WORDS, (uint8_t)PSRAM_PASS_CHECK_INV, &f);
    buf[3] ^= 1U;
    psram_pass_span((uint8_t)PSRAM_PASS_CHECK_INV, buf, SPAN_WORDS, &f);

    TEST_ASSERT_EQUAL_UINT32(1U, f.errors);
    TEST_ASSERT_EQUAL_HEX32(~addr_of(&buf[3]), f.wrote);
    TEST_ASSERT_EQUAL_HEX32(f.wrote ^ 1U, f.read);
}

TEST_CASE("the error count stops at its cap and first stays the first",
          "[autotest_psram]")
{
    /** A span with more bad words than the cap. */
    static uint32_t big[KODE_PSRAM_TEST_ERRORS_MAX + 24U];
    psram_fault_t f;

    (void)memset(&f, 0, sizeof(f));
    psram_pass_span((uint8_t)PSRAM_PASS_WRITE_ADDR, big,
                    sizeof(big) / sizeof(big[0]), &f);
    (void)memset(big, 0, sizeof(big));
    psram_pass_span((uint8_t)PSRAM_PASS_CHECK_ADDR, big,
                    sizeof(big) / sizeof(big[0]), &f);

    TEST_ASSERT_EQUAL_UINT32(KODE_PSRAM_TEST_ERRORS_MAX, f.errors);
    TEST_ASSERT_TRUE(f.first == (uintptr_t)&big[0]);
    TEST_ASSERT_EQUAL_HEX32(0U, f.read);
}
