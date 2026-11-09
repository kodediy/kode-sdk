/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_event_os.c
 * @brief In the kodeOS image the standard layer starts nothing and says why.
 */
#include <string.h>

#include "unity.h"

#define kode_standard_ao_ensure        kode_standard_ao_ensure_os_build
#define kode_standard_ao_screen_attach kode_standard_ao_screen_attach_os_build
#define kode_standard_ao_screen_post   kode_standard_ao_screen_post_os_build
#define kode_standard_ao_touch_listen  kode_standard_ao_touch_listen_os_build
#define kode_standard_ao_pin_arm       kode_standard_ao_pin_arm_os_build
#define kode_standard_ao_power_expect kode_standard_ao_power_expect_os_build
#define kode_standard_ao_power_await  kode_standard_ao_power_await_os_build
#define kode_standard_ao_subscribe     kode_standard_ao_subscribe_os_build
#include "kode_standard_ao.c"
#undef kode_standard_ao_touch_listen
#undef kode_standard_ao_pin_arm
#undef kode_standard_ao_power_expect
#undef kode_standard_ao_power_await
#undef kode_standard_ao_subscribe
#undef kode_standard_ao_screen_post
#undef kode_standard_ao_screen_attach
#undef kode_standard_ao_ensure

TEST_CASE("the kodeOS image refuses the standard layer", "[kode_event]")
{
#if !CONFIG_KODEOS_BOOT_ROLE_OS
    TEST_FAIL_MESSAGE("CONFIG_KODEOS_BOOT_ROLE_OS is not set: this case would be "
                      "asserting about the app build");
#endif
    TEST_ASSERT_FALSE(kode_standard_ao_ensure_os_build());
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_FALSE(kode_standard_ao_touch_listen_os_build(&touch_subscribe));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_FALSE(kode_standard_ao_pin_arm_os_build(KODE_PIN_FIRST));
    TEST_ASSERT_FALSE(kode_standard_ao_subscribe_os_build(&ble_app_subscribe));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
}
