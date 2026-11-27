/* SPDX-License-Identifier: GPL-3.0-or-later */
/**
 * @file test_kode_names.c
 * @brief The names kode.h takes as text: a constant for each, and one warning
 *        for the first unknown name of each kind.
 */
#include <math.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unity.h"

#include "fakes.h"
#include "kdrv_buttons.h"
#include "kode.h"
#include "kode_entry.h"
#include "kode_sys.h"
#include "kode_sys_priv.h"
#include "qf_support.h"

/** @brief One constant and the word it stands for. */
typedef struct {
    const char *constant;   /**< What the constant expands to. */
    const char *word;       /**< The word kode.h documents.    */
} name_row_t;

/** @brief Compare every row's constant with its word. */
static void rows_check(const name_row_t *rows, size_t n)
{
    for ( size_t i = 0U; n > i; i++ )
    {
        TEST_ASSERT_EQUAL_INT_MESSAGE(0, strcmp(rows[i].constant, rows[i].word), rows[i].word);
    }
}

/** @brief Make kode_init() a no-op, and forget every warning given so far. */
static void names_fresh(void)
{
    qf_support_init();
    atomic_store(&kode_sys_init_once.state, KHAL_ONCE_DONE);
    atomic_store(&kode_name_warned, 0U);
    fake_klog_reset();
}

/** @brief How many warnings the log holds. */
static unsigned warnings(void)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; i++ )
    {
        const char *const line = fake_klog_line(i);

        if ( (NULL != line) && (strncmp(line, "!W ", 3U) == 0) )
        {
            n++;
        }
    }
    return n;
}

/** @brief How many warnings the log holds that say @p text. */
static unsigned warnings_saying(const char *text)
{
    unsigned n = 0U;

    for ( unsigned i = 0U; fake_klog_count() > i; i++ )
    {
        const char *const line = fake_klog_line(i);

        if ( (NULL != line) && (strncmp(line, "!W ", 3U) == 0) && (strstr(line, text) != NULL) )
        {
            n++;
        }
    }
    return n;
}

/* ---- The constants ---------------------------------------------------------- */

TEST_CASE("every button constant is its word", "[kode_names]")
{
    static const name_row_t rows[] = {
        { KODE_BUTTON_UP, "up" },       { KODE_BUTTON_DOWN, "down" },
        { KODE_BUTTON_LEFT, "left" },   { KODE_BUTTON_RIGHT, "right" },
        { KODE_BUTTON_TOP, "top" },     { KODE_BUTTON_BOTTOM, "bottom" },
    };

    rows_check(rows, sizeof(rows) / sizeof(rows[0]));
}

TEST_CASE("every IMU and magnetometer axis constant is its word", "[kode_names]")
{
    static const name_row_t rows[] = {
        { KODE_IMU_ACCEL_X, "accel_x" }, { KODE_IMU_ACCEL_Y, "accel_y" },
        { KODE_IMU_ACCEL_Z, "accel_z" }, { KODE_IMU_GYRO_X, "gyro_x" },
        { KODE_IMU_GYRO_Y, "gyro_y" },   { KODE_IMU_GYRO_Z, "gyro_z" },
        { KODE_IMU_TEMP, "temp" },
        { KODE_MAG_X, "x" }, { KODE_MAG_Y, "y" }, { KODE_MAG_Z, "z" },
    };

    rows_check(rows, sizeof(rows) / sizeof(rows[0]));
}

TEST_CASE("every vibration constant is its word", "[kode_names]")
{
    static const name_row_t rows[] = {
        { KODE_HAPTIC_CLICK, "click" },     { KODE_HAPTIC_TICK, "tick" },
        { KODE_HAPTIC_DOUBLE, "double" },   { KODE_HAPTIC_SUCCESS, "success" },
        { KODE_HAPTIC_WARNING, "warning" }, { KODE_HAPTIC_ERROR, "error" },
    };

    rows_check(rows, sizeof(rows) / sizeof(rows[0]));
}

TEST_CASE("every audio setting, infrared and NFC constant is its word", "[kode_names]")
{
    static const name_row_t rows[] = {
        { KODE_AUDIO_VOLUME, "volume" }, { KODE_AUDIO_MIC_GAIN, "mic_gain" },
        { KODE_AUDIO_MUTE, "mute" },
        { KODE_IR_NEC, "NEC" }, { KODE_IR_UNKNOWN, "unknown" },
        { KODE_NFC_TEXT, "text" }, { KODE_NFC_URI, "uri" }, { KODE_NFC_UID, "uid" },
    };

    rows_check(rows, sizeof(rows) / sizeof(rows[0]));
}

TEST_CASE("every event constant is its word", "[kode_names]")
{
    static const name_row_t rows[] = {
        { KODE_EVENT_BUTTON_UP_PRESSED, "button_up_pressed" },
        { KODE_EVENT_BUTTON_UP_RELEASED, "button_up_released" },
        { KODE_EVENT_BUTTON_DOWN_PRESSED, "button_down_pressed" },
        { KODE_EVENT_BUTTON_DOWN_RELEASED, "button_down_released" },
        { KODE_EVENT_BUTTON_LEFT_PRESSED, "button_left_pressed" },
        { KODE_EVENT_BUTTON_LEFT_RELEASED, "button_left_released" },
        { KODE_EVENT_BUTTON_RIGHT_PRESSED, "button_right_pressed" },
        { KODE_EVENT_BUTTON_RIGHT_RELEASED, "button_right_released" },
        { KODE_EVENT_BUTTON_TOP_PRESSED, "button_top_pressed" },
        { KODE_EVENT_BUTTON_TOP_RELEASED, "button_top_released" },
        { KODE_EVENT_BUTTON_TOP_HELD, "button_top_held" },
        { KODE_EVENT_BUTTON_BOTTOM_PRESSED, "button_bottom_pressed" },
        { KODE_EVENT_BUTTON_BOTTOM_RELEASED, "button_bottom_released" },
        { KODE_EVENT_BUTTON_BOTTOM_HELD, "button_bottom_held" },
        { KODE_EVENT_PIN_1_CHANGED, "pin_1_changed" },
        { KODE_EVENT_PIN_2_CHANGED, "pin_2_changed" },
        { KODE_EVENT_PIN_3_CHANGED, "pin_3_changed" },
        { KODE_EVENT_PIN_4_CHANGED, "pin_4_changed" },
        { KODE_EVENT_PIN_5_CHANGED, "pin_5_changed" },
        { KODE_EVENT_PIN_6_CHANGED, "pin_6_changed" },
        { KODE_EVENT_PIN_7_CHANGED, "pin_7_changed" },
        { KODE_EVENT_PIN_8_CHANGED, "pin_8_changed" },
        { KODE_EVENT_PIN_9_CHANGED, "pin_9_changed" },
        { KODE_EVENT_PIN_10_CHANGED, "pin_10_changed" },
        { KODE_EVENT_PIN_11_CHANGED, "pin_11_changed" },
        { KODE_EVENT_PIN_12_CHANGED, "pin_12_changed" },
        { KODE_EVENT_PIN_13_CHANGED, "pin_13_changed" },
        { KODE_EVENT_PIN_14_CHANGED, "pin_14_changed" },
        { KODE_EVENT_MQTT_MESSAGE, "mqtt_message" },
        { KODE_EVENT_BATTERY_LOW, "battery_low" },
        { KODE_EVENT_PIN_POWER_5V_CUT, "pin_power_5v_cut" },
        { KODE_EVENT_NFC_TAG, "nfc_tag" },
    };

    rows_check(rows, sizeof(rows) / sizeof(rows[0]));
}

/* ---- The warning ------------------------------------------------------------ */

TEST_CASE("the first unknown name of a kind is one warning with kode_error()'s text", "[kode_names]")
{
    names_fresh();
    TEST_ASSERT_FALSE(kode_button_is_pressed("middle"));
    TEST_ASSERT_EQUAL_STRING("unknown button 'middle'", kode_error());
    TEST_ASSERT_EQUAL_UINT(1U, warnings());
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown button 'middle'"));
}

TEST_CASE("a second unknown name of the same kind writes nothing", "[kode_names]")
{
    names_fresh();
    TEST_ASSERT_FALSE(kode_button_is_pressed("middle"));
    TEST_ASSERT_FALSE(kode_button_is_pressed("bot"));
    TEST_ASSERT_EQUAL_STRING("unknown button 'bot' (did you mean 'bottom'?)", kode_error());
    TEST_ASSERT_EQUAL_UINT32(0U, kode_button_get_presses("side"));
    TEST_ASSERT_EQUAL_UINT(1U, warnings());
    TEST_ASSERT_EQUAL_UINT(0U, warnings_saying("bot"));
}

TEST_CASE("the did-you-mean refusal warns as the first unknown button", "[kode_names]")
{
    names_fresh();
    TEST_ASSERT_FALSE(kode_button_was_pressed("BOT"));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown button 'BOT' (did you mean 'bottom'?)"));
}

TEST_CASE("an unknown name of another kind writes its own warning", "[kode_names]")
{
    names_fresh();
    TEST_ASSERT_FALSE(kode_button_is_pressed("middle"));
    TEST_ASSERT_FALSE(kode_event_add_listener("button_middle_pressed", NULL));
    TEST_ASSERT_EQUAL_STRING("unknown event 'button_middle_pressed'", kode_error());
    TEST_ASSERT_EQUAL_UINT(2U, warnings());
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown event 'button_middle_pressed'"));

    /* And each kind once: a second event writes nothing more. */
    TEST_ASSERT_FALSE(kode_event_wait("pin_01_changed", 0U, NULL));
    TEST_ASSERT_EQUAL_UINT(2U, warnings());
}

TEST_CASE("a NULL name is refused with no warning", "[kode_names]")
{
    names_fresh();
    TEST_ASSERT_FALSE(kode_button_is_pressed(NULL));
    TEST_ASSERT_EQUAL_STRING("button name is NULL", kode_error());
    TEST_ASSERT_FALSE(kode_event_add_listener(NULL, NULL));
    TEST_ASSERT_EQUAL_STRING("event name is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, warnings());

    /* The kinds are still unwarned: the first unknown name warns. */
    TEST_ASSERT_FALSE(kode_button_is_pressed("middle"));
    TEST_ASSERT_EQUAL_UINT(1U, warnings());
}

TEST_CASE("a known name, as a word or a constant, writes no warning", "[kode_names]")
{
    names_fresh();
    TEST_ASSERT_FALSE(kode_button_is_pressed("top"));
    TEST_ASSERT_FALSE(kode_button_is_pressed(KODE_BUTTON_TOP));
    TEST_ASSERT_EQUAL_STRING("", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, warnings());
}

TEST_CASE("every refusal of an unknown name warns once for its own kind", "[kode_names]")
{
    names_fresh();

    TEST_ASSERT_TRUE(isnan(kode_imu_read_axis("accel_w")));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown axis 'accel_w'"));
    TEST_ASSERT_TRUE(isnan(kode_mag_read_axis("w")));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown axis 'w'"));
    TEST_ASSERT_FALSE(kode_haptic_play("buzz"));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown vibration 'buzz'"));
    TEST_ASSERT_FALSE(kode_audio_set("bass", 1U));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown setting 'bass'"));
    TEST_ASSERT_FALSE(kode_ir_send("rc5", 1U, 2U));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("protocol 'rc5' not supported"));
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("Bogus", 1U));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown NFC type 'Bogus'"));
    TEST_ASSERT_EQUAL_UINT(6U, warnings());

    /* A second unknown name of each of those kinds writes nothing more. */
    TEST_ASSERT_TRUE(isnan(kode_imu_read_axis("gyro_w")));
    TEST_ASSERT_TRUE(isnan(kode_mag_read_axis("xx")));
    TEST_ASSERT_FALSE(kode_haptic_play("hum"));
    TEST_ASSERT_FALSE(kode_audio_set("treble", 1U));
    TEST_ASSERT_FALSE(kode_ir_send("sony", 1U, 2U));
    TEST_ASSERT_EQUAL_STRING("", kode_nfc_read("uid", 1U));
    TEST_ASSERT_EQUAL_STRING("unknown NFC type 'uid'", kode_error());
    TEST_ASSERT_EQUAL_UINT(6U, warnings());
}

TEST_CASE("a refusal that is not about an unknown name writes no warning", "[kode_names]")
{
    names_fresh();
    TEST_ASSERT_FALSE(kode_audio_set(KODE_AUDIO_VOLUME, 101U));
    TEST_ASSERT_FALSE(kode_ir_send(KODE_IR_NEC, 70000U, 1U));
    TEST_ASSERT_FALSE(kode_event_add_listener(KODE_EVENT_BUTTON_TOP_PRESSED, NULL));
    TEST_ASSERT_EQUAL_STRING("listener is NULL", kode_error());
    TEST_ASSERT_EQUAL_UINT(0U, warnings());
}

TEST_CASE("each kind warns once on its own, whatever the others did", "[kode_names]")
{
    names_fresh();
    for ( uint32_t kind = 0U; (uint32_t)KODE_NAME_KINDS > kind; kind++ )
    {
        kode_err_set("unknown thing %u", (unsigned)kind);
        kode_name_unknown_warn((kode_name_kind_t)kind);
        kode_name_unknown_warn((kode_name_kind_t)kind);
    }
    TEST_ASSERT_EQUAL_UINT((unsigned)KODE_NAME_KINDS, warnings());
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown thing 0"));
    TEST_ASSERT_EQUAL_UINT(1U, warnings_saying("unknown thing 7"));

    /* A kind past the last is no kind: nothing is written. */
    kode_err_set("unknown thing past");
    kode_name_unknown_warn(KODE_NAME_KINDS);
    TEST_ASSERT_EQUAL_UINT((unsigned)KODE_NAME_KINDS, warnings());
}
