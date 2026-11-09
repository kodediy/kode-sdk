/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_event_clash.c
 * @brief The standard layer refuses to start on a QP priority someone else holds.
 */
#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "ao_spy.h"
#include "fakes.h"
#include "kode_sys_priv.h"
#include "probe_ao.h"
#include "qf_support.h"

/** @brief The longest the case waits for the probe, in ms. */
#define PROBE_WAIT_MS 2000U

#undef CONFIG_KODEOS_BOOT_ROLE_OS
#define kode_standard_ao_ensure        kode_standard_ao_ensure_clash
#define kode_standard_ao_screen_attach kode_standard_ao_screen_attach_clash
#define kode_standard_ao_screen_post   kode_standard_ao_screen_post_clash
#define kode_standard_ao_touch_listen  kode_standard_ao_touch_listen_clash
#define kode_standard_ao_pin_arm       kode_standard_ao_pin_arm_clash
#define kode_standard_ao_power_expect kode_standard_ao_power_expect_clash
#define kode_standard_ao_power_await  kode_standard_ao_power_await_clash
#define kode_standard_ao_subscribe     kode_standard_ao_subscribe_clash
#define AO_KodeStandard AO_KodeStandard_clash
#include "kode_standard_ao.c"
#undef AO_KodeStandard
#undef kode_standard_ao_screen_post
#undef kode_standard_ao_touch_listen
#undef kode_standard_ao_pin_arm
#undef kode_standard_ao_power_expect
#undef kode_standard_ao_power_await
#undef kode_standard_ao_subscribe
#undef kode_standard_ao_screen_attach
#undef kode_standard_ao_ensure

/* The real one, which holds the level. */
bool kode_standard_ao_ensure(void);

TEST_CASE("a start at a level someone else holds refuses and names the holder", "[kode_event]")
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    fake_sync_real(true);
    TEST_ASSERT_TRUE(kode_standard_ao_ensure());

    TEST_ASSERT_FALSE(kode_standard_ao_ensure_clash());
    TEST_ASSERT_EQUAL_STRING("QP level 3 is taken by another Active Object (kode_standard)",
                             kode_error());
    /* Nothing of the copy's was started: its queue was never made. */
    TEST_ASSERT_NULL(AO_KodeStandard_clash->eQueue);
}

TEST_CASE("kqp_prio_taken answers for held, free and out-of-range levels", "[kode_event]")
{
    const char *name = "untouched";

    /* The spy of the other cases holds level 1 and has no task. */
    (void)ao_spy();
    TEST_ASSERT_TRUE(kqp_prio_taken(KODE_PRIO_USER_LOW, &name));
    TEST_ASSERT_EQUAL_STRING("?", name);
    TEST_ASSERT_TRUE(kqp_prio_taken(KODE_PRIO_USER_LOW, NULL));

    /* The probe holds the last level, with a task, so it has a name. Every
     * level from 1 to 24 has an owner in this binary (probe_ao.h), so the free
     * answer is read off the two levels outside the range. */
    TEST_ASSERT_TRUE(probe_ao_run(NULL, NULL, PROBE_WAIT_MS));
    TEST_ASSERT_TRUE(kqp_prio_taken(PROBE_AO_PRIO, &name));
    TEST_ASSERT_EQUAL_STRING(PROBE_AO_NAME, name);

    name = "untouched";
    TEST_ASSERT_FALSE(kqp_prio_taken(0U, &name));
    TEST_ASSERT_FALSE(kqp_prio_taken((uint8_t)(QF_MAX_ACTIVE + 1U), &name));
    TEST_ASSERT_EQUAL_STRING("untouched", name);
}
