/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kqp_current_ao.c
 * @brief kqp_current_is_ao(): which task the caller is running in.
 */
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "unity.h"

#include "freertos/FreeRTOS.h"

#include "kqp.h"
#include "probe_ao.h"

/** @brief The longest a case waits for the probe AO, in ms. */
#define PROBE_WAIT_MS 2000U

/** @brief Size of the buffer the probe copies the AO's name into. */
#define PROBE_NAME_MAX 32U

/** @brief What the probe found when it asked. */
typedef struct {
    bool is_ao;                  /**< The answer.            */
    char name[PROBE_NAME_MAX];   /**< The name it came with. */
} Found;

/** @brief The probe's job: ask kqp and record it. */
static void ask_job(void *ctx)
{
    Found      *f    = (Found *)ctx;
    const char *name = NULL;

    f->is_ao = kqp_current_is_ao(&name);
    if ( NULL != name )
    {
        (void)strncpy(f->name, name, sizeof(f->name) - 1U);
    }
}

TEST_CASE("a task that is no Active Object's is told so", "[kqp]")
{
    const char *name = "untouched";

    /* With an Active Object up and running, so the answer is about the task. */
    TEST_ASSERT_TRUE(probe_ao_run(NULL, NULL, PROBE_WAIT_MS));
    TEST_ASSERT_FALSE(kqp_current_is_ao(&name));
    TEST_ASSERT_EQUAL_STRING("untouched", name);
    TEST_ASSERT_FALSE(kqp_current_is_ao(NULL));
}

TEST_CASE("inside an Active Object's step the caller is told it is one",
          "[kqp]")
{
    Found f;

    f.is_ao = false;
    (void)memset(f.name, 0, sizeof(f.name));

    TEST_ASSERT_TRUE(probe_ao_run(&ask_job, &f, PROBE_WAIT_MS));
    TEST_ASSERT_TRUE(f.is_ao);
    TEST_ASSERT_EQUAL_STRING(PROBE_AO_NAME, f.name);
}
