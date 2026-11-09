/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kaos_park_c5_run.c
 * @brief CONFIG_KODEOS_C5_RUN: an app without AO_Radio keeps the C5 running for itself.
 */
#undef CONFIG_KODEOS_RADIO
#define CONFIG_KODEOS_C5_RUN 1
#define kaos_parked       kaos_parked_c5_run
#define kaos_park_storage kaos_park_storage_c5_run
#define kaos_park_c5      kaos_park_c5_c5_run
#define kaos_park_led     kaos_park_led_c5_run
#define kaos_park_ir      kaos_park_ir_c5_run
#define kaos_park_nfc     kaos_park_nfc_c5_run
#define kaos_park_rfid    kaos_park_rfid_c5_run
#define kaos_park_audio   kaos_park_audio_c5_run
#define kaos_park_imu     kaos_park_imu_c5_run
#define kaos_park_haptic  kaos_park_haptic_c5_run
#define kaos_park_appmgr  kaos_park_appmgr_c5_run
#define kaos_park_touch   kaos_park_touch_c5_run
#define kaos_park_display kaos_park_display_c5_run
#include "kaos_park.c"

#include <stdbool.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kbsp_owners.h"
#include "kdrv_radio.h"

/**
 * @brief Free the C5 for this build's park, from whichever owner held it.
 * @return true when AO_Radio held it, for c5_back().
 */
static bool c5_lend(void)
{
    bool const held = (kbsp_owner_name(KBSP_RES_C5) != NULL);

    kbsp_release(KBSP_RES_C5, AO_Radio);
    return held;
}

/**
 * @brief Take the C5 back from this build's park, and give it to AO_Radio again if it held it.
 * @param held What c5_lend() answered.
 */
static void c5_back(bool held)
{
    kbsp_release(KBSP_RES_C5, &kaos_parked_c5_run);
    if ( held )
    {
        (void)kbsp_claim(KBSP_RES_C5, AO_Radio, "AO_Radio");
    }
}

/** @brief Whether a captured log line contains @p needle. */
static bool c5_run_log_has(const char *needle)
{
    bool has = false;

    for ( unsigned i = 0U; fake_klog_count() > i; ++i )
    {
        if ( strstr(fake_klog_line(i), needle) != NULL )
        {
            has = true;
        }
    }
    return has;
}

TEST_CASE("with KODEOS_C5_RUN and no AO_Radio the C5 is reset once, left "
          "running and claimed for the app", "[kaos_park]")
{
    bool const held = c5_lend();

    fake_radio_reset();
    fake_klog_reset();
    fake_kaos_absent(&kaos_audio_start);

    kaos_park_c5_c5_run();

    TEST_ASSERT_EQUAL_UINT_MESSAGE(1U, fake_radio_c5_holds(),
        "the C5_EN pulse the amplifier's branch gives was not given");
    TEST_ASSERT_FALSE_MESSAGE(kdrv_radio_c5_held(), "the C5 was left in reset");
    TEST_ASSERT_EQUAL_STRING("running for the app", kbsp_owner_name(KBSP_RES_C5));
    TEST_ASSERT_TRUE(c5_run_log_has("the C5 reset and left running for the app"));
    fake_kaos_linked_reset();
    c5_back(held);
    fake_radio_reset();
}

TEST_CASE("with KODEOS_C5_RUN the claim is the app's with AO_Audio linked too",
          "[kaos_park]")
{
    bool const held = c5_lend();

    fake_radio_reset();

    kaos_park_c5_c5_run();

    TEST_ASSERT_EQUAL_UINT(1U, fake_radio_c5_holds());
    TEST_ASSERT_FALSE(kdrv_radio_c5_held());
    TEST_ASSERT_EQUAL_STRING("running for the app", kbsp_owner_name(KBSP_RES_C5));
    c5_back(held);
    fake_radio_reset();
}
