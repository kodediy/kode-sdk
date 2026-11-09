/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_screen_os.c
 * @brief In the kodeOS image the screen calls fail and say why.
 */
#include <stdatomic.h>

#include "unity.h"

#define kode_standard_ao_ensure        kode_standard_ao_ensure_screen_os
#define kode_standard_ao_screen_attach kode_standard_ao_screen_attach_screen_os
#define kode_standard_ao_screen_post   kode_standard_ao_screen_post_screen_os
#define kode_standard_ao_touch_listen  kode_standard_ao_touch_listen_screen_os
#define kode_standard_ao_pin_arm       kode_standard_ao_pin_arm_screen_os
#define kode_standard_ao_power_expect kode_standard_ao_power_expect_screen_os
#define kode_standard_ao_power_await  kode_standard_ao_power_await_screen_os
#define kode_standard_ao_subscribe     kode_standard_ao_subscribe_screen_os
#define kode_screen_new                    kode_screen_new_os
#define kode_screen_add_text               kode_screen_add_text_os
#define kode_widget_set_text           kode_widget_set_text_os
#define kode_widget_set_color          kode_widget_set_color_os
#define kode_screen_add_value              kode_screen_add_value_os
#define kode_screen_add_button             kode_screen_add_button_os
#define kode_widget_was_pressed kode_widget_was_pressed_os
#define kode_widget_tapped             kode_widget_tapped_os
#define kode_screen_add_canvas             kode_screen_add_canvas_os
#define kode_draw_text               kode_draw_text_os
#define kode_screen_add_image              kode_screen_add_image_os
#define kode_screen_image_add          kode_screen_image_add_os
#define kode_widget_text_done          kode_widget_text_done_os
#include "kode_standard_ao.c"
#include "kode_widget.c"

#include "fakes.h"
#include "kode_sys_priv.h"

TEST_CASE("the kodeOS image refuses the screen", "[kode_screen]")
{
#if !CONFIG_KODEOS_BOOT_ROLE_OS
    TEST_FAIL_MESSAGE("CONFIG_KODEOS_BOOT_ROLE_OS is not set: this case would be "
                      "asserting about the app build");
#endif
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);

    TEST_ASSERT_FALSE(kode_screen_new_os("Title"));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_text_os("text"));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_text_os(1U, "text"));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_FALSE(kode_widget_set_color_os(1U, KODE_RED));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_value_os("1", "C"));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_button_os("Go"));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_FALSE(kode_widget_was_pressed_os(1U));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_canvas_os(10U, 10U));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_FALSE(kode_draw_text_os(1U, 0, 0, "x", KODE_RED, KODE_ALIGN_LEFT));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_screen_add_image_os("flash/a.png"));
    TEST_ASSERT_EQUAL_STRING("the standard layer is for apps, not kodeOS", kode_error());
}
