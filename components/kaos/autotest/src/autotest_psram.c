/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file autotest_psram.c
 * @brief The PSRAM row's patterns.
 */
#include "autotest_psram.h"

#include <stdbool.h>

#include "kqp_config.h"

/**
 * @brief What word @p w holds in pass @p pass.
 * @param pass PSRAM_PASS_*.
 * @param w    The word.
 * @return Its address, or the complement of it in the second half.
 */
static uint32_t pattern(uint8_t pass, const volatile uint32_t *w)
{
    /* cppcheck-suppress misra-c2012-11.4 ; the pattern is the word's own address: that is the test of the address lines */
    uint32_t value = (uint32_t)(uintptr_t)w;

    if ( (uint8_t)PSRAM_PASS_WRITE_INV <= pass )
    {
        value = ~value;
    }
    return value;
}

/* A pass covers the whole range before the next starts, so the cache holds no word by its check.
 * No cache invalidate is ever issued: on this chip it triggers the I-cache race. */
void psram_pass_span(uint8_t pass, volatile uint32_t *words, size_t count,
                     psram_fault_t *fault)
{
    bool const check = (((uint8_t)PSRAM_PASS_CHECK_ADDR == pass)
                        || ((uint8_t)PSRAM_PASS_CHECK_INV == pass));

    for ( size_t i = 0U; i < count; i++ )
    {
        uint32_t const want = pattern(pass, &words[i]);

        if ( !check )
        {
            words[i] = want;
        }
        else
        {
            /* Every word is read whatever the count says, so a pass costs
             * the same with errors or without. */
            uint32_t const got = words[i];

            if ( (got != want) && (KODE_PSRAM_TEST_ERRORS_MAX > fault->errors) )
            {
                if ( 0U == fault->errors )
                {
                    /* cppcheck-suppress misra-c2012-11.4 ; the address of the bad word is the diagnosis */
                    fault->first = (uintptr_t)&words[i];
                    fault->wrote = want;
                    fault->read  = got;
                }
                fault->errors += 1U;
            }
        }
    }
}
